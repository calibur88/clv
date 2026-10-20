/* test_video_payload.cpp
 *
 * **两面混用**：端到端与句柄行为走公开 C ABI（`CLV_Video*` 族 15 个入口、实测 148 处调用，如 `CLV_VideoBitstreamGetBytes`、`CLV_VideoBitstreamGetSlice`、`CLV_VideoBitstreamRelease`），
 *   单元级直测走实现层头（`video/block.h`、`video/encoder_hook.h`、`video/macroblock.h`、`video/padding.h`、`video/payload.h`、`video/quant.h`、`video/reconstruct.h`、`video/status.h`）。**仅静态构建（`CLV_BUILD_SHARED=OFF`）下可链**的原因是后面这些头，
 *   不是「不经公开 ABI」——两面各写各的：主体过公开 C ABI，另有几处直调实现层函数钉住不进 ABI 的行为。
 *
 * VOP 载荷 = 帧头 + 宏块序列的编解码，以及编码器句柄到语法层的端到端。
 * 块数据的长度由宏块头里的 block_size 自定界，故读方只拿载荷字节 + mb_count 就能一段段切开。
 */

#include "CLV_Video.h"
#include "test_config_expect.h"
#include "video/block.h"
#include "video/encoder_hook.h"
#include "video/macroblock.h"
#include "video/padding.h"
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

	using clv::video::BvopMacroblock;
	using clv::video::DecodeVopPayload;
	using clv::video::EncodeVopPayload;
	using clv::video::Macroblock;
	using clv::video::MotionVector;
	using clv::video::VideoErr;
	using clv::video::VopHeader;
	using clv::video::VopPayload;

	constexpr uint8_t kQp = 7u;

	VopHeader MakeHead(uint8_t type, uint64_t mb_count)
	{
		VopHeader h;
		h.type = type;
		h.quant = 24u;
		h.fcode_forward = 1u;
		h.fcode_backward = 1u;
		h.intra_dc_vlc_thr = 0u;
		h.dquant = 0;
		h.mb_count = mb_count;
		return h;
	}

	std::vector<uint8_t> Patterned(size_t n, uint8_t seed)
	{
		std::vector<uint8_t> v(n);
		for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 7u + seed) & 0xFFu);
		return v;
	}

	MotionVector Mv(int32_t h, int32_t v)
	{
		MotionVector m;
		m.h = h;
		m.v = v;
		return m;
	}

	/* 帧内块系数的期望载体 = **冻结件**，由 scripts/gen_video_payload_fixtures.py 按规范「块数据的系数
	 * 载体」那一节独立重写一遍「取块 → 减 128 → Hadamard8 → 最近电平量化 → 定长直写」后写进
	 * video_payload_fixtures_p.h，用例只比字面量。
	 * 口径记在这里备查：q' = 24 落帧内 17..32 档 ⇒ 每系数 9 bit ⇒ 单块 64 × 9 ÷ 8 = 72 B、
	 * 六块全在场（位图 0x3F）⇒ 一宏块 6 × 72 = 432 B。 */
	std::vector<uint8_t> FrozenIntraCarrier(const unsigned char* bytes, uint32_t len)
	{
		return std::vector<uint8_t>(bytes, bytes + len);
	}

	/* 有效区里的总绝对差。度量必须进断言，不能只往 stdout 报一句——报出去的数字漂了没人拦，
	 * 而「差异非零」这一族的判据就是两条并列：不等点数 > 0 与总绝对差 > 0，缺一条就退化成打印。 */
	size_t TotalAbsDiff(CLV_VideoFrame& a, CLV_VideoFrame& b, uint32_t w, uint32_t h)
	{
		CLV_VideoFrameDescC da, db;
		std::memset(&da, 0, sizeof(da));
		std::memset(&db, 0, sizeof(db));
		if (CLV_VideoFrameGetDesc(&a, &da, nullptr) != CLV_VIDEO_OK) return 0u;
		if (CLV_VideoFrameGetDesc(&b, &db, nullptr) != CLV_VIDEO_OK) return 0u;
		const uint32_t pw[3] = { w, w / 2u, w / 2u };
		const uint32_t ph[3] = { h, h / 2u, h / 2u };
		size_t sum = 0u;
		for (uint32_t pl = 0u; pl < 3u; ++pl)
		{
			for (uint32_t r = 0u; r < ph[pl]; ++r)
			{
				const uint8_t* pa = da.plane[pl] + static_cast<size_t>(r) * da.stride[pl];
				const uint8_t* pb = db.plane[pl] + static_cast<size_t>(r) * db.stride[pl];
				for (uint32_t c = 0u; c < pw[pl]; ++c)
					sum += static_cast<size_t>(pa[c] > pb[c] ? pa[c] - pb[c] : pb[c] - pa[c]);
			}
		}
		return sum;
	}

	/* 缓冲不同源守卫：编码器内部重建帧与解码输出必须各占一块内存。
	 * 实现层钩子若把解码输出的平面别名回来，「逐字节全等」就退化成自己比自己、恒真。
	 * 三平面逐条判，判在比较之前——红了要说清是哪一块撞了。 */
	void ExpectSeparateBuffers(CLV_VideoFrame& rec, CLV_VideoFrame& decoded, const char* what)
	{
		CLV_VideoFrameDescC dr, dd;
		std::memset(&dr, 0, sizeof(dr));
		std::memset(&dd, 0, sizeof(dd));
		ASSERT_EQ(CLV_VideoFrameGetDesc(&rec, &dr, nullptr), CLV_VIDEO_OK) << what << "：重建帧取不到描述符";
		ASSERT_EQ(CLV_VideoFrameGetDesc(&decoded, &dd, nullptr), CLV_VIDEO_OK) << what << "：解码输出取不到描述符";
		for (int p = 0; p < 3; ++p)
			ASSERT_NE(dr.plane[p], dd.plane[p]) << what << "：平面 " << p << " 撞同一块缓冲，全等断言失去意义";
	}

	/* 两帧的像素逐字节比（只比有效区，不比补边列）。 */
	size_t CompareFrames(CLV_VideoFrame& a, CLV_VideoFrame& b, uint32_t w, uint32_t h)
	{
		CLV_VideoFrameDescC da, db;
		std::memset(&da, 0, sizeof(da));
		std::memset(&db, 0, sizeof(db));
		if (CLV_VideoFrameGetDesc(&a, &da, nullptr) != CLV_VIDEO_OK) return 0u;
		if (CLV_VideoFrameGetDesc(&b, &db, nullptr) != CLV_VIDEO_OK) return 0u;
		const uint32_t pw[3] = { w, w / 2u, w / 2u };
		const uint32_t ph[3] = { h, h / 2u, h / 2u };
		size_t same = 0u;
		for (uint32_t pl = 0u; pl < 3u; ++pl)
		{
			for (uint32_t r = 0u; r < ph[pl]; ++r)
			{
				const uint8_t* pa = da.plane[pl] + static_cast<size_t>(r) * da.stride[pl];
				const uint8_t* pb = db.plane[pl] + static_cast<size_t>(r) * db.stride[pl];
				for (uint32_t c = 0u; c < pw[pl]; ++c)
					if (pa[c] == pb[c]) ++same;
			}
		}
		return same;
	}

	/* 高频面：差异非零（源 vs 重建必须不等）用它。 */
	void WriteDensePattern(CLV_VideoFrame* f, uint32_t w, uint32_t h)
	{
		CLV_VideoFrameDescC desc;
		std::memset(&desc, 0, sizeof(desc));
		if (CLV_VideoFrameGetDesc(f, &desc, nullptr) != CLV_VIDEO_OK) return;
		const uint32_t pw[3] = { w, w / 2u, w / 2u };
		const uint32_t ph[3] = { h, h / 2u, h / 2u };
		for (uint32_t pl = 0u; pl < 3u; ++pl)
		{
			for (uint32_t r = 0u; r < ph[pl]; ++r)
			{
				uint8_t* row = desc.plane[pl] + static_cast<size_t>(r) * desc.stride[pl];
				for (uint32_t c = 0u; c < pw[pl]; ++c)
				{
					const uint32_t v = r * 37u + c * 91u + ((r ^ c) * 13u) + pl * 61u;
					row[c] = static_cast<uint8_t>(v & 0xFFu);
				}
			}
		}
	}

}	 // namespace

