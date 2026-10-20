/* test_video_lossless_roundtrip.cpp
 *
 * 公开 C ABI 面上的无损往返：编码器输入帧 → 码流 → 解码器输出，**逐字节全等**。
 * 本件只用公开头，不碰实现层符号；变换系数那一层的算术前提钉在实现层件里。
 *
 * 三条口径：
 *   一 · 源是**自写图样**（彩条 / 满幅极值行带 / 运动方块），不经过任何外部解码或色彩转换
 *        —— 往返全等只能由本库负责，不能被上游取整污染。
 *   二 · 无损的模式信号在码流里（帧头的量化步长 = 0），解码侧不接任何模式参数；
 *        故本件的解码句柄一律按有损参数建，仍要能解出无损流。
 *   三 · 模式与量化步长必须一致：有损档不接受 0，无损档不接受非 0，两边都判参数矛盾，
 *        不做静默降级；`default_quant` 只作声明值，无损模式下不参与任何判定。
 *
 * 图样里放了「整块全 0 像素」的行带：帧内残差 = 0 − 128 = −128 ⇒ 该块 DC = 64 × (−128) = −8192，
 * 正好压在最细无损载体的下边界上；载体位宽不够就会被夹取，下面的逐字节全等当场翻红。 */

#include "CLV_Video.h"

#include "gtest/gtest.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{

	constexpr uint16_t kW = 64u;
	constexpr uint16_t kH = 64u;
	constexpr uint8_t kQp = 7u;
	constexpr size_t kYpx = static_cast<size_t>(kW) * kH;
	constexpr size_t kCpx = kYpx / 4u;

	// 彩条定值（bt.709 有限范围，与门面基准序列生成器同一张表）
	const int kBarY[7] = { 235, 219, 188, 173, 78, 63, 32 };
	const int kBarCb[7] = { 128, 16, 154, 42, 214, 102, 240 };
	const int kBarCr[7] = { 128, 138, 16, 26, 230, 240, 118 };

	struct Frame
	{
		std::vector<uint8_t> y;
		std::vector<uint8_t> cb;
		std::vector<uint8_t> cr;
	};

	/* 行带布局（64×64，十六行为一条带）：
	 *   y ∈ [0,16)   七色彩条
	 *   y ∈ [16,32)  全 0 —— 顶出 −8192 的满幅系数
	 *   y ∈ [32,48)  全 255 —— 对称的近界
	 *   y ∈ [48,64)  16×16 方块每帧右移 8 像素回卷，底色 16、块内 235
	 * 色度：彩条带按表取常量；全 0 带取 240、全 255 带取 16（色度两个界）；方块带 128。 */
	Frame MakeFrame(int idx)
	{
		Frame f;
		f.y.assign(kYpx, 0u);
		f.cb.assign(kCpx, 128u);
		f.cr.assign(kCpx, 128u);
		const int bx = (8 * idx) % (kW - 16);
		for (int y = 0; y < kH; ++y)
		{
			uint8_t* row = f.y.data() + static_cast<size_t>(y) * kW;
			for (int x = 0; x < kW; ++x)
				if (y < 16) row[x] = static_cast<uint8_t>(kBarY[(x * 7) / kW]);
				else if (y < 32) row[x] = 0u;
				else if (y < 48) row[x] = 255u;
				else row[x] = (x >= bx && x < bx + 16) ? 235u : 16u;
		}
		for (int cy = 0; cy < kH / 2; ++cy)
		{
			uint8_t* rowb = f.cb.data() + static_cast<size_t>(cy) * (kW / 2);
			uint8_t* rowr = f.cr.data() + static_cast<size_t>(cy) * (kW / 2);
			for (int cx = 0; cx < kW / 2; ++cx)
			{
				if (cy < 8)
				{
					const int bar = (cx * 2 * 7) / kW;
					rowb[cx] = static_cast<uint8_t>(kBarCb[bar]);
					rowr[cx] = static_cast<uint8_t>(kBarCr[bar]);
				}
				else if (cy < 16)
				{
					rowb[cx] = 240u;
					rowr[cx] = 240u;
				}
				else if (cy < 24)
				{
					rowb[cx] = 16u;
					rowr[cx] = 16u;
				}
				else
				{
					rowb[cx] = 128u;
					rowr[cx] = 128u;
				}
			}
		}
		return f;
	}

	void FillInto(CLV_VideoFrame* handle, const Frame& src)
	{
		CLV_VideoFrameDescC d;
		std::memset(&d, 0, sizeof(d));
		CLV_VideoFrameGetDesc(handle, &d, nullptr);
		const size_t pw[3] = { d.coded_width, d.coded_width / 2u, d.coded_width / 2u };
		const size_t ph[3] = { d.coded_height, d.coded_height / 2u, d.coded_height / 2u };
		const uint8_t* lists[3] = { src.y.data(), src.cb.data(), src.cr.data() };
		for (uint32_t pl = 0u; pl < 3u; ++pl)
			for (size_t r = 0u; r < ph[pl]; ++r)
				std::memcpy(d.plane[pl] + r * d.stride[pl], lists[pl] + r * pw[pl], pw[pl]);
	}

	/* 解码输出与源帧逐字节比，按**显示尺寸**取（补边区不参与，与画质度量同一口径）。 */
	size_t DiffAgainst(CLV_VideoFrame* out, const Frame& src, size_t* checked)
	{
		CLV_VideoFrameDescC d;
		std::memset(&d, 0, sizeof(d));
		CLV_VideoFrameGetDesc(out, &d, nullptr);
		const size_t pw[3] = { d.display_width, d.display_width / 2u, d.display_width / 2u };
		const size_t ph[3] = { d.display_height, d.display_height / 2u, d.display_height / 2u };
		const uint8_t* lists[3] = { src.y.data(), src.cb.data(), src.cr.data() };
		size_t bad = 0u;
		for (uint32_t pl = 0u; pl < 3u; ++pl)
		{
			for (size_t r = 0u; r < ph[pl]; ++r)
			{
				const uint8_t* pa = d.plane[pl] + r * d.stride[pl];
				const uint8_t* pb = lists[pl] + r * pw[pl];
				for (size_t c = 0u; c < pw[pl]; ++c)
				{
					if (pa[c] != pb[c]) ++bad;
					++(*checked);
				}
			}
		}
		return bad;
	}

	CLV_VideoEncParamsC Params(bool lossless)
	{
		CLV_VideoEncParamsC p;
		std::memset(&p, 0, sizeof(p));
		p.display_width = kW;
		p.display_height = kH;
		p.bit_depth = 8u;
		p.quant_precision = kQp;
		p.default_quant = 8u;
		p.max_b_frames = 0u;
		p.slice_mb_count = 0u;
		p.intra_refresh_period = 15u;
		p.mode = lossless ? CLV_VIDEO_MODE_LOSSLESS : CLV_VIDEO_MODE_LOSSY;
		return p;
	}

	CLV_VideoStreamParamsC StreamParams()
	{
		CLV_VideoStreamParamsC s;
		std::memset(&s, 0, sizeof(s));
		s.bit_depth = 8u;
		s.quant_precision = kQp;
		s.max_b_frames = 0u;
		s.obmc_disable = 1u;
		s.display_width = kW;
		s.display_height = kH;
		return s;
	}

}	 // namespace

