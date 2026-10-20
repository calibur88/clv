#include "headers.h"

#include "../core/varint.h"
#include "quant.h"

#include <cstring>

namespace clv
{
	namespace video
	{

		namespace
		{

			constexpr uint8_t kVopFixedBytes = 6u;
			constexpr uint8_t kSliceFixedNoQuant = 8u;
			constexpr uint8_t kSliceFixedWithQuant = 9u;
			constexpr uint64_t kMaxVarintScratch = 10u;

			uint32_t ReadU32LE(const uint8_t* p)
			{
				return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) |
					   (static_cast<uint32_t>(p[2]) << 16u) | (static_cast<uint32_t>(p[3]) << 24u);
			}

			uint16_t ReadU16LE(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8u)); }

			void WriteU32LE(uint32_t v, uint8_t* p)
			{
				p[0] = static_cast<uint8_t>(v & 0xFFu);
				p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
				p[2] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
				p[3] = static_cast<uint8_t>((v >> 24u) & 0xFFu);
			}

			bool ReadVarint(const uint8_t* p, size_t avail, uint64_t* out, size_t* used)
			{
				const size_t n = clv::core::DecodeVarint(p, avail, out);
				if (n == 0u) return false;
				// 非最短形式（overlong）按载荷不成立
				if (! clv::core::IsShortestVarint(p, n)) return false;
				*used = n;
				return true;
			}

			void AppendVarint(std::vector<uint8_t>* out, uint64_t v)
			{
				uint8_t scratch[kMaxVarintScratch];
				const size_t n = clv::core::EncodeVarint(v, scratch);
				out->insert(out->end(), scratch, scratch + n);
			}

		}	 // namespace

		uint8_t EffectiveQuant(uint8_t q, int8_t d_vop, int8_t d_mb, uint8_t quant_precision)
		{
			const int ceiling = static_cast<int>(QuantCeiling(quant_precision));
			int v = static_cast<int>(q) + static_cast<int>(d_vop) + static_cast<int>(d_mb);
			if (v < 1) v = 1;
			if (v > ceiling) v = ceiling;
			return static_cast<uint8_t>(v);
		}

		uint32_t CodedSize(uint16_t orig)
		{
			// 32 位无符号推导：orig 最大 65535 时补后为 65536，16 位容器会回绕成 0
			const uint32_t v = orig;
			return (v + 15u) & ~15u;
		}

		bool EncodeVopHeader(const VopHeader& h, uint8_t quant_precision, std::vector<uint8_t>* out)
		{
			if (out == nullptr) return false;
			if (h.type > 2u) return false;
			if (! QuantInStreamDomain(h.quant, quant_precision)) return false;
			if (h.fcode_forward < 1u || h.fcode_forward > 7u) return false;
			if (h.fcode_backward < 1u || h.fcode_backward > 7u) return false;
			if (h.intra_dc_vlc_thr > 7u) return false;
			if (static_cast<uint8_t>(h.coef_coding) > kCoefCodingMax) return false;
			if (h.dquant < kVopDquantMin || h.dquant > kVopDquantMax) return false;
			if (h.mb_count == 0u) return false;

			out->clear();
			out->reserve(kVopFixedBytes + 4u);
			out->push_back(h.type);
			out->push_back(h.quant);
			out->push_back(h.fcode_forward);
			out->push_back(h.fcode_backward);
			// bit0-2 = intra_dc_vlc_thr，bit3 / bit4 恒 0，bit5-7 = 系数编码法
			out->push_back(static_cast<uint8_t>((h.intra_dc_vlc_thr & 0x07u) |
												(static_cast<uint8_t>(h.coef_coding) << kVopCoefCodingShift)));
			uint8_t dq = static_cast<uint8_t>(h.dquant & 0x1F);	   // 5 bit 带符号的原值
			out->push_back(dq);									   // bit5-7 恒 0
			AppendVarint(out, h.mb_count);
			return true;
		}

		bool DecodeVopHeader(const uint8_t* data, size_t size, uint8_t quant_precision, VopHeader* out,
							 size_t* consumed)
		{
			if (data == nullptr || out == nullptr || size < kVopFixedBytes + 1u) return false;

			VopHeader h;
			h.type = data[0];
			h.quant = data[1];
			h.fcode_forward = data[2];
			h.fcode_backward = data[3];
			const uint8_t flags = data[4];
			const uint8_t dquant_byte = data[5];

			if (h.type > 2u) return false;
			if (! QuantInStreamDomain(h.quant, quant_precision)) return false;
			if (h.fcode_forward < 1u || h.fcode_forward > 7u) return false;
			if (h.fcode_backward < 1u || h.fcode_backward > 7u) return false;
			// v1：顶场优先与隔行扫描位都必须为 0
			if ((flags & kVopFlagsReservedMask) != 0u) return false;
			if ((dquant_byte & kDquantByteReservedMask) != 0u) return false;
			/* 系数编码法只认已赋法的取值：读到未赋法的值一律判帧头不成立，
			 * 不允许「未知当 0 处理」—— 那样一条新流会被按定宽凑着读完，静默解出错值比崩更糟。 */
			const uint8_t coding = static_cast<uint8_t>((flags & kVopCoefCodingMask) >> kVopCoefCodingShift);
			if (coding > kCoefCodingMax) return false;

			uint8_t d = static_cast<uint8_t>(dquant_byte & 0x1Fu);
			if (d & 0x10u) d = static_cast<uint8_t>(d | 0xE0u);	   // 5 bit 补码扩展到 8 bit
			h.dquant = static_cast<int8_t>(d);
			h.intra_dc_vlc_thr = static_cast<uint8_t>(flags & 0x07u);
			h.coef_coding = static_cast<CoefCoding>(coding);

			size_t used = 0u;
			if (! ReadVarint(data + kVopFixedBytes, size - kVopFixedBytes, &h.mb_count, &used)) return false;
			if (h.mb_count == 0u) return false;

			*out = h;
			if (consumed != nullptr) *consumed = kVopFixedBytes + used;
			return true;
		}

		bool EncodeSliceHeader(const SliceHeader& h, uint8_t quant_precision, std::vector<uint8_t>* out)
		{
			if (out == nullptr) return false;
			if (h.fragment_count < 2u) return false;	// 单片帧不发片头
			if (h.mb_run == 0u) return false;			// 空片不合法：每片至少一个宏块
			if (h.vop_type > 2u) return false;
			if (h.has_vop_quant != h.is_first) return false;
			if (h.is_first && ! QuantInStreamDomain(h.vop_quant, quant_precision)) return false;

			out->clear();
			out->reserve(kSliceFixedWithQuant + 4u);
			uint8_t fixed[kSliceFixedWithQuant];
			WriteU32LE(h.frame_id, fixed);
			fixed[4] = static_cast<uint8_t>(h.fragment_count & 0xFFu);
			fixed[5] = static_cast<uint8_t>((h.fragment_count >> 8) & 0xFFu);
			uint8_t flags = 0u;
			if (h.is_last) flags |= 0x01u;
			if (h.is_first) flags |= 0x02u;
			if (h.has_vop_quant) flags |= 0x04u;
			fixed[6] = flags;	 // bit3-7 恒 0
			fixed[7] = h.vop_type;
			fixed[8] = h.vop_quant;
			out->insert(out->end(), fixed, fixed + (h.has_vop_quant ? kSliceFixedWithQuant : kSliceFixedNoQuant));
			AppendVarint(out, h.first_mb);
			AppendVarint(out, h.mb_run);
			return true;
		}

		bool DecodeSliceHeader(const uint8_t* data, size_t size, uint8_t quant_precision, bool first_authoritative,
							   bool last_authoritative, SliceHeader* out, size_t* consumed)
		{
			if (data == nullptr || out == nullptr || size < kSliceFixedNoQuant + 1u) return false;

			SliceHeader h;
			h.frame_id = ReadU32LE(data);
			h.fragment_count = ReadU16LE(data + 4);
			const uint8_t flags = data[6];
			h.vop_type = data[7];
			if ((flags & kSliceFlagsReservedMask) != 0u) return false;	  // 片头 bit3-7 reserved
			if (h.fragment_count < 2u) return false;					  // 单片帧不该有片头
			if (h.vop_type > 2u) return false;

			h.is_last = (flags & 0x01u) != 0u;
			h.is_first = (flags & 0x02u) != 0u;
			h.has_vop_quant = (flags & 0x04u) != 0u;
			// 片头的两个位置位是冗余声明，容器外层为权威：不一致按片头不成立
			if (h.is_first != first_authoritative || h.is_last != last_authoritative) return false;
			if (h.has_vop_quant != h.is_first) return false;

			size_t pos = kSliceFixedNoQuant;
			if (h.has_vop_quant)
			{
				if (size < kSliceFixedWithQuant + 1u) return false;
				h.vop_quant = data[8];
				if (! QuantInStreamDomain(h.vop_quant, quant_precision)) return false;
				pos = kSliceFixedWithQuant;
			}

			size_t used = 0u;
			if (! ReadVarint(data + pos, size - pos, &h.first_mb, &used)) return false;
			pos += used;

			// 变量部分第二个 varint = 该片实际宏块数；0 = 空片
			if (! ReadVarint(data + pos, size - pos, &h.mb_run, &used)) return false;
			pos += used;
			/* 字段先交出再判空片：调用方要按档分派 —— 空片属**值域不成立**，
			 * 与「片头读不尽 / 携带位矛盾」不是一回事。 */
			*out = h;
			if (consumed != nullptr) *consumed = pos;
			return h.mb_run != 0u;
		}

		bool ParseEncMeta(const uint8_t* data, size_t size, EncMeta* out)
		{
			if (data == nullptr || out == nullptr || size != 8u) return false;
			EncMeta m;
			m.max_b_frames = data[0];
			m.quant_precision = data[1];
			m.ext_tools = data[2];
			m.reserved = data[3];
			m.display_width = ReadU16LE(data + 4);
			m.display_height = ReadU16LE(data + 6);
			if (m.reserved != 0u) return false;								  // v1 = 0
			if ((m.ext_tools & kExtToolsReservedMask) != 0u) return false;	  // bit1-7 v1 = 0
			if (m.quant_precision < kQuantPrecisionMin || m.quant_precision > kQuantPrecisionMax) return false;
			// 尺寸是硬要求：0 不再当「未声明」收下，写 0 即值域不成立
			if (m.display_width == 0u || m.display_height == 0u) return false;
			*out = m;
			return true;
		}

	}	 // namespace video
}	 // namespace clv
