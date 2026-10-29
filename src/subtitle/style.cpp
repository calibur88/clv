/* 全局样式块：固定头 20 字节全量在场，bit0–bit4 只决定字段是否采纳。
 * 判序固定：长度下界 → 版本 → flags 高位 → 字族长度域判 → 位与长度对脸 → 长度式与定界
 * → 单位与保留位 → 字族串 UTF-8；先中先判、一次只出一个码。 */
#include "style.h"

#include "core.h"

#include <cstring>

namespace clv
{
	namespace subtitle
	{
		namespace
		{
			uint16_t Rd16(const uint8_t* p)
			{
				return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
			}

			void Wr16(uint8_t* p, uint16_t v)
			{
				p[0] = static_cast<uint8_t>(v & 0xFFu);
				p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
			}

			/* 线上按 R,G,B,A 字节序；公开面 R 落在 u32 的低 8 位，与这里的小端读法同值 */
			uint32_t RdRgba(const uint8_t* p)
			{
				return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
					   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
			}

			void WrRgba(uint8_t* p, uint32_t v)
			{
				p[0] = static_cast<uint8_t>(v & 0xFFu);
				p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
				p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
				p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
			}
		}	 // namespace

		Err ParseStyleBlock(const uint8_t* payload, size_t size, StyleBlock* out)
		{
			if (out == nullptr || payload == nullptr) return kInvalidArgument;
			std::memset(out, 0, sizeof(*out));

			if (size < kStyleBlockFixedLen) return kStyleBlockBad;

			const uint16_t struct_size = Rd16(payload + 0);
			const uint8_t version = payload[2];
			const uint8_t flags = payload[3];
			const uint16_t font_size = Rd16(payload + 4);
			const uint8_t font_size_unit = payload[6];
			const uint8_t reserved = payload[7];
			const uint32_t font_color = RdRgba(payload + 8);
			const uint32_t stroke_color = RdRgba(payload + 12);
			const uint16_t stroke_width = Rd16(payload + 16);
			const uint16_t font_family_len = Rd16(payload + 18);

			if (struct_size < kStyleBlockFixedLen) return kStyleBlockBad;
			if (version != 1u) return kStyleBlockBad;
			if ((flags & 0xE0u) != 0u) return kStyleBlockBad;
			if (font_family_len > kMaxFontFamilyLen) return kStyleBlockBad;

			const uint8_t has_family = static_cast<uint8_t>(flags & 1u);
			if (has_family == 0u && font_family_len != 0u) return kStyleBlockBad;
			if (has_family != 0u && font_family_len == 0u) return kStyleBlockBad;

			if (static_cast<uint32_t>(struct_size) != kStyleBlockFixedLen + static_cast<uint32_t>(font_family_len))
				return kStyleBlockBad;
			if (static_cast<size_t>(struct_size) != size) return kStyleBlockBad;

			if (font_size_unit > 1u) return kStyleBlockBad;
			if (reserved != 0u) return kStyleBlockBad;
			if (has_family != 0u && ! Utf8Valid(payload + kStyleBlockFixedLen, static_cast<size_t>(font_family_len)))
				return kStyleBlockBad;

			uint32_t adopted = 0u;
			if ((flags & kWireFontSize) != 0u)
			{
				adopted |= kStyleBitFontSize;
				out->font_size = font_size;
				out->font_size_unit = font_size_unit;
			}
			if ((flags & kWireFontColor) != 0u)
			{
				adopted |= kStyleBitFontColor;
				out->font_color_rgba = font_color;
			}
			if ((flags & kWireStrokeColor) != 0u)
			{
				adopted |= kStyleBitStrokeColor;
				out->stroke_color_rgba = stroke_color;
			}
			if ((flags & kWireStrokeWidth) != 0u)
			{
				adopted |= kStyleBitStrokeWidth;
				out->stroke_width = stroke_width;
			}
			if (has_family != 0u)
			{
				adopted |= kStyleBitFontFamily;
				out->font_family = payload + kStyleBlockFixedLen;
				out->font_family_size = font_family_len;
			}
			out->flags = adopted;
			return kOk;
		}

		Err EncodeStyleBlock(const StyleBlock* block, uint8_t* out, size_t capacity, size_t* out_size)
		{
			if (block == nullptr || out_size == nullptr) return kInvalidArgument;
			if (out == nullptr && capacity != 0u) return kInvalidArgument;
			/* 样式块只承载这五位；别的位（块属性才有的位置 / 滚动等）递进来即拒，不静默丢 */
			if ((block->flags & ~kStyleBlockAll) != 0u) return kStyleBlockBad;
			if ((block->flags & kStyleBitFontFamily) != 0u && block->font_family == nullptr) return kInvalidArgument;
			if (block->font_family_size > kMaxFontFamilyLen) return kStyleBlockBad;

			const size_t fam_len =
				(block->flags & kStyleBitFontFamily) != 0u ? static_cast<size_t>(block->font_family_size) : 0u;
			const size_t need = kStyleBlockFixedLen + fam_len;
			*out_size = need;
			if (need > capacity) return kOutputTooSmall;

			uint8_t wire = 0u;
			if ((block->flags & kStyleBitFontFamily) != 0u) wire |= static_cast<uint8_t>(kWireFontFamily);
			if ((block->flags & kStyleBitFontSize) != 0u) wire |= static_cast<uint8_t>(kWireFontSize);
			if ((block->flags & kStyleBitFontColor) != 0u) wire |= static_cast<uint8_t>(kWireFontColor);
			if ((block->flags & kStyleBitStrokeColor) != 0u) wire |= static_cast<uint8_t>(kWireStrokeColor);
			if ((block->flags & kStyleBitStrokeWidth) != 0u) wire |= static_cast<uint8_t>(kWireStrokeWidth);

			Wr16(out + 0, static_cast<uint16_t>(need));
			out[2] = 1u;
			out[3] = wire;
			Wr16(out + 4, block->font_size);
			out[6] = block->font_size_unit;
			out[7] = 0u;
			WrRgba(out + 8, block->font_color_rgba);
			WrRgba(out + 12, block->stroke_color_rgba);
			Wr16(out + 16, block->stroke_width);
			Wr16(out + 18, static_cast<uint16_t>(fam_len));
			if (fam_len != 0u) std::memcpy(out + kStyleBlockFixedLen, block->font_family, fam_len);
			return kOk;
		}
	}	 // namespace subtitle
}	 // namespace clv
