/* test_container_extchain_timebase.cpp
 *
 * **实现层用例**：直接 include 实现层头（io / reader / timebase / writer），不经公开 C ABI，
 * **仅静态构建（`CLV_BUILD_SHARED=OFF`）下可链**。
 *
 * 钉三条口径：
 *   一，扩展块链里一条链断掉只停那一条链，其余链照常走，包区起点照常推进到已确认的扩展区末尾：
 *   `src/container/reader.cpp` 的 `WalkExtChains` 每处断链都是 `break`，`area_start_ = tail`
 *   无条件排在函数末尾。
 *   二，时间基换算的商一出 64 位无符号域即拒，出参不被截断值写脏：`src/container/timebase.cpp` 的
 *   `ToGlobalTicks` 对 4 个 32 位 limb 做长除，`i <= 1 && part != 0` 即返回 false，
 *   而 `*ticks = q` 排在整个循环之后。
 *   三，断链三档各自有没有块计数：载荷解析不成立（含 crc32 不符）计一处、声明长度超出接受域计一处、
 *   声明值在接受域内而整块读不进文件不计 —— 前两档见下面两条用例的算式与出处。
 * 统计字段的名称与含义按 `include/CLV_Container.h` 的 `CLV_ReaderStatsC`（实现层同名项见
 * `src/container/reader.h` 的 `ReaderStats`，C ABI 侧逐字段搬运见 `src/container/clv_container.cpp`）。
 *
 * 布局：文件头 64 | 流描述符 68×N | 扩展块区 | 包区 | 索引区（16 + 20×条目）| 文件尾 16。
 * 扩展块线上形：magic 4 | ext_type 1 | ext_version 1 | ext_len 4 | next_ext_offset 8 |
 * 载荷 ext_len | crc32 4，单块长 = 18 + ext_len + 4；crc32 覆盖 magic 起到 crc32 之前
 * （`src/container/structure.cpp` 的 `EncodeExtBlock` / `DecodeExtBlock`）。
 * 链按流分组：同一条流的块连续存放、组内保持登记序，组尾 `next_ext_offset` = 0
 * （`src/container/writer.cpp` 的 `BeginPackets`）。
 */

#include "container/io.h"
#include "container/reader.h"
#include "container/timebase.h"
#include "container/writer.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <string>
#include <vector>

namespace
{

	using clv::container::ContainerErr;
	using clv::container::ContainerReader;
	using clv::container::ContainerWriter;
	using clv::container::ExtRecord;
	using clv::container::MemorySink;
	using clv::container::MemorySource;
	using clv::container::ReassembledFrame;
	using clv::container::StreamDesc;
	using clv::container::StreamType;
	using clv::container::WriterConfig;
	using clv::container::WriteSummary;

	constexpr uint8_t kVideo = static_cast<uint8_t>(StreamType::kVideo);
	constexpr uint8_t kSub = static_cast<uint8_t>(StreamType::kSubtitle);
	constexpr uint8_t kExtVersion = 1;

	// 算式里的基础时间基必须与实现层同一个值
	constexpr uint32_t kBase = 1081080000u;
	static_assert(static_cast<uint64_t>(kBase) == clv::container::kBaseTicksPerSecond,
				  "基础时间基与 src/container/timebase.h 的常量不一致");

	// 拒绝路径下出参该保持原样，用它做「没被写过」的记号
	constexpr uint64_t kSentinel = 0xDEADBEEFCAFEF00Dull;

	StreamDesc MakeStream(uint8_t type, uint8_t id, uint32_t max_packet)
	{
		StreamDesc d;
		d.stream_id = id;
		d.stream_type = type;
		d.codec_id = 0;
		d.codec_version = 1;
		d.max_packet_size = max_packet;
		d.first_dts = 0;
		// 时间基 1/1：索引区的换算与文件头时长都按 tick 原样走
		d.timebase_num = 1;
		d.timebase_den = 1;
		d.sample_rate = 0;
		d.channels = 0;
		d.bit_depth = type == kSub ? 0 : 8;
		return d;
	}

