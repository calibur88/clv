/* CLV_Memory.h
 *
 * 分配口的 C ABI 契约。本期是 C 运行时分配器的直通，不带策略、不重试、不记日志。
 *
 * - CLV_Alloc 不清零；失败返回 NULL。全库对内存耗尽不设防：判空由调用方做。
 * - CLV_Free 接受 NULL，是 no-op；对同一指针重复 Free 是未定义行为（同 C free）。
 * - CLV_Realloc 语义同 C realloc：p 为 NULL 等价于 Alloc(size)；size 为 0 的行为不承诺。
 * - 三者的指针可跨模块边界传递（同一 CRT 堆内），对齐要求按基础类型，不承诺超对齐。
 */
#ifndef CLV_MEMORY_H
#define CLV_MEMORY_H

#include "CLV_Export.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

	CLV_API void* CLV_Alloc(size_t size);
	CLV_API void* CLV_Realloc(void* p, size_t size);
	CLV_API void CLV_Free(void* p);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_MEMORY_H
