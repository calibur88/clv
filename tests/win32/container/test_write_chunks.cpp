/* test_write_chunks.cpp
 *
 * CLV_ContainerWriterWriteFrameChunks 的覆盖面：本件 13 例 = 6 条正路 + 7 条拒绝档。
 *   正路 —— 单片即一帧一包、多片按数组序一包一片、同帧各片带同一 dts_delta、读回逐字节拼合、
 *     一次坏递交不影响已写成的帧、与 WriteFrame 交错写同一文件（6 例）。
 *   拒绝档 —— 任一片 size 为 0、任一片 data 为 NULL、片数为 0、片数组为 NULL 而片数非 0、
 *     单片越出该流 max_packet_size、flags 的 bit1-7 非 0、一帧声明合计越出单帧上界 2^30（7 例）。
 *   两类都在落盘之前判完：出错时一片都不写（判点次序见 src/container/writer.cpp 的 WriteFrameChunks
 *   与 src/container/clv_container.cpp 的 CLV_ContainerWriterWriteFrameChunks）。
 *
 * 包外层字段用实现层 DecodePacket 直接读文件里的字节，
 * 帧载荷用公开读方重拼出来的结果。后续分片包能过 DecodePacket 的字段区，
 * 就说明线上确实没有 pts_delta——该字段只在帧首包出现，位置由 fragment_index 决定。
 */

