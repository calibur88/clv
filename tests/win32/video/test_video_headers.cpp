/* test_video_headers.cpp
 *
 * **两面混用**：端到端与句柄行为走公开 C ABI（`CLV_Video*` 族 17 个入口、实测 43 处调用，如 `CLV_VideoBitstreamGetBytes`、`CLV_VideoBitstreamRelease`、`CLV_VideoBitstreamSliceCount`），
 *   单元级直测走实现层头（`video/headers.h`、`video/reconstruct.h`、`video/status.h`）。**仅静态构建（`CLV_BUILD_SHARED=OFF`）下可链**的原因是后面这些头，
 *   不是「不经公开 ABI」——两面各写各的：主体过公开 C ABI，另有几处直调实现层函数钉住不进 ABI 的行为。
 *
 * VOP 帧头、分片片头、`ext_type = 3` 元数据块与补后尺寸推导的用例。
 * 字节布局就是已定的 VOP 帧头 / 分片片头 / 编码器元数据块三处条款的字段表；
 * 这里只断言「已定布局 + 合法域拒绝」，不含任何自创字段。
 */

#include "CLV_Video.h"
#include "video/headers.h"
#include "video/reconstruct.h"
#include "video/status.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace
{

	using clv::video::CoefCoding;
	using clv::video::EncMeta;
	using clv::video::kCoefCodingMax;
	using clv::video::SliceHeader;
	using clv::video::VopHeader;

	VopHeader MakeVop(uint8_t type = 0u, uint8_t q = 8u, uint64_t mb = 99u)
	{
		VopHeader h;
		h.type = type;
		h.quant = q;
		h.fcode_forward = 1u;
		h.fcode_backward = 1u;
		h.intra_dc_vlc_thr = 3u;
		h.dquant = 0;
		h.mb_count = mb;
		return h;
	}

	bool EncVopFails(const VopHeader& h, uint8_t qp = 7u)
	{
		std::vector<uint8_t> scratch;
		return ! clv::video::EncodeVopHeader(h, qp, &scratch);
	}

	bool EncSliceFails(const SliceHeader& s)
	{
		std::vector<uint8_t> scratch;
		return ! clv::video::EncodeSliceHeader(s, 7u, &scratch);
	}

	std::vector<uint8_t> EncodeOk(const VopHeader& h, uint8_t qp = 7u)
	{
		std::vector<uint8_t> out;
		EXPECT_TRUE(clv::video::EncodeVopHeader(h, qp, &out));
		return out;
	}

	SliceHeader MakeSlice(bool first, bool last, uint16_t count = 3u)
	{
		SliceHeader s;
		s.frame_id = 0x01020304u;
		s.fragment_count = count;
		s.is_first = first;
		s.is_last = last;
		s.has_vop_quant = first;
		s.vop_type = 1u;
		s.vop_quant = first ? 24u : 0u;
		s.first_mb = first ? 0u : 12u;
		s.mb_run = 4u;
		return s;
	}

	std::vector<uint8_t> MetaBytes(uint8_t max_b = 2u, uint8_t qp = 7u, uint8_t tools = 0u, uint8_t reserved = 0u,
								   uint16_t w = 176u, uint16_t h = 144u)
	{
		return { max_b,
				 qp,
				 tools,
				 reserved,
				 static_cast<uint8_t>(w & 0xFFu),
				 static_cast<uint8_t>(w >> 8),
				 static_cast<uint8_t>(h & 0xFFu),
				 static_cast<uint8_t>(h >> 8) };
	}

	/* 声明档不合 v1 → 句柄照交、置拒解态，Open 不返 NULL；*err 的致命码由调用处逐例断。
	 * 拒解态下除 Reset / Close / StreamRejected 外的每个入口都返 STREAM_REJECTED，且该判定先于入参检查。
	 * 返 NULL 的是入参档（越界的量化精度 / 尺寸）与内存不足档，与这里两档不同。 */
	void DecoderOpensRejected(const CLV_VideoStreamParamsC* params, CLV_VideoError* err)
	{
		CLV_VideoDecoder* d = CLV_VideoDecoderOpen(params, err);
		ASSERT_NE(d, nullptr);
		uint8_t rejected = 0u;
		EXPECT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK);
		EXPECT_EQ(rejected, 1u);
		CLV_VideoFrame* frame = nullptr;
		uint8_t got = 1u;
		EXPECT_EQ(CLV_VideoDecoderNextOutput(d, &frame, &got), CLV_VIDEO_STREAM_REJECTED);
		EXPECT_EQ(frame, nullptr);
		EXPECT_EQ(got, 0u);
		EXPECT_EQ(CLV_VideoDecoderDecode(d, nullptr, 0u, 1u, 0, 0, nullptr), CLV_VIDEO_STREAM_REJECTED);
		CLV_VideoDecoderClose(d);
	}

}	 // namespace

TEST(VideoHeaders, VopHeaderRoundTripsEveryType)
{
	static const uint8_t kTypes[] = { 0u, 1u, 2u };
	for (const uint8_t type: kTypes)
	{
		const VopHeader src = MakeVop(type, 60u, 7920u);
		const std::vector<uint8_t> bytes = EncodeOk(src);
		ASSERT_GT(bytes.size(), 6u);
		VopHeader got;
		size_t consumed = 0u;
		ASSERT_TRUE(clv::video::DecodeVopHeader(bytes.data(), bytes.size(), 7u, &got, &consumed)) << "type " << type;
		EXPECT_EQ(got.type, src.type);
		EXPECT_EQ(got.quant, src.quant);
		EXPECT_EQ(got.fcode_forward, src.fcode_forward);
		EXPECT_EQ(got.fcode_backward, src.fcode_backward);
		EXPECT_EQ(got.intra_dc_vlc_thr, src.intra_dc_vlc_thr);
		EXPECT_EQ(got.dquant, src.dquant);
		EXPECT_EQ(got.mb_count, src.mb_count);
		// 6 个定长字段 + mb_count 的 varint（7920 要 2 字节）
		EXPECT_EQ(consumed, 8u);
	}
}

