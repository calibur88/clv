/* test_video_motion.cpp
 *
 * **两面混用**：端到端与句柄行为走公开 C ABI（`CLV_Video*` 族 13 个入口、实测 27 处调用，如 `CLV_VideoBitstreamGetBytes`、`CLV_VideoBitstreamRelease`、`CLV_VideoDecoderClose`），
 *   单元级直测走实现层头（`video/block.h`、`video/encoder_hook.h`、`video/macroblock.h`、`video/motion.h`、`video/payload.h`、`video/reconstruct.h`、`video/status.h`）。**仅静态构建（`CLV_BUILD_SHARED=OFF`）下可链**的原因是后面这些头，
 *   不是「不经公开 ABI」——两面各写各的：主体过公开 C ABI，另有几处直调实现层函数钉住不进 ABI 的行为。
 *
 * 运动补偿取样口径的落点：半像素插值核（b / c / d 三式）、色度同核 + 矢量换算、边界复制带、双向平均，
 * 以及「真位移 → 真矢量」的端到端实证。
 *
 * 静态素材下零运动就是最优解，光看「编解码全等」测不出运动补偿是否真的参与。
 * 因此这里喂一个确实发生了位移的序列，同时钉住两条：宏块里出现非零矢量，
 * 且编解两侧仍逐字节全等。
 */
#include "CLV_Video.h"
#include "video/block.h"
#include "video/encoder_hook.h"
#include "video/macroblock.h"
#include "video/motion.h"
#include "video/payload.h"
#include "video/reconstruct.h"
#include "video/status.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{

	using clv::video::BidirectionalAverage;
	using clv::video::ChromaMotionVector;
	using clv::video::kBlockCoefs;
	using clv::video::kBlocksPerMacroblock;
	using clv::video::Macroblock;
	using clv::video::MotionVector;
	using clv::video::PlaneView;
	using clv::video::PredictBlock8x8;
	using clv::video::PredictMacroblock;
	using clv::video::VideoErr;
	using clv::video::VopPayload;

	/* 确定性图案：逐点不同，便于逐像素对拍（checkerboard 那种规整面会让不同式子算出同一个值）。 */
	uint8_t Pattern(uint32_t r, uint32_t c, uint32_t plane)
	{
		const uint32_t v = r * 37u + c * 91u + ((r ^ c) * 13u) + plane * 61u;
		return static_cast<uint8_t>(v & 0xFFu);
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

	/* 直接读一个像素（不经过任何插值），用于独立复算期望值。 */
	uint8_t Pix(const PlaneView& v, uint32_t plane, int64_t x, int64_t y)
	{
		return v.p[plane][static_cast<size_t>(y) * v.stride[plane] + static_cast<size_t>(x)];
	}

	void FillPattern(CLV_VideoFrame* f, int shift_x)
	{
		const PlaneView v = ViewOf(f);
		const uint32_t w = v.width, h = v.height;
		const uint32_t pw[3] = { w, w / 2u, w / 2u };
		const uint32_t ph[3] = { h, h / 2u, h / 2u };
		for (uint32_t pl = 0u; pl < 3u; ++pl)
		{
			for (uint32_t r = 0u; r < ph[pl]; ++r)
			{
				for (uint32_t c = 0u; c < pw[pl]; ++c)
				{
					// 色度平面的位移按 4:2:0 减半：亮度平移 4 像素 = 色度平移 2 像素
					const int half = (pl == 0u) ? shift_x : (shift_x / 2);
					const int src = static_cast<int>(c) - half;
					const uint32_t cc = (src < 0) ? 0u : static_cast<uint32_t>(src);
					v.p[pl][static_cast<size_t>(r) * v.stride[pl] + c] = Pattern(r, cc, pl);
				}
			}
		}
	}

	CLV_VideoFrame* AllocFrame(CLV_VideoEncoder* e)
	{
		CLV_VideoFrame* f = nullptr;
		if (CLV_VideoEncoderAllocFrame(e, &f) != CLV_VIDEO_OK) return nullptr;
		return f;
	}

	int OpenEncoder(CLV_VideoEncoder** out, uint32_t w, uint32_t h, uint8_t q)
	{
		CLV_VideoEncParamsC p;
		std::memset(&p, 0, sizeof(p));
		p.display_width = static_cast<uint16_t>(w);
		p.display_height = static_cast<uint16_t>(h);
		p.bit_depth = 8u;
		p.quant_precision = 7u;
		p.default_quant = q;
		CLV_VideoError err = CLV_VIDEO_OK;
		*out = CLV_VideoEncoderOpen(&p, &err);
		return static_cast<int>(err);
	}

}	 // namespace

