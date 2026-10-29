/* test_subtitle_render.cpp —— 渲染器端到端：布局 / 折行 / 滚动裁剪 / 颜色与描边 / 脏矩形。
 *
 * 走公开两面：解析器取块与行，渲染器 Layout → Render → DirtyRects。
 * 字体固定用仓库内 Noto Sans CJK SC。 */

#include "CLV_Subtitle.h"
#include "subtitle/packet.h"
#include "subtitle/parser.h"

#include "gtest/gtest.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using clv::subtitle::EncodePacket;
using clv::subtitle::kOk;
using clv::subtitle::kTicksPerMs;

namespace
{
	std::string CjkDir()
	{
#ifdef CLV_FONTS_DIR
		return std::string(CLV_FONTS_DIR) + "/NotoSansCJKsc";
#else
		return std::string("assets/fonts/NotoSansCJKsc");
#endif
	}

	std::vector<uint8_t> Pkt(const std::string& text, int64_t pts)
	{
		clv::subtitle::PacketHeader h {};
		h.start_ticks = pts;
		h.end_ticks = pts + 1;
		std::vector<uint8_t> buf(text.size() + 32u);
		size_t n = 0;
		EXPECT_EQ(
			EncodePacket(&h, reinterpret_cast<const uint8_t*>(text.data()), text.size(), buf.data(), buf.size(), &n),
			kOk);
		buf.resize(n);
		return buf;
	}

	struct View
	{
		std::vector<CLV_SubtitleBlockC> blocks;
		std::vector<CLV_SubtitleLineC> lines;
	};

	View Collect(CLV_SubtitleParser* p)
	{
		View v;
		uint32_t nb = 0;
		EXPECT_EQ(CLV_SubtitleParserBlockCount(p, &nb), CLV_SUBTITLE_OK);
		v.blocks.resize(nb);
		for (uint32_t i = 0; i < nb; ++i) EXPECT_EQ(CLV_SubtitleParserBlockAt(p, i, &v.blocks[i]), CLV_SUBTITLE_OK);
		uint32_t nl = 0;
		EXPECT_EQ(CLV_SubtitleParserLineCount(p, &nl), CLV_SUBTITLE_OK);
		v.lines.resize(nl);
		for (uint32_t i = 0; i < nl; ++i) EXPECT_EQ(CLV_SubtitleParserLineAt(p, i, &v.lines[i]), CLV_SUBTITLE_OK);
		return v;
	}

	uint32_t NonZeroPixels(const std::vector<uint8_t>& rgba)
	{
		uint32_t n = 0;
		for (size_t i = 3; i < rgba.size(); i += 4)
			if (rgba[i] != 0u) ++n;
		return n;
	}

	CLV_SubtitleRenderer* OpenRenderer(uint32_t w, uint32_t h)
	{
		CLV_SubtitleRendererParamsC rp {};
		rp.width = w;
		rp.height = h;
		CLV_SubtitleError err = CLV_SUBTITLE_INVALID_ARGUMENT;
		CLV_SubtitleRenderer* r = CLV_SubtitleRendererOpen(&rp, &err);
		EXPECT_EQ(err, CLV_SUBTITLE_OK);
		if (r != nullptr) EXPECT_EQ(CLV_SubtitleRendererSetFontDir(r, CjkDir().c_str()), CLV_SUBTITLE_OK);
		return r;
	}
}	 // namespace