#include "CLV_Container.h"
#include "container/packet.h"
#include "container/reader.h"
#include "container/writer.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace
{

	using clv::container::DecodePacket;
	using clv::container::kMaxFramePayload;
	using clv::container::kMaxPacketPayload;
	using clv::container::kOuterOverhead;
	using clv::container::PacketParse;
	using clv::container::ParsedPacket;

	// 外层最坏开销取实现层的具名常量（src/container/reader.h）：单包帧与分片包同值
	constexpr uint64_t kOuterWorstPad = kOuterOverhead;

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

	std::vector<uint8_t> Payload(size_t n, uint8_t seed)
	{
		std::vector<uint8_t> v(n);
		for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 41u + seed) & 0xFFu);
		return v;
	}

	CLV_StreamDescC VideoDesc(uint32_t max_packet)
	{
		CLV_StreamDescC d;
		std::memset(&d, 0, sizeof(d));
		d.stream_id = 0;
		d.stream_type = 0;
		d.codec_id = 0;
		d.codec_version = 1;
		d.timebase_num = 1;
		d.timebase_den = 1081080000;
		d.max_packet_size = max_packet;
		d.bit_depth = 8;
		return d;
	}

	/* 每条视频流必须挂 ext_type = 3 参数块（`docs/容器规范.md` 的扩展块一节，v2 起对视频流必选）：缺块的流会被读方
	 * 记 video_stream_no_param_block 并判该流不可用。载荷 8 字节，字段布局照 src/video/headers.cpp 的
	 * ParseEncMeta：
	 *   [0] max_b_frames  [1] quant_precision  [2] ext_tools  [3] reserved  [4..5] width LE16  [6..7] height LE16
	 * 本件只验分片写入的容器面，画面尺寸取一个合法非零值（kDisplayW x kDisplayH），B 帧声明 0。 */
	constexpr uint16_t kDisplayW = 352u;
	constexpr uint16_t kDisplayH = 288u;

	CLV_ContainerError AddVideoParams(CLV_ContainerWriter* w, uint8_t stream_id, uint16_t width, uint16_t height)
	{
		const uint8_t meta[8] = { 0u,
								  7u,	 // quant_precision
								  0u,	 // ext_tools
								  0u,	 // reserved
								  static_cast<uint8_t>(width & 0xFFu),
								  static_cast<uint8_t>((width >> 8) & 0xFFu),
								  static_cast<uint8_t>(height & 0xFFu),
								  static_cast<uint8_t>((height >> 8) & 0xFFu) };
		return CLV_ContainerWriterAddExtBlock(w, stream_id, 3, 1, meta, sizeof(meta));
	}

	// 打开写方并挂一条视频流（含 v2 必带的 ext_type = 3 参数块）；句柄由调用方 Close
	CLV_ContainerWriter* OpenWriter(const char* path, uint32_t max_packet)
	{
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(path, nullptr, nullptr);
		if (w == nullptr) return nullptr;
		const CLV_StreamDescC d = VideoDesc(max_packet);
		if (CLV_ContainerWriterAddStream(w, &d) != CLV_CONTAINER_OK)
		{
			CLV_ContainerWriterClose(w);
			return nullptr;
		}
		if (AddVideoParams(w, d.stream_id, kDisplayW, kDisplayH) != CLV_CONTAINER_OK)
		{
			CLV_ContainerWriterClose(w);
			return nullptr;
		}
		return w;
	}

	std::vector<uint8_t> ReadWholeFile(const std::string& path)
	{
		std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
		return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	// 每帧只在首包上建索引条目，故索引给出的就是各帧首包的绝对偏移
	std::vector<uint64_t> FrameOffsets(const std::string& path)
	{
		std::vector<uint64_t> out;
		CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path.c_str(), nullptr);
		if (r == nullptr) return out;
		uint32_t count = 0;
		if (CLV_ContainerReaderIndexCount(r, &count) == CLV_CONTAINER_OK)
		{
			for (uint32_t i = 0; i < count; ++i)
			{
				CLV_IndexEntryC e;
				std::memset(&e, 0, sizeof(e));
				if (CLV_ContainerReaderGetIndexEntry(r, i, &e) == CLV_CONTAINER_OK) out.push_back(e.file_offset);
			}
		}
		CLV_ContainerReaderClose(r);
		return out;
	}

	struct PacketFacts
	{
		bool is_fragment = false;
		bool is_last_fragment = false;
		bool is_keyframe = false;
		uint64_t fragment_index = 0;
		uint64_t dts_delta = 0;
		int64_t pts_delta = 0;
		std::vector<uint8_t> payload;
	};

	// 从 from 起连读 want 个包；任何一个包解析不过就返回 false
	bool ReadPackets(const std::vector<uint8_t>& buf, uint64_t from, uint32_t max_packet, size_t want,
					 std::vector<PacketFacts>* out)
	{
		const uint64_t limit = static_cast<uint64_t>(max_packet) + kOuterWorstPad;
		uint64_t at = from;
		for (size_t i = 0; i < want; ++i)
		{
			if (at >= buf.size()) return false;
			ParsedPacket p;
			if (DecodePacket(buf.data() + at, buf.size() - at, limit, &p) != PacketParse::kOk) return false;
			PacketFacts f;
			f.is_fragment = p.is_fragment;
			f.is_last_fragment = p.is_last_fragment;
			f.is_keyframe = p.is_keyframe;
			f.fragment_index = p.fragment_index;
			f.dts_delta = p.dts_delta;
			f.pts_delta = p.pts_delta;
			f.payload = p.payload;
			out->push_back(f);
			at += p.WireSize();
		}
		return true;
	}

	struct FramePayload
	{
		CLV_FrameC head;
		std::vector<uint8_t> payload;
	};

	std::vector<FramePayload> TakeFrames(CLV_ContainerReader* r)
	{
		std::vector<FramePayload> out;
		for (;;)
		{
			CLV_FrameC f;
			std::memset(&f, 0, sizeof(f));
			uint8_t got = 0;
			if (CLV_ContainerReaderNextFrame(r, &f, &got) != CLV_CONTAINER_OK) break;
			if (got == 0) break;
			FramePayload fp;
			fp.head = f;
			if (f.payload != nullptr && f.payload_size != 0) fp.payload.assign(f.payload, f.payload + f.payload_size);
			out.push_back(std::move(fp));
		}
		return out;
	}

}	 // namespace

