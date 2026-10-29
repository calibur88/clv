/* test_subtitle_cal.cpp —— CAL 七接口：降级面确定性、真实文本栈、face 选择与系统解析。
 *
 * 真件用仓库内 assets/fonts（Noto Sans CJK SC / Noto Naskh Arabic / Noto Color Emoji），
 * 路径由 CLV_FONTS_DIR 给出；系统字体解析走本机 DirectWrite，取 "Segoe UI"。 */

#include "subtitle/cal/cal.h"

#include "gtest/gtest.h"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using clv::subtitle::cal::BidiRun;
using clv::subtitle::cal::Cal;
using clv::subtitle::cal::FaceInfo;
using clv::subtitle::cal::kGlyphRgba8;
using clv::subtitle::cal::kMetricFt;
using clv::subtitle::cal::kMetricOt;
using clv::subtitle::kOk;
using clv::subtitle::kUnsupported;

namespace
{
	std::string FontsDir()
	{
#ifdef CLV_FONTS_DIR
		return std::string(CLV_FONTS_DIR);
#else
		return std::string("assets/fonts");
#endif
	}

	std::string CjkFont() { return FontsDir() + "/NotoSansCJKsc/NotoSansCJKsc-Regular.otf"; }

	std::string ArabicFont()
	{
		return FontsDir() + "/NotoNaskhArabic-v2.021/NotoNaskhArabic/hinted/ttf/NotoNaskhArabic-Regular.ttf";
	}

	std::string EmojiFont() { return FontsDir() + "/noto-emoji/NotoColorEmoji.ttf"; }

	bool HasAnyNonZero(const uint8_t* p, size_t n)
	{
		for (size_t i = 0; i < n; ++i)
			if (p[i] != 0u) return true;
		return false;
	}
}	 // namespace

TEST(SubtitleCal, DegradedModeIsDeterministic)
{
	std::unique_ptr<Cal> cal(Cal::Create(true));
	ASSERT_NE(cal, nullptr);
	EXPECT_TRUE(cal->Degraded());

	const uint8_t good[] = { 'a', 0xC3u, 0xA9u };
	const uint8_t bad[] = { 0xE2u, 0x28u, 0xA1u };
	EXPECT_TRUE(cal->Utf8Valid(good, sizeof(good)));
	EXPECT_FALSE(cal->Utf8Valid(bad, sizeof(bad)));

	uint8_t nfc[8] = { 0 };
	size_t nfc_len = 0;
	ASSERT_EQ(cal->Nfc(good, sizeof(good), nfc, sizeof(nfc), &nfc_len), kOk);
	EXPECT_EQ(nfc_len, sizeof(good));
	EXPECT_EQ(std::memcmp(nfc, good, sizeof(good)), 0);

	int32_t base = -1;
	int32_t runs = -1;
	ASSERT_EQ(cal->Bidi(good, sizeof(good), &base, &runs), kOk);
	EXPECT_EQ(base, 0);
	EXPECT_EQ(runs, 1);
	BidiRun run {};
	size_t run_count = 0;
	ASSERT_EQ(cal->BidiRuns(good, sizeof(good), &run, 1, &run_count), kOk);
	ASSERT_EQ(run_count, 1u);
	EXPECT_EQ(run.off, 0u);
	EXPECT_EQ(run.len, sizeof(good));
	EXPECT_EQ(run.level, 0);

	/* 码位当字形、advance = 字号 */
	uint32_t glyphs[8] = { 0 };
	int32_t advances[8] = { 0 };
	size_t glyph_count = 0;
	int32_t rtl = -1;
	const uint8_t txt[] = { 'A', 'B' };
	ASSERT_EQ(cal->Shape(nullptr, 0u, txt, sizeof(txt), 32u, glyphs, advances, 8, &glyph_count, &rtl), kOk);
	ASSERT_EQ(glyph_count, 2u);
	EXPECT_EQ(glyphs[0], static_cast<uint32_t>('A'));
	EXPECT_EQ(advances[0], 32);
	EXPECT_EQ(rtl, 0);

	uint8_t* bmp = nullptr;
	uint32_t w = 0;
	uint32_t h = 0;
	int32_t left = 0;
	int32_t top = 0;
	ASSERT_EQ(cal->Raster(nullptr, 0u, 16u, 'A', &bmp, &w, &h, &left, &top), kOk);
	ASSERT_NE(bmp, nullptr);
	EXPECT_EQ(w, 2u);
	EXPECT_EQ(h, 2u);
	std::free(bmp);

	char path[64] = { 0 };
	size_t plen = 0;
	EXPECT_EQ(cal->ResolveSystemFont("Segoe UI", path, sizeof(path), &plen), kUnsupported);
}

