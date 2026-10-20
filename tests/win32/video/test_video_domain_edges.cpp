/* test_video_domain_edges.cpp
 *
 * **两面混用**：端到端与句柄行为走公开 C ABI（`CLV_Video*` 族 13 个入口、实测 24 处调用，如 `CLV_VideoBitstreamGetBytes`、`CLV_VideoBitstreamRelease`、`CLV_VideoDecoderClose`），
 *   单元级直测走实现层头（`video/bits.h`、`video/block.h`、`video/encoder_hook.h`、`video/macroblock.h`、`video/payload.h`、`video/quant.h`、`video/reconstruct.h`、`video/status.h`）。**仅静态构建（`CLV_BUILD_SHARED=OFF`）下可链**的原因是后面这些头，
 *   不是「不经公开 ABI」——两面在此按实码各写各的，不写成单向排除。
 *
 * 饱和域四级的**边界图样**补测：帧间系数域与重建域的上下界各钉一次。
 * 这两档的难处在于造图样：帧间系数域要到 ±16320，必须整块残差 = ±255，
 * 而通用合成图样的块均值都靠近中间电平，DC 远够不着窗口上界。本件两条路都走：
 *
 *   一 · 单元面：直接喂 `ReconstructBlock` 一组只动 DC 的载体极值，把三级域的界各钉一次。
 *       极值取的是**载体窗口上界**（`BlockQfMax`），不是随手写的大数：
 *         帧间 `q' = 1` 档 14 bit ⇒ |QF| ≤ 8159 ⇒ 反分段 (2×8159+1)×1 = **16319**
 *         帧间 `q' = 32` 档 9 bit ⇒ |QF| ≤ 255 ⇒ 反分段 (2×255+1)×32 − 1 = **16351** > 16320 ⇒ 触发夹取
 *         帧内 `q' = 127` 档 7 bit ⇒ |QF| ≤ 63 ⇒ 反分段 (2×63+1)×127 = **16129** > 8192 ⇒ 触发夹取
 *       顺带钉一条算术性质：电平集对任何 q 都给出**奇数**反分段值（q 奇 ⇒ 奇×奇；q 偶 ⇒ 奇×偶−1），
 *       所以 ±16320 / ±8192 这两档设计界**只能由夹取产生**，反分段自己到不了（差 1）。
 *
 *   二 · 全链面：真造一张「整块残差 = ±255」的图样 —— 参考帧平一色、当前帧平另一色，
 *       零运动就是最优（平面上任何矢量代价相同，平票取零矢量），于是预测块恒等于参考面，
 *       残差 = 255 − 0 = **+255**（或 0 − 255 = **−255**）。走编码器→码流→解码器，
 *       断载体字节块长、首系数的符号 + 13 bit 幅值确实等于窗口上界、以及重建像素落在像素域端点。
 */