/* 一 · 无损往返逐字节全等：1 个 I + 3 个 P，三个平面全部字节相等。 */
TEST(VideoLossless, RoundTripOfSyntheticPatternIsByteExact)
{
	constexpr int kFrames = 4;
	std::vector<Frame> srcs;
	for (int i = 0; i < kFrames; ++i) srcs.push_back(MakeFrame(i));

	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoEncParamsC ep = Params(true);
	CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&ep, &err);
	ASSERT_NE(e, nullptr) << "无损句柄，err = " << static_cast<int>(err);
	CLV_VideoFrame* f = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
	CLV_VideoStreamParamsC sp = StreamParams();
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&sp, &err);
	ASSERT_NE(d, nullptr) << "解码句柄（不带模式参数）";

	size_t checked = 0u, bad = 0u, out_seq = 0u, payload = 0u;
	for (int idx = 0; idx < kFrames; ++idx)
	{
		FillInto(f, srcs[static_cast<size_t>(idx)]);
		CLV_VideoBitstream* bs = nullptr;
		ASSERT_EQ(CLV_VideoEncoderEncode(e, f, idx == 0 ? CLV_VIDEO_FRAME_I : CLV_VIDEO_FRAME_P, 0u, idx, idx, &bs),
				  CLV_VIDEO_OK)
			<< "无损档帧 " << idx;
		ASSERT_NE(bs, nullptr);

		const uint8_t* all = nullptr;
		size_t n = 0u;
		ASSERT_EQ(CLV_VideoBitstreamGetBytes(bs, &all, &n), CLV_VIDEO_OK);
		payload += n;
		uint32_t ns = 0u;
		ASSERT_EQ(CLV_VideoBitstreamSliceCount(bs, &ns), CLV_VIDEO_OK);
		const uint8_t* p = nullptr;
		size_t ss = 0u;
		ASSERT_EQ(CLV_VideoBitstreamGetSlice(bs, 0u, &p, &ss), CLV_VIDEO_OK);

		// 码流里唯一可见的无损证据就是帧头那个 0；解码器只看它，本件不传任何模式参数
		CLV_VideoFrameInfoC info;
		std::memset(&info, 0, sizeof(info));
		ASSERT_EQ(CLV_VideoDecoderDecode(d, p, ss, ns, idx, idx, &info), CLV_VIDEO_OK);
		EXPECT_EQ(info.quant_used, 0u) << "帧 " << idx << " 的量化步长必须交回 0";
		EXPECT_EQ(info.type, idx == 0 ? CLV_VIDEO_FRAME_I : CLV_VIDEO_FRAME_P);

		CLV_VideoFrame* out = nullptr;
		uint8_t got = 0u;
		ASSERT_EQ(CLV_VideoDecoderNextOutput(d, &out, &got), CLV_VIDEO_OK);
		if (got != 0u && out != nullptr)
		{
			bad += DiffAgainst(out, srcs[out_seq], &checked);
			++out_seq;
			CLV_VideoFrameRelease(out);
		}
		else if (out != nullptr)
		{
			CLV_VideoFrameRelease(out);
		}
		CLV_VideoBitstreamRelease(bs);
	}

	// 收尾冲刷：仍在队列里的帧按显示序放出来比掉
	while (out_seq < static_cast<size_t>(kFrames))
	{
		CLV_VideoFrame* out = nullptr;
		uint8_t got = 0u;
		if (CLV_VideoDecoderFlush(d, &out, &got) != CLV_VIDEO_OK || got == 0u)
		{
			if (out != nullptr) CLV_VideoFrameRelease(out);
			break;
		}
		bad += DiffAgainst(out, srcs[out_seq], &checked);
		++out_seq;
		CLV_VideoFrameRelease(out);
	}

	std::printf("无损往返：出帧 %zu / %d，比对 %zu 字节，不符 %zu 字节，载荷合计 %zu B\n", out_seq, kFrames, checked,
				bad, payload);
	EXPECT_EQ(out_seq, static_cast<size_t>(kFrames));
	EXPECT_EQ(bad, 0u);
	EXPECT_EQ(checked, (kYpx + 2u * kCpx) * static_cast<size_t>(kFrames));

	CLV_VideoFrameRelease(f);
	CLV_VideoDecoderClose(d);
	CLV_VideoEncoderClose(e);
}

