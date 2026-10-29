/* test_subtitle_text.cpp —— 文本层：整包 UTF-8 校验与 `[\` 转义解码。 */

#include "subtitle/core.h"

#include "gtest/gtest.h"
#include <cstring>
#include <string>
#include <vector>

using clv::subtitle::DecodeText;
using clv::subtitle::Utf8Valid;

namespace
{
	std::string Decode(const std::string& in, std::vector<CLV_SubtitleSpanC>* spans_out)
	{
		std::vector<uint8_t> buf(in.size() + 1u, 0);
		std::vector<CLV_SubtitleSpanC> spans(16);
		size_t ns = 0;
		const size_t n = DecodeText(reinterpret_cast<const uint8_t*>(in.data()), in.size(), buf.data(), buf.size(),
									spans.data(), spans.size(), &ns);
		spans_out->assign(spans.begin(),
						  spans.begin() + static_cast<std::ptrdiff_t>(ns < spans.size() ? ns : spans.size()));
		return std::string(reinterpret_cast<const char*>(buf.data()), n);
	}
}	 // namespace

TEST(SubtitleText, Utf8ValidationMatchesSpecBoundaries)
{
	const auto valid = [](const std::vector<uint8_t>& v)
	{ return Utf8Valid(v.empty() ? nullptr : v.data(), v.size()); };
	EXPECT_TRUE(valid({}));
	EXPECT_TRUE(valid({ 'a', 'b', 'c' }));
	EXPECT_TRUE(valid({ 0xC2u, 0x80u }));				   // U+0080：二字节下界
	EXPECT_TRUE(valid({ 0xE0u, 0xA0u, 0x80u }));		   // U+0800：三字节下界
	EXPECT_TRUE(valid({ 0xF0u, 0x90u, 0x80u, 0x80u }));	   // U+10000：四字节下界
	EXPECT_TRUE(valid({ 0xF4u, 0x8Fu, 0xBFu, 0xBFu }));	   // U+10FFFF：四字节上界

	EXPECT_FALSE(valid({ 0x80u }));							// 单字节续尾
	EXPECT_FALSE(valid({ 0xC0u, 0x80u }));					// overlong 首字节 0xC0
	EXPECT_FALSE(valid({ 0xC1u, 0xBFu }));					// overlong 首字节 0xC1
	EXPECT_FALSE(valid({ 0xE0u, 0x80u, 0x80u }));			// overlong 三字节
	EXPECT_FALSE(valid({ 0xEDu, 0xA0u, 0x80u }));			// 代理区 U+D800
	EXPECT_FALSE(valid({ 0xF5u, 0x80u, 0x80u, 0x80u }));	// 首字节越界
	EXPECT_FALSE(valid({ 0xE2u, 0x82u }));					// 截断
	EXPECT_FALSE(valid({ 0xE2u, 0x28u, 0xA1u }));			// 续尾字节不合法
	EXPECT_FALSE(Utf8Valid(nullptr, 3u));
}

TEST(SubtitleText, DecodesEscapesAndEmojiSpans)
{
	std::vector<CLV_SubtitleSpanC> spans;

	EXPECT_EQ(Decode("plain", &spans), "plain");
	EXPECT_TRUE(spans.empty());

	// 前缀 `[\` + 指令标记 `\` + 指令名
	EXPECT_EQ(Decode("a[\\\\n]b", &spans), "a\nb");
	EXPECT_TRUE(spans.empty());

	EXPECT_EQ(Decode("[\\\\]", &spans), "\\");
	EXPECT_TRUE(spans.empty());

	// 未知指令原样输出（连指令标记一起）
	EXPECT_EQ(Decode("x[\\\\foo bar]y", &spans), "x\\foo bary");

	// 无参指令被给了参数：按「其他」档原样输出
	EXPECT_EQ(Decode("[\\\\n x]", &spans), "\\n x");

	// 内容不以 `\` 开头：只输出内容，前缀与后缀被消费
	EXPECT_EQ(Decode("[\\hello]", &spans), "hello");

	// `[` 后不是 `\`：字面
	EXPECT_EQ(Decode("[hello]", &spans), "[hello]");

	// emoji 段：参数整段标成 EMOJI，偏移按解码后字节数
	EXPECT_EQ(Decode("ab[\\\\emoji \xF0\x9F\x98\x80]c", &spans), "ab\xF0\x9F\x98\x80"
																 "c");
	ASSERT_EQ(spans.size(), 1u);
	EXPECT_EQ(spans[0].kind, CLV_SUBTITLE_SPAN_EMOJI);
	EXPECT_EQ(spans[0].off, 2u);
	EXPECT_EQ(spans[0].len, 4u);

	// 空参数：不产出零长段
	EXPECT_EQ(Decode("[\\\\emoji ]", &spans), "");
	EXPECT_TRUE(spans.empty());

	// 无封闭 `]`：从 `[\` 起丢到本次输入末尾
	EXPECT_EQ(Decode("ab[\\\\n", &spans), "ab");
	EXPECT_EQ(Decode("ab[\\\\emoji \xF0\x9F\x98", &spans), "ab");

	// 无嵌套：内层 `[\` 只当普通字符，封闭符是第一个 `]`
	EXPECT_EQ(Decode("A[\\B[\\n]C", &spans), "AB[\\nC");
}

TEST(SubtitleText, DecodeTruncatesWithoutOverrun)
{
	const std::string in = "hello[\\\\emoji \xF0\x9F\x98\x80]";
	uint8_t buf[4] = { 0xAAu, 0xAAu, 0xAAu, 0xAAu };
	CLV_SubtitleSpanC spans[1];
	size_t ns = 99u;
	// 无限容量下产出 9 字节；写入被截到 4 字节，返回总数仍是 9
	const size_t n =
		DecodeText(reinterpret_cast<const uint8_t*>(in.data()), in.size(), buf, sizeof(buf), spans, 1, &ns);
	EXPECT_EQ(n, 9u);
	EXPECT_EQ(std::memcmp(buf, "hell", 4), 0);
	ASSERT_EQ(ns, 1u);
	EXPECT_EQ(spans[0].off, 5u);
	EXPECT_EQ(spans[0].len, 4u);

	// 缓冲与段数组都为空：只报总数
	EXPECT_EQ(DecodeText(reinterpret_cast<const uint8_t*>(in.data()), in.size(), nullptr, 0, nullptr, 0, &ns), 9u);
	EXPECT_EQ(ns, 1u);

	// 空指针入参
	EXPECT_EQ(DecodeText(nullptr, 5u, nullptr, 0, nullptr, 0, &ns), 0u);
	EXPECT_EQ(ns, 0u);
}
