/* CLV_Subtitle.h
 *
 * CLV 字幕流（`stream_type = 2` / `ext_type = 4`）解析与渲染面的 C ABI 契约。
 * 解析器把字幕包载荷解成「块 + 文本行」，渲染器把块与样式排成 RGBA8 字幕层并给出脏矩形。
 * 函数名、结构体与出参形态都是对外 ABI 的一部分，改动即破坏调用方；错误码只在段尾追加，
 * 既有码值与语义不动。
 *
 * 面：不透明句柄 + 自由函数 + 整型错误码 + POD 描述符。零第三方类型、零平台类型、零文件接口。
 * 本面只有解析器与渲染器；不含播放器句柄、播放时钟、设备回调，也不做音视频混合——字幕层与
 * 视频帧的混合归宿主。
 *
 * 与容器的分工：一包一字幕 payload（含包外层头与文本），跨包的重组、片序与时间戳链归容器层；
 * 包内结构（包外层、块语法、样式块）只有字幕层认识，容器不解释。
 *
 * 时间轴：块的时间窗以 ticks 表示，按容器包的 pts 平移后给出；库不拥有播放时钟，不做换算、不做调度。
 * 平移后为负的块照样交出、标记为不可显示，不判错、不丢弃。
 *
 * 整数域：本面不出现浮点。百分比一律按千分比整数表示（1000 = 100%），像素按整数。
 *
 * 所有权：解析器与渲染器持有自己解析/布局出的文本与样式，返回值里的指针只到下一次 Feed / Reset /
 * Layout / Close 为止有效，调用方要留内容自己拷。字符串入参按 (ptr, len) 出借，所有权归调用方。
 *
 * 句柄与线程：所有句柄按单线程使用计，同一句柄不得被两个线程同时调用；不同句柄之间无共享可变状态。
 *
 * 数组与容量：输出数组两趟——先按 capacity = 0 要总数（不写数组），再按容量填；容量不足返回
 * CLV_SUBTITLE_OUTPUT_TOO_SMALL 并把实际总数写进出参。
 *
 * 结构体版本：首字段恒为 struct_size；传 0 按本布局受理，传 sizeof(本结构) 显式声明本布局，
 * 其余任何值判 CLV_SUBTITLE_INVALID_ARGUMENT。库不按字节长度猜版本。
 */
#ifndef CLV_SUBTITLE_H
#define CLV_SUBTITLE_H