	std::vector<uint8_t> DataOf(size_t n, uint8_t seed)
	{
		std::vector<uint8_t> v(n);
		for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 37u + seed) & 0xFFu);
		return v;
	}

	/* 视频流的 ext_type = 3 参数块（`docs/容器规范.md` 的扩展块一节，v2 起对视频流必选）：缺块的流会被读方记
	 * video_stream_no_param_block 并判该流不可用（src/container/reader.cpp），那样本件的链面就验不下去。
	 * 载荷 8 字节，字段布局照 src/video/headers.cpp 的 ParseEncMeta：
	 *   [0] max_b_frames  [1] quant_precision  [2] ext_tools  [3] reserved  [4..5] width LE16  [6..7] height LE16
	 * 本件只验扩展块链的容器面，画面尺寸取一个合法非零值（kDisplayW x kDisplayH）。 */
	constexpr uint16_t kDisplayW = 352u;
	constexpr uint16_t kDisplayH = 288u;

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

	ContainerErr AddVideoParams(ContainerWriter& w, uint8_t stream_id, uint16_t width, uint16_t height)
	{
		const std::vector<uint8_t> meta = VideoParamsBytes(width, height);
		return w.AddExtBlock(stream_id, clv::container::kExtTypeEncMeta, kExtVersion, meta.data(), meta.size());
	}

	// 扩展块线上长 = 前缀 18 + ext_len + crc32 4
	constexpr size_t ExtWire(size_t ext_len)
	{
		return clv::container::kExtBlockFixedPrefix + ext_len + clv::container::kExtBlockCrcSize;
	}

	/* 单个扩展块载荷的读方接受域上界（`src/container/reader.cpp` 的 `kMaxExtPayloadAccept`）：
	 * 那条算式 = 默认读方窗口 − 扩展块定长前缀 − 块尾 CRC = 8 MiB − 18 − 4 = 8 388 586 B。
	 * 该常量住在 .cpp 的匿名命名空间里、不经头暴露，本件按同一算式用头里的三个具名常量推出，
	 * 并把数值与算式的对应钉在下面的 static_assert 上（改包结构时这里当场编译失败）。 */
	constexpr uint64_t kExtAcceptCeil = clv::container::kDefaultReadWindow -
										static_cast<uint64_t>(clv::container::kExtBlockFixedPrefix) -
										static_cast<uint64_t>(clv::container::kExtBlockCrcSize);
	static_assert(kExtAcceptCeil == 8388586ull, "接受域上界 = 8 MiB − 18 − 4 = 8 388 586");

	// 两条流 → 扩展区起点 = 64 + 68×2 = 200。流 0（视频）的链四块：先 ext_type = 3 参数块（载荷 8 B），
	// 再 ext_type 4 → 5 → 6（载荷 100 / 200 / 60 B）；流 1（字幕）的链两块（载荷 16 / 24 B），
	// 按登记序连续排；单块线上长 = 18 + ext_len + 4 = 30 / 122 / 222 / 82 / 38 / 46 B，
	// 于是六块分别落在 @200 / @230 / @352 / @574 / @656 / @694，
	// 扩展区末尾 = @694 + 46 = 740 = 包区起点。
	constexpr uint64_t kExtAreaStart = clv::container::kFileHeadSize + 2ull * clv::container::kStreamDescSize;
	constexpr uint64_t kM = kExtAreaStart;			// 流 0 的参数块（8 B）
	constexpr uint64_t kA0 = kM + ExtWire(8);		// 100 B
	constexpr uint64_t kA1 = kA0 + ExtWire(100);	// 200 B
	constexpr uint64_t kA2 = kA1 + ExtWire(200);	// 60 B
	constexpr uint64_t kB0 = kA2 + ExtWire(60);		// 16 B（流 1 链首）
	constexpr uint64_t kB1 = kB0 + ExtWire(16);		// 24 B
	constexpr uint64_t kPacketStart = kB1 + ExtWire(24);

	std::vector<uint8_t> ExtData(int index)
	{
		// 五块「任意载荷」块各自的字节：下标 = 链上的块序（流 0 的后三块、流 1 的两块），长度与上面的算式一致；
		// 流 0 链首那个参数块不在这里，走 VideoParamsBytes()
		static const size_t kLens[5] = { 100u, 200u, 60u, 16u, 24u };
		static const uint8_t kSeeds[5] = { 11u, 21u, 31u, 41u, 51u };
		return DataOf(kLens[index], kSeeds[index]);
	}

	std::vector<std::vector<uint8_t>> FramePayloads() { return { DataOf(300, 1), DataOf(40, 2), DataOf(200, 3) }; }

	// 把 @352 那块（流 0 链上参数块之后的第二块定长块）的 ext_len 改成给定声明值（块内 4 字节小端）
	void SetDeclaredExtLen(std::vector<uint8_t>& bytes, uint64_t declared)
	{
		for (size_t i = 0; i < 4u; ++i)
		{
			bytes[static_cast<size_t>(kA1) + clv::container::kExtLenOffset + i] =
				static_cast<uint8_t>((declared >> (8u * i)) & 0xFFu);
		}
	}

	struct Built
	{
		std::vector<uint8_t> bytes;
		WriteSummary sum;
	};

	Built BuildTwoChains()
	{
		MemorySink sink;
		ContainerWriter w(sink, WriterConfig {});
		EXPECT_EQ(w.AddStream(MakeStream(kVideo, 0, 4096)), ContainerErr::kOk);
		EXPECT_EQ(w.AddStream(MakeStream(kSub, 1, 1024)), ContainerErr::kOk);
		// 视频流的参数块登记在链首：停链档也要先读到它，那条流才不被判不可用
		EXPECT_EQ(AddVideoParams(w, 0, kDisplayW, kDisplayH), ContainerErr::kOk);

		// 登记序就是链序：流 0 的 ext_type 3 → 4 → 5 → 6，流 1 的 7 → 8
		const std::vector<uint8_t> a0 = ExtData(0);
		const std::vector<uint8_t> a1 = ExtData(1);
		const std::vector<uint8_t> a2 = ExtData(2);
		const std::vector<uint8_t> b0 = ExtData(3);
		const std::vector<uint8_t> b1 = ExtData(4);
		EXPECT_EQ(w.AddExtBlock(0, 4, kExtVersion, a0.data(), a0.size()), ContainerErr::kOk);
		EXPECT_EQ(w.AddExtBlock(0, 5, kExtVersion, a1.data(), a1.size()), ContainerErr::kOk);
		EXPECT_EQ(w.AddExtBlock(0, 6, kExtVersion, a2.data(), a2.size()), ContainerErr::kOk);
		EXPECT_EQ(w.AddExtBlock(1, 7, kExtVersion, b0.data(), b0.size()), ContainerErr::kOk);
		EXPECT_EQ(w.AddExtBlock(1, 8, kExtVersion, b1.data(), b1.size()), ContainerErr::kOk);

		const std::vector<std::vector<uint8_t>> fps = FramePayloads();
		clv::container::FrameInput in;
		in.stream_id = 0;
		in.is_keyframe = true;
		in.payload = fps[0].data();
		in.payload_size = fps[0].size();
		EXPECT_EQ(w.WriteFrame(in), ContainerErr::kOk);
		in.stream_id = 1;
		in.payload = fps[1].data();
		in.payload_size = fps[1].size();
		EXPECT_EQ(w.WriteFrame(in), ContainerErr::kOk);
		in.stream_id = 0;
		in.dts = 100;
		in.pts = 100;
		in.payload = fps[2].data();
		in.payload_size = fps[2].size();
		EXPECT_EQ(w.WriteFrame(in), ContainerErr::kOk);

		Built out;
		EXPECT_EQ(w.Finish(&out.sum), ContainerErr::kOk);
		out.bytes = sink.Bytes();
		return out;
	}

	// 走完包区并逐帧对账载荷，返回帧数
	size_t ReadFrames(ContainerReader& r, const std::vector<std::vector<uint8_t>>& want)
	{
		size_t n = 0;
		for (;;)
		{
			ReassembledFrame f;
			if (! r.NextFrame(&f)) break;
			if (n < want.size()) EXPECT_EQ(f.payload, want[n]) << "第 " << n << " 帧";
			++n;
		}
		return n;
	}

	// 动手之前的对账件：六个块按算式落进扩展区、包区起点就是扩展区末尾、三帧全读得回
	void CheckCleanBuild(const Built& b)
	{
		EXPECT_EQ(b.sum.packet_area_start, kPacketStart);
		EXPECT_EQ(b.sum.packets, 3u);
		EXPECT_EQ(b.sum.frames, 3u);
		EXPECT_EQ(b.sum.index_entries, 3u);

		MemorySource src(b.bytes);
		ContainerReader r(src);
		ASSERT_EQ(r.Open(), ContainerErr::kOk);
		const std::vector<ExtRecord>& rec = r.ExtBlocks();
		ASSERT_EQ(rec.size(), 6u);
		const uint64_t want_ofs[6] = { kM, kA0, kA1, kA2, kB0, kB1 };
		const uint8_t want_type[6] = { 3u, 4u, 5u, 6u, 7u, 8u };
		const uint8_t want_stream[6] = { 0u, 0u, 0u, 0u, 1u, 1u };
		for (int i = 0; i < 6; ++i)
		{
			EXPECT_EQ(rec[i].offset, want_ofs[i]) << "块 " << i;
			EXPECT_EQ(rec[i].type, want_type[i]) << "块 " << i;
			EXPECT_EQ(rec[i].stream_id, want_stream[i]) << "块 " << i;
			// 块 0 是视频参数块（8 B 定形），其余五块是链上的任意载荷块
			const std::vector<uint8_t> want_data = (i == 0) ? VideoParamsBytes(kDisplayW, kDisplayH) : ExtData(i - 1);
			EXPECT_EQ(rec[i].data, want_data) << "块 " << i;
		}
		EXPECT_EQ(r.Stats().ext_blocks_bad, 0u);
		EXPECT_EQ(ReadFrames(r, FramePayloads()), 3u);
	}

	// 坏链之后两条链共同的结果：包区起点仍是 740（= 上面 kPacketStart 那条算式），三帧全读回、一次重同步都不做
	void CheckPacketAreaEnteredNormally(const ContainerReader& r, uint64_t packets)
	{
		const clv::container::ReaderStats& st = r.Stats();
		EXPECT_EQ(st.packets_ok, packets);
		EXPECT_EQ(st.resync_count, 0u);
		EXPECT_EQ(st.resync_bytes_scanned, 0u);
		EXPECT_EQ(st.resync_failed, 0u);
		EXPECT_EQ(st.dropped_crc, 0u);
		EXPECT_EQ(st.dropped_field, 0u);
		EXPECT_EQ(st.dropped_too_large, 0u);
		EXPECT_EQ(st.dropped_unknown_stream, 0u);
		EXPECT_EQ(st.fragment_gaps, 0u);
		EXPECT_EQ(st.frames_ok, 3u);
		EXPECT_FALSE(st.abandoned);
		EXPECT_FALSE(st.truncated);
	}

}	 // namespace

