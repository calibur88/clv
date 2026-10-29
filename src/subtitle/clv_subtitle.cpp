/* 字幕面 C ABI 门面：公开结构 ↔ 内部视图的转换、入参校验与错误码落点。
 * 句柄与 CLV_File 同法：公开名只是标签，实体是内部类型，靠 reinterpret_cast 互转。
 * 渲染器未编入时各口按 UNSUPPORTED 回报。 */
#include "CLV_Subtitle.h"

#include "parser.h"
#include "render/renderer.h"
#include "style.h"

#include <cstring>

namespace clv
{
	namespace subtitle
	{
		namespace
		{
			bool StructSizeOk(uint32_t size, size_t expect)
			{
				return size == 0u || size == static_cast<uint32_t>(expect);
			}

			Parser* ToParser(CLV_SubtitleParser* p) { return reinterpret_cast<Parser*>(p); }

			const Parser* ToParser(const CLV_SubtitleParser* p) { return reinterpret_cast<const Parser*>(p); }

			CLV_SubtitleParser* ToHandle(Parser* p) { return reinterpret_cast<CLV_SubtitleParser*>(p); }

			bool StyleValid(const CLV_SubtitleStyleC& s)
			{
				if ((s.flags & ~CLV_SUBTITLE_STYLE_ALL) != 0u) return false;
				if (s.font_size_unit > CLV_SUBTITLE_UNIT_PX) return false;
				if (s.pos_unit > CLV_SUBTITLE_UNIT_PX) return false;
				if (s.line_height_unit > CLV_SUBTITLE_UNIT_PX) return false;
				if (s.scroll_unit > CLV_SUBTITLE_UNIT_PX) return false;
				if (s.text_align > CLV_SUBTITLE_ALIGN_RIGHT) return false;
				if (s.scroll > CLV_SUBTITLE_SCROLL_RIGHT) return false;
				if (s.opacity > 1000u) return false;
				if (s.font_style > 1u) return false;
				if (s.font_weight > 900u) return false;
				if ((s.flags & CLV_SUBTITLE_STYLE_FONT_FAMILY) != 0u)
				{
					if (s.font_family == nullptr || s.font_family_size == 0u) return false;
					if (! Utf8Valid(s.font_family, s.font_family_size)) return false;
				}
				return true;
			}

			StyleValue ToInternal(const CLV_SubtitleStyleC& s)
			{
				StyleValue v {};
				v.flags = s.flags;
				v.font_color_rgba = s.font_color_rgba;
				v.stroke_color_rgba = s.stroke_color_rgba;
				v.font_size = s.font_size;
				v.font_size_unit = s.font_size_unit;
				v.font_weight = s.font_weight;
				v.font_style = s.font_style;
				v.text_align = s.text_align;
				v.pos_x = s.pos_x;
				v.pos_y = s.pos_y;
				v.pos_unit = s.pos_unit;
				v.line_height = s.line_height;
				v.line_height_unit = s.line_height_unit;
				v.opacity = s.opacity;
				v.scroll = s.scroll;
				v.scroll_x = s.scroll_x;
				v.scroll_y = s.scroll_y;
				v.scroll_w = s.scroll_w;
				v.scroll_h = s.scroll_h;
				v.scroll_unit = s.scroll_unit;
				v.stroke_width = s.stroke_width;
				v.font_family = s.font_family;
				v.font_family_size = s.font_family_size;
				return v;
			}

			void ToPublic(const StyleValue& v, CLV_SubtitleStyleC* out)
			{
				std::memset(out, 0, sizeof(*out));
				out->struct_size = sizeof(CLV_SubtitleStyleC);
				out->flags = v.flags;
				out->font_color_rgba = v.font_color_rgba;
				out->stroke_color_rgba = v.stroke_color_rgba;
				out->font_size = v.font_size;
				out->font_size_unit = v.font_size_unit;
				out->font_weight = v.font_weight;
				out->font_style = v.font_style;
				out->text_align = v.text_align;
				out->pos_x = v.pos_x;
				out->pos_y = v.pos_y;
				out->pos_unit = v.pos_unit;
				out->line_height = v.line_height;
				out->line_height_unit = v.line_height_unit;
				out->opacity = v.opacity;
				out->scroll = v.scroll;
				out->scroll_x = v.scroll_x;
				out->scroll_y = v.scroll_y;
				out->scroll_w = v.scroll_w;
				out->scroll_h = v.scroll_h;
				out->scroll_unit = v.scroll_unit;
				out->stroke_width = v.stroke_width;
				out->font_family = v.font_family;
				out->font_family_size = v.font_family_size;
			}
		}	 // namespace
	}	 // namespace subtitle
}	 // namespace clv

