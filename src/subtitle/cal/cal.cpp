/* CAL 门面：后端选择与降级分发。
 * 降级面不链第三方：标量 UTF-8、NFC 直通、LTR、码位当字形、预烘焙度量与 2×2 位图。 */
#include "cal.h"

#include "subtitle/core.h"

#include <cstdlib>
#include <cstring>
#include <new>

namespace clv
{
	namespace subtitle
	{
		namespace cal
		{
			namespace
			{
				class ScalarValidator: public IUtf8Validator
				{
					public:

					bool Valid(const uint8_t* data, size_t size) override { return Utf8Valid(data, size); }
				};

				class PassNormalizer: public INormalizer
				{
					public:

					Err Nfc(const uint8_t* in, size_t in_len, uint8_t* out, size_t cap, size_t* out_len) override
					{
						if (out_len == nullptr || (in == nullptr && in_len != 0u) || (out == nullptr && cap != 0u))
							return kInvalidArgument;
						*out_len = in_len;
						if (in_len > cap) return kOutputTooSmall;
						if (in_len != 0u) std::memcpy(out, in, in_len);
						return kOk;
					}
				};

				class LtrBidi: public IBidiEngine
				{
					public:

					Err Paragraph(const uint8_t* utf8, size_t len, int32_t* base_level, int32_t* run_count) override
					{
						if (base_level == nullptr || run_count == nullptr || (utf8 == nullptr && len != 0u))
							return kInvalidArgument;
						*base_level = 0;
						*run_count = len == 0u ? 0 : 1;
						return kOk;
					}

					Err Runs(const uint8_t* utf8, size_t len, BidiRun* runs, size_t cap, size_t* run_count) override
					{
						if (run_count == nullptr || (utf8 == nullptr && len != 0u) || (runs == nullptr && cap != 0u))
							return kInvalidArgument;
						*run_count = len == 0u ? 0u : 1u;
						if (len != 0u && cap >= 1u)
						{
							runs[0].off = 0;
							runs[0].len = static_cast<uint32_t>(len);
							runs[0].level = 0;
						}
						return kOk;
					}
				};

				class DegradedMetrics: public Metrics
				{
					public:

					DegradedMetrics(uint32_t pixel_size, int kind): px_(pixel_size), kind_(kind) {}

					int Kind() const override { return kind_; }

					Err Advance(uint32_t, int32_t* advance) override
					{
						if (advance == nullptr) return kInvalidArgument;
						*advance = static_cast<int32_t>(px_);
						return kOk;
					}

					Err Extents(int32_t* ascender, int32_t* descender, int32_t* line_gap) override
					{
						if (ascender == nullptr || descender == nullptr || line_gap == nullptr) return kInvalidArgument;
						*ascender = static_cast<int32_t>(px_);
						*descender = -static_cast<int32_t>(px_ / 4u);
						*line_gap = 0;
						return kOk;
					}

					private:

					uint32_t px_;
					int kind_;
				};

				class IdentityShaper: public ITextShaper
				{
					public:

					Err Shape(const char*, uint32_t, const uint8_t* utf8, size_t len, uint32_t pixel_size,
							  uint32_t* glyphs, int32_t* advances, size_t cap, size_t* glyph_count,
							  int32_t* rtl) override
					{
						if (glyph_count == nullptr || rtl == nullptr || (utf8 == nullptr && len != 0u) ||
							(cap != 0u && (glyphs == nullptr || advances == nullptr)))
						{
							return kInvalidArgument;
						}
						size_t i = 0;
						size_t n = 0;
						while (i < len)
						{
							const uint8_t b0 = utf8[i];
							const size_t step = b0 < 0x80u ? 1u : (b0 < 0xE0u ? 2u : (b0 < 0xF0u ? 3u : 4u));
							uint32_t cp = b0;
							if (step == 1u)
							{
								cp = b0;
							}
							else if (step == 2u)
							{
								cp = (static_cast<uint32_t>(b0 & 0x1Fu) << 6) | (utf8[i + 1] & 0x3Fu);
							}
							else if (step == 3u)
							{
								cp = (static_cast<uint32_t>(b0 & 0x0Fu) << 12) |
									 (static_cast<uint32_t>(utf8[i + 1] & 0x3Fu) << 6) | (utf8[i + 2] & 0x3Fu);
							}
							else
							{
								cp = (static_cast<uint32_t>(b0 & 0x07u) << 18) |
									 (static_cast<uint32_t>(utf8[i + 1] & 0x3Fu) << 12) |
									 (static_cast<uint32_t>(utf8[i + 2] & 0x3Fu) << 6) | (utf8[i + 3] & 0x3Fu);
							}
							if (n < cap)
							{
								glyphs[n] = cp;
								advances[n] = static_cast<int32_t>(pixel_size);
							}
							++n;
							i += step;
						}
						*glyph_count = n;
						*rtl = 0;
						return n > cap ? kOutputTooSmall : kOk;
					}