TEST(VideoHeaders, VopHeaderFixedLayout)
{
	VopHeader src = MakeVop(2u, 127u, 200u);
	src.dquant = 5;
	src.intra_dc_vlc_thr = 7u;
	const std::vector<uint8_t> b = EncodeOk(src);
	ASSERT_GE(b.size(), 7u);
	EXPECT_EQ(b[0], 2u);	  // vop_type
	EXPECT_EQ(b[1], 127u);	  // vop_quant
	EXPECT_EQ(b[2], 1u);	  // vop_fcode_forward
	EXPECT_EQ(b[3], 1u);	  // vop_fcode_backward
	EXPECT_EQ(b[4], 7u);	  // vop_flags：bit0-2 intra_dc_vlc_thr、bit3-4 恒 0、bit5-7 系数编码法（未赋法 = 0）
	EXPECT_EQ(b[5], 5u);	  // vop_dquant：bit0-4，bit5-7 恒 0
	EXPECT_EQ(b[6], 200u);	  // mb_count varint 首字节
}

TEST(VideoHeaders, VopHeaderRejectsIllegalFields)
{
	VopHeader got;
	size_t consumed = 0u;

	const uint8_t qp = 7u;
	// 帧类型只有 I / P / B
	VopHeader h = MakeVop();
	h.type = 3u;
	EXPECT_TRUE(EncVopFails(h, qp));
	h = MakeVop();
	h.type = 2u;
	EXPECT_FALSE(EncVopFails(h, qp));

	/* 量化步长的域分三段：0 = 无损（合法）、1..2^qp−1 = 有损（合法）、上界加一起域外。
	 * 无损流在码流里的信号就是帧头那个 0，读方不必再要一个模式位。 */
	h = MakeVop(0u, 0u);
	EXPECT_FALSE(EncVopFails(h, qp));
	h = MakeVop(0u, 1u);
	EXPECT_FALSE(EncVopFails(h, qp));
	h = MakeVop(0u, 127u);
	EXPECT_FALSE(EncVopFails(h, qp));
	h = MakeVop(0u, 128u);
	EXPECT_TRUE(EncVopFails(h, qp));

	// fcode 是规范原值 1..7，0 非法
	h = MakeVop();
	h.fcode_forward = 0u;
	EXPECT_TRUE(EncVopFails(h, qp));
	h = MakeVop();
	h.fcode_backward = 8u;
	EXPECT_TRUE(EncVopFails(h, qp));

	// 宏块数为 0 不成立
	h = MakeVop();
	h.mb_count = 0u;
	EXPECT_TRUE(EncVopFails(h, qp));

	std::vector<uint8_t> bytes = EncodeOk(MakeVop());
	// flags 的 bit3（顶场优先）/ bit4（隔行）在 v1 必须为 0
	static const uint8_t kFlagBits[] = { 3u, 4u };
	for (const uint8_t bit: kFlagBits)
	{
		std::vector<uint8_t> bad = bytes;
		bad[4] = static_cast<uint8_t>(bad[4] | (1u << bit));
		EXPECT_FALSE(clv::video::DecodeVopHeader(bad.data(), bad.size(), qp, &got, &consumed)) << "bit " << bit;
	}
	/* bit5-7 = 系数编码法：只认已赋法的 0 与 1，2..7 一律判帧头不成立。
	 * 这条不是保守起见 —— 未赋法的值若被「当 0 处理」，一条新流会被按定宽凑着读完，
	 * 静默解出错值比崩更糟。字段位置在本文件按掩码现算，不写字面 32。*/
	for (uint8_t v = 2u; v <= 7u; ++v)
	{
		std::vector<uint8_t> bad = bytes;
		bad[4] = static_cast<uint8_t>((bad[4] & ~clv::video::kVopCoefCodingMask) |
									  static_cast<uint8_t>(v << clv::video::kVopCoefCodingShift));
		EXPECT_FALSE(clv::video::DecodeVopHeader(bad.data(), bad.size(), qp, &got, &consumed)) << "编码法 " << +v;
	}
	// dquant 的 bit5-7 保留位
	static const uint8_t kDqBits[] = { 5u, 6u, 7u };
	for (const uint8_t bit: kDqBits)
	{
		std::vector<uint8_t> bad = bytes;
		bad[5] = static_cast<uint8_t>(bad[5] | (1u << bit));
		EXPECT_FALSE(clv::video::DecodeVopHeader(bad.data(), bad.size(), qp, &got, &consumed)) << "bit " << bit;
	}
	// 量化步长越上界（线上改了字节）：128 越界，0 不越界——0 是无损流的合法声明
	std::vector<uint8_t> bad_q = bytes;
	bad_q[1] = 128u;
	EXPECT_FALSE(clv::video::DecodeVopHeader(bad_q.data(), bad_q.size(), qp, &got, &consumed));
	std::vector<uint8_t> lossless_q = bytes;
	lossless_q[1] = 0u;
	ASSERT_TRUE(clv::video::DecodeVopHeader(lossless_q.data(), lossless_q.size(), qp, &got, &consumed));
	EXPECT_EQ(got.quant, 0u);	 // 读方从流里拿到的模式信号就是 0
	// 截断、空指针、非最短 varint
	EXPECT_FALSE(clv::video::DecodeVopHeader(bytes.data(), 6u, qp, &got, &consumed));
	EXPECT_FALSE(clv::video::DecodeVopHeader(nullptr, bytes.size(), qp, &got, &consumed));
	EXPECT_FALSE(clv::video::DecodeVopHeader(bytes.data(), bytes.size(), qp, nullptr, &consumed));
	std::vector<uint8_t> overlong = { 0u, 8u, 1u, 1u, 3u, 0u, 0x80u, 0x00u };
	EXPECT_FALSE(clv::video::DecodeVopHeader(overlong.data(), overlong.size(), qp, &got, &consumed));
}

