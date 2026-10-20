/* test_segments.cpp
 *
 * **分段器落地用例**：容器面的段身份字段与多段续读。实现层直测钉结构层变换，
 * 公开面端到端钉「一组段读成一条连续帧序列」与两个分段专属错误码的产生点。
 *
 * 线上形状（照 docs/容器规范.md）：
 *   文件头 64 = 44 + file_ext_offset 8 + reserved 12；flags bit3 = segmented；
 *   段身份落在文件级扩展块链首块的 ext_type = 7 载荷（48 B），整块长 = 18 + 48 + 4 = 70 B；
 *   链入口只在文件头的 file_ext_offset 里，逐流链（描述符 ext_offset）与它同框不同入口，
 *   所以段描述块不进 ExtBlocks() 视图。
 * 时间轴（后续段不平移那一档）：分段容器的后续段 first_dts = 本段该流首帧的绝对 dts，
 *   写方不平移 ⇒ 落盘 dts 就是绝对值，且首帧 dts 必须与 first_dts 同值；段 0 上每条流的 first_dts 必须为 0
 *   （零起点钉只在写方判，判点与这两档都在 src/container/writer.cpp 的 AddStream 与 AdvanceTimeline）。
 *
 * 段与分片无关：段界落在帧边界上，读方不存在跨段重组；段与段的先后以段块里的 seg_index 为权威，
 *   清单序与文件名只是线索。
 */

#include "CLV_Container.h"
#include "container/io.h"
#include "container/reader.h"
#include "container/structure.h"
#include "container/writer.h"

