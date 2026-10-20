/* test_ind_container.cpp
 *
 * 期望来源限定为公开头 `include/CLV_Container.h` 的契约文字、实现层声明域常量
 * （`src/container/structure.h`、`src/container/reader.h`、`src/core/varint.h`）与本件注释里写明的算式；
 * 不从被测读写方的返回值现取期望。Suite 名以 `Independent` 起头，供按名前缀单独过滤。
 *
 * 布局不变量（包区起点与各块偏移的算式都由它推得）：
 *   文件 = 文件头 64 | 流描述符 68×N | 扩展块区 | 包区 | 索引头 16 + 条目 20×M | 文件尾 16
 *   （`src/container/writer.cpp` 的 `BeginPackets` 布局注释；各项字节长见 `src/container/structure.h`）
 *   扩展块线上形 = 前缀 18（magic 4 | type 1 | version 1 | ext_len 4 | next_ext_offset 8）
 *   + 载荷 ext_len + crc32 4（`src/container/structure.h` 的 kExtBlockFixedPrefix / kExtBlockCrcSize）
 *   包线上形 = total_size LE32 + body，body = stream_id | flags | [fragment_index] | dts_delta |
 *   [pts_delta] | payload_size | ext_len_varint | payload | ext | crc32，
 *   crc32 覆盖 body 去掉自身那 4 字节（`src/container/packet.h` 件头）
 *
 * 判据口径：界下档与界上档都断唯一结论，结论取自实现层判式的比较方向（判式写的是 `<=` 还是 `>`，
 * 界本身就落在对应那一侧），一格只给一条判据；越界档断「拒」。
 * 崩溃、挂起、或不报错的静默错位（读回结果与上面的布局算式不符）一律停手上报，不放宽断言。
 */

