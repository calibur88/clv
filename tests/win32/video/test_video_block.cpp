/* test_video_block.cpp
 *
 * **两面混用**：端到端与句柄行为走公开 C ABI（`CLV_Video*` 族 12 个入口、实测 36 处调用，如 `CLV_VideoBitstreamGetBytes`、`CLV_VideoBitstreamRelease`、`CLV_VideoDecoderClose`），
 *   单元级直测走实现层头（`video/block.h`、`video/encoder_hook.h`、`video/quant.h`、`video/reconstruct.h`、`video/status.h`）。**仅静态构建（`CLV_BUILD_SHARED=OFF`）下可链**的原因是后面这些头，
 *   不是「不经公开 ABI」——两面各写各的：主体过公开 C ABI，另有几处直调实现层函数钉住不进 ABI 的行为。
 *
 * 块系数载体（定宽那一法）的单元面，以及编解逐字节全等 / 差异非零 / 误差单调三条的落点。
 * 本文件的载体例显式传 CoefCoding::kFixed，把各自的字节算式钉在法 0 上；
 * 变长那一法与两法共有的往返 / 上界 / 拒绝面在 test_video_block_coef.cpp。
 *
 * 语法往返在 test_video_macroblock.cpp / test_video_payload.cpp 的载荷层；本文件的块载体单元面与重建断言
 * 与它们并存，各测一层：载荷级搬运、编解逐字节全等、差异非零、误差单调互不覆盖。
 *
 * 编解逐字节全等 要「编码器内部重建帧」，公开面无此出口，走实现层钩子 `clv_internal_encoder_reconstruction`
 * （见 src/video/encoder_hook.h：不新增导出函数、不动 ABI）。
 *
 * 素材：本文件用内联生成的确定性高频图样，期望值由同一条量化 / 反量化链路现算，不预先写死。
 * 冻结字节的 `.clv` 素材及其期望值在 `assets/README.md` 里逐文件列出。
 */

