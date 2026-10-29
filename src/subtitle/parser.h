/* 字幕解析器：块与文本行的文档语法、流级单调、tick 平移与样式覆盖链。
 *
 * 一包一调 ParserFeed：包外层 → 整包 UTF-8 → 逐行文档语法 → 流级单调 → 平移
 * → 每个闭合块当场按覆盖链压成最终样式并记 style_source。
 *
 * 文本行的解码结果由解析器持有（解码串按行存在 deque 里，指针在 Reset 前不变）。
 */
#ifndef CLV_SUBTITLE_PARSER_H
#define CLV_SUBTITLE_PARSER_H

#include "core.h"
#include "packet.h"
#include "status.h"
#include "style.h"

#include <stddef.h>
#include <stdint.h>

namespace clv
{
	namespace subtitle
	{
		inline constexpr int64_t kTicksPerMs = 1081080; /* 1 s = 1 081 080 000 ticks，整除到毫秒 */
		inline constexpr uint32_t kStyleBitCount = 13u;

		/* 最终样式值：字段与公开结构同序同名，缺 struct_size 一格 */
		struct StyleValue
		{
			uint32_t flags;
			uint32_t font_color_rgba;
			uint32_t stroke_color_rgba;
			uint16_t font_size;
			uint8_t font_size_unit;
			uint16_t font_weight;
			uint8_t font_style;
			uint8_t text_align;
			int32_t pos_x;
			int32_t pos_y;
			uint8_t pos_unit;
			uint16_t line_height;
			uint8_t line_height_unit;
			uint16_t opacity;
			uint8_t scroll;
			int32_t scroll_x;
			int32_t scroll_y;
			int32_t scroll_w;
			int32_t scroll_h;
			uint8_t scroll_unit;
			uint16_t stroke_width;
			const uint8_t* font_family;
			uint32_t font_family_size;
		};

		struct Block
		{
			uint32_t id;
			int64_t start_ticks;
			int64_t end_ticks;
			uint8_t displayable;
			uint32_t reason_mask;
			uint64_t style_source;
			StyleValue style;
			uint32_t first_line;
			uint32_t line_count;
		};

		struct Line
		{
			uint32_t block_id; /* 所属块的 id */
			const uint8_t* text;
			uint32_t size;
			const CLV_SubtitleSpanC* spans;
			uint32_t span_count;
			uint8_t scope_ok;
		};

		struct Stats
		{
			uint32_t packets_seen;
			uint32_t packets_bad_header;
			uint32_t packets_too_large;
			uint32_t packets_bad_utf8;
			uint32_t packets_translated;
			uint32_t blocks_total;
			uint32_t blocks_unusable;
			uint32_t unusable_reason_mask;
			uint32_t blocks_time_overflow;
			uint32_t blocks_negative_tick;
			uint32_t lines_total;
			uint32_t lines_ignored;
			uint32_t resets;
		};

		struct Parser;

		Parser* ParserCreate(const StyleValue* parser_default, uint32_t max_packet_bytes);
		void ParserDestroy(Parser* p);
		void ParserReset(Parser* p);

		/* 全局样式块进覆盖链第 2 档；传 NULL 撤销。只影响此后闭合的块。
		 * `font_family` 按 (ptr, len) 出借，宿主保证在解析器存活期间有效。 */
		void ParserSetStyleBlock(Parser* p, const StyleBlock* block);

		Err ParserFeed(Parser* p, const uint8_t* payload, size_t size, int64_t container_pts);

		uint32_t ParserBlockCount(const Parser* p);
		const Block* ParserBlockAt(const Parser* p, uint32_t index);
		uint32_t ParserLineCount(const Parser* p);
		const Line* ParserLineAt(const Parser* p, uint32_t index);
		void ParserStats(const Parser* p, Stats* out);

		/* 六档链：built_in → renderer_default → style_block → parser_default → packet，
		 * 块属性在 inout_style->flags 里，永远最高、不被覆盖。未获值的字段记 unset（6）。 */
		void ResolveStyleChain(StyleValue* inout_style, uint64_t* inout_source, const StyleValue* built_in,
							   const StyleValue* renderer_default, const StyleBlock* style_block,
							   const StyleValue* parser_default, const PacketHeader* packet);

		uint32_t StyleSourceGet(uint64_t packed, uint32_t bit);
	}	 // namespace subtitle
}	 // namespace clv

#endif	  // CLV_SUBTITLE_PARSER_H
