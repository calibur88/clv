/* 渲染器实现：参考实现的两段式布局（逻辑行 → 折行）与码位级回退。
 *
 * 一帧：Layout 累加可见块 → Render 合成并出脏矩形 → 记录清空。
 * 字符颜色按公开面口径（R 落低 8 位）；CAL 的度量是 26.6 定点，像素量在这里除 64。
 */
#include "renderer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <new>
#include <utility>

namespace clv
{
	namespace subtitle
	{
		namespace render
		{
			namespace
			{
				enum Stage
				{
					kStageAuthor = 0,
					kStageBase = 1,
					kStageSystem = 2,
					kStageTofu = 3,
					kStageEmoji = 4
				};

				struct Line
				{
					std::string text;
					std::vector<uint8_t> kind; /* 每字节：0 = 普通文字，1 = `[\emoji ...]` 路由段 */
				};

				struct Bitmap
				{
					uint32_t w = 0;
					uint32_t h = 0;
					int32_t left = 0;
					int32_t top = 0;
					std::vector<uint8_t> px;
				};

				struct ColorBitmap
				{
					uint32_t w = 0;
					uint32_t h = 0;
					int32_t left = 0;
					int32_t top = 0;
					bool color = false;
					std::vector<uint8_t> px;
				};

				struct StrokeBitmap
				{
					uint32_t w = 0;
					uint32_t h = 0;
					int32_t left = 0;
					int32_t top = 0;
					std::vector<uint8_t> fill;
					std::vector<uint8_t> outline;
				};

				struct RStyle
				{
					int32_t font_size = 1;
					int32_t line_height = 1;
					uint32_t color = 0xFFFFFFFFu;
					uint32_t stroke_color = 0xFF000000u;
					int32_t stroke_w = 0;
					int32_t opacity = 1000;
					int32_t align = 1;
					bool has_pos = false;
					int32_t pos_x = 500;
					int32_t pos_y = 900;
					uint8_t pos_unit = CLV_SUBTITLE_UNIT_PCT;
					int32_t scroll = 0;
					bool has_rect = false;
					int32_t rect_x = 0;
					int32_t rect_y = 0;
					int32_t rect_w = 0;
					int32_t rect_h = 0;
					uint8_t rect_unit = CLV_SUBTITLE_UNIT_PCT;
					std::vector<std::string> authors;
					uint32_t weight = 400u;
					int32_t italic = 0;
				};

				struct View
				{
					int64_t x = 0;
					int64_t y = 0;
					int64_t w = 0;
					int64_t h = 0;
				};

				struct FacePick
				{
					std::string path;
					uint32_t face_index = 0;
					int32_t stage = kStageTofu;
				};

				struct SegLayout
				{
					std::vector<uint32_t> glyphs;
					std::vector<int32_t> advances;
					std::string path;
					uint32_t face_index = 0;
					int32_t stage = kStageTofu;
					bool color = false;
					bool tofu = false;
					uint32_t off = 0;
					uint32_t len = 0;
					int32_t x = 0;
				};

				struct LineLayout
				{
					std::vector<SegLayout> segs;
					int64_t width = 0;
				};

				struct Placed
				{
					int64_t time_ticks = 0;
					int64_t start = 0;
					int64_t end = 0;
					RStyle style;
					std::string font_path;
					uint32_t face_index = 0;
					int32_t stage = kStageAuthor;
					bool tofu = false;
					int32_t stroke_w = 0;
					uint32_t stroke_color = 0xFF000000u;
					View view;
					std::vector<LineLayout> lines;
					std::vector<std::string> pieces;
					int64_t content_w = 0;
					int64_t content_h = 0;
					int64_t center_x = 0;
					int64_t center_y = 0;
					bool is_scroll = false;
				};

				struct ShapeResult
				{
					std::vector<uint32_t> glyphs;
					std::vector<int32_t> advances;
					int32_t rtl = 0;
					bool ok = false;
				};

				const char* kBaseFamilies[] = { "NotoSansCJKsc", "NotoNaskhArabic" };
				constexpr size_t kBaseFamilyCount = 2;

				std::vector<std::string> SplitFamilies(const std::string& raw)
				{
					std::vector<std::string> out;
					size_t pos = 0;
					while (pos <= raw.size())
					{
						size_t comma = raw.find(',', pos);
						if (comma == std::string::npos) comma = raw.size();
						std::string item = raw.substr(pos, comma - pos);
						const size_t b = item.find_first_not_of(" \t\"'");
						const size_t e = item.find_last_not_of(" \t\"'");
						if (b != std::string::npos && e != std::string::npos)
						{
							item = item.substr(b, e - b + 1u);
							if (! item.empty()) out.push_back(item);
						}
						if (comma == raw.size()) break;
						pos = comma + 1u;
					}
					return out;
				}

				int64_t ClampI64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

				uint32_t CpAt(const std::string& s, size_t i, size_t* len)
				{
					const uint8_t b0 = static_cast<uint8_t>(s[i]);
					size_t n = 1;
					if (b0 >= 0xF0u) n = 4;
					else if (b0 >= 0xE0u) n = 3;
					else if (b0 >= 0xC0u) n = 2;
					uint32_t cp = b0;
					if (n > 1 && i + n <= s.size())
					{
						cp = 0;
						for (size_t k = 0; k < n; ++k)
							if (k == 0u) cp = b0 & (0xFFu >> (n + 1u));
							else cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3Fu);
					}
					*len = n;
					return cp;
				}

				bool HeadForbidden(uint32_t cp)
				{
					static const uint32_t k[] = { 0x002Cu, 0x002Eu, 0x0021u, 0x003Fu, 0x003Bu, 0x003Au, 0x0029u,
												  0x005Du, 0x007Du, 0x0025u, 0x00B7u, 0x2014u, 0x2018u, 0x2019u,
												  0x201Cu, 0x201Du, 0x2026u, 0x3001u, 0x3002u, 0x3009u, 0x300Bu,
												  0x300Du, 0x300Fu, 0x3011u, 0x3015u, 0x3017u, 0xFF01u, 0xFF09u,
												  0xFF0Cu, 0xFF0Eu, 0xFF1Au, 0xFF1Bu, 0xFF1Fu, 0xFF3Du, 0xFF5Du,
												  0xFF61u, 0xFF05u };
					for (uint32_t c: k)
						if (c == cp) return true;
					return false;
				}

				bool TailForbidden(uint32_t cp)
				{
					static const uint32_t k[] = { 0x0028u, 0x005Bu, 0x007Bu, 0x00A3u, 0x0024u,
												  0x3008u, 0x300Au, 0x300Cu, 0x300Eu, 0x3010u,
												  0x3014u, 0x3016u, 0xFF08u, 0xFF3Bu, 0xFF5Bu };
					for (uint32_t c: k)
						if (c == cp) return true;
					return false;
				}

				bool BreakAfterOk(uint32_t cp)
				{
					return (cp >= 0x2E80u && cp <= 0x9FFFu) || (cp >= 0xAC00u && cp <= 0xD7AFu) ||
						   (cp >= 0x3040u && cp <= 0x30FFu) || (cp >= 0xF900u && cp <= 0xFAFFu) ||
						   (cp >= 0xFF66u && cp <= 0xFF9Fu);
				}

