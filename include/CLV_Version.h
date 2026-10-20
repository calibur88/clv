/* CLV_Version.h
 *
 * 库版本查询。三段数字与字符串同源：版本号只在下面的四个宏处定义，`src/version.cpp` 从宏取值。
 * 字符串带预发布后缀（-dev），三段数字只表达数值部分。
 */
#ifndef CLV_VERSION_H
#define CLV_VERSION_H

#include "CLV_Export.h"

#define CLV_VERSION_MAJOR  0
#define CLV_VERSION_MINOR  8
#define CLV_VERSION_PATCH  1
#define CLV_VERSION_STRING "0.8.1-dev"

#ifdef __cplusplus
extern "C"
{
#endif

	CLV_EXPORT const char* CLV_VersionString(void);
	CLV_EXPORT int CLV_VersionMajor(void);
	CLV_EXPORT int CLV_VersionMinor(void);
	CLV_EXPORT int CLV_VersionPatch(void);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_VERSION_H
