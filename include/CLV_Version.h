/* CLV_Version.h
 *
 * 库版本查询。三段数字与字符串同源：改版本只动下面的四个宏，`src/version.cpp` 读宏不写字面量。
 * 字符串带预发布后缀（-dev），三段数字只表达数值部分。
 */
#ifndef CLV_VERSION_H
#define CLV_VERSION_H

#include "CLV_Export.h"

#define CLV_VERSION_MAJOR  0
#define CLV_VERSION_MINOR  0
#define CLV_VERSION_PATCH  2
#define CLV_VERSION_STRING "0.0.2-dev"

#ifdef __cplusplus
extern "C"
{
#endif

	CLV_API const char* CLV_VersionString(void);
	CLV_API int CLV_VersionMajor(void);
	CLV_API int CLV_VersionMinor(void);
	CLV_API int CLV_VersionPatch(void);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_VERSION_H