/* 系数编码法是逐帧携带的码流事实：写侧把它放进 vop_flags bit5-7，读侧原样取回。
 * 定宽那一法必须编出与「还没有这一字段」时逐字节相同的帧头 —— 这条是判据，不是巧合：
 * 加的是「逐帧选一档」的法，帧头格式没变，既有码流与既有对平靶照样读得回来。 */
TEST(VideoHeaders, VopHeaderCarriesCoefCoding)
{
	VopHeader got;
	size_t consumed = 0u;
	static const CoefCoding kAll[] = { CoefCoding::kFixed, CoefCoding::kEobEg };
	for (const CoefCoding c: kAll)
	{
		VopHeader h = MakeVop(1u, 8u, 99u);
		h.coef_coding = c;
		const std::vector<uint8_t> b = EncodeOk(h);
		ASSERT_GE(b.size(), 7u);
		/* vop_flags 的字节 = intra_dc_vlc_thr(MakeVop 填 3) | 本法取值 << 5，bit3-4 恒 0。 */
		const uint8_t want = static_cast<uint8_t>(3u | (static_cast<uint8_t>(c) << clv::video::kVopCoefCodingShift));
		EXPECT_EQ(b[4], want) << "法 " << +static_cast<uint8_t>(c);
		EXPECT_EQ(b[4] & clv::video::kVopFlagsReservedMask, 0u) << "bit3-4 仍须为 0";
		ASSERT_TRUE(clv::video::DecodeVopHeader(b.data(), b.size(), 7u, &got, &consumed));
		EXPECT_EQ(got.coef_coding, c) << "读侧没把法取回来";
		EXPECT_EQ(got.intra_dc_vlc_thr, 3u) << "同一字节里的另 3 bit 不该被法字段串位";
	}
	/* 定宽那一法的帧头字节与「字段全 0」同值：这一条把「旧流照样读得回来」钉住。 */
	VopHeader legacy = MakeVop(1u, 8u, 99u);
	EXPECT_EQ(EncodeOk(legacy)[4], 3u);
	VopHeader explicit_fixed = MakeVop(1u, 8u, 99u);
	explicit_fixed.coef_coding = CoefCoding::kFixed;
	EXPECT_EQ(EncodeOk(legacy), EncodeOk(explicit_fixed));
}

/* 写侧与读侧同一口径：未赋法的取值既读不进也不许写出，避免「库里编出一条自己解不回的流」。 */
TEST(VideoHeaders, EncodeVopHeaderRefusesUnassignedCoding)
{
	constexpr uint8_t kFieldTop = static_cast<uint8_t>(
		clv::video::kVopCoefCodingMask >> clv::video::kVopCoefCodingShift);	   // 3 bit 字段能表达的顶格 = 7
	for (uint8_t v = static_cast<uint8_t>(kCoefCodingMax + 1u); v <= kFieldTop; ++v)
	{
		VopHeader h = MakeVop();
		h.coef_coding = static_cast<CoefCoding>(v);
		EXPECT_TRUE(EncVopFails(h, 7u)) << "编码法 " << +v << " 未赋法，写侧不该受理";
	}
	VopHeader ok = MakeVop();
	ok.coef_coding = CoefCoding::kEobEg;
	EXPECT_FALSE(EncVopFails(ok, 7u));
}

TEST(VideoHeaders, DquantIsFiveBitSigned)
{
	VopHeader got;
	size_t consumed = 0u;
	std::vector<uint8_t> bytes = EncodeOk(MakeVop());

	// 0x10 = -16（下界），0x0F = +15（上界），0x1F = -1
	struct Probe
	{
		uint8_t byte;
		int want;
	};

	static const Probe kDqProbes[] = {
		{ 0x10u, -16 },
		   { 0x0Fu, 15	},
		 { 0x1Fu, -1  },
		   { 0x00u, 0	}
	};
	for (const Probe& probe: kDqProbes)
	{
		std::vector<uint8_t> one = bytes;
		one[5] = probe.byte;
		ASSERT_TRUE(clv::video::DecodeVopHeader(one.data(), one.size(), 7u, &got, &consumed));
		EXPECT_EQ(got.dquant, probe.want) << "字节 0x" << std::hex << static_cast<int>(probe.byte);
	}
	// 编码侧也守域：+16 与 -17 都拒绝
	VopHeader hi = MakeVop();
	hi.dquant = 16;
	EXPECT_TRUE(EncVopFails(hi));
	VopHeader lo = MakeVop();
	lo.dquant = -17;
	EXPECT_TRUE(EncVopFails(lo));
}

TEST(VideoHeaders, EffectiveQuantAddsBothLevelsAndClamps)
{
	// 两级叠加：q + d_vop + d_mb
	EXPECT_EQ(clv::video::EffectiveQuant(5u, 3, -2, 7u), 6u);
	EXPECT_EQ(clv::video::EffectiveQuant(10u, 0, 0, 7u), 10u);
	EXPECT_EQ(clv::video::EffectiveQuant(2u, 3, 1, 7u), 6u);
	// 下边界：1
	EXPECT_EQ(clv::video::EffectiveQuant(1u, -16, -2, 7u), 1u);
	EXPECT_EQ(clv::video::EffectiveQuant(5u, -16, 0, 7u), 1u);
	// 上边界 127 = 2^7 - 1
	EXPECT_EQ(clv::video::EffectiveQuant(127u, 15, 1, 7u), 127u);
	EXPECT_EQ(clv::video::EffectiveQuant(120u, 15, 1, 7u), 127u);
	// 上界跟着 quant_precision 走：5 位 → 31
	EXPECT_EQ(clv::video::EffectiveQuant(30u, 5, 0, 5u), 31u);
	EXPECT_EQ(clv::video::EffectiveQuant(31u, 5, 1, 5u), 31u);
}