TEST(SubtitleRender, DrawsBlockAndReportsDirtyRects)
{
#if ! CLV_SUBTITLE_RENDERER_ENABLED
	GTEST_SKIP() << "本构型未编入字幕渲染器";
#endif
	CLV_SubtitleParserParamsC pp {};
	CLV_SubtitleError err = CLV_SUBTITLE_INVALID_ARGUMENT;
	CLV_SubtitleParser* p = CLV_SubtitleParserOpen(&pp, &err);
	ASSERT_NE(p, nullptr);
	const std::string doc = "<#1/\n"
							"time: 00:00:01.000 - 00:00:02.000\n"
							"font-size: 32px\n"
							"font-color: #FFFFFF\n"
							"/>\n"
							"#1 \xE4\xBD\xA0\xE5\xA5\xBD\n";
	const std::vector<uint8_t> pkt = Pkt(doc, 0);
	ASSERT_EQ(CLV_SubtitleParserFeed(p, pkt.data(), pkt.size(), 0), CLV_SUBTITLE_OK);
	const View v = Collect(p);
	ASSERT_EQ(v.blocks.size(), 1u);
	ASSERT_EQ(v.lines.size(), 1u);

	CLV_SubtitleRenderer* r = OpenRenderer(320u, 180u);
	ASSERT_NE(r, nullptr);
	const int64_t t = 1500 * kTicksPerMs;
	ASSERT_EQ(CLV_SubtitleRendererLayout(r, &v.blocks[0], v.lines.data(), 1u, nullptr, t), CLV_SUBTITLE_OK);

	std::vector<uint8_t> rgba(320u * 180u * 4u, 0u);
	ASSERT_EQ(CLV_SubtitleRendererRender(r, rgba.data(), 320u * 4u, 320u, 180u), CLV_SUBTITLE_OK);
	EXPECT_GT(NonZeroPixels(rgba), 0u);

	uint32_t total = 0;
	ASSERT_EQ(CLV_SubtitleRendererDirtyRects(r, nullptr, 0u, &total), CLV_SUBTITLE_OK);
	EXPECT_GE(total, 1u);
	std::vector<CLV_SubtitleRectC> rects(total);
	ASSERT_EQ(CLV_SubtitleRendererDirtyRects(r, rects.data(), total, &total), CLV_SUBTITLE_OK);
	for (const CLV_SubtitleRectC& rc: rects)
	{
		EXPECT_GE(rc.x, 0);
		EXPECT_GE(rc.y, 0);
		EXPECT_LE(static_cast<int64_t>(rc.x) + rc.w, 320);
		EXPECT_LE(static_cast<int64_t>(rc.y) + rc.h, 180);
	}

	CLV_SubtitleRenderStatsC st {};
	ASSERT_EQ(CLV_SubtitleRendererReport(r, &st), CLV_SUBTITLE_OK);
	EXPECT_EQ(st.struct_size, sizeof(CLV_SubtitleRenderStatsC));
	EXPECT_GT(st.glyphs, 0u);
	EXPECT_EQ(st.missing_glyphs, 0u);

	// 时间窗外：不画、不出脏矩形
	ASSERT_EQ(CLV_SubtitleRendererLayout(r, &v.blocks[0], v.lines.data(), 1u, nullptr, 500 * kTicksPerMs),
			  CLV_SUBTITLE_OK);
	std::vector<uint8_t> empty(320u * 180u * 4u, 0u);
	ASSERT_EQ(CLV_SubtitleRendererRender(r, empty.data(), 320u * 4u, 320u, 180u), CLV_SUBTITLE_OK);
	EXPECT_EQ(NonZeroPixels(empty), 0u);
	ASSERT_EQ(CLV_SubtitleRendererDirtyRects(r, nullptr, 0u, &total), CLV_SUBTITLE_OK);
	EXPECT_EQ(total, 0u);

	// 同刻再排一次：候选面 / 覆盖档 / 整形三张缓存至少命中一次，命中数出得了口
	ASSERT_EQ(CLV_SubtitleRendererLayout(r, &v.blocks[0], v.lines.data(), 1u, nullptr, t), CLV_SUBTITLE_OK);
	ASSERT_EQ(CLV_SubtitleRendererRender(r, rgba.data(), 320u * 4u, 320u, 180u), CLV_SUBTITLE_OK);
	CLV_SubtitleRenderStatsC again {};
	ASSERT_EQ(CLV_SubtitleRendererReport(r, &again), CLV_SUBTITLE_OK);
	EXPECT_GT(again.cache_hits, 0u);
	EXPECT_GE(again.glyphs, st.glyphs);

	CLV_SubtitleRendererClose(r);
	CLV_SubtitleParserClose(p);
}

