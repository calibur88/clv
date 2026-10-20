/* CLV_Memory.h
 *
 * 分配口的 C ABI 契约：直通 C 运行时分配器，无策略、无重试、无日志。
 *
 * - CLV_Alloc 不清零；失败返回 NULL。**分配口本身**对内存耗尽不设防（不重试、不记日志、不抛），判空由调用方做；
 *   三个媒体与容器入口（视频 / 音频 / 容器面）在各自的 `extern "C"` 边界上把分配失败收口成该面的
 *   `CLV_VIDEO_OUT_OF_MEMORY` / `CLV_AUDIO_OUT_OF_MEMORY` / `CLV_CONTAINER_OUT_OF_MEMORY`，所以「调用方判空」这条只对
 *   直接使用 `CLV_Alloc` 系列的口子成立，经三面入口进来的分配失败不会以 NULL 形态出现在调用方手里。
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

	CLV_EXPORT void* CLV_Alloc(size_t size);
	CLV_EXPORT void* CLV_Realloc(void* p, size_t size);
	CLV_EXPORT void CLV_Free(void* p);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_MEMORY_H