TEST(VideoHeaders, SliceHeaderRoundTripsAndPinsOffsets)
{
	SliceHeader got;
	size_t consumed = 0u;

	const SliceHeader first = MakeSlice(true, false);
	std::vector<uint8_t> fb;
	ASSERT_TRUE(clv::video::EncodeSliceHeader(first, 7u, &fb));
	// 定长 9（首片带 vop_quant）+ first_mb 与 mb_run 两个 1 字节 varint
	EXPECT_EQ(fb.size(), 11u);
	EXPECT_EQ(fb[0], 0x04u);	// frame_id 小端
	EXPECT_EQ(fb[1], 0x03u);
	EXPECT_EQ(fb[2], 0x02u);
	EXPECT_EQ(fb[3], 0x01u);
	EXPECT_EQ(fb[4], 3u);	 // fragment_count 小端
	EXPECT_EQ(fb[5], 0u);
	EXPECT_EQ(fb[6], 0x02u | 0x04u);	// is_first + has_vop_quant
	EXPECT_EQ(fb[7], 1u);				// vop_type
	EXPECT_EQ(fb[8], 24u);				// vop_quant
	EXPECT_EQ(fb[9], 0u);				// first_mb = 0
	EXPECT_EQ(fb[10], 4u);				// mb_run = 4
	ASSERT_TRUE(clv::video::DecodeSliceHeader(fb.data(), fb.size(), 7u, true, false, &got, &consumed));
	EXPECT_EQ(consumed, 11u);
	EXPECT_EQ(got.frame_id, first.frame_id);
	EXPECT_EQ(got.fragment_count, first.fragment_count);
	EXPECT_TRUE(got.is_first);
	EXPECT_FALSE(got.is_last);
	EXPECT_TRUE(got.has_vop_quant);
	EXPECT_EQ(got.vop_quant, 24u);
	EXPECT_EQ(got.first_mb, 0u);
	EXPECT_EQ(got.mb_run, 4u);

	const SliceHeader mid = MakeSlice(false, false);
	std::vector<uint8_t> mb2;
	ASSERT_TRUE(clv::video::EncodeSliceHeader(mid, 7u, &mb2));
	// 后续片定长 8，不含 vop_quant 字节；变量部分仍是两个 varint
	EXPECT_EQ(mb2.size(), 10u);
	EXPECT_EQ(mb2[6], 0u);
	ASSERT_TRUE(clv::video::DecodeSliceHeader(mb2.data(), mb2.size(), 7u, false, false, &got, &consumed));
	EXPECT_EQ(consumed, 10u);
	EXPECT_FALSE(got.has_vop_quant);
	EXPECT_EQ(got.first_mb, 12u);
	EXPECT_EQ(got.mb_run, 4u);

	const SliceHeader tail = MakeSlice(false, true);
	std::vector<uint8_t> tb;
	ASSERT_TRUE(clv::video::EncodeSliceHeader(tail, 7u, &tb));
	EXPECT_EQ(tb[6], 0x01u);
	ASSERT_TRUE(clv::video::DecodeSliceHeader(tb.data(), tb.size(), 7u, false, true, &got, &consumed));
	EXPECT_TRUE(got.is_last);
}

