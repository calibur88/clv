/* test_audio_codec.cpp
 *
 * **两面混用**：端到端与句柄行为走公开 C ABI（`CLV_Audio*` 族 20 个入口、按「函数名 + 左括号」在本件正文里数得 122 处调用点，如 `CLV_AudioBlobGetBytes`、`CLV_AudioBlobRelease`、`CLV_AudioBufferAddRef`），
 *   单元级直测走实现层头（`audio/frame.h`）。**仅静态构建（`CLV_BUILD_SHARED=OFF`）下可链**的原因是后面这些头，
 *   不是「不经公开 ABI」——两面各写各的：主体过公开 C ABI，另有几处直调实现层函数钉住不进 ABI 的行为。
 *
 * 音频编解码闭环的用例：素材在本文件里按确定性算式生成，走完「编码 → 一包一帧 → 解码」之后
 * 与源 PCM 逐字节对账。FLAC 是无损的，所以这里的断言就是「字节等源」，不存在容差。
 *
 * 各断言组分别钉住闭环的一个行为点：帧数、逐帧块大小与样本号、最坏帧开销上限、拒解与丢帧计数。
 */

#include "CLV_Audio.h"
#include "audio/frame.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

	constexpr uint32_t kRate = 44100u;
	constexpr uint16_t kBlocksize = 1024u;
	constexpr size_t kPacketCap = 1u << 20;

	const uint8_t kStub[8] = { 0xFFu, 0xF8u, 0x22u, 0x40u, 0x00u, 0x00u, 0x00u, 0x00u };

	CLV_AudioEncParamsC EncParams(uint16_t channels, uint16_t bit_depth, uint16_t blocksize)
	{
		CLV_AudioEncParamsC p {};
		p.sample_rate = kRate;
		p.channels = channels;
		p.bit_depth = bit_depth;
		p.blocksize = blocksize;
		p.level = 5u;
		return p;
	}

	CLV_AudioStreamParamsC StreamParams(uint16_t channels, uint16_t bit_depth)
	{
		CLV_AudioStreamParamsC p {};
		p.sample_rate = kRate;
		p.channels = channels;
		p.bit_depth = bit_depth;
		p.max_packet_size = static_cast<uint32_t>(kPacketCap);
		return p;
	}

	/* 确定性正弦样片：整型域内自算，不依赖 libm，任何平台上重跑同值。 */
	std::vector<uint8_t> MakePcm(uint32_t samples_per_channel, uint16_t channels, uint16_t bit_depth)
	{
		const size_t bps = (bit_depth + 7u) / 8u;
		std::vector<uint8_t> pcm(static_cast<size_t>(samples_per_channel) * channels * bps, 0u);
		for (uint32_t s = 0u; s < samples_per_channel; ++s)
		{
			for (uint16_t c = 0u; c < channels; ++c)
			{
				const int32_t scale = bit_depth == 16u ? 20000 : 5000000;
				/* 三个不同频率的三角波叠加，周期取整到样本，保证可复算。 */
				const int32_t tri =
					static_cast<int32_t>(s % 37u) + static_cast<int32_t>(s % 53u) * 2 - static_cast<int32_t>(s % 91u);
				int32_t v = (tri * scale) / 160;
				if (c == 1u) v = -v;
				const size_t off = (static_cast<size_t>(s) * channels + c) * bps;
				const uint32_t u = static_cast<uint32_t>(static_cast<uint32_t>(v));
				pcm[off] = static_cast<uint8_t>(u & 0xFFu);
				pcm[off + 1u] = static_cast<uint8_t>((u >> 8) & 0xFFu);
				if (bps == 3u) pcm[off + 2u] = static_cast<uint8_t>((u >> 16) & 0xFFu);
			}
		}
		return pcm;
	}

	/* 近似不可压的伪随机素材（LCG），用于最坏帧开销上限的核对。 */
	std::vector<uint8_t> MakeNoisePcm(uint32_t samples_per_channel, uint16_t channels, uint16_t bit_depth)
	{
		const size_t bps = (bit_depth + 7u) / 8u;
		std::vector<uint8_t> pcm(static_cast<size_t>(samples_per_channel) * channels * bps, 0u);
		uint32_t x = 12345u;
		for (size_t i = 0u; i < pcm.size(); ++i)
		{
			x = x * 1664525u + 1013904223u;
			pcm[i] = static_cast<uint8_t>((x >> 16) & 0xFFu);
		}
		return pcm;
	}

	struct EncodedStream
	{
		std::vector<std::vector<uint8_t>> frames;
		std::vector<uint8_t> streaminfo;
	};

	/* 拉取式取帧：Write 全部喂完 → Flush → NextFrame 取到空为止。 */
	void EncodeAll(const CLV_AudioEncParamsC& ep, const std::vector<uint8_t>& pcm, uint32_t write_batch,
				   EncodedStream* out, CLV_AudioError* err)
	{
		out->frames.clear();
		out->streaminfo.clear();
		*err = CLV_AUDIO_OK;

		CLV_AudioEncoder* e = CLV_AudioEncoderOpen(&ep, err);
		ASSERT_NE(e, nullptr) << CLV_AudioStrError(*err);

		const size_t bps = (ep.bit_depth + 7u) / 8u;
		const size_t batch_bytes = static_cast<size_t>(write_batch) * ep.channels * bps;
		for (size_t off = 0u; off < pcm.size(); off += batch_bytes)
		{
			const size_t n = pcm.size() - off < batch_bytes ? pcm.size() - off : batch_bytes;
			const uint32_t sc = static_cast<uint32_t>(n / (ep.channels * bps));
			*err = CLV_AudioEncoderWrite(e, pcm.data() + off, n, sc);
			ASSERT_EQ(*err, CLV_AUDIO_OK) << CLV_AudioStrError(*err);
		}
		*err = CLV_AudioEncoderFlush(e);
		ASSERT_EQ(*err, CLV_AUDIO_OK) << CLV_AudioStrError(*err);

		for (;;)
		{
			const uint8_t* data = nullptr;
			size_t size = 0u;
			uint8_t got = 0u;
			*err = CLV_AudioEncoderNextFrame(e, &data, &size, &got);
			ASSERT_EQ(*err, CLV_AUDIO_OK) << CLV_AudioStrError(*err);
			if (got == 0u) break;
			out->frames.emplace_back(data, data + size);
		}

		CLV_AudioBlob* blob = nullptr;
		*err = CLV_AudioEncoderTakeStreamInfo(e, &blob);
		ASSERT_EQ(*err, CLV_AUDIO_OK) << CLV_AudioStrError(*err);
		const uint8_t* si = nullptr;
		size_t si_size = 0u;
		ASSERT_EQ(CLV_AudioBlobGetBytes(blob, &si, &si_size), CLV_AUDIO_OK);
		out->streaminfo.assign(si, si + si_size);
		CLV_AudioBlobRelease(blob);
		CLV_AudioEncoderClose(e);
	}

	/* 解码整条流，返回拼起来的 PCM；顺带交出逐帧 info，供块大小对账。 */
	std::vector<uint8_t> DecodeAll(const CLV_AudioStreamParamsC& sp, const EncodedStream& es,
								   std::vector<CLV_AudioFrameInfoC>* infos, size_t* failures)
	{
		std::vector<uint8_t> pcm;
		infos->clear();
		*failures = 0u;

		CLV_AudioError err = CLV_AUDIO_OK;
		CLV_AudioDecoder* d = CLV_AudioDecoderOpen(&sp, es.streaminfo.data(), es.streaminfo.size(), &err);
		EXPECT_NE(d, nullptr) << CLV_AudioStrError(err);
		if (d == nullptr) return pcm;

		for (const auto& f: es.frames)
		{
			CLV_AudioBuffer* buf = nullptr;
			uint8_t got = 0u;
			CLV_AudioFrameInfoC info {};
			err = CLV_AudioDecoderDecodeFrame(d, f.data(), f.size(), &buf, &got, &info);
			if (err != CLV_AUDIO_OK || got == 0u)
			{
				++*failures;
				if (buf != nullptr) CLV_AudioBufferRelease(buf);
				continue;
			}
			CLV_AudioBufferDescC desc {};
			EXPECT_EQ(CLV_AudioBufferGetDesc(buf, &desc), CLV_AUDIO_OK);
			pcm.insert(pcm.end(), desc.data, desc.data + desc.size);
			infos->push_back(info);
			CLV_AudioBufferRelease(buf);
		}
		CLV_AudioDecoderClose(d);
		return pcm;
	}

}	 // namespace

