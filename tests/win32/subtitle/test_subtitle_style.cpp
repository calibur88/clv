/* test_subtitle_style.cpp —— 全局样式块的定界、位与长度对脸、在场与采纳。
 *
 * 本件吃实现层头，不碰公开句柄：样式块的字节布局由解析器经 CLV_SubtitleParserSetStyleBlock
 * 对外体现，内部件只判它自己的码。 */

#include "subtitle/style.h"

#include "gtest/gtest.h"
#include <cstring>
#include <vector>

using clv::subtitle::EncodeStyleBlock;
using clv::subtitle::Err;
using clv::subtitle::kInvalidArgument;
using clv::subtitle::kMaxFontFamilyLen;
using clv::subtitle::kOk;
using clv::subtitle::kOutputTooSmall;
using clv::subtitle::kStyleBitFontColor;
using clv::subtitle::kStyleBitFontFamily;
using clv::subtitle::kStyleBitFontSize;
using clv::subtitle::kStyleBitStrokeColor;
using clv::subtitle::kStyleBitStrokeWidth;
using clv::subtitle::kStyleBlockAll;
using clv::subtitle::kStyleBlockBad;
using clv::subtitle::kStyleBlockFixedLen;
using clv::subtitle::ParseStyleBlock;
using clv::subtitle::StyleBlock;

namespace
{
	void Put16(std::vector<uint8_t>& b, size_t off, uint16_t v)
	{
		b[off] = static_cast<uint8_t>(v & 0xFFu);
		b[off + 1] = static_cast<uint8_t>(v >> 8);
	}

	std::vector<uint8_t> Base()
	{
		std::vector<uint8_t> b(kStyleBlockFixedLen, 0);
		Put16(b, 0, kStyleBlockFixedLen);
		b[2] = 1u;
		return b;
	}
}	 // namespace

TEST(SubtitleStyleBlock, ParsesAdoptedFieldsAndKeepsWireColorOrder)
{
	const char* fam = "Noto Sans CJK SC,Arial";
	const size_t fam_len = std::strlen(fam);
	std::vector<uint8_t> b(kStyleBlockFixedLen + fam_len, 0);
	Put16(b, 0, static_cast<uint16_t>(b.size()));
	b[2] = 1u;
	b[3] = 0x1Fu;
	Put16(b, 4, 40u);
	b[6] = 0u;
	b[8] = 0x11u;
	b[9] = 0x22u;
	b[10] = 0x33u;
	b[11] = 0x44u;
	b[12] = 0xAAu;
	b[13] = 0xBBu;
	b[14] = 0xCCu;
	b[15] = 0xDDu;
	Put16(b, 16, 3u);
	Put16(b, 18, static_cast<uint16_t>(fam_len));
	std::memcpy(b.data() + kStyleBlockFixedLen, fam, fam_len);

	StyleBlock sb {};
	ASSERT_EQ(ParseStyleBlock(b.data(), b.size(), &sb), kOk);
	EXPECT_EQ(sb.flags, kStyleBitFontFamily | kStyleBitFontSize | kStyleBitFontColor | kStyleBitStrokeColor |
							kStyleBitStrokeWidth);
	EXPECT_EQ(sb.font_size, 40u);
	EXPECT_EQ(sb.font_size_unit, 0u);
	// 线上字节序 R,G,B,A；公开面 R 落低 8 位
	EXPECT_EQ(sb.font_color_rgba, 0x44332211u);
	EXPECT_EQ(sb.stroke_color_rgba, 0xDDCCBBAAu);
	EXPECT_EQ(sb.stroke_width, 3u);
	ASSERT_EQ(sb.font_family_size, fam_len);
	EXPECT_EQ(std::memcmp(sb.font_family, fam, fam_len), 0);
}