#include "CLV_Video.h"
#include "test_config_expect.h"
#include "video/bits.h"
#include "video/block.h"
#include "video/encoder_hook.h"
#include "video/macroblock.h"
#include "video/payload.h"
#include "video/quant.h"
#include "video/reconstruct.h"
#include "video/status.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{

	using clv::video::BitReader;
	using clv::video::BlockQfMax;
	using clv::video::DequantizeValue;
	using clv::video::kBlockCoefs;
	using clv::video::kCoefClampMaxInter8;
	using clv::video::kCoefClampMaxIntra8;
	using clv::video::kIntraPredLevel8;
	using clv::video::kMbTypeInter;
	using clv::video::kResidualClampMax8;
	using clv::video::kResidualClampMin8;
	using clv::video::kSampleMax8;
	using clv::video::PlaneView;
	using clv::video::ReconstructBlock;
	using clv::video::RoundResidual;
	using clv::video::VideoErr;
	using clv::video::VopPayload;

	/* 只有 DC 非零的一块：帧间 q'=1 档窗口上界 = 8159。 */
	void DcOnly(int32_t qf[kBlockCoefs], int32_t dc)
	{
		std::memset(qf, 0, sizeof(int32_t) * kBlockCoefs);
		qf[0] = dc;
	}

	void ExpectAll(const uint8_t out[kBlockCoefs], uint8_t want, const char* tag)
	{
		size_t bad = 0u;
		for (int i = 0; i < kBlockCoefs; ++i)
			if (out[i] != want) ++bad;
		EXPECT_EQ(bad, 0u) << tag << "：应有 " << kBlockCoefs << " 点全等于 " << static_cast<int>(want);
	}

	PlaneView ViewOf(CLV_VideoFrame* f)
	{
		CLV_VideoFrameDescC d;
		std::memset(&d, 0, sizeof(d));
		CLV_VideoFrameGetDesc(f, &d, nullptr);
		PlaneView v;
		v.width = d.coded_width;
		v.height = d.coded_height;
		for (uint32_t p = 0u; p < 3u; ++p)
		{
			v.p[p] = d.plane[p];
			v.stride[p] = d.stride[p];
		}
		return v;
	}

	void FillFlat(CLV_VideoFrame* f, uint8_t luma, uint8_t chroma)
	{
		const PlaneView v = ViewOf(f);
		const uint32_t pw[3] = { v.width, v.width / 2u, v.width / 2u };
		const uint32_t ph[3] = { v.height, v.height / 2u, v.height / 2u };
		const uint8_t lvl[3] = { luma, chroma, chroma };
		for (uint32_t pl = 0u; pl < 3u; ++pl)
			for (uint32_t r = 0u; r < ph[pl]; ++r)
				std::memset(v.p[pl] + static_cast<size_t>(r) * v.stride[pl], lvl[pl], pw[pl]);
	}

	size_t AbsPlaneSum(const PlaneView& a, const PlaneView& b)
	{
		const uint32_t pw[3] = { a.width, a.width / 2u, a.width / 2u };
		const uint32_t ph[3] = { a.height, a.height / 2u, a.height / 2u };
		size_t sum = 0u;
		for (uint32_t pl = 0u; pl < 3u; ++pl)
		{
			for (uint32_t r = 0u; r < ph[pl]; ++r)
			{
				const uint8_t* pa = a.p[pl] + static_cast<size_t>(r) * a.stride[pl];
				const uint8_t* pb = b.p[pl] + static_cast<size_t>(r) * b.stride[pl];
				for (uint32_t c = 0u; c < pw[pl]; ++c)
				{
					const int d = static_cast<int>(pa[c]) - static_cast<int>(pb[c]);
					sum += static_cast<size_t>(d < 0 ? -d : d);
				}
			}
		}
		return sum;
	}

}	 // namespace

/* ---------- 一 · 单元面：三级域的界各钉一次 ---------- */

/* 帧间窗口上界反分段得 16319，**不触发**夹取；反变换后残差 = round(16319 ÷ 64) = 255；
 * 预测面取全 0 ⇒ 像素落在上端点 255。 */
TEST(DomainEdges, InterQ1DcWindowEdge8159DequantizesTo16319AndPixelsOutAt255)
{
	ASSERT_EQ(kCoefClampMaxInter8, 16320);
	ASSERT_EQ(BlockQfMax(1u, false), 8159);
	ASSERT_EQ(DequantizeValue(8159, 1u), 16319);

	int32_t qf[kBlockCoefs];
	DcOnly(qf, 8159);
	uint8_t pred[kBlockCoefs], out[kBlockCoefs];
	std::memset(pred, 0, sizeof(pred));
	ReconstructBlock(qf, 1u, false, pred, out);
	// 16319 ÷ 64 = 254.984375 → 远离零取整 → 255
	ASSERT_EQ(RoundResidual(16319.0 / 64.0), 255);
	ExpectAll(out, 255, "帧间窗口上界 8159 / q=1 / 预测 0");
	std::cout << "[帧间窗口上界] |QF|=8159 → 反分段 " << DequantizeValue(8159, 1u) << " → 残差 255 → 像素 255\n";
}

/* 帧间 q=32 档窗口上界 255 反分段得 16351，**越过** 16320 ⇒ 夹到 16320；
 * 16320 是偶数，只有夹取能给出这个值（反分段自己全是奇数）。残差 = 16320 ÷ 64 = 255 整。 */
