/* test_video_macroblock.cpp
 *
 * **实现层用例**：直接 include 实现层头，不经公开 C ABI，**仅静态构建（`CLV_BUILD_SHARED=OFF`）下可链**。
 *
 * 宏块头两套语法与补边 / 裁回的用例。
 * I / P 的码字与列值一律从 annex_b_tables_p.h 里取（该表是外部原表删去 `+q` 行的结果），
 * 不在这里重抄码字；B-VOP 是 CLV 自研的字节对齐语法。
 */

#include "video/annex_b_tables_p.h"
#include "video/bits.h"
#include "video/macroblock.h"
#include "video/padding.h"
#include "video/status.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

	/* 层用例的宏块行宽：预测器要它推左 / 上 / 右上三个邻居。 */
	inline constexpr uint32_t kMbWidth = 2u;

	using clv::video::BitReader;
	using clv::video::BitWriter;
	using clv::video::BvopMacroblockHeader;
	using clv::video::BvopMvdCount;
	using clv::video::BvopPrediction;
	using clv::video::CbpyEntry;
	using clv::video::DecodeMacroblockLayer;
	using clv::video::EncodeBvopMacroblockHeader;
	using clv::video::EncodeMacroblockHeader;
	using clv::video::EncodeMacroblockLayer;
	using clv::video::FrameGeometry;
	using clv::video::IsIntraMacroblock;
	using clv::video::kBlackChroma8;
	using clv::video::kBlackLuma8;
	using clv::video::kBvopBackward;
	using clv::video::kBvopDirect;
	using clv::video::kBvopForward;
	using clv::video::kBvopInterpolate;
	using clv::video::kCbpyFourBlocks;
	using clv::video::kMbDquantMax;
	using clv::video::kMbDquantMin;
	using clv::video::kMbTypeInter;
	using clv::video::kMbTypeInter4V;
	using clv::video::kMbTypeIntra;
	using clv::video::kMcbpcIVop;
	using clv::video::kMcbpcIVopDroppedQRows;
	using clv::video::kMcbpcPVop;
	using clv::video::kMcbpcPVopDroppedQRows;
	using clv::video::kVopTypeB;
	using clv::video::kVopTypeI;
	using clv::video::kVopTypeP;
	using clv::video::Macroblock;
	using clv::video::MacroblockHeader;
	using clv::video::MbDquantFromBits;
	using clv::video::MbDquantToBits;
	using clv::video::McbpcEntry;
	using clv::video::MedianPredictor;
	using clv::video::MotionVector;
	using clv::video::PlaneBuf;
	using clv::video::VideoErr;

	std::string BitsOf(uint16_t code, uint8_t len)
	{
		std::string s;
		for (uint8_t i = 0; i < len; ++i) s.push_back(((code >> (len - 1u - i)) & 1u) ? '1' : '0');
		return s;
	}

	void WritePattern(BitWriter* bw, const std::string& bits)
	{
		for (char c: bits) bw->Write(static_cast<uint32_t>(c - '0'), 1u);
	}

	// 头里写了 block_size 就得真有那么多字节在场：按 0x5A 填一段凑成完整载荷
	void AppendBlockFiller(BitWriter* bw, uint32_t cnt)
	{
		for (uint32_t i = 0; i < cnt; ++i) bw->Write(0x5Au, 8u);
	}

	bool IsPrefixOf(const std::string& a, const std::string& b)
	{
		return a.size() <= b.size() && b.compare(0, a.size(), a) == 0;
	}

	const McbpcEntry* StuffingRow(const McbpcEntry* t, size_t n)
	{
		for (size_t i = 0; i < n; ++i)
			if (t[i].mb_type < 0) return &t[i];
		return nullptr;
	}

	const McbpcEntry* RowWithMbType(const McbpcEntry* t, size_t n, int8_t mt)
	{
		for (size_t i = 0; i < n; ++i)
			if (t[i].mb_type == mt) return &t[i];
		return nullptr;
	}

	size_t CountWithMbType(const McbpcEntry* t, size_t n, int8_t mt)
	{
		size_t c = 0u;
		for (size_t i = 0; i < n; ++i)
			if (t[i].mb_type == mt) ++c;
		return c;
	}

	size_t CheckedDroppedRows(const McbpcEntry* t, size_t n)
	{
		size_t c = 0u;
		for (size_t i = 0; i < n; ++i)
			if (t[i].mb_type == 1 || t[i].mb_type == 4) ++c;
		return c;
	}

	const CbpyEntry* CbpyRowWithPattern(bool intra, uint8_t pattern)
	{
		for (const CbpyEntry& e: kCbpyFourBlocks)
			if ((intra ? e.intra : e.inter) == pattern) return &e;
		return nullptr;
	}

	/* 单宏块的往返是**孤立用例**：预测器不参与，故两侧都给零预测值，
	 * 残差就等于矢量本身 —— 预测器那套（IpState / IpPredictAt）归层用例验。 */
	VideoErr EncodeOne(const MacroblockHeader& in, uint8_t vop_type, BitWriter* bw)
	{
		return EncodeMacroblockHeader(bw, vop_type, in, MotionVector {});
	}

	VideoErr DecodeOne(BitReader* br, uint8_t vop_type, MacroblockHeader* out)
	{
		return clv::video::DecodeMacroblockHeader(br, vop_type, out, MotionVector {});
	}

	VideoErr RoundTrip(uint8_t vop_type, const MacroblockHeader& in, MacroblockHeader* out)
	{
		BitWriter bw;
		const VideoErr e = EncodeOne(in, vop_type, &bw);
		if (e != VideoErr::kOk) return e;
		AppendBlockFiller(&bw, in.block_size);
		bw.AlignToByte(0u);
		BitReader br(bw.Bytes().data(), bw.Bytes().size());
		return DecodeOne(&br, vop_type, out);
	}

	std::vector<uint8_t> EncodeBytes(uint8_t vop_type, const MacroblockHeader& in)
	{
		BitWriter bw;
		EXPECT_EQ(EncodeOne(in, vop_type, &bw), VideoErr::kOk);
		AppendBlockFiller(&bw, in.block_size);
		bw.AlignToByte(0u);
		return bw.Bytes();
	}

	VideoErr DecodeBytes(const std::vector<uint8_t>& bytes, uint8_t vop_type, MacroblockHeader* out)
	{
		BitReader br(bytes.data(), bytes.size());
		return DecodeOne(&br, vop_type, out);
	}

	/* not_coded = 0 之后直接塞一段任意位串，看解码器怎么判。 */
	VideoErr FeedRawBits(const std::string& bits, uint8_t vop_type, MacroblockHeader* out)
	{
		BitWriter bw;
		bw.Write(0u, 1u);
		WritePattern(&bw, bits);
		for (int i = 0; i < 9; ++i) bw.Write(0u, 1u);	 // 够读到最大码长
		bw.AlignToByte(0u);
		return DecodeBytes(bw.Bytes(), vop_type, out);
	}

	MotionVector MakeMv(int32_t h, int32_t v)
	{
		MotionVector m;
		m.h = h;
		m.v = v;
		return m;
	}

}	 // namespace

