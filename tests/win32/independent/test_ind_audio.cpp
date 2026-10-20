/* test_ind_audio.cpp
 *
 * 期望来源限定为公开头 `include/CLV_Audio.h` 的契约文字、实现层声明域常量
 * （`src/audio/frame.h` 的单点界值：子集上界三值、按位深分档的块大小下界两值、采样率拒档那一界；
 * `src/audio/metadata.h` 的 kStreamInfoSize）、
 * 本件注释里写明的算式，以及本件自己按算式生成的输入素材；不从被测面返回值现取期望。
 * Suite 名以 `Independent` 起头，供按名前缀单独过滤。
 *
 * 帧头位域（`src/audio/frame.h` 件头与 `src/audio/frame.cpp` 的 ParseFrameHead）：
 *   byte0 = 0xFF；byte1 = 同步码低 6 位 + 保留位（必须 0）+ 块策略位（固定块流取 0xF8）；
 *   byte2 = 块大小码 4 位 | 采样率码 4 位；byte3 = 声道码 4 位 | 位深码 3 位 | 保留位 1 位（必须 0）；
 *   其后是扩展 UTF-8 编码号（1 字节起），块大小码 0b0110 跟 1 字节、0b0111 跟 2 字节大端存储值
 *   （存储值 = 块大小 − 1），末字节是覆盖帧头前置字节的 CRC-8；帧尾 2 字节大端 CRC-16 覆盖除自身以外的整帧。
 *   块大小码 0b1000..0b1111 是定值码，块大小 = 2^码值；采样率码 1..11 按下表（frame.cpp 的 rate_from_code）：
 *   1 → 88200、2 → 176400、3 → 192000、4 → 8000、5 → 16000、6 → 22050、7 → 24000、8 → 32000、
 *   9 → 44100、10 → 48000、11 → 96000。
 *
 * 两道护栏各自独立：帧头自核只判码值是否保留/禁用与两枚校验自洽；流式子集另判
 *   块大小 ≤ 16384，且采样率 ≤ 48000 时块大小 ≤ 4608（frame.h 的 kSubsetMaxBlocksize /
 *   kSubsetRateThresholdForShortBlock / kSubsetMaxBlocksizeAtLowRate 三值）。
 *   帧头层收下的合法码值不许当坏值喂，越子集由子集那一道拒，码 CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE。
 *
 * 判据口径：界下档与界上档都断唯一结论，结论取自实现层判式的比较方向（判式写的是 `<=` 还是 `>`，
 * 界本身就落在对应那一侧），一格只给一条判据；越界档断「拒」。
 * 崩溃、挂起、或不报错的静默错位一律停手上报，不放宽断言。
 */

