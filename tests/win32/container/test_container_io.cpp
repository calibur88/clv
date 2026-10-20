#include "container/io.h"
#include "container/reader.h"
#include "container/writer.h"
#include "core/crc32.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <vector>

namespace
{

	using clv::container::ContainerErr;
	using clv::container::ContainerReader;
	using clv::container::ContainerWriter;
	using clv::container::DecodePacket;
	using clv::container::FileHead;
	using clv::container::HeadFlagBit;
	using clv::container::MemorySink;
	using clv::container::MemorySource;
	using clv::container::PacketParse;
	using clv::container::ParsedPacket;
	using clv::container::ReaderConfig;
	using clv::container::ReassembledFrame;
	using clv::container::StreamDesc;
	using clv::container::StreamType;
	using clv::container::WriterConfig;
	using clv::container::WriteSummary;

	constexpr uint8_t kVideo = static_cast<uint8_t>(StreamType::kVideo);
	constexpr uint8_t kAudio = static_cast<uint8_t>(StreamType::kAudio);
	constexpr uint8_t kSub = static_cast<uint8_t>(StreamType::kSubtitle);

	StreamDesc MakeStream(uint8_t type, uint8_t id, uint32_t max_packet)
	{
		StreamDesc d;
		d.stream_id = id;
		d.stream_type = type;
		d.codec_id = type == kAudio ? 1 : 0;
		d.codec_version = 1;
		d.max_packet_size = max_packet;
		d.first_dts = 0;
		if (type == kAudio)
		{
			d.timebase_num = 1;
			d.timebase_den = 48000;
			d.sample_rate = 48000;
			d.channels = 2;
			d.bit_depth = 16;
		}
		else
		{
			d.timebase_num = 1;
			d.timebase_den = 1;
			d.sample_rate = 0;
			d.channels = 0;
			d.bit_depth = type == kSub ? 0 : 8;
		}
		return d;
	}

	std::vector<uint8_t> Payload(size_t n, uint8_t seed)
	{
		std::vector<uint8_t> v(n);
		for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 31u + seed) & 0xFFu);
		return v;
	}

	/* 每条视频流必须挂 ext_type = 3 参数块（`docs/容器规范.md` 的扩展块一节，v2 起对视频流必选）：缺块的流会被读方
	 * 记 video_stream_no_param_block 并判该流不可用（src/container/reader.cpp）。载荷 8 字节，
	 * 字段布局照 src/video/headers.cpp 的 ParseEncMeta：
	 *   [0] max_b_frames  [1] quant_precision  [2] ext_tools  [3] reserved  [4..5] width  [6..7] height
	 * 本文件的用例只验容器面，没有真画面对应，尺寸取一个合法非零值（kDisplayW x kDisplayH），
	 * B 帧一律声明 0（这些用例不写 B 帧）。 */
	constexpr uint16_t kDisplayW = 352u;
	constexpr uint16_t kDisplayH = 288u;
	constexpr uint8_t kQuantPrecision = 7u;
	constexpr uint8_t kExtVersion = 1;

	ContainerErr AddVideoParams(ContainerWriter& w, uint8_t stream_id, uint16_t width, uint16_t height)
	{
		const uint8_t meta[8] = { 0u,
								  kQuantPrecision,
								  0u,
								  0u,
								  static_cast<uint8_t>(width & 0xFFu),
								  static_cast<uint8_t>((width >> 8) & 0xFFu),
								  static_cast<uint8_t>(height & 0xFFu),
								  static_cast<uint8_t>((height >> 8) & 0xFFu) };
		return w.AddExtBlock(stream_id, clv::container::kExtTypeEncMeta, kExtVersion, meta, sizeof(meta));
	}

	// 走完包区，返回每个包的起始偏移
	std::vector<uint64_t> PacketOffsets(const std::vector<uint8_t>& buf, uint64_t area_start, uint64_t area_end,
										uint64_t limit)
	{
		std::vector<uint64_t> out;
		uint64_t at = area_start;
		while (at < area_end)
		{
			ParsedPacket p;
			const size_t avail = static_cast<size_t>(area_end - at);
			if (avail < 4) break;
			if (DecodePacket(buf.data() + at, avail, limit, &p) != PacketParse::kOk) break;
			out.push_back(at);
			at += p.WireSize();
		}
		return out;
	}

	struct Built
	{
		std::vector<uint8_t> bytes;
		WriteSummary sum;
	};

}	 // namespace

