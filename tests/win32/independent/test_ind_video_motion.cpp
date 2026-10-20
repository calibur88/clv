/* test_ind_video_motion.cpp
 *
 * B 帧的两条梯子（声明深度、帧级时间基准）与三条「拿已冻结的期望做错变形」的分辨力核对。
 *
 * 每档的期望只有三处来源，都不向被测函数取数：
 *   公开头 include/CLV_Video.h 的契约文字：max_b_frames 是唯一权威声明、编码器不得超出，
 *     丢帧不是错误（句柄仍可用、时间轴照常推进），处置结论逐项取 CLV_VideoDisposition 的取值。
 *   实现里的判据：src/video/clv_video.cpp 的 B 帧时间基准段（判据形如 trd > 0 且 0 < trb < trd，
 *     不成立即丢帧并返 OK）、max_b_frames 段（连续 B 的游程到达声明值即判值域不成立）。
 *   已冻结的期望：tests/win32/video/video_payload_fixtures_p.h 的载体字节、位图、每系数位宽与 DC 电平。
 * 变形的期望一律写成「与实际输出不等」的判定：分辨力不足的断言才会让变形期望也通过。
 */

#include "../video/test_config_expect.h"
#include "CLV_Video.h"
#include "video/payload.h"
#include "video/status.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{

	using clv::video::VideoErr;
	using clv::video::VopPayload;

	constexpr uint8_t kPrecisionV1 = 7u;
	constexpr uint8_t kCarrierQuant = 24u;			  // 冻结载体所在档：q′ = 24 → 帧内每系数 9 bit
	constexpr int64_t kFwdPts = 0;					  // 前向锚点的 PTS
	constexpr int64_t kBackPts = 30;				  // 后向锚点的 PTS
	constexpr int64_t kProbePts = 31;				  // 可用性探针的 PTS：比两支锚点都大，进队后不会被弹出
	constexpr uint32_t kIndSampleSeed = 20261008u;	  // 取样种子：素材与取样点都由它派生，跑一遍即可复现

	/* 出帧取样：先抽 NextOutput 再抽 Flush，直到两条都空；按显示序返回帧与 PTS。 */
	struct Drain
	{
		std::vector<CLV_VideoFrame*> frames;
		std::vector<int64_t> pts;
	};

	Drain DrainAll(CLV_VideoDecoder* d)
	{
		Drain out;
		CLV_VideoFrame* f = nullptr;
		uint8_t got = 0u;
		while (CLV_VideoDecoderNextOutput(d, &f, &got) == CLV_VIDEO_OK && got != 0u)
		{
			CLV_VideoFrameInfoC info {};
			if (CLV_VideoFrameGetDesc(f, nullptr, &info) == CLV_VIDEO_OK) out.pts.push_back(info.pts_ticks);
			out.frames.push_back(f);
			f = nullptr;
			got = 0u;
		}
		while (CLV_VideoDecoderFlush(d, &f, &got) == CLV_VIDEO_OK && got != 0u)
		{
			CLV_VideoFrameInfoC info {};
			if (CLV_VideoFrameGetDesc(f, nullptr, &info) == CLV_VIDEO_OK) out.pts.push_back(info.pts_ticks);
			out.frames.push_back(f);
			f = nullptr;
			got = 0u;
		}
		return out;
	}

	void ReleaseAll(Drain* d)
	{
		for (CLV_VideoFrame* f: d->frames) CLV_VideoFrameRelease(f);
		d->frames.clear();
		d->pts.clear();
	}

	CLV_VideoDecoder* OpenDecoder(CLV_VideoError* err, uint16_t w, uint16_t h, uint8_t max_b)
	{
		CLV_VideoStreamParamsC p {};
		p.bit_depth = 8u;
		p.quant_precision = kPrecisionV1;
		p.max_b_frames = max_b;
		p.obmc_disable = 1u;	// v1 = 1
		p.display_width = w;
		p.display_height = h;
		return CLV_VideoDecoderOpen(&p, err);
	}

	CLV_VideoEncoder* OpenEncoder(CLV_VideoError* err, uint16_t w, uint16_t h, uint8_t q, uint8_t max_b)
	{
		CLV_VideoEncParamsC p {};
		p.display_width = w;
		p.display_height = h;
		p.bit_depth = 8u;
		p.quant_precision = kPrecisionV1;
		p.default_quant = q;
		p.max_b_frames = max_b;
		return CLV_VideoEncoderOpen(&p, err);
	}

	/* 编码一帧并把整帧载荷拷出来：码流缓冲归调用方，载荷字节只到 Release 前有效。 */
	bool EncodeToBytes(std::vector<uint8_t>* out, CLV_VideoEncoder* e, CLV_VideoFrame* f, CLV_VideoFrameType type,
					   uint8_t q, int64_t dts)
	{
		CLV_VideoBitstream* bs = nullptr;
		if (CLV_VideoEncoderEncode(e, f, type, q, dts, dts, &bs) != CLV_VIDEO_OK) return false;
		const uint8_t* data = nullptr;
		size_t size = 0u;
		const bool ok = (CLV_VideoBitstreamGetBytes(bs, &data, &size) == CLV_VIDEO_OK);
		if (ok) out->assign(data, data + size);
		CLV_VideoBitstreamRelease(bs);
		return ok;
	}

	/* 有效区（补边前的显示尺寸）按 Y、U、V 顺序拼成一个字节串。 */
	std::vector<uint8_t> ValidArea(CLV_VideoFrame* f, uint32_t w, uint32_t h)
	{
		CLV_VideoFrameDescC desc {};
		std::vector<uint8_t> out;
		if (CLV_VideoFrameGetDesc(f, &desc, nullptr) != CLV_VIDEO_OK) return out;
		const uint32_t pw[3] = { w, w / 2u, w / 2u };
		const uint32_t ph[3] = { h, h / 2u, h / 2u };
		for (uint32_t pl = 0u; pl < 3u; ++pl)
		{
			for (uint32_t r = 0u; r < ph[pl]; ++r)
			{
				const uint8_t* row = desc.plane[pl] + static_cast<size_t>(r) * desc.stride[pl];
				out.insert(out.end(), row, row + pw[pl]);
			}
		}
		return out;
	}

	/* 素材 = (行 × 37 + 列 × 91 + (行 ^ 列) × 13 + 平面 × 61 + 种子) mod 256：
	 * 非平坦且含高频 —— 源与重建「碰巧全等」只能是链路没接上的信号，不是素材太平。
	 * 种子是常量，同一素材在每一档里都逐字节相同，档位之间只差量化步长这一个变量。 */
	void WriteSeeded(CLV_VideoFrame* f, uint32_t w, uint32_t h, uint32_t seed)
	{
		CLV_VideoFrameDescC desc {};
		if (CLV_VideoFrameGetDesc(f, &desc, nullptr) != CLV_VIDEO_OK) return;
		const uint32_t pw[3] = { w, w / 2u, w / 2u };
		const uint32_t ph[3] = { h, h / 2u, h / 2u };
		for (uint32_t pl = 0u; pl < 3u; ++pl)
		{
			for (uint32_t r = 0u; r < ph[pl]; ++r)
			{
				uint8_t* row = desc.plane[pl] + static_cast<size_t>(r) * desc.stride[pl];
				for (uint32_t c = 0u; c < pw[pl]; ++c)
					row[c] = static_cast<uint8_t>((r * 37u + c * 91u + ((r ^ c) * 13u) + pl * 61u + seed) & 0xFFu);
			}
		}
	}

	/* 三平面每一字节都写 0：冻结载体那份期望的建模口径就是「像素全零」。 */
	void ZeroPlanes(CLV_VideoFrame* f)
	{
		CLV_VideoFrameDescC desc {};
		if (CLV_VideoFrameGetDesc(f, &desc, nullptr) != CLV_VIDEO_OK) return;
		const uint32_t rows[3] = { desc.coded_height, desc.coded_height / 2u, desc.coded_height / 2u };
		for (uint32_t pl = 0u; pl < 3u; ++pl)
			for (uint32_t r = 0u; r < rows[pl]; ++r)
				std::memset(desc.plane[pl] + static_cast<size_t>(r) * desc.stride[pl], 0, desc.stride[pl]);
	}

	size_t EqualBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
	{
		const size_t n = (a.size() < b.size()) ? a.size() : b.size();
		size_t same = 0u;
		for (size_t i = 0u; i < n; ++i)
			if (a[i] == b[i]) ++same;
		return same;
	}

	uint64_t SumAbsDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
	{
		const size_t n = (a.size() < b.size()) ? a.size() : b.size();
		uint64_t sum = 0ull;
		for (size_t i = 0u; i < n; ++i)
		{
			const int32_t d = static_cast<int32_t>(a[i]) - static_cast<int32_t>(b[i]);
			sum += static_cast<uint64_t>(d >= 0 ? d : -d);
		}
		return sum;
	}

}	 // namespace

