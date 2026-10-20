/* test_ind_video.cpp
 *
 * 视频面声明域的逐档核对：有效量化步长 q′ 的域、帧头里 vop_quant 与 dquant 两个字段的可表达域、
 * 块模式位图、按宏块算的补后尺寸、补后尺寸积的像素上界。
 *
 * 每档的期望只有三处来源，都不向被测函数取数：
 *   公开头 include/CLV_Video.h 的契约文字：v1 量化精度 7 ⇒ q ∈ [1, 127]、原始尺寸为 0 不合法、
 *     补后尺寸 = 原始尺寸向上取整到 16 的倍数且不落字节、丢帧不是错误。
 *   实现里的声明域常量：quant.h 的 QuantCeiling / kQuantMin / kQuantPrecisionMin / kQuantPrecisionMax、
 *     headers.h 的 kVopDquantMin / kVopDquantMax / kDquantByteReservedMask、block.h 的 kPatternMask6、
 *     reconstruct.h 的 kMbSize / kIntraPredLevel8 / kMaxReconstructPixels。
 *   注释里写清算式的量：ceil16 的补后尺寸、按 32 字节对齐的行距、2^p − 1、在场块数 × 单块字节数。
 * 正常档与正好卡在上界那一档都断「对」——实现层的界判式一律是「不大于上界」，上界本身落在受理侧；
 * 越界档断「拒」。
 */

#include "../video/video_payload_fixtures_p.h"
#include "CLV_Video.h"
#include "video/block.h"
#include "video/headers.h"
#include "video/macroblock.h"
#include "video/payload.h"
#include "video/quant.h"
#include "video/reconstruct.h"
#include "video/status.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{

	using clv::video::BitWriter;
	using clv::video::BvopMacroblockHeader;
	using clv::video::BvopPrediction;
	using clv::video::CoefCoding;
	using clv::video::DecodeBlockData;
	using clv::video::DecodeBvopRun;
	using clv::video::EncodeBlockData;
	using clv::video::EncodeBvopMacroblockHeader;
	using clv::video::Macroblock;
	using clv::video::VideoErr;
	using clv::video::VopPayload;

	/* 声明域常量：v1 只有一个量化精度取值，界 = 2^7 − 1 = 127（公开头的量化段写死）。 */
	constexpr uint8_t kPrecisionV1 = 7u;
	constexpr uint8_t kCeilingV1 = 127u;		  // 2^7 − 1 的手算结果
	constexpr uint8_t kCeilingAt3 = 7u;			  // 声明域下界 p = 3 → 2^3 − 1
	constexpr uint8_t kCeilingAt9 = UINT8_MAX;	  // p = 9 → 2^9 − 1 = 511，但载体只有 1 字节 → 收口 255
	constexpr uint8_t kCarrierQuant = 24u;		  // 冻结载体所在档：q′ = 24 → 每系数 9 bit

	/* 块模式位图：bit5..bit2 = 亮度块 0..3、bit1 = Cb、bit0 = Cr，合成 6 bit。
	 * 位序与在场块数都按 block.h 的口径写死在常量与算式里，不从 PresentBlockCount 反推。 */
	constexpr uint8_t kPatternNone = 0x00u;
	constexpr uint8_t kPatternTwoLuma = 0x30u;
	constexpr uint8_t kPatternAll = 0x3Fu;
	constexpr uint8_t kPatternOverSixBits = 0x40u;

	/* 界值与字段宽度的静态钉：全部与手算档对账，算式写在名字旁边的注释里。 */
	static_assert(clv::video::QuantCeiling(kPrecisionV1) == kCeilingV1, "界 = 2^quant_precision - 1，p = 7 → 127");
	static_assert(clv::video::QuantCeiling(clv::video::kQuantPrecisionMin) == kCeilingAt3, "p = 3 → 7");
	static_assert(clv::video::QuantCeiling(clv::video::kQuantPrecisionMax) == kCeilingAt9, "p = 9 → 255（8 位收口）");
	static_assert(clv::video::kQuantMin == 1, "下界恒 1，与 quant_precision 无关");

	/* vop_quant 在帧头与片头都是一字节：可表达顶 = 255。p ≥ 8 时上界正落这一格，
	 * 于是「界上再加一档」在该声明档下无法写进字段。 */
	static_assert(sizeof(clv::video::VopHeader::quant) == 1u, "帧头 vop_quant 是 1 字节");
	static_assert(sizeof(clv::video::SliceHeader::vop_quant) == 1u, "片头携带的 vop_quant 同宽");

	/* dquant：1 字节里的 5 bit 二补码（32 个取值 = [−16, 15]），bit5-7 保留恒 0。
	 * 域外的写法只能靠违反保留位表达，故本档的「越界」= 保留位非 0。 */
	static_assert(clv::video::kVopDquantMin == -16 && clv::video::kVopDquantMax == 15, "5 bit 二补码域");
	static_assert(static_cast<int>(clv::video::kVopDquantMax) - static_cast<int>(clv::video::kVopDquantMin) + 1 == 32,
				  "5 bit 恰容 32 个取值");
	static_assert(clv::video::kDquantByteReservedMask == 0xE0u, "保留位掩码 = bit5-7 = 1110 0000");

	/* 尺寸字段的宽度：声明侧 16 位、推导出的补后侧 32 位。 */
	static_assert(sizeof(CLV_VideoEncParamsC::display_width) == 2u, "原始尺寸声明字段 16 位");
	static_assert(sizeof(CLV_VideoStreamParamsC::display_width) == 2u, "解码侧同宽");
	static_assert(sizeof(CLV_VideoFrameDescC::coded_width) == 4u, "补后尺寸出参 32 位");

	/* 一个宏块的在场块数与单块系数数：位图全在 = 6 块、每块 64 系数。 */
	static_assert(clv::video::kPatternMask6 == 0x3F, "块模式 6 bit");
	static_assert(clv::video::kBlocksPerMacroblock == 6, "一宏块六块");

	/* 出帧队列的显示序取样：先抽 NextOutput 再抽 Flush，直到两条都空。
	 * frames 里攒下的那一次持有是刻意留着的：用例要等两条口都抽完才数帧序、比像素，
	 * 一帧一还就把跨帧比对那一档比不了（占用的账按「同时在场的帧数」记，未归还那一份落在调用方名下）。
	 * 归还集中在 FreeAll 这一处，位置固定在关闭句柄之前。 */
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
		/* 两条口都在入口先把帧出参写成空指针，抽到返回码不成立或 got = 0 的那一档手里没有帧，
		 * 早退这一支不需要归还；攒下的帧全部由 FreeAll 交回。 */
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

	/* 逐帧交回带回报的那扇门：取帧口加上的那一次持有只受理一次归还，成功返 OK，
	 * 三档不成立（传入为空 / 已被重置或关闭作废 / 同一帧重复归还）都记进断言，
	 * 归还失败不静默。断言用非致命档，一帧还不上也要把剩下的逐帧走完。 */
	void FreeAll(Drain* d)
	{
		for (CLV_VideoFrame* f: d->frames) EXPECT_EQ(CLV_VideoFrameFree(f), CLV_VIDEO_OK);
		d->frames.clear();
		d->pts.clear();
	}

	CLV_VideoDecoder* OpenDecoder(CLV_VideoError* err, uint16_t w, uint16_t h, uint8_t precision = kPrecisionV1,
								  uint8_t max_b = 0u)
	{
		CLV_VideoStreamParamsC p {};
		p.bit_depth = 8u;
		p.quant_precision = precision;
		p.max_b_frames = max_b;
		p.obmc_disable = 1u;	// v1 = 1
		p.display_width = w;
		p.display_height = h;
		return CLV_VideoDecoderOpen(&p, err);
	}

	CLV_VideoEncoder* OpenEncoder(CLV_VideoError* err, uint16_t w, uint16_t h, uint8_t q,
								  uint8_t precision = kPrecisionV1, uint8_t max_b = 0u, uint16_t slice_mb = 0u)
	{
		CLV_VideoEncParamsC p {};
		p.display_width = w;
		p.display_height = h;
		p.bit_depth = 8u;
		p.quant_precision = precision;
		p.default_quant = q;
		p.max_b_frames = max_b;
		p.slice_mb_count = slice_mb;
		return CLV_VideoEncoderOpen(&p, err);
	}

	/* 把可写平面的有效区按 (平面, 行, 列) 与一个固定种子定值填满；任何一块错位都会比不出来。
	 * 种子是常量，取样结果与素材都可复现。 */
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
					row[c] = static_cast<uint8_t>((pl * 61u + r * 7u + c * 3u + seed) & 0xFFu);
			}
		}
	}

	/* 有效区按 Y、U、V 顺序拼成一个字节串，用于逐字节比较与全等计数。 */
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

	size_t EqualBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
	{
		const size_t n = (a.size() < b.size()) ? a.size() : b.size();
		size_t same = 0u;
		for (size_t i = 0u; i < n; ++i)
			if (a[i] == b[i]) ++same;
		return same;
	}

	/* 编一帧并把整帧载荷拷出来：码流缓冲归调用方，载荷字节只到 Release 前有效。 */
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

	/* 单宏块（16×16）的整帧载荷：一个宏块 + 指定位图与块数据，单片帧不带片头。
	 * 位图全零那一档不带块数据，于是块长与生效步长之间没有牵连，帧头字段可以单独改。 */
	bool BuildOneMbPayload(std::vector<uint8_t>* out, uint8_t vop_type, bool not_coded, uint8_t q, int8_t dquant,
						   uint8_t cbpy, uint8_t cbpc, const uint8_t* block, size_t block_size)
	{
		VopPayload in;
		in.head.type = vop_type;
		in.head.quant = q;
		in.head.fcode_forward = 1u;
		in.head.fcode_backward = 1u;
		in.head.intra_dc_vlc_thr = 0u;
		in.head.dquant = dquant;
		in.head.mb_count = 1u;
		Macroblock m;
		m.head.not_coded = not_coded;
		m.head.mb_type = (vop_type == clv::video::kVopTypeI) ? clv::video::kMbTypeIntra : clv::video::kMbTypeInter;
		m.head.cbpc = cbpc;
		m.head.cbpy = cbpy;
		m.head.dquant = 0;
		m.head.block_size = not_coded ? 0u : static_cast<uint32_t>(block_size);
		if (! not_coded && block_size != 0u) m.block.assign(block, block + block_size);
		in.macroblocks.push_back(std::move(m));
		return clv::video::EncodeVopPayload(kPrecisionV1, 1u, in, out) == VideoErr::kOk;
	}

	/* 载荷里的字段偏移由帧头布局给出：0 = 帧类型、1 = vop_quant、4 = 帧标志、5 = dquant 字节，
	 * 6 起是 mb_count 的 varint（mb_count = 1 → 最短形式 1 字节），再后面就是宏块层。 */
	constexpr size_t kVopQuantByte = 1u;
	constexpr size_t kVopDquantByte = 5u;
	constexpr size_t kMbLayerByte = 7u;

}	 // namespace