#include "CLV_Container.h"
#include "container/io.h"
#include "container/packet.h"
#include "container/reader.h"
#include "container/structure.h"
#include "container/writer.h"
#include "core/crc32.h"
#include "core/varint.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace
{

	using clv::container::ChunkFrameInput;
	using clv::container::ChunkInput;
	using clv::container::ContainerErr;
	using clv::container::ContainerReader;
	using clv::container::ContainerWriter;
	using clv::container::DecodePacket;
	using clv::container::ExtRecord;
	using clv::container::FrameInput;
	using clv::container::IndexEntry;
	using clv::container::MemorySink;
	using clv::container::MemorySource;
	using clv::container::PacketParse;
	using clv::container::ParsedPacket;
	using clv::container::ReaderStats;
	using clv::container::ReassembledFrame;
	using clv::container::StreamDesc;
	using clv::container::StreamType;
	using clv::container::WriterConfig;
	using clv::container::WriteSummary;

	constexpr uint8_t kVideoType = static_cast<uint8_t>(StreamType::kVideo);
	constexpr uint8_t kExtType = 4u;
	constexpr uint8_t kExtVersion = 1u;

	/* 读方每条扩展块链的跳数上限 4096：读自 `src/container/reader.cpp` 匿名命名空间里的
	 * kMaxExtChainHops，该常量不经头暴露，本件只能按值复述并把出处钉在符号名上。 */
	constexpr uint32_t kExtChainHopCap = 4096u;

	/* 包外层最坏开销直接取头里那一个定义（本件已 include `container/reader.h`），不复述数值；
	 * 读方的接受上限 = 该流 max_packet_size + 它（reader.cpp 的 OuterLimitFor）。 */
	using clv::container::kOuterOverhead;

	/* ext_len 线上是 4 字节小端（`src/container/structure.h` 的 kExtLenOffset 起、四字节宽），
	 * 故单块载荷的可表达上界 = 该字段全 1 = 0xFFFFFFFF；写方按同一上界在落盘前拒
	 * （writer.cpp 的 AddExtBlock，上界判排在 data 判之前，故越界档不必真备载荷）。
	 * 上界那一档要 4 GiB 实载荷，用例不构造 —— 位宽档只测上界外侧与两字节全 1 那一格的对照，见下面那段。 */
	constexpr uint64_t kExtDataMax = 0xFFFFFFFFull;

	// 定长尺寸档：这三个数是容器字节形的承重点，写死在此，实现层漂了即编译失败
	static_assert(clv::container::kStreamDescSize == 68u, "流描述符定长必须 68 字节");
	static_assert(clv::container::kIndexEntrySize == 20u, "索引条目定长必须 20 字节");
	static_assert(clv::container::kExtBlockFixedPrefix == 18u, "扩展块定长前缀必须 18 字节");

	// 链长档位用的单块载荷字节数：取小值，让各档之间只差块数
	constexpr size_t kBlockDataLen = 16u;

	// 手工拼包用的流 payload 上限，与 VideoStream 的 max_packet_size 同值
	constexpr uint32_t kMaxPacketForTest = 1024u;

	constexpr uint8_t kSeedChain0 = 11u;
	constexpr uint8_t kSeedChain1 = 41u;

	// flags bit3 has_pts 恒 1、未分片（src/container/packet.h 的位定义）
	constexpr uint8_t kFlagsFrameFirst = 0x08u;

	// 出参「没被写过」的记号
	constexpr uint64_t kSentinel = 0x5EED5EED5EED5EEDull;

	// 流描述符表之后就是扩展区：文件头 64 + 每条流 68（两件长都取自 structure.h 的具名常量）
	constexpr uint64_t ExtAreaStart(size_t streams)
	{
		return static_cast<uint64_t>(clv::container::kFileHeadSize) +
			   static_cast<uint64_t>(streams) * clv::container::kStreamDescSize;
	}

	// 单块线上长 = 前缀 18 + 载荷 + crc32 4
	constexpr size_t ExtWire(size_t data_len)
	{
		return clv::container::kExtBlockFixedPrefix + data_len + clv::container::kExtBlockCrcSize;
	}

	/* 每条视频流必挂 ext_type = 3 参数块（`docs/容器规范.md` 的扩展块一节，v2 起对视频流必选）：缺块的流被读方记
	 * video_stream_no_param_block 并判该流不可用（src/container/reader.cpp），本件的链长档、分片位宽档与
	 * 流数上界档就全都验不下去。载荷 8 字节，字段布局照 src/video/headers.cpp 的 ParseEncMeta：
	 *   [0] max_b_frames  [1] quant_precision  [2] ext_tools  [3] reserved  [4..5] width LE16  [6..7] height LE16
	 * 本件的用例只验容器面，没有真画面对应，尺寸取一个合法非零值（kDisplayW x kDisplayH）。
	 * 参数块一律登记在各链链首：停链档也得先读到它。 */
	constexpr uint8_t kExtTypeParams = 3u;
	static_assert(kExtTypeParams == clv::container::kExtTypeEncMeta, "视频参数块的 ext_type 与实现层常量不符");
	constexpr uint16_t kDisplayW = 352u;
	constexpr uint16_t kDisplayH = 288u;
	constexpr size_t kParamDataLen = 8u;
	constexpr size_t kParamWireLen = ExtWire(kParamDataLen);	// 18 + 8 + 4 = 30 B

	std::vector<uint8_t> VideoParamsBytes(uint16_t width, uint16_t height)
	{
		return std::vector<uint8_t> { 0u,
									  7u,	 // quant_precision
									  0u,	 // ext_tools
									  0u,	 // reserved
									  static_cast<uint8_t>(width & 0xFFu),
									  static_cast<uint8_t>((width >> 8) & 0xFFu),
									  static_cast<uint8_t>(height & 0xFFu),
									  static_cast<uint8_t>((height >> 8) & 0xFFu) };
	}

	// 实现层入口：把参数块登记成某条流链上的首块
	ContainerErr AddVideoParams(ContainerWriter& w, uint8_t stream_id, uint16_t width, uint16_t height)
	{
		const std::vector<uint8_t> meta = VideoParamsBytes(width, height);
		return w.AddExtBlock(stream_id, kExtTypeParams, kExtVersion, meta.data(), meta.size());
	}

	// 公开面入口：同样八字节走 CLV_ContainerWriterAddExtBlock
	CLV_ContainerError ApiAddVideoParams(CLV_ContainerWriter* w, uint8_t stream_id, uint16_t width, uint16_t height)
	{
		const std::vector<uint8_t> meta = VideoParamsBytes(width, height);
		return CLV_ContainerWriterAddExtBlock(w, stream_id, kExtTypeParams, kExtVersion, meta.data(), meta.size());
	}

	/* 包区起点与各块偏移的算式：每条链首挂一个 30 B 的参数块（kParamWireLen），其余块仍是
	 * ExtWire(kBlockDataLen)，故按下面三个式子走；n0 / n1 是含参数块的**块数**。 */

	// 某条链内第 i 块（i 从 0 数，0 即链首参数块）的线上偏移
	constexpr uint64_t ChainBlockAt(uint64_t chain_start, size_t i)
	{
		return i == 0u ? chain_start : chain_start + kParamWireLen + (i - 1u) * ExtWire(kBlockDataLen);
	}

	// 流 0 的链起点 = 扩展区起点；流 1 的链起点 = 流 0 那 n0 块排完
	constexpr uint64_t Chain0Start(size_t streams) { return ExtAreaStart(streams); }

	constexpr uint64_t Chain1Start(size_t streams, size_t n0)
	{
		return Chain0Start(streams) + kParamWireLen + (n0 - 1u) * ExtWire(kBlockDataLen);
	}

	// 两条链（各带一个参数块）都排完 = 包区起点
	constexpr uint64_t TwoChainPacketStart(size_t streams, size_t n0, size_t n1)
	{
		return ExtAreaStart(streams) + 2u * kParamWireLen + (n0 + n1 - 2u) * ExtWire(kBlockDataLen);
	}

	StreamDesc VideoStream(uint8_t id, uint32_t max_packet)
	{
		StreamDesc d;
		d.stream_id = id;
		d.stream_type = kVideoType;
		d.codec_id = 0;
		d.codec_version = 1;
		// 时间基 1/90000：两项都非零且落进 u32，过 DescValuesOk 的时间基判
		d.timebase_num = 1;
		d.timebase_den = 90000;
		d.max_packet_size = max_packet;
		d.first_dts = 0;
		d.bit_depth = 8;	// 视频位深允许集 {8, 10, 12}
		return d;
	}

	/* 扩展块载荷：块序与种子进算式，同一条链上每块内容互不相同，
	 * 于是「读回第 j 块」的期望只按链位算得出。 */
	std::vector<uint8_t> ExtData(size_t block_index, size_t len, uint8_t seed)
	{
		std::vector<uint8_t> v(len);
		for (size_t i = 0; i < len; ++i) v[i] = static_cast<uint8_t>((block_index * 7u + i * 11u + seed) & 0xFFu);
		return v;
	}

	std::vector<uint8_t> FrameData(size_t n, uint8_t seed)
	{
		std::vector<uint8_t> v(n);
		for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 29u + seed) & 0xFFu);
		return v;
	}

	struct Built
	{
		std::vector<uint8_t> bytes;
		WriteSummary sum;
	};

	/* 两条流、两条链：流 0 的链 n0 块、流 1 的链 n1 块（**含链首那个 30 B 的参数块**，其余块载荷等长）；
	 * 之后两条流各一帧。两条流都是视频流，故各挂一个 ext_type = 3（v2 起对视频流必选）。
	 * 流 1 的组排在扩展区最后一段，故「扩展区末尾 = 包区起点」这条算式与流 0 是否停链无关。 */
	Built BuildTwoChains(size_t n0, size_t n1)
	{
		Built out;
		MemorySink sink;
		ContainerWriter w(sink, WriterConfig {});
		EXPECT_EQ(w.AddStream(VideoStream(0, 4096)), ContainerErr::kOk);
		EXPECT_EQ(w.AddStream(VideoStream(1, 4096)), ContainerErr::kOk);
		EXPECT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);
		EXPECT_EQ(AddVideoParams(w, 1, kDisplayW, kDisplayH), ContainerErr::kOk);

		for (size_t i = 1; i < n0; ++i)
		{
			const std::vector<uint8_t> d = ExtData(i - 1u, kBlockDataLen, kSeedChain0);
			EXPECT_EQ(w.AddExtBlock(0, kExtType, kExtVersion, d.data(), d.size()), ContainerErr::kOk)
				<< "流 0 第 " << i << " 块写入";
		}
		for (size_t i = 1; i < n1; ++i)
		{
			const std::vector<uint8_t> d = ExtData(i - 1u, kBlockDataLen, kSeedChain1);
			EXPECT_EQ(w.AddExtBlock(1, kExtType, kExtVersion, d.data(), d.size()), ContainerErr::kOk)
				<< "流 1 第 " << i << " 块写入";
		}

		const std::vector<uint8_t> f0 = FrameData(300, 1);
		const std::vector<uint8_t> f1 = FrameData(120, 2);
		FrameInput a;
		a.stream_id = 0;
		a.is_keyframe = true;
		a.dts = 0;
		a.pts = 4;
		a.payload = f0.data();
		a.payload_size = f0.size();
		EXPECT_EQ(w.WriteFrame(a), ContainerErr::kOk);

		FrameInput b;
		b.stream_id = 1;
		b.is_keyframe = false;
		b.dts = 0;
		b.pts = 0;
		b.payload = f1.data();
		b.payload_size = f1.size();
		EXPECT_EQ(w.WriteFrame(b), ContainerErr::kOk);

		EXPECT_EQ(w.Finish(&out.sum), ContainerErr::kOk);
		out.bytes = sink.Bytes();
		return out;
	}

	size_t CountChain(const std::vector<ExtRecord>& rec, uint8_t stream_id)
	{
		size_t n = 0;
		for (const ExtRecord& r: rec)
			if (r.stream_id == stream_id) ++n;
		return n;
	}

	std::vector<ReassembledFrame> TakeAll(ContainerReader& r)
	{
		std::vector<ReassembledFrame> out;
		for (;;)
		{
			ReassembledFrame f;
			if (! r.NextFrame(&f)) break;
			out.push_back(std::move(f));
		}
		return out;
	}

	// 停链范围：流 1 的整条链必须在视图里，且偏移按「流 0 的全部块都已排布」的算式落位
	void CheckTailChainIntact(const ContainerReader& r, size_t n0, size_t n1)
	{
		const std::vector<ExtRecord>& rec = r.ExtBlocks();
		const size_t got0 = CountChain(rec, 0);
		ASSERT_EQ(CountChain(rec, 1), n1);
		// 链序是登记序：流 0 的组在前，流 1 的组紧随其后，故下标 got0 + i 可用
		ASSERT_EQ(rec.size(), got0 + n1);
		const uint64_t group1 = Chain1Start(2u, n0);
		for (size_t i = 0; i < n1; ++i)
		{
			const ExtRecord& b = rec[got0 + i];
			EXPECT_EQ(b.offset, ChainBlockAt(group1, i)) << "流 1 第 " << i << " 块偏移";
			// i == 0 是链首的视频参数块（v2 起对视频流必选），其余是 kExtType 的定长块
			EXPECT_EQ(b.type, i == 0u ? kExtTypeParams : kExtType) << "流 1 第 " << i << " 块类型";
			EXPECT_EQ(b.version, kExtVersion) << "流 1 第 " << i << " 块版本";
			const std::vector<uint8_t> want =
				(i == 0u) ? VideoParamsBytes(kDisplayW, kDisplayH) : ExtData(i - 1u, kBlockDataLen, kSeedChain1);
			EXPECT_EQ(b.data, want) << "流 1 第 " << i << " 块载荷";
		}
	}

	/* 包区没被扩展区污染的三条证据：首包偏移 = 布局算式、两帧逐字节读回、一次重同步都没走。
	 * 静默把扩展区当包区解析必然在这三条里留下一条。
	 * 取数次序由实现的更新点决定：`ContainerReader::Open()` 只解析头、描述符、扩展链、索引与文件尾，
	 * 不扫包区；`packets_ok` 只在 `ContainerReader::NextFrame()` 解包成功处递增，
	 * `frames_ok` / `fragment_gaps` 经 `FeedFrames()`→`Absorb()` 在 NextFrame 里累加，
	 * `resync_*` 在 `ContainerReader::Resync()`，`dropped_*` / `truncated` / `abandoned`
	 * 也全在 NextFrame 的包区扫描分支里。故先用 NextFrame 把包区读完，再采统计；
	 * 索引与文件尾属 Open 即就绪的读回视图（`reader.h` 的 `HasIndex()` / `IndexEntries()` /
	 * `HasFileTail()` / `TotalPackets()` 一组访问口），先于驱动核对。 */
	void CheckPacketAreaClean(ContainerReader& r, uint64_t want_start)
	{
		ASSERT_TRUE(r.HasIndex());
		const std::vector<IndexEntry>& idx = r.IndexEntries();
		ASSERT_EQ(idx.size(), 2u);
		EXPECT_EQ(static_cast<uint64_t>(idx[0].file_offset), want_start) << "首包偏移与布局算式不符";

		// 文件尾 = 末 16 字节仍读得回，包数的第二出处与写方计数一致：扩展区没挤动包区与尾
		EXPECT_TRUE(r.HasFileTail());
		EXPECT_EQ(r.TotalPackets(), 2u);
		EXPECT_EQ(r.TailTotalPackets(), 2u);

		const std::vector<ReassembledFrame> frames = TakeAll(r);
		ASSERT_EQ(frames.size(), 2u);
		EXPECT_EQ(frames[0].payload, FrameData(300, 1));
		EXPECT_EQ(frames[1].payload, FrameData(120, 2));

		const ReaderStats& st = r.Stats();
		EXPECT_EQ(st.packets_ok, 2u);
		EXPECT_EQ(st.frames_ok, 2u);
		EXPECT_EQ(st.resync_count, 0u);
		EXPECT_EQ(st.resync_bytes_scanned, 0u);
		EXPECT_EQ(st.resync_failed, 0u);
		EXPECT_EQ(st.dropped_crc, 0u);
		EXPECT_EQ(st.dropped_field, 0u);
		EXPECT_EQ(st.dropped_too_large, 0u);
		EXPECT_EQ(st.dropped_unknown_stream, 0u);
		EXPECT_EQ(st.fragment_gaps, 0u);
		EXPECT_FALSE(st.abandoned);
		EXPECT_FALSE(st.truncated);
	}

	/* 手工拼一个包：body = 字段区 + 载荷，线上再加 4 字节 total_size 前缀与 4 字节 crc32 收尾。
	 * crc32 用 core::Crc32 现算，它是本档的输入构造件、不是被判的 varint 面。 */
	std::vector<uint8_t> RawPacket(const std::vector<uint8_t>& fields, const std::vector<uint8_t>& payload)
	{
		std::vector<uint8_t> body = fields;
		body.insert(body.end(), payload.begin(), payload.end());
		const size_t covered = body.size();
		body.resize(covered + 4u);
		const uint32_t crc = clv::core::Crc32(body.data(), covered);
		body[covered + 0u] = static_cast<uint8_t>(crc & 0xFFu);
		body[covered + 1u] = static_cast<uint8_t>((crc >> 8) & 0xFFu);
		body[covered + 2u] = static_cast<uint8_t>((crc >> 16) & 0xFFu);
		body[covered + 3u] = static_cast<uint8_t>((crc >> 24) & 0xFFu);

		const uint32_t total_size = static_cast<uint32_t>(body.size());
		std::vector<uint8_t> out = { static_cast<uint8_t>(total_size & 0xFFu),
									 static_cast<uint8_t>((total_size >> 8) & 0xFFu),
									 static_cast<uint8_t>((total_size >> 16) & 0xFFu),
									 static_cast<uint8_t>((total_size >> 24) & 0xFFu) };
		out.insert(out.end(), body.begin(), body.end());
		return out;
	}

	// 无符号 LEB128 读数 = Σ (字节 & 0x7F) << 7i，i 为字节序（src/core/varint.cpp 的 DecodeVarint 定义）
	uint64_t SumVarintValue(const std::vector<uint8_t>& bytes)
	{
		uint64_t v = 0;
		for (size_t i = 0; i < bytes.size(); ++i) v |= static_cast<uint64_t>(bytes[i] & 0x7Fu) << (7u * i);
		return v;
	}

	// ---- 公开面（流数档）用的临时文件与描述符 ----

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

	CLV_StreamDescC ApiVideoDesc(uint8_t id)
	{
		CLV_StreamDescC d;
		std::memset(&d, 0, sizeof(d));
		d.stream_id = id;
		d.stream_type = 0;	  // 0 = 视频（include/CLV_Container.h 的 CLV_StreamDescC 注释）
		d.codec_id = 0;
		d.codec_version = 1;
		d.timebase_num = 1;
		d.timebase_den = 90000;
		d.max_packet_size = 512;
		d.bit_depth = 8;
		return d;
	}

}	 // namespace