TEST(IndependentVideoMotion, BFrameDepthLadder)
{
	/* 声明字段是 1 字节无符号，全域都是合法声明，建句柄处不判它 ——
	 * 「超声明」的判定落在编码入口：连续 B 的游程到达声明值即判值域不成立、不产载荷。
	 * 每档喂「声明数 + 1」支连续 B：前声明数支必须出载荷，多出的那一支必须出不了。 */
	const uint8_t depths[] = { 0u, 1u, 2u, 3u };
	for (uint8_t depth: depths)
	{
		SCOPED_TRACE(static_cast<int>(depth));
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoEncoder* e = OpenEncoder(&err, 16u, 16u, kCarrierQuant, depth);
		ASSERT_NE(e, nullptr) << "声明本身不是值域判定项";
		EXPECT_EQ(err, CLV_VIDEO_OK);
		CLV_VideoFrame* f = nullptr;
		ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
		int64_t dts = 0;
		std::vector<uint8_t> anchor;
		ASSERT_TRUE(EncodeToBytes(&anchor, e, f, CLV_VIDEO_FRAME_I, kCarrierQuant, dts));
		++dts;
		for (uint8_t run = 1u; run <= depth; ++run)
		{
			SCOPED_TRACE(static_cast<int>(run));
			CLV_VideoBitstream* bs = nullptr;
			ASSERT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_B, kCarrierQuant, dts, dts, &bs), CLV_VIDEO_OK)
				<< "游程 " << run << " 还在声明档内";
			ASSERT_NE(bs, nullptr);
			CLV_VideoBitstreamRelease(bs);
			++dts;
		}
		// 声明 + 1：越声明的那一支拒掉，且不给调用方交出码流缓冲
		{
			CLV_VideoBitstream* bs = reinterpret_cast<CLV_VideoBitstream*>(1u);
			EXPECT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_B, kCarrierQuant, dts, dts, &bs),
					  CLV_VIDEO_VALUE_RANGE);
			EXPECT_EQ(bs, nullptr) << "拒档不得交出半成品码流";
			++dts;
		}
		// 游程由 I / P 断开：断开后第一支 B 是否合法只看声明值本身
		std::vector<uint8_t> mid;
		ASSERT_TRUE(EncodeToBytes(&mid, e, f, CLV_VIDEO_FRAME_P, kCarrierQuant, dts));
		++dts;
		{
			CLV_VideoBitstream* bs = nullptr;
			const CLV_VideoError r = CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_B, kCarrierQuant, dts, dts, &bs);
			EXPECT_EQ(r, static_cast<unsigned>(depth) >= 1u ? CLV_VIDEO_OK : CLV_VIDEO_VALUE_RANGE)
				<< "断开后的第一支 B";
			if (r == CLV_VIDEO_OK) CLV_VideoBitstreamRelease(bs);
			++dts;
		}
		// 断开后的第二支：只有声明 ≥ 2 才合法
		{
			CLV_VideoBitstream* bs = nullptr;
			const CLV_VideoError r = CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_B, kCarrierQuant, dts, dts, &bs);
			EXPECT_EQ(r, static_cast<unsigned>(depth) >= 2u ? CLV_VIDEO_OK : CLV_VIDEO_VALUE_RANGE)
				<< "断开后的第二支 B";
			if (r == CLV_VIDEO_OK) CLV_VideoBitstreamRelease(bs);
		}
		CLV_VideoFrameRelease(f);
		CLV_VideoEncoderClose(e);
	}

	// 声明顶格：字段是 1 字节，255 也是合法声明，建句柄不拒
	{
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoEncoder* e = OpenEncoder(&err, 16u, 16u, kCarrierQuant, UINT8_MAX);
		ASSERT_NE(e, nullptr);
		EXPECT_EQ(err, CLV_VIDEO_OK);
		CLV_VideoFrame* f = nullptr;
		ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
		CLV_VideoBitstream* bs = nullptr;
		// 顶格档也要有参考可依：先落一个 I，再验 255 声明下的 B 出得来
		std::vector<uint8_t> anchor;
		ASSERT_TRUE(EncodeToBytes(&anchor, e, f, CLV_VIDEO_FRAME_I, kCarrierQuant, 0));
		EXPECT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_B, kCarrierQuant, 0, 0, &bs), CLV_VIDEO_OK);
		if (bs != nullptr) CLV_VideoBitstreamRelease(bs);
		CLV_VideoFrameRelease(f);
		CLV_VideoEncoderClose(e);
	}
	std::cout << "B 帧深度：声明 0 / 1 / 2 / 3 四档各喂「声明 + 1」支连续 B，越声明一律 CLV_VIDEO_VALUE_RANGE，"
				 "判定落点在编码入口（建句柄不判声明）\n";
}