#include "gtest/gtest.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

	using clv::container::ContainerErr;
	using clv::container::ContainerReader;
	using clv::container::ContainerWriter;
	using clv::container::DecodeFileHead;
	using clv::container::DecodeSegBlock;
	using clv::container::DescValuesOk;
	using clv::container::EncodeFileHead;
	using clv::container::EncodeSegBlock;
	using clv::container::FileHead;
	using clv::container::FrameInput;
	using clv::container::HeadFlagBit;
	using clv::container::HeadValuesOk;
	using clv::container::kExtBlockCrcSize;
	using clv::container::kExtBlockFixedPrefix;
	using clv::container::kExtLenOffset;
	using clv::container::kFileHeadSize;
	using clv::container::kSegBlockSize;
	using clv::container::kStreamDescSize;
	using clv::container::kUnknownSegmentTotal;
	using clv::container::MemorySink;
	using clv::container::MemorySource;
	using clv::container::ReassembledFrame;
	using clv::container::SegBlock;
	using clv::container::StreamDesc;
	using clv::container::StreamType;
	using clv::container::WriterConfig;
	using clv::container::WriteSummary;

	constexpr uint8_t kSub = static_cast<uint8_t>(StreamType::kSubtitle);
	constexpr uint32_t kMaxPacket = 4096u;

	// 段块的整块长：定长前缀 kExtBlockFixedPrefix 18 + 载荷 kSegBlockSize 48 + crc32 kExtBlockCrcSize 4
	constexpr size_t kSegBlockWire = kExtBlockFixedPrefix + kSegBlockSize + kExtBlockCrcSize;

	// 一条字幕流（不挂 ext_type = 3 也合法：必选那只对视频流生效），dts 按 tick 原样走
	StreamDesc MakeStream(uint32_t max_packet = kMaxPacket, uint64_t first_dts = 0)
	{
		StreamDesc d;
		d.stream_id = 0;
		d.stream_type = kSub;
		d.codec_version = 1;
		d.max_packet_size = max_packet;
		d.first_dts = first_dts;
		d.timebase_num = 1;
		d.timebase_den = 1;
		return d;
	}

	std::vector<uint8_t> PayloadOf(uint32_t i, uint8_t seed)
	{
		std::vector<uint8_t> v(16u + i);
		for (size_t k = 0; k < v.size(); ++k) v[k] = static_cast<uint8_t>((k * 31u + seed + i) & 0xFFu);
		return v;
	}

	SegBlock ValidSegBlock(uint64_t index, uint64_t total)
	{
		SegBlock s;
		s.flags = 1u;	 // bit0 = 总数已知
		s.index = index;
		s.total = total;
		s.start_ticks = index * 2ull;
		s.end_ticks = index * 2ull + 1ull;
		return s;
	}

	/* 实现层写一段到内存：帧的 dts 从 first_dts 起连续递增，与声明的 first_dts 同值才写得进去。 */
	std::vector<uint8_t> BuildSegmentMemory(uint64_t first_dts, uint32_t frames, uint8_t seed, uint64_t seg_index = 0,
											uint64_t seg_total = 3u, bool segmented = true)
	{
		WriterConfig cfg;
		cfg.segmented = segmented;
		cfg.seg = ValidSegBlock(seg_index, seg_total);
		MemorySink sink;
		ContainerWriter w(sink, cfg);
		StreamDesc d = MakeStream(kMaxPacket, first_dts);
		if (w.AddStream(d) != ContainerErr::kOk) return {};
		for (uint32_t i = 0; i < frames; ++i)
		{
			const std::vector<uint8_t> p = PayloadOf(i, seed);
			FrameInput in;
			in.stream_id = 0;
			in.is_keyframe = i == 0u;
			in.dts = static_cast<int64_t>(first_dts + i);
			in.pts = static_cast<int64_t>(first_dts + i);
			in.payload = p.data();
			in.payload_size = p.size();
			if (w.WriteFrame(in) != ContainerErr::kOk) return {};
		}
		WriteSummary sum;
		if (w.Finish(&sum) != ContainerErr::kOk) return {};
		return sink.Bytes();
	}

	/* 公开面写一段到真实文件：本件的段清单要经路径打开，所以用临时目录里的实体文件。 */
	struct ScopedFiles
	{
		explicit ScopedFiles(std::vector<std::string> paths): paths_(std::move(paths)) {}

		~ScopedFiles()
		{
			std::error_code ec;
			for (const std::string& p: paths_) std::filesystem::remove(std::filesystem::u8path(p), ec);
		}

		std::vector<const char*> Ptrs() const
		{
			std::vector<const char*> v;
			v.reserve(paths_.size());
			for (const std::string& p: paths_) v.push_back(p.c_str());
			return v;
		}

		const std::string& Path(size_t i) const { return paths_[i]; }

		std::vector<std::string> paths_;
	};

	std::vector<std::string> TempPaths(const char* tag, size_t n)
	{
		std::error_code ec;
		const auto dir = std::filesystem::temp_directory_path(ec);
		std::vector<std::string> out;
		for (size_t i = 0; i < n; ++i)
		{
			out.push_back(
				(dir / (std::string("clv_segments_") + tag + "_" + std::to_string(i) + ".clv")).generic_string());
		}
		return out;
	}

	CLV_WriteParams SegParams(uint64_t seg_index, uint64_t seg_total)
	{
		CLV_WriteParams p;
		std::memset(&p, 0, sizeof(p));
		p.struct_size = static_cast<uint32_t>(sizeof(p));
		p.index_present = 1;
		p.segmented = 1;
		p.seg_flags = 1u | 2u;	  // 总数已知 + 段首帧是关键帧
		p.seg_index = seg_index;
		p.seg_total = seg_total;
		return p;
	}

	/* 写一段：frames 帧、该流 first_dts = 起点绝对值、max_packet 可用来造描述符不一致的段。
	 * 返回 false 时 *err 带着失败档位，调用方据此判据。 */
	bool WriteSegment(const std::string& path, uint64_t first_dts, uint32_t frames, uint8_t seed, uint64_t seg_index,
					  uint64_t seg_total, uint32_t max_packet, CLV_ContainerError* err)
	{
		CLV_WriteParams p = SegParams(seg_index, seg_total);
		p.seg_start_ticks = first_dts;
		p.seg_end_ticks = first_dts + frames - 1ull;
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(path.c_str(), &p, err);
		if (w == nullptr) return false;
		CLV_StreamDescC d;
		std::memset(&d, 0, sizeof(d));
		d.stream_id = 0;
		d.stream_type = kSub;
		d.codec_version = 1;
		d.max_packet_size = max_packet;
		d.first_dts = first_dts;
		d.timebase_num = 1;
		d.timebase_den = 1;
		if (const CLV_ContainerError ae = CLV_ContainerWriterAddStream(w, &d); ae != CLV_CONTAINER_OK)
		{
			CLV_ContainerWriterClose(w);
			// 回填库的实际回报：不拿别的公开码顶「这段没写成」这个内部状态
			*err = ae;
			return false;
		}
		for (uint32_t i = 0; i < frames; ++i)
		{
			const std::vector<uint8_t> pl = PayloadOf(i, seed);
			const CLV_ContainerError e =
				CLV_ContainerWriterWriteFrame(w, 0u, i == 0u ? 1u : 0u, static_cast<int64_t>(first_dts + i),
											  static_cast<int64_t>(first_dts + i), pl.data(), pl.size());
			if (e != CLV_CONTAINER_OK)
			{
				CLV_ContainerWriterClose(w);
				*err = e;
				return false;
			}
		}
		uint64_t packets = 0;
		const CLV_ContainerError fe = CLV_ContainerWriterFinish(w, &packets);
		CLV_ContainerWriterClose(w);
		if (fe != CLV_CONTAINER_OK)
		{
			*err = fe;
			return false;
		}
		*err = CLV_CONTAINER_OK;
		return true;
	}

	// 读尽一个句柄的帧，返回 (stream_id, dts_delta, payload) 三元组的序列
	struct Seen
	{
		uint8_t stream_id = 0;
		uint64_t dts_delta = 0;
		std::vector<uint8_t> payload;
	};

	std::vector<Seen> Drain(CLV_ContainerReader* r, CLV_ContainerError* last)
	{
		std::vector<Seen> out;
		for (;;)
		{
			CLV_FrameC f;
			std::memset(&f, 0, sizeof(f));
			uint8_t got = 0;
			const CLV_ContainerError e = CLV_ContainerReaderNextFrame(r, &f, &got);
			if (last != nullptr) *last = e;
			if (e != CLV_CONTAINER_OK) break;
			if (got == 0u) break;
			Seen s;
			s.stream_id = f.stream_id;
			s.dts_delta = f.dts_delta;
			if (f.payload != nullptr && f.payload_size != 0) s.payload.assign(f.payload, f.payload + f.payload_size);
			out.push_back(std::move(s));
		}
		return out;
	}

	// 尺寸档用例的共同动作：一条字幕流 + 一帧 + 收尾，文件要能被读回来才谈得上观测口的值
	bool SeedOneFrame(CLV_ContainerWriter* w)
	{
		CLV_StreamDescC d;
		std::memset(&d, 0, sizeof(d));
		d.stream_id = 0;
		d.stream_type = kSub;
		d.codec_version = 1;
		d.max_packet_size = kMaxPacket;
		d.first_dts = 0;
		d.timebase_num = 1;
		d.timebase_den = 1;
		if (CLV_ContainerWriterAddStream(w, &d) != CLV_CONTAINER_OK) return false;
		const std::vector<uint8_t> p = PayloadOf(0u, 5u);
		if (CLV_ContainerWriterWriteFrame(w, 0u, 1u, 0, 0, p.data(), p.size()) != CLV_CONTAINER_OK) return false;
		uint64_t packets = 0;
		return CLV_ContainerWriterFinish(w, &packets) == CLV_CONTAINER_OK;
	}

	// 尺寸档：本尺寸的尾部字段起点，即上一版结构的 sizeof
	constexpr uint32_t kParamsSizeOld = static_cast<uint32_t>(offsetof(CLV_WriteParams, segmented));
	constexpr uint8_t kSegFlagTotalKnown = 1u;
	constexpr uint8_t kSegFlagKeyAtHead = 2u;
	constexpr uint8_t kSegFlagGap = 4u;

	/* 分段矩阵的视频面写法：一条视频流，画面尺寸走 ext_type = 3 参数块。
	 * 尺寸不在描述符里（`SameDesc` 比的那 15 个字段没有宽与高），所以段间尺寸矛盾结构上不可能被描述符对账抓到，
	 * 只有扩展块这一层的主动查询口抓得到 —— 见 SegmentExtBlockContentMismatchIsCountedNotRejected。
	 * meta_w 逐段可换，用来造「段间参数块内容矛盾」那一格；with_meta = false 时段里不挂参数块
	 * （视频流没有 ext_type = 3 参数块就被读方摘掉，这是容器面写死的条件）。
	 * extra_sub != 0 时再挂一条字幕流 + 它的样式块，帧按奇偶分到两条流上（多流那格）。
	 * pay_bytes != 0 时把每帧载荷撑到那么多字节，同时抬高该流的 `max_packet_size`（大包那格）。 */
	bool WriteVideoSegment(const std::string& path, uint64_t first_dts, uint32_t frames, uint8_t seed,
						   uint64_t seg_index, uint64_t seg_total, bool with_meta, uint16_t meta_w, uint8_t extra_sub,
						   CLV_ContainerError* err, uint32_t pay_bytes = 0u)
	{
		CLV_WriteParams p = SegParams(seg_index, seg_total);
		p.seg_start_ticks = first_dts;
		p.seg_end_ticks = first_dts + frames - 1ull;
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(path.c_str(), &p, err);
		if (w == nullptr) return false;
		auto bail = [w, err](CLV_ContainerError code) -> bool
		{
			CLV_ContainerWriterClose(w);
			*err = code;
			return false;
		};

		CLV_StreamDescC d;
		std::memset(&d, 0, sizeof(d));
		d.stream_id = 0;
		d.stream_type = static_cast<uint8_t>(StreamType::kVideo);
		d.codec_version = 1;
		d.max_packet_size = (pay_bytes != 0u) ? pay_bytes + 64u : kMaxPacket;
		d.first_dts = static_cast<int64_t>(first_dts);
		d.bit_depth = 8;	// 视频流位深合法域 = 8 / 10 / 12；填 0 在描述符值域那关就被拒（实测）
		d.timebase_num = 1;
		d.timebase_den = 1081080000ull;	   // 正典视频时基
		if (const CLV_ContainerError e0 = CLV_ContainerWriterAddStream(w, &d); e0 != CLV_CONTAINER_OK) return bail(e0);
		if (extra_sub != 0u)
		{
			CLV_StreamDescC s = d;
			s.stream_id = 1;
			s.stream_type = kSub;
			s.bit_depth = 0;
			s.timebase_den = 1;
			/* 两条流的 first_dts 都取本段起点（与视频流同值，不再 +1）：依据两判同向 ——
			 * 段 0 上写方要求每条流 first_dts = 0（src/container/writer.cpp 的 AddStream，规范侧见
			 * docs/容器规范.md 流描述符表那一行「首段 = 0，写方判」），后续段不平移时要求该流第一帧的 dts
			 * 等于它自己声明的 first_dts（同文件 AdvanceTimeline 的 absolute_timeline 那一判）。
			 * 奇偶分流下字幕流的第一帧是 i = 1，所以它的 dts 按自己那半条轴算（见下面的 stream_dts），
			 * 声明值与本段起点同值才同时过这两判。 */
			s.first_dts = static_cast<int64_t>(first_dts);
			if (const CLV_ContainerError e1 = CLV_ContainerWriterAddStream(w, &s); e1 != CLV_CONTAINER_OK)
				return bail(e1);
			const uint8_t style[4] = { 1u, 0u, 0u, 0u };
			if (const CLV_ContainerError e2 = CLV_ContainerWriterAddExtBlock(w, 1u, 4u, 1u, style, sizeof(style));
				e2 != CLV_CONTAINER_OK)
			{
				return bail(e2);
			}
		}
		if (with_meta)
		{
			const uint16_t meta_h = 288u;
			const uint8_t meta[8] = { 0u,
									  7u,
									  0u,
									  0u,
									  static_cast<uint8_t>(meta_w & 0xFFu),
									  static_cast<uint8_t>(meta_w >> 8),
									  static_cast<uint8_t>(meta_h & 0xFFu),
									  static_cast<uint8_t>(meta_h >> 8) };
			if (const CLV_ContainerError e3 = CLV_ContainerWriterAddExtBlock(w, 0u, 3u, 1u, meta, sizeof(meta));
				e3 != CLV_CONTAINER_OK)
			{
				return bail(e3);
			}
		}
		for (uint32_t i = 0; i < frames; ++i)
		{
			std::vector<uint8_t> pl = PayloadOf(i, seed);
			if (pay_bytes != 0u)
			{
				pl.assign(pay_bytes, 0u);
				for (size_t k = 0; k < pl.size(); ++k) pl[k] = static_cast<uint8_t>((k * 17u + seed + i) & 0xFFu);
			}
			const uint8_t sid = (extra_sub != 0u && (i & 1u) != 0u) ? 1u : 0u;
			// 奇偶分流后各流自己的轴：流 0 收偶数帧（偏移 0、2…），流 1 收奇数帧（偏移 0、2…），
			// 两条流在本段的第一帧都落在本段起点 first_dts 上
			const int64_t stream_dts = static_cast<int64_t>(first_dts + (sid == 0u ? i : i - 1u));
			const CLV_ContainerError e =
				CLV_ContainerWriterWriteFrame(w, sid, i == 0u ? 1u : 0u, stream_dts, stream_dts, pl.data(), pl.size());
			if (e != CLV_CONTAINER_OK) return bail(e);
		}
		uint64_t packets = 0;
		const CLV_ContainerError fe = CLV_ContainerWriterFinish(w, &packets);
		CLV_ContainerWriterClose(w);
		*err = fe;
		return fe == CLV_CONTAINER_OK;
	}

	// 三条段的指针数组：OpenSegments 要的是 const char* 数组，字符串本体留在 names 里
	std::vector<const char*> SegPtrs(const std::vector<std::string>& names, std::vector<const char*>* raw)
	{
		raw->clear();
		for (const std::string& s: names) raw->push_back(s.c_str());
		return *raw;
	}

}	 // namespace

TEST(Segments, SegBlockWireIsFortyEightBytesAndDecodesBack)
{
	const SegBlock s = ValidSegBlock(3u, 14u);
	std::vector<uint8_t> bytes;
	EncodeSegBlock(s, bytes);
	ASSERT_EQ(bytes.size(), kSegBlockSize);
	static_assert(kSegBlockSize == 48u, "段描述块载荷长写死 48 字节");

	SegBlock back;
	ASSERT_TRUE(DecodeSegBlock(bytes.data(), bytes.size(), &back));
	EXPECT_EQ(back.flags, s.flags);
	EXPECT_EQ(back.index, s.index);
	EXPECT_EQ(back.total, s.total);
	EXPECT_EQ(back.start_ticks, s.start_ticks);
	EXPECT_EQ(back.end_ticks, s.end_ticks);
	// 短一个字节就不成立：48 是定长，没有「读多少算多少」这一档
	EXPECT_FALSE(DecodeSegBlock(bytes.data(), kSegBlockSize - 1u, &back));
	EXPECT_FALSE(DecodeSegBlock(nullptr, kSegBlockSize, &back));
}

