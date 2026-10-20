/* test_video_slice_predictor.cpp
 *
 * **实现层用例**：直接 include 实现层头，不经公开 C ABI，仅静态构建下可链。
 *
 * 分片边界预测器重置（运动矢量一侧）与块层「没有帧内预测状态」两条。
 * 载体事实（逐处对码实测）：
 *   · I / P 宏块头里带的是**相对三候选中值预测器的残差**（两条 zigzag varint），绝对矢量不落码流；
 *     帧内宏块与 not_coded 宏块不带该字段。
 *   · 预测器状态在每次层函数调用内新建（多片路径逐片调用 = 片界重置），候选按 `mb_width` 取
 *     左 / 上 / 右上三个位置，**下标是本次调用内的相对下标**，不是帧内绝对宏块号。
 *   · 无效候选（越界、缺位、邻居是帧内 / 不带该方向）按零矢量参与中值。
 *     本实现的口径记在这里：走「无效候选置零后取三候选中值」，两个候选无效时中值恒为 0；
 *     分片边界预测器那条「仅两个候选无效时取第三个候选」的写法本库不取，两者在顶行给出不同的值。
 *     顶行宏块（上与右上都在帧外）因此一律拿到零预测器，残差 = 绝对矢量本身，本件按这个值断。
 *   · 块层系数载体定宽逐系数写入，入口只吃本块自己的 64 个系数，不吃位置、不吃邻块、也不吃前一块的 DC ——
 *     帧内 DC / AC 预测在本实现里不存在，故规范里「边界外 DC 取默认值」「边界外 AC 系数置零」两条在 v1 无作用对象。
 *
 * 断言写法：每个位置的预测器都用「显式重放」钉死 —— 拿同一批宏块头、按人算出的预测器手工写一遍，
 * 与层函数输出逐字节相等才算数；只比字节大小不算证据。
 */