TEST(IndependentVideoMotion, BFrameTimeBaseLadder)
{
	/* 帧级时间基准：trb = 本帧 PTS − 前向参考 PTS，trd = 后向参考 PTS − 前向参考 PTS。
	 * 判据 trd > 0 且 0 < trb < trd，不成立即丢帧：返 OK、disposition 记 DROP_DIRECT_TIME、
	 * 不建帧不入队，句柄仍可用，时间轴照常推进。
	 *
	 * 前向 / 后向参考都取「驻留参考缓冲里 PTS 最近的一帧」，后向只取 PTS 不小于本帧的那一帧，
	 * 于是 trd ≥ trb 恒成立：pb > pp 写不进这条判据，喂到后向锚点之外命中的是「参考缺失」那一档。
	 * pb = pp 要求本帧 PTS 与后向锚点同值，同一取法又让两个方向都选中那一帧，trd 归 0 ——
	 * 落进同一条丢弃分支，只是命中的是 trd > 0 那一子句。 */
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoEncoder* e = OpenEncoder(&err, 16u, 16u, kCarrierQuant, 1u);
	ASSERT_NE(e, nullptr);
	CLV_VideoFrame* src = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
	WriteSeeded(src, 16u, 16u, kIndSampleSeed);
	std::vector<uint8_t> ipay;
	std::vector<uint8_t> ppay;
	std::vector<uint8_t> bpay;
	ASSERT_TRUE(EncodeToBytes(&ipay, e, src, CLV_VIDEO_FRAME_I, kCarrierQuant, 0));
	ASSERT_TRUE(EncodeToBytes(&ppay, e, src, CLV_VIDEO_FRAME_P, kCarrierQuant, 1));
	ASSERT_TRUE(EncodeToBytes(&bpay, e, src, CLV_VIDEO_FRAME_B, kCarrierQuant, 2));
	CLV_VideoEncoderClose(e);

	struct TimeStep
	{
		int64_t b_pts;
		CLV_VideoDisposition want;
		bool produces;
		const char* how;
	};

	const TimeStep steps[] = {
		{ 1,  CLV_VIDEO_DISP_DELAYED,		   true,  "time_pb = 1 < time_pp = 30：权重成立"			 },
		{ 29, CLV_VIDEO_DISP_DELAYED,		   true,  "time_pb = 29，贴紧后向锚点"						 },
		{ 0,  CLV_VIDEO_DISP_DROP_DIRECT_TIME, false, "time_pb = 0：必丢"								 },
		{ 30, CLV_VIDEO_DISP_DROP_DIRECT_TIME, false, "time_pb = time_pp：退化到 trd = 0，同一分支"		 },
		{ 40, CLV_VIDEO_DISP_DROP_REF_MISSING, false, "time_pb > time_pp 写不出来：越过后向锚点即缺参考" },
	};
	for (const TimeStep& s: steps)
	{
		SCOPED_TRACE(s.how);
		CLV_VideoDecoder* d = OpenDecoder(&err, 16u, 16u, 1u);
		ASSERT_NE(d, nullptr);
		ASSERT_EQ(CLV_VideoDecoderDecode(d, ipay.data(), ipay.size(), 1u, 0, kFwdPts, nullptr), CLV_VIDEO_OK);
		ASSERT_EQ(CLV_VideoDecoderDecode(d, ppay.data(), ppay.size(), 1u, 1, kBackPts, nullptr), CLV_VIDEO_OK);
		CLV_VideoFrameInfoC info {};
		std::memset(&info, 0xFF, sizeof(info));
		const CLV_VideoError r = CLV_VideoDecoderDecode(d, bpay.data(), bpay.size(), 1u, 2, s.b_pts, &info);
		// 丢帧不是错误：返 OK，处置结论落在 disposition 上
		EXPECT_EQ(r, CLV_VIDEO_OK);
		EXPECT_EQ(info.disposition, s.want);
		EXPECT_EQ(info.type, CLV_VIDEO_FRAME_B);
		// 时间轴照常推进：被丢那一帧的 dts / pts 仍原样交回
		EXPECT_EQ(info.dts_ticks, 2);
		EXPECT_EQ(info.pts_ticks, s.b_pts);
		/* 句柄是否还可用，在抽帧之前判：抽帧走到流结束时参考缓冲一并放空，再喂就换了一档情境。
		 * 探针取一支 I（不依赖参考），PTS 取全部驻留帧之外且最大的一格，
		 * 于是它进队后被弹出的一定不是它 —— 结论固定为「已解码、暂存待输出」。 */
		CLV_VideoFrameInfoC again {};
		const CLV_VideoError r2 = CLV_VideoDecoderDecode(d, ipay.data(), ipay.size(), 1u, 3, kProbePts, &again);
		EXPECT_EQ(r2, CLV_VIDEO_OK);
		EXPECT_EQ(again.disposition, CLV_VIDEO_DISP_DELAYED) << "丢弃不该打掉句柄";
		EXPECT_EQ(again.pts_ticks, kProbePts);
		uint8_t rejected = 1u;
		EXPECT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK);
		EXPECT_EQ(rejected, 0u) << "丢帧不该把整条流置拒解";
		Drain got = DrainAll(d);
		// 出帧集合 = 两支锚点 + 成立的 B（本档成立则两支、本档丢弃则只有补喂的那支）
		const size_t want_count = s.produces ? 4u : 3u;
		EXPECT_EQ(got.frames.size(), want_count) << (s.produces ? "收档要出到帧" : "丢档不得出帧");
		for (size_t i = 1u; i < got.pts.size(); ++i) EXPECT_LT(got.pts[i - 1u], got.pts[i]);
		ReleaseAll(&got);
		CLV_VideoDecoderClose(d);
	}
	CLV_VideoFrameRelease(src);
	std::cout << "B 帧时间基准：0 / 1 / 贴紧 / 等于 time_pp / 越过后向锚点五档，"
				 "三档丢帧两档出帧，全部返 OK 且句柄可用\n";
}