				bool BreakAllowed(const std::string& s, size_t pos, size_t i)
				{
					if (i <= pos || i >= s.size()) return false;
					size_t j = i - 1u;
					while (j > pos && (static_cast<uint8_t>(s[j]) & 0xC0u) == 0x80u) --j;
					size_t ln = 1;
					size_t rn = 1;
					const uint32_t left = CpAt(s, j, &ln);
					const uint32_t right = CpAt(s, i, &rn);
					if (HeadForbidden(right)) return false;
					if (TailForbidden(left)) return false;
					if (left == 0x20u) return true;
					return BreakAfterOk(left) || BreakAfterOk(right);
				}

				Line SliceLine(const Line& in, size_t pos, size_t len)
				{
					Line out;
					if (pos > in.text.size()) pos = in.text.size();
					if (len > in.text.size() - pos) len = in.text.size() - pos;
					out.text.assign(in.text, pos, len);
					out.kind.assign(in.kind.begin() + static_cast<std::ptrdiff_t>(pos),
									in.kind.begin() + static_cast<std::ptrdiff_t>(pos + len));
					return out;
				}

				void BlendPixel(uint8_t* px, uint8_t sr, uint8_t sg, uint8_t sb, int32_t sa)
				{
					if (sa <= 0) return;
					const int32_t da = px[3];
					const int32_t out_a = sa + da * (255 - sa) / 255;
					if (out_a <= 0) return;
					px[0] = static_cast<uint8_t>((sr * sa + px[0] * da * (255 - sa) / 255) / out_a);
					px[1] = static_cast<uint8_t>((sg * sa + px[1] * da * (255 - sa) / 255) / out_a);
					px[2] = static_cast<uint8_t>((sb * sa + px[2] * da * (255 - sa) / 255) / out_a);
					px[3] = static_cast<uint8_t>(out_a);
				}

				bool MergePair(const CLV_SubtitleRectC& a, const CLV_SubtitleRectC& b)
				{
					return ! (static_cast<int64_t>(a.x) + a.w < static_cast<int64_t>(b.x) - 1 ||
							  static_cast<int64_t>(b.x) + b.w < static_cast<int64_t>(a.x) - 1 ||
							  static_cast<int64_t>(a.y) + a.h < static_cast<int64_t>(b.y) - 1 ||
							  static_cast<int64_t>(b.y) + b.h < static_cast<int64_t>(a.y) - 1);
				}

				void MergeRects(std::vector<CLV_SubtitleRectC>* rects)
				{
					bool changed = true;
					while (changed)
					{
						changed = false;
						for (size_t i = 0; i < rects->size(); ++i)
						{
							for (size_t j = i + 1; j < rects->size();)
							{
								if (! MergePair((*rects)[i], (*rects)[j]))
								{
									++j;
									continue;
								}
								const int64_t x0 = std::min<int64_t>((*rects)[i].x, (*rects)[j].x);
								const int64_t y0 = std::min<int64_t>((*rects)[i].y, (*rects)[j].y);
								const int64_t x1 =
									std::max<int64_t>(static_cast<int64_t>((*rects)[i].x) + (*rects)[i].w,
													  static_cast<int64_t>((*rects)[j].x) + (*rects)[j].w);
								const int64_t y1 =
									std::max<int64_t>(static_cast<int64_t>((*rects)[i].y) + (*rects)[i].h,
													  static_cast<int64_t>((*rects)[j].y) + (*rects)[j].h);
								(*rects)[i].x = static_cast<int32_t>(x0);
								(*rects)[i].y = static_cast<int32_t>(y0);
								(*rects)[i].w = static_cast<uint32_t>(x1 - x0);
								(*rects)[i].h = static_cast<uint32_t>(y1 - y0);
								rects->erase(rects->begin() + static_cast<std::ptrdiff_t>(j));
								changed = true;
							}
						}
					}
					std::sort(rects->begin(), rects->end(),
							  [](const CLV_SubtitleRectC& a, const CLV_SubtitleRectC& b)
							  {
								  if (a.y != b.y) return a.y < b.y;
								  return a.x < b.x;
							  });
				}
			}	 // namespace

			struct Renderer::Impl
			{
				uint32_t width = 0;
				uint32_t height = 0;
				int metric_kind = 0;
				cal::Cal* cal = nullptr;
				std::vector<std::string> font_dirs;
				std::map<uint32_t, Placed> placed;
				std::map<std::string, std::vector<FacePick>> cand_cache;
				std::map<std::string, bool> cover_cache;
				std::map<std::string, ShapeResult> shape_cache;
				std::map<std::string, std::unique_ptr<cal::Metrics>> metrics;
				std::map<std::string, Bitmap> glyphs;
				std::map<std::string, ColorBitmap> color_glyphs;
				std::map<std::string, StrokeBitmap> stroke_glyphs;
				std::string emoji_path;
				uint32_t emoji_index = 0;
				bool has_emoji = false;
				FacePick tofu_pick;
				bool tofu_ready = false;
				StyleBlock style_block {};
				bool has_style_block = false;
				CLV_SubtitleStyleC default_style {};
				bool has_default_style = false;
				std::vector<CLV_SubtitleRectC> frame_rects;
				CLV_SubtitleRenderStatsC stats {};

				~Impl() { delete cal; }

				Err SelectLocalFace(const char* family, uint32_t weight, int32_t italic, cal::FaceInfo* out, char* path,
									size_t cap, size_t* len) const
				{
					Err rc = kUnsupported;
					for (const std::string& dir: font_dirs)
					{
						rc = cal->SelectFace(dir.c_str(), family, weight, italic, out, path, cap, len);
						if (rc == kOk) return rc;
					}
					return rc;
				}

				FacePick PickTofu()
				{
					if (tofu_ready) return tofu_pick;
					FacePick pick;
					pick.stage = kStageTofu;
					cal::FaceInfo info {};
					char path[1024] = { 0 };
					size_t n = 0;
					bool ok = false;
					for (size_t i = kBaseFamilyCount; i > 0u && ! ok; --i)
						ok = SelectLocalFace(kBaseFamilies[i - 1u], 400u, 0, &info, path, sizeof(path), &n) == kOk;
					if (ok)
					{
						pick.path.assign(path, n);
						pick.face_index = info.face_index;
					}
					else
					{
						pick.face_index = 0u;
					}
					tofu_pick = pick;
					tofu_ready = true;
					return pick;
				}

				static bool SameFace(const FacePick& a, const FacePick& b)
				{
					return a.face_index == b.face_index && a.path == b.path;
				}

