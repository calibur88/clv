/* test_subtitle_parser.cpp —— 文档语法、流级单调、tick 平移、覆盖链与公开门面。
 *
 * 前半吃实现层头（subtitle/parser.h），后半走公开 C ABI，确认门面只做转换、不另加口径。 */

#include "CLV_Subtitle.h"
#include "subtitle/parser.h"

#include "gtest/gtest.h"
#include <cstring>
#include <string>
#include <vector>

using clv::subtitle::Block;
using clv::subtitle::kOk;
using clv::subtitle::kTicksPerMs;
using clv::subtitle::Line;
using clv::subtitle::Parser;
using clv::subtitle::ParserBlockAt;
using clv::subtitle::ParserBlockCount;
using clv::subtitle::ParserCreate;
using clv::subtitle::ParserDestroy;
using clv::subtitle::ParserFeed;
using clv::subtitle::ParserLineAt;
using clv::subtitle::ParserLineCount;
using clv::subtitle::ParserReset;
using clv::subtitle::ParserSetStyleBlock;
using clv::subtitle::ParserStats;
using clv::subtitle::ResolveStyleChain;
using clv::subtitle::Stats;
using clv::subtitle::StyleBlock;
using clv::subtitle::StyleSourceGet;
using clv::subtitle::StyleValue;

namespace
{
	std::vector<uint8_t> Pkt(const std::string& text, int64_t start_ticks, bool align = false, uint8_t a = 0,
							 bool pos = false, int32_t x = 0, int32_t y = 0)
	{
		clv::subtitle::PacketHeader h {};
		h.start_ticks = start_ticks;
		h.end_ticks = start_ticks + 1;
		h.has_alignment = align ? 1u : 0u;
		h.alignment = a;
		h.has_position = pos ? 1u : 0u;
		h.position_x = x;
		h.position_y = y;
		h.flags = static_cast<uint8_t>((align ? 1u : 0u) | ((pos ? 1u : 0u) << 1));
		std::vector<uint8_t> buf(text.size() + 32u);
		size_t n = 0;
		EXPECT_EQ(clv::subtitle::EncodePacket(&h, reinterpret_cast<const uint8_t*>(text.data()), text.size(),
											  buf.data(), buf.size(), &n),
				  kOk);
		buf.resize(n);
		return buf;
	}

	std::vector<uint8_t> StyleBlockBytes(uint32_t flags, const char* fam)
	{
		StyleBlock sb {};
		sb.flags = flags;
		sb.font_size = 40u;
		sb.font_size_unit = CLV_SUBTITLE_UNIT_PCT;
		sb.font_color_rgba = 0x00332211u; /* R=0x11, G=0x22, B=0x33, A=0 */
		sb.stroke_color_rgba = 0x00DDCCBBu;
		sb.stroke_width = 2u;
		sb.font_family = reinterpret_cast<const uint8_t*>(fam);
		sb.font_family_size = static_cast<uint32_t>(std::strlen(fam));
		std::vector<uint8_t> buf(64u + sb.font_family_size);
		size_t n = 0;
		EXPECT_EQ(clv::subtitle::EncodeStyleBlock(&sb, buf.data(), buf.size(), &n), kOk);
		buf.resize(n);
		return buf;
	}
}	 // namespace