/* 二 · 分辨点放在**码流**上，不放在重建上。
 *
 * 两条实测事实决定了这么测：平滑图样在 `q = 1` 也逐字节全等（6144 字节比对、不符 0）；
 * 1 像素交替的棋盘**同样全等** —— 那条交替图样本身就是 8×8 Sylvester Hadamard 的一个基向量，
 * 能量全落在单个系数上，单系数误差 ≤ 1 经 ÷64 摊到每像素 ≤ 1/64，舍入照样抹平。
 * 也就是说「重建是否全等」分辨不开无损档与最细有损档（真实屏摄内容能分开：`q = 1` 在屏摄锚帧上
 * 实测 PSNR 75.2 dB，那是六十四个系数误差同向累加的结果）。
 * 两条通路真正的区别在载体：同一份在场块图样，无损走 15 bit / 系数、最细有损走 13 bit，
 * 载荷必不相同；而 `q = 0` 载的是系数原值、`q = 1` 载的是电平序号，语义都不同。 */
TEST(VideoLossless, LosslessStreamDiffersFromTheFinestLossyTier)
{
	struct Leg
	{
		size_t payload = 0u;
		size_t bad = 0u;
		size_t checked = 0u;
	};

	auto run = [](const Frame& src, uint8_t q, Leg* out) -> bool
	{
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoEncParamsC ep = Params(q == 0u);
		CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&ep, &err);
		if (e == nullptr) return false;
		CLV_VideoFrame* f = nullptr;
		CLV_VideoFrame* got_frame = nullptr;
		bool ok = CLV_VideoEncoderAllocFrame(e, &f) == CLV_VIDEO_OK;
		if (ok)
		{
			FillInto(f, src);
			CLV_VideoBitstream* bs = nullptr;
			ok = CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_I, q, 0, 0, &bs) == CLV_VIDEO_OK && bs != nullptr;
			if (ok)
			{
				CLV_VideoStreamParamsC sp = StreamParams();
				CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&sp, &err);
				if (d != nullptr)
				{
					const uint8_t* all = nullptr;
					size_t n = 0u;
					ok = CLV_VideoBitstreamGetBytes(bs, &all, &n) == CLV_VIDEO_OK && n > 0u;
					if (ok)
					{
						CLV_VideoFrameInfoC info;
						std::memset(&info, 0, sizeof(info));
						ok = CLV_VideoDecoderDecode(d, all, n, 1u, 0, 0, &info) == CLV_VIDEO_OK && info.quant_used == q;
						out->payload = n;
					}
					if (ok)
					{
						// 单帧序列的出帧可能整个落在冲刷里（显示序队列的正常收尾），两条通路都一样
						uint8_t flag = 0u;
						ok = CLV_VideoDecoderNextOutput(d, &got_frame, &flag) == CLV_VIDEO_OK;
						if (ok && flag == 0u)
						{
							if (got_frame != nullptr)
							{
								CLV_VideoFrameRelease(got_frame);
								got_frame = nullptr;
							}
							ok = CLV_VideoDecoderFlush(d, &got_frame, &flag) == CLV_VIDEO_OK && flag != 0u;
						}
						else
						{
							ok = ok && flag != 0u;
						}
						if (ok && got_frame != nullptr) out->bad = DiffAgainst(got_frame, src, &out->checked);
					}
					/* 用完即还，且还在那一侧句柄关闭之前：重置与关闭会作废所有已交出的帧，
					 * 排在 Close 之后那次归还只能落到「已作废」那一档，白拿一次不成立。 */
					if (got_frame != nullptr)
					{
						CLV_VideoFrameRelease(got_frame);
						got_frame = nullptr;
					}
					CLV_VideoDecoderClose(d);
				}
				CLV_VideoBitstreamRelease(bs);
			}
			CLV_VideoFrameRelease(f);
		}
		if (got_frame != nullptr) CLV_VideoFrameRelease(got_frame);
		CLV_VideoEncoderClose(e);
		return ok;
	};

	// 两条图样各测一次：分段常量带（顶得出 −8192 满幅 DC）与 1 像素交替棋盘
	std::vector<Frame> pats;
	pats.push_back(MakeFrame(0));
	{
		Frame f;
		f.y.assign(kYpx, 0u);
		f.cb.assign(kCpx, 128u);
		f.cr.assign(kCpx, 128u);
		for (int y = 0; y < kH; ++y)
		{
			uint8_t* row = f.y.data() + static_cast<size_t>(y) * kW;
			for (int x = 0; x < kW; ++x) row[x] = ((x + y) & 1) ? 255u : 0u;
		}
		pats.push_back(f);
	}
	for (size_t which = 0; which < pats.size(); ++which)
	{
		const Frame& src = pats[which];
		Leg lossless, q1;
		ASSERT_TRUE(run(src, 0u, &lossless)) << "which = " << which;
		ASSERT_TRUE(run(src, 1u, &q1)) << "which = " << which;
		std::printf("图样 %zu：无损载荷 %zu B / 不符 %zu 字节；q=1 载荷 %zu B / 不符 %zu 字节\n", which,
					lossless.payload, lossless.bad, q1.payload, q1.bad);
		// 无损必须全等；最细有损档允许全等（平滑与单基向量内容就是会全等），但码流必须不同
		EXPECT_EQ(lossless.bad, 0u);
		/* 两条通路的 checked 自比恒等（同一份图样尺寸累加出来的两个数），钉它等于钉噪音 ⇒
		 * 判据是「两条通路都比满了有效区」：应比点数由本文件的几何常量独立给出：
		 * Y 全幅 64 × 64 = 4096，两路色度各 32 × 32 = 1024，合计 4096 + 2 × 1024 = 6144。 */
		const size_t valid_points = kYpx + 2u * kCpx;
		EXPECT_EQ(valid_points, 6144u) << "有效区点数与本文件的尺寸不符，前提要重看";
		EXPECT_EQ(lossless.checked, valid_points) << "无损那条没比满有效区";
		EXPECT_EQ(q1.checked, valid_points) << "q=1 那条没比满有效区";
		EXPECT_NE(lossless.payload, q1.payload) << "两条通路在码流上必须分得开";
		EXPECT_GT(lossless.payload, q1.payload) << "在场块图样相同时，15 bit 档必然比 13 bit 档长";
	}
}

