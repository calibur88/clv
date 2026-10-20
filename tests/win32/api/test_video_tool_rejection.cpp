/* test_video_tool_rejection.cpp
 *
 * 公开 C ABI 用例：排除集工具与声明面载体位的拒绝路径（判据各写「拒整条流」与「只拒这一帧」两档）。
 * 本件只管两件事：
 *   1. 拒解的范围 —— 视频流被拒时，同一个文件里其余流照常读出、照常解码，视频流自己的载荷也不损坏；
 *   2. v1 不收的工具在**声明面**能落到哪几个字节上，那些字节一旦非零走哪条拒绝路径。
 *
 * 声明面只有三处载体（逐处对码实测）：
 *   · 流元数据块（`ext_type = 3`，固定 8 字节）：`ext_tools` 只定义 bit0，bit1-7 与 `reserved` 一律判非 0 即拒；
 *     块长不是 8 也拒 —— 载荷里没有给任何扩展工具留描述空间。
 *   · 解码器入参 `CLV_VideoStreamParamsC` 的特征位：`vlc_table_id` / `alpha_enabled` / `obmc_disable` / `layer_id`。
 *   · 帧头 `vop_flags`：bit3 顶场优先、bit4 隔行、bit5-7 保留，任一置位拒**这一帧**。
 * 本库排除的工具集里，sprite 与 GMC 这两类在 v1 语法里没有载体位：sprite 属视频对象层的声明
 * （CLV 没有对象层，流级声明只有上面那 8 字节），GMC 靠宏块头的 `mcsel` / `modb` / `dbquant` 分支承载
 * （CLV 的宏块头不含这些分支）。故本件对这两类点名判的是「声明面是关着的」这一侧：
 * 任何想多带一层工具描述的写法都落在被钉成 0 的位或钉死的块长上。
 */

