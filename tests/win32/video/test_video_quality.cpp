/* 画质与分片开销：C 库真实现的数值断言件（公开 C ABI，不经实现层头）。
 *
 * 两档，刻意分开：
 *   确定性档 —— 不需要外部素材，克隆即跑：
 *     · 色度平面确实走变换量化（不是整块复制）；
 *     · 宏块级分片开销落在名义区间并可用算式复述（对应分片结构开销从未端到端验证那一条）。
 *   条件档 —— `CLV_TEST_YUV` 指到 16 对齐的 yuv420p raw 文件、`CLV_TEST_YUV_DIM` 给 "宽x高" 时才跑，
 *     不在场记跳过：真实素材上 Y / U / V 三路 PSNR 与真实码率随量化步长的单调性。
 *
 * 度量口径按下面三条写死（与画质基准的取线口径同源）：
 *   PSNR = **逐帧 PSNR 的算术平均**（不是各帧 MSE 汇总后再算一次）；
 *   只在裁回原始尺寸的区域上算，补边不计入；
 *   PSNR 单帧式 `10*log10(255^2 * N / SSE)`，N = 该帧该平面像素数。
 * 本件只断单调性与区间，**不定验收线** —— 验收线要人眼看过出图才立得住，不由数值断言代劳。
 */
#include "CLV_Video.h"