#include "CLV_Audio.h"
#include "audio/frame.h"
#include "audio/metadata.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace
{

	using clv::audio::Crc16;
	using clv::audio::Crc8;
	using clv::audio::FrameCrcOk;
	using clv::audio::FrameHead;
	using clv::audio::ParseFrameHead;
	using clv::audio::StreamableSubsetOk;

	constexpr uint32_t kRate = 48000u;
	constexpr uint16_t kChannels = 2u;
	constexpr uint16_t kDepth = 16u;
	constexpr uint32_t kBlocksize = 512u;
	constexpr uint32_t kPacketCap = 1u << 18;

	// 每样本字节数 = ceil(bit_depth ÷ 8)（include/CLV_Audio.h 的 CLV_AudioBufferDescC 条目）
	constexpr size_t BytesPerSample() { return (kDepth + 7u) / 8u; }

	// 交错布局里一个采样帧（各声道各一样本）的字节数 = channels × ceil(bit_depth ÷ 8)
	constexpr size_t SampleFrameBytes() { return static_cast<size_t>(kChannels) * BytesPerSample(); }

	// 一帧满块的 PCM 字节数 = 块大小 × channels × ceil(bit_depth ÷ 8)
	constexpr size_t FramePcmBytes() { return static_cast<size_t>(kBlocksize) * SampleFrameBytes(); }

	// 满块帧数 = ⌈样本数 ÷ 块大小⌉
	size_t WantFrames(uint32_t samples) { return (static_cast<size_t>(samples) + kBlocksize - 1u) / kBlocksize; }

	// 帧头位域取值：声道码 1 → 声道数 = 码 + 1 = 2；位深码 4 → 16 位
	constexpr uint8_t kChCode = 1u;
	constexpr uint8_t kBitsCode = 4u;
	constexpr uint8_t kSrCode96k = 11u;
	constexpr uint8_t kSrCode44k = 9u;
	// 块大小码：0b0111 走 2 字节显式存储值；0b1110 / 0b1111 是 2^v 定值码
	constexpr uint8_t kBsCodeExplicit16 = 7u;
	constexpr uint8_t kBsCode14 = 14u;
	constexpr uint8_t kBsCode15 = 15u;

	CLV_AudioEncParamsC EncParams()
	{
		CLV_AudioEncParamsC p {};
		p.sample_rate = kRate;
		p.channels = kChannels;
		p.bit_depth = kDepth;
		p.blocksize = static_cast<uint16_t>(kBlocksize);
		p.level = 0u;
		p.reserved = 0u;
		return p;
	}

	CLV_AudioStreamParamsC StreamParams()
	{
		CLV_AudioStreamParamsC p {};
		p.sample_rate = kRate;
		p.channels = kChannels;
		p.bit_depth = kDepth;
		p.max_packet_size = kPacketCap;
		return p;
	}

	/* 确定性样本：v(s, c) = ((31s + 7c) mod 4001) − 2000，按小端补码逐声道写两字节。
	 * 取值域在 int16 之内；相邻采样的两个声道都取值不同，故任何整采样错位都会改动字节。 */
	std::vector<uint8_t> MakePcm(uint32_t samples_per_channel)
	{
		std::vector<uint8_t> pcm(static_cast<size_t>(samples_per_channel) * SampleFrameBytes(), 0u);
		for (uint32_t s = 0u; s < samples_per_channel; ++s)
		{
			for (uint16_t c = 0u; c < kChannels; ++c)
			{
				const int32_t v = static_cast<int32_t>((31ull * s + 7ull * c) % 4001ull) - 2000;
				const uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(v));
				const size_t off = (static_cast<size_t>(s) * kChannels + c) * BytesPerSample();
				pcm[off] = static_cast<uint8_t>(u & 0xFFu);
				pcm[off + 1u] = static_cast<uint8_t>((u >> 8) & 0xFFu);
			}
		}
		return pcm;
	}

	// PCM 第 [a, b) 个采样的字节段
	std::vector<uint8_t> Slice(const std::vector<uint8_t>& pcm, uint32_t a, uint32_t b)
	{
		const size_t step = SampleFrameBytes();
		return std::vector<uint8_t>(pcm.begin() + static_cast<long>(a) * static_cast<long>(step),
									pcm.begin() + static_cast<long>(b) * static_cast<long>(step));
	}

	struct Encoded
	{
		std::vector<std::vector<uint8_t>> frames;
		std::vector<uint8_t> streaminfo;
	};

	// 编码闭环：整批喂入 → Flush → 逐帧取回；流信息载荷定长 34 字节
	void EncodeAll(const std::vector<uint8_t>& pcm, uint32_t batch_samples, Encoded* out)
	{
		out->frames.clear();
		out->streaminfo.clear();

		const CLV_AudioEncParamsC ep = EncParams();
		CLV_AudioError err = CLV_AUDIO_OK;
		CLV_AudioEncoder* e = CLV_AudioEncoderOpen(&ep, &err);
		ASSERT_NE(e, nullptr) << CLV_AudioStrError(err);

		const size_t stride = static_cast<size_t>(batch_samples) * SampleFrameBytes();
		for (size_t off = 0u; off < pcm.size(); off += stride)
		{
			const size_t n = pcm.size() - off < stride ? pcm.size() - off : stride;
			const uint32_t sc = static_cast<uint32_t>(n / SampleFrameBytes());
			ASSERT_EQ(CLV_AudioEncoderWrite(e, pcm.data() + off, n, sc), CLV_AUDIO_OK);
		}
		ASSERT_EQ(CLV_AudioEncoderFlush(e), CLV_AUDIO_OK);

		for (;;)
		{
			const uint8_t* data = nullptr;
			size_t size = 0u;
			uint8_t got = 0u;
			ASSERT_EQ(CLV_AudioEncoderNextFrame(e, &data, &size, &got), CLV_AUDIO_OK);
			if (got == 0u) break;
			out->frames.emplace_back(data, data + size);
		}

		CLV_AudioBlob* blob = nullptr;
		ASSERT_EQ(CLV_AudioEncoderTakeStreamInfo(e, &blob), CLV_AUDIO_OK);
		const uint8_t* si = nullptr;
		size_t si_size = 0u;
		ASSERT_EQ(CLV_AudioBlobGetBytes(blob, &si, &si_size), CLV_AUDIO_OK);
		ASSERT_EQ(si_size, clv::audio::kStreamInfoSize);	// src/audio/metadata.h
		out->streaminfo.assign(si, si + si_size);
		CLV_AudioBlobRelease(blob);
		CLV_AudioEncoderClose(e);
	}

	// 解一帧并交出 PCM；出参缓冲由库拥有，取完即释放
	CLV_AudioError DecodeOne(CLV_AudioDecoder* d, const std::vector<uint8_t>& frame, std::vector<uint8_t>* pcm,
							 CLV_AudioFrameInfoC* info)
	{
		CLV_AudioBuffer* buf = nullptr;
		uint8_t got = 0u;
		const CLV_AudioError e = CLV_AudioDecoderDecodeFrame(d, frame.data(), frame.size(), &buf, &got, info);
		if (e == CLV_AUDIO_OK && got == 1u && buf != nullptr)
		{
			CLV_AudioBufferDescC desc {};
			if (CLV_AudioBufferGetDesc(buf, &desc) == CLV_AUDIO_OK) pcm->assign(desc.data, desc.data + desc.size);
		}
		if (buf != nullptr) CLV_AudioBufferRelease(buf);
		return e;
	}

	CLV_AudioDecoder* OpenDecoder(const Encoded& es, CLV_AudioError* err)
	{
		const CLV_AudioStreamParamsC sp = StreamParams();
		return CLV_AudioDecoderOpen(&sp, es.streaminfo.data(), es.streaminfo.size(), err);
	}

	/* 合成一帧自洽帧头（固定块流、编码号 1 字节 = 帧号 0）+ 只有帧尾校验的样本区。
	 * 两枚校验用 frame.h 交出的 Crc8 / Crc16 现算：它们是本件的输入构造件，不是被判的那道护栏。 */
	std::vector<uint8_t> StubFrame(uint8_t bs_code, uint8_t sr_code, uint32_t declared_blocksize)
	{
		std::vector<uint8_t> f;
		f.push_back(0xFFu);
		f.push_back(0xF8u);
		f.push_back(static_cast<uint8_t>((bs_code << 4u) | sr_code));
		f.push_back(static_cast<uint8_t>((kChCode << 4u) | (kBitsCode << 1u)));
		f.push_back(0x00u);
		if (bs_code == kBsCodeExplicit16)
		{
			const uint32_t stored = declared_blocksize - 1u;	// 块大小 = 存储值 + 1
			f.push_back(static_cast<uint8_t>((stored >> 8u) & 0xFFu));
			f.push_back(static_cast<uint8_t>(stored & 0xFFu));
		}
		f.push_back(Crc8(f.data(), f.size()));
		const uint16_t tail = Crc16(f.data(), f.size());
		f.push_back(static_cast<uint8_t>(tail >> 8u));
		f.push_back(static_cast<uint8_t>(tail & 0xFFu));
		return f;
	}

	// 出参「没被写过」的记号：每个字段都停在填充值上
	CLV_AudioFrameHeadC SentinelHead()
	{
		CLV_AudioFrameHeadC h {};
		std::memset(&h, 0xFF, sizeof(h));
		return h;
	}

	bool HeadUntouched(const CLV_AudioFrameHeadC& h)
	{
		const uint32_t u32_max = std::numeric_limits<uint32_t>::max();
		const uint16_t u16_max = std::numeric_limits<uint16_t>::max();
		return h.blocksize == u32_max && h.sample_rate == u32_max && h.channels == u16_max && h.bit_depth == u16_max &&
			   h.frame_or_sample_number == std::numeric_limits<uint64_t>::max() && h.blocking_strategy == 0xFFu &&
			   h.header_size == 0xFFu;
	}

	// 承载块：首字节 = FLAC 原块类型码，其后 = 原块载荷（include/CLV_Audio.h 的 SplitMetaBlock 条目）
	std::vector<uint8_t> MetaBlock(uint8_t flac_block_type, uint8_t fill)
	{
		std::vector<uint8_t> v(1u, flac_block_type);
		v.insert(v.end(), 8u, fill);
		return v;
	}

}	 // namespace

