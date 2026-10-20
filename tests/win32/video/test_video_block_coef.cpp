/* test_video_block_coef.cpp
 *
 * 两种系数编码法（`video/block.h` 的 CoefCoding：0 = 定宽直写、1 = EOB 截尾 + Exp-Golomb 幅值）
 * 的块载体单元面。只吃实现层头 ⇒ 恒链静态件，与 test_video_block.cpp 同侧。
 *
 * 本文件钉的是四件事，按「改语法不能悄悄改像素」的口径排：
 *   一 · 往返忠实：两法、任何系数形状，解回来必须逐系数全等。
 *   二 · 上界成立：编码字节数 ≤ 在场块数 × 本法单块最坏字节数。**满幅稠密块是这条的回归位** ——
 *        变长法的最坏块比定宽宽（满幅系数的 EG 码长过定宽位宽），沿用定宽上界时它会被解码侧
 *        自己判成码流不成立，所以这一格必须真出现在用例里，不能靠实测素材「碰巧没触界」。
 *   三 · 代价随内容走：同位图、同 q、同帧型，只换块内非零个数 —— 法 0 下两种形状**逐字节等长**
 *        （这条把「零也算载荷」钉成断言而不是叙述），法 1 下只有 DC 的必须更便宜。
 *   四 · 拒绝路径不丢：截断、越上界、跳过 MB 带字节、法 0 的在场全零块、法 1 的 eob 位为零，逐条断言。
 *
 * 期望值不取自被测实现的输出快照：位宽、上界、EG 码长与手写的码字都由本文件按定义式现算。
 */

#include "video/bits.h"
#include "video/block.h"
#include "video/status.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <vector>

namespace
{

	using clv::video::BitWriter;
	using clv::video::BlockBits;
	using clv::video::BlockBytes;
	using clv::video::BlockBytesMax;
	using clv::video::BlockQfMax;
	using clv::video::BlockSizeMatches;
	using clv::video::CoefCoding;
	using clv::video::DecodeBlockData;
	using clv::video::EncodeBlockData;
	using clv::video::kBlockCoefs;
	using clv::video::kBlocksPerMacroblock;
	using clv::video::VideoErr;

	using Coefs = int32_t[kBlocksPerMacroblock][kBlockCoefs];

	constexpr uint8_t kPatternAll6 = 0x3Fu;
	constexpr uint8_t kPatternFirstLuma = 0x20u;	// bit5 = 亮度块 0 在场，其余不在场
	constexpr CoefCoding kBoth[2] = { CoefCoding::kFixed, CoefCoding::kEobEg };
	constexpr uint8_t kQs3[] = { 1u, 8u, 127u };
	constexpr uint8_t kQs4[] = { 1u, 8u, 64u, 127u };	 // 六块全在场那组用的档

	/* 确定性系数形状：不取随机，逐格可复算。 */
	void ShapeDcOnly(Coefs qf, int32_t dc)
	{
		for (int b = 0; b < kBlocksPerMacroblock; ++b)
			for (int i = 0; i < kBlockCoefs; ++i) qf[b][i] = (i == 0) ? dc : 0;
	}

	void ShapeSparse(Coefs qf, int32_t unit)
	{
		for (int b = 0; b < kBlocksPerMacroblock; ++b)
			for (int i = 0; i < kBlockCoefs; ++i) qf[b][i] = ((i % 13) == 0) ? ((i / 13) % 2 ? -unit : unit) : 0;
	}

	/* 满块：64 个系数全非零且取到 |QF| 上界 —— 这是法 1 最坏字节数的最坏输入。 */
	void ShapeDense(Coefs qf, int32_t cap)
	{
		for (int b = 0; b < kBlocksPerMacroblock; ++b)
			for (int i = 0; i < kBlockCoefs; ++i) qf[b][i] = (i & 1) ? -cap : cap;
	}

	void ShapeZero(Coefs qf)
	{
		for (int b = 0; b < kBlocksPerMacroblock; ++b)
			for (int i = 0; i < kBlockCoefs; ++i) qf[b][i] = 0;
	}