#include "CLV_Video.h"
#include "video/block.h"
#include "video/encoder_hook.h"
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

	using clv::video::BlockBits;
	using clv::video::BlockBytes;
	using clv::video::BlockQfMax;
	using clv::video::CoefCoding;
	using clv::video::DecodeBlockData;
	using clv::video::EncodeBlockData;
	using clv::video::kBlockCoefs;
	using clv::video::kBlocksPerMacroblock;
	using clv::video::RoundResidual;
	using clv::video::VideoErr;

	/* 确定性高频图样：逐像素变化的伪随机面（不是纯翻转面 —— 纯 checkerboard 恰是 Hadamard 的
	 * 高频基向量，只有一个系数非零，反量化误差可能恰好被舍入吃掉，证不出「差异非零」）。 */
	void WriteHighFrequency(CLV_VideoFrame* f, uint32_t w, uint32_t h)
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

	/* 平块图样：亮度整面同一电平、色度整面 128（残差恒零 ⇒ 色度块不在场）。
	 * 帧内预测电平恒 128 ⇒ 亮度残差 = level − 128，平块的变换系数只有 DC 一支 = 64 × 残差。 */
	void WriteFlatLuma(CLV_VideoFrame* f, uint32_t w, uint32_t h, uint8_t level)
	{
		CLV_VideoFrameDescC desc;
		std::memset(&desc, 0, sizeof(desc));
		if (CLV_VideoFrameGetDesc(f, &desc, nullptr) != CLV_VIDEO_OK) return;
		const uint32_t pw[3] = { w, w / 2u, w / 2u };
		const uint32_t ph[3] = { h, h / 2u, h / 2u };
		const uint8_t fill[3] = { level, 128u, 128u };
		for (uint32_t pl = 0u; pl < 3u; ++pl)
			for (uint32_t r = 0u; r < ph[pl]; ++r)
				std::memset(desc.plane[pl] + static_cast<size_t>(r) * desc.stride[pl], fill[pl], pw[pl]);
	}

	int OpenEncoder(CLV_VideoEncoder** out, uint32_t w, uint32_t h, uint8_t q)
	{
		CLV_VideoEncParamsC p;
		std::memset(&p, 0, sizeof(p));
		p.display_width = static_cast<uint16_t>(w);
		p.display_height = static_cast<uint16_t>(h);
		p.bit_depth = 8u;
		p.quant_precision = 7u;
		p.default_quant = q;
		CLV_VideoError err = CLV_VIDEO_OK;
		*out = CLV_VideoEncoderOpen(&p, &err);
		return static_cast<int>(err);
	}

	int OpenDecoder(CLV_VideoDecoder** out, uint32_t w, uint32_t h)
	{
		CLV_VideoStreamParamsC p;
		std::memset(&p, 0, sizeof(p));
		p.bit_depth = 8u;
		p.quant_precision = 7u;
		p.max_b_frames = 0u;
		p.obmc_disable = 1u;
		p.display_width = static_cast<uint16_t>(w);
		p.display_height = static_cast<uint16_t>(h);
		CLV_VideoError err = CLV_VIDEO_OK;
		*out = CLV_VideoDecoderOpen(&p, &err);
		return static_cast<int>(err);
	}

	/* 有效区里逐字节相等的点数（只比有效区，不比补边列）。 */
	size_t CountSame(CLV_VideoFrame& a, CLV_VideoFrame& b, uint32_t w, uint32_t h)
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

	/* 有效区里的像素总数（Y 全幅 + 两色度各半幅）。 */
	size_t ValidCount(uint32_t w, uint32_t h) { return static_cast<size_t>(w) * h + 2u * (w / 2u) * (h / 2u); }

	/* 有效区里的总绝对误差（差异非零与误差单调都用它作度量）。 */
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
				{
					const int d = static_cast<int>(pa[c]) - static_cast<int>(pb[c]);
					sum += static_cast<size_t>(d < 0 ? -d : d);
				}
			}
		}
		return sum;
	}

	/* 编一帧 I 并把解码输出取出来（单片、max_b_frames = 0 故要 Flush）。
	 * rec_out 若不是 nullptr，带出编码器内部重建帧（只读借用）。 */
	bool EncodeDecodeOneI(CLV_VideoEncoder* e, CLV_VideoDecoder* d, CLV_VideoFrame* src, uint8_t q,
						  CLV_VideoFrame** out_frame, CLV_VideoFrame** rec_out, size_t* payload_bytes)
	{
		CLV_VideoBitstream* bs = nullptr;
		if (CLV_VideoEncoderEncode(e, src, CLV_VIDEO_FRAME_I, q, 0, 0, &bs) != CLV_VIDEO_OK) return false;
		const uint8_t* data = nullptr;
		size_t size = 0u;
		if (CLV_VideoBitstreamGetBytes(bs, &data, &size) != CLV_VIDEO_OK)
		{
			CLV_VideoBitstreamRelease(bs);
			return false;
		}
		std::vector<uint8_t> whole(data, data + size);
		if (payload_bytes != nullptr) *payload_bytes = size;
		CLV_VideoBitstreamRelease(bs);
		if (CLV_VideoDecoderDecode(d, whole.data(), whole.size(), 1u, 0, 0, nullptr) != CLV_VIDEO_OK) return false;
		CLV_VideoFrame* got = nullptr;
		uint8_t flag = 0u;
		if (CLV_VideoDecoderFlush(d, &got, &flag) != CLV_VIDEO_OK || flag != 1u) return false;
		if (rec_out != nullptr) *rec_out = clv_internal_encoder_reconstruction(e);
		*out_frame = got;
		return true;
	}

}	 // namespace

/* ---------- 载体单元面 ---------- */