#include "video/bits.h"
#include "video/block.h"
#include "video/macroblock.h"
#include "video/status.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{

	using clv::video::BitReader;
	using clv::video::BitWriter;
	using clv::video::BvopMacroblock;
	using clv::video::BvopMacroblockHeader;
	using clv::video::BvopPrediction;
	using clv::video::CoefCoding;
	using clv::video::DecodeBvopRun;
	using clv::video::DecodeMacroblockHeader;
	using clv::video::DecodeMacroblockLayer;
	using clv::video::DecodeMacroblockRun;
	using clv::video::EncodeBlock;
	using clv::video::EncodeBlockData;
	using clv::video::EncodeBvopLayer;
	using clv::video::EncodeBvopMacroblockHeader;
	using clv::video::EncodeMacroblockHeader;
	using clv::video::EncodeMacroblockLayer;
	using clv::video::kBlockCoefs;
	using clv::video::kBlocksPerMacroblock;
	using clv::video::kBvopForward;
	using clv::video::kBvopInterpolate;
	using clv::video::kMbTypeInter;
	using clv::video::kMbTypeIntra;
	using clv::video::kVopTypeP;
	using clv::video::Macroblock;
	using clv::video::MacroblockHeader;
	using clv::video::MedianPredictor;
	using clv::video::MotionVector;
	using clv::video::VideoErr;

	inline constexpr uint8_t kQ = 8u;

	MotionVector MV(int32_t h, int32_t v)
	{
		MotionVector m;
		m.h = h;
		m.v = v;
		return m;
	}

	/* 只有头、无块数据的宏块：cbpy = 0 与 block_size = 0 是配套形状，本件只谈运动矢量预测器。 */
	Macroblock InterMb(int32_t h, int32_t v)
	{
		Macroblock m;
		m.head.mb_type = kMbTypeInter;
		m.head.cbpc = 0u;
		m.head.cbpy = 0u;
		m.head.dquant = 0;
		m.head.block_size = 0u;
		m.head.mv = MV(h, v);
		return m;
	}

	Macroblock IntraMb()
	{
		Macroblock m = InterMb(0, 0);
		m.head.mb_type = kMbTypeIntra;
		return m;
	}

	BvopMacroblock BvopForwardOnly(int32_t h, int32_t v)
	{
		BvopMacroblock m;
		m.head.mb_type = kBvopForward;
		m.head.cbpy = 0u;
		m.head.block_size = 0u;
		m.head.forward = MV(h, v);
		return m;
	}

	BvopMacroblock BvopBoth(int32_t fh, int32_t fv, int32_t bh, int32_t bv)
	{
		BvopMacroblock m;
		m.head.mb_type = kBvopInterpolate;
		m.head.cbpy = 0u;
		m.head.block_size = 0u;
		m.head.forward = MV(fh, fv);
		m.head.backward = MV(bh, bv);
		return m;
	}

	/* 手工重放：按给定的预测器序列逐个写头，末尾对齐。
	 * 层函数的输出与这份重放逐字节相等，才说明层函数在每个位置上用的就是这里写的那份预测器。 */
	std::vector<uint8_t> ReplayIp(const std::vector<Macroblock>& mbs, const std::vector<MotionVector>& preds)
	{
		BitWriter bw;
		for (size_t i = 0u; i < mbs.size(); ++i)
			if (EncodeMacroblockHeader(&bw, kVopTypeP, mbs[i].head, preds[i]) != VideoErr::kOk) return {};
		bw.AlignToByte(0u);
		return bw.Bytes();
	}

	std::vector<uint8_t> ReplayBvop(const std::vector<BvopMacroblock>& mbs, const std::vector<BvopPrediction>& preds)
	{
		BitWriter bw;
		for (size_t i = 0u; i < mbs.size(); ++i)
			if (EncodeBvopMacroblockHeader(&bw, mbs[i].head, preds[i]) != VideoErr::kOk) return {};
		bw.AlignToByte(0u);
		return bw.Bytes();
	}

	std::vector<uint8_t> EncodeLayer(const std::vector<Macroblock>& mbs, uint32_t mb_width)
	{
		std::vector<uint8_t> out;
		if (EncodeMacroblockLayer(kVopTypeP, mb_width, mbs, &out) != VideoErr::kOk) return {};
		return out;
	}

	/* 一次调度解 count 个宏块（不做「整层用完」的收尾校验，适合逐片路径）。 */
	std::vector<Macroblock> DecodeRun(const std::vector<uint8_t>& bytes, uint32_t mb_width, uint64_t count)
	{
		std::vector<Macroblock> out;
		if (DecodeMacroblockRun(bytes.data(), bytes.size(), kVopTypeP, mb_width, count, &out, nullptr) != VideoErr::kOk)
			return {};
		return out;
	}

	int32_t Zig(int32_t r) noexcept { return r >= 0 ? 2 * r : -2 * r - 1; }

	/* zigzag + LEB128 的字节宽：|r| ≤ 63 时 zig ≤ 126 → 1 字节，|r| ≥ 64 时 zig ≥ 128 → 2 字节。 */
	int VarintBytes(int32_t r) noexcept { return Zig(r) < 128 ? 1 : 2; }

	void ExpectMbsEqual(const std::vector<Macroblock>& got, const std::vector<Macroblock>& want, const char* tag)
	{
		ASSERT_EQ(got.size(), want.size()) << tag;
		for (size_t i = 0u; i < want.size(); ++i)
		{
			EXPECT_EQ(got[i].head.mv.h, want[i].head.mv.h) << tag << " 第 " << i << " 格 h";
			EXPECT_EQ(got[i].head.mv.v, want[i].head.mv.v) << tag << " 第 " << i << " 格 v";
		}
	}

}	 // namespace

/* 三候选中值：逐分量取中值，两分量互不牵扯。 */
TEST(SlicePredictor, MedianOfThreeCandidatesIsPerComponent)
{
	// 分量各自排序取中：h 取 (0,4,9) 的 4，v 取 (0,8,2) 的 2 —— 两分量互不借对方的序
	EXPECT_EQ(MedianPredictor(MV(0, 0), MV(4, 8), MV(9, 2)).h, 4);
	EXPECT_EQ(MedianPredictor(MV(0, 0), MV(4, 8), MV(9, 2)).v, 2);
	EXPECT_EQ(MedianPredictor(MV(-6, 3), MV(0, 0), MV(7, -5)).h, 0);
	EXPECT_EQ(MedianPredictor(MV(-6, 3), MV(0, 0), MV(7, -5)).v, 0);
	// 两条零 + 一条大值 ⇒ 中值仍是零（这一档正是与「取第三个候选」分歧的落点）
	EXPECT_EQ(MedianPredictor(MV(0, 0), MV(0, 0), MV(120, 120)).h, 0);
	EXPECT_EQ(MedianPredictor(MV(0, 0), MV(0, 0), MV(120, 120)).v, 0);
	EXPECT_EQ(MedianPredictor(MV(-100, 500), MV(-1, -1), MV(0, 0)).h, -1);
	EXPECT_EQ(MedianPredictor(MV(-100, 500), MV(-1, -1), MV(0, 0)).v, 0);
	EXPECT_EQ(MedianPredictor(MV(64, 64), MV(64, 64), MV(0, 0)).h, 64);
	// 负值参与时同样逐分量取中：(-2,1,-1) 取 -1，(3,5,7) 取 5
	EXPECT_EQ(MedianPredictor(MV(-2, 3), MV(1, 5), MV(-1, 7)).h, -1);
	EXPECT_EQ(MedianPredictor(MV(-2, 3), MV(1, 5), MV(-1, 7)).v, 5);
}