	/* 位图必须与内容一致：编码器只给「至少一个非零」的块置位图，所以拿全 6 位图去解一个
	 * 含全零在场块的码流，被拒是**正确行为**（法 0 那一侧），不是往返失败。 */
	bool RoundTrip(const Coefs& in, uint8_t pattern, uint8_t q, bool intra, CoefCoding coding, Coefs* out)
	{
		std::vector<uint8_t> bytes;
		if (EncodeBlockData(pattern, in, q, intra, coding, &bytes) != VideoErr::kOk) return false;
		if (DecodeBlockData(pattern, bytes.data(), bytes.size(), q, intra, coding, *out) != VideoErr::kOk) return false;
		for (int b = 0; b < kBlocksPerMacroblock; ++b)
		{
			for (int i = 0; i < kBlockCoefs; ++i)
				if ((*out)[b][i] != in[b][i]) return false;
		}
		return true;
	}

	size_t EncodedSize(const Coefs& qf, uint8_t pattern, uint8_t q, bool intra, CoefCoding coding)
	{
		std::vector<uint8_t> bytes;
		return EncodeBlockData(pattern, qf, q, intra, coding, &bytes) == VideoErr::kOk ? bytes.size() : 0u;
	}

	/* Exp-Golomb order-0 的码长与码字，按定义式在本文件独立写一遍（不读实现的输出）：
	 * x = v + 1、L = floor(log2 x)，写 L 个 0、再写 1、再写 x 的低 L 位。 */
	uint32_t EgLen(uint32_t v) noexcept
	{
		uint32_t L = 0u;
		for (uint32_t t = v + 1u; t > 1u; t >>= 1) ++L;
		return 2u * L + 1u;
	}

	void PutEg(BitWriter* bw, uint32_t v)
	{
		const uint32_t x = v + 1u;
		uint32_t L = 0u;
		for (uint32_t t = x; t > 1u; t >>= 1) ++L;
		bw->Write(0u, static_cast<uint8_t>(L));
		bw->Write(1u, 1u);
		if (L > 0u) bw->Write(x & ((1u << L) - 1u), static_cast<uint8_t>(L));
	}

}	 // namespace

/* 一 · 往返忠实：三档 q × 帧内 / 帧间 × 三种非零形状 × 两法，逐系数全等。 */
TEST(BlockCoef, RoundTripIsExactForBothMethods)
{
	for (uint8_t q: kQs3)
	{
		for (bool intra: { true, false })
		{
			const int32_t cap = BlockQfMax(q, intra);
			Coefs a {}, b {}, c {}, one {}, back {};
			ShapeDcOnly(a, cap > 1 ? cap / 2 : 1);
			ShapeSparse(b, cap > 1 ? cap / 3 : 1);
			ShapeDense(c, cap);
			/* 单块在场那格必须用「只有块 0 非零」的形状：位图只给块 0 置位，其余五块不占字节，
			 * 往返比对是对全部六块做的 —— 拿六块全非零的形状配单位图，比的是自己的用例写错。 */
			ShapeZero(one);
			ShapeSparse(one, cap > 1 ? cap / 3 : 1);
			for (int i = 1; i < kBlocksPerMacroblock; ++i)
				for (int k = 0; k < kBlockCoefs; ++k) one[i][k] = 0;
			for (CoefCoding coding: kBoth)
			{
				EXPECT_TRUE(RoundTrip(a, kPatternAll6, q, intra, coding, &back))
					<< "q=" << +q << " intra=" << intra << " 法=" << +static_cast<uint8_t>(coding) << " DC-only";
				EXPECT_TRUE(RoundTrip(b, kPatternAll6, q, intra, coding, &back)) << "q=" << +q << " intra=" << intra;
				EXPECT_TRUE(RoundTrip(c, kPatternAll6, q, intra, coding, &back)) << "q=" << +q << " 满块";
				EXPECT_TRUE(RoundTrip(one, kPatternFirstLuma, q, intra, coding, &back)) << "q=" << +q << " 单块在场";
			}
		}
	}
}