TEST(ContainerExtChainTimebase, ExtChainBadBlockStopsOnlyThatChain)
{
	const Built b = BuildTwoChains();
	CheckCleanBuild(b);

	std::vector<uint8_t> bytes = b.bytes;
	// 流 0 链上第三块（@352，即参数块之后的那块 200 B）的载荷改坏一个字节：ext_len 与 next_ext_offset 都不动，
	// 18 字节前缀读得进、整块长 18 + 200 + 4 = 222 B 也仍在文件里，
	// 拦下来它的是覆盖 18 + 200 = 218 字节的 crc32 —— 走「记一处坏块 + 停本链」那一档
	bytes[static_cast<size_t>(kA1) + clv::container::kExtBlockFixedPrefix + 3] ^= 0xFF;

	MemorySource src(bytes);
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);

	// 本链止于坏块：参数块与它后面那块（@230）在手，@574 那块不再登记；流 1 的两块照常走完整条链
	const std::vector<ExtRecord>& rec = r.ExtBlocks();
	ASSERT_EQ(rec.size(), 4u);
	EXPECT_EQ(rec[0].offset, kM);
	EXPECT_EQ(rec[0].type, 3);
	EXPECT_EQ(rec[0].stream_id, 0);
	EXPECT_EQ(rec[0].data, VideoParamsBytes(kDisplayW, kDisplayH));
	EXPECT_EQ(rec[1].offset, kA0);
	EXPECT_EQ(rec[1].type, 4);
	EXPECT_EQ(rec[1].stream_id, 0);
	EXPECT_EQ(rec[1].data, ExtData(0));
	EXPECT_EQ(rec[2].offset, kB0);
	EXPECT_EQ(rec[2].type, 7);
	EXPECT_EQ(rec[2].stream_id, 1);
	EXPECT_EQ(rec[2].data, ExtData(3));
	EXPECT_EQ(rec[3].offset, kB1);
	EXPECT_EQ(rec[3].type, 8);
	EXPECT_EQ(rec[3].data, ExtData(4));

	// 坏块计数一处，字段口径同 CLV_ReaderStatsC 的 ext_blocks_bad
	EXPECT_EQ(r.Stats().ext_blocks_bad, 1u);

	// 包区起点仍推到已确认的扩展区末尾 740：第一个包就从真包首解析，扩展区没被当包区读
	EXPECT_EQ(ReadFrames(r, FramePayloads()), 3u);
	CheckPacketAreaEnteredNormally(r, b.sum.packets);
	EXPECT_TRUE(r.HasIndex());
	EXPECT_EQ(r.IndexEntryCount(), 3u);
}