TEST(ContainerIo, RoundTripsTwoStreamsByteExact)
{
	MemorySink sink;
	ContainerWriter w(sink, WriterConfig {});
	ASSERT_EQ(w.AddStream(MakeStream(kVideo, 0, 4096)), ContainerErr::kOk);
	ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);
	ASSERT_EQ(w.AddStream(MakeStream(kSub, 1, 1024)), ContainerErr::kOk);

	const std::vector<uint8_t> v0 = Payload(900, 1);
	const std::vector<uint8_t> v1 = Payload(1200, 2);
	const std::vector<uint8_t> s0 = Payload(30, 3);

	clv::container::FrameInput in;
	in.stream_id = 0;
	in.is_keyframe = true;
	in.dts = 0;
	in.pts = 80;
	in.payload = v0.data();
	in.payload_size = v0.size();
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);

	in.stream_id = 1;
	in.is_keyframe = false;
	in.dts = 0;
	in.pts = 0;
	in.payload = s0.data();
	in.payload_size = s0.size();
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);

	in.stream_id = 0;
	in.is_keyframe = false;
	in.dts = 43243200;
	in.pts = 43243200;
	in.payload = v1.data();
	in.payload_size = v1.size();
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);

	WriteSummary sum;
	ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);
	EXPECT_EQ(sum.packets, 3u);
	EXPECT_EQ(sum.frames, 3u);
	EXPECT_EQ(sum.index_entries, 3u);
	EXPECT_GT(sum.duration_ticks, 0u);

	MemorySource src(sink.Bytes());
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);
	EXPECT_EQ(r.Descs().size(), 2u);

	std::vector<ReassembledFrame> frames;
	for (;;)
	{
		ReassembledFrame f;
		if (! r.NextFrame(&f)) break;
		frames.push_back(std::move(f));
	}

	const clv::container::ReaderStats& st = r.Stats();
	EXPECT_EQ(st.dropped_crc, 0u);
	EXPECT_EQ(st.dropped_field, 0u);
	EXPECT_EQ(st.resync_count, 0u);
	EXPECT_FALSE(st.abandoned);
	EXPECT_FALSE(st.truncated);
	ASSERT_EQ(frames.size(), 3u);
	EXPECT_EQ(frames[0].payload, v0);
	EXPECT_EQ(frames[0].pts_delta, 80);
	EXPECT_TRUE(frames[0].is_keyframe);
	EXPECT_EQ(frames[1].payload, s0);
	EXPECT_EQ(frames[2].payload, v1);
	EXPECT_EQ(frames[2].dts_delta, 43243200u);
}

TEST(ContainerIo, AudioNegativeInitialDtsIsShiftedByWriter)
{
	// 真实编码器（B 帧前瞻）首包 dts 为负，写方整体平移到 0，读方不做平移
	MemorySink sink;
	ContainerWriter w(sink, WriterConfig {});
	StreamDesc audio = MakeStream(kAudio, 0, 4096);
	audio.timebase_num = 1;
	audio.timebase_den = 1000;	  // 测试用毫秒刻度，避开大数
	audio.sample_rate = 1000;
	ASSERT_EQ(w.AddStream(audio), ContainerErr::kOk);

	const int64_t timeline[] = { -40, 0, 40, 80 };
	std::vector<uint8_t> bytes[4];
	for (int i = 0; i < 4; ++i)
	{
		bytes[i] = Payload(20, static_cast<uint8_t>(i + 10));
		clv::container::FrameInput in;
		in.stream_id = 0;
		in.dts = timeline[i];
		in.pts = timeline[i];
		in.payload = bytes[i].data();
		in.payload_size = bytes[i].size();
		ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	}

	WriteSummary sum;
	ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);
	EXPECT_EQ(w.StreamShift(0), -40);

	MemorySource src(sink.Bytes());
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);

	uint64_t acc = 0;
	int count = 0;
	std::vector<uint64_t> deltas;
	for (;;)
	{
		ReassembledFrame f;
		if (! r.NextFrame(&f)) break;
		deltas.push_back(f.dts_delta);
		acc += f.dts_delta;
		++count;
	}
	ASSERT_EQ(count, 4);
	EXPECT_EQ(deltas[0], 0u);	 // 平移后首帧增量为 0
	for (size_t i = 1; i < deltas.size(); ++i) EXPECT_EQ(deltas[i], 40u);
	EXPECT_EQ(acc, 120u);	 // 80 - (-40)：原始跨度守恒
}