/* 一之二 · 无损档同样两法全等：q = 0 时载体直载变换原值，界是 8192 / 16320 而不是量化档。 */
TEST(BlockCoef, RoundTripIsExactAtLosslessCarrier)
{
	for (CoefCoding coding: kBoth)
	{
		for (bool intra: { true, false })
		{
			const int32_t cap = BlockQfMax(0u, intra);
			Coefs dense {}, back {};
			ShapeDense(dense, cap);
			EXPECT_TRUE(RoundTrip(dense, kPatternAll6, 0u, intra, coding, &back))
				<< "无损 intra=" << intra << " 法=" << +static_cast<uint8_t>(coding) << " 满幅 " << cap;
		}
	}
	EXPECT_GT(BlockQfMax(0u, false), BlockQfMax(1u, false))
		<< "无损档的界必须比最细有损档宽，否则这条测的不是同一支载体";
}

/* 二 · 上界成立。稠密满幅那格同时量出「本法最坏块确实长过定宽」，作为不能沿用定宽上界的证据；
 *    下面那条 EXPECT_GE 就是这个证据的反面：法 1 的上界若短于定宽，说明上界算错了。 */
TEST(BlockCoef, DenseBlockStaysWithinItsOwnMethodBound)
{
	for (uint8_t q: kQs3)
	{
		for (bool intra: { true, false })
		{
			Coefs dense {}, back {};
			ShapeDense(dense, BlockQfMax(q, intra));
			const uint32_t fixed = 6u * BlockBytes(q, intra);
			const uint32_t bound_fixed = 6u * BlockBytesMax(q, intra, CoefCoding::kFixed);
			const uint32_t bound_eg = 6u * BlockBytesMax(q, intra, CoefCoding::kEobEg);
			const size_t got_fixed = EncodedSize(dense, kPatternAll6, q, intra, CoefCoding::kFixed);
			const size_t got_eg = EncodedSize(dense, kPatternAll6, q, intra, CoefCoding::kEobEg);
			ASSERT_GT(got_fixed, 0u);
			ASSERT_GT(got_eg, 0u);
			/* 法 0：恰等，且它的上界与它的单块字节数是同一个数。 */
			EXPECT_EQ(got_fixed, fixed);
			EXPECT_EQ(bound_fixed, fixed);
			/* 法 1：不越自己的上界，但满幅内容比法 0 贵 —— 这条就是「不能沿用定宽上界」的实测量。 */
			EXPECT_LE(got_eg, bound_eg) << "q=" << +q << " intra=" << intra << " 越上界";
			EXPECT_GE(bound_eg, fixed) << "q=" << +q << " intra=" << intra << " 法 1 上界若小于定宽，说明算松了";
			EXPECT_GT(got_eg, got_fixed) << "q=" << +q << " intra=" << intra << " 满幅稠密下法 1 应比定宽贵";
			EXPECT_TRUE(BlockSizeMatches(kPatternAll6, static_cast<uint32_t>(got_fixed), q, intra, CoefCoding::kFixed));
			EXPECT_TRUE(BlockSizeMatches(kPatternAll6, static_cast<uint32_t>(got_eg), q, intra, CoefCoding::kEobEg))
				<< "q=" << +q << " intra=" << intra << " 自校验谓词不认编码器自己的产物";
			EXPECT_TRUE(RoundTrip(dense, kPatternAll6, q, intra, CoefCoding::kEobEg, &back));
		}
	}
}

/* 二之二 · 定宽那侧的上界是恒等式：BlockBytesMax == BlockBytes（64 × bits 恰为 8 的倍数）。
 *        这条把「法 0 一字未动」从说法变成断言。 */
TEST(BlockCoef, FixedWidthUpperBoundIsIdentity)
{
	for (uint8_t q: kQs4)
	{
		for (bool intra: { true, false })
		{
			const uint32_t bits = static_cast<uint32_t>(BlockBits(q, intra));
			ASSERT_EQ(bits * kBlockCoefs % 8u, 0u) << "q=" << +q << " 定宽块宽必须整字节";
			EXPECT_EQ(BlockBytesMax(q, intra, CoefCoding::kFixed), BlockBytes(q, intra)) << "q=" << +q;
			EXPECT_EQ(BlockBytes(q, intra), static_cast<uint32_t>(bits) * static_cast<uint32_t>(kBlockCoefs) / 8u)
				<< "q=" << +q << " intra=" << intra;
		}
	}
}

