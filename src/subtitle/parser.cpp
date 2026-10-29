/* 字幕文档语法与流级状态。
 *
 * 行分类：空行 / 注释 / 块定义 / 属性行 / 文本行；块定义与文本行必须同包，
 * 块未闭合则整块忽略。块 ID 与 start 跨包单调（Reset 后首个块无条件当新基准）。
 *
 * 时间：文本里的 HH:MM:SS.mmm 折成 ticks 后按包级 delta 平移；平移越界丢块并计数，
 * 平移为负不判错、不丢，只标不显示。
 *
 * 样式：每个块闭合时当场走六档覆盖链（built_in → renderer_default → 全局样式块 →
 * parser_default → 包级 → 块属性），并把逐字段来源压进 style_source。之后新设的
 * 样式块不再回头改老块。
 */
#include "parser.h"

#include <cstring>
#include <deque>
#include <new>
#include <vector>

namespace clv
{
	namespace subtitle
	{
		namespace
		{
			constexpr uint32_t kNoBlock = 0xFFFFFFFFu;

			const uint8_t* FindSeq(const uint8_t* hay, size_t hn, const char* needle)
			{
				const size_t nl = std::strlen(needle);
				if (nl == 0u || hn < nl) return nullptr;
				for (size_t i = 0; i + nl <= hn; ++i)
				{
					size_t k = 0;
					while (k < nl && hay[i + k] == static_cast<uint8_t>(needle[k])) ++k;
					if (k == nl) return hay + i;
				}
				return nullptr;
			}

			bool IsBlankLine(const uint8_t* p, size_t n)
			{
				for (size_t i = 0; i < n; ++i)
					if (p[i] != static_cast<uint8_t>(' ') && p[i] != static_cast<uint8_t>('\t')) return false;
				return true;
			}

			bool IsDigit(uint8_t c) { return c >= static_cast<uint8_t>('0') && c <= static_cast<uint8_t>('9'); }

			uint8_t LowerOf(uint8_t c)
			{
				return (c >= static_cast<uint8_t>('A') && c <= static_cast<uint8_t>('Z')) ?
						   static_cast<uint8_t>(c + 32u) :
						   c;
			}

			/* digit+ → uint32；空串或超 2^32−1 回 false */
			bool ParseId(const uint8_t* p, size_t n, uint32_t* out)
			{
				if (n == 0u) return false;
				uint64_t v = 0;
				for (size_t i = 0; i < n; ++i)
				{
					if (! IsDigit(p[i])) return false;
					v = v * 10u + static_cast<uint64_t>(p[i] - static_cast<uint8_t>('0'));
					if (v > static_cast<uint64_t>(UINT32_MAX)) return false;
				}
				*out = static_cast<uint32_t>(v);
				return true;
			}

			/* 严格 HH:MM:SS.mmm：小时 ≥ 2 位、分秒各 2 位、毫秒固定 3 位 */
			bool ParseClock(const uint8_t* p, size_t n, size_t* consumed, int64_t* ms_out)
			{
				size_t i = 0;
				size_t dh = 0;
				while (i < n && IsDigit(p[i]))
				{
					++i;
					++dh;
				}
				if (dh < 2u || i >= n || p[i] != static_cast<uint8_t>(':')) return false;
				++i;
				if (i + 2u >= n || ! IsDigit(p[i]) || ! IsDigit(p[i + 1u]) || p[i + 2u] != static_cast<uint8_t>(':'))
					return false;
				const int64_t mm = static_cast<int64_t>(p[i] - static_cast<uint8_t>('0')) * 10 +
								   static_cast<int64_t>(p[i + 1u] - static_cast<uint8_t>('0'));
				i += 3u;
				if (i + 2u >= n || ! IsDigit(p[i]) || ! IsDigit(p[i + 1u]) || p[i + 2u] != static_cast<uint8_t>('.'))
					return false;
				const int64_t ss = static_cast<int64_t>(p[i] - static_cast<uint8_t>('0')) * 10 +
								   static_cast<int64_t>(p[i + 1u] - static_cast<uint8_t>('0'));
				i += 3u;
				if (i + 3u > n) return false;
				if (! IsDigit(p[i]) || ! IsDigit(p[i + 1u]) || ! IsDigit(p[i + 2u])) return false;
				const int64_t mmm = static_cast<int64_t>(p[i] - static_cast<uint8_t>('0')) * 100 +
									static_cast<int64_t>(p[i + 1u] - static_cast<uint8_t>('0')) * 10 +
									static_cast<int64_t>(p[i + 2u] - static_cast<uint8_t>('0'));
				int64_t hh = 0;
				for (size_t k = 0; k < dh; ++k) hh = hh * 10 + static_cast<int64_t>(p[k] - static_cast<uint8_t>('0'));
				*ms_out = hh * 3600000 + mm * 60000 + ss * 1000 + mmm;
				*consumed = i + 3u;
				return true;
			}