TEST(ContainerIo, FragmentsReassembleAndIndexOncePerFrame)
{
	WriterConfig cfg;
	cfg.fragment_chunk_size = 256;
	MemorySink sink;
	ContainerWriter w(sink, cfg);
	ASSERT_EQ(w.AddStream(MakeStream(kVideo, 0, 4096)), ContainerErr::kOk);
	ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);

	const std::vector<uint8_t> big = Payload(1000, 7);
	clv::container::FrameInput in;
	in.stream_id = 0;
	in.is_keyframe = true;
	in.payload = big.data();
	in.payload_size = big.size();
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);

	WriteSummary sum;
	ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);
	EXPECT_EQ(sum.frames, 2u);
	EXPECT_EQ(sum.packets, 8u);			 // 每帧四片
	EXPECT_EQ(sum.index_entries, 2u);	 // 同一帧只在首分片建条目

	MemorySource src(sink.Bytes());
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);

	ReassembledFrame f;
	ASSERT_TRUE(r.NextFrame(&f));
	EXPECT_EQ(f.payload, big);
	EXPECT_EQ(f.fragments_received, 4u);
	EXPECT_EQ(f.fragments_expected, 4u);
	EXPECT_TRUE(f.complete);
	ASSERT_TRUE(r.NextFrame(&f));
	EXPECT_EQ(f.payload, big);
	EXPECT_FALSE(r.NextFrame(&f));
	EXPECT_EQ(r.Stats().fragment_gaps, 0u);
}

TEST(ContainerIo, PayloadBitFlipDropsPacketWithoutResync)
{
	// CRC 不过只丢包并跳 total_size，不进重同步
	MemorySink sink;
	ContainerWriter w(sink, WriterConfig {});
	ASSERT_EQ(w.AddStream(MakeStream(kVideo, 0, 4096)), ContainerErr::kOk);
	ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);

	const std::vector<uint8_t> a = Payload(300, 1);
	const std::vector<uint8_t> b = Payload(300, 2);
	clv::container::FrameInput in;
	in.stream_id = 0;
	in.payload = a.data();
	in.payload_size = a.size();
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	in.dts = 100;
	in.payload = b.data();
	in.payload_size = b.size();
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);

	WriteSummary sum;
	ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);

	std::vector<uint8_t> bytes = sink.Bytes();
	bytes[static_cast<size_t>(sum.packet_area_start) + 20] ^= 0x01;

	MemorySource src(bytes);
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);

	int frames = 0;
	ReassembledFrame f;
	while (r.NextFrame(&f)) ++frames;
	EXPECT_EQ(frames, 1);
	EXPECT_EQ(r.Stats().dropped_crc, 1u);
	EXPECT_EQ(r.Stats().resync_count, 0u);
	EXPECT_FALSE(r.Stats().abandoned);
}

TEST(ContainerIo, ZeroedTotalSizeTriggersResyncAndRecovers)
{
	MemorySink sink;
	ContainerWriter w(sink, WriterConfig {});
	ASSERT_EQ(w.AddStream(MakeStream(kVideo, 0, 4096)), ContainerErr::kOk);
	ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);

	for (int i = 0; i < 4; ++i)
	{
		const std::vector<uint8_t> p = Payload(200, static_cast<uint8_t>(i));
		clv::container::FrameInput in;
		in.stream_id = 0;
		in.dts = i * 100;
		in.payload = p.data();
		in.payload_size = p.size();
		ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	}
	WriteSummary sum;
	ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);

	std::vector<uint8_t> bytes = sink.Bytes();
	for (size_t i = 0; i < 4; ++i) bytes[static_cast<size_t>(sum.packet_area_start) + i] = 0;

	MemorySource src(bytes);
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);

	int frames = 0;
	ReassembledFrame f;
	while (r.NextFrame(&f)) ++frames;
	EXPECT_EQ(frames, 3);
	EXPECT_EQ(r.Stats().resync_count, 1u);
	EXPECT_EQ(r.Stats().dropped_crc, 0u);
	EXPECT_FALSE(r.Stats().abandoned);
}

