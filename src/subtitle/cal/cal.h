/* 字幕文本栈的 CAL：七个接口 + 后端选择 + 降级分发。
 *
 * 第三方适配在 backends/text/，系统字体解析在 platform/font/；本层只做接口、选择与降级，
 * 不直接包含任何第三方头。降级面是不依赖第三方的确定性实现：标量 UTF-8、NFC 直通、
 * LTR、码位当字形、预烘焙度量与位图，用于文本栈缺席时仍能跑通布局与合成。
 */
#ifndef CLV_SUBTITLE_CAL_H
#define CLV_SUBTITLE_CAL_H

#include "subtitle/status.h"

#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace clv
{
	namespace subtitle
	{
		namespace cal
		{
			struct BidiRun
			{
				uint32_t off; /* 字节偏移 */
				uint32_t len; /* 字节数 */
				int32_t level;
			};

			/* face 身份：family / subfamily 取 name 表的 ASCII 串，截到定长；weight 读 OS/2，
			 * italic 读 OS/2 fsSelection bit0（表不足时退回 head macStyle bit1）。 */
			struct FaceInfo
			{
				uint32_t face_index;
				uint32_t weight;
				int32_t italic;
				char family[64];
				char subfamily[64];
			};

			inline constexpr int kGlyphA8 = 0;
			inline constexpr int kGlyphRgba8 = 1;

			inline constexpr int kMetricOt = 0; /* hb-ot-font */
			inline constexpr int kMetricFt = 1; /* hb-ft */

			class Metrics
			{
				public:

				virtual ~Metrics() = default;
				virtual int Kind() const = 0;
				virtual Err Advance(uint32_t codepoint, int32_t* advance) = 0;
				virtual Err Extents(int32_t* ascender, int32_t* descender, int32_t* line_gap) = 0;
			};

			struct IUtf8Validator
			{
				virtual ~IUtf8Validator() = default;
				virtual bool Valid(const uint8_t* data, size_t size) = 0;
			};

			struct INormalizer
			{
				virtual ~INormalizer() = default;
				virtual Err Nfc(const uint8_t* in, size_t in_len, uint8_t* out, size_t cap, size_t* out_len) = 0;
			};

			struct IBidiEngine
			{
				virtual ~IBidiEngine() = default;
				virtual Err Paragraph(const uint8_t* utf8, size_t len, int32_t* base_level, int32_t* run_count) = 0;
				virtual Err Runs(const uint8_t* utf8, size_t len, BidiRun* runs, size_t cap, size_t* run_count) = 0;
			};

			struct ITextShaper
			{
				virtual ~ITextShaper() = default;
				virtual Err Shape(const char* font_path, uint32_t face_index, const uint8_t* utf8, size_t len,
								  uint32_t pixel_size, uint32_t* glyphs, int32_t* advances, size_t cap,
								  size_t* glyph_count, int32_t* rtl) = 0;
				virtual Metrics* OpenMetrics(const char* font_path, uint32_t face_index, uint32_t pixel_size,
											 int kind) = 0;
			};

			struct IFontFace
			{
				virtual ~IFontFace() = default;
				virtual Err Covers(const char* font_path, uint32_t face_index, uint32_t codepoint, int32_t* covers) = 0;
				virtual Err EnumFileFaces(const char* font_path, FaceInfo* out, size_t cap, size_t* out_count) = 0;
				virtual Err SelectFace(const char* dir, const char* family, uint32_t weight, int32_t italic,
									   FaceInfo* out_face, char* out_path, size_t cap, size_t* out_len) = 0;
			};

			struct IFontResolver
			{
				virtual ~IFontResolver() = default;
				virtual Err ResolveSystem(const char* family, char* out_path, size_t cap, size_t* out_len) = 0;
				virtual Err SelectSystem(const char* family, uint32_t weight, int32_t italic, FaceInfo* out_face,
										 char* out_path, size_t cap, size_t* out_len) = 0;
			};

			struct IGlyphRasterizer
			{
				virtual ~IGlyphRasterizer() = default;
				virtual Err Raster(const char* font_path, uint32_t face_index, uint32_t pixel_size, uint32_t codepoint,
								   uint8_t** bitmap, uint32_t* width, uint32_t* height, int32_t* left,
								   int32_t* top) = 0;
				virtual Err RasterGlyph(const char* font_path, uint32_t face_index, uint32_t pixel_size,
										uint32_t glyph_id, uint8_t** bitmap, uint32_t* width, uint32_t* height,
										int32_t* left, int32_t* top) = 0;
				virtual Err RasterGlyphFmt(const char* font_path, uint32_t face_index, uint32_t pixel_size,
										   uint32_t glyph_id, uint8_t** bitmap, uint32_t* width, uint32_t* height,
										   int32_t* left, int32_t* top, int32_t* format) = 0;
				/* fill / outline 都是 A8、同原点同尺寸；outline 只含字外那一圈，宽 0 时整块为零。 */
				virtual Err RasterGlyphStroked(const char* font_path, uint32_t face_index, uint32_t pixel_size,
											   uint32_t glyph_id, uint32_t stroke_width, uint8_t** fill,
											   uint8_t** outline, uint32_t* width, uint32_t* height, int32_t* left,
											   int32_t* top) = 0;
			};

			class Cal
			{
				public:

				static Cal* Create(bool degraded);
				~Cal();

				bool Degraded() const { return degraded_; }

				bool Utf8Valid(const uint8_t* data, size_t size);
				Err Nfc(const uint8_t* in, size_t in_len, uint8_t* out, size_t cap, size_t* out_len);
				Err Bidi(const uint8_t* utf8, size_t len, int32_t* base_level, int32_t* run_count);
				Err BidiRuns(const uint8_t* utf8, size_t len, BidiRun* runs, size_t cap, size_t* run_count);
				Err Shape(const char* font_path, uint32_t face_index, const uint8_t* utf8, size_t len,
						  uint32_t pixel_size, uint32_t* glyphs, int32_t* advances, size_t cap, size_t* glyph_count,
						  int32_t* rtl);
				Metrics* OpenMetrics(const char* font_path, uint32_t face_index, uint32_t pixel_size, int kind);
				Err FaceCovers(const char* font_path, uint32_t face_index, uint32_t codepoint, int32_t* covers);
				Err Raster(const char* font_path, uint32_t face_index, uint32_t pixel_size, uint32_t codepoint,
						   uint8_t** bitmap, uint32_t* width, uint32_t* height, int32_t* left, int32_t* top);
				Err RasterGlyph(const char* font_path, uint32_t face_index, uint32_t pixel_size, uint32_t glyph_id,
								uint8_t** bitmap, uint32_t* width, uint32_t* height, int32_t* left, int32_t* top);
				Err RasterGlyphFmt(const char* font_path, uint32_t face_index, uint32_t pixel_size, uint32_t glyph_id,
								   uint8_t** bitmap, uint32_t* width, uint32_t* height, int32_t* left, int32_t* top,
								   int32_t* format);
				Err RasterGlyphStroked(const char* font_path, uint32_t face_index, uint32_t pixel_size,
									   uint32_t glyph_id, uint32_t stroke_width, uint8_t** fill, uint8_t** outline,
									   uint32_t* width, uint32_t* height, int32_t* left, int32_t* top);
				Err ResolveSystemFont(const char* family, char* out_path, size_t cap, size_t* out_len);
				Err EnumFileFaces(const char* font_path, FaceInfo* out, size_t cap, size_t* out_count);
				Err SelectFace(const char* dir, const char* family, uint32_t weight, int32_t italic, FaceInfo* out_face,
							   char* out_path, size_t cap, size_t* out_len);
				Err SelectSystemFace(const char* family, uint32_t weight, int32_t italic, FaceInfo* out_face,
									 char* out_path, size_t cap, size_t* out_len);

				private:

				Cal() = default;
				bool degraded_ = true;
				std::unique_ptr<IUtf8Validator> utf8_;
				std::unique_ptr<INormalizer> normalize_;
				std::unique_ptr<IBidiEngine> bidi_;
				std::unique_ptr<ITextShaper> shaper_;
				std::unique_ptr<IFontFace> face_;
				std::unique_ptr<IFontResolver> resolver_;
				std::unique_ptr<IGlyphRasterizer> raster_;
			};

			/* 后端工厂：返回 nullptr 表示该后端缺席，Cal 就回落降级面。 */
			IUtf8Validator* CreateSimdutfValidator();
			INormalizer* CreateUtf8procNormalizer();
			IBidiEngine* CreateSheenBidiEngine();
			ITextShaper* CreateHarfBuzzShaper();
			IFontFace* CreateFreeTypeFace();
			IFontResolver* CreatePlatformFontResolver();
			IGlyphRasterizer* CreateFreeTypeRasterizer();

			/* face 选择的两条共用判据（目录侧与系统侧同用一份）：归一化字族名、按 style 硬过滤 + weight 最近匹配。 */
			struct PickInput
			{
				const char* family;
				uint32_t weight;
				int32_t italic;
			};

			std::string NormalizeFamily(const std::string& s);
			Err PickFace(const PickInput& in, const std::vector<std::string>& paths, FaceInfo* out_face,
						 std::string* out_path);
		}	 // namespace cal
	}	 // namespace subtitle
}	 // namespace clv

#endif	  // CLV_SUBTITLE_CAL_H