TEST(ContainerExtChainTimebase, ExtBlockLengthBeyondFileStopsChainWithoutCounting)
{
	const Built b = BuildTwoChains();
	CheckCleanBuild(b);

	/* 声明长度取接受域上界本身 = 8 388 586（界上档，判式是「> 上界」才进接受域档，等于上界不算越界）：
	 * 整块长 = 18 + 8 388 586 + 4 = 8 388 608 B，远超该块起点到文件末尾的可读量，
	 * 于是断在「整块读不进文件」那一档。这一档排在 crc32 校验与整块分配之前，故一处坏块都不计，
	 * 也不会按声明值去要那块内存。本件这份文件比声明的整块长短得多——下面那句 ASSERT_LT 把这个前提
	 * 钉在运行时读数上，故不必真备 8 MiB 载荷。 */
	std::vector<uint8_t> bytes = b.bytes;
	SetDeclaredExtLen(bytes, kExtAcceptCeil);
	ASSERT_LT(bytes.size(), static_cast<size_t>(kA1) + ExtWire(kExtAcceptCeil));

	MemorySource src(bytes);
	ContainerReader r(src);
	ASSERT_EQ(r.Open(), ContainerErr::kOk);

	const std::vector<ExtRecord>& rec = r.ExtBlocks();
	ASSERT_EQ(rec.size(), 4u);
	EXPECT_EQ(rec[0].offset, kM);
	EXPECT_EQ(rec[0].type, 3);
	EXPECT_EQ(rec[1].offset, kA0);
	EXPECT_EQ(rec[1].type, 4);
	EXPECT_EQ(rec[2].offset, kB0);
	EXPECT_EQ(rec[2].type, 7);
	EXPECT_EQ(rec[3].offset, kB1);
	EXPECT_EQ(rec[3].type, 8);

	// 停链但不计坏块：整块读不进文件既不分配、也不记「解析不成立」
	EXPECT_EQ(r.Stats().ext_blocks_bad, 0u);

	EXPECT_EQ(ReadFrames(r, FramePayloads()), 3u);
	CheckPacketAreaEnteredNormally(r, b.sum.packets);
}