TEST(VideoPayload, RoundTripsForIAndPWithPatternedBlocks)
{
	size_t points = 0u;
	for (uint8_t type: { clv::video::kVopTypeI, clv::video::kVopTypeP })
	{
		VopPayload in;
		in.head = MakeHead(type, 4u);
		for (uint32_t i = 0; i < 4u; ++i)
		{
			Macroblock m;
			// I-VOP 无 skip 语义：帧内全零宏块走块模式全零，not_coded 只在 P 支出现
			m.head.not_coded = (type == clv::video::kVopTypeP && i == 2u);
			m.head.mb_type =
				(type == clv::video::kVopTypeI || i == 1u) ? clv::video::kMbTypeIntra : clv::video::kMbTypeInter;
			m.head.cbpc = static_cast<uint8_t>(i & 3u);
			m.head.cbpy = static_cast<uint8_t>((i * 5u) & 0xFu);
			m.head.dquant = static_cast<int8_t>(i % 4u) - 2;
			if (! m.head.not_coded)
			{
				m.block = Patterned(1u + i * 3u, static_cast<uint8_t>(i));
				m.head.block_size = static_cast<uint32_t>(m.block.size());
			}
			in.macroblocks.push_back(std::move(m));
		}

		std::vector<uint8_t> bytes;
		ASSERT_EQ(EncodeVopPayload(kQp, 1u, in, &bytes), VideoErr::kOk) << "type " << int(type);

		VopPayload out;
		size_t consumed = 0u;
		ASSERT_EQ(DecodeVopPayload(bytes.data(), bytes.size(), kQp, 1u, &out, &consumed), VideoErr::kOk);
		EXPECT_EQ(consumed, bytes.size());
		EXPECT_EQ(out.head.type, in.head.type);
		EXPECT_EQ(out.head.quant, in.head.quant);
		EXPECT_EQ(out.head.mb_count, in.head.mb_count);
		ASSERT_EQ(out.macroblocks.size(), in.macroblocks.size());
		for (size_t i = 0; i < in.macroblocks.size(); ++i)
		{
			EXPECT_EQ(out.macroblocks[i].head.not_coded, in.macroblocks[i].head.not_coded);
			if (! in.macroblocks[i].head.not_coded)
			{
				EXPECT_EQ(out.macroblocks[i].head.mb_type, in.macroblocks[i].head.mb_type);
				EXPECT_EQ(out.macroblocks[i].head.cbpc, in.macroblocks[i].head.cbpc);
				EXPECT_EQ(out.macroblocks[i].head.cbpy, in.macroblocks[i].head.cbpy);
				EXPECT_EQ(out.macroblocks[i].head.dquant, in.macroblocks[i].head.dquant);
				EXPECT_EQ(out.macroblocks[i].head.block_size, in.macroblocks[i].head.block_size);
				++points;
			}
			EXPECT_EQ(out.macroblocks[i].block, in.macroblocks[i].block) << "第 " << i << " 个宏块的块数据";
			++points;
		}

		std::vector<uint8_t> again;
		ASSERT_EQ(EncodeVopPayload(kQp, 1u, out, &again), VideoErr::kOk);
		EXPECT_EQ(again, bytes);
	}
	// I 型四个宏块全已编码 = 4 次块数据 + 4 次字段组 = 8；P 型的第 3 个走 not_coded = 4 + 3 = 7；合计 15
	EXPECT_EQ(points, 15u);
	std::cout << "载荷往返：I 与 P 两型各 4 个宏块，逐字段与逐字节全等，再编一次逐字节相同；比对点 = " << points
			  << "\n";
}

TEST(VideoPayload, RejectsCountMismatchAndTrailingBytes)
{
	VopPayload in;
	in.head = MakeHead(clv::video::kVopTypeP, 3u);	  // 声明三个却只给两个
	Macroblock m;
	m.head.mb_type = clv::video::kMbTypeInter;
	m.head.block_size = 1u;
	m.block = { 0x42u };
	in.macroblocks.push_back(m);
	in.macroblocks.push_back(m);
	std::vector<uint8_t> bytes;
	EXPECT_EQ(EncodeVopPayload(kQp, 1u, in, &bytes), VideoErr::kInvalidArgument);

	in.head.mb_count = 2u;
	ASSERT_EQ(EncodeVopPayload(kQp, 1u, in, &bytes), VideoErr::kOk);
	bytes.push_back(0x00u);	   // 尾部多一个整字节
	VopPayload out;
	EXPECT_EQ(DecodeVopPayload(bytes.data(), bytes.size(), kQp, 1u, &out, nullptr), VideoErr::kHeaderBad);
}

TEST(VideoPayload, BvopPredictorStateContinuesAcrossMacroblocks)
{
	// 2 × 2 的宏块网格，四个 interpolate 宏块各带绝对矢量；预测器按光栅序取左 / 上 / 右上邻居
	const uint32_t mb_width = 2u;
	const MotionVector f[4] = { Mv(3, -4), Mv(11, 2), Mv(-5, 7), Mv(1, 1) };
	const MotionVector b[4] = { Mv(-2, 5), Mv(6, -9), Mv(0, 0), Mv(13, 13) };

	VopPayload in;
	in.head = MakeHead(clv::video::kVopTypeB, 4u);
	for (uint32_t i = 0; i < 4u; ++i)
	{
		BvopMacroblock m;
		m.head.mb_type = clv::video::kBvopInterpolate;
		m.head.forward = f[i];
		m.head.backward = b[i];
		m.head.cbpy = 0xFu;
		m.block = Patterned(2u + i, static_cast<uint8_t>(i));
		m.head.block_size = static_cast<uint32_t>(m.block.size());
		in.bvops.push_back(std::move(m));
	}

	std::vector<uint8_t> bytes;
	ASSERT_EQ(EncodeVopPayload(kQp, mb_width, in, &bytes), VideoErr::kOk);

	VopPayload back;
	ASSERT_EQ(DecodeVopPayload(bytes.data(), bytes.size(), kQp, mb_width, &back, nullptr), VideoErr::kOk);
	ASSERT_EQ(back.bvops.size(), 4u);
	for (size_t i = 0; i < 4u; ++i)
	{
		EXPECT_EQ(back.bvops[i].head.forward.h, f[i].h) << "第 " << i << " 个宏块前向 h";
		EXPECT_EQ(back.bvops[i].head.forward.v, f[i].v) << "第 " << i << " 个宏块前向 v";
		EXPECT_EQ(back.bvops[i].head.backward.h, b[i].h) << "第 " << i << " 个宏块后向 h";
		EXPECT_EQ(back.bvops[i].head.backward.v, b[i].v) << "第 " << i << " 个宏块后向 v";
		EXPECT_EQ(back.bvops[i].block, in.bvops[i].block);
	}

	// 换一个列数：邻居取法变了，残差解出来的绝对矢量必须跟着变（证明状态真的跨宏块延续）
	VopPayload shifted;
	ASSERT_EQ(DecodeVopPayload(bytes.data(), bytes.size(), kQp, 4u, &shifted, nullptr), VideoErr::kOk);
	bool differs = false;
	for (size_t i = 0; i < 4u; ++i)
		if (shifted.bvops[i].head.forward.h != f[i].h || shifted.bvops[i].head.backward.h != b[i].h) differs = true;
	EXPECT_TRUE(differs);
	std::cout << "B-VOP 预测器状态：mb_width 2 逐矢量等值，改按 4 列解则结果改变\n";
}