#include "gtest/gtest.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{

	constexpr uint8_t kQp = 7u;		  // v1 声明的量化精度
	constexpr uint8_t kQFine = 4u;	  // 分片开销档：误差与结构无关，取最细档即可

	struct Encoded
	{
		std::vector<uint8_t> payload;
		uint32_t slices = 0u;
	};

	/* 用编码器现产一帧载荷；type 与 q 由调用方给，slice_mb_count 由 slices 给（0 = 整帧一片） */
	Encoded EncodeOne(uint16_t w, uint16_t h, uint16_t slice_mb, CLV_VideoFrameType type, uint8_t q,
					  const std::vector<uint8_t>& yuv)
	{
		CLV_VideoEncParamsC ep;
		std::memset(&ep, 0, sizeof(ep));
		ep.display_width = w;
		ep.display_height = h;
		ep.bit_depth = 8u;
		ep.quant_precision = kQp;
		ep.default_quant = q;
		ep.max_b_frames = 0u;
		ep.slice_mb_count = slice_mb;
		ep.intra_refresh_period = 15u;

		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&ep, &err);
		EXPECT_NE(e, nullptr) << CLV_VideoStrError(err);
		if (e == nullptr) return {};

		CLV_VideoFrame* f = nullptr;
		EXPECT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
		CLV_VideoFrameDescC d;
		std::memset(&d, 0, sizeof(d));
		EXPECT_EQ(CLV_VideoFrameGetDesc(f, &d, nullptr), CLV_VIDEO_OK);

		auto write_plane = [&](int plane, const uint8_t* src, int rows, int cols)
		{
			for (int r = 0; r < rows; ++r)
			{
				std::memcpy(d.plane[plane] + static_cast<size_t>(r) * d.stride[plane],
							src + static_cast<size_t>(r) * cols, static_cast<size_t>(cols));
			}
		};
		const size_t lw = static_cast<size_t>(w) * h;
		const size_t cw = (static_cast<size_t>(w) / 2) * (static_cast<size_t>(h) / 2);
		write_plane(0, yuv.data(), h, w);
		write_plane(1, yuv.data() + lw, h / 2, w / 2);
		write_plane(2, yuv.data() + lw + cw, h / 2, w / 2);

		CLV_VideoBitstream* bs = nullptr;
		EXPECT_EQ(CLV_VideoEncoderEncode(e, f, type, q, 0, 0, &bs), CLV_VIDEO_OK);
		Encoded out;
		uint32_t ns = 0u;
		CLV_VideoBitstreamSliceCount(bs, &ns);
		out.slices = ns;
		const uint8_t* all = nullptr;
		size_t n = 0u;
		CLV_VideoBitstreamGetBytes(bs, &all, &n);
		out.payload.assign(all, all + n);

		CLV_VideoBitstreamRelease(bs);
		CLV_VideoFrameRelease(f);
		CLV_VideoEncoderClose(e);
		return out;
	}

	/* 单包帧解回一帧（max_b_frames = 0 ⇒ 显示序 == 解码序）；返回三平面 SSE 与像素数 */
	void DecodeOneSse(uint16_t w, uint16_t h, const Encoded& enc, const std::vector<uint8_t>& src, uint64_t sse[3],
					  uint64_t npix[3])
	{
		CLV_VideoStreamParamsC sp;
		std::memset(&sp, 0, sizeof(sp));
		sp.bit_depth = 8u;
		sp.quant_precision = kQp;
		sp.max_b_frames = 0u;
		sp.vlc_table_id = 0u;
		sp.obmc_disable = 1u;
		sp.display_width = w;
		sp.display_height = h;
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&sp, &err);
		ASSERT_NE(d, nullptr) << CLV_VideoStrError(err);
		ASSERT_EQ(CLV_VideoDecoderDecode(d, enc.payload.data(), enc.payload.size(), enc.slices, 0, 0, nullptr),
				  CLV_VIDEO_OK);

		CLV_VideoFrame* out = nullptr;
		uint8_t got = 0u;
		ASSERT_EQ(CLV_VideoDecoderFlush(d, &out, &got), CLV_VIDEO_OK);
		ASSERT_EQ(got, 1u) << "单包 I 帧冲刷后必须出一帧";
		CLV_VideoFrameDescC od;
		std::memset(&od, 0, sizeof(od));
		ASSERT_EQ(CLV_VideoFrameGetDesc(out, &od, nullptr), CLV_VIDEO_OK);

		const size_t lw = static_cast<size_t>(w) * h;
		const size_t cw = (static_cast<size_t>(w) / 2) * (static_cast<size_t>(h) / 2);
		for (int p = 0; p < 3; ++p)
		{
			const int rows = (p == 0) ? h : h / 2;
			const int cols = (p == 0) ? w : w / 2;
			const uint8_t* s = src.data() + (p == 0 ? 0u : (p == 1 ? lw : lw + cw));
			for (int r = 0; r < rows; ++r)
			{
				const uint8_t* o = od.plane[p] + static_cast<size_t>(r) * od.stride[p];
				for (int c = 0; c < cols; ++c)
				{
					const int64_t diff = static_cast<int64_t>(o[c]) - static_cast<int64_t>(s[r * cols + c]);
					sse[p] += static_cast<uint64_t>(diff * diff);
				}
				npix[p] += static_cast<uint64_t>(cols);
			}
		}
		CLV_VideoFrameRelease(out);
		CLV_VideoDecoderClose(d);
	}

	/* 176×144 = 11×9 = 99 宏块；亮度中灰、色度带横向渐变 ⇒ 色度一定有非零 AC，走得到变换量化 */
	std::vector<uint8_t> ColorFrame(uint16_t w, uint16_t h)
	{
		std::vector<uint8_t> yuv(static_cast<size_t>(w) * h * 3 / 2, 0u);
		const size_t lw = static_cast<size_t>(w) * h;
		for (size_t i = 0; i < lw; ++i) yuv[i] = 96u;
		const int cw = w / 2;
		const int ch = h / 2;
		for (int r = 0; r < ch; ++r)
		{
			for (int c = 0; c < cw; ++c)
			{
				yuv[lw + static_cast<size_t>(r) * cw + c] = static_cast<uint8_t>(96u + (c % 31u));
				yuv[lw + static_cast<size_t>(ch) * cw + static_cast<size_t>(r) * cw + c] =
					static_cast<uint8_t>(160u - (c % 31u));
			}
		}
		return yuv;
	}

	double PlanePsnr(uint64_t sse, uint64_t npix)
	{
		if (sse == 0u) return 99.0;
		return 10.0 * std::log10(255.0 * 255.0 * static_cast<double>(npix) / static_cast<double>(sse));
	}

	/* MSVC 把 getenv / fopen / sscanf 挂成 C4996，/WX 下直接红；这三个窄壳把有告警的调用关在本文件内，
	   不给 tests 目标放宽编译开关。 */
	std::string EnvValue(const char* key)
	{
#ifdef _MSC_VER
		char* raw = nullptr;
		size_t len = 0u;
		if (_dupenv_s(&raw, &len, key) != 0 || raw == nullptr) return {};
		std::string value(raw);
		std::free(raw);
		return value;
#else
		const char* raw = std::getenv(key);
		return raw == nullptr ? std::string() : std::string(raw);
#endif
	}

	/* "宽x高" → (w, h)，x 与 X 都认；任一环不成立返回 false */
	bool ParseDims(const std::string& text, int* w, int* h)
	{
		const size_t sep = text.find_first_of("xX");
		if (sep == std::string::npos || sep == 0u || sep + 1u >= text.size()) return false;
		char* end = nullptr;
		const unsigned long a = std::strtoul(text.substr(0, sep).c_str(), &end, 10);
		if (end == nullptr || *end != '\0' || a == 0u || a > 65535u) return false;
		const unsigned long b = std::strtoul(text.substr(sep + 1u).c_str(), &end, 10);
		if (end == nullptr || *end != '\0' || b == 0u || b > 65535u) return false;
		*w = static_cast<int>(a);
		*h = static_cast<int>(b);
		return true;
	}

}	 // namespace