TEST(IndependentAudio, MetaBlockTypeSixIsStoredSevenIsSkipped)
{
	const std::vector<uint8_t> pcm = MakePcm(kBlocksize);
	Encoded es;
	EncodeAll(pcm, kBlocksize, &es);
	ASSERT_EQ(es.frames.size(), WantFrames(kBlocksize));

	/* 类型码 → 去向的界：0 = 流信息（不得进承载块）、1 = 填充（丢弃）、2..6 = 原样收、≥ 7 = 跳过，
	 * 出自 include/CLV_Audio.h 的 ClassifyMetaBlock 条目；分类实现见 src/audio/metadata.cpp。 */
	struct CodeTier
	{
		uint8_t type;
		CLV_AudioError code;
		CLV_AudioMetaAction action;
	};

	const CodeTier tiers[] = {
		CodeTier { 0u,	 CLV_AUDIO_METADATA_BAD,		   CLV_AUDIO_META_REJECT_STREAMINFO },
		CodeTier { 1u,	 CLV_AUDIO_OK,					   CLV_AUDIO_META_DROP_PADDING		},
		CodeTier { 2u,	 CLV_AUDIO_OK,					   CLV_AUDIO_META_STORE				},
		CodeTier { 3u,	 CLV_AUDIO_OK,					   CLV_AUDIO_META_STORE				},
		CodeTier { 5u,	 CLV_AUDIO_OK,					   CLV_AUDIO_META_STORE				},
		CodeTier { 6u,	 CLV_AUDIO_OK,					   CLV_AUDIO_META_STORE				}, // 「收」这一路的界上值
		CodeTier { 7u,	 CLV_AUDIO_BLOCK_TYPE_UNSUPPORTED, CLV_AUDIO_META_SKIP_UNKNOWN		}, // 越界第一档
		CodeTier { 8u,	 CLV_AUDIO_BLOCK_TYPE_UNSUPPORTED, CLV_AUDIO_META_SKIP_UNKNOWN		},
		CodeTier { 126u, CLV_AUDIO_BLOCK_TYPE_UNSUPPORTED, CLV_AUDIO_META_SKIP_UNKNOWN		},
		CodeTier { 255u, CLV_AUDIO_BLOCK_TYPE_UNSUPPORTED, CLV_AUDIO_META_SKIP_UNKNOWN		}
	};

	for (const CodeTier& t: tiers)
	{
		SCOPED_TRACE("块类型码 " + std::to_string(static_cast<unsigned>(t.type)));
		CLV_AudioMetaAction act = CLV_AUDIO_META_STORE;
		// 越界档现行码是 BLOCK_TYPE_UNSUPPORTED（include/CLV_Audio.h 的枚举条目），不是 UNSUPPORTED
		EXPECT_EQ(CLV_AudioClassifyMetaBlock(t.type, &act), t.code);
		EXPECT_EQ(act, t.action);
	}

	// 已知会被拒的档：主出参为 NULL → 参数不成立（include/CLV_Audio.h 的出参口径）
	EXPECT_EQ(CLV_AudioClassifyMetaBlock(6u, nullptr), CLV_AUDIO_INVALID_ARGUMENT);

	// 句柄路径：分类与计数各归各，四种处置都不判损坏
	CLV_AudioError err = CLV_AUDIO_OK;
	CLV_AudioDecoder* d = OpenDecoder(es, &err);
	ASSERT_NE(d, nullptr) << CLV_AudioStrError(err);
	EXPECT_EQ(err, CLV_AUDIO_OK);

	const uint8_t stored_types[] = { 2u, 3u, 5u, 6u };
	const uint8_t unknown_types[] = { 7u, 255u };
	uint64_t want_stored = 0, want_padding = 0, want_rejected = 0, want_skipped = 0;

	for (const uint8_t t: stored_types)
	{
		const std::vector<uint8_t> b = MetaBlock(t, 0xA0u);
		SCOPED_TRACE("承载块类型 " + std::to_string(static_cast<unsigned>(t)));
		EXPECT_EQ(CLV_AudioDecoderAddMetaBlock(d, b.data(), b.size()), CLV_AUDIO_OK);
		++want_stored;
	}
	for (const uint8_t t: unknown_types)
	{
		const std::vector<uint8_t> b = MetaBlock(t, 0xB0u);
		SCOPED_TRACE("承载块类型 " + std::to_string(static_cast<unsigned>(t)));
		EXPECT_EQ(CLV_AudioDecoderAddMetaBlock(d, b.data(), b.size()), CLV_AUDIO_BLOCK_TYPE_UNSUPPORTED);
		++want_skipped;
	}
	{
		const std::vector<uint8_t> padding = MetaBlock(1u, 0x00u);
		EXPECT_EQ(CLV_AudioDecoderAddMetaBlock(d, padding.data(), padding.size()), CLV_AUDIO_OK);
		++want_padding;
		// 已知会被拒的档：承载块里出现流信息类型码 → 丢块 + 计数 + 继续
		const std::vector<uint8_t> inside = MetaBlock(0u, 0x00u);
		EXPECT_EQ(CLV_AudioDecoderAddMetaBlock(d, inside.data(), inside.size()), CLV_AUDIO_METADATA_BAD);
		++want_rejected;
	}

	CLV_AudioStatsC st {};
	ASSERT_EQ(CLV_AudioDecoderStats(d, &st), CLV_AUDIO_OK);
	EXPECT_EQ(st.blocks_stored, want_stored);
	EXPECT_EQ(st.blocks_dropped_padding, want_padding);
	EXPECT_EQ(st.blocks_rejected_streaminfo, want_rejected);
	EXPECT_EQ(st.blocks_skipped_unknown, want_skipped);
	EXPECT_EQ(st.stream_rejected, 0) << "承载块的分类不该把整条流判坏";

	// 计数之外再断一句柄仍可用：真帧照常解出，PCM 逐字节等于源
	std::vector<uint8_t> got;
	CLV_AudioFrameInfoC info {};
	ASSERT_EQ(DecodeOne(d, es.frames[0], &got, &info), CLV_AUDIO_OK);
	EXPECT_EQ(got, pcm);
	EXPECT_EQ(info.blocksize, kBlocksize);
	EXPECT_EQ(info.first_sample, 0u);

	// 已知会被拒的档：载荷为空
	const std::vector<uint8_t> empty_block;
	EXPECT_EQ(CLV_AudioDecoderAddMetaBlock(d, empty_block.data(), 0u), CLV_AUDIO_INVALID_ARGUMENT);

	CLV_AudioDecoderClose(d);
}

