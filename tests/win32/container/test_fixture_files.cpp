/* test_fixture_files.cpp
 *
 * 入库素材的端到端读回测试：读 `assets/fixtures/` 三份确定性 `.clv`，走公开 C ABI 一遍
 * （打开 → 文件头 → 流描述符 → 扩展块 → 索引 → 逐帧 → 统计）。
 *
 * 断言基准都在仓库里：字节数与整文件 CRC-32/MPEG-2 取 `assets/README.md` 的冻结表；
 * 头字段、描述符、扩展块载荷、索引条目数与逐帧 (流号, 载荷字节数, 片数, 关键帧位) 是这三份文件的结构事实，
 * 改动画面生成器或改了线上布局都会让这里变红。
 *
 * 统计口径：读方统计必须在全程取帧**之后**再读一次，`packets_ok` 才等于文件头的 `total_packets`，
 * 各丢弃与重同步计数才能证明整轮没丢东西。
 */

#include "CLV_Container.h"
#include "core/crc32.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace
{

	struct ExpectStream
	{
		uint8_t id;
		uint8_t type;
		uint8_t codec;
		uint64_t tb_num;
		uint64_t tb_den;
		uint32_t max_packet;
		uint16_t bit_depth;
	};

	struct ExpectFrame
	{
		uint8_t stream_id;
		uint64_t payload_size;
		uint64_t fragments;
		uint8_t is_keyframe;
	};

	/* 一条扩展块的期望：读回视图的顺序 = 链按流分组、组内保持登记序，故按块序逐条登记。 */
	struct ExpectExt
	{
		uint8_t stream_id;
		uint8_t type;
		uint64_t size;
		std::string payload;
	};

	struct ExpectFixture
	{
		const char* file;
		uint64_t bytes;			 // 冻结值：字节数
		uint32_t crc32_mpeg2;	 // 冻结值：整文件 CRC-32/MPEG-2
		uint8_t stream_count;
		uint64_t duration_ticks;
		uint64_t total_packets;
		uint64_t index_offset;
		uint64_t index_entries;
		std::vector<ExpectStream> streams;
		std::vector<ExpectFrame> frames;
		uint32_t ext_count;
		std::vector<ExpectExt> exts;	// 条数与 ext_count 一一对应，按块序
	};

	// 素材目录由 CMake 传进来；没定义就退回相对路径，让失败信息直白
#ifndef CLV_ASSETS_DIR
	#define CLV_ASSETS_DIR "assets/fixtures"
#endif

	std::string FixturePath(const char* name) { return std::string(CLV_ASSETS_DIR) + "/" + name; }

	bool ReadAllBytes(const std::string& path, std::vector<uint8_t>& out)
	{
		std::ifstream f(path, std::ios::binary);
		if (! f) return false;
		out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		return true;
	}

	void CheckFixture(const ExpectFixture& e)
	{
		const std::string path = FixturePath(e.file);

		// ---- 冻结值：字节数 + 整文件 CRC-32/MPEG-2 ----
		std::vector<uint8_t> raw;
		ASSERT_TRUE(ReadAllBytes(path, raw)) << "打不开素材，路径 = " << path;
		ASSERT_EQ(raw.size(), static_cast<size_t>(e.bytes)) << e.file << " 字节数与冻结值不符";
		ASSERT_EQ(clv::core::Crc32(raw.data(), raw.size()), e.crc32_mpeg2) << e.file << " 整文件 CRC 与冻结值不符";

		CLV_ContainerError err = CLV_CONTAINER_OK;
		CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path.c_str(), &err);
		ASSERT_NE(r, nullptr) << e.file << " 打开失败：" << CLV_ContainerStrError(err);

		// ---- 文件头 ----
		CLV_FileHeadC head;
		ASSERT_EQ(CLV_ContainerReaderGetHead(r, &head), CLV_CONTAINER_OK);
		EXPECT_EQ(head.version_major, 2);
		EXPECT_EQ(head.version_minor, 0);
		EXPECT_EQ(head.stream_count, e.stream_count);
		EXPECT_NE(head.flags & 0x01u, 0u) << "三份素材都该带索引";
		EXPECT_EQ(head.duration_ticks, e.duration_ticks);
		EXPECT_EQ(head.total_packets, e.total_packets);
		EXPECT_EQ(head.index_offset, e.index_offset);
		ASSERT_LT(head.index_offset, raw.size());

		// ---- 流描述符 ----
		uint32_t scount = 0;
		ASSERT_EQ(CLV_ContainerReaderStreamCount(r, &scount), CLV_CONTAINER_OK);
		ASSERT_EQ(scount, static_cast<uint32_t>(e.stream_count));
		for (size_t i = 0; i < e.streams.size(); ++i)
		{
			CLV_StreamDescC d;
			uint8_t usable = 0;
			ASSERT_EQ(CLV_ContainerReaderGetStream(r, static_cast<uint32_t>(i), &d, &usable), CLV_CONTAINER_OK);
			const ExpectStream& x = e.streams[i];
			EXPECT_EQ(d.stream_id, x.id);
			EXPECT_EQ(d.stream_type, x.type);
			EXPECT_EQ(d.codec_id, x.codec);
			EXPECT_EQ(d.codec_version, 1);
			EXPECT_EQ(d.timebase_num, x.tb_num);
			EXPECT_EQ(d.timebase_den, x.tb_den);
			EXPECT_EQ(d.max_packet_size, x.max_packet);
			EXPECT_EQ(d.first_dts, 0ull) << "非分段文件强制 first_dts = 0";
			EXPECT_EQ(d.bit_depth, x.bit_depth);
			EXPECT_EQ(usable, 1);
		}

		// ---- 扩展块：条数与类型逐条对平，载荷逐字节 ----
		uint32_t ext_count = 0;
		ASSERT_EQ(CLV_ContainerReaderExtBlockCount(r, &ext_count), CLV_CONTAINER_OK);
		ASSERT_EQ(ext_count, e.ext_count) << e.file;
		ASSERT_EQ(e.exts.size(), static_cast<size_t>(e.ext_count)) << e.file << " 期望表条数与块数不符";
		for (size_t i = 0; i < e.exts.size(); ++i)
		{
			CLV_ExtBlockC b;
			ASSERT_EQ(CLV_ContainerReaderGetExtBlock(r, static_cast<uint32_t>(i), &b), CLV_CONTAINER_OK) << e.file;
			const ExpectExt& x = e.exts[i];
			EXPECT_EQ(b.stream_id, x.stream_id) << e.file << " 块 " << i << " 挂的流号不符";
			EXPECT_EQ(b.ext_type, x.type) << e.file << " 块 " << i << " 类型不符";
			EXPECT_EQ(b.ext_version, 1) << e.file << " 块 " << i << " 版本不符";
			ASSERT_EQ(b.size, static_cast<size_t>(x.size)) << e.file << " 块 " << i << " 载荷长不符";
			EXPECT_EQ(std::string(reinterpret_cast<const char*>(b.data), b.size), x.payload)
				<< e.file << " 块 " << i << " 载荷字节不符";
		}

		// ---- 索引 ----
		uint8_t has_index = 0;
		ASSERT_EQ(CLV_ContainerReaderHasIndex(r, &has_index), CLV_CONTAINER_OK);
		EXPECT_EQ(has_index, 1);
		uint32_t entries = 0;
		ASSERT_EQ(CLV_ContainerReaderIndexCount(r, &entries), CLV_CONTAINER_OK);
		ASSERT_EQ(entries, static_cast<uint32_t>(e.index_entries));
		uint64_t prev_ticks[4] = { 0, 0, 0, 0 };
		/* 索引区的排序键是统一时间轴：整表按 (dts, stream_id) 非降。
		 * 只查「同一条流不回退」的话，两条流互相排错、dts 相等时 stream_id 倒挂，这里照样全绿。 */
		std::vector<std::pair<uint8_t, uint64_t>> index_order;
		bool has_prev = false;
		uint64_t prev_dts = 0;
		uint8_t prev_stream = 0;
		for (uint32_t i = 0; i < entries; ++i)
		{
			CLV_IndexEntryC en;
			ASSERT_EQ(CLV_ContainerReaderGetIndexEntry(r, i, &en), CLV_CONTAINER_OK);
			EXPECT_LT(en.file_offset, head.index_offset) << "索引条目不能指到索引区里";
			ASSERT_LT(en.stream_id, 4u);
			// 同一条流的 dts 在索引里必须不减（统一时间轴排序后，同流的相对序不变）
			EXPECT_GE(en.dts, prev_ticks[en.stream_id])
				<< "流" << static_cast<int>(en.stream_id) << " 的索引 dts 出现回退";
			prev_ticks[en.stream_id] = en.dts;
			if (has_prev)
			{
				const bool not_before = en.dts > prev_dts || (en.dts == prev_dts && en.stream_id >= prev_stream);
				EXPECT_TRUE(not_before) << "索引第 " << i
										<< " 条排在上一条之前：统一时间轴 (dts, stream_id) 次序不成立";
			}
			has_prev = true;
			prev_dts = en.dts;
			prev_stream = en.stream_id;
			index_order.emplace_back(en.stream_id, en.dts);
		}
		EXPECT_EQ(index_order.size(), static_cast<size_t>(e.index_entries));

		// ---- 全程取帧（统计要在这之后取）----
		std::vector<ExpectFrame> got;
		uint64_t complete_frames = 0;
		for (;;)
		{
			CLV_FrameC fr;
			uint8_t got_frame = 0;
			ASSERT_EQ(CLV_ContainerReaderNextFrame(r, &fr, &got_frame), CLV_CONTAINER_OK);
			if (! got_frame) break;
			got.push_back(ExpectFrame { fr.stream_id, fr.payload_size, fr.fragments_received, fr.is_keyframe });
			EXPECT_EQ(fr.complete, 1) << "素材里的帧都该是完整的";
			if (fr.complete) ++complete_frames;
			if (fr.fragments_expected != 0) EXPECT_EQ(fr.fragments_received, fr.fragments_expected);
			// 载荷不得超过该流描述符声明的 max_packet_size
			uint32_t limit = 0;
			for (const ExpectStream& x: e.streams)
				if (x.id == fr.stream_id) limit = x.max_packet;
			ASSERT_GT(limit, 0u) << "出现了描述符里没有的 stream_id";
			EXPECT_LE(fr.payload_size, static_cast<size_t>(limit))
				<< "流" << static_cast<int>(fr.stream_id) << " 载荷越出其 max_packet_size";
			ASSERT_NE(fr.payload, nullptr);
			// 载荷即文件里的字节，不在此逐字节比对；此处断言长度非零
			EXPECT_GT(fr.payload_size, 0u);
		}
		ASSERT_EQ(got.size(), e.frames.size()) << e.file << " 帧数不符";
		/* 按位比，不排序：期望表就是按写出序登记的，读回序本身进断言
		 * （件头与素材说明的口径 = 时间轴序、dts 相等时按 stream_id 判先后）。 */
		for (size_t i = 0; i < e.frames.size(); ++i)
		{
			const ExpectFrame& w = e.frames[i];
			const ExpectFrame& g = got[i];
			EXPECT_EQ(g.stream_id, w.stream_id) << e.file << " 第 " << i << " 帧的读回序或流号不符";
			EXPECT_EQ(g.payload_size, w.payload_size) << e.file << " 第 " << i << " 帧载荷不符";
			EXPECT_EQ(g.fragments, w.fragments) << e.file << " 第 " << i << " 帧片数不符";
			EXPECT_EQ(g.is_keyframe, w.is_keyframe) << e.file << " 第 " << i << " 帧关键帧位不符";
		}
		/* 读回序与索引序互证：两份都出自同一批包，条数与流号必须逐位对得上。
		 * 这一步不靠手填的期望表，拿文件自己的两个视图对撞，登记表写反了也会在这里红。 */
		if (got.size() == index_order.size())
		{
			for (size_t i = 0; i < got.size(); ++i)
			{
				EXPECT_EQ(got[i].stream_id, index_order[i].first)
					<< e.file << " 第 " << i << " 帧与索引条目的流号不同序";
			}
		}
		else
		{
			ADD_FAILURE() << e.file << " 帧数与索引条目数不等（" << got.size() << " vs " << index_order.size()
						  << "），两份视图失去互证条件";
		}

		// ---- 取完全程后的统计：packets_ok 与各丢弃 / 重同步计数 ----
		CLV_ReaderStatsC st;
		ASSERT_EQ(CLV_ContainerReaderStats(r, &st), CLV_CONTAINER_OK);
		EXPECT_EQ(st.packets_ok, e.total_packets) << "读回的包数必须等于文件头声明值";
		EXPECT_EQ(st.frames_ok, static_cast<uint64_t>(e.frames.size()));
		EXPECT_EQ(complete_frames, static_cast<uint64_t>(e.frames.size()));
		EXPECT_EQ(st.dropped_crc, 0u);
		EXPECT_EQ(st.dropped_field, 0u);
		EXPECT_EQ(st.dropped_too_large, 0u);
		EXPECT_EQ(st.dropped_unknown_stream, 0u);
		EXPECT_EQ(st.resync_count, 0u);
		EXPECT_EQ(st.resync_failed, 0u);
		EXPECT_EQ(st.fragment_gaps, 0u);
		EXPECT_EQ(st.missing_first_packets, 0u);
		EXPECT_EQ(st.duplicate_fragments, 0u);
		EXPECT_EQ(st.ext_blocks_bad, 0u);
		EXPECT_EQ(st.truncated, 0u);
		EXPECT_EQ(st.abandoned, 0u);

		CLV_ContainerReaderClose(r);
	}

	// 结构期望值就是这三份文件的事实；字节数与 CRC 两个数与 assets/README.md 的冻结表逐字对应

	const ExpectStream kVideoDesc = { 0, 0, 0, 1, 1081080000ull, 4096, 8 };
	const ExpectStream kSubDesc = { 1, 2, 0, 1, 1081080000ull, 1024, 0 };

	const std::string kStyle = "[Script Info]\nTitle: clv fixture\nScriptType: v4.00+\n";

	/* 每条视频流的 ext_type = 3 参数块（`docs/容器规范.md` 的扩展块一节，v2 起必选，缺块的流会被读方判不可用）。
	 * 8 字节载荷：max_b_frames 0 | quant_precision 7 | ext_tools 0 | reserved 0 |
	 * width 352 | height 288（各 u16 小端、非 0）。这三份素材的视频载荷是合成图案，
	 * 没有本用例自己在用的显示尺寸可沿，尺寸取一个合法非零值（352x288）—— 素材只验容器面。 */
	const std::string kVideoParams("\x00\x07\x00\x00\x60\x01\x20\x01", 8);
	const ExpectExt kVideoParamsExt = { 0, 3, 8, kVideoParams };

}	 // namespace

