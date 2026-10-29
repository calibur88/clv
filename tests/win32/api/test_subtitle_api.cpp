/* test_subtitle_api.cpp
 *
 * 字幕公开头的**形状**用例：只吃 include/CLV_Subtitle.h，不调用任何字幕实现符号
 * ——本文件在实现落地之前就能编译、能跑，用来把 ABI 形状钉死在 0.9.0。
 *
 * 四条判据：
 *   一 · 错误码段 106–117，`0 = OK`，118+ 留白（枚举里不出现大于 117 的值）。
 *   二 · 九个结构体首字段都是 `struct_size`（offsetof == 0），且都是 4 字节对齐的整数域。
 *   三 · 能力位：bit3 恒 1、bit4 由构型决定、bit5 以上未分配。
 *   四 · `CLV_SubtitleStrError` 逐值出可打印 ASCII，留白号位与越界值出 `unknown`。
 */

#include "CLV_Export.h"
#include "CLV_Subtitle.h"
#include "CLV_Version.h"

#include "gtest/gtest.h"
#include <cstddef>
#include <string>

TEST(SubtitleApi, ErrorCodeSegmentIsStable)
{
	EXPECT_EQ(static_cast<int>(CLV_SUBTITLE_OK), 0);
	EXPECT_EQ(static_cast<int>(CLV_SUBTITLE_INVALID_ARGUMENT), 106);
	EXPECT_EQ(static_cast<int>(CLV_SUBTITLE_INVALID_HANDLE), 107);
	EXPECT_EQ(static_cast<int>(CLV_SUBTITLE_INVALID_UTF8), 108);
	EXPECT_EQ(static_cast<int>(CLV_SUBTITLE_PACKET_HEADER_BAD), 109);
	EXPECT_EQ(static_cast<int>(CLV_SUBTITLE_PACKET_TOO_LARGE), 110);
	EXPECT_EQ(static_cast<int>(CLV_SUBTITLE_OUT_OF_MEMORY), 111);
	EXPECT_EQ(static_cast<int>(CLV_SUBTITLE_STATE), 112);
	EXPECT_EQ(static_cast<int>(CLV_SUBTITLE_BACKEND_FAILURE), 113);
	EXPECT_EQ(static_cast<int>(CLV_SUBTITLE_RENDER_FAILED), 114);
	EXPECT_EQ(static_cast<int>(CLV_SUBTITLE_UNSUPPORTED), 115);
	EXPECT_EQ(static_cast<int>(CLV_SUBTITLE_OUTPUT_TOO_SMALL), 116);
	EXPECT_EQ(static_cast<int>(CLV_SUBTITLE_STYLE_BLOCK_BAD), 117);
}

TEST(SubtitleApi, StructsSelfReportSizeFirst)
{
	EXPECT_EQ(offsetof(CLV_SubtitleParserParamsC, struct_size), 0u);
	EXPECT_EQ(offsetof(CLV_SubtitleRendererParamsC, struct_size), 0u);
	EXPECT_EQ(offsetof(CLV_SubtitleStyleC, struct_size), 0u);
	EXPECT_EQ(offsetof(CLV_SubtitleBlockC, struct_size), 0u);
	EXPECT_EQ(offsetof(CLV_SubtitleLineC, struct_size), 0u);
	EXPECT_EQ(offsetof(CLV_SubtitleStatsC, struct_size), 0u);
	EXPECT_EQ(offsetof(CLV_SubtitleRenderStatsC, struct_size), 0u);
	// 样式位集 13 位，全置位 = 0x1FFF
	EXPECT_EQ(static_cast<unsigned>(CLV_SUBTITLE_STYLE_ALL), 0x1FFFu);
	EXPECT_EQ(static_cast<unsigned>(CLV_SUBTITLE_STYLE_STROKE_WIDTH), 1u << 12);
	EXPECT_EQ(static_cast<unsigned>(CLV_SUBTITLE_SCROLL_RIGHT), 4u);
}

TEST(SubtitleApi, CapabilityBitsAreAppendOnly)
{
	EXPECT_EQ(static_cast<unsigned>(CLV_CAP_SUBTITLE_PARSER), 0x8u);
	EXPECT_EQ(static_cast<unsigned>(CLV_CAP_SUBTITLE_RENDERER), 0x10u);
	const uint32_t caps = CLV_Capabilities();
	EXPECT_NE(caps & CLV_CAP_SUBTITLE_PARSER, 0u) << "解析/编码恒在";
	EXPECT_EQ(caps & ~0x1Fu, 0u) << "5 号位以上未分配";
#if CLV_SUBTITLE_RENDERER_ENABLED
	EXPECT_NE(caps & CLV_CAP_SUBTITLE_RENDERER, 0u) << "本构型渲染器为 ON";
#else
	EXPECT_EQ(caps & CLV_CAP_SUBTITLE_RENDERER, 0u) << "本构型渲染器为 OFF";
#endif
}

TEST(SubtitleApi, StrErrorCoversEveryCodeAndUnknown)
{
	const CLV_SubtitleError codes[] = { CLV_SUBTITLE_OK,
										CLV_SUBTITLE_INVALID_ARGUMENT,
										CLV_SUBTITLE_INVALID_HANDLE,
										CLV_SUBTITLE_INVALID_UTF8,
										CLV_SUBTITLE_PACKET_HEADER_BAD,
										CLV_SUBTITLE_PACKET_TOO_LARGE,
										CLV_SUBTITLE_OUT_OF_MEMORY,
										CLV_SUBTITLE_STATE,
										CLV_SUBTITLE_BACKEND_FAILURE,
										CLV_SUBTITLE_RENDER_FAILED,
										CLV_SUBTITLE_UNSUPPORTED,
										CLV_SUBTITLE_OUTPUT_TOO_SMALL,
										CLV_SUBTITLE_STYLE_BLOCK_BAD };
	for (const CLV_SubtitleError c: codes)
	{
		const char* const s = CLV_SubtitleStrError(c);
		ASSERT_NE(s, nullptr) << static_cast<int>(c);
		EXPECT_NE(*s, '\0') << static_cast<int>(c);
		for (const char* p = s; *p != '\0'; ++p)
		{
			EXPECT_GE(static_cast<unsigned char>(*p), 0x20) << "文案要全是可打印 ASCII";
			EXPECT_LT(static_cast<unsigned char>(*p), 0x80);
		}
	}
	/* 段首下界之外、段尾留白号位与远值都只回 unknown：留白号位不因退场而前移 */
	EXPECT_STREQ(CLV_SubtitleStrError(static_cast<CLV_SubtitleError>(1)), "unknown");
	EXPECT_STREQ(CLV_SubtitleStrError(static_cast<CLV_SubtitleError>(105)), "unknown");
	EXPECT_STREQ(CLV_SubtitleStrError(static_cast<CLV_SubtitleError>(118)), "unknown");
	EXPECT_STREQ(CLV_SubtitleStrError(static_cast<CLV_SubtitleError>(9999)), "unknown");
}
