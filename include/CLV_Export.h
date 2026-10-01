/* CLV_Export.h
 *
 * 符号可见性开关。四态：
 * - Windows + 构建动态库（CLV_BUILD_SHARED）      -> __declspec(dllexport)
 * - Windows + 使用动态库（CLV_USE_SHARED）        -> __declspec(dllimport)
 * - Windows + 静态库（两个宏都不定义）            -> 空
 * - 非 Windows + 构建动态库                       -> __attribute__((visibility("default")))
 * - 非 Windows + 静态库 / 使用侧                  -> 空
 *
 * 注：这里判 _WIN32 是为了在 __declspec 与 __attribute__ 两种符号可见性语法之间选择，
 * 不是平台业务判断。属明文豁免，不违反「裸宏只出现在 platform/platform.h」的约束。
 */
#ifndef CLV_EXPORT_H
#define CLV_EXPORT_H

#if defined(_WIN32)
	#if defined(CLV_BUILD_SHARED)
		#define CLV_API __declspec(dllexport)
	#elif defined(CLV_USE_SHARED)
		#define CLV_API __declspec(dllimport)
	#else
		#define CLV_API
	#endif
#else
	#if defined(CLV_BUILD_SHARED)
		#define CLV_API __attribute__((visibility("default")))
	#else
		#define CLV_API
	#endif
#endif

#endif	  // CLV_EXPORT_H