TEST(BlockCarrier, WidthTableHasSevenStepsPerBlockType)
{
	struct Row
	{
		uint8_t q;
		uint8_t bits;
		int32_t qfmax;
		uint32_t bytes;
	};

	const Row intra[7] = {
		{ 1u,  13u, 4095, 104u },
		{ 3u,  12u, 1365, 96u  },
		   { 5u,  11u, 819,	 88u  },
		 { 9u,	10u, 455,  80u	},
		{ 17u, 9u,	240,  72u  },
		  { 33u, 8u,  124,	64u	 },
		{ 65u, 7u,	63,	  56u  }
	};
	const Row inter[7] = {
		{ 1u,  14u, 8159, 112u },
		{ 2u,  13u, 4080, 104u },
		{ 4u,  12u, 2040, 96u  },
		   { 8u,  11u, 1020, 88u  },
		{ 16u, 10u, 510,  80u  },
		   { 32u, 9u,  255,	 72u  },
		 { 64u, 8u,	 127,  64u	}
	};

	for (int i = 0; i < 7; ++i)
	{
		EXPECT_EQ(BlockBits(intra[i].q, true), intra[i].bits) << "帧内 q'=" << int(intra[i].q);
		EXPECT_EQ(BlockQfMax(intra[i].q, true), intra[i].qfmax);
		EXPECT_EQ(BlockBytes(intra[i].q, true), intra[i].bytes);
		EXPECT_EQ(BlockBits(inter[i].q, false), inter[i].bits) << "帧间 q'=" << int(inter[i].q);
		EXPECT_EQ(BlockQfMax(inter[i].q, false), inter[i].qfmax);
		EXPECT_EQ(BlockBytes(inter[i].q, false), inter[i].bytes);
	}

	/* 全域扫描的判据 = 「作者写死的独立表 vs 被测函数」：期望取自上面那两张七档表（帧内起点
	 * 1/3/5/9/17/33/65、帧间 1/2/4/8/16/32/64），不拿 BlockQfMax 的输出套
	 * ceil(log2(QF_max+1)) + 1 去比 BlockBits 的输出 —— 那两个都是被测函数，互证不证实现。
	 * 作者表自身也按同一条算式核一遍，表与算式谁漂了都红。 */
	for (int intra_side = 1; intra_side >= 0; --intra_side)
	{
		const bool intra_f = intra_side != 0;
		const Row* const table = intra_f ? intra : inter;
		int steps = 0;
		int prev = -1;
		for (int q = 1; q <= 127; ++q)
		{
			int want_bits = table[0].bits;
			int32_t want_max = table[0].qfmax;
			uint32_t want_bytes = table[0].bytes;
			for (int i = 0; i < 7; ++i)
			{
				if (q < table[i].q) continue;
				want_bits = table[i].bits;
				want_max = table[i].qfmax;
				want_bytes = table[i].bytes;
			}
			EXPECT_EQ(BlockBits(static_cast<uint8_t>(q), intra_f), want_bits) << "q'=" << q << " intra=" << intra_side;
			EXPECT_EQ(BlockQfMax(static_cast<uint8_t>(q), intra_f), want_max) << "q'=" << q << " intra=" << intra_side;
			// 算式对作者表本身核一遍：bits = ceil(log2(QF_max + 1)) + 1（1 位符号 + 幅值位）
			int n = 0;
			while ((int64_t(1) << n) < static_cast<int64_t>(want_max) + 1) ++n;
			EXPECT_EQ(want_bits, n + 1) << "作者表 q'=" << q << " intra=" << intra_side;
			// 单块字节 = 64 系数 × bits ÷ 8，整除无余位：9 bit → 72 B、10 bit → 80 B
			EXPECT_EQ(BlockBytes(static_cast<uint8_t>(q), intra_f), static_cast<uint32_t>(64 * want_bits / 8))
				<< "q'=" << q << " intra=" << intra_side;
			EXPECT_EQ(BlockBytes(static_cast<uint8_t>(q), intra_f), want_bytes)
				<< "q'=" << q << " intra=" << intra_side;
			if (want_bits != prev)
			{
				++steps;
				prev = want_bits;
			}
		}
		EXPECT_EQ(steps, 7);
	}
	std::cout << "位宽表：帧内 / 帧间各 7 档，127 个 q' 逐个对作者表的 bits / QF_max / 单块字节，"
				 "算式 ceil(log2(QF_max+1))+1 核的是作者表自己\n";
}

