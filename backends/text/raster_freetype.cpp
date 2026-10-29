/* 栅格后端：FreeType 的 A8 / 彩色位图 / 描边环。 */
#include "subtitle/cal/cal.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_STROKER_H

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace clv
{
	namespace subtitle
	{
		namespace cal
		{
			namespace
			{
				bool LoadFtFace(const char* font_path, uint32_t face_index, uint32_t pixel_size, FT_Library* lib_out,
								FT_Face* face_out)
				{
					FT_Library lib = nullptr;
					FT_Face face = nullptr;
					if (FT_Init_FreeType(&lib) != 0) return false;
					if (FT_New_Face(lib, font_path, static_cast<long>(face_index), &face) != 0)
					{
						FT_Done_FreeType(lib);
						return false;
					}
					if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size)) != 0)
					{
						FT_Done_Face(face);
						FT_Done_FreeType(lib);
						return false;
					}
					*lib_out = lib;
					*face_out = face;
					return true;
				}

				/* 按 pitch 拷成紧密排布；pitch 为负（自底向上）时按行号反算源行 */
				void CopyBitmapRows(const FT_Bitmap& bm, uint32_t bytes_per_pixel, std::vector<uint8_t>* out)
				{
					const size_t row = static_cast<size_t>(bm.width) * bytes_per_pixel;
					out->assign(static_cast<size_t>(bm.width) * static_cast<size_t>(bm.rows) * bytes_per_pixel, 0u);
					if (bm.buffer == nullptr) return;
					const long pitch = bm.pitch;
					for (uint32_t y = 0; y < bm.rows; ++y)
					{
						const long src_y = pitch >= 0 ? static_cast<long>(y) : static_cast<long>(bm.rows - 1u - y);
						const uint8_t* src =
							bm.buffer + static_cast<size_t>(src_y) * static_cast<size_t>(pitch >= 0 ? pitch : -pitch);
						std::memcpy(out->data() + static_cast<size_t>(y) * row, src, row);
					}
				}

				struct GlyphMask
				{
					std::vector<uint8_t> px;
					uint32_t w = 0;
					uint32_t h = 0;
					int32_t left = 0;
					int32_t top = 0;
				};

				/* 轮廓 → A8；stroke_px > 0 时先做外侧扩边（StrokeBorder 的第三个参数 0 = 只要外环）。 */
				bool RenderGlyphA8(FT_Face face, FT_UInt glyph_id, uint32_t stroke_px, GlyphMask* out)
				{
					if (FT_Load_Glyph(face, glyph_id, FT_LOAD_NO_BITMAP) != 0) return false;
					FT_Glyph glyph = nullptr;
					if (FT_Get_Glyph(face->glyph, &glyph) != 0) return false;
					if (stroke_px > 0u)
					{
						FT_Stroker stroker = nullptr;
						if (FT_Stroker_New(face->glyph->library, &stroker) != 0)
						{
							FT_Done_Glyph(glyph);
							return false;
						}
						FT_Stroker_Set(stroker, static_cast<FT_Fixed>(stroke_px) * 64, FT_STROKER_LINECAP_ROUND,
									   FT_STROKER_LINEJOIN_ROUND, 0);
						const FT_Error e = FT_Glyph_StrokeBorder(&glyph, stroker, 0, 0);
						FT_Stroker_Done(stroker);
						if (e != 0)
						{
							FT_Done_Glyph(glyph);
							return false;
						}
					}
					if (FT_Glyph_To_Bitmap(&glyph, FT_RENDER_MODE_NORMAL, nullptr, 1) != 0)
					{
						FT_Done_Glyph(glyph);
						return false;
					}
					const FT_BitmapGlyph bg = reinterpret_cast<FT_BitmapGlyph>(glyph);
					out->w = bg->bitmap.width;
					out->h = bg->bitmap.rows;
					out->left = bg->left;
					out->top = bg->top;
					CopyBitmapRows(bg->bitmap, 1u, &out->px);
					FT_Done_Glyph(glyph);
					return true;
				}

				/* 彩色位图缩放：预乘累加后除以 alpha；直 alpha 直接平均会把透明像素的黑渗进边缘。 */
				void ScaleRgbaStraight(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh, uint32_t dw,
									   uint32_t dh, std::vector<uint8_t>* dst)
				{
					dst->assign(static_cast<size_t>(dw) * dh * 4u, 0u);
					if (sw == 0u || sh == 0u || dw == 0u || dh == 0u) return;
					for (uint32_t dy = 0; dy < dh; ++dy)
					{
						const uint32_t sy0 = static_cast<uint32_t>(static_cast<uint64_t>(dy) * sh / dh);
						uint32_t sy1 = static_cast<uint32_t>(static_cast<uint64_t>(dy + 1u) * sh / dh);
						if (sy1 <= sy0) sy1 = sy0 + 1u;
						for (uint32_t dx = 0; dx < dw; ++dx)
						{
							const uint32_t sx0 = static_cast<uint32_t>(static_cast<uint64_t>(dx) * sw / dw);
							uint32_t sx1 = static_cast<uint32_t>(static_cast<uint64_t>(dx + 1u) * sw / dw);
							if (sx1 <= sx0) sx1 = sx0 + 1u;
							uint64_t sum_a = 0;
							uint64_t sum_r = 0;
							uint64_t sum_g = 0;
							uint64_t sum_b = 0;
							uint64_t n = 0;
							for (uint32_t sy = sy0; sy < sy1 && sy < sh; ++sy)
							{
								for (uint32_t sx = sx0; sx < sx1 && sx < sw; ++sx)
								{
									const uint8_t* p = &src[(static_cast<size_t>(sy) * sw + sx) * 4u];
									sum_a += p[3];
									sum_r += static_cast<uint64_t>(p[0]) * p[3];
									sum_g += static_cast<uint64_t>(p[1]) * p[3];
									sum_b += static_cast<uint64_t>(p[2]) * p[3];
									++n;
								}
							}
							uint8_t* o = &(*dst)[(static_cast<size_t>(dy) * dw + dx) * 4u];
							if (n == 0u || sum_a == 0u) continue;
							o[3] = static_cast<uint8_t>(sum_a / n);
							o[0] = static_cast<uint8_t>(sum_r / sum_a);
							o[1] = static_cast<uint8_t>(sum_g / sum_a);
							o[2] = static_cast<uint8_t>(sum_b / sum_a);
						}
					}
				}

				uint8_t* DupBitmap(const uint8_t* src, size_t n)
				{
					uint8_t* px = static_cast<uint8_t*>(std::malloc(n == 0u ? 1u : n));
					if (px == nullptr) return nullptr;
					if (n != 0u && src != nullptr) std::memcpy(px, src, n);
					return px;
				}

				class FreeTypeRasterizer: public IGlyphRasterizer
				{
					public:

					Err Raster(const char* font_path, uint32_t face_index, uint32_t pixel_size, uint32_t codepoint,
							   uint8_t** bitmap, uint32_t* width, uint32_t* height, int32_t* left,
							   int32_t* top) override
					{
						if (font_path == nullptr || bitmap == nullptr || width == nullptr || height == nullptr ||
							left == nullptr || top == nullptr)
						{
							return kInvalidArgument;
						}
						FT_Library lib = nullptr;
						FT_Face face = nullptr;
						if (! LoadFtFace(font_path, face_index, pixel_size, &lib, &face)) return kBackendFailure;
						if (FT_Load_Char(face, static_cast<FT_ULong>(codepoint), FT_LOAD_RENDER) != 0)
						{
							FT_Done_Face(face);
							FT_Done_FreeType(lib);
							return kBackendFailure;
						}
						const FT_Bitmap* bm = &face->glyph->bitmap;
						uint8_t* px = DupBitmap(bm->buffer, static_cast<size_t>(bm->width) * bm->rows);
						if (px == nullptr)
						{
							FT_Done_Face(face);
							FT_Done_FreeType(lib);
							return kOutOfMemory;
						}
						*bitmap = px;
						*width = bm->width;
						*height = bm->rows;
						*left = face->glyph->bitmap_left;
						*top = face->glyph->bitmap_top;
						FT_Done_Face(face);
						FT_Done_FreeType(lib);
						return kOk;
					}

					Err RasterGlyph(const char* font_path, uint32_t face_index, uint32_t pixel_size, uint32_t glyph_id,
									uint8_t** bitmap, uint32_t* width, uint32_t* height, int32_t* left,
									int32_t* top) override
					{
						if (font_path == nullptr || bitmap == nullptr || width == nullptr || height == nullptr ||
							left == nullptr || top == nullptr)
						{
							return kInvalidArgument;
						}
						FT_Library lib = nullptr;
						FT_Face face = nullptr;
						if (! LoadFtFace(font_path, face_index, pixel_size, &lib, &face)) return kBackendFailure;
						if (FT_Load_Glyph(face, static_cast<FT_UInt>(glyph_id), FT_LOAD_RENDER) != 0)
						{
							FT_Done_Face(face);
							FT_Done_FreeType(lib);
							return kBackendFailure;
						}
						const FT_Bitmap* bm = &face->glyph->bitmap;
						uint8_t* px = DupBitmap(bm->buffer, static_cast<size_t>(bm->width) * bm->rows);
						if (px == nullptr)
						{
							FT_Done_Face(face);
							FT_Done_FreeType(lib);
							return kOutOfMemory;
						}
						*bitmap = px;
						*width = bm->width;
						*height = bm->rows;
						*left = face->glyph->bitmap_left;
						*top = face->glyph->bitmap_top;
						FT_Done_Face(face);
						FT_Done_FreeType(lib);
						return kOk;
					}

					Err RasterGlyphFmt(const char* font_path, uint32_t face_index, uint32_t pixel_size,
									   uint32_t glyph_id, uint8_t** bitmap, uint32_t* width, uint32_t* height,
									   int32_t* left, int32_t* top, int32_t* format) override
					{
						if (font_path == nullptr || bitmap == nullptr || width == nullptr || height == nullptr ||
							left == nullptr || top == nullptr || format == nullptr)
						{
							return kInvalidArgument;
						}
						FT_Library lib = nullptr;
						FT_Face face = nullptr;
						if (FT_Init_FreeType(&lib) != 0) return kBackendFailure;
						if (FT_New_Face(lib, font_path, static_cast<long>(face_index), &face) != 0)
						{
							FT_Done_FreeType(lib);
							return kBackendFailure;
						}
						/* 彩色 emoji 面只带原生 strike：先取原生档，出图后再缩；scalable 面按请求字号设 */
						const bool scalable = FT_IS_SCALABLE(face) != 0;
						if (scalable)
						{
							if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size)) != 0)
							{
								FT_Done_Face(face);
								FT_Done_FreeType(lib);
								return kBackendFailure;
							}
						}
						else if (FT_Select_Size(face, 0) != 0)
						{
							FT_Done_Face(face);
							FT_Done_FreeType(lib);
							return kBackendFailure;
						}
						if (FT_Load_Glyph(face, static_cast<FT_UInt>(glyph_id), FT_LOAD_COLOR | FT_LOAD_RENDER) != 0)
						{
							FT_Done_Face(face);
							FT_Done_FreeType(lib);
							return kBackendFailure;
						}
						const FT_Bitmap& bm = face->glyph->bitmap;
						std::vector<uint8_t> out;
						uint32_t out_w = bm.width;
						uint32_t out_h = bm.rows;
						int32_t out_left = face->glyph->bitmap_left;
						int32_t out_top = face->glyph->bitmap_top;
						int32_t out_format = kGlyphA8;

						if (bm.pixel_mode == FT_PIXEL_MODE_BGRA)
						{
							/* FT 交回预乘 BGRA：反预乘并转 RGB 序（straight alpha） */
							std::vector<uint8_t> bgra;
							CopyBitmapRows(bm, 4u, &bgra);
							out.assign(static_cast<size_t>(bm.width) * bm.rows * 4u, 0u);
							for (size_t i = 0; i < static_cast<size_t>(bm.width) * bm.rows; ++i)
							{
								const uint8_t b = bgra[i * 4u + 0u];
								const uint8_t g = bgra[i * 4u + 1u];
								const uint8_t r = bgra[i * 4u + 2u];
								const uint8_t a = bgra[i * 4u + 3u];
								uint8_t* o = &out[i * 4u];
								o[3] = a;
								if (a != 0u)
								{
									o[0] = static_cast<uint8_t>(
										std::min<uint32_t>(255u, (static_cast<uint32_t>(r) * 255u + a / 2u) / a));
									o[1] = static_cast<uint8_t>(
										std::min<uint32_t>(255u, (static_cast<uint32_t>(g) * 255u + a / 2u) / a));
									o[2] = static_cast<uint8_t>(
										std::min<uint32_t>(255u, (static_cast<uint32_t>(b) * 255u + a / 2u) / a));
								}
							}
							out_format = kGlyphRgba8;
						}
						else if (bm.pixel_mode == FT_PIXEL_MODE_GRAY)
						{
							CopyBitmapRows(bm, 1u, &out);
						}
						else
						{
							FT_Done_Face(face);
							FT_Done_FreeType(lib);
							return kBackendFailure;
						}

						if (! scalable)
						{
							/* 原生档的 ppem 从 strike 表取：available_sizes[].y_ppem 是 26.6 定点，
							 * size->metrics.y_ppem 在纯位图面上并不总是 ppem。 */
							uint32_t ppem = 0;
							if (face->num_fixed_sizes > 0)
								ppem = static_cast<uint32_t>(face->available_sizes[0].y_ppem) >> 6;
							if (ppem == 0u) ppem = static_cast<uint32_t>(face->size->metrics.y_ppem);
							if (ppem != 0u && ppem != pixel_size)
							{
								const uint32_t dw = static_cast<uint32_t>(std::max<int64_t>(
									1, (static_cast<int64_t>(out_w) * pixel_size + ppem / 2u) / ppem));
								const uint32_t dh = static_cast<uint32_t>(std::max<int64_t>(
									1, (static_cast<int64_t>(out_h) * pixel_size + ppem / 2u) / ppem));
								std::vector<uint8_t> scaled;
								if (out_format == kGlyphRgba8)
								{
									ScaleRgbaStraight(out, out_w, out_h, dw, dh, &scaled);
								}
								else
								{
									scaled.assign(static_cast<size_t>(dw) * dh, 0u);
									for (uint32_t y = 0; y < dh; ++y)
									{
										for (uint32_t x = 0; x < dw; ++x)
										{
											scaled[static_cast<size_t>(y) * dw + x] =
												out[static_cast<size_t>(y) * out_h / dh * out_w + x * out_w / dw];
										}
									}
								}
								out.swap(scaled);
								out_left = static_cast<int32_t>((static_cast<int64_t>(out_left) * pixel_size) / ppem);
								out_top = static_cast<int32_t>((static_cast<int64_t>(out_top) * pixel_size) / ppem);
								out_w = dw;
								out_h = dh;
							}
						}

						const size_t bytes = static_cast<size_t>(out_w) * out_h * (out_format == kGlyphRgba8 ? 4u : 1u);
						uint8_t* px = DupBitmap(out.empty() ? nullptr : out.data(), bytes);
						if (px == nullptr)
						{
							FT_Done_Face(face);
							FT_Done_FreeType(lib);
							return kOutOfMemory;
						}
						*bitmap = px;
						*width = out_w;
						*height = out_h;
						*left = out_left;
						*top = out_top;
						*format = out_format;
						FT_Done_Face(face);
						FT_Done_FreeType(lib);
						return kOk;
					}

					Err RasterGlyphStroked(const char* font_path, uint32_t face_index, uint32_t pixel_size,
										   uint32_t glyph_id, uint32_t stroke_width, uint8_t** fill, uint8_t** outline,
										   uint32_t* width, uint32_t* height, int32_t* left, int32_t* top) override
					{
						if (font_path == nullptr || fill == nullptr || outline == nullptr || width == nullptr ||
							height == nullptr || left == nullptr || top == nullptr)
						{
							return kInvalidArgument;
						}
						FT_Library lib = nullptr;
						FT_Face face = nullptr;
						if (! LoadFtFace(font_path, face_index, pixel_size, &lib, &face)) return kBackendFailure;

						GlyphMask fill_mask;
						GlyphMask outline_mask;
						bool ok = RenderGlyphA8(face, static_cast<FT_UInt>(glyph_id), 0u, &fill_mask);
						if (ok && stroke_width > 0u)
							ok = RenderGlyphA8(face, static_cast<FT_UInt>(glyph_id), stroke_width, &outline_mask);
						FT_Done_Face(face);
						FT_Done_FreeType(lib);
						if (! ok) return kBackendFailure;

						const bool has_outline = outline_mask.w != 0u && outline_mask.h != 0u;
						int32_t x0 = fill_mask.left;
						int32_t x1 = fill_mask.left + static_cast<int32_t>(fill_mask.w);
						int32_t y1 = fill_mask.top;
						int32_t y0 = fill_mask.top - static_cast<int32_t>(fill_mask.h);
						if (has_outline)
						{
							x0 = std::min(x0, outline_mask.left);
							x1 = std::max(x1, outline_mask.left + static_cast<int32_t>(outline_mask.w));
							y1 = std::max(y1, outline_mask.top);
							y0 = std::min(y0, outline_mask.top - static_cast<int32_t>(outline_mask.h));
						}
						const uint32_t w = static_cast<uint32_t>(std::max(1, x1 - x0));
						const uint32_t h = static_cast<uint32_t>(std::max(1, y1 - y0));

						uint8_t* fbuf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(w) * h));
						uint8_t* obuf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(w) * h));
						if (fbuf == nullptr || obuf == nullptr)
						{
							std::free(fbuf);
							std::free(obuf);
							return kOutOfMemory;
						}
						std::memset(fbuf, 0, static_cast<size_t>(w) * h);
						std::memset(obuf, 0, static_cast<size_t>(w) * h);

						auto blit = [&](const GlyphMask& mask, uint8_t* dst)
						{
							if (mask.w == 0u || mask.h == 0u) return;
							const int32_t mask_y0 = mask.top - static_cast<int32_t>(mask.h);
							for (uint32_t yy = 0; yy < h; ++yy)
							{
								const int32_t sy = y0 + static_cast<int32_t>(yy) - mask_y0;
								if (sy < 0 || sy >= static_cast<int32_t>(mask.h)) continue;
								for (uint32_t xx = 0; xx < w; ++xx)
								{
									const int32_t sx = x0 + static_cast<int32_t>(xx) - mask.left;
									if (sx < 0 || sx >= static_cast<int32_t>(mask.w)) continue;
									const uint8_t v =
										mask.px[static_cast<size_t>(sy) * mask.w + static_cast<size_t>(sx)];
									if (v > dst[static_cast<size_t>(yy) * w + xx])
										dst[static_cast<size_t>(yy) * w + xx] = v;
								}
							}
						};
						blit(fill_mask, fbuf);
						if (has_outline)
						{
							blit(outline_mask, obuf);
							/* StrokeBorder 交回外扩后的整块轮廓（覆盖字内），减掉字本体才只剩外圈 */
							for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
								obuf[i] = obuf[i] > fbuf[i] ? static_cast<uint8_t>(obuf[i] - fbuf[i]) : 0u;
						}

						*fill = fbuf;
						*outline = obuf;
						*width = w;
						*height = h;
						*left = x0;
						*top = y1;
						return kOk;
					}
				};
			}	 // namespace

			IGlyphRasterizer* CreateFreeTypeRasterizer() { return new (std::nothrow) FreeTypeRasterizer(); }
		}	 // namespace cal
	}	 // namespace subtitle
}	 // namespace clv