TEST(SubtitleRender, WrapsLongCjkLineIntoSeveralRows)
{
#if ! CLV_SUBTITLE_RENDERER_ENABLED
	GTEST_SKIP() << "本构型未编入字幕渲染器";
#endif
	CLV_SubtitleParserParamsC pp {};
	CLV_SubtitleError err = CLV_SUBTITLE_INVALID_ARGUMENT;
	CLV_SubtitleParser* p = CLV_SubtitleParserOpen(&pp, &err);
	ASSERT_NE(p, nullptr);
	const std::string doc =
		"<#1/\n"
		"time: 00:00:01.000 - 00:00:02.000\n"
		"font-size: 16px\n"
		"/>\n"
		"#1 \xE5\xAD\x97\xE5\xB9\x95\xE8\x87\xAA\xE5\x8A\xA8\xE6\x8A\x98\xE8\xA1\x8C\xE6\xB5\x8B\xE8\xAF\x95\n";
	const std::vector<uint8_t> pkt = Pkt(doc, 0);
	ASSERT_EQ(CLV_SubtitleParserFeed(p, pkt.data(), pkt.size(), 0), CLV_SUBTITLE_OK);
	const View v = Collect(p);
	ASSERT_EQ(v.blocks.size(), 1u);

	CLV_SubtitleRenderer* r = OpenRenderer(64u, 64u);
	ASSERT_NE(r, nullptr);
	ASSERT_EQ(CLV_SubtitleRendererLayout(r, &v.blocks[0], v.lines.data(), 1u, nullptr, 1500 * kTicksPerMs),
			  CLV_SUBTITLE_OK);
	std::vector<uint8_t> rgba(64u * 64u * 4u, 0u);
	ASSERT_EQ(CLV_SubtitleRendererRender(r, rgba.data(), 64u * 4u, 64u, 64u), CLV_SUBTITLE_OK);
	EXPECT_GT(NonZeroPixels(rgba), 0u);

	uint32_t total = 0;
	ASSERT_EQ(CLV_SubtitleRendererDirtyRects(r, nullptr, 0u, &total), CLV_SUBTITLE_OK);
	std::vector<CLV_SubtitleRectC> rects(total);
	ASSERT_EQ(CLV_SubtitleRendererDirtyRects(r, rects.data(), total, &total), CLV_SUBTITLE_OK);
	// 10 个汉字 × 16 px 在 64 px 宽里折成多行：脏矩形的 y 至少落在两条不同的行上
	std::vector<int32_t> rows;
	for (const CLV_SubtitleRectC& rc: rects)
		if (std::find(rows.begin(), rows.end(), rc.y) == rows.end()) rows.push_back(rc.y);
	EXPECT_GE(rows.size(), 2u);

	CLV_SubtitleRendererClose(r);
	CLV_SubtitleParserClose(p);
}

TEST(SubtitleRender, ScrollClipsToViewport)
{
#if ! CLV_SUBTITLE_RENDERER_ENABLED
	GTEST_SKIP() << "本构型未编入字幕渲染器";
#endif
	CLV_SubtitleParserParamsC pp {};
	CLV_SubtitleError err = CLV_SUBTITLE_INVALID_ARGUMENT;
	CLV_SubtitleParser* p = CLV_SubtitleParserOpen(&pp, &err);
	ASSERT_NE(p, nullptr);
	const std::string doc = "<#1/\n"
							"time: 00:00:01.000 - 00:00:03.000\n"
							"font-size: 16px\n"
							"scroll: top\n"
							"scroll-rect: 0% 0% 100% 50%\n"
							"/>\n"
							"#1 \xE6\xBB\x9A\xE5\x8A\xA8\xE6\x96\x87\xE5\xAD\x97\xE6\xB5\x8B\xE8\xAF\x95\n";
	const std::vector<uint8_t> pkt = Pkt(doc, 0);
	ASSERT_EQ(CLV_SubtitleParserFeed(p, pkt.data(), pkt.size(), 0), CLV_SUBTITLE_OK);
	const View v = Collect(p);
	ASSERT_EQ(v.blocks.size(), 1u);

	CLV_SubtitleRenderer* r = OpenRenderer(128u, 128u);
	ASSERT_NE(r, nullptr);

	// 起点：内容在视口下方，未进入 —— 不出墨
	ASSERT_EQ(CLV_SubtitleRendererLayout(r, &v.blocks[0], v.lines.data(), 1u, nullptr, 1000 * kTicksPerMs),
			  CLV_SUBTITLE_OK);
	std::vector<uint8_t> rgba(128u * 128u * 4u, 0u);
	ASSERT_EQ(CLV_SubtitleRendererRender(r, rgba.data(), 128u * 4u, 128u, 128u), CLV_SUBTITLE_OK);
	uint32_t total = 0;
	ASSERT_EQ(CLV_SubtitleRendererDirtyRects(r, nullptr, 0u, &total), CLV_SUBTITLE_OK);
	EXPECT_EQ(total, 0u);

	// 中段：墨只在视口（上半屏）内
	ASSERT_EQ(CLV_SubtitleRendererLayout(r, &v.blocks[0], v.lines.data(), 1u, nullptr, 2000 * kTicksPerMs),
			  CLV_SUBTITLE_OK);
	std::vector<uint8_t> mid(128u * 128u * 4u, 0u);
	ASSERT_EQ(CLV_SubtitleRendererRender(r, mid.data(), 128u * 4u, 128u, 128u), CLV_SUBTITLE_OK);
	EXPECT_GT(NonZeroPixels(mid), 0u);
	ASSERT_EQ(CLV_SubtitleRendererDirtyRects(r, nullptr, 0u, &total), CLV_SUBTITLE_OK);
	EXPECT_GE(total, 1u);
	std::vector<CLV_SubtitleRectC> rects(total);
	ASSERT_EQ(CLV_SubtitleRendererDirtyRects(r, rects.data(), total, &total), CLV_SUBTITLE_OK);
	for (const CLV_SubtitleRectC& rc: rects) EXPECT_LE(static_cast<int64_t>(rc.y) + rc.h, 64);

	CLV_SubtitleRendererClose(r);
	CLV_SubtitleParserClose(p);
}

