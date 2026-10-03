#include "container/io.h"
#include "container/reader.h"
#include "container/writer.h"

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
	const std::vector<uint64_t> offs = PacketOffsets(built, sum.packet_area_start, area_end_no_index, 4096 + 41);
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
	// 第二条描述符（偏移 64 + 64）坏一个字节，不补 CRC：只有该流不可用
	bytes[64 + clv::container::kStreamDescSize] ^= 0xFF;

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

	StreamDesc bad = MakeStream(kVideo, 1, 256);
	bad.first_dts = 5;	  // v1 强制 0
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
