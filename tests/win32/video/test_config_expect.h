/* test_config_expect.h
 *
 * 判据分档的唯一出处：全树只有本件按系数编码法分档，`#if` 只许写在这里。
 * 生成的期望载体件（`video_payload_fixtures_p.h`）两档数组都发（后缀 F0 = 定宽、F1 = EOB+EG），
 * 自己不做选择、也不留 block_size 与整帧合计这类「选了哪档才知道」的数；本件把选择结果用回既有名字，
 * 所以消费方不必知道分档存在。直接 include 本件的四件（按名字列，不数调用点）：本目录的
 * `test_video_payload.cpp`、`test_video_domain_edges.cpp`，以及 `api/test_video_tool_rejection.cpp`、
 * `independent/test_ind_video_motion.cpp`（后两件按相对路径引本件）。
 *
 * 为什么要有本件：定宽档与默认档（法 1）写出的载体字节与实长不同，选哪一档由视频帧头声明，
 * 期望值必须跟着档走 —— 只钉默认档那一份的话，换 `CLV_COEF_CODING=0` 构型时红的是绑错档的期望值，
 * 不是器件回归。全树的分档只落这一件，消费方读的是本件选完档之后的名字。
 * 两档的数各由 `scripts/gen_video_payload_fixtures.py` 独立算出，实长另在下面的 static_assert 上钉住算式。
 */
#ifndef CLV_TEST_CONFIG_EXPECT_H
#define CLV_TEST_CONFIG_EXPECT_H

#ifndef CLV_COEF_CODING
	#error "CLV_COEF_CODING 必须以 0 或 1 显式到达测试 TU（仓域 add_compile_definitions 负责送；到不了就是配置漂移）"
#endif

#include "video_payload_fixtures_p.h"

#include <cstdint>

namespace clvtest
{

#if CLV_COEF_CODING
	/* 法 1：EOB 截尾 + Exp-Golomb 幅值，块边界自终止 ⇒ 载体实长由内容决定，没有「块数 × 单块」的闭式。
	 * 平帧对：4 个亮度块在场，各 all_zero(1) + eob(6) + EG(8159) = 33 bit ⇒ 132 bit ⇒ 补齐 17 B。 */
	inline constexpr bool kHasCoefCodingSelector = true;
	inline constexpr const unsigned char* kZeroFrameMbCarrier = kZeroFrameMbCarrierF1;
	inline constexpr uint32_t kZeroFrameMbCarrierLen = kZeroFrameMbCarrierLenF1;
	inline constexpr const unsigned char* kPlane16MbCarrier = kPlane16MbCarrierF1;
	inline constexpr uint32_t kPlane16MbCarrierLen = kPlane16MbCarrierLenF1;
	inline constexpr uint32_t kPayloadAllZeroMB = kZeroFrameMbCarrierLenF1;	   // = 18u
	inline constexpr uint32_t kPayloadPlane16MB = kPlane16MbCarrierLenF1;	   // = 56u
	inline constexpr uint32_t kPayloadFlatPairMB = 17u;
#else
	/* 定宽：每系数 1 位符号 + bits−1 位幅值直写，零照样占位 ⇒ 实长可乘。
	 * q' = 24 帧内落 17..32 档 → bits = 9 → 单块 64 × 9 ÷ 8 = 72 B、宏块 6 块 = 432 B。
	 * 平帧对是帧间块：q = 1 落 1..1 档 → bits = 14 → 单块 112 B、4 块 = 448 B。 */
	inline constexpr bool kHasCoefCodingSelector = false;
	inline constexpr const unsigned char* kZeroFrameMbCarrier = kZeroFrameMbCarrierF0;
	inline constexpr uint32_t kZeroFrameMbCarrierLen = kZeroFrameMbCarrierLenF0;
	inline constexpr const unsigned char* kPlane16MbCarrier = kPlane16MbCarrierF0;
	inline constexpr uint32_t kPlane16MbCarrierLen = kPlane16MbCarrierLenF0;
	inline constexpr uint32_t kPayloadAllZeroMB = kZeroFrameMbCarrierLenF0;	   // = 432u
	inline constexpr uint32_t kPayloadPlane16MB = kPlane16MbCarrierLenF0;	   // = 432u
	inline constexpr uint32_t kPayloadFlatPairMB = 448u;
#endif

	/* block_size 与整帧载体合计 = 「本档载体实长」的推论，不在别处再写一遍数字，有读者的格才发。
	 * 「全树不再有第二处 18u / 56u / 432u」的机械证明 = 下面四条 static_assert：数组尺寸与登记的实长必须相等。 */
	inline constexpr uint32_t kZeroFrameMbBlockSize = kPayloadAllZeroMB;
	inline constexpr uint32_t kZeroFrameMbFrameCarrierTotal = kZeroFrameMbMbCount * kPayloadAllZeroMB;

	static_assert(sizeof(kZeroFrameMbCarrierF0) == kZeroFrameMbCarrierLenF0 * sizeof(unsigned char),
				  "定宽载体数组尺寸与登记的实长不符");
	static_assert(sizeof(kZeroFrameMbCarrierF1) == kZeroFrameMbCarrierLenF1 * sizeof(unsigned char),
				  "法 1 载体数组尺寸与登记的实长不符");
	static_assert(sizeof(kPlane16MbCarrierF0) == kPlane16MbCarrierLenF0 * sizeof(unsigned char),
				  "定宽 16×16 载体数组尺寸与登记的实长不符");
	static_assert(sizeof(kPlane16MbCarrierF1) == kPlane16MbCarrierLenF1 * sizeof(unsigned char),
				  "法 1 16×16 载体数组尺寸与登记的实长不符");
#if CLV_COEF_CODING
	static_assert(kPayloadAllZeroMB == 18u && kPayloadPlane16MB == 56u && kPayloadFlatPairMB == 17u,
				  "法 1 一侧的实长与本件登记值不符 ⇒ 生成件或本件有一处被改过，先对表再往下走");
#else
	static_assert(kPayloadAllZeroMB == 432u && kPayloadPlane16MB == 432u && kPayloadFlatPairMB == 448u,
				  "定宽一侧的实长与本件登记值不符：432 = 6 块 × 72 B、448 = 4 块 × 112 B");
#endif

}	 // namespace clvtest

#endif	  // CLV_TEST_CONFIG_EXPECT_H