			bool ParseTimeAttr(const uint8_t* v, size_t n, int64_t* start_ms, int64_t* end_ms)
			{
				size_t used = 0;
				if (! ParseClock(v, n, &used, start_ms)) return false;
				if (used + 3u > n) return false;
				if (v[used] != static_cast<uint8_t>(' ') || v[used + 1u] != static_cast<uint8_t>('-') ||
					v[used + 2u] != static_cast<uint8_t>(' '))
				{
					return false;
				}
				size_t used2 = 0;
				if (! ParseClock(v + used + 3u, n - (used + 3u), &used2, end_ms)) return false;
				return (used + 3u + used2) == n;
			}

			int64_t MsToTicks(int64_t ms, bool* ok)
			{
				if (ms < 0)
				{
					*ok = false;
					return 0;
				}
				if (ms > INT64_MAX / kTicksPerMs)
				{
					*ok = false;
					return 0;
				}
				*ok = true;
				return ms * kTicksPerMs;
			}

			bool AddChecked(int64_t a, int64_t b, int64_t* out)
			{
				if (b > 0 && a > INT64_MAX - b) return false;
				if (b < 0 && a < INT64_MIN - b) return false;
				*out = a + b;
				return true;
			}

			void Trim(const uint8_t** p, size_t* n)
			{
				size_t a = 0;
				size_t b = *n;
				while (a < b && ((*p)[a] == static_cast<uint8_t>(' ') || (*p)[a] == static_cast<uint8_t>('\t'))) ++a;
				while (b > a &&
					   ((*p)[b - 1u] == static_cast<uint8_t>(' ') || (*p)[b - 1u] == static_cast<uint8_t>('\t')))
				{
					--b;
				}
				*p += a;
				*n = b - a;
			}

			bool ParseUintStrict(const uint8_t* p, size_t n, uint64_t* out)
			{
				if (n == 0u) return false;
				uint64_t v = 0;
				for (size_t i = 0; i < n; ++i)
				{
					if (! IsDigit(p[i])) return false;
					v = v * 10u + static_cast<uint64_t>(p[i] - static_cast<uint8_t>('0'));
					if (v > (static_cast<uint64_t>(1u) << 40)) return false;
				}
				*out = v;
				return true;
			}

			bool HexNib(uint8_t c, uint32_t* out)
			{
				if (c >= static_cast<uint8_t>('0') && c <= static_cast<uint8_t>('9'))
				{
					*out = static_cast<uint32_t>(c - static_cast<uint8_t>('0'));
					return true;
				}
				if (c >= static_cast<uint8_t>('a') && c <= static_cast<uint8_t>('f'))
				{
					*out = static_cast<uint32_t>(c - static_cast<uint8_t>('a')) + 10u;
					return true;
				}
				if (c >= static_cast<uint8_t>('A') && c <= static_cast<uint8_t>('F'))
				{
					*out = static_cast<uint32_t>(c - static_cast<uint8_t>('A')) + 10u;
					return true;
				}
				return false;
			}

			/* `#RRGGBB` / `#RRGGBBAA` → R 落低 8 位的 u32，与公开样式值同口径 */
			bool ParseHexColor(const uint8_t* p, size_t n, uint32_t* out)
			{
				if (n != 7u && n != 9u) return false;
				if (p[0] != static_cast<uint8_t>('#')) return false;
				uint32_t ch[4] = { 0, 0, 0, 0xFFu };
				const size_t pairs = (n - 1u) / 2u;
				for (size_t i = 0; i < pairs; ++i)
				{
					uint32_t hi = 0;
					uint32_t lo = 0;
					if (! HexNib(p[1u + i * 2u], &hi) || ! HexNib(p[2u + i * 2u], &lo)) return false;
					ch[i] = (hi << 4) | lo;
				}
				*out = ch[0] | (ch[1] << 8) | (ch[2] << 16) | (ch[3] << 24);
				return true;
			}

			/* "N%" / "Npx" → 值 + 单位；% 折成 0.1% 单位（×10） */
			bool ParseSizeLike(const uint8_t* p, size_t n, uint32_t* value, uint8_t* unit)
			{
				Trim(&p, &n);
				if (n == 0u) return false;
				if (n >= 2u && p[n - 1u] == static_cast<uint8_t>('%'))
				{
					uint64_t v = 0;
					if (! ParseUintStrict(p, n - 1u, &v) || v > static_cast<uint64_t>(UINT16_MAX) / 10u) return false;
					*value = static_cast<uint32_t>(v * 10u);
					*unit = CLV_SUBTITLE_UNIT_PCT;
					return true;
				}
				if (n >= 3u && p[n - 2u] == static_cast<uint8_t>('p') && p[n - 1u] == static_cast<uint8_t>('x'))
				{
					uint64_t v = 0;
					if (! ParseUintStrict(p, n - 2u, &v) || v > static_cast<uint64_t>(UINT16_MAX)) return false;
					*value = static_cast<uint32_t>(v);
					*unit = CLV_SUBTITLE_UNIT_PX;
					return true;
				}
				return false;
			}

