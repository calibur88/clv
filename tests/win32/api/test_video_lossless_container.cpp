/* test_video_lossless_container.cpp
 *
 * 公开 C ABI 用例：**无损流走容器面往返**。
 *
 * 无损通路本身在 `tests/win32/video/test_video_lossless_roundtrip.cpp` 里是「载荷直喂解码器」证的，
 * 那条路径绕过了打包 / 分片 / 片序 / 索引 / CRC。本件把同一条无损流塞进容器面，并且把写参数的
 * **分片粒度设得远小于一帧载荷**，让它必须切成多片再拼回来 —— 无损帧比有损帧大（每系数多 2 bit），
 * 载荷越大越容易顶到分片边界，这条链得单独钉一次。
 *
 * 三种计数别混：`max_packet_size` 是**拒收阈值**（单帧载荷超过它就 TOO_LARGE，不是分片开关）；
 * `fragment_chunk_size` 才是容器切片粒度；解码器入参 `fragment_count` 要的是**视频层的片数**
 * （由编码器自报），不是容器侧的包片数。
 *
 * 判据是「逐字节全等」类：容器读回的帧载荷必须与编码器产出的字节完全相同，
 * 解码出的像素必须与输入帧完全相同，两处都不适用任何浮动。 */

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

	constexpr uint16_t kW = 64u;
	constexpr uint16_t kH = 64u;
	constexpr uint8_t kQp = 7u;
	constexpr uint32_t kFrames = 3u;
	constexpr uint64_t kVideoTimebase = 1081080000ull;
	/* 容器侧分片粒度：必须显著小于本素材无损档的最大帧载荷，否则「分片」这条路根本没被走到。
	 * 最大帧载荷随系数载体那一档变（定长档按「块数 × 单块」乘得出来，法 1 档随内容走），
	 * 单次构建只有一档在盘上，故这里不写任何一档的实测字节数：粒度与实长的关系由
	 * `ASSERT_GT(biggest, kFragmentChunk)` 与 `ASSERT_GT(frag_seen[0], 1u)` 两条现算断言守住，
	 * 载荷没超过粒度或没被切开就当场红。 */
	constexpr uint32_t kFragmentChunk = 256u;

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

	struct Frame
	{
		std::vector<uint8_t> y;
		std::vector<uint8_t> cb;
		std::vector<uint8_t> cr;
	};

	/* 与门面基准同式的图样：彩条带 + 全 0 带（顶满幅 DC = −8192）+ 全 255 带 + 每帧右移的方块 */
	Frame MakeFrame(int idx)
	{
		static const int kBarY[7] = { 235, 219, 188, 173, 78, 63, 32 };
		Frame f;
		f.y.assign(static_cast<size_t>(kW) * kH, 0u);
		f.cb.assign(static_cast<size_t>(kW / 2) * (kH / 2), 128u);
		f.cr.assign(static_cast<size_t>(kW / 2) * (kH / 2), 128u);
		const int bx = (8 * idx) % (kW - 16);
		for (int y = 0; y < kH; ++y)
		{
			uint8_t* row = f.y.data() + static_cast<size_t>(y) * kW;
			for (int x = 0; x < kW; ++x)
				if (y < 16) row[x] = static_cast<uint8_t>(kBarY[(x * 7) / kW]);
				else if (y < 32) row[x] = 0u;
				else if (y < 48) row[x] = 255u;
				else row[x] = (x >= bx && x < bx + 16) ? 235u : 16u;
		}
		return f;
	}

	void FillInto(CLV_VideoFrame* handle, const Frame& src)
	{
		CLV_VideoFrameDescC d;
		std::memset(&d, 0, sizeof(d));
		CLV_VideoFrameGetDesc(handle, &d, nullptr);
		const size_t pw[3] = { d.coded_width, d.coded_width / 2u, d.coded_width / 2u };
		const size_t ph[3] = { d.coded_height, d.coded_height / 2u, d.coded_height / 2u };
		const uint8_t* lists[3] = { src.y.data(), src.cb.data(), src.cr.data() };
		for (uint32_t pl = 0u; pl < 3u; ++pl)
			for (size_t r = 0u; r < ph[pl]; ++r)
				std::memcpy(d.plane[pl] + r * d.stride[pl], lists[pl] + r * pw[pl], pw[pl]);
	}

	/* 解码输出与源帧按显示尺寸逐字节比（补边区不计入）。 */
	size_t DiffAgainst(CLV_VideoFrame* out, const Frame& src, size_t* checked)
	{
		CLV_VideoFrameDescC d;
		std::memset(&d, 0, sizeof(d));
		CLV_VideoFrameGetDesc(out, &d, nullptr);
		const size_t pw[3] = { d.display_width, d.display_width / 2u, d.display_width / 2u };
		const size_t ph[3] = { d.display_height, d.display_height / 2u, d.display_height / 2u };
		const uint8_t* lists[3] = { src.y.data(), src.cb.data(), src.cr.data() };
		size_t bad = 0u;
		for (uint32_t pl = 0u; pl < 3u; ++pl)
		{
			for (size_t r = 0u; r < ph[pl]; ++r)
			{
				const uint8_t* pa = d.plane[pl] + r * d.stride[pl];
				const uint8_t* pb = lists[pl] + r * pw[pl];
				for (size_t c = 0u; c < pw[pl]; ++c)
				{
					if (pa[c] != pb[c]) ++bad;
					++(*checked);
				}
			}
		}
		return bad;
	}

}	 // namespace