TEST(DomainEdges, InterQ32DcWindowEdge255ClampsTo16320Exactly)
{
	ASSERT_EQ(BlockQfMax(32u, false), 255);
	const int32_t raw = DequantizeValue(255, 32u);
	ASSERT_EQ(raw, 16351);
	ASSERT_GT(raw, kCoefClampMaxInter8);
	// 电平集恒奇：q 偶时 (2n+1)q − 1 也是奇数 ⇒ 16320 这个偶数界只能由夹取产生
	EXPECT_EQ(raw & 1, 1);

	int32_t qf[kBlockCoefs];
	DcOnly(qf, 255);
	uint8_t pred[kBlockCoefs], out[kBlockCoefs];
	std::memset(pred, 0, sizeof(pred));
	ReconstructBlock(qf, 32u, false, pred, out);
	ASSERT_EQ(kResidualClampMax8, 255);
	ExpectAll(out, 255, "帧间 q=32 夹到 16320 / 预测 0");
	std::cout << "[帧间夹取] |QF|=255, q=32 → 反分段 " << raw << " 越界 → 夹 " << kCoefClampMaxInter8
			  << " → 残差 255 → 像素 255\n";
}

/* 帧内 q=127 档窗口上界 63 反分段得 16129，越过 8192 ⇒ 夹到 8192；残差 = 8192 ÷ 64 = 128 整。
 * 这一条钉的是**帧内界靠夹取才碰得到**：帧内变换输入域是像素 − 128，|残差| ≤ 128 ⇒ 合法内容自己
 * 到不了 ±8192 以外的地方，但载体极值能把它顶到夹取上。 */
TEST(DomainEdges, IntraQ127DcWindowEdge63ClampsTo8192Exactly)
{
	ASSERT_EQ(kCoefClampMaxIntra8, 8192);
	ASSERT_EQ(BlockQfMax(127u, true), 63);
	const int32_t raw = DequantizeValue(63, 127u);
	// 127 是奇数 ⇒ δ = 0 ⇒ 反分段 = (2×63+1)×127 = 127×127 = 16129
	ASSERT_EQ(raw, 16129);
	ASSERT_GT(raw, kCoefClampMaxIntra8);

	int32_t qf[kBlockCoefs];
	DcOnly(qf, 63);
	uint8_t pred[kBlockCoefs], out[kBlockCoefs];
	std::memset(pred, kIntraPredLevel8, sizeof(pred));	  // 帧内预测电平恒 128
	ReconstructBlock(qf, 127u, true, pred, out);
	// 8192 ÷ 64 = 128 → 128 + 128 = 256 → 像素域夹到 255（上端点）
	ASSERT_EQ(RoundResidual(8192.0 / 64.0), 128);
	ExpectAll(out, 255, "帧内夹到 8192 / 预测 128");

	// 同一份夹取值配低预测面：23 + 128 = 151，未越界，说明上一行的 255 是像素域夹出来的
	std::memset(pred, 23, sizeof(pred));
	ReconstructBlock(qf, 127u, true, pred, out);
	ExpectAll(out, 151, "帧内夹到 8192 / 预测 23（不夹像素）");
	std::cout << "[帧内夹取] |QF|=63, q=127 → 反分段 " << raw << " 越界 → 夹 8192 → 残差 128 →"
			  << " 预测 128 时像素 255（夹）、预测 23 时像素 151（不夹）\n";
}

/* 像素域两端点的系统覆盖：正负两个方向的极值残差 × 七档预测电平，
 * 每一格都按闭式独立复算 `clamp(pred ± 255)`，再断实际输出。 */