TEST(IndependentContainer, ExtChainOverHopCapIsRejectedWithCode)
{
	// 流 1 的链恒短于跳数上限，且它的组排在扩展区最后一段
	constexpr size_t kTailChain = 100u;
	const size_t chains[] = { 1u, 10u, 100u, 1000u, kExtChainHopCap - 1u, kExtChainHopCap, kExtChainHopCap + 1u };

	for (const size_t chain0: chains)
	{
		SCOPED_TRACE("流 0 链长 = " + std::to_string(chain0));
		const uint64_t want_start = TwoChainPacketStart(2u, chain0, kTailChain);
		const Built b = BuildTwoChains(chain0, kTailChain);

		// 写方的落位先与算式对上，之后才允许拿算式当读方的期望
		ASSERT_EQ(b.sum.packet_area_start, want_start);
		ASSERT_EQ(b.sum.packets, 2u);
		ASSERT_EQ(b.sum.frames, 2u);
		ASSERT_EQ(b.sum.index_entries, 2u);

		MemorySource src(b.bytes);
		ContainerReader r(src);

		if (chain0 <= kExtChainHopCap)
		{
			// 界下档与界上档（链长恰好等于上限也算走完）：整条链必须逐个读回。
			// 上限语义 = 「走满 cap 跳仍能到头」，只有第 cap + 1 跳仍不到头才进拒档，
			// 与 src/container/reader.cpp 的 kMaxExtChainHops 注释同源。
			ASSERT_EQ(r.Open(), ContainerErr::kOk);

			const std::vector<ExtRecord>& rec = r.ExtBlocks();
			const size_t got0 = CountChain(rec, 0);
			EXPECT_EQ(got0, chain0);
			EXPECT_EQ(rec.size(), chain0 + kTailChain);
			for (size_t i = 0; i < got0; ++i)
			{
				EXPECT_EQ(rec[i].offset, ChainBlockAt(Chain0Start(2u), i)) << "流 0 第 " << i << " 块偏移";
				// i == 0 是链首的视频参数块，其余是 kExtType 的定长块
				const std::vector<uint8_t> want =
					(i == 0u) ? VideoParamsBytes(kDisplayW, kDisplayH) : ExtData(i - 1u, kBlockDataLen, kSeedChain0);
				EXPECT_EQ(rec[i].data, want) << "流 0 第 " << i << " 块载荷";
				EXPECT_EQ(rec[i].type, i == 0u ? kExtTypeParams : kExtType) << "流 0 第 " << i << " 块类型";
			}
			RecordProperty("chain_cap_path", "accepted");

			// 两档共同：另一条完整链照常读回，包区起点按布局算式，扩展区没被当包区解析
			CheckTailChainIntact(r, chain0, kTailChain);
			EXPECT_EQ(r.Stats().ext_blocks_bad, 0u);
			CheckPacketAreaClean(r, want_start);
		}
		else
		{
			// 越界档（cap + 1）：逐块成立、链路一直向前，却在跳数上限处仍没到头 ⇒ 整次打开拒，
			// 指定码 kExtChainTooLong；对外的落点是公开码 CLV_CONTAINER_EXT_CHAIN_TOO_LONG
			// （门面 ToApi 显式映射，位置有 static_assert 钉）。
			// 打开失败后 ExtBlocks() 里已收的块不作为契约内容，故本档只断「不再返回 kOk」与码值本身。
			RecordProperty("chain_cap_path", "rejected");
			EXPECT_EQ(r.Open(), ContainerErr::kExtChainTooLong) << "越界链必须给指定码，不许静默截断";
		}
	}

	/* ext_len 位宽档。字段是 4 字节 ⇒ 可表达上界 0xFFFFFFFF，那一档要 4 GiB 实载荷、本件不构造，
	 * 所以界上档不可测，改测「四字节域内的三格」与「上界外侧一格」两组：
	 *   65 535 = 两字节全 1 那一格，落在四字节域内 ⇒ 受理
	 *   65 536 = 紧邻它上一格，同样在域内 ⇒ 受理；这一格才真能证明位宽是 4 字节 ——
	 *            写入路径若按两字节截，声明值就落不进域内，这一格会被拒
	 *   432 411 = 一张仓外 FLAC 一致性件的封面块真尺寸：PICTURE 载荷 432 410 B 加承载块首字节的类型码 1 B，
	 *             比两字节全 1 那一格大 432 411 ÷ 65 535 ≈ 6.6 倍。该参考件不在仓内盘上，此处不可复核；
	 *             仓外素材的来源与整件哈希以 `assets/fixtures/provenance.txt` 为权威登记件。
	 *             本件把它当「域内的大载荷」样例用，期望值只由上面的算式与常量给出
	 *   上界外 0x1_F000_0000 不适用，取 2^32 一档：必须拒，码 kTooLarge，且不必真备载荷 */
	{
		constexpr size_t kTwoByteAllOnes = 65535u;	  // 两字节全 1 那一格
		constexpr size_t kRealCover = 432411u;		  // 封面块载荷 432 410 B + 1 B 类型码
		const size_t lens[] = { kTwoByteAllOnes, kTwoByteAllOnes + 1u, kRealCover };
		std::vector<size_t> accepted_lens;
		std::vector<std::vector<uint8_t>> accepted_data;

		MemorySink sink;
		ContainerWriter w(sink, WriterConfig {});
		ASSERT_EQ(w.AddStream(VideoStream(0, 4096)), ContainerErr::kOk);
		ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);

		for (size_t i = 0; i < 3u; ++i)
		{
			SCOPED_TRACE("扩展块载荷 " + std::to_string(lens[i]) + " B");
			const std::vector<uint8_t> d = ExtData(i, lens[i], kSeedChain0);
			const ContainerErr e = w.AddExtBlock(0, kExtType, kExtVersion, d.data(), d.size());
			// 三档全在四字节可表达域内，一律受理；受理集就是后面读回对账的期望
			ASSERT_EQ(e, ContainerErr::kOk) << "载荷 " << lens[i] << " B 在 ext_len 四字节域内却被拒";
			accepted_lens.push_back(lens[i]);
			accepted_data.push_back(d);
		}

		// 上界外一档：n = 0xFFFFFFFF + 1。上界判排在 data 判之前，故 nullptr 载荷也能拿到 kTooLarge
		{
			const ContainerErr e = w.AddExtBlock(0, kExtType, kExtVersion, nullptr, kExtDataMax + 1ull);
			EXPECT_EQ(e, ContainerErr::kTooLarge)
				<< "载荷声明 " << (kExtDataMax + 1ull) << " B 越出 ext_len 四字节可表达域却没被拒";
		}

		const std::vector<uint8_t> f0 = FrameData(200, 5);
		FrameInput in;
		in.stream_id = 0;
		in.is_keyframe = true;
		in.payload = f0.data();
		in.payload_size = f0.size();
		ASSERT_EQ(w.WriteFrame(in), ContainerErr::kOk);
		WriteSummary sum;
		ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);

		// 链首是 30 B 的视频参数块，其后才是上面受理的那三块
		uint64_t want_start = ExtAreaStart(1u) + kParamWireLen;
		for (size_t i = 0; i < accepted_lens.size(); ++i) want_start += ExtWire(accepted_lens[i]);
		EXPECT_EQ(sum.packet_area_start, want_start);

		MemorySource src(sink.Bytes());
		ContainerReader r(src);
		ASSERT_EQ(r.Open(), ContainerErr::kOk);
		const std::vector<ExtRecord>& rec = r.ExtBlocks();
		// 被拒的那块没进文件，读回视图里也就没有它；链首那个参数块另计一条
		ASSERT_EQ(rec.size(), accepted_lens.size() + 1u);
		EXPECT_EQ(rec[0].offset, ExtAreaStart(1u));
		EXPECT_EQ(rec[0].type, kExtTypeParams);
		EXPECT_EQ(rec[0].stream_id, 0u);
		EXPECT_EQ(rec[0].data, VideoParamsBytes(kDisplayW, kDisplayH));
		uint64_t off = ExtAreaStart(1u) + kParamWireLen;
		for (size_t i = 0; i < accepted_lens.size(); ++i)
		{
			EXPECT_EQ(rec[i + 1u].offset, off) << "块 " << i;
			EXPECT_EQ(rec[i + 1u].data.size(), accepted_lens[i]) << "块 " << i;
			EXPECT_EQ(rec[i + 1u].data, accepted_data[i]) << "块 " << i;
			off += ExtWire(accepted_lens[i]);
		}
		// ext_blocks_bad 在 Open 的扩展链行走（`ContainerReader::WalkExtChains`）里更新，不依赖包区驱动
		EXPECT_EQ(r.Stats().ext_blocks_bad, 0u);

		const std::vector<ReassembledFrame> frames = TakeAll(r);
		ASSERT_EQ(frames.size(), 1u);
		EXPECT_EQ(frames[0].payload, f0);
		// resync_count 是包区阶段计数，只随 NextFrame → `ContainerReader::Resync` 的扫描递增，读完再验
		EXPECT_EQ(r.Stats().resync_count, 0u);
	}

	/* 已知会被拒的档（停链范围的反面对照）：流 0 链上第 3 块（链首参数块之后的第二块）的载荷改坏一字节 →
	 * 前缀与整块长仍成立，拦下它的是覆盖前缀 + 载荷的 crc32；本链止于此，流 1 照常整条读完。 */
	{
		constexpr size_t n0 = 5u;
		constexpr size_t n1 = 4u;
		const Built b = BuildTwoChains(n0, n1);
		std::vector<uint8_t> bytes = b.bytes;
		// 链位 0 是参数块，链位 1 是首个定长块，坏的是链位 2 ⇒ 停链时手里有 2 块（参数块 + 1 块定长块）
		const uint64_t third = ChainBlockAt(Chain0Start(2u), 2u);
		bytes[static_cast<size_t>(third) + clv::container::kExtBlockFixedPrefix + 3u] ^= 0xFF;

		MemorySource src(bytes);
		ContainerReader r(src);
		ASSERT_EQ(r.Open(), ContainerErr::kOk);
		const std::vector<ExtRecord>& rec = r.ExtBlocks();
		// 只停那一条链：流 0 停在坏块之前（链首参数块 + 1 块在手），流 1 的 4 块全在手
		EXPECT_EQ(CountChain(rec, 0), 2u);
		EXPECT_EQ(rec.size(), 2u + n1);
		EXPECT_EQ(r.Stats().ext_blocks_bad, 1u);
		CheckTailChainIntact(r, n0, n1);
		CheckPacketAreaClean(r, TwoChainPacketStart(2u, n0, n1));
	}
}