TEST(SubtitleCal, RealTextStackNfcBidiShapeRaster)
{
	std::unique_ptr<Cal> cal(Cal::Create(false));
	ASSERT_NE(cal, nullptr);
	ASSERT_FALSE(cal->Degraded());

	/* NFC：e + U+0301 → é（C3 A9） */
	const uint8_t decomposed[] = { 'e', 0xCCu, 0x81u };
	uint8_t nfc[8] = { 0 };
	size_t nfc_len = 0;
	ASSERT_EQ(cal->Nfc(decomposed, sizeof(decomposed), nfc, sizeof(nfc), &nfc_len), kOk);
	ASSERT_EQ(nfc_len, 2u);
	EXPECT_EQ(nfc[0], 0xC3u);
	EXPECT_EQ(nfc[1], 0xA9u);

	/* 阿拉伯文段落基级为 1（RTL），整形取 RTL 且拿到字形 */
	const uint8_t arabic[] = { 0xD9u, 0x85u, 0xD8u, 0xB1u, 0xD8u, 0xADu, 0xD8u, 0xA8u, 0xD8u, 0xA7u };
	int32_t base = -1;
	int32_t run_count = -1;
	ASSERT_EQ(cal->Bidi(arabic, sizeof(arabic), &base, &run_count), kOk);
	EXPECT_EQ(base, 1);
	EXPECT_GE(run_count, 1);
	uint32_t glyphs[16] = { 0 };
	int32_t advances[16] = { 0 };
	size_t glyph_count = 0;
	int32_t rtl = -1;
	ASSERT_EQ(
		cal->Shape(ArabicFont().c_str(), 0u, arabic, sizeof(arabic), 32u, glyphs, advances, 16, &glyph_count, &rtl),
		kOk);
	EXPECT_GT(glyph_count, 0u);
	EXPECT_EQ(rtl, 1);

	/* cmap 覆盖：CJK 面覆盖 U+4F60，不覆盖 U+1F600（emoji 在另一张脸） */
	int32_t covers = -1;
	ASSERT_EQ(cal->FaceCovers(CjkFont().c_str(), 0u, 0x4F60u, &covers), kOk);
	EXPECT_EQ(covers, 1);
	ASSERT_EQ(cal->FaceCovers(CjkFont().c_str(), 0u, 0x1F600u, &covers), kOk);
	EXPECT_EQ(covers, 0);

	/* 单色字形给 A8 覆盖度 */
	uint8_t* bmp = nullptr;
	uint32_t w = 0;
	uint32_t h = 0;
	int32_t left = 0;
	int32_t top = 0;
	ASSERT_EQ(cal->Raster(CjkFont().c_str(), 0u, 32u, 0x4F60u, &bmp, &w, &h, &left, &top), kOk);
	ASSERT_NE(bmp, nullptr);
	EXPECT_GT(w, 0u);
	EXPECT_GT(h, 0u);
	std::free(bmp);

	/* 彩色字形给 RGBA8（NotoColorEmoji 只有原生 strike，按 32px 缩）；字形号从整形拿 */
	const uint8_t emoji_cp[] = { 0xF0u, 0x9Fu, 0x98u, 0x80u };
	size_t emoji_count = 0;
	int32_t emoji_rtl = -1;
	ASSERT_EQ(cal->Shape(EmojiFont().c_str(), 0u, emoji_cp, sizeof(emoji_cp), 32u, glyphs, advances, 16, &emoji_count,
						 &emoji_rtl),
			  kOk);
	ASSERT_EQ(emoji_count, 1u);
	int32_t format = -1;
	const clv::subtitle::Err emoji_rc =
		cal->RasterGlyphFmt(EmojiFont().c_str(), 0u, 32u, glyphs[0], &bmp, &w, &h, &left, &top, &format);
	ASSERT_EQ(emoji_rc, kOk) << "gid=" << glyphs[0];
	ASSERT_NE(bmp, nullptr);
	EXPECT_EQ(format, kGlyphRgba8);
	EXPECT_GT(w, 0u);
	EXPECT_GT(h, 0u);
	std::free(bmp);

	/* 描边宽 0：outline 整块为零、fill 有墨（字形号用「你」的，gid 1 可能是空格） */
	const uint8_t cjk_cp[] = { 0xE4u, 0xBDu, 0xA0u };
	size_t cjk_count = 0;
	int32_t cjk_rtl = -1;
	ASSERT_EQ(
		cal->Shape(CjkFont().c_str(), 0u, cjk_cp, sizeof(cjk_cp), 32u, glyphs, advances, 16, &cjk_count, &cjk_rtl),
		kOk);
	ASSERT_EQ(cjk_count, 1u);
	uint8_t* fill = nullptr;
	uint8_t* outline = nullptr;
	ASSERT_EQ(cal->RasterGlyphStroked(CjkFont().c_str(), 0u, 32u, glyphs[0], 0u, &fill, &outline, &w, &h, &left, &top),
			  kOk);
	ASSERT_NE(fill, nullptr);
	ASSERT_NE(outline, nullptr);
	EXPECT_TRUE(HasAnyNonZero(fill, static_cast<size_t>(w) * h));
	EXPECT_FALSE(HasAnyNonZero(outline, static_cast<size_t>(w) * h));
	std::free(fill);
	std::free(outline);

	/* 两条度量入口都给正 advance 与正 ascender */
	for (int kind: { kMetricOt, kMetricFt })
	{
		std::unique_ptr<clv::subtitle::cal::Metrics> m(cal->OpenMetrics(CjkFont().c_str(), 0u, 32u, kind));
		ASSERT_NE(m, nullptr) << "kind " << kind;
		EXPECT_EQ(m->Kind(), kind);
		int32_t adv = 0;
		ASSERT_EQ(m->Advance('A', &adv), kOk);
		EXPECT_GT(adv, 0);
		int32_t asc = 0;
		int32_t desc = 0;
		int32_t gap = 0;
		ASSERT_EQ(m->Extents(&asc, &desc, &gap), kOk);
		EXPECT_GT(asc, 0);
	}
}