TEST(DomainEdges, PixelSaturationBothEndsAcrossSevenPredLevelsAndTwoSigns)
{
	struct Row
	{
		uint8_t pred;
		uint8_t want_up;	  // 残差 +255
		uint8_t want_down;	  // 残差 −255
	};

	// 独立复算：min(255, pred+255) 与 max(0, pred−255)
	const Row rows[] = {
		{ 0,   255, 0 },
		  { 1,	 255, 0 },
		{ 127, 255, 0 },
		{ 128, 255, 0 },
		{ 253, 255, 0 },
		{ 254, 255, 0 },
		{ 255, 255, 0 }
	};
	int32_t qf_up[kBlockCoefs], qf_down[kBlockCoefs];
	DcOnly(qf_up, 8159);	   // 帧间 +255 残差
	DcOnly(qf_down, -8159);	   // 帧间 −255 残差
	ASSERT_EQ(DequantizeValue(-8159, 1u), -16319);
	ASSERT_EQ(RoundResidual(-16319.0 / 64.0), -255);
	ASSERT_EQ(kResidualClampMin8, -256);	// −255 未触第三级下界，说明像素 0 是第四级夹出来的

	size_t hits_up = 0u, hits_down = 0u;
	for (const Row& r: rows)
	{
		uint8_t pred[kBlockCoefs], out[kBlockCoefs];
		std::memset(pred, r.pred, sizeof(pred));
		ReconstructBlock(qf_up, 1u, false, pred, out);
		ExpectAll(out, r.want_up, "残差 +255");
		ReconstructBlock(qf_down, 1u, false, pred, out);
		ExpectAll(out, r.want_down, "残差 −255");
		if (r.want_up == kSampleMax8) ++hits_up;
		if (r.want_down == 0u) ++hits_down;
		// 逐格复算一遍，期望值不从实现里抄
		const int32_t up = static_cast<int32_t>(r.pred) + 255;
		const int32_t down = static_cast<int32_t>(r.pred) - 255;
		EXPECT_EQ(r.want_up, static_cast<uint8_t>(up > 255 ? 255 : (up < 0 ? 0 : up)));
		EXPECT_EQ(r.want_down, static_cast<uint8_t>(down > 255 ? 255 : (down < 0 ? 0 : down)));
	}
	std::cout << "[像素端点系统覆盖] " << (sizeof(rows) / sizeof(rows[0]))
			  << " 档预测电平 × 2 个符号 = " << 2 * (sizeof(rows) / sizeof(rows[0])) << " 格，落在上端点 255 的 "
			  << hits_up << " 格、"
			  << "落在下端点 0 的 " << hits_down << " 格\n";
	EXPECT_EQ(hits_up, sizeof(rows) / sizeof(rows[0]));
	EXPECT_EQ(hits_down, sizeof(rows) / sizeof(rows[0]));
}

/* 第三级残差域 [−256, 255] 也不是死码：只动 DC 到不了那里（DC-only 的像素就是 |f2| ÷ 64），
 * 但把 64 个系数全摆到窗口上界时，(0,0) 点合成出 16319 → 夹到 255，其余点为 0。 */
TEST(DomainEdges, AllCoefficientsAtWindowEdgeFiresThirdLevelClampAtOnePixel)
{
	int32_t qf[kBlockCoefs];
	for (int i = 0; i < kBlockCoefs; ++i) qf[i] = 8159;
	for (int i = 0; i < kBlockCoefs; ++i) EXPECT_LE(qf[i] < 0 ? -qf[i] : qf[i], BlockQfMax(1u, false));

	uint8_t pred[kBlockCoefs], out[kBlockCoefs];
	std::memset(pred, 7, sizeof(pred));
	ReconstructBlock(qf, 1u, false, pred, out);

	// 反变换：x[i][j] = (Σ_u H[i][u])(Σ_v H[j][v]) × 16319 ÷ 64，只有 i=j=0 时两个和都是 8
	size_t at_255 = 0u, at_7 = 0u;
	for (int i = 0; i < kBlockCoefs; ++i)
	{
		if (out[i] == 255) ++at_255;
		if (out[i] == 7) ++at_7;
	}
	EXPECT_EQ(at_255, 1u) << "只有 (0,0) 点越第三级上界，夹到 255 再 +7 仍夹在 255";
	EXPECT_EQ(at_7, static_cast<size_t>(kBlockCoefs - 1));
	std::cout << "[第三级夹取] 64 系数全取窗口上界 ⇒ 合成为 (0,0) 点 16319 → 夹 255，其余 63 点预测原值 7；"
				 "实测 255 的点 "
			  << at_255 << " 个、原值的点 " << at_7 << " 个\n";
}

/* ---------- 二 · 全链面：真造整块残差 = ±255 的图样 ---------- */

namespace
{

	constexpr uint32_t kW = 16u;
	constexpr uint32_t kH = 16u;

