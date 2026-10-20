/* test_video_direct_scale.cpp
 *
 * **实现层用例**：直接 include 实现层头，不经公开 C ABI，**仅静态构建（`CLV_BUILD_SHARED=OFF`）下可链**。
 *
 * direct 模式缩放的 golden 表与四类钉。
 * 口径：MVF = 向零截断(TRB × MV_ref / TRD)，MVB = MVF − MV_ref。
 * 「向零截断」的语义与库内 `direct_scale.h` 的除法约定同源（整数除法、结果向零截断，两侧不各写一份）。
 * 表值按定义式 MVF = 向零截断(TRB × MV_ref ÷ TRD) 算出，与本文件的字面量逐行回比对，两边不一致即报行。
 */

#include "video/direct_scale.h"
#include "video/macroblock.h"
#include "video/status.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>

namespace
{

	using clv::video::DirectScale;
	using clv::video::DirectScaled;
	using clv::video::MotionVector;
	using clv::video::VideoErr;

	struct Golden
	{
		int64_t trb;
		int64_t trd;
		int32_t mv_ref;
		int32_t mvf;
		int32_t mvb;
	};

	/* 金表两列的取值：TRB ∈ {1, 2}（各 11 行）、TRD 恒 3 —— 两列之间没有倍数关系，
	 * 按「TRD = 3 × TRB」读就错了（TRB = 2 那半张表里 TRD 仍是 3）。
	 * 每半张表逐行覆盖 MV_ref ∈ {0, ±1, ±2, ±5, ±2047, ±2048}（11 个取值）。 */
	const Golden kGolden[] = {
		{ 1, 3, 0,	   0,	  0		}, // 1×0/3 = 0 → 0
		{ 1, 3, 1,	   0,	  -1	}, // 1×1/3 = 0.33 → 向零 0
		{ 1, 3, -1,	   0,	  1		},
		   { 1, 3, 2,	  0,	 -2	   }, // 1×2/3 = 0.67 → 向零 0（四舍五入会得 1）
		{ 1, 3, -2,	   0,	  2		},
		   { 1, 3, 5,	  1,	 -4	   }, // 1×5/3 = 1.67 → 1
		{ 1, 3, -5,	   -1,	  4		},
		{ 1, 3, 2047,  682,	  -1365 }, // 2047/3 = 682.33
		{ 1, 3, -2047, -682,  1365	},
		{ 1, 3, 2048,  682,	  -1366 }, // 2048/3 = 682.67 → 仍 682
		{ 1, 3, -2048, -682,  1366	},
		{ 2, 3, 0,	   0,	  0		},
		  { 2, 3, 1,	 0,		-1	  }, // 2×1/3 = 0.67 → 0
		{ 2, 3, -1,	   0,	  1		},
		   { 2, 3, 2,	  1,	 -1	   }, // 2×2/3 = 1.33 → 1
		{ 2, 3, -2,	   -1,	  1		},
		{ 2, 3, 5,	   3,	  -2	}, // 10/3 = 3.33
		{ 2, 3, -5,	   -3,	  2		},
		{ 2, 3, 2047,  1364,  -683	}, // 4094/3 = 1364.67
		{ 2, 3, -2047, -1364, 683	},
		{ 2, 3, 2048,  1365,  -683	}, // 4096/3 = 1365.33
		{ 2, 3, -2048, -1365, 683	},
	};

	MotionVector Mv(int32_t x)
	{
		MotionVector m;
		m.h = x;
		m.v = x;
		return m;
	}

	bool IsDegenerate(const Golden& g)
	{
		return g.mv_ref == 0 || g.trb == g.trd;	   // 零矢量与比例 1 不判方向
	}