					Metrics* OpenMetrics(const char*, uint32_t, uint32_t pixel_size, int kind) override
					{
						return new (std::nothrow) DegradedMetrics(pixel_size, kind);
					}
				};

				class NoFace: public IFontFace
				{
					public:

					Err Covers(const char*, uint32_t, uint32_t, int32_t* covers) override
					{
						if (covers == nullptr) return kInvalidArgument;
						*covers = 0;
						return kOk;
					}

					Err EnumFileFaces(const char*, FaceInfo*, size_t, size_t*) override { return kUnsupported; }

					Err SelectFace(const char*, const char*, uint32_t, int32_t, FaceInfo*, char*, size_t,
								   size_t*) override
					{
						return kUnsupported;
					}
				};

				class NoResolver: public IFontResolver
				{
					public:

					Err ResolveSystem(const char*, char*, size_t, size_t*) override { return kUnsupported; }

					Err SelectSystem(const char*, uint32_t, int32_t, FaceInfo*, char*, size_t, size_t*) override
					{
						return kUnsupported;
					}
				};

				class BlockRaster: public IGlyphRasterizer
				{
					public:

					Err Raster(const char*, uint32_t, uint32_t, uint32_t, uint8_t** bitmap, uint32_t* width,
							   uint32_t* height, int32_t* left, int32_t* top) override
					{
						return Block(bitmap, width, height, left, top);
					}

					Err RasterGlyph(const char*, uint32_t, uint32_t, uint32_t, uint8_t** bitmap, uint32_t* width,
									uint32_t* height, int32_t* left, int32_t* top) override
					{
						return Block(bitmap, width, height, left, top);
					}

					Err RasterGlyphFmt(const char*, uint32_t, uint32_t, uint32_t, uint8_t** bitmap, uint32_t* width,
									   uint32_t* height, int32_t* left, int32_t* top, int32_t* format) override
					{
						if (format == nullptr) return kInvalidArgument;
						*format = kGlyphA8;
						return Block(bitmap, width, height, left, top);
					}

					Err RasterGlyphStroked(const char*, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t** fill,
										   uint8_t** outline, uint32_t* width, uint32_t* height, int32_t* left,
										   int32_t* top) override
					{
						if (fill == nullptr || outline == nullptr || width == nullptr || height == nullptr ||
							left == nullptr || top == nullptr)
						{
							return kInvalidArgument;
						}
						uint8_t* f = static_cast<uint8_t*>(std::malloc(4u));
						uint8_t* o = static_cast<uint8_t*>(std::malloc(4u));
						if (f == nullptr || o == nullptr)
						{
							std::free(f);
							std::free(o);
							return kOutOfMemory;
						}
						std::memset(f, 0xFF, 4u);
						std::memset(o, 0x00, 4u);
						*fill = f;
						*outline = o;
						*width = 2u;
						*height = 2u;
						*left = 0;
						*top = 0;
						return kOk;
					}

					private:

					static Err Block(uint8_t** bitmap, uint32_t* width, uint32_t* height, int32_t* left, int32_t* top)
					{
						if (bitmap == nullptr || width == nullptr || height == nullptr || left == nullptr ||
							top == nullptr)
						{
							return kInvalidArgument;
						}
						uint8_t* px = static_cast<uint8_t*>(std::malloc(4u));
						if (px == nullptr) return kOutOfMemory;
						std::memset(px, 0xFF, 4u);
						*bitmap = px;
						*width = 2u;
						*height = 2u;
						*left = 0;
						*top = 0;
						return kOk;
					}
				};
			}	 // namespace

			Cal* Cal::Create(bool degraded)
			{
				std::unique_ptr<Cal> cal(new (std::nothrow) Cal());
				if (! cal) return nullptr;
				if (! degraded)
				{
					cal->utf8_.reset(CreateSimdutfValidator());
					cal->normalize_.reset(CreateUtf8procNormalizer());
					cal->bidi_.reset(CreateSheenBidiEngine());
					cal->shaper_.reset(CreateHarfBuzzShaper());
					cal->face_.reset(CreateFreeTypeFace());
					cal->resolver_.reset(CreatePlatformFontResolver());
					cal->raster_.reset(CreateFreeTypeRasterizer());
				}
				const bool real =
					cal->utf8_ && cal->normalize_ && cal->bidi_ && cal->shaper_ && cal->face_ && cal->raster_;
				cal->degraded_ = ! real;
				if (! cal->utf8_) cal->utf8_.reset(new (std::nothrow) ScalarValidator());
				if (! cal->normalize_) cal->normalize_.reset(new (std::nothrow) PassNormalizer());
				if (! cal->bidi_) cal->bidi_.reset(new (std::nothrow) LtrBidi());
				if (! cal->shaper_) cal->shaper_.reset(new (std::nothrow) IdentityShaper());
				if (! cal->face_) cal->face_.reset(new (std::nothrow) NoFace());
				if (! cal->resolver_) cal->resolver_.reset(new (std::nothrow) NoResolver());
				if (! cal->raster_) cal->raster_.reset(new (std::nothrow) BlockRaster());
				if (! cal->utf8_ || ! cal->normalize_ || ! cal->bidi_ || ! cal->shaper_ || ! cal->face_ ||
					! cal->resolver_ || ! cal->raster_)
				{
					return nullptr;
				}
				return cal.release();
			}