TEST(ContainerChunks, SingleChunkFrameIsOneUnfragmentedPacket)
{
	const std::string path = TempPath("clv_chunks_one_");
	ScopedFile guard(path);
	const std::vector<uint8_t> only = Payload(64, 7);

	uint64_t packets = 0;
	{
		CLV_ContainerWriter* w = OpenWriter(path.c_str(), 1024);
		ASSERT_NE(w, nullptr);
		const CLV_Chunk chunk { only.data(), only.size() };
		ASSERT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, &chunk, 1, 100, 140, 1), CLV_CONTAINER_OK);
		ASSERT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
		CLV_ContainerWriterClose(w);
	}
	EXPECT_EQ(packets, 1u);

	const std::vector<uint8_t> buf = ReadWholeFile(path);
	const std::vector<uint64_t> offsets = FrameOffsets(path);
	ASSERT_EQ(offsets.size(), 1u);

	std::vector<PacketFacts> pk;
	ASSERT_TRUE(ReadPackets(buf, offsets[0], 1024, 1, &pk));
	EXPECT_FALSE(pk[0].is_fragment);
	EXPECT_FALSE(pk[0].is_last_fragment);
	EXPECT_TRUE(pk[0].is_keyframe);
	EXPECT_EQ(pk[0].payload, only);
	// 首帧的 dts 被平移到 0，pts_delta 恒等于「本帧 PTS - 本帧 DTS」
	EXPECT_EQ(pk[0].dts_delta, 0u);
	EXPECT_EQ(pk[0].pts_delta, 40);

	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path.c_str(), nullptr);
	ASSERT_NE(r, nullptr);
	const std::vector<FramePayload> frames = TakeFrames(r);
	CLV_ContainerReaderClose(r);
	ASSERT_EQ(frames.size(), 1u);
	EXPECT_EQ(frames[0].payload, only);
	EXPECT_EQ(frames[0].head.fragments_received, 1u);
	EXPECT_EQ(frames[0].head.fragments_expected, 1u);
	EXPECT_TRUE(frames[0].head.complete);
}

TEST(ContainerChunks, ChunkedFrameWritesOnePacketPerChunkInArrayOrder)
{
	const std::string path = TempPath("clv_chunks_three_");
	ScopedFile guard(path);
	const std::vector<uint8_t> c0 = Payload(50, 1);
	const std::vector<uint8_t> c1 = Payload(60, 2);
	const std::vector<uint8_t> c2 = Payload(70, 3);

	uint64_t packets = 0;
	{
		CLV_ContainerWriter* w = OpenWriter(path.c_str(), 1024);
		ASSERT_NE(w, nullptr);
		const std::vector<CLV_Chunk> chunks {
			CLV_Chunk { c0.data(), c0.size() },
				CLV_Chunk { c1.data(), c1.size() },
				CLV_Chunk { c2.data(), c2.size() }
		};
		ASSERT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, chunks.data(), 3, 100, 140, 1), CLV_CONTAINER_OK);
		ASSERT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
		CLV_ContainerWriterClose(w);
	}
	EXPECT_EQ(packets, 3u);

	// 索引只给帧首包，一条条目对三个分片包
	const std::vector<uint64_t> offsets = FrameOffsets(path);
	ASSERT_EQ(offsets.size(), 1u);

	const std::vector<uint8_t> buf = ReadWholeFile(path);
	std::vector<PacketFacts> pk;
	ASSERT_TRUE(ReadPackets(buf, offsets[0], 1024, 3, &pk));
	for (size_t i = 0; i < 3; ++i)
	{
		EXPECT_TRUE(pk[i].is_fragment) << "packet " << i;
		EXPECT_EQ(pk[i].fragment_index, i);
		EXPECT_TRUE(pk[i].is_keyframe) << "packet " << i;
	}
	EXPECT_FALSE(pk[0].is_last_fragment);
	EXPECT_FALSE(pk[1].is_last_fragment);
	EXPECT_TRUE(pk[2].is_last_fragment);
	const std::vector<std::vector<uint8_t>> want { c0, c1, c2 };
	for (size_t i = 0; i < 3; ++i) EXPECT_EQ(pk[i].payload, want[i]) << "packet " << i;
	// pts_delta 只落帧首包，后续分片不带（带上了字段区就不成立，上面就解析不过）
	EXPECT_EQ(pk[0].pts_delta, 40);
	EXPECT_EQ(pk[1].pts_delta, 0);
	EXPECT_EQ(pk[2].pts_delta, 0);
}