TEST(BlockCarrier, PackedFormRoundTripsAndSelfDescribes)
{
	int32_t qf[kBlocksPerMacroblock][kBlockCoefs];
	for (int b = 0; b < kBlocksPerMacroblock; ++b)
		for (int i = 0; i < kBlockCoefs; ++i) qf[b][i] = 0;

	// 模式 = 0b101111：Y0 / Y2 / Y3 / Cb / Cr 在场，Y1 不在场
	const uint8_t pattern = 0x2Fu;
	// q = 24：帧内 9 bit（|QF| ≤ 240），故取值全落在 [−240, 240] 内，且首末触到上下界
	int32_t vals[kBlockCoefs];
	for (int i = 0; i < kBlockCoefs; ++i) vals[i] = ((i * 37) % 241) - 120;
	vals[0] = 240;
	vals[1] = -240;
	vals[2] = 0;
	for (int i = 0; i < kBlockCoefs; ++i)
	{
		for (int b = 0; b < kBlocksPerMacroblock; ++b)
			if ((pattern & static_cast<uint8_t>(1u << (5 - b))) != 0u) qf[b][i] = vals[i];
	}

	std::vector<uint8_t> packed;
	ASSERT_EQ(EncodeBlockData(pattern, qf, 24u, true, CoefCoding::kFixed, &packed), VideoErr::kOk);
	// 期望写死成字面量，算式：模式 0x2F 里 1 的个数 = 5 个在场块；帧内 q'=24 → 每系数 9 bit
	//   ⇒ 单块 64 × 9 ÷ 8 = 72 B ⇒ 5 × 72 = 360 B。期望不拿 BlockBytes() 的输出现算 —— 那个也是被测函数。
	EXPECT_EQ(packed.size(), 360u);

	int32_t back[kBlocksPerMacroblock][kBlockCoefs];
	ASSERT_EQ(DecodeBlockData(pattern, packed.data(), packed.size(), 24u, true, CoefCoding::kFixed, back),
			  VideoErr::kOk);
	for (int b = 0; b < kBlocksPerMacroblock; ++b)
		for (int i = 0; i < kBlockCoefs; ++i) EXPECT_EQ(back[b][i], qf[b][i]) << "块 " << b << " 系数 " << i;

	// 不在场的块解出来必须是 0（块模式位为 0 ⇒ 不占字节、按零残差处置）
	for (int i = 0; i < kBlockCoefs; ++i) EXPECT_EQ(back[1][i], 0);
	std::cout << "载体往返：模式 0x2F 五块在场，载荷 " << packed.size() << " B，逐系数全等\n";
}