TEST(AudioCodec, RoundTripIsByteEqualFor16BitStereo)
{
	const auto ep = EncParams(2u, 16u, kBlocksize);
	const auto sp = StreamParams(2u, 16u);
	const auto pcm = MakePcm(kBlocksize * 4u, 2u, 16u);

	EncodedStream es;
	CLV_AudioError err = CLV_AUDIO_OK;
	EncodeAll(ep, pcm, kBlocksize * 2u, &es, &err);
	ASSERT_EQ(err, CLV_AUDIO_OK);
	ASSERT_EQ(es.frames.size(), 4u);

	std::vector<CLV_AudioFrameInfoC> infos;
	size_t failures = 0u;
	const auto back = DecodeAll(sp, es, &infos, &failures);
	EXPECT_EQ(failures, 0u);
	EXPECT_EQ(back, pcm);
}

TEST(AudioCodec, RoundTripIsByteEqualFor24BitStereo)
{
	const auto ep = EncParams(2u, 24u, kBlocksize);
	const auto sp = StreamParams(2u, 24u);
	const auto pcm = MakePcm(kBlocksize * 3u, 2u, 24u);

	EncodedStream es;
	CLV_AudioError err = CLV_AUDIO_OK;
	EncodeAll(ep, pcm, kBlocksize, &es, &err);
	ASSERT_EQ(err, CLV_AUDIO_OK);

	std::vector<CLV_AudioFrameInfoC> infos;
	size_t failures = 0u;
	const auto back = DecodeAll(sp, es, &infos, &failures);
	EXPECT_EQ(failures, 0u);
	EXPECT_EQ(back.size(), pcm.size());
	EXPECT_EQ(back, pcm);
}

TEST(AudioCodec, RoundTripIsByteEqualForMonoAndOddBatchSizes)
{
	const auto ep = EncParams(1u, 16u, kBlocksize);
	const auto sp = StreamParams(1u, 16u);
	const auto pcm = MakePcm(2000u, 1u, 16u);

	EncodedStream es;
	CLV_AudioError err = CLV_AUDIO_OK;
	EncodeAll(ep, pcm, 300u, &es, &err);	// 每次喂的都不是块大小的整数倍
	ASSERT_EQ(err, CLV_AUDIO_OK);

	std::vector<CLV_AudioFrameInfoC> infos;
	size_t failures = 0u;
	const auto back = DecodeAll(sp, es, &infos, &failures);
	EXPECT_EQ(failures, 0u);
	EXPECT_EQ(back, pcm);
}

/* 一包一帧。帧数由样本数与块大小算出，每帧都能独立过帧头自检。 */
TEST(AudioCodec, OneFramePerPacketAndEachFrameSelfParses)
{
	const auto ep = EncParams(2u, 16u, kBlocksize);
	const uint32_t total = kBlocksize * 5u + 111u;	  // 故意留一个不完整的尾块
	const auto pcm = MakePcm(total, 2u, 16u);

	EncodedStream es;
	CLV_AudioError err = CLV_AUDIO_OK;
	EncodeAll(ep, pcm, kBlocksize, &es, &err);
	ASSERT_EQ(err, CLV_AUDIO_OK);

	const size_t want = (total + kBlocksize - 1u) / kBlocksize;	   // ceil(6331 / 1024) = 7
	ASSERT_EQ(es.frames.size(), want);

	uint32_t samples_seen = 0u;
	for (const auto& f: es.frames)
	{
		CLV_AudioFrameHeadC h {};
		ASSERT_EQ(CLV_AudioInspectFrame(f.data(), f.size(), &h), CLV_AUDIO_OK);
		EXPECT_EQ(h.sample_rate, kRate);
		EXPECT_EQ(h.channels, 2u);
		EXPECT_EQ(h.bit_depth, 16u);
		samples_seen += h.blocksize;
	}
	EXPECT_EQ(samples_seen, total);
}