	/* 参考帧一色、当前帧另一色：编 I 再编 P，取回 P 的载体信息与「最后一帧」的像素统计。
	 * 出帧节奏（max_b_frames = 0）：喂 I 不出帧，喂 P 时吐 I，Flush 吐 P —— 所以比的是 Flush 那一帧。 */
	struct FullLink
	{
		size_t block_size = 0u;
		int32_t first_sign = 0;
		int32_t first_mag = 0;
		int mv_h = 0;
		int mv_v = 0;
		size_t payload_bytes = 0u;
		size_t luma_wanted = 0u;	// 末帧里等于期望亮度的点数
		size_t chroma_wanted = 0u;
		size_t recon_vs_decoded = static_cast<size_t>(-1);	  // 编码器内部重建 vs 解码输出的总绝对差
	};

	bool RunFlatPair(uint8_t ref_luma, uint8_t cur_luma, uint8_t cur_expect, FullLink* fl)
	{
		CLV_VideoEncParamsC ep;
		std::memset(&ep, 0, sizeof(ep));
		ep.display_width = kW;
		ep.display_height = kH;
		ep.bit_depth = 8u;
		ep.quant_precision = 7u;
		ep.default_quant = 1u;
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&ep, &err);
		if (e == nullptr) return false;
		CLV_VideoFrame* f1 = nullptr;
		CLV_VideoFrame* f2 = nullptr;
		bool ok =
			CLV_VideoEncoderAllocFrame(e, &f1) == CLV_VIDEO_OK && CLV_VideoEncoderAllocFrame(e, &f2) == CLV_VIDEO_OK;
		std::vector<uint8_t> bs_i, bs_p;
		if (ok)
		{
			FillFlat(f1, ref_luma, 128u);
			FillFlat(f2, cur_luma, 128u);
			CLV_VideoBitstream* h1 = nullptr;
			CLV_VideoBitstream* h2 = nullptr;
			ok = CLV_VideoEncoderEncode(e, f1, CLV_VIDEO_FRAME_I, 1u, 0, 0, &h1) == CLV_VIDEO_OK &&
				 CLV_VideoEncoderEncode(e, f2, CLV_VIDEO_FRAME_P, 1u, 1, 1, &h2) == CLV_VIDEO_OK && h1 != nullptr &&
				 h2 != nullptr;
			if (ok)
			{
				const uint8_t* d = nullptr;
				size_t n = 0u;
				if (CLV_VideoBitstreamGetBytes(h1, &d, &n) != CLV_VIDEO_OK) ok = false;
				else bs_i.assign(d, d + n);
				if (ok && CLV_VideoBitstreamGetBytes(h2, &d, &n) != CLV_VIDEO_OK) ok = false;
				if (ok) bs_p.assign(d, d + n);
			}
			if (h1 != nullptr) CLV_VideoBitstreamRelease(h1);
			if (h2 != nullptr) CLV_VideoBitstreamRelease(h2);
		}
		if (ok)
		{
			fl->payload_bytes = bs_p.size();
			VopPayload vp;
			ok = clv::video::DecodeVopPayload(bs_p.data(), bs_p.size(), 7u, kW / 16u, &vp, nullptr) == VideoErr::kOk;
			if (ok) ok = ! vp.macroblocks.empty();
			if (ok)
			{
				const clv::video::Macroblock& m = vp.macroblocks[0];
				fl->block_size = m.head.block_size;
				fl->mv_h = m.head.mv.h;
				fl->mv_v = m.head.mv.v;
				if (! m.block.empty())
				{
					/* 首系数不按字面位序手抠 —— 那种写法与「定宽」这一档绑死，换一档就得重写。
					 * 这里按本帧帧头声明的那法解回 64 个系数再看 DC：断言落在系数上，不在位上。 */
					int32_t qf[kBlockCoefs] = {};
					BitReader br(m.block.data(), m.block.size());
					ok = clv::video::DecodeBlock(&br, 1u, false, vp.head.coef_coding, qf) == VideoErr::kOk;
					fl->first_mag = qf[0] < 0 ? -qf[0] : qf[0];
					fl->first_sign = qf[0] < 0 ? 1 : 0;
				}
			}
		}
		if (ok)
		{
			CLV_VideoStreamParamsC dp;
			std::memset(&dp, 0, sizeof(dp));
			dp.bit_depth = 8u;
			dp.quant_precision = 7u;
			dp.max_b_frames = 0u;
			dp.obmc_disable = 1u;
			dp.display_width = kW;
			dp.display_height = kH;
			CLV_VideoError derr = CLV_VIDEO_OK;
			CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&dp, &derr);
			ok = d != nullptr;
			if (ok) ok = CLV_VideoDecoderDecode(d, bs_i.data(), bs_i.size(), 1u, 0, 0, nullptr) == CLV_VIDEO_OK;
			if (ok) ok = CLV_VideoDecoderDecode(d, bs_p.data(), bs_p.size(), 1u, 1, 1, nullptr) == CLV_VIDEO_OK;
			CLV_VideoFrame* last = nullptr;
			if (ok)
			{
				// 先把队列里能出的出干净，最后一支就是 P 帧
				for (;;)
				{
					CLV_VideoFrame* f = nullptr;
					uint8_t flag = 0u;
					if (CLV_VideoDecoderNextOutput(d, &f, &flag) != CLV_VIDEO_OK)
					{
						ok = false;
						break;
					}
					if (! flag)
					{
						if (f != nullptr) CLV_VideoFrameRelease(f);
						break;
					}
					/* 比的是**最后**那一支，所以这里刻意让一支借用跨过后续取帧调用：
					 * 手上那支只在拿到新一支之后才归还，任何时刻同时在场的只有两支。
					 * 取帧口只按显示序往前推，重置与关闭才是作废点 ⇒ 这样持有是本口的合法用法，不是漏还。 */
					if (last != nullptr) CLV_VideoFrameRelease(last);
					last = f;
				}
			}
			if (ok)
			{
				CLV_VideoFrame* f = nullptr;
				uint8_t flag = 0u;
				if (CLV_VideoDecoderFlush(d, &f, &flag) == CLV_VIDEO_OK && flag)
				{
					if (last != nullptr) CLV_VideoFrameRelease(last);
					last = f;
				}
				else if (f != nullptr)
				{
					CLV_VideoFrameRelease(f);
				}
			}
			if (ok) ok = last != nullptr;
			if (ok)
			{
				const PlaneView v = ViewOf(last);
				for (uint32_t pl = 0u; pl < 2u; ++pl)
				{
					const uint32_t pw = pl == 0u ? v.width : v.width / 2u;
					const uint32_t ph = pl == 0u ? v.height : v.height / 2u;
					const uint8_t want = pl == 0u ? cur_expect : 128u;
					size_t hit = 0u;
					for (uint32_t r = 0u; r < ph; ++r)
					{
						for (uint32_t c = 0u; c < pw; ++c)
							if (v.p[pl][static_cast<size_t>(r) * v.stride[pl] + c] == want) ++hit;
					}
					if (pl == 0u) fl->luma_wanted = hit;
					else fl->chroma_wanted = hit;
				}
				CLV_VideoFrame* rec = clv_internal_encoder_reconstruction(e);
				if (rec != nullptr)
				{
					const PlaneView rv = ViewOf(rec);
					/* 缓冲不同源守卫：钩子若把解码输出的平面别名回来，这个「重建 vs 解码」的绝对差和恒为 0，
					 * 读数就只是自己减自己。这里在返回 bool 的走查函数里，ASSERT_ 系宏不能用（它要 return），
					 * 改用 EXPECT_ 加一个互异标记：撞了就报红，并且不再把自减得到的 0 记进读数。 */
					bool distinct = true;
					for (uint32_t pl = 0u; pl < 3u; ++pl)
					{
						if (rv.p[pl] == v.p[pl])
						{
							distinct = false;
							ADD_FAILURE() << "平面 " << pl << " 撞同一块缓冲，绝对差失去意义";
						}
					}
					if (distinct) fl->recon_vs_decoded = AbsPlaneSum(rv, v);
				}
			}
			/* 归还落在判定之外：出错分支（取帧口报错、或最后没拿到帧）也一样要还。
			 * last 可能是上一次迭代里接过的那一支，只在成功路径归还会让它一直悬在句柄名下；
			 * 空指针走归还口是 no-op，不必另外判。 */
			CLV_VideoFrameRelease(last);
			CLV_VideoDecoderClose(d);
		}
		if (f1 != nullptr) CLV_VideoFrameRelease(f1);
		if (f2 != nullptr) CLV_VideoFrameRelease(f2);
		CLV_VideoEncoderClose(e);
		return ok;
	}

}	 // namespace

