/* test_video_quant_dpb.cpp
 *
 * **实现层用例**：直接 include 实现层头，不经公开 C ABI，**仅静态构建（`CLV_BUILD_SHARED=OFF`）下可链**。
 *
 * 变换 / 量化 / 反量化的 C++ 侧对拍，与参考帧管理的输出序用例。
 *
 * 对拍素材由 `scripts/gen_video_quant_fixtures.py` 现算生成，期望值来自一份**独立实现的
 * 量化器参考件**——
 * C++ 侧必须逐样本复现正变换、最近电平量化与第二法反分段（电平集 (2n+1)q − δ）的结果。
 * 输出序断言用的是参考帧管理那节给出的工作样例（max_b_frames = 2，阈值 4）。
 */

#include "video/dpb.h"
#include "video/quant.h"
#include "video_payload_fixtures_p.h"
#include "video_quant_fixtures_p.h"

#include "gtest/gtest.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace
{

	using clv::video::OutputItem;
	using clv::video::Scheduler;

	constexpr int kN = clvtest::kQuantQCount;

	/* 持有只有一份账、记在帧对象上（`video/dpb.h` 文件头），参考帧管理类不自建第二张按槽号的表。
	 * 这里实现的 `FrameHolders` 把 `Scheduler` 那只手加减的那一份记账：它只管参考窗那一次持有，
	 * 架上（待输出队列）那一次由门面在建帧时加上、不经本类。
	 * 所以「同时在场的帧数」= 参考窗里还挂着持有的槽号 ∪ 仍在待输出队列里的槽号；
	 * 内存峰值按这个并集钉，单看本账钉的是「谁进了参考窗、哪一步放掉」。 */
	class HoldLedger: public clv::video::FrameHolders
	{
		public:

		void Acquire(int id) override { Entry(id).second += 1u; }

		void Release(int id) override
		{
			const size_t r = Refs(id);
			Entry(id).second = (r == 0u) ? 0u : r - 1u;
		}

		/* 该槽号在参考窗里还挂着几次持有；从没登记过的槽号一律 0。 */
		size_t Refs(int id) const
		{
			for (const auto& e: held_)
				if (e.first == id) return e.second;
			return 0u;
		}

		bool Held(int id) const { return Refs(id) != 0u; }

		/* 还挂着持有的槽号个数（不是持有次数）。 */
		size_t HeldSlots() const
		{
			size_t n = 0u;
			for (const auto& e: held_)
				if (e.second != 0u) ++n;
			return n;
		}

		private:

		std::pair<int, size_t>& Entry(int id)
		{
			for (auto& e: held_)
				if (e.first == id) return e;
			held_.push_back(std::make_pair(id, 0u));
			return held_.back();
		}

		std::vector<std::pair<int, size_t>> held_;
	};

	int32_t max_abs_diff(const int32_t* a, const int32_t* b)
	{
		int32_t worst = 0;
		for (int i = 0; i < clv::video::kBlockCoefs; ++i)
		{
			const int64_t d = std::llabs(static_cast<int64_t>(a[i]) - static_cast<int64_t>(b[i]));
			if (d > worst) worst = static_cast<int32_t>(d);
		}
		return worst;
	}

}	 // namespace

TEST(VideoQuant, HadamardTransformMatchesReference)
{
	int32_t t[clv::video::kBlockCoefs];
	clv::video::Hadamard8(clvtest::kBlock0Pixels, t);
	EXPECT_EQ(max_abs_diff(t, clvtest::kBlock0Coefs), 0) << "块 0 的正变换与参考不一致";
	clv::video::Hadamard8(clvtest::kBlock1Pixels, t);
	EXPECT_EQ(max_abs_diff(t, clvtest::kBlock1Coefs), 0) << "块 1 的正变换与参考不一致";
}