				std::vector<FacePick> FaceCandidates(const RStyle& s)
				{
					std::string key;
					for (const std::string& a: s.authors)
					{
						key += a;
						key += '|';
					}
					key += "#" + std::to_string(s.weight) + "#" + std::to_string(s.italic) + (has_emoji ? "#e" : "");
					const auto hit = cand_cache.find(key);
					if (hit != cand_cache.end())
					{
						++stats.cache_hits;
						return hit->second;
					}

					std::vector<FacePick> out;
					char path[1024] = { 0 };
					size_t n = 0;
					cal::FaceInfo info {};
					auto add = [&out](const FacePick& p)
					{
						for (const FacePick& q: out)
							if (SameFace(p, q)) return;
						out.push_back(p);
					};
					auto make = [&](int32_t stage)
					{
						FacePick p;
						p.path.assign(path, n);
						p.face_index = info.face_index;
						p.stage = stage;
						return p;
					};

					for (const std::string& item: s.authors)
						if (SelectLocalFace(item.c_str(), s.weight, s.italic, &info, path, sizeof(path), &n) == kOk)
							add(make(kStageAuthor));
					for (size_t i = 0; i < kBaseFamilyCount; ++i)
						if (SelectLocalFace(kBaseFamilies[i], s.weight, s.italic, &info, path, sizeof(path), &n) == kOk)
							add(make(kStageBase));
					if (has_emoji)
					{
						FacePick p;
						p.path = emoji_path;
						p.face_index = emoji_index;
						p.stage = kStageEmoji;
						add(p);
					}
					{
						std::vector<std::string> items = s.authors;
						if (items.empty())
							for (size_t i = 0; i < kBaseFamilyCount; ++i) items.push_back(kBaseFamilies[i]);
						for (const std::string& item: items)
						{
							if (cal->SelectSystemFace(item.c_str(), s.weight, s.italic, &info, path, sizeof(path),
													  &n) == kOk)
							{
								add(make(kStageSystem));
							}
						}
					}
					cand_cache.emplace(key, out);
					return out;
				}

				bool FaceCovers(const FacePick& f, uint32_t cp)
				{
					const std::string key = f.path + "@" + std::to_string(f.face_index) + "#" + std::to_string(cp);
					const auto it = cover_cache.find(key);
					if (it != cover_cache.end())
					{
						++stats.cache_hits;
						return it->second;
					}
					int32_t cov = 0;
					if (cal->FaceCovers(f.path.c_str(), f.face_index, cp, &cov) != kOk) cov = 0;
					cover_cache.emplace(key, cov != 0);
					return cov != 0;
				}

				ShapeResult ShapeText(const std::string& path, uint32_t index, uint32_t size, const std::string& text)
				{
					const std::string key =
						path + "@" + std::to_string(index) + "#" + std::to_string(size) + ":" + text;
					const auto hit = shape_cache.find(key);
					if (hit != shape_cache.end())
					{
						++stats.cache_hits;
						return hit->second;
					}
					ShapeResult res;
					const size_t cap = text.size() + 8u;
					res.glyphs.resize(cap);
					res.advances.resize(cap);
					size_t count = 0;
					int32_t rtl = 0;
					if (cal->Shape(path.c_str(), index, reinterpret_cast<const uint8_t*>(text.data()), text.size(),
								   size, res.glyphs.data(), res.advances.data(), cap, &count, &rtl) == kOk)
					{
						res.glyphs.resize(count);
						res.advances.resize(count);
						res.rtl = rtl;
						res.ok = true;
					}
					else
					{
						res.glyphs.clear();
						res.advances.clear();
					}
					shape_cache.emplace(key, res);
					return res;
				}

				int32_t NotdefAdvance(const std::string& path, uint32_t index, uint32_t size)
				{
					if (path.empty()) return static_cast<int32_t>(size);
					static const std::string probe("\xF4\x8F\xBF\xBF", 4);
					const ShapeResult sr = ShapeText(path, index, size, probe);
					return (sr.ok && ! sr.advances.empty()) ? sr.advances[0] / 64 : static_cast<int32_t>(size);
				}

				cal::Metrics* GetMetrics(const std::string& path, uint32_t face_index, uint32_t size)
				{
					const std::string key = path + "@" + std::to_string(face_index) + "#" + std::to_string(size);
					const auto it = metrics.find(key);
					if (it != metrics.end()) return it->second.get();
					cal::Metrics* m = cal->OpenMetrics(path.c_str(), face_index, size, metric_kind);
					metrics.emplace(key, std::unique_ptr<cal::Metrics>(m));
					return m;
				}

				const Bitmap* Glyph(const std::string& path, uint32_t face_index, uint32_t size, uint32_t glyph_id)
				{
					const std::string key = path + "@" + std::to_string(face_index) + "#" + std::to_string(size) + "#" +
											std::to_string(glyph_id);
					const auto it = glyphs.find(key);
					if (it != glyphs.end()) return &it->second;
					Bitmap bm;
					uint8_t* px = nullptr;
					uint32_t w = 0;
					uint32_t h = 0;
					int32_t left = 0;
					int32_t top = 0;
					if (cal->RasterGlyph(path.c_str(), face_index, size, glyph_id, &px, &w, &h, &left, &top) == kOk)
					{
						bm.w = w;
						bm.h = h;
						bm.left = left;
						bm.top = top;
						const size_t n = static_cast<size_t>(w) * h;
						if (n != 0u && px != nullptr) bm.px.assign(px, px + n);
						std::free(px);
					}
					const auto ins = glyphs.emplace(key, std::move(bm));
					return &ins.first->second;
				}

				const ColorBitmap* ColorGlyph(const std::string& path, uint32_t face_index, uint32_t size,
											  uint32_t glyph_id)
				{
					const std::string key = path + "@" + std::to_string(face_index) + "#" + std::to_string(size) + "#" +
											std::to_string(glyph_id) + "#c";
					const auto it = color_glyphs.find(key);
					if (it != color_glyphs.end()) return &it->second;
					ColorBitmap bm;
					uint8_t* px = nullptr;
					uint32_t w = 0;
					uint32_t h = 0;
					int32_t left = 0;
					int32_t top = 0;
					int32_t fmt = cal::kGlyphA8;
					if (cal->RasterGlyphFmt(path.c_str(), face_index, size, glyph_id, &px, &w, &h, &left, &top, &fmt) ==
						kOk)
					{
						bm.w = w;
						bm.h = h;
						bm.left = left;
						bm.top = top;
						bm.color = fmt == cal::kGlyphRgba8;
						const size_t n = static_cast<size_t>(w) * h * (bm.color ? 4u : 1u);
						if (n != 0u && px != nullptr) bm.px.assign(px, px + n);
						std::free(px);
					}
					const auto ins = color_glyphs.emplace(key, std::move(bm));
					return &ins.first->second;
				}

				const StrokeBitmap* StrokeGlyph(const std::string& path, uint32_t face_index, uint32_t size,
												uint32_t glyph_id, uint32_t stroke_w)
				{
					const std::string key = path + "@" + std::to_string(face_index) + "#" + std::to_string(size) + "#" +
											std::to_string(glyph_id) + "#s" + std::to_string(stroke_w);
					const auto it = stroke_glyphs.find(key);
					if (it != stroke_glyphs.end()) return &it->second;
					StrokeBitmap bm;
					uint8_t* f = nullptr;
					uint8_t* o = nullptr;
					uint32_t w = 0;
					uint32_t h = 0;
					int32_t left = 0;
					int32_t top = 0;
					if (cal->RasterGlyphStroked(path.c_str(), face_index, size, glyph_id, stroke_w, &f, &o, &w, &h,
												&left, &top) == kOk)
					{
						bm.w = w;
						bm.h = h;
						bm.left = left;
						bm.top = top;
						const size_t n = static_cast<size_t>(w) * h;
						if (n != 0u)
						{
							if (f != nullptr) bm.fill.assign(f, f + n);
							if (o != nullptr) bm.outline.assign(o, o + n);
						}
						std::free(f);
						std::free(o);
					}
					const auto ins = stroke_glyphs.emplace(key, std::move(bm));
					return &ins.first->second;
				}

