/* test_video_transform_lossless.cpp
 *
 * **实现层用例**：直接 include 实现层头，不经公开 C ABI，仅静态构建（`CLV_BUILD_SHARED=OFF`）下可链。
 *
 * 「这个编解码器做不做得到源无损」的三条算术前提，落成常驻断言：
 *   一 · 变换本身可逆：正变换不归一（`T = H X Hᵀ`），反变换逐点精确还原整数输入块（`H Hᵀ = 64 I`）
 *        ⇒ 绕过量化直接载变换系数这条路，在变换这一层没有精度损失。
 *   二 · **有损**档不给无损留位置：最细档 `q = 1` 的载体上界恰好是满幅包络减一
 *        （帧内 4095 ⇒ 反分段 8191，满幅 8192；帧间 8159 ⇒ 反分段 16319，满幅 16320），
 *        而有损电平集对任何 q 都只给奇数（q 奇 ⇒ 奇×奇；q 偶 ⇒ 奇×偶 − 1），
 *        那两个偶数满幅值由量化本身取不到 ⇒ 无损必须自己占一条载体档。
 *   三 · 无损档占的就是那条 15 bit 档：位宽 = 1 bit 符号 + ⌈log2(满幅界 + 1)⌉ = 1 + 14，
 *        帧内帧间同宽；它只服务 `q = 0`，帧内 / 帧间各七档有损位宽一位没动。
 */

#include "video/block.h"
#include "video/quant.h"
#include "video/reconstruct.h"
#include "video/status.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{

	using clv::video::BlockBits;
	using clv::video::BlockBytes;
	using clv::video::BlockQfMax;
	using clv::video::DequantizeValue;
	using clv::video::Hadamard8;
	using clv::video::InverseHadamard8;
	using clv::video::kBlockCoefs;
	using clv::video::kCoefClampMaxInter8;
	using clv::video::kCoefClampMaxIntra8;
	using clv::video::QuantizeValue;
	using clv::video::RoundResidual;

	/* 确定性整数块：覆盖帧内残差域 [−128, 127] 与帧间残差域 [−255, 255] 两端，
	 * 外加「只动一个系数」「锯齿」「满幅常数」三种形态。 */
	std::vector<std::vector<int32_t>> ProbeBlocks()
	{
		std::vector<std::vector<int32_t>> out;
		std::vector<int32_t> b(kBlockCoefs);
		for (int i = 0; i < kBlockCoefs; ++i) b[i] = ((i % 9) - 4) * 31;	// 帧内域内起伏
		out.push_back(b);
		for (int i = 0; i < kBlockCoefs; ++i) b[i] = (i % 2 == 0) ? 127 : -128;	   // 帧内满幅锯齿
		out.push_back(b);
		for (int i = 0; i < kBlockCoefs; ++i) b[i] = (i % 2 == 0) ? 255 : -255;	   // 帧间满幅锯齿
		out.push_back(b);
		for (int i = 0; i < kBlockCoefs; ++i) b[i] = 255;
		out.push_back(b);
		for (int i = 0; i < kBlockCoefs; ++i) b[i] = -255;
		out.push_back(b);
		std::vector<int32_t> one(kBlockCoefs, 0);
		one[3 * 8 + 5] = 77;	// 只动一个 AC
		out.push_back(one);
		one.assign(kBlockCoefs, 0);
		one[0] = -129;	  // 只动 DC
		out.push_back(one);
		return out;
	}

}	 // namespace

/* 一 · 变换可逆：整数输入块正变换后立刻反变换，逐点等于原值（不经量化）。 */
TEST(TransformLossless, ForwardThenInverseHadamardIsExactOnIntegerBlocks)
{
	size_t checked = 0u;
	for (const auto& x: ProbeBlocks())
	{
		int32_t t[kBlockCoefs];
		Hadamard8(x.data(), t);
		int32_t b[kBlockCoefs];
		InverseHadamard8(t, b);
		size_t bad = 0u;
		for (int i = 0; i < kBlockCoefs; ++i)
		{
			// 反变换给出的整数还原必须逐点等于原值，容差 0
			if (RoundResidual(b[i]) != x[i]) ++bad;
		}
		EXPECT_EQ(bad, 0u) << "块 0 点 = " << x[0];
		checked += kBlockCoefs;
	}
	EXPECT_EQ(checked, static_cast<size_t>(7) * kBlockCoefs);
	std::printf("变换可逆：%zu 个整数系数逐点还原，容差 0\n", checked);
}