TEST(Segments, SegBlockValueDomainRejectsEveryReservedAndFlagCase)
{
	const SegBlock ok = ValidSegBlock(2u, 3u);
	std::vector<uint8_t> base;
	EncodeSegBlock(ok, base);
	SegBlock back;
	ASSERT_TRUE(DecodeSegBlock(base.data(), base.size(), &back));

	// 逐档：每档只破坏一处，且都有一份「不破坏就成立」的对照（上面那条）
	for (size_t i = 34u; i < 48u; ++i)
	{
		std::vector<uint8_t> bytes = base;
		bytes[i] = 0x01u;
		EXPECT_FALSE(DecodeSegBlock(bytes.data(), bytes.size(), &back)) << "reserved 字节 " << i << " 非 0 却受理";
	}

	struct FlagCase
	{
		const char* what;
		uint8_t flags;
		uint64_t total;
	};

	const FlagCase cases[] = {
		{ "seg_flags bit3 非 0（reserved）", static_cast<uint8_t>(kSegFlagTotalKnown | 8u),	   3u					},
		{ "seg_flags bit7 非 0（reserved）", static_cast<uint8_t>(kSegFlagTotalKnown | 0x80u), 3u					},
		{ "seg_total 填 0（未知要用全 1）",	 kSegFlagTotalKnown,							   0u					},
		{ "bit0 说总数已知却填全 1",		 kSegFlagTotalKnown,							   kUnknownSegmentTotal },
		{ "bit0 说未知却填了数",			 0u,											   5u					},
	};
	for (const FlagCase& c: cases)
	{
		SegBlock s = ok;
		s.flags = c.flags;
		s.total = c.total;
		std::vector<uint8_t> bytes;
		EncodeSegBlock(s, bytes);
		EXPECT_FALSE(DecodeSegBlock(bytes.data(), bytes.size(), &back)) << c.what;
	}

	// 段尾时刻早于段首时刻：时间轴倒挂，不成立
	{
		SegBlock s = ok;
		s.start_ticks = 9u;
		s.end_ticks = 8u;
		std::vector<uint8_t> bytes;
		EncodeSegBlock(s, bytes);
		EXPECT_FALSE(DecodeSegBlock(bytes.data(), bytes.size(), &back)) << "end_ticks < start_ticks 却受理";
	}
	// 总数未知（bit0 清 + 全 1）是合法档，不是拒的对象
	{
		SegBlock s = ValidSegBlock(0u, kUnknownSegmentTotal);
		s.flags = 0u;
		std::vector<uint8_t> bytes;
		EncodeSegBlock(s, bytes);
		EXPECT_TRUE(DecodeSegBlock(bytes.data(), bytes.size(), &back)) << "未知总数这一档被误拒";
		EXPECT_EQ(back.total, kUnknownSegmentTotal);
	}
}

TEST(Segments, HeadValuesOkTiesSegmentedFlagToFileExtOffsetBothWays)
{
	FileHead h;
	h.stream_count = 1;
	h.flags = static_cast<uint8_t>(HeadFlagBit::kIndexPresent);	   // 带索引区才允许 index_offset 非 0
	h.total_packets = 2;
	h.index_offset = 100;
	ASSERT_TRUE(HeadValuesOk(h));	 // 非分段 + 偏移 0

	h.flags |= static_cast<uint8_t>(HeadFlagBit::kSegmented);
	EXPECT_FALSE(HeadValuesOk(h)) << "说自己是分段容器却没有文件级链入口";

	h.file_ext_offset = kFileHeadSize + kStreamDescSize;	// 一段一流：链首紧跟描述符表
	EXPECT_TRUE(HeadValuesOk(h));

	h.flags &= static_cast<uint8_t>(~static_cast<uint8_t>(HeadFlagBit::kSegmented));
	EXPECT_FALSE(HeadValuesOk(h)) << "不分段却带着一个无主的偏移";

	// reserved 两段的非 0 递交照旧拒（本版必须全 0）
	FileHead r;
	r.stream_count = 1;
	r.reserved_tail = 1;
	EXPECT_FALSE(HeadValuesOk(r));
	r.reserved_tail = 0;
	r.reserved_tail2 = 1;
	EXPECT_FALSE(HeadValuesOk(r));
}

TEST(Segments, DescValuesOkOpensFirstDtsOnlyForSegmentedFiles)
{
	StreamDesc d = MakeStream(kMaxPacket, 0);
	EXPECT_TRUE(DescValuesOk(d, false));
	EXPECT_TRUE(DescValuesOk(d, true));

	d.first_dts = 5;
	EXPECT_FALSE(DescValuesOk(d, false)) << "非分段文件的零起点钉被放宽了";
	EXPECT_TRUE(DescValuesOk(d, true)) << "分段容器的后续段拿不到本段起点";

	// 文件头的编码与解码把 file_ext_offset 原样带回，round trip 后值域判仍成立
	FileHead h;
	h.stream_count = 1;
	h.total_packets = 2;
	h.index_offset = 100;
	h.flags = static_cast<uint8_t>(HeadFlagBit::kSegmented) | static_cast<uint8_t>(HeadFlagBit::kIndexPresent);
	h.file_ext_offset = 132u;
	std::vector<uint8_t> bytes;
	EncodeFileHead(h, bytes);
	ASSERT_EQ(bytes.size(), kFileHeadSize);
	FileHead back;
	ASSERT_TRUE(DecodeFileHead(bytes.data(), bytes.size(), &back));
	EXPECT_EQ(back.file_ext_offset, 132u);
	EXPECT_TRUE(HeadValuesOk(back));
}

TEST(Segments, WriterEmitsFileLevelChainAndReaderRecoversTheBlock)
{
	const std::vector<uint8_t> bytes = BuildSegmentMemory(2u, 2u, 7u, 1u, 3u);
	ASSERT_FALSE(bytes.empty());
	// 链首位置算式：文件头 64 + 描述符 68×1 = 132，段块整块长 70
	const uint64_t chain_at = kFileHeadSize + kStreamDescSize;
	ASSERT_GE(bytes.size(), chain_at + kSegBlockWire);

	MemorySource src(bytes);
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);
	EXPECT_TRUE(r.Segmented());
	const SegBlock* sb = r.Seg();
	ASSERT_NE(sb, nullptr);
	EXPECT_EQ(sb->flags, kSegFlagTotalKnown);
	EXPECT_EQ(sb->index, 1u);
	EXPECT_EQ(sb->total, 3u);
	EXPECT_EQ(sb->start_ticks, 2u);
	EXPECT_EQ(sb->end_ticks, 3u);
	EXPECT_EQ(r.Head().file_ext_offset, chain_at);

	// 段块只在文件级链里，不进逐流的扩展块视图
	EXPECT_TRUE(r.ExtBlocks().empty());
	// 观测三档都是 0：块拿到了、解析成立、没声明间断
	const ContainerReader::SegmentStats& s = r.SegStats();
	EXPECT_EQ(s.block_missing, 0u);
	EXPECT_EQ(s.block_bad, 0u);
	EXPECT_EQ(s.discontinuities, 0u);

	size_t frames = 0;
	ReassembledFrame f;
	while (r.NextFrame(&f)) ++frames;
	EXPECT_EQ(frames, 2u);
}