/* 参考面全 0、当前面全 255 ⇒ 每个亮度块整块残差 +255 ⇒ DC = 64 × 255 = 16320，
 * q = 1 时量化落在窗口上界 8159（帧间 14 bit 档），反分段回 16319，残差 255，像素 255 —— 与源逐字节全等。 */
TEST(DomainEdges, FullLinkFlatBlackToWhitePFrameHitsInterWindowEdgeAndKeepsExact)
{
	FullLink fl;
	ASSERT_TRUE(RunFlatPair(0u, 255u, 255u, &fl));
	// 整块残差 +255 ⇒ 只有 DC 非零：四个亮度块在场，色度残差全零不在场
	// 法 1（EOB+EG）下载体实长：4 块各 all_zero(1) + eob(6) + EG(8159) = 25 bit + 符号 1 bit = 33 bit
	// ⇒ 4 × 33 = 132 bit ⇒ 补齐 17 B；定宽那一档同内容是 4 块 × (64 × 14 ÷ 8 = 112 B) = 448 B。
	// 选哪一档不写在用例里：`clvtest::kPayloadFlatPairMB` 由 test_config_expect.h 按编译期的法定。
	EXPECT_EQ(fl.block_size, clvtest::kPayloadFlatPairMB);
	// 平面上每个候选矢量的代价都相同 ⇒ **平票取零**：零矢量的代价在粗搜趟首先取，之后的候选打平换不掉它。
	// 预测面本就处处同值，矢量取谁都不影响重建；实测载荷 460 B 与改前同值（MV 走 zigzag varint，0 与 −8 都占 1 字节）。
	EXPECT_EQ(fl.mv_h, 0);
	EXPECT_EQ(fl.mv_v, 0);
	EXPECT_EQ(fl.first_sign, 0);
	EXPECT_EQ(fl.first_mag, 8159);
	std::cout << "[全链 +255] P 载荷 " << fl.payload_bytes << " B，块数据 " << fl.block_size << " B（4 块 × 33 bit），"
			  << "首系数 符号 " << fl.first_sign << " 幅值 " << fl.first_mag
			  << "（帧间窗口上界 8159）；末帧亮度 255 的点 " << fl.luma_wanted << " / 256，色度 128 的点 "
			  << fl.chroma_wanted << " / 64，"
			  << "编码器内部重建 vs 解码输出总绝对差 " << fl.recon_vs_decoded << "\n";
	EXPECT_EQ(fl.luma_wanted, static_cast<size_t>(kW * kH));
	EXPECT_EQ(fl.chroma_wanted, static_cast<size_t>(kW / 2u * kH / 2u));
	EXPECT_EQ(fl.recon_vs_decoded, 0u) << "同域比较：编码器内部重建帧与解码输出逐字节全等";
}