				static void MergeMissing(CLV_SubtitleStyleC* dst, const CLV_SubtitleStyleC& src)
				{
					if ((dst->flags & src.flags) == src.flags) return;
					const uint32_t take = src.flags & ~dst->flags;
					if ((take & CLV_SUBTITLE_STYLE_FONT_COLOR) != 0u) dst->font_color_rgba = src.font_color_rgba;
					if ((take & CLV_SUBTITLE_STYLE_STROKE_COLOR) != 0u) dst->stroke_color_rgba = src.stroke_color_rgba;
					if ((take & CLV_SUBTITLE_STYLE_FONT_SIZE) != 0u)
					{
						dst->font_size = src.font_size;
						dst->font_size_unit = src.font_size_unit;
					}
					if ((take & CLV_SUBTITLE_STYLE_FONT_WEIGHT) != 0u) dst->font_weight = src.font_weight;
					if ((take & CLV_SUBTITLE_STYLE_FONT_STYLE) != 0u) dst->font_style = src.font_style;
					if ((take & CLV_SUBTITLE_STYLE_TEXT_ALIGN) != 0u) dst->text_align = src.text_align;
					if ((take & CLV_SUBTITLE_STYLE_POSITION) != 0u)
					{
						dst->pos_x = src.pos_x;
						dst->pos_y = src.pos_y;
						dst->pos_unit = src.pos_unit;
					}
					if ((take & CLV_SUBTITLE_STYLE_LINE_HEIGHT) != 0u)
					{
						dst->line_height = src.line_height;
						dst->line_height_unit = src.line_height_unit;
					}
					if ((take & CLV_SUBTITLE_STYLE_OPACITY) != 0u) dst->opacity = src.opacity;
					if ((take & CLV_SUBTITLE_STYLE_SCROLL) != 0u) dst->scroll = src.scroll;
					if ((take & CLV_SUBTITLE_STYLE_SCROLL_RECT) != 0u)
					{
						dst->scroll_x = src.scroll_x;
						dst->scroll_y = src.scroll_y;
						dst->scroll_w = src.scroll_w;
						dst->scroll_h = src.scroll_h;
						dst->scroll_unit = src.scroll_unit;
					}
					if ((take & CLV_SUBTITLE_STYLE_FONT_FAMILY) != 0u)
					{
						dst->font_family = src.font_family;
						dst->font_family_size = src.font_family_size;
					}
					if ((take & CLV_SUBTITLE_STYLE_STROKE_WIDTH) != 0u) dst->stroke_width = src.stroke_width;
					dst->flags |= take;
				}

				RStyle ResolveStyle(const CLV_SubtitleBlockC& block, const CLV_SubtitleStyleC* style)
				{
					CLV_SubtitleStyleC raw = style != nullptr ? *style : block.style;
					if (has_default_style) MergeMissing(&raw, default_style);
					uint32_t flags = raw.flags;
					if (has_style_block)
					{
						/* 宿主给了最终值：样式块只补它没给的字段（覆盖链第 2 档的位置） */
						if ((flags & CLV_SUBTITLE_STYLE_FONT_SIZE) == 0u &&
							(style_block.flags & CLV_SUBTITLE_STYLE_FONT_SIZE) != 0u)
						{
							raw.font_size = style_block.font_size;
							raw.font_size_unit = style_block.font_size_unit;
							flags |= CLV_SUBTITLE_STYLE_FONT_SIZE;
						}
						if ((flags & CLV_SUBTITLE_STYLE_FONT_COLOR) == 0u &&
							(style_block.flags & CLV_SUBTITLE_STYLE_FONT_COLOR) != 0u)
						{
							raw.font_color_rgba = style_block.font_color_rgba;
							flags |= CLV_SUBTITLE_STYLE_FONT_COLOR;
						}
						if ((flags & CLV_SUBTITLE_STYLE_STROKE_COLOR) == 0u &&
							(style_block.flags & CLV_SUBTITLE_STYLE_STROKE_COLOR) != 0u)
						{
							raw.stroke_color_rgba = style_block.stroke_color_rgba;
							flags |= CLV_SUBTITLE_STYLE_STROKE_COLOR;
						}
						if ((flags & CLV_SUBTITLE_STYLE_STROKE_WIDTH) == 0u &&
							(style_block.flags & CLV_SUBTITLE_STYLE_STROKE_WIDTH) != 0u)
						{
							raw.stroke_width = style_block.stroke_width;
							flags |= CLV_SUBTITLE_STYLE_STROKE_WIDTH;
						}
						if ((flags & CLV_SUBTITLE_STYLE_FONT_FAMILY) == 0u &&
							(style_block.flags & CLV_SUBTITLE_STYLE_FONT_FAMILY) != 0u)
						{
							raw.font_family = style_block.font_family;
							raw.font_family_size = style_block.font_family_size;
							flags |= CLV_SUBTITLE_STYLE_FONT_FAMILY;
						}
						if (flags != raw.flags) stats.style_blocks_applied += 1u;
					}

					RStyle s;
					if ((flags & CLV_SUBTITLE_STYLE_FONT_SIZE) != 0u)
					{
						s.font_size = raw.font_size_unit == CLV_SUBTITLE_UNIT_PX ?
										  static_cast<int32_t>(raw.font_size) :
										  static_cast<int32_t>(static_cast<int64_t>(height) * raw.font_size / 1000);
					}
					else
					{
						s.font_size = static_cast<int32_t>(static_cast<int64_t>(height) * 40 / 1000);
					}
					if (s.font_size < 1) s.font_size = 1;

					if ((flags & CLV_SUBTITLE_STYLE_LINE_HEIGHT) != 0u)
					{
						s.line_height =
							raw.line_height_unit == CLV_SUBTITLE_UNIT_PX ?
								static_cast<int32_t>(raw.line_height) :
								static_cast<int32_t>(static_cast<int64_t>(s.font_size) * raw.line_height / 1000);
					}
					else
					{
						s.line_height = static_cast<int32_t>(static_cast<int64_t>(s.font_size) * 1200 / 1000);
					}
					if (s.line_height < 1) s.line_height = 1;

					s.color = (flags & CLV_SUBTITLE_STYLE_FONT_COLOR) != 0u ? raw.font_color_rgba : 0xFFFFFFFFu;
					s.stroke_color =
						(flags & CLV_SUBTITLE_STYLE_STROKE_COLOR) != 0u ? raw.stroke_color_rgba : 0xFF000000u;
					s.stroke_w = (flags & CLV_SUBTITLE_STYLE_STROKE_WIDTH) != 0u ? raw.stroke_width : 0;
					if (s.stroke_w < 0) s.stroke_w = 0;
					s.opacity = (flags & CLV_SUBTITLE_STYLE_OPACITY) != 0u ? raw.opacity : 1000;
					if (s.opacity < 0) s.opacity = 0;
					if (s.opacity > 1000) s.opacity = 1000;
					s.align = (flags & CLV_SUBTITLE_STYLE_TEXT_ALIGN) != 0u ? raw.text_align : 1;
					if (s.align < 0 || s.align > 2) s.align = 1;

					s.has_pos = (flags & CLV_SUBTITLE_STYLE_POSITION) != 0u;
					if (s.has_pos)
					{
						s.pos_x = raw.pos_x;
						s.pos_y = raw.pos_y;
						s.pos_unit = raw.pos_unit;
					}
					s.scroll = (flags & CLV_SUBTITLE_STYLE_SCROLL) != 0u ? raw.scroll : 0;
					if (s.scroll < 0 || s.scroll > 4) s.scroll = 0;
					s.has_rect = (flags & CLV_SUBTITLE_STYLE_SCROLL_RECT) != 0u;
					s.rect_x = raw.scroll_x;
					s.rect_y = raw.scroll_y;
					s.rect_w = raw.scroll_w;
					s.rect_h = raw.scroll_h;
					s.rect_unit = raw.scroll_unit;
					if ((flags & CLV_SUBTITLE_STYLE_FONT_FAMILY) != 0u && raw.font_family != nullptr &&
						raw.font_family_size != 0u)
					{
						s.authors = SplitFamilies(
							std::string(reinterpret_cast<const char*>(raw.font_family), raw.font_family_size));
					}
					s.weight = (flags & CLV_SUBTITLE_STYLE_FONT_WEIGHT) != 0u && raw.font_weight != 0u ?
								   static_cast<uint32_t>(raw.font_weight) :
								   400u;
					s.italic = ((flags & CLV_SUBTITLE_STYLE_FONT_STYLE) != 0u && raw.font_style == 1u) ? 1 : 0;
					return s;
				}