TEST(VideoPayload, EncoderHandleEndToEndAtSyntaxLevel)
{
	CLV_VideoEncParamsC p;
	std::memset(&p, 0, sizeof(p));
	p.display_width = 176u;
	p.display_height = 144u;
	p.bit_depth = 8u;
	p.quant_precision = kQp;
	p.default_quant = 24u;
	p.max_b_frames = 0u;
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&p, &err);
	ASSERT_NE(e, nullptr);

	CLV_VideoFrame* f = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
	ASSERT_NE(f, nullptr);
	CLV_VideoFrameDescC desc;
	std::memset(&desc, 0, sizeof(desc));
	ASSERT_EQ(CLV_VideoFrameGetDesc(f, &desc, nullptr), CLV_VIDEO_OK);
	// 行距按 32 字节对齐：176 → 192，色度 88 → 96
	EXPECT_EQ(desc.stride[0], 192u);
	EXPECT_EQ(desc.stride[1], 96u);
	EXPECT_EQ(desc.coded_width, 176u);
	EXPECT_EQ(desc.coded_height, 144u);
	// AllocFrame 交出的是黑电平（Y = 16、Cb/Cr = 128），色度残差恒零 ⇒ 色度块不在场。
	// 冻结件按「三平面每像素都是 0」算出，故这里显式写零，让输入与冻结件的建模口径一致。
	for (uint32_t pl = 0u; pl < 3u; ++pl)
	{
		const uint32_t plane_rows[3] = { desc.coded_height, desc.coded_height / 2u, desc.coded_height / 2u };
		for (uint32_t r = 0u; r < plane_rows[pl]; ++r)
			std::memset(desc.plane[pl] + static_cast<size_t>(r) * desc.stride[pl], 0, desc.stride[pl]);
	}

	CLV_VideoBitstream* bs = nullptr;
	ASSERT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_I, 24u, 0, 0, &bs), CLV_VIDEO_OK);
	ASSERT_NE(bs, nullptr);
	uint32_t slice_count = 0u;
	ASSERT_EQ(CLV_VideoBitstreamSliceCount(bs, &slice_count), CLV_VIDEO_OK);
	EXPECT_EQ(slice_count, 1u);	   // 整帧一片
	const uint8_t* data = nullptr;
	size_t size = 0u;
	ASSERT_EQ(CLV_VideoBitstreamGetBytes(bs, &data, &size), CLV_VIDEO_OK);
	const uint8_t* slice = nullptr;
	size_t slice_size = 0u;
	ASSERT_EQ(CLV_VideoBitstreamGetSlice(bs, 0u, &slice, &slice_size), CLV_VIDEO_OK);
	EXPECT_EQ(slice, data);
	EXPECT_EQ(slice_size, size);

	// 解码吃回：按 mb_count 自解析，逐宏块逐字节比到帧缓冲的平面上
	VopPayload back;
	size_t consumed = 0u;
	ASSERT_EQ(DecodeVopPayload(data, size, kQp, 11u, &back, &consumed), VideoErr::kOk);
	EXPECT_EQ(consumed, size);
	EXPECT_EQ(back.head.type, clv::video::kVopTypeI);
	EXPECT_EQ(back.head.mb_count, 99u);	   // 176/16 × 144/16 = 11 × 9
	ASSERT_EQ(back.macroblocks.size(), 99u);
	// 块数据按帧头声明的那法写（本法 = EOB 截尾 + EG 幅值），长度与内容都由量化链路决定。
	// 期望载体取自 video_payload_fixtures_p.h 的冻结件（生成件独立重写整条链路，不在这里现算）。
	// 本例的输入是上面逐平面写零的那张图 ⇒ 像素全 0 ⇒ 99 个宏块共用同一份载体。
	const std::vector<uint8_t> want = FrozenIntraCarrier(clvtest::kZeroFrameMbCarrier, clvtest::kZeroFrameMbCarrierLen);
	size_t compared = 0u;
	size_t present_blocks = 0u;
	for (uint32_t i = 0; i < clvtest::kZeroFrameMbMbCount; ++i)
	{
		const uint32_t mx = i % 11u, my = i / 11u;
		// 块模式：I / P 侧 = cbpy（4 bit 亮度）<<2 | cbpc（2 bit 色度），与合成位序同构
		EXPECT_EQ(back.macroblocks[i].head.cbpy, static_cast<uint8_t>(clvtest::kZeroFrameMbCbpy));
		EXPECT_EQ(back.macroblocks[i].head.cbpc, static_cast<uint8_t>(clvtest::kZeroFrameMbCbpc));
		// block_size = 本档载体实长（法 1 = 18 B / 定宽 = 432 B），选档只发生在 test_config_expect.h 一处
		EXPECT_EQ(back.macroblocks[i].head.block_size, clvtest::kPayloadAllZeroMB);
		EXPECT_EQ(back.macroblocks[i].block, want) << "第 " << i << " 个宏块（mb_x=" << mx << ", mb_y=" << my << "）";
		compared += clvtest::kZeroFrameMbCarrierLen;
		present_blocks += clvtest::kZeroFrameMbPresentBlocks;
	}
	// 合计也挂冻结件：默认的法 1 档 99 × 18 = 1 782 B、99 × 6 = 594 块（定宽档同式是 99 × 432 = 42 768 B）
	EXPECT_EQ(compared, clvtest::kZeroFrameMbFrameCarrierTotal);
	EXPECT_EQ(present_blocks, clvtest::kZeroFrameMbFramePresentBlocks);
	std::cout << "句柄端到端：载荷 " << size << " B，99 个宏块系数载体合计 " << compared << " B（在场块 "
			  << present_blocks << " / " << clvtest::kZeroFrameMbMbCount * clvtest::kZeroFrameMbPresentBlocks
			  << "，q' = 24 → 定宽那一档单块 " << clvtest::kCarrierBlockBytesAt24 << " B，本法载体实长取自冻结件）\n";

	// 码流缓冲归调用方所有：编码器 Close 不替你释放
	CLV_VideoBitstreamRelease(bs);
	CLV_VideoFrameRelease(f);
	CLV_VideoEncoderClose(e);
}

TEST(VideoPayload, EncoderHandleInputChecks)
{
	CLV_VideoEncParamsC p;
	std::memset(&p, 0, sizeof(p));
	p.display_width = 32u;
	p.display_height = 16u;
	p.bit_depth = 8u;
	p.quant_precision = kQp;
	p.default_quant = 8u;
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&p, &err);
	ASSERT_NE(e, nullptr);
	CLV_VideoFrame* f = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
	CLV_VideoBitstream* bs = reinterpret_cast<CLV_VideoBitstream*>(1u);

	EXPECT_EQ(CLV_VideoEncoderEncode(nullptr, f, CLV_VIDEO_FRAME_I, 8u, 0, 0, &bs), CLV_VIDEO_INVALID_HANDLE);
	EXPECT_EQ(bs, nullptr);
	EXPECT_EQ(CLV_VideoEncoderEncode(e, nullptr, CLV_VIDEO_FRAME_I, 8u, 0, 0, &bs), CLV_VIDEO_INVALID_HANDLE);
	// 逐调用侧的越界走 84（声明侧 default_quant 越界才是 69）
	EXPECT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_I, 0u, 0, 0, &bs), CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL);
	EXPECT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_I, 128u, 0, 0, &bs), CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL);
	EXPECT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_I, 8u, 0, 0, nullptr), CLV_VIDEO_INVALID_ARGUMENT);
	// 无参考可依的位置请求帧间 = 顺序不成立：这样的载荷本库解码侧按参考帧缺失丢帧，入口就拒
	EXPECT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_P, 8u, 0, 0, &bs), CLV_VIDEO_STATE_ERROR);
	// 先落一个 I 建立参考，帧档才排得上
	ASSERT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_I, 8u, 0, 0, &bs), CLV_VIDEO_OK);
	CLV_VideoBitstreamRelease(bs);
	bs = nullptr;
	ASSERT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_P, 8u, 100, 100, &bs), CLV_VIDEO_OK);
	CLV_VideoBitstream* first = bs;
	bs = nullptr;
	// 时间轴不后退
	EXPECT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_P, 8u, 50, 50, &bs), CLV_VIDEO_STATE_ERROR);
	// 负 composition offset 合法：PTS 小于 DTS 不判入参错
	ASSERT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_P, 8u, 200, 150, &bs), CLV_VIDEO_OK);
	// 失败调用不得覆盖出参：STATE_ERROR 那次之后 bs 仍是本次产出的对象，与前一次不同
	EXPECT_NE(bs, first);
	// 归调用方的两块都要释放（编码器不持有）
	CLV_VideoBitstreamRelease(first);
	CLV_VideoBitstreamRelease(bs);

	CLV_VideoFrameRelease(f);
	CLV_VideoEncoderClose(e);

	// 每片宏块数是编码器配置：非 0 即真分片，片数越上限 65 535 才判值域不成立
	p.slice_mb_count = 4u;
	CLV_VideoEncoder* e2 = CLV_VideoEncoderOpen(&p, &err);
	EXPECT_NE(e2, nullptr);
	EXPECT_EQ(err, CLV_VIDEO_OK);
	CLV_VideoEncoderClose(e2);
	p.slice_mb_count = 0u;
	p.bit_depth = 10u;
	EXPECT_EQ(CLV_VideoEncoderOpen(&p, &err), nullptr);
	EXPECT_EQ(err, CLV_VIDEO_BIT_DEPTH_UNSUPPORTED);
	p.bit_depth = 8u;
	p.display_width = 0u;
	EXPECT_EQ(CLV_VideoEncoderOpen(&p, &err), nullptr);
	EXPECT_EQ(err, CLV_VIDEO_VALUE_RANGE);
}