TEST(ContainerIo, JunkAtPacketBoundaryIsScannedOutWithinWindow)
{
	// 无索引文件才能安全插入垃圾（不破坏索引里的偏移）；插在包边界上，之后的包应全部找回
	WriterConfig cfg;
	cfg.index_present = false;
	MemorySink sink;
	ContainerWriter w(sink, cfg);
	ASSERT_EQ(w.AddStream(MakeStream(kVideo, 0, 4096)), ContainerErr::kOk);
	ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);

	for (int i = 0; i < 3; ++i)
	{
		const std::vector<uint8_t> p = Payload(200, static_cast<uint8_t>(i));
		clv::container::FrameInput in;
		in.stream_id = 0;
		in.dts = i * 100;
		in.payload = p.data();
		in.payload_size = p.size();
		ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	}
	WriteSummary sum;
	ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);

	const std::vector<uint8_t> built = sink.Bytes();
	const uint64_t area_end_no_index = built.size() - clv::container::kFileTailSize;
	// 外层接受上限 = 该流 max_packet_size + 外层最坏开销，开销取 src/container/reader.h 的具名常量
	const std::vector<uint64_t> offs =
		PacketOffsets(built, sum.packet_area_start, area_end_no_index, 4096ull + clv::container::kOuterOverhead);
	ASSERT_EQ(offs.size(), 3u);

	std::vector<uint8_t> bytes = built;
	const size_t insert_at = static_cast<size_t>(offs[1]);
	const std::vector<uint8_t> junk(16 * 1024, 0x77);
	bytes.insert(bytes.begin() + insert_at, junk.begin(), junk.end());

	MemorySource src(bytes);
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);

	int frames = 0;
	ReassembledFrame f;
	while (r.NextFrame(&f)) ++frames;
	EXPECT_EQ(frames, 3);
	EXPECT_EQ(r.Stats().resync_count, 1u);
	EXPECT_FALSE(r.Stats().abandoned);
}

TEST(ContainerIo, RuntimeLimitShieldsOversizedPackets)
{
	MemorySink sink;
	ContainerWriter w(sink, WriterConfig {});
	ASSERT_EQ(w.AddStream(MakeStream(kVideo, 0, 4096)), ContainerErr::kOk);
	ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);

	for (int i = 0; i < 3; ++i)
	{
		const std::vector<uint8_t> p = Payload(2000, static_cast<uint8_t>(i));
		clv::container::FrameInput in;
		in.stream_id = 0;
		in.dts = i * 100;
		in.payload = p.data();
		in.payload_size = p.size();
		ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	}
	WriteSummary sum;
	ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);

	// 接受上限：只给 512 字节，超的是包不是文件
	ReaderConfig rcfg;
	rcfg.runtime_packet_limit = 512;
	MemorySource src(sink.Bytes());
	ContainerReader r(src, rcfg);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);

	int frames = 0;
	ReassembledFrame f;
	while (r.NextFrame(&f)) ++frames;
	EXPECT_EQ(frames, 0);
	EXPECT_EQ(r.Stats().dropped_too_large, 3u);
	EXPECT_EQ(r.Stats().resync_count, 0u);
	EXPECT_FALSE(r.Stats().abandoned);
}

TEST(ContainerIo, BadDescriptorDisablesOnlyThatStream)
{
	MemorySink sink;
	ContainerWriter w(sink, WriterConfig {});
	ASSERT_EQ(w.AddStream(MakeStream(kVideo, 0, 4096)), ContainerErr::kOk);
	ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);
	ASSERT_EQ(w.AddStream(MakeStream(kSub, 1, 1024)), ContainerErr::kOk);

	const std::vector<uint8_t> v = Payload(300, 1);
	const std::vector<uint8_t> s = Payload(40, 2);
	clv::container::FrameInput in;
	in.stream_id = 0;
	in.payload = v.data();
	in.payload_size = v.size();
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	in.stream_id = 1;
	in.payload = s.data();
	in.payload_size = s.size();
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);

	WriteSummary sum;
	ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);

	std::vector<uint8_t> bytes = sink.Bytes();
	// 第二条描述符（起点 = 文件头 kFileHeadSize + 一条 kStreamDescSize）坏一个字节，不补 CRC：
	// 该条描述符在 Open 就被丢弃（不进 Descs()、不回报坏流标记），于是它的包按未知流丢掉
	bytes[clv::container::kFileHeadSize + clv::container::kStreamDescSize] ^= 0xFF;

	MemorySource src(bytes);
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);
	EXPECT_TRUE(r.StreamUsable(0));
	EXPECT_FALSE(r.StreamUsable(1));

	int frames = 0;
	ReassembledFrame f;
	while (r.NextFrame(&f)) ++frames;
	EXPECT_EQ(frames, 1);
	EXPECT_EQ(r.Stats().dropped_unknown_stream, 1u);
	EXPECT_FALSE(r.Stats().abandoned);
}