				static View ViewportOf(const RStyle& s, uint32_t screen_w, uint32_t screen_h)
				{
					View v { 0, 0, static_cast<int64_t>(screen_w), static_cast<int64_t>(screen_h) };
					if (s.scroll == 0 || ! s.has_rect) return v;
					auto conv = [](int32_t value, uint8_t unit, int64_t basis) -> int64_t
					{ return unit == CLV_SUBTITLE_UNIT_PCT ? basis * value / 1000 : value; };
					v.x = conv(s.rect_x, s.rect_unit, screen_w);
					v.y = conv(s.rect_y, s.rect_unit, screen_h);
					v.w = conv(s.rect_w, s.rect_unit, screen_w);
					v.h = conv(s.rect_h, s.rect_unit, screen_h);
					if (v.w <= 0 || v.h <= 0)
					{
						v.x = 0;
						v.y = 0;
						v.w = screen_w;
						v.h = screen_h;
					}
					return v;
				}

				void SplitRun(const std::vector<FacePick>& cands, const Line& line, size_t run_off, size_t run_len,
							  std::vector<std::pair<FacePick, SegLayout>>* out)
				{
					size_t p = run_off;
					const size_t end = run_off + run_len;
					while (p < end)
					{
						const uint8_t kind = line.kind[p];
						size_t e = p + 1u;
						while (e < end && line.kind[e] == kind) ++e;

						if (kind == CLV_SUBTITLE_SPAN_EMOJI)
						{
							SegLayout seg;
							seg.off = static_cast<uint32_t>(p);
							seg.len = static_cast<uint32_t>(e - p);
							FacePick face;
							bool found = false;
							for (const FacePick& c: cands)
							{
								if (c.stage == kStageEmoji)
								{
									face = c;
									found = true;
									break;
								}
							}
							if (! found)
							{
								face = PickTofu();
								seg.tofu = true;
							}
							out->emplace_back(face, seg);
							p = e;
							continue;
						}

						size_t q = p;
						while (q < e)
						{
							size_t clen = 1;
							const uint32_t cp = CpAt(line.text, q, &clen);
							int ci = -1;
							for (size_t i = 0; i < cands.size(); ++i)
							{
								if (FaceCovers(cands[i], cp))
								{
									ci = static_cast<int>(i);
									break;
								}
							}
							const size_t s0 = q;
							SegLayout seg;
							FacePick face;
							if (ci < 0)
							{
								for (;;)
								{
									size_t l2 = 1;
									const uint32_t c2 = CpAt(line.text, q, &l2);
									bool any = false;
									for (const FacePick& c: cands)
									{
										if (FaceCovers(c, c2))
										{
											any = true;
											break;
										}
									}
									if (any) break;
									q += l2;
									if (q >= e) break;
								}
								seg.tofu = true;
								face = PickTofu();
							}
							else
							{
								for (;;)
								{
									size_t l2 = 1;
									const uint32_t c2 = CpAt(line.text, q, &l2);
									bool cov = FaceCovers(cands[static_cast<size_t>(ci)], c2);
									if (! cov && cands[static_cast<size_t>(ci)].stage == kStageEmoji &&
										(c2 == 0x200Du || c2 == 0xFE0Fu))
									{
										cov = true;
									}
									if (! cov) break;
									q += l2;
									if (q >= e) break;
								}
								face = cands[static_cast<size_t>(ci)];
							}
							seg.off = static_cast<uint32_t>(s0);
							seg.len = static_cast<uint32_t>(q - s0);
							out->emplace_back(face, seg);
						}
						p = e;
					}
				}

				LineLayout BuildLineLayout(const Placed& p, const Line& line)
				{
					LineLayout ll;
					if (line.text.empty()) return ll;
					const uint32_t size = static_cast<uint32_t>(p.style.font_size);
					const std::vector<FacePick> cands = FaceCandidates(p.style);

					std::vector<cal::BidiRun> runs(line.text.size() + 2u);
					size_t run_count = 0;
					if (cal->BidiRuns(reinterpret_cast<const uint8_t*>(line.text.data()), line.text.size(), runs.data(),
									  runs.size(), &run_count) != kOk ||
						run_count == 0u)
					{
						runs.resize(1);
						runs[0].off = 0;
						runs[0].len = static_cast<uint32_t>(line.text.size());
						runs[0].level = 0;
						run_count = 1;
					}
					runs.resize(run_count);

					for (const cal::BidiRun& run: runs)
					{
						std::vector<std::pair<FacePick, SegLayout>> local;
						SplitRun(cands, line, run.off, run.len, &local);
						if ((run.level & 1) != 0) std::reverse(local.begin(), local.end());
						for (auto& item: local)
						{
							SegLayout& seg = item.second;
							const FacePick& face = item.first;
							seg.path = face.path;
							seg.face_index = face.face_index;
							seg.stage = face.stage;
							seg.color = (face.stage == kStageEmoji) && ! seg.tofu;

							if (! seg.tofu)
							{
								const std::string piece(line.text.data() + seg.off, seg.len);
								const ShapeResult sr = ShapeText(seg.path, seg.face_index, size, piece);
								if (sr.ok)
								{
									seg.glyphs = sr.glyphs;
									seg.advances = sr.advances;
								}
								else
								{
									seg.tofu = true;
									seg.color = false;
								}
							}
							if (seg.tofu)
							{
								size_t cells = 0;
								for (size_t i = seg.off; i < seg.off + seg.len; ++i)
									if ((static_cast<uint8_t>(line.text[i]) & 0xC0u) != 0x80u) ++cells;
								const int32_t adv = NotdefAdvance(seg.path, seg.face_index, size);
								seg.glyphs.assign(cells, 0u);
								seg.advances.assign(cells, adv);
							}

							seg.x = static_cast<int32_t>(ll.width);
							for (int32_t adv: seg.advances)
							{
								const int32_t step = adv / 64;
								if (step > 0) ll.width += step;
							}
							ll.segs.push_back(std::move(seg));
						}
					}
					return ll;
				}