TEST(VideoQuant, QuantizeAndDequantizeMatchReference)
{
	int32_t qf[clv::video::kBlockCoefs];
	int32_t f2[clv::video::kBlockCoefs];

	for (int qi = 0; qi < kN; ++qi)
	{
		const uint8_t q = clvtest::kQuantQ[qi];
		clv::video::QuantizeBlock(clvtest::kBlock0Coefs, q, qf);
		clv::video::DequantizeBlock(qf, q, f2);
		if (qi == 0)
		{
			EXPECT_EQ(max_abs_diff(qf, clvtest::kBlock0Qf0), 0);
			EXPECT_EQ(max_abs_diff(f2, clvtest::kBlock0F20), 0);
		}
		else if (qi == 1)
		{
			EXPECT_EQ(max_abs_diff(qf, clvtest::kBlock0Qf1), 0);
			EXPECT_EQ(max_abs_diff(f2, clvtest::kBlock0F21), 0);
		}
		else
		{
			EXPECT_EQ(max_abs_diff(qf, clvtest::kBlock0Qf2), 0);
			EXPECT_EQ(max_abs_diff(f2, clvtest::kBlock0F22), 0);
		}
	}

	clv::video::QuantizeBlock(clvtest::kBlock1Coefs, clvtest::kQuantQ[0], qf);
	clv::video::DequantizeBlock(qf, clvtest::kQuantQ[0], f2);
	EXPECT_EQ(max_abs_diff(qf, clvtest::kBlock1Qf0), 0);
	EXPECT_EQ(max_abs_diff(f2, clvtest::kBlock1F20), 0);
}

TEST(VideoQuant, CoefficientErrorMatchesFixtureMaxErr)
{
	int32_t qf[clv::video::kBlockCoefs];
	int32_t f2[clv::video::kBlockCoefs];
	int32_t worst = 0;
	for (int qi = 0; qi < kN; ++qi)
	{
		const uint8_t q = clvtest::kQuantQ[qi];
		clv::video::QuantizeBlock(clvtest::kBlock0Coefs, q, qf);
		clv::video::DequantizeBlock(qf, q, f2);
		worst = 0;
		EXPECT_TRUE(clv::video::CoefficientBoundOk(clvtest::kBlock0Coefs, f2, q, &worst)) << "q = " << q;
		if (qi == 0) EXPECT_EQ(worst, clvtest::kBlock0MaxErr0);
		if (qi == 1) EXPECT_EQ(worst, clvtest::kBlock0MaxErr1);
		if (qi == 2) EXPECT_EQ(worst, clvtest::kBlock0MaxErr2);
	}
}

