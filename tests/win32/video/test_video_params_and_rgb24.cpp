/* test_video_params_and_rgb24.cpp
 *
 * 本件钉四个公开面对象的入参档与正确性，这些判据既有断言没盖到：
 *
 *   一 · 出帧阈值（`preroll_frames`）：缺省档 = 重排地板、**只升不降**、大 N 原样受理；
 *   二 · 布局：两个尾部字段落在填充里，结构体长度不变，所以自报长度判不出新旧布局 ——
 *        这条差别只能靠断言钉住；
 *   三 · 起池档 `prefill_mode` 的声明侧取值域：三档之外交不出句柄；
 *   四 · I420 → RGB24：向量化支与恒标量支逐字节同、公开入口的入参档、增益式的一个已知点。
 *
 * 出帧序与参考缓冲驱逐那两套既有断言在本文件里不重复：阈值缺省档 = 旧算式，它们必须原样绿。
 */
#include "CLV_Video.h"
#include "video/dpb.h"
#include "video/rgb.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace
{

	using clv::video::Rgb24FromI420;
	using clv::video::Rgb24FromI420Scalar;
	using clv::video::Scheduler;

	/* 持有只有一份账、记在帧对象上，参考帧管理类不自建第二张按槽号的表（见 `video/dpb.h` 文件头），
	 * 所以这里只给 `Scheduler` 一只真的 `FrameHolders` 手 —— 与门面建句柄时把自身交给它同一形状。
	 * 本用例不喂帧，那只手一次也不该被调到，`Calls()` 把这件事一并钉住。 */
	class HoldLedger: public clv::video::FrameHolders
	{
		public:

		void Acquire(int id) override
		{
			(void) id;
			++calls_;
		}

		void Release(int id) override
		{
			(void) id;
			++calls_;
		}

		size_t Calls() const { return calls_; }

		private:

		size_t calls_ = 0u;
	};

	/* 阈值与重排地板都是构造期定一次的算式，不需要喂帧就能钉。 */
	TEST(VideoParamsAndRgb24, PrerollDefaultTakesReorderFloor)
	{
		HoldLedger ledger;
		const Scheduler a(2u, 0u, &ledger);
		EXPECT_EQ(a.ReorderFloor(), 4u);
		EXPECT_EQ(a.Threshold(), 4u);
		const Scheduler b(0u, 0u, &ledger);
		EXPECT_EQ(b.ReorderFloor(), 2u);
		EXPECT_EQ(b.Threshold(), 2u);
		EXPECT_EQ(ledger.Calls(), 0u);	  // 只读算式的路径不碰持有那一只手
	}

	TEST(VideoParamsAndRgb24, PrerollNeverShrinksBelowFloorAndGrowsAsDeclared)
	{
		HoldLedger ledger;
		// max_b = 1 ⇒ 地板 3：声明 1 / 2 都被钳回 3，声明 3 等价缺省，声明 8 / 255 原样受理
		const Scheduler c1(1u, 1u, &ledger);
		EXPECT_EQ(c1.Threshold(), 3u);
		const Scheduler c2(1u, 2u, &ledger);
		EXPECT_EQ(c2.Threshold(), 3u);
		const Scheduler c3(1u, 3u, &ledger);
		EXPECT_EQ(c3.Threshold(), 3u);
		const Scheduler c8(0u, 8u, &ledger);
		EXPECT_EQ(c8.Threshold(), 8u);
		const Scheduler cmax(0u, 255u, &ledger);
		EXPECT_EQ(cmax.Threshold(), 255u);
		EXPECT_EQ(ledger.Calls(), 0u);	  // 阈值只升不降这条也不需要碰持有那一只手
	}

	/* 尾部两个 uint8_t 落在原有填充里：sizeof 不变 ⇒ struct_size 分不出新旧布局，
	 * 这条事实要由断言守着（它决定了混编判定只能落在取值上）。 */
	TEST(VideoParamsAndRgb24, TailFieldsLiveInPaddingSoStructSizeCannotGateThem)
	{
		EXPECT_EQ(sizeof(CLV_VideoStreamParamsC), 40u);
		EXPECT_EQ(offsetof(CLV_VideoStreamParamsC, preroll_frames), 36u);
		EXPECT_EQ(offsetof(CLV_VideoStreamParamsC, prefill_mode), 37u);
	}

	CLV_VideoStreamParamsC MakeParams()
	{
		CLV_VideoStreamParamsC p {};
		std::memset(&p, 0, sizeof(p));
		p.struct_size = sizeof(p);
		p.bit_depth = 8u;
		p.quant_precision = 7u;
		p.max_b_frames = 0u;
		p.vlc_table_id = 0u;
		p.extended_esc = 0u;
		p.alpha_enabled = 0u;
		p.obmc_disable = 1u;
		p.layer_id = 0u;
		p.display_width = 32u;
		p.display_height = 32u;
		p.tool_list = nullptr;
		p.tool_list_size = 0u;
		return p;
	}

	TEST(VideoParamsAndRgb24, PrefillModeDomainIsRejectedAtOpenNotSilentlyDefaulted)
	{
		CLV_VideoError e = CLV_VIDEO_OK;
		{
			CLV_VideoStreamParamsC p = MakeParams();
			p.prefill_mode = 3u;	// 三档之外
			CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&p, &e);
			EXPECT_EQ(d, nullptr);
			EXPECT_EQ(e, CLV_VIDEO_VALUE_RANGE);
		}
		for (uint8_t m: { uint8_t(CLV_PREFILL_DEFAULT), uint8_t(CLV_PREFILL_SERIAL), uint8_t(CLV_PREFILL_PARALLEL) })
		{
			CLV_VideoStreamParamsC p = MakeParams();
			p.prefill_mode = m;
			p.preroll_frames = m;	 // 阈值不设业务上限，2 也照受理
			CLV_VideoError e2 = CLV_VIDEO_OK;
			CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&p, &e2);
			EXPECT_NE(d, nullptr) << "mode=" << int(m);
			EXPECT_EQ(e2, CLV_VIDEO_OK) << "mode=" << int(m);
			if (d != nullptr) CLV_VideoDecoderClose(d);
		}
	}

	/* 两支同式的对平面：宽度覆盖 8 的整倍数与非倍数（尾组走标量收尾），
	 * 亮度 / 色度都扫到端点，保证夹取与移位两侧都被踩到。 */
	void ExpectBothBranchesEqual(uint32_t w, uint32_t h)
	{
		const uint32_t cw = (w + 1u) / 2u, ch = (h + 1u) / 2u;
		std::vector<uint8_t> y(size_t(w) * h), u(size_t(cw) * ch), v(size_t(cw) * ch);
		for (size_t i = 0; i < y.size(); ++i) y[i] = uint8_t((i * 7u + (i / 3u)) & 0xFFu);
		for (size_t i = 0; i < u.size(); ++i)
		{
			u[i] = uint8_t((i * 101u) & 0xFFu);
			v[i] = uint8_t(255u - ((i * 37u) & 0xFFu));
		}
		/* 输出缓冲 = 紧密排列的一帧 RGB24（3w 字节一行、行间不留填充）⇒ 写入长度恰为 w × h × 3。
		 * 尾部再挂 kGuardBytes 字节的 0xEE 预填带：两支谁越出这个长度就写进带里，读回来不是 0xEE 即红。 */
		const size_t frame_bytes = size_t(w) * h * 3u;
		static constexpr size_t kGuardBytes = 16u;
		static constexpr uint8_t kSentinel = 0xEEu;
		std::vector<uint8_t> a(frame_bytes + kGuardBytes, kSentinel), b(frame_bytes + kGuardBytes, kSentinel);
		Rgb24FromI420(y.data(), w, u.data(), cw, v.data(), cw, w, h, a.data());
		Rgb24FromI420Scalar(y.data(), w, u.data(), cw, v.data(), cw, w, h, b.data());
		// 首字节与末写入字节：写入窗口两端都在缓冲里，两支同值（逐字节全等含守卫带，见下一行）
		EXPECT_EQ(a.front(), b.front()) << "w=" << w << " h=" << h;
		EXPECT_EQ(a[frame_bytes - 1u], b[frame_bytes - 1u]) << "w=" << w << " h=" << h;
		EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size()), 0) << "w=" << w << " h=" << h;
		// 两支都不许越出输出缓冲：守卫带一格都不许被写过
		for (size_t i = 0; i < kGuardBytes; ++i)
		{
			EXPECT_EQ(a[frame_bytes + i], kSentinel)
				<< "w=" << w << " h=" << h << " 向量支越界写进守卫带第 " << i << " 字节";
			EXPECT_EQ(b[frame_bytes + i], kSentinel)
				<< "w=" << w << " h=" << h << " 标量支越界写进守卫带第 " << i << " 字节";
		}
	}

	TEST(VideoParamsAndRgb24, Rgb24VectorBranchMatchesScalarBranch)
	{
		for (uint32_t w: { 8u, 16u, 17u, 24u, 31u, 64u, 65u })
			for (uint32_t h: { 2u, 3u, 8u }) ExpectBothBranchesEqual(w, h);
	}

	TEST(VideoParamsAndRgb24, Rgb24GainFormulaMatchesDocumentedKnownPoint)
	{
		// Y = U = V = 128 ⇒ 三道偏移全为 0 ⇒ 输出恒 128
		const uint32_t w = 16u, h = 4u, cw = 8u, ch = 2u;
		std::vector<uint8_t> y(size_t(w) * h, 128u), u(size_t(cw) * ch, 128u), v(size_t(cw) * ch, 128u),
			out(size_t(w) * h * 3u, 0u);
		Rgb24FromI420(y.data(), w, u.data(), cw, v.data(), cw, w, h, out.data());
		for (size_t i = 0; i < out.size(); ++i) EXPECT_EQ(out[i], 128u) << "i=" << i;

		// 满幅黑 + 色度下限：R = clip(16 + (1436*(16-128)>>10)) = clip(16 - 157) = 0
		std::fill(y.begin(), y.end(), uint8_t(16u));
		std::fill(u.begin(), u.end(), uint8_t(16u));
		std::fill(v.begin(), v.end(), uint8_t(16u));
		Rgb24FromI420(y.data(), w, u.data(), cw, v.data(), cw, w, h, out.data());
		EXPECT_EQ(out[0], 0u);
		EXPECT_EQ(out[2], 0u);
	}

	TEST(VideoParamsAndRgb24, Rgb24EntryRejectsBadArgumentsWithoutWriting)
	{
		CLV_VideoFrameDescC d {};
		std::vector<uint8_t> buf(16u * 4u * 3u, 0xA5u);
		d.pixel_format = CLV_VIDEO_PIX_YUV420P;
		d.bit_depth = 8u;
		d.coded_width = 16u;
		d.coded_height = 4u;
		d.display_width = 16u;
		d.display_height = 4u;
		d.stride[0] = 16u;
		d.stride[1] = d.stride[2] = 8u;
		std::vector<uint8_t> pl0(16u * 4u, 128u), pl1(8u * 2u, 128u), pl2(8u * 2u, 128u);
		d.plane[0] = pl0.data();
		d.plane[1] = pl1.data();
		d.plane[2] = pl2.data();

		EXPECT_EQ(CLV_VideoFrameToRGB24(nullptr, buf.data()), CLV_VIDEO_INVALID_ARGUMENT);
		EXPECT_EQ(CLV_VideoFrameToRGB24(&d, nullptr), CLV_VIDEO_INVALID_ARGUMENT);
		d.bit_depth = 10u;
		EXPECT_EQ(CLV_VideoFrameToRGB24(&d, buf.data()), CLV_VIDEO_BIT_DEPTH_UNSUPPORTED);
		d.bit_depth = 8u;
		d.plane[2] = nullptr;
		EXPECT_EQ(CLV_VideoFrameToRGB24(&d, buf.data()), CLV_VIDEO_INVALID_ARGUMENT);
		d.plane[2] = pl2.data();
		d.display_width = d.display_height = 0u;
		d.coded_width = d.coded_height = 0u;
		EXPECT_EQ(CLV_VideoFrameToRGB24(&d, buf.data()), CLV_VIDEO_VALUE_RANGE);
		for (size_t i = 0; i < buf.size(); ++i)
			if (buf[i] != 0xA5u) FAIL() << "入参不成立的一趟写了 dst，首个改动在下标 " << i;
	}

}	 // namespace