TEST(BlockCarrier, SizeMismatchAndOutOfWindowCoefficientsAreRejected)
{
	int32_t qf[kBlocksPerMacroblock][kBlockCoefs];
	for (int b = 0; b < kBlocksPerMacroblock; ++b)
		for (int i = 0; i < kBlockCoefs; ++i) qf[b][i] = (i == 0) ? 7 : 0;

	const uint8_t pattern = 0x01u;	  // 只有 Cr 在场（bit0）
	std::vector<uint8_t> packed;
	ASSERT_EQ(EncodeBlockData(pattern, qf, 24u, false, CoefCoding::kFixed, &packed), VideoErr::kOk);
	// 字面量期望，算式：帧间 q'=24 落 16..31 档 → 每系数 10 bit → 单块 64 × 10 ÷ 8 = 80 B，在场块 1 ⇒ 80 B
	EXPECT_EQ(packed.size(), 80u);

	int32_t back[kBlocksPerMacroblock][kBlockCoefs];
	// 多一字节 / 少一字节都判取值不成立（块字节自校验 的 block_size == present × 单块字节）
	std::vector<uint8_t> longer = packed;
	longer.push_back(0u);
	EXPECT_EQ(DecodeBlockData(pattern, longer.data(), longer.size(), 24u, false, CoefCoding::kFixed, back),
			  VideoErr::kValueRange);
	ASSERT_FALSE(packed.empty());
	EXPECT_EQ(DecodeBlockData(pattern, packed.data(), packed.size() - 1u, 24u, false, CoefCoding::kFixed, back),
			  VideoErr::kValueRange);

	/* 越窗系数：帧内 q'=3 → 12 bit（1 符号 + 11 幅值，位里可写到 2047），但窗口 |QF| ≤ 1365。
	 * 1366 恰越上界 1 —— 能写进位流，读出时必须判取值不成立。 */
	int32_t over[kBlocksPerMacroblock][kBlockCoefs];
	for (int b = 0; b < kBlocksPerMacroblock; ++b)
		for (int i = 0; i < kBlockCoefs; ++i) over[b][i] = 0;
	over[5][0] = 1366;
	std::vector<uint8_t> o;
	ASSERT_EQ(EncodeBlockData(pattern, over, 3u, true, CoefCoding::kFixed, &o), VideoErr::kOk);
	EXPECT_EQ(DecodeBlockData(pattern, o.data(), o.size(), 3u, true, CoefCoding::kFixed, back), VideoErr::kValueRange);

	// 上界本身（1365）必须读得过
	over[5][0] = 1365;
	std::vector<uint8_t> okbytes;
	ASSERT_EQ(EncodeBlockData(pattern, over, 3u, true, CoefCoding::kFixed, &okbytes), VideoErr::kOk);
	EXPECT_EQ(DecodeBlockData(pattern, okbytes.data(), okbytes.size(), 3u, true, CoefCoding::kFixed, back),
			  VideoErr::kOk);
	EXPECT_EQ(back[5][0], 1365);
	std::cout << "载体越界：字节数不符与系数越窗（1366 > 1365）都判取值不成立，上界本身读得过\n";
}

TEST(BlockCarrier, RoundingFormulaMatchesTheSpec)
{
	// f = round(b / 64)，四舍五入、半程离零（b 是反变换的整数出口）
	EXPECT_EQ(RoundResidual(0), 0);
	EXPECT_EQ(RoundResidual(31), 0);	// 31/64 = 0.484375 → 0
	EXPECT_EQ(RoundResidual(32), 1);	// 32/64 = 0.5 → 1，半程离零（不是就近偶）
	EXPECT_EQ(RoundResidual(-32), -1);
	EXPECT_EQ(RoundResidual(-31), 0);
	EXPECT_EQ(RoundResidual(8160), 128);	// 127.5 → 128
	EXPECT_EQ(RoundResidual(-8160), -128);
	EXPECT_EQ(RoundResidual(4096), 64);
	EXPECT_EQ(RoundResidual(-4096), -64);
	std::cout << "舍入式：半程离零（0.5 → 1、-0.5 → -1），非就近偶\n";
}

/* ---------- 编解逐字节全等 / 差异非零 / 误差单调 ---------- */