TEST(VideoPayload, EncoderHandleWritesBvopSyntaxForBFrames)
{
	CLV_VideoEncParamsC p;
	std::memset(&p, 0, sizeof(p));
	p.display_width = 16u;
	p.display_height = 16u;
	p.bit_depth = 8u;
	p.quant_precision = kQp;
	p.default_quant = 12u;
	// max_b_frames 是流的声明：声明 0 的编码器不得出 B 帧，本例只出 1 个 B ⇒ 声明 1
	p.max_b_frames = 1u;
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&p, &err);
	ASSERT_NE(e, nullptr);
	CLV_VideoFrame* f = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
	CLV_VideoBitstream* bs = nullptr;
	// 无参考位置的 B 请求在入口就拒：max_b_frames 声明够用，缺的是参考可依
	EXPECT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_B, 12u, 0, 0, &bs), CLV_VIDEO_STATE_ERROR);
	// 先落一个 I 建立参考；B 的载荷语法与参考在不在无关，下面读回的还是同一份形状
	CLV_VideoBitstream* seed = nullptr;
	ASSERT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_I, 12u, 0, 0, &seed), CLV_VIDEO_OK);
	CLV_VideoBitstreamRelease(seed);
	ASSERT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_B, 12u, 0, 0, &bs), CLV_VIDEO_OK);
	const uint8_t* data = nullptr;
	size_t size = 0u;
	ASSERT_EQ(CLV_VideoBitstreamGetBytes(bs, &data, &size), CLV_VIDEO_OK);

	VopPayload back;
	ASSERT_EQ(DecodeVopPayload(data, size, kQp, 1u, &back, nullptr), VideoErr::kOk);
	EXPECT_EQ(back.head.type, clv::video::kVopTypeB);
	ASSERT_EQ(back.bvops.size(), 1u);
	EXPECT_EQ(back.bvops[0].head.mb_type, clv::video::kBvopDirect);
	// 编码侧 B 帧一律 direct + not_coded，不带残差、不带块数据
	EXPECT_TRUE(back.bvops[0].head.not_coded);
	EXPECT_EQ(back.bvops[0].head.cbpy, 0u);
	EXPECT_EQ(back.bvops[0].head.block_size, 0u);
	EXPECT_EQ(back.bvops[0].block.size(), 0u);
	std::cout << "B 帧句柄：载荷 " << size << " B，1 个 direct + not_coded 宏块（B 预测块生成运动补偿落地后）\n";

	CLV_VideoBitstreamRelease(bs);
	CLV_VideoFrameRelease(f);
	CLV_VideoEncoderClose(e);
}

TEST(VideoPayload, EncoderConsumesBytesWrittenThroughDescriptorPlaneThroughToReconstruction)
{
	// 16×16 = 一个宏块。块数据不是源像素 384 B 的直存：写进 desc.plane 的像素要过变换 + 量化，
	// 长度与内容由量化链路决定。核对链路为：写入像素 → 编码（变换 + 量化 + 定长载体）→
	// 解码（反量化 + 反变换 + 夹取）→ 与编码器内部重建逐字节全等，
	// 即编解两侧逐字节全等在「可写平面」入口下的变体。
	CLV_VideoEncParamsC p {};
	p.display_width = 16u;
	p.display_height = 16u;
	p.bit_depth = 8u;
	p.quant_precision = kQp;
	p.default_quant = 24u;
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&p, &err);
	ASSERT_NE(e, nullptr);
	CLV_VideoFrame* f = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
	CLV_VideoFrameDescC desc;
	std::memset(&desc, 0, sizeof(desc));
	ASSERT_EQ(CLV_VideoFrameGetDesc(f, &desc, nullptr), CLV_VIDEO_OK);
	ASSERT_EQ(desc.coded_width, 16u);
	ASSERT_EQ(desc.coded_height, 16u);

	const uint32_t plane_w[3] = { 16u, 8u, 8u };
	const uint32_t plane_h[3] = { 16u, 8u, 8u };
	for (uint32_t pl = 0u; pl < 3u; ++pl)
	{
		ASSERT_NE(desc.plane[pl], nullptr);
		for (uint32_t r = 0u; r < plane_h[pl]; ++r)
		{
			uint8_t* row = desc.plane[pl] + static_cast<size_t>(r) * desc.stride[pl];
			for (uint32_t c = 0u; c < plane_w[pl]; ++c) row[c] = static_cast<uint8_t>(0x10u + pl * 0x20u + r * 3u + c);
		}
	}

	CLV_VideoBitstream* bs = nullptr;
	ASSERT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_I, 24u, 0, 0, &bs), CLV_VIDEO_OK);
	const uint8_t* data = nullptr;
	size_t size = 0u;
	ASSERT_EQ(CLV_VideoBitstreamGetBytes(bs, &data, &size), CLV_VIDEO_OK);

	// 语法面：块数据按帧头声明的那法写，期望字节取自同一件里的另一档数组（选档只在 test_config_expect.h 一处）
	VopPayload back;
	size_t consumed = 0u;
	ASSERT_EQ(DecodeVopPayload(data, size, kQp, 1u, &back, &consumed), VideoErr::kOk);
	ASSERT_EQ(back.macroblocks.size(), 1u);
	const std::vector<uint8_t> want = FrozenIntraCarrier(clvtest::kPlane16MbCarrier, clvtest::kPlane16MbCarrierLen);
	ASSERT_FALSE(want.empty());
	EXPECT_EQ(back.macroblocks[0].block, want);
	// block_size 吃的是码流里解出来的那一格，与本档载体实长对平；两常量同源的恒等式不当判据
	//（那条关系归 test_config_expect.h 的 static_assert 在编译期核，这里核的是码流对得上它）
	EXPECT_EQ(back.macroblocks[0].head.block_size, clvtest::kPayloadPlane16MB);
	EXPECT_EQ(back.macroblocks[0].head.cbpy, static_cast<uint8_t>(clvtest::kPlane16MbCbpy));
	EXPECT_EQ(back.macroblocks[0].head.cbpc, static_cast<uint8_t>(clvtest::kPlane16MbCbpc));

	// 像素面：解码重建 == 编码器内部重建
	CLV_VideoStreamParamsC dp;
	std::memset(&dp, 0, sizeof(dp));
	dp.bit_depth = 8u;
	dp.quant_precision = kQp;
	dp.max_b_frames = 0u;
	dp.obmc_disable = 1u;
	dp.display_width = 16u;
	dp.display_height = 16u;
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&dp, &err);
	ASSERT_NE(d, nullptr);
	ASSERT_EQ(CLV_VideoDecoderDecode(d, data, size, 1u, 0, 0, nullptr), CLV_VIDEO_OK);
	CLV_VideoFrame* got = nullptr;
	uint8_t flag = 0u;
	ASSERT_EQ(CLV_VideoDecoderFlush(d, &got, &flag), CLV_VIDEO_OK);
	ASSERT_EQ(flag, 1u);
	CLV_VideoFrame* rec = clv_internal_encoder_reconstruction(e);
	ASSERT_NE(rec, nullptr);
	ExpectSeparateBuffers(*rec, *got, "可写平面核");
	EXPECT_EQ(CompareFrames(*rec, *got, 16u, 16u), 16u * 16u + 2u * (8u * 8u));
	std::cout << "可写平面核：写 " << (16u * 16u + 2u * 8u * 8u) << " B 进 desc.plane，载体 "
			  << back.macroblocks[0].block.size() << " B，解码重建与编码器内部重建逐字节全等\n";

	CLV_VideoBitstreamRelease(bs);	  // data 指向它的字节，故留到两次消费之后
	CLV_VideoFrameRelease(got);
	CLV_VideoFrameRelease(f);
	CLV_VideoDecoderClose(d);
	CLV_VideoEncoderClose(e);
}