TEST(IndependentVideoMotion, ZeroTimePbDropsTheFrame)
{
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoEncoder* e = OpenEncoder(&err, 16u, 16u, kCarrierQuant, 1u);
	ASSERT_NE(e, nullptr);
	CLV_VideoFrame* src = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
	WriteSeeded(src, 16u, 16u, kIndSampleSeed + 1u);
	std::vector<uint8_t> ipay;
	std::vector<uint8_t> ppay;
	std::vector<uint8_t> bpay;
	ASSERT_TRUE(EncodeToBytes(&ipay, e, src, CLV_VIDEO_FRAME_I, kCarrierQuant, 0));
	ASSERT_TRUE(EncodeToBytes(&ppay, e, src, CLV_VIDEO_FRAME_P, kCarrierQuant, 1));
	ASSERT_TRUE(EncodeToBytes(&bpay, e, src, CLV_VIDEO_FRAME_B, kCarrierQuant, 2));
	CLV_VideoEncoderClose(e);

	CLV_VideoDecoder* d = OpenDecoder(&err, 16u, 16u, 1u);
	ASSERT_NE(d, nullptr);
	ASSERT_EQ(CLV_VideoDecoderDecode(d, ipay.data(), ipay.size(), 1u, 0, kFwdPts, nullptr), CLV_VIDEO_OK);
	ASSERT_EQ(CLV_VideoDecoderDecode(d, ppay.data(), ppay.size(), 1u, 1, kBackPts, nullptr), CLV_VIDEO_OK);
	// time_pb = 0：本帧 PTS 与前向锚点同值，两个方向都选中同一帧，trd 随之归 0
	CLV_VideoFrameInfoC info {};
	ASSERT_EQ(CLV_VideoDecoderDecode(d, bpay.data(), bpay.size(), 1u, 2, kFwdPts, &info), CLV_VIDEO_OK);
	EXPECT_EQ(info.disposition, CLV_VIDEO_DISP_DROP_DIRECT_TIME);
	// 探针：紧跟一次不依赖参考的 I，句柄若被这次丢弃打掉就出不了结论
	CLV_VideoFrameInfoC probe {};
	ASSERT_EQ(CLV_VideoDecoderDecode(d, ipay.data(), ipay.size(), 1u, 3, kProbePts, &probe), CLV_VIDEO_OK);
	EXPECT_EQ(probe.disposition, CLV_VIDEO_DISP_DELAYED);
	Drain got = DrainAll(d);
	/* 队列阈值 = 声明深度 + 2 = 3：三支（两支锚点 + 探针）出得到，被丢的那支不在集合里。
	 * PTS 与锚点重合正是这条护栏的可观测处：护栏没接上时同一 PTS 会出两支帧。 */
	ASSERT_EQ(got.frames.size(), 3u);
	size_t at_zero = 0u;
	for (const int64_t p: got.pts)
		if (p == kFwdPts) ++at_zero;
	EXPECT_EQ(at_zero, 1u) << "被丢的 B 与本帧前向锚点同 PTS，出两支即为护栏缺失";
	for (size_t i = 1u; i < got.pts.size(); ++i) EXPECT_LT(got.pts[i - 1u], got.pts[i]);
	ReleaseAll(&got);
	uint8_t rejected = 1u;
	EXPECT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK);
	EXPECT_EQ(rejected, 0u);
	CLV_VideoDecoderClose(d);
	CLV_VideoFrameRelease(src);
	std::cout << "time_pb = 0 的护栏：必丢、返 OK、丢的帧不出现在出帧集合里、句柄继续可用\n";
}

