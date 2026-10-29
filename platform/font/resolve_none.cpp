// 平台判定先取兜底推导件：CMake 没传 CLV_PLATFORM_* 宏时（手编 / IDE 索引 / clang-tidy）由它按
// 编译器目标宏推导，守卫与整件自守卫的平台件才会判到同一个平台。
#include "platform/platform.h"

/* 非 Windows 平台没有系统字体解析实现：工厂回 nullptr，Cal 落 NoResolver（各口回 UNSUPPORTED）。
 * Windows 上这份整件不产出符号，由 platform/font/resolve_win32.cpp 那一份给。 */
#if ! defined(CLV_PLATFORM_WINDOWS)

	#include "subtitle/cal/cal.h"

namespace clv
{
	namespace subtitle
	{
		namespace cal
		{
			IFontResolver* CreatePlatformFontResolver() { return nullptr; }
		}	 // namespace cal
	}	 // namespace subtitle
}	 // namespace clv

#endif