TEST(VideoPayload, BitstreamBelongsToCallerUntilReleased)
{
	CLV_VideoEncParamsC p {};
	p.display_width = 16u;
	p.display_height = 16u;
	p.bit_depth = 8u;
	p.quant_precision = kQp;
	p.default_quant = 24u;
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&p, &err);
	ASSERT_NE(e, nullptr);
	CLV_VideoFrame* f = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);

	CLV_VideoBitstream* a = nullptr;
	CLV_VideoBitstream* b = nullptr;
	ASSERT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_I, 24u, 0, 0, &a), CLV_VIDEO_OK);
	ASSERT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_P, 24u, 100, 100, &b), CLV_VIDEO_OK);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	// 两次产出是两个独立对象，各自归调用方
	EXPECT_NE(a, b);
	const uint8_t* da = nullptr;
	const uint8_t* db = nullptr;
	size_t sa = 0u, sb = 0u;
	ASSERT_EQ(CLV_VideoBitstreamGetBytes(a, &da, &sa), CLV_VIDEO_OK);
	ASSERT_EQ(CLV_VideoBitstreamGetBytes(b, &db, &sb), CLV_VIDEO_OK);
	EXPECT_NE(da, db);

	// 先释放 a：b 不受影响，仍可读出原长度 —— 编码器没有持有它们的表
	CLV_VideoBitstreamRelease(a);
	const uint8_t* db2 = nullptr;
	size_t sb2 = 0u;
	EXPECT_EQ(CLV_VideoBitstreamGetBytes(b, &db2, &sb2), CLV_VIDEO_OK);
	EXPECT_EQ(sb2, sb);
	EXPECT_EQ(db2, db);
	CLV_VideoBitstreamRelease(b);
	// 对 NULL 是 no-op
	CLV_VideoBitstreamRelease(nullptr);
	std::cout << "码流所有权：两块长度 " << sa << " B / " << sb << " B\n";
	std::cout << "各自 Release，释放一块不影响另一块\n";

	CLV_VideoFrameRelease(f);
	CLV_VideoEncoderClose(e);
}

/* ================= 多片载荷：解码器与多片载荷 =================
 *
 * 块数据是系数的定长载体：下面这些往返例既核对载荷级搬运自洽（整帧 = 各片按序拼接、片界由片头给出），
 * 也核对有损重建一致性——比较对象是编码器内部重建帧，不是源帧。
 * 源经量化链路后重建必不等于源，逐字节全等只在编解两侧之间成立。
 * 片界的权威 = 片头 varint `mb_run`，`fragment_count` 由容器出参给。
 */

namespace
{

	struct Encoded
	{
		std::vector<uint8_t> whole;
		std::vector<std::vector<uint8_t>> slices;
	};

	Encoded EncodeOne(CLV_VideoEncoder* e, CLV_VideoFrame* f, CLV_VideoFrameType type, uint8_t q, int64_t dts,
					  int64_t pts)
	{
		Encoded out;
		CLV_VideoBitstream* bs = nullptr;
		if (CLV_VideoEncoderEncode(e, f, type, q, dts, pts, &bs) != CLV_VIDEO_OK) return out;
		const uint8_t* data = nullptr;
		size_t size = 0u;
		CLV_VideoBitstreamGetBytes(bs, &data, &size);
		out.whole.assign(data, data + size);
		uint32_t n = 0u;
		CLV_VideoBitstreamSliceCount(bs, &n);
		for (uint32_t i = 0u; i < n; ++i)
		{
			const uint8_t* sd = nullptr;
			size_t ss = 0u;
			CLV_VideoBitstreamGetSlice(bs, i, &sd, &ss);
			out.slices.emplace_back(sd, sd + ss);
		}
		CLV_VideoBitstreamRelease(bs);
		return out;
	}

	// 图样按 (平面, 行, 列) 定值：任何一块错位或丢字节都会立刻比不出来
	void WritePattern(CLV_VideoFrame* f, uint32_t w, uint32_t h)
	{
		CLV_VideoFrameDescC desc;
		std::memset(&desc, 0, sizeof(desc));
		if (CLV_VideoFrameGetDesc(f, &desc, nullptr) != CLV_VIDEO_OK) return;
		const uint32_t pw[3] = { w, w / 2u, w / 2u };
		const uint32_t ph[3] = { h, h / 2u, h / 2u };
		for (uint32_t pl = 0u; pl < 3u; ++pl)
		{
			for (uint32_t r = 0u; r < ph[pl]; ++r)
			{
				for (uint32_t c = 0u; c < pw[pl]; ++c)
				{
					desc.plane[pl][static_cast<size_t>(r) * desc.stride[pl] + c] =
						static_cast<uint8_t>(pl * 61u + r * 7u + c * 3u);
				}
			}
		}
	}

	int OpenDecoder(CLV_VideoDecoder** out, uint32_t w, uint32_t h, uint8_t max_b)
	{
		CLV_VideoStreamParamsC p;
		std::memset(&p, 0, sizeof(p));
		p.bit_depth = 8u;
		p.quant_precision = kQp;
		p.max_b_frames = max_b;
		p.obmc_disable = 1u;
		p.display_width = static_cast<uint16_t>(w);
		p.display_height = static_cast<uint16_t>(h);
		CLV_VideoError err = CLV_VIDEO_OK;
		*out = CLV_VideoDecoderOpen(&p, &err);
		return static_cast<int>(err);
	}

	int OpenEncoder(CLV_VideoEncoder** out, uint32_t w, uint32_t h, uint16_t slice_mb_count, uint8_t max_b = 0u)
	{
		CLV_VideoEncParamsC p;
		std::memset(&p, 0, sizeof(p));
		p.display_width = static_cast<uint16_t>(w);
		p.display_height = static_cast<uint16_t>(h);
		p.bit_depth = 8u;
		p.quant_precision = kQp;
		p.default_quant = 24u;
		p.max_b_frames = max_b;
		p.slice_mb_count = slice_mb_count;
		CLV_VideoError err = CLV_VIDEO_OK;
		*out = CLV_VideoEncoderOpen(&p, &err);
		return static_cast<int>(err);
	}

}	 // namespace

TEST(VideoPayload, SingleSliceFrameRoundTripsThroughDecoder)
{
	// 32×16 = 2 个宏块
	CLV_VideoEncoder* e = nullptr;
	ASSERT_EQ(OpenEncoder(&e, 32u, 16u, 0u), CLV_VIDEO_OK);
	CLV_VideoDecoder* d = nullptr;
	ASSERT_EQ(OpenDecoder(&d, 32u, 16u, 0u), CLV_VIDEO_OK);

	CLV_VideoFrame* src = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
	WritePattern(src, 32u, 16u);

	const Encoded enc = EncodeOne(e, src, CLV_VIDEO_FRAME_I, 24u, 0, 0);
	ASSERT_FALSE(enc.whole.empty());
	ASSERT_EQ(enc.slices.size(), 1u);

	CLV_VideoFrameInfoC info;
	ASSERT_EQ(CLV_VideoDecoderDecode(d, enc.whole.data(), enc.whole.size(), 1u, 0, 0, &info), CLV_VIDEO_OK);
	EXPECT_EQ(info.slice_count, 1u);
	EXPECT_EQ(info.macroblock_count, 2u);
	// max_b_frames = 0 时输出阈值 = 2：第一帧先延后，Flush 才出
	EXPECT_EQ(info.disposition, CLV_VIDEO_DISP_DELAYED);

	CLV_VideoFrame* got = nullptr;
	uint8_t flag = 1u;
	EXPECT_EQ(CLV_VideoDecoderNextOutput(d, &got, &flag), CLV_VIDEO_OK);
	EXPECT_EQ(flag, 0u);
	ASSERT_EQ(CLV_VideoDecoderFlush(d, &got, &flag), CLV_VIDEO_OK);
	ASSERT_EQ(flag, 1u);
	ASSERT_NE(got, nullptr);

	// 编解逐字节全等：比较对象是编码器内部重建帧，不是源帧（源经量化后重建必不等于源）
	CLV_VideoFrame* rec = clv_internal_encoder_reconstruction(e);
	ASSERT_NE(rec, nullptr);
	ExpectSeparateBuffers(*rec, *got, "单片往返");
	const size_t compared = CompareFrames(*rec, *got, 32u, 16u);
	EXPECT_EQ(compared, 32u * 16u + 2u * (16u * 8u));
	std::cout << "单片（编解逐字节全等）：载荷 " << enc.whole.size()
			  << " B，1 片，编码器内部重建 vs 解码输出逐字节全等 " << compared << " 点\n";

	CLV_VideoFrameRelease(got);
	CLV_VideoFrameRelease(src);
	CLV_VideoDecoderClose(d);
	CLV_VideoEncoderClose(e);
}