namespace
{

	/* 生效步长 q′ = clamp(q + d_vop + d_mb, 1, 2^quant_precision − 1)：
	 * want 一律按这条算式手算，越界方向写清夹到哪一个界。 */
	struct ClampedStep
	{
		uint8_t q;
		int8_t d_vop;
		int8_t d_mb;
		uint8_t want;
		const char* how;
	};

	const ClampedStep kClampedSteps[] = {
		{ 1u,	0,	 0,	 1u,   "下界 1 + 0 + 0 = 1"						  },
		{ 126u, 0,	 0,	 126u, "界内直取 126"							  },
		{ 127u, 0,	 0,	 127u, "界上档 127 = 2^7 - 1"					  },
		{ 127u, 15,	 1,	 127u, "127 + 15 + 1 = 143 越上界 → 夹回 127"	  },
		{ 1u,	-16, -2, 1u,   "1 - 16 - 2 = -17 越下界 → 夹回 1"		  },
		{ 10u,	14,	 0,	 24u,  "10 + 14 = 24，界内直取（冻结载体所在档）" },
	};

	/* 帧头 vop_quant 一字节的可表达档：p = 7 的界是 127，界上加一（128）与字段顶格（255）都写得出，
	 * 故这两档必须拒；p = 9 的界按 2^9 − 1 = 511 推再收口到 1 字节 = 255，顶格即界 ⇒ 界外写不出来。 */
	struct HeaderQuantStep
	{
		uint8_t precision;
		uint8_t quant_byte;
		bool accept;
		const char* how;
	};

	const HeaderQuantStep kHeaderQuantSteps[] = {
		{ 7u, 1u,	true,  "p=7 下界"										 },
		{ 7u, 126u, true,  "p=7 界内，界上减一"								 },
		{ 7u, 127u, true,  "p=7 界上档 = 127"								 },
		{ 7u, 128u, false, "p=7 界外加一，一字节写得进 → 必拒"				 },
		{ 7u, 255u, false, "字段顶格 255，仍越 p=7 的界"					 },
		{ 9u, 128u, true,  "界随声明精度走：p=9 的界 = 255，128 落界内"		 },
		{ 9u, 255u, true,  "p=9 的界上档 = 字段顶格 255，顶格之上无可表达值" },
	};

	/* dquant 档：五 bit 二补码的上下界各一档收；越界只能靠违反保留位表达，那一档必拒。 */
	struct DquantStep
	{
		int8_t dquant;
		uint8_t reserved_or;
		bool accept;
		const char* how;
	};

	const DquantStep kDquantSteps[] = {
		{ -16, 0u,	  true,	 "下界 -16 = 0x10 的五 bit 二补码原值" },
		{ 15,  0u,	  true,	 "上界 +15 = 0x0F"					   },
		{ 0,   0x20u, false, "bit5 置 1：域外写法即保留位非 0"	   },
		{ 0,   0xE0u, false, "bit5-7 全置：同上，另一形状"		   },
	};

}	 // namespace