/* 三 · 模式与量化步长必须一致，两边都不静默降级。 */
TEST(VideoLossless, ModeAndQuantizerMustAgree)
{
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoEncParamsC lossy = Params(false);
	CLV_VideoEncoder* e_lossy = CLV_VideoEncoderOpen(&lossy, &err);
	ASSERT_NE(e_lossy, nullptr);
	CLV_VideoFrame* f = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e_lossy, &f), CLV_VIDEO_OK);
	CLV_VideoBitstream* bs = reinterpret_cast<CLV_VideoBitstream*>(1u);
	// 有损模式喂 0：0 不在有损档的合法域 [1, QuantCeiling(p)] 内，判越界
	EXPECT_EQ(CLV_VideoEncoderEncode(e_lossy, f, CLV_VIDEO_FRAME_I, 0u, 0, 0, &bs), CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL);
	EXPECT_EQ(bs, nullptr);
	EXPECT_EQ(CLV_VideoEncoderEncode(e_lossy, f, CLV_VIDEO_FRAME_I, 8u, 0, 0, &bs), CLV_VIDEO_OK);
	CLV_VideoBitstreamRelease(bs);
	bs = nullptr;
	CLV_VideoFrameRelease(f);
	CLV_VideoEncoderClose(e_lossy);

	CLV_VideoEncParamsC lossless = Params(true);
	CLV_VideoEncoder* e_lossless = CLV_VideoEncoderOpen(&lossless, &err);
	ASSERT_NE(e_lossless, nullptr);
	CLV_VideoFrame* f2 = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e_lossless, &f2), CLV_VIDEO_OK);
	// 无损模式喂非 0：参数矛盾，不改成有损档悄悄编一帧
	// 无损模式带非零 q = 逐调用侧参数矛盾，走 84
	EXPECT_EQ(CLV_VideoEncoderEncode(e_lossless, f2, CLV_VIDEO_FRAME_I, 32u, 0, 0, &bs),
			  CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL);
	EXPECT_EQ(bs, nullptr);
	EXPECT_EQ(CLV_VideoEncoderEncode(e_lossless, f2, CLV_VIDEO_FRAME_I, 0u, 0, 0, &bs), CLV_VIDEO_OK);
	CLV_VideoBitstreamRelease(bs);
	CLV_VideoFrameRelease(f2);
	CLV_VideoEncoderClose(e_lossless);
}