TEST(VideoPayload, SingleSliceReconstructionDiffersFromSource)
{
	// 差异非零：素材含高频，源 vs 重建必须不等（逐点全等 = 量化链路未参与）
	CLV_VideoEncoder* e = nullptr;
	ASSERT_EQ(OpenEncoder(&e, 32u, 16u, 0u), CLV_VIDEO_OK);
	CLV_VideoDecoder* d = nullptr;
	ASSERT_EQ(OpenDecoder(&d, 32u, 16u, 0u), CLV_VIDEO_OK);
	CLV_VideoFrame* src = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
	WriteDensePattern(src, 32u, 16u);

	const Encoded enc = EncodeOne(e, src, CLV_VIDEO_FRAME_I, 24u, 0, 0);
	ASSERT_FALSE(enc.whole.empty());
	ASSERT_EQ(CLV_VideoDecoderDecode(d, enc.whole.data(), enc.whole.size(), 1u, 0, 0, nullptr), CLV_VIDEO_OK);
	CLV_VideoFrame* got = nullptr;
	uint8_t flag = 0u;
	ASSERT_EQ(CLV_VideoDecoderFlush(d, &got, &flag), CLV_VIDEO_OK);
	ASSERT_EQ(flag, 1u);

	const size_t valid = 32u * 16u + 2u * (16u * 8u);
	const size_t same = CompareFrames(*src, *got, 32u, 16u);
	const size_t absdiff = TotalAbsDiff(*src, *got, 32u, 16u);
	EXPECT_LT(same, valid) << "源与重建必须不等：全等 = 量化链路没接上";
	/* 度量数值进断言：不等点数与总绝对差两条都得成立，只往 stdout 报一句的话读数漂了没人拦。 */
	EXPECT_GT(valid - same, 0u) << "差异点数没算出来";
	EXPECT_GT(absdiff, 0u) << "总绝对差为零 ⇒ 量化其实没参与，点数判据也会跟着失真";
	std::cout << "差异非零（单片）：源 vs 重建，" << (valid - same) << " / " << valid << " 点不同，总绝对差 " << absdiff
			  << "\n";

	CLV_VideoFrameRelease(got);
	CLV_VideoFrameRelease(src);
	CLV_VideoDecoderClose(d);
	CLV_VideoEncoderClose(e);
}

TEST(VideoPayload, MultiSliceFrameRoundTripsAndSlicesSelfDescribe)
{
	CLV_VideoEncoder* e = nullptr;
	ASSERT_EQ(OpenEncoder(&e, 32u, 16u, 1u), CLV_VIDEO_OK);	   // 每片 1 个宏块 → 2 片
	CLV_VideoDecoder* d = nullptr;
	ASSERT_EQ(OpenDecoder(&d, 32u, 16u, 0u), CLV_VIDEO_OK);

	CLV_VideoFrame* src = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
	WritePattern(src, 32u, 16u);

	const Encoded enc = EncodeOne(e, src, CLV_VIDEO_FRAME_I, 24u, 0, 0);
	ASSERT_EQ(enc.slices.size(), 2u);
	// 整帧载荷 = 两片按序拼接（容器按 fragment_index 拼回来的就是它）
	std::vector<uint8_t> joined;
	for (const std::vector<uint8_t>& s: enc.slices) joined.insert(joined.end(), s.begin(), s.end());
	EXPECT_EQ(joined, enc.whole);

	// 逐片自描述：两片都带片头，首片的片头之后才是完整 VOP 头
	clv::video::SliceHeader sh;
	size_t used = 0u;
	ASSERT_TRUE(clv::video::DecodeSliceHeader(enc.slices[0].data(), enc.slices[0].size(), 7u, true, false, &sh, &used));
	EXPECT_EQ(sh.fragment_count, 2u);
	EXPECT_TRUE(sh.is_first);
	EXPECT_TRUE(sh.has_vop_quant);
	EXPECT_EQ(sh.first_mb, 0u);
	EXPECT_EQ(sh.mb_run, 1u);
	VopHeader vh;
	size_t vused = 0u;
	ASSERT_TRUE(
		clv::video::DecodeVopHeader(enc.slices[0].data() + used, enc.slices[0].size() - used, kQp, &vh, &vused));
	EXPECT_EQ(vh.mb_count, 2u);
	ASSERT_TRUE(clv::video::DecodeSliceHeader(enc.slices[1].data(), enc.slices[1].size(), 7u, false, true, &sh, &used));
	EXPECT_TRUE(sh.is_last);
	EXPECT_FALSE(sh.has_vop_quant);
	EXPECT_EQ(sh.first_mb, 1u);
	EXPECT_EQ(sh.mb_run, 1u);

	CLV_VideoFrameInfoC info;
	ASSERT_EQ(CLV_VideoDecoderDecode(d, enc.whole.data(), enc.whole.size(), 2u, 0, 0, &info), CLV_VIDEO_OK);
	EXPECT_EQ(info.slice_count, 2u);

	CLV_VideoFrame* got = nullptr;
	uint8_t flag = 0u;
	ASSERT_EQ(CLV_VideoDecoderFlush(d, &got, &flag), CLV_VIDEO_OK);
	ASSERT_EQ(flag, 1u);
	// 编解逐字节全等 口径：编码器内部重建 vs 解码输出（编解逐字节全等 与片数无关，多片同样成立）
	CLV_VideoFrame* rec = clv_internal_encoder_reconstruction(e);
	ASSERT_NE(rec, nullptr);
	ExpectSeparateBuffers(*rec, *got, "两片往返");
	const size_t compared = CompareFrames(*rec, *got, 32u, 16u);
	EXPECT_EQ(compared, 32u * 16u + 2u * (16u * 8u));
	std::cout << "多片（编解逐字节全等）：2 片、片头各带 mb_run，编码器内部重建 vs 解码输出逐字节全等 " << compared
			  << " 点\n";

	CLV_VideoFrameRelease(got);
	CLV_VideoFrameRelease(src);
	CLV_VideoDecoderClose(d);
	CLV_VideoEncoderClose(e);
}

TEST(VideoPayload, MultiSliceReconstructionDiffersFromSource)
{
	// 差异非零 的多片版本：分片不该把有损性「分没了」
	CLV_VideoEncoder* e = nullptr;
	ASSERT_EQ(OpenEncoder(&e, 32u, 16u, 1u), CLV_VIDEO_OK);	   // 每片 1 个宏块 → 2 片
	CLV_VideoDecoder* d = nullptr;
	ASSERT_EQ(OpenDecoder(&d, 32u, 16u, 0u), CLV_VIDEO_OK);
	CLV_VideoFrame* src = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
	WriteDensePattern(src, 32u, 16u);

	const Encoded enc = EncodeOne(e, src, CLV_VIDEO_FRAME_I, 24u, 0, 0);
	ASSERT_EQ(enc.slices.size(), 2u);
	ASSERT_EQ(CLV_VideoDecoderDecode(d, enc.whole.data(), enc.whole.size(), 2u, 0, 0, nullptr), CLV_VIDEO_OK);
	CLV_VideoFrame* got = nullptr;
	uint8_t flag = 0u;
	ASSERT_EQ(CLV_VideoDecoderFlush(d, &got, &flag), CLV_VIDEO_OK);
	ASSERT_EQ(flag, 1u);

	const size_t valid = 32u * 16u + 2u * (16u * 8u);
	const size_t same = CompareFrames(*src, *got, 32u, 16u);
	const size_t absdiff = TotalAbsDiff(*src, *got, 32u, 16u);
	EXPECT_LT(same, valid) << "分片不该把有损性分没了";
	EXPECT_GT(valid - same, 0u);
	EXPECT_GT(absdiff, 0u) << "总绝对差为零 ⇒ 两片这条路径上量化没参与";
	/* 与单片那条同料同档：分片数不该改动重建的一个字节，两处的读数因此可以直接对平。 */
	std::cout << "差异非零（2 片）：源 vs 重建，" << (valid - same) << " / " << valid << " 点不同，总绝对差 " << absdiff
			  << "\n";

	CLV_VideoFrameRelease(got);
	CLV_VideoFrameRelease(src);
	CLV_VideoDecoderClose(d);
	CLV_VideoEncoderClose(e);
}