#include "../video/test_config_expect.h"
#include "CLV_Audio.h"
#include "CLV_Container.h"
#include "CLV_Video.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace
{

	constexpr uint16_t kW = 16u;
	constexpr uint16_t kH = 16u;
	constexpr uint8_t kQp = 7u;
	constexpr uint8_t kQuant = 24u;
	constexpr uint32_t kVideoFrames = 3u;
	constexpr uint32_t kRate = 48000u;
	constexpr uint16_t kAudioCh = 2u;
	constexpr uint16_t kAudioBits = 16u;
	constexpr uint16_t kAudioBps = 2u;
	constexpr uint32_t kAudioSamples = 4608u;	 // 4 个 1152 样本块
	constexpr uint32_t kAudioBatch = 1152u;

	std::string TempPath(const std::string& stem)
	{
		std::error_code ec;
		const auto dir = std::filesystem::temp_directory_path(ec);
		std::string name = dir.generic_string();
		if (! name.empty() && name.back() != '/') name.push_back('/');
		name += stem + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".clv";
		return name;
	}

	class ScopedFile
	{
		public:

		explicit ScopedFile(std::string path): path_(std::move(path)) {}

		~ScopedFile() { Discard(); }

		const char* c_str() const { return path_.c_str(); }

		void Discard()
		{
			std::error_code ec;
			std::filesystem::remove(std::filesystem::u8path(path_), ec);
		}

		private:

		std::string path_;
	};

	/* 解码器入参从文件里的流描述符取，不从测试自己造的常量取：
	 * 这样「拒解是流的声明造成的」这句话才有落点。 */
	CLV_VideoStreamParamsC VideoParamsFrom(const CLV_StreamDescC& d)
	{
		CLV_VideoStreamParamsC p;
		std::memset(&p, 0, sizeof(p));
		p.bit_depth = d.bit_depth;
		p.quant_precision = kQp;
		p.max_b_frames = 0u;
		p.vlc_table_id = d.vlc_table_id;
		p.alpha_enabled = 0u;
		p.obmc_disable = 1u;
		p.layer_id = d.layer_id;
		p.display_width = kW;
		p.display_height = kH;
		return p;
	}

	/* 视频载荷：编码器现产 kVideoFrames 支 I 帧，本件只当素材来源。 */
	std::vector<std::vector<uint8_t>> EncodeVideoIFrames()
	{
		std::vector<std::vector<uint8_t>> out;
		CLV_VideoEncParamsC ep;
		std::memset(&ep, 0, sizeof(ep));
		ep.display_width = kW;
		ep.display_height = kH;
		ep.bit_depth = 8u;
		ep.quant_precision = kQp;
		ep.default_quant = kQuant;
		CLV_VideoError verr = CLV_VIDEO_OK;
		CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&ep, &verr);
		if (e == nullptr) return out;
		CLV_VideoFrame* f = nullptr;
		if (CLV_VideoEncoderAllocFrame(e, &f) == CLV_VIDEO_OK)
		{
			for (uint32_t i = 0u; i < kVideoFrames; ++i)
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
				CLV_VideoBitstreamRelease(bs);
			}
			CLV_VideoFrameRelease(f);
		}
		CLV_VideoEncoderClose(e);
		return out;
	}

	/* 音频载荷：一段确定性斜波 PCM 走 CLV 音频面编成若干 FLAC 帧，顺带取回 34 字节流信息。
	 * 源 PCM 留在 `pcm` 里，解码回来逐字节对账。 */
	std::vector<std::vector<uint8_t>> EncodeAudio(std::vector<uint8_t>* pcm, std::vector<uint8_t>* streaminfo)
	{
		pcm->assign(static_cast<size_t>(kAudioSamples) * kAudioCh * kAudioBps, 0u);
		for (uint32_t s = 0u; s < kAudioSamples; ++s)
		{
			const int16_t l = static_cast<int16_t>((s * 13u) & 0x7FFFu);
			const int16_t r = static_cast<int16_t>(0 - static_cast<int32_t>((s * 7u) & 0x7FFFu));
			(*pcm)[(static_cast<size_t>(s) * 2u + 0u) * 2u + 0u] = static_cast<uint8_t>(l & 0xFFu);
			(*pcm)[(static_cast<size_t>(s) * 2u + 0u) * 2u + 1u] = static_cast<uint8_t>((l >> 8) & 0xFFu);
			(*pcm)[(static_cast<size_t>(s) * 2u + 1u) * 2u + 0u] = static_cast<uint8_t>(r & 0xFFu);
			(*pcm)[(static_cast<size_t>(s) * 2u + 1u) * 2u + 1u] = static_cast<uint8_t>((r >> 8) & 0xFFu);
		}

		CLV_AudioEncParamsC ep;
		std::memset(&ep, 0, sizeof(ep));
		ep.sample_rate = kRate;
		ep.channels = kAudioCh;
		ep.bit_depth = kAudioBits;
		ep.blocksize = 0u;
		ep.level = 5u;
		CLV_AudioError aerr = CLV_AUDIO_OK;
		CLV_AudioEncoder* e = CLV_AudioEncoderOpen(&ep, &aerr);
		if (e == nullptr) return {};
		std::vector<std::vector<uint8_t>> frames;
		uint32_t done = 0u;
		while (done < kAudioSamples)
		{
			uint32_t cnt = kAudioSamples - done;
			if (cnt > kAudioBatch) cnt = kAudioBatch;
			const size_t off = static_cast<size_t>(done) * kAudioCh * kAudioBps;
			if (CLV_AudioEncoderWrite(e, pcm->data() + off, static_cast<size_t>(cnt) * kAudioCh * kAudioBps, cnt) !=
				CLV_AUDIO_OK)
			{
				CLV_AudioEncoderClose(e);
				return {};
			}
			done += cnt;
			for (;;)
			{
				const uint8_t* d = nullptr;
				size_t sz = 0u;
				uint8_t got = 0u;
				if (CLV_AudioEncoderNextFrame(e, &d, &sz, &got) != CLV_AUDIO_OK || ! got) break;
				frames.emplace_back(d, d + sz);
			}
		}
		if (CLV_AudioEncoderFlush(e) != CLV_AUDIO_OK)
		{
			CLV_AudioEncoderClose(e);
			return {};
		}
		for (;;)
		{
			const uint8_t* d = nullptr;
			size_t sz = 0u;
			uint8_t got = 0u;
			if (CLV_AudioEncoderNextFrame(e, &d, &sz, &got) != CLV_AUDIO_OK || ! got) break;
			frames.emplace_back(d, d + sz);
		}
		CLV_AudioBlob* blob = nullptr;
		if (CLV_AudioEncoderTakeStreamInfo(e, &blob) == CLV_AUDIO_OK && blob != nullptr)
		{
			const uint8_t* si = nullptr;
			size_t sisz = 0u;
			if (CLV_AudioBlobGetBytes(blob, &si, &sisz) == CLV_AUDIO_OK) streaminfo->assign(si, si + sisz);
			CLV_AudioBlobRelease(blob);
		}
		CLV_AudioEncoderClose(e);
		return frames;
	}

	CLV_StreamDescC MakeDesc(uint8_t type, uint8_t id, uint32_t max_packet)
	{
		CLV_StreamDescC d;
		std::memset(&d, 0, sizeof(d));
		d.stream_id = id;
		d.stream_type = type;
		d.codec_id = type == 1u ? 1u : 0u;
		d.codec_version = 1;
		d.timebase_num = 1;
		d.timebase_den = type == 1u ? kRate : 1081080000ull;
		d.max_packet_size = max_packet;
		d.first_dts = 0;
		d.sample_rate = type == 1u ? kRate : 0u;
		d.channels = type == 1u ? kAudioCh : 0u;
		d.bit_depth = type == 1u ? kAudioBits : 8u;
		d.layer_id = 0;
		return d;
	}

	/* 每条视频流必须挂 ext_type = 3 参数块（`docs/容器规范.md` 的扩展块一节，v2 起对视频流必选）：缺块的流被读方记
	 * video_stream_no_param_block 并判该流不可用，本件「一条流被拒、其余流照常」的对账就落不到视频流上。
	 * 载荷 8 字节，字段布局照 src/video/headers.cpp 的 ParseEncMeta：
	 *   [0] max_b_frames  [1] quant_precision  [2] ext_tools  [3] reserved  [4..5] width LE16  [6..7] height LE16
	 * 尺寸沿用本件编码器在用的 kW x kH，B 帧声明 0（编码侧只出 I 帧）。 */
	CLV_ContainerError AddVideoParams(CLV_ContainerWriter* w, uint8_t stream_id, uint16_t width, uint16_t height)
	{
		const uint8_t meta[8] = { 0u,	  // max_b_frames
								  kQp,	  // quant_precision
								  0u,	  // ext_tools
								  0u,	  // reserved
								  static_cast<uint8_t>(width & 0xFFu),
								  static_cast<uint8_t>((width >> 8) & 0xFFu),
								  static_cast<uint8_t>(height & 0xFFu),
								  static_cast<uint8_t>((height >> 8) & 0xFFu) };
		return CLV_ContainerWriterAddExtBlock(w, stream_id, 3, 1, meta, sizeof(meta));
	}

}	 // namespace

