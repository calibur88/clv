/* 系统字体解析：DirectWrite 只把 family 展开成文件清单；face / weight / style 的判定
 * 仍走 PickFace 那一份读真实表的规则，与目录侧同判据。 */
#include "subtitle/cal/cal.h"

#include <algorithm>
#include <cstring>
#include <dwrite.h>
#include <new>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace clv
{
	namespace subtitle
	{
		namespace cal
		{
			namespace
			{
				Err CopyPath(const std::string& path, char* out, size_t cap, size_t* out_len)
				{
					*out_len = path.size();
					if (path.size() + 1u > cap) return kOutputTooSmall;
					std::memcpy(out, path.c_str(), path.size() + 1u);
					return kOk;
				}

				struct CoScope
				{
					bool uninit = false;

					bool Init()
					{
						const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
						if (hr == RPC_E_CHANGED_MODE) return true; /* 已在别的套间初始化：照用，不做收尾 */
						if (FAILED(hr)) return false;
						uninit = true;
						return true;
					}

					~CoScope()
					{
						if (uninit) CoUninitialize();
					}
				};

				bool ToWide(const char* utf8, std::wstring* out)
				{
					const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
					if (n <= 1) return false;
					out->assign(static_cast<size_t>(n), L'\0');
					return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out->data(), n) == n;
				}

				bool ToUtf8(const wchar_t* wide, std::string* out)
				{
					const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
					if (n <= 1) return false;
					std::vector<char> buf(static_cast<size_t>(n), '\0');
					if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf.data(), n, nullptr, nullptr) != n) return false;
					out->assign(buf.data());
					return true;
				}

				/* 一个 family 的全部 face 所在文件（去重，保持首次出现顺序）。 */
				Err CollectFamilyPaths(const wchar_t* family_w, std::vector<std::string>* out)
				{
					CoScope co;
					if (! co.Init()) return kBackendFailure;

					ComPtr<IDWriteFactory> factory;
					if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
												   reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))
					{
						return kBackendFailure;
					}

					ComPtr<IDWriteFontCollection> collection;
					if (FAILED(factory->GetSystemFontCollection(&collection, FALSE))) return kBackendFailure;

					UINT32 index = 0;
					BOOL exists = FALSE;
					if (FAILED(collection->FindFamilyName(family_w, &index, &exists)) || ! exists) return kUnsupported;

					ComPtr<IDWriteFontFamily> font_family;
					if (FAILED(collection->GetFontFamily(index, &font_family))) return kUnsupported;

					const UINT32 font_count = font_family->GetFontCount();
					Err rc = kUnsupported;
					for (UINT32 i = 0; i < font_count; ++i)
					{
						ComPtr<IDWriteFont> font;
						if (FAILED(font_family->GetFont(i, &font))) continue;
						ComPtr<IDWriteFontFace> font_face;
						if (FAILED(font->CreateFontFace(&font_face))) continue;

						UINT32 file_count = 0;
						if (FAILED(font_face->GetFiles(&file_count, nullptr)) || file_count == 0u) continue;
						std::vector<IDWriteFontFile*> files(file_count, nullptr);
						if (FAILED(font_face->GetFiles(&file_count, files.data())))
						{
							for (IDWriteFontFile* file: files)
								if (file != nullptr) file->Release();
							continue;
						}
						for (UINT32 k = 0; k < file_count; ++k)
						{
							IDWriteFontFile* file = files[k];
							if (file == nullptr) continue;
							const void* key = nullptr;
							UINT32 key_size = 0;
							if (FAILED(file->GetReferenceKey(&key, &key_size))) continue;
							ComPtr<IDWriteFontFileLoader> loader;
							if (FAILED(file->GetLoader(&loader))) continue;
							ComPtr<IDWriteLocalFontFileLoader> local;
							if (FAILED(loader.As(&local))) continue; /* 非本机 loader：不冒充路径 */
							UINT32 path_len = 0;
							if (FAILED(local->GetFilePathLengthFromKey(key, key_size, &path_len))) continue;
							std::wstring wide_path(static_cast<size_t>(path_len) + 1u, L'\0');
							if (FAILED(local->GetFilePathFromKey(key, key_size, wide_path.data(), path_len + 1u)))
								continue;
							std::string utf8_path;
							if (! ToUtf8(wide_path.c_str(), &utf8_path)) continue;
							if (std::find(out->begin(), out->end(), utf8_path) == out->end())
							{
								out->push_back(utf8_path);
								rc = kOk;
							}
						}
						for (IDWriteFontFile* file: files)
							if (file != nullptr) file->Release();
					}
					return rc;
				}

				class DirectWriteResolver: public IFontResolver
				{
					public:

					Err ResolveSystem(const char* family, char* out_path, size_t cap, size_t* out_len) override
					{
						if (family == nullptr || out_path == nullptr || out_len == nullptr) return kInvalidArgument;
						std::wstring family_w;
						if (! ToWide(family, &family_w)) return kInvalidArgument;
						std::vector<std::string> paths;
						const Err rc = CollectFamilyPaths(family_w.c_str(), &paths);
						if (rc != kOk) return rc;
						return CopyPath(paths.front(), out_path, cap, out_len);
					}

					Err SelectSystem(const char* family, uint32_t weight, int32_t italic, FaceInfo* out_face,
									 char* out_path, size_t cap, size_t* out_len) override
					{
						if (family == nullptr || out_face == nullptr || out_path == nullptr || out_len == nullptr)
							return kInvalidArgument;
						std::wstring family_w;
						if (! ToWide(family, &family_w)) return kInvalidArgument;
						std::vector<std::string> paths;
						const Err rc = CollectFamilyPaths(family_w.c_str(), &paths);
						if (rc != kOk) return rc;
						PickInput in { family, weight, italic };
						std::string picked;
						const Err pick = PickFace(in, paths, out_face, &picked);
						if (pick != kOk) return pick;
						return CopyPath(picked, out_path, cap, out_len);
					}
				};
			}	 // namespace

			IFontResolver* CreatePlatformFontResolver() { return new (std::nothrow) DirectWriteResolver(); }
		}	 // namespace cal
	}	 // namespace subtitle
}	 // namespace clv