TEST(SubtitleParser, ParsesBlocksLinesAndTimes)
{
	Parser* p = ParserCreate(nullptr, 0u);
	ASSERT_NE(p, nullptr);
	const std::string doc = "<#1/\n"
							"time: 00:00:01.000 - 00:00:02.000\n"
							"font-size: 24px\n"
							"font-color: #FF0000\n"
							"/>\n"
							"#1 hello\n"
							"#1 world\n"
							"<#2/\n"
							"time: 00:00:03.000 - 00:00:04.000\n"
							"/>\n"
							"#2 [\\\\emoji \xF0\x9F\x98\x80]\n";
	const std::vector<uint8_t> pkt = Pkt(doc, 0);
	ASSERT_EQ(ParserFeed(p, pkt.data(), pkt.size(), 0), kOk);

	ASSERT_EQ(ParserBlockCount(p), 2u);
	const Block* b0 = ParserBlockAt(p, 0);
	ASSERT_NE(b0, nullptr);
	EXPECT_EQ(b0->id, 1u);
	EXPECT_EQ(b0->start_ticks, 1000 * kTicksPerMs);
	EXPECT_EQ(b0->end_ticks, 2000 * kTicksPerMs);
	EXPECT_EQ(b0->displayable, 1u);
	EXPECT_EQ(b0->reason_mask, 0u);
	EXPECT_EQ(b0->first_line, 0u);
	EXPECT_EQ(b0->line_count, 2u);
	EXPECT_EQ(b0->style.font_size, 24u);
	EXPECT_EQ(b0->style.font_size_unit, CLV_SUBTITLE_UNIT_PX);
	EXPECT_EQ(b0->style.font_color_rgba, 0xFF0000FFu); /* R 在低 8 位，A 缺省 FF */
	EXPECT_EQ(StyleSourceGet(b0->style_source, 2u), CLV_SUBTITLE_SRC_BLOCK_ATTR);

	ASSERT_EQ(ParserLineCount(p), 3u);
	const Line* l0 = ParserLineAt(p, 0);
	ASSERT_NE(l0, nullptr);
	EXPECT_EQ(l0->block_id, 1u);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(l0->text), l0->size), "hello");
	EXPECT_EQ(l0->span_count, 0u);
	EXPECT_EQ(l0->scope_ok, 1u);

	const Line* l2 = ParserLineAt(p, 2);
	ASSERT_NE(l2, nullptr);
	EXPECT_EQ(l2->block_id, 2u);
	ASSERT_EQ(l2->span_count, 1u);
	EXPECT_EQ(l2->spans[0].kind, CLV_SUBTITLE_SPAN_EMOJI);
	EXPECT_EQ(l2->spans[0].off, 0u);
	EXPECT_EQ(l2->spans[0].len, 4u);

	Stats st {};
	ParserStats(p, &st);
	EXPECT_EQ(st.packets_seen, 1u);
	EXPECT_EQ(st.blocks_total, 2u);
	EXPECT_EQ(st.lines_total, 3u);
	EXPECT_EQ(st.lines_ignored, 0u);
	EXPECT_EQ(st.unusable_reason_mask, 0u);

	ParserDestroy(p);
}