TEST(IndependentAudio, BlocksizeCodeTableAndSubsetGateAreTwoLayers)
{
	// 子集上界三值是声明域常量，先钉住它们与帧头码表的对应关系
	static_assert(clv::audio::kSubsetMaxBlocksize == 16384u, "子集块大小上界");
	static_assert(clv::audio::kSubsetMaxBlocksizeAtLowRate == 4608u, "采样率 ≤ 48000 时的上界");
	static_assert(clv::audio::kSubsetRateThresholdForShortBlock == 48000u, "第二道界的采样率阈值");

	struct Tier
	{
		const char* label;
		uint8_t bs_code;
		uint8_t sr_code;
		uint32_t blocksize;
		uint32_t rate;
		bool subset_ok;
	};

	/* 定值码档：块大小 = 2^码值（src/audio/frame.cpp 的 2^v 分支），0b1110 / 0b1111 都不是保留值。
	 * 第三档把同一块大小配 44100 采样率码，撞的是第二道界（≤ 48000 时 ≤ 4608）。 */
	const Tier tiers[] = {
		Tier { "定值码 0b1110",			 kBsCode14, kSrCode96k, 1u << 14u, 96000u, true	 },
		Tier { "定值码 0b1111",			 kBsCode15, kSrCode96k, 1u << 15u, 96000u, false },
		Tier { "定值码 0b1110 配 44100", kBsCode14, kSrCode44k, 1u << 14u, 44100u, false }
	};

	for (const Tier& t: tiers)
	{
		SCOPED_TRACE(std::string("档：") + t.label);
		const std::vector<uint8_t> f = StubFrame(t.bs_code, t.sr_code, t.blocksize);
		// 帧头 4 定长 + 1 编码号 + 1 CRC-8，样本区只剩 2 字节帧尾校验
		ASSERT_EQ(f.size(), 8u);

		// 帧头层：两枚定值码都是合法码，不许当坏值喂
		FrameHead raw {};
		ASSERT_TRUE(ParseFrameHead(f.data(), f.size(), &raw)) << "帧头层拒了合法块大小码";
		EXPECT_EQ(raw.blocksize, t.blocksize);
		EXPECT_EQ(raw.sample_rate, t.rate);
		EXPECT_EQ(raw.channels, static_cast<uint16_t>(kChCode + 1u));
		EXPECT_EQ(raw.bit_depth, 16);	 // 位深码 0b100 → 16
		EXPECT_EQ(raw.blocking_strategy, 0);
		EXPECT_EQ(raw.header_size, 6);
		EXPECT_TRUE(FrameCrcOk(f.data(), f.size()));

		// 子集层：只有它判块大小上界
		EXPECT_EQ(StreamableSubsetOk(raw.blocksize, raw.sample_rate), t.subset_ok);

		CLV_AudioFrameHeadC h = SentinelHead();
		const CLV_AudioError e = CLV_AudioInspectFrame(f.data(), f.size(), &h);
		if (t.subset_ok)
		{
			// 界上档：16384 正是上界本身 → 对
			ASSERT_EQ(e, CLV_AUDIO_OK) << "子集内的合法帧被公开口拒了";
			EXPECT_EQ(h.blocksize, t.blocksize);
			EXPECT_EQ(h.sample_rate, t.rate);
			EXPECT_EQ(h.header_size, 6);
		}
		else
		{
			// 越界档：拒，且码是子集那枚而不是帧头坏
			EXPECT_EQ(e, CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE);
			EXPECT_TRUE(HeadUntouched(h)) << "拒档不该写出参";
		}
	}

	// 两道界各断一对邻值：上界本身收、上界 + 1 拒
	EXPECT_TRUE(StreamableSubsetOk(clv::audio::kSubsetMaxBlocksize, 96000u));
	EXPECT_FALSE(StreamableSubsetOk(clv::audio::kSubsetMaxBlocksize + 1u, 96000u));
	EXPECT_TRUE(StreamableSubsetOk(clv::audio::kSubsetMaxBlocksizeAtLowRate, kRate));
	EXPECT_FALSE(StreamableSubsetOk(clv::audio::kSubsetMaxBlocksizeAtLowRate + 1u, kRate));
	// 采样率 0 = 「值只在流信息里给」，第二道界不参与判定
	EXPECT_TRUE(StreamableSubsetOk(clv::audio::kSubsetMaxBlocksize, 0u));
}