TEST(Segments, ReaderCountsSegBlockAbsentBadAndDeclaredGap)
{
	const std::vector<uint8_t> good = BuildSegmentMemory(2u, 2u, 7u, 1u, 3u);
	ASSERT_FALSE(good.empty());

	// 一，声明了分段但入口指向文件之外：块拿不到，计 block_missing，文件照开
	{
		std::vector<uint8_t> bytes = good;
		FileHead h;
		ASSERT_TRUE(DecodeFileHead(bytes.data(), kFileHeadSize, &h));
		h.file_ext_offset = bytes.size() + 16u;
		const size_t before = bytes.size();
		std::vector<uint8_t> rebuilt;
		EncodeFileHead(h, rebuilt);
		ASSERT_EQ(rebuilt.size(), kFileHeadSize);
		std::copy(rebuilt.begin(), rebuilt.end(), bytes.begin());
		ASSERT_EQ(bytes.size(), before);

		MemorySource src(bytes);
		ContainerReader r(src);
		ASSERT_EQ(r.Open(), ContainerErr::kOk) << "段标识失效不该判整文件损坏";
		EXPECT_EQ(r.Seg(), nullptr);
		EXPECT_EQ(r.SegStats().block_missing, 1u);
		EXPECT_EQ(r.SegStats().block_bad, 0u);
	}

	// 二，块读得进但载荷不成立（reserved 非 0）：计 block_bad
	{
		std::vector<uint8_t> bytes = good;
		const uint64_t payload_at = kFileHeadSize + kStreamDescSize + kExtBlockFixedPrefix;
		bytes[static_cast<size_t>(payload_at) + 34u] ^= 0xFFu;	  // 载荷首块的 reserved 段
		MemorySource src(bytes);
		ContainerReader r(src);
		ASSERT_EQ(r.Open(), ContainerErr::kOk);
		EXPECT_EQ(r.Seg(), nullptr);
		EXPECT_EQ(r.SegStats().block_bad, 1u);
		EXPECT_EQ(r.SegStats().block_missing, 0u);
	}

	/* 二之二，段块的声明长度超出接受域：同样计 block_bad。
	 * 接受域上界 = 默认读方窗口 − 扩展块定长前缀 − 块尾 CRC = 8 MiB − 18 − 4 = 8 388 586
	 * （算式与 src/container/reader.cpp 的 kMaxExtPayloadAccept 同式，该常量不经头暴露，此处按算式取）。
	 * 判式是「声明长度 > 上界」⇒ 取上界 + 1 = 8 388 587。文件级链与逐流链走同一口径：
	 * 声明长度超出接受域的块同样计入块计数：块计数记的是「解析不成立的块数」，含接受域外的那一类。
	 * 接受域判排在分配与整块读入之前，故这一格不必真备 8 MiB 载荷。 */
	{
		std::vector<uint8_t> bytes = good;
		constexpr uint64_t over = clv::container::kDefaultReadWindow - static_cast<uint64_t>(kExtBlockFixedPrefix) -
								  static_cast<uint64_t>(kExtBlockCrcSize) + 1ull;
		static_assert(over == 8388587ull, "接受域上界 8 MiB − 18 − 4 再 +1");
		const uint64_t len_at = kFileHeadSize + kStreamDescSize + kExtLenOffset;
		for (size_t i = 0u; i < 4u; ++i)
			bytes[static_cast<size_t>(len_at) + i] = static_cast<uint8_t>((over >> (8u * i)) & 0xFFu);

		MemorySource src(bytes);
		ContainerReader r(src);
		ASSERT_EQ(r.Open(), ContainerErr::kOk) << "域外声明只停这条链，不判整文件损坏";
		EXPECT_EQ(r.Seg(), nullptr);
		EXPECT_EQ(r.SegStats().block_bad, 1u) << "接受域外的段块同样计入块计数";
		EXPECT_EQ(r.SegStats().block_missing, 0u) << "块读得进，不该同时计成拿不到";
	}

	// 三，段块声明与上一段之间有间断：discontinuities 计数，读面照常
	{
		WriterConfig cfg;
		cfg.segmented = true;
		cfg.seg = ValidSegBlock(1u, 3u);
		cfg.seg.flags = static_cast<uint8_t>(kSegFlagTotalKnown | kSegFlagGap);
		MemorySink sink;
		ContainerWriter w(sink, cfg);
		ASSERT_EQ(w.AddStream(MakeStream(kMaxPacket, 2u)), ContainerErr::kOk);
		const std::vector<uint8_t> p = PayloadOf(0u, 3u);
		FrameInput in;
		in.stream_id = 0;
		in.is_keyframe = true;
		in.dts = 2;
		in.pts = 2;
		in.payload = p.data();
		in.payload_size = p.size();
		ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
		WriteSummary sum;
		ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);

		MemorySource src(sink.Bytes());
		ContainerReader r(src);
		ASSERT_EQ(r.Open(), ContainerErr::kOk);
		ASSERT_NE(r.Seg(), nullptr);
		EXPECT_EQ(r.SegStats().discontinuities, 1u);
		EXPECT_EQ(r.SegStats().block_missing, 0u);
	}
}

TEST(Segments, WriterRefusesNonZeroFirstDtsOnFirstSegmentOnly)
{
	/* 首段的零起点钉只在写方判（src/container/writer.cpp 的 AddStream）：
	 * 段 0 就是整条逻辑序列的开头，该流 first_dts 必须为 0；放开它的是分段容器的后续段。
	 * 判点排在 DescValuesOk 之后、任何字节落盘之前，与同函数其余三档同码同位置。 */
	{
		WriterConfig cfg;
		cfg.segmented = true;
		cfg.seg = ValidSegBlock(0u, 3u);	// 段 0
		MemorySink sink;
		ContainerWriter w(sink, cfg);
		EXPECT_EQ(w.AddStream(MakeStream(kMaxPacket, 5u)), ContainerErr::kValueRange)
			<< "段 0 的非零 first_dts 被放了行";
		// 同一句柄、同一档配置：first_dts = 0 照样受理（拒绝档的对照档）
		ASSERT_EQ(w.AddStream(MakeStream(kMaxPacket, 0u)), ContainerErr::kOk);
	}

	// 后续段放开：段 1 的 first_dts 就是本段该流首帧的绝对 dts，非零合法
	{
		WriterConfig cfg;
		cfg.segmented = true;
		cfg.seg = ValidSegBlock(1u, 3u);
		MemorySink sink;
		ContainerWriter w(sink, cfg);
		ASSERT_EQ(w.AddStream(MakeStream(kMaxPacket, 5u)), ContainerErr::kOk) << "后续段被零起点钉挡住了";
	}

	// 不分段那一档的零起点钉照旧（新档排在它之后，不替换它）
	{
		WriterConfig cfg;	 // segmented = false，seg.index 不参与判
		MemorySink sink;
		ContainerWriter w(sink, cfg);
		EXPECT_EQ(w.AddStream(MakeStream(kMaxPacket, 5u)), ContainerErr::kValueRange);
		EXPECT_EQ(w.AddStream(MakeStream(kMaxPacket, 0u)), ContainerErr::kOk);
	}

	/* 读方那一层没有「首段零起点」判：src/container/reader.cpp 只按文件头 flags 的 segmented 位调 DescValuesOk，
	 * 拿不到段号，因此首段带非零 first_dts 的在盘文件不会被这道新档判坏。 */
	StreamDesc d = MakeStream(kMaxPacket, 5u);
	EXPECT_TRUE(DescValuesOk(d, true)) << "读方那一层出现了首段零起点判";

	// 公开面同形：段 0 递交非零 first_dts 当场拒，段 0 递交 0 照常写成
	{
		const ScopedFiles guard(TempPaths("firstseg", 2u));
		CLV_ContainerError err = CLV_CONTAINER_OK;
		{
			// WriteSegment 把库的实际回报回填到 err，这里直接读那一档的码
			EXPECT_FALSE(WriteSegment(guard.Path(0u), 5u, 2u, 11u, 0u, 3u, kMaxPacket, &err));
			EXPECT_EQ(err, CLV_CONTAINER_VALUE_RANGE) << "公开面的段 0 非零 first_dts 没被拒";
		}
		{
			err = CLV_CONTAINER_OK;
			// 对照档：段 0 的零起点仍照常写成，且能被读方打开
			ASSERT_TRUE(WriteSegment(guard.Path(1u), 0u, 2u, 22u, 0u, 3u, kMaxPacket, &err))
				<< CLV_ContainerStrError(err);
			CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(guard.Path(1u).c_str(), &err);
			ASSERT_NE(r, nullptr) << CLV_ContainerStrError(err);
			CLV_ContainerReaderClose(r);
		}
	}
}

TEST(Segments, WriterRefusesFrameTimesThatDisagreeWithDeclaredFirstDts)
{
	// 后续段不平移 ⇒ 首帧 dts 必须与该流声明的 first_dts 同值，差一拍就判值域不成立
	WriterConfig cfg;
	cfg.segmented = true;
	cfg.seg = ValidSegBlock(1u, 3u);
	MemorySink sink;
	ContainerWriter w(sink, cfg);
	ASSERT_EQ(w.AddStream(MakeStream(kMaxPacket, 2u)), ContainerErr::kOk);
	const std::vector<uint8_t> p = PayloadOf(0u, 1u);
	FrameInput in;
	in.stream_id = 0;
	in.is_keyframe = true;
	in.dts = 3;	   // 声明的段起点是 2
	in.pts = 3;
	in.payload = p.data();
	in.payload_size = p.size();
	EXPECT_EQ(w.WriteFrame(in), ContainerErr::kValueRange);

	in.dts = 2;
	in.pts = 2;
	ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
	// 第二帧不必等于 first_dts，只要求非降
	in.dts = 4;
	in.pts = 4;
	EXPECT_EQ(w.WriteFrame(in), ContainerErr::kOk);
}