TEST(IndependentVideo, QuantStepDomainLadder)
{
	for (const ClampedStep& s: kClampedSteps)
	{
		SCOPED_TRACE(s.how);
		// q′ 的夹取算式在 headers.h 的 EffectiveQuant 条目里，want 来自上表的手算档
		EXPECT_EQ(clv::video::EffectiveQuant(s.q, s.d_vop, s.d_mb, kPrecisionV1), s.want);
	}

	/* 建句柄与编码入口两档：default_quant / q 越出 [1, 2^p − 1] 分别交不出句柄、产不出载荷。
	 * 0 与 128 都是 1 字节字段写得出的越界值，故这两档是「拒」而不是「不可表达」。 */
	const struct
	{
		uint8_t q;
		bool accept;
		const char* how;
	} entry[] = {
		{ 0u,	false, "下界减一：0 不是合法步长"	 },
		{ 1u,	true,  "下界"						 },
		{ 126u, true,  "界上减一"					 },
		{ 127u, true,  "界上档 2^7 - 1"				 },
		{ 128u, false, "界上加一，字段可表达 → 必拒" },
	};

	for (const auto& s: entry)
	{
		SCOPED_TRACE(s.how);
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoEncoder* e = OpenEncoder(&err, 16u, 16u, s.q);
		if (! s.accept)
		{
			EXPECT_EQ(e, nullptr);
			EXPECT_EQ(err, CLV_VIDEO_QUANT_OUT_OF_RANGE);
			continue;
		}
		ASSERT_NE(e, nullptr) << "建句柄档";
		EXPECT_EQ(err, CLV_VIDEO_OK);
		CLV_VideoFrame* f = nullptr;
		ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
		ASSERT_NE(f, nullptr);
		std::vector<uint8_t> payload;
		ASSERT_TRUE(EncodeToBytes(&payload, e, f, CLV_VIDEO_FRAME_I, s.q, 0));
		/* 载荷在 EncodeToBytes 里已经整份拷出，这块入帧缓冲到此用完 ⇒ 立刻归还，不把它带过后面那段
		 * 解码与出帧的断言。编码器自有的两份参考缓冲不引用调用方这一帧，先还不影响解码。 */
		EXPECT_EQ(CLV_VideoFrameFree(f), CLV_VIDEO_OK);
		f = nullptr;	// 归还之后该帧指针失效
		CLV_VideoDecoder* d = OpenDecoder(&err, 16u, 16u);
		ASSERT_NE(d, nullptr);
		CLV_VideoFrameInfoC info {};
		EXPECT_EQ(CLV_VideoDecoderDecode(d, payload.data(), payload.size(), 1u, 0, 0, &info), CLV_VIDEO_OK);
		EXPECT_EQ(info.quant_used, s.q);	// 帧信息交回的是帧头声明的 q
		EXPECT_EQ(info.type, CLV_VIDEO_FRAME_I);
		Drain got = DrainAll(d);
		EXPECT_EQ(got.frames.size(), 1u) << "收档必须真出一帧";
		FreeAll(&got);
		CLV_VideoDecoderClose(d);
		CLV_VideoEncoderClose(e);
	}

	// 编码入口的第二处判定：界内句柄喂越界 q，出参指针停在空指针（不交出半成品码流）
	{
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoEncoder* e = OpenEncoder(&err, 16u, 16u, kCeilingV1);
		ASSERT_NE(e, nullptr);
		CLV_VideoFrame* f = nullptr;
		ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
		const uint8_t over[] = { static_cast<uint8_t>(kCeilingV1 + 1u), static_cast<uint8_t>(kCeilingV1 + 2u), 0u };
		for (uint8_t bad: over)
		{
			SCOPED_TRACE(static_cast<int>(bad));
			CLV_VideoBitstream* bs = reinterpret_cast<CLV_VideoBitstream*>(1u);
			EXPECT_EQ(CLV_VideoEncoderEncode(e, f, CLV_VIDEO_FRAME_I, bad, 0, 0, &bs),
					  CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL);
			EXPECT_EQ(bs, nullptr);
		}
		/* 三档越界请求都在入口被拒、一段载荷都没产出，这块入帧缓冲到循环末尾就用完了 ⇒ 立刻归还。
		 * 归还走带回报的那扇门：成功归还返 OK，命中「空指针 / 已作废 / 重复归还」三档才判参数不成立。 */
		EXPECT_EQ(CLV_VideoFrameFree(f), CLV_VIDEO_OK);
		f = nullptr;	// 归还之后该帧指针失效
		CLV_VideoEncoderClose(e);
	}

	/* 帧头 vop_quant 字段档：位图全零的宏块不带块数据，块长与生效步长没有牵连，
	 * 故这一档只剩字段自身的值域判定；界随 quant_precision 派生，不是写死的 127。 */
	for (const HeaderQuantStep& s: kHeaderQuantSteps)
	{
		SCOPED_TRACE(s.how);
		std::vector<uint8_t> payload;
		// 载荷按 q = 1 建（两个声明档都界内），再把帧头第 1 字节改成取样值
		ASSERT_TRUE(BuildOneMbPayload(&payload, clv::video::kVopTypeI, false, 1u, 0, kPatternNone, 0u, nullptr, 0u));
		ASSERT_EQ(payload.size(), 9u) << "定长 6 + mb_count 的 varint 1 + 宏块层 2 字节";
		ASSERT_EQ(payload[kVopQuantByte], 1);
		payload[kVopQuantByte] = s.quant_byte;
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoDecoder* d = OpenDecoder(&err, 16u, 16u, s.precision);
		ASSERT_NE(d, nullptr);
		EXPECT_EQ(err, CLV_VIDEO_OK);
		CLV_VideoFrameInfoC info {};
		const CLV_VideoError r = CLV_VideoDecoderDecode(d, payload.data(), payload.size(), 1u, 0, 0, &info);
		if (s.accept)
		{
			EXPECT_EQ(r, CLV_VIDEO_OK);
			EXPECT_EQ(info.quant_used, s.quant_byte);
			Drain got = DrainAll(d);
			EXPECT_EQ(got.frames.size(), 1u);
			FreeAll(&got);
		}
		else
		{
			// 帧头七字段不成立 = 丢这一帧，公开码走帧头档；句柄不进拒解态
			EXPECT_EQ(r, CLV_VIDEO_HEADER_BAD);
			Drain got = DrainAll(d);
			EXPECT_EQ(got.frames.size(), 0u) << "拒档不得产出帧";
			FreeAll(&got);
			uint8_t rejected = 1u;
			EXPECT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK);
			EXPECT_EQ(rejected, 0u) << "字段值域不成立不该把整条流置拒解";
		}
		CLV_VideoDecoderClose(d);
	}

	for (const DquantStep& s: kDquantSteps)
	{
		SCOPED_TRACE(s.how);
		std::vector<uint8_t> payload;
		// dquant 的界内档由写方直接落进字节；保留位档在语法里写不出来，只能在字节上置位复现
		ASSERT_TRUE(BuildOneMbPayload(&payload, clv::video::kVopTypeI, false, kCeilingV1, s.dquant, kPatternNone, 0u,
									  nullptr, 0u));
		ASSERT_EQ(payload[kVopDquantByte], static_cast<uint8_t>(s.dquant & 0x1F));
		payload[kVopDquantByte] = static_cast<uint8_t>(payload[kVopDquantByte] | s.reserved_or);
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoDecoder* d = OpenDecoder(&err, 16u, 16u);
		ASSERT_NE(d, nullptr);
		CLV_VideoFrameInfoC info {};
		const CLV_VideoError r = CLV_VideoDecoderDecode(d, payload.data(), payload.size(), 1u, 0, 0, &info);
		EXPECT_EQ(r, s.accept ? CLV_VIDEO_OK : CLV_VIDEO_HEADER_BAD);
		if (s.accept) EXPECT_EQ(info.quant_used, kCeilingV1);
		Drain got = DrainAll(d);
		EXPECT_EQ(got.frames.size(), s.accept ? 1u : 0u);
		FreeAll(&got);
		CLV_VideoDecoderClose(d);
	}
	std::cout << "量化档：夹取算式 6 档、建句柄与编码入口 5 档、帧头字段 7 档、dquant 4 档，"
				 "界全部由 quant_precision 派生\n";
}