TEST(Reconstruction, EncoderRebuildEqualsDecoderOutput)
{
	// 编解逐字节全等：编码器内部重建帧 == 解码器输出，逐字节。六项比较域逐项成断言。
	const uint32_t w = 48u, h = 32u;
	CLV_VideoEncoder* e = nullptr;
	ASSERT_EQ(OpenEncoder(&e, w, h, 24u), CLV_VIDEO_OK);
	CLV_VideoDecoder* d = nullptr;
	ASSERT_EQ(OpenDecoder(&d, w, h), CLV_VIDEO_OK);
	CLV_VideoFrame* src = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
	WriteHighFrequency(src, w, h);

	CLV_VideoFrame* got = nullptr;
	CLV_VideoFrame* rec = nullptr;
	size_t payload = 0u;
	ASSERT_TRUE(EncodeDecodeOneI(e, d, src, 24u, &got, &rec, &payload));
	ASSERT_NE(got, nullptr);
	ASSERT_NE(rec, nullptr) << "编码器内部重建帧必须可得（I / P 帧后才更新）";

	CLV_VideoFrameDescC dr, dg;
	std::memset(&dr, 0, sizeof(dr));
	std::memset(&dg, 0, sizeof(dg));
	ASSERT_EQ(CLV_VideoFrameGetDesc(rec, &dr, nullptr), CLV_VIDEO_OK);
	ASSERT_EQ(CLV_VideoFrameGetDesc(got, &dg, nullptr), CLV_VIDEO_OK);

	// 同一输出域：色彩空间 / 位深 / range 口径 / 采样 / 平面排列 / 字节序 / 尺寸对齐逐项相同
	EXPECT_EQ(dr.pixel_format, dg.pixel_format);
	EXPECT_EQ(dr.bit_depth, dg.bit_depth);
	EXPECT_EQ(dr.coded_width, dg.coded_width);
	EXPECT_EQ(dr.coded_height, dg.coded_height);
	EXPECT_EQ(dr.display_width, dg.display_width);
	EXPECT_EQ(dr.display_height, dg.display_height);
	for (int p = 0; p < 3; ++p) EXPECT_EQ(dr.stride[p], dg.stride[p]);

	// 缓冲不同源守卫：重建帧与解码输出必须是两块独立内存。钩子若把解码输出那三块别名回来，
	// 下面的「逐字节全等」就成了自己比自己、恒真。三平面逐条判，plane[0] 是亮度面。
	ASSERT_NE(dr.plane[0], dg.plane[0]) << "亮度平面撞同一块缓冲，全等断言失去意义";
	for (int p = 1; p < 3; ++p) ASSERT_NE(dr.plane[p], dg.plane[p]) << "色度平面 " << p << " 撞同一块缓冲";

	// 逐帧一对一：一条 VOP 码流 → 一帧输出（EncodeDecodeOneI 里 flag == 1 已钉）
	// 中间精度先量化到输出格式：两侧都过同一条 RoundResidual + 夹到 8-bit（同一实现，见 reconstruct.cpp）
	const size_t same = CountSame(*rec, *got, w, h);
	// 有效区点数钉字面量：48 × 32 + 2 × (24 × 16) = 1536 + 768 = 2304
	EXPECT_EQ(ValidCount(w, h), 2304u);
	EXPECT_EQ(same, ValidCount(w, h)) << "载荷 " << payload << " B";
	EXPECT_EQ(same, 2304u) << "载荷 " << payload << " B";
	RecordProperty("RebuildEqualsDecoderOutputSamePoints", static_cast<int>(same));
	RecordProperty("RebuildEqualsDecoderOutputPayloadBytes", static_cast<int>(payload));
	std::cout << "编解逐字节全等：编码器内部重建 vs 解码输出，" << same << " 点逐字节全等（冻结值 2304），载荷 "
			  << payload << " B\n";

	CLV_VideoFrameRelease(got);
	CLV_VideoFrameRelease(src);
	CLV_VideoDecoderClose(d);
	CLV_VideoEncoderClose(e);
}