/* 二 · 最细档载体装不下满幅包络，且电平集恒奇 ⇒ 偶数满幅值取不到。
 * 这既是「q=1 不是无损档」的算术理由，也说明真无损要动码流格式。 */
TEST(TransformLossless, FinestTierCarrierCannotHoldFullAmplitudeAndLevelsAreOddOnly)
{
	// 帧内：变换输入是 像素 − 128 ∈ [−128, 127] ⇒ 满幅包络 = 64 × 128 = 8192
	ASSERT_EQ(kCoefClampMaxIntra8, 8192);
	ASSERT_EQ(64 * 128, kCoefClampMaxIntra8);
	EXPECT_EQ(BlockQfMax(1u, true), 4095);
	EXPECT_EQ(DequantizeValue(4095, 1u), 8191);
	EXPECT_LT(DequantizeValue(BlockQfMax(1u, true), 1u), kCoefClampMaxIntra8);

	// 帧间：变换输入是 源 − 预测 ∈ [−255, 255] ⇒ 满幅包络 = 64 × 255 = 16320
	ASSERT_EQ(kCoefClampMaxInter8, 16320);
	ASSERT_EQ(64 * 255, kCoefClampMaxInter8);
	EXPECT_EQ(BlockQfMax(1u, false), 8159);
	EXPECT_EQ(DequantizeValue(8159, 1u), 16319);
	EXPECT_LT(DequantizeValue(BlockQfMax(1u, false), 1u), kCoefClampMaxInter8);

	// 电平集恒奇：q 奇时 (2n+1)q 是奇×奇；q 偶时 (2n+1)q − 1 也是奇数。
	// ⇒ 8192 / 16320 这两个偶数界只能由夹取给出，量化自己到不了。
	int odd_cases = 0;
	for (uint8_t q = 1u; q <= 127u; ++q)
	{
		for (int32_t n = 1; n <= 63; ++n)
		{
			const int32_t v = DequantizeValue(n, q);
			ASSERT_NE(v & 1, 0) << "q = " << static_cast<int>(q) << " n = " << n;
			++odd_cases;
		}
	}
	std::printf("电平集奇偶性：%d 组 (q, n) 全为奇数反分段值\n", odd_cases);
	EXPECT_EQ(odd_cases, 127 * 63);
}

/* 三 · 无损档占的是自己那条 15 bit 载体：界 = 架构界，且帧内 / 帧间各七档有损位宽一位没动。
 * 「有损字节流零变动」在位宽表这一层的形态就是：同一 q、同一块类型，bits 与 qf_max 逐档等于原值。 */