			/* scroll-rect: "x y w h"，四个分量同单位；% 相对屏幕（0.1% 单位），px 为视频像素 */
			bool ParseScrollRect(const uint8_t* p, size_t n, int32_t* x, int32_t* y, int32_t* w, int32_t* h,
								 uint8_t* unit)
			{
				uint32_t vals[4] = { 0, 0, 0, 0 };
				uint8_t units[4] = { 0, 0, 0, 0 };
				size_t i = 0;
				for (int k = 0; k < 4; ++k)
				{
					while (i < n && (p[i] == static_cast<uint8_t>(' ') || p[i] == static_cast<uint8_t>('\t'))) ++i;
					const size_t start = i;
					while (i < n && p[i] != static_cast<uint8_t>(' ') && p[i] != static_cast<uint8_t>('\t')) ++i;
					if (start == i) return false;
					if (! ParseSizeLike(p + start, i - start, &vals[k], &units[k])) return false;
					if (k != 0 && units[k] != units[0]) return false;
				}
				while (i < n && (p[i] == static_cast<uint8_t>(' ') || p[i] == static_cast<uint8_t>('\t'))) ++i;
				if (i != n) return false;
				if (vals[2] == 0u || vals[3] == 0u) return false;
				*x = static_cast<int32_t>(vals[0]);
				*y = static_cast<int32_t>(vals[1]);
				*w = static_cast<int32_t>(vals[2]);
				*h = static_cast<int32_t>(vals[3]);
				*unit = units[0];
				return true;
			}

			/* "N"/"N.N" → 千分整数，无浮点 */
			bool ParseOpacity(const uint8_t* p, size_t n, uint32_t* out)
			{
				Trim(&p, &n);
				size_t dot = 0;
				bool has_dot = false;
				for (size_t i = 0; i < n; ++i)
				{
					if (p[i] == static_cast<uint8_t>('.'))
					{
						dot = i;
						has_dot = true;
						break;
					}
				}
				if (! has_dot)
				{
					uint64_t v = 0;
					if (! ParseUintStrict(p, n, &v)) return false;
					if (v != 0u && v != 1u) return false;
					*out = v == 1u ? 1000u : 0u;
					return true;
				}
				uint64_t ip = 0;
				uint64_t fp = 0;
				if (! ParseUintStrict(p, dot, &ip) || ip > 1u) return false;
				const size_t fl = n - dot - 1u;
				if (fl == 0u || fl > 3u) return false;
				for (size_t i = 0; i < fl; ++i)
				{
					if (! IsDigit(p[dot + 1u + i])) return false;
					fp = fp * 10u + static_cast<uint64_t>(p[dot + 1u + i] - static_cast<uint8_t>('0'));
				}
				for (size_t i = fl; i < 3u; ++i) fp *= 10u;
				if (ip == 1u && fp != 0u) return false;
				*out = static_cast<uint32_t>(ip * 1000u + fp);
				return true;
			}

			/* key 区分大小写：只有全小写形态命中 */
			bool KeyIs(const uint8_t* p, size_t n, const char* lit)
			{
				const size_t len = std::strlen(lit);
				if (n != len) return false;
				for (size_t i = 0; i < len; ++i)
					if (LowerOf(p[i]) != static_cast<uint8_t>(lit[i])) return false;
				return true;
			}

			bool KeyExactLower(const uint8_t* p, size_t n)
			{
				for (size_t i = 0; i < n; ++i)
				{
					const uint8_t c = p[i];
					const bool ok = IsDigit(c) || (c >= static_cast<uint8_t>('a') && c <= static_cast<uint8_t>('z')) ||
									c == static_cast<uint8_t>('-') || c == static_cast<uint8_t>('_');
					if (! ok) return false;
				}
				return n != 0u;
			}

			bool ParseTwoSized(const uint8_t* p, size_t n, int32_t* x, uint8_t* unit, int32_t* y)
			{
				Trim(&p, &n);
				size_t i = 0;
				while (i < n && p[i] != static_cast<uint8_t>(' ') && p[i] != static_cast<uint8_t>('\t')) ++i;
				if (i == 0u || i >= n) return false;
				uint32_t vx = 0;
				uint32_t vy = 0;
				uint8_t ux = 0;
				uint8_t uy = 0;
				if (! ParseSizeLike(p, i, &vx, &ux)) return false;
				const uint8_t* rest = p + i;
				size_t rn = n - i;
				Trim(&rest, &rn);
				if (! ParseSizeLike(rest, rn, &vy, &uy)) return false;
				if (ux != uy) return false;
				*x = static_cast<int32_t>(vx);
				*y = static_cast<int32_t>(vy);
				*unit = ux;
				return true;
			}