TEST(Reconstruction, QuantizedPathDiffersFromSource)
{
	// 差异非零：源含高频，量化后重建必不等于源（逐点全等 = 量化链路未参与）
	const uint32_t w = 48u, h = 32u;
	CLV_VideoEncoder* e = nullptr;
	ASSERT_EQ(OpenEncoder(&e, w, h, 24u), CLV_VIDEO_OK);
	CLV_VideoDecoder* d = nullptr;
	ASSERT_EQ(OpenDecoder(&d, w, h), CLV_VIDEO_OK);
	CLV_VideoFrame* src = nullptr;
	ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
	WriteHighFrequency(src, w, h);

	CLV_VideoFrame* got = nullptr;
	ASSERT_TRUE(EncodeDecodeOneI(e, d, src, 24u, &got, nullptr, nullptr));
	ASSERT_NE(got, nullptr);

	const size_t same = CountSame(*src, *got, w, h);
	const size_t valid = ValidCount(w, h);
	const size_t diff = valid - same;
	const size_t absdiff = TotalAbsDiff(*src, *got, w, h);
	EXPECT_GT(diff, 0u) << "源与重建必须不等：全等 = 量化链路没接上";
	EXPECT_GT(absdiff, 0u);
	// 冻结数值进断言（只打印就红不出来）：48 × 32 + 2 × 24 × 16 = 2304 点里
	//   不同点 1388（⇒ 相等点 916 = 2304 − 1388），逐点绝对差合计 2215。
	EXPECT_EQ(valid, 2304u);
	EXPECT_EQ(diff, 1388u);
	EXPECT_EQ(absdiff, 2215u);
	RecordProperty("QuantizedDiffPoints", static_cast<int>(diff));
	RecordProperty("QuantizedTotalAbsDiff", static_cast<int>(absdiff));
	std::cout << "差异非零：源 vs 重建，" << diff << " / " << valid << " 点不同（冻结值 1388 / 2304），总绝对差 "
			  << absdiff << "（冻结值 2215）\n";

	CLV_VideoFrameRelease(got);
	CLV_VideoFrameRelease(src);
	CLV_VideoDecoderClose(d);
	CLV_VideoEncoderClose(e);
}

TEST(Reconstruction, ErrorGrowsWithQuantizer)
{
	// 误差单调：单调性 q = 8 / 32 / 96 三点
	const uint32_t w = 48u, h = 32u;
	const uint8_t qs[3] = { 8u, 32u, 96u };
	size_t same[3] = { 0u, 0u, 0u };
	size_t absdiff[3] = { 0u, 0u, 0u };
	for (int k = 0; k < 3; ++k)
	{
		CLV_VideoEncoder* e = nullptr;
		ASSERT_EQ(OpenEncoder(&e, w, h, qs[k]), CLV_VIDEO_OK);
		CLV_VideoDecoder* d = nullptr;
		ASSERT_EQ(OpenDecoder(&d, w, h), CLV_VIDEO_OK);
		CLV_VideoFrame* src = nullptr;
		ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK);
		WriteHighFrequency(src, w, h);

		CLV_VideoFrame* got = nullptr;
		ASSERT_TRUE(EncodeDecodeOneI(e, d, src, qs[k], &got, nullptr, nullptr));
		ASSERT_NE(got, nullptr);
		same[k] = CountSame(*src, *got, w, h);
		absdiff[k] = TotalAbsDiff(*src, *got, w, h);

		CLV_VideoFrameRelease(got);
		CLV_VideoFrameRelease(src);
		CLV_VideoDecoderClose(d);
		CLV_VideoEncoderClose(e);
	}

	const size_t valid = ValidCount(w, h);
	for (int k = 0; k < 3; ++k)
	{
		std::cout << "  q=" << int(qs[k]) << " 相等点 " << same[k] << " / " << valid << "，总绝对差 " << absdiff[k]
				  << "\n";
	}
	// 步长越大，误差越大：总绝对差单调增，相等点单调减
	EXPECT_LT(absdiff[0], absdiff[1]);
	EXPECT_LT(absdiff[1], absdiff[2]);
	EXPECT_GT(same[0], same[1]);
	EXPECT_GT(same[1], same[2]);
	/* 三个档的冻结数值一并钉住（只比不等式，数值漂了红不出来）。
	 * 有效区点数 = 48 × 32 + 2 × (24 × 16) = 1536 + 768 = 2304，于是：
	 *   q = 8  ：相等点 1279 ⇒ 不同点 2304 − 1279 = 1025，总绝对差 1222
	 *   q = 32 ：相等点 256  ⇒ 不同点 2304 − 256  = 2048，总绝对差 5898
	 *   q = 96 ：相等点 173  ⇒ 不同点 2304 − 173  = 2131，总绝对差 11361
	 * 单调性按这三对数读：总绝对差 1222 < 5898 < 11361，相等点 1279 > 256 > 173。 */
	EXPECT_EQ(absdiff[0], 1222u);
	EXPECT_EQ(absdiff[1], 5898u);
	EXPECT_EQ(absdiff[2], 11361u);
	EXPECT_EQ(same[0], 1279u);
	EXPECT_EQ(same[1], 256u);
	EXPECT_EQ(same[2], 173u);
	RecordProperty("TotalAbsDiffQ8", static_cast<int>(absdiff[0]));
	RecordProperty("TotalAbsDiffQ32", static_cast<int>(absdiff[1]));
	RecordProperty("TotalAbsDiffQ96", static_cast<int>(absdiff[2]));
	RecordProperty("SamePointsQ8", static_cast<int>(same[0]));
	RecordProperty("SamePointsQ32", static_cast<int>(same[1]));
	RecordProperty("SamePointsQ96", static_cast<int>(same[2]));
	std::cout << "误差单调：q = 8 / 32 / 96 三点总绝对差 " << absdiff[0] << " < " << absdiff[1] << " < " << absdiff[2]
			  << "（严格递增），相等点 " << same[0] << " > " << same[1] << " > " << same[2] << "（严格递减），有效区 "
			  << valid << " 点\n";
}