TEST(ContainerChunks, ChunkedFrameKeepsFrameLevelDtsDeltaOnEveryPacket)
{
	const std::string path = TempPath("clv_chunks_dts_");
	ScopedFile guard(path);
	const std::vector<uint8_t> c0 = Payload(40, 4);
	const std::vector<uint8_t> c1 = Payload(40, 5);

	{
		CLV_ContainerWriter* w = OpenWriter(path.c_str(), 1024);
		ASSERT_NE(w, nullptr);
		const std::vector<uint8_t> one = Payload(30, 6);
		const CLV_Chunk single { one.data(), one.size() };
		ASSERT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, &single, 1, 0, 0, 1), CLV_CONTAINER_OK);
		const std::vector<CLV_Chunk> chunks {
			CLV_Chunk { c0.data(), c0.size() },
				CLV_Chunk { c1.data(), c1.size() }
		};
		ASSERT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, chunks.data(), 2, 360360000, 360360000, 0),
				  CLV_CONTAINER_OK);
		ASSERT_EQ(CLV_ContainerWriterFinish(w, nullptr), CLV_CONTAINER_OK);
		CLV_ContainerWriterClose(w);
	}

	const std::vector<uint8_t> buf = ReadWholeFile(path);
	const std::vector<uint64_t> offsets = FrameOffsets(path);
	ASSERT_EQ(offsets.size(), 2u);

	std::vector<PacketFacts> first;
	ASSERT_TRUE(ReadPackets(buf, offsets[0], 1024, 1, &first));
	EXPECT_EQ(first[0].dts_delta, 0u);

	std::vector<PacketFacts> second;
	ASSERT_TRUE(ReadPackets(buf, offsets[1], 1024, 2, &second));
	// 同帧各片携带同一 dts_delta：帧级冗余，与「相对上一包」的读法在单包帧上等价
	EXPECT_EQ(second[0].dts_delta, second[1].dts_delta);
	EXPECT_EQ(second[0].dts_delta, 360360000u);
}

TEST(ContainerChunks, ReaderReassemblesChunksInByteExactOrder)
{
	const std::string path = TempPath("clv_chunks_join_");
	ScopedFile guard(path);
	const std::vector<uint8_t> c0 = Payload(33, 11);
	const std::vector<uint8_t> c1 = Payload(44, 22);
	const std::vector<uint8_t> c2 = Payload(55, 33);
	std::vector<uint8_t> joined;
	joined.insert(joined.end(), c0.begin(), c0.end());
	joined.insert(joined.end(), c1.begin(), c1.end());
	joined.insert(joined.end(), c2.begin(), c2.end());

	{
		CLV_ContainerWriter* w = OpenWriter(path.c_str(), 1024);
		ASSERT_NE(w, nullptr);
		const std::vector<CLV_Chunk> chunks {
			CLV_Chunk { c0.data(), c0.size() },
				CLV_Chunk { c1.data(), c1.size() },
				CLV_Chunk { c2.data(), c2.size() }
		};
		ASSERT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, chunks.data(), 3, 0, 0, 1), CLV_CONTAINER_OK);
		ASSERT_EQ(CLV_ContainerWriterFinish(w, nullptr), CLV_CONTAINER_OK);
		CLV_ContainerWriterClose(w);
	}

	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path.c_str(), nullptr);
	ASSERT_NE(r, nullptr);
	const std::vector<FramePayload> frames = TakeFrames(r);
	CLV_ReaderStatsC stats;
	std::memset(&stats, 0, sizeof(stats));
	ASSERT_EQ(CLV_ContainerReaderStats(r, &stats), CLV_CONTAINER_OK);
	CLV_ContainerReaderClose(r);

	ASSERT_EQ(frames.size(), 1u);
	EXPECT_EQ(frames[0].payload, joined);
	EXPECT_EQ(frames[0].head.fragments_received, 3u);
	EXPECT_EQ(frames[0].head.fragments_expected, 3u);
	EXPECT_TRUE(frames[0].head.complete);
	EXPECT_EQ(stats.packets_ok, 3u);
	EXPECT_EQ(stats.frames_ok, 1u);
	EXPECT_EQ(stats.dropped_crc, 0u);
	EXPECT_EQ(stats.resync_count, 0u);
	EXPECT_EQ(stats.fragment_gaps, 0u);
}

