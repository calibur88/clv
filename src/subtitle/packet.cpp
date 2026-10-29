/* 字幕包载荷外层：判序固定：定长头不全 → 可选字段被截 → 长度不符/尾随 → flags 高位
 * → 时间域判与 start/end 关系 → 对齐枚举 → 超过 max_packet_bytes；先中先判、一次只出一个码。 */
#include "packet.h"

#include <cstring>

namespace clv
{
	namespace subtitle
	{
		namespace
		{
			uint32_t Rd32(const uint8_t* p)
			{
				return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
					   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
			}

			uint64_t Rd64(const uint8_t* p)
			{
				return static_cast<uint64_t>(Rd32(p)) | (static_cast<uint64_t>(Rd32(p + 4)) << 32);
			}

			void Wr32(uint8_t* p, uint32_t v)
			{
				p[0] = static_cast<uint8_t>(v);
				p[1] = static_cast<uint8_t>(v >> 8);
				p[2] = static_cast<uint8_t>(v >> 16);
				p[3] = static_cast<uint8_t>(v >> 24);
			}

			void Wr64(uint8_t* p, uint64_t v)
			{
				Wr32(p, static_cast<uint32_t>(v));
				Wr32(p + 4, static_cast<uint32_t>(v >> 32));
			}

			bool I64Add(int64_t a, int64_t b, int64_t* out)
			{
				if (b > 0 && a > INT64_MAX - b) return false;
				if (b < 0 && a < INT64_MIN - b) return false;
				*out = a + b;
				return true;
			}

			bool I64Sub(int64_t a, int64_t b, int64_t* out)
			{
				if (b < 0 && a > INT64_MAX + b) return false;
				if (b > 0 && a < INT64_MIN + b) return false;
				*out = a - b;
				return true;
			}
		}	 // namespace

		Err ParsePacketHeader(const uint8_t* payload, size_t size, int64_t container_pts, uint32_t max_packet_bytes,
							  PacketHeader* out)
		{
			if (out == nullptr || payload == nullptr) return kInvalidArgument;
			std::memset(out, 0, sizeof(*out));

			if (size < kPacketHeaderFixedLen) return kPacketHeaderBad;

			const uint64_t start_raw = Rd64(payload);
			const uint64_t end_raw = Rd64(payload + 8);
			const uint32_t text_len = Rd32(payload + 16);
			const uint8_t flags = payload[20];

			const uint8_t has_position = static_cast<uint8_t>(flags & 1u);
			const uint8_t has_alignment = static_cast<uint8_t>((flags >> 1) & 1u);
			const uint32_t header_len =
				kPacketHeaderFixedLen + (has_position != 0u ? 8u : 0u) + (has_alignment != 0u ? 1u : 0u);

			if (size < header_len) return kPacketHeaderBad;

			const uint64_t total = static_cast<uint64_t>(header_len) + static_cast<uint64_t>(text_len);
			if (total != static_cast<uint64_t>(size)) return kPacketHeaderBad;
			if ((flags & 0xFCu) != 0u) return kPacketHeaderBad;
			if (start_raw > static_cast<uint64_t>(INT64_MAX) || end_raw > static_cast<uint64_t>(INT64_MAX))
				return kPacketHeaderBad;

			const int64_t start_i64 = static_cast<int64_t>(start_raw);
			const int64_t end_i64 = static_cast<int64_t>(end_raw);
			if (end_i64 <= start_i64) return kPacketHeaderBad;

			uint8_t alignment = 0;
			if (has_alignment != 0u)
			{
				alignment = payload[kPacketHeaderFixedLen + (has_position != 0u ? 8u : 0u)];
				if (alignment > 2u) return kPacketHeaderBad;
			}

			const uint32_t limit = max_packet_bytes == 0u ? kMaxPacketSize : max_packet_bytes;
			if (total > static_cast<uint64_t>(limit)) return kPacketTooLarge;

			int64_t delta = 0;
			if (! I64Sub(container_pts, start_i64, &delta)) return kPacketHeaderBad;
			int64_t end_shifted = 0;
			if (! I64Add(end_i64, delta, &end_shifted)) return kPacketHeaderBad;

			out->delta_ticks = delta;
			out->start_ticks = container_pts;
			out->end_ticks = end_shifted;
			out->text_len = text_len;
			out->header_len = header_len;
			out->flags = flags;
			out->has_position = has_position;
			out->has_alignment = has_alignment;
			out->alignment = alignment;
			if (has_position != 0u)
			{
				const uint8_t* pos = payload + kPacketHeaderFixedLen;
				out->position_x = static_cast<int32_t>(Rd32(pos));
				out->position_y = static_cast<int32_t>(Rd32(pos + 4));
			}
			return kOk;
		}

		Err EncodePacket(const PacketHeader* header, const uint8_t* text, size_t text_len, uint8_t* out,
						 size_t capacity, size_t* out_size)
		{
			if (header == nullptr || out_size == nullptr) return kInvalidArgument;
			if (text_len > 0u && text == nullptr) return kInvalidArgument;
			*out_size = 0;

			if ((header->flags & 0xFCu) != 0u) return kInvalidArgument;
			const uint8_t expect = static_cast<uint8_t>((header->has_position != 0u ? 1u : 0u) |
														((header->has_alignment != 0u ? 1u : 0u) << 1));
			if (header->flags != expect) return kInvalidArgument;
			if (header->alignment > 2u) return kInvalidArgument;
			if (header->end_ticks <= header->start_ticks) return kInvalidArgument;
			if (text_len > UINT32_MAX) return kInvalidArgument;

			const uint32_t header_len = kPacketHeaderFixedLen + (header->has_position != 0u ? 8u : 0u) +
										(header->has_alignment != 0u ? 1u : 0u);
			const uint64_t total = static_cast<uint64_t>(header_len) + static_cast<uint64_t>(text_len);
			if (total > static_cast<uint64_t>(kMaxPacketSize)) return kPacketTooLarge;
			if (total > static_cast<uint64_t>(capacity))
			{
				*out_size = static_cast<size_t>(total);
				return kOutputTooSmall;
			}
			if (header->start_ticks < 0 || header->end_ticks < 0) return kInvalidArgument;

			Wr64(out, static_cast<uint64_t>(header->start_ticks));
			Wr64(out + 8, static_cast<uint64_t>(header->end_ticks));
			Wr32(out + 16, static_cast<uint32_t>(text_len));
			out[20] = header->flags;
			uint32_t off = kPacketHeaderFixedLen;
			if (header->has_position != 0u)
			{
				Wr32(out + off, static_cast<uint32_t>(header->position_x));
				Wr32(out + off + 4, static_cast<uint32_t>(header->position_y));
				off += 8u;
			}
			if (header->has_alignment != 0u)
			{
				out[off] = header->alignment;
				off += 1u;
			}
			if (text_len > 0u) std::memcpy(out + off, text, text_len);

			*out_size = static_cast<size_t>(total);
			return kOk;
		}
	}	 // namespace subtitle
}	 // namespace clv