/* 三 · 代价是否随内容走：同位图、同 q、同帧型，只换块内非零个数。 */
TEST(BlockCoef, CostTracksContentNotJustPresentCount)
{
	for (bool intra: { true, false })
	{
		for (uint8_t q: kQs4)
		{
			const int32_t cap = BlockQfMax(q, intra);
			Coefs dc {}, mid {};
			ShapeDcOnly(dc, cap / 2 > 0 ? cap / 2 : 1);
			ShapeDense(mid, cap);
			const size_t s_fixed_dc = EncodedSize(dc, kPatternAll6, q, intra, CoefCoding::kFixed);
			const size_t s_fixed_full = EncodedSize(mid, kPatternAll6, q, intra, CoefCoding::kFixed);
			const size_t s_eg_dc = EncodedSize(dc, kPatternAll6, q, intra, CoefCoding::kEobEg);
			const size_t s_eg_full = EncodedSize(mid, kPatternAll6, q, intra, CoefCoding::kEobEg);
			ASSERT_GT(s_fixed_dc, 0u);
			/* 定宽法：一块只有 DC 与一块 64 个系数全满付一样的钱。 */
			EXPECT_EQ(s_fixed_dc, s_fixed_full) << "法 0 下两者必须逐字节等长：q=" << +q << " intra=" << intra;
			/* 变长法：截尾真的省到了尾部零上。满幅那格反过来，见 DenseBlockStaysWithinItsOwnMethodBound。 */
			EXPECT_LT(s_eg_dc, s_fixed_dc) << "法 1 的 DC-only 必须比定宽便宜：q=" << +q << " intra=" << intra;
			EXPECT_LT(s_eg_dc, s_eg_full) << "法 1 下只有 DC 的块必须比满块便宜：q=" << +q;
		}
	}
}

/* 三之二 · 法 1 的块边界自终止，所以 MB 级长度只卡上界：DC-only 的实际字节远小于
 *        present × BlockBytesMax，而谓词必须认它。定宽那侧同形状的字节恰等于 6 × BlockBytes。 */
TEST(BlockCoef, SelfTerminatedBlocksNeedNotFillTheBound)
{
	for (uint8_t q: kQs3)
	{
		for (bool intra: { true, false })
		{
			Coefs dc {};
			ShapeDcOnly(dc, BlockQfMax(q, intra) / 2);
			const size_t s = EncodedSize(dc, kPatternAll6, q, intra, CoefCoding::kEobEg);
			const uint32_t bound = 6u * BlockBytesMax(q, intra, CoefCoding::kEobEg);
			ASSERT_GT(s, 0u);
			ASSERT_LT(s, static_cast<size_t>(bound)) << "q=" << +q << " 没留出可判的余量，这条退化成恒等";
			EXPECT_TRUE(BlockSizeMatches(kPatternAll6, static_cast<uint32_t>(s), q, intra, CoefCoding::kEobEg));
			/* 越出上界那一格必须拒：谓词不是「一律认」。 */
			EXPECT_FALSE(BlockSizeMatches(kPatternAll6, bound + 1u, q, intra, CoefCoding::kEobEg)) << "q=" << +q;
		}
	}
}

/* 四 · 拒绝路径一条不丢：截断与越上界，两法各断。 */
TEST(BlockCoef, MalformedStreamsAreRejected)
{
	const uint8_t q = 8u;
	for (CoefCoding coding: kBoth)
	{
		Coefs in {}, back {};
		ShapeSparse(in, 7);
		std::vector<uint8_t> bytes;
		ASSERT_EQ(EncodeBlockData(kPatternAll6, in, q, true, coding, &bytes), VideoErr::kOk);
		ASSERT_FALSE(bytes.empty());
		/* 截断一个以上字节 ⇒ 必失败，不得静默解出错系数。 */
		for (size_t cut = 1; cut < bytes.size() && cut < 6; ++cut)
		{
			std::vector<uint8_t> short_(bytes.begin(), bytes.end() - static_cast<long>(cut));
			if (coding == CoefCoding::kFixed)
			{
				EXPECT_EQ(DecodeBlockData(kPatternAll6, short_.data(), short_.size(), q, true, coding, back),
						  VideoErr::kValueRange)
					<< "截断 " << cut << " 字节：定宽下长度不洽即拒";
			}
			else
			{
				EXPECT_NE(DecodeBlockData(kPatternAll6, short_.data(), short_.size(), q, true, coding, back),
						  VideoErr::kOk)
					<< "截断 " << cut << " 字节仍被判成功（法 1）";
			}
		}
		/* 越上界：在原产物后面补零到越过 present × BlockBytesMax。 */
		std::vector<uint8_t> over = bytes;
		const size_t want = 6u * BlockBytesMax(q, true, coding);
		while (over.size() <= want) over.push_back(0u);
		EXPECT_NE(DecodeBlockData(kPatternAll6, over.data(), over.size(), q, true, coding, back), VideoErr::kOk)
			<< "size 越上界未被拒 ⇒ 上界判据形同虚设（法=" << +static_cast<uint8_t>(coding) << "）";
	}
}