TEST(VideoMacroblock, ClvTablesDropPlusQAndKeepExclusions)
{
	EXPECT_EQ(std::size(kMcbpcIVop), 5u);
	EXPECT_EQ(std::size(kMcbpcPVop), 13u);
	EXPECT_EQ(std::size(kMcbpcIVopDroppedQRows), 4u);
	EXPECT_EQ(std::size(kMcbpcPVopDroppedQRows), 8u);

	for (const McbpcEntry& e: kMcbpcIVop)
		ASSERT_TRUE(e.mb_type == kMbTypeIntra || e.mb_type < 0) << "I 表里出现了 mb_type " << int(e.mb_type);
	for (const McbpcEntry& e: kMcbpcPVop)
	{
		ASSERT_TRUE(e.mb_type == kMbTypeInter || e.mb_type == kMbTypeInter4V || e.mb_type == kMbTypeIntra ||
					e.mb_type < 0)
			<< "P 表里出现了 mb_type " << int(e.mb_type);
	}
	for (const McbpcEntry* t: { kMcbpcIVopDroppedQRows, kMcbpcPVopDroppedQRows })
	{
		const size_t n =
			t == kMcbpcIVopDroppedQRows ? std::size(kMcbpcIVopDroppedQRows) : std::size(kMcbpcPVopDroppedQRows);
		ASSERT_EQ(CheckedDroppedRows(t, n), n) << "被删表里混进了非 +q 行";
	}

	// 排除集行仍留在表里，为的是能识别成「被排除的工具」
	EXPECT_EQ(CountWithMbType(kMcbpcPVop, std::size(kMcbpcPVop), kMbTypeInter4V), 4u);
	EXPECT_NE(StuffingRow(kMcbpcIVop, std::size(kMcbpcIVop)), nullptr);
	EXPECT_NE(StuffingRow(kMcbpcPVop, std::size(kMcbpcPVop)), nullptr);
}

TEST(VideoMacroblock, AnnexBTablesArePrefixFree)
{
	const McbpcEntry* tables[] = { kMcbpcIVop, kMcbpcPVop };
	const size_t sizes[] = { std::size(kMcbpcIVop), std::size(kMcbpcPVop) };
	size_t pairs = 0u;
	for (size_t k = 0; k < 2; ++k)
	{
		for (size_t i = 0; i < sizes[k]; ++i)
		{
			for (size_t j = 0; j < sizes[k]; ++j)
			{
				if (i == j) continue;
				const std::string a = BitsOf(tables[k][i].code, tables[k][i].code_len);
				const std::string b = BitsOf(tables[k][j].code, tables[k][j].code_len);
				if (a.size() < b.size())
				{
					EXPECT_FALSE(IsPrefixOf(a, b)) << a << " 是 " << b << " 的前缀";
					++pairs;
				}
			}
		}
	}
	for (size_t i = 0; i < std::size(kCbpyFourBlocks); ++i)
	{
		for (size_t j = 0; j < std::size(kCbpyFourBlocks); ++j)
		{
			if (i == j) continue;
			const std::string a = BitsOf(kCbpyFourBlocks[i].code, kCbpyFourBlocks[i].code_len);
			const std::string b = BitsOf(kCbpyFourBlocks[j].code, kCbpyFourBlocks[j].code_len);
			if (a.size() < b.size())
			{
				EXPECT_FALSE(IsPrefixOf(a, b)) << a << " 是 " << b << " 的前缀";
				++pairs;
			}
		}
	}
	/* 判据不能是自家计数器「跑过几对」（断 > 0 的话，表漂成另一副样子也可能仍 > 0）⇒ 钉死对数。
	 * 算式按码长分布逐表列：
	 *   I 表 5 行，码长 {1, 3, 3, 3, 9} → 短前长后的有序对 = 1×4 + 3×1 = 7
	 *   P 表 13 行，码长 {1,3,4,4,5,6,7,7,7,8,8,8,9} → 1×12 + 1×11 + 2×9 + 1×8 + 1×7 + 3×4 + 3×1 = 71
		 *   cbpy 那张表 16 行，码长 {2,4×9,5×4,6×2} → 1×15 + 9×6 + 4×2 = 77
	 *   合计 7 + 71 + 77 = 155 对（任何一行的码长变了，这个数就变） */
	EXPECT_EQ(pairs, 155u);
}

TEST(VideoMacroblock, MbDquantMappingIsTwoComplement)
{
	struct Case
	{
		uint8_t bits;
		int8_t value;
	};

	const Case cases[] = {
		{ 0u, 0	 },
		  { 1u, 1  },
		{ 2u, -2 },
		   { 3u, -1 }
	};
	for (const Case& c: cases)
	{
		EXPECT_EQ(MbDquantFromBits(c.bits), c.value);
		EXPECT_EQ(MbDquantToBits(c.value), c.bits);
	}
	EXPECT_EQ(kMbDquantMin, -2);
	EXPECT_EQ(kMbDquantMax, 1);
}

TEST(VideoMacroblock, EveryLegalRowRoundTripsForAllCbpyAndDquant)
{
	size_t checked = 0u;
	size_t excluded = 0u;
	size_t legal_per_table[2] = { 0u, 0u };

	const struct
	{
		uint8_t vop;
		const McbpcEntry* table;
		size_t n;
	} jobs[] = {
		{ kVopTypeI, kMcbpcIVop, std::size(kMcbpcIVop) },
		 { kVopTypeP, kMcbpcPVop, std::size(kMcbpcPVop) }
	};

	for (size_t t = 0; t < 2; ++t)
	{
		const auto& job = jobs[t];
		size_t legal_rows = 0u;
		for (size_t i = 0; i < job.n; ++i)
		{
			const McbpcEntry& row = job.table[i];
			if (row.mb_type < 0 || row.mb_type == kMbTypeInter4V)
			{
				// 排除集（Stuffing / 4MV）由码字侧那条用例判报错路径，这里只数合法行
				++excluded;
				continue;
			}
			++legal_rows;
			for (uint8_t pattern = 0u; pattern < 16u; ++pattern)
			{
				for (int8_t d = kMbDquantMin; d <= kMbDquantMax; ++d)
				{
					MacroblockHeader in;
					in.not_coded = false;
					in.mb_type = row.mb_type;
					in.cbpc = row.cbpc;
					in.cbpy = pattern;
					in.dquant = d;
					in.block_size = 1u + (pattern & 3u);

					MacroblockHeader out;
					ASSERT_EQ(RoundTrip(job.vop, in, &out), VideoErr::kOk)
						<< "vop=" << static_cast<int>(job.vop) << " code=" << BitsOf(row.code, row.code_len)
						<< " pattern=" << static_cast<int>(pattern) << " d=" << static_cast<int>(d);
					EXPECT_FALSE(out.not_coded);
					EXPECT_EQ(out.mb_type, row.mb_type);
					EXPECT_EQ(out.cbpc, row.cbpc);
					EXPECT_EQ(out.cbpy, pattern);
					EXPECT_EQ(out.dquant, d);
					++checked;
				}
			}
		}
		legal_per_table[t] = legal_rows;
	}
	// 两条按码表行数分档的独立断言（自家计数器「跑了多少行」不算判据）：合法行 = 表里去掉 Stuffing
	//（mb_type < 0）与 4MV 的行。I 表 5 行 = 4 intra + 1 Stuffing ⇒ 4；P 表 13 行 = 4 intra + 4 inter + 4 inter4v
	// + 1 Stuffing ⇒ 8。这两数是规范给的宏块头取值集，不随表怎么排而变。
	EXPECT_EQ(legal_per_table[0], 4u) << "I-VOP 码表的合法行数";
	EXPECT_EQ(legal_per_table[1], 8u) << "P-VOP 码表的合法行数";
	// 排除集同样钉住：I 表 1 行 Stuffing、P 表 4 行 4MV + 1 行 Stuffing = 5
	EXPECT_EQ(excluded, 1u + 5u);
	// 每行 × 16 个 cbpy 图样 × 4 个 dquant 位型：(4 + 8) × 16 × 4 = 768
	EXPECT_EQ(checked, (legal_per_table[0] + legal_per_table[1]) * 16u * 4u);
	EXPECT_EQ(checked, 768u);
	std::cout << "宏块头逐字段往返判定点 = " << checked << "（合法行 4 + 8，各配 16 图样 × 4 dquant）\n";
}