TEST(SubtitleParser, ResolvesSixTierChainAndSources)
{
	StyleValue built {};
	built.flags = CLV_SUBTITLE_STYLE_FONT_SIZE | CLV_SUBTITLE_STYLE_OPACITY;
	built.font_size = 40u;
	built.opacity = 1000u;

	StyleValue renderer {};
	renderer.flags = CLV_SUBTITLE_STYLE_FONT_SIZE | CLV_SUBTITLE_STYLE_LINE_HEIGHT;
	renderer.font_size = 50u;
	renderer.line_height = 1200u;

	StyleBlock blockst {};
	blockst.flags = CLV_SUBTITLE_STYLE_FONT_SIZE | CLV_SUBTITLE_STYLE_FONT_FAMILY;
	blockst.font_size = 60u;
	blockst.font_family = reinterpret_cast<const uint8_t*>("Arial");
	blockst.font_family_size = 5u;

	StyleValue parser {};
	parser.flags = CLV_SUBTITLE_STYLE_FONT_SIZE | CLV_SUBTITLE_STYLE_TEXT_ALIGN;
	parser.font_size = 70u;
	parser.text_align = CLV_SUBTITLE_ALIGN_LEFT;

	clv::subtitle::PacketHeader hdr {};
	hdr.has_alignment = 1u;
	hdr.alignment = CLV_SUBTITLE_ALIGN_CENTER;
	hdr.has_position = 1u;
	hdr.position_x = 10;
	hdr.position_y = 20;

	StyleValue st {};
	st.flags = CLV_SUBTITLE_STYLE_FONT_SIZE | CLV_SUBTITLE_STYLE_TEXT_ALIGN;
	st.font_size = 99u;
	st.text_align = CLV_SUBTITLE_ALIGN_RIGHT;
	uint64_t src = 0;
	ResolveStyleChain(&st, &src, &built, &renderer, &blockst, &parser, &hdr);

	EXPECT_EQ(st.font_size, 99u); /* 块属性不被任何低档改写 */
	EXPECT_EQ(StyleSourceGet(src, 2u), CLV_SUBTITLE_SRC_BLOCK_ATTR);
	EXPECT_EQ(st.text_align, CLV_SUBTITLE_ALIGN_RIGHT);
	EXPECT_EQ(StyleSourceGet(src, 5u), CLV_SUBTITLE_SRC_BLOCK_ATTR);
	EXPECT_EQ(st.font_family_size, 5u); /* 样式块载体有字族、无字重 */
	EXPECT_EQ(StyleSourceGet(src, 11u), CLV_SUBTITLE_SRC_STYLE_BLOCK);
	EXPECT_EQ(st.line_height, 1200u);
	EXPECT_EQ(StyleSourceGet(src, 7u), CLV_SUBTITLE_SRC_RENDERER);
	EXPECT_EQ(st.opacity, 1000u);
	EXPECT_EQ(StyleSourceGet(src, 8u), CLV_SUBTITLE_SRC_BUILT_IN);
	EXPECT_EQ(st.pos_x, 10);
	EXPECT_EQ(st.pos_y, 20);
	EXPECT_EQ(st.pos_unit, CLV_SUBTITLE_UNIT_PX);
	EXPECT_EQ(StyleSourceGet(src, 6u), CLV_SUBTITLE_SRC_PACKET);
	EXPECT_EQ(st.font_weight, 0u);
	EXPECT_EQ(StyleSourceGet(src, 3u), CLV_SUBTITLE_SRC_UNSET);

	/* 空链：每个字段都报 unset */
	StyleValue empty {};
	uint64_t src2 = 0;
	ResolveStyleChain(&empty, &src2, nullptr, nullptr, nullptr, nullptr, nullptr);
	EXPECT_EQ(empty.flags, 0u);
	for (uint32_t bit = 0u; bit < clv::subtitle::kStyleBitCount; ++bit)
		EXPECT_EQ(StyleSourceGet(src2, bit), CLV_SUBTITLE_SRC_UNSET) << "bit " << bit;
}