TEST(Segments, WriteParamsSizeTiersOldNewZeroAcceptedForeignRejected)
{
	const ScopedFiles guard(TempPaths("tiers", 2u));
	CLV_ContainerError err = CLV_CONTAINER_OK;

	// 本尺寸：分段身份读得到
	{
		CLV_WriteParams p = SegParams(0u, 3u);
		p.seg_start_ticks = 0;
		p.seg_end_ticks = 0;
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(guard.Path(0u).c_str(), &p, &err);
		ASSERT_EQ(err, CLV_CONTAINER_OK);
		ASSERT_NE(w, nullptr);
		ASSERT_TRUE(SeedOneFrame(w));
		CLV_ContainerWriterClose(w);
		CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(guard.Path(0u).c_str(), &err);
		ASSERT_NE(r, nullptr);
		uint64_t total = 0;
		ASSERT_EQ(CLV_ContainerReaderSegmentTotal(r, &total), CLV_CONTAINER_OK);
		EXPECT_EQ(total, 3u) << "本尺寸递交应当拿到 seg_total";
		CLV_ContainerReaderClose(r);
	}

	// 旧尺寸：尾部按不分段处理，即使那段内存里写着分段值
	{
		std::vector<uint8_t> raw(sizeof(CLV_WriteParams), 0u);
		CLV_WriteParams* p = reinterpret_cast<CLV_WriteParams*>(raw.data());
		std::memset(p, 0, raw.size());
		p->struct_size = kParamsSizeOld;
		p->index_present = 1;
		// 尾部填值：这些字节在旧布局里不存在，本布局不得当字段读
		p->segmented = 1;
		p->seg_flags = 3u;
		p->seg_index = 9u;
		p->seg_total = 9u;
		p->seg_start_ticks = 1u;
		p->seg_end_ticks = 2u;
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(guard.Path(1u).c_str(), p, &err);
		ASSERT_EQ(err, CLV_CONTAINER_OK) << "旧尺寸该被受理（向后承接），不是拒";
		ASSERT_NE(w, nullptr);
		ASSERT_TRUE(SeedOneFrame(w));
		CLV_ContainerWriterClose(w);

		CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(guard.Path(1u).c_str(), &err);
		ASSERT_NE(r, nullptr);
		CLV_FileHeadC head;
		std::memset(&head, 0, sizeof(head));
		ASSERT_EQ(CLV_ContainerReaderGetHead(r, &head), CLV_CONTAINER_OK);
		EXPECT_EQ(head.flags & 8u, 0u) << "旧尺寸的尾部被当成字段读，产出了自称分段的文件";
		uint64_t total = 0;
		ASSERT_EQ(CLV_ContainerReaderSegmentTotal(r, &total), CLV_CONTAINER_OK);
		EXPECT_EQ(total, kUnknownSegmentTotal) << "不分段的文件该报未知，而不是 0";
		uint64_t index = 9;
		ASSERT_EQ(CLV_ContainerReaderSegmentIndex(r, &index), CLV_CONTAINER_OK);
		EXPECT_EQ(index, 0u);
		CLV_ContainerReaderClose(r);
	}

	// 留 0 走默认（memset 存量写法）：同样按不分段处理
	{
		CLV_WriteParams p;
		std::memset(&p, 0, sizeof(p));
		p.index_present = 1;
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(guard.Path(0u).c_str(), &p, &err);
		ASSERT_EQ(err, CLV_CONTAINER_OK);
		ASSERT_NE(w, nullptr);
		ASSERT_TRUE(SeedOneFrame(w));
		CLV_ContainerWriterClose(w);
		CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(guard.Path(0u).c_str(), &err);
		ASSERT_NE(r, nullptr);
		uint64_t total = 0;
		ASSERT_EQ(CLV_ContainerReaderSegmentTotal(r, &total), CLV_CONTAINER_OK);
		EXPECT_EQ(total, kUnknownSegmentTotal);
		CLV_ContainerReaderClose(r);
	}

	// 其余尺寸一律拒，且一个字节都不落
	for (uint32_t bad: { kParamsSizeOld + 1u, 1u, static_cast<uint32_t>(sizeof(CLV_WriteParams)) + 1u })
	{
		CLV_WriteParams p = SegParams(0u, 3u);
		p.struct_size = bad;
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(guard.Path(1u).c_str(), &p, &err);
		EXPECT_EQ(w, nullptr) << "自报尺寸 " << bad << " 不该受理";
		EXPECT_EQ(err, CLV_CONTAINER_INVALID_ARGUMENT);
	}
}

TEST(Segments, OpenSegmentsArgumentContract)
{
	const ScopedFiles guard(TempPaths("args", 1u));
	CLV_ContainerError err = CLV_CONTAINER_OK;

	std::vector<const char*> none;
	EXPECT_EQ(CLV_ContainerReaderOpenSegments(nullptr, 1u, 1u, &err), nullptr);
	EXPECT_EQ(err, CLV_CONTAINER_INVALID_ARGUMENT);

	const std::vector<std::string> one { guard.Path(0u) };
	std::vector<const char*> ptrs = [&]
	{
		std::vector<const char*> v;
		for (const std::string& s: one) v.push_back(s.c_str());
		return v;
	}();
	EXPECT_EQ(CLV_ContainerReaderOpenSegments(ptrs.data(), 0u, 1u, &err), nullptr);
	EXPECT_EQ(err, CLV_CONTAINER_INVALID_ARGUMENT);

	std::vector<const char*> with_null { ptrs[0], nullptr };
	EXPECT_EQ(CLV_ContainerReaderOpenSegments(with_null.data(), 2u, 1u, &err), nullptr);
	EXPECT_EQ(err, CLV_CONTAINER_INVALID_ARGUMENT);

	// paths[0] 打不开与 OpenFile 同档
	const std::string missing = (std::filesystem::temp_directory_path().generic_string() + "/clv_segments_absent.clv");
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(missing), ec);
	const char* arr[] = { missing.c_str() };
	EXPECT_EQ(CLV_ContainerReaderOpenSegments(arr, 1u, 1u, &err), nullptr);
	EXPECT_EQ(err, CLV_CONTAINER_IO_FAILED);
}

TEST(Segments, ThreeSegmentsReadAsOneContinuousFrameSequence)
{
	const ScopedFiles guard(TempPaths("three", 3u));
	CLV_ContainerError err = CLV_CONTAINER_OK;
	ASSERT_TRUE(WriteSegment(guard.Path(0u), 0u, 2u, 11u, 0u, 3u, kMaxPacket, &err));
	ASSERT_TRUE(WriteSegment(guard.Path(1u), 2u, 2u, 22u, 1u, 3u, kMaxPacket, &err));
	ASSERT_TRUE(WriteSegment(guard.Path(2u), 4u, 2u, 33u, 2u, 3u, kMaxPacket, &err));

	std::vector<const char*> ptrs = guard.Ptrs();
	CLV_ContainerReader* r = CLV_ContainerReaderOpenSegments(ptrs.data(), 3u, 1u, &err);
	ASSERT_NE(r, nullptr);
	ASSERT_EQ(err, CLV_CONTAINER_OK);

	CLV_ContainerError last = CLV_CONTAINER_OK;
	const std::vector<Seen> seen = Drain(r, &last);
	ASSERT_EQ(seen.size(), 6u) << "三段各两帧，自动续接该交出六帧";
	EXPECT_EQ(last, CLV_CONTAINER_OK);
	for (size_t i = 0; i < seen.size(); ++i)
	{
		const size_t seg = i / 2u;
		EXPECT_EQ(seen[i].stream_id, 0u) << "帧 " << i;
		EXPECT_EQ(seen[i].payload, PayloadOf(static_cast<uint32_t>(i % 2u), static_cast<uint8_t>(11u + seg * 11u)))
			<< "帧 " << i << " 的载荷属于哪一段由清单序定";
	}
	// 段界处首帧的增量按该段自己的起点算：0,1 | 2,1 | 4,1
	const uint64_t want_delta[] = { 0u, 1u, 2u, 1u, 4u, 1u };
	for (size_t i = 0; i < 6u; ++i) EXPECT_EQ(seen[i].dts_delta, want_delta[i]) << "帧 " << i;

	CLV_ReaderSegStatsC s;
	std::memset(&s, 0, sizeof(s));
	ASSERT_EQ(CLV_ContainerReaderSegStats(r, &s), CLV_CONTAINER_OK);
	EXPECT_EQ(s.segments_read, 3u);
	EXPECT_EQ(s.segments_missing, 0u);
	EXPECT_EQ(s.segments_mismatched, 0u);
	EXPECT_EQ(s.segments_reordered, 0u);
	EXPECT_EQ(s.last_seg_index, 2u);
	EXPECT_EQ(s.block_missing, 0u);
	EXPECT_EQ(s.block_bad, 0u);
	EXPECT_EQ(s.discontinuities, 0u);

	uint64_t seg_index = 0, seg_total = 0;
	ASSERT_EQ(CLV_ContainerReaderSegmentIndex(r, &seg_index), CLV_CONTAINER_OK);
	EXPECT_EQ(seg_index, 2u);
	ASSERT_EQ(CLV_ContainerReaderSegmentTotal(r, &seg_total), CLV_CONTAINER_OK);
	EXPECT_EQ(seg_total, 3u);
	CLV_ContainerReaderClose(r);
}

TEST(Segments, AutoAdvanceOffStopsAtTheSegmentEnd)
{
	const ScopedFiles guard(TempPaths("off", 2u));
	CLV_ContainerError err = CLV_CONTAINER_OK;
	ASSERT_TRUE(WriteSegment(guard.Path(0u), 0u, 2u, 11u, 0u, 2u, kMaxPacket, &err));
	ASSERT_TRUE(WriteSegment(guard.Path(1u), 2u, 2u, 22u, 1u, 2u, kMaxPacket, &err));

	std::vector<const char*> ptrs = guard.Ptrs();
	CLV_ContainerReader* r = CLV_ContainerReaderOpenSegments(ptrs.data(), 2u, 0u, &err);
	ASSERT_NE(r, nullptr);

	CLV_ContainerError last = CLV_CONTAINER_OK;
	const std::vector<Seen> seen = Drain(r, &last);
	EXPECT_EQ(seen.size(), 2u) << "关了自动续接就该与单段同形";
	EXPECT_EQ(last, CLV_CONTAINER_OK);

	CLV_ReaderSegStatsC s;
	std::memset(&s, 0, sizeof(s));
	ASSERT_EQ(CLV_ContainerReaderSegStats(r, &s), CLV_CONTAINER_OK);
	EXPECT_EQ(s.segments_read, 1u);
	EXPECT_EQ(s.segments_missing, 0u);
	CLV_ContainerReaderClose(r);
}

