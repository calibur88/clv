/* CLV_Export.h
 *
 * 导出宏只有一个名字：`CLV_EXPORT`，不留同义别名。
 * 依据：同一条 ABI 上挂两个名字，改一处漏一处的风险不划算。
 *
 * 五种组合：
 * - Windows + 构建动态库（`CLV_BUILD_DLL`）   -> __declspec(dllexport)
 * - Windows + 使用动态库（`CLV_USE_DLL`）     -> __declspec(dllimport)
 * - Windows + 静态库（两个宏都不定义）        -> 空
 * - 非 Windows + 构建动态库                   -> __attribute__((visibility("default")))（GCC 4 起才有该属性）
 * - 非 Windows + 静态库 / 使用侧              -> 空
 *
 * 两个宏都由构建侧给：动态目标带 PRIVATE `CLV_BUILD_DLL`、INTERFACE `CLV_USE_DLL`，
 * 静态目标两者都不带 ⇒ 消费者不需要自己定义任何宏，链哪个目标就拿到哪个的声明。
 *
 * 判 `_WIN32` 只用于在 __declspec 与 __attribute__ 两种符号可见性语法之间选择，
 * 不用于平台业务判断；业务平台判定一律走 platform/platform.h，本头不扩用这条判定。
 */
#ifndef CLV_EXPORT_H
#define CLV_EXPORT_H

#if defined(_WIN32)
	#if defined(CLV_BUILD_DLL)
		#define CLV_EXPORT __declspec(dllexport)
	#elif defined(CLV_USE_DLL)
		#define CLV_EXPORT __declspec(dllimport)
	#else
		#define CLV_EXPORT
	#endif
#else
	#if defined(CLV_BUILD_DLL) && defined(__GNUC__) && (__GNUC__ >= 4)
		#define CLV_EXPORT __attribute__((visibility("default")))
	#else
		#define CLV_EXPORT
	#endif
#endif

/* 能力位（`CLV_Capabilities` 的返回值形状）。本头带这些常量是因为它们描述的是「这一份构建的形状」，
 * 与导出宏同源，都由构建侧决定；调用方拿到位值后按名判，不要写死数字。
 *
 * 这三格描述的是**本构建的形状，不随调用改变**：没有任何入口能把一位从 0 翻成 1。
 * 段尾追加，已定义的位永不改义；号 2 尤其如此 —— 宿主注入分配器这件事不做，那位永远是 0。 */
#define CLV_CAP_MEMORY_FILE	   0x1u /* bit0：内存缓冲句柄可用（文件面的 `CLV_OpenMemoryFile` 是实现态） */
#define CLV_CAP_LOG_STORAGE	   0x2u /* bit1：日志可落盘。spdlog 后端置 1，空后端置 0 */
#define CLV_CAP_HOST_ALLOCATOR 0x4u /* bit2：恒 0，且永不赋予新义（库内分配不经公开分配口） */

#endif	  // CLV_EXPORT_H