TEST(SubtitleParser, StreamMonotonicAndTimeEdges)
{
	Parser* p = ParserCreate(nullptr, 0u);
	ASSERT_NE(p, nullptr);

	// 首块
	std::vector<uint8_t> a = Pkt("<#5/\ntime: 00:00:01.000 - 00:00:02.000\n/>\n#5 a\n", 0);
	ASSERT_EQ(ParserFeed(p, a.data(), a.size(), 0), kOk);
	ASSERT_EQ(ParserBlockCount(p), 1u);

	// ID 回退：块进视图但语法忽略，行 scope_ok = 0
	std::vector<uint8_t> rollback = Pkt("<#3/\ntime: 00:00:05.000 - 00:00:06.000\n/>\n#3 b\n", 0);
	ASSERT_EQ(ParserFeed(p, rollback.data(), rollback.size(), 0), kOk);
	ASSERT_EQ(ParserBlockCount(p), 2u);
	const Block* b1 = ParserBlockAt(p, 1);
	ASSERT_NE(b1, nullptr);
	EXPECT_EQ(b1->id, 3u);
	EXPECT_EQ(b1->displayable, 0u);
	EXPECT_EQ(b1->reason_mask & CLV_SUBTITLE_BLOCK_REASON_SYNTAX_IGNORE, CLV_SUBTITLE_BLOCK_REASON_SYNTAX_IGNORE);
	EXPECT_EQ(ParserLineAt(p, 1)->scope_ok, 0u);

	// start 序违规
	std::vector<uint8_t> back = Pkt("<#6/\ntime: 00:00:00.500 - 00:00:01.500\n/>\n#6 c\n", 0);
	ASSERT_EQ(ParserFeed(p, back.data(), back.size(), 0), kOk);
	EXPECT_EQ(ParserBlockAt(p, 2)->reason_mask & CLV_SUBTITLE_BLOCK_REASON_SYNTAX_IGNORE,
			  CLV_SUBTITLE_BLOCK_REASON_SYNTAX_IGNORE);

	// 缺 time 与 end = start：时间不显示
	std::vector<uint8_t> no_time = Pkt("<#7/\n/>\n#7 d\n", 0);
	ASSERT_EQ(ParserFeed(p, no_time.data(), no_time.size(), 0), kOk);
	EXPECT_EQ(ParserBlockAt(p, 3)->reason_mask, CLV_SUBTITLE_BLOCK_REASON_TIME_HIDDEN);
	std::vector<uint8_t> zero_len = Pkt("<#8/\ntime: 00:00:09.000 - 00:00:09.000\n/>\n#8 e\n", 0);
	ASSERT_EQ(ParserFeed(p, zero_len.data(), zero_len.size(), 0), kOk);
	EXPECT_EQ(ParserBlockAt(p, 4)->reason_mask, CLV_SUBTITLE_BLOCK_REASON_TIME_HIDDEN);

	// 平移后为负：照常交出、不判错、不丢（块 [1s, 2s] 按 −1.5s 平移 ⇒ [−0.5s, +0.5s]）
	// 负 start 必然小于前一块的 last_start，先 Reset 取新基准，避开 start 序违规档
	ParserReset(p);
	std::vector<uint8_t> neg = Pkt("<#9/\ntime: 00:00:01.000 - 00:00:02.000\n/>\n#9 f\n", 0);
	ASSERT_EQ(ParserFeed(p, neg.data(), neg.size(), -1500 * kTicksPerMs), kOk);
	ASSERT_EQ(ParserBlockCount(p), 1u);
	const Block* bneg = ParserBlockAt(p, 0);
	ASSERT_NE(bneg, nullptr);
	EXPECT_EQ(bneg->start_ticks, -500 * kTicksPerMs);
	EXPECT_EQ(bneg->end_ticks, 500 * kTicksPerMs);
	EXPECT_EQ(bneg->displayable, 1u);
	EXPECT_EQ(bneg->reason_mask, 0u);

	// 平移越界：丢该块并计数（包头的 end 仍在域内，越界的只是块级 time）
	std::vector<uint8_t> over = Pkt("<#10/\ntime: 00:00:01.000 - 00:00:02.000\n/>\n#10 g\n", 0);
	ASSERT_EQ(ParserFeed(p, over.data(), over.size(), INT64_MAX - 1000), kOk);
	ASSERT_EQ(ParserBlockCount(p), 2u);
	EXPECT_EQ(ParserBlockAt(p, 1)->reason_mask & CLV_SUBTITLE_BLOCK_REASON_SYNTAX_IGNORE,
			  CLV_SUBTITLE_BLOCK_REASON_SYNTAX_IGNORE);

	// Reset 后首个块无条件当新基准
	ParserReset(p);
	EXPECT_EQ(ParserBlockCount(p), 0u);
	std::vector<uint8_t> fresh = Pkt("<#2/\ntime: 00:00:01.000 - 00:00:02.000\n/>\n#2 h\n", 0);
	ASSERT_EQ(ParserFeed(p, fresh.data(), fresh.size(), 0), kOk);
	ASSERT_EQ(ParserBlockCount(p), 1u);
	EXPECT_EQ(ParserBlockAt(p, 0)->displayable, 1u);

	Stats st {};
	ParserStats(p, &st);
	EXPECT_EQ(st.blocks_negative_tick, 1u);
	EXPECT_EQ(st.blocks_time_overflow, 1u);
	EXPECT_EQ(st.resets, 2u);
	EXPECT_EQ(st.blocks_unusable, 5u); /* 回退 / 序违规 / 缺 time / end=start / 平移越界 */
	ParserDestroy(p);
}