TEST(VideoPayload, FragmentCountMistakesAreRejectedNotUndefined)
{
	CLV_VideoEncoder* e = nullptr;
	ASSERT_EQ(OpenEncoder(&e, 64u, 16u, 1u), CLV_VIDEO_OK);	   // 4 个宏块 → 4 片
	CLV_VideoDecoder* d = nullptr;
	ASSERT_EQ(OpenDecoder(&d, 64u, 16u, 0u), CLV_VIDEO_OK);
	CLV_VideoFrame* src = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
	WritePattern(src, 64u, 16u);
	const Encoded enc = EncodeOne(e, src, CLV_VIDEO_FRAME_I, 24u, 0, 0);
	ASSERT_EQ(enc.slices.size(), 4u);

	// 0 = 没等到末片就喂；> 65 535 = 越上限；两者都不进解析
	EXPECT_EQ(CLV_VideoDecoderDecode(d, enc.whole.data(), enc.whole.size(), 0u, 0, 0, nullptr), CLV_VIDEO_VALUE_RANGE);
	EXPECT_EQ(CLV_VideoDecoderDecode(d, enc.whole.data(), enc.whole.size(), 65536u, 0, 0, nullptr),
			  CLV_VIDEO_VALUE_RANGE);
	// 报少了（声明 1 而实为 4 片）：走的是单片解析路径，字节不是一片的合法载荷 → 帧头不成立
	EXPECT_EQ(CLV_VideoDecoderDecode(d, enc.whole.data(), enc.whole.size(), 1u, 0, 0, nullptr), CLV_VIDEO_HEADER_BAD);
	// 声明 ≥ 2 而与实际不符：片头在带的分片数是唯一载体，与声明值对得上才算数 → 值域不成立
	EXPECT_EQ(CLV_VideoDecoderDecode(d, enc.whole.data(), enc.whole.size(), 2u, 0, 0, nullptr), CLV_VIDEO_VALUE_RANGE);
	// 报多了（声明 5 而实为 4 片）：同一条在带对账 → 值域不成立
	EXPECT_EQ(CLV_VideoDecoderDecode(d, enc.whole.data(), enc.whole.size(), 5u, 0, 0, nullptr), CLV_VIDEO_VALUE_RANGE);

	// 片序错乱（把第 2、3 片调换）：first_mb 不接续，判片序不成立
	std::vector<uint8_t> swapped;
	swapped.insert(swapped.end(), enc.slices[0].begin(), enc.slices[0].end());
	swapped.insert(swapped.end(), enc.slices[2].begin(), enc.slices[2].end());
	swapped.insert(swapped.end(), enc.slices[1].begin(), enc.slices[1].end());
	swapped.insert(swapped.end(), enc.slices[3].begin(), enc.slices[3].end());
	EXPECT_EQ(CLV_VideoDecoderDecode(d, swapped.data(), swapped.size(), 4u, 0, 0, nullptr), CLV_VIDEO_SLICE_SEQUENCE);

	// 空片：把首片片头最后一个 varint（该片宏块数）改成 0 → 值域不成立，不是片头不成立
	std::vector<uint8_t> empty_run = enc.whole;
	empty_run[10] = 0u;	   // 首片片头 = 定长 9 + first_mb 1 B + mb_run 1 B
	EXPECT_EQ(CLV_VideoDecoderDecode(d, empty_run.data(), empty_run.size(), 4u, 0, 0, nullptr), CLV_VIDEO_VALUE_RANGE);
	// 片头读不尽（截到片头中段）：这才是片头不成立
	std::vector<uint8_t> truncated(enc.whole.begin(), enc.whole.begin() + 5);
	EXPECT_EQ(CLV_VideoDecoderDecode(d, truncated.data(), truncated.size(), 4u, 0, 0, nullptr),
			  CLV_VIDEO_SLICE_HEADER_BAD);

	// 正确片数才能过
	EXPECT_EQ(CLV_VideoDecoderDecode(d, enc.whole.data(), enc.whole.size(), 4u, 0, 0, nullptr), CLV_VIDEO_OK);
	std::cout << "片数契约：0 / >65535 / 少报 / 多报 / 乱序 各有判定，正确值 4 片通过\n";

	CLV_VideoFrameRelease(src);
	CLV_VideoDecoderClose(d);
	CLV_VideoEncoderClose(e);
}

TEST(VideoPayload, SliceCountBoundaryFollowsTheArithmetic)
{
	// 每片宏块数下限由算式定：coded 上限 65 536 × 65 536 → 宏块 16 777 216 个，
	// 片数 ≤ 65 535 ⇒ 每片最少 ⌈16 777 216 / 65 535⌉ = 257 个宏块（> u8 上限 255，故字段是 u16）
	const uint64_t total = 4096ull * 4096ull;
	EXPECT_EQ(total, 16777216u);
	EXPECT_EQ((total + 254u) / 255u, 65794u);	 // 每片 255 个宏块时片数越上限
	EXPECT_EQ((total + 256u - 1u) / 256u, 65536u);
	EXPECT_EQ((total + 257u - 1u) / 257u, 65281u);	  // 257 起才落进 65 535 以内

	// 句柄侧兑现用 4096 × 4096（补后同值 = 65 536 个宏块）：宏块数照样超片数上限，
	// 而这一尺寸的补后尺寸积 2^24 落在帧缓冲上界内，能建得出编码器。
	// 每片 1 个宏块时 65 536 片 > 65 535 ⇒ 建编码器即判值域不成立。
	CLV_VideoEncoder* e = nullptr;
	EXPECT_EQ(OpenEncoder(&e, 4096u, 4096u, 1u), CLV_VIDEO_VALUE_RANGE);
	EXPECT_EQ(e, nullptr);
	// 同一尺寸下每片 257 个宏块可建（不分配帧缓冲，只验参数）
	EXPECT_EQ(OpenEncoder(&e, 4096u, 4096u, 257u), CLV_VIDEO_OK);
	ASSERT_NE(e, nullptr);
	CLV_VideoEncoderClose(e);

	// 小尺寸分档：1 片 / 2 片 / 4 片都走通
	for (uint16_t smc: { uint16_t(0u), uint16_t(2u), uint16_t(1u) })
	{
		CLV_VideoEncoder* enc = nullptr;
		ASSERT_EQ(OpenEncoder(&enc, 64u, 16u, smc), CLV_VIDEO_OK);
		CLV_VideoDecoder* dec = nullptr;
		ASSERT_EQ(OpenDecoder(&dec, 64u, 16u, 0u), CLV_VIDEO_OK);
		CLV_VideoFrame* f = nullptr;
		ASSERT_EQ(CLV_VideoEncoderAllocFrame(enc, &f), CLV_VIDEO_OK);
		WritePattern(f, 64u, 16u);
		const Encoded one = EncodeOne(enc, f, CLV_VIDEO_FRAME_I, 24u, 0, 0);
		const uint64_t want = (smc == 0u) ? 1u : (4u + smc - 1u) / smc;
		EXPECT_EQ(one.slices.size(), want) << "slice_mb_count = " << smc;
		ASSERT_EQ(CLV_VideoDecoderDecode(dec, one.whole.data(), one.whole.size(), want, 0, 0, nullptr), CLV_VIDEO_OK);
		CLV_VideoFrame* got = nullptr;
		uint8_t flag = 0u;
		ASSERT_EQ(CLV_VideoDecoderFlush(dec, &got, &flag), CLV_VIDEO_OK);
		ASSERT_EQ(flag, 1u);
		// 编解逐字节全等：比较对象是编码器内部重建帧（三档切片与片数算式不受影响）
		CLV_VideoFrame* rec = clv_internal_encoder_reconstruction(enc);
		ASSERT_NE(rec, nullptr);
		ExpectSeparateBuffers(*rec, *got, "GOP 显示序出帧");
		EXPECT_EQ(CompareFrames(*rec, *got, 64u, 16u), 64u * 16u + 2u * (32u * 8u));
		CLV_VideoFrameRelease(got);
		CLV_VideoFrameRelease(f);
		CLV_VideoDecoderClose(dec);
		CLV_VideoEncoderClose(enc);
	}
	std::cout << "分片边界：1 / 2 / 4 片三档，编码器内部重建 vs 解码输出均逐字节全等\n";
}

