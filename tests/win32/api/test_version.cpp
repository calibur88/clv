#include "CLV_Version.h"

#include "gtest/gtest.h"
#include <cstring>

TEST(ClvVersion, StringMatchesMacros)
{
	const char* text = CLV_VersionString();
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, CLV_VERSION_STRING);
}

TEST(ClvVersion, SegmentsMatchMacros)
{
	EXPECT_EQ(CLV_VersionMajor(), CLV_VERSION_MAJOR);
	EXPECT_EQ(CLV_VersionMinor(), CLV_VERSION_MINOR);
	EXPECT_EQ(CLV_VersionPatch(), CLV_VERSION_PATCH);
}

TEST(ClvVersion, VersionIsStableAcrossCalls)
{
	/* 依据注：本件可判的是契约里的两件事——返回的是库拥有的静态缓冲（不是每次现造），
	 * 以及三段数字与串同源。两者都要跨调用比对才量得到，单点取值恒真。 */
	const char* first = CLV_VersionString();
	ASSERT_NE(first, nullptr);
	char held[64];
	const size_t held_len = std::strlen(first);
	ASSERT_LT(held_len, sizeof(held));
	std::memcpy(held, first, held_len + 1u);	// 先存一份内容，后面那次调用若改了缓冲才看得出来

	const char* second = CLV_VersionString();
	ASSERT_NE(second, nullptr);
	EXPECT_EQ(first, second) << "版本串应当是静态缓冲，不该每次调用换一个地址";
	EXPECT_STREQ(second, CLV_VERSION_STRING);
	EXPECT_STREQ(held, CLV_VERSION_STRING) << "第一次取到的内容被后续调用改过";

	/* 依据注：串与三段数字的同源判定不借格式化函数（MSVC 下 snprintf 有告警风险），按形状逐点数：
	 * 三段数字由两个点分开、点完就是预发布分隔符 '-'，每一段的数值与对应 getter 相等。 */
	const char* v = CLV_VersionString();
	const int seg[3] = { CLV_VersionMajor(), CLV_VersionMinor(), CLV_VersionPatch() };
	for (int i = 0; i < 3; ++i)
	{
		size_t len = 0u;
		while (v[len] >= '0' && v[len] <= '9') ++len;
		EXPECT_GT(len, 0u) << "第 " << i << " 段没数到数字";
		int value = 0;
		for (size_t k = 0u; k < len; ++k) value = value * 10 + (v[k] - '0');
		EXPECT_EQ(value, seg[i]) << "第 " << i << " 段与 getter 不同源";
		v += len;
		if (i < 2)
		{
			EXPECT_EQ(*v, '.') << "第 " << i << " 段之后不是点";
			++v;
		}
	}
	/* 数字段之后只剩预发布标记：串的形状必须是「三段数字 + '-' + 标记」，不混进别的东西。 */
	EXPECT_EQ(*v, '-') << "三段之后该是预发布分隔符";
	EXPECT_STREQ(v + 1, "dev") << "预发布标记不是本仓那一个";
}

TEST(ClvVersion, ProjectVersionTrackIsIndependentFromDocumentVersion)
{
	// 工程版本与文档版本是两条独立轨道：文档版本只出现在设计文档与 README 的抬头声明里，不进代码。
	// 两轨可以取同一数值，但来源与变更时机各自独立，所以这里钉的是工程串自己的形状。
	// 这条刻意把工程号钉成字面量（与上面那条用宏的用例互补），抬版本号时这里三处一起改：串两处 + patch 一处。
	EXPECT_STREQ(CLV_VersionString(), "0.8.0-dev");
	EXPECT_STREQ(CLV_VERSION_STRING, "0.8.0-dev");
	EXPECT_EQ(CLV_VersionMajor(), 0);
	EXPECT_EQ(CLV_VersionMinor(), 8);
	EXPECT_EQ(CLV_VersionPatch(), 0);
	EXPECT_NE(std::strcmp(CLV_VersionString(), "0.8.0"), 0) << "工程串缺预发布标记，与裸文档号同形";
}