TEST(TransformLossless, LosslessRowCarriesFullAmplitudeAndLossyRowsAreUntouched)
{
	// 无损档（q = 0）：1 bit 符号 + 14 bit 幅值 = 15 bit，每块 64 × 15 ÷ 8 = 120 B
	EXPECT_EQ(BlockBits(0u, true), 15);
	EXPECT_EQ(BlockBits(0u, false), 15);
	EXPECT_EQ(BlockBytes(0u, true), 64u * 15u / 8u);
	EXPECT_EQ(BlockBytes(0u, false), 64u * 15u / 8u);
	// 载体界取架构界本身，与反量化第二级饱和域同值 ⇒ 无损系数既不夹取，也不会被载体判越界
	EXPECT_EQ(BlockQfMax(0u, true), kCoefClampMaxIntra8);
	EXPECT_EQ(BlockQfMax(0u, false), kCoefClampMaxInter8);

	// 满幅系数在无损档往返恒等：量化与反量化都不动它（两个符号各测一次）
	for (const int32_t v:
		 { kCoefClampMaxIntra8, -kCoefClampMaxIntra8, kCoefClampMaxInter8, -kCoefClampMaxInter8, 1, -1 })
	{
		EXPECT_EQ(QuantizeValue(v, 0u), v);
		EXPECT_EQ(DequantizeValue(v, 0u), v);
	}

	static const uint8_t kIntraStart[7] = { 1u, 3u, 5u, 9u, 17u, 33u, 65u };
	static const uint8_t kIntraBits[7] = { 13u, 12u, 11u, 10u, 9u, 8u, 7u };
	static const int32_t kIntraMax[7] = { 4095, 1365, 819, 455, 240, 124, 63 };
	static const uint8_t kInterStart[7] = { 1u, 2u, 4u, 8u, 16u, 32u, 64u };
	static const uint8_t kInterBits[7] = { 14u, 13u, 12u, 11u, 10u, 9u, 8u };
	static const int32_t kInterMax[7] = { 8159, 4080, 2040, 1020, 510, 255, 127 };
	for (int i = 0; i < 7; ++i)
	{
		EXPECT_EQ(BlockBits(kIntraStart[i], true), kIntraBits[i]) << "帧内档起点 " << static_cast<int>(kIntraStart[i]);
		EXPECT_EQ(BlockQfMax(kIntraStart[i], true), kIntraMax[i]) << "帧内档起点 " << static_cast<int>(kIntraStart[i]);
		EXPECT_EQ(BlockBits(kInterStart[i], false), kInterBits[i]) << "帧间档起点 " << static_cast<int>(kInterStart[i]);
		EXPECT_EQ(BlockQfMax(kInterStart[i], false), kInterMax[i]) << "帧间档起点 " << static_cast<int>(kInterStart[i]);
	}
	// 取档落在档位内部时不重置：档内任意 q 沿用该档位宽
	EXPECT_EQ(BlockBits(2u, true), 13);
	EXPECT_EQ(BlockBits(4u, true), 12);
	EXPECT_EQ(BlockBits(127u, true), 7);
	EXPECT_EQ(BlockBits(127u, false), 8);

	std::printf("无损载体档：bits=15，帧内界 %d / 帧间界 %d，每块 %u B；有损七档逐档核对未动\n", kCoefClampMaxIntra8,
				kCoefClampMaxInter8, BlockBytes(0u, true));
}

/* 四 · 满幅系数真的走得住：全 0 像素块的帧内残差恒 −128 ⇒ DC = 64 × (−128) = −8192，
 * 恰好等于无损载体的界。有损最细档的界只有 4095，装不下它 —— 无损要自占一档的算式就在这一个块上。 */
TEST(TransformLossless, FullAmplitudeIntraBlockHitsExactlyTheLosslessCarrierBound)
{
	int32_t x[kBlockCoefs];
	for (int i = 0; i < kBlockCoefs; ++i) x[i] = -128;	  // 像素 0 − 预测电平 128
	int32_t t[kBlockCoefs];
	Hadamard8(x, t);
	EXPECT_EQ(t[0], 64 * -128);
	EXPECT_EQ(t[0], -kCoefClampMaxIntra8);
	for (int i = 1; i < kBlockCoefs; ++i) EXPECT_EQ(t[i], 0);	 // 常数块的 AC 全为零

	// 无损档往返恒等：不进量化式，也不受夹取
	EXPECT_EQ(QuantizeValue(t[0], 0u), t[0]);
	EXPECT_EQ(DequantizeValue(t[0], 0u), t[0]);
	// 有损最细档的窗口容纳不了它，这正是「无损不是把档取到最细」的算术根据
	EXPECT_GT(static_cast<int32_t>(-t[0]), BlockQfMax(1u, true));
	std::printf("全 0 块：DC = %d；无损档界 %d（走得通），有损最细档界 %d（差 %d）\n", t[0], BlockQfMax(0u, true),
				BlockQfMax(1u, true), BlockQfMax(0u, true) - BlockQfMax(1u, true));
}