TEST(ContainerExtChainTimebase, ExtBlockDeclaredLengthBeyondAcceptDomainIsCounted)
{
	const Built b = BuildTwoChains();
	CheckCleanBuild(b);

	/* 接受域档（与上面「整块读不进文件」那一档分开钉，两档结论不同：那一档不计数、这一档计数）：
	 * 声明长度只要大于接受域上界 8 388 586 就落在这一档 —— 接受域判排在整块长度核对、分配与 crc32 之前，
	 * 故界上外侧一档（上界 + 1）与该字段全 1 一档都走到这里、各计一处。
	 * 计数口径：声明长度超出接受域的块同样计入块计数：块计数记的是「解析不成立的块数」，
	 * 含接受域外的那一类。判式是「声明长度 > 上界」，所以上界本身不在此档（见上一条用例）。 */
	static const uint64_t kBeyond[] = { kExtAcceptCeil + 1ull, 0xFFFFFFFFull };
	for (const uint64_t declared: kBeyond)
	{
		SCOPED_TRACE("声明载荷 = " + std::to_string(declared));
		std::vector<uint8_t> bytes = b.bytes;
		SetDeclaredExtLen(bytes, declared);
		// 两档的声明值都换来「整块比文件还长」，本件不构造 8 MiB 素材；靠的就是接受域判排在整块核对之前
		ASSERT_LT(bytes.size(), static_cast<size_t>(kA1) + clv::container::kExtBlockFixedPrefix + declared +
									static_cast<uint64_t>(clv::container::kExtBlockCrcSize));

		MemorySource src(bytes);
		ContainerReader r(src);
		ASSERT_EQ(r.Open(), ContainerErr::kOk) << "域外声明只停本链，不判整文件损坏";

		// 只停流 0 那一条链：参数块与它后面那块在手，@574 那块不再登记；流 1 的链照常走完
		const std::vector<ExtRecord>& rec = r.ExtBlocks();
		ASSERT_EQ(rec.size(), 4u);
		EXPECT_EQ(rec[0].offset, kM);
		EXPECT_EQ(rec[1].offset, kA0);
		EXPECT_EQ(rec[2].offset, kB0);
		EXPECT_EQ(rec[3].offset, kB1);

		// 这一档计一处块计数（域外也计数：两条链同判的其中一条，另一条是段描述块，见 test_segments.cpp）
		EXPECT_EQ(r.Stats().ext_blocks_bad, 1u);

		// 包区起点仍按已确认的扩展区末尾推进，帧一帧不少
		EXPECT_EQ(ReadFrames(r, FramePayloads()), 3u);
		CheckPacketAreaEnteredNormally(r, b.sum.packets);
	}
}