TEST(VideoHeaders, SliceHeaderRejectsContradictions)
{
	SliceHeader got;
	size_t consumed = 0u;

	// 单片帧不发片头：fragment_count = 1 出现在片头里即不成立
	SliceHeader s = MakeSlice(true, true, 1u);
	EXPECT_TRUE(EncSliceFails(s));
	std::vector<uint8_t> one = []
	{
		SliceHeader ok = MakeSlice(true, false);
		std::vector<uint8_t> b;
		EXPECT_TRUE(clv::video::EncodeSliceHeader(ok, 7u, &b));
		b[4] = 1u;
		b[5] = 0u;
		return b;
	}();
	EXPECT_FALSE(clv::video::DecodeSliceHeader(one.data(), one.size(), 7u, true, false, &got, &consumed));

	// has_vop_quant 只能出现在首片
	SliceHeader wrong = MakeSlice(false, false);
	wrong.has_vop_quant = true;
	wrong.vop_quant = 30u;
	EXPECT_TRUE(EncSliceFails(wrong));

	// 片头的位置位与容器外层权威不一致 → 以容器为准，片头按不成立
	std::vector<uint8_t> fb;
	const SliceHeader first = MakeSlice(true, false);
	ASSERT_TRUE(clv::video::EncodeSliceHeader(first, 7u, &fb));
	EXPECT_FALSE(clv::video::DecodeSliceHeader(fb.data(), fb.size(), 7u, false, false, &got, &consumed));
	std::vector<uint8_t> midb;
	ASSERT_TRUE(clv::video::EncodeSliceHeader(MakeSlice(false, false), 7u, &midb));
	EXPECT_FALSE(clv::video::DecodeSliceHeader(midb.data(), midb.size(), 7u, true, false, &got, &consumed));
	// 末片位不符
	std::vector<uint8_t> tb;
	ASSERT_TRUE(clv::video::EncodeSliceHeader(MakeSlice(false, true), 7u, &tb));
	EXPECT_FALSE(clv::video::DecodeSliceHeader(tb.data(), tb.size(), 7u, false, false, &got, &consumed));

	// reserved 位、帧类型、量化上界、截断
	static const uint8_t kSliceBits[] = { 3u, 5u, 7u };
	for (const uint8_t bit: kSliceBits)
	{
		std::vector<uint8_t> bad = fb;
		bad[6] = static_cast<uint8_t>(bad[6] | (1u << bit));
		EXPECT_FALSE(clv::video::DecodeSliceHeader(bad.data(), bad.size(), 7u, true, false, &got, &consumed))
			<< "bit " << bit;
	}
	std::vector<uint8_t> bad_type = fb;
	bad_type[7] = 3u;
	EXPECT_FALSE(clv::video::DecodeSliceHeader(bad_type.data(), bad_type.size(), 7u, true, false, &got, &consumed));
	std::vector<uint8_t> bad_q = fb;
	bad_q[8] = 128u;
	EXPECT_FALSE(clv::video::DecodeSliceHeader(bad_q.data(), bad_q.size(), 7u, true, false, &got, &consumed));
	// 片头携带 0 同样成立：无损流的模式信号在帧头与片头是同一个值
	std::vector<uint8_t> lq = fb;
	lq[8] = 0u;
	ASSERT_TRUE(clv::video::DecodeSliceHeader(lq.data(), lq.size(), 7u, true, false, &got, &consumed));
	EXPECT_EQ(got.vop_quant, 0u);
	EXPECT_FALSE(clv::video::DecodeSliceHeader(fb.data(), 8u, 7u, true, false, &got, &consumed));
	SliceHeader big = MakeSlice(true, false);
	big.vop_type = 2u;
	std::vector<uint8_t> bb;
	EXPECT_TRUE(clv::video::EncodeSliceHeader(big, 7u, &bb));
	// 判据只能落在码流字节与解回来的值上：断自己刚赋的 `big.vop_type` 恒真，等于什么都没钉。三处：
	// 首片带 vop_quant ⇒ 定长 9 字节 + first_mb 1 字节 + mb_run 1 字节 = 11；
	// vop_type 落在线上的第 8 字节（下标 7，与上面 fb[7] = 1 同位）；解回来还得是 2。
	ASSERT_EQ(bb.size(), 11u);
	EXPECT_EQ(bb[7], 2u);
	SliceHeader back;
	size_t used = 0u;
	ASSERT_TRUE(clv::video::DecodeSliceHeader(bb.data(), bb.size(), 7u, true, false, &back, &used));
	EXPECT_EQ(back.vop_type, 2u);
	EXPECT_EQ(used, 11u);

	// 该片宏块数 = 0 是空片：编码侧不写，解码侧不认
	SliceHeader empty_run = MakeSlice(true, false);
	empty_run.mb_run = 0u;
	EXPECT_TRUE(EncSliceFails(empty_run));
	std::vector<uint8_t> run_zero = fb;
	run_zero[run_zero.size() - 1u] = 0u;	// 末字节就是 mb_run 的 varint
	EXPECT_FALSE(clv::video::DecodeSliceHeader(run_zero.data(), run_zero.size(), 7u, true, false, &got, &consumed));
}

TEST(VideoHeaders, EncMetaBlockParsesEightBytePayload)
{
	EncMeta m;
	const std::vector<uint8_t> ok = MetaBytes();
	ASSERT_TRUE(clv::video::ParseEncMeta(ok.data(), ok.size(), &m));
	EXPECT_EQ(m.max_b_frames, 2u);
	EXPECT_EQ(m.quant_precision, 7u);
	EXPECT_EQ(m.ext_tools, 0u);
	EXPECT_EQ(m.display_width, 176u);
	EXPECT_EQ(m.display_height, 144u);

	// 尺寸字段是小端 u16
	const std::vector<uint8_t> w = MetaBytes(0u, 7u, 0u, 0u, 0x1234u, 0x0001u);
	ASSERT_TRUE(clv::video::ParseEncMeta(w.data(), w.size(), &m));
	EXPECT_EQ(m.display_width, 0x1234u);
	EXPECT_EQ(m.display_height, 1u);

	// 宽高必须声明：声明为 0 落值域不成立，本件里没有「0 = 未声明」这一档；1×1 是合法下界的对照
	const std::vector<uint8_t> zero = MetaBytes(0u, 7u, 0u, 0u, 0u, 0u);
	EXPECT_FALSE(clv::video::ParseEncMeta(zero.data(), zero.size(), &m)) << "0×0 仍被当成未声明收下";
	const std::vector<uint8_t> zero_w = MetaBytes(0u, 7u, 0u, 0u, 0u, 144u);
	EXPECT_FALSE(clv::video::ParseEncMeta(zero_w.data(), zero_w.size(), &m)) << "宽为 0";
	const std::vector<uint8_t> one = MetaBytes(0u, 7u, 0u, 0u, 1u, 1u);
	ASSERT_TRUE(clv::video::ParseEncMeta(one.data(), one.size(), &m));
	EXPECT_EQ(m.display_width, 1u);

	// 扩展工具存在位可以用，bit1-7 保留必须为 0
	ASSERT_TRUE(clv::video::ParseEncMeta(MetaBytes(0u, 7u, 1u).data(), 8u, &m));
	EXPECT_NE(m.ext_tools & 1u, 0u);
	EXPECT_FALSE(clv::video::ParseEncMeta(MetaBytes(0u, 7u, 2u).data(), 8u, &m));
	EXPECT_FALSE(clv::video::ParseEncMeta(MetaBytes(0u, 7u, 0x80u).data(), 8u, &m));
	// 第四个字节保留
	EXPECT_FALSE(clv::video::ParseEncMeta(MetaBytes(0u, 7u, 0u, 1u).data(), 8u, &m));
	// 量化精度域 [3, 9]
	EXPECT_FALSE(clv::video::ParseEncMeta(MetaBytes(0u, 2u).data(), 8u, &m));
	EXPECT_FALSE(clv::video::ParseEncMeta(MetaBytes(0u, 10u).data(), 8u, &m));
	EXPECT_TRUE(clv::video::ParseEncMeta(MetaBytes(0u, 9u).data(), 8u, &m));
	// 长度与空指针
	EXPECT_FALSE(clv::video::ParseEncMeta(ok.data(), 7u, &m));
	EXPECT_FALSE(clv::video::ParseEncMeta(ok.data(), 9u, &m));
	EXPECT_FALSE(clv::video::ParseEncMeta(nullptr, 8u, &m));
}