	/* 取符号：负 −1 / 零 0 / 正 +1。同号判据用它，两侧都不取自被测输出。 */
	int SignOf(int32_t v) noexcept { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

}	 // namespace

TEST(VideoDirectScale, GoldenTableBothComponents)
{
	size_t points = 0u;
	for (const Golden& g: kGolden)
	{
		DirectScaled r;
		ASSERT_EQ(DirectScale(Mv(g.mv_ref), g.trb, g.trd, &r), VideoErr::kOk)
			<< "TRB=" << g.trb << " TRD=" << g.trd << " MV=" << g.mv_ref;
		// x / y 两维同值入参，两维各自断言
		EXPECT_EQ(r.forward.h, g.mvf) << "x 维 TRB=" << g.trb << " TRD=" << g.trd << " MV=" << g.mv_ref;
		EXPECT_EQ(r.forward.v, g.mvf) << "y 维 TRB=" << g.trb << " TRD=" << g.trd << " MV=" << g.mv_ref;
		EXPECT_EQ(r.backward.h, g.mvb) << "x 维反向 TRB=" << g.trb << " TRD=" << g.trd << " MV=" << g.mv_ref;
		EXPECT_EQ(r.backward.v, g.mvb) << "y 维反向 TRB=" << g.trb << " TRD=" << g.trd << " MV=" << g.mv_ref;
		points += 4u;
	}
	std::cout << "golden 主表 = " << std::size(kGolden) << " 行 × x / y 两维 × 正反两矢量 = " << points
			  << " 个断言点\n";
}

TEST(VideoDirectScale, TruncationDirectionIsThePin)
{
	// 口径钉：1×2/3 = 0.67，向零截断取 0；换成四舍五入或向下取整，主表里 ±2 与 ±2048 那几行全红
	DirectScaled pos, neg;
	ASSERT_EQ(DirectScale(Mv(2), 1, 3, &pos), VideoErr::kOk);
	ASSERT_EQ(DirectScale(Mv(-2), 1, 3, &neg), VideoErr::kOk);
	EXPECT_EQ(pos.forward.h, 0);
	EXPECT_EQ(pos.backward.h, -2);
	EXPECT_EQ(neg.forward.h, 0);
	EXPECT_EQ(neg.backward.h, 2);

	// 同一条式子在 2048 上再钉一次：682.67 → 682，不是 683
	DirectScaled big;
	ASSERT_EQ(DirectScale(Mv(2048), 1, 3, &big), VideoErr::kOk);
	EXPECT_EQ(big.forward.h, 682);
}

TEST(VideoDirectScale, BothDirectionCasesAreLegalAndDistinguishable)
{
	// 方向钉：TRD = 4 下 TRB = 1 与 TRB = 3 都是合法入口，结果必须分得开；公式方向算反必红
	DirectScaled early, late;
	ASSERT_EQ(DirectScale(Mv(4), 1, 4, &early), VideoErr::kOk);
	ASSERT_EQ(DirectScale(Mv(4), 3, 4, &late), VideoErr::kOk);
	EXPECT_EQ(early.forward.h, 1);
	EXPECT_EQ(early.backward.h, -3);
	EXPECT_EQ(late.forward.h, 3);
	EXPECT_EQ(late.backward.h, -1);
	std::cout << "方向钉：TRB=1 → MVF=1 MVB=-3 ; TRB=3 → MVF=3 MVB=-1\n";
}

TEST(VideoDirectScale, CollocatedFallbackIsZeroVector)
{
	// 回退钉：同位块是 intra / skip（无 MV）/ 缺位时调用方传零矢量，两向都得是零
	for (int64_t trb: { 1, 2 })
	{
		DirectScaled r;
		ASSERT_EQ(DirectScale(Mv(0), trb, 3, &r), VideoErr::kOk);
		EXPECT_EQ(r.forward.h, 0);
		EXPECT_EQ(r.forward.v, 0);
		EXPECT_EQ(r.backward.h, 0);
		EXPECT_EQ(r.backward.v, 0);
	}
}

TEST(VideoDirectScale, ProductNeedsSixtyFourBits)
{
	// 溢出钉：(10^7 − 1) × 2048 = 20 479 997 952，越 32 位有符号上限；32 位中间量必错
	DirectScaled r;
	ASSERT_EQ(DirectScale(Mv(2048), 10000000 - 1, 10000000, &r), VideoErr::kOk);
	EXPECT_EQ(r.forward.h, 2047);
	EXPECT_EQ(r.backward.h, -1);
	std::cout << "溢出钉：TRB=10^7-1, TRD=10^7, MV_ref=2048 → MVF=2047 MVB=-1（乘积 20479997952 越 32 位）\n";
}

TEST(VideoDirectScale, TrbAboveTrdIsRejectedAtEntry)
{
	// 方向越界钉：TRB = 4 > TRD = 1 在入口就判参数不成立；
	// 若实现不做检查而算下去，会得到 MVF = 16 / MVB = 12 —— 拿到那两个值就是漏判
	DirectScaled r;
	r.forward = Mv(999);
	r.backward = Mv(999);
	EXPECT_EQ(DirectScale(Mv(4), 4, 1, &r), VideoErr::kValueRange);
	EXPECT_EQ(r.forward.h, 999);	// 判拒时不写出参

	EXPECT_EQ(DirectScale(Mv(4), 5, 4, &r), VideoErr::kValueRange);
	EXPECT_EQ(DirectScale(Mv(4), 0, 0, &r), VideoErr::kValueRange);	   // 除零与非正时间基
	EXPECT_EQ(DirectScale(Mv(4), 1, -3, &r), VideoErr::kValueRange);
	EXPECT_EQ(DirectScale(Mv(4), 1, 3, nullptr), VideoErr::kInvalidArgument);
}

TEST(VideoDirectScale, InvariantsHoldForEveryNonDegenerateRow)
{
	// 一致性不变量：期望全部取自冻结表 kGolden 的字面量，不取被测函数的另一路输出
	size_t asserted = 0u;
	for (const Golden& g: kGolden)
	{
		// 表自身的恒等式先核一遍：MVB = MVF − MV_ref（作者写错的行在这里就红，不会拖到下游）
		ASSERT_EQ(g.mvb, g.mvf - g.mv_ref) << "表行 TRB=" << g.trb << " TRD=" << g.trd << " MV=" << g.mv_ref;

		DirectScaled r;
		ASSERT_EQ(DirectScale(Mv(g.mv_ref), g.trb, g.trd, &r), VideoErr::kOk);
		// 反向量对表：r.backward 必须等于表里那一行，而不是「r.forward − MV_ref」这种现算期望
		EXPECT_EQ(r.backward.h, g.mvb) << "反向 x TRB=" << g.trb << " MV=" << g.mv_ref;
		EXPECT_EQ(r.backward.v, g.mvb) << "反向 y TRB=" << g.trb << " MV=" << g.mv_ref;
		EXPECT_EQ(r.forward.h, g.mvf) << "正向 x TRB=" << g.trb << " MV=" << g.mv_ref;
		if (IsDegenerate(g)) continue;
		EXPECT_LE(std::abs(r.forward.h), std::abs(g.mv_ref)) << "TRB=" << g.trb << " MV=" << g.mv_ref;
		// 同号钉：缩放后的矢量不得翻符号（左边是 被测输出 × 表字面量，不是两个被测输出互证）
		EXPECT_GE(static_cast<int64_t>(r.forward.h) * g.mv_ref, 0)
			<< "缩放后应与同位块矢量同号：TRB=" << g.trb << " MV=" << g.mv_ref;
		// 并且必须与表里那一行的 MVF 同号：表给 0 的行只许解出 0，表给 ±的行不许漂成 0
		if (g.mvf == 0)
			EXPECT_EQ(r.forward.h, 0) << "表行 MVF = 0，实现不得给出非零：TRB=" << g.trb << " MV=" << g.mv_ref;
		else EXPECT_EQ(SignOf(r.forward.h), SignOf(g.mvf)) << "TRB=" << g.trb << " MV=" << g.mv_ref;
		++asserted;
	}
	// 非退化行数：22 行里去掉 mv_ref = 0 的 2 行与 trb = trd 的 0 行 → 20 行进不变量断言
	EXPECT_EQ(asserted, 20u);
	std::cout << "一致性断言覆盖非退化行 " << asserted << " 行（期望取表字面量）\n";
}

TEST(VideoDirectScale, RatioOneIsLegalAndYieldsZeroBackward)
{
	// TRB = TRD（比例 1）不在入口判拒之列；帧级跳帧由 0 < time_pb < time_pp 单独约束
	DirectScaled r;
	ASSERT_EQ(DirectScale(Mv(-7), 5, 5, &r), VideoErr::kOk);
	EXPECT_EQ(r.forward.h, -7);
	EXPECT_EQ(r.backward.h, 0);
}