/* 载体是残差不是绝对矢量：同一绝对矢量在不同预测器下位数不同；
 * 用同一个预测器解回来才拿得回原值，换错预测器就解出另一个绝对矢量。 */
TEST(SlicePredictor, MacroblockHeaderCarriesResidualAgainstPredictorNotAbsoluteVector)
{
	const MacroblockHeader head = InterMb(127, 127).head;

	BitWriter w_zero, w_near;
	ASSERT_EQ(EncodeMacroblockHeader(&w_zero, kVopTypeP, head, MV(0, 0)), VideoErr::kOk);
	ASSERT_EQ(EncodeMacroblockHeader(&w_near, kVopTypeP, head, MV(64, 64)), VideoErr::kOk);
	// 残差 (127,127) 两条各 2 字节、残差 (63,63) 两条各 1 字节：位数差 = 2 × 8
	// （一条两字节的 varint 占 16 bit：首字节 7 个数据位 + 1 个延续位，末字节 8 个数据位）
	EXPECT_EQ(VarintBytes(127), 2);
	EXPECT_EQ(VarintBytes(63), 1);
	EXPECT_EQ(w_zero.BitCount() - w_near.BitCount(), 16u);

	const MotionVector preds[3] = { MV(0, 0), MV(63, 63), MV(64, 64) };
	for (const MotionVector& pred: preds)
	{
		BitWriter w;
		ASSERT_EQ(EncodeMacroblockHeader(&w, kVopTypeP, head, pred), VideoErr::kOk);
		w.AlignToByte(0u);
		const std::vector<uint8_t> bytes = w.Bytes();
		BitReader br(bytes.data(), bytes.size());
		MacroblockHeader got;
		ASSERT_EQ(DecodeMacroblockHeader(&br, kVopTypeP, &got, pred), VideoErr::kOk);
		EXPECT_EQ(got.mv.h, 127) << "预测器 (" << pred.h << "," << pred.v << ")";
		EXPECT_EQ(got.mv.v, 127);
	}

	// 预测器错一档 = 解出另一个绝对矢量，码流里没有绝对值可查：残差是 127−63 = 64，零预测器下读回 64
	BitWriter w;
	ASSERT_EQ(EncodeMacroblockHeader(&w, kVopTypeP, head, MV(63, 63)), VideoErr::kOk);
	w.AlignToByte(0u);
	const std::vector<uint8_t> bytes = w.Bytes();
	BitReader br(bytes.data(), bytes.size());
	MacroblockHeader got;
	ASSERT_EQ(DecodeMacroblockHeader(&br, kVopTypeP, &got, MV(0, 0)), VideoErr::kOk);
	EXPECT_EQ(got.mv.h, 64);
	EXPECT_EQ(got.mv.v, 64);
}

/* 三候选全不在场（单宏块调度）⇒ 预测器置零。层输出与「显式零预测器手工重放」逐字节相等。 */
TEST(SlicePredictor, SingleMacroblockRunUsesZeroPredictor)
{
	const std::vector<Macroblock> mbs = { InterMb(127, 127) };
	const std::vector<uint8_t> layer = EncodeLayer(mbs, 4u);
	ASSERT_FALSE(layer.empty());
	EXPECT_EQ(layer, ReplayIp(mbs, { MV(0, 0) }));

	std::vector<Macroblock> back;
	ASSERT_EQ(DecodeMacroblockLayer(layer.data(), layer.size(), kVopTypeP, 4u, 1u, &back), VideoErr::kOk);
	ExpectMbsEqual(back, mbs, "单格整层");
}