TEST(VideoHeaders, CodedSizeUsesWideUnsigned)
{
	struct Probe
	{
		uint16_t orig;
		uint32_t coded;
	};

	// 16 的倍数不动、非倍数向上取整；65521 起补后 65536 需要 17 位
	static const Probe kProbes[] = {
		{ 1u,	  16u	 },
		{ 15u,	  16u	 },
		 { 16u,	   16u	  },
		  { 17u,	32u	   },
		   { 175u,	 176u	},
		  { 176u,	176u   },
		{ 4095u,  4096u	 },
		 { 65520u, 65520u },
		{ 65521u, 65536u },
		   { 65534u, 65536u },
		  { 65535u, 65536u }
	};
	// 取整函数本身不算积：单边补后值逐条照表成立，65 536 也照样算得出来。
	for (const Probe& p: kProbes) EXPECT_EQ(clv::video::CodedSize(p.orig), p.coded) << "orig " << p.orig;

	/* 句柄级另有一道界：补后尺寸积的帧缓冲上界是编译期常量（`reconstruct.h` 的
	 * `kMaxReconstructPixels` = 2^28 = 16 384 × 16 384）。积越界的尺寸在任何帧缓冲分配入口都
	 * 分配不出来，句柄级入口就判值域不成立 —— 出参半套尺寸留到分配点才炸是更坏的结果。 */
	for (const Probe& p: kProbes)
	{
		const uint64_t pixels = static_cast<uint64_t>(p.coded) * p.coded;
		uint32_t w = 0u;
		uint32_t h = 0u;
		if (pixels > clv::video::kMaxReconstructPixels)
		{
			w = 0xA5A5A5A5u;	// 拒时不写出参：哨兵值断它没被动
			h = 0xA5A5A5A5u;
			EXPECT_EQ(CLV_VideoCodedSize(p.orig, p.orig, &w, &h), CLV_VIDEO_VALUE_RANGE) << "orig " << p.orig;
			EXPECT_EQ(w, 0xA5A5A5A5u) << "orig " << p.orig << " 的宽度出参被写了";
			EXPECT_EQ(h, 0xA5A5A5A5u) << "orig " << p.orig << " 的高度出参被写了";
			continue;
		}
		ASSERT_EQ(CLV_VideoCodedSize(p.orig, p.orig, &w, &h), CLV_VIDEO_OK) << "orig " << p.orig;
		EXPECT_EQ(w, p.coded);
		EXPECT_EQ(h, p.coded);
	}
	{
		uint32_t w = 0u;
		uint32_t h = 0u;
		// 界上档：16 384 × 16 384 的积正好等于界，必须收
		EXPECT_EQ(CLV_VideoCodedSize(16384u, 16384u, &w, &h), CLV_VIDEO_OK);
		EXPECT_EQ(static_cast<uint64_t>(w) * h, clv::video::kMaxReconstructPixels);
		// 界外一档：16 385 补后 16 400，积 268 992 000 = 16 400 × 16 400 > 界
		EXPECT_EQ(CLV_VideoCodedSize(16385u, 16385u, &w, &h), CLV_VIDEO_VALUE_RANGE);
	}
	uint32_t w = 0u;
	uint32_t h = 0u;
	// 原始尺寸合法域 [1, 65535]，0 是「未声明」不是尺寸
	EXPECT_EQ(CLV_VideoCodedSize(0u, 100u, &w, &h), CLV_VIDEO_VALUE_RANGE);
	EXPECT_EQ(CLV_VideoCodedSize(100u, 0u, &w, &h), CLV_VIDEO_VALUE_RANGE);
	EXPECT_EQ(CLV_VideoCodedSize(100u, 100u, nullptr, &h), CLV_VIDEO_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_VideoCodedSize(100u, 100u, &w, nullptr), CLV_VIDEO_INVALID_ARGUMENT);
}