			/* 把一个属性行折进 style；未知 key 忽略，值不成立的属性也忽略（不报错、不置位） */
			bool ApplyAttr(StyleValue* st, const uint8_t* k, size_t kn, const uint8_t* v, size_t vn)
			{
				if (! KeyExactLower(k, kn)) return false;
				uint64_t tmp = 0;
				if (KeyIs(k, kn, "time")) return true;
				if (KeyIs(k, kn, "font-color"))
				{
					uint32_t c = 0;
					Trim(&v, &vn);
					if (! ParseHexColor(v, vn, &c)) return true;
					st->font_color_rgba = c;
					st->flags |= CLV_SUBTITLE_STYLE_FONT_COLOR;
					return true;
				}
				if (KeyIs(k, kn, "font-size"))
				{
					uint32_t val = 0;
					uint8_t u = 0;
					if (! ParseSizeLike(v, vn, &val, &u)) return true;
					st->font_size = static_cast<uint16_t>(val);
					st->font_size_unit = u;
					st->flags |= CLV_SUBTITLE_STYLE_FONT_SIZE;
					return true;
				}
				if (KeyIs(k, kn, "font-family"))
				{
					Trim(&v, &vn);
					if (vn == 0u) return true;
					st->font_family = v;
					st->font_family_size = static_cast<uint32_t>(vn);
					st->flags |= CLV_SUBTITLE_STYLE_FONT_FAMILY;
					return true;
				}
				if (KeyIs(k, kn, "font-weight"))
				{
					Trim(&v, &vn);
					if (KeyIs(v, vn, "normal"))
					{
						st->font_weight = 400;
					}
					else if (KeyIs(v, vn, "bold"))
					{
						st->font_weight = 700;
					}
					else
					{
						if (! ParseUintStrict(v, vn, &tmp) || tmp < 100u || tmp > 900u) return true;
						st->font_weight = static_cast<uint16_t>(tmp);
					}
					st->flags |= CLV_SUBTITLE_STYLE_FONT_WEIGHT;
					return true;
				}
				if (KeyIs(k, kn, "font-style"))
				{
					Trim(&v, &vn);
					if (KeyIs(v, vn, "normal")) st->font_style = 0;
					else if (KeyIs(v, vn, "italic")) st->font_style = 1;
					else return true;
					st->flags |= CLV_SUBTITLE_STYLE_FONT_STYLE;
					return true;
				}
				if (KeyIs(k, kn, "text-align"))
				{
					Trim(&v, &vn);
					if (KeyIs(v, vn, "left")) st->text_align = CLV_SUBTITLE_ALIGN_LEFT;
					else if (KeyIs(v, vn, "center")) st->text_align = CLV_SUBTITLE_ALIGN_CENTER;
					else if (KeyIs(v, vn, "right")) st->text_align = CLV_SUBTITLE_ALIGN_RIGHT;
					else return true;
					st->flags |= CLV_SUBTITLE_STYLE_TEXT_ALIGN;
					return true;
				}
				if (KeyIs(k, kn, "position"))
				{
					int32_t x = 0;
					int32_t y = 0;
					uint8_t u = 0;
					if (! ParseTwoSized(v, vn, &x, &u, &y)) return true;
					st->pos_x = x;
					st->pos_y = y;
					st->pos_unit = u;
					st->flags |= CLV_SUBTITLE_STYLE_POSITION;
					return true;
				}
				if (KeyIs(k, kn, "line-height"))
				{
					uint32_t val = 0;
					uint8_t u = 0;
					if (! ParseSizeLike(v, vn, &val, &u)) return true;
					st->line_height = static_cast<uint16_t>(val);
					st->line_height_unit = u;
					st->flags |= CLV_SUBTITLE_STYLE_LINE_HEIGHT;
					return true;
				}
				if (KeyIs(k, kn, "opacity"))
				{
					uint32_t o = 0;
					if (! ParseOpacity(v, vn, &o)) return true;
					st->opacity = static_cast<uint16_t>(o);
					st->flags |= CLV_SUBTITLE_STYLE_OPACITY;
					return true;
				}
				if (KeyIs(k, kn, "scroll"))
				{
					Trim(&v, &vn);
					if (KeyIs(v, vn, "top")) st->scroll = CLV_SUBTITLE_SCROLL_TOP;
					else if (KeyIs(v, vn, "bottom")) st->scroll = CLV_SUBTITLE_SCROLL_BOTTOM;
					else if (KeyIs(v, vn, "left")) st->scroll = CLV_SUBTITLE_SCROLL_LEFT;
					else if (KeyIs(v, vn, "right")) st->scroll = CLV_SUBTITLE_SCROLL_RIGHT;
					else return true;
					st->flags |= CLV_SUBTITLE_STYLE_SCROLL;
					return true;
				}
				if (KeyIs(k, kn, "scroll-rect"))
				{
					int32_t rx = 0;
					int32_t ry = 0;
					int32_t rw = 0;
					int32_t rh = 0;
					uint8_t u = 0;
					if (! ParseScrollRect(v, vn, &rx, &ry, &rw, &rh, &u)) return true;
					st->scroll_x = rx;
					st->scroll_y = ry;
					st->scroll_w = rw;
					st->scroll_h = rh;
					st->scroll_unit = u;
					st->flags |= CLV_SUBTITLE_STYLE_SCROLL_RECT;
					return true;
				}
				return false;
			}

			uint64_t SourcePut(uint64_t packed, uint32_t bit, uint32_t value)
			{
				const int shift = static_cast<int>(bit * 3u);
				packed &= ~(0x7ull << shift);
				packed |= static_cast<uint64_t>(value & 0x7u) << shift;
				return packed;
			}