TEST(Segments, AbsentSegmentIsSkippedCountedAndDoesNotStopTheRest)
{
	const ScopedFiles guard(TempPaths("absent", 3u));
	CLV_ContainerError err = CLV_CONTAINER_OK;
	ASSERT_TRUE(WriteSegment(guard.Path(0u), 0u, 2u, 11u, 0u, 3u, kMaxPacket, &err));
	ASSERT_TRUE(WriteSegment(guard.Path(2u), 4u, 2u, 33u, 2u, 3u, kMaxPacket, &err));
	const std::string hole = guard.Path(1u);
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(hole), ec);
	ASSERT_FALSE(std::filesystem::exists(std::filesystem::u8path(hole)));

	std::vector<const char*> ptrs = guard.Ptrs();
	CLV_ContainerReader* r = CLV_ContainerReaderOpenSegments(ptrs.data(), 3u, 1u, &err);
	ASSERT_NE(r, nullptr);

	CLV_ContainerError last = CLV_CONTAINER_OK;
	const std::vector<Seen> seen = Drain(r, &last);
	EXPECT_EQ(seen.size(), 4u) << "中间缺一段不该把后段也带走";
	EXPECT_EQ(last, CLV_CONTAINER_OK) << "缺段是声明面的事实，不是错误返回";

	CLV_ReaderSegStatsC s;
	std::memset(&s, 0, sizeof(s));
	ASSERT_EQ(CLV_ContainerReaderSegStats(r, &s), CLV_CONTAINER_OK);
	EXPECT_EQ(s.segments_read, 2u);
	EXPECT_EQ(s.segments_missing, 1u) << "缺一段就跳一段";
	EXPECT_EQ(s.segments_mismatched, 0u) << "跳过的段没走到描述符比对，不该同时计成拒接";
	EXPECT_EQ(s.segments_reordered, 0u);
	CLV_ContainerReaderClose(r);
}

TEST(Segments, DescriptorMismatchRejectsThatSegmentAndIsReported)
{
	const ScopedFiles guard(TempPaths("mismatch", 3u));
	CLV_ContainerError err = CLV_CONTAINER_OK;
	ASSERT_TRUE(WriteSegment(guard.Path(0u), 0u, 2u, 11u, 0u, 3u, kMaxPacket, &err));
	// 第 2 段把该流的 max_packet_size 改一档：描述符逐字段比对不过
	ASSERT_TRUE(WriteSegment(guard.Path(1u), 2u, 2u, 22u, 1u, 3u, kMaxPacket * 2u, &err));
	ASSERT_TRUE(WriteSegment(guard.Path(2u), 4u, 2u, 33u, 2u, 3u, kMaxPacket, &err));

	std::vector<const char*> ptrs = guard.Ptrs();
	CLV_ContainerReader* r = CLV_ContainerReaderOpenSegments(ptrs.data(), 3u, 1u, &err);
	ASSERT_NE(r, nullptr);

	CLV_ContainerError last = CLV_CONTAINER_OK;
	const std::vector<Seen> seen = Drain(r, &last);
	EXPECT_EQ(seen.size(), 4u) << "被拒的那一段不该出帧，其余段照常";
	EXPECT_EQ(last, CLV_CONTAINER_SEGMENT_MISMATCH) << "清单走空的原因是有一段被拒，该有码可查";

	CLV_ReaderSegStatsC s;
	std::memset(&s, 0, sizeof(s));
	ASSERT_EQ(CLV_ContainerReaderSegStats(r, &s), CLV_CONTAINER_OK);
	EXPECT_EQ(s.segments_read, 2u);
	EXPECT_EQ(s.segments_mismatched, 1u) << "描述符对不上的那一段计拒接档";
	EXPECT_EQ(s.segments_reordered, 0u) << "拒接与错位是两格，不该互相污染";
	EXPECT_EQ(s.segments_missing, 0u);
	EXPECT_EQ(s.last_seg_index, 2u);
	CLV_ContainerReaderClose(r);
}

TEST(Segments, SegmentOrderOutOfManifestPlaceIsDeclaredNotCorruption)
{
	const ScopedFiles guard(TempPaths("order", 2u));
	CLV_ContainerError err = CLV_CONTAINER_OK;
	ASSERT_TRUE(WriteSegment(guard.Path(0u), 0u, 2u, 11u, 0u, 2u, kMaxPacket, &err));
	// 清单把这一段放在位置 1，它自己声明的段号是 7：以头里的段号为权威，段照样接
	ASSERT_TRUE(WriteSegment(guard.Path(1u), 2u, 2u, 22u, 7u, 2u, kMaxPacket, &err));

	std::vector<const char*> ptrs = guard.Ptrs();
	CLV_ContainerReader* r = CLV_ContainerReaderOpenSegments(ptrs.data(), 2u, 1u, &err);
	ASSERT_NE(r, nullptr);

	CLV_ContainerError last = CLV_CONTAINER_OK;
	const std::vector<Seen> seen = Drain(r, &last);
	EXPECT_EQ(seen.size(), 4u) << "段号错位不判损坏，帧一帧都不该少";
	EXPECT_EQ(last, CLV_CONTAINER_SEGMENT_ORDER);

	CLV_ReaderSegStatsC s;
	std::memset(&s, 0, sizeof(s));
	ASSERT_EQ(CLV_ContainerReaderSegStats(r, &s), CLV_CONTAINER_OK);
	EXPECT_EQ(s.segments_read, 2u) << "错位不阻断，两段都走过了";
	EXPECT_EQ(s.segments_reordered, 1u) << "段号与清单位置不符计错位档";
	EXPECT_EQ(s.segments_mismatched, 0u) << "这一段的描述符是齐的，不该计进拒接档";
	EXPECT_EQ(s.last_seg_index, 7u);

	uint64_t seg_index = 0;
	ASSERT_EQ(CLV_ContainerReaderSegmentIndex(r, &seg_index), CLV_CONTAINER_OK);
	EXPECT_EQ(seg_index, 7u) << "观测口回的是头里的段号，不是清单下标";
	CLV_ContainerReaderClose(r);
}

TEST(Segments, SingleFileOpenKeepsSegmentObservationsSilent)
{
	const ScopedFiles guard(TempPaths("single", 1u));
	CLV_ContainerError err = CLV_CONTAINER_OK;
	ASSERT_TRUE(WriteSegment(guard.Path(0u), 0u, 2u, 11u, 0u, 1u, kMaxPacket, &err));

	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(guard.Path(0u).c_str(), &err);
	ASSERT_NE(r, nullptr);
	CLV_ContainerError last = CLV_CONTAINER_OK;
	EXPECT_EQ(Drain(r, &last).size(), 2u);
	EXPECT_EQ(last, CLV_CONTAINER_OK);

	CLV_ReaderSegStatsC s;
	std::memset(&s, 0, sizeof(s));
	ASSERT_EQ(CLV_ContainerReaderSegStats(r, &s), CLV_CONTAINER_OK);
	EXPECT_EQ(s.segments_read, 1u) << "单段句柄也算走过一段，但不是清单走过";
	EXPECT_EQ(s.segments_missing, 0u);
	EXPECT_EQ(s.segments_mismatched, 0u);
	EXPECT_EQ(s.segments_reordered, 0u);
	EXPECT_EQ(s.last_seg_index, 0u);

	// 入参判：句柄空 / 出参空
	EXPECT_EQ(CLV_ContainerReaderSegStats(nullptr, &s), CLV_CONTAINER_INVALID_HANDLE);
	EXPECT_EQ(CLV_ContainerReaderSegStats(r, nullptr), CLV_CONTAINER_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_ContainerReaderSegmentIndex(r, nullptr), CLV_CONTAINER_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_ContainerReaderSegmentTotal(nullptr, &s.last_seg_index), CLV_CONTAINER_INVALID_HANDLE);
	CLV_ContainerReaderClose(r);
}

TEST(Segments, StrErrorCoversBothSegmentCodesWithText)
{
	EXPECT_STREQ(CLV_ContainerStrError(CLV_CONTAINER_SEGMENT_MISMATCH), "segment mismatch");
	EXPECT_STREQ(CLV_ContainerStrError(CLV_CONTAINER_SEGMENT_ORDER), "segment order");
	// 值序即 ABI：两个新码排在既有末尾之后，不重排旧号
	EXPECT_EQ(static_cast<int>(CLV_CONTAINER_SEGMENT_MISMATCH), 12);
	EXPECT_EQ(static_cast<int>(CLV_CONTAINER_SEGMENT_ORDER), 13);
}

/* ---------- 分段矩阵：视频流 × 分段 × 参数块 ---------- */