TEST(IndependentContainer, FragmentCountFieldHasNoSixteenBitWall)
{
	/* 越界可表达性检查（先做，再决定档位怎么断）：
	 *   入口 CLV_WriteParams::fragment_chunk_size 是 uint32_t（include/CLV_Container.h）；
	 *   实现层入口 ChunkFrameInput::chunk_count 是 uint32_t（src/container/writer.h）；
	 *   线上 fragment_index 是无符号 LEB128，最长 ⌈64 ÷ 7⌉ = 10 字节、覆盖 uint64 全量程
	 *   （src/container/packet.cpp 的 EncodePacket 用 EncodeVarint 写它；长度上界见 src/core/varint.h 件头）。
	 * 三处都不是 2 字节宽的分片数字段 → 65 535 不是这条路径的界，65 536 也表得出，
	 * 故该档改断「字段位宽与最大值本身 + 可表得出即必能整帧往返」，不硬造非法编码。 */
	static_assert(std::numeric_limits<decltype(CLV_WriteParams::fragment_chunk_size)>::max() == 0xFFFFFFFFu);
	static_assert(std::numeric_limits<decltype(ChunkFrameInput::chunk_count)>::max() == 0xFFFFFFFFu);
	static_assert(std::numeric_limits<decltype(ParsedPacket::fragment_index)>::max() == 0xFFFFFFFFFFFFFFFFull);

	constexpr uint32_t kChunkBytes = 8u;
	const uint32_t max16 = std::numeric_limits<uint16_t>::max();
	const uint32_t tiers[] = { max16 - 1u, max16, max16 + 1u };

	for (const uint32_t n: tiers)
	{
		SCOPED_TRACE("分片数 = " + std::to_string(n));
		std::vector<uint8_t> pool(static_cast<size_t>(n) * kChunkBytes);
		for (size_t i = 0; i < pool.size(); ++i) pool[i] = static_cast<uint8_t>((i * 29u + 5u) & 0xFFu);
		std::vector<ChunkInput> chunks(n);
		for (uint32_t i = 0; i < n; ++i)
		{
			chunks[i].data = pool.data() + static_cast<size_t>(i) * kChunkBytes;
			chunks[i].size = kChunkBytes;
		}

		MemorySink sink;
		ContainerWriter w(sink, WriterConfig {});
		ASSERT_EQ(w.AddStream(VideoStream(0, kMaxPacketForTest)), ContainerErr::kOk);
		ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);

		ChunkFrameInput in;
		in.stream_id = 0;
		in.is_keyframe = true;
		in.dts = 0;
		in.pts = 0;
		in.chunks = chunks.data();
		in.chunk_count = n;
		const ContainerErr e = w.WriteFrameChunks(in);

		if (n == max16 - 1u)
		{
			// 界下档：收
			ASSERT_EQ(e, ContainerErr::kOk);
		}
		else if (n == max16)
		{
			/* 界上档：收。写方这一路只有两条闸 —— `chunk_count == 0` 与载荷合计越 kMaxFramePayload
			 * （src/container/writer.cpp 的 WriteFrameChunks），分片数本身不参与判界；
			 * 本档合计 = 65 535 × 8 = 524 280 B，而 kMaxFramePayload = 2^30 = 1 073 741 824 B
			 * （src/container/writer.h 的派生注）⇒ 65 535 离拒侧还差三个数量级，唯一结论是「对」，
			 * 与下面 65 536 档（本件已钉成「收」）同一条通路、同一处置。 */
			ASSERT_EQ(e, ContainerErr::kOk) << "分片数正好等于两字节域顶被拒，与本例开头的位宽检查矛盾";
		}
		else
		{
			// 不可表达确认档：见本例开头的位宽检查 —— 65 536 表得出，故无「越界必拒」可断
			ASSERT_EQ(e, ContainerErr::kOk) << "三处字段位宽都表得出的分片数被拒";
		}

		WriteSummary sum;
		ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);
		// 一片一包；索引只在帧首包建条目
		EXPECT_EQ(sum.packets, static_cast<uint64_t>(n));
		EXPECT_EQ(sum.frames, 1u);
		EXPECT_EQ(sum.index_entries, 1u);
		// 扩展区里只有链首那一个 30 B 的视频参数块 → 包区起点 = 扩展区起点 + 它
		EXPECT_EQ(sum.packet_area_start, ExtAreaStart(1u) + kParamWireLen);

		MemorySource src(sink.Bytes());
		ContainerReader r(src);
		ASSERT_EQ(r.Open(), ContainerErr::kOk);
		const std::vector<ReassembledFrame> frames = TakeAll(r);
		ASSERT_EQ(frames.size(), 1u);
		EXPECT_EQ(frames[0].payload, pool);
		EXPECT_EQ(frames[0].fragments_received, static_cast<uint64_t>(n));
		EXPECT_EQ(frames[0].fragments_expected, static_cast<uint64_t>(n));
		EXPECT_TRUE(frames[0].complete);

		const ReaderStats& st = r.Stats();
		EXPECT_EQ(st.packets_ok, static_cast<uint64_t>(n));
		EXPECT_EQ(st.frames_ok, 1u);
		EXPECT_EQ(st.fragment_gaps, 0u);
		EXPECT_EQ(st.duplicate_fragments, 0u);
		EXPECT_EQ(st.missing_first_packets, 0u);
		EXPECT_EQ(st.frames_incomplete, 0u);
		EXPECT_EQ(st.dropped_crc, 0u);
		EXPECT_EQ(st.dropped_field, 0u);
		EXPECT_EQ(st.dropped_too_large, 0u);
		EXPECT_EQ(st.resync_count, 0u);
		EXPECT_FALSE(st.abandoned);
		EXPECT_FALSE(st.truncated);

		ASSERT_TRUE(r.HasIndex());
		ASSERT_EQ(r.IndexEntries().size(), 1u);
		EXPECT_EQ(static_cast<uint64_t>(r.IndexEntries()[0].file_offset), ExtAreaStart(1u) + kParamWireLen);
	}

	// 已知会被拒的两档：片数为 0、单片超该流 max_packet_size —— 都在落盘前判完
	{
		MemorySink sink;
		ContainerWriter w(sink, WriterConfig {});
		ASSERT_EQ(w.AddStream(VideoStream(0, kMaxPacketForTest)), ContainerErr::kOk);
		ASSERT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);

		const std::vector<uint8_t> one = FrameData(16, 3);
		const ChunkInput good { one.data(), one.size() };
		ChunkFrameInput zero_count;
		zero_count.stream_id = 0;
		zero_count.chunks = &good;
		zero_count.chunk_count = 0;
		EXPECT_EQ(w.WriteFrameChunks(zero_count), ContainerErr::kInvalidArgument);

		const std::vector<uint8_t> over = FrameData(kMaxPacketForTest + 1u, 4);
		const ChunkInput big { over.data(), over.size() };
		ChunkFrameInput too_big;
		too_big.stream_id = 0;
		too_big.chunks = &big;
		too_big.chunk_count = 1;
		EXPECT_EQ(w.WriteFrameChunks(too_big), ContainerErr::kTooLarge);

		WriteSummary sum;
		ASSERT_EQ(w.Finish(&sum), ContainerErr::kOk);
		EXPECT_EQ(sum.packets, 0u) << "被拒的分片调用不许落任何一片";
	}
}

