/* test_video_rejected_state.cpp
 *
 * 公开 C ABI 用例：只 include `include/CLV_Video.h`，不经实现层头。
 *
 * 拒解态的进与出：
 *   进 —— `CLV_VideoDecoderOpen` 命中致命声明档时照常交出句柄（不返 NULL），`*err` 写命中的致命码，
 *   句柄一开始就带拒解态。致命档共三处置位点：位深非 8；`vlc_table_id` / `alpha_enabled` /
 *   `obmc_disable` / `layer_id` 任一呈 v1 不收的形状；声明了工具清单却没给清单内容
 *   （见 `src/video/clv_video.cpp` 的 `CLV_VideoDecoderOpen`）。
 *   返 NULL 的是入参档（尺寸为 0、`quant_precision` 越出声明域 [3, 9]）与分配失败档。
 *   置位之后除 `CLV_VideoDecoderStreamRejected` / `CLV_VideoDecoderReset` / `CLV_VideoDecoderClose`
 *   外每个入口都返 `CLV_VIDEO_STREAM_REJECTED`，且这一判定排在入参检查之前
 *   （`CLV_VideoDecoderDecode` 里 `memset(info)` 之后、载荷与片数判定之前）。
 *   出 —— `CLV_VideoDecoderReset`：公开头把 Reset 列在该名单之外并写明它是该态的出口，
 *   实现里 `rejected = false` 只发生在 Reset 一处，本入口返回 `CLV_VIDEO_OK`。
 *   Reset 不清流声明、也不重跑声明档判定，出口之后的句柄仍带原声明：出帧描述符里的 `bit_depth`
 *   就是当初那个致命声明值（帧缓冲按 8-bit 平面布局分配，布局与位深无关）。
 *
 * 出帧额度：待输出队列攒到「声明的 max_b_frames + 2」支才按显示序（PTS 升序）出第一支
 * （`src/video/dpb.cpp` 的 `Scheduler::PushDecoded`）。本件声明 max_b_frames = 0，
 * 故额度是 2：喂一支不出、喂两支出 PTS 最小那支。
 * 载荷素材由编码器现产：显示 16×16 → 补后 16×16 → 宏块 1×1，一帧一片（slice_mb_count = 0），
 * 故喂 `CLV_VideoDecoderDecode` 时 `fragment_count` = 1（单包帧不带片头）。
 *
 * 本件另管第二套面：帧归还门（`VideoFrameHandout` 那一族）。同一个句柄生命周期面里的另外两档 ——
 * 交出那一次只受理一次的重复归还、以及重置作废之后再归还。素材、出帧额度与上面同一套。
 */

#include "CLV_Video.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

	constexpr uint16_t kW = 16u;
	constexpr uint16_t kH = 16u;
	constexpr uint8_t kQp = 7u;
	constexpr uint8_t kQuant = 24u;

	CLV_VideoStreamParamsC DecoderParams()
	{
		CLV_VideoStreamParamsC p;
		std::memset(&p, 0, sizeof(p));
		p.bit_depth = 8u;
		p.quant_precision = kQp;
		p.max_b_frames = 0u;
		p.obmc_disable = 1u;	// v1 = 1
		p.display_width = kW;
		p.display_height = kH;
		return p;
	}

	/* 两条 I 帧载荷（pts 取 0 与 1）：编码器在这里只是素材产出口，不参与任何断言。
	 * 编码侧位深必须 8，否则连句柄都建不出来 —— 素材与拒解声明是两回事。 */
	std::vector<std::vector<uint8_t>> EncodeTwoIFrames(CLV_VideoError* err)
	{
		std::vector<std::vector<uint8_t>> out;
		CLV_VideoEncParamsC ep;
		std::memset(&ep, 0, sizeof(ep));
		ep.display_width = kW;
		ep.display_height = kH;
		ep.bit_depth = 8u;
		ep.quant_precision = kQp;
		ep.default_quant = kQuant;
		CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&ep, err);
		if (e == nullptr) return out;

		CLV_VideoFrame* f = nullptr;
		if (CLV_VideoEncoderAllocFrame(e, &f) == CLV_VIDEO_OK)
		{
			for (int i = 0; i < 2; ++i)
			{
				CLV_VideoBitstream* bs = nullptr;
				if (CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_I, kQuant, i, i, &bs) != CLV_VIDEO_OK) break;
				const uint8_t* data = nullptr;
				size_t size = 0u;
				if (CLV_VideoBitstreamGetBytes(bs, &data, &size) != CLV_VIDEO_OK)
				{
					CLV_VideoBitstreamRelease(bs);
					break;
				}
				out.emplace_back(data, data + size);
				CLV_VideoBitstreamRelease(bs);	  // 字节已拷进 out，码流归调用方、此处即用完
			}
			CLV_VideoFrameRelease(f);
		}
		CLV_VideoEncoderClose(e);
		return out;
	}

	/* 读一帧名下还剩几次持有，读完原样收回：AddRef 加一、Release 减一，两只口配对，净零。
	 * 前提是该帧**交出**的那一次已经还掉了 —— 还没还时 Release 先消耗那一笔（公开头的口径），
	 * 读数就会顺手把调用方名下的持有吃掉。所以本函数只在归还之后用。 */
	uint32_t HeldCount(CLV_VideoFrame* f)
	{
		const uint32_t up = CLV_VideoFrameAddRef(f);
		CLV_VideoFrameRelease(f);
		return (up == 0u) ? 0u : up - 1u;
	}

}	 // namespace