TEST(SubtitleRender, ColorAndStrokeChangeInk)
{
#if ! CLV_SUBTITLE_RENDERER_ENABLED
	GTEST_SKIP() << "本构型未编入字幕渲染器";
#endif
	const std::string base = "<#1/\n"
							 "time: 00:00:01.000 - 00:00:02.000\n"
							 "font-size: 40px\n";
	const std::string doc_plain = base + "/>\n#1 \xE4\xBD\xA0\n";
	const std::string doc_stroke = base + "/>\n#1 \xE4\xBD\xA0\n";
	const std::string doc_red = base + "font-color: #FF0000\n/>\n#1 \xE4\xBD\xA0\n";

	// 文本属性侧没有描边（v0.1 的 text-stroke 按未知属性忽略）：描边走全局样式块
	std::vector<uint8_t> stroke_block(64u, 0u);
	size_t stroke_len = 0;
	{
		clv::subtitle::StyleBlock sb {};
		sb.flags = CLV_SUBTITLE_STYLE_STROKE_WIDTH;
		sb.stroke_width = 2u;
		ASSERT_EQ(clv::subtitle::EncodeStyleBlock(&sb, stroke_block.data(), stroke_block.size(), &stroke_len), kOk);
	}

	auto render_doc = [&](const std::string& doc, bool with_stroke, std::vector<uint8_t>* rgba, uint32_t* ink)
	{
		CLV_SubtitleParserParamsC pp {};
		CLV_SubtitleError err = CLV_SUBTITLE_INVALID_ARGUMENT;
		CLV_SubtitleParser* p = CLV_SubtitleParserOpen(&pp, &err);
		EXPECT_NE(p, nullptr);
		if (with_stroke)
			EXPECT_EQ(CLV_SubtitleParserSetStyleBlock(p, stroke_block.data(), stroke_len), CLV_SUBTITLE_OK);
		const std::vector<uint8_t> pkt = Pkt(doc, 0);
		EXPECT_EQ(CLV_SubtitleParserFeed(p, pkt.data(), pkt.size(), 0), CLV_SUBTITLE_OK);
		const View v = Collect(p);
		EXPECT_EQ(v.blocks.size(), 1u);
		CLV_SubtitleRenderer* r = OpenRenderer(160u, 120u);
		EXPECT_NE(r, nullptr);
		EXPECT_EQ(CLV_SubtitleRendererLayout(r, &v.blocks[0], v.lines.data(), 1u, nullptr, 1500 * kTicksPerMs),
				  CLV_SUBTITLE_OK);
		rgba->assign(160u * 120u * 4u, 0u);
		EXPECT_EQ(CLV_SubtitleRendererRender(r, rgba->data(), 160u * 4u, 160u, 120u), CLV_SUBTITLE_OK);
		*ink = NonZeroPixels(*rgba);
		CLV_SubtitleRendererClose(r);
		CLV_SubtitleParserClose(p);
	};

	std::vector<uint8_t> plain;
	std::vector<uint8_t> stroked;
	std::vector<uint8_t> red;
	uint32_t ink_plain = 0;
	uint32_t ink_stroked = 0;
	uint32_t ink_red = 0;
	render_doc(doc_plain, false, &plain, &ink_plain);
	render_doc(doc_stroke, true, &stroked, &ink_stroked);
	render_doc(doc_red, false, &red, &ink_red);

	EXPECT_GT(ink_plain, 0u);
	EXPECT_GT(ink_stroked, ink_plain) << "描边在字形外侧加墨，可见像素应变多";

	// 红字：不透明像素的 R 高、G/B 低（颜色按公开面 R 落低 8 位）
	uint32_t red_pixels = 0;
	for (size_t i = 0; i < red.size(); i += 4)
	{
		if (red[i + 3] < 200u) continue;
		EXPECT_GT(red[i + 0], 200u);
		EXPECT_LT(red[i + 1], 40u);
		EXPECT_LT(red[i + 2], 40u);
		++red_pixels;
	}
	EXPECT_GT(red_pixels, 0u);
}