/* 确定性档 · 色度走变换量化而不是整块复制，且误差随步长严格递增 */
TEST(VideoQuality, ChromaPlanesGoThroughQuantization)
{
	constexpr uint16_t W = 176u;
	constexpr uint16_t H = 144u;
	const std::vector<uint8_t> src = ColorFrame(W, H);
	/* q ≤ 6 在这张合成帧上位精确（实测三平面 SSE 全 0），拿来证「色度被量化过」没有分辨力；
	   起点必须选在误差已经出来的档位。 */
	static const uint8_t tiers[] = { 24u, 64u, 96u };
	uint64_t sse_u[3] = { 0u, 0u, 0u };
	uint64_t sse_v[3] = { 0u, 0u, 0u };
	for (int t = 0; t < 3; ++t)
	{
		const Encoded enc = EncodeOne(W, H, 0u, CLV_VIDEO_FRAME_I, tiers[t], src);
		ASSERT_FALSE(enc.payload.empty());
		uint64_t sse[3] = { 0u, 0u, 0u };
		uint64_t n[3] = { 0u, 0u, 0u };
		DecodeOneSse(W, H, enc, src, sse, n);
		sse_u[t] = sse[1];
		sse_v[t] = sse[2];
		std::printf("q=%3u 载荷=%zu B  PSNR/dB Y=%6.2f U=%6.2f V=%6.2f\n", tiers[t], enc.payload.size(),
					PlanePsnr(sse[0], n[0]), PlanePsnr(sse[1], n[1]), PlanePsnr(sse[2], n[2]));
	}
	/* 若实现是「整块复制色度」，色度 SSE 会恒为 0；三档都非零且严格递增 ⇒ 色块确实进了变换 + 量化通路 */
	for (int t = 0; t < 3; ++t)
	{
		EXPECT_GT(sse_u[t], 0u) << "q=" << static_cast<int>(tiers[t]) << " U 平面零误差，像是被整块复制";
		EXPECT_GT(sse_v[t], 0u) << "q=" << static_cast<int>(tiers[t]) << " V 平面零误差，像是被整块复制";
	}
	EXPECT_GT(sse_u[1], sse_u[0]);
	EXPECT_GT(sse_u[2], sse_u[1]);
	EXPECT_GT(sse_v[1], sse_v[0]);
	EXPECT_GT(sse_v[2], sse_v[1]);
}

/* 确定性档 · 宏块级分片开销可用算式复述（对应「结构开销从未端到端验证」那一条） */
TEST(VideoQuality, SliceOverheadMatchesNominalArithmetic)
{
	constexpr uint16_t W = 176u;
	constexpr uint16_t H = 144u;
	const int mb_total = (W / 16) * (H / 16);
	const std::vector<uint8_t> src = ColorFrame(W, H);

	const Encoded one = EncodeOne(W, H, 0u, CLV_VIDEO_FRAME_I, kQFine, src);	 // 整帧一片：单包帧免分片头
	const Encoded many = EncodeOne(W, H, 1u, CLV_VIDEO_FRAME_I, kQFine, src);	 // 每片 1 宏块
	ASSERT_FALSE(one.payload.empty());
	ASSERT_EQ(many.slices, static_cast<uint32_t>(mb_total));

	const size_t extra = many.payload.size() - one.payload.size();
	const double per_slice = static_cast<double>(extra) / static_cast<double>(many.slices);
	std::printf("分片开销：一片=%zu B、%d 片=%zu B、多出 %zu B，平均每片 %.2f B\n", one.payload.size(), many.slices,
				many.payload.size(), extra, per_slice);
	/* 名义构成 = 定长 8 B（后续片）+ first_mb varint 1 B；首片再 +1 B 带 vop_quant。
	   所以每片增量必须落在 [9, 10] 之外不可，取 [8, 12] 为硬区间。 */
	EXPECT_GE(per_slice, 8.0);
	EXPECT_LE(per_slice, 12.0);

	const Encoded mid = EncodeOne(W, H, 8u, CLV_VIDEO_FRAME_I, kQFine, src);
	std::printf("分片数递减验证：1 宏块/片 = %u 片 %zu B，8 宏块/片 = %u 片 %zu B\n", many.slices, many.payload.size(),
				mid.slices, mid.payload.size());
	EXPECT_LT(mid.payload.size(), many.payload.size());
	EXPECT_GT(mid.payload.size(), one.payload.size());
}