TEST(FixtureFiles, Minimal)
{
	ExpectFixture e;
	e.file = "minimal.clv";
	e.bytes = 377;
	e.crc32_mpeg2 = 0x17655092u;
	e.stream_count = 1;
	e.duration_ticks = 43243200ull;
	e.total_packets = 2;
	e.index_offset = 305;
	e.index_entries = 2;
	e.streams = { kVideoDesc };
	e.frames = {
		ExpectFrame { 0, 64, 1, 1 },
		 ExpectFrame { 0, 48, 1, 0 }
	};
	e.ext_count = 1;
	e.exts = { kVideoParamsExt };
	CheckFixture(e);
}

TEST(FixtureFiles, NegativeStartDts)
{
	ExpectFixture e;
	e.file = "neg_dts.clv";
	e.bytes = 1123;
	e.crc32_mpeg2 = 0xC07BC799u;
	e.stream_count = 1;
	e.duration_ticks = 43243280ull;
	e.total_packets = 4;
	e.index_offset = 1051;
	e.index_entries = 2;
	e.streams = { kVideoDesc };
	// 第二帧 700 B 切三片：写方平移后首帧 dts = 0（源 dts = -40）
	e.frames = {
		ExpectFrame { 0, 120, 1, 1 },
		  ExpectFrame { 0, 700, 3, 0 }
	};
	e.ext_count = 1;
	e.exts = { kVideoParamsExt };
	CheckFixture(e);
}

