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

TEST(ClvVersion, VersionIsStableAcrossCalls) { EXPECT_EQ(std::strcmp(CLV_VersionString(), CLV_VersionString()), 0); }

TEST(ClvVersion, ProjectVersionTrackIsIndependentFromDocumentVersion)
{
	// 工程版本与文档版本是两条独立轨道：文档版本 0.2.0-dev 只出现在设计文档头部，不进气运代码。
	// 这条刻意把工程号钉成字面量（与上面那条用宏的用例互补），抬版本号时这里三处一起改：
	// 串两处 + patch 一处。
	EXPECT_STREQ(CLV_VersionString(), "0.0.2-dev");
	EXPECT_STREQ(CLV_VERSION_STRING, "0.0.2-dev");
	EXPECT_EQ(CLV_VersionMajor(), 0);
	EXPECT_EQ(CLV_VersionMinor(), 0);
	EXPECT_EQ(CLV_VersionPatch(), 2);
	EXPECT_NE(std::strcmp(CLV_VersionString(), "0.2.0"), 0) << "工程版本轨道被文档版本污染";
}