#include "CLV_Export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	/* ---- 错误码（段 106–117；0 = OK；118+ 留白、不复用、不因任何退场而前移） ---- */
	typedef enum CLV_SubtitleError
	{
		CLV_SUBTITLE_OK = 0,
		CLV_SUBTITLE_INVALID_ARGUMENT = 106,
		CLV_SUBTITLE_INVALID_HANDLE = 107,
		CLV_SUBTITLE_INVALID_UTF8 = 108,
		CLV_SUBTITLE_PACKET_HEADER_BAD = 109,
		CLV_SUBTITLE_PACKET_TOO_LARGE = 110,
		CLV_SUBTITLE_OUT_OF_MEMORY = 111,
		CLV_SUBTITLE_STATE = 112,
		CLV_SUBTITLE_BACKEND_FAILURE = 113,
		CLV_SUBTITLE_RENDER_FAILED = 114,
		CLV_SUBTITLE_UNSUPPORTED = 115,
		CLV_SUBTITLE_OUTPUT_TOO_SMALL = 116,
		CLV_SUBTITLE_STYLE_BLOCK_BAD = 117
	} CLV_SubtitleError;

	/* 每个错误值返回非空静态 ASCII 串；未知值返回 "unknown"。永不返回 NULL。 */
	CLV_EXPORT const char* CLV_SubtitleStrError(CLV_SubtitleError err);

	typedef struct CLV_SubtitleParser CLV_SubtitleParser;
	typedef struct CLV_SubtitleRenderer CLV_SubtitleRenderer;

	/* ---- 样式字段位（`CLV_SubtitleStyleC.flags` 与 `style_source` 的位序；一位一字段，共 13 位） ---- */
	enum
	{
		CLV_SUBTITLE_STYLE_FONT_COLOR = 1u << 0,
		CLV_SUBTITLE_STYLE_STROKE_COLOR = 1u << 1,
		CLV_SUBTITLE_STYLE_FONT_SIZE = 1u << 2,
		CLV_SUBTITLE_STYLE_FONT_WEIGHT = 1u << 3,
		CLV_SUBTITLE_STYLE_FONT_STYLE = 1u << 4,
		CLV_SUBTITLE_STYLE_TEXT_ALIGN = 1u << 5,
		CLV_SUBTITLE_STYLE_POSITION = 1u << 6,
		CLV_SUBTITLE_STYLE_LINE_HEIGHT = 1u << 7,
		CLV_SUBTITLE_STYLE_OPACITY = 1u << 8,
		CLV_SUBTITLE_STYLE_SCROLL = 1u << 9,
		CLV_SUBTITLE_STYLE_SCROLL_RECT = 1u << 10,
		CLV_SUBTITLE_STYLE_FONT_FAMILY = 1u << 11,
		CLV_SUBTITLE_STYLE_STROKE_WIDTH = 1u << 12,
		CLV_SUBTITLE_STYLE_ALL = (1u << 13) - 1u
	};

	/* 长度单位：px = 视频像素；pct = 千分比（字号 / 行高相对字号，坐标与矩形相对屏幕） */
	typedef enum CLV_SubtitleUnit
	{
		CLV_SUBTITLE_UNIT_PCT = 0,
		CLV_SUBTITLE_UNIT_PX = 1
	} CLV_SubtitleUnit;

	typedef enum CLV_SubtitleAlign
	{
		CLV_SUBTITLE_ALIGN_LEFT = 0,
		CLV_SUBTITLE_ALIGN_CENTER = 1,
		CLV_SUBTITLE_ALIGN_RIGHT = 2
	} CLV_SubtitleAlign;

	typedef enum CLV_SubtitleScroll
	{
		CLV_SUBTITLE_SCROLL_NONE = 0,
		CLV_SUBTITLE_SCROLL_TOP = 1,
		CLV_SUBTITLE_SCROLL_BOTTOM = 2,
		CLV_SUBTITLE_SCROLL_LEFT = 3,
		CLV_SUBTITLE_SCROLL_RIGHT = 4
	} CLV_SubtitleScroll;

	/* 覆盖链：数值即优先级，值大的赢；6 = unset（标记，不参与赢档比较）；7 保留、不产出 */
	typedef enum CLV_SubtitleStyleSource
	{
		CLV_SUBTITLE_SRC_BUILT_IN = 0,
		CLV_SUBTITLE_SRC_RENDERER = 1,
		CLV_SUBTITLE_SRC_STYLE_BLOCK = 2,
		CLV_SUBTITLE_SRC_PARSER = 3,
		CLV_SUBTITLE_SRC_PACKET = 4,
		CLV_SUBTITLE_SRC_BLOCK_ATTR = 5,
		CLV_SUBTITLE_SRC_UNSET = 6,
		CLV_SUBTITLE_SRC_RESERVED = 7
	} CLV_SubtitleStyleSource;

	/* ---- 样式值：整数域；字符串按 (ptr, len) 出借，所有权归调用方 ---- */
	typedef struct CLV_SubtitleStyleC
	{
		uint32_t struct_size;
		uint32_t flags;			  /* CLV_SUBTITLE_STYLE_* 位集；未置位的字段不采纳 */
		uint32_t font_color_rgba; /* R 在低 8 位起（R,G,B,A 依次向高位） */
		uint32_t stroke_color_rgba;
		uint16_t font_size;		/* 千分比或像素，见 font_size_unit */
		uint8_t font_size_unit; /* CLV_SubtitleUnit */
		uint16_t font_weight;	/* 100–900；0 = normal */
		uint8_t font_style;		/* 0 = normal，1 = italic */
		uint8_t text_align;		/* CLV_SubtitleAlign */
		int32_t pos_x;			/* 块中心；原点左上、x 向右 */
		int32_t pos_y;
		uint8_t pos_unit;	  /* CLV_SubtitleUnit */
		uint16_t line_height; /* 千分比（相对字号）或像素 */
		uint8_t line_height_unit;
		uint16_t opacity; /* 0..1000 千分（1000 = 1.0） */
		uint8_t scroll;	  /* CLV_SubtitleScroll */
		int32_t scroll_x; /* 滚动视口；四分量同单位 */
		int32_t scroll_y;
		int32_t scroll_w;
		int32_t scroll_h;
		uint8_t scroll_unit;
		uint16_t stroke_width;		/* 像素；0 = 不描边 */
		const uint8_t* font_family; /* UTF-8、逗号分隔的族链；不保证 NUL 结尾 */
		uint32_t font_family_size;
	} CLV_SubtitleStyleC;