TEST(SubtitleParser, StyleBlockAppliesOnlyToLaterBlocks)
{
	Parser* p = ParserCreate(nullptr, 0u);
	ASSERT_NE(p, nullptr);

	std::vector<uint8_t> a = Pkt("<#1/\ntime: 00:00:01.000 - 00:00:02.000\n/>\n#1 a\n", 0);
	ASSERT_EQ(ParserFeed(p, a.data(), a.size(), 0), kOk);
	const Block* b0 = ParserBlockAt(p, 0);
	ASSERT_NE(b0, nullptr);
	EXPECT_EQ(b0->style.font_size, 0u);
	EXPECT_EQ(StyleSourceGet(b0->style_source, 2u), CLV_SUBTITLE_SRC_UNSET);

	// 设样式块只对此后闭合的块生效
	const std::vector<uint8_t> sb =
		StyleBlockBytes(CLV_SUBTITLE_STYLE_FONT_SIZE | CLV_SUBTITLE_STYLE_FONT_COLOR, "Arial");
	StyleBlock parsed {};
	ASSERT_EQ(clv::subtitle::ParseStyleBlock(sb.data(), sb.size(), &parsed), kOk);
	ParserSetStyleBlock(p, &parsed);

	std::vector<uint8_t> b = Pkt("<#2/\ntime: 00:00:03.000 - 00:00:04.000\n/>\n#2 b\n", 0);
	ASSERT_EQ(ParserFeed(p, b.data(), b.size(), 0), kOk);
	const Block* b1 = ParserBlockAt(p, 1);
	ASSERT_NE(b1, nullptr);
	EXPECT_EQ(b1->style.font_size, 40u);
	EXPECT_EQ(b1->style.font_size_unit, CLV_SUBTITLE_UNIT_PCT);
	EXPECT_EQ(StyleSourceGet(b1->style_source, 2u), CLV_SUBTITLE_SRC_STYLE_BLOCK);
	EXPECT_EQ(StyleSourceGet(b1->style_source, 0u), CLV_SUBTITLE_SRC_STYLE_BLOCK);

	// 老块不被回头改写
	EXPECT_EQ(ParserBlockAt(p, 0)->style.font_size, 0u);
	EXPECT_EQ(StyleSourceGet(ParserBlockAt(p, 0)->style_source, 2u), CLV_SUBTITLE_SRC_UNSET);
	// 老块的行文本仍可读
	const Line* l0 = ParserLineAt(p, 0);
	ASSERT_NE(l0, nullptr);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(l0->text), l0->size), "a");

	// 撤销后不再进链
	ParserSetStyleBlock(p, nullptr);
	std::vector<uint8_t> c = Pkt("<#3/\ntime: 00:00:05.000 - 00:00:06.000\n/>\n#3 c\n", 0);
	ASSERT_EQ(ParserFeed(p, c.data(), c.size(), 0), kOk);
	const Block* b2 = ParserBlockAt(p, 2);
	ASSERT_NE(b2, nullptr);
	EXPECT_EQ(b2->style.font_size, 0u);
	EXPECT_EQ(StyleSourceGet(b2->style_source, 2u), CLV_SUBTITLE_SRC_UNSET);

	ParserDestroy(p);
}