TEST(IndependentContainer, StreamCountCapIsOneBelowIdSpace)
{
	/* 界与可表达性：
	 *   文件头 stream_count 是 1 字节（include/CLV_Container.h 的 CLV_FileHeadC）→ 线上最大 255；
	 *   流 id 空间 = 256（src/container/structure.h 的 kStreamIdSpace，stream_id 是 u8）；
	 *   写方在流数达到 kStreamIdSpace − 1 时拒新增（src/container/writer.cpp 的 AddStream），
	 *   即上限 = 255，与线上字段的顶值同值；越出即 kTooLarge（src/container/status.h 的注：流数 > 255）。 */
	static_assert(clv::container::kStreamIdSpace == 256u);
	static_assert(std::numeric_limits<decltype(CLV_FileHeadC::stream_count)>::max() == 255u);
	constexpr uint32_t kStreamCap = static_cast<uint32_t>(clv::container::kStreamIdSpace) - 1u;

	const uint32_t tiers[] = { kStreamCap - 1u, kStreamCap, kStreamCap + 1u };
	for (const uint32_t want: tiers)
	{
		SCOPED_TRACE("流数 = " + std::to_string(want));
		const std::string path = TempPath("clv_ind_streams_");
		ScopedFile guard(path);

		const std::vector<uint8_t> f_first = FrameData(64, 1);
		const std::vector<uint8_t> f_last = FrameData(40, 2);
		uint32_t accepted = 0u;
		CLV_ContainerError refusal = CLV_CONTAINER_OK;
		uint64_t packets = 0;
		{
			CLV_ContainerError err = CLV_CONTAINER_OK;
			CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(path.c_str(), nullptr, &err);
			ASSERT_NE(w, nullptr) << CLV_ContainerStrError(err);
			for (uint32_t i = 0; i < want; ++i)
			{
				const CLV_StreamDescC d = ApiVideoDesc(static_cast<uint8_t>(i));
				const CLV_ContainerError e = CLV_ContainerWriterAddStream(w, &d);
				ASSERT_TRUE(e == CLV_CONTAINER_OK || e == CLV_CONTAINER_TOO_LARGE)
					<< "第 " << i << " 条流：" << CLV_ContainerStrError(e);
				if (e == CLV_CONTAINER_OK)
				{
					// 每条视频流都得挂 ext_type = 3 参数块（v2 起对视频流必选）：缺块的流读方判不可用，
					// 下面那句 usable == 1 的逐流对账就红
					ASSERT_EQ(ApiAddVideoParams(w, static_cast<uint8_t>(i), kDisplayW, kDisplayH), CLV_CONTAINER_OK)
						<< "第 " << i << " 条流的参数块";
					++accepted;
				}
				else
				{
					refusal = e;
				}
			}

			EXPECT_EQ(CLV_ContainerWriterWriteFrame(w, 0, 1, 0, 0, f_first.data(), f_first.size()), CLV_CONTAINER_OK);
			EXPECT_EQ(CLV_ContainerWriterWriteFrame(w, static_cast<uint8_t>(accepted - 1u), 0, 10, 10, f_last.data(),
													f_last.size()),
					  CLV_CONTAINER_OK);
			EXPECT_EQ(CLV_ContainerWriterFinish(w, &packets), CLV_CONTAINER_OK);
			CLV_ContainerWriterClose(w);
		}

		if (want < kStreamCap)
		{
			// 界下档：全收
			EXPECT_EQ(accepted, want);
			EXPECT_EQ(refusal, CLV_CONTAINER_OK);
		}
		else if (want == kStreamCap)
		{
			/* 界上档：全收。写方的闸是「手里已经有 kStreamIdSpace − 1 条才拒新增」，判式
			 * `streams_.size() >= kStreamIdSpace - 1u`（src/container/writer.cpp 的 AddStream）⇒
			 * 登记第 255 条时手里只有 254 条，落在受理侧；被拒的是第 256 次登记，那正是下面越界档钉的那一格。 */
			EXPECT_EQ(accepted, want);
			EXPECT_EQ(refusal, CLV_CONTAINER_OK);
		}
		else
		{
			// 越界档：必须拒，码 CLV_CONTAINER_TOO_LARGE
			EXPECT_EQ(refusal, CLV_CONTAINER_TOO_LARGE) << "第 " << want << " 条流被收了，流数上界没咬住";
			EXPECT_EQ(accepted, kStreamCap);
		}

		EXPECT_EQ(packets, 2u);

		CLV_ContainerError err = CLV_CONTAINER_OK;
		CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path.c_str(), &err);
		ASSERT_NE(r, nullptr) << CLV_ContainerStrError(err);

		CLV_FileHeadC head;
		std::memset(&head, 0, sizeof(head));
		ASSERT_EQ(CLV_ContainerReaderGetHead(r, &head), CLV_CONTAINER_OK);
		EXPECT_EQ(static_cast<uint32_t>(head.stream_count), accepted) << "文件头的流数字段与实收条数不符";

		uint32_t count = 0u;
		ASSERT_EQ(CLV_ContainerReaderStreamCount(r, &count), CLV_CONTAINER_OK);
		EXPECT_EQ(count, accepted);

		for (uint32_t i = 0; i < accepted; ++i)
		{
			CLV_StreamDescC d;
			std::memset(&d, 0, sizeof(d));
			uint8_t usable = 0u;
			ASSERT_EQ(CLV_ContainerReaderGetStream(r, i, &d, &usable), CLV_CONTAINER_OK) << "第 " << i << " 条流";
			EXPECT_EQ(d.stream_id, static_cast<uint8_t>(i)) << "第 " << i << " 条流的 id";
			EXPECT_EQ(usable, 1) << "第 " << i << " 条流不可用";
		}

		uint32_t idx_count = 0u;
		ASSERT_EQ(CLV_ContainerReaderIndexCount(r, &idx_count), CLV_CONTAINER_OK);
		EXPECT_EQ(idx_count, 2u);

		std::vector<std::vector<uint8_t>> got;
		for (;;)
		{
			CLV_FrameC f;
			std::memset(&f, 0, sizeof(f));
			uint8_t frame_got = 0u;
			ASSERT_EQ(CLV_ContainerReaderNextFrame(r, &f, &frame_got), CLV_CONTAINER_OK);
			if (frame_got == 0u) break;
			std::vector<uint8_t> copy;
			if (f.payload != nullptr && f.payload_size != 0) copy.assign(f.payload, f.payload + f.payload_size);
			got.push_back(std::move(copy));
		}
		ASSERT_EQ(got.size(), 2u);
		EXPECT_EQ(got[0], f_first);
		EXPECT_EQ(got[1], f_last);

		/* 统计出参是句柄当前值的当刻拷贝（`CLV_ContainerReaderStats`），而 packets_ok /
		 * frames_ok / dropped_* / resync_* / truncated / abandoned 只在 `ContainerReader::NextFrame()`
		 * 的包区扫描里更新（`Open()` 不扫包区），ext_blocks_bad 在 `Open()` 经 `WalkExtChains()` 即定
		 * —— 故统计采样排在 NextFrame 循环读完之后。 */
		CLV_ReaderStatsC st;
		std::memset(&st, 0, sizeof(st));
		ASSERT_EQ(CLV_ContainerReaderStats(r, &st), CLV_CONTAINER_OK);
		EXPECT_EQ(st.packets_ok, 2u);
		EXPECT_EQ(st.frames_ok, 2u);
		EXPECT_EQ(st.dropped_crc, 0u);
		EXPECT_EQ(st.dropped_field, 0u);
		EXPECT_EQ(st.dropped_unknown_stream, 0u);
		EXPECT_EQ(st.resync_count, 0u);
		EXPECT_EQ(st.ext_blocks_bad, 0u);
		EXPECT_EQ(st.abandoned, 0);
		EXPECT_EQ(st.truncated, 0);

		CLV_ContainerReaderClose(r);
	}

	// 已知会被拒的档：重复登记同一条 id、以及给未登记的流名写帧
	{
		const std::string path = TempPath("clv_ind_streamid_");
		ScopedFile guard(path);
		CLV_ContainerError err = CLV_CONTAINER_OK;
		CLV_ContainerWriter* w = CLV_ContainerWriterOpenFile(path.c_str(), nullptr, &err);
		ASSERT_NE(w, nullptr) << CLV_ContainerStrError(err);
		const CLV_StreamDescC d = ApiVideoDesc(0);
		ASSERT_EQ(CLV_ContainerWriterAddStream(w, &d), CLV_CONTAINER_OK);
		ASSERT_EQ(ApiAddVideoParams(w, 0, kDisplayW, kDisplayH), CLV_CONTAINER_OK);
		// id 已在册：还在收流阶段即拒
		EXPECT_EQ(CLV_ContainerWriterAddStream(w, &d), CLV_CONTAINER_INVALID_ARGUMENT);
		const std::vector<uint8_t> f = FrameData(16, 9);
		EXPECT_EQ(CLV_ContainerWriterWriteFrame(w, 1u, 1, 0, 0, f.data(), f.size()), CLV_CONTAINER_STREAM_NOT_FOUND);
		EXPECT_EQ(CLV_ContainerWriterFinish(w, nullptr), CLV_CONTAINER_OK);
		CLV_ContainerWriterClose(w);
	}
}