TEST(IndependentVideoMotion, DepthOverrunIsRefused)
{
	/* 声明 1 支 B：游程到 1 即停，第二支连续 B 判值域不成立。
	 * 实际返的码取实现里那条判据的返回档，不接受「悄悄出一支 B」的走法。 */
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoEncoder* e = OpenEncoder(&err, 16u, 16u, kCarrierQuant, 1u);
	ASSERT_NE(e, nullptr);
	CLV_VideoFrame* f = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
	std::vector<uint8_t> anchor;
	ASSERT_TRUE(EncodeToBytes(&anchor, e, f, CLV_VIDEO_FRAME_I, kCarrierQuant, 0));
	CLV_VideoBitstream* first = nullptr;
	ASSERT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_B, kCarrierQuant, 1, 1, &first), CLV_VIDEO_OK);
	ASSERT_NE(first, nullptr);
	CLV_VideoBitstreamRelease(first);
	CLV_VideoBitstream* second = reinterpret_cast<CLV_VideoBitstream*>(1u);
	const CLV_VideoError r = CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_B, kCarrierQuant, 2, 2, &second);
	EXPECT_EQ(r, CLV_VIDEO_VALUE_RANGE);
	EXPECT_EQ(second, nullptr);
	// 被拒的那一次不推进时间轴状态：后续更大 dts 的请求仍按声明判
	CLV_VideoBitstream* third = nullptr;
	const CLV_VideoError r3 = CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_B, kCarrierQuant, 3, 3, &third);
	EXPECT_EQ(r3, CLV_VIDEO_VALUE_RANGE);
	if (r3 == CLV_VIDEO_OK && third != nullptr) CLV_VideoBitstreamRelease(third);
	std::vector<uint8_t> reset;
	EXPECT_TRUE(EncodeToBytes(&reset, e, f, CLV_VIDEO_FRAME_I, kCarrierQuant, 4));
	CLV_VideoBitstream* after = nullptr;
	EXPECT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_B, kCarrierQuant, 5, 5, &after), CLV_VIDEO_OK);
	if (after != nullptr) CLV_VideoBitstreamRelease(after);
	CLV_VideoFrameRelease(f);
	CLV_VideoEncoderClose(e);
	std::cout << "max_b_frames + 1 的护栏：连续第二支 B 一律 CLV_VIDEO_VALUE_RANGE 且不产码流\n";
}