TEST(ContainerChunks, EmptyChunkIsRejectedBeforeAnyWrite)
{
	const std::string path = TempPath("clv_chunks_empty_");
	ScopedFile guard(path);
	const std::vector<uint8_t> c0 = Payload(20, 1);
	const std::vector<uint8_t> c2 = Payload(20, 2);

	CLV_ContainerWriter* w = OpenWriter(path.c_str(), 1024);
	ASSERT_NE(w, nullptr);
	const std::vector<CLV_Chunk> chunks {
		CLV_Chunk { c0.data(), c0.size() },
			CLV_Chunk { c2.data(), 0		 },
			CLV_Chunk { c2.data(), c2.size() }
	};
	EXPECT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, chunks.data(), 3, 0, 0, 1), CLV_CONTAINER_INVALID_ARGUMENT);
	uint64_t packets = 0;
	ASSERT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
	CLV_ContainerWriterClose(w);
	// 空片不跳过：整次调用一片都不落
	EXPECT_EQ(packets, 0u);
}

TEST(ContainerChunks, NullChunkDataIsRejected)
{
	const std::string path = TempPath("clv_chunks_null_");
	ScopedFile guard(path);

	CLV_ContainerWriter* w = OpenWriter(path.c_str(), 1024);
	ASSERT_NE(w, nullptr);
	const CLV_Chunk chunk { nullptr, 16 };
	EXPECT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, &chunk, 1, 0, 0, 1), CLV_CONTAINER_INVALID_ARGUMENT);
	CLV_ContainerWriterClose(w);
}

TEST(ContainerChunks, ZeroChunkCountIsRejected)
{
	const std::string path = TempPath("clv_chunks_zero_");
	ScopedFile guard(path);
	const std::vector<uint8_t> c0 = Payload(16, 3);

	CLV_ContainerWriter* w = OpenWriter(path.c_str(), 1024);
	ASSERT_NE(w, nullptr);
	const CLV_Chunk chunk { c0.data(), c0.size() };
	EXPECT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, &chunk, 0, 0, 0, 1), CLV_CONTAINER_INVALID_ARGUMENT);
	CLV_ContainerWriterClose(w);
}

TEST(ContainerChunks, NullArrayWithNonZeroCountIsRejected)
{
	const std::string path = TempPath("clv_chunks_nulln_");
	ScopedFile guard(path);

	CLV_ContainerWriter* w = OpenWriter(path.c_str(), 1024);
	ASSERT_NE(w, nullptr);
	EXPECT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, nullptr, 2, 0, 0, 1), CLV_CONTAINER_INVALID_ARGUMENT);
	CLV_ContainerWriterClose(w);
}

TEST(ContainerChunks, OversizeChunkIsTooLarge)
{
	const std::string path = TempPath("clv_chunks_big_");
	ScopedFile guard(path);
	const std::vector<uint8_t> ok = Payload(300, 9);
	const std::vector<uint8_t> big = Payload(1025, 8);

	CLV_ContainerWriter* w = OpenWriter(path.c_str(), 1024);
	ASSERT_NE(w, nullptr);
	const std::vector<CLV_Chunk> chunks {
		CLV_Chunk { ok.data(),	ok.size()  },
			CLV_Chunk { big.data(), big.size() }
	};
	EXPECT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, chunks.data(), 2, 0, 0, 1), CLV_CONTAINER_TOO_LARGE);
	uint64_t packets = 0;
	ASSERT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
	CLV_ContainerWriterClose(w);
	// 越限片与整帧越限同档，同样在落盘前判完
	EXPECT_EQ(packets, 0u);
}