/* 逐帧块大小实跑对账。尾帧的块大小就是余数，样本号按实测块大小累加。 */
TEST(AudioCodec, PerFrameBlocksizeReconcilesWithTailRemainder)
{
	const auto ep = EncParams(2u, 16u, kBlocksize);
	const uint32_t total = kBlocksize * 3u + 250u;
	const auto pcm = MakePcm(total, 2u, 16u);
	const auto sp = StreamParams(2u, 16u);

	EncodedStream es;
	CLV_AudioError err = CLV_AUDIO_OK;
	EncodeAll(ep, pcm, kBlocksize, &es, &err);
	ASSERT_EQ(err, CLV_AUDIO_OK);
	ASSERT_EQ(es.frames.size(), 4u);

	std::vector<CLV_AudioFrameInfoC> infos;
	size_t failures = 0u;
	const auto back = DecodeAll(sp, es, &infos, &failures);
	ASSERT_EQ(failures, 0u);
	ASSERT_EQ(infos.size(), 4u);

	uint64_t expect_first = 0u;
	for (size_t i = 0u; i < infos.size(); ++i)
	{
		CLV_AudioFrameHeadC h {};
		ASSERT_EQ(CLV_AudioInspectFrame(es.frames[i].data(), es.frames[i].size(), &h), CLV_AUDIO_OK);
		const uint32_t tail = (i + 1u == infos.size()) ? 250u : kBlocksize;
		EXPECT_EQ(infos[i].blocksize, tail) << "frame " << i;
		EXPECT_EQ(h.blocksize, tail) << "frame " << i;
		EXPECT_EQ(infos[i].first_sample, expect_first) << "frame " << i;
		expect_first += tail;
	}
	EXPECT_EQ(expect_first, total);
	EXPECT_EQ(back, pcm);

	/* 流信息里的 min / max 块大小必须是实测值，不是声明值。 */
	CLV_AudioStreamInfoC si {};
	ASSERT_EQ(CLV_AudioParseStreamInfo(es.streaminfo.data(), es.streaminfo.size(), &si), CLV_AUDIO_OK);
	EXPECT_EQ(si.min_blocksize, 250u);
	EXPECT_EQ(si.max_blocksize, kBlocksize);
	EXPECT_EQ(si.total_samples, total);
	EXPECT_GT(si.min_framesize, 0u);
	EXPECT_GE(si.max_framesize, si.min_framesize);
}

/* 流信息与流声明互相矛盾 → 拒解整条音频流。
 * 致命流问题交出拒解态句柄（非 nullptr），参数不成立才交 nullptr：坏流给句柄、非法入参给 null。 */
TEST(AudioCodec, StreamInfoContradictingDeclaredParamsRejectsOpen)
{
	const auto ep = EncParams(2u, 16u, kBlocksize);
	const auto pcm = MakePcm(kBlocksize * 2u, 2u, 16u);

	EncodedStream es;
	CLV_AudioError err = CLV_AUDIO_OK;
	EncodeAll(ep, pcm, kBlocksize, &es, &err);
	ASSERT_EQ(err, CLV_AUDIO_OK);

	/* 两条矛盾（采样率 / 位深）逐条钉：句柄非空 + err = STREAM_REJECTED + 后续调用粘滞 + 一帧不出。 */
	CLV_AudioStreamParamsC contradictory[2];
	contradictory[0] = StreamParams(2u, 16u);
	contradictory[0].sample_rate = 48000u;		 // 与 STREAMINFO 里的 44100 矛盾
	contradictory[1] = StreamParams(2u, 24u);	 // 位深矛盾
	for (const CLV_AudioStreamParamsC& sp: contradictory)
	{
		err = CLV_AUDIO_OK;
		CLV_AudioDecoder* d = CLV_AudioDecoderOpen(&sp, es.streaminfo.data(), es.streaminfo.size(), &err);
		ASSERT_NE(d, nullptr) << "坏流要给句柄，不给 nullptr";
		EXPECT_EQ(err, CLV_AUDIO_STREAM_REJECTED);

		// 粘滞：每个解码口都返 STREAM_REJECTED，且 NextOutput 出帧数 = 0
		CLV_AudioBuffer* buf = nullptr;
		uint8_t got = 1u;
		CLV_AudioFrameInfoC info {};
		EXPECT_EQ(CLV_AudioDecoderDecodeFrame(d, es.frames.front().data(), es.frames.front().size(), &buf, &got, &info),
				  CLV_AUDIO_STREAM_REJECTED);
		EXPECT_EQ(buf, nullptr);
		EXPECT_EQ(got, 0u);
		// 拒解态先于入参判定：载荷为空也仍是 STREAM_REJECTED
		EXPECT_EQ(CLV_AudioDecoderDecodeFrame(d, nullptr, 0u, &buf, &got, &info), CLV_AUDIO_STREAM_REJECTED);
		EXPECT_EQ(CLV_AudioDecoderAddMetaBlock(d, es.streaminfo.data(), es.streaminfo.size()),
				  CLV_AUDIO_STREAM_REJECTED);

		// 观测口不被拦截：拒解态如实从这里读出
		CLV_AudioStatsC st {};
		EXPECT_EQ(CLV_AudioDecoderStats(d, &st), CLV_AUDIO_OK);
		EXPECT_EQ(st.stream_rejected, 1u);
		EXPECT_EQ(st.frames_ok, 0u);

		// 冒烟：Close 之后同配置仍能再 Open（句柄可重建）。回收在这套 ABI 上没有可观测出口，
		// 这一档不充当无泄漏判据——任何小于可用内存的泄漏都照样通过，不假装在这里看到了它。
		CLV_AudioDecoderClose(d);
		err = CLV_AUDIO_OK;
		CLV_AudioDecoder* again = CLV_AudioDecoderOpen(&sp, es.streaminfo.data(), es.streaminfo.size(), &err);
		ASSERT_NE(again, nullptr) << "Close 之后同配置应能再 Open（只验句柄可重建）";
		EXPECT_EQ(err, CLV_AUDIO_STREAM_REJECTED);
		CLV_AudioDecoderClose(again);
	}

	// 缺流信息 / 长度不对：是另一条致命码，同样给拒解态句柄（不是 nullptr）
	err = CLV_AUDIO_OK;
	CLV_AudioDecoder* d_none = CLV_AudioDecoderOpen(&contradictory[0], nullptr, 0u, &err);
	ASSERT_NE(d_none, nullptr);
	EXPECT_EQ(err, CLV_AUDIO_STREAMINFO_ABSENT);
	CLV_AudioDecoderClose(d_none);
	err = CLV_AUDIO_OK;
	CLV_AudioDecoder* d_short = CLV_AudioDecoderOpen(&contradictory[0], es.streaminfo.data(), 33u, &err);
	ASSERT_NE(d_short, nullptr);
	EXPECT_EQ(err, CLV_AUDIO_STREAMINFO_ABSENT);
	CLV_AudioDecoderClose(d_short);

	// 参数不成立仍是 nullptr：那是调用方的事，不是坏流
	CLV_AudioStreamParamsC zero = StreamParams(2u, 16u);
	zero.sample_rate = 0u;
	err = CLV_AUDIO_OK;
	EXPECT_EQ(CLV_AudioDecoderOpen(&zero, es.streaminfo.data(), es.streaminfo.size(), &err), nullptr);
	EXPECT_EQ(err, CLV_AUDIO_VALUE_RANGE);
}