TEST(VideoMacroblock, DeletedPlusQCodesDecodeAsBadStructure)
{
	size_t nails = 0u;

	const struct
	{
		uint8_t vop;
		const McbpcEntry* kept;
		size_t kn;
		const McbpcEntry* dropped;
		size_t dn;
	} jobs[] = {
		{ kVopTypeI, kMcbpcIVop, std::size(kMcbpcIVop), kMcbpcIVopDroppedQRows, std::size(kMcbpcIVopDroppedQRows) },
		{ kVopTypeP, kMcbpcPVop, std::size(kMcbpcPVop), kMcbpcPVopDroppedQRows, std::size(kMcbpcPVopDroppedQRows) }
	};

	for (const auto& job: jobs)
	{
		for (size_t i = 0; i < job.dn; ++i)
		{
			const std::string code = BitsOf(job.dropped[i].code, job.dropped[i].code_len);
			// 前提：被删码字与在表码字之间互不为前缀，否则「喂它就判表外」不成立
			for (size_t k = 0; k < job.kn; ++k)
			{
				const std::string keep = BitsOf(job.kept[k].code, job.kept[k].code_len);
				ASSERT_FALSE(IsPrefixOf(code, keep)) << code << " 是 " << keep << " 的前缀";
				ASSERT_FALSE(IsPrefixOf(keep, code)) << keep << " 是 " << code << " 的前缀";
			}
			MacroblockHeader out;
			EXPECT_EQ(FeedRawBits(code, job.vop, &out), VideoErr::kHeaderBad)
				<< "码字 " << code << "（mb_type " << int(job.dropped[i].mb_type) << "）应判表外码字";
			++nails;
		}
	}
	EXPECT_EQ(nails, 12u);
	std::cout << "被删 +q 码字回投样本 = " << nails << " 个，全部判 kHeaderBad\n";
}

TEST(VideoMacroblock, ReEncodingADecodedHeaderIsByteIdentical)
{
	const McbpcEntry* row = RowWithMbType(kMcbpcPVop, std::size(kMcbpcPVop), kMbTypeInter);
	ASSERT_NE(row, nullptr);
	for (uint8_t pattern = 0u; pattern < 16u; ++pattern)
	{
		MacroblockHeader a;
		a.mb_type = row->mb_type;
		a.cbpc = row->cbpc;
		a.cbpy = pattern;
		a.dquant = MbDquantFromBits(static_cast<uint8_t>(pattern & 3u));
		a.block_size = 1u + pattern;
		const std::vector<uint8_t> first = EncodeBytes(kVopTypeP, a);

		MacroblockHeader b;
		ASSERT_EQ(DecodeBytes(first, kVopTypeP, &b), VideoErr::kOk);
		EXPECT_EQ(EncodeBytes(kVopTypeP, b), first);
	}
}

TEST(VideoMacroblock, NotCodedMacroblockCostsExactlyOneBit)
{
	MacroblockHeader in;
	in.not_coded = true;
	in.mb_type = kMbTypeIntra;
	in.cbpc = 3u;
	in.cbpy = 0xFu;
	in.dquant = kMbDquantMin;

	BitWriter bw;
	ASSERT_EQ(EncodeOne(in, kVopTypeP, &bw), VideoErr::kOk);
	EXPECT_EQ(bw.BitCount(), 1u);
	bw.AlignToByte(0u);
	ASSERT_EQ(bw.Bytes().size(), 1u);
	EXPECT_EQ(bw.Bytes()[0], 0x80);

	MacroblockHeader out;
	ASSERT_EQ(DecodeBytes(bw.Bytes(), kVopTypeP, &out), VideoErr::kOk);
	EXPECT_TRUE(out.not_coded);
	EXPECT_EQ(out.cbpc, 0u);
	EXPECT_EQ(out.cbpy, 0u);
	EXPECT_EQ(out.dquant, 0);
}

TEST(VideoMacroblock, FourMvCodeIsRejectedOnPAndIsLegalAsIntraOnI)
{
	const McbpcEntry* four_mv = RowWithMbType(kMcbpcPVop, std::size(kMcbpcPVop), kMbTypeInter4V);
	ASSERT_NE(four_mv, nullptr);
	const std::string code = BitsOf(four_mv->code, four_mv->code_len);
	const CbpyEntry* cb = CbpyRowWithPattern(true, 0x0u);
	ASSERT_NE(cb, nullptr);

	BitWriter bw;
	bw.Write(0u, 1u);
	WritePattern(&bw, code);
	WritePattern(&bw, BitsOf(cb->code, cb->code_len));
	bw.Write(0u, 2u);	 // dquant = 00
	bw.Write(3u, 8u);	 // block_size = 3
	AppendBlockFiller(&bw, 3u);
	bw.AlignToByte(0u);
	const std::vector<uint8_t> bytes = bw.Bytes();

	MacroblockHeader out;
	// 4MV 行在 mcbpc 这一步就被拒，后面的 cbpy 都不用读
	EXPECT_EQ(DecodeBytes(bytes, kVopTypeP, &out), VideoErr::kToolExcluded);

	// 同一串位在 I 表里是 intra 的合法码字
	const McbpcEntry* on_i = nullptr;
	for (const McbpcEntry& e: kMcbpcIVop)
		if (BitsOf(e.code, e.code_len) == code) on_i = &e;
	ASSERT_NE(on_i, nullptr);
	ASSERT_EQ(DecodeBytes(bytes, kVopTypeI, &out), VideoErr::kOk);
	EXPECT_EQ(out.mb_type, on_i->mb_type);
	EXPECT_EQ(out.cbpc, on_i->cbpc);
	EXPECT_EQ(out.cbpy, cb->intra);
	std::cout << "同一串位 " << code << "：P 判 4MV 拒该帧，I 判 intra 合法\n";
}

TEST(VideoMacroblock, StuffingCodeIsRejected)
{
	const McbpcEntry* st = StuffingRow(kMcbpcPVop, std::size(kMcbpcPVop));
	ASSERT_NE(st, nullptr);

	BitWriter bw;
	bw.Write(0u, 1u);
	WritePattern(&bw, BitsOf(st->code, st->code_len));
	bw.AlignToByte(0u);

	MacroblockHeader out;
	EXPECT_EQ(DecodeBytes(bw.Bytes(), kVopTypeP, &out), VideoErr::kToolExcluded);
	EXPECT_EQ(DecodeBytes(bw.Bytes(), kVopTypeI, &out), VideoErr::kToolExcluded);
}