				void LayoutLogicalLine(Placed& p, const Line& text, int32_t avail)
				{
					auto emit = [&p](const std::string& piece, LineLayout ll)
					{
						p.pieces.push_back(piece);
						p.lines.push_back(std::move(ll));
					};
					if (text.text.empty())
					{
						emit(std::string(), LineLayout());
						return;
					}
					size_t pos = 0;
					while (pos < text.text.size())
					{
						const Line piece = SliceLine(text, pos, text.text.size() - pos);
						LineLayout ll = BuildLineLayout(p, piece);
						if (avail <= 0 || ll.width <= avail)
						{
							emit(piece.text, std::move(ll));
							return;
						}
						size_t fit = 0;
						{
							size_t i = pos;
							size_t w = 0;
							while (i < text.text.size())
							{
								size_t n = 1;
								CpAt(text.text, i, &n);
								const LineLayout one = BuildLineLayout(p, SliceLine(text, i, n));
								const int64_t step = one.width;
								if (step > 0 && w + static_cast<uint64_t>(step) > static_cast<uint64_t>(avail) &&
									i > pos)
								{
									break;
								}
								w += static_cast<size_t>(step);
								i += n;
								if (i > pos) fit = i;
							}
						}
						size_t brk = 0;
						for (size_t i = fit; i > pos; --i)
						{
							if (BreakAllowed(text.text, pos, i))
							{
								brk = i;
								break;
							}
						}
						if (brk == 0u) brk = (fit > pos) ? fit : pos + 1u;
						size_t end = brk;
						while (end > pos && text.text[end - 1u] == ' ') --end;
						const Line head = SliceLine(text, pos, end - pos);
						emit(head.text, BuildLineLayout(p, head));
						pos = brk;
						while (pos < text.text.size() && text.text[pos] == ' ') ++pos;
					}
				}

				bool BuildPlaced(const CLV_SubtitleBlockC& block, const std::vector<Line>& lines, int64_t time_ticks,
								 RStyle style, Placed* out)
				{
					if (time_ticks < block.start_ticks || time_ticks >= block.end_ticks) return false;
					if (block.reason_mask != 0u || lines.empty()) return false;

					const std::vector<FacePick> cands = FaceCandidates(style);
					const FacePick pick = cands.empty() ? PickTofu() : cands.front();

					Placed p;
					p.time_ticks = time_ticks;
					p.start = block.start_ticks;
					p.end = block.end_ticks;
					p.style = style;
					p.font_path = pick.path;
					p.face_index = pick.face_index;
					p.stage = pick.stage;
					p.tofu = pick.stage == kStageTofu;
					p.stroke_w = style.stroke_w;
					p.stroke_color = style.stroke_color;
					p.is_scroll = style.scroll != 0;
					p.view = ViewportOf(style, width, height);

					for (const Line& src: lines)
					{
						size_t start = 0;
						for (;;)
						{
							const size_t nl = src.text.find('\n', start);
							const size_t len = nl == std::string::npos ? src.text.size() - start : nl - start;
							LayoutLogicalLine(p, SliceLine(src, start, len), static_cast<int32_t>(width));
							if (nl == std::string::npos) break;
							start = nl + 1u;
						}
					}

					{
						const SegLayout* primary = nullptr;
						for (const LineLayout& ll: p.lines)
						{
							for (const SegLayout& seg: ll.segs)
							{
								if (! seg.tofu)
								{
									primary = &seg;
									break;
								}
							}
							if (primary != nullptr) break;
						}
						if (primary != nullptr)
						{
							p.font_path = primary->path;
							p.face_index = primary->face_index;
							p.stage = primary->stage;
							p.tofu = false;
						}
						else
						{
							const FacePick tf = PickTofu();
							p.font_path = tf.path;
							p.face_index = tf.face_index;
							p.stage = kStageTofu;
							p.tofu = true;
						}
					}

					int64_t max_w = 0;
					for (const LineLayout& ll: p.lines)
						if (ll.width > max_w) max_w = ll.width;
					p.content_w = max_w + 2 * static_cast<int64_t>(p.stroke_w);
					p.content_h =
						static_cast<int64_t>(p.lines.size()) * style.line_height + 2 * static_cast<int64_t>(p.stroke_w);
					if (p.content_w <= 0) p.content_w = 1;
					if (p.content_h <= 0) p.content_h = style.line_height;

					if (style.has_pos)
					{
						const int64_t basis_x = (p.is_scroll && style.has_rect) ? p.view.x : 0;
						const int64_t basis_y = (p.is_scroll && style.has_rect) ? p.view.y : 0;
						const int64_t basis_w = (p.is_scroll && style.has_rect) ? p.view.w : width;
						const int64_t basis_h = (p.is_scroll && style.has_rect) ? p.view.h : height;
						p.center_x = basis_x + (style.pos_unit == CLV_SUBTITLE_UNIT_PCT ? basis_w * style.pos_x / 1000 :
																						  style.pos_x);
						p.center_y = basis_y + (style.pos_unit == CLV_SUBTITLE_UNIT_PCT ? basis_h * style.pos_y / 1000 :
																						  style.pos_y);
					}
					else if (p.is_scroll)
					{
						p.center_x = p.view.x + p.view.w / 2;
						p.center_y = p.view.y + p.view.h * 900 / 1000;
					}
					else
					{
						p.center_x = width / 2;
						p.center_y = static_cast<int64_t>(height) * 900 / 1000;
					}

					*out = std::move(p);
					return true;
				}

				static bool ScrollProgress(const Placed& p, int64_t time_ticks, int64_t* axis, int64_t* offset)
				{
					if (! p.is_scroll) return false;
					const int64_t den = p.end - p.start;
					if (den <= 0) return false;
					const int64_t num = ClampI64(time_ticks - p.start, 0, den);
					const bool vertical =
						p.style.scroll == CLV_SUBTITLE_SCROLL_TOP || p.style.scroll == CLV_SUBTITLE_SCROLL_BOTTOM;
					*axis = vertical ? (p.view.h + p.content_h) : (p.view.w + p.content_w);
					*offset = *axis * num / den;
					return true;
				}

				void ContentOrigin(const Placed& p, int64_t time_ticks, int64_t* left, int64_t* top) const
				{
					*left = p.center_x - p.content_w / 2;
					*top = p.center_y - p.content_h / 2;
					if (! p.is_scroll) return;
					int64_t axis = 0;
					int64_t offset = 0;
					if (! ScrollProgress(p, time_ticks, &axis, &offset)) return;
					if (p.style.scroll == CLV_SUBTITLE_SCROLL_TOP || p.style.scroll == CLV_SUBTITLE_SCROLL_BOTTOM)
					{
						if (p.content_w <= p.view.w)
							*left = ClampI64(*left, p.view.x, p.view.x + p.view.w - p.content_w);
					}
					else
					{
						if (p.content_h <= p.view.h) *top = ClampI64(*top, p.view.y, p.view.y + p.view.h - p.content_h);
					}
					if (p.style.scroll == CLV_SUBTITLE_SCROLL_TOP) *top = p.view.y + p.view.h - offset;
					else if (p.style.scroll == CLV_SUBTITLE_SCROLL_BOTTOM) *top = p.view.y - p.content_h + offset;
					else if (p.style.scroll == CLV_SUBTITLE_SCROLL_LEFT) *left = p.view.x + p.view.w - offset;
					else if (p.style.scroll == CLV_SUBTITLE_SCROLL_RIGHT) *left = p.view.x - p.content_w + offset;
				}