/* 未知块类型 warn + 跳过 + 计数，不判损坏；填充块与混进来的流信息块各有去向。 */
TEST(AudioCodec, MetaBlockActionsAreCountedPerKind)
{
	const auto ep = EncParams(2u, 16u, kBlocksize);
	const auto sp = StreamParams(2u, 16u);
	const auto pcm = MakePcm(kBlocksize, 2u, 16u);

	EncodedStream es;
	CLV_AudioError err = CLV_AUDIO_OK;
	EncodeAll(ep, pcm, kBlocksize, &es, &err);
	ASSERT_EQ(err, CLV_AUDIO_OK);

	CLV_AudioDecoder* d = CLV_AudioDecoderOpen(&sp, es.streaminfo.data(), es.streaminfo.size(), &err);
	ASSERT_NE(d, nullptr) << CLV_AudioStrError(err);

	const std::vector<uint8_t> payload(8u, 0xABu);
	auto block = [&](uint8_t type)
	{
		std::vector<uint8_t> v(1u, type);
		v.insert(v.end(), payload.begin(), payload.end());
		return v;
	};

	const uint8_t unknown7 = 7u;
	const uint8_t unknown9 = 9u;
	for (uint8_t t: { unknown7, unknown9 })
	{
		const auto b = block(t);
		EXPECT_EQ(CLV_AudioDecoderAddMetaBlock(d, b.data(), b.size()), CLV_AUDIO_BLOCK_TYPE_UNSUPPORTED)
			<< "type " << t;
	}
	const auto padding = block(1u);
	EXPECT_EQ(CLV_AudioDecoderAddMetaBlock(d, padding.data(), padding.size()), CLV_AUDIO_OK);
	const auto inside = block(0u);	  // 承载块里不该出现 STREAMINFO
	EXPECT_EQ(CLV_AudioDecoderAddMetaBlock(d, inside.data(), inside.size()), CLV_AUDIO_METADATA_BAD);
	const auto seektable = block(3u);
	EXPECT_EQ(CLV_AudioDecoderAddMetaBlock(d, seektable.data(), seektable.size()), CLV_AUDIO_OK);

	CLV_AudioStatsC st {};
	ASSERT_EQ(CLV_AudioDecoderStats(d, &st), CLV_AUDIO_OK);
	EXPECT_EQ(st.blocks_skipped_unknown, 2u);
	EXPECT_EQ(st.blocks_dropped_padding, 1u);
	EXPECT_EQ(st.blocks_rejected_streaminfo, 1u);
	EXPECT_EQ(st.blocks_stored, 1u);
	EXPECT_EQ(st.stream_rejected, 0u);

	// 三种处置都不判损坏：同一句柄照常把帧解出来
	CLV_AudioBuffer* buf = nullptr;
	uint8_t got = 0u;
	EXPECT_EQ(CLV_AudioDecoderDecodeFrame(d, es.frames[0].data(), es.frames[0].size(), &buf, &got, nullptr),
			  CLV_AUDIO_OK);
	ASSERT_EQ(got, 1u);
	CLV_AudioBufferRelease(buf);
	CLV_AudioDecoderClose(d);
}

/* 最坏帧开销复核 payload 上限公式（PCM 尺寸 + 64）。 */
TEST(AudioCodec, WorstCaseFrameFitsThePlus64PayloadBound)
{
	const uint16_t bps = 2u;
	const auto ep = EncParams(2u, 16u, kBlocksize);
	const auto pcm = MakeNoisePcm(kBlocksize * 3u, 2u, 16u);

	EncodedStream es;
	CLV_AudioError err = CLV_AUDIO_OK;
	EncodeAll(ep, pcm, kBlocksize, &es, &err);
	ASSERT_EQ(err, CLV_AUDIO_OK);

	const size_t bound = static_cast<size_t>(kBlocksize) * ep.channels * bps + 64u;
	size_t worst = 0u;
	for (const auto& f: es.frames)
	{
		EXPECT_LE(f.size(), bound) << "帧 " << f.size() << " B 越出上限 " << bound << " B";
		worst = worst > f.size() ? worst : f.size();
	}
	// 噪声素材上 verbatim 兜底路径必然被走到：最坏帧 ≈ PCM 尺寸 + 个位数开销
	const size_t pcm_bytes = static_cast<size_t>(kBlocksize) * ep.channels * bps;
	EXPECT_GE(worst, pcm_bytes);
	EXPECT_LE(worst, pcm_bytes + 64u);
	RecordProperty("worst_noise_frame_bytes", static_cast<int>(worst));
	RecordProperty("pcm_bytes_per_frame", static_cast<int>(pcm_bytes));
}