TEST(ContainerExtChainTimebase, GlobalTicksOverflowIsRefusedWithoutWritingOutParam)
{
	// 判据：商 = v × num × 1081080000 ÷ den 一出 64 位无符号域即返回 false 且出参不写。
	// 等价式：商 ≥ 2^64 ⟺ v × num × 1081080000 ≥ den × 2^64。
	// 2^64 = 18446744073709551616。
	uint64_t ticks = 0;

	// num = 基础时间基、den = 1、v = 16：
	// 1081080000² = 1168733966400000000，16 × 它 = 18699743462400000000
	// = 2^64 + 252999388690448384 → 乘积的高 64 位 = 1，
	// 拆成 4 个 32 位 limb 后第一段 = 1 >> 32 = 0、第二段 = 1，den = 1 → 第二段商 = 1 ≠ 0
	ticks = kSentinel;
	EXPECT_FALSE(clv::container::ToGlobalTicks(16ull, kBase, 1u, &ticks));
	EXPECT_EQ(ticks, kSentinel);

	// num = 1、den = 48000、v = 819036255908961：
	// v × 1081080000 = 885443715538059557880000 ≥ 48000 × 2^64 = 885443715538058477568000
	// → 高 64 位 = 48000 = den；第一段 limb = 48000 >> 32 = 0（只盯第一段就放过），
	// 第二段 limb = 48000，商 = 48000 ÷ 48000 = 1 ≠ 0
	ticks = kSentinel;
	EXPECT_FALSE(clv::container::ToGlobalTicks(819036255908961ull, 1u, 48000u, &ticks));
	EXPECT_EQ(ticks, kSentinel);

	// num = 1、den = 1、v = 17063255332：v × 1081080000 = 18446744074318560000
	// = 2^64 + 609008384 → 高 64 位 = 1，第二段商 = 1 ÷ 1 = 1 ≠ 0
	ticks = kSentinel;
	EXPECT_FALSE(clv::container::ToGlobalTicks(17063255332ull, 1u, 1u, &ticks));
	EXPECT_EQ(ticks, kSentinel);
}