				void Draw(const Placed& p, int64_t time_ticks, uint8_t* rgba, size_t stride, uint32_t fb_w,
						  uint32_t fb_h, std::vector<CLV_SubtitleRectC>* rects)
				{
					int64_t left = 0;
					int64_t top = 0;
					ContentOrigin(p, time_ticks, &left, &top);

					int64_t clip_x0 = 0;
					int64_t clip_y0 = 0;
					int64_t clip_x1 = fb_w;
					int64_t clip_y1 = fb_h;
					if (p.is_scroll)
					{
						clip_x0 = std::max<int64_t>(clip_x0, p.view.x);
						clip_y0 = std::max<int64_t>(clip_y0, p.view.y);
						clip_x1 = std::min<int64_t>(clip_x1, p.view.x + p.view.w);
						clip_y1 = std::min<int64_t>(clip_y1, p.view.y + p.view.h);
					}
					if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0) return;

					int32_t asc = 0;
					int32_t desc = 0;
					int32_t gap = 0;
					if (! p.tofu)
					{
						cal::Metrics* m =
							GetMetrics(p.font_path, p.face_index, static_cast<uint32_t>(p.style.font_size));
						if (m == nullptr || m->Extents(&asc, &desc, &gap) != kOk) asc = 0;
					}
					if (asc <= 0)
					{
						asc = p.style.font_size * 64;
						desc = p.style.font_size * 64 / 4;
						gap = 0;
					}
					const int32_t asc_px = asc / 64;
					(void) gap;

					const int32_t opacity = p.style.opacity;
					const uint32_t color = p.style.color;
					const uint32_t stroke_color = p.stroke_color;
					const int32_t sw = p.stroke_w;
					const int64_t inner_w = p.content_w - 2 * static_cast<int64_t>(sw);
					const uint8_t cr = static_cast<uint8_t>(color & 0xFFu);
					const uint8_t cg = static_cast<uint8_t>((color >> 8) & 0xFFu);
					const uint8_t cb = static_cast<uint8_t>((color >> 16) & 0xFFu);
					const int32_t base_alpha =
						static_cast<int32_t>(static_cast<uint32_t>((color >> 24) & 0xFFu) * opacity / 1000u);
					const uint8_t scr = static_cast<uint8_t>(stroke_color & 0xFFu);
					const uint8_t scg = static_cast<uint8_t>((stroke_color >> 8) & 0xFFu);
					const uint8_t scb = static_cast<uint8_t>((stroke_color >> 16) & 0xFFu);
					const int32_t stroke_alpha =
						static_cast<int32_t>(static_cast<uint32_t>((stroke_color >> 24) & 0xFFu) * opacity / 1000u);

					auto composite_a8 = [&](int64_t dx0, int64_t dy0, uint32_t w, uint32_t h, const uint8_t* cov,
											uint8_t fr, uint8_t fg, uint8_t fb, int32_t alpha)
					{
						for (uint32_t yy = 0; yy < h; ++yy)
						{
							const int64_t dy = dy0 + static_cast<int64_t>(yy);
							if (dy < clip_y0 || dy >= clip_y1) continue;
							for (uint32_t xx = 0; xx < w; ++xx)
							{
								const int64_t dx = dx0 + static_cast<int64_t>(xx);
								if (dx < clip_x0 || dx >= clip_x1) continue;
								const uint8_t c = cov[static_cast<size_t>(yy) * w + xx];
								if (c == 0u) continue;
								const int32_t sa = static_cast<int32_t>(c) * alpha / 255;
								if (sa <= 0) continue;
								BlendPixel(rgba + static_cast<size_t>(dy) * stride + static_cast<size_t>(dx) * 4u, fr,
										   fg, fb, sa);
							}
						}
					};
					auto composite_rgba = [&](int64_t dx0, int64_t dy0, uint32_t w, uint32_t h, const uint8_t* src)
					{
						for (uint32_t yy = 0; yy < h; ++yy)
						{
							const int64_t dy = dy0 + static_cast<int64_t>(yy);
							if (dy < clip_y0 || dy >= clip_y1) continue;
							for (uint32_t xx = 0; xx < w; ++xx)
							{
								const int64_t dx = dx0 + static_cast<int64_t>(xx);
								if (dx < clip_x0 || dx >= clip_x1) continue;
								const uint8_t* s = src + (static_cast<size_t>(yy) * w + xx) * 4u;
								if (s[3] == 0u) continue;
								const int32_t sa = static_cast<int32_t>(s[3]) * opacity / 1000;
								if (sa <= 0) continue;
								BlendPixel(rgba + static_cast<size_t>(dy) * stride + static_cast<size_t>(dx) * 4u, s[0],
										   s[1], s[2], sa);
							}
						}
					};
					auto emit_rect = [&](int64_t dx0, int64_t dy0, uint32_t w, uint32_t h)
					{
						if (w == 0u || h == 0u) return;
						CLV_SubtitleRectC r;
						const int64_t rx0 = std::max<int64_t>(dx0, clip_x0);
						const int64_t ry0 = std::max<int64_t>(dy0, clip_y0);
						const int64_t rx1 = std::min<int64_t>(dx0 + static_cast<int64_t>(w), clip_x1);
						const int64_t ry1 = std::min<int64_t>(dy0 + static_cast<int64_t>(h), clip_y1);
						if (rx1 <= rx0 || ry1 <= ry0) return;
						r.x = static_cast<int32_t>(rx0);
						r.y = static_cast<int32_t>(ry0);
						r.w = static_cast<uint32_t>(rx1 - rx0);
						r.h = static_cast<uint32_t>(ry1 - ry0);
						rects->push_back(r);
					};

					for (size_t li = 0; li < p.lines.size(); ++li)
					{
						const LineLayout& ll = p.lines[li];
						int64_t align_off = 0;
						if (p.style.align == 1) align_off = (inner_w - ll.width) / 2;
						else if (p.style.align == 2) align_off = inner_w - ll.width;
						if (align_off < 0) align_off = 0;

						int64_t pen = left + sw + align_off;
						const int64_t baseline = top + sw + static_cast<int64_t>(li) * p.style.line_height + asc_px;
						for (const SegLayout& seg: ll.segs)
						{
							for (size_t i = 0; i < seg.glyphs.size(); ++i)
							{
								const uint32_t gid = seg.glyphs[i];
								const uint32_t size = static_cast<uint32_t>(p.style.font_size);
								if (seg.color)
								{
									const ColorBitmap* bm = ColorGlyph(seg.path, seg.face_index, size, gid);
									if (bm != nullptr && bm->w != 0u && bm->h != 0u)
									{
										const int64_t dx0 = pen + bm->left;
										const int64_t dy0 = baseline - bm->top;
										if (bm->color) composite_rgba(dx0, dy0, bm->w, bm->h, bm->px.data());
										else
											composite_a8(dx0, dy0, bm->w, bm->h, bm->px.data(), cr, cg, cb, base_alpha);
										emit_rect(dx0, dy0, bm->w, bm->h);
									}
								}
								else if (sw > 0)
								{
									const StrokeBitmap* bm =
										StrokeGlyph(seg.path, seg.face_index, size, gid, static_cast<uint32_t>(sw));
									if (bm != nullptr && bm->w != 0u && bm->h != 0u && ! bm->fill.empty() &&
										! bm->outline.empty())
									{
										const int64_t dx0 = pen + bm->left;
										const int64_t dy0 = baseline - bm->top;
										composite_a8(dx0, dy0, bm->w, bm->h, bm->outline.data(), scr, scg, scb,
													 stroke_alpha);
										composite_a8(dx0, dy0, bm->w, bm->h, bm->fill.data(), cr, cg, cb, base_alpha);
										emit_rect(dx0, dy0, bm->w, bm->h);
									}
								}
								else
								{
									const Bitmap* bm = Glyph(seg.path, seg.face_index, size, gid);
									if (bm != nullptr && bm->w != 0u && bm->h != 0u)
									{
										const int64_t dx0 = pen + bm->left;
										const int64_t dy0 = baseline - bm->top;
										composite_a8(dx0, dy0, bm->w, bm->h, bm->px.data(), cr, cg, cb, base_alpha);
										emit_rect(dx0, dy0, bm->w, bm->h);
									}
								}
								pen += seg.advances[i] / 64;
								stats.glyphs += 1u;
								if (seg.tofu)
								{
									stats.missing_glyphs += 1u;
								}
								else if (seg.stage == kStageBase || seg.stage == kStageSystem ||
										 seg.stage == kStageEmoji)
								{
									stats.fallback_fonts += 1u;
								}
							}
						}
					}
				}