/* 拒解范围：视频流被声明档拒掉，同一个文件的音频流照常出帧，PCM 与源逐字节全等。
 * 视频流自己的载荷也完好 —— Reset 解除拒解态后，同一份载荷照常解出帧。 */
TEST(VideoToolRejection, RejectedVideoStreamLeavesOtherStreamsPlayable)
{
	std::vector<uint8_t> pcm;
	std::vector<uint8_t> streaminfo;
	const std::vector<std::vector<uint8_t>> audio = EncodeAudio(&pcm, &streaminfo);
	ASSERT_GE(audio.size(), 2u) << "音频面没产出帧，本件素材不成立";
	ASSERT_EQ(streaminfo.size(), 34u);
	const std::vector<std::vector<uint8_t>> video = EncodeVideoIFrames();
	ASSERT_EQ(video.size(), kVideoFrames);

	size_t audio_max = 0u;
	for (const auto& f: audio)
		if (f.size() > audio_max) audio_max = f.size();
	size_t video_max = 0u;
	for (const auto& f: video)
		if (f.size() > video_max) video_max = f.size();

	const std::string path = TempPath("clv_test_profile_xstream_");
	ScopedFile guard(path);
	uint64_t packets = 0u;
	{
		CLV_ContainerError err = CLV_CONTAINER_OK;
		CLV_WriteParams wp;
		std::memset(&wp, 0, sizeof(wp));
		wp.version_minor = 0;
		wp.index_present = 1;
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(path.c_str(), &wp, &err);
		ASSERT_NE(w, nullptr) << CLV_ContainerStrError(err);
		const CLV_StreamDescC vd = MakeDesc(0u, 0u, static_cast<uint32_t>(video_max + 16u));
		const CLV_StreamDescC ad = MakeDesc(1u, 1u, static_cast<uint32_t>(audio_max + 16u));
		ASSERT_EQ(CLV_ContainerWriterAddStream(w, &vd), CLV_CONTAINER_OK);
		ASSERT_EQ(CLV_ContainerWriterAddStream(w, &ad), CLV_CONTAINER_OK);
		ASSERT_EQ(AddVideoParams(w, vd.stream_id, kW, kH), CLV_CONTAINER_OK);
		// 两条流在包区里交错写入，各自的时间戳按自己的流单调递增
		for (uint32_t i = 0u; i < kVideoFrames; ++i)
		{
			ASSERT_EQ(CLV_ContainerWriterWriteFrame(w, 0u, static_cast<uint8_t>(i == 0u ? 1u : 0u),
													static_cast<int64_t>(i), static_cast<int64_t>(i), video[i].data(),
													video[i].size()),
					  CLV_CONTAINER_OK);
			if (i < audio.size())
			{
				ASSERT_EQ(CLV_ContainerWriterWriteFrame(w, 1u, 1u, static_cast<int64_t>(i), static_cast<int64_t>(i),
														audio[i].data(), audio[i].size()),
						  CLV_CONTAINER_OK);
			}
		}
		for (size_t i = kVideoFrames; i < audio.size(); ++i)
		{
			const int64_t t = static_cast<int64_t>(i);
			ASSERT_EQ(CLV_ContainerWriterWriteFrame(w, 1u, 0u, t, t, audio[i].data(), audio[i].size()),
					  CLV_CONTAINER_OK);
		}
		ASSERT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
		CLV_ContainerWriterClose(w);
	}

	CLV_ContainerError err = CLV_CONTAINER_OK;
	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path.c_str(), &err);
	ASSERT_NE(r, nullptr) << CLV_ContainerStrError(err);
	uint32_t streams = 0u;
	ASSERT_EQ(CLV_ContainerReaderStreamCount(r, &streams), CLV_CONTAINER_OK);
	ASSERT_EQ(streams, 2u);
	CLV_StreamDescC vd, ad;
	uint8_t usable = 0u;
	ASSERT_EQ(CLV_ContainerReaderGetStream(r, 0u, &vd, &usable), CLV_CONTAINER_OK);
	EXPECT_EQ(usable, 1u);
	ASSERT_EQ(CLV_ContainerReaderGetStream(r, 1u, &ad, &usable), CLV_CONTAINER_OK);
	EXPECT_EQ(usable, 1u);

	std::vector<std::vector<uint8_t>> vid_from_file, aud_from_file;
	for (;;)
	{
		CLV_FrameC f;
		std::memset(&f, 0, sizeof(f));
		uint8_t got = 0u;
		ASSERT_EQ(CLV_ContainerReaderNextFrame(r, &f, &got), CLV_CONTAINER_OK);
		if (! got) break;
		if (f.stream_id == 0u) vid_from_file.emplace_back(f.payload, f.payload + f.payload_size);
		else if (f.stream_id == 1u) aud_from_file.emplace_back(f.payload, f.payload + f.payload_size);
		else FAIL() << "读到没声明的流号";
	}
	ASSERT_EQ(vid_from_file.size(), kVideoFrames);
	ASSERT_EQ(aud_from_file.size(), audio.size());
	for (uint32_t i = 0u; i < kVideoFrames; ++i) EXPECT_EQ(vid_from_file[i], video[i]);
	for (size_t i = 0u; i < aud_from_file.size(); ++i) EXPECT_EQ(aud_from_file[i], audio[i]);

	// 视频流：声明面命中排除集（透明块），句柄带拒解态
	CLV_VideoStreamParamsC vp = VideoParamsFrom(vd);
	vp.alpha_enabled = 1u;
	CLV_VideoError verr = CLV_VIDEO_OK;
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&vp, &verr);
	ASSERT_NE(d, nullptr);
	EXPECT_EQ(verr, CLV_VIDEO_TOOL_EXCLUDED);
	uint8_t rejected = 0u;
	ASSERT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK);
	EXPECT_EQ(rejected, 1u);
	for (const auto& p: vid_from_file)
		EXPECT_EQ(CLV_VideoDecoderDecode(d, p.data(), p.size(), 1u, 0, 0, nullptr), CLV_VIDEO_STREAM_REJECTED);

	// 音频流：同一个文件、同一轮读取，照常解出全部帧，PCM 与源逐字节全等
	CLV_AudioStreamParamsC ap;
	std::memset(&ap, 0, sizeof(ap));
	ap.sample_rate = ad.sample_rate;
	ap.channels = ad.channels;
	ap.bit_depth = ad.bit_depth;
	ap.max_packet_size = ad.max_packet_size;
	CLV_AudioError aerr = CLV_AUDIO_OK;
	CLV_AudioDecoder* a = CLV_AudioDecoderOpen(&ap, streaminfo.data(), streaminfo.size(), &aerr);
	ASSERT_NE(a, nullptr) << CLV_AudioStrError(aerr);
	std::vector<uint8_t> back;
	for (const auto& p: aud_from_file)
	{
		CLV_AudioBuffer* buf = nullptr;
		uint8_t got = 0u;
		ASSERT_EQ(CLV_AudioDecoderDecodeFrame(a, p.data(), p.size(), &buf, &got, nullptr), CLV_AUDIO_OK);
		ASSERT_EQ(got, 1u);
		CLV_AudioBufferDescC bd;
		std::memset(&bd, 0, sizeof(bd));
		ASSERT_EQ(CLV_AudioBufferGetDesc(buf, &bd), CLV_AUDIO_OK);
		back.insert(back.end(), bd.data, bd.data + bd.size);
		CLV_AudioBufferRelease(buf);
	}
	CLV_AudioStatsC as;
	std::memset(&as, 0, sizeof(as));
	ASSERT_EQ(CLV_AudioDecoderStats(a, &as), CLV_AUDIO_OK);
	EXPECT_EQ(as.stream_rejected, 0u);
	EXPECT_EQ(as.frames_ok, static_cast<uint64_t>(aud_from_file.size()));
	EXPECT_EQ(as.frames_dropped_header, 0u);
	EXPECT_EQ(as.frames_dropped_mismatch, 0u);
	CLV_AudioDecoderClose(a);
	ASSERT_EQ(back.size(), pcm.size());
	EXPECT_EQ(std::memcmp(back.data(), pcm.data(), pcm.size()), 0);

	// 视频流自己的载荷没被拒解态波及：Reset 是该态唯一出口，出去之后同一份载荷照常解
	ASSERT_EQ(CLV_VideoDecoderReset(d), CLV_VIDEO_OK);
	CLV_VideoFrameInfoC info;
	for (size_t i = 0u; i < vid_from_file.size(); ++i)
	{
		std::memset(&info, 0, sizeof(info));
		ASSERT_EQ(CLV_VideoDecoderDecode(d, vid_from_file[i].data(), vid_from_file[i].size(), 1u,
										 static_cast<int64_t>(i), static_cast<int64_t>(i), &info),
				  CLV_VIDEO_OK)
			<< "第 " << i << " 支载荷不该因流声明而坏掉";
		EXPECT_EQ(info.type, CLV_VIDEO_FRAME_I);
	}
	CLV_VideoFrame* out = nullptr;
	uint8_t got = 0u;
	ASSERT_EQ(CLV_VideoDecoderFlush(d, &out, &got), CLV_VIDEO_OK);
	if (out != nullptr) CLV_VideoFrameRelease(out);
	CLV_VideoDecoderClose(d);

	CLV_ReaderStatsC st;
	std::memset(&st, 0, sizeof(st));
	ASSERT_EQ(CLV_ContainerReaderStats(r, &st), CLV_CONTAINER_OK);
	EXPECT_EQ(st.dropped_crc, 0u);
	EXPECT_EQ(st.dropped_field, 0u);
	EXPECT_EQ(st.frames_incomplete, 0u);
	EXPECT_EQ(st.packets_ok, packets);
	std::cout << "[跨流存活] 包数=" << packets << "，视频帧=" << vid_from_file.size()
			  << "，音频帧=" << aud_from_file.size() << "，音频 PCM 往返=" << back.size() << " B（源 " << pcm.size()
			  << " B），读方坏包计数=" << st.dropped_crc + st.dropped_field + st.dropped_too_large << "\n";
	CLV_ContainerReaderClose(r);
}