TEST(ContainerChunks, RejectedCallLeavesEarlierFramesIntact)
{
	const std::string path = TempPath("clv_chunks_keep_");
	ScopedFile guard(path);
	const std::vector<uint8_t> c0 = Payload(24, 1);
	const std::vector<uint8_t> bad = Payload(24, 2);

	uint64_t packets = 0;
	{
		CLV_ContainerWriter* w = OpenWriter(path.c_str(), 1024);
		ASSERT_NE(w, nullptr);
		const CLV_Chunk good_chunk { c0.data(), c0.size() };
		ASSERT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, &good_chunk, 1, 0, 0, 1), CLV_CONTAINER_OK);
		const std::vector<CLV_Chunk> broken {
			CLV_Chunk { bad.data(), bad.size() },
			  CLV_Chunk { nullptr,	  8			 }
		};
		EXPECT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, broken.data(), 2, 100, 100, 0),
				  CLV_CONTAINER_INVALID_ARGUMENT);
		const std::vector<CLV_Chunk> again {
			CLV_Chunk { c0.data(),	c0.size()  },
				CLV_Chunk { bad.data(), bad.size() }
		};
		ASSERT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, again.data(), 2, 200, 200, 0), CLV_CONTAINER_OK);
		ASSERT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
		CLV_ContainerWriterClose(w);
	}
	EXPECT_EQ(packets, 3u);

	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path.c_str(), nullptr);
	ASSERT_NE(r, nullptr);
	const std::vector<FramePayload> frames = TakeFrames(r);
	CLV_ContainerReaderClose(r);
	ASSERT_EQ(frames.size(), 2u);
	EXPECT_EQ(frames[0].payload, c0);
	std::vector<uint8_t> want;
	want.insert(want.end(), c0.begin(), c0.end());
	want.insert(want.end(), bad.begin(), bad.end());
	EXPECT_EQ(frames[1].payload, want);
}

TEST(ContainerChunks, ChunkedFramesInterleaveWithWriteFrame)
{
	const std::string path = TempPath("clv_chunks_mix_");
	ScopedFile guard(path);
	const std::vector<uint8_t> plain = Payload(30, 5);
	const std::vector<uint8_t> c0 = Payload(30, 6);
	const std::vector<uint8_t> c1 = Payload(30, 7);

	{
		CLV_ContainerWriter* w = OpenWriter(path.c_str(), 1024);
		ASSERT_NE(w, nullptr);
		ASSERT_EQ(CLV_ContainerWriterWriteFrame(w, 0, 1, 0, 0, plain.data(), plain.size()), CLV_CONTAINER_OK);
		const std::vector<CLV_Chunk> chunks {
			CLV_Chunk { c0.data(), c0.size() },
				CLV_Chunk { c1.data(), c1.size() }
		};
		ASSERT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, chunks.data(), 2, 100, 100, 0), CLV_CONTAINER_OK);
		ASSERT_EQ(CLV_ContainerWriterFinish(w, nullptr), CLV_CONTAINER_OK);
		CLV_ContainerWriterClose(w);
	}

	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path.c_str(), nullptr);
	ASSERT_NE(r, nullptr);
	const std::vector<FramePayload> frames = TakeFrames(r);
	CLV_ContainerReaderClose(r);
	ASSERT_EQ(frames.size(), 2u);
	EXPECT_EQ(frames[0].payload, plain);
	EXPECT_EQ(frames[0].head.fragments_expected, 1u);
	std::vector<uint8_t> want;
	want.insert(want.end(), c0.begin(), c0.end());
	want.insert(want.end(), c1.begin(), c1.end());
	EXPECT_EQ(frames[1].payload, want);
	EXPECT_EQ(frames[1].head.fragments_expected, 2u);
}

/* flags 的保留位档：公开口只用 bit0 = is_keyframe，bit1-7 非 0 一律当场拒。
 * 判点取自公开头 CLV_ContainerWriterWriteFrameChunks 的前置条件「flags：本版只用 bit0 = is_keyframe，
 * bit1-7 必须为 0」，落点在 src/container/clv_container.cpp（掩码 0xFE = bit1..bit7），
 * 排在取片数组与落盘之前 —— 与读侧 src/container/packet.cpp 对包头保留位的同一道口径。
 * 成功档取 bit0 = 0 与 1 两种递交（坏档八个值一个包都不落，两档好递交落成两个包）。 */