TEST(IndependentVideoMotion, MutatedCarrierByteDiffersFromFrozen)
{
	/* 冻结期望（载体数组由 scripts/gen_video_payload_fixtures.py 两档都发，选档在 tests/win32/video/test_config_expect.h）：
	 * 输入 = 176×144、三平面每一字节都写 0，每块 64 个像素恒 −128 ⇒ 变换只有 DC = 64 × (−128) = −8192，其余 63 项 0；
	 * q′ = 24 落帧内 17..32 档 ⇒ |QF| ≤ 240、每系数 9 bit。取样点 = 宏块 0 的块 0 的 DC：冻结值 QF = −170。
	 * 两档写这同一个 DC 的位序不同，逐位摆开也各自钉一次：
	 *   法 1（EOB 截尾 + EG 幅值）= 每块 all_zero(1) + eob(6) + EG(170)(15 bit) + 符号(1) = 23 bit，六块 138 bit ⇒ 补齐 18 B。
	 *     位序：0 | 000000(eob=0) | 0000000 1 0101011(EG(170)，x = 171、L = 7) | 1(负) ⇒ 首三字节 = 0x00 0x02 0xAE。
	 *   定宽（每系数 9 bit 直写、零照样占位）= 六块 × 64 槽 × 9 bit = 432 B。
	 *     位序：1(负) | 10101010(幅值 170，MSB 先出) ⇒ 首字节 0xD5，第 9 位是幅值的末位 0 ⇒ 次字节 0x00，其后 63 槽全零。 */
	RecordProperty("ReproduceSeed", kIndSampleSeed);
	RecordProperty("SamplePoint", "176x144 / mb 0 / block 0 / coefficient 0 (DC)");
	std::vector<uint8_t> frozen(clvtest::kZeroFrameMbCarrier,
								clvtest::kZeroFrameMbCarrier + clvtest::kZeroFrameMbCarrierLen);
	ASSERT_EQ(frozen.size(), clvtest::kPayloadAllZeroMB);

	/* 变形 = 只把宏块 0 的块 0 DC 幅值改一档（170 → 171）。两档落点不同但分辨力相同：
	 *   法 1：EG 后缀 0101011 → 0101100，x 从 171 到 172 不过 2 的幂界 ⇒ 码长仍 15 bit ⇒ 改动全落在第三字节 0xAE → 0xB2。
	 *   定宽：改的是 9 bit 组的末位（幅值的最低位），MSB 先出 ⇒ 该位是第二字节的最高位 ⇒ 0x00 → 0x80。 */
	std::vector<uint8_t> mutated = frozen;
	if (clvtest::kHasCoefCodingSelector)
	{
		ASSERT_EQ(frozen[0], 0x00u) << "块 0 的 all_zero = 0 与 eob = 0 那 7 位全为 0";
		ASSERT_EQ(frozen[1], 0x02u) << "EG 前导零的最后一位、分隔的 1、后缀最高位";
		ASSERT_EQ(frozen[2], 0xAEu) << "后缀 0101011 + 符号 1 + 下一块 all_zero 0";
		mutated[2] = 0xB2u;
	}
	else
	{
		ASSERT_EQ(frozen[0], 0xD5u) << "符号 1 + 幅值 170 的高 7 位（1010101）";
		ASSERT_EQ(frozen[1], 0x00u) << "幅值 170 的末位（0）+ 槽 1 的符号与高 6 位，全为 0";
		ASSERT_EQ(frozen[2], 0x00u) << "其余 63 槽零照样占位 ⇒ 逐字节为 0";
		mutated[1] = 0x80u;
	}
	ASSERT_NE(mutated, frozen);

	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoEncoder* e = OpenEncoder(&err, 176u, 144u, kCarrierQuant, 0u);
	ASSERT_NE(e, nullptr);
	CLV_VideoFrame* f = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
	ZeroPlanes(f);
	std::vector<uint8_t> payload;
	ASSERT_TRUE(EncodeToBytes(&payload, e, f, CLV_VIDEO_FRAME_I, kCarrierQuant, 0));
	CLV_VideoFrameRelease(f);

	// 176 ÷ 16 = 11 列宏块：载荷按整帧一片给出，语法层拆回来的块数据即载体字节
	VopPayload back;
	size_t consumed = 0u;
	ASSERT_TRUE(! payload.empty());
	const VideoErr de =
		clv::video::DecodeVopPayload(payload.data(), payload.size(), kPrecisionV1, 11u, &back, &consumed);
	CLV_VideoEncoderClose(e);
	ASSERT_EQ(de, VideoErr::kOk);
	ASSERT_EQ(back.macroblocks.size(), 99u) << "11 × 9 = 99 个宏块";
	const std::vector<uint8_t>& actual = back.macroblocks[0].block;
	// 未变形的冻结期望与实际输出相等 —— 下面的「不等」才只可能来自变形本身
	EXPECT_EQ(actual, frozen);
	// 变形一档系数的期望必须与实际输出不相等：相等 = 断言对系数没有分辨力
	EXPECT_NE(actual, mutated);
	EXPECT_EQ(actual.size(), clvtest::kZeroFrameMbBlockSize);
	std::cout << "系数级分辨力：块 0 的 DC 幅值改一档（170 → 171）后期望与实际输出不等，"
				 "未变形期望与实际输出相等（载体 "
			  << actual.size() << " B / 宏块）\n";
}