TEST(VideoQuant, ErrorBoundHoldsForEveryLegalStep)
{
	// q = 1..127 逐值：两块输入的系数域与像素域都不得越界
	int32_t qf[clv::video::kBlockCoefs];
	int32_t f2[clv::video::kBlockCoefs];
	double y_src[clv::video::kBlockCoefs];
	double y_rec[clv::video::kBlockCoefs];
	double diff[clv::video::kBlockCoefs];
	int violations = 0;
	int32_t worst_ratio_num = 0;
	int32_t worst_ratio_den = 1;
	/* 逐块各记一档：生成件冻的「最紧一档」是按块分别扫出来的，合在一起记就对不上那两行。
	 * 取档谓词与生成件同式（2·err × 已记档的分母 > 已记档的分子 × 本档界，严格大于 ⇒ 平局留先出现的档）。 */
	int32_t block_worst_num[2] = { 0, 0 };
	int32_t block_worst_den[2] = { 1, 1 };
	int32_t block_worst_q[2] = { 0, 0 };

	for (uint8_t q = 1u; q <= 127u; ++q)
	{
		const int32_t* const kBlocks[] = { clvtest::kBlock0Coefs, clvtest::kBlock1Coefs };
		for (size_t b = 0; b < 2u; ++b)
		{
			const int32_t* block = kBlocks[b];
			clv::video::QuantizeBlock(block, q, qf);
			clv::video::DequantizeBlock(qf, q, f2);
			int32_t max_err = 0;
			if (! clv::video::CoefficientBoundOk(block, f2, q, &max_err)) ++violations;
			clv::video::InverseHadamard8(block, y_src);
			clv::video::InverseHadamard8(f2, y_rec);
			for (int i = 0; i < clv::video::kBlockCoefs; ++i) diff[i] = y_src[i] - y_rec[i];
			double pix_max = 0.0;
			if (! clv::video::PixelBoundOk(diff, q, &pix_max)) ++violations;
			// 记录最紧的比值（整数量化：2·err 与 3q−δ）
			const int32_t delta = (q & 1u) ? 0 : 1;
			const int32_t bound2 = 3 * static_cast<int32_t>(q) - delta;
			if (2 * max_err * block_worst_den[b] > block_worst_num[b] * bound2)
			{
				block_worst_num[b] = 2 * max_err;
				block_worst_den[b] = bound2;
				block_worst_q[b] = static_cast<int32_t>(q);
			}
			if (2 * max_err * worst_ratio_den > worst_ratio_num * bound2)
			{
				worst_ratio_num = 2 * max_err;
				worst_ratio_den = bound2;
			}
		}
	}
	EXPECT_EQ(violations, 0);
	// 最紧一档必须贴界但不越界
	EXPECT_LE(worst_ratio_num, worst_ratio_den);
	/* 外部锚：生成件按 video_quant_fixtures_p.h 冻好的两份系数独立扫掠 q' = 1..127，
	 * 把每块最紧一档的三个数冻进 video_payload_fixtures_p.h。上面只断「Num ≤ Den」，
	 * 最紧那一档贴没贴界（380 / 381 之间那一格）没人核 ⇒ 这里逐格等值钉死。 */
	EXPECT_EQ(block_worst_num[0], static_cast<int32_t>(clvtest::kSweepWorstBlock0Num)) << "块 0 最紧一档的 2·max|err|";
	EXPECT_EQ(block_worst_den[0], static_cast<int32_t>(clvtest::kSweepWorstBlock0Den)) << "块 0 同档的界 3q' − δ";
	EXPECT_EQ(block_worst_q[0], static_cast<int32_t>(clvtest::kSweepWorstBlock0Q)) << "块 0 取到最紧一档的 q'";
	EXPECT_EQ(block_worst_num[1], static_cast<int32_t>(clvtest::kSweepWorstBlock1Num)) << "块 1 最紧一档的 2·max|err|";
	EXPECT_EQ(block_worst_den[1], static_cast<int32_t>(clvtest::kSweepWorstBlock1Den)) << "块 1 同档的界 3q' − δ";
	EXPECT_EQ(block_worst_q[1], static_cast<int32_t>(clvtest::kSweepWorstBlock1Q)) << "块 1 取到最紧一档的 q'";
	// 冻进来的最紧一档自身也得是「贴界不越界」的那一档，否则锚本身漂了
	EXPECT_EQ(clvtest::kSweepWorstBlock0Den,
			  3u * clvtest::kSweepWorstBlock0Q - (clvtest::kSweepWorstBlock0Q & 1u ? 0u : 1u));
	EXPECT_LT(clvtest::kSweepWorstBlock0Num, clvtest::kSweepWorstBlock0Den);
	EXPECT_EQ(clvtest::kSweepWorstBlock1Den,
			  3u * clvtest::kSweepWorstBlock1Q - (clvtest::kSweepWorstBlock1Q & 1u ? 0u : 1u));
	EXPECT_LT(clvtest::kSweepWorstBlock1Num, clvtest::kSweepWorstBlock1Den);
}

TEST(VideoQuant, LevelSetHasNoPlusMinusQStep)
{
	// 电平集 = {0} ∪ {±((2n+1)q − δ)}：没有 ±q 这一级 → 死区
	for (uint8_t q = 1u; q <= 127u; ++q)
	{
		const int32_t delta = (q & 1u) ? 0 : 1;
		const int32_t first = 3 * static_cast<int32_t>(q) - delta;	  // n = 1 的电平
		// ±q 级不存在：幅度为 q 的系数必须落到 0
		EXPECT_EQ(clv::video::QuantizeValue(static_cast<int32_t>(q), q), 0) << "q = " << q;
		EXPECT_EQ(clv::video::QuantizeValue(-static_cast<int32_t>(q), q), 0) << "q = " << q;
		// 首电平原样回来
		EXPECT_EQ(clv::video::QuantizeValue(first, q), 1) << "q = " << q;
		EXPECT_EQ(clv::video::QuantizeValue(-first, q), -1) << "q = " << q;
		EXPECT_EQ(clv::video::DequantizeValue(1, q), first);
		EXPECT_EQ(clv::video::DequantizeValue(-1, q), -first);
		// 相邻电平间隔恒为 2q
		for (int32_t n = 1; n < 5; ++n)
		{
			const int32_t a = clv::video::DequantizeValue(n, q);
			const int32_t b = clv::video::DequantizeValue(n + 1, q);
			EXPECT_EQ(b - a, 2 * static_cast<int32_t>(q)) << "q = " << q << " n = " << n;
		}
	}
}