TEST(VideoRejectedState, RejectedDecoderExitsThroughResetAndDecodes)
{
	CLV_VideoError err = CLV_VIDEO_OK;
	const std::vector<std::vector<uint8_t>> frames = EncodeTwoIFrames(&err);
	ASSERT_EQ(frames.size(), 2u) << CLV_VideoStrError(err);

	CLV_VideoStreamParamsC p = DecoderParams();
	p.bit_depth = 10u;	  // 致命声明档
	err = CLV_VIDEO_OK;
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&p, &err);
	ASSERT_NE(d, nullptr) << "致命声明交出的是拒解态句柄，不是 NULL";
	EXPECT_EQ(err, CLV_VIDEO_BIT_DEPTH_UNSUPPORTED);

	uint8_t rejected = 0u;
	ASSERT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK);
	EXPECT_EQ(rejected, 1u);

	// 拦截先于入参检查：喂真载荷也照样被挡，出参一律被清干净
	CLV_VideoFrameInfoC info;
	std::memset(&info, 0xFF, sizeof(info));	   // 先填非零记号，才看得出这一刀到底写没写字段
	EXPECT_EQ(CLV_VideoDecoderDecode(d, frames[0].data(), frames[0].size(), 1u, 0, 0, &info),
			  CLV_VIDEO_STREAM_REJECTED);
	// Decode 入口的 memset 排在拒解判定之前：这一刀只清零，不填任何字段
	EXPECT_EQ(info.macroblock_count, 0u);
	EXPECT_EQ(info.slice_count, 0u);
	EXPECT_EQ(info.dts_ticks, 0);

	CLV_VideoFrame* out = reinterpret_cast<CLV_VideoFrame*>(0x1u);
	uint8_t got = 1u;
	EXPECT_EQ(CLV_VideoDecoderNextOutput(d, &out, &got), CLV_VIDEO_STREAM_REJECTED);
	EXPECT_EQ(out, nullptr);
	EXPECT_EQ(got, 0u);
	out = reinterpret_cast<CLV_VideoFrame*>(0x1u);
	got = 1u;
	EXPECT_EQ(CLV_VideoDecoderFlush(d, &out, &got), CLV_VIDEO_STREAM_REJECTED);
	EXPECT_EQ(out, nullptr);
	EXPECT_EQ(got, 0u);

	// Reset：该态的出口，本身返回 OK
	EXPECT_EQ(CLV_VideoDecoderReset(d), CLV_VIDEO_OK);
	rejected = 1u;
	ASSERT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK);
	EXPECT_EQ(rejected, 0u);

	// 同一参数、同一份合法载荷重新喂：队列额度 = 声明 0 + 2，故第一支只入队不出帧
	ASSERT_EQ(CLV_VideoDecoderDecode(d, frames[0].data(), frames[0].size(), 1u, 0, 0, &info), CLV_VIDEO_OK);
	EXPECT_EQ(info.disposition, CLV_VIDEO_DISP_DELAYED);
	EXPECT_EQ(info.type, CLV_VIDEO_FRAME_I);
	EXPECT_EQ(info.is_keyframe, 1u);
	out = nullptr;
	got = 1u;
	ASSERT_EQ(CLV_VideoDecoderNextOutput(d, &out, &got), CLV_VIDEO_OK);
	EXPECT_EQ(got, 0u);
	EXPECT_EQ(out, nullptr);

	// 第二支进队即满额度，按 PTS 升序出的是第一支（pts 0），本支仍待输出
	ASSERT_EQ(CLV_VideoDecoderDecode(d, frames[1].data(), frames[1].size(), 1u, 1, 1, &info), CLV_VIDEO_OK);
	EXPECT_EQ(info.disposition, CLV_VIDEO_DISP_DELAYED);
	out = nullptr;
	got = 0u;
	ASSERT_EQ(CLV_VideoDecoderNextOutput(d, &out, &got), CLV_VIDEO_OK);
	ASSERT_EQ(got, 1u);
	ASSERT_NE(out, nullptr);

	CLV_VideoFrameDescC desc;
	CLV_VideoFrameInfoC got_info;
	std::memset(&desc, 0, sizeof(desc));
	std::memset(&got_info, 0, sizeof(got_info));
	ASSERT_EQ(CLV_VideoFrameGetDesc(out, &desc, &got_info), CLV_VIDEO_OK);
	EXPECT_EQ(got_info.type, CLV_VIDEO_FRAME_I);
	EXPECT_EQ(got_info.is_keyframe, 1u);
	EXPECT_EQ(got_info.macroblock_count, 1u);	 // 补后 16×16 → 宏块 1×1
	EXPECT_EQ(got_info.pts_ticks, 0);			 // 出的是 PTS 最小那一支
	EXPECT_EQ(got_info.dts_ticks, 0);
	EXPECT_EQ(got_info.slice_count, 1u);	// slice_mb_count = 0 → 整帧一片
	EXPECT_EQ(desc.pixel_format, CLV_VIDEO_PIX_YUV420P);
	EXPECT_EQ(desc.coded_width, static_cast<uint32_t>(kW));
	EXPECT_EQ(desc.coded_height, static_cast<uint32_t>(kH));
	EXPECT_EQ(desc.display_width, static_cast<uint32_t>(kW));
	EXPECT_EQ(desc.display_height, static_cast<uint32_t>(kH));
	EXPECT_EQ(desc.stride[0], 32u);	   // 行距按 32 字节对齐：Align32(16) = 32
	EXPECT_EQ(desc.stride[1], 32u);	   // 色度半幅 8 → Align32(8) = 32
	EXPECT_EQ(desc.stride[2], 32u);
	// Reset 只清拒解标志，流声明原样留着：出帧描述符照原声明报位深
	EXPECT_EQ(desc.bit_depth, 10u);

	CLV_VideoFrameRelease(out);
	// 出口之后句柄正常可关：Close 从来不在拦截名单里
	CLV_VideoDecoderClose(d);
}