/* 压线档 · 平块亮度全档位精确。
 * 取值：level 逐档扫 0..255、q = 1（最细的一档，饱和域够不够宽在这一档最看得出来）。
 * 判的是「第二级饱和域不误夹合法 DC」这条语义，不是某个冻结测量值 —— 平块 DC = 64 × (level − 128)，
 * |DC| 上界 64 × 128 = 8192 恰等于帧内档 T_max，故全 256 档都该逐点回到源。
 * 对照档：饱和域若只到 ±2048，就只有 |level − 128| ≤ 2048 ÷ 64 = 32 的 level ∈ [96, 160] 共 65 档位精确，
 * 其余 191 档被砍 ⇒ 「256 档全等」这一条把界宽钉在 8192 那一档。 */
TEST(Reconstruction, FlatLumaSweep0To255BitExactAtQ1)
{
	const uint32_t w = 48u, h = 32u;
	const size_t valid = ValidCount(w, h);
	ASSERT_EQ(valid, 2304u);
	int bad = 0;
	int first_bad = -1;
	for (uint32_t lv = 0u; lv < 256u; ++lv)
	{
		/* 每档一对新句柄：EncodeDecodeOneI 收尾走 Flush，冲刷后解码句柄进 flushed 态，
		   再喂下一帧按状态不成立拒解，复用句柄证不了像素。 */
		CLV_VideoEncoder* e = nullptr;
		ASSERT_EQ(OpenEncoder(&e, w, h, 1u), CLV_VIDEO_OK) << "level=" << lv;
		CLV_VideoDecoder* d = nullptr;
		ASSERT_EQ(OpenDecoder(&d, w, h), CLV_VIDEO_OK) << "level=" << lv;
		CLV_VideoFrame* src = nullptr;
		ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &src), CLV_VIDEO_OK) << "level=" << lv;
		WriteFlatLuma(src, w, h, static_cast<uint8_t>(lv));
		CLV_VideoFrame* got = nullptr;
		ASSERT_TRUE(EncodeDecodeOneI(e, d, src, 1u, &got, nullptr, nullptr)) << "level=" << lv;
		ASSERT_NE(got, nullptr) << "level=" << lv;
		if (CountSame(*src, *got, w, h) != valid)
		{
			++bad;
			if (first_bad < 0) first_bad = static_cast<int>(lv);
		}
		CLV_VideoFrameRelease(got);
		CLV_VideoFrameRelease(src);
		CLV_VideoDecoderClose(d);
		CLV_VideoEncoderClose(e);
	}
	EXPECT_EQ(bad, 0) << "首个不等档 level=" << first_bad;
	std::cout << "平块压线全档：level 0..255 共 256 档、q=1 逐档编解，不等档数 " << bad << "（要求 0），有效区 "
			  << valid << " 点/档\n";
}