/* 半像素三式：水平 / 垂直 / 对角三个半像素位置，逐像素等于 b / c / d 三式。
 * 期望值在这里**按定义独立复算**（A / B / C / D 直接读平面），不调用被测的取样函数。 */
TEST(Motion, HalfPixelSamplesFollowTheThreeFormulas)
{
	CLV_VideoEncoder* e = nullptr;
	ASSERT_EQ(OpenEncoder(&e, 64u, 64u, 8u), static_cast<int>(CLV_VIDEO_OK));
	CLV_VideoFrame* f = AllocFrame(e);
	ASSERT_NE(f, nullptr);
	FillPattern(f, 0);
	const PlaneView v = ViewOf(f);

	const uint32_t px = 8u, py = 8u;
	uint8_t out[kBlockCoefs];

	// b 式：水平半像素 = (A + B + 1) >> 1
	PredictBlock8x8(v, 0u, px, py, MotionVector { 1, 0 }, out);
	for (uint32_t r = 0u; r < 8u; ++r)
	{
		for (uint32_t c = 0u; c < 8u; ++c)
		{
			const uint32_t aa = Pix(v, 0u, px + c, py + r);
			const uint32_t bb = Pix(v, 0u, px + c + 1u, py + r);
			EXPECT_EQ(out[r * 8u + c], static_cast<uint8_t>((aa + bb + 1u) >> 1u));
		}
	}

	// c 式：垂直半像素 = (A + C + 1) >> 1
	PredictBlock8x8(v, 0u, px, py, MotionVector { 0, 1 }, out);
	for (uint32_t r = 0u; r < 8u; ++r)
	{
		for (uint32_t c = 0u; c < 8u; ++c)
		{
			const uint32_t aa = Pix(v, 0u, px + c, py + r);
			const uint32_t cc = Pix(v, 0u, px + c, py + r + 1u);
			EXPECT_EQ(out[r * 8u + c], static_cast<uint8_t>((aa + cc + 1u) >> 1u));
		}
	}

	// d 式：对角半像素 = (A + B + C + D + 2) >> 2
	PredictBlock8x8(v, 0u, px, py, MotionVector { 1, 1 }, out);
	for (uint32_t r = 0u; r < 8u; ++r)
	{
		for (uint32_t c = 0u; c < 8u; ++c)
		{
			const uint32_t aa = Pix(v, 0u, px + c, py + r);
			const uint32_t bb = Pix(v, 0u, px + c + 1u, py + r);
			const uint32_t cc = Pix(v, 0u, px + c, py + r + 1u);
			const uint32_t dd = Pix(v, 0u, px + c + 1u, py + r + 1u);
			EXPECT_EQ(out[r * 8u + c], static_cast<uint8_t>((aa + bb + cc + dd + 2u) >> 2u));
		}
	}

	// 整像素（偶值矢量）不作插值，逐像素就是同位像素 —— 零运动是这些式的退化情形
	PredictBlock8x8(v, 0u, px, py, MotionVector { 0, 0 }, out);
	for (uint32_t r = 0u; r < 8u; ++r)
		for (uint32_t c = 0u; c < 8u; ++c) EXPECT_EQ(out[r * 8u + c], Pix(v, 0u, px + c, py + r));

	CLV_VideoFrameRelease(f);
	CLV_VideoEncoderClose(e);
}