TEST(ContainerIo, ExtBlocksRoundTripAndChain)
{
	MemorySink sink;
	ContainerWriter w(sink, WriterConfig {});
	StreamDesc sub = MakeStream(kSub, 0, 4096);
	ASSERT_EQ(w.AddStream(sub), ContainerErr::kOk);

	const std::vector<uint8_t> styles = Payload(577, 9);
	const std::vector<uint8_t> extra = Payload(16, 8);
	ASSERT_EQ(w.AddExtBlock(0, 4, 1, styles.data(), styles.size()), ContainerErr::kOk);
	ASSERT_EQ(w.AddExtBlock(0, 6, 1, extra.data(), extra.size()), ContainerErr::kOk);

	const std::vector<uint8_t> ev = Payload(30, 3);
	clv::container::FrameInput in;
	in.stream_id = 0;
	in.payload = ev.data();
	in.payload_size = ev.size();
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	ASSERT_EQ(w.Finish(nullptr), ContainerErr::kOk);

	MemorySource src(sink.Bytes());
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);
	ASSERT_EQ(r.ExtBlocks().size(), 2u);
	EXPECT_EQ(r.ExtBlocks()[0].type, 4);
	EXPECT_EQ(r.ExtBlocks()[0].data, styles);
	EXPECT_EQ(r.ExtBlocks()[1].type, 6);
	EXPECT_EQ(r.ExtBlocks()[1].data, extra);
	EXPECT_GT(r.ExtBlocks()[1].offset, r.ExtBlocks()[0].offset);
	EXPECT_EQ(r.Stats().ext_blocks_bad, 0u);

	int frames = 0;
	ReassembledFrame f;
	while (r.NextFrame(&f)) ++frames;
	EXPECT_EQ(frames, 1);
}

TEST(ContainerIo, StreamingHeadLeavesPacketCountUnknown)
{
	WriterConfig cfg;
	cfg.streaming = true;
	MemorySink sink;
	ContainerWriter w(sink, cfg);
	ASSERT_EQ(w.AddStream(MakeStream(kVideo, 0, 4096)), ContainerErr::kOk);
	ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);

	const std::vector<uint8_t> p = Payload(120, 4);
	clv::container::FrameInput in;
	in.stream_id = 0;
	in.payload = p.data();
	in.payload_size = p.size();
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	ASSERT_EQ(w.Finish(nullptr), ContainerErr::kOk);

	MemorySource src(sink.Bytes());
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);
	EXPECT_EQ(r.Head().total_packets, clv::container::kUnknownPacketCount);
	EXPECT_EQ(r.Head().flags & static_cast<uint8_t>(HeadFlagBit::kStreaming),
			  static_cast<uint8_t>(HeadFlagBit::kStreaming));

	int frames = 0;
	ReassembledFrame f;
	while (r.NextFrame(&f)) ++frames;
	EXPECT_EQ(frames, 1);
}

TEST(ContainerIo, IndexSortsAcrossStreamsByUnifiedAxis)
{
	// 视频按 tick、音频按样本，两条流的条目按统一时间轴排，不按流分组
	MemorySink sink;
	ContainerWriter w(sink, WriterConfig {});
	StreamDesc video = MakeStream(kVideo, 0, 4096);
	video.timebase_num = 1;
	video.timebase_den = 1081080000;	// 基础时间基
	StreamDesc audio = MakeStream(kAudio, 1, 4096);
	ASSERT_EQ(w.AddStream(video), ContainerErr::kOk);
	ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);
	ASSERT_EQ(w.AddStream(audio), ContainerErr::kOk);

	const std::vector<uint8_t> p = Payload(64, 5);
	clv::container::FrameInput in;
	in.payload = p.data();
	in.payload_size = p.size();

	// 0 ms 视频、480 样本（=10 ms）音频、27027000 tick 视频（=25 ms）
	in.stream_id = 0;
	in.dts = 0;
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	in.stream_id = 1;
	in.dts = 480;
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	in.stream_id = 0;
	in.dts = 27027000;
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);

	WriteSummary sum;
	ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);

	MemorySource src(sink.Bytes());
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);
	ASSERT_TRUE(r.HasIndex());
	ASSERT_EQ(r.IndexEntryCount(), 3u);

	const std::vector<clv::container::IndexEntry>& idx = r.IndexEntries();
	EXPECT_EQ(idx[0].stream_id, 0);
	EXPECT_EQ(idx[1].stream_id, 1);	   // 音频条目夹在两条视频之间
	EXPECT_EQ(idx[2].stream_id, 0);

	// 二分：每条流各自平移到 0，所以两条流的首帧都落在 tick 0，
	// 第三帧（视频 25 ms）才是 27027000 —— 10 ms 之后命中的是它
	const uint64_t ten_ms = 10810800ull;
	EXPECT_EQ(r.LowerBoundTick(ten_ms), 2u);
	EXPECT_EQ(r.LowerBoundTick(0), 0u);
	EXPECT_EQ(r.LowerBoundTick(1081080000ull * 100ull), r.IndexEntryCount());
}