extern "C"
{
	using clv::subtitle::Block;
	using clv::subtitle::Line;
	using clv::subtitle::Parser;
	using clv::subtitle::render::Renderer;
	using clv::subtitle::Stats;
	using clv::subtitle::StyleValue;

	const char* CLV_SubtitleStrError(CLV_SubtitleError err)
	{
		switch (err)
		{
		case CLV_SUBTITLE_OK: return "ok";
		case CLV_SUBTITLE_INVALID_ARGUMENT: return "invalid argument";
		case CLV_SUBTITLE_INVALID_HANDLE: return "invalid handle";
		case CLV_SUBTITLE_INVALID_UTF8: return "invalid utf-8";
		case CLV_SUBTITLE_PACKET_HEADER_BAD: return "packet header bad";
		case CLV_SUBTITLE_PACKET_TOO_LARGE: return "packet too large";
		case CLV_SUBTITLE_OUT_OF_MEMORY: return "out of memory";
		case CLV_SUBTITLE_STATE: return "state";
		case CLV_SUBTITLE_BACKEND_FAILURE: return "backend failure";
		case CLV_SUBTITLE_RENDER_FAILED: return "render failed";
		case CLV_SUBTITLE_UNSUPPORTED: return "unsupported";
		case CLV_SUBTITLE_OUTPUT_TOO_SMALL: return "output too small";
		case CLV_SUBTITLE_STYLE_BLOCK_BAD: return "style block bad";
		default: return "unknown";
		}
	}

	CLV_SubtitleParser* CLV_SubtitleParserOpen(const CLV_SubtitleParserParamsC* params, CLV_SubtitleError* err)
	{
		if (err != nullptr) *err = CLV_SUBTITLE_OK;
		if (params == nullptr)
		{
			if (err != nullptr) *err = CLV_SUBTITLE_INVALID_ARGUMENT;
			return nullptr;
		}
		if (! clv::subtitle::StructSizeOk(params->struct_size, sizeof(CLV_SubtitleParserParamsC)) ||
			params->reserved0 != 0u)
		{
			if (err != nullptr) *err = CLV_SUBTITLE_INVALID_ARGUMENT;
			return nullptr;
		}

		StyleValue def {};
		const StyleValue* def_ptr = nullptr;
		if (params->parser_default != nullptr)
		{
			const CLV_SubtitleStyleC* s = params->parser_default;
			if (! clv::subtitle::StructSizeOk(s->struct_size, sizeof(CLV_SubtitleStyleC)) ||
				! clv::subtitle::StyleValid(*s))
			{
				if (err != nullptr) *err = CLV_SUBTITLE_INVALID_ARGUMENT;
				return nullptr;
			}
			def = clv::subtitle::ToInternal(*s);
			def_ptr = &def;
		}

		Parser* p = clv::subtitle::ParserCreate(def_ptr, params->max_packet_bytes);
		if (p == nullptr)
		{
			if (err != nullptr) *err = CLV_SUBTITLE_OUT_OF_MEMORY;
			return nullptr;
		}
		return clv::subtitle::ToHandle(p);
	}

	CLV_SubtitleError CLV_SubtitleParserFeed(CLV_SubtitleParser* p, const uint8_t* payload, size_t size,
											 int64_t container_pts)
	{
		return clv::subtitle::ParserFeed(clv::subtitle::ToParser(p), payload, size, container_pts);
	}

	CLV_SubtitleError CLV_SubtitleParserSetStyleBlock(CLV_SubtitleParser* p, const uint8_t* block, size_t size)
	{
		if (p == nullptr) return CLV_SUBTITLE_INVALID_HANDLE;
		if (block == nullptr || size == 0u)
		{
			clv::subtitle::ParserSetStyleBlock(clv::subtitle::ToParser(p), nullptr);
			return CLV_SUBTITLE_OK;
		}
		clv::subtitle::StyleBlock sb {};
		const clv::subtitle::Err e = clv::subtitle::ParseStyleBlock(block, size, &sb);
		if (e != clv::subtitle::kOk) return e;
		clv::subtitle::ParserSetStyleBlock(clv::subtitle::ToParser(p), &sb);
		return CLV_SUBTITLE_OK;
	}

	CLV_SubtitleError CLV_SubtitleParserReset(CLV_SubtitleParser* p)
	{
		if (p == nullptr) return CLV_SUBTITLE_INVALID_HANDLE;
		clv::subtitle::ParserReset(clv::subtitle::ToParser(p));
		return CLV_SUBTITLE_OK;
	}

	CLV_SubtitleError CLV_SubtitleParserBlockCount(const CLV_SubtitleParser* p, uint32_t* out_count)
	{
		if (p == nullptr) return CLV_SUBTITLE_INVALID_HANDLE;
		if (out_count == nullptr) return CLV_SUBTITLE_INVALID_ARGUMENT;
		*out_count = clv::subtitle::ParserBlockCount(clv::subtitle::ToParser(p));
		return CLV_SUBTITLE_OK;
	}

	CLV_SubtitleError CLV_SubtitleParserBlockAt(const CLV_SubtitleParser* p, uint32_t index, CLV_SubtitleBlockC* out)
	{
		if (p == nullptr) return CLV_SUBTITLE_INVALID_HANDLE;
		if (out == nullptr) return CLV_SUBTITLE_INVALID_ARGUMENT;
		if (! clv::subtitle::StructSizeOk(out->struct_size, sizeof(CLV_SubtitleBlockC)))
			return CLV_SUBTITLE_INVALID_ARGUMENT;
		const Block* b = clv::subtitle::ParserBlockAt(clv::subtitle::ToParser(p), index);
		if (b == nullptr) return CLV_SUBTITLE_INVALID_ARGUMENT;

		std::memset(out, 0, sizeof(*out));
		out->struct_size = sizeof(CLV_SubtitleBlockC);
		out->id = b->id;
		out->start_ticks = b->start_ticks;
		out->end_ticks = b->end_ticks;
		out->displayable = b->displayable != 0u ? 1u : 0u;
		out->reason_mask = b->reason_mask;
		out->style_source = b->style_source;
		clv::subtitle::ToPublic(b->style, &out->style);
		out->first_line = b->first_line;
		out->line_count = b->line_count;
		return CLV_SUBTITLE_OK;
	}

	CLV_SubtitleError CLV_SubtitleParserLineCount(const CLV_SubtitleParser* p, uint32_t* out_count)
	{
		if (p == nullptr) return CLV_SUBTITLE_INVALID_HANDLE;
		if (out_count == nullptr) return CLV_SUBTITLE_INVALID_ARGUMENT;
		*out_count = clv::subtitle::ParserLineCount(clv::subtitle::ToParser(p));
		return CLV_SUBTITLE_OK;
	}

	CLV_SubtitleError CLV_SubtitleParserLineAt(const CLV_SubtitleParser* p, uint32_t index, CLV_SubtitleLineC* out)
	{
		if (p == nullptr) return CLV_SUBTITLE_INVALID_HANDLE;
		if (out == nullptr) return CLV_SUBTITLE_INVALID_ARGUMENT;
		if (! clv::subtitle::StructSizeOk(out->struct_size, sizeof(CLV_SubtitleLineC)))
			return CLV_SUBTITLE_INVALID_ARGUMENT;
		const Line* l = clv::subtitle::ParserLineAt(clv::subtitle::ToParser(p), index);
		if (l == nullptr) return CLV_SUBTITLE_INVALID_ARGUMENT;

		std::memset(out, 0, sizeof(*out));
		out->struct_size = sizeof(CLV_SubtitleLineC);
		out->block_id = l->block_id;
		out->text = l->text;
		out->text_size = l->size;
		out->spans = nullptr;
		out->span_count = 0u;
		if (l->span_count != 0u)
		{
			out->spans = l->spans;
			out->span_count = l->span_count;
		}
		out->scope_ok = l->scope_ok != 0u ? 1u : 0u;
		return CLV_SUBTITLE_OK;
	}

	CLV_SubtitleError CLV_SubtitleParserStats(const CLV_SubtitleParser* p, CLV_SubtitleStatsC* out)
	{
		if (p == nullptr) return CLV_SUBTITLE_INVALID_HANDLE;
		if (out == nullptr) return CLV_SUBTITLE_INVALID_ARGUMENT;
		if (! clv::subtitle::StructSizeOk(out->struct_size, sizeof(CLV_SubtitleStatsC)))
			return CLV_SUBTITLE_INVALID_ARGUMENT;
		Stats st {};
		clv::subtitle::ParserStats(clv::subtitle::ToParser(p), &st);

		std::memset(out, 0, sizeof(*out));
		out->struct_size = sizeof(CLV_SubtitleStatsC);
		out->packets_seen = st.packets_seen;
		out->packets_bad_header = st.packets_bad_header;
		out->packets_too_large = st.packets_too_large;
		out->packets_bad_utf8 = st.packets_bad_utf8;
		out->packets_translated = st.packets_translated;
		out->blocks_total = st.blocks_total;
		out->blocks_unusable = st.blocks_unusable;
		out->unusable_reason_mask = st.unusable_reason_mask;
		out->blocks_time_overflow = st.blocks_time_overflow;
		out->blocks_negative_tick = st.blocks_negative_tick;
		out->lines_total = st.lines_total;
		out->lines_ignored = st.lines_ignored;
		out->resets = st.resets;
		return CLV_SUBTITLE_OK;
	}

	void CLV_SubtitleParserClose(CLV_SubtitleParser* p) { clv::subtitle::ParserDestroy(clv::subtitle::ToParser(p)); }

#if CLV_SUBTITLE_RENDERER_ENABLED
	namespace
	{
		Renderer* ToRenderer(CLV_SubtitleRenderer* r) { return reinterpret_cast<Renderer*>(r); }

		const Renderer* ToRendererConst(const CLV_SubtitleRenderer* r) { return reinterpret_cast<const Renderer*>(r); }

		CLV_SubtitleRenderer* RendererHandle(Renderer* r) { return reinterpret_cast<CLV_SubtitleRenderer*>(r); }
	}	 // namespace

	CLV_SubtitleRenderer* CLV_SubtitleRendererOpen(const CLV_SubtitleRendererParamsC* params, CLV_SubtitleError* err)
	{
		if (err != nullptr) *err = CLV_SUBTITLE_OK;
		if (params == nullptr)
		{
			if (err != nullptr) *err = CLV_SUBTITLE_INVALID_ARGUMENT;
			return nullptr;
		}
		if (! clv::subtitle::StructSizeOk(params->struct_size, sizeof(CLV_SubtitleRendererParamsC)) ||
			params->width == 0u || params->height == 0u || params->metric_entry > CLV_SUBTITLE_METRIC_HB_FT ||
			params->flags != 0u)
		{
			if (err != nullptr) *err = CLV_SUBTITLE_INVALID_ARGUMENT;
			return nullptr;
		}
		if (params->renderer_default != nullptr &&
			(! clv::subtitle::StructSizeOk(params->renderer_default->struct_size, sizeof(CLV_SubtitleStyleC)) ||
			 ! clv::subtitle::StyleValid(*params->renderer_default)))
		{
			if (err != nullptr) *err = CLV_SUBTITLE_INVALID_ARGUMENT;
			return nullptr;
		}
		Renderer* r = Renderer::Create(params->width, params->height, static_cast<int>(params->metric_entry),
									   params->renderer_default);
		if (r == nullptr)
		{
			if (err != nullptr) *err = CLV_SUBTITLE_OUT_OF_MEMORY;
			return nullptr;
		}
		return RendererHandle(r);
	}

	CLV_SubtitleError CLV_SubtitleRendererSetStyleBlock(CLV_SubtitleRenderer* r, const uint8_t* block, size_t size)
	{
		if (r == nullptr) return CLV_SUBTITLE_INVALID_HANDLE;
		return ToRenderer(r)->SetStyleBlock(block, size);
	}

	CLV_SubtitleError CLV_SubtitleRendererSetFontDir(CLV_SubtitleRenderer* r, const char* utf8_dir)
	{
		if (r == nullptr) return CLV_SUBTITLE_INVALID_HANDLE;
		return ToRenderer(r)->AddFontDir(utf8_dir);
	}

	CLV_SubtitleError CLV_SubtitleRendererLayout(CLV_SubtitleRenderer* r, const CLV_SubtitleBlockC* block,
												 const CLV_SubtitleLineC* lines, uint32_t line_count,
												 const CLV_SubtitleStyleC* style, int64_t time_ticks)
	{
		if (r == nullptr) return CLV_SUBTITLE_INVALID_HANDLE;
		if (block == nullptr || (lines == nullptr && line_count != 0u)) return CLV_SUBTITLE_INVALID_ARGUMENT;
		if (! clv::subtitle::StructSizeOk(block->struct_size, sizeof(CLV_SubtitleBlockC)))
			return CLV_SUBTITLE_INVALID_ARGUMENT;
		if (style != nullptr && ! clv::subtitle::StructSizeOk(style->struct_size, sizeof(CLV_SubtitleStyleC)))
			return CLV_SUBTITLE_INVALID_ARGUMENT;
		return ToRenderer(r)->Layout(*block, lines, line_count, style, time_ticks);
	}

	CLV_SubtitleError CLV_SubtitleRendererDirtyRects(const CLV_SubtitleRenderer* r, CLV_SubtitleRectC* rects,
													 uint32_t capacity, uint32_t* out_total)
	{
		if (r == nullptr) return CLV_SUBTITLE_INVALID_HANDLE;
		return ToRendererConst(r)->DirtyRects(rects, capacity, out_total);
	}

	CLV_SubtitleError CLV_SubtitleRendererRender(CLV_SubtitleRenderer* r, uint8_t* rgba, uint32_t stride_bytes,
												 uint32_t width, uint32_t height)
	{
		if (r == nullptr) return CLV_SUBTITLE_INVALID_HANDLE;
		return ToRenderer(r)->Render(rgba, stride_bytes, width, height);
	}

	CLV_SubtitleError CLV_SubtitleRendererReport(const CLV_SubtitleRenderer* r, CLV_SubtitleRenderStatsC* out)
	{
		if (r == nullptr) return CLV_SUBTITLE_INVALID_HANDLE;
		if (out == nullptr) return CLV_SUBTITLE_INVALID_ARGUMENT;
		if (! clv::subtitle::StructSizeOk(out->struct_size, sizeof(CLV_SubtitleRenderStatsC)))
			return CLV_SUBTITLE_INVALID_ARGUMENT;
		return ToRendererConst(r)->Report(out);
	}

	void CLV_SubtitleRendererClose(CLV_SubtitleRenderer* r) { delete ToRenderer(r); }
#else
	CLV_SubtitleRenderer* CLV_SubtitleRendererOpen(const CLV_SubtitleRendererParamsC* params, CLV_SubtitleError* err)
	{
		(void) params;
		if (err != nullptr) *err = CLV_SUBTITLE_UNSUPPORTED;
		return nullptr;
	}

	CLV_SubtitleError CLV_SubtitleRendererSetStyleBlock(CLV_SubtitleRenderer* r, const uint8_t* block, size_t size)
	{
		(void) r;
		(void) block;
		(void) size;
		return CLV_SUBTITLE_UNSUPPORTED;
	}

	CLV_SubtitleError CLV_SubtitleRendererSetFontDir(CLV_SubtitleRenderer* r, const char* utf8_dir)
	{
		(void) r;
		(void) utf8_dir;
		return CLV_SUBTITLE_UNSUPPORTED;
	}

	CLV_SubtitleError CLV_SubtitleRendererLayout(CLV_SubtitleRenderer* r, const CLV_SubtitleBlockC* block,
												 const CLV_SubtitleLineC* lines, uint32_t line_count,
												 const CLV_SubtitleStyleC* style, int64_t time_ticks)
	{
		(void) r;
		(void) block;
		(void) lines;
		(void) line_count;
		(void) style;
		(void) time_ticks;
		return CLV_SUBTITLE_UNSUPPORTED;
	}

	CLV_SubtitleError CLV_SubtitleRendererDirtyRects(const CLV_SubtitleRenderer* r, CLV_SubtitleRectC* rects,
													 uint32_t capacity, uint32_t* out_total)
	{
		(void) r;
		(void) rects;
		(void) capacity;
		(void) out_total;
		return CLV_SUBTITLE_UNSUPPORTED;
	}

	CLV_SubtitleError CLV_SubtitleRendererRender(CLV_SubtitleRenderer* r, uint8_t* rgba, uint32_t stride_bytes,
												 uint32_t width, uint32_t height)
	{
		(void) r;
		(void) rgba;
		(void) stride_bytes;
		(void) width;
		(void) height;
		return CLV_SUBTITLE_UNSUPPORTED;
	}

	CLV_SubtitleError CLV_SubtitleRendererReport(const CLV_SubtitleRenderer* r, CLV_SubtitleRenderStatsC* out)
	{
		(void) r;
		(void) out;
		return CLV_SUBTITLE_UNSUPPORTED;
	}

	void CLV_SubtitleRendererClose(CLV_SubtitleRenderer* r) { (void) r; }
#endif
}