TEST(ContainerExtChainTimebase, GlobalTicksAcceptsEveryQuotientInsideUnsigned64)
{
	uint64_t ticks = 0;

	// 上一条里 v = 16 的邻位：15 × 1168733966400000000 = 17531009496000000000 < 2^64
	ticks = kSentinel;
	ASSERT_TRUE(clv::container::ToGlobalTicks(15ull, kBase, 1u, &ticks));
	EXPECT_EQ(ticks, 17531009496000000000ull);

	// den = 48000 档的邻位：v = 819036255908960 → v × 1081080000 = 885443715538058476800000，
	// 高 64 位 = 47999 < 48000 → 收；商 = 885443715538058476800000 ÷ 48000
	// = 18446744073709551600 = 2^64 − 16，整除无余数
	ticks = kSentinel;
	ASSERT_TRUE(clv::container::ToGlobalTicks(819036255908960ull, 1u, 48000u, &ticks));
	EXPECT_EQ(ticks, 18446744073709551600ull);

	// den = 1 档的邻位：v = 17063255331 → 17063255331 × 1081080000 = 18446744073237480000
	// = 2^64 − 472071616，仍在 64 位无符号域内
	ticks = kSentinel;
	ASSERT_TRUE(clv::container::ToGlobalTicks(17063255331ull, 1u, 1u, &ticks));
	EXPECT_EQ(ticks, 18446744073237480000ull);

	// 日常档：48000 个样本按 1/48000 换算正好一秒 → 1081080000 个基础 tick
	// 48000 × 1 × 1081080000 ÷ 48000 = 1081080000
	ticks = kSentinel;
	ASSERT_TRUE(clv::container::ToGlobalTicks(48000ull, 1u, 48000u, &ticks));
	EXPECT_EQ(ticks, 1081080000ull);

	// v = 0 不属溢出档：乘积与商都是 0
	ticks = kSentinel;
	ASSERT_TRUE(clv::container::ToGlobalTicks(0ull, kBase, 48000u, &ticks));
	EXPECT_EQ(ticks, 0ull);
}

TEST(ContainerExtChainTimebase, GlobalTicksRejectsZeroOperandsAndNullOutParam)
{
	// 入口第一条检查：出参为空、分子为 0、分母为 0 都直接 false，一个 limb 都不动
	uint64_t ticks = kSentinel;
	EXPECT_FALSE(clv::container::ToGlobalTicks(1ull, 1u, 1u, nullptr));
	EXPECT_FALSE(clv::container::ToGlobalTicks(1ull, 0u, 1u, &ticks));
	EXPECT_FALSE(clv::container::ToGlobalTicks(1ull, 1u, 0u, &ticks));
	EXPECT_EQ(ticks, kSentinel);

	// num / den 的允许上界由 TimebaseOk 定（各自 ≤ 0xFFFFFFFF 且非零），换算口自己不重复设界
	ticks = kSentinel;
	ASSERT_TRUE(clv::container::ToGlobalTicks(1ull, 0xFFFFFFFFu, 0xFFFFFFFFu, &ticks));
	EXPECT_EQ(ticks, 1081080000ull);	// 1 × 0xFFFFFFFF × 1081080000 ÷ 0xFFFFFFFF
}