/* 顶行第二格：左候选有效、上与右上都在帧外，实现按「无效候选置零」取中值 ⇒ 预测器仍是零。
 * 若「仅两候选无效取第三个候选」那一条生效，这里应拿到 (127,127)，字节会短一截 —— 现码不是那样。 */
TEST(SlicePredictor, TopRowSecondMacroblockStillGetsZeroPredictor)
{
	const std::vector<Macroblock> mbs = { InterMb(127, 127), InterMb(127, 127) };
	const std::vector<uint8_t> layer = EncodeLayer(mbs, 4u);
	ASSERT_FALSE(layer.empty());
	EXPECT_EQ(layer, ReplayIp(mbs, { MV(0, 0), MV(0, 0) }));
	const std::vector<uint8_t> if_rule_three_applied = ReplayIp(mbs, { MV(0, 0), MV(127, 127) });
	ASSERT_FALSE(if_rule_three_applied.empty());
	EXPECT_NE(layer, if_rule_three_applied);
	// 少掉的那两条残差各占 1 字节（zig 0），现码比它长
	EXPECT_GT(layer.size(), if_rule_three_applied.size());
	std::cout << "[顶行第二格] 现码（预测器零）= " << layer.size() << " B，"
			  << "若按「两候选无效取第三候选」（预测器 (127,127)）= " << if_rule_three_applied.size() << " B\n";
	// 同一格的位数差（去掉层对齐的整数化影响）：现码残差 (127,127) vs 规则三那档残差 (0,0)
	BitWriter bits_now, bits_rule3;
	ASSERT_EQ(EncodeMacroblockHeader(&bits_now, kVopTypeP, mbs[1].head, MV(0, 0)), VideoErr::kOk);
	ASSERT_EQ(EncodeMacroblockHeader(&bits_rule3, kVopTypeP, mbs[1].head, MV(127, 127)), VideoErr::kOk);
	std::cout << "[顶行第二格·位数] 现码 " << bits_now.BitCount() << " bit，规则三那档 " << bits_rule3.BitCount()
			  << " bit，差 " << bits_now.BitCount() - bits_rule3.BitCount() << " bit\n";
	ExpectMbsEqual(DecodeRun(layer, 4u, 2u), mbs, "顶行两格");
}

/* 第二行首列：左候选越界（无效），上与右上在位 —— 恰好一个无效，规范与实现这一档一致（置零后取中值）。
 * 上邻居换成帧内（不带矢量 ⇒ 也无效）后两个候选都无效，预测器落回零，同一绝对矢量的残差随之变宽。 */
TEST(SlicePredictor, IntraNeighbourCountsAsInvalidCandidate)
{
	// mb_width = 2：下标 0、1 在第一行，下标 2 在第二行首列
	const std::vector<Macroblock> with_inter = { InterMb(64, 64), InterMb(200, 200), InterMb(127, 127) };
	const std::vector<uint8_t> bytes_inter = EncodeLayer(with_inter, 2u);
	ASSERT_FALSE(bytes_inter.empty());
	EXPECT_EQ(bytes_inter,
			  ReplayIp(with_inter, { MV(0, 0), MV(0, 0), MedianPredictor(MV(0, 0), MV(64, 64), MV(200, 200)) }));
	// 一个候选无效这一档：中值取 (64,64)，残差 (63,63) 每条一字节
	EXPECT_EQ(MedianPredictor(MV(0, 0), MV(64, 64), MV(200, 200)).h, 64);
	EXPECT_EQ(VarintBytes(127 - 64), 1);

	const std::vector<Macroblock> with_intra = { IntraMb(), InterMb(200, 200), InterMb(127, 127) };
	const std::vector<uint8_t> bytes_intra = EncodeLayer(with_intra, 2u);
	ASSERT_FALSE(bytes_intra.empty());
	// 帧内邻居不承载矢量，按无效置零参与：两个候选都无效 ⇒ 预测器零，残差 (127,127) 每条两字节
	EXPECT_EQ(bytes_intra, ReplayIp(with_intra, { MV(0, 0), MV(0, 0), MV(0, 0) }));
	EXPECT_EQ(VarintBytes(127), 2);
	// 整层字节不比大小：换成帧内后第一格自己少了两条残差，层宽的净变化不是单变量。
	// 预测器取值由上面两份逐字节重放钉死，残差宽度由 VarintBytes 两行钉死。

	ExpectMbsEqual(DecodeRun(bytes_inter, 2u, 3u), with_inter, "帧间上邻居");
	std::vector<Macroblock> want_intra = with_intra;
	want_intra[0].head.mb_type = kMbTypeIntra;
	ExpectMbsEqual(DecodeRun(bytes_intra, 2u, 3u), want_intra, "帧内上邻居");
}