TEST(IndependentVideoMotion, LosslessExpectationDiffersFromOutput)
{
	/* 变形期望：把重建当成源帧本身（即「这一路无损、逐字节相同」）。
	 * 实际输出必须与该期望不相等 —— 有损闭环真的在链路里时，两者不可能逐字节相同。 */
	RecordProperty("ReproduceSeed", kIndSampleSeed);
	RecordProperty("SamplePoint", "32x16 三平面全有效区");
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoEncoder* e = OpenEncoder(&err, 32u, 16u, kCarrierQuant, 0u);
	ASSERT_NE(e, nullptr);
	CLV_VideoDecoder* d = OpenDecoder(&err, 32u, 16u, 0u);
	ASSERT_NE(d, nullptr);
	CLV_VideoFrame* src = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
	WriteSeeded(src, 32u, 16u, kIndSampleSeed);
	std::vector<uint8_t> payload;
	ASSERT_TRUE(EncodeToBytes(&payload, e, src, CLV_VIDEO_FRAME_I, kCarrierQuant, 0));
	const std::vector<uint8_t> source_bytes = ValidArea(src, 32u, 16u);
	// 有效区 = 32×16 + 2 × 16×8 = 768 字节
	ASSERT_EQ(source_bytes.size(), 32u * 16u + 2u * 16u * 8u);

	ASSERT_EQ(CLV_VideoDecoderDecode(d, payload.data(), payload.size(), 1u, 0, 0, nullptr), CLV_VIDEO_OK);
	Drain got = DrainAll(d);
	ASSERT_EQ(got.frames.size(), 1u);
	const std::vector<uint8_t> out_bytes = ValidArea(got.frames[0], 32u, 16u);
	ASSERT_EQ(out_bytes.size(), source_bytes.size());
	const size_t same = EqualBytes(source_bytes, out_bytes);
	// 「输出 == 源」这条变形期望与实际输出的字节数相等判据不符：不等才算断言有分辨力
	EXPECT_NE(same, source_bytes.size()) << "全等 = 变换量化闭环没接上";
	EXPECT_LT(same, source_bytes.size());
	ReleaseAll(&got);
	CLV_VideoDecoderClose(d);
	CLV_VideoFrameRelease(src);
	CLV_VideoEncoderClose(e);
	std::cout << "无损变形：源 vs 输出，" << (source_bytes.size() - same) << " / " << source_bytes.size()
			  << " 字节不同\n";
}

