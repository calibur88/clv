/* platform.h
 *
 * 平台判定的兜底落点 + 平台宏映射。
 *
 * - CMake 构建：顶层的 if(WIN32) / if(APPLE) / if(UNIX) 是主推，判定后传 -DCLV_PLATFORM_*=1，
 *   本文件外层 !defined 检测因此跳过推导。
 * - 手编 / IDE 索引 / clang-tidy：没有 CMake 传宏，本文件按编译器目标宏自己推。
 * - 两条路径互斥，不存在两份并行判定。
 *
 * 裸宏（_WIN32 / __linux__ / __APPLE__ / TARGET_OS_*）只允许出现在本文件；其余文件一律只判
 * CLV_PLATFORM_*。唯一豁免是 include/CLV_Export.h 判 _WIN32 选符号可见性语法。
 *
 * 约定：CMake 侧只传值为 1 的宏（-DCLV_PLATFORM_WINDOWS=1），不传 0——下面的互斥判定用
 * defined() 计数，值为 0 也算「已定义」。
 */
#ifndef CLV_PLATFORM_H
#define CLV_PLATFORM_H

// ---------- 编译器目标宏 -> CLV_PLATFORM_*（兜底）----------

#if ! defined(CLV_PLATFORM_WINDOWS) && ! defined(CLV_PLATFORM_LINUX) && ! defined(CLV_PLATFORM_MACOS) && \
	! defined(CLV_PLATFORM_IOS)

	#if defined(_WIN32)
		#define CLV_PLATFORM_WINDOWS 1

	#elif defined(__linux__)
		// Android 扩展点：将来要区分 Android 时，此分支内先判 __ANDROID__，外层 !defined 那一排
		// 补 CLV_PLATFORM_ANDROID，CMake 的 if 块也加一支。
		#define CLV_PLATFORM_LINUX 1

	#elif defined(__APPLE__)
		// TargetConditionals.h 是 Apple 专属头，只允许出现在这个分支里。
		#include <TargetConditionals.h>
		#if TARGET_OS_OSX
			#define CLV_PLATFORM_MACOS 1
		#elif TARGET_OS_IOS
			#define CLV_PLATFORM_IOS 1
		#else
			#error "CLV: unknown Apple platform"
		#endif

	#else
		#error "CLV: unsupported platform"
	#endif

#endif

// ---------- 至多一个：防止外部误定义多个平台标记 ----------

#if (defined(CLV_PLATFORM_WINDOWS) + defined(CLV_PLATFORM_LINUX) + defined(CLV_PLATFORM_MACOS) + \
	 defined(CLV_PLATFORM_IOS)) > 1
	#error "CLV: multiple CLV_PLATFORM_* defined"
#endif

// ---------- 平台名字（只给运行时信息与日志用，不参与任何 #if 选源）----------

#if defined(CLV_PLATFORM_WINDOWS)
	#define CLV_PLATFORM_NAME "windows"
#elif defined(CLV_PLATFORM_LINUX)
	#define CLV_PLATFORM_NAME "linux"
#elif defined(CLV_PLATFORM_MACOS)
	#define CLV_PLATFORM_NAME "macos"
#elif defined(CLV_PLATFORM_IOS)
	#define CLV_PLATFORM_NAME "ios"
#else
	#error "CLV: CLV_PLATFORM_NAME not defined for current platform"
#endif

#endif	  // CLV_PLATFORM_H