				void FindEmojiFace()
				{
					for (const std::string& dir: font_dirs)
					{
						std::error_code ec;
						for (const auto& entry: std::filesystem::directory_iterator(dir, ec))
						{
							if (! entry.is_regular_file()) continue;
							std::string name = entry.path().filename().string();
							for (char& c: name)
								if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
							if (name.find("emoji") == std::string::npos) continue;
							emoji_path = entry.path().string();
							emoji_index = 0;
							has_emoji = true;
							return;
						}
					}
				}
			};

			Renderer::Renderer(uint32_t width, uint32_t height, int metric_kind): impl_(new Impl())
			{
				impl_->width = width;
				impl_->height = height;
				impl_->metric_kind = metric_kind;
			}

			bool Renderer::Init()
			{
				impl_->cal = cal::Cal::Create(false);
				if (impl_->cal == nullptr) return false;
				impl_->stats.struct_size = sizeof(CLV_SubtitleRenderStatsC);
				return true;
			}

			Renderer* Renderer::Create(uint32_t width, uint32_t height, int metric_kind,
									   const CLV_SubtitleStyleC* renderer_default)
			{
				if (width == 0u || height == 0u) return nullptr;
				Renderer* r = new (std::nothrow) Renderer(width, height, metric_kind);
				if (r == nullptr) return nullptr;
				if (! r->Init())
				{
					delete r;
					return nullptr;
				}
				if (renderer_default != nullptr)
				{
					r->impl_->default_style = *renderer_default;
					r->impl_->default_style.struct_size = sizeof(CLV_SubtitleStyleC);
					r->impl_->has_default_style = true;
				}
				return r;
			}

			Renderer::~Renderer() = default;

			Err Renderer::SetStyleBlock(const uint8_t* payload, size_t size)
			{
				if (payload == nullptr || size == 0u)
				{
					impl_->style_block = StyleBlock {};
					impl_->has_style_block = false;
					return kOk;
				}
				StyleBlock sb {};
				const Err e = ParseStyleBlock(payload, size, &sb);
				if (e != kOk) return e;
				impl_->style_block = sb;
				impl_->has_style_block = true;
				return kOk;
			}

			Err Renderer::AddFontDir(const char* dir)
			{
				if (dir == nullptr) return kInvalidArgument;
				impl_->font_dirs.emplace_back(dir);
				impl_->cand_cache.clear();
				impl_->cover_cache.clear();
				impl_->tofu_ready = false;
				impl_->has_emoji = false;
				impl_->FindEmojiFace();
				return kOk;
			}

			Err Renderer::Layout(const CLV_SubtitleBlockC& block, const CLV_SubtitleLineC* lines, uint32_t line_count,
								 const CLV_SubtitleStyleC* style, int64_t time_ticks)
			{
				if (lines == nullptr && line_count != 0u) return kInvalidArgument;
				if (block.reason_mask != 0u) return kOk;
				if (time_ticks < block.start_ticks || time_ticks >= block.end_ticks) return kOk;

				std::vector<Line> view;
				view.reserve(line_count);
				for (uint32_t i = 0; i < line_count; ++i)
				{
					Line l;
					if (lines[i].text != nullptr && lines[i].text_size != 0u)
						l.text.assign(reinterpret_cast<const char*>(lines[i].text), lines[i].text_size);
					l.kind.assign(l.text.size(), 0u);
					for (uint32_t s = 0; s < lines[i].span_count; ++s)
					{
						if (lines[i].spans == nullptr) break;
						const CLV_SubtitleSpanC& sp = lines[i].spans[s];
						for (uint32_t k = 0; k < sp.len; ++k)
						{
							const size_t at = static_cast<size_t>(sp.off) + k;
							if (at < l.kind.size()) l.kind[at] = static_cast<uint8_t>(sp.kind);
						}
					}
					view.push_back(std::move(l));
				}

				RStyle st = impl_->ResolveStyle(block, style);
				Placed p;
				if (! impl_->BuildPlaced(block, view, time_ticks, st, &p)) return kOk;
				impl_->placed[block.id] = std::move(p);
				return kOk;
			}

			Err Renderer::Render(uint8_t* rgba, size_t stride, uint32_t width, uint32_t height)
			{
				if (rgba == nullptr) return kInvalidArgument;
				const uint32_t fb_w = std::min(width, impl_->width);
				const uint32_t fb_h = std::min(height, impl_->height);
				if (stride < static_cast<size_t>(fb_w) * 4u) return kOutputTooSmall;
				impl_->frame_rects.clear();
				for (auto& kv: impl_->placed)
					impl_->Draw(kv.second, kv.second.time_ticks, rgba, stride, fb_w, fb_h, &impl_->frame_rects);
				MergeRects(&impl_->frame_rects);
				impl_->placed.clear();
				return kOk;
			}

			Err Renderer::DirtyRects(CLV_SubtitleRectC* rects, uint32_t capacity, uint32_t* out_total) const
			{
				if (out_total == nullptr) return kInvalidArgument;
				*out_total = static_cast<uint32_t>(impl_->frame_rects.size());
				if (rects == nullptr && capacity != 0u) return kInvalidArgument;
				if (capacity == 0u) return kOk; /* 第一趟：只取总数 */
				const uint32_t n = std::min<uint32_t>(capacity, *out_total);
				for (uint32_t i = 0; i < n; ++i) rects[i] = impl_->frame_rects[i];
				return *out_total > capacity ? kOutputTooSmall : kOk;
			}

			Err Renderer::Report(CLV_SubtitleRenderStatsC* out) const
			{
				if (out == nullptr) return kInvalidArgument;
				const uint32_t size = out->struct_size;
				*out = impl_->stats;
				out->struct_size = size == 0u ? sizeof(CLV_SubtitleRenderStatsC) : size;
				return kOk;
			}
		}	 // namespace render
	}	 // namespace subtitle
}	 // namespace clv