/* sprite 一类的点名件：流级声明面是一扇关着的门。
 * `ext_tools` 只定义 bit0，bit1-7 与 `reserved` 任一非 0 即拒；块长钉死在 8 ——
 * 想多带一段 sprite 轨迹描述的写法没有落点。 */
TEST(VideoToolRejection, SpriteStyleExtraToolDeclarationHasNoCarrier)
{
	std::vector<uint8_t> meta(8u, 0u);
	meta[1] = kQp;
	// 尺寸字段非 0 才是合法声明（0 判值域不成立）：176 × 144，小端
	meta[4] = 176u;
	meta[6] = 144u;
	CLV_VideoEncMetaC out;

	// 合法形状：bit0 = 有清单，保留位全 0
	meta[2] = 0x01u;
	meta[3] = 0u;
	ASSERT_EQ(CLV_VideoParseEncMeta(meta.data(), meta.size(), &out), CLV_VIDEO_OK);
	EXPECT_EQ(out.ext_tools, 0x01u);

	// 保留位逐个置起来：bit1-7 任一非 0 都不成立
	static const uint8_t kToolBits[] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u };
	for (const uint8_t bit: kToolBits)
	{
		std::vector<uint8_t> bad = meta;
		bad[2] = static_cast<uint8_t>(0x01u | (1u << bit));
		EXPECT_EQ(CLV_VideoParseEncMeta(bad.data(), bad.size(), &out), CLV_VIDEO_HEADER_BAD) << "ext_tools bit " << bit;
	}

	// 那个「v1 = 0」的 reserved 字节是同一扇门
	static const uint8_t kResBits[] = { 0u, 3u, 7u };
	for (const uint8_t bit: kResBits)
	{
		std::vector<uint8_t> bad = meta;
		bad[2] = 0x01u;
		bad[3] = static_cast<uint8_t>(1u << bit);
		EXPECT_EQ(CLV_VideoParseEncMeta(bad.data(), bad.size(), &out), CLV_VIDEO_HEADER_BAD) << "reserved bit " << bit;
	}

	// 块长钉死在 8：加长到装得下工具描述，本身就是不成立的写法。
	// 过长与过短是两个码：过短 = `CLV_VIDEO_PAYLOAD_TOO_SMALL`（公开定义钉死「短于定长」），
	// 过长 = `CLV_VIDEO_PAYLOAD_LENGTH_MISMATCH`（段尾追加的「载荷长度不符」新码）—— 两格期望照公开头逐字对。
	std::vector<uint8_t> wide(16u, 0u);
	wide[1] = kQp;
	EXPECT_EQ(CLV_VideoParseEncMeta(wide.data(), wide.size(), &out), CLV_VIDEO_PAYLOAD_LENGTH_MISMATCH);
	EXPECT_EQ(CLV_VideoParseEncMeta(meta.data(), 7u, &out), CLV_VIDEO_PAYLOAD_TOO_SMALL);

	// 尺寸写 0：值域不成立，不是「未声明」
	std::vector<uint8_t> no_dim = meta;
	no_dim[4] = 0u;
	no_dim[6] = 0u;
	EXPECT_EQ(CLV_VideoParseEncMeta(no_dim.data(), no_dim.size(), &out), CLV_VIDEO_VALUE_RANGE);
}

