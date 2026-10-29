/* 全局样式块：20 字节定长头 + 可选字族串，线上字节布局如下（读、写两侧共用这一份）：

 *   0  u16 struct_size = 20 + font_family_len
 *   2  u8  version = 1
 *   3  u8  flags：bit0 font_family / bit1 font_size / bit2 font_color / bit3 stroke_color /
 *          bit4 stroke_width；bit5–7 必须 0
 *   4  u16 font_size
 *   6  u8  font_size_unit（0 = 0.1% 屏高，1 = px）
 *   7  u8  reserved = 0
 *   8  4B  font_color，按 R,G,B,A 字节序，straight alpha
 *   12 4B  stroke_color，同序
 *   16 u16 stroke_width（像素，0 = 无描边）
 *   18 u16 font_family_len（UTF-8 字节数，不含 NUL）
 *   20 N   font_family

 * 固定头 20 字节全量在场；只有 font_family 受 bit0 影响。
 * 颜色在公开面按 u32 的 R 落低 8 位，与本件的小端读法同值。
 */
#ifndef CLV_SUBTITLE_STYLE_H
#define CLV_SUBTITLE_STYLE_H

#include "status.h"

#include <stddef.h>
#include <stdint.h>

namespace clv
{
	namespace subtitle
	{
		inline constexpr uint32_t kStyleBlockFixedLen = 20u;
		inline constexpr uint32_t kMaxFontFamilyLen = 65515u;

		/* 线上 flags 字节的五位 */
		inline constexpr uint32_t kWireFontFamily = 1u << 0;
		inline constexpr uint32_t kWireFontSize = 1u << 1;
		inline constexpr uint32_t kWireFontColor = 1u << 2;
		inline constexpr uint32_t kWireStrokeColor = 1u << 3;
		inline constexpr uint32_t kWireStrokeWidth = 1u << 4;
		inline constexpr uint32_t kWireAll =
			kWireFontFamily | kWireFontSize | kWireFontColor | kWireStrokeColor | kWireStrokeWidth;

		/* 同五位在公开样式位里的位置（两套位序不同，映射只在本件写一遍） */
		inline constexpr uint32_t kStyleBitFontFamily = CLV_SUBTITLE_STYLE_FONT_FAMILY;
		inline constexpr uint32_t kStyleBitFontSize = CLV_SUBTITLE_STYLE_FONT_SIZE;
		inline constexpr uint32_t kStyleBitFontColor = CLV_SUBTITLE_STYLE_FONT_COLOR;
		inline constexpr uint32_t kStyleBitStrokeColor = CLV_SUBTITLE_STYLE_STROKE_COLOR;
		inline constexpr uint32_t kStyleBitStrokeWidth = CLV_SUBTITLE_STYLE_STROKE_WIDTH;
		inline constexpr uint32_t kStyleBlockAll =
			kStyleBitFontFamily | kStyleBitFontSize | kStyleBitFontColor | kStyleBitStrokeColor | kStyleBitStrokeWidth;

		struct StyleBlock
		{
			uint32_t flags; /* 采纳的字段位（公开样式位）；位为 0 的字段只在线不在本结构里 */
			uint16_t font_size;
			uint8_t font_size_unit;
			uint32_t font_color_rgba;
			uint32_t stroke_color_rgba;
			uint16_t stroke_width;
			const uint8_t* font_family; /* 出借：指向调用方给的那段载荷 */
			uint32_t font_family_size;
		};

		Err ParseStyleBlock(const uint8_t* payload, size_t size, StyleBlock* out);

		/* 反向写出（造测试件与演示件用）；容量不足返 kOutputTooSmall 并把所需长度写进 out_size。 */
		Err EncodeStyleBlock(const StyleBlock* block, uint8_t* out, size_t capacity, size_t* out_size);
	}	 // namespace subtitle
}	 // namespace clv

#endif	  // CLV_SUBTITLE_STYLE_H