			Cal::~Cal() = default;

			bool Cal::Utf8Valid(const uint8_t* data, size_t size) { return utf8_->Valid(data, size); }

			Err Cal::Nfc(const uint8_t* in, size_t in_len, uint8_t* out, size_t cap, size_t* out_len)
			{
				return normalize_->Nfc(in, in_len, out, cap, out_len);
			}

			Err Cal::Bidi(const uint8_t* utf8, size_t len, int32_t* base_level, int32_t* run_count)
			{
				return bidi_->Paragraph(utf8, len, base_level, run_count);
			}

			Err Cal::BidiRuns(const uint8_t* utf8, size_t len, BidiRun* runs, size_t cap, size_t* run_count)
			{
				return bidi_->Runs(utf8, len, runs, cap, run_count);
			}

			Err Cal::Shape(const char* font_path, uint32_t face_index, const uint8_t* utf8, size_t len,
						   uint32_t pixel_size, uint32_t* glyphs, int32_t* advances, size_t cap, size_t* glyph_count,
						   int32_t* rtl)
			{
				return shaper_->Shape(font_path, face_index, utf8, len, pixel_size, glyphs, advances, cap, glyph_count,
									  rtl);
			}

			Metrics* Cal::OpenMetrics(const char* font_path, uint32_t face_index, uint32_t pixel_size, int kind)
			{
				return shaper_->OpenMetrics(font_path, face_index, pixel_size, kind);
			}

			Err Cal::FaceCovers(const char* font_path, uint32_t face_index, uint32_t codepoint, int32_t* covers)
			{
				return face_->Covers(font_path, face_index, codepoint, covers);
			}

			Err Cal::Raster(const char* font_path, uint32_t face_index, uint32_t pixel_size, uint32_t codepoint,
							uint8_t** bitmap, uint32_t* width, uint32_t* height, int32_t* left, int32_t* top)
			{
				return raster_->Raster(font_path, face_index, pixel_size, codepoint, bitmap, width, height, left, top);
			}

			Err Cal::RasterGlyph(const char* font_path, uint32_t face_index, uint32_t pixel_size, uint32_t glyph_id,
								 uint8_t** bitmap, uint32_t* width, uint32_t* height, int32_t* left, int32_t* top)
			{
				return raster_->RasterGlyph(font_path, face_index, pixel_size, glyph_id, bitmap, width, height, left,
											top);
			}

			Err Cal::RasterGlyphFmt(const char* font_path, uint32_t face_index, uint32_t pixel_size, uint32_t glyph_id,
									uint8_t** bitmap, uint32_t* width, uint32_t* height, int32_t* left, int32_t* top,
									int32_t* format)
			{
				return raster_->RasterGlyphFmt(font_path, face_index, pixel_size, glyph_id, bitmap, width, height, left,
											   top, format);
			}

			Err Cal::RasterGlyphStroked(const char* font_path, uint32_t face_index, uint32_t pixel_size,
										uint32_t glyph_id, uint32_t stroke_width, uint8_t** fill, uint8_t** outline,
										uint32_t* width, uint32_t* height, int32_t* left, int32_t* top)
			{
				return raster_->RasterGlyphStroked(font_path, face_index, pixel_size, glyph_id, stroke_width, fill,
												   outline, width, height, left, top);
			}

			Err Cal::ResolveSystemFont(const char* family, char* out_path, size_t cap, size_t* out_len)
			{
				return resolver_->ResolveSystem(family, out_path, cap, out_len);
			}

			Err Cal::EnumFileFaces(const char* font_path, FaceInfo* out, size_t cap, size_t* out_count)
			{
				return face_->EnumFileFaces(font_path, out, cap, out_count);
			}

			Err Cal::SelectFace(const char* dir, const char* family, uint32_t weight, int32_t italic,
								FaceInfo* out_face, char* out_path, size_t cap, size_t* out_len)
			{
				return face_->SelectFace(dir, family, weight, italic, out_face, out_path, cap, out_len);
			}

			Err Cal::SelectSystemFace(const char* family, uint32_t weight, int32_t italic, FaceInfo* out_face,
									  char* out_path, size_t cap, size_t* out_len)
			{
				return resolver_->SelectSystem(family, weight, italic, out_face, out_path, cap, out_len);
			}
		}	 // namespace cal
	}	 // namespace subtitle
}	 // namespace clv