TEST(VideoDpb, ReproducesTheWorkedExample)
{
	// 解码序（DTS 升序）与显示序（PTS）：I(0) P(3) B(1) B(2) P(6) B(4) B(5)，max_b_frames = 2
	struct Dec
	{
		uint8_t type;
		int id;
		int64_t pts;
	};

	static const Dec kSeq[] = {
		{ 0u, 0, 0 },
		 { 1u, 1, 3 },
		  { 2u, 2, 1 },
		   { 2u, 3, 2 },
		{ 1u, 4, 6 },
		 { 2u, 5, 4 },
		  { 2u, 6, 5 },
	};
	static const int kExpectedEmit[] = { 0, 2, 3, 1, 5, 6, 4 };	   // I0 B1 B2 P3 B4 B5 P6

	HoldLedger ledger;
	Scheduler sched(2u, 0u, &ledger);	 // 起池深度 0 = 未声明 ⇒ 阈值取重排地板
	std::vector<int> emitted;
	size_t peak_live = 0u;
	size_t peak_ref_window = 0u;
	size_t peak_dpb = 0u;

	for (size_t step = 0; step < 7u; ++step)
	{
		const Dec& d = kSeq[step];
		const auto out = sched.PushDecoded(d.id, d.type, d.pts);
		for (const OutputItem& o: out) emitted.push_back(o.id);
		// 同时在场的帧 = 参考窗里还挂着持有的 ∪ 仍在待输出队列里的；架上那一次持有由门面加，不经本账
		size_t alive = 0u;
		for (const Dec& s: kSeq)
			if (ledger.Held(s.id) || sched.InQueue(s.id)) ++alive;
		peak_live = std::max(peak_live, alive);
		peak_ref_window = std::max(peak_ref_window, ledger.HeldSlots());
		peak_dpb = std::max(peak_dpb, sched.DpbCount());
		ASSERT_LE(sched.DpbCount(), 2u) << "step " << step;	   // 参考缓冲容量恒为 2
	}
	const auto tail = sched.Flush();
	for (const OutputItem& o: tail) emitted.push_back(o.id);

	ASSERT_EQ(emitted.size(), 7u);
	for (size_t i = 0; i < 7u; ++i)
	{
		ASSERT_LT(i, sizeof(kExpectedEmit) / sizeof(kExpectedEmit[0]));
		EXPECT_EQ(emitted[i], kExpectedEmit[i]) << "第 " << i << " 个出帧";
	}
	EXPECT_EQ(peak_live, 4u);		   // 样例的内存峰值 = 4（参考窗持有 ∪ 待输出队列的并集帧数）
	EXPECT_EQ(peak_ref_window, 2u);	   // 参考窗那一份账的峰值 = 容量 2，与 max_b_frames 无关
	EXPECT_EQ(peak_dpb, 2u);
	size_t alive_after_flush = 0u;
	for (const Dec& s: kSeq)
		if (ledger.Held(s.id) || sched.InQueue(s.id)) ++alive_after_flush;
	EXPECT_EQ(alive_after_flush, 0u);	  // 两个队列都不再持有才算回收
	EXPECT_EQ(ledger.HeldSlots(), 0u);	  // 冲刷把参考窗那一次全部放掉
}