TEST(IndependentAudio, SubsetBlocksizeDeclarationAboveMaxIsRefused)
{
	/* 显式声明档：块大小码 0b0111 后跟 2 字节大端存储值，块大小 = 存储值 + 1
	 * （src/audio/frame.cpp 的 0b0110 / 0b0111 分支）。这一档表得出任意 ≤ 65536 的块大小，
	 * 所以「超过 16384 的声明」写得出来，必须被子集护栏拒。 */
	const std::vector<uint8_t> pcm = MakePcm(kBlocksize);
	Encoded es;
	EncodeAll(pcm, kBlocksize, &es);
	ASSERT_EQ(es.frames.size(), WantFrames(kBlocksize));

	const uint32_t above[] = { clv::audio::kSubsetMaxBlocksize + 1u, 20000u, 32768u, 65536u };
	for (const uint32_t declared: above)
	{
		SCOPED_TRACE("声明块大小 = " + std::to_string(declared));
		const std::vector<uint8_t> f = StubFrame(kBsCodeExplicit16, kSrCode96k, declared);
		// 帧头自洽、两枚校验齐：拦它的只有子集那一道
		FrameHead raw {};
		ASSERT_TRUE(ParseFrameHead(f.data(), f.size(), &raw));
		ASSERT_EQ(raw.blocksize, declared);
		ASSERT_TRUE(FrameCrcOk(f.data(), f.size()));
		EXPECT_FALSE(StreamableSubsetOk(raw.blocksize, raw.sample_rate));

		CLV_AudioFrameHeadC h = SentinelHead();
		EXPECT_EQ(CLV_AudioInspectFrame(f.data(), f.size(), &h), CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE);
		EXPECT_TRUE(HeadUntouched(h)) << "拒档不该写出参";
	}

	/* 界上档：上界本身 16384（配 96000 的采样率码，不落进第二道界）。子集判式写的是 `blocksize > 上界` 才不成立
	 * （src/audio/frame.cpp 的 StreamableSubsetOk 引 frame.h 的 kSubsetMaxBlocksize），本件前面那对邻值断言
	 * 钉的就是同一判式的两侧 ⇒ 界本身在受理侧，公开口 CLV_AudioInspectFrame 走的就是这一支（clv_audio.cpp
	 * 的 InspectFrame 子集分支），唯一结论是「收」。 */
	const std::vector<uint8_t> at_max = StubFrame(kBsCodeExplicit16, kSrCode96k, clv::audio::kSubsetMaxBlocksize);
	CLV_AudioFrameHeadC edge_head {};
	const CLV_AudioError edge = CLV_AudioInspectFrame(at_max.data(), at_max.size(), &edge_head);
	ASSERT_EQ(edge, CLV_AUDIO_OK) << "块大小正好等于子集上界被拒，与判式的比较方向矛盾";
	EXPECT_EQ(edge_head.blocksize, clv::audio::kSubsetMaxBlocksize);

	/* 句柄路径：越界声明在子集这一步就返回，对账与后端都不被触及；
	 * 处置是丢帧 + 计数 + 返码（src/audio/clv_audio.cpp 的 DecodeFrame 子集分支）。 */
	CLV_AudioError err = CLV_AUDIO_OK;
	CLV_AudioDecoder* d = OpenDecoder(es, &err);
	ASSERT_NE(d, nullptr) << CLV_AudioStrError(err);

	for (const uint32_t declared: above)
	{
		const std::vector<uint8_t> f = StubFrame(kBsCodeExplicit16, kSrCode96k, declared);
		std::vector<uint8_t> got;
		CLV_AudioFrameInfoC info {};
		SCOPED_TRACE("句柄档：声明 " + std::to_string(declared));
		EXPECT_EQ(DecodeOne(d, f, &got, &info), CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE);
		EXPECT_EQ(got.size(), 0u);
	}

	CLV_AudioStatsC st {};
	ASSERT_EQ(CLV_AudioDecoderStats(d, &st), CLV_AUDIO_OK);
	// 越界声明计入帧头自核的丢帧档，且不判整条流损坏
	EXPECT_EQ(st.frames_dropped_header, static_cast<uint64_t>(sizeof(above) / sizeof(above[0])));
	EXPECT_EQ(st.frames_ok, 0u);
	EXPECT_EQ(st.frames_dropped_mismatch, 0u);
	EXPECT_EQ(st.stream_rejected, 0);

	// 拒完仍可用：同一句柄把真帧解出来，PCM 逐字节等于源
	std::vector<uint8_t> got;
	CLV_AudioFrameInfoC info {};
	ASSERT_EQ(DecodeOne(d, es.frames[0], &got, &info), CLV_AUDIO_OK);
	EXPECT_EQ(got, pcm);
	CLV_AudioDecoderClose(d);
}