TEST(VideoPayload, DecoderCarriesFramesAcrossGopInDisplayOrder)
{
	// I(0) P(30) B(10)：B 需要前向 I 与后向 P 都在参考缓冲里，出帧按 PTS 升序
	CLV_VideoEncoder* e = nullptr;
	ASSERT_EQ(OpenEncoder(&e, 32u, 16u, 0u, 2u), CLV_VIDEO_OK);
	CLV_VideoDecoder* d = nullptr;
	ASSERT_EQ(OpenDecoder(&d, 32u, 16u, 2u), CLV_VIDEO_OK);

	CLV_VideoFrame* src = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
	WritePattern(src, 32u, 16u);

	struct Sent
	{
		CLV_VideoFrameType type;
		int64_t dts;
		int64_t pts;
	};

	const Sent seq[] = {
		{ CLV_VIDEO_FRAME_I, 0, 0  },
		{ CLV_VIDEO_FRAME_P, 1, 30 },
		 { CLV_VIDEO_FRAME_B, 2, 10 }
	};
	std::vector<CLV_VideoFrame*> outs;
	for (const Sent& one: seq)
	{
		const Encoded enc = EncodeOne(e, src, one.type, 24u, one.dts, one.pts);
		ASSERT_FALSE(enc.whole.empty());
		CLV_VideoFrameInfoC info;
		ASSERT_EQ(CLV_VideoDecoderDecode(d, enc.whole.data(), enc.whole.size(), 1u, one.dts, one.pts, &info),
				  CLV_VIDEO_OK);
		CLV_VideoFrame* got = nullptr;
		uint8_t flag = 0u;
		EXPECT_EQ(CLV_VideoDecoderNextOutput(d, &got, &flag), CLV_VIDEO_OK);
		/* 收进 outs 是刻意持有：本例要按显示序逐支比时间戳，所以每一支都得跨过后续 Decode 留着，
		 * 出帧全部收齐后在本件末尾一次性归还（见下面那句遍历 outs 的归还）。
		 * 中途没有重置也没有冲刷，持有不会作废；额度没到阈值时取帧口给的是 OK + flag = 0，
		 * 那一档帧出参写成 NULL，本步就没有要留的。 */
		if (flag == 1u) outs.push_back(got);
	}
	uint8_t flag = 0u;
	CLV_VideoFrame* rest = nullptr;
	// 冲刷段与上面同一形状：收进 outs 就是刻意持有，末尾一并归还
	while (CLV_VideoDecoderFlush(d, &rest, &flag) == CLV_VIDEO_OK && flag == 1u)
	{
		outs.push_back(rest);
		flag = 0u;
		rest = nullptr;
	}
	ASSERT_EQ(outs.size(), 3u);
	// 出帧序 = 显示序：pts 0 → 10 → 30
	EXPECT_EQ(CLV_VideoFrameGetDesc(outs[0], nullptr, nullptr), CLV_VIDEO_OK);
	std::vector<uint64_t> shown;
	for (CLV_VideoFrame* f: outs)
	{
		CLV_VideoFrameInfoC info;
		CLV_VideoFrameGetDesc(f, nullptr, &info);
		shown.push_back(static_cast<uint64_t>(info.pts_ticks));
	}
	// 出帧序必须是 PTS 升序：解码序是 0 / 30 / 10，显示序是 0 / 10 / 30
	ASSERT_EQ(shown.size(), 3u);
	EXPECT_EQ(shown[0], 0u);
	EXPECT_EQ(shown[1], 10u);
	EXPECT_EQ(shown[2], 30u);
	// 收尾：上面刻意收着的每一支都在这里归还，且排在关闭之前（关闭会作废已交出的帧，那时再还只能拿到不成立）
	for (CLV_VideoFrame* f: outs) CLV_VideoFrameRelease(f);
	CLV_VideoFrameRelease(src);
	CLV_VideoDecoderClose(d);
	CLV_VideoEncoderClose(e);
	std::cout << "出帧序：收到 " << outs.size() << " 帧，pts 序列首项 " << (shown.empty() ? 0u : shown.front())
			  << "、末项 " << (shown.empty() ? 0u : shown.back()) << "\n";
}

TEST(VideoPayload, MaxSliceCountRoundTripsAtSliceCap)
{
	// 最大片数档：65 535 片 = 每片 1 个宏块 × 65 535 个宏块。
	// 尺寸取 257 × 255 个宏块（补后 4112 × 4080，两值都是 16 的倍数），宏块数正好 65 535。
	CLV_VideoEncoder* e = nullptr;
	ASSERT_EQ(OpenEncoder(&e, 4112u, 4080u, 1u), CLV_VIDEO_OK);
	CLV_VideoDecoder* d = nullptr;
	ASSERT_EQ(OpenDecoder(&d, 4112u, 4080u, 0u), CLV_VIDEO_OK);

	CLV_VideoFrame* src = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
	WritePattern(src, 4112u, 4080u);

	const Encoded enc = EncodeOne(e, src, CLV_VIDEO_FRAME_I, 24u, 0, 0);
	ASSERT_EQ(enc.slices.size(), 65535u);

	CLV_VideoFrameInfoC info;
	ASSERT_EQ(CLV_VideoDecoderDecode(d, enc.whole.data(), enc.whole.size(), 65535u, 0, 0, &info), CLV_VIDEO_OK);
	EXPECT_EQ(info.slice_count, 65535u);
	EXPECT_EQ(info.macroblock_count, 65535u);

	CLV_VideoFrame* got = nullptr;
	uint8_t flag = 0u;
	ASSERT_EQ(CLV_VideoDecoderFlush(d, &got, &flag), CLV_VIDEO_OK);
	ASSERT_EQ(flag, 1u);
	// 编解逐字节全等 口径：65 535 个宏块全过一遍有损重建，与解码输出逐字节全等
	CLV_VideoFrame* rec = clv_internal_encoder_reconstruction(e);
	ASSERT_NE(rec, nullptr);
	ExpectSeparateBuffers(*rec, *got, "满档分片");
	const size_t compared = CompareFrames(*rec, *got, 4112u, 4080u);
	EXPECT_EQ(compared, 4112u * 4080u + 2u * (2056u * 2040u));
	/* 载荷字节数进断言（只打印就红不出来）。定宽那一档这里有闭式：
	 *   65 535 宏块 × (6 块 × 72 B) + 966 523 = 29 277 643 B
	 *   72 B = 单块 64 系数 × 9 bit ÷ 8（q' = 24 落帧内 17..32 档 → 每系数 9 bit），头不随内容变 ⇒ 可乘。
	 * 法 1（EOB 截尾 + EG 幅值）下载体长度随块内内容走，「宏块数 × 单块」的闭式不再成立，
	 * 于是那一档里这一例钉的是方向：本图样的残差以零为主，截尾省的正是这些位，载荷必须显著低于定宽档。
	 * 绝对值的跨库对照不在本例的判据范围，这一例只钉对定宽闭式的方向。
	 * 判据取「不大于」而不是「小于」：定宽档这一档这个图样恰好等于自己的闭式（零照样占位，没得省），
	 * 那是正确结果而不是回归；法 1 截尾省的正是这些位，严格小于照旧成立（≤ 与 < 对法 1 等价，因为它是整数字节数）。 */
	EXPECT_LE(enc.whole.size(), 29277643u);
	std::cout << "最大片数：65 535 片、载荷 " << enc.whole.size()
			  << " B（定宽档闭式 29 277 643 B 是本图样的上界参照），编码器内部重建 vs 解码输出逐字节全等 " << compared
			  << " 点\n";

	CLV_VideoFrameRelease(got);
	CLV_VideoFrameRelease(src);
	CLV_VideoDecoderClose(d);
	CLV_VideoEncoderClose(e);
}

