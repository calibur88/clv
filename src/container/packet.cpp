#include "packet.h"

#include "../core/crc32.h"
#include "../core/varint.h"
#include "byte_io.h"

namespace clv
{
	namespace container
	{

		namespace
		{

			constexpr size_t kMaxVarintBytes = 10;
			constexpr uint32_t kMinTotalSize = 9;

			uint8_t MakeFlags(const PacketFields& f) noexcept
			{
				uint8_t flags = static_cast<uint8_t>(PacketFlagBit::kHasPts);	 // 冗余位，各片恒 1
				if (f.is_keyframe) flags |= static_cast<uint8_t>(PacketFlagBit::kKeyframe);
				if (f.is_fragment) flags |= static_cast<uint8_t>(PacketFlagBit::kFragment);
				if (f.is_last_fragment) flags |= static_cast<uint8_t>(PacketFlagBit::kLastFragment);
				return flags;
			}

			// varint 字段：解码 + 最短形式检查，非最短形式（如 0x80 0x00 表 0）按字段区不成立处理。
			// 失败不推进游标。
			bool ReadVarintField(Cursor& c, uint64_t* v) noexcept
			{
				const uint8_t* at = c.Base() + c.Used();
				const size_t used = core::DecodeVarint(at, c.Remain(), v);
				if (used == 0 || ! core::IsShortestVarint(at, used)) return false;

				const uint8_t* dummy = nullptr;
				return c.Take(used, &dummy);
			}

			bool ReadZigZagField(Cursor& c, int64_t* v) noexcept
			{
				const uint8_t* at = c.Base() + c.Used();
				const size_t used = core::DecodeZigZag(at, c.Remain(), v);
				if (used == 0 || ! core::IsShortestVarint(at, used)) return false;

				const uint8_t* dummy = nullptr;
				return c.Take(used, &dummy);
			}

		}	 // namespace

		bool FlagsFingerprintOk(uint8_t flags) noexcept
		{
			const bool has_pts = (flags & static_cast<uint8_t>(PacketFlagBit::kHasPts)) != 0;
			const bool has_fec = (flags & static_cast<uint8_t>(PacketFlagBit::kHasFec)) != 0;
			const bool reserved_zero = (flags & 0xE0u) == 0;
			return has_pts && ! has_fec && reserved_zero;
		}

		size_t EncodePacket(const PacketFields& f, const uint8_t* payload, size_t payload_size, uint64_t max_payload,
							std::vector<uint8_t>& out)
		{
			if (payload_size != 0 && payload == nullptr) return 0;
			if (payload_size > max_payload) return 0;
			if (f.is_last_fragment && ! f.is_fragment) return 0;

			const bool frame_first = ! f.is_fragment || f.fragment_index == 0;
			const size_t base = out.size();

			std::vector<uint8_t> body;
			Sink s(body);
			uint8_t tmp[kMaxVarintBytes];

			s.U8(f.stream_id);
			s.U8(MakeFlags(f));

			if (f.is_fragment)
			{
				const size_t n = core::EncodeVarint(f.fragment_index, tmp);
				s.Bytes(tmp, n);
			}
			{
				const size_t n = core::EncodeVarint(f.dts_delta, tmp);
				s.Bytes(tmp, n);
			}
			if (frame_first)
			{
				const size_t n = core::EncodeZigZag(f.pts_delta, tmp);
				s.Bytes(tmp, n);
			}
			{
				const size_t n = core::EncodeVarint(payload_size, tmp);
				s.Bytes(tmp, n);
			}
			{
				const size_t n = core::EncodeVarint(f.ext.size(), tmp);
				s.Bytes(tmp, n);
			}

			s.Bytes(payload, payload_size);
			s.Bytes(f.ext.data(), f.ext.size());

			// crc32 先占 4 字节，算完整段再回填
			const uint8_t zero[4] = { 0, 0, 0, 0 };
			s.Bytes(zero, 4);

			if (body.size() > 0xFFFFFFFFull) return 0;
			const uint32_t total_size = static_cast<uint32_t>(body.size());

			// 线上布局 = total_size(4) + body(total_size)，body 末 4 字节就是 crc32
			out.insert(out.end(), 4, 0);
			out.insert(out.end(), body.begin(), body.end());

			WriteLE32(out.data() + base, total_size);
			const uint32_t crc = core::Crc32(out.data() + base + 4, total_size - 4);
			WriteLE32(out.data() + base + 4 + (total_size - 4), crc);

			return static_cast<size_t>(total_size) + 4u;
		}