TEST(VideoHeaders, PublicSurfaceStaysInsideVideoSegment)
{
	EXPECT_EQ(static_cast<int>(CLV_VIDEO_OK), 0);
	EXPECT_EQ(static_cast<int>(CLV_VIDEO_INVALID_HANDLE), 64);
	EXPECT_EQ(static_cast<int>(CLV_VIDEO_STREAM_REJECTED), 83);
	// 84 = 逐调用侧量化越界，紧跟段尾追加；69（声明侧）与它必须是两个值
	EXPECT_EQ(static_cast<int>(CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL), 84);
	/* 85 = 载荷长度不符（含过长与过短）：过短一支仍走 67（那个码的公开定义钉死是「短于定长」），
	 * 过长一支判本码；号位同样在段尾追加，不复用 73 / 78 / 81 / 82 那几个留白位。 */
	EXPECT_EQ(static_cast<int>(CLV_VIDEO_PAYLOAD_LENGTH_MISMATCH), 85);
	// 四档零产点码退场后显式给值的后继格：号位若被后续追加挤偏，这里先红
	EXPECT_EQ(static_cast<int>(CLV_VIDEO_SLICE_HEADER_BAD), 74);
	EXPECT_EQ(static_cast<int>(CLV_VIDEO_STATE_ERROR), 79);
	EXPECT_EQ(static_cast<int>(CLV_VIDEO_OUT_OF_MEMORY), 80);

	/* 手抄镜像表 = 活码的号位与文案，两处逐字对照：号位与含义取公开头 `CLV_Video.h` 的枚举注释，
	 * 文案取实现层门面 `StrError` 那张 switch（内部枚举与公开枚举逐项同序，见 `video/status.h`）。
	 * 73 / 78 / 81 / 82 是不进表的留白号位（号位不复用）⇒ 与段外取值同判 unknown。 */
	struct CodeText
	{
		CLV_VideoError code;
		int number;
		const char* text;
	};

	static const CodeText kMirror[] = {
		{ CLV_VIDEO_OK,						 0,	 "ok"					   },
		{ CLV_VIDEO_INVALID_HANDLE,			 64, "invalid handle"		   },
		{ CLV_VIDEO_INVALID_ARGUMENT,		 65, "invalid argument"		   },
		{ CLV_VIDEO_VALUE_RANGE,			 66, "value range"			   },
		{ CLV_VIDEO_PAYLOAD_TOO_SMALL,		 67, "payload too small"	   },
		{ CLV_VIDEO_HEADER_BAD,				 68, "vop header"			   },
		{ CLV_VIDEO_QUANT_OUT_OF_RANGE,		 69, "quant range"			   },
		{ CLV_VIDEO_BIT_DEPTH_UNSUPPORTED,	 70, "bit depth"			   },
		{ CLV_VIDEO_TOOL_EXCLUDED,			 71, "tool excluded"		   },
		{ CLV_VIDEO_TOOL_LIST_ABSENT,		 72, "tool list absent"		   },
		{ CLV_VIDEO_SLICE_HEADER_BAD,		 74, "slice header"			   },
		{ CLV_VIDEO_SLICE_SEQUENCE,			 75, "slice sequence"		   },
		{ CLV_VIDEO_REF_FRAME_MISSING,		 76, "reference missing"	   },
		{ CLV_VIDEO_OUTPUT_TOO_SMALL,		 77, "output too small"		   },
		{ CLV_VIDEO_STATE_ERROR,			 79, "state error"			   },
		{ CLV_VIDEO_OUT_OF_MEMORY,			 80, "out of memory"		   },
		{ CLV_VIDEO_STREAM_REJECTED,		 83, "stream rejected"		   },
		{ CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL, 84, "quant range per call"	   },
		{ CLV_VIDEO_PAYLOAD_LENGTH_MISMATCH, 85, "payload length mismatch" }
	};
	/* 表自身先对账：1 个成功码 + 段内 18 个失败码（64 到 72 九格、74 到 77 四格、79 到 80 两格、
	 * 83 到 85 三格）= 19 行；每行的枚举名与号位必须同值，号位逐行递增（漏一行这里先红，
	 * 不用等遍历 —— 遍历对「表里没登记的活码」只会报 unknown 不符，报不出「漏了哪一格」）。 */
	EXPECT_EQ(std::size(kMirror), 19u);
	int mirrored_previous = -1;
	for (const CodeText& e: kMirror)
	{
		EXPECT_EQ(static_cast<int>(e.code), e.number) << "镜像行的枚举名与号位不符";
		EXPECT_GT(e.number, mirrored_previous) << "镜像行的号位没按段内升序登记";
		mirrored_previous = e.number;
	}
	/* 值域全覆盖：段内 64..85 与段内预留位 86..95、段外 1..63 逐格都要有唯一结论 ——
	 * 在表里就给该码的文案，不在表里就一律 unknown（不得与任何已命名码同值同义）。 */
	for (int v = 0; v <= 95; ++v)
	{
		const char* want = "unknown";
		for (const CodeText& e: kMirror)
			if (e.number == v) want = e.text;
		const char* s = CLV_VideoStrError(static_cast<CLV_VideoError>(v));
		EXPECT_NE(s, nullptr) << "code " << v;
		EXPECT_STREQ(s, want) << "code " << v;
	}
	// 无名留白四格单独报一句「空位不该有名字」：漏进表里时报的就是这句话
	for (const int gap: { 73, 78, 81, 82 })
		EXPECT_STREQ(CLV_VideoStrError(static_cast<CLV_VideoError>(gap)), "unknown") << "空位 " << gap << " 不该有名字";
	EXPECT_STREQ(CLV_VideoStrError(static_cast<CLV_VideoError>(999)), "unknown");

	const std::vector<uint8_t> meta = MetaBytes();
	CLV_VideoEncMetaC out;
	std::memset(&out, 0, sizeof(out));
	EXPECT_EQ(CLV_VideoParseEncMeta(meta.data(), 7u, &out), CLV_VIDEO_PAYLOAD_TOO_SMALL);
	// 过长一支判载荷长度不符：定长 8 字节之外的两档不是一个出口，短了读不出字段、长了是装进了规定外的字节
	EXPECT_EQ(CLV_VideoParseEncMeta(meta.data(), 9u, &out), CLV_VIDEO_PAYLOAD_LENGTH_MISMATCH);
	std::vector<uint8_t> bad_reserved = MetaBytes(0u, 7u, 0u, 9u);
	EXPECT_EQ(CLV_VideoParseEncMeta(bad_reserved.data(), bad_reserved.size(), &out), CLV_VIDEO_HEADER_BAD);
	std::vector<uint8_t> bad_qp = MetaBytes(0u, 1u);
	EXPECT_EQ(CLV_VideoParseEncMeta(bad_qp.data(), bad_qp.size(), &out), CLV_VIDEO_VALUE_RANGE);
	std::vector<uint8_t> zero_dim = MetaBytes(0u, 7u, 0u, 0u, 0u, 144u);
	EXPECT_EQ(CLV_VideoParseEncMeta(zero_dim.data(), zero_dim.size(), &out), CLV_VIDEO_VALUE_RANGE);
	EXPECT_EQ(CLV_VideoParseEncMeta(nullptr, 8u, &out), CLV_VIDEO_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_VideoParseEncMeta(meta.data(), meta.size(), nullptr), CLV_VIDEO_INVALID_ARGUMENT);
}