/* 定宽那一法的块数据在本件里按定义式现搭，不借冻结件的字节：冻结件按编码器实际采用的
	 * 那一档（法 1 档，先写 EOB 再用位数递增的 Exp-Golomb 编码写幅值）生成，而这三格要的是「帧头 bit5-7 = 0 ⇒ 读方按定宽解」那条通路。
	 * 每系数 1 bit 符号 + bits(q')−1 bit 幅值、MSB 先出、零也算载荷 ⇒ 在场块数 × 64 × bits ÷ 8。
	 * 系数取冻结件里的 DC 电平（−170），所以图样仍是独立算出来的，只是打包在这一件里现做。
	 * 定义式与调用点都只在本件内，故取内部链接，不带进测试可执行文件的其他编译单元。 */
static std::vector<uint8_t> FixedCarrier(int blocks, int32_t dc)
{
	const uint8_t bits = clv::video::BlockBits(kCarrierQuant, true);
	BitWriter bw;
	for (int b = 0; b < blocks; ++b)
	{
		for (int i = 0; i < clv::video::kBlockCoefs; ++i)
		{
			const int32_t v = (i == 0) ? dc : 0;
			bw.Write(v < 0 ? 1u : 0u, 1u);
			bw.Write(static_cast<uint32_t>(v < 0 ? -static_cast<int64_t>(v) : v), static_cast<uint8_t>(bits - 1u));
		}
	}
	bw.AlignToByte(0u);
	return bw.Bytes();
}