TEST(IndependentVarint, TenByteCeilingAndShortestFormGate)
{
	/* 长度上界：无符号 LEB128 表 uint64 → 最长 ⌈64 ÷ 7⌉ = 10 字节
	 * （src/core/varint.cpp 的 kMaxBytes = 10 与本算式同值；src/core/varint.h 件头同述）。
	 * overlong 由 2 个函数各判一条，两处判点各自独立：`core::DecodeVarint` 只拒「第 10 字节贡献位数超出
	 * uint64 量程」那一支（varint.cpp 的 i == kMaxBytes − 1 分支），`core::IsShortestVarint` 另判最短形式
	 * （varint.cpp 的 ShortestLen(value) == used）。
	 * 容器层字段区把这两个函数串成 2 个封装：`ReadVarintField` 与 `ReadZigZagField`
	 * （src/container/packet.cpp，后者经 core::DecodeZigZag 间接走同一支量程判）——
	 * 封装内部各 2 处调用点、合计 4 处；两个封装自身在 packet.cpp 的 DecodePacket 里共 5 处调用点
	 * （fragment_index / dts_delta / pts_delta / payload_size / ext_len_varint 各一处，
	 * 其中 fragment_index 与 pts_delta 那两处带条件）。不成立即 PacketParse::kFieldFail。 */
	constexpr size_t kMaxBytes = (64u + 6u) / 7u;
	static_assert(kMaxBytes == 10u, "与 src/core/varint.cpp 的 kMaxBytes 同值");
	constexpr uint64_t kU64Max = std::numeric_limits<uint64_t>::max();

	// 最长合法编码：前 9 字节各贡献 7 位全 1（63 位），第 10 字节贡献 1 位 → 值 = 2^64 − 1
	std::vector<uint8_t> longest(9u, 0xFFu);
	longest.push_back(0x01u);
	// 超长 1 字节：10 个延续字节 + 1 个终止字节，终止字节落在 10 字节窗口外 → 解码这一步就拒
	std::vector<uint8_t> too_long(10u, 0x80u);
	too_long.push_back(0x00u);
	// overlong（量程外）：第 10 字节载荷给 2，需要第 65 位 → 拒而不是折返
	std::vector<uint8_t> over_range(9u, 0x80u);
	over_range.push_back(0x82u);
	// overlong（非最短）：值 0 用两字节表 → 解码放过，最短形式判拒
	const std::vector<uint8_t> padded_zero = { 0x80u, 0x00u };

	struct Tier
	{
		const char* label;
		std::vector<uint8_t> bytes;
		size_t consumed;	// 0 = 解码档拒
		uint64_t value;
		bool shortest;
	};

	const Tier tiers[] = {
		Tier { "最长合法 10 字节",	longest,	 kMaxBytes, kU64Max, true  },
		Tier { "超长 11 字节",		too_long,	 0u,		0u,		 false },
		  Tier { "第 10 字节量程外",  over_range,  0u,		  0u,	   false },
		Tier { "值 0 的非最短形式", padded_zero, 2u,		0u,		 false }
	};

	for (const Tier& t: tiers)
	{
		SCOPED_TRACE(std::string("档：") + t.label);

		uint64_t out = kSentinel;
		const size_t used = clv::core::DecodeVarint(t.bytes.data(), t.bytes.size(), &out);
		EXPECT_EQ(used, t.consumed);
		if (t.consumed == 0u)
		{
			// 拒码：DecodeVarint 的失败约定是返回 0 且不写出参（src/core/varint.h 的返回值说明）
			EXPECT_EQ(out, kSentinel) << "拒档把出参写脏了";
		}
		else
		{
			// 读数按 Σ (字节 & 0x7F) << 7i 独立复算，不取被测函数的返回值
			EXPECT_EQ(out, SumVarintValue(t.bytes));
			EXPECT_EQ(out, t.value);
		}
		EXPECT_EQ(clv::core::IsShortestVarint(t.bytes.data(), t.bytes.size()), t.shortest);
	}

	// 缺终止字节的截断输入同属拒档
	uint64_t probe = kSentinel;
	EXPECT_EQ(clv::core::DecodeVarint(longest.data(), 9u, &probe), 0u);
	EXPECT_EQ(probe, kSentinel);

	const uint64_t limit = static_cast<uint64_t>(kMaxPacketForTest) + kOuterOverhead;

	// 合法最长编码不许当坏值喂：dts_delta 用满 10 字节 → 字段区成立
	std::vector<uint8_t> full_length = { 0x00u, kFlagsFrameFirst };
	full_length.insert(full_length.end(), longest.begin(), longest.end());
	const uint8_t tail_fields[] = { 0x00u, 0x03u, 0x00u };	  // pts_delta 0 / payload_size 3 / ext_len 0
	full_length.insert(full_length.end(), tail_fields, tail_fields + 3u);
	const std::vector<uint8_t> pay = FrameData(3, 7);
	const std::vector<uint8_t> ok_packet = RawPacket(full_length, pay);
	ParsedPacket p;
	ASSERT_EQ(DecodePacket(ok_packet.data(), ok_packet.size(), limit, &p), PacketParse::kOk);
	EXPECT_EQ(p.dts_delta, kU64Max);
	EXPECT_EQ(p.payload, pay);

	// 非最短的 payload_size：crc32 与 flags 指纹都成立，拦下它的只有最短形式那一道
	std::vector<uint8_t> non_shortest = { 0x00u, kFlagsFrameFirst, 0x00u, 0x00u };
	non_shortest.insert(non_shortest.end(), padded_zero.begin(), padded_zero.end());
	non_shortest.push_back(0x00u);
	const std::vector<uint8_t> non_shortest_packet = RawPacket(non_shortest, {});
	ParsedPacket q;
	EXPECT_EQ(DecodePacket(non_shortest_packet.data(), non_shortest_packet.size(), limit, &q), PacketParse::kFieldFail);

	// 同形对照：payload_size 换成最短形式（1 字节 0）即成立
	std::vector<uint8_t> shortest_form = { 0x00u, kFlagsFrameFirst, 0x00u, 0x00u, 0x00u, 0x00u };
	const std::vector<uint8_t> good_packet = RawPacket(shortest_form, {});
	ParsedPacket s;
	ASSERT_EQ(DecodePacket(good_packet.data(), good_packet.size(), limit, &s), PacketParse::kOk);
	EXPECT_EQ(s.payload_size, 0u);

	// 超长 11 字节进字段区：解码一步返回 0 → 字段区不成立
	std::vector<uint8_t> over_long_field = { 0x00u, kFlagsFrameFirst, 0x00u, 0x00u };
	over_long_field.insert(over_long_field.end(), too_long.begin(), too_long.end());
	over_long_field.push_back(0x00u);
	const std::vector<uint8_t> over_long_packet = RawPacket(over_long_field, {});
	ParsedPacket t;
	EXPECT_EQ(DecodePacket(over_long_packet.data(), over_long_packet.size(), limit, &t), PacketParse::kFieldFail);

	// 量程外的第 10 字节同样进字段区
	std::vector<uint8_t> over_range_field = { 0x00u, kFlagsFrameFirst, 0x00u, 0x00u };
	over_range_field.insert(over_range_field.end(), over_range.begin(), over_range.end());
	over_range_field.push_back(0x00u);
	const std::vector<uint8_t> over_range_packet = RawPacket(over_range_field, {});
	ParsedPacket u;
	EXPECT_EQ(DecodePacket(over_range_packet.data(), over_range_packet.size(), limit, &u), PacketParse::kFieldFail);
}