TEST(VideoDpb, PopIsNotEvictionAndBNeverEntersReference)
{
	HoldLedger ledger;
	Scheduler sched(2u, 0u, &ledger);	 // 起池深度 0 = 未声明 ⇒ 阈值取重排地板
	sched.PushDecoded(10, 0u, 0);		 // I，PTS 0
	sched.PushDecoded(11, 1u, 3);		 // P，PTS 3
	sched.PushDecoded(12, 2u, 1);		 // B，PTS 1
	const size_t dpb_before = sched.DpbCount();
	sched.PushDecoded(13, 2u, 2);	 // B，PTS 2 → 队列到阈值，弹出 PTS 最小的 I
	const auto after = sched.PushDecoded(14, 1u, 6);

	// B 帧不入参考缓冲
	EXPECT_EQ(sched.DpbCount(), dpb_before) << "两个 B 帧之后参考缓冲不该变长";
	EXPECT_FALSE(sched.InDpb(12));
	EXPECT_FALSE(sched.InDpb(13));
	// 弹出按显示序（PTS 最小），与 FIFO 驱逐是两回事：这一步弹出的是 B1（PTS 1）
	ASSERT_EQ(after.size(), 1u);
	EXPECT_EQ(after[0].id, 12);
	// I(PTS 0) 先前已出队，此刻又被新 P 帧按解码序最旧驱逐，两个队列都不再持有 → 回收
	EXPECT_FALSE(sched.InQueue(10));
	EXPECT_FALSE(sched.InDpb(10));
	EXPECT_EQ(ledger.Refs(10), 0u);
	// 刚进场的 P 帧同时在两个队列里：本账记的只是它进参考窗那一次持有
	// （架上那一次由门面在建帧时加、不落在这张表上，「两个队列都在」由上面两行 InDpb / InQueue 钉）
	EXPECT_TRUE(sched.InDpb(14));
	EXPECT_TRUE(sched.InQueue(14));
	EXPECT_EQ(ledger.Refs(14), 1u);
}

TEST(VideoDpb, EvictionIsFifoByDecodeOrder)
{
	HoldLedger ledger;
	Scheduler sched(0u, 0u, &ledger);	 // 阈值 2，出帧很快，但参考缓冲仍要按解码序驱逐
	sched.PushDecoded(1, 0u, 0);		 // I0
	sched.PushDecoded(2, 1u, 1);		 // P1
	sched.PushDecoded(3, 1u, 2);		 // P2 → 满，驱逐解码序最旧的 I0
	EXPECT_FALSE(sched.InDpb(1));
	EXPECT_TRUE(sched.InDpb(2));
	EXPECT_TRUE(sched.InDpb(3));
	EXPECT_EQ(sched.DpbCount(), 2u);
	// 驱逐这一步就是放掉被驱逐帧的参考窗持有，进场的两帧各挂一次
	EXPECT_EQ(ledger.Refs(1), 0u);
	EXPECT_EQ(ledger.Refs(2), 1u);
	EXPECT_EQ(ledger.Refs(3), 1u);
}

TEST(VideoDpb, ZeroBStructureEmitsSecondFrameImmediately)
{
	HoldLedger ledger;
	Scheduler sched(0u, 0u, &ledger);
	// max_b_frames = 0 时阈值 = 2：首帧在第二帧解码后即出
	EXPECT_TRUE(sched.PushDecoded(1, 0u, 0).empty());
	const auto out = sched.PushDecoded(2, 1u, 1);
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0].id, 1);
	const auto tail = sched.Flush();
	ASSERT_EQ(tail.size(), 1u);
	EXPECT_EQ(tail[0].id, 2);
	EXPECT_EQ(ledger.HeldSlots(), 0u);	  // 两个队列都不再持有才算回收
}

TEST(VideoDpb, ResetClearsBothQueues)
{
	HoldLedger ledger;
	Scheduler sched(2u, 0u, &ledger);	 // 起池深度 0 = 未声明 ⇒ 阈值取重排地板
	sched.PushDecoded(1, 0u, 0);
	sched.PushDecoded(2, 1u, 3);
	sched.PushDecoded(3, 2u, 1);
	EXPECT_GT(sched.DpbCount(), 0u);
	EXPECT_GT(sched.QueueCount(), 0u);
	sched.Reset();
	EXPECT_EQ(sched.DpbCount(), 0u);
	EXPECT_EQ(sched.QueueCount(), 0u);
	EXPECT_EQ(ledger.HeldSlots(), 0u);	  // 重置把本类加过的持有全部放掉
}