/* 四之二 · 「位图为 1 但整块全零」的处置按法分岔，这条把分岔本身钉住：
 *        定宽法没有能表达「在场且全零」的码字（编码器不会给它置位图），法 1 有 all_zero 一位。 */
TEST(BlockCoef, AllZeroPresentBlockFollowsMethod)
{
	const uint8_t q = 8u;
	Coefs zero {}, back {};
	ShapeZero(zero);
	std::vector<uint8_t> bytes_fixed, bytes_eg;
	ASSERT_EQ(EncodeBlockData(kPatternAll6, zero, q, true, CoefCoding::kFixed, &bytes_fixed), VideoErr::kOk);
	ASSERT_EQ(EncodeBlockData(kPatternAll6, zero, q, true, CoefCoding::kEobEg, &bytes_eg), VideoErr::kOk);
	/* 定宽：写出来就是 432 个零位，读回来必须拒。 */
	EXPECT_EQ(DecodeBlockData(kPatternAll6, bytes_fixed.data(), bytes_fixed.size(), q, true, CoefCoding::kFixed, back),
			  VideoErr::kValueRange)
		<< "定宽法不承认「在场块整块全零」";
	/* 法 1：六块各 1 bit 的 all_zero 标志，读回来是全零系数而不是失败。 */
	ASSERT_EQ(DecodeBlockData(kPatternAll6, bytes_eg.data(), bytes_eg.size(), q, true, CoefCoding::kEobEg, back),
			  VideoErr::kOk)
		<< "法 1 用 all_zero 标志把全零块写成合法，解回来不该失败";
	for (int b = 0; b < kBlocksPerMacroblock; ++b)
		for (int i = 0; i < kBlockCoefs; ++i) EXPECT_EQ(back[b][i], 0);
	EXPECT_LT(bytes_eg.size(), bytes_fixed.size()) << "全零块正是两法差价最大的一格";
}

/* 四之三 · 跳过 MB（位图全 0）只接受 size == 0。定宽那侧由「恰等」天然给出，
 *        改成上界时必须把这一支单独留着，否则 P 帧的跳过 MB 会被判成码流不成立
 *        （症状：首帧 I 可解、第二帧 P 即失败 —— I 帧没有跳过 MB）。 */
TEST(BlockCoef, SkippedMacroblockAcceptsOnlyZeroSize)
{
	for (uint8_t q: kQs3)
	{
		for (CoefCoding coding: kBoth)
		{
			EXPECT_TRUE(BlockSizeMatches(0u, 0u, q, true, coding)) << "q=" << +q << " 位图全 0 且 size 0 必须合法";
			EXPECT_FALSE(BlockSizeMatches(0u, 1u, q, true, coding)) << "q=" << +q << " 位图全 0 却带字节，必须拒";
			Coefs back {};
			const uint8_t none[1] = { 0u };
			EXPECT_EQ(DecodeBlockData(0u, none, 0u, q, true, coding, back), VideoErr::kOk) << "q=" << +q;
			EXPECT_EQ(DecodeBlockData(0u, none, 1u, q, true, coding, back), VideoErr::kValueRange) << "q=" << +q;
		}
	}
}

/* 四之四 · 法 1 的 eob 位必须落在非零系数上：手写一条「eob 说最后一个非零在 3、3 号槽却写 0」的码流，
 *        必须按取值不成立拒掉。这条是「EOB 的定义」的守门位，长度与上界都拦不住它。 */