TEST(IndependentAudio, FrameTailCrcBitFlipDropsOnlyThatFrame)
{
	constexpr uint32_t kSamples = kBlocksize * 3u;
	const std::vector<uint8_t> pcm = MakePcm(kSamples);
	Encoded es;
	EncodeAll(pcm, kBlocksize, &es);
	ASSERT_EQ(es.frames.size(), WantFrames(kSamples));

	CLV_AudioError err = CLV_AUDIO_OK;
	CLV_AudioDecoder* d = OpenDecoder(es, &err);
	ASSERT_NE(d, nullptr) << CLV_AudioStrError(err);

	// 正确帧：读出 PCM，逐字节等于源的第一段
	std::vector<uint8_t> got;
	CLV_AudioFrameInfoC info {};
	ASSERT_EQ(DecodeOne(d, es.frames[0], &got, &info), CLV_AUDIO_OK);
	ASSERT_EQ(got.size(), FramePcmBytes());
	EXPECT_EQ(got, Slice(pcm, 0u, kBlocksize));
	EXPECT_EQ(info.blocksize, kBlocksize);
	EXPECT_EQ(info.first_sample, 0u);
	// 时间戳不经本口进来：三项恒 0（include/CLV_Audio.h 的 CLV_AudioFrameInfoC 条目）
	EXPECT_EQ(info.dts_ticks, 0);
	EXPECT_EQ(info.pts_ticks, 0);
	EXPECT_EQ(info.timebase_den, 0u);

	// 帧尾 CRC-16 翻一位：丢帧 + 计数 + 返码，三条都是现行处置
	std::vector<uint8_t> bad = es.frames[1];
	ASSERT_GT(bad.size(), 2u);
	bad[bad.size() - 1u] = static_cast<uint8_t>(bad[bad.size() - 1u] ^ 0x01u);
	EXPECT_FALSE(FrameCrcOk(bad.data(), bad.size()));

	CLV_AudioBuffer* out = nullptr;
	uint8_t got_flag = 1u;
	EXPECT_EQ(CLV_AudioDecoderDecodeFrame(d, bad.data(), bad.size(), &out, &got_flag, nullptr),
			  CLV_AUDIO_FRAME_HEADER_BAD);
	EXPECT_EQ(got_flag, 0);
	EXPECT_EQ(out, nullptr);

	CLV_AudioStatsC st {};
	ASSERT_EQ(CLV_AudioDecoderStats(d, &st), CLV_AUDIO_OK);
	EXPECT_EQ(st.frames_ok, 1u);
	EXPECT_EQ(st.frames_dropped_header, 1u);
	EXPECT_EQ(st.frames_dropped_mismatch, 0u);
	EXPECT_EQ(st.stream_rejected, 0);

	// 句柄仍可用：第三帧照常解出；样本轴只按实收帧累加，被丢的那帧不计入
	got.clear();
	info = CLV_AudioFrameInfoC {};
	ASSERT_EQ(DecodeOne(d, es.frames[2], &got, &info), CLV_AUDIO_OK);
	EXPECT_EQ(got, Slice(pcm, kBlocksize * 2u, kSamples));
	EXPECT_EQ(info.blocksize, kBlocksize);
	EXPECT_EQ(info.first_sample, static_cast<uint64_t>(kBlocksize));

	CLV_AudioDecoderClose(d);
}