TEST(FixtureFiles, SubtitleExt4)
{
	ExpectFixture e;
	e.file = "subtitle_ext4.clv";
	e.bytes = 1776;
	e.crc32_mpeg2 = 0xD0AB5526u;
	e.stream_count = 2;
	e.duration_ticks = 43243240ull;
	e.total_packets = 7;
	e.index_offset = 1664;
	e.index_entries = 4;
	e.streams = { kVideoDesc, kSubDesc };
	/* 帧序按落盘的实际读出序登记，不是排序后的字典序：
	 * 写方按统一时间轴排包 ⇒ 读出是 (流0 dts 0) → (流1 dts 0) → (流1 dts 21 621 600) → (流0 dts 43 243 240)。
	 * 依据两处独立对平：`scripts/gen_fixtures.py` 里这条素材的四帧输入序，与 `clv_demo_container info` 读回的索引四条
	 * (dts, stream_id) = (0,0) (0,1) (21621600,1) (43243240,0)。 */
	e.frames = {
		ExpectFrame { 0, 1000, 4, 1 },
		   ExpectFrame { 1, 30,	  1, 0 },
			ExpectFrame { 1, 20,   1, 0 },
		ExpectFrame { 0, 200,  1, 0 }
	};
	// 两块：流 0 的 ext_type = 3 参数块在前，流 1 的 ext_type = 4 样式块紧随其后
	e.ext_count = 2;
	e.exts = {
		kVideoParamsExt, ExpectExt { 1, 4, kStyle.size(), kStyle }
	};
	CheckFixture(e);
}