/* 片界重置：状态每次调用新建。同样四个宏块，整条调度写与切成两段写出的字节不同 ——
 * 片切分是码流的一部分，编解两侧必须同切。 */
TEST(SlicePredictor, RunStateIsCreatedPerCallSoPartitioningMustMatch)
{
	const std::vector<Macroblock> all = { InterMb(200, 200), InterMb(100, 100), InterMb(127, 127), InterMb(50, 50) };
	const std::vector<uint8_t> whole = EncodeLayer(all, 2u);
	ASSERT_FALSE(whole.empty());
	// 整条调度：下标 2 = 第二行首列（左越界、上=第 0 格、右上=第 1 格），下标 3 = 第二行第二列
	const MotionVector p2 = MedianPredictor(MV(0, 0), MV(200, 200), MV(100, 100));
	const MotionVector p3 = MedianPredictor(MV(127, 127), MV(100, 100), MV(0, 0));
	ASSERT_EQ(p2.h, 100);
	ASSERT_EQ(p3.h, 100);
	EXPECT_EQ(whole, ReplayIp(all, { MV(0, 0), MV(0, 0), p2, p3 }));
	ExpectMbsEqual(DecodeRun(whole, 2u, 4u), all, "整条调度");

	// 切成两片：每片各自新建状态、片内下标从 0 起 ⇒ 两片的第一个宏块都拿到零预测器
	const std::vector<Macroblock> s0 = { all[0], all[1] };
	const std::vector<Macroblock> s1 = { all[2], all[3] };
	const std::vector<uint8_t> b0 = EncodeLayer(s0, 2u);
	const std::vector<uint8_t> b1 = EncodeLayer(s1, 2u);
	ASSERT_FALSE(b0.empty());
	ASSERT_FALSE(b1.empty());
	EXPECT_EQ(b0, ReplayIp(s0, { MV(0, 0), MV(0, 0) }));
	EXPECT_EQ(b1, ReplayIp(s1, { MV(0, 0), MV(0, 0) }));

	std::vector<uint8_t> joined = b0;
	joined.insert(joined.end(), b1.begin(), b1.end());
	EXPECT_NE(joined, whole);
	// 第二片按本片自己的状态解，绝对矢量全部还原
	ExpectMbsEqual(DecodeRun(b0, 2u, 2u), s0, "第一片");
	ExpectMbsEqual(DecodeRun(b1, 2u, 2u), s1, "第二片");
	// 拿整条调度的走法去读两片的字节
	const std::vector<Macroblock> crossed = DecodeRun(joined, 2u, 4u);
	/* 判据只能是「读不回东西」：DecodeRun 在解析不成立时返回空表（见其体内的 `return {};`），
	 * 「凑够四格才判第三格的值」这种条件断言在空表出口下永不执行——用例照绿，什么都没判，所以不用。
	 * 本例钉的是实测行为：整条调度的走法读两片拼接的字节，第三格落在第二片自己的零预测器上，
	 * 绝对矢量还原不出来，四格的量根本读不满，解析直接不成立。
	 * 这条一旦变红（读回东西了），就得按新行为把逐格的值判据补回来。 */
	EXPECT_TRUE(crossed.empty()) << "错位读法读回了 " << crossed.size() << " 格：本例前提变了，逐值判据要补回来";
	EXPECT_NE(crossed.size(), 4u) << "整条调度走法不该能把两片字节读成四格";
}

/* B-VOP 一侧的方向性：只带前向的邻居在后向那套候选里无效，按零参与。
 * 同一组邻居给前向与后向算出的预测器不同，逐字节重放把两个值都钉死。 */