/* 输出布局。交错、每样本字节数 = ceil(bit_depth ÷ 8)、缓冲由库拥有。 */
TEST(AudioCodec, OutputBufferIsInterleavedWithCeilBytesPerSample)
{
	const auto ep = EncParams(2u, 24u, kBlocksize);
	const auto sp = StreamParams(2u, 24u);
	const auto pcm = MakePcm(kBlocksize, 2u, 24u);

	EncodedStream es;
	CLV_AudioError err = CLV_AUDIO_OK;
	EncodeAll(ep, pcm, kBlocksize, &es, &err);
	ASSERT_EQ(err, CLV_AUDIO_OK);

	CLV_AudioDecoder* d = CLV_AudioDecoderOpen(&sp, es.streaminfo.data(), es.streaminfo.size(), &err);
	ASSERT_NE(d, nullptr) << CLV_AudioStrError(err);

	CLV_AudioBuffer* buf = nullptr;
	uint8_t got = 0u;
	ASSERT_EQ(CLV_AudioDecoderDecodeFrame(d, es.frames[0].data(), es.frames[0].size(), &buf, &got, nullptr),
			  CLV_AUDIO_OK);
	ASSERT_EQ(got, 1u);

	CLV_AudioBufferDescC desc {};
	ASSERT_EQ(CLV_AudioBufferGetDesc(buf, &desc), CLV_AUDIO_OK);
	EXPECT_EQ(desc.format, CLV_AUDIO_FMT_INTERLEAVED);
	EXPECT_EQ(desc.bytes_per_sample, 3u);
	EXPECT_EQ(desc.bit_depth, 24u);
	EXPECT_EQ(desc.channels, 2u);
	EXPECT_EQ(desc.sample_rate, kRate);
	EXPECT_EQ(desc.samples_per_channel, kBlocksize);
	EXPECT_EQ(desc.size, static_cast<size_t>(kBlocksize) * 2u * 3u);

	// 首样本逐声道值直接从交错字节里按 3 字节小端读回，与源同一位置比对
	const int32_t s0c0 =
		static_cast<int32_t>(static_cast<uint32_t>(desc.data[0]) | (static_cast<uint32_t>(desc.data[1]) << 8) |
							 (static_cast<uint32_t>(desc.data[2]) << 16)) >>
		8;
	const int32_t src0 = static_cast<int32_t>(static_cast<uint32_t>(pcm[0]) | (static_cast<uint32_t>(pcm[1]) << 8) |
											  (static_cast<uint32_t>(pcm[2]) << 16)) >>
						 8;
	EXPECT_EQ(s0c0, src0);

	EXPECT_EQ(CLV_AudioBufferAddRef(buf), 2u);
	CLV_AudioBufferRelease(buf);	// 2 → 1
	CLV_AudioBufferRelease(buf);	// 1 → 0，此处才真正释放
	CLV_AudioDecoderClose(d);
}

/* 帧头改坏一处对账字段：丢帧 + 计数，句柄不判整条流损坏，后续帧照常。 */
TEST(AudioCodec, FrameHeaderSelfCheckRejectsTamperedHeader)
{
	const auto ep = EncParams(2u, 16u, kBlocksize);
	const auto sp = StreamParams(2u, 16u);
	const auto pcm = MakePcm(kBlocksize * 2u, 2u, 16u);

	EncodedStream es;
	CLV_AudioError err = CLV_AUDIO_OK;
	EncodeAll(ep, pcm, kBlocksize, &es, &err);
	ASSERT_EQ(err, CLV_AUDIO_OK);
	ASSERT_EQ(es.frames.size(), 2u);

	auto bad = es.frames[0];
	bad[3] = static_cast<uint8_t>(bad[3] | 1u);	   // 位深码之后那个必须为 0 的保留位

	CLV_AudioDecoder* d = CLV_AudioDecoderOpen(&sp, es.streaminfo.data(), es.streaminfo.size(), &err);
	ASSERT_NE(d, nullptr) << CLV_AudioStrError(err);

	CLV_AudioBuffer* buf = nullptr;
	uint8_t got = 1u;
	EXPECT_EQ(CLV_AudioDecoderDecodeFrame(d, bad.data(), bad.size(), &buf, &got, nullptr), CLV_AUDIO_FRAME_HEADER_BAD);
	EXPECT_EQ(got, 0u);
	EXPECT_EQ(buf, nullptr);

	// 只丢这一帧：同一句柄下一帧照常出
	buf = nullptr;
	got = 0u;
	EXPECT_EQ(CLV_AudioDecoderDecodeFrame(d, es.frames[1].data(), es.frames[1].size(), &buf, &got, nullptr),
			  CLV_AUDIO_OK);
	EXPECT_EQ(got, 1u);

	CLV_AudioStatsC st {};
	ASSERT_EQ(CLV_AudioDecoderStats(d, &st), CLV_AUDIO_OK);
	EXPECT_EQ(st.frames_ok, 1u);
	EXPECT_EQ(st.frames_dropped_header, 1u);
	EXPECT_EQ(st.frames_dropped_mismatch, 0u);
	EXPECT_EQ(st.stream_rejected, 0u);
	CLV_AudioBufferRelease(buf);
	CLV_AudioDecoderClose(d);
}