TEST(VideoMacroblock, CodeAbsentFromEveryTableIsBadStructure)
{
	// 两张 mcbpc 表共 5 + 13 = 18 行（两个行数由本件前面那两条 std::size 等值断言钉着），
	// 取一个两边都没有的位串：Stuffing 末位翻过来
	const McbpcEntry* st = StuffingRow(kMcbpcPVop, std::size(kMcbpcPVop));
	ASSERT_NE(st, nullptr);
	std::string unknown = BitsOf(st->code, st->code_len);
	unknown[unknown.size() - 1] = '0';
	const McbpcEntry* tables[] = { kMcbpcIVop, kMcbpcPVop };
	const size_t sizes[] = { std::size(kMcbpcIVop), std::size(kMcbpcPVop) };
	for (size_t len = 1; len <= unknown.size(); ++len)
	{
		const std::string pre = unknown.substr(0, len);
		for (size_t k = 0; k < 2; ++k)
			for (size_t i = 0; i < sizes[k]; ++i)
				ASSERT_FALSE(BitsOf(tables[k][i].code, tables[k][i].code_len) == pre) << pre;
	}

	MacroblockHeader out;
	EXPECT_EQ(FeedRawBits(unknown, kVopTypeP, &out), VideoErr::kHeaderBad);
	EXPECT_EQ(FeedRawBits(unknown, kVopTypeI, &out), VideoErr::kHeaderBad);
}

TEST(VideoMacroblock, TruncatedHeaderIsNotMistakenForRejection)
{
	BitWriter bw;
	bw.Write(0u, 1u);
	bw.AlignToByte(0u);

	MacroblockHeader out;
	EXPECT_EQ(DecodeBytes(bw.Bytes(), kVopTypeP, &out), VideoErr::kPayloadTooSmall);
}

TEST(VideoMacroblock, CbpyColumnFollowsMacroblockType)
{
	const McbpcEntry* m_intra = RowWithMbType(kMcbpcPVop, std::size(kMcbpcPVop), kMbTypeIntra);
	const McbpcEntry* m_inter = RowWithMbType(kMcbpcPVop, std::size(kMcbpcPVop), kMbTypeInter);
	const CbpyEntry* c_intra = CbpyRowWithPattern(true, 0x0u);
	const CbpyEntry* c_inter = CbpyRowWithPattern(false, 0x0u);
	ASSERT_NE(m_intra, nullptr);
	ASSERT_NE(m_inter, nullptr);
	ASSERT_NE(c_intra, nullptr);
	ASSERT_NE(c_inter, nullptr);
	ASSERT_GT(c_intra->code_len, c_inter->code_len);
	ASSERT_TRUE(IsIntraMacroblock(kMbTypeIntra));
	ASSERT_FALSE(IsIntraMacroblock(kMbTypeInter));

	MacroblockHeader intra;
	intra.mb_type = kMbTypeIntra;
	intra.cbpc = m_intra->cbpc;
	intra.cbpy = 0x0u;
	intra.block_size = 7u;
	MacroblockHeader inter;
	inter.mb_type = kMbTypeInter;
	inter.cbpc = m_inter->cbpc;
	inter.cbpy = 0x0u;
	inter.block_size = 7u;

	BitWriter bi, bb;
	ASSERT_EQ(EncodeOne(intra, kVopTypeP, &bi), VideoErr::kOk);
	ASSERT_EQ(EncodeOne(inter, kVopTypeP, &bb), VideoErr::kOk);
	/* 1 位 not_coded=0 + mcbpc + cbpy + 2 位 dquant + block_size 的 varint（7 < 128 → 1 字节）。 */
	EXPECT_EQ(bi.BitCount(), 1u + m_intra->code_len + c_intra->code_len + 2u + 8u);
	/* 帧间多一个 motion_vector() 在场条件 的 motion_vector()：本例矢量是零、预测值也是零，故两条 zigzag varint
	 * 各 1 字节 —— 帧内那一行没有这两字节，两条断言的差就是 motion_vector() 的在场与否。 */
	EXPECT_EQ(bb.BitCount(), 1u + m_inter->code_len + c_inter->code_len + 2u + 8u + 16u);
	AppendBlockFiller(&bi, intra.block_size);
	AppendBlockFiller(&bb, inter.block_size);

	bi.AlignToByte(0u);
	bb.AlignToByte(0u);
	MacroblockHeader oi, ob;
	ASSERT_EQ(DecodeBytes(bi.Bytes(), kVopTypeP, &oi), VideoErr::kOk);
	ASSERT_EQ(DecodeBytes(bb.Bytes(), kVopTypeP, &ob), VideoErr::kOk);
	EXPECT_EQ(oi.cbpy, c_intra->intra);
	EXPECT_EQ(ob.cbpy, c_inter->inter);
}

TEST(VideoMacroblock, EncodeRejectsFieldsOutsideTheTables)
{
	MacroblockHeader bad;
	bad.block_size = 1u;	// 先满足定界字段，让下面每条各自命中自己的检查
	bad.mb_type = kMbTypeInter4V;
	BitWriter b0;
	EXPECT_EQ(EncodeOne(bad, kVopTypeP, &b0), VideoErr::kToolExcluded);

	// block_size = 0 而块模式非全零：有残差却没块数据
	bad.mb_type = kMbTypeInter;
	bad.cbpy = 5u;	  // 位图非零前置条件：零长度的合法性只看位图
	bad.block_size = 0u;
	BitWriter b0b;
	EXPECT_EQ(EncodeOne(bad, kVopTypeP, &b0b), VideoErr::kValueRange);
	bad.block_size = 1u;

	// not_coded = 1 却带块数据长度
	MacroblockHeader skip;
	skip.not_coded = true;
	skip.block_size = 2u;
	BitWriter b0c;
	EXPECT_EQ(EncodeOne(skip, kVopTypeP, &b0c), VideoErr::kValueRange);

	bad.mb_type = kMbTypeInter;
	bad.cbpc = 4u;
	BitWriter b1;
	EXPECT_EQ(EncodeOne(bad, kVopTypeP, &b1), VideoErr::kValueRange);

	bad.cbpc = 0u;
	bad.cbpy = 16u;
	BitWriter b2;
	EXPECT_EQ(EncodeOne(bad, kVopTypeP, &b2), VideoErr::kValueRange);

	bad.cbpy = 0u;
	bad.dquant = 2;
	BitWriter b3;
	EXPECT_EQ(EncodeOne(bad, kVopTypeP, &b3), VideoErr::kValueRange);

	bad.dquant = -3;
	BitWriter b4;
	EXPECT_EQ(EncodeOne(bad, kVopTypeP, &b4), VideoErr::kValueRange);

	// I 表里没有 inter 型；被删的 +q 两型同样不可写
	bad.dquant = 0;
	bad.mb_type = kMbTypeInter;
	BitWriter b5;
	EXPECT_EQ(EncodeOne(bad, kVopTypeI, &b5), VideoErr::kInvalidArgument);
	for (int8_t q: { int8_t(1), int8_t(4) })
	{
		MacroblockHeader m;
		m.mb_type = q;
		m.block_size = 1u;
		BitWriter b;
		EXPECT_EQ(EncodeOne(m, kVopTypeP, &b), VideoErr::kInvalidArgument);
	}

	BitWriter b6;
	EXPECT_EQ(EncodeOne(bad, kVopTypeB, &b6), VideoErr::kToolExcluded);
	EXPECT_EQ(EncodeOne(bad, 7u, &b6), VideoErr::kInvalidArgument);
	EXPECT_EQ(EncodeOne(bad, kVopTypeP, nullptr), VideoErr::kInvalidArgument);
}