			/* 一条覆盖：src 里位为 f 的字段写进 dst，来源记 tier；受保护的位不动 */
			void ApplyTier(StyleValue* dst, uint64_t* src_packed, uint32_t tier, const StyleValue* s,
						   uint32_t allowed_mask, uint32_t protected_mask)
			{
				if (s == nullptr) return;
				const uint32_t eff = s->flags & allowed_mask & ~protected_mask;
				if ((eff & CLV_SUBTITLE_STYLE_FONT_COLOR) != 0u)
				{
					dst->font_color_rgba = s->font_color_rgba;
					*src_packed = SourcePut(*src_packed, 0u, tier);
				}
				if ((eff & CLV_SUBTITLE_STYLE_STROKE_COLOR) != 0u)
				{
					dst->stroke_color_rgba = s->stroke_color_rgba;
					*src_packed = SourcePut(*src_packed, 1u, tier);
				}
				if ((eff & CLV_SUBTITLE_STYLE_FONT_SIZE) != 0u)
				{
					dst->font_size = s->font_size;
					dst->font_size_unit = s->font_size_unit;
					*src_packed = SourcePut(*src_packed, 2u, tier);
				}
				if ((eff & CLV_SUBTITLE_STYLE_FONT_WEIGHT) != 0u)
				{
					dst->font_weight = s->font_weight;
					*src_packed = SourcePut(*src_packed, 3u, tier);
				}
				if ((eff & CLV_SUBTITLE_STYLE_FONT_STYLE) != 0u)
				{
					dst->font_style = s->font_style;
					*src_packed = SourcePut(*src_packed, 4u, tier);
				}
				if ((eff & CLV_SUBTITLE_STYLE_TEXT_ALIGN) != 0u)
				{
					dst->text_align = s->text_align;
					*src_packed = SourcePut(*src_packed, 5u, tier);
				}
				if ((eff & CLV_SUBTITLE_STYLE_POSITION) != 0u)
				{
					dst->pos_x = s->pos_x;
					dst->pos_y = s->pos_y;
					dst->pos_unit = s->pos_unit;
					*src_packed = SourcePut(*src_packed, 6u, tier);
				}
				if ((eff & CLV_SUBTITLE_STYLE_LINE_HEIGHT) != 0u)
				{
					dst->line_height = s->line_height;
					dst->line_height_unit = s->line_height_unit;
					*src_packed = SourcePut(*src_packed, 7u, tier);
				}
				if ((eff & CLV_SUBTITLE_STYLE_OPACITY) != 0u)
				{
					dst->opacity = s->opacity;
					*src_packed = SourcePut(*src_packed, 8u, tier);
				}
				if ((eff & CLV_SUBTITLE_STYLE_SCROLL) != 0u)
				{
					dst->scroll = s->scroll;
					*src_packed = SourcePut(*src_packed, 9u, tier);
				}
				if ((eff & CLV_SUBTITLE_STYLE_SCROLL_RECT) != 0u)
				{
					dst->scroll_x = s->scroll_x;
					dst->scroll_y = s->scroll_y;
					dst->scroll_w = s->scroll_w;
					dst->scroll_h = s->scroll_h;
					dst->scroll_unit = s->scroll_unit;
					*src_packed = SourcePut(*src_packed, 10u, tier);
				}
				if ((eff & CLV_SUBTITLE_STYLE_FONT_FAMILY) != 0u)
				{
					dst->font_family = s->font_family;
					dst->font_family_size = s->font_family_size;
					*src_packed = SourcePut(*src_packed, 11u, tier);
				}
				if ((eff & CLV_SUBTITLE_STYLE_STROKE_WIDTH) != 0u)
				{
					dst->stroke_width = s->stroke_width;
					*src_packed = SourcePut(*src_packed, 12u, tier);
				}
				dst->flags |= eff;
			}

			void NoteUnusable(Stats* stats, uint32_t reason)
			{
				stats->blocks_unusable += 1u;
				stats->unusable_reason_mask |= reason;
			}
		}	 // namespace

		struct DecodedLine
		{
			std::vector<uint8_t> text;
			std::vector<CLV_SubtitleSpanC> spans;
		};

		struct Parser
		{
			std::vector<Block> blocks;
			std::vector<Line> lines;
			std::deque<DecodedLine> decoded;
			Stats stats;
			uint32_t max_packet_bytes = kMaxPacketSize;
			uint32_t max_id = 0u;
			bool has_max_id = false;
			int64_t last_start = 0;
			bool has_last_start = false;
			bool reset_pending = false;
			StyleValue parser_default {};
			bool has_parser_default = false;
			StyleBlock style_block {};
			bool has_style_block = false;
		};