TEST(IndependentVideoMotion, FlatErrorExpectationDiffersFromMeasured)
{
	/* 变形期望：把误差与量化步长的关系抹平 —— 断三档的差度量取同一个值。
	 * 实测必须不满足这条「常数」判据：逐对不相等，且没有一档落到全等的 0。
	 * 采样点取公开判据写死的三档 q = 8 / 32 / 96，同一素材、同一帧序列。 */
	RecordProperty("ReproduceSeed", kIndSampleSeed);
	RecordProperty("QuantSteps", "8,32,96");
	const uint8_t qs[3] = { 8u, 32u, 96u };
	uint64_t measure[3] = { 0ull, 0ull, 0ull };
	for (int i = 0; i < 3; ++i)
	{
		SCOPED_TRACE(static_cast<int>(qs[i]));
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoEncoder* e = OpenEncoder(&err, 32u, 16u, qs[i], 0u);
		ASSERT_NE(e, nullptr);
		CLV_VideoDecoder* d = OpenDecoder(&err, 32u, 16u, 0u);
		ASSERT_NE(d, nullptr);
		CLV_VideoFrame* src = nullptr;
		ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
		WriteSeeded(src, 32u, 16u, kIndSampleSeed);
		std::vector<uint8_t> payload;
		ASSERT_TRUE(EncodeToBytes(&payload, e, src, CLV_VIDEO_FRAME_I, qs[i], 0));
		const std::vector<uint8_t> source_bytes = ValidArea(src, 32u, 16u);
		ASSERT_EQ(CLV_VideoDecoderDecode(d, payload.data(), payload.size(), 1u, 0, 0, nullptr), CLV_VIDEO_OK);
		Drain got = DrainAll(d);
		ASSERT_EQ(got.frames.size(), 1u);
		measure[i] = SumAbsDiff(source_bytes, ValidArea(got.frames[0], 32u, 16u));
		// 步长越大死区越宽 ⇒ 同素材的绝对差和随 q 不减（判据的单调方向）
		if (i > 0) EXPECT_GE(measure[i], measure[i - 1]);
		ReleaseAll(&got);
		CLV_VideoDecoderClose(d);
		CLV_VideoFrameRelease(src);
		CLV_VideoEncoderClose(e);
	}
	RecordProperty("AbsDiffAtQ8", measure[0]);
	RecordProperty("AbsDiffAtQ32", measure[1]);
	RecordProperty("AbsDiffAtQ96", measure[2]);
	// 抹平成常数的变形期望与实际测量不符：相邻两档都不得相等
	EXPECT_NE(measure[0], measure[1]);
	EXPECT_NE(measure[1], measure[2]);
	// 常数化成 0（即「三档都无损」）同样与实际不符
	EXPECT_NE(measure[0], 0ull);
	EXPECT_NE(measure[2], 0ull);
	std::cout << "单调性变形：三档绝对差和 " << measure[0] << " / " << measure[1] << " / " << measure[2]
			  << "，与「取同一个值」的变形期望不等\n";
}