TEST(ContainerIo, RejectsWritesThatWouldBreakTheFormat)
{
	MemorySink sink;
	ContainerWriter w(sink, WriterConfig {});
	StreamDesc v = MakeStream(kVideo, 0, 256);
	ASSERT_EQ(w.AddStream(v), ContainerErr::kOk);
	// 流 1 的 AddStream 本身就拒（见下），所以参数块只挂得上流 0
	ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);

	StreamDesc bad = MakeStream(kVideo, 1, 256);
	bad.first_dts = 5;	  // 非分段文件强制 0
	EXPECT_EQ(w.AddStream(bad), ContainerErr::kValueRange);
	bad.first_dts = 0;
	bad.timebase_num = 0x100000000ull;	  // 超 u32 值域
	EXPECT_EQ(w.AddStream(bad), ContainerErr::kValueRange);

	const std::vector<uint8_t> big = Payload(300, 1);
	// 还在收集流阶段：未知流名的扩展块报 StreamNotFound
	EXPECT_EQ(w.AddExtBlock(9, 4, 1, big.data(), 8), ContainerErr::kStreamNotFound);

	clv::container::FrameInput in;
	in.stream_id = 0;
	in.payload = big.data();
	in.payload_size = big.size();
	EXPECT_EQ(w.WriteFrame(in), ContainerErr::kTooLarge);	 // 超该流 payload 上限

	in.payload_size = 100;
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	in.dts = -5;	// 首帧之后回退
	EXPECT_EQ(w.WriteFrame(in), ContainerErr::kInvalidArgument);

	EXPECT_EQ(w.AddExtBlock(9, 4, 1, big.data(), 8), ContainerErr::kStateError);	// 已出包，阶段不对
	EXPECT_EQ(w.AddStream(v), ContainerErr::kStateError);

	std::vector<uint8_t> tiny = Payload(8, 2);
	in.stream_id = 0;
	in.dts = 100;
	in.payload = tiny.data();
	in.payload_size = tiny.size();
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	ASSERT_EQ(w.Finish(nullptr), ContainerErr::kOk);
	EXPECT_EQ(w.Finish(nullptr), ContainerErr::kStateError);
}

/* 索引区条目数字段的承载域核对（与 src/container/writer.cpp 的 Finish 那条早失败判同界）。
 * 写方一帧只建一条条目 ⇒ 「条目数越出 32 位可表达域」这一档要 4 294 967 296 帧才进得去，
 * 而写方是先在内存里攒 vector<IndexEntry> 再落盘（条目数判排在编码与落盘之前），
 * 门禁里造不出这一档；本例钉它守的域界本身，外加一条同域的可运行对照：
 * 索引头声明的条目数越出「房间 ÷ 条目长」时，读方把索引判不可用，不截断也不越界读。 */