TEST(VideoHeaders, DecoderHandleOpensForV1AndRejectsExcludedTools)
{
	CLV_VideoStreamParamsC p;
	std::memset(&p, 0, sizeof(p));
	p.bit_depth = 8u;
	p.quant_precision = 7u;
	p.obmc_disable = 1u;
	p.display_width = 176u;
	p.display_height = 144u;
	CLV_VideoError err = CLV_VIDEO_OK;
	// 解码器句柄是真实现：v1 合规参数能开
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&p, &err);
	ASSERT_NE(d, nullptr);
	EXPECT_EQ(err, CLV_VIDEO_OK);
	uint8_t rejected = 1u;
	EXPECT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK);
	EXPECT_EQ(rejected, 0u);
	CLV_VideoDecoderClose(d);

	// 位深非 8 → 拒解（10 / 12-bit 的像素电平 v1 未定义）
	CLV_VideoStreamParamsC depth = p;
	depth.bit_depth = 10u;
	DecoderOpensRejected(&depth, &err);
	EXPECT_EQ(err, CLV_VIDEO_BIT_DEPTH_UNSUPPORTED);
	// 排除集特征位一出现就拒整条流：透明、隔行、layer 非 0、非默认 VLC 表
	CLV_VideoStreamParamsC alpha = p;
	alpha.alpha_enabled = 1u;
	DecoderOpensRejected(&alpha, &err);
	EXPECT_EQ(err, CLV_VIDEO_TOOL_EXCLUDED);
	CLV_VideoStreamParamsC layer = p;
	layer.layer_id = 1u;
	DecoderOpensRejected(&layer, &err);
	EXPECT_EQ(err, CLV_VIDEO_TOOL_EXCLUDED);
	CLV_VideoStreamParamsC table = p;
	table.vlc_table_id = 1u;
	DecoderOpensRejected(&table, &err);
	EXPECT_EQ(err, CLV_VIDEO_TOOL_EXCLUDED);
	// 声明了清单却没给内容 = 字段不成立
	CLV_VideoStreamParamsC list = p;
	list.tool_list_size = 4u;
	DecoderOpensRejected(&list, &err);
	EXPECT_EQ(err, CLV_VIDEO_TOOL_LIST_ABSENT);
	// 量化精度与尺寸的合法域
	CLV_VideoStreamParamsC qp = p;
	qp.quant_precision = 2u;
	EXPECT_EQ(CLV_VideoDecoderOpen(&qp, &err), nullptr);
	EXPECT_EQ(err, CLV_VIDEO_VALUE_RANGE);
	CLV_VideoStreamParamsC zero = p;
	zero.display_height = 0u;
	EXPECT_EQ(CLV_VideoDecoderOpen(&zero, &err), nullptr);
	EXPECT_EQ(err, CLV_VIDEO_VALUE_RANGE);

	// 编码器句柄已接真写（语法层）：能开、能出帧缓冲、能出载荷
	CLV_VideoEncParamsC ep;
	std::memset(&ep, 0, sizeof(ep));
	ep.display_width = 176u;
	ep.display_height = 144u;
	ep.bit_depth = 8u;
	ep.quant_precision = 7u;
	ep.default_quant = 24u;
	CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&ep, &err);
	ASSERT_NE(e, nullptr);
	EXPECT_EQ(err, CLV_VIDEO_OK);
	CLV_VideoFrame* owned = nullptr;
	EXPECT_EQ(CLV_VideoEncoderAllocFrame(e, &owned), CLV_VIDEO_OK);
	ASSERT_NE(owned, nullptr);
	CLV_VideoFrameRelease(owned);
	CLV_VideoEncoderClose(e);

	// 空句柄先报句柄，不掩盖入参检查
	CLV_VideoFrame* frame = nullptr;
	uint8_t got = 1u;
	EXPECT_EQ(CLV_VideoDecoderNextOutput(nullptr, &frame, &got), CLV_VIDEO_INVALID_HANDLE);
	EXPECT_EQ(frame, nullptr);
	EXPECT_EQ(got, 0u);
	rejected = 1u;
	EXPECT_EQ(CLV_VideoDecoderStreamRejected(nullptr, &rejected), CLV_VIDEO_INVALID_HANDLE);
	EXPECT_EQ(CLV_VideoEncoderAllocFrame(nullptr, &frame), CLV_VIDEO_INVALID_HANDLE);
	uint32_t slices = 1u;
	EXPECT_EQ(CLV_VideoBitstreamSliceCount(nullptr, &slices), CLV_VIDEO_INVALID_HANDLE);
	const uint8_t* data = nullptr;
	size_t size = 1u;
	EXPECT_EQ(CLV_VideoBitstreamGetBytes(nullptr, &data, &size), CLV_VIDEO_INVALID_HANDLE);
	CLV_VideoFrameDescC desc;
	std::memset(&desc, 0, sizeof(desc));
	EXPECT_EQ(CLV_VideoFrameGetDesc(nullptr, &desc, nullptr), CLV_VIDEO_INVALID_HANDLE);
	EXPECT_EQ(CLV_VideoFrameAddRef(nullptr), 0u);
	CLV_VideoFrameRelease(nullptr);
	// 码流释放口对 NULL 也是 no-op
	CLV_VideoBitstreamRelease(nullptr);
	CLV_VideoDecoderClose(nullptr);
	CLV_VideoEncoderClose(nullptr);
}