// 每段都挂 ext_type = 3 的视频分段：整条读回，且一致性口报「无不一致」
TEST(Segments, VideoSegmentedWithParamBlockPerSegmentReadsBackWholeRun)
{
	const ScopedFiles guard(TempPaths("segv_meta", 3u));
	CLV_ContainerError err = CLV_CONTAINER_OK;
	for (size_t s = 0; s < 3u; ++s)
	{
		ASSERT_TRUE(WriteVideoSegment(guard.Path(s), s * 4ull, 4u, static_cast<uint8_t>(11u + s * 11u),
									  static_cast<uint64_t>(s), 3u, true, 352u, 0u, &err))
			<< "段 " << s << " 写失败：" << CLV_ContainerStrError(err);
	}
	const std::vector<std::string> names = { guard.Path(0u), guard.Path(1u), guard.Path(2u) };
	std::vector<const char*> holder;
	const std::vector<const char*> ptrs = SegPtrs(names, &holder);

	CLV_ContainerReader* r = CLV_ContainerReaderOpenSegments(ptrs.data(), 3u, 1u, &err);
	ASSERT_NE(r, nullptr) << CLV_ContainerStrError(err);
	CLV_ContainerError last = CLV_CONTAINER_OK;
	const std::vector<Seen> seen = Drain(r, &last);
	EXPECT_EQ(seen.size(), 12u) << "每段自带参数块的视频分段必须整条读回；读成 0 帧就是交叉格没接通";
	EXPECT_EQ(last, CLV_CONTAINER_OK);

	CLV_ReaderSegStatsC ss;
	std::memset(&ss, 0, sizeof(ss));
	ASSERT_EQ(CLV_ContainerReaderSegStats(r, &ss), CLV_CONTAINER_OK);
	EXPECT_EQ(ss.segments_read, 3u);
	EXPECT_EQ(ss.segments_mismatched, 0u);
	EXPECT_EQ(ss.video_stream_no_param_block, 0u);
	EXPECT_EQ(ss.segments_ext_scanned, 0u) << "没查过必须读成 0，不能与「查了且一致」混成一格";
	EXPECT_EQ(ss.segments_ext_inconsistent, 0u);

	CLV_ReaderSegExtCheckC ec;
	std::memset(&ec, 0, sizeof(ec));
	ASSERT_EQ(CLV_ContainerReaderSegExtCheck(r, &ec), CLV_CONTAINER_OK);
	EXPECT_EQ(ec.segments_scanned, 3u);
	EXPECT_EQ(ec.segments_unreadable, 0u);
	EXPECT_EQ(ec.baseline_seg_index, 0u) << "基准 = 清单里第一个读得开的段，与描述符基准同源";
	EXPECT_EQ(ec.blocks_compared, 2u) << "两条对照段 × 一条参数块";
	EXPECT_EQ(ec.blocks_missing, 0u);
	EXPECT_EQ(ec.blocks_differing, 0u);
	EXPECT_EQ(ec.segments_inconsistent, 0u);
	EXPECT_EQ(ec.first_diff_reason, 0u);
	EXPECT_EQ(ec.first_diff_seg_index, ~0ull);

	ASSERT_EQ(CLV_ContainerReaderSegStats(r, &ss), CLV_CONTAINER_OK);
	EXPECT_EQ(ss.segments_ext_scanned, 3u) << "查询口的读数要落到段观测口，不然调用方得自己存";
	EXPECT_EQ(ss.segments_ext_inconsistent, 0u);
	CLV_ContainerReaderClose(r);
}

/* 分段件里首包大于读方重同步窗口（64 KiB）的那一格：包区起点必须把文件级扩展块区一并算进去。
 * 只按逐流链算起点 ⇒ 起点停在段描述块上 ⇒ 第一刀把段描述块当包解 ⇒ 走重同步 ⇒ 首包比窗口宽时找不回来 ⇒
 * 整段一帧不出（实测形态：`abandoned = 1`、`resync_failed = 1`、帧数 0，而段级计数全干净）。
 * 载荷小于窗口时这一格会被重同步救回来，所以本格的判据列是"重同步一次都不该发生"。 */
TEST(Segments, SegmentedPacketsLargerThanResyncWindowReadBackWithoutResync)
{
	const ScopedFiles guard(TempPaths("segv_bigpay", 3u));
	CLV_ContainerError err = CLV_CONTAINER_OK;
	constexpr uint32_t kPay = 200000u;	  // > 64 KiB 的重同步窗口
	for (size_t s = 0; s < 3u; ++s)
	{
		ASSERT_TRUE(WriteVideoSegment(guard.Path(s), s * 2ull, 2u, static_cast<uint8_t>(13u + s * 13u),
									  static_cast<uint64_t>(s), 3u, true, 352u, 0u, &err, kPay))
			<< "段 " << s << " 写失败：" << CLV_ContainerStrError(err);
	}
	const std::vector<std::string> names = { guard.Path(0u), guard.Path(1u), guard.Path(2u) };
	std::vector<const char*> holder;
	const std::vector<const char*> ptrs = SegPtrs(names, &holder);

	CLV_ContainerReader* r = CLV_ContainerReaderOpenSegments(ptrs.data(), 3u, 1u, &err);
	ASSERT_NE(r, nullptr) << CLV_ContainerStrError(err);
	CLV_ContainerError last = CLV_CONTAINER_OK;
	const std::vector<Seen> seen = Drain(r, &last);
	EXPECT_EQ(seen.size(), 6u) << "大包的分段视频必须整条读回，读成 0 帧就是包区起点留了空档";
	EXPECT_EQ(last, CLV_CONTAINER_OK);
	for (const Seen& one: seen) EXPECT_EQ(one.payload.size(), static_cast<size_t>(kPay));

	CLV_ReaderStatsC st;
	std::memset(&st, 0, sizeof(st));
	ASSERT_EQ(CLV_ContainerReaderStats(r, &st), CLV_CONTAINER_OK);
	// Stats 反映**当前段**（句柄轮转只换 reader），跨段累计在 SegStats 那侧 ⇒ 这里按末段的量核。
	EXPECT_EQ(st.packets_ok, 2u);
	EXPECT_EQ(st.frames_ok, 2u);
	EXPECT_EQ(st.resync_count, 0u) << "包区起点对了就不该走重同步——走了一次哪怕找回也算这格没修干净";
	EXPECT_EQ(st.resync_failed, 0u);
	EXPECT_EQ(st.abandoned, 0u);
	EXPECT_EQ(st.truncated, 0u);
	EXPECT_EQ(st.dropped_crc, 0u);
	EXPECT_EQ(st.dropped_field, 0u);
	EXPECT_EQ(st.dropped_too_large, 0u);

	CLV_ReaderSegStatsC ss;
	std::memset(&ss, 0, sizeof(ss));
	ASSERT_EQ(CLV_ContainerReaderSegStats(r, &ss), CLV_CONTAINER_OK);
	EXPECT_EQ(ss.segments_read, 3u);
	EXPECT_EQ(ss.segments_mismatched, 0u);
	EXPECT_EQ(ss.video_stream_no_param_block, 0u);
	CLV_ContainerReaderClose(r);
}

// 段 1 的参数块内容与段 0 矛盾（尺寸 368 vs 352）：段照交、返回码仍是 OK，但「想查能查到」
TEST(Segments, SegmentExtBlockContentMismatchIsCountedNotRejected)
{
	const ScopedFiles guard(TempPaths("segv_diff", 3u));
	CLV_ContainerError err = CLV_CONTAINER_OK;
	ASSERT_TRUE(WriteVideoSegment(guard.Path(0u), 0ull, 4u, 11u, 0u, 3u, true, 352u, 0u, &err));
	ASSERT_TRUE(WriteVideoSegment(guard.Path(1u), 4ull, 4u, 22u, 1u, 3u, true, 368u, 0u, &err));
	ASSERT_TRUE(WriteVideoSegment(guard.Path(2u), 8ull, 4u, 33u, 2u, 3u, true, 352u, 0u, &err));
	const std::vector<std::string> names = { guard.Path(0u), guard.Path(1u), guard.Path(2u) };
	std::vector<const char*> holder;
	const std::vector<const char*> ptrs = SegPtrs(names, &holder);

	CLV_ContainerReader* r = CLV_ContainerReaderOpenSegments(ptrs.data(), 3u, 1u, &err);
	ASSERT_NE(r, nullptr);
	CLV_ContainerError last = CLV_CONTAINER_OK;
	const std::vector<Seen> seen = Drain(r, &last);
	EXPECT_EQ(seen.size(), 12u) << "本版的口径是不拒接：尺寸矛盾不能少交一帧";
	EXPECT_EQ(last, CLV_CONTAINER_OK);

	CLV_ReaderSegStatsC ss;
	std::memset(&ss, 0, sizeof(ss));
	ASSERT_EQ(CLV_ContainerReaderSegStats(r, &ss), CLV_CONTAINER_OK);
	EXPECT_EQ(ss.segments_mismatched, 0u) << "描述符里没有宽与高，尺寸矛盾结构上到不了描述符对账那一档";

	CLV_ReaderSegExtCheckC ec;
	std::memset(&ec, 0, sizeof(ec));
	ASSERT_EQ(CLV_ContainerReaderSegExtCheck(r, &ec), CLV_CONTAINER_OK);
	EXPECT_EQ(ec.segments_scanned, 3u);
	EXPECT_EQ(ec.blocks_compared, 2u);
	EXPECT_EQ(ec.blocks_differing, 1u) << "段 1 与段 0 的参数块逐字节不同";
	EXPECT_EQ(ec.blocks_missing, 0u);
	EXPECT_EQ(ec.segments_inconsistent, 1u);
	EXPECT_EQ(ec.first_diff_seg_index, 1u);
	EXPECT_EQ(ec.first_diff_stream_id, 0u);
	EXPECT_EQ(ec.first_diff_ext_type, 3u);
	EXPECT_EQ(ec.first_diff_reason, 1u) << "1 = 内容不同（2 才是一侧缺块）";

	ASSERT_EQ(CLV_ContainerReaderSegStats(r, &ss), CLV_CONTAINER_OK);
	EXPECT_EQ(ss.segments_ext_scanned, 3u);
	EXPECT_EQ(ss.segments_ext_inconsistent, 1u);
	CLV_ContainerReaderClose(r);
}

