/* C++17 compile probe for include/CLV_Subtitle.h: the same header under extern "C" and C++17,
 * with no C++-only requirement leaking into the public surface. */
#include "CLV_Subtitle.h"

/* MSVC 的 __cplusplus 默认停在 199711L，除非加 /Zc:__cplusplus —— 用 _MSVC_LANG 判实际语言档 */
#if defined(_MSVC_LANG)
	#if _MSVC_LANG < 201703L
		#error "subtitle header probe expects C++17"
	#endif
#elif __cplusplus < 201703L
	#error "subtitle header probe expects C++17"
#endif

int clv_subtitle_header_probe_cxx17()
{
	CLV_SubtitleLineC line {};
	CLV_SubtitleStatsC st {};
	CLV_SubtitleRenderStatsC rs {};
	line.struct_size = static_cast<uint32_t>(sizeof(line));
	st.struct_size = static_cast<uint32_t>(sizeof(st));
	rs.struct_size = static_cast<uint32_t>(sizeof(rs));
	return static_cast<int>(line.struct_size + st.struct_size + rs.struct_size);
}