/* 色度换算口径：色度矢量 = round_half(亮度分量)，取样用**与亮度同一套核**，只有块尺寸是 8×8。 */
TEST(Motion, ChromaSharesTheKernelAndHalvesTheVector)
{
	// round_half(v) = (v + 1) >> 1，取值是 floor((v+1)/2)：正数 .5 进一，负数也按向下取整
	EXPECT_EQ(ChromaMotionVector(MotionVector { 0, 0 }).h, 0);
	EXPECT_EQ(ChromaMotionVector(MotionVector { 1, 0 }).h, 1);
	EXPECT_EQ(ChromaMotionVector(MotionVector { 2, 0 }).h, 1);
	EXPECT_EQ(ChromaMotionVector(MotionVector { 3, 0 }).h, 2);
	EXPECT_EQ(ChromaMotionVector(MotionVector { 4, 0 }).h, 2);
	EXPECT_EQ(ChromaMotionVector(MotionVector { -1, 0 }).h, 0);
	EXPECT_EQ(ChromaMotionVector(MotionVector { -2, 0 }).h, -1);
	EXPECT_EQ(ChromaMotionVector(MotionVector { -3, 0 }).h, -1);
	EXPECT_EQ(ChromaMotionVector(MotionVector { -4, 0 }).h, -2);
	// 亮度右移 4 像素 = 半像素单位 8 → 色度 4（= 色度平面右移 2 像素）
	EXPECT_EQ(ChromaMotionVector(MotionVector { 8, 0 }).h, 4);

	CLV_VideoEncoder* e = nullptr;
	ASSERT_EQ(OpenEncoder(&e, 64u, 64u, 8u), static_cast<int>(CLV_VIDEO_OK));
	CLV_VideoFrame* f = AllocFrame(e);
	ASSERT_NE(f, nullptr);
	FillPattern(f, 0);
	const PlaneView v = ViewOf(f);

	// 同一个宏块：亮度矢量 (8,0)，一次取六块
	uint8_t six[kBlocksPerMacroblock][kBlockCoefs];
	PredictMacroblock(v, 1u, 1u, MotionVector { 8, 0 }, six);
	// Cb（idx=4）应当等于「用换算后的色度矢量、色度平面、8×8 块」单独取的结果 —— 同核，不是两套核
	uint8_t cb[kBlockCoefs];
	PredictBlock8x8(v, 1u, 8u, 8u, ChromaMotionVector(MotionVector { 8, 0 }), cb);
	for (int i = 0; i < kBlockCoefs; ++i) EXPECT_EQ(six[4][i], cb[i]);

	CLV_VideoFrameRelease(f);
	CLV_VideoEncoderClose(e);
}

/* 边缘复制带：参考缓冲带一圈 1 像素的边缘复制带 —— 越界取最外像素，矢量本身不裁剪。 */
TEST(Motion, BorderReplicatesTheOutermostPixel)
{
	CLV_VideoEncoder* e = nullptr;
	ASSERT_EQ(OpenEncoder(&e, 64u, 64u, 8u), static_cast<int>(CLV_VIDEO_OK));
	CLV_VideoFrame* f = AllocFrame(e);
	ASSERT_NE(f, nullptr);
	FillPattern(f, 0);
	const PlaneView v = ViewOf(f);

	uint8_t out[kBlockCoefs];
	/* 矢量 (-8,-8) 是整块左移上移各 4 像素（半像素单位 ÷2）：
	 * 块内前 4 行 / 前 4 列的取样位置落到帧外，按复制带夹到 0；后 4 行 / 列照常取。 */
	PredictBlock8x8(v, 0u, 0u, 0u, MotionVector { -8, -8 }, out);
	for (uint32_t r = 0u; r < 8u; ++r)
	{
		for (uint32_t c = 0u; c < 8u; ++c)
		{
			const int64_t sx = static_cast<int64_t>(c) - 4;
			const int64_t sy = static_cast<int64_t>(r) - 4;
			const uint32_t ex = (sx < 0) ? 0u : static_cast<uint32_t>(sx);
			const uint32_t ey = (sy < 0) ? 0u : static_cast<uint32_t>(sy);
			EXPECT_EQ(out[r * 8u + c], Pix(v, 0u, ex, ey)) << "(" << r << "," << c << ")";
		}
	}

	// 右边界：块起点在最右块，矢量再往右 4 像素 → 越出最右列的部分取最右列
	const uint32_t last = 64u - 8u;
	PredictBlock8x8(v, 0u, last, 0u, MotionVector { 8, 0 }, out);
	for (uint32_t r = 0u; r < 8u; ++r)
	{
		for (uint32_t c = 0u; c < 8u; ++c)
		{
			const uint32_t x = last + c + 4u;
			const uint32_t ex = (x > 63u) ? 63u : x;
			EXPECT_EQ(out[r * 8u + c], Pix(v, 0u, ex, r)) << "(" << r << "," << c << ")";
		}
	}

	CLV_VideoFrameRelease(f);
	CLV_VideoEncoderClose(e);
}