/* 四 · `default_quant` 只作声明值：无损模式下越出有损域也照样交得出句柄，有损模式照旧拦；
 * 模式字段本身只认两个值，第三个值判值域不成立。把「无视」写成测过的事实，而不是注释里的承诺。 */
TEST(VideoLossless, DefaultQuantIsDeclarationOnly)
{
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoEncParamsC lossless = Params(true);
	lossless.default_quant = 200u;	  // 越出 [1, 127]
	CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&lossless, &err);
	ASSERT_NE(e, nullptr) << "无损模式不该用有损域去校验声明值";
	EXPECT_EQ(err, CLV_VIDEO_OK);
	CLV_VideoEncoderClose(e);

	CLV_VideoEncParamsC lossy = Params(false);
	lossy.default_quant = 200u;
	err = CLV_VIDEO_OK;
	e = CLV_VideoEncoderOpen(&lossy, &err);
	EXPECT_EQ(e, nullptr);
	EXPECT_EQ(err, CLV_VIDEO_QUANT_OUT_OF_RANGE);

	CLV_VideoEncParamsC bogus = Params(false);
	bogus.mode = static_cast<CLV_VideoModeC>(7);
	err = CLV_VIDEO_OK;
	e = CLV_VideoEncoderOpen(&bogus, &err);
	EXPECT_EQ(e, nullptr);
	EXPECT_EQ(err, CLV_VIDEO_VALUE_RANGE);
}