		void ResolveStyleChain(StyleValue* inout_style, uint64_t* inout_source, const StyleValue* built_in,
							   const StyleValue* renderer_default, const StyleBlock* style_block,
							   const StyleValue* parser_default, const PacketHeader* packet)
		{
			if (inout_style == nullptr || inout_source == nullptr) return;
			const uint32_t block_preset = inout_style->flags; /* 块属性最高，永不被覆盖 */
			uint64_t packed = *inout_source;
			for (uint32_t bit = 0u; bit < kStyleBitCount; ++bit)
				if ((block_preset & (1u << bit)) != 0u) packed = SourcePut(packed, bit, CLV_SUBTITLE_SRC_BLOCK_ATTR);

			const uint32_t style_block_allowed = CLV_SUBTITLE_STYLE_FONT_COLOR | CLV_SUBTITLE_STYLE_STROKE_COLOR |
												 CLV_SUBTITLE_STYLE_FONT_SIZE | CLV_SUBTITLE_STYLE_FONT_FAMILY |
												 CLV_SUBTITLE_STYLE_STROKE_WIDTH;
			const uint32_t packet_allowed = CLV_SUBTITLE_STYLE_TEXT_ALIGN | CLV_SUBTITLE_STYLE_POSITION;

			ApplyTier(inout_style, &packed, CLV_SUBTITLE_SRC_BUILT_IN, built_in, CLV_SUBTITLE_STYLE_ALL, block_preset);
			if (renderer_default != nullptr)
			{
				ApplyTier(inout_style, &packed, CLV_SUBTITLE_SRC_RENDERER, renderer_default, CLV_SUBTITLE_STYLE_ALL,
						  block_preset);
			}

			if (style_block != nullptr)
			{
				StyleValue sb {};
				sb.flags = style_block->flags;
				sb.font_size = style_block->font_size;
				sb.font_size_unit = style_block->font_size_unit;
				sb.font_color_rgba = style_block->font_color_rgba;
				sb.stroke_color_rgba = style_block->stroke_color_rgba;
				sb.stroke_width = style_block->stroke_width;
				sb.font_family = style_block->font_family;
				sb.font_family_size = style_block->font_family_size;
				ApplyTier(inout_style, &packed, CLV_SUBTITLE_SRC_STYLE_BLOCK, &sb, style_block_allowed, block_preset);
			}

			ApplyTier(inout_style, &packed, CLV_SUBTITLE_SRC_PARSER, parser_default, CLV_SUBTITLE_STYLE_ALL,
					  block_preset);

			if (packet != nullptr && (packet->has_alignment != 0u || packet->has_position != 0u))
			{
				StyleValue pk {};
				if (packet->has_alignment != 0u)
				{
					pk.text_align = packet->alignment;
					pk.flags |= CLV_SUBTITLE_STYLE_TEXT_ALIGN;
				}
				if (packet->has_position != 0u)
				{
					pk.pos_x = packet->position_x;
					pk.pos_y = packet->position_y;
					pk.pos_unit = CLV_SUBTITLE_UNIT_PX;
					pk.flags |= CLV_SUBTITLE_STYLE_POSITION;
				}
				ApplyTier(inout_style, &packed, CLV_SUBTITLE_SRC_PACKET, &pk, packet_allowed, block_preset);
			}

			inout_style->flags |= block_preset;
			for (uint32_t bit = 0u; bit < kStyleBitCount; ++bit)
				if ((inout_style->flags & (1u << bit)) == 0u) packed = SourcePut(packed, bit, CLV_SUBTITLE_SRC_UNSET);
			*inout_source = packed;
		}

		Parser* ParserCreate(const StyleValue* parser_default, uint32_t max_packet_bytes)
		{
			Parser* p = new (std::nothrow) Parser();
			if (p == nullptr) return nullptr;
			p->max_packet_bytes = max_packet_bytes == 0u ? kMaxPacketSize : max_packet_bytes;
			if (parser_default != nullptr)
			{
				p->parser_default = *parser_default;
				p->has_parser_default = true;
			}
			return p;
		}

		void ParserDestroy(Parser* p) { delete p; }

		void ParserSetStyleBlock(Parser* p, const StyleBlock* block)
		{
			if (p == nullptr) return;
			if (block == nullptr)
			{
				p->style_block = StyleBlock {};
				p->has_style_block = false;
				return;
			}
			p->style_block = *block;
			p->has_style_block = true;
		}

		void ParserReset(Parser* p)
		{
			if (p == nullptr) return;
			p->blocks.clear();
			p->lines.clear();
			p->decoded.clear();
			p->max_id = 0u;
			p->has_max_id = false;
			p->last_start = 0;
			p->has_last_start = false;
			p->reset_pending = true;
			p->stats.resets += 1u;
		}

		uint32_t ParserBlockCount(const Parser* p)
		{
			return p == nullptr ? 0u : static_cast<uint32_t>(p->blocks.size());
		}

		const Block* ParserBlockAt(const Parser* p, uint32_t index)
		{
			if (p == nullptr || index >= p->blocks.size()) return nullptr;
			return &p->blocks[index];
		}

		uint32_t ParserLineCount(const Parser* p) { return p == nullptr ? 0u : static_cast<uint32_t>(p->lines.size()); }

		const Line* ParserLineAt(const Parser* p, uint32_t index)
		{
			if (p == nullptr || index >= p->lines.size()) return nullptr;
			return &p->lines[index];
		}

		void ParserStats(const Parser* p, Stats* out)
		{
			if (out == nullptr) return;
			*out = Stats {};
			if (p != nullptr) *out = p->stats;
		}