/* 帧头侧的点名件：帧头里能「多带一层场序 / 精度描述」的位全是钉 0 的位（隔行的场序位也在这一组）。
 * 命中时拒**这一帧**，流态不受影响 —— 与声明档那条「拒整条流」是两条不同路径。 */
TEST(VideoToolRejection, FrameHeaderExtraSignallingBitsRejectOnlyThatFrameNotTheStream)
{
	const std::vector<std::vector<uint8_t>> video = EncodeVideoIFrames();
	ASSERT_EQ(video.size(), kVideoFrames);
	ASSERT_GE(video[0].size(), 8u);

	const CLV_StreamDescC vd = MakeDesc(0u, 0u, 4096u);
	CLV_VideoStreamParamsC vp = VideoParamsFrom(vd);
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&vp, &err);
	ASSERT_NE(d, nullptr) << CLV_VideoStrError(err);
	ASSERT_EQ(err, CLV_VIDEO_OK);

	/* 单片帧的载荷就是「VOP 头 + 宏块序列」，帧头第 5 字节是 vop_flags。
	 * bit3 / bit4 仍是保留位；bit5-7 是系数编码法 —— 编码器默认写的那一法就把这一格填成非 0，
	 * 所以「置某一位就该拒」只在**保留位**与**未赋法的取值**上成立，不能对整个 3 bit 字段一律套用。
	 * 未赋法的取值 = 2..7：置 bit6 ⇒ 3、置 bit7 ⇒ 5、两位都置 ⇒ 7。 */
	struct Bad
	{
		uint8_t or_bits;
		uint8_t and_bits;	 // 0xFF = 不动
		const char* what;
	};

	static const Bad kBad[] = {
		{ 0x08u, 0xFFu, "bit3 顶场优先（保留）" },
		  { 0x10u, 0xFFu, "bit4 隔行扫描（保留）" },
		{ 0x40u, 0xFFu, "编码法 3（未赋法）"	},
		   { 0x80u, 0xFFu, "编码法 5（未赋法）"	   },
		{ 0xC0u, 0xFFu, "编码法 7（未赋法）"	},
	};
	for (const Bad& b: kBad)
	{
		std::vector<uint8_t> bad = video[0];
		bad[4] = static_cast<uint8_t>((bad[4] & b.and_bits) | b.or_bits);
		EXPECT_EQ(CLV_VideoDecoderDecode(d, bad.data(), bad.size(), 1u, 0, 0, nullptr), CLV_VIDEO_HEADER_BAD) << b.what;
		// 拒的是这一帧：流没进拒解态，同一条流下一帧照常
		uint8_t rejected = 1u;
		ASSERT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK);
		EXPECT_EQ(rejected, 0u) << b.what;
	}

	/* 解码侧取的是**帧头里那 3 bit**，不是建解码器时的假设 —— 两档各造一次谎，各断一次：
	 *   默认档（法 1 写在选择器上）：抹成 0（= 宣称定宽），其余一字不动 ⇒ 块数据被按错的法读 ⇒ 拒在这一帧。
	 *   定宽档（0 写在选择器上）：抹成 1（= 宣称法 1），其余一字不动 ⇒ 同一件事的对称形态，实测见下面 else 分支的注释。
	 * 这条是「码流自带标记、不靠两侧约定同步」的端到端落点，两档都跑得通就不留跳过项。 */
	if (clvtest::kHasCoefCodingSelector)
	{
		std::vector<uint8_t> lying = video[0];
		ASSERT_NE(lying[4] & 0xE0u, 0u) << "编码器没把法写进帧头，本条前提不成立";
		lying[4] = static_cast<uint8_t>(lying[4] & ~0xE0u);
		CLV_VideoError e = CLV_VideoDecoderDecode(d, lying.data(), lying.size(), 1u, 0, 0, nullptr);
		EXPECT_NE(e, CLV_VIDEO_OK) << "帧头说谎说成定宽，解码侧照单全收";
		uint8_t rejected = 1u;
		ASSERT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK);
		EXPECT_EQ(rejected, 0u) << "取错法只坏这一帧，不得把整条流打进拒解态";
	}
	else
	{
		/* 定宽流的对称条：把帧头 bit5-7 从 0 抹成 1（宣称法 1），其余字节一字不动。
		 * 实测形态 = 码 **67 `CLV_VIDEO_PAYLOAD_TOO_SMALL`**（该素材载荷 298 B，帧头第 5 字节 0 → 32）；
		 *   段 = **宏块载体读取**，不是帧头解析 —— 选择器 1 是合法取值，帧头那关放行（上面 kBad 表里未赋法的 2..7 才当场 `HEADER_BAD`），
		 *   读不到法 1 语义下的载体就见底（`src/video/block.cpp` 的 `DecodeBlock` 法 1 支读位失败与 `src/video/macroblock.cpp`
		 *   的 `ReadVarint` 读字节失败两处之一 —— 公开面分不出这两处，所以只断到「这一帧被拒、且拒在头之后」）。
		 *   范围 = **单帧拒**：`StreamRejected` 读回 0，本块之后那条「下一干净帧照旧出 I 帧」的断言在两档都成立。 */
		std::vector<uint8_t> lying = video[0];
		ASSERT_EQ(lying[4] & 0xE0u, 0u) << "定宽档帧头选择器应写 0，本条前提不成立";
		lying[4] = static_cast<uint8_t>((lying[4] & ~0xE0u) | 0x20u);
		EXPECT_EQ(CLV_VideoDecoderDecode(d, lying.data(), lying.size(), 1u, 0, 0, nullptr), CLV_VIDEO_PAYLOAD_TOO_SMALL)
			<< "定宽流谎称法 1，解码侧照单全收";
		uint8_t rejected = 1u;
		ASSERT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK);
		EXPECT_EQ(rejected, 0u) << "取错法只坏这一帧，不得把整条流打进拒解态";
	}
	// 坏帧之后干净帧照旧出帧
	CLV_VideoFrameInfoC info;
	std::memset(&info, 0, sizeof(info));
	ASSERT_EQ(CLV_VideoDecoderDecode(d, video[1].data(), video[1].size(), 1u, 1, 1, &info), CLV_VIDEO_OK);
	EXPECT_EQ(info.type, CLV_VIDEO_FRAME_I);
	CLV_VideoDecoderClose(d);
}