TEST(IndependentVideo, BlockPatternBitmapLadder)
{
	/* 位图全零：六个块都不在场 ⇒ 块数据零长度 ⇒ 重建就是帧内预测电平。
	 * 期望电平 = 2^(bpp − 1)，bpp = 8 ⇒ 128（reconstruct.h 的 kBpp / kIntraPredLevel8），
	 * 残差项按零处置 ⇒ 三平面有效区每一字节都是 128。 */
	{
		std::vector<uint8_t> payload;
		ASSERT_TRUE(
			BuildOneMbPayload(&payload, clv::video::kVopTypeI, false, kCarrierQuant, 0, kPatternNone, 0u, nullptr, 0u));
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoDecoder* d = OpenDecoder(&err, 16u, 16u);
		ASSERT_NE(d, nullptr);
		ASSERT_EQ(CLV_VideoDecoderDecode(d, payload.data(), payload.size(), 1u, 0, 0, nullptr), CLV_VIDEO_OK);
		Drain got = DrainAll(d);
		ASSERT_EQ(got.frames.size(), 1u);
		const std::vector<uint8_t> pix = ValidArea(got.frames[0], 16u, 16u);
		// 有效区 = 16×16 + 2 × 8×8 = 384 字节，全部等于帧内预测电平
		ASSERT_EQ(pix.size(), 16u * 16u + 2u * 8u * 8u);
		for (uint8_t v: pix) EXPECT_EQ(v, clv::video::kIntraPredLevel8);
		FreeAll(&got);
		CLV_VideoDecoderClose(d);
	}

	/* 部分在场：位图 0x30 = 亮度块 0 与 1 在场 ⇒ 定宽块数据 = 2 × 单块 72 B = 144 B，
	 * 两块各带一个非零 DC（电平取冻结件的 −170）。 */
	{
		const std::vector<uint8_t> two = FixedCarrier(2, clvtest::kZeroFrameMbBlock0DcQf);
		ASSERT_EQ(two.size(), 2u * clvtest::kCarrierBlockBytesAt24);
		ASSERT_EQ(two.size(), 144u);
		std::vector<uint8_t> payload;
		ASSERT_TRUE(BuildOneMbPayload(&payload, clv::video::kVopTypeI, false, kCarrierQuant, 0,
									  static_cast<uint8_t>(kPatternTwoLuma >> 2),
									  static_cast<uint8_t>(kPatternTwoLuma & 0x03u), two.data(), two.size()));
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoDecoder* d = OpenDecoder(&err, 16u, 16u);
		ASSERT_NE(d, nullptr);
		CLV_VideoFrameInfoC info {};
		EXPECT_EQ(CLV_VideoDecoderDecode(d, payload.data(), payload.size(), 1u, 0, 0, &info), CLV_VIDEO_OK);
		Drain got = DrainAll(d);
		EXPECT_EQ(got.frames.size(), 1u) << "部分在场是界内档，必须出帧";
		FreeAll(&got);
		CLV_VideoDecoderClose(d);
	}

	/* 全在场：位图 0x3F ⇒ 定宽块数据 = 6 × 72 = 432 B。 */
	{
		const std::vector<uint8_t> six = FixedCarrier(6, clvtest::kZeroFrameMbBlock0DcQf);
		ASSERT_EQ(six.size(), 432u);
		std::vector<uint8_t> payload;
		ASSERT_TRUE(BuildOneMbPayload(&payload, clv::video::kVopTypeI, false, kCarrierQuant, 0,
									  static_cast<uint8_t>(clvtest::kZeroFrameMbCbpy),
									  static_cast<uint8_t>(clvtest::kZeroFrameMbCbpc), six.data(), six.size()));
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoDecoder* d = OpenDecoder(&err, 16u, 16u);
		ASSERT_NE(d, nullptr);
		EXPECT_EQ(CLV_VideoDecoderDecode(d, payload.data(), payload.size(), 1u, 0, 0, nullptr), CLV_VIDEO_OK);
		Drain got = DrainAll(d);
		EXPECT_EQ(got.frames.size(), 1u);
		FreeAll(&got);
		CLV_VideoDecoderClose(d);
	}

	/* 位为 1 但 64 个系数全零：定宽那一法把每一位都写出来，全零块只允许出现在位图为 0 的位置上。
	 * 块数据取 432 个零字节 —— 长度与位图自洽（6 × 72），越界只在系数层，故拒它靠的就是这条判据。 */
	{
		std::vector<uint8_t> zeros(6u * clvtest::kCarrierBlockBytesAt24, 0u);
		ASSERT_EQ(zeros.size(), 432u);
		std::vector<uint8_t> payload;
		ASSERT_TRUE(BuildOneMbPayload(&payload, clv::video::kVopTypeI, false, kCarrierQuant, 0,
									  static_cast<uint8_t>(clvtest::kZeroFrameMbCbpy),
									  static_cast<uint8_t>(clvtest::kZeroFrameMbCbpc), zeros.data(), zeros.size()));
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoDecoder* d = OpenDecoder(&err, 16u, 16u);
		ASSERT_NE(d, nullptr);
		// 取值不成立那一档：块模式位与块内容矛盾，走值域码
		EXPECT_EQ(CLV_VideoDecoderDecode(d, payload.data(), payload.size(), 1u, 0, 0, nullptr), CLV_VIDEO_VALUE_RANGE);
		Drain got = DrainAll(d);
		EXPECT_EQ(got.frames.size(), 0u) << "拒档不得产出帧";
		FreeAll(&got);
		CLV_VideoDecoderClose(d);

		int32_t qf[clv::video::kBlocksPerMacroblock][clv::video::kBlockCoefs] = {};
		EXPECT_EQ(DecodeBlockData(kPatternAll, zeros.data(), zeros.size(), kCarrierQuant, true, CoefCoding::kFixed, qf),
				  VideoErr::kValueRange);
	}

	/* 位图超过 6 bit：bit6 起不属于块模式，读写两侧都在入口判取值不成立。 */
	{
		int32_t qf[clv::video::kBlocksPerMacroblock][clv::video::kBlockCoefs] = {};
		std::vector<uint8_t> bytes;
		EXPECT_EQ(EncodeBlockData(kPatternOverSixBits, qf, kCarrierQuant, true, CoefCoding::kFixed, &bytes),
				  VideoErr::kValueRange);
		EXPECT_EQ(DecodeBlockData(kPatternOverSixBits, nullptr, 0u, kCarrierQuant, true, CoefCoding::kFixed, qf),
				  VideoErr::kValueRange);
		// 同一档在 B-VOP 的宏块头层：那里的 cbpy 是 6 bit 全块模式，64 即越界
		BitWriter bw;
		BvopMacroblockHeader mb;
		mb.not_coded = false;
		mb.mb_type = clv::video::kBvopDirect;
		mb.cbpy = kPatternOverSixBits;
		mb.block_size = 0u;
		const BvopPrediction pred {};
		EXPECT_EQ(EncodeBvopMacroblockHeader(&bw, mb, pred), VideoErr::kValueRange);
		// 读侧：not_coded = 0、mb_type = 0（direct，不带 mvd）、cbpy 的 varint = 0x40
		const uint8_t wire[3] = { 0x00u, 0x00u, 0x40u };
		std::vector<clv::video::BvopMacroblock> run;
		size_t consumed = 0u;
		EXPECT_EQ(DecodeBvopRun(wire, sizeof(wire), 1u, 1ull, &run, &consumed), VideoErr::kValueRange);
	}

	/* 帧内宏块没有 skip 语义：I-VOP 里全零内容走位图全零，不走 not_coded。
	 * 写方直接拒；读方读到那个位型也拒，两档返回码不同（写方是值域、读方是帧头不成立）。 */
	{
		VopPayload in;
		in.head.type = clv::video::kVopTypeI;
		in.head.quant = kCarrierQuant;
		in.head.fcode_forward = 1u;
		in.head.fcode_backward = 1u;
		in.head.mb_count = 1u;
		Macroblock m;
		m.head.not_coded = true;
		m.head.mb_type = clv::video::kMbTypeIntra;
		in.macroblocks.push_back(m);
		std::vector<uint8_t> bytes;
		EXPECT_EQ(clv::video::EncodeVopPayload(kPrecisionV1, 1u, in, &bytes), VideoErr::kValueRange);

		std::vector<uint8_t> payload;
		ASSERT_TRUE(
			BuildOneMbPayload(&payload, clv::video::kVopTypeI, false, kCarrierQuant, 0, kPatternNone, 0u, nullptr, 0u));
		payload[kMbLayerByte] = static_cast<uint8_t>(payload[kMbLayerByte] | 0x80u);	// 宏块层第一个位 = not_coded
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoDecoder* d = OpenDecoder(&err, 16u, 16u);
		ASSERT_NE(d, nullptr);
		EXPECT_EQ(CLV_VideoDecoderDecode(d, payload.data(), payload.size(), 1u, 0, 0, nullptr), CLV_VIDEO_HEADER_BAD);
		Drain got = DrainAll(d);
		EXPECT_EQ(got.frames.size(), 0u);
		FreeAll(&got);
		CLV_VideoDecoderClose(d);
	}

	/* 帧间的 skip 合法：not_coded 不带 motion_vector()，矢量按零处理 ⇒ 预测块就是参考帧同位块，
	 * 于是这一帧的输出与它所参考的那一帧逐字节相同（零运动是取样式的退化情形，不是第二条通路）。 */
	{
		std::vector<uint8_t> ipay;
		ASSERT_TRUE(
			BuildOneMbPayload(&ipay, clv::video::kVopTypeI, false, kCarrierQuant, 0, kPatternNone, 0u, nullptr, 0u));
		std::vector<uint8_t> ppay;
		ASSERT_TRUE(BuildOneMbPayload(&ppay, clv::video::kVopTypeP, true, kCarrierQuant, 0, 0u, 0u, nullptr, 0u));
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoDecoder* d = OpenDecoder(&err, 16u, 16u);
		ASSERT_NE(d, nullptr);
		ASSERT_EQ(CLV_VideoDecoderDecode(d, ipay.data(), ipay.size(), 1u, 0, 0, nullptr), CLV_VIDEO_OK);
		CLV_VideoFrameInfoC info {};
		EXPECT_EQ(CLV_VideoDecoderDecode(d, ppay.data(), ppay.size(), 1u, 1, 1, &info), CLV_VIDEO_OK);
		EXPECT_EQ(info.type, CLV_VIDEO_FRAME_P);
		Drain got = DrainAll(d);
		ASSERT_EQ(got.frames.size(), 2u) << "参考帧与 skip 帧都该出";
		const std::vector<uint8_t> a = ValidArea(got.frames[0], 16u, 16u);
		const std::vector<uint8_t> b = ValidArea(got.frames[1], 16u, 16u);
		ASSERT_EQ(a.size(), b.size());
		EXPECT_EQ(EqualBytes(a, b), a.size()) << "零矢量的 skip 帧应逐字节等于其参考帧";
		FreeAll(&got);
		CLV_VideoDecoderClose(d);
	}
	std::cout << "块模式位图：全零 / 两块在场 / 六块在场 / 位一而系数全零 / 位图越 6 bit / 帧内 skip "
				 "六档各按现行判据落位\n";
}