/* 参考面全 255、当前面全 0 ⇒ 整块残差 −255 ⇒ DC = −16320，QF = −8159（符号位 1），
 * 反分段 −16319 → 残差 −255 → 像素 0（下端点），仍与源逐字节全等。 */
TEST(DomainEdges, FullLinkFlatWhiteToBlackPFrameHitsPixelZeroAndKeepsExact)
{
	FullLink fl;
	ASSERT_TRUE(RunFlatPair(255u, 0u, 0u, &fl));
	// 与 +255 那一档同长：幅值同是 8159，只有符号位不同
	EXPECT_EQ(fl.block_size, clvtest::kPayloadFlatPairMB);
	EXPECT_EQ(fl.first_sign, 1);
	EXPECT_EQ(fl.first_mag, 8159);
	std::cout << "[全链 −255] P 载荷 " << fl.payload_bytes << " B，块数据 " << fl.block_size << " B，首系数 符号 "
			  << fl.first_sign << " 幅值 " << fl.first_mag << "；末帧亮度 0 的点 " << fl.luma_wanted
			  << " / 256，色度 128 的点 " << fl.chroma_wanted << " / 64，重建 vs 解码总绝对差 " << fl.recon_vs_decoded
			  << "\n";
	EXPECT_EQ(fl.luma_wanted, static_cast<size_t>(kW * kH));
	EXPECT_EQ(fl.chroma_wanted, static_cast<size_t>(kW / 2u * kH / 2u));
	EXPECT_EQ(fl.recon_vs_decoded, 0u);
}