/* 清单档的分界点名：声明了工具清单却不给内容 = 字段不成立，拒**整条流**；
 * 给了内容的清单在本层不解释，v1 不据它放宽任何判定。 */
TEST(VideoToolRejection, ToolListDeclarationWithoutContentRejectsWholeStream)
{
	CLV_VideoStreamParamsC vp = VideoParamsFrom(MakeDesc(0u, 0u, 4096u));
	vp.tool_list = nullptr;
	vp.tool_list_size = 4u;	   // 声明有 4 字节清单，指针却没给
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoDecoder* d = CLV_VideoDecoderOpen(&vp, &err);
	ASSERT_NE(d, nullptr);
	EXPECT_EQ(err, CLV_VIDEO_TOOL_LIST_ABSENT);
	uint8_t rejected = 0u;
	ASSERT_EQ(CLV_VideoDecoderStreamRejected(d, &rejected), CLV_VIDEO_OK);
	EXPECT_EQ(rejected, 1u);
	CLV_VideoDecoderClose(d);

	// 内容在场：本层不读清单写了什么工具，句柄照常可用（清单与排除集是否相交在本层不参与判定）
	static const uint8_t kList[4] = { 1u, 2u, 3u, 4u };
	CLV_VideoStreamParamsC ok = vp;
	ok.tool_list = kList;
	ok.tool_list_size = 4u;
	err = CLV_VIDEO_OK;
	CLV_VideoDecoder* d2 = CLV_VideoDecoderOpen(&ok, &err);
	ASSERT_NE(d2, nullptr);
	EXPECT_EQ(err, CLV_VIDEO_OK);
	rejected = 1u;
	ASSERT_EQ(CLV_VideoDecoderStreamRejected(d2, &rejected), CLV_VIDEO_OK);
	EXPECT_EQ(rejected, 0u);
	CLV_VideoDecoderClose(d2);
}