TEST(VideoMacroblock, OneFrameOfHeadersPlusBlockDataRoundTripsFieldByField)
{
	struct Item
	{
		MacroblockHeader mb;
		std::vector<uint8_t> block;
	};

	const std::vector<uint8_t> blk0 = { 0x10u, 0x20u, 0x30u };
	const std::vector<uint8_t> blk1 = { 0xABu };
	const std::vector<uint8_t> blk2(9u, 0x5Au);

	std::vector<Item> items;
	{
		MacroblockHeader a;
		a.not_coded = true;
		items.push_back({ a, {} });
		MacroblockHeader b;
		b.mb_type = kMbTypeInter;
		b.cbpc = 3u;
		b.cbpy = 0x7u;
		b.dquant = kMbDquantMin;
		items.push_back({ b, blk0 });
		MacroblockHeader c;
		c.mb_type = kMbTypeIntra;
		c.cbpc = 1u;
		c.cbpy = 0xFu;
		c.dquant = kMbDquantMax;
		items.push_back({ c, blk1 });
		MacroblockHeader d;
		d.mb_type = kMbTypeInter;
		d.cbpc = 2u;
		d.cbpy = 0x0u;
		d.dquant = -1;
		items.push_back({ d, blk2 });
		MacroblockHeader e;
		e.not_coded = true;
		items.push_back({ e, {} });
		MacroblockHeader f;
		f.mb_type = kMbTypeIntra;
		f.cbpc = 0u;
		f.cbpy = 0x5u;
		f.dquant = 0;
		items.push_back({ f, blk0 });
	}

	// 块数据长度由头里的 block_size 自定界：写方只交宏块列表，读方按 mb_count 一段段切开
	std::vector<Macroblock> mbs;
	size_t block_bytes = 0u;
	for (const Item& it: items)
	{
		Macroblock m;
		m.head = it.mb;
		m.block = it.block;
		m.head.block_size = static_cast<uint32_t>(it.block.size());
		mbs.push_back(m);
		block_bytes += it.block.size();
	}

	std::vector<uint8_t> bytes;
	ASSERT_EQ(EncodeMacroblockLayer(kVopTypeP, kMbWidth, mbs, &bytes), VideoErr::kOk);

	std::vector<Macroblock> back;
	ASSERT_EQ(DecodeMacroblockLayer(bytes.data(), bytes.size(), kVopTypeP, kMbWidth, mbs.size(), &back), VideoErr::kOk);
	ASSERT_EQ(back.size(), mbs.size());
	for (size_t i = 0; i < mbs.size(); ++i)
	{
		EXPECT_EQ(back[i].head.not_coded, mbs[i].head.not_coded) << "第 " << i << " 个宏块";
		if (! mbs[i].head.not_coded)
		{
			EXPECT_EQ(back[i].head.mb_type, mbs[i].head.mb_type) << "第 " << i << " 个宏块";
			EXPECT_EQ(back[i].head.cbpc, mbs[i].head.cbpc) << "第 " << i << " 个宏块";
			EXPECT_EQ(back[i].head.cbpy, mbs[i].head.cbpy) << "第 " << i << " 个宏块";
			EXPECT_EQ(back[i].head.dquant, mbs[i].head.dquant) << "第 " << i << " 个宏块";
			EXPECT_EQ(back[i].head.block_size, mbs[i].head.block_size) << "第 " << i << " 个宏块";
		}
		EXPECT_EQ(back[i].block, mbs[i].block) << "第 " << i << " 个宏块的块数据";
	}
	// 解出来的列表再编一次，字节必须一模一样
	std::vector<uint8_t> again;
	ASSERT_EQ(EncodeMacroblockLayer(kVopTypeP, kMbWidth, back, &again), VideoErr::kOk);
	EXPECT_EQ(again, bytes);
	ASSERT_GE(bytes.size(), block_bytes);
	std::cout << "一帧 " << mbs.size() << " 个宏块自定界闭环：落盘 " << bytes.size() << " B = 块数据 " << block_bytes
			  << " B + 宏块头与 block_size 及尾补齐共 " << (bytes.size() - block_bytes) << " B\n";
}

TEST(VideoMacroblock, LayerRejectsTrailingWholeBytes)
{
	std::vector<Macroblock> mbs(2);
	mbs[0].head.mb_type = kMbTypeIntra;
	mbs[0].head.block_size = 1u;
	mbs[0].block = { 0x7Eu };
	mbs[1].head.not_coded = true;
	std::vector<uint8_t> bytes;
	ASSERT_EQ(EncodeMacroblockLayer(kVopTypeP, kMbWidth, mbs, &bytes), VideoErr::kOk);
	bytes.push_back(0x00u);	   // 解满两个宏块后还剩整字节

	std::vector<Macroblock> back;
	EXPECT_EQ(DecodeMacroblockLayer(bytes.data(), bytes.size(), kVopTypeP, kMbWidth, 2u, &back), VideoErr::kHeaderBad);
}

TEST(VideoMacroblock, LayerRejectsShortSequence)
{
	std::vector<Macroblock> mbs(1);
	mbs[0].head.not_coded = true;
	std::vector<uint8_t> bytes;
	ASSERT_EQ(EncodeMacroblockLayer(kVopTypeP, kMbWidth, mbs, &bytes), VideoErr::kOk);

	std::vector<Macroblock> back;
	// 声明三个宏块却只有一个的料
	EXPECT_EQ(DecodeMacroblockLayer(bytes.data(), bytes.size(), kVopTypeP, kMbWidth, 3u, &back),
			  VideoErr::kPayloadTooSmall);
}

TEST(VideoMacroblock, BlockSizeOverrunIsPayloadTooSmall)
{
	// 头里写 block_size = 99，载荷只剩 2 字节
	MacroblockHeader h;
	h.mb_type = kMbTypeIntra;
	h.cbpc = 0u;
	h.cbpy = 0x0u;
	h.block_size = 99u;
	BitWriter bw;
	ASSERT_EQ(EncodeOne(h, kVopTypeP, &bw), VideoErr::kOk);
	const uint8_t two[2] = { 0x01u, 0x02u };
	bw.WriteBytes(two, 2u);
	bw.AlignToByte(0u);

	MacroblockHeader out;
	BitReader br(bw.Bytes().data(), bw.Bytes().size());
	EXPECT_EQ(DecodeOne(&br, kVopTypeP, &out), VideoErr::kPayloadTooSmall);
}

TEST(VideoMacroblock, BlockSizeVarintTruncatedIsPayloadTooSmall)
{
	// 只给 varint 的首字节（续位 1），后面什么都没有
	MacroblockHeader h;
	h.mb_type = kMbTypeIntra;
	h.block_size = 300u;	// > 127 → 两字节 varint
	BitWriter bw;
	ASSERT_EQ(EncodeOne(h, kVopTypeP, &bw), VideoErr::kOk);
	bw.AlignToByte(0u);
	std::vector<uint8_t> bytes = bw.Bytes();
	bytes.resize(static_cast<size_t>(bytes.size()) - 1u);	 // 砍掉 varint 的尾字节

	MacroblockHeader out;
	BitReader br(bytes.data(), bytes.size());
	EXPECT_EQ(DecodeOne(&br, kVopTypeP, &out), VideoErr::kPayloadTooSmall);
}

TEST(VideoBvop, MedianPredictorTakesMiddlePerComponent)
{
	const MotionVector a = MakeMv(4, -2), b = MakeMv(-1, 6), c = MakeMv(7, 6);
	const MotionVector p = MedianPredictor(a, b, c);
	EXPECT_EQ(p.h, 4);
	EXPECT_EQ(p.v, 6);
	// 无效候选由调用方置零后参与
	const MotionVector z = MakeMv(0, 0);
	const MotionVector p2 = MedianPredictor(b, z, z);
	EXPECT_EQ(p2.h, 0);
	EXPECT_EQ(p2.v, 0);
}