TEST(ContainerChunks, ReservedFlagBitsAreRejectedAndBitZeroOneAccepted)
{
	const std::string path = TempPath("clv_chunks_rsvflag_");
	ScopedFile guard(path);

	CLV_ContainerWriter* w = OpenWriter(path.c_str(), 1024);
	ASSERT_NE(w, nullptr);
	const std::vector<uint8_t> c0 = Payload(24, 1);
	const CLV_Chunk chunk { c0.data(), c0.size() };

	static const uint8_t kBadFlags[] = { 0x02u, 0x04u, 0x08u, 0x10u, 0x20u, 0x40u, 0x80u, 0xFFu };
	for (const uint8_t f: kBadFlags)
	{
		SCOPED_TRACE("flags = 0x" + std::to_string(static_cast<unsigned>(f)));
		EXPECT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, &chunk, 1u, 0, 0, f), CLV_CONTAINER_INVALID_ARGUMENT)
			<< "保留位非 0 的递交被静默收下，调用方的位丢了没人说";
	}

	// 成功档：bit0 取 0 与取 1 都受理（同一句柄接着写，时间戳非降）
	const CLV_ContainerError e0 = CLV_ContainerWriterWriteFrameChunks(w, 0, &chunk, 1u, 0, 0, 0);
	const CLV_ContainerError e1 = CLV_ContainerWriterWriteFrameChunks(w, 0, &chunk, 1u, 10, 10, 1);
	EXPECT_EQ(e0, CLV_CONTAINER_OK);
	EXPECT_EQ(e1, CLV_CONTAINER_OK);

	uint64_t packets = 0;
	ASSERT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
	CLV_ContainerWriterClose(w);
	// 八个坏递交一片都没落：包数只等于两档好递交
	EXPECT_EQ(packets, 2u);
}

/* 写方一条早失败判：一帧（含其全部分片）的声明载荷合计越出单帧上界即整次拒、一片都不写。
 * 上界的派生写在 src/container/writer.h 的 kMaxFramePayload（宏块数上界 ÷ 每宏块像素 × 单宏块线上最坏字节
 * = 2^28 ÷ 256 × 1024 = 2^30 = 1 073 741 824 B），本件按常量引用、不另抄一份数。
 * 用例只递送片描述符：合计判排在 EncodePacket 之前，片字节从没被读过，故声明值敢取到单包上界，
 * 不必真产 2^30 字节。界内与界外的片数由下面三条 static_assert 钉住。 */
TEST(ContainerChunks, DeclaredChunkTotalBeyondFrameCapIsRejected)
{
	static_assert(kMaxPacketPayload == 8388567ull, "单包载荷上界 = 默认窗口 8 MiB − 外层最坏 41");
	static_assert(kMaxFramePayload == 1073741824ull, "单帧上界 = 2^30");
	// 128 片 × 8 388 567 = 1 073 736 576 ≤ 2^30（界内最后一档）；129 片 = 1 082 125 143 > 2^30（越界第一档）
	static_assert(128ull * static_cast<uint64_t>(kMaxPacketPayload) <= kMaxFramePayload, "128 片仍在一帧上界内");
	static_assert(129ull * static_cast<uint64_t>(kMaxPacketPayload) > kMaxFramePayload, "129 片越出一帧上界");
	static_assert(static_cast<uint64_t>(kMaxPacketPayload) <= 0xFFFFFFFFull,
				  "单包上界仍在 max_packet_size 的 u32 域内");

	constexpr uint32_t kOneChunk = static_cast<uint32_t>(kMaxPacketPayload);
	const std::string path = TempPath("clv_chunks_framecap_");
	ScopedFile guard(path);

	CLV_ContainerWriter* w = OpenWriter(path.c_str(), kOneChunk);
	ASSERT_NE(w, nullptr);

	// 界内对照档：小合计照常写成
	const std::vector<uint8_t> small = Payload(300, 3);
	const CLV_Chunk small_chunk { small.data(), small.size() };
	ASSERT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, &small_chunk, 1u, 0, 0, 1), CLV_CONTAINER_OK);

	// 越界档：129 片各声明单包上界，合计 1 082 125 143 > 2^30。片数据指针只要求非空，本档不读它
	static const uint8_t kPlaceholder[1] = { 0u };
	std::vector<CLV_Chunk> chunks(129u, CLV_Chunk { kPlaceholder, kOneChunk });
	EXPECT_EQ(CLV_ContainerWriterWriteFrameChunks(w, 0, chunks.data(), 129u, 100, 100, 1), CLV_CONTAINER_TOO_LARGE)
		<< "一帧合计越出 2^30 却没被拒";

	uint64_t packets = 0;
	ASSERT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
	CLV_ContainerWriterClose(w);
	// 越界那次一片都不写：包数仍是对照档那一包
	EXPECT_EQ(packets, 1u);
}