TEST(AudioCodec, FrameHeaderContradictingStreamDeclarationIsDropped)
{
	const auto ep = EncParams(2u, 16u, kBlocksize);
	const auto sp = StreamParams(2u, 16u);
	const auto pcm = MakePcm(kBlocksize * 2u, 2u, 16u);

	EncodedStream es;
	CLV_AudioError err = CLV_AUDIO_OK;
	EncodeAll(ep, pcm, kBlocksize, &es, &err);
	ASSERT_EQ(err, CLV_AUDIO_OK);
	ASSERT_EQ(es.frames.size(), 2u);

	// 声道码改成 3 声道（码 2），帧头 CRC-8 与帧尾 CRC-16 一起改对：两档自核都放过，对账必须拦
	CLV_AudioFrameHeadC h {};
	ASSERT_EQ(CLV_AudioInspectFrame(es.frames[0].data(), es.frames[0].size(), &h), CLV_AUDIO_OK);
	ASSERT_EQ(h.channels, 2u);
	ASSERT_GT(h.header_size, 3u);
	ASSERT_GT(es.frames[0].size(), 3u);

	auto bad = es.frames[0];
	bad[3] = static_cast<uint8_t>((bad[3] & 0x0Fu) | (2u << 4u));
	bad[h.header_size - 1u] = clv::audio::Crc8(bad.data(), h.header_size - 1u);
	// 帧尾 CRC-16 覆盖除校验值自身的整帧，改帧头必须跟着重算，否则这帧先撞自核档、对账档不被触及
	const uint16_t tail = clv::audio::Crc16(bad.data(), bad.size() - 2u);
	bad[bad.size() - 2u] = static_cast<uint8_t>(tail >> 8u);
	bad[bad.size() - 1u] = static_cast<uint8_t>(tail & 0xFFu);

	CLV_AudioFrameHeadC back {};
	ASSERT_EQ(CLV_AudioInspectFrame(bad.data(), bad.size(), &back), CLV_AUDIO_OK);
	ASSERT_EQ(back.channels, 3u);

	CLV_AudioDecoder* d = CLV_AudioDecoderOpen(&sp, es.streaminfo.data(), es.streaminfo.size(), &err);
	ASSERT_NE(d, nullptr) << CLV_AudioStrError(err);

	CLV_AudioBuffer* buf = nullptr;
	uint8_t got = 1u;
	EXPECT_EQ(CLV_AudioDecoderDecodeFrame(d, bad.data(), bad.size(), &buf, &got, nullptr), CLV_AUDIO_FIELD_MISMATCH);
	EXPECT_EQ(got, 0u);
	EXPECT_EQ(buf, nullptr);

	buf = nullptr;
	got = 0u;
	EXPECT_EQ(CLV_AudioDecoderDecodeFrame(d, es.frames[1].data(), es.frames[1].size(), &buf, &got, nullptr),
			  CLV_AUDIO_OK);
	EXPECT_EQ(got, 1u);

	CLV_AudioStatsC st {};
	ASSERT_EQ(CLV_AudioDecoderStats(d, &st), CLV_AUDIO_OK);
	EXPECT_EQ(st.frames_ok, 1u);
	EXPECT_EQ(st.frames_dropped_mismatch, 1u);
	EXPECT_EQ(st.frames_dropped_header, 0u);
	EXPECT_EQ(st.stream_rejected, 0u);
	CLV_AudioBufferRelease(buf);
	CLV_AudioDecoderClose(d);
}

/* 帧尾 CRC-16 与帧内容不符：帧头自核放过、CRC-16 拦下，按坏帧头档丢帧，不进对账档，后续帧照常。 */
TEST(AudioCodec, FrameTailCrc16MismatchIsDroppedAsBadHeader)
{
	const auto ep = EncParams(2u, 16u, kBlocksize);
	const auto sp = StreamParams(2u, 16u);
	const auto pcm = MakePcm(kBlocksize * 2u, 2u, 16u);

	EncodedStream es;
	CLV_AudioError err = CLV_AUDIO_OK;
	EncodeAll(ep, pcm, kBlocksize, &es, &err);
	ASSERT_EQ(err, CLV_AUDIO_OK);
	ASSERT_EQ(es.frames.size(), 2u);

	// 帧头与载荷一字不改，只把帧尾的校验值改坏：坏的是校验，不是字段
	auto bad = es.frames[0];
	ASSERT_GT(bad.size(), 8u);
	bad[bad.size() - 1u] = static_cast<uint8_t>(bad[bad.size() - 1u] ^ 0xFFu);

	// 帧头面自核仍然放过（InspectFrame 不吃帧尾 CRC-16）：拦下这帧的只有 CRC-16 那一道
	CLV_AudioFrameHeadC back {};
	ASSERT_EQ(CLV_AudioInspectFrame(bad.data(), bad.size(), &back), CLV_AUDIO_OK);
	EXPECT_EQ(back.channels, 2u);
	EXPECT_FALSE(clv::audio::FrameCrcOk(bad.data(), bad.size()));

	CLV_AudioDecoder* d = CLV_AudioDecoderOpen(&sp, es.streaminfo.data(), es.streaminfo.size(), &err);
	ASSERT_NE(d, nullptr) << CLV_AudioStrError(err);

	CLV_AudioBuffer* buf = nullptr;
	uint8_t got = 1u;
	EXPECT_EQ(CLV_AudioDecoderDecodeFrame(d, bad.data(), bad.size(), &buf, &got, nullptr), CLV_AUDIO_FRAME_HEADER_BAD);
	EXPECT_EQ(got, 0u);
	EXPECT_EQ(buf, nullptr);

	buf = nullptr;
	got = 0u;
	EXPECT_EQ(CLV_AudioDecoderDecodeFrame(d, es.frames[1].data(), es.frames[1].size(), &buf, &got, nullptr),
			  CLV_AUDIO_OK);
	EXPECT_EQ(got, 1u);

	CLV_AudioStatsC st {};
	ASSERT_EQ(CLV_AudioDecoderStats(d, &st), CLV_AUDIO_OK);
	EXPECT_EQ(st.frames_ok, 1u);
	EXPECT_EQ(st.frames_dropped_header, 1u);
	EXPECT_EQ(st.frames_dropped_mismatch, 0u);
	EXPECT_EQ(st.stream_rejected, 0u);
	CLV_AudioBufferRelease(buf);
	CLV_AudioDecoderClose(d);
}

/* Reset 清样本累加基准，统计计数保留。 */
TEST(AudioCodec, ResetRestartsSampleAxisAndKeepsStats)
{
	const auto ep = EncParams(2u, 16u, kBlocksize);
	const auto sp = StreamParams(2u, 16u);
	const auto pcm = MakePcm(kBlocksize * 3u + 250u, 2u, 16u);

	EncodedStream es;
	CLV_AudioError err = CLV_AUDIO_OK;
	EncodeAll(ep, pcm, kBlocksize, &es, &err);
	ASSERT_EQ(err, CLV_AUDIO_OK);
	ASSERT_EQ(es.frames.size(), 4u);

	CLV_AudioDecoder* d = CLV_AudioDecoderOpen(&sp, es.streaminfo.data(), es.streaminfo.size(), &err);
	ASSERT_NE(d, nullptr) << CLV_AudioStrError(err);

	for (int round = 0; round < 2; ++round)
	{
		uint64_t expect = 0u;
		for (size_t i = 0u; i < es.frames.size(); ++i)
		{
			CLV_AudioBuffer* buf = nullptr;
			uint8_t got = 0u;
			CLV_AudioFrameInfoC info {};
			ASSERT_EQ(CLV_AudioDecoderDecodeFrame(d, es.frames[i].data(), es.frames[i].size(), &buf, &got, &info),
					  CLV_AUDIO_OK)
				<< "round " << round << " frame " << i;
			EXPECT_EQ(info.first_sample, expect) << "round " << round << " frame " << i;
			expect += info.blocksize;
			CLV_AudioBufferRelease(buf);
		}
		EXPECT_EQ(expect, static_cast<uint64_t>(kBlocksize) * 3u + 250u) << "round " << round;
		ASSERT_EQ(CLV_AudioDecoderReset(d), CLV_AUDIO_OK);
	}

	CLV_AudioStatsC st {};
	ASSERT_EQ(CLV_AudioDecoderStats(d, &st), CLV_AUDIO_OK);
	EXPECT_EQ(st.frames_ok, 8u);	// Reset 只清样本轴与在途状态，计数保留
	CLV_AudioDecoderClose(d);
}