TEST(VideoBvop, MvdCountFollowsMbType)
{
	EXPECT_EQ(BvopMvdCount(kBvopDirect), 0u);
	EXPECT_EQ(BvopMvdCount(kBvopInterpolate), 2u);
	EXPECT_EQ(BvopMvdCount(kBvopBackward), 1u);
	EXPECT_EQ(BvopMvdCount(kBvopForward), 1u);
	EXPECT_EQ(BvopMvdCount(5u), 0u);
}

TEST(VideoBvop, AllFourTypesRoundTripThroughPredictor)
{
	const BvopPrediction pred { MakeMv(3, -5), MakeMv(-8, 11) };
	size_t checked = 0u;
	size_t cbpy_sum = 0u;
	size_t mvd_total = 0u;
	for (uint8_t mt = kBvopDirect; mt <= kBvopForward; ++mt)
	{
		BvopMacroblockHeader in;
		in.mb_type = mt;
		in.cbpy = static_cast<uint8_t>(3u + mt);
		in.block_size = 1u + mt;
		if (mt != kBvopDirect)
		{
			in.forward = MakeMv(pred.forward.h + 2, pred.forward.v - 7);
			in.backward = MakeMv(pred.backward.h - 1, pred.backward.v + 9);
		}

		BitWriter bw;
		ASSERT_EQ(EncodeBvopMacroblockHeader(&bw, in, pred), VideoErr::kOk);
		AppendBlockFiller(&bw, in.block_size);
		bw.AlignToByte(0u);

		BvopMacroblockHeader out;
		BitReader br(bw.Bytes().data(), bw.Bytes().size());
		ASSERT_EQ(clv::video::DecodeBvopMacroblockHeader(&br, &out, pred), VideoErr::kOk) << "mb_type " << int(mt);
		EXPECT_EQ(out.mb_type, in.mb_type);
		EXPECT_EQ(out.cbpy, in.cbpy);
		const size_t n = BvopMvdCount(mt);
		if (n >= 2u || mt == kBvopForward)
		{
			EXPECT_EQ(out.forward.h, in.forward.h);
			EXPECT_EQ(out.forward.v, in.forward.v);
		}
		if (n >= 2u || mt == kBvopBackward)
		{
			EXPECT_EQ(out.backward.h, in.backward.h);
			EXPECT_EQ(out.backward.v, in.backward.v);
		}
		if (mt == kBvopDirect)
		{
			EXPECT_EQ(out.forward.h, 0);
			EXPECT_EQ(out.forward.v, 0);
			EXPECT_EQ(out.backward.h, 0);
			EXPECT_EQ(out.backward.v, 0);
		}
		std::cout << "B-VOP mb_type " << int(mt) << "：落盘 " << bw.Bytes().size() << " B（mvd 个数 " << n << "）\n";
		++checked;
		// 把「解出来的宏块」累计成两条可核对的量，期望由算式给死（direct..forward = mb_type 0..3）
		cbpy_sum += out.cbpy;
		mvd_total += BvopMvdCount(out.mb_type);
	}
	// 自家计数器只承担「四型都走了」这一条，其余判据落在 SUT 输出的两个累计量上：
	//   cbpy 合计 = (3+0) + (3+1) + (3+2) + (3+3) = 3 + 4 + 5 + 6 = 18
	//   mvd 个数合计 = direct 0 + interpolate 2 + backward 1 + forward 1 = 4
	EXPECT_EQ(checked, 4u) << "四个 mb_type 都走了一遍";
	EXPECT_EQ(cbpy_sum, 18u);
	EXPECT_EQ(mvd_total, 4u);
}

TEST(VideoBvop, DirectCarriesNoResidualAndIgnoresMvFields)
{
	const BvopPrediction pred { MakeMv(3, -5), MakeMv(-8, 11) };
	BvopMacroblockHeader a;
	a.mb_type = kBvopDirect;
	a.cbpy = 5u;
	a.block_size = 3u;
	BvopMacroblockHeader b = a;
	b.forward = MakeMv(999, -999);
	b.backward = MakeMv(-1234, 4321);

	BitWriter x, y;
	ASSERT_EQ(EncodeBvopMacroblockHeader(&x, a, pred), VideoErr::kOk);
	ASSERT_EQ(EncodeBvopMacroblockHeader(&y, b, pred), VideoErr::kOk);
	const size_t head_bits = x.BitCount();
	AppendBlockFiller(&x, a.block_size);
	AppendBlockFiller(&y, b.block_size);
	x.AlignToByte(0u);
	y.AlignToByte(0u);
	// 两个绝对矢量字段不参与 direct 的码流：字节必须完全一样
	EXPECT_EQ(x.Bytes(), y.Bytes());
	EXPECT_EQ(head_bits, 32u);	  // not_coded + mb_type + cbpy + block_size，各 1 字节
}

TEST(VideoBvop, ReservedMbTypeIsRejectedOnBothSides)
{
	const BvopPrediction pred {};
	for (uint8_t mt: { uint8_t(5u), uint8_t(200u) })
	{
		BvopMacroblockHeader in;
		in.mb_type = mt;
		BitWriter bw;
		EXPECT_EQ(EncodeBvopMacroblockHeader(&bw, in, pred), VideoErr::kToolExcluded) << "mb_type " << int(mt);

		BitWriter raw;
		raw.Write(0u, 8u);
		raw.Write(mt, 8u);
		raw.Write(0u, 8u);
		raw.AlignToByte(0u);
		BvopMacroblockHeader out;
		BitReader br(raw.Bytes().data(), raw.Bytes().size());
		EXPECT_EQ(clv::video::DecodeBvopMacroblockHeader(&br, &out, pred), VideoErr::kToolExcluded)
			<< "读侧 mb_type " << int(mt);
	}
}

TEST(VideoBvop, NotCodedForcesZeroCbpy)
{
	const BvopPrediction pred {};
	BvopMacroblockHeader in;
	in.not_coded = true;
	in.cbpy = 3u;
	BitWriter bw;
	EXPECT_EQ(EncodeBvopMacroblockHeader(&bw, in, pred), VideoErr::kValueRange);

	BitWriter raw;
	raw.Write(1u, 8u);
	raw.Write(kBvopDirect, 8u);
	raw.Write(3u, 8u);
	raw.AlignToByte(0u);
	BvopMacroblockHeader out;
	BitReader br(raw.Bytes().data(), raw.Bytes().size());
	EXPECT_EQ(clv::video::DecodeBvopMacroblockHeader(&br, &out, pred), VideoErr::kHeaderBad);

	// 与 I / P 那条不同：这里的 not_coded = 1 只免掉块数据，mb_type / mvd / cbpy 三项照旧在场
	BvopMacroblockHeader skip;
	skip.not_coded = true;
	skip.mb_type = kBvopInterpolate;
	skip.cbpy = 0u;
	BitWriter keep;
	ASSERT_EQ(EncodeBvopMacroblockHeader(&keep, skip, pred), VideoErr::kOk);
	keep.AlignToByte(0u);
	EXPECT_EQ(keep.Bytes().size(), 2u + 4u + 1u);	 // 两个方向的残差各两分量 + cbpy，一段都没省
	BvopMacroblockHeader back;
	BitReader br2(keep.Bytes().data(), keep.Bytes().size());
	ASSERT_EQ(clv::video::DecodeBvopMacroblockHeader(&br2, &back, pred), VideoErr::kOk);
	EXPECT_TRUE(back.not_coded);
	EXPECT_EQ(back.mb_type, kBvopInterpolate);
	EXPECT_EQ(back.cbpy, 0u);
	std::cout << "B-VOP not_coded = 1 且 mb_type = interpolate：仍落 " << keep.Bytes().size() << " B，字段一个不跳\n";
}