TEST(BlockCoef, EobSlotMustCarryNonZeroCoefficient)
{
	const uint8_t q = 8u;
	const bool intra = true;
	BitWriter bw;
	bw.Write(0u, 1u);	 // all_zero = 0
	bw.Write(3u, 6u);	 // eob = 3：声明最后一个非零在下标 3
	PutEg(&bw, 1u);		 // 槽 0：|QF| = 1
	bw.Write(0u, 1u);	 // 符号：正
	PutEg(&bw, 0u);		 // 槽 1 = 0（不写符号位）
	PutEg(&bw, 0u);		 // 槽 2 = 0
	PutEg(&bw, 0u);		 // 槽 3 = 0 ← 与上面声明的 eob 自相矛盾
	bw.AlignToByte(0u);
	const std::vector<uint8_t> bytes = bw.Bytes();
	Coefs back {};
	EXPECT_EQ(DecodeBlockData(kPatternFirstLuma, bytes.data(), bytes.size(), q, intra, CoefCoding::kEobEg, back),
			  VideoErr::kValueRange)
		<< "eob 位读回零还受理，等于把 EOB 的定义放弃掉";
}

/* 五 · 语法本身对平：手写法 1 码字（按 EG 定义式独立生成，不读实现输出）解回来的系数、
 *    字节数与位宽都要与算式一致。这条是「规范条文与实现同轨」的落点。 */
TEST(BlockCoef, HandWrittenEobEgStreamDecodesAsWritten)
{
	struct Case
	{
		uint32_t vals[4];	 // 槽 0..3 的 |QF|
		int32_t signs;		 // bit0..bit3：1 = 负
	};

	const Case cases[] = {
		{ { 1u, 0u, 0u, 5u },		0b0000 },
		   { { 7u, 0u, 2u, 0u },	   0b1001 },
		  { { 63u, 64u, 65u, 8159u }, 0b0110 }
	};
	const uint8_t q = 1u;
	const bool intra = false;	 // 帧间 q' = 1 的界 = 8159，正好让最后一格走到满幅
	const int32_t cap = BlockQfMax(q, intra);
	ASSERT_EQ(cap, 8159);

	for (const Case& c: cases)
	{
		const uint32_t eob = (c.vals[3] != 0u) ? 3u : ((c.vals[2] != 0u) ? 2u : ((c.vals[1] != 0u) ? 1u : 0u));
		BitWriter bw;
		bw.Write(0u, 1u);
		bw.Write(eob, 6u);
		size_t bits = 7u;	 // all_zero(1) + eob(6)
		int32_t want[kBlockCoefs] = {};
		for (uint32_t i = 0; i <= eob; ++i)
		{
			PutEg(&bw, c.vals[i]);
			bits += EgLen(c.vals[i]);
			if (c.vals[i] != 0u)
			{
				const uint32_t s = (static_cast<uint32_t>(c.signs) >> i) & 1u;
				bw.Write(s, 1u);
				bits += 1u;
				want[i] = (s != 0u) ? -static_cast<int32_t>(c.vals[i]) : static_cast<int32_t>(c.vals[i]);
			}
		}
		bw.AlignToByte(0u);
		const std::vector<uint8_t> bytes = bw.Bytes();
		ASSERT_EQ(bytes.size(), (bits + 7u) / 8u) << "字节数与位数的算式对不上";
		/* 手写的码字必须落在本法上界之内，否则这条测的是拒绝路径而不是解码。 */
		ASSERT_LE(bytes.size(), BlockBytesMax(q, intra, CoefCoding::kEobEg));

		Coefs back {};
		ASSERT_EQ(DecodeBlockData(kPatternFirstLuma, bytes.data(), bytes.size(), q, intra, CoefCoding::kEobEg, back),
				  VideoErr::kOk);
		for (int i = 0; i < kBlockCoefs; ++i) EXPECT_EQ(back[0][i], want[i]) << "槽 " << i;
		for (int b = 1; b < kBlocksPerMacroblock; ++b)
			for (int i = 0; i < kBlockCoefs; ++i) EXPECT_EQ(back[b][i], 0) << "不在场的块必须被清零";
	}
}