TEST(IndependentAudio, LosslessCompareCatchesOneSampleFrameSkew)
{
	// 三满块 + 一个 100 样本的尾块：帧数 = ⌈(3 × 512 + 100) ÷ 512⌉ = 4
	constexpr uint32_t kSamples = kBlocksize * 3u + 100u;
	const std::vector<uint8_t> pcm = MakePcm(kSamples);
	Encoded es;
	EncodeAll(pcm, kBlocksize, &es);
	ASSERT_EQ(es.frames.size(), WantFrames(kSamples));

	CLV_AudioError err = CLV_AUDIO_OK;
	CLV_AudioDecoder* d = OpenDecoder(es, &err);
	ASSERT_NE(d, nullptr) << CLV_AudioStrError(err);

	std::vector<uint8_t> got;
	size_t dropped = 0u;
	for (const std::vector<uint8_t>& f: es.frames)
	{
		std::vector<uint8_t> one;
		if (DecodeOne(d, f, &one, nullptr) != CLV_AUDIO_OK || one.empty())
		{
			++dropped;
			continue;
		}
		got.insert(got.end(), one.begin(), one.end());
	}
	CLV_AudioDecoderClose(d);

	ASSERT_EQ(dropped, 0u);
	// 无损：逐字节等源
	ASSERT_EQ(got.size(), pcm.size());
	EXPECT_EQ(got, pcm);

	/* 对照档：期望整体错位一个采样帧（channels × ceil(bit_depth ÷ 8) 字节），环形移位保持等长。
	 * 这一档要红：判据写成「比较器必须判出不等」，红在断言本身而不是用例失败。 */
	const size_t step = SampleFrameBytes();
	std::vector<uint8_t> skewed(pcm.begin() + static_cast<long>(step), pcm.end());
	skewed.insert(skewed.end(), pcm.begin(), pcm.begin() + static_cast<long>(step));
	ASSERT_EQ(skewed.size(), got.size()) << "错位件必须与解码输出等长，否则 NE 只是长度差";
	EXPECT_NE(skewed, pcm) << "生成器周期过短：错位一个采样帧竟与源相同";
	EXPECT_NE(got, skewed) << "比较器抓不到一个采样帧的错位";
	// 差异点落在头部第一段：解码输出与源同、与错位件不同
	EXPECT_EQ(std::memcmp(got.data(), pcm.data(), step), 0);
	EXPECT_NE(got.front(), skewed.front());
}