TEST(VideoBvop, InterpolateResidualsAreRelativeToPredictor)
{
	const BvopPrediction pred { MakeMv(100, -100), MakeMv(-100, 100) };
	BvopMacroblockHeader in;
	in.mb_type = kBvopInterpolate;
	in.forward = MakeMv(101, -101);	   // 残差 (1, -1)
	in.backward = MakeMv(-99, 99);	   // 残差 (1, -1)
	in.cbpy = 0xFu;
	in.block_size = 4u;

	BitWriter bw;
	ASSERT_EQ(EncodeBvopMacroblockHeader(&bw, in, pred), VideoErr::kOk);
	// 残差小 → 每个分量 zigzag 后 1 字节：2 字节头 + 4 个残差 + cbpy + block_size = 8 B
	EXPECT_EQ(bw.BitCount(), 64u);
	AppendBlockFiller(&bw, in.block_size);
	bw.AlignToByte(0u);
	EXPECT_EQ(bw.Bytes().size(), 12u);

	BvopMacroblockHeader out;
	BitReader br(bw.Bytes().data(), bw.Bytes().size());
	ASSERT_EQ(clv::video::DecodeBvopMacroblockHeader(&br, &out, pred), VideoErr::kOk);
	EXPECT_EQ(out.forward.h, in.forward.h);
	EXPECT_EQ(out.forward.v, in.forward.v);
	EXPECT_EQ(out.backward.h, in.backward.h);
	EXPECT_EQ(out.backward.v, in.backward.v);

	// 同一份码流换一套预测器，解出的绝对矢量跟着变
	BvopMacroblockHeader shifted;
	BitReader br2(bw.Bytes().data(), bw.Bytes().size());
	const BvopPrediction other { MakeMv(7, 7), MakeMv(-7, -7) };
	ASSERT_EQ(clv::video::DecodeBvopMacroblockHeader(&br2, &shifted, other), VideoErr::kOk);
	EXPECT_EQ(shifted.forward.h, 8);
	EXPECT_EQ(shifted.forward.v, 6);
	EXPECT_EQ(shifted.backward.h, -6);
	EXPECT_EQ(shifted.backward.v, -8);
}

TEST(VideoBvop, CbpyIsStoredAsPatternByteNotAsIvopVlc)
{
	const BvopPrediction pred {};
	size_t checked = 0u;
	for (uint8_t pattern = 0u; pattern < 16u; ++pattern)
	{
		BvopMacroblockHeader in;
		in.mb_type = kBvopDirect;
		in.cbpy = pattern;
		in.block_size = 1u;
		BitWriter bw;
		ASSERT_EQ(EncodeBvopMacroblockHeader(&bw, in, pred), VideoErr::kOk);
		const size_t bits = bw.BitCount();
		AppendBlockFiller(&bw, in.block_size);
		bw.AlignToByte(0u);
		// 线上就是图样本身：not_coded 一字节 + mb_type 一字节 + 图样一字节
		ASSERT_EQ(bits, 32u) << "图样 " << static_cast<int>(pattern);
		EXPECT_EQ(bw.Bytes()[0], 0u);
		EXPECT_EQ(bw.Bytes()[1], kBvopDirect);
		EXPECT_EQ(bw.Bytes()[2], pattern);
		EXPECT_EQ(bw.Bytes()[3], 1u);	 // block_size = 1
		checked++;

		BvopMacroblockHeader out;
		BitReader br(bw.Bytes().data(), bw.Bytes().size());
		ASSERT_EQ(clv::video::DecodeBvopMacroblockHeader(&br, &out, pred), VideoErr::kOk);
		EXPECT_EQ(out.cbpy, pattern);

		// 与 I / P 那条路径可分辨：B-8 的码字长度随图样变，这里恒 8 bit
		const CbpyEntry* vlc = CbpyRowWithPattern(false, pattern);
		ASSERT_NE(vlc, nullptr);
		EXPECT_NE(vlc->code_len, 8u);
	}
	EXPECT_EQ(checked, 16u);
	std::cout << "B-VOP cbpy 直存核：16 个图样各落 1 字节且字节值 = 图样；同图样在 I / P 侧的 B-8 码长恒不等于 8 bit\n";
}

TEST(VideoBvop, BvopHasNoIvopMcbpcPath)
{
	MacroblockHeader in;
	in.mb_type = kMbTypeIntra;
	BitWriter bw;
	EXPECT_EQ(EncodeOne(in, kVopTypeB, &bw), VideoErr::kToolExcluded);

	const std::vector<uint8_t> junk = { 0x00u };
	MacroblockHeader out;
	EXPECT_EQ(DecodeBytes(junk, kVopTypeB, &out), VideoErr::kToolExcluded);
}

TEST(VideoPadding, GeometryCeilsToSixteen)
{
	FrameGeometry g;
	ASSERT_TRUE(clv::video::MakeFrameGeometry(30u, 22u, &g));
	EXPECT_EQ(g.coded_width, 32u);
	EXPECT_EQ(g.coded_height, 32u);

	ASSERT_TRUE(clv::video::MakeFrameGeometry(16u, 1u, &g));
	EXPECT_EQ(g.coded_width, 16u);
	EXPECT_EQ(g.coded_height, 16u);

	// 65535 → 65536：16 位容器会回绕成 0
	ASSERT_TRUE(clv::video::MakeFrameGeometry(65535u, 16u, &g));
	EXPECT_EQ(g.coded_width, 65536u);

	EXPECT_FALSE(clv::video::MakeFrameGeometry(32u, 32u, nullptr));
	EXPECT_FALSE(clv::video::MakeFrameGeometry(0u, 16u, &g));
	EXPECT_FALSE(clv::video::MakeFrameGeometry(16u, 0u, &g));
}

