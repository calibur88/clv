/* face 侧后端：FreeType 的 cmap、垂直度量、真实表枚举与目录内选择。
 * style 硬过滤 + weight 最近匹配只写一份（PickFace），系统侧复用同一份判据。 */
#include "subtitle/cal/cal.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

namespace clv
{
	namespace subtitle
	{
		namespace cal
		{
			namespace
			{
				constexpr long kFaceScanCap = 64;

				void CopyName(char* dst, size_t cap, const char* src)
				{
					if (cap == 0u) return;
					if (src == nullptr)
					{
						dst[0] = '\0';
						return;
					}
					size_t i = 0;
					for (; i + 1u < cap && src[i] != '\0'; ++i) dst[i] = src[i];
					dst[i] = '\0';
				}

				void ReadFace(FT_Face face, uint32_t index, FaceInfo* out)
				{
					std::memset(out, 0, sizeof(*out));
					out->face_index = index;
					out->weight = 400u;
					CopyName(out->family, sizeof(out->family), face->family_name);
					CopyName(out->subfamily, sizeof(out->subfamily), face->style_name);

					const TT_OS2* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
					if (os2 != nullptr && os2->usWeightClass != 0u)
					{
						uint32_t w = static_cast<uint32_t>(os2->usWeightClass);
						if (w > 1000u) w = 1000u;
						out->weight = w;
					}
					/* fsSelection 从 OS/2 v2 起才有；旧 Mac 字体 FreeType 把版本记成 0xFFFF */
					if (os2 != nullptr && os2->version >= 2u && os2->version != 0xFFFFu)
					{
						out->italic = (os2->fsSelection & 0x1u) != 0u ? 1 : 0;
					}
					else
					{
						const TT_Header* head = static_cast<const TT_Header*>(FT_Get_Sfnt_Table(face, FT_SFNT_HEAD));
						out->italic = (head != nullptr && (head->Mac_Style & 0x2u) != 0u) ? 1 : 0;
					}
				}

				/* 读不动的文件给空清单：回退链的语义是「这一项给不出就用下一项」。 */
				std::vector<FaceInfo> FacesOf(const std::string& path)
				{
					std::vector<FaceInfo> out;
					FT_Library lib = nullptr;
					if (FT_Init_FreeType(&lib) != 0) return out;
					FT_Face probe = nullptr;
					if (FT_New_Face(lib, path.c_str(), 0, &probe) != 0)
					{
						FT_Done_FreeType(lib);
						return out;
					}
					long total = probe->num_faces > 0 ? probe->num_faces : 1;
					FT_Done_Face(probe);
					if (total > kFaceScanCap) total = kFaceScanCap;
					for (long i = 0; i < total; ++i)
					{
						FT_Face face = nullptr;
						if (FT_New_Face(lib, path.c_str(), i, &face) != 0) continue;
						FaceInfo info;
						ReadFace(face, static_cast<uint32_t>(i), &info);
						FT_Done_Face(face);
						out.push_back(info);
					}
					FT_Done_FreeType(lib);
					return out;
				}

				bool IsFontFile(const std::string& ext_lower)
				{
					return ext_lower == ".otf" || ext_lower == ".ttf" || ext_lower == ".otc" || ext_lower == ".ttc";
				}

				Err CopyPath(const std::string& path, char* out, size_t cap, size_t* out_len)
				{
					*out_len = path.size();
					if (path.size() + 1u > cap) return kOutputTooSmall;
					std::memcpy(out, path.c_str(), path.size() + 1u);
					return kOk;
				}

				std::string LowerAscii(std::string s)
				{
					for (char& c: s)
						if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
					return s;
				}

				bool HitsFamily(const std::string& q, const FaceInfo& f, const std::string& stem)
				{
					if (q.empty()) return false;
					const std::string fam = NormalizeFamily(f.family);
					const std::string both = fam + NormalizeFamily(f.subfamily);
					if (q == fam || q == both) return true;
					if (! fam.empty() && (fam.find(q) != std::string::npos || q.find(fam) != std::string::npos))
						return true;
					if (! both.empty() && (both.find(q) != std::string::npos || q.find(both) != std::string::npos))
						return true;
					if (! stem.empty() && (stem.find(q) != std::string::npos || q.find(stem) != std::string::npos))
						return true;
					return false;
				}

				class FreeTypeFace: public IFontFace
				{
					public:

					Err Covers(const char* font_path, uint32_t face_index, uint32_t codepoint, int32_t* covers) override
					{
						if (font_path == nullptr || covers == nullptr) return kInvalidArgument;
						FT_Library lib = nullptr;
						FT_Face face = nullptr;
						/* 覆盖查询与字号无关：不设 Pixel_Sizes，否则非 scalable 面会因非原生 ppem 报错 */
						if (FT_Init_FreeType(&lib) != 0) return kBackendFailure;
						if (FT_New_Face(lib, font_path, static_cast<long>(face_index), &face) != 0)
						{
							FT_Done_FreeType(lib);
							return kBackendFailure;
						}
						if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) *covers = 0;
						else *covers = FT_Get_Char_Index(face, static_cast<FT_ULong>(codepoint)) != 0u ? 1 : 0;
						FT_Done_Face(face);
						FT_Done_FreeType(lib);
						return kOk;
					}

					Err EnumFileFaces(const char* font_path, FaceInfo* out, size_t cap, size_t* out_count) override
					{
						if (font_path == nullptr || out_count == nullptr || (out == nullptr && cap != 0u))
							return kInvalidArgument;
						const std::vector<FaceInfo> all = FacesOf(font_path);
						if (all.empty()) return kBackendFailure;
						*out_count = all.size();
						const size_t n = std::min(cap, all.size());
						for (size_t i = 0; i < n; ++i) out[i] = all[i];
						return all.size() > cap ? kOutputTooSmall : kOk;
					}

					Err SelectFace(const char* dir, const char* family, uint32_t weight, int32_t italic,
								   FaceInfo* out_face, char* out_path, size_t cap, size_t* out_len) override
					{
						if (dir == nullptr || family == nullptr || out_face == nullptr || out_path == nullptr ||
							out_len == nullptr)
						{
							return kInvalidArgument;
						}
						std::vector<std::string> paths;
						try
						{
							for (const auto& entry: std::filesystem::directory_iterator(dir))
							{
								if (! entry.is_regular_file()) continue;
								if (! IsFontFile(LowerAscii(entry.path().extension().string()))) continue;
								paths.push_back(entry.path().string());
							}
						}
						catch (...)
						{
							return kBackendFailure;
						}
						PickInput in { family, weight, italic };
						std::string picked;
						const Err rc = PickFace(in, paths, out_face, &picked);
						if (rc != kOk) return rc;
						return CopyPath(picked, out_path, cap, out_len);
					}
				};
			}	 // namespace

			std::string NormalizeFamily(const std::string& s)
			{
				std::string out;
				out.reserve(s.size());
				for (size_t i = 0; i < s.size(); ++i)
				{
					const unsigned char c = static_cast<unsigned char>(s[i]);
					if (c >= 'A' && c <= 'Z') out.push_back(static_cast<char>(c - 'A' + 'a'));
					else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(static_cast<char>(c));
					/* 空格 / 连字符 / 下划线 / 点丢掉：比较只看字母数字 */
				}
				return out;
			}

			Err PickFace(const PickInput& in, const std::vector<std::string>& paths, FaceInfo* out_face,
						 std::string* out_path)
			{
				if (in.family == nullptr || out_face == nullptr || out_path == nullptr) return kInvalidArgument;
				const std::string q = NormalizeFamily(in.family);
				const int32_t want = in.italic != 0 ? 1 : 0;
				std::vector<std::string> sorted = paths;
				std::sort(sorted.begin(), sorted.end());

				bool found = false;
				uint64_t best_gap = 0;
				FaceInfo best;
				std::memset(&best, 0, sizeof(best));
				std::string best_path;

				for (const std::string& path: sorted)
				{
					const std::filesystem::path p(path);
					const std::string stem = NormalizeFamily(p.stem().string());
					const std::vector<FaceInfo> faces = FacesOf(path);
					for (const FaceInfo& f: faces)
					{
						if (! HitsFamily(q, f, stem)) continue;
						if (f.italic != want) continue; /* style 硬过滤 */
						const uint64_t gap = f.weight > in.weight ? static_cast<uint64_t>(f.weight - in.weight) :
																	static_cast<uint64_t>(in.weight - f.weight);
						if (! found || gap < best_gap || (gap == best_gap && f.weight < best.weight))
						{
							found = true;
							best_gap = gap;
							best = f;
							best_path = path;
						}
					}
				}
				if (! found) return kUnsupported;
				*out_face = best;
				*out_path = best_path;
				return kOk;
			}

			IFontFace* CreateFreeTypeFace() { return new (std::nothrow) FreeTypeFace(); }
		}	 // namespace cal
	}	 // namespace subtitle
}	 // namespace clv