		Err ParserFeed(Parser* p, const uint8_t* payload, size_t size, int64_t container_pts)
		{
			if (p == nullptr) return kInvalidHandle;
			if (payload == nullptr) return kInvalidArgument;
			p->stats.packets_seen += 1u;

			PacketHeader hdr {};
			const Err he = ParsePacketHeader(payload, size, container_pts, p->max_packet_bytes, &hdr);
			if (he != kOk)
			{
				if (he == kPacketTooLarge) p->stats.packets_too_large += 1u;
				else p->stats.packets_bad_header += 1u;
				return he;
			}
			if (hdr.delta_ticks != 0) p->stats.packets_translated += 1u;

			const uint8_t* text = payload + hdr.header_len;
			size_t n = static_cast<size_t>(hdr.text_len);
			if (n > 0u && ! Utf8Valid(text, n))
			{
				p->stats.packets_bad_utf8 += 1u;
				return kInvalidUtf8;
			}
			/* 每个载荷开头允许一个 UTF-8 BOM，忽略 */
			if (n >= 3u && text[0] == static_cast<uint8_t>(0xEF) && text[1] == static_cast<uint8_t>(0xBB) &&
				text[2] == static_cast<uint8_t>(0xBF))
			{
				text += 3u;
				n -= 3u;
			}

			struct DefState
			{
				bool in_def = false;
				uint32_t id = 0u;
				bool has_time = false;
				int64_t start_ms = 0;
				int64_t end_ms = 0;
				StyleValue st {};
				bool closed_ok = false;
			};

			try
			{
				size_t i = 0;
				DefState def;
				bool have_scope = false;
				uint32_t scope_block_index = kNoBlock;
				uint32_t scope_id = 0u;
				uint32_t group_id = 0u;
				bool group_open = false;

				while (i < n)
				{
					size_t e = i;
					while (e < n && text[e] != static_cast<uint8_t>('\n')) ++e;
					const uint8_t* lp = text + i;
					size_t ln = e - i;
					i = (e < n) ? e + 1u : e;

					if (ln == 0u || IsBlankLine(lp, ln)) continue;
					size_t lead = 0;
					while (lead < ln &&
						   (lp[lead] == static_cast<uint8_t>(' ') || lp[lead] == static_cast<uint8_t>('\t')))
					{
						++lead;
					}
					if (lead + 1u < ln && lp[lead] == static_cast<uint8_t>('/') &&
						lp[lead + 1u] == static_cast<uint8_t>('/'))
					{
						continue;
					}
					if (lead + 4u <= ln && lp[lead] == static_cast<uint8_t>('<') && ln >= lead + 7u &&
						std::memcmp(lp + lead, "<!--", 4u) == 0)
					{
						/* 块注释只在同行闭合时才算注释，否则按 other 忽略 */
						if (FindSeq(lp + lead + 4u, ln - (lead + 4u), "-->") != nullptr) continue;
					}

					if (ln >= 2u && lp[0] == static_cast<uint8_t>('<') && lp[1] == static_cast<uint8_t>('#'))
					{
						size_t k = 2u;
						while (k < ln && IsDigit(lp[k])) ++k;
						uint32_t id = 0u;
						const bool id_ok =
							(k > 2u) && ParseId(lp + 2u, k - 2u, &id) && k < ln && lp[k] == static_cast<uint8_t>('/');
						if (k < ln && lp[k] == static_cast<uint8_t>('/'))
						{
							/* 上一个 def 未闭合 → 整体忽略（只计数，不进块视图） */
							if (def.in_def && ! def.closed_ok)
								NoteUnusable(&p->stats, CLV_SUBTITLE_BLOCK_REASON_SYNTAX_IGNORE);
							def = DefState {};
							def.in_def = true;
							def.id = id_ok ? id : 0u;
							def.closed_ok = false;
							group_open = false;
							have_scope = false;
						}
						continue;
					}

					if (def.in_def)
					{
						if (ln >= 2u && lp[ln - 2u] == static_cast<uint8_t>('/') &&
							lp[ln - 1u] == static_cast<uint8_t>('>') && IsBlankLine(lp, ln - 2u))
						{
							def.closed_ok = true;

							Block blk {};
							blk.id = def.id;
							bool syntax_ignore = false;
							bool nodisplay = false;
							if (def.id == 0u) syntax_ignore = true; /* ID 不可解析 / 超范围 */
							if (! p->reset_pending && p->has_max_id && def.id <= p->max_id) syntax_ignore = true;
							if (! def.has_time)
							{
								nodisplay = true;
							}
							else
							{
								bool ok1 = false;
								bool ok2 = false;
								const int64_t st_t = MsToTicks(def.start_ms, &ok1);
								const int64_t en_t = MsToTicks(def.end_ms, &ok2);
								if (! ok1 || ! ok2)
								{
									nodisplay = true;
								}
								else
								{
									int64_t s2 = 0;
									int64_t e2 = 0;
									if (! AddChecked(st_t, hdr.delta_ticks, &s2) ||
										! AddChecked(en_t, hdr.delta_ticks, &e2))
									{
										p->stats.blocks_time_overflow += 1u;
										syntax_ignore = true;
									}
									else
									{
										blk.start_ticks = s2;
										blk.end_ticks = e2;
										if (! p->reset_pending && p->has_last_start && s2 < p->last_start)
											syntax_ignore = true;
										if (e2 <= s2) nodisplay = true;
										if (s2 < 0)
										{
											p->stats.blocks_negative_tick += 1u;
											if (e2 <= 0) nodisplay = true;
										}
									}
								}
							}

							uint32_t reason = 0u;
							if (syntax_ignore) reason |= CLV_SUBTITLE_BLOCK_REASON_SYNTAX_IGNORE;
							if (nodisplay) reason |= CLV_SUBTITLE_BLOCK_REASON_TIME_HIDDEN;
							blk.reason_mask = reason;

							StyleValue st = def.st;
							uint64_t src = 0u;
							ResolveStyleChain(&st, &src, nullptr, nullptr,
											  p->has_style_block ? &p->style_block : nullptr,
											  p->has_parser_default ? &p->parser_default : nullptr, &hdr);
							blk.style = st;
							blk.style_source = src;

							const uint32_t idx = static_cast<uint32_t>(p->blocks.size());
							p->blocks.push_back(blk);
							if (reason != 0u)
							{
								NoteUnusable(&p->stats, reason);
								p->blocks[idx].displayable = 0u;
							}
							if (! syntax_ignore)
							{
								if (nodisplay)
								{
									if (def.id != 0u && (! p->has_max_id || def.id > p->max_id))
									{
										p->max_id = def.id;
										p->has_max_id = true;
									}
								}
								else
								{
									p->blocks[idx].displayable = 1u;
									p->stats.blocks_total += 1u;
									if (! p->has_last_start || blk.start_ticks > p->last_start)
									{
										p->last_start = blk.start_ticks;
										p->has_last_start = true;
									}
									if (def.id != 0u && (! p->has_max_id || def.id > p->max_id))
									{
										p->max_id = def.id;
										p->has_max_id = true;
									}
								}
							}
							p->reset_pending = false;
							have_scope = true;
							scope_block_index = idx;
							scope_id = def.id;
							group_open = false;
							def.in_def = false;
							continue;
						}

						/* 属性行：key 与值之间恰好一个空格 */
						size_t c = 0;
						while (c < ln && lp[c] != static_cast<uint8_t>(':')) ++c;
						if (c > 0u && c + 1u < ln && lp[c + 1u] == static_cast<uint8_t>(' '))
						{
							const uint8_t* k = lp;
							const size_t kn = c;
							const uint8_t* v = lp + c + 2u;
							size_t vn = ln - (c + 2u);
							if (KeyExactLower(k, kn) && KeyIs(k, kn, "time"))
							{
								int64_t a = 0;
								int64_t b = 0;
								if (ParseTimeAttr(v, vn, &a, &b))
								{
									def.start_ms = a;
									def.end_ms = b;
									def.has_time = true;
								}
								continue;
							}
							ApplyAttr(&def.st, k, kn, v, vn);
							continue;
						}
						continue;
					}

					/* def 区之外：文本行 */
					if (ln >= 2u && lp[0] == static_cast<uint8_t>('#'))
					{
						size_t k = 1u;
						while (k < ln && IsDigit(lp[k])) ++k;
						uint32_t id = 0u;
						const bool id_ok = (k > 1u) && ParseId(lp + 1u, k - 1u, &id) && k < ln &&
										   (lp[k] == static_cast<uint8_t>(' ') || lp[k] == static_cast<uint8_t>('\t'));
						if (! id_ok)
						{
							p->stats.lines_ignored += 1u;
							continue;
						}
						if (! have_scope || id != scope_id)
						{
							p->stats.lines_ignored += 1u;
							continue;
						}
						if (group_open && group_id != id)
						{
							p->stats.lines_ignored += 1u;
							continue;
						}
						Line line {};
						line.block_id = p->blocks[scope_block_index].id;
						const uint8_t* raw = lp + k + 1u;
						const size_t raw_n = ln - (k + 1u);
						p->decoded.push_back(DecodedLine());
						DecodedLine& dl = p->decoded.back();
						size_t span_need = 0;
						const size_t text_need = DecodeText(raw, raw_n, nullptr, 0u, nullptr, 0u, &span_need);
						dl.text.assign(text_need + 1u, static_cast<uint8_t>(0));
						dl.spans.resize(span_need);
						size_t span_got = 0;
						DecodeText(raw, raw_n, dl.text.data(), text_need, span_need == 0u ? nullptr : dl.spans.data(),
								   span_need, &span_got);
						dl.spans.resize(span_got);
						line.text = dl.text.data();
						line.size = static_cast<uint32_t>(text_need);
						line.spans = span_got == 0u ? nullptr : dl.spans.data();
						line.span_count = static_cast<uint32_t>(span_got);
						line.scope_ok =
							(p->blocks[scope_block_index].reason_mask & CLV_SUBTITLE_BLOCK_REASON_SYNTAX_IGNORE) == 0u ?
								1u :
								0u;
						p->lines.push_back(line);
						if (p->blocks[scope_block_index].line_count == 0u)
							p->blocks[scope_block_index].first_line = static_cast<uint32_t>(p->lines.size() - 1u);
						p->blocks[scope_block_index].line_count += 1u;
						group_open = true;
						group_id = id;
						p->stats.lines_total += 1u;
						continue;
					}
					/* 块外裸文本：忽略，不产生行记录 */
				}

				if (def.in_def && ! def.closed_ok) NoteUnusable(&p->stats, CLV_SUBTITLE_BLOCK_REASON_SYNTAX_IGNORE);
			}
			catch (const std::bad_alloc&)
			{
				return kOutOfMemory;
			}
			return kOk;
		}

		uint32_t StyleSourceGet(uint64_t packed, uint32_t bit)
		{
			return static_cast<uint32_t>((packed >> (bit * 3u)) & 0x7u);
		}
	}	 // namespace subtitle
}	 // namespace clv