TEST(VideoLosslessContainer, LosslessStreamSurvivesContainerFragmentationByteExact)
{
	std::vector<Frame> src;
	for (uint32_t i = 0u; i < kFrames; ++i) src.push_back(MakeFrame(static_cast<int>(i)));

	// 一 · 编码侧：无损模式（q = 0），载荷与视频层片数一起留下
	std::vector<std::vector<uint8_t>> payloads;
	std::vector<uint32_t> slice_counts;
	size_t total_payload = 0u;
	{
		CLV_VideoEncParamsC ep;
		std::memset(&ep, 0, sizeof(ep));
		ep.display_width = kW;
		ep.display_height = kH;
		ep.bit_depth = 8u;
		ep.quant_precision = kQp;
		ep.mode = CLV_VIDEO_MODE_LOSSLESS;
		CLV_VideoError err = CLV_VIDEO_OK;
		CLV_VideoEncoder* e = CLV_VideoEncoderOpen(&ep, &err);
		ASSERT_NE(e, nullptr) << CLV_VideoStrError(err);
		CLV_VideoFrame* f = nullptr;
		ASSERT_EQ(CLV_VideoEncoderAllocFrame(e, &f), CLV_VIDEO_OK);
		for (uint32_t i = 0u; i < kFrames; ++i)
		{
			FillInto(f, src[i]);
			CLV_VideoBitstream* bs = nullptr;
			ASSERT_EQ(CLV_VideoEncoderEncode(e, f, i == 0u ? CLV_VIDEO_FRAME_I : CLV_VIDEO_FRAME_P, 0u,
											 static_cast<int64_t>(i), static_cast<int64_t>(i), &bs),
					  CLV_VIDEO_OK)
				<< "无损编码帧 " << i;
			const uint8_t* data = nullptr;
			size_t size = 0u;
			uint32_t ns = 0u;
			ASSERT_EQ(CLV_VideoBitstreamGetBytes(bs, &data, &size), CLV_VIDEO_OK);
			ASSERT_EQ(CLV_VideoBitstreamSliceCount(bs, &ns), CLV_VIDEO_OK);
			payloads.emplace_back(data, data + size);
			slice_counts.push_back(ns);
			total_payload += size;
			CLV_VideoBitstreamRelease(bs);
		}
		CLV_VideoFrameRelease(f);
		CLV_VideoEncoderClose(e);
	}
	ASSERT_EQ(payloads.size(), kFrames);
	size_t biggest = 0u;
	for (const auto& p: payloads) biggest = p.size() > biggest ? p.size() : biggest;
	// 本档实测最大帧载荷现算并记进属性，档位漂了在这里留痕，不在注释里另抄一份数
	RecordProperty("MaxLosslessFrameBytes", static_cast<uint64_t>(biggest));
	ASSERT_GT(biggest, static_cast<size_t>(kFragmentChunk)) << "载荷没超过分片粒度，本件就没真正测到分片";

	// 二 · 写进容器：包上限按最大帧给足（它是拒收阈值），分片粒度设小 ⇒ 每帧切成多片
	const std::string path = TempPath("clv_lossless_container");
	ScopedFile guard(path);
	uint64_t packets = 0u;
	{
		CLV_ContainerError err = CLV_CONTAINER_OK;
		CLV_WriteParams wp;
		std::memset(&wp, 0, sizeof(wp));
		wp.version_minor = 0;
		wp.index_present = 1;
		wp.fragment_chunk_size = kFragmentChunk;
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(path.c_str(), &wp, &err);
		ASSERT_NE(w, nullptr) << CLV_ContainerStrError(err);
		CLV_StreamDescC d;
		std::memset(&d, 0, sizeof(d));
		d.stream_id = 0u;
		d.stream_type = 0u;
		d.codec_id = 0u;
		d.codec_version = 1;
		d.timebase_num = 1;
		d.timebase_den = kVideoTimebase;
		d.max_packet_size = static_cast<uint32_t>(biggest + 64u);
		d.bit_depth = 8u;
		ASSERT_EQ(CLV_ContainerWriterAddStream(w, &d), CLV_CONTAINER_OK);
		/* 视频流必挂 ext_type = 3 参数块（`docs/容器规范.md` 的扩展块一节，v2 起对视频流必选）：缺块的流被读方记
		 * video_stream_no_param_block 并判该流不可用，下面第三步就取不到帧。
		 * 载荷 8 字节，字段布局照 src/video/headers.cpp 的 ParseEncMeta：
		 *   [0] max_b_frames  [1] quant_precision  [2] ext_tools  [3] reserved  [4..5] width LE16  [6..7] height LE16
		 * 尺寸与精度沿用本件的编码器入参（kW x kH、q = kQp 档无损即 quant_precision = 7），无损通路无 B 帧。 */
		const uint8_t enc_meta[8] = { 0u,	  // max_b_frames
									  kQp,	  // quant_precision
									  0u,	  // ext_tools
									  0u,	  // reserved
									  static_cast<uint8_t>(kW & 0xFFu),
									  static_cast<uint8_t>((kW >> 8) & 0xFFu),
									  static_cast<uint8_t>(kH & 0xFFu),
									  static_cast<uint8_t>((kH >> 8) & 0xFFu) };
		ASSERT_EQ(CLV_ContainerWriterAddExtBlock(w, 0u, 3, 1, enc_meta, sizeof(enc_meta)), CLV_CONTAINER_OK);
		for (uint32_t i = 0u; i < kFrames; ++i)
		{
			ASSERT_EQ(CLV_ContainerWriterWriteFrame(w, 0u, static_cast<uint8_t>(i == 0u ? 1u : 0u),
													static_cast<int64_t>(i), static_cast<int64_t>(i),
													payloads[i].data(), payloads[i].size()),
					  CLV_CONTAINER_OK)
				<< "写帧 " << i;
		}
		ASSERT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
		CLV_ContainerWriterClose(w);
	}

	// 三 · 读回来：帧载荷逐字节等于编码器产出的那一段，并留下容器侧片数作分片证据
	CLV_ContainerError err = CLV_CONTAINER_OK;
	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path.c_str(), &err);
	ASSERT_NE(r, nullptr) << CLV_ContainerStrError(err);
	std::vector<std::vector<uint8_t>> from_file;
	std::vector<uint64_t> frag_seen;
	for (;;)
	{
		CLV_FrameC f;
		std::memset(&f, 0, sizeof(f));
		uint8_t got = 0u;
		ASSERT_EQ(CLV_ContainerReaderNextFrame(r, &f, &got), CLV_CONTAINER_OK);
		if (! got) break;
		ASSERT_EQ(f.stream_id, 0u);
		EXPECT_EQ(f.complete, 1u);
		from_file.emplace_back(f.payload, f.payload + f.payload_size);
		frag_seen.push_back(f.fragments_received);
	}
	ASSERT_EQ(from_file.size(), kFrames);
	// 只有最大的那帧必然被切开（小 P 帧可能就一片），断言落在帧 0 与包数上
	ASSERT_GT(frag_seen[0], 1u) << "帧 0 没被切开，本件没测到分片";
	size_t split_frames = 0u;
	for (size_t i = 0u; i < frag_seen.size(); ++i)
		if (frag_seen[i] > 1u) ++split_frames;
	size_t bad_bytes = 0u;
	for (uint32_t i = 0u; i < kFrames; ++i)
	{
		ASSERT_EQ(from_file[i].size(), payloads[i].size()) << "帧 " << i << " 重组后长度变了";
		for (size_t k = 0u; k < payloads[i].size(); ++k)
			if (from_file[i][k] != payloads[i][k]) ++bad_bytes;
	}

	// 四 · 用文件里的流描述符建解码器，把拼回来的载荷解成帧再逐字节比
	CLV_StreamDescC vd;
	uint8_t usable = 0u;
	ASSERT_EQ(CLV_ContainerReaderGetStream(r, 0u, &vd, &usable), CLV_CONTAINER_OK);
	CLV_ContainerReaderClose(r);
	ASSERT_EQ(usable, 1u);

	CLV_VideoStreamParamsC sp;
	std::memset(&sp, 0, sizeof(sp));
	sp.bit_depth = vd.bit_depth;
	sp.quant_precision = kQp;
	sp.max_b_frames = 0u;
	sp.obmc_disable = 1u;
	sp.display_width = kW;
	sp.display_height = kH;
	CLV_VideoError verr = CLV_VIDEO_OK;
	CLV_VideoDecoder* dec = CLV_VideoDecoderOpen(&sp, &verr);
	ASSERT_NE(dec, nullptr) << CLV_VideoStrError(verr);

	// 先全喂进去再收：冲刷会把句柄打进 EOS 态，边喂边冲刷会让下一帧判 STATE_ERROR
	for (uint32_t i = 0u; i < kFrames; ++i)
	{
		CLV_VideoFrameInfoC info;
		std::memset(&info, 0, sizeof(info));
		ASSERT_EQ(CLV_VideoDecoderDecode(dec, from_file[i].data(), from_file[i].size(), slice_counts[i],
										 static_cast<int64_t>(i), static_cast<int64_t>(i), &info),
				  CLV_VIDEO_OK)
			<< "解码帧 " << i;
		EXPECT_EQ(info.quant_used, 0u);
	}
	size_t out_seq = 0u, bad_px = 0u, checked_px = 0u;
	for (;;)
	{
		CLV_VideoFrame* out = nullptr;
		uint8_t got = 0u;
		if (CLV_VideoDecoderNextOutput(dec, &out, &got) != CLV_VIDEO_OK || got == 0u)
		{
			if (out != nullptr) CLV_VideoFrameRelease(out);
			break;
		}
		bad_px += DiffAgainst(out, src[out_seq], &checked_px);
		++out_seq;
		CLV_VideoFrameRelease(out);
	}
	while (out_seq < static_cast<size_t>(kFrames))
	{
		CLV_VideoFrame* out = nullptr;
		uint8_t got = 0u;
		if (CLV_VideoDecoderFlush(dec, &out, &got) != CLV_VIDEO_OK || got == 0u)
		{
			if (out != nullptr) CLV_VideoFrameRelease(out);
			break;
		}
		bad_px += DiffAgainst(out, src[out_seq], &checked_px);
		++out_seq;
		CLV_VideoFrameRelease(out);
	}
	CLV_VideoDecoderClose(dec);

	std::cout << "无损走容器面：包数 = " << packets << "，分片粒度 = " << kFragmentChunk
			  << " B，最大单帧载荷 = " << biggest << " B，容器侧片数（帧 0）= " << frag_seen[0]
			  << "，视频层片数 = " << slice_counts[0] << "；载荷合计 " << total_payload
			  << " B；读回不符字节 = " << bad_bytes << "；出帧 " << out_seq << "/" << kFrames << "，比对 " << checked_px
			  << " 像素字节，不符 " << bad_px << "\n";

	EXPECT_EQ(bad_bytes, 0u);
	EXPECT_EQ(out_seq, static_cast<size_t>(kFrames));
	EXPECT_EQ(bad_px, 0u);
	EXPECT_GT(packets, static_cast<uint64_t>(kFrames)) << "包数应大于帧数，否则没测到分片";
	EXPECT_GT(split_frames, 0u);
	guard.Discard();
}