TEST(VideoPadding, FillsRightAndBottomWithBlackLevelsAndCropsBack)
{
	const uint16_t dw = 30u, dh = 22u;
	FrameGeometry g;
	ASSERT_TRUE(clv::video::MakeFrameGeometry(dw, dh, &g));
	const uint32_t cw = g.coded_width, ch = g.coded_height;
	const uint32_t ccw = cw / 2u, cch = ch / 2u;
	const uint32_t dcw = (dw + 1u) / 2u, dch = (dh + 1u) / 2u;

	std::vector<uint8_t> y(static_cast<size_t>(ch) * cw, 0x00u);
	std::vector<uint8_t> u(static_cast<size_t>(cch) * ccw, 0x00u);
	std::vector<uint8_t> v(static_cast<size_t>(cch) * ccw, 0xFFu);
	// 整帧先填成可辨识的非黑值，补边只许动右与下那一圈
	for (uint32_t r = 0; r < ch; ++r) std::memset(y.data() + static_cast<size_t>(r) * cw, 0x11u, cw);
	for (uint32_t r = 0; r < cch; ++r) std::memset(u.data() + static_cast<size_t>(r) * ccw, 0x22u, ccw);
	for (uint32_t r = 0; r < cch; ++r) std::memset(v.data() + static_cast<size_t>(r) * ccw, 0x33u, ccw);

	const PlaneBuf planes[3] = {
		{ y.data(), cw	},
		 { u.data(), ccw },
		   { v.data(), ccw }
	};
	ASSERT_EQ(clv::video::FillPadding(g, planes, 8u), VideoErr::kOk);

	size_t luma_black = 0u;
	for (uint32_t r = 0; r < ch; ++r)
	{
		for (uint32_t c = 0; c < cw; ++c)
		{
			const uint8_t got = y[static_cast<size_t>(r) * cw + c];
			if (r >= dh || c >= dw)
			{
				EXPECT_EQ(got, kBlackLuma8) << "luma 补边 (" << c << "," << r << ")";
				++luma_black;
			}
			else
			{
				EXPECT_EQ(got, 0x11u) << "luma 显示区 (" << c << "," << r << ")";
			}
		}
	}
	EXPECT_EQ(luma_black, static_cast<size_t>(ch) * cw - static_cast<size_t>(dh) * dw);

	size_t chroma_black = 0u;
	for (int p = 0; p < 2; ++p)
	{
		const std::vector<uint8_t>& plane = p == 0 ? u : v;
		const uint8_t keep = p == 0 ? 0x22u : 0x33u;
		for (uint32_t r = 0; r < cch; ++r)
		{
			for (uint32_t c = 0; c < ccw; ++c)
			{
				const uint8_t got = plane[static_cast<size_t>(r) * ccw + c];
				if (r >= dch || c >= dcw)
				{
					EXPECT_EQ(got, kBlackChroma8) << "chroma" << p << " 补边 (" << c << "," << r << ")";
					++chroma_black;
				}
				else
				{
					EXPECT_EQ(got, keep) << "chroma" << p << " 显示区 (" << c << "," << r << ")";
				}
			}
		}
	}
	EXPECT_EQ(chroma_black, 2u * (static_cast<size_t>(cch) * ccw - static_cast<size_t>(dch) * dcw));
	std::cout << "显示 " << dw << "×" << dh << " → 编码 " << cw << "×" << ch << "；luma 补边 " << luma_black
			  << " 点全 = " << static_cast<int>(kBlackLuma8) << "，chroma 补边 " << chroma_black
			  << " 点全 = " << static_cast<int>(kBlackChroma8) << "\n";

	std::vector<uint8_t> oy(static_cast<size_t>(dh) * dw, 0xEEu);
	std::vector<uint8_t> ou(static_cast<size_t>(dch) * dcw, 0xEEu);
	std::vector<uint8_t> ov(static_cast<size_t>(dch) * dcw, 0xEEu);
	const PlaneBuf dst[3] = {
		{ oy.data(), dw	 },
		  { ou.data(), dcw },
		 { ov.data(), dcw }
	};
	ASSERT_EQ(clv::video::CropToDisplay(g, planes, dst, 8u), VideoErr::kOk);

	for (uint32_t r = 0; r < dh; ++r)
		for (uint32_t c = 0; c < dw; ++c)
			EXPECT_EQ(oy[static_cast<size_t>(r) * dw + c], 0x11u) << "裁回 luma (" << c << "," << r << ")";
	for (uint32_t r = 0; r < dch; ++r)
	{
		for (uint32_t c = 0; c < dcw; ++c)
		{
			EXPECT_EQ(ou[static_cast<size_t>(r) * dcw + c], 0x22u);
			EXPECT_EQ(ov[static_cast<size_t>(r) * dcw + c], 0x33u);
		}
	}
}

TEST(VideoPadding, RejectsOtherBitDepthsAndUndersizedStrides)
{
	FrameGeometry g;
	ASSERT_TRUE(clv::video::MakeFrameGeometry(20u, 20u, &g));
	std::vector<uint8_t> y(static_cast<size_t>(g.coded_height) * g.coded_width, 0u);
	std::vector<uint8_t> c(static_cast<size_t>(g.coded_height / 2u) * (g.coded_width / 2u), 0u);
	const PlaneBuf planes[3] = {
		{ y.data(), g.coded_width	   },
		{ c.data(), g.coded_width / 2u },
		 { c.data(), g.coded_width / 2u }
	};

	EXPECT_EQ(clv::video::FillPadding(g, planes, 10u), VideoErr::kBitDepthUnsupported);
	EXPECT_EQ(clv::video::FillPadding(g, planes, 12u), VideoErr::kBitDepthUnsupported);
	EXPECT_EQ(clv::video::CropToDisplay(g, planes, planes, 12u), VideoErr::kBitDepthUnsupported);

	const PlaneBuf narrow[3] = {
		{ y.data(), g.coded_width - 1u },
		 { c.data(), g.coded_width / 2u },
		  { c.data(), g.coded_width / 2u }
	};
	EXPECT_EQ(clv::video::FillPadding(g, narrow, 8u), VideoErr::kInvalidArgument);

	std::vector<uint8_t> dst_y(static_cast<size_t>(g.display_height) * g.display_width, 0u);
	const PlaneBuf tight_dst[3] = {
		{ dst_y.data(), g.display_width - 1u },
		   { c.data(),	   g.coded_width / 2u	},
		{ c.data(),		g.coded_width / 2u	 }
	};
	EXPECT_EQ(clv::video::CropToDisplay(g, planes, tight_dst, 8u), VideoErr::kOutputTooSmall);
	EXPECT_EQ(clv::video::FillPadding(g, nullptr, 8u), VideoErr::kInvalidArgument);
}

TEST(VideoMacroblock, ZeroBlockPatternMakesEmptyBlockDataLegal)
{
	// 位图全零 ⇒ block_size = 0 合法；位图非零 ⇒ 判值域不成立
	MacroblockHeader mb;
	mb.mb_type = kMbTypeInter;
	mb.cbpc = 0u;
	mb.cbpy = 0u;
	mb.block_size = 0u;
	MacroblockHeader back;
	ASSERT_EQ(RoundTrip(kVopTypeP, mb, &back), VideoErr::kOk);
	EXPECT_EQ(back.cbpc, 0u);
	EXPECT_EQ(back.cbpy, 0u);
	EXPECT_EQ(back.block_size, 0u);

	mb.cbpy = 5u;
	BitWriter bad;
	EXPECT_EQ(EncodeOne(mb, kVopTypeP, &bad), VideoErr::kValueRange);
}

TEST(VideoBvop, ZeroBlockPatternAndSixBitCbpyBounds)
{
	const BvopPrediction pred {};
	// 位图全零：块数据长度 0 合法
	BvopMacroblockHeader in;
	in.mb_type = kBvopInterpolate;
	in.cbpy = 0u;
	in.block_size = 0u;
	BitWriter bw;
	ASSERT_EQ(EncodeBvopMacroblockHeader(&bw, in, pred), VideoErr::kOk);
	bw.AlignToByte(0u);
	BitReader br(bw.Bytes().data(), bw.Bytes().size());
	BvopMacroblockHeader back;
	ASSERT_EQ(DecodeBvopMacroblockHeader(&br, &back, pred), VideoErr::kOk);
	EXPECT_EQ(back.cbpy, 0u);
	EXPECT_EQ(back.block_size, 0u);

	// 位图非零而长度 0：拒
	in.cbpy = 3u;
	in.block_size = 0u;
	BitWriter bad;
	EXPECT_EQ(EncodeBvopMacroblockHeader(&bad, in, pred), VideoErr::kValueRange);

	// cbpy 是 6 bit 全位图：63 收、64 拒
	in.cbpy = 63u;
	in.block_size = 1u;
	BitWriter wide;
	EXPECT_EQ(EncodeBvopMacroblockHeader(&wide, in, pred), VideoErr::kOk);
	in.cbpy = 64u;
	BitWriter over;
	EXPECT_EQ(EncodeBvopMacroblockHeader(&over, in, pred), VideoErr::kValueRange);

	std::cout << "位图全零允许零长度块数据；";
	std::cout << "B-VOP cbpy 现按 6 bit（0..63）判\n";
}