TEST(VideoRejectedState, EachFatalDeclarationIsExitedByReset)
{
	CLV_VideoStreamParamsC cases[6];
	for (CLV_VideoStreamParamsC& c: cases) c = DecoderParams();
	cases[0].bit_depth = 10u;	 // 位深档
	cases[1].bit_depth = 12u;
	cases[2].alpha_enabled = 1u;	// 工具档：四形状任一即命中，命中码都是 TOOL_EXCLUDED
	cases[3].vlc_table_id = 1u;
	cases[4].obmc_disable = 0u;
	cases[5].tool_list_size = 4u;	 // 清单档：声明了清单却没给内容（tool_list 仍是 NULL）
	const CLV_VideoError want[] = { CLV_VIDEO_BIT_DEPTH_UNSUPPORTED, CLV_VIDEO_BIT_DEPTH_UNSUPPORTED,
									CLV_VIDEO_TOOL_EXCLUDED,		 CLV_VIDEO_TOOL_EXCLUDED,
									CLV_VIDEO_TOOL_EXCLUDED,		 CLV_VIDEO_TOOL_LIST_ABSENT };

	for (int i = 0; i < 6; ++i)
	{
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&cases[i], &err);
		ASSERT_NE(d, nullptr) << "声明档 " << i << " 要给句柄";
		EXPECT_EQ(err, want[i]) << "声明档 " << i;

		uint8_t rejected = 0u;
		ASSERT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK) << "声明档 " << i;
		EXPECT_EQ(rejected, 1u) << "声明档 " << i;

		CLV_VideoFrame* out = reinterpret_cast<CLV_VideoFrame*>(0x1u);
		uint8_t got = 1u;
		EXPECT_EQ(CLV_VideoDecoderNextOutput(d, &out, &got), CLV_VIDEO_STREAM_REJECTED) << "声明档 " << i;
		EXPECT_EQ(out, nullptr) << "声明档 " << i;
		EXPECT_EQ(got, 0u) << "声明档 " << i;
		// 空载荷也仍是 STREAM_REJECTED：拒解判定先于入参判定
		CLV_VideoFrame* out2 = reinterpret_cast<CLV_VideoFrame*>(0x1u);
		uint8_t got2 = 1u;
		EXPECT_EQ(CLV_VideoDecoderFlush(d, &out2, &got2), CLV_VIDEO_STREAM_REJECTED) << "声明档 " << i;
		EXPECT_EQ(out2, nullptr) << "声明档 " << i;
		EXPECT_EQ(got2, 0u) << "声明档 " << i;

		EXPECT_EQ(CLV_VideoDecoderReset(d), CLV_VIDEO_OK) << "声明档 " << i;
		rejected = 1u;
		ASSERT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK) << "声明档 " << i;
		EXPECT_EQ(rejected, 0u) << "声明档 " << i;

		// 出口之后 Decode 不再返 STREAM_REJECTED：拒解那一刀确实解除（出帧细节见上一件）。
		// 全零 16 字节走的是解析档：帧头的定长 6 字节与 mb_count 那一个 varint 字节都读得尽，
		// 但 vop_quant（帧头第 2 字节）= 0 越出 [1, 2^7 − 1] → 帧头不成立
		const std::vector<uint8_t> probe(16u, 0u);
		EXPECT_EQ(CLV_VideoDecoderDecode(d, probe.data(), probe.size(), 1u, 0, 0, nullptr), CLV_VIDEO_HEADER_BAD)
			<< "声明档 " << i;

		CLV_VideoDecoderClose(d);
	}
}