TEST(AudioCodec, NullHandlesReportHandleErrors)
{
	CLV_AudioBuffer* buf = nullptr;
	uint8_t got = 1u;
	EXPECT_EQ(CLV_AudioDecoderDecodeFrame(nullptr, kStub, 8u, &buf, &got, nullptr), CLV_AUDIO_INVALID_HANDLE);
	EXPECT_EQ(buf, nullptr);
	EXPECT_EQ(got, 0u);

	const uint8_t* data = reinterpret_cast<const uint8_t*>(0x1u);
	size_t size = 1u;
	EXPECT_EQ(CLV_AudioEncoderNextFrame(nullptr, &data, &size, &got), CLV_AUDIO_INVALID_HANDLE);
	CLV_AudioStatsC stats {};
	EXPECT_EQ(CLV_AudioDecoderStats(nullptr, &stats), CLV_AUDIO_INVALID_HANDLE);
	EXPECT_EQ(CLV_AudioDecoderStats(nullptr, nullptr), CLV_AUDIO_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_AudioDecoderAddMetaBlock(nullptr, kStub, 8u), CLV_AUDIO_INVALID_HANDLE);
	EXPECT_EQ(CLV_AudioDecoderReset(nullptr), CLV_AUDIO_INVALID_HANDLE);
	CLV_AudioBlob* blob = reinterpret_cast<CLV_AudioBlob*>(0x1u);
	EXPECT_EQ(CLV_AudioEncoderTakeStreamInfo(nullptr, &blob), CLV_AUDIO_INVALID_HANDLE);
	EXPECT_EQ(blob, nullptr);
	CLV_AudioBufferDescC desc {};
	EXPECT_EQ(CLV_AudioBufferGetDesc(nullptr, &desc), CLV_AUDIO_INVALID_HANDLE);
	EXPECT_EQ(CLV_AudioBufferAddRef(nullptr), 0u);
	CLV_AudioBufferRelease(nullptr);
	CLV_AudioEncoderClose(nullptr);
	CLV_AudioDecoderClose(nullptr);
}

TEST(AudioCodec, EncoderOpenRejectsOutOfSubsetParams)
{
	CLV_AudioError err = CLV_AUDIO_OK;

	CLV_AudioEncParamsC bad_bits = EncParams(2u, 16u, kBlocksize);
	bad_bits.bit_depth = 8u;
	EXPECT_EQ(CLV_AudioEncoderOpen(&bad_bits, &err), nullptr);
	EXPECT_EQ(err, CLV_AUDIO_VALUE_RANGE);

	CLV_AudioEncParamsC bad_channels = EncParams(9u, 16u, kBlocksize);
	EXPECT_EQ(CLV_AudioEncoderOpen(&bad_channels, &err), nullptr);
	EXPECT_EQ(err, CLV_AUDIO_VALUE_RANGE);

	CLV_AudioEncParamsC bad_rate = EncParams(2u, 16u, kBlocksize);
	bad_rate.sample_rate = 0u;
	EXPECT_EQ(CLV_AudioEncoderOpen(&bad_rate, &err), nullptr);
	EXPECT_EQ(err, CLV_AUDIO_VALUE_RANGE);

	/* 块大小四道界全部取自 src/audio/frame.h 的具名常量，本件不再手抄界值：
	 * 下界按位深分档（16-bit 取 kSubsetMinBlocksize16 之下一档；24-bit 档用 16-bit 那一界本身，
	 * 证明下界随位深走而不是全局一个值），上界两档取两界 + 1。 */
	CLV_AudioEncParamsC small_block = EncParams(2u, 16u, static_cast<uint16_t>(clv::audio::kSubsetMinBlocksize16 - 1u));
	EXPECT_EQ(CLV_AudioEncoderOpen(&small_block, &err), nullptr);
	EXPECT_EQ(err, CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE);

	CLV_AudioEncParamsC block24_at_16bit_floor =
		EncParams(2u, 24u, static_cast<uint16_t>(clv::audio::kSubsetMinBlocksize16));
	EXPECT_EQ(CLV_AudioEncoderOpen(&block24_at_16bit_floor, &err), nullptr);
	EXPECT_EQ(err, CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE);

	CLV_AudioEncParamsC block24_below =
		EncParams(2u, 24u, static_cast<uint16_t>(clv::audio::kSubsetMinBlocksize24 - 1u));
	EXPECT_EQ(CLV_AudioEncoderOpen(&block24_below, &err), nullptr);
	EXPECT_EQ(err, CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE);

	/* 上界两档：块大小 ≤ kSubsetMaxBlocksize，且采样率 ≤ kSubsetRateThresholdForShortBlock 时还要
	 * ≤ kSubsetMaxBlocksizeAtLowRate。这两道界必须在本库入口判——值一旦流到后端，
	 * 越界块大小只以 BACKEND_FAILURE 收场，调用方分不到本码。 */
	CLV_AudioEncParamsC over_lowrate = EncParams(
		2u, 16u, static_cast<uint16_t>(clv::audio::kSubsetMaxBlocksizeAtLowRate + 1u));	   // 44100 Hz ⇒ 撞低速率那一界
	EXPECT_EQ(CLV_AudioEncoderOpen(&over_lowrate, &err), nullptr);
	EXPECT_EQ(err, CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE);

	CLV_AudioEncParamsC over_hirate = EncParams(2u, 16u, static_cast<uint16_t>(clv::audio::kSubsetMaxBlocksize + 1u));
	over_hirate.sample_rate = 96000u;	 // 高于 kSubsetRateThresholdForShortBlock ⇒ 只受总上界约束
	EXPECT_EQ(CLV_AudioEncoderOpen(&over_hirate, &err), nullptr);
	EXPECT_EQ(err, CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE);

	CLV_AudioEncParamsC bad_level = EncParams(2u, 16u, kBlocksize);
	bad_level.level = 9u;
	EXPECT_EQ(CLV_AudioEncoderOpen(&bad_level, &err), nullptr);
	EXPECT_EQ(err, CLV_AUDIO_VALUE_RANGE);

	CLV_AudioEncParamsC bad_reserved = EncParams(2u, 16u, kBlocksize);
	bad_reserved.reserved = 1u;
	EXPECT_EQ(CLV_AudioEncoderOpen(&bad_reserved, &err), nullptr);
	EXPECT_EQ(err, CLV_AUDIO_VALUE_RANGE);

	// 过了值域检查，再验状态机：Flush 之后不得继续喂、也不得重复 Flush
	const auto ok = EncParams(2u, 16u, kBlocksize);
	const auto pcm = MakePcm(kBlocksize, 2u, 16u);
	CLV_AudioEncoder* e = CLV_AudioEncoderOpen(&ok, &err);
	ASSERT_NE(e, nullptr) << CLV_AudioStrError(err);
	ASSERT_EQ(CLV_AudioEncoderWrite(e, pcm.data(), pcm.size(), kBlocksize), CLV_AUDIO_OK);
	ASSERT_EQ(CLV_AudioEncoderFlush(e), CLV_AUDIO_OK);
	EXPECT_EQ(CLV_AudioEncoderWrite(e, pcm.data(), pcm.size(), kBlocksize), CLV_AUDIO_STATE_ERROR);
	EXPECT_EQ(CLV_AudioEncoderFlush(e), CLV_AUDIO_STATE_ERROR);
	CLV_AudioEncoderClose(e);
}

