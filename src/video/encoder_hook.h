/* encoder_hook.h
 *
 * 实现层测试钩子。**不进公开头、不新增 CLV_EXPORT、不动 ABI**。
 *
 * 单测要逐字节比对「编码器内部重建帧 == 解码器输出」，就得拿到编码器自己那份重建帧。
 * 它是一次编码的中间产物、不是 ABI 的一部分，故这个查询只开在实现层，公开面不给该出口。
 * 单测已经 include `src/` 下的实现层头（`tests/CMakeLists.txt` 的
 * `target_include_directories(... ${CMAKE_SOURCE_DIR}/src)`）。
 *
 * 带 `clv_internal_` 前缀、**不带 `CLV_EXPORT`**：符号不进动态件的导出表，外部消费者拿不到。
 * 本仓库的单测恒链静态件 `clv_static`（`CLV_BUILD_SHARED` 开不开都一样），所以这个钩子照常可链，
 * 不需要另立实现层测试目标。
 * 不要顺手给它加 `CLV_EXPORT` —— 加上即成 ABI 的一部分。
 *
 * 拿到的 `CLV_VideoFrame*` 归编码器所有，**不得 Release**，下次 Encode 后内容即被覆盖；
 * 只有 I / P 帧会更新它（B 帧不作参考）。
 */
#ifndef CLV_VIDEO_ENCODER_HOOK_H
#define CLV_VIDEO_ENCODER_HOOK_H

#include "CLV_Video.h"

#ifdef __cplusplus
extern "C"
{
#endif

	/* 返回编码器自己的参考帧（= 最近一次 I / P 编码的内部重建）。还没编过 I / P 帧时返回 NULL。
	 * 借出期间**只读、不得 Release**，编码器也不得 Close；内容以下一次 Encode 为界。
	 * 出参非 const：要能直接喂 `CLV_VideoFrameGetDesc`（它收非 const 入参）。 */
	CLV_VideoFrame* clv_internal_encoder_reconstruction(const CLV_VideoEncoder* e);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_VIDEO_ENCODER_HOOK_H