TEST(VideoRejectedState, ResetOnNullHandleIsInvalidHandleAndIsRepeatable)
{
	// NULL 档：Reset 也先报句柄
	EXPECT_EQ(CLV_VideoDecoderReset(nullptr), CLV_VIDEO_INVALID_HANDLE);

	CLV_VideoStreamParamsC p = DecoderParams();
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&p, &err);
	ASSERT_NE(d, nullptr) << CLV_VideoStrError(err);
	EXPECT_EQ(err, CLV_VIDEO_OK);

	// 非拒解句柄上 Reset 可连着调：这里没有「已重置」这类状态位拦第二次调用
	EXPECT_EQ(CLV_VideoDecoderReset(d), CLV_VIDEO_OK);
	EXPECT_EQ(CLV_VideoDecoderReset(d), CLV_VIDEO_OK);
	uint8_t rejected = 1u;
	EXPECT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK);
	EXPECT_EQ(rejected, 0u);
	CLV_VideoDecoderClose(d);
}

TEST(VideoRejectedState, DecodeAfterFlushRequiresReset)
{
	// 素材：编码器现产两条 I 帧载荷，本例只用作喂料，不参与断言
	CLV_VideoError err = CLV_VIDEO_OK;
	const std::vector<std::vector<uint8_t>> bs = EncodeTwoIFrames(&err);
	ASSERT_EQ(bs.size(), 2u);

	CLV_VideoStreamParamsC p = DecoderParams();
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&p, &err);
	ASSERT_NE(d, nullptr) << CLV_VideoStrError(err);

	ASSERT_EQ(CLV_VideoDecoderDecode(d, bs[0].data(), bs[0].size(), 1u, 0, 0, nullptr), CLV_VIDEO_OK);
	CLV_VideoFrame* out = nullptr;
	uint8_t got = 0u;
	ASSERT_EQ(CLV_VideoDecoderFlush(d, &out, &got), CLV_VIDEO_OK);
	ASSERT_EQ(got, 1u);
	CLV_VideoFrameRelease(out);

	// 冲刷后直接喂：显式顺序错误，且不留「返回 OK 却不产帧」的静默路径
	EXPECT_EQ(CLV_VideoDecoderDecode(d, bs[1].data(), bs[1].size(), 1u, 1, 1, nullptr), CLV_VIDEO_STATE_ERROR);

	// Reset 是该态的出口：同一载荷照常接受，出帧恢复
	ASSERT_EQ(CLV_VideoDecoderReset(d), CLV_VIDEO_OK);
	EXPECT_EQ(CLV_VideoDecoderDecode(d, bs[1].data(), bs[1].size(), 1u, 1, 1, nullptr), CLV_VIDEO_OK);
	out = nullptr;
	got = 0u;
	EXPECT_EQ(CLV_VideoDecoderFlush(d, &out, &got), CLV_VIDEO_OK);
	EXPECT_EQ(got, 1u);
	if (out != nullptr) CLV_VideoFrameRelease(out);
	CLV_VideoDecoderClose(d);
}