TEST(SubtitleCal, FaceSelectionAndSystemResolve)
{
	std::unique_ptr<Cal> cal(Cal::Create(false));
	ASSERT_NE(cal, nullptr);

	FaceInfo faces[8];
	size_t count = 0;
	ASSERT_EQ(cal->EnumFileFaces(CjkFont().c_str(), faces, 8, &count), kOk);
	ASSERT_GE(count, 1u);
	EXPECT_EQ(faces[0].face_index, 0u);
	EXPECT_NE(std::strstr(faces[0].family, "Noto Sans CJK"), nullptr);

	FaceInfo picked {};
	char path[512] = { 0 };
	size_t plen = 0;
	const std::string dir = FontsDir() + "/NotoSansCJKsc";
	ASSERT_EQ(cal->SelectFace(dir.c_str(), "Noto Sans CJK SC", 700u, 0, &picked, path, sizeof(path), &plen), kOk);
	EXPECT_EQ(picked.weight, 700u);
	EXPECT_NE(std::strstr(path, "Bold"), nullptr);

	/* 这一族没有 italic：硬过滤后过筛集为空 ⇒ UNSUPPORTED，不拿正体顶替 */
	EXPECT_EQ(cal->SelectFace(dir.c_str(), "Noto Sans CJK SC", 400u, 1, &picked, path, sizeof(path), &plen),
			  kUnsupported);

	/* 系统侧：本机 DirectWrite 把 family 展开成文件，再按同一份规则选 */
	ASSERT_EQ(cal->ResolveSystemFont("Segoe UI", path, sizeof(path), &plen), kOk) << "本机缺 Segoe UI";
	EXPECT_GT(plen, 0u);
	ASSERT_EQ(cal->SelectSystemFace("Segoe UI", 400u, 0, &picked, path, sizeof(path), &plen), kOk);
	EXPECT_NE(std::strstr(picked.family, "Segoe UI"), nullptr);
}