TEST(IndependentAudio, OnlyFrameCrcGateRejectsFieldCleanFrame)
{
	constexpr uint32_t kSamples = kBlocksize * 2u;
	const std::vector<uint8_t> pcm = MakePcm(kSamples);
	Encoded es;
	EncodeAll(pcm, kBlocksize, &es);
	ASSERT_EQ(es.frames.size(), WantFrames(kSamples));

	const std::vector<uint8_t>& pristine = es.frames[0];

	// 「没护栏就会收」的那一类输入：帧头自洽、字段与流声明一致，坏的只有帧尾校验
	CLV_AudioFrameHeadC h {};
	ASSERT_EQ(CLV_AudioInspectFrame(pristine.data(), pristine.size(), &h), CLV_AUDIO_OK);
	const CLV_AudioStreamParamsC sp = StreamParams();
	ASSERT_EQ(h.sample_rate, sp.sample_rate);
	ASSERT_EQ(h.channels, sp.channels);
	ASSERT_EQ(h.bit_depth, sp.bit_depth);
	ASSERT_TRUE(FrameCrcOk(pristine.data(), pristine.size()));

	std::vector<uint8_t> bad = pristine;
	bad[bad.size() - 1u] = static_cast<uint8_t>(bad[bad.size() - 1u] ^ 0x01u);
	// 改动只在末两字节的校验值上：帧头与载荷逐字节没动
	ASSERT_EQ(std::memcmp(bad.data(), pristine.data(), pristine.size() - 2u), 0);
	CLV_AudioFrameHeadC h2 = SentinelHead();
	EXPECT_EQ(CLV_AudioInspectFrame(bad.data(), bad.size(), &h2), CLV_AUDIO_OK) << "帧头面不吃帧尾校验，这一档必须被收";
	EXPECT_EQ(h2.blocksize, h.blocksize);
	EXPECT_EQ(h2.sample_rate, h.sample_rate);
	EXPECT_EQ(h2.channels, h.channels);
	EXPECT_EQ(h2.bit_depth, h.bit_depth);
	EXPECT_EQ(h2.header_size, h.header_size);
	EXPECT_FALSE(FrameCrcOk(bad.data(), bad.size()));

	CLV_AudioError err = CLV_AUDIO_OK;
	CLV_AudioDecoder* d = OpenDecoder(es, &err);
	ASSERT_NE(d, nullptr) << CLV_AudioStrError(err);

	std::vector<uint8_t> got;
	// 对照：未改的那一帧先解出 PCM，逐字节等于源第一段
	ASSERT_EQ(DecodeOne(d, pristine, &got, nullptr), CLV_AUDIO_OK);
	EXPECT_EQ(got, Slice(pcm, 0u, kBlocksize));

	got.clear();
	EXPECT_EQ(DecodeOne(d, bad, &got, nullptr), CLV_AUDIO_FRAME_HEADER_BAD);
	EXPECT_EQ(got.size(), 0u);

	CLV_AudioStatsC st {};
	ASSERT_EQ(CLV_AudioDecoderStats(d, &st), CLV_AUDIO_OK);
	EXPECT_EQ(st.frames_ok, 1u);
	EXPECT_EQ(st.frames_dropped_header, 1u) << "拦住它的只有帧尾校验这一道";
	EXPECT_EQ(st.frames_dropped_mismatch, 0u) << "对账那一步不该被触及";
	EXPECT_EQ(st.stream_rejected, 0);

	// 只丢这一帧：第二帧照解，样本轴按实收帧累加
	got.clear();
	CLV_AudioFrameInfoC info {};
	ASSERT_EQ(DecodeOne(d, es.frames[1], &got, &info), CLV_AUDIO_OK);
	EXPECT_EQ(got, Slice(pcm, kBlocksize, kSamples));
	EXPECT_EQ(info.first_sample, static_cast<uint64_t>(kBlocksize));

	CLV_AudioDecoderClose(d);
}