/* 条件档 · 真实素材上的三路 PSNR 与真实码率单调性 */
TEST(VideoQualityRealMedia, ThreePlanePsnrAndBitrateFallMonotonicallyWithQuantizer)
{
	const std::string path = EnvValue("CLV_TEST_YUV");
	const std::string dim = EnvValue("CLV_TEST_YUV_DIM");
	if (path.empty() || dim.empty())
		GTEST_SKIP() << "没给 CLV_TEST_YUV / CLV_TEST_YUV_DIM：真实素材档跳过（素材不入库）";
	int w = 0;
	int h = 0;
	if (! ParseDims(dim, &w, &h) || (w % 16) != 0 || (h % 16) != 0)
		GTEST_SKIP() << "CLV_TEST_YUV_DIM 必须是 16 对齐的 宽x高";
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (! in) GTEST_SKIP() << "素材打不开：" << path;
	const size_t total = static_cast<size_t>(in.tellg());
	in.seekg(0, std::ios::beg);
	const size_t per = static_cast<size_t>(w) * h * 3 / 2;
	int frames = (per == 0u) ? 0 : static_cast<int>(total / per);
	if (frames > 16) frames = 16;	 // 控时长：单调性用不到更多帧
	if (frames <= 0) GTEST_SKIP() << "素材不足一帧";
	std::vector<uint8_t> buf(per * static_cast<size_t>(frames));
	in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
	if (static_cast<size_t>(in.gcount()) != buf.size()) GTEST_SKIP() << "素材读取不足一帧";

	static const uint8_t tiers[] = { 8u, 32u, 96u };
	double last[3] = { 1e9, 1e9, 1e9 };
	size_t last_bytes = 0u;
	for (const uint8_t q: tiers)
	{
		/* 一条 I 帧链起一遍句柄，逐帧编码 + 解码，PSNR 逐帧算再平均 */
		std::vector<double> per_frame[3];
		size_t bytes = 0u;
		CLV_VideoEncParamsC ep;
		std::memset(&ep, 0, sizeof(ep));
		ep.display_width = static_cast<uint16_t>(w);
		ep.display_height = static_cast<uint16_t>(h);
		ep.bit_depth = 8u;
		ep.quant_precision = kQp;
		ep.default_quant = q;
		ep.max_b_frames = 0u;
		ep.intra_refresh_period = 15u;
		CLV_VideoStreamParamsC sp;
		std::memset(&sp, 0, sizeof(sp));
		sp.bit_depth = 8u;
		sp.quant_precision = kQp;
		sp.max_b_frames = 0u;
		sp.vlc_table_id = 0u;
		sp.obmc_disable = 1u;
		sp.display_width = static_cast<uint16_t>(w);
		sp.display_height = static_cast<uint16_t>(h);
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&ep, &err);
		ASSERT_NE(e, nullptr) << CLV_VideoStrError(err);
		CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&sp, &err);
		ASSERT_NE(d, nullptr) << CLV_VideoStrError(err);
		CLV_VideoFrame* fr = nullptr;
		ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &fr), CLV_VIDEO_OK);
		CLV_VideoFrameDescC fd;
		std::memset(&fd, 0, sizeof(fd));
		ASSERT_EQ(CLV_VideoFrameGetDesc(fr, &fd, nullptr), CLV_VIDEO_OK);

		for (int i = 0; i < frames; ++i)
		{
			for (int p = 0; p < 3; ++p)
			{
				const int rows = (p == 0) ? h : h / 2;
				const int cols = (p == 0) ? w : w / 2;
				const size_t lw = static_cast<size_t>(w) * h;
				const size_t cw = (static_cast<size_t>(w) / 2) * (static_cast<size_t>(h) / 2);
				const uint8_t* s = buf.data() + static_cast<size_t>(i) * per + (p == 0 ? 0u : (p == 1 ? lw : lw + cw));
				for (int r = 0; r < rows; ++r)
				{
					std::memcpy(fd.plane[p] + static_cast<size_t>(r) * fd.stride[p], s + static_cast<size_t>(r) * cols,
								static_cast<size_t>(cols));
				}
			}
			CLV_VideoBitstream* bs = nullptr;
			const CLV_VideoFrameType t = (i % 15 == 0) ? CLV_VIDEO_FRAME_I : CLV_VIDEO_FRAME_P;
			ASSERT_EQ(CLV_VideoEncoderEncode(e, fr, t, q, i, i, &bs), CLV_VIDEO_OK);
			uint32_t ns = 0u;
			CLV_VideoBitstreamSliceCount(bs, &ns);
			const uint8_t* sd = nullptr;
			size_t ss = 0u;
			for (uint32_t s = 0u; s < ns; ++s)
			{
				CLV_VideoBitstreamGetSlice(bs, s, &sd, &ss);
				ASSERT_EQ(CLV_VideoDecoderDecode(d, sd, ss, ns, i, i, nullptr), CLV_VIDEO_OK);
			}
			const uint8_t* all = nullptr;
			size_t alln = 0u;
			CLV_VideoBitstreamGetBytes(bs, &all, &alln);
			bytes += alln;
			CLV_VideoBitstreamRelease(bs);

			CLV_VideoFrame* out = nullptr;
			uint8_t got = 0u;
			if (CLV_VideoDecoderNextOutput(d, &out, &got) != CLV_VIDEO_OK || ! got) continue;
			CLV_VideoFrameDescC od;
			std::memset(&od, 0, sizeof(od));
			CLV_VideoFrameGetDesc(out, &od, nullptr);
			uint64_t f_sse[3] = { 0u, 0u, 0u };
			uint64_t f_n[3] = { 0u, 0u, 0u };
			for (int p = 0; p < 3; ++p)
			{
				const int rows = (p == 0) ? h : h / 2;
				const int cols = (p == 0) ? w : w / 2;
				const size_t lw = static_cast<size_t>(w) * h;
				const size_t cw = (static_cast<size_t>(w) / 2) * (static_cast<size_t>(h) / 2);
				const uint8_t* s = buf.data() + static_cast<size_t>(i) * per + (p == 0 ? 0u : (p == 1 ? lw : lw + cw));
				for (int r = 0; r < rows; ++r)
				{
					const uint8_t* o = od.plane[p] + static_cast<size_t>(r) * od.stride[p];
					for (int c = 0; c < cols; ++c)
					{
						const int64_t diff = static_cast<int64_t>(o[c]) - static_cast<int64_t>(s[r * cols + c]);
						f_sse[p] += static_cast<uint64_t>(diff * diff);
					}
					f_n[p] += static_cast<uint64_t>(cols);
				}
				per_frame[p].push_back(PlanePsnr(f_sse[p], f_n[p]));
			}
			CLV_VideoFrameRelease(out);
		}
		for (;;)
		{
			CLV_VideoFrame* out = nullptr;
			uint8_t got = 0u;
			if (CLV_VideoDecoderFlush(d, &out, &got) != CLV_VIDEO_OK || ! got)
			{
				if (out != nullptr) CLV_VideoFrameRelease(out);
				break;
			}
			CLV_VideoFrameRelease(out);
		}
		CLV_VideoFrameRelease(fr);
		CLV_VideoDecoderClose(d);
		CLV_VideoEncoderClose(e);

		double mean[3];
		for (int p = 0; p < 3; ++p)
		{
			ASSERT_FALSE(per_frame[p].empty()) << "q=" << static_cast<int>(q) << " 没收到任何出帧";
			double s = 0.0;
			for (const double v: per_frame[p]) s += v;
			mean[p] = s / static_cast<double>(per_frame[p].size());
		}
		const double bpp = static_cast<double>(bytes) * 8.0 / (static_cast<double>(frames) * per);
		std::printf("q=%3u 帧=%d Y=%6.2f U=%6.2f V=%6.2f 载荷=%zu B bpp=%.4f\n", q, frames, mean[0], mean[1], mean[2],
					bytes, bpp);
		for (int p = 0; p < 3; ++p)
		{
			EXPECT_LT(mean[p], last[p] + 1e-9) << "q=" << static_cast<int>(q) << " 平面 " << p << " 的均值没随 q 降";
			last[p] = mean[p];
		}
		if (last_bytes != 0u) EXPECT_LT(bytes, last_bytes) << "真实码率没随 q 变粗而下降";
		last_bytes = bytes;
	}
}