TEST(ContainerIo, IndexEntryCountDomainIsU32AndOversizedDeclarationDisablesIndex)
{
	static_assert(clv::container::kIndexHeadSize == 16u, "索引头定长 16 字节");
	static_assert(clv::container::kIndexEntrySize == 20u, "索引条目定长 20 字节");
	static_assert(sizeof(clv::container::IndexEntry) == 24u, "写方内存态条目一项的字节数（越界档按它估容量）");

	// 线上条目数字段 = u32（structure.h 的 EncodeIndexHead 入参、落盘的 LE32）：取顶格做一次往返
	uint32_t back = 0u;
	std::vector<uint8_t> head_bytes;
	clv::container::EncodeIndexHead(0xFFFFFFFFu, head_bytes);
	ASSERT_EQ(head_bytes.size(), static_cast<size_t>(clv::container::kIndexHeadSize));
	ASSERT_TRUE(clv::container::DecodeIndexHead(head_bytes.data(), head_bytes.size(), &back));
	EXPECT_EQ(back, 0xFFFFFFFFu) << "条目数字段的顶格不是 0xFFFFFFFF";

	// 三帧的字幕文件：条目数与帧数逐格相等（一帧一条条目）
	MemorySink sink;
	ContainerWriter w(sink, WriterConfig {});
	ASSERT_EQ(w.AddStream(MakeStream(kSub, 0, 1024)), ContainerErr::kOk);
	const std::vector<uint8_t> p = Payload(20, 4);
	clv::container::FrameInput in;
	in.stream_id = 0;
	in.is_keyframe = true;
	in.payload = p.data();
	in.payload_size = p.size();
	for (uint32_t i = 0u; i < 3u; ++i)
	{
		in.dts = static_cast<int64_t>(i);
		in.pts = static_cast<int64_t>(i);
		ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	}
	WriteSummary sum;
	ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);
	EXPECT_EQ(sum.index_entries, 3u);
	EXPECT_EQ(sum.frames, 3u);

	{
		MemorySource src(sink.Bytes());
		ContainerReader r(src);
		ASSERT_EQ(r.Open(), ContainerErr::kOk);
		ASSERT_TRUE(r.HasIndex());
		EXPECT_EQ(r.IndexEntryCount(), 3u);
	}

	// 把索引头的条目数改成 u32 顶格并重算索引头的 header_crc32（覆盖 [0,8)），条目区一个字节都不动
	std::vector<uint8_t> bytes = sink.Bytes();
	const size_t ih = static_cast<size_t>(sum.index_area_start);
	ASSERT_GT(ih, 0u);
	ASSERT_LE(ih + clv::container::kIndexHeadSize, bytes.size());
	for (size_t i = 0u; i < 4u; ++i) bytes[ih + i] = static_cast<uint8_t>((0xFFFFFFFFu >> (8u * i)) & 0xFFu);
	const uint32_t recalc = clv::core::Crc32(bytes.data() + ih, 8u);
	for (size_t i = 0u; i < 4u; ++i) bytes[ih + 8u + i] = static_cast<uint8_t>((recalc >> (8u * i)) & 0xFFu);

	MemorySource src2(bytes);
	ContainerReader r2(src2);
	ASSERT_EQ(r2.Open(), ContainerErr::kOk) << "声明条目数越界不该判整文件损坏";
	EXPECT_FALSE(r2.HasIndex()) << "越界的声明条目数被当成可用索引走了";
	EXPECT_EQ(r2.IndexEntryCount(), 0u);
	int frames = 0;
	ReassembledFrame f;
	while (r2.NextFrame(&f)) ++frames;
	EXPECT_EQ(frames, 3) << "索引判不可用不该带走包区";
}

TEST(ContainerIo, EmptyPayloadFrameRoundTrips)
{
	MemorySink sink;
	ContainerWriter w(sink, WriterConfig {});
	ASSERT_EQ(w.AddStream(MakeStream(kSub, 0, 1024)), ContainerErr::kOk);

	clv::container::FrameInput in;
	in.stream_id = 0;
	in.payload = nullptr;
	in.payload_size = 0;
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	ASSERT_EQ(w.Finish(nullptr), ContainerErr::kOk);

	MemorySource src(sink.Bytes());
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);
	ReassembledFrame f;
	ASSERT_TRUE(r.NextFrame(&f));
	EXPECT_EQ(f.payload.size(), 0u);
	EXPECT_TRUE(f.complete);
	EXPECT_FALSE(r.NextFrame(&f));
}

/* 文件头 flags 的保留位闸（判点在 src/container/structure.cpp 的 HeadValuesOk，读方在 Open 里跟判）：
 * 第 0 到第 3 位是声明位（有索引 / 全局有序 / 流式 / 分段，见 HeadFlagBit），第 4 到第 7 位必须为 0
 * （docs/容器规范.md 的 flags 字段定义）。闸取 0xF0：位 3 属合法编码，位 4-7 递交非 0 即整头不成立。
 * flags 落在线上第 13 字节，header_crc32 覆盖 [0, 40)（structure.h 件头），所以改字节必须重算 CRC，
 * 否则坏在 DecodeFileHead 那一关，够不到这道值域闸。 */