/* 帧归还门的两个档：重复归还、作废之后再归还。
 *
 * 两只口走同一份计数，区别只在报不回报：`CLV_VideoFrameFree` 带回报码且只受理交出那一次，
 * `CLV_VideoFrameRelease` 与 `CLV_VideoFrameAddRef` 配对、交出的那一次还没还时先消耗它。
 * 素材仍由编码器现产；声明 max_b_frames = 0 ⇒ 出帧额度 = 0 + 2，喂满两支才按显示序出第一支，
 * 而第一支是 I 帧 —— 它还留在参考缓冲里，所以归还之后计数不为零。
 * 这一条正是重复归还的判点必须落在状态闸、而不能只落在「计数是否归零」上的原因：
 * 只判归零会让第二次归还把还被别的帧引用的帧当空闲位收走。 */
TEST(VideoFrameHandout, RepeatedFreeIsRejectedAndLeavesTheCountAlone)
{
	CLV_VideoError err = CLV_VIDEO_OK;
	const std::vector<std::vector<uint8_t>> bs = EncodeTwoIFrames(&err);
	ASSERT_EQ(bs.size(), 2u);
	CLV_VideoStreamParamsC p = DecoderParams();
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&p, &err);
	ASSERT_NE(d, nullptr) << CLV_VideoStrError(err);

	for (size_t i = 0u; i < bs.size(); ++i)
	{
		ASSERT_EQ(CLV_VideoDecoderDecode(d, bs[i].data(), bs[i].size(), 1u, static_cast<int64_t>(i),
										 static_cast<int64_t>(i), nullptr),
				  CLV_VIDEO_OK)
			<< "第 " << i << " 支素材";
	}

	CLV_VideoFrame* out = nullptr;
	uint8_t got = 0u;
	ASSERT_EQ(CLV_VideoDecoderNextOutput(d, &out, &got), CLV_VIDEO_OK);
	ASSERT_EQ(got, 1u);
	ASSERT_NE(out, nullptr);

	// 第一次归还走带回报的那扇门
	EXPECT_EQ(CLV_VideoFrameFree(out), CLV_VIDEO_OK);
	// 交出那一次还掉之后，这帧名下只剩参考窗那一次持有
	const uint32_t held = HeldCount(out);
	EXPECT_EQ(held, 1u) << "归还后的净持有应只剩参考窗那一次";

	// 重复归还：状态闸已经落下，第二次、第三次都判参数不成立，且一律不动计数
	EXPECT_EQ(CLV_VideoFrameFree(out), CLV_VIDEO_INVALID_ARGUMENT);
	EXPECT_EQ(HeldCount(out), held) << "被拒的第一次重复归还动了计数";
	EXPECT_EQ(CLV_VideoFrameFree(out), CLV_VIDEO_INVALID_ARGUMENT);
	EXPECT_EQ(HeldCount(out), held) << "被拒的第二次重复归还动了计数";

	// 计数字面未归零 ⇒ 这帧没被当空闲位收走：描述符与时间戳照常读得出
	CLV_VideoFrameDescC desc;
	CLV_VideoFrameInfoC info;
	std::memset(&desc, 0, sizeof(desc));
	std::memset(&info, 0, sizeof(info));
	EXPECT_EQ(CLV_VideoFrameGetDesc(out, &desc, &info), CLV_VIDEO_OK);
	EXPECT_EQ(info.pts_ticks, 0);
	// 还在参考窗里 ⇒ 加得动计数（已作废的帧一律返 0，见下一件）
	EXPECT_EQ(CLV_VideoFrameAddRef(out), held + 1u);
	CLV_VideoFrameRelease(out);

	// 被拒的归还不得把句柄弄坏：队列里剩下那支照常出、照常还
	CLV_VideoFrame* second = nullptr;
	uint8_t got2 = 0u;
	ASSERT_EQ(CLV_VideoDecoderFlush(d, &second, &got2), CLV_VIDEO_OK);
	ASSERT_EQ(got2, 1u);
	ASSERT_NE(second, nullptr);
	EXPECT_EQ(CLV_VideoFrameFree(second), CLV_VIDEO_OK);
	CLV_VideoDecoderClose(d);
}

