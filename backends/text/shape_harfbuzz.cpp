/* 整形与文本度量后端：HarfBuzz。
 * 两个度量入口共用一份参数：OT = hb-ot-font，FT = hb-ft（度量经 FreeType）。 */
#include "subtitle/cal/cal.h"

#include <ft2build.h>
#include <hb-ft.h>
#include <hb-ot.h>
#include <hb.h>
#include FT_FREETYPE_H

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
				struct HbBundle
				{
					hb_blob_t* blob = nullptr;
					hb_face_t* face = nullptr;
					hb_font_t* font = nullptr;

					~HbBundle()
					{
						if (font != nullptr) hb_font_destroy(font);
						if (face != nullptr) hb_face_destroy(face);
						if (blob != nullptr) hb_blob_destroy(blob);
					}

					HbBundle(const HbBundle&) = delete;
					HbBundle& operator=(const HbBundle&) = delete;
					HbBundle() = default;

					bool Load(const char* font_path, uint32_t face_index, uint32_t pixel_size)
					{
						blob = hb_blob_create_from_file(font_path);
						if (blob == nullptr || hb_blob_get_length(blob) == 0u) return false;
						face = hb_face_create(blob, face_index);
						if (face == nullptr) return false;
						font = hb_font_create(face);
						if (font == nullptr) return false;
						hb_ot_font_set_funcs(font);
						hb_font_set_scale(font, static_cast<int>(pixel_size) * 64, static_cast<int>(pixel_size) * 64);
						return true;
					}
				};

				class HbMetrics: public Metrics
				{
					public:

					HbMetrics(int kind, uint32_t pixel_size): kind_(kind), px_(pixel_size) {}

					~HbMetrics() override
					{
						if (font_ != nullptr)
						{
							hb_font_destroy(font_);
							if (kind_ == kMetricFt) ft_face_ = nullptr; /* 引用归 hb-ft 的销毁回调 */
						}
						if (face_ != nullptr) hb_face_destroy(face_);
						if (blob_ != nullptr) hb_blob_destroy(blob_);
						if (ft_face_ != nullptr) FT_Done_Face(ft_face_);
						if (ft_lib_ != nullptr) FT_Done_FreeType(ft_lib_);
					}

					bool LoadOt(const char* font_path, uint32_t face_index)
					{
						blob_ = hb_blob_create_from_file(font_path);
						if (blob_ == nullptr || hb_blob_get_length(blob_) == 0u) return false;
						face_ = hb_face_create(blob_, face_index);
						if (face_ == nullptr) return false;
						font_ = hb_font_create(face_);
						if (font_ == nullptr) return false;
						hb_ot_font_set_funcs(font_);
						hb_font_set_scale(font_, static_cast<int>(px_) * 64, static_cast<int>(px_) * 64);
						return true;
					}

					bool LoadFt(const char* font_path, uint32_t face_index)
					{
						if (FT_Init_FreeType(&ft_lib_) != 0) return false;
						if (FT_New_Face(ft_lib_, font_path, static_cast<long>(face_index), &ft_face_) != 0)
							return false;
						if (FT_Set_Pixel_Sizes(ft_face_, 0, static_cast<FT_UInt>(px_)) != 0) return false;
						font_ = hb_ft_font_create_referenced(ft_face_);
						if (font_ == nullptr) return false;
						hb_ft_font_set_load_flags(font_, FT_LOAD_DEFAULT);
						hb_font_set_scale(font_, static_cast<int>(px_) * 64, static_cast<int>(px_) * 64);
						return true;
					}

					int Kind() const override { return kind_; }

					Err Advance(uint32_t codepoint, int32_t* advance) override
					{
						if (advance == nullptr) return kInvalidArgument;
						hb_codepoint_t gid = 0;
						if (! hb_font_get_nominal_glyph(font_, codepoint, &gid))
						{
							*advance = 0;
							return kOk;
						}
						*advance = static_cast<int32_t>(hb_font_get_glyph_h_advance(font_, gid));
						return kOk;
					}

					Err Extents(int32_t* ascender, int32_t* descender, int32_t* line_gap) override
					{
						if (ascender == nullptr || descender == nullptr || line_gap == nullptr) return kInvalidArgument;
						hb_font_extents_t ext;
						std::memset(&ext, 0, sizeof(ext));
						if (! hb_font_get_h_extents(font_, &ext)) return kBackendFailure;
						*ascender = static_cast<int32_t>(ext.ascender);
						*descender = static_cast<int32_t>(ext.descender);
						*line_gap = static_cast<int32_t>(ext.line_gap);
						return kOk;
					}

					private:

					int kind_;
					uint32_t px_;
					hb_blob_t* blob_ = nullptr;
					hb_face_t* face_ = nullptr;
					hb_font_t* font_ = nullptr;
					FT_Library ft_lib_ = nullptr;
					FT_Face ft_face_ = nullptr;
				};

				class HarfBuzzShaper: public ITextShaper
				{
					public:

					Err Shape(const char* font_path, uint32_t face_index, const uint8_t* utf8, size_t len,
							  uint32_t pixel_size, uint32_t* glyphs, int32_t* advances, size_t cap, size_t* glyph_count,
							  int32_t* rtl) override
					{
						if (font_path == nullptr || glyph_count == nullptr || rtl == nullptr ||
							(utf8 == nullptr && len != 0u) || (cap != 0u && (glyphs == nullptr || advances == nullptr)))
						{
							return kInvalidArgument;
						}

						HbBundle bundle;
						if (! bundle.Load(font_path, face_index, pixel_size)) return kBackendFailure;
						hb_buffer_t* buf = hb_buffer_create();
						if (buf == nullptr) return kOutOfMemory;
						hb_buffer_add_utf8(buf, reinterpret_cast<const char*>(utf8), static_cast<int>(len), 0,
										   static_cast<int>(len));
						hb_buffer_guess_segment_properties(buf);
						hb_shape(bundle.font, buf, nullptr, 0);

						unsigned count = 0;
						const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &count);
						const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, nullptr);
						*glyph_count = count;
						*rtl = hb_buffer_get_direction(buf) == HB_DIRECTION_RTL ? 1 : 0;
						const size_t fill = count < cap ? count : cap;
						for (size_t i = 0; i < fill; ++i)
						{
							glyphs[i] = info[i].codepoint;
							advances[i] = pos != nullptr ? pos[i].x_advance : 0;
						}
						hb_buffer_destroy(buf);
						return count > cap ? kOutputTooSmall : kOk;
					}

					Metrics* OpenMetrics(const char* font_path, uint32_t face_index, uint32_t pixel_size,
										 int kind) override
					{
						if (font_path == nullptr || pixel_size == 0u) return nullptr;
						if (kind != kMetricOt && kind != kMetricFt) return nullptr;
						HbMetrics* m = new (std::nothrow) HbMetrics(kind, pixel_size);
						if (m == nullptr) return nullptr;
						const bool ok =
							kind == kMetricOt ? m->LoadOt(font_path, face_index) : m->LoadFt(font_path, face_index);
						if (! ok)
						{
							delete m;
							return nullptr;
						}
						return m;
					}
				};
			}	 // namespace

			ITextShaper* CreateHarfBuzzShaper() { return new (std::nothrow) HarfBuzzShaper(); }
		}	 // namespace cal
	}	 // namespace subtitle
}	 // namespace clv