/* 半像素三式 · 双向平均：P = (Pf + Pb + 1) >> 1，操作数恒非负，.5 进一。 */
TEST(Motion, BidirectionalAverageRoundsHalfUp)
{
	EXPECT_EQ(BidirectionalAverage(0u, 0u), 0u);
	EXPECT_EQ(BidirectionalAverage(0u, 1u), 1u);
	EXPECT_EQ(BidirectionalAverage(1u, 1u), 1u);
	EXPECT_EQ(BidirectionalAverage(1u, 2u), 2u);
	EXPECT_EQ(BidirectionalAverage(255u, 255u), 255u);
	EXPECT_EQ(BidirectionalAverage(100u, 201u), 151u);	  // (301 + 1) >> 1
}

/* 端到端实证：序列**真的发生了位移**，于是
 *   ① 宏块里出现非零矢量（零运动是最优解的话这条不成立）；
 *   ② 位移量对得上（亮度 4 像素 = 半像素单位 8，色度随之半分）；
 *   ③ 编解逐字节全等仍成立 —— 编解两侧走的是同一份运动补偿取样实现。 */
TEST(Motion, RealDisplacementProducesNonZeroVectorsAndStaysByteIdentical)
{
	constexpr uint32_t kW = 64u, kH = 64u;
	constexpr int kShift = 4;	 // 亮度右移 4 像素

	CLV_VideoEncoder* e = nullptr;
	ASSERT_EQ(OpenEncoder(&e, kW, kH, 8u), static_cast<int>(CLV_VIDEO_OK));

	CLV_VideoFrame* f1 = AllocFrame(e);
	CLV_VideoFrame* f2 = AllocFrame(e);
	ASSERT_NE(f1, nullptr);
	ASSERT_NE(f2, nullptr);
	FillPattern(f1, 0);
	FillPattern(f2, kShift);

	CLV_VideoBitstream* bs1 = nullptr;
	ASSERT_EQ(CLV_VideoEncoderEncode(e, f1, CLV_VIDEO_FRAME_I, 8u, 0, 0, &bs1), CLV_VIDEO_OK);

	CLV_VideoBitstream* bs2 = nullptr;
	ASSERT_EQ(CLV_VideoEncoderEncode(e, f2, CLV_VIDEO_FRAME_P, 8u, 1, 1, &bs2), CLV_VIDEO_OK);
	ASSERT_NE(bs2, nullptr);

	const uint8_t* data = nullptr;
	size_t size = 0u;
	ASSERT_EQ(CLV_VideoBitstreamGetBytes(bs2, &data, &size), CLV_VIDEO_OK);

	VopPayload vp;
	ASSERT_EQ(clv::video::DecodeVopPayload(data, size, 7u, kW / 16u, &vp, nullptr), VideoErr::kOk);
	ASSERT_FALSE(vp.macroblocks.empty());

	/* ① 统计矢量：源帧是把图案**右移** 4 像素得到的，于是取样位置要往**左**找 4 像素，
	 * 即 mv.h = -8（半像素单位，负号 = 向左）。垂直方向没有位移，故 v 恒为 0。 */
	constexpr int kExpect = -kShift * 2;
	size_t nonzero = 0u;
	size_t exact = 0u;
	std::cout << "真位移端到端：逐宏块矢量 =";
	for (const Macroblock& m: vp.macroblocks)
	{
		std::cout << " (" << m.head.mv.h << "," << m.head.mv.v << ")";
		if (m.head.mv.h != 0 || m.head.mv.v != 0) ++nonzero;
		if (m.head.mv.h == kExpect && m.head.mv.v == 0) ++exact;
		EXPECT_EQ(m.head.mv.v, 0) << "垂直分量不该有位移";
	}
	std::cout << "\n";
	EXPECT_GT(nonzero, vp.macroblocks.size() / 2u) << "多数宏块应当找到非零矢量";
	EXPECT_GT(exact, vp.macroblocks.size() / 2u) << "多数宏块应当命中 -8（左移 4 像素）";
	std::cout << "真位移端到端：" << vp.macroblocks.size() << " 个宏块，非零矢量 " << nonzero << " 个，"
			  << "命中 -8 " << exact << " 个\n";

	// ③ 编解逐字节全等：解码输出与编码器内部重建逐字节全等
	CLV_VideoStreamParamsC dp;
	std::memset(&dp, 0, sizeof(dp));
	dp.bit_depth = 8u;
	dp.quant_precision = 7u;
	dp.max_b_frames = 0u;
	dp.obmc_disable = 1u;
	dp.display_width = static_cast<uint16_t>(kW);
	dp.display_height = static_cast<uint16_t>(kH);
	CLV_VideoError derr = CLV_VIDEO_OK;
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&dp, &derr);
	ASSERT_EQ(derr, CLV_VIDEO_OK);
	ASSERT_NE(d, nullptr);

	// 先喂 I 帧（P 帧的前向参考），再喂 P 帧
	const uint8_t* d1 = nullptr;
	size_t s1 = 0u;
	ASSERT_EQ(CLV_VideoBitstreamGetBytes(bs1, &d1, &s1), CLV_VIDEO_OK);
	ASSERT_EQ(CLV_VideoDecoderDecode(d, d1, s1, 1u, 0, 0, nullptr), CLV_VIDEO_OK);

	ASSERT_EQ(CLV_VideoDecoderDecode(d, data, size, 1u, 1, 1, nullptr), CLV_VIDEO_OK);
	/* 出帧序：max_b_frames = 0 时无重排，I 帧与 P 帧依次吐出；要比的是**最后**那一帧（P 帧）。
	 * 这里取到取不动为止，不假定吐几帧 —— DPB 的出帧策略是本库自己的事。 */
	CLV_VideoFrame* last = nullptr;
	for (;;)
	{
		CLV_VideoFrame* cur = nullptr;
		if (CLV_VideoDecoderNextOutput(d, &cur, nullptr) != CLV_VIDEO_OK || cur == nullptr) break;
		/* 刻意持有：本例比的是**最后**那一支，所以手上这支要跨过下一次取帧调用留着 ——
		 * 每拿到新一支才归还上一支，任何时刻同时在场的只有两支，末尾那支在本件收尾处归还。
		 * 出帧口只按显示序往前推，作废点是重置与关闭，两者本例都没碰。 */
		if (last != nullptr) CLV_VideoFrameRelease(last);
		last = cur;
	}
	// 输出队列的阈值是 max_b_frames + 2：两帧刚到阈值只吐一帧，剩下那帧要 Flush 才出来
	for (;;)
	{
		CLV_VideoFrame* cur = nullptr;
		uint8_t got = 0u;
		if (CLV_VideoDecoderFlush(d, &cur, &got) != CLV_VIDEO_OK || got == 0u || cur == nullptr) break;
		// 与上面取帧那一段同一形状：拿到新一支才归还上一支
		if (last != nullptr) CLV_VideoFrameRelease(last);
		last = cur;
	}
	CLV_VideoFrame* out = last;
	ASSERT_NE(out, nullptr);

	const CLV_VideoFrame* rec = clv_internal_encoder_reconstruction(e);
	ASSERT_NE(rec, nullptr);
	const PlaneView pv = ViewOf(out);
	const PlaneView rv = ViewOf(const_cast<CLV_VideoFrame*>(rec));
	ASSERT_EQ(pv.width, rv.width);
	// 缓冲不同源守卫：两块内存合成一块时，下面的逐点相等就成了自比，恒真且测不出运动补偿
	for (uint32_t pl = 0u; pl < 3u; ++pl)
		ASSERT_NE(pv.p[pl], rv.p[pl]) << "平面 " << pl << " 撞同一块缓冲，全等断言失去意义";
	size_t same = 0u, total = 0u;
	for (uint32_t pl = 0u; pl < 3u; ++pl)
	{
		const uint32_t w = (pl == 0u) ? pv.width : pv.width / 2u;
		const uint32_t h = (pl == 0u) ? pv.height : pv.height / 2u;
		for (uint32_t r = 0u; r < h; ++r)
		{
			for (uint32_t c = 0u; c < w; ++c)
			{
				const uint8_t a = pv.p[pl][static_cast<size_t>(r) * pv.stride[pl] + c];
				const uint8_t b = rv.p[pl][static_cast<size_t>(r) * rv.stride[pl] + c];
				EXPECT_EQ(a, b) << "平面 " << pl << " (" << r << "," << c << ")";
				if (a == b) ++same;
				++total;
			}
		}
	}
	EXPECT_EQ(same, total);
	std::cout << "真位移端到端：解码输出 vs 编码器内部重建，" << total << " 点逐字节全等\n";

	CLV_VideoFrameRelease(out);
	CLV_VideoBitstreamRelease(bs1);
	CLV_VideoBitstreamRelease(bs2);
	CLV_VideoFrameRelease(f1);
	CLV_VideoFrameRelease(f2);
	CLV_VideoDecoderClose(d);
	CLV_VideoEncoderClose(e);
}