/* ---- 块视图 ---- */
#define CLV_SUBTITLE_BLOCK_REASON_SYNTAX_IGNORE 0x1u /* 语法忽略：ID 非法 / 序违规 / 未闭合 */
#define CLV_SUBTITLE_BLOCK_REASON_TIME_HIDDEN	0x2u /* 时间不显示：缺 time / end ≤ start / 整段在过去 */

	typedef struct CLV_SubtitleBlockC
	{
		uint32_t struct_size;
		uint32_t id;
		int64_t start_ticks; /* 已按容器 pts 平移，可为负 */
		int64_t end_ticks;
		uint32_t displayable;	  /* reason_mask == 0 */
		uint32_t reason_mask;	  /* 见 CLV_SUBTITLE_BLOCK_REASON_*；bit2–31 必须 0 */
		uint64_t style_source;	  /* 每 3 bit 一位字段来源，13 字段占 0–38 位；39–63 必须 0 */
		CLV_SubtitleStyleC style; /* 已按覆盖链解析出的最终样式 */
		uint32_t first_line;
		uint32_t line_count;
	} CLV_SubtitleBlockC;

	/* 解码后文本的段标注：普通文字与 `\emoji` 指令路由段分开取面 */
	typedef enum CLV_SubtitleSpanKind
	{
		CLV_SUBTITLE_SPAN_TEXT = 0,
		CLV_SUBTITLE_SPAN_EMOJI = 1
	} CLV_SubtitleSpanKind;

	typedef struct CLV_SubtitleSpanC
	{
		uint32_t off;  /* 解码后字节串内的起始 */
		uint32_t len;  /* 字节数；0 长度的段不产出 */
		uint32_t kind; /* CLV_SubtitleSpanKind */
		uint32_t reserved0;
	} CLV_SubtitleSpanC;

	typedef struct CLV_SubtitleLineC
	{
		uint32_t struct_size;
		uint32_t block_id;	 /* 该行所属块的 id */
		const uint8_t* text; /* 转义解码后的字节，归解析器所有，与句柄同生命周期 */
		uint32_t text_size;
		const CLV_SubtitleSpanC* spans; /* 与 text 同生命周期；无指令时 span_count = 0 */
		uint32_t span_count;
		uint32_t scope_ok; /* 落在某块定义之后、下一块定义之前 */
	} CLV_SubtitleLineC;

	typedef struct CLV_SubtitleStatsC
	{
		uint32_t struct_size;
		uint32_t packets_seen;
		uint32_t packets_bad_header;
		uint32_t packets_too_large;
		uint32_t packets_bad_utf8;
		uint32_t packets_translated; /* start_time != 容器 pts */
		uint32_t blocks_total;
		uint32_t blocks_unusable;	   /* 不可用块数；未闭合块只进此计数、不进块视图 */
		uint32_t unusable_reason_mask; /* 原因按位或；bit2–31 恒 0 */
		uint32_t blocks_time_overflow; /* 平移后越出 int64：丢块 */
		uint32_t blocks_negative_tick; /* 平移后为负：不显示、不判错、不丢 */
		uint32_t lines_total;
		uint32_t lines_ignored;
		uint32_t resets;
	} CLV_SubtitleStatsC;

	typedef struct CLV_SubtitleRectC
	{
		int32_t x, y;
		uint32_t w, h;
	} CLV_SubtitleRectC;

	typedef struct CLV_SubtitleRenderStatsC
	{
		uint32_t struct_size;
		uint32_t glyphs;
		uint32_t cache_hits;
		uint32_t fallback_fonts;
		uint32_t missing_glyphs;
		uint32_t style_blocks_applied;
		uint32_t reserved0[2];
	} CLV_SubtitleRenderStatsC;

	/* ---- 解析器 ---- */
	typedef struct CLV_SubtitleParserParamsC
	{
		uint32_t struct_size;
		uint32_t max_packet_bytes;				  /* 0 = 库缺省 4096 */
		const CLV_SubtitleStyleC* parser_default; /* 可为 NULL：解析器缺省样式（覆盖链第 3 档） */
		uint32_t reserved0;
	} CLV_SubtitleParserParamsC;

	CLV_EXPORT CLV_SubtitleParser* CLV_SubtitleParserOpen(const CLV_SubtitleParserParamsC* params,
														  CLV_SubtitleError* err);

	/* 喂一包字幕 payload（容器 `stream_type = 2` 流的 `ext_type = 4` 载荷本体）。
	 * `container_pts` = 容器包的时间戳（与载荷里的 start_time 同单位），用于按包平移块级 time。
	 * 坏 UTF-8 丢整包并计数、返 108；结构问题各返其码。已解析的块在下一次 Feed / Reset 前有效。 */
	CLV_EXPORT CLV_SubtitleError CLV_SubtitleParserFeed(CLV_SubtitleParser* p, const uint8_t* payload, size_t size,
														int64_t container_pts);

	/* 容器级全局样式块：载荷按线上字节布局原样传入（20 字节定长头 + 可选字族串）。传 NULL / size 0 撤销。
	 * 缓冲归调用方所有：库只记指针与长度、不复制，块内字族串按 (ptr, len) 出借，故 `block` 须存活到本句柄
	 * `Close` 或下一次 `SetStyleBlock` 覆盖之后；存续期内改动或释放它，之后的解析会读到失效内存。 */
	CLV_EXPORT CLV_SubtitleError CLV_SubtitleParserSetStyleBlock(CLV_SubtitleParser* p, const uint8_t* block,
																 size_t size);

	CLV_EXPORT CLV_SubtitleError CLV_SubtitleParserReset(CLV_SubtitleParser* p);

	CLV_EXPORT CLV_SubtitleError CLV_SubtitleParserBlockCount(const CLV_SubtitleParser* p, uint32_t* out_count);
	CLV_EXPORT CLV_SubtitleError CLV_SubtitleParserBlockAt(const CLV_SubtitleParser* p, uint32_t index,
														   CLV_SubtitleBlockC* out);
	CLV_EXPORT CLV_SubtitleError CLV_SubtitleParserLineCount(const CLV_SubtitleParser* p, uint32_t* out_count);
	CLV_EXPORT CLV_SubtitleError CLV_SubtitleParserLineAt(const CLV_SubtitleParser* p, uint32_t index,
														  CLV_SubtitleLineC* out);
	CLV_EXPORT CLV_SubtitleError CLV_SubtitleParserStats(const CLV_SubtitleParser* p, CLV_SubtitleStatsC* out);
	CLV_EXPORT void CLV_SubtitleParserClose(CLV_SubtitleParser* p);

	/* ---- 渲染器 ---- */
	typedef enum CLV_SubtitleMetricEntry
	{
		CLV_SUBTITLE_METRIC_HB_OT_FONT = 0,
		CLV_SUBTITLE_METRIC_HB_FT = 1
	} CLV_SubtitleMetricEntry;

	typedef struct CLV_SubtitleRendererParamsC
	{
		uint32_t struct_size;
		uint32_t width;
		uint32_t height;
		uint32_t metric_entry;
		uint32_t flags;								/* 保留，必须 0 */
		const CLV_SubtitleStyleC* renderer_default; /* 可为 NULL：覆盖链第 1 档 */
	} CLV_SubtitleRendererParamsC;

	/* 渲染器未编入本构建时返 NULL、`err = CLV_SUBTITLE_UNSUPPORTED`。 */
	CLV_EXPORT CLV_SubtitleRenderer* CLV_SubtitleRendererOpen(const CLV_SubtitleRendererParamsC* params,
															  CLV_SubtitleError* err);
	/* 渲染器侧的全局样式块：与解析器同一份载荷，按线上字节布局原样传入。缓冲同样归调用方、库不复制——
	 * 字族串要到 `Layout` 期才取，故 `block` 须存活到本渲染器 `Close` 或下一次设置之后。 */
	CLV_EXPORT CLV_SubtitleError CLV_SubtitleRendererSetStyleBlock(CLV_SubtitleRenderer* r, const uint8_t* block,
																   size_t size);
	/* 字体扫描目录：可多次调用，按加入顺序扫描（先命中的目录先用）；只影响之后的 Layout。 */
	CLV_EXPORT CLV_SubtitleError CLV_SubtitleRendererSetFontDir(CLV_SubtitleRenderer* r, const char* utf8_dir);

	/* 排一块的字：`lines` 是该块的文本行（按 `first_line` / `line_count` 从解析器取），
	 * `style` 非 NULL 时覆盖 `block->style`；`time_ticks` 与块时间窗同单位。 */
	CLV_EXPORT CLV_SubtitleError CLV_SubtitleRendererLayout(CLV_SubtitleRenderer* r, const CLV_SubtitleBlockC* block,
															const CLV_SubtitleLineC* lines, uint32_t line_count,
															const CLV_SubtitleStyleC* style, int64_t time_ticks);
	CLV_EXPORT CLV_SubtitleError CLV_SubtitleRendererDirtyRects(const CLV_SubtitleRenderer* r, CLV_SubtitleRectC* rects,
																uint32_t capacity, uint32_t* out_total);
	CLV_EXPORT CLV_SubtitleError CLV_SubtitleRendererRender(CLV_SubtitleRenderer* r, uint8_t* rgba,
															uint32_t stride_bytes, uint32_t width, uint32_t height);
	CLV_EXPORT CLV_SubtitleError CLV_SubtitleRendererReport(const CLV_SubtitleRenderer* r,
															CLV_SubtitleRenderStatsC* out);
	CLV_EXPORT void CLV_SubtitleRendererClose(CLV_SubtitleRenderer* r);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_SUBTITLE_H