TEST(SlicePredictor, BvopNeighbourWithoutThatDirectionIsInvalid)
{
	const std::vector<BvopMacroblock> mbs = { BvopForwardOnly(200, 200), BvopForwardOnly(100, 100),
											  BvopBoth(127, 127, 127, 127) };
	std::vector<uint8_t> out;
	ASSERT_EQ(EncodeBvopLayer(2u, mbs, &out), VideoErr::kOk);
	ASSERT_FALSE(out.empty());

	BvopPrediction zero_zero;
	zero_zero.forward = MV(0, 0);
	zero_zero.backward = MV(0, 0);
	// 下标 2（第二行首列）：左候选越界；上 = 第 0 格、右上 = 第 1 格，两格都只带前向
	BvopPrediction p2;
	p2.forward = MedianPredictor(MV(0, 0), MV(200, 200), MV(100, 100));
	p2.backward = MedianPredictor(MV(0, 0), MV(0, 0), MV(0, 0));
	ASSERT_EQ(p2.forward.h, 100);
	ASSERT_EQ(p2.backward.h, 0);

	std::vector<uint8_t> replay = ReplayBvop(mbs, { zero_zero, zero_zero, p2 });
	ASSERT_FALSE(replay.empty());
	EXPECT_EQ(out, replay);

	std::vector<BvopMacroblock> back;
	ASSERT_EQ(DecodeBvopRun(out.data(), out.size(), 2u, 3u, &back, nullptr), VideoErr::kOk);
	ASSERT_EQ(back.size(), 3u);
	for (size_t i = 0u; i < 3u; ++i)
	{
		EXPECT_EQ(back[i].head.forward.h, mbs[i].head.forward.h) << "前向第 " << i << " 格";
		EXPECT_EQ(back[i].head.backward.h, mbs[i].head.backward.h) << "后向第 " << i << " 格";
	}
}

/* 块层没有帧内预测状态：同一组系数放在块位 0（亮度块 0）与块位 5（Cr）写出的字节完全相同，
 * 单块入口也不吃位置。分片边界对块层没有可重置的东西 ——
 * 规范里「边界外 DC 取默认值」「边界外 AC 系数置零」两条在 v1 没有作用对象。 */
TEST(SlicePredictor, BlockLayerHasNoPositionOrNeighbourDependentState)
{
	int32_t at_first[kBlocksPerMacroblock][kBlockCoefs];
	int32_t at_last[kBlocksPerMacroblock][kBlockCoefs];
	std::memset(at_first, 0, sizeof(at_first));
	std::memset(at_last, 0, sizeof(at_last));
	for (int i = 0; i < kBlockCoefs; ++i)
	{
		at_first[0][i] = (i % 7) - 3;
		at_last[5][i] = (i % 7) - 3;
	}

	std::vector<uint8_t> first_bytes, last_bytes;
	// 块模式位序 bit5..bit0 = 块 0..5：0x20 只含块 0，0x01 只含块 5
	ASSERT_EQ(EncodeBlockData(0x20u, at_first, kQ, true, CoefCoding::kFixed, &first_bytes), VideoErr::kOk);
	ASSERT_EQ(EncodeBlockData(0x01u, at_last, kQ, true, CoefCoding::kFixed, &last_bytes), VideoErr::kOk);
	ASSERT_FALSE(first_bytes.empty());
	EXPECT_EQ(first_bytes, last_bytes);

	BitWriter w1, w2;
	EncodeBlock(at_first[0], kQ, true, CoefCoding::kFixed, &w1);
	EncodeBlock(at_last[5], kQ, true, CoefCoding::kFixed, &w2);
	EXPECT_EQ(w1.BitCount(), w2.BitCount());
	w1.AlignToByte(0u);
	w2.AlignToByte(0u);
	EXPECT_EQ(w1.Bytes(), w2.Bytes());

	// 同一组系数换了 DC 的位置（DC 是下标 0 那个系数），载体按自然行序逐系数写，不认「前一块的 DC」
	std::vector<uint8_t> moved;
	int32_t shifted[kBlockCoefs];
	std::memcpy(shifted, at_first[0], sizeof(shifted));
	const int32_t dc = shifted[0];
	shifted[0] = shifted[1];
	shifted[1] = dc;
	BitWriter w3;
	EncodeBlock(shifted, kQ, true, CoefCoding::kFixed, &w3);
	w3.AlignToByte(0u);
	moved = w3.Bytes();
	EXPECT_NE(moved, w1.Bytes());
}