TEST(ContainerIo, FileHeadRejectsReservedFlagBits4To7)
{
	// 闸这一侧：底子（除保留位外全部成立）先过，再逐位置第 4 到第 7 位，每一格都必须判不成立
	FileHead base;
	base.stream_count = 1;
	ASSERT_TRUE(clv::container::HeadValuesOk(base));
	const uint8_t reserved_bits[] = { 0x10u, 0x20u, 0x40u, 0x80u };
	for (const uint8_t bit: reserved_bits)
	{
		FileHead h = base;
		h.flags = bit;
		EXPECT_FALSE(clv::container::HeadValuesOk(h))
			<< "保留位 0x" << std::hex << static_cast<unsigned>(bit) << " 置位的文件头被闸受理";
	}

	// 读方这一侧：一条字幕流的合法文件先证开得上，再把线上 flags 的第 4 位置起并重算 CRC 后递交
	MemorySink sink;
	ContainerWriter w(sink, WriterConfig {});
	ASSERT_EQ(w.AddStream(MakeStream(kSub, 0, 1024)), ContainerErr::kOk);
	const std::vector<uint8_t> p = Payload(24, 6);
	clv::container::FrameInput in;
	in.stream_id = 0;
	in.payload = p.data();
	in.payload_size = p.size();
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	ASSERT_EQ(w.Finish(nullptr), ContainerErr::kOk);
	{
		MemorySource src_ok(sink.Bytes());
		ContainerReader r_ok(src_ok);
		ASSERT_EQ(r_ok.Open(), ContainerErr::kOk) << "底子文件自己就开不动，构造失效";
	}

	std::vector<uint8_t> bytes = sink.Bytes();
	ASSERT_GE(bytes.size(), clv::container::kFileHeadSize);
	ASSERT_EQ(bytes[13] & 0xF0u, 0u) << "写方的合法文件头不该带保留位";
	bytes[13] = static_cast<uint8_t>(bytes[13] | 0x10u);
	const uint32_t recalc = clv::core::Crc32(bytes.data(), 40u);
	for (size_t i = 0u; i < 4u; ++i) bytes[40u + i] = static_cast<uint8_t>((recalc >> (8u * i)) & 0xFFu);

	MemorySource src(bytes);
	ContainerReader r(src);
	EXPECT_EQ(r.Open(), ContainerErr::kValueRange) << "保留位第 4 位（0x10）置起的文件头被读方受理";
}

/* 与上一条成对：分段位（第 3 位，0x08）是合法声明，第 4-7 位为 0 的头必须受理。
 * 这两条「受理」断言钉的就是掩码的下边界：保留位掩码一旦写宽到含第 3 位（0xF8），
 * 下面的 EXPECT_TRUE 与分段文件的 Open==kOk 当场变红——合法分段文件会被库自己拒读。 */
TEST(ContainerIo, FileHeadAcceptsSegmentedBit3WhenReservedBitsClear)
{
	static_assert(static_cast<uint8_t>(HeadFlagBit::kSegmented) == 0x08u, "分段位 = 第 3 位（0x08）");

	// 闸这一侧：只置分段位（分段要求 file_ext_offset 非 0，配套给出）
	FileHead h;
	h.stream_count = 1;
	h.flags = static_cast<uint8_t>(HeadFlagBit::kSegmented);
	h.file_ext_offset = clv::container::kFileHeadSize + clv::container::kStreamDescSize;
	EXPECT_TRUE(clv::container::HeadValuesOk(h)) << "分段位置起、保留位全 0 的合法头被拒——掩码写宽过第 3 位了";

	// 读方这一侧：写方产的真分段文件（flags 落线上第 13 字节 = 分段位加索引位）必须开得上
	WriterConfig cfg;
	cfg.segmented = true;
	cfg.seg.flags = 1u;	   // bit0 = 段总数已知
	cfg.seg.index = 0u;
	cfg.seg.total = 1u;
	MemorySink sink;
	ContainerWriter w(sink, cfg);
	ASSERT_EQ(w.AddStream(MakeStream(kSub, 0, 1024)), ContainerErr::kOk);
	const std::vector<uint8_t> p = Payload(24, 6);
	clv::container::FrameInput in;
	in.stream_id = 0;
	in.is_keyframe = true;
	in.payload = p.data();
	in.payload_size = p.size();
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	ASSERT_EQ(w.Finish(nullptr), ContainerErr::kOk);

	const std::vector<uint8_t>& bytes = sink.Bytes();
	ASSERT_GE(bytes.size(), clv::container::kFileHeadSize);
	EXPECT_NE(bytes[13] & 0x08u, 0u) << "分段文件的线上 flags 没带第 3 位";
	EXPECT_EQ(bytes[13] & 0xF0u, 0u) << "分段文件的线上 flags 带了保留位";

	MemorySource src(bytes);
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk) << "分段位合法的文件头被读方拒开";
	EXPECT_TRUE(r.Segmented());
}