TEST(SubtitleParser, PublicAbiFeedsAndDescribes)
{
	CLV_SubtitleParserParamsC params {};
	params.struct_size = 0u;
	CLV_SubtitleError err = CLV_SUBTITLE_INVALID_ARGUMENT;
	CLV_SubtitleParser* p = CLV_SubtitleParserOpen(&params, &err);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(err, CLV_SUBTITLE_OK);

	const std::string doc = "<#1/\ntime: 00:00:01.000 - 00:00:02.000\n/>\n#1 ab\n";
	const std::vector<uint8_t> pkt = Pkt(doc, 0);
	ASSERT_EQ(CLV_SubtitleParserFeed(p, pkt.data(), pkt.size(), 0), CLV_SUBTITLE_OK);

	uint32_t count = 0;
	ASSERT_EQ(CLV_SubtitleParserBlockCount(p, &count), CLV_SUBTITLE_OK);
	ASSERT_EQ(count, 1u);
	CLV_SubtitleBlockC blk {};
	ASSERT_EQ(CLV_SubtitleParserBlockAt(p, 0, &blk), CLV_SUBTITLE_OK);
	EXPECT_EQ(blk.struct_size, sizeof(CLV_SubtitleBlockC));
	EXPECT_EQ(blk.id, 1u);
	EXPECT_EQ(blk.displayable, 1u);
	EXPECT_EQ(blk.style.struct_size, sizeof(CLV_SubtitleStyleC));

	ASSERT_EQ(CLV_SubtitleParserLineCount(p, &count), CLV_SUBTITLE_OK);
	ASSERT_EQ(count, 1u);
	CLV_SubtitleLineC line {};
	ASSERT_EQ(CLV_SubtitleParserLineAt(p, 0, &line), CLV_SUBTITLE_OK);
	EXPECT_EQ(line.struct_size, sizeof(CLV_SubtitleLineC));
	EXPECT_EQ(line.block_id, 1u);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(line.text), line.text_size), "ab");

	CLV_SubtitleStatsC st {};
	ASSERT_EQ(CLV_SubtitleParserStats(p, &st), CLV_SUBTITLE_OK);
	EXPECT_EQ(st.struct_size, sizeof(CLV_SubtitleStatsC));
	EXPECT_EQ(st.packets_seen, 1u);
	EXPECT_EQ(st.blocks_total, 1u);

	// 句柄与入参档
	EXPECT_EQ(CLV_SubtitleParserFeed(nullptr, pkt.data(), pkt.size(), 0), CLV_SUBTITLE_INVALID_HANDLE);
	EXPECT_EQ(CLV_SubtitleParserBlockAt(p, 5u, &blk), CLV_SUBTITLE_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_SubtitleParserBlockAt(p, 0u, nullptr), CLV_SUBTITLE_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_SubtitleParserFeed(p, nullptr, 0u, 0), CLV_SUBTITLE_INVALID_ARGUMENT);

	// 布局自报：非 0 又不等于本版本 sizeof 的对象一律拒
	CLV_SubtitleParserParamsC bad {};
	bad.struct_size = 1u;
	EXPECT_EQ(CLV_SubtitleParserOpen(&bad, &err), nullptr);
	EXPECT_EQ(err, CLV_SUBTITLE_INVALID_ARGUMENT);

	// 渲染器按构型分侧：未编入 ⇒ NULL + UNSUPPORTED；已编入 ⇒ 正常交出句柄
	CLV_SubtitleRendererParamsC rp {};
	rp.width = 320u;
	rp.height = 180u;
	err = CLV_SUBTITLE_OK;
	CLV_SubtitleRenderer* ren = CLV_SubtitleRendererOpen(&rp, &err);
#if CLV_SUBTITLE_RENDERER_ENABLED
	ASSERT_NE(ren, nullptr);
	EXPECT_EQ(err, CLV_SUBTITLE_OK);
	CLV_SubtitleRendererClose(ren);
#else
	EXPECT_EQ(ren, nullptr);
	EXPECT_EQ(err, CLV_SUBTITLE_UNSUPPORTED);
#endif

	// 坏包只丢包、句柄继续可用
	std::vector<uint8_t> badflags = pkt;
	badflags[20] = 0x04u;
	EXPECT_EQ(CLV_SubtitleParserFeed(p, badflags.data(), badflags.size(), 0), CLV_SUBTITLE_PACKET_HEADER_BAD);
	EXPECT_EQ(CLV_SubtitleParserFeed(p, pkt.data(), pkt.size(), 0), CLV_SUBTITLE_OK);

	CLV_SubtitleParserClose(p);
}