// 后续段不挂参数块：视频流缺 ext_type = 3 参数块即被读方摘掉 ⇒ 段身份对账整段拒接（码 12），一致性口报「一侧缺块」
TEST(Segments, SegmentExtBlockMissingLaterSegmentsRejectedByDescriptor)
{
	const ScopedFiles guard(TempPaths("segv_missing", 3u));
	CLV_ContainerError err = CLV_CONTAINER_OK;
	ASSERT_TRUE(WriteVideoSegment(guard.Path(0u), 0ull, 4u, 11u, 0u, 3u, true, 352u, 0u, &err));
	ASSERT_TRUE(WriteVideoSegment(guard.Path(1u), 4ull, 4u, 22u, 1u, 3u, false, 352u, 0u, &err));
	ASSERT_TRUE(WriteVideoSegment(guard.Path(2u), 8ull, 4u, 33u, 2u, 3u, false, 352u, 0u, &err));
	const std::vector<std::string> names = { guard.Path(0u), guard.Path(1u), guard.Path(2u) };
	std::vector<const char*> holder;
	const std::vector<const char*> ptrs = SegPtrs(names, &holder);

	CLV_ContainerReader* r = CLV_ContainerReaderOpenSegments(ptrs.data(), 3u, 1u, &err);
	ASSERT_NE(r, nullptr);
	CLV_ContainerError last = CLV_CONTAINER_OK;
	const std::vector<Seen> seen = Drain(r, &last);
	EXPECT_EQ(seen.size(), 4u) << "缺参数块的段整段不接：只交得出首段那 4 帧";
	EXPECT_EQ(last, CLV_CONTAINER_SEGMENT_MISMATCH);

	CLV_ReaderSegStatsC ss;
	std::memset(&ss, 0, sizeof(ss));
	ASSERT_EQ(CLV_ContainerReaderSegStats(r, &ss), CLV_CONTAINER_OK);
	EXPECT_EQ(ss.segments_mismatched, 2u) << "拒接的那两段计在 mismatched 档，不是 reordered";
	EXPECT_EQ(ss.video_stream_no_param_block, 2u) << "拒接要能归因：这两段各缺一条视频参数块，不是只报「拒了」";

	CLV_ReaderSegExtCheckC ec;
	std::memset(&ec, 0, sizeof(ec));
	ASSERT_EQ(CLV_ContainerReaderSegExtCheck(r, &ec), CLV_CONTAINER_OK);
	EXPECT_EQ(ec.segments_scanned, 3u) << "查询口读的是盘上的段，不受游标被拒接影响";
	EXPECT_EQ(ec.blocks_compared, 0u);
	EXPECT_EQ(ec.blocks_missing, 2u) << "基准段有参数块、对照段没有 ⇒ 记缺块，不记内容不同";
	EXPECT_EQ(ec.segments_inconsistent, 2u);
	EXPECT_EQ(ec.first_diff_seg_index, 1u);
	EXPECT_EQ(ec.first_diff_ext_type, 3u);
	EXPECT_EQ(ec.first_diff_reason, 2u);
	CLV_ContainerReaderClose(r);
}

// 参数契约 + 单段句柄（清单为空时基准就是自己，scanned = 1 表示「查过了、没得比」）
TEST(Segments, SegExtCheckArgumentContractAndSingleFileHandle)
{
	CLV_ReaderSegExtCheckC ec;
	std::memset(&ec, 0, sizeof(ec));
	CLV_ContainerError err = CLV_CONTAINER_OK;
	EXPECT_EQ(CLV_ContainerReaderSegExtCheck(nullptr, &ec), CLV_CONTAINER_INVALID_HANDLE);

	const ScopedFiles guard(TempPaths("segv_single", 1u));
	ASSERT_TRUE(WriteVideoSegment(guard.Path(0u), 0ull, 4u, 11u, 0u, 3u, true, 352u, 0u, &err));
	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(guard.Path(0u).c_str(), &err);
	ASSERT_NE(r, nullptr);
	EXPECT_EQ(CLV_ContainerReaderSegExtCheck(r, nullptr), CLV_CONTAINER_INVALID_ARGUMENT);
	ASSERT_EQ(CLV_ContainerReaderSegExtCheck(r, &ec), CLV_CONTAINER_OK);
	EXPECT_EQ(ec.segments_scanned, 1u) << "单段句柄也要留下「查过」的痕迹，否则 0 与「没查」分不开";
	EXPECT_EQ(ec.blocks_compared, 0u);
	EXPECT_EQ(ec.segments_inconsistent, 0u);
	EXPECT_EQ(ec.first_diff_seg_index, ~0ull);
	CLV_ContainerReaderClose(r);
}

// 双流分段（视频 + 字幕，各带自己的块）与音频分段两格：分段矩阵里流数与流类型这两条轴
TEST(Segments, DualStreamAndAudioSegmentedMatrixCellsClose)
{
	CLV_ContainerError err = CLV_CONTAINER_OK;
	{
		const ScopedFiles guard(TempPaths("segv_dual", 3u));
		for (size_t s = 0; s < 3u; ++s)
		{
			ASSERT_TRUE(WriteVideoSegment(guard.Path(s), s * 4ull, 4u, static_cast<uint8_t>(11u + s * 11u),
										  static_cast<uint64_t>(s), 3u, true, 352u, 1u, &err))
				<< "段 " << s << " 双流写失败：" << CLV_ContainerStrError(err);
		}
		const std::vector<std::string> names = { guard.Path(0u), guard.Path(1u), guard.Path(2u) };
		std::vector<const char*> holder;
		const std::vector<const char*> ptrs = SegPtrs(names, &holder);
		CLV_ContainerReader* r = CLV_ContainerReaderOpenSegments(ptrs.data(), 3u, 1u, &err);
		ASSERT_NE(r, nullptr);
		CLV_ContainerError last = CLV_CONTAINER_OK;
		EXPECT_EQ(Drain(r, &last).size(), 12u) << "双流分段的帧数闭合按帧计，不按流计";
		EXPECT_EQ(last, CLV_CONTAINER_OK);
		uint32_t streams = 0;
		ASSERT_EQ(CLV_ContainerReaderStreamCount(r, &streams), CLV_CONTAINER_OK);
		EXPECT_EQ(streams, 2u);
		CLV_ReaderSegExtCheckC ec;
		std::memset(&ec, 0, sizeof(ec));
		ASSERT_EQ(CLV_ContainerReaderSegExtCheck(r, &ec), CLV_CONTAINER_OK);
		EXPECT_EQ(ec.blocks_compared, 4u) << "两条块（参数块 + 样式块）× 两条对照段";
		EXPECT_EQ(ec.segments_inconsistent, 0u);
		CLV_ContainerReaderClose(r);
	}
	{
		const ScopedFiles guard(TempPaths("sega_seg", 2u));
		for (size_t s = 0; s < 2u; ++s)
		{
			CLV_WriteParams p = SegParams(static_cast<uint64_t>(s), 2u);
			p.seg_start_ticks = s * 4ull;
			p.seg_end_ticks = s * 4ull + 3ull;
			CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(guard.Path(s).c_str(), &p, &err);
			ASSERT_NE(w, nullptr);
			CLV_StreamDescC d;
			std::memset(&d, 0, sizeof(d));
			d.stream_id = 0;
			d.stream_type = static_cast<uint8_t>(StreamType::kAudio);
			d.codec_version = 1;
			d.max_packet_size = kMaxPacket;
			d.sample_rate = 48000u;
			d.channels = 1;
			d.bit_depth = 16;
			d.timebase_num = 1;
			d.timebase_den = 1;
			ASSERT_EQ(CLV_ContainerWriterAddStream(w, &d), CLV_CONTAINER_OK);
			for (uint32_t i = 0; i < 4u; ++i)
			{
				const std::vector<uint8_t> pl = PayloadOf(i, static_cast<uint8_t>(7u + s));
				ASSERT_EQ(CLV_ContainerWriterWriteFrame(w, 0u, i == 0u ? 1u : 0u, static_cast<int64_t>(s * 4ull + i),
														static_cast<int64_t>(s * 4ull + i), pl.data(), pl.size()),
						  CLV_CONTAINER_OK);
			}
			uint64_t packets = 0;
			ASSERT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
			CLV_ContainerWriterClose(w);
		}
		const std::vector<std::string> names = { guard.Path(0u), guard.Path(1u) };
		std::vector<const char*> holder;
		const std::vector<const char*> ptrs = SegPtrs(names, &holder);
		CLV_ContainerReader* r = CLV_ContainerReaderOpenSegments(ptrs.data(), 2u, 1u, &err);
		ASSERT_NE(r, nullptr);
		CLV_ContainerError last = CLV_CONTAINER_OK;
		EXPECT_EQ(Drain(r, &last).size(), 8u) << "音频分段格：音频流不受必选块约束，帧数必须闭合";
		EXPECT_EQ(last, CLV_CONTAINER_OK);
		CLV_ReaderSegExtCheckC ec;
		std::memset(&ec, 0, sizeof(ec));
		ASSERT_EQ(CLV_ContainerReaderSegExtCheck(r, &ec), CLV_CONTAINER_OK);
		EXPECT_EQ(ec.segments_scanned, 2u);
		EXPECT_EQ(ec.blocks_compared, 0u) << "两条段都没有扩展块：比过 0 条，但不等于比对失败";
		EXPECT_EQ(ec.segments_inconsistent, 0u);
		CLV_ContainerReaderClose(r);
	}
}