TEST(SubtitleStyleBlock, RejectsStructuralDefects)
{
	StyleBlock sb {};

	// 定长头都不全
	std::vector<uint8_t> short_head = Base();
	EXPECT_EQ(ParseStyleBlock(short_head.data(), 10, &sb), kStyleBlockBad);

	// 位为 0 的字段照读不采纳：随机值不判错
	std::vector<uint8_t> ignored = Base();
	std::memset(ignored.data() + 4, 0xA5, 14u);
	ignored[6] = 0u;
	ignored[7] = 0u;
	ASSERT_EQ(ParseStyleBlock(ignored.data(), ignored.size(), &sb), kOk);
	EXPECT_EQ(sb.flags, 0u);
	EXPECT_EQ(sb.font_size, 0u);
	EXPECT_EQ(sb.font_color_rgba, 0u);

	// bit0 = 1 而 len = 0
	std::vector<uint8_t> fam_no_len = Base();
	fam_no_len[3] = 0x01u;
	Put16(fam_no_len, 18, 0u);
	EXPECT_EQ(ParseStyleBlock(fam_no_len.data(), fam_no_len.size(), &sb), kStyleBlockBad);

	// bit0 = 0 而 len > 0
	std::vector<uint8_t> len_no_bit = Base();
	Put16(len_no_bit, 18, 1u);
	EXPECT_EQ(ParseStyleBlock(len_no_bit.data(), len_no_bit.size(), &sb), kStyleBlockBad);

	// 长度域判：65 515 通过、65 516 拒
	std::vector<uint8_t> max_fam(kStyleBlockFixedLen + kMaxFontFamilyLen, static_cast<uint8_t>('A'));
	Put16(max_fam, 0, 65535u);
	max_fam[2] = 1u;
	max_fam[3] = 0x01u;
	max_fam[6] = 0u;
	max_fam[7] = 0u;
	Put16(max_fam, 18, static_cast<uint16_t>(kMaxFontFamilyLen));
	ASSERT_EQ(ParseStyleBlock(max_fam.data(), max_fam.size(), &sb), kOk);
	EXPECT_EQ(sb.font_family_size, kMaxFontFamilyLen);
	std::vector<uint8_t> over_fam = Base();
	over_fam[3] = 0x01u;
	Put16(over_fam, 18, static_cast<uint16_t>(kMaxFontFamilyLen + 1u));
	EXPECT_EQ(ParseStyleBlock(over_fam.data(), over_fam.size(), &sb), kStyleBlockBad);

	// 版本 / flags 高位 / 单位 / reserved / 尾随 / struct_size 下界
	std::vector<uint8_t> bad = Base();
	bad[2] = 2u;
	EXPECT_EQ(ParseStyleBlock(bad.data(), bad.size(), &sb), kStyleBlockBad);
	bad[2] = 1u;
	bad[3] = 0x20u;
	EXPECT_EQ(ParseStyleBlock(bad.data(), bad.size(), &sb), kStyleBlockBad);
	bad[3] = 0u;
	bad[6] = 2u;
	EXPECT_EQ(ParseStyleBlock(bad.data(), bad.size(), &sb), kStyleBlockBad);
	bad[6] = 0u;
	bad[7] = 1u;
	EXPECT_EQ(ParseStyleBlock(bad.data(), bad.size(), &sb), kStyleBlockBad);
	bad[7] = 0u;
	bad.push_back(0u);
	EXPECT_EQ(ParseStyleBlock(bad.data(), bad.size(), &sb), kStyleBlockBad);
	bad.pop_back();
	Put16(bad, 0, 19u);
	EXPECT_EQ(ParseStyleBlock(bad.data(), bad.size(), &sb), kStyleBlockBad);

	// 字族串非法 UTF-8
	std::vector<uint8_t> bad_utf8(kStyleBlockFixedLen + 1u, 0);
	Put16(bad_utf8, 0, static_cast<uint16_t>(bad_utf8.size()));
	bad_utf8[2] = 1u;
	bad_utf8[3] = 0x01u;
	Put16(bad_utf8, 18, 1u);
	bad_utf8[kStyleBlockFixedLen] = 0xFFu;
	EXPECT_EQ(ParseStyleBlock(bad_utf8.data(), bad_utf8.size(), &sb), kStyleBlockBad);

	// 空指针
	EXPECT_EQ(ParseStyleBlock(nullptr, 0, &sb), kInvalidArgument);
	EXPECT_EQ(ParseStyleBlock(fam_no_len.data(), fam_no_len.size(), nullptr), kInvalidArgument);
}

TEST(SubtitleStyleBlock, EncodesRoundTripAndReportsNeededLength)
{
	const char* fam = "Arial";
	const size_t fam_len = std::strlen(fam);
	StyleBlock in {};
	in.flags = kStyleBlockAll;
	in.font_size = 32u;
	in.font_size_unit = 1u;
	in.font_color_rgba = 0x01020304u;
	in.stroke_color_rgba = 0x0A0B0C0Du;
	in.stroke_width = 2u;
	in.font_family = reinterpret_cast<const uint8_t*>(fam);
	in.font_family_size = static_cast<uint32_t>(fam_len);

	size_t need = 0;
	uint8_t one = 0;
	ASSERT_EQ(EncodeStyleBlock(&in, &one, 0, &need), kOutputTooSmall);
	EXPECT_EQ(need, kStyleBlockFixedLen + fam_len);

	std::vector<uint8_t> buf(need, 0);
	ASSERT_EQ(EncodeStyleBlock(&in, buf.data(), buf.size(), &need), kOk);
	EXPECT_EQ(need, buf.size());
	EXPECT_EQ(buf[3], 0x1Fu);	  // 五位全采纳：线上按 bit0–bit4 排
	EXPECT_EQ(buf[8], 0x04u);	  // R 在字节 8
	EXPECT_EQ(buf[11], 0x01u);	  // A 在字节 11

	StyleBlock out {};
	ASSERT_EQ(ParseStyleBlock(buf.data(), buf.size(), &out), kOk);
	EXPECT_EQ(out.flags, in.flags);
	EXPECT_EQ(out.font_color_rgba, in.font_color_rgba);
	EXPECT_EQ(out.stroke_color_rgba, in.stroke_color_rgba);
	EXPECT_EQ(out.stroke_width, in.stroke_width);
	EXPECT_EQ(std::memcmp(out.font_family, fam, fam_len), 0);

	// 块属性才有的位不许进样式块；字族位为 1 而指针为空
	StyleBlock foreign = in;
	foreign.flags = kStyleBlockAll | CLV_SUBTITLE_STYLE_POSITION;
	EXPECT_EQ(EncodeStyleBlock(&foreign, buf.data(), buf.size(), &need), kStyleBlockBad);
	StyleBlock no_ptr = in;
	no_ptr.font_family = nullptr;
	EXPECT_EQ(EncodeStyleBlock(&no_ptr, buf.data(), buf.size(), &need), kInvalidArgument);

	// 未采纳的字段照偏移写出：位为 0 时值不参与往返比对，但字节仍写
	StyleBlock bare {};
	bare.flags = 0u;
	ASSERT_EQ(EncodeStyleBlock(&bare, buf.data(), buf.size(), &need), kOk);
	EXPECT_EQ(need, kStyleBlockFixedLen);
	EXPECT_EQ(buf[3], 0u);
}