TEST(IndependentVideo, CodedSizeMacroblockLadder)
{
	/* 算式：coded = (原始 + 15) & ~15，按 32 位无符号推；宏块数 = (coded_w / kMbSize) × (coded_h / kMbSize)。
	 * 行距 = 原始宽（色度取半）向上取整到 32 的倍数，每平面同式。
	 * 表里的 coded 与宏块数都是照式子手算的数。 */
	struct SizeStep
	{
		uint16_t dw;
		uint16_t dh;
		uint32_t cw;
		uint32_t ch;
		uint32_t mbs;
		bool accept;
		const char* how;
	};

	const SizeStep sizes[] = {
		{ 16u,	  16u, 16u,	   16u, 1u,	   true,  "最小：一个宏块"								 },
		{ 1u,	  1u,  16u,	   16u, 1u,	   true,  "1 → 补到 16"									 },
		{ 17u,	  1u,  32u,	   16u, 2u,	   true,  "17 → 32：两列宏块"							 },
		{ 65520u, 16u, 65520u, 16u, 4095u, true,  "原始已是 16 的倍数 → 不补；65520 / 16 = 4095" },
		{ 65521u, 16u, 65536u, 16u, 4096u, true,  "17 位那一格：65536，用 16 位容器会回绕成 0"	 },
		{ 65535u, 16u, 65536u, 16u, 4096u, true,  "原始字段顶格 → 补后仍是 65536"				 },
		{ 0u,	  16u, 0u,	   0u,	0u,	   false, "原始尺寸 0 = 未声明，不合法"					 },
		{ 16u,	  0u,  0u,	   0u,	0u,	   false, "另一维同理"									 },
	};
	for (const SizeStep& s: sizes)
	{
		SCOPED_TRACE(s.how);
		// 出参先填哨兵：拒档必须一个都不写，不留半套尺寸
		uint32_t cw = 0xDEAD'BEEFu;
		uint32_t ch = 0xCAFE'F00Du;
		const CLV_VideoError r = CLV_VideoCodedSize(s.dw, s.dh, &cw, &ch);
		if (s.accept)
		{
			EXPECT_EQ(r, CLV_VIDEO_OK);
			EXPECT_EQ(cw, s.cw);
			EXPECT_EQ(ch, s.ch);
			EXPECT_EQ((s.cw / clv::video::kMbSize) * (s.ch / clv::video::kMbSize), s.mbs);
			// 句柄侧同一算式：建解码器不分配帧缓冲，只核尺寸推导
			CLV_VideoError err = CLV_VIDEO_OK;
			CLV_VideoDecoder* d = OpenDecoder(&err, s.dw, s.dh);
			EXPECT_NE(d, nullptr);
			EXPECT_EQ(err, CLV_VIDEO_OK);
			CLV_VideoDecoderClose(d);
			CLV_VideoEncoder* e = OpenEncoder(&err, s.dw, s.dh, kCarrierQuant);
			EXPECT_NE(e, nullptr);
			CLV_VideoEncoderClose(e);
		}
		else
		{
			EXPECT_EQ(r, CLV_VIDEO_VALUE_RANGE);
			EXPECT_EQ(cw, 0xDEAD'BEEFu) << "拒时不写出参";
			EXPECT_EQ(ch, 0xCAFE'F00Du);
			CLV_VideoError err = CLV_VIDEO_OK;
			EXPECT_EQ(OpenDecoder(&err, s.dw, s.dh), nullptr);
			EXPECT_EQ(err, CLV_VIDEO_VALUE_RANGE);
		}
	}

	// 出参指针为空是入参不成立，与值域档分开
	uint32_t cw = 0u;
	EXPECT_EQ(CLV_VideoCodedSize(16u, 16u, nullptr, &cw), CLV_VIDEO_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_VideoCodedSize(16u, 16u, &cw, nullptr), CLV_VIDEO_INVALID_ARGUMENT);

	/* 行距档：每平面按 32 字节对齐，色度平面的行距按半宽推。表里的行距是照算式手算的数。 */
	const struct
	{
		uint16_t dw;
		uint16_t dh;
		uint32_t cw;
		uint32_t ch;
		uint32_t stride0;
		uint32_t stride1;
		const char* how;
	} aligns[] = {
		{ 16u,	16u,  16u,	16u,  32u,	32u, "Align32(16) = 32；色度半宽 8 → 32"		  },
		{ 176u, 144u, 176u, 144u, 192u, 96u, "176 = 5×32 + 16 → 192；88 = 2×32 + 24 → 96" },
		{ 32u,	32u,  32u,	32u,  32u,	32u, "整格：不外加字节；半宽 16 → 32"			  },
		{ 64u,	16u,  64u,	16u,  64u,	32u, "64 已是 32 的倍数；半宽 32 → 32"			  },
		{ 20u,	20u,  32u,	32u,  32u,	32u, "20 → 补后 32：行距取补后宽"				  },
	};

	for (const auto& s: aligns)
	{
		SCOPED_TRACE(s.how);
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoEncoder* e = OpenEncoder(&err, s.dw, s.dh, kCarrierQuant);
		ASSERT_NE(e, nullptr);
		CLV_VideoFrame* f = nullptr;
		ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
		CLV_VideoFrameDescC desc {};
		const CLV_VideoError dr = CLV_VideoFrameGetDesc(f, &desc, nullptr);
		/* 这一档比的全是描述符里的整数字段（三平面行距与两侧尺寸），它们在 GetDesc 那一刻已经是副本，
		 * 平面字节一个都不读 ⇒ 取完立刻归还，不把这块持有带过下面那几行断言。
		 * 成功归还返 OK；「空指针 / 已作废 / 重复归还」三档都不该在这里命中。 */
		EXPECT_EQ(CLV_VideoFrameFree(f), CLV_VIDEO_OK);
		f = nullptr;	// 归还之后该帧指针失效
		ASSERT_EQ(dr, CLV_VIDEO_OK);
		EXPECT_EQ(desc.stride[0], s.stride0);
		EXPECT_EQ(desc.stride[1], s.stride1);
		EXPECT_EQ(desc.stride[2], s.stride1);
		EXPECT_EQ(desc.coded_width, s.cw);
		EXPECT_EQ(desc.coded_height, s.ch);
		EXPECT_EQ(desc.display_width, static_cast<uint32_t>(s.dw));
		CLV_VideoEncoderClose(e);
	}

	/* 原始尺寸不是 16 的倍数时，补边那一圈照常按宏块编码：20×20 → 补后 32×32 → 宏块 4 个。
	 * 帧信息里的宏块数与解码侧的宏块网格都得落在这条推导上。 */
	{
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoEncoder* e = OpenEncoder(&err, 20u, 20u, kCarrierQuant);
		ASSERT_NE(e, nullptr);
		CLV_VideoFrame* f = nullptr;
		ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
		WriteSeeded(f, 20u, 20u, 0x5Au);
		std::vector<uint8_t> payload;
		ASSERT_TRUE(EncodeToBytes(&payload, e, f, CLV_VIDEO_FRAME_I, kCarrierQuant, 0));
		/* 载荷已整份拷出，这块入帧缓冲到此用完 ⇒ 立刻归还，不把它带过下面解码与出帧那几段断言。
		 * 成功归还返 OK；「空指针 / 已作废 / 重复归还」三档在这里都不该命中。 */
		EXPECT_EQ(CLV_VideoFrameFree(f), CLV_VIDEO_OK);
		f = nullptr;	// 归还之后该帧指针失效
		CLV_VideoDecoder* d = OpenDecoder(&err, 20u, 20u);
		ASSERT_NE(d, nullptr);
		CLV_VideoFrameInfoC info {};
		ASSERT_EQ(CLV_VideoDecoderDecode(d, payload.data(), payload.size(), 1u, 0, 0, &info), CLV_VIDEO_OK);
		EXPECT_EQ(info.macroblock_count, 4u) << "(32/16) × (32/16)";
		Drain got = DrainAll(d);
		ASSERT_EQ(got.frames.size(), 1u);
		CLV_VideoFrameDescC desc {};
		ASSERT_EQ(CLV_VideoFrameGetDesc(got.frames[0], &desc, nullptr), CLV_VIDEO_OK);
		EXPECT_EQ(desc.coded_width, 32u);
		EXPECT_EQ(desc.coded_height, 32u);
		EXPECT_EQ(desc.display_width, 20u);
		EXPECT_EQ(desc.display_height, 20u);
		FreeAll(&got);
		CLV_VideoDecoderClose(d);

		// 宏块数与补后尺寸对不上：同一载荷喂给 16×16 的网格（宏块 1 个）判值域不成立
		CLV_VideoDecoder* small = OpenDecoder(&err, 16u, 16u);
		ASSERT_NE(small, nullptr);
		EXPECT_EQ(CLV_VideoDecoderDecode(small, payload.data(), payload.size(), 1u, 0, 0, nullptr),
				  CLV_VIDEO_VALUE_RANGE);
		Drain none = DrainAll(small);
		EXPECT_EQ(none.frames.size(), 0u);
		FreeAll(&none);
		CLV_VideoDecoderClose(small);
		// 入帧缓冲的归还排在编码那一段之后、解码那一段之前，这里句柄之外没有未还的帧
		CLV_VideoEncoderClose(e);
	}

	/* 越界可表达性：原始尺寸的声明字段是 16 位，顶格 65 535，补后单边最大 65 536（17 位）——
	 * 两维各自都落得进 32 位出参，故「单边越 32 位可表达域」在本层写不出来；
	 * 第一个越出 32 位可表达域的量是补后尺寸积 65 536 × 65 536 = 2^32，那一档归像素上界那一条。 */
	constexpr uint64_t kMaxSide = (static_cast<uint64_t>(UINT16_MAX) + 15u) & ~15ull;
	static_assert(kMaxSide == 65536ull, "补后单边可表达顶格");
	static_assert(kMaxSide < (1ull << 32), "单边落进 32 位容器");
	static_assert(kMaxSide * kMaxSide == (1ull << 32), "积越出 32 位可表达域");
	std::cout << "补后尺寸：ceil16 与宏块数推导 8 档、行距 4 档、补边编码 1 档，"
				 "单边越 32 位不可表达（顶格 65 536），越界档落在尺寸积上\n";
}

/* 补后尺寸积的像素上界：界值只在 reconstruct.h 的常量定义处出现一次，本件一律引符号。 */
namespace
{

	/* 编译期常量钉：界能当模板非类型实参用，即为编译期可求值 —— 运行时算出来的量做不到这一点。 */
	template<uint64_t N>
	struct BoundIsCompileTime
	{
		static constexpr uint64_t value = N;
	};

	using PinnedBound = BoundIsCompileTime<clv::video::kMaxReconstructPixels>;

	static_assert(PinnedBound::value == clv::video::kMaxReconstructPixels, "界必须是编译期常量");

	constexpr uint64_t kBound = PinnedBound::value;
	constexpr uint64_t kMaxCodedSide = (static_cast<uint64_t>(UINT16_MAX) + 15u) & ~15ull;
	constexpr uint64_t kRowPixels = static_cast<uint64_t>(clv::video::kMbSize) * kMaxCodedSide;

	/* 界上档的行数 = ⌊界 / 一行像素数⌋（向下取整保证积 ≤ 界）；越界档再加一行宏块高的行数。
	 * 积的可达步进是一整行 = kMaxCodedSide 个像素，故「界 + 1」本身摆不出来，
	 * 可达的最省越界档是界 + 超出量（≤ 一行像素数），静态断言把这两条推式钉住。 */
	constexpr uint64_t kRowsAtBound = kBound / kRowPixels;
	constexpr uint32_t kHeightInside = static_cast<uint32_t>((kRowsAtBound - 1ull) * clv::video::kMbSize);
	constexpr uint32_t kHeightAtBound = static_cast<uint32_t>(kRowsAtBound * clv::video::kMbSize);
	constexpr uint32_t kHeightOver = static_cast<uint32_t>((kRowsAtBound + 1ull) * clv::video::kMbSize);

	static_assert(kRowsAtBound >= 2ull, "界内至少容得下两行宏块，否则本梯的界内档摆不出来");
	static_assert(static_cast<uint64_t>(kHeightInside) * kMaxCodedSide < kBound, "界内档：积严格小于界");
	static_assert(static_cast<uint64_t>(kHeightAtBound) * kMaxCodedSide <= kBound, "界上档：积不大于界");
	static_assert(static_cast<uint64_t>(kHeightOver) * kMaxCodedSide > kBound, "越界档：积越界");
	static_assert(kHeightOver <= static_cast<uint32_t>(UINT16_MAX), "越界档的尺寸还得是可声明的原始尺寸");

	/* 界上档与越界档的结论直接由实现层那判式在编译期求值取来，不靠用例自己猜方向：
	 * CodedPixelsWithinBound 写的是 `积 <= kMaxReconstructPixels`，故积等于界那一档成立（收），
	 * 积大于界那一档不成立（拒）。运行时入口 CLV_VideoCodedSize 引用的就是这一个函数。 */
	static_assert(clv::video::CodedPixelsWithinBound(static_cast<uint32_t>(kMaxCodedSide), kHeightAtBound),
				  "界上档：实现层判式在受理侧");
	static_assert(! clv::video::CodedPixelsWithinBound(static_cast<uint32_t>(kMaxCodedSide), kHeightOver),
				  "越界档：实现层判式在拒侧");

	/* 字段可表达的最大宽 × 最大高必须比界大：界取的是「真能分配出来」的容量而不是字段顶格，
	 * 若这条不成立，界本身就与可表达域矛盾。 */
	static_assert(kMaxCodedSide * kMaxCodedSide > kBound, "可表达的最大尺寸积必须越界");

}	 // namespace

TEST(IndependentVideo, PixelProductBoundLadder)
{
	/* 三档：界内（积严格小于界）/ 界上（积正好等于界，或界内可达的最大值）/ 越界（界上档再加一行）。
	 * 句柄建出来即「收」——界判排在任何分配之前，建句柄不占帧缓冲。
	 * 界上档也断「收」：实现层的判式是 `积 <= kMaxReconstructPixels`（src/video/reconstruct.h 的
	 * CodedPixelsWithinBound，比较方向由 CLV_VideoCodedSize 入口那一支引用），积正好等于界即在受理侧。 */
	const struct
	{
		uint16_t dh;
		const char* how;
	} rows[] = {
		{ static_cast<uint16_t>(kHeightInside),	 "界内档：最宽列 × 行数比界上档少一行" },
		{ static_cast<uint16_t>(kHeightAtBound), "界上档：最宽列 × 界内可达的最大行数" },
		{ static_cast<uint16_t>(kHeightOver),	 "越界档：界上档再加一行宏块高"		   },
	};

	uint64_t products[3] = { 0ull, 0ull, 0ull };
	size_t row_index = 0u;
	for (const auto& s: rows)
	{
		SCOPED_TRACE(s.how);
		const uint64_t product = kMaxCodedSide * static_cast<uint64_t>(s.dh);
		products[row_index++] = product;
		const bool strictly_inside = product < kBound;
		const bool over = product > kBound;
		uint32_t cw = 0xDEAD'BEEFu;
		uint32_t ch = 0xCAFE'F00Du;
		const CLV_VideoError r = CLV_VideoCodedSize(UINT16_MAX, s.dh, &cw, &ch);
		if (over)
		{
			RecordProperty("OverBranch", "refused");
			EXPECT_EQ(r, CLV_VIDEO_VALUE_RANGE);
			EXPECT_EQ(cw, 0xDEAD'BEEFu) << "拒时不写出参，不留半套尺寸";
			EXPECT_EQ(ch, 0xCAFE'F00Du);
			// 交不出句柄 = 一帧都产不出来：出帧的每一条入口都只认句柄，NULL 句柄一律 INVALID_HANDLE
			CLV_VideoError err = CLV_VIDEO_OK;
			EXPECT_EQ(OpenDecoder(&err, UINT16_MAX, s.dh), nullptr);
			EXPECT_EQ(err, CLV_VIDEO_VALUE_RANGE);
			EXPECT_EQ(OpenEncoder(&err, UINT16_MAX, s.dh, kCarrierQuant), nullptr);
			EXPECT_EQ(err, CLV_VIDEO_VALUE_RANGE);
			CLV_VideoFrame* leaked = reinterpret_cast<CLV_VideoFrame*>(1u);
			uint8_t got = 1u;
			EXPECT_EQ(CLV_VideoDecoderNextOutput(nullptr, &leaked, &got), CLV_VIDEO_INVALID_HANDLE);
			EXPECT_EQ(leaked, nullptr);
			EXPECT_EQ(got, 0u);
			const uint8_t one[1] = { 0u };
			EXPECT_EQ(CLV_VideoDecoderDecode(nullptr, one, 1u, 1u, 0, 0, nullptr), CLV_VIDEO_INVALID_HANDLE);
			CLV_VideoBitstream* bs = reinterpret_cast<CLV_VideoBitstream*>(1u);
			EXPECT_EQ(CLV_VideoEncoderEncode(nullptr, nullptr, CLV_VIDEO_FRAME_I, kCarrierQuant, 0, 0, &bs),
					  CLV_VIDEO_INVALID_HANDLE);
			EXPECT_EQ(bs, nullptr);
			continue;
		}
		// 界内与界上两档的唯一结论都是「收」：比较方向由上面的编译期钉给出，这里只断那一条。
		if (strictly_inside) RecordProperty("InsideBranch", "accepted");
		else RecordProperty("AtBoundBranch", "accepted");
		ASSERT_EQ(r, CLV_VIDEO_OK) << (strictly_inside ? "界内档必须收" : "界上档：积正好等于界，在受理侧");
		// 收档的出参就是 ceil16 推出来的原值，不被往界内钳
		EXPECT_EQ(cw, static_cast<uint32_t>(kMaxCodedSide));
		EXPECT_EQ(ch, static_cast<uint32_t>(s.dh));
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoDecoder* d = OpenDecoder(&err, UINT16_MAX, s.dh);
		if (d != nullptr)
		{
			EXPECT_EQ(err, CLV_VIDEO_OK);
			CLV_VideoDecoderClose(d);
		}
		else
		{
			EXPECT_EQ(err, CLV_VIDEO_VALUE_RANGE);
		}
		CLV_VideoEncoder* e = OpenEncoder(&err, UINT16_MAX, s.dh, kCarrierQuant);
		if (e != nullptr) CLV_VideoEncoderClose(e);
		else EXPECT_EQ(err, CLV_VIDEO_VALUE_RANGE);
	}
	/* 三档之间的间距是一整行 = `kRowPixels`（最宽列 × 一个宏块高 = 65 536 × 16）。
	 * 界正好被一行整除（2^28 ÷ 2^20 = 256 行），故可达的最小越界档 = 界 + 一整行，
	 * 超出量 = `kRowPixels` 本身；「界 + 1」在 16 的行高步进下摆不出来。 */
	RecordProperty("InsidePixels", products[0]);
	RecordProperty("AtBoundPixels", products[1]);
	RecordProperty("OverPixels", products[2]);
	EXPECT_EQ(products[0] + kRowPixels, products[1]);
	EXPECT_EQ(products[1] + kRowPixels, products[2]);
	EXPECT_GT(products[2], kBound);
	const uint64_t over_by = products[2] - kBound;
	EXPECT_GT(over_by, 0ull);
	EXPECT_LE(over_by, kRowPixels);

	/* 不回绕：积按 32 位算在 65 536 × 65 536 处正好回绕成 0，按无符号 64 位算是 2^32。
	 * 这一档若按 32 位积比界就会被判成「界内」而交出句柄；句柄交不出来即为按 64 位算的证据。 */
	{
		const uint32_t wrapped = static_cast<uint32_t>(kMaxCodedSide) * static_cast<uint32_t>(kMaxCodedSide);
		EXPECT_EQ(wrapped, 0u) << "32 位积在这里正好回绕成 0";
		EXPECT_NE(kMaxCodedSide * kMaxCodedSide, static_cast<uint64_t>(wrapped));
		uint32_t cw = 0xDEAD'BEEFu;
		uint32_t ch = 0xCAFE'F00Du;
		EXPECT_EQ(CLV_VideoCodedSize(UINT16_MAX, UINT16_MAX, &cw, &ch), CLV_VIDEO_VALUE_RANGE);
		EXPECT_EQ(cw, 0xDEAD'BEEFu);
		CLV_VideoError err = CLV_VIDEO_OK;
		EXPECT_EQ(OpenDecoder(&err, UINT16_MAX, UINT16_MAX), nullptr);
		EXPECT_EQ(err, CLV_VIDEO_VALUE_RANGE);
		EXPECT_EQ(OpenEncoder(&err, UINT16_MAX, UINT16_MAX, kCarrierQuant), nullptr);
		EXPECT_EQ(err, CLV_VIDEO_VALUE_RANGE);
	}
	std::cout << "尺寸积上界：界 = 编译期常量，界内 / 界上 / 越界三档 + 不回绕档，越界一律交不出句柄\n";
}