TEST(VideoFrameHandout, FreeAfterResetIsRejectedWhileTheHandleKeepsWorking)
{
	CLV_VideoError err = CLV_VIDEO_OK;
	const std::vector<std::vector<uint8_t>> bs = EncodeTwoIFrames(&err);
	ASSERT_EQ(bs.size(), 2u);
	CLV_VideoStreamParamsC p = DecoderParams();
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&p, &err);
	ASSERT_NE(d, nullptr) << CLV_VideoStrError(err);

	for (size_t i = 0u; i < bs.size(); ++i)
	{
		ASSERT_EQ(CLV_VideoDecoderDecode(d, bs[i].data(), bs[i].size(), 1u, static_cast<int64_t>(i),
										 static_cast<int64_t>(i), nullptr),
				  CLV_VIDEO_OK)
			<< "第 " << i << " 支素材";
	}
	CLV_VideoFrame* out = nullptr;
	uint8_t got = 0u;
	ASSERT_EQ(CLV_VideoDecoderNextOutput(d, &out, &got), CLV_VIDEO_OK);
	ASSERT_EQ(got, 1u);
	ASSERT_NE(out, nullptr);

	// 不还就重置：重置作废所有已交出的帧 —— 计数清零、像素缓冲回收、指针失效
	ASSERT_EQ(CLV_VideoDecoderReset(d), CLV_VIDEO_OK);
	EXPECT_EQ(CLV_VideoFrameFree(out), CLV_VIDEO_INVALID_ARGUMENT);
	// 同一次作废在另外两只口上是同一档：不写出参、也不给计数
	CLV_VideoFrameDescC desc;
	std::memset(&desc, 0, sizeof(desc));
	CLV_VideoFrameInfoC info;
	std::memset(&info, 0, sizeof(info));
	EXPECT_EQ(CLV_VideoFrameGetDesc(out, &desc, &info), CLV_VIDEO_INVALID_ARGUMENT);
	EXPECT_EQ(desc.coded_width, 0u) << "作废帧不许写出参";
	EXPECT_EQ(desc.plane[0], nullptr) << "作废帧不许写出参";
	EXPECT_EQ(info.pts_ticks, 0) << "作废帧不许写出参";
	EXPECT_EQ(CLV_VideoFrameAddRef(out), 0u);	 // 已作废的帧不复活
	// 上面这几次读的是还挂在句柄名下的那个对象壳（像素缓冲已退还），回收点是关闭

	// 句柄照常：重置之后同一份载荷重新喂、照常出帧，归还也在关闭之前做完
	for (size_t i = 0u; i < bs.size(); ++i)
	{
		ASSERT_EQ(CLV_VideoDecoderDecode(d, bs[i].data(), bs[i].size(), 1u, static_cast<int64_t>(i),
										 static_cast<int64_t>(i), nullptr),
				  CLV_VIDEO_OK)
			<< "重置后第 " << i << " 支素材";
	}
	CLV_VideoFrame* again = nullptr;
	uint8_t got2 = 0u;
	ASSERT_EQ(CLV_VideoDecoderNextOutput(d, &again, &got2), CLV_VIDEO_OK);
	ASSERT_EQ(got2, 1u);
	ASSERT_NE(again, nullptr);
	EXPECT_EQ(CLV_VideoFrameFree(again), CLV_VIDEO_OK);
	/* 关闭之后那次归还不在这里断言：公开头只承诺「关闭后不得再使用该句柄」，
	 * 并不承诺关闭后的归还回报什么 —— 句柄连壳一起没了，旧指针此后怎么读都不作数。 */
	CLV_VideoDecoderClose(d);
}
