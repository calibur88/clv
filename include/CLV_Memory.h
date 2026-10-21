/* CLV_Memory.h
 *
 * 分配口的 C ABI 契约：直通 C 运行时分配器，无策略、无重试、无日志。
 *
 * 1. 这三只是给调用方的**便利口**，不是注入点。库里没有任何一处把调用方的分配器当钩子用，
 *    也没有任何入口能替换它们 —— 「重载它们来改变库的行为」这条路不存在。
 * 2. **库内分配不经这三只口**：实现层走标准库分配器（STL 容器、智能指针、`new` 表达式），
 *    所以宿主在自己模块里重载全局 `operator new` / `operator delete` 只会影响库内部那一路，
 *    与 `CLV_Alloc` 拿到的内存**不承诺同源**。要同源，只能宿主自己用 `CLV_Alloc` 分配再交进来。
 * 3. 三只口继续走 C 运行时 `malloc` / `realloc` / `free`：C++ 的分配函数不带「按尺寸查询」，
 *    `CLV_Realloc` 的语义因此做不到由 `operator new` 承担。失败行为 = 返回 NULL、不抛、不重试、不记日志，
 *    判空由调用方做。这是**本库唯一一处「分配失败以 NULL 形态出现在调用方手里」**：
 *    视频 / 音频 / 容器 / 节拍四面入口在各自的 `extern "C"` 边界上接住 `std::bad_alloc`，
 *    收口成该面的 `CLV_VIDEO_OUT_OF_MEMORY` / `CLV_AUDIO_OUT_OF_MEMORY` / `CLV_CONTAINER_OUT_OF_MEMORY` /
 *    `CLV_PACE_OUT_OF_MEMORY`，所以「调用方判空」只对直接用 `CLV_Alloc` 系列成立，
 *    经四面入口进来的分配失败不会以 NULL 出现。两档形态是刻意分开的，不是漏收。
 * 4. 要接管库内分配 = 本 ABI **未提供的能力**：`CLV_Capabilities` 的 bit2（`CLV_CAP_HOST_ALLOCATOR`）
 *    恒为 0，且那一格永不赋予新义。
 * 5. `CLV_Free` 接受 NULL，是 no-op；对同一指针重复 Free 是未定义行为（同 C free）。
 *    `CLV_Realloc` 语义同 C realloc：p 为 NULL 等价于 Alloc(size)；size 为 0 的行为不承诺。
 *    `CLV_Alloc` 不清零。
 * 6. 跨模块传递与释放需宿主自行保证同一 CRT 实例（静态 `/MT*` 与动态 `/MD*` 不可混链）；
 *    对齐要求按基础类型，不承诺超对齐。**本 ABI 对「同一 CRT 堆」不作承诺，也不检测**。
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