		PacketParse DecodePacket(const uint8_t* buf, size_t avail, uint64_t outer_limit, ParsedPacket* out)
		{
			if (buf == nullptr || out == nullptr) return PacketParse::kTruncated;
			if (avail < 4) return PacketParse::kTruncated;

			const uint32_t total_size = ReadLE32(buf);
			if (total_size < kMinTotalSize) return PacketParse::kHeadInvalid;

			const uint64_t wire = 4ull + total_size;
			if (wire > outer_limit) return PacketParse::kTooLarge;
			if (avail < wire) return PacketParse::kTruncated;

			if (! FlagsFingerprintOk(buf[5])) return PacketParse::kHeadInvalid;

			if (ReadLE32(buf + total_size) != core::Crc32(buf + 4, total_size - 4)) return PacketParse::kCrcFail;

			*out = ParsedPacket {};
			out->total_size = total_size;
			out->flags = buf[5];
			out->is_keyframe = (out->flags & static_cast<uint8_t>(PacketFlagBit::kKeyframe)) != 0;
			out->is_fragment = (out->flags & static_cast<uint8_t>(PacketFlagBit::kFragment)) != 0;
			out->is_last_fragment = (out->flags & static_cast<uint8_t>(PacketFlagBit::kLastFragment)) != 0;

			// 游标覆盖 stream_id 到 crc32 末（长度 = total_size），末尾 4 字节即 crc32，
			// 这样 payload_size 仲裁式可以直接写成「剩余 = payload_size + ext_len_varint + 4」
			Cursor c(buf + 4, total_size);
			if (! c.U8(&out->stream_id)) return PacketParse::kFieldFail;

			// flags 已在步 4 按同一字节校验过，这里只把游标推进过去
			const uint8_t* skip = nullptr;
			if (! c.Take(1, &skip)) return PacketParse::kFieldFail;

			if (out->is_fragment && ! ReadVarintField(c, &out->fragment_index)) return PacketParse::kFieldFail;
			if (! ReadVarintField(c, &out->dts_delta)) return PacketParse::kFieldFail;
			if (out->IsFrameFirst() && ! ReadZigZagField(c, &out->pts_delta)) return PacketParse::kFieldFail;
			if (! ReadVarintField(c, &out->payload_size)) return PacketParse::kFieldFail;
			if (! ReadVarintField(c, &out->ext_len_varint)) return PacketParse::kFieldFail;

			// payload_size 仲裁式的等价形：剩余 = payload_size + ext_len_varint + crc32(4)。
			// 两者同源同值，写成剩余式是因为字段长度都已按实际 varint 消费掉。
			//
			// 失败时上层跳包只认这里解码出的 total_size：包边界由 total_size 自证，
			// 不得回头按字节形态找「下一个像包头」的位置来推断起点——那是重同步的活，
			// 只有包头不成立才允许走。
			if (out->payload_size > c.Remain() || out->ext_len_varint > c.Remain()) return PacketParse::kFieldFail;
			if (out->payload_size + out->ext_len_varint + 4u != c.Remain()) return PacketParse::kFieldFail;

			if (! c.CopyTo(out->payload, static_cast<size_t>(out->payload_size))) return PacketParse::kFieldFail;
			if (! c.CopyTo(out->ext, static_cast<size_t>(out->ext_len_varint))) return PacketParse::kFieldFail;

			const uint8_t* dummy = nullptr;
			if (! c.Take(4, &dummy)) return PacketParse::kFieldFail;
			if (c.Remain() != 0) return PacketParse::kFieldFail;

			return PacketParse::kOk;
		}

	}	 // namespace container
}	 // namespace clv
