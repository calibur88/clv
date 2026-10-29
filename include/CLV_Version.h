/* CLV_Version.h
 *
 * 库版本与「本构建的形状」查询。三段数字与字符串同源：版本号只在下面的四个宏处定义，
 * `src/version.cpp` 从宏取值。字符串带预发布后缀（-dev），三段数字只表达数值部分。
 *
 * `CLV_Capabilities` 返回能力位图，位名在 `CLV_Export.h`（`CLV_CAP_*`）。这一问回答的是**这一份构建**
 * 编进了什么，不随任何调用改变：没有任何入口能把一位从 0 翻成 1，也没有入口能让句柄「暂时不支持」。
 * 未分配的位（3 起）读到 0 不携带任何信息，调用方不该据此推断「以后会是 1」。
 */
#ifndef CLV_VERSION_H
#define CLV_VERSION_H

#include "CLV_Export.h"

#include <stdint.h>

#define CLV_VERSION_MAJOR  0
#define CLV_VERSION_MINOR  9
#define CLV_VERSION_PATCH  0
#define CLV_VERSION_STRING "0.9.0-dev"

#ifdef __cplusplus
extern "C"
{
#endif

	CLV_EXPORT const char* CLV_VersionString(void);
	CLV_EXPORT int CLV_VersionMajor(void);
	CLV_EXPORT int CLV_VersionMinor(void);
	CLV_EXPORT int CLV_VersionPatch(void);
	/* 能力位图。位名与各自的承诺见 `CLV_Export.h`；返回值的未定义位恒 0。 */
	CLV_EXPORT uint32_t CLV_Capabilities(void);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_VERSION_H