/* 取帧口交出的数据指针有效期（include/CLV_Audio.h 的 NextFrame 条目只写「取下一帧编码结果」，
 * 没写指针到哪儿失效）。缓存对象那一侧的「各次调用之间恒定」见 test_audio_blob_refs.cpp 件头，
 * 这一条补取帧口那一侧：同一编码器上，前一次交出的指针在**下一次取帧或 Close 之前**仍读得通、
 * 逐字节不变。下一次取帧之后那份指针归不归库复用，公开头没承诺 ⇒ 本条不在那之后引用它，
 * 也不把「失效」写成断言（写出来就是替库承诺一件它没承诺的事）。 */
TEST(AudioCodec, NextFramePointerStaysValidUntilTheNextTake)
{
	constexpr int kFrames = 3;
	const auto ep = EncParams(2u, 16u, kBlocksize);
	const auto pcm = MakePcm(kBlocksize * static_cast<uint32_t>(kFrames), 2u, 16u);

	CLV_AudioError err = CLV_AUDIO_OK;
	CLV_AudioEncoder* e = CLV_AudioEncoderOpen(&ep, &err);
	ASSERT_NE(e, nullptr) << CLV_AudioStrError(err);
	ASSERT_EQ(CLV_AudioEncoderWrite(e, pcm.data(), pcm.size(), kBlocksize * static_cast<uint32_t>(kFrames)),
			  CLV_AUDIO_OK);
	ASSERT_EQ(CLV_AudioEncoderFlush(e), CLV_AUDIO_OK);

	const uint8_t* prev = nullptr;
	size_t prev_size = 0u;
	std::vector<uint8_t> prev_kept;
	uint32_t samples_seen = 0u;

	for (int i = 0; i < kFrames; ++i)
	{
		// 窗口末端复读：这一次取帧之前，前一份指针的起点、长度与字节都还得与前一次交出时一致
		if (prev != nullptr)
		{
			SCOPED_TRACE("第 " + std::to_string(i) + " 次取帧之前回读前一份指针");
			EXPECT_EQ(prev_size, prev_kept.size());
			EXPECT_EQ(std::memcmp(prev, prev_kept.data(), prev_kept.size()), 0) << "前一次的字节被改写了";
		}

		const uint8_t* data = nullptr;
		size_t size = 0u;
		uint8_t got = 0u;
		ASSERT_EQ(CLV_AudioEncoderNextFrame(e, &data, &size, &got), CLV_AUDIO_OK) << "帧 " << i;
		ASSERT_EQ(got, 1u) << "帧 " << i;
		ASSERT_GT(size, 2u) << "帧 " << i;
		// 交出的这一帧本身可读且自洽：块大小按声明走，样本数据此累加与喂入量对平
		CLV_AudioFrameHeadC h {};
		ASSERT_EQ(CLV_AudioInspectFrame(data, size, &h), CLV_AUDIO_OK) << "帧 " << i;
		EXPECT_EQ(h.blocksize, kBlocksize) << "帧 " << i;
		samples_seen += h.blocksize;

		prev = data;
		prev_size = size;
		prev_kept.assign(data, data + size);
	}
	EXPECT_EQ(samples_seen, kBlocksize * static_cast<uint32_t>(kFrames)) << "取帧口交出的样本数与喂入量不符";

	// 取空那一档：got = 0 不是错误，且入口把三个出参清零后再返回（不留上一帧的字节当结果）
	std::memset(&prev_kept[0], 0u, prev_kept.size());	 // 上面那次窗口复读已完成，这里不再依赖旧指针
	const uint8_t* extra = nullptr;
	size_t extra_size = 9u;
	uint8_t extra_got = 1u;
	ASSERT_EQ(CLV_AudioEncoderNextFrame(e, &extra, &extra_size, &extra_got), CLV_AUDIO_OK);
	EXPECT_EQ(extra_got, 0u);
	EXPECT_EQ(extra, nullptr);
	EXPECT_EQ(extra_size, 0u);

	CLV_AudioEncoderClose(e);
}

