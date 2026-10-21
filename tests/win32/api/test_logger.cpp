#include "CLV_Logger.h"

#include "gtest/gtest.h"
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

// 号位三件事（本面错误枚举的唯一机械证明，改任何一格都要这里先红）：
// ① 成功档与其余五面同零；② 失败码从 102 起且三格连续（段尾追加，不插中间）；
// ③ 末码停在 105 —— 日志面的段尾就是这一格，字幕面从它的下一格起，本面不许再往里塞。
static_assert(CLV_LOG_OK == 0, "六套错误枚举的成功档必须同零");
static_assert(CLV_LOG_INVALID_PATH == 102 && CLV_LOG_UNSUPPORTED_BACKEND == 103 && CLV_LOG_OPEN_FAILED == 104,
			  "日志面失败码段：102 起、紧邻节拍面末码 101、三格连续");
static_assert(CLV_LOG_INVALID_ARGUMENT == 105, "日志面末码 = 105；字幕面从 106 起");

namespace
{

	// 日志的投递口没有错误通道（契约：日志失败不许影响宿主），因此可断言的是：
	// 任何合法或非法入参都不许崩、不许改变后续调用的可用性。
	// 落盘口是本面唯一有回报的入口，判据按构型分侧（见下面那条 #if 的注）。
	//
	// **本件的投递侧定性 = 冒烟档**：投递口不回报，当前生效等级在本件也不可观测（公开面上还没有等级查询口），
	// 所以下面以 SUCCEED 收尾的用例**不作为判据**，只钉「调得动、不崩、后续照常可用」这一条；
	// 投递的内容、条数、顺序与被过滤数量都不在本件的断言范围。
	// 落盘侧不属这一档：它有唯一一条有回报的入口，产物又是一份可回读的文件，所以下面 SetStorageEx 那几条都是实判。
	// 过滤效果与格式位不吞的可见验证走 `demo/README.md` 的日志等级人工项（须用 spdlog 后端构建）。

	void LogEveryLevel()
	{
		CLV_Log(CLV_LOG_DEBUG, "debug line");
		CLV_Log(CLV_LOG_INFO, "info line");
		CLV_Log(CLV_LOG_WARN, "warn line");
		CLV_Log(CLV_LOG_ERROR, "error line");
	}

	// 临时目录里的日志件名：平台无关取法，且用 generic_string() 保证 '/' 分隔符（路径契约不收反斜杠）。
	std::string TempLogPath(const char* tag)
	{
		std::error_code ec;
		const auto dir = std::filesystem::temp_directory_path(ec);
		EXPECT_FALSE(ec) << "临时目录取不到，本条前提不成立：" << ec.message();
		return dir.generic_string() + "/clv_logger_" + tag + "_" +
			   std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".log";
	}

	// 读回整份文件；读不到返回 false。用于判「落没落」与「内容在不在」。
	bool ReadWholeFile(const std::string& path, std::string& out)
	{
		std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
		if (! in) return false;
		out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		return true;
	}

}	 // namespace

TEST(ClvLogger, EveryLevelIsAccepted)
{
	LogEveryLevel();
	SUCCEED();
}

TEST(ClvLogger, FormatPlaceholderCharactersAreNotConsumed)
{
	// 后端若把消息当格式串（fmt 的 "{}" / printf 的 "%s"），这里会越界读参数或直接抛异常。
	// 契约规定消息按 UTF-8 字节串直投，因此两种后端下都必须安全通过。
	CLV_Log(CLV_LOG_ERROR, "literal {} {} {} and %s %d stay as text");
	CLV_Log(CLV_LOG_WARN, "{}");
	SUCCEED();
}

TEST(ClvLogger, NullMessageIsDroppedSilently)
{
	CLV_Log(CLV_LOG_INFO, nullptr);
	CLV_Log(CLV_LOG_ERROR, "");
	SUCCEED();
}

TEST(ClvLogger, OutOfRangeLevelIsDroppedAndKeepsLoggerUsable)
{
	CLV_Log(static_cast<CLV_LogLevel>(-1), "should be dropped");
	CLV_Log(static_cast<CLV_LogLevel>(99), "should be dropped");
	LogEveryLevel();	// 越界调用不许让后续调用失效
	SUCCEED();
}

TEST(ClvLogger, SetLevelAcceptsValidAndRejectsInvalidWithoutEffect)
{
	CLV_LogSetLevel(CLV_LOG_DEBUG);
	CLV_LogSetLevel(CLV_LOG_ERROR);
	CLV_LogSetLevel(static_cast<CLV_LogLevel>(42));	   // 契约：越界 no-op，保持原等级
	LogEveryLevel();
	SUCCEED();
}

TEST(ClvLogger, SetLevelBackToDefaultAfterUse)
{
	CLV_LogSetLevel(CLV_LOG_DEBUG);
	CLV_LogSetLevel(CLV_LOG_INFO);
	// 冒烟档也要真的走一次「后续可用」：设完等级之后照常投几条，不崩即守住了本件的契约。
	LogEveryLevel();
	SUCCEED();
}

TEST(ClvLogger, StrErrorCoversEveryCode)
{
	// 第六套错误枚举的文案面：段里每一格都要能被问到，且返非空 ASCII；未知值走 "unknown"。
	EXPECT_STREQ(CLV_LogStrError(CLV_LOG_OK), "OK");
	EXPECT_STREQ(CLV_LogStrError(CLV_LOG_INVALID_PATH), "invalid path");
	EXPECT_STREQ(CLV_LogStrError(CLV_LOG_UNSUPPORTED_BACKEND), "unsupported backend");
	EXPECT_STREQ(CLV_LogStrError(CLV_LOG_OPEN_FAILED), "open failed");
	EXPECT_STREQ(CLV_LogStrError(CLV_LOG_INVALID_ARGUMENT), "invalid argument");
	EXPECT_STREQ(CLV_LogStrError(static_cast<CLV_LogError>(101)), "unknown");
	EXPECT_STREQ(CLV_LogStrError(static_cast<CLV_LogError>(106)), "unknown");
	EXPECT_NE(CLV_LogStrError(static_cast<CLV_LogError>(-1)), nullptr);
}

TEST(ClvLogger, SetStorageExRejectsPathTierBeforeAnyBackend)
{
	// 路径档与文件面同一把尺，且判在后端之前 ⇒ 两把后端下结论一致，null 后端也不例外。
	const std::string path = TempLogPath("path_tier");
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(path), ec);

	EXPECT_EQ(CLV_LogSetStorageEx(nullptr, 64, 1), CLV_LOG_INVALID_PATH);
	EXPECT_EQ(CLV_LogSetStorageEx("", 64, 1), CLV_LOG_INVALID_PATH);
	const std::string backslash = path + "\\nested.log";	// 反斜杠一律不收，哪怕它指向的位置可写
	EXPECT_EQ(CLV_LogSetStorageEx(backslash.c_str(), 64, 1), CLV_LOG_INVALID_PATH);

	ec.clear();
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(path), ec)) << "参数不成立的那几次不许碰盘";
}

TEST(ClvLogger, SetStorageExRejectsArgumentTier)
{
	const std::string path = TempLogPath("arg_tier");
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(path), ec);

	// rotate_size_kb == 0 不解释成「无限」，判参数档
	EXPECT_EQ(CLV_LogSetStorageEx(path.c_str(), 0, 1), CLV_LOG_INVALID_ARGUMENT);
	// 乘 1024 要乘得下：越界的那几档同判参数档，绝不回绕成小值把上限抹掉
	EXPECT_EQ(CLV_LogSetStorageEx(path.c_str(), SIZE_MAX, 1), CLV_LOG_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_LogSetStorageEx(path.c_str(), SIZE_MAX / 512u, 1), CLV_LOG_INVALID_ARGUMENT);
	// 份数上限由本库自钉，不靠后端抛异常兜
	EXPECT_EQ(CLV_LogSetStorageEx(path.c_str(), 64, 200001), CLV_LOG_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_LogSetStorageEx(path.c_str(), 64, SIZE_MAX), CLV_LOG_INVALID_ARGUMENT);

	ec.clear();
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(path), ec)) << "参数档五次都不该装上一份落点";
}

TEST(ClvLogger, SetStorageExBackendTierFollowsBuildConfig)
{
	/* 这一条是全库唯一按后端选型分档的判据处（构型宏由 CMake 同时送给库目标与本测试目标，
	 * 测试里不重算一次）。spdlog 侧判「真落盘且消息读得回」，null 侧判「回 UNSUPPORTED_BACKEND 且不假装成功」。 */
	const std::string path = TempLogPath("backend_tier");
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(path), ec);

#if defined(CLV_LOGGER_USE_SPDLOG)
	const CLV_LogError rc = CLV_LogSetStorageEx(path.c_str(), 64, 1);
	ASSERT_EQ(rc, CLV_LOG_OK) << "spdlog 后端装不上落点，本条判据不成立";
	// flush_on(WARN) 保证这条一投就落，不用等析构
	CLV_Log(CLV_LOG_ERROR, "clv-marker-backend-tier");
	std::string body;
	ASSERT_TRUE(ReadWholeFile(path, body)) << "落盘件读不回，路径 = " << path;
	EXPECT_NE(body.find("clv-marker-backend-tier"), std::string::npos) << "文件里没有那条消息：" << body;
#else
	const CLV_LogError rc = CLV_LogSetStorageEx(path.c_str(), 64, 1);
	EXPECT_EQ(rc, CLV_LOG_UNSUPPORTED_BACKEND) << "空后端不许假装装上了落点";
	EXPECT_NE(CLV_LogStrError(rc), nullptr);
	ec.clear();
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(path), ec)) << "判成落不了盘的这一次不该留件";
#endif

	// 两档共同的那一条：换落点之后投递口照常可用，不因为落不了盘就坏掉
	LogEveryLevel();
	SUCCEED();
}

TEST(ClvLogger, SetStorageExReplacesPreviousTargetAndKeepsConsole)
{
	/* 「再次调用换落点，不叠加」。两档构型都注册这一条（构型换档不改 ctest 注册数，这是闸的既有口径），
	 * 所以 null 侧也判一件事：两次调用都回后端档、两份落点都不留件 —— 换不了落点也不能假装换过。 */
	const std::string first = TempLogPath("target_first");
	const std::string second = TempLogPath("target_second");
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(first), ec);
	std::filesystem::remove(std::filesystem::u8path(second), ec);

#if defined(CLV_LOGGER_USE_SPDLOG)
	ASSERT_EQ(CLV_LogSetStorageEx(first.c_str(), 64, 1), CLV_LOG_OK);
	CLV_Log(CLV_LOG_ERROR, "clv-marker-into-first");
	ASSERT_EQ(CLV_LogSetStorageEx(second.c_str(), 64, 1), CLV_LOG_OK);
	CLV_Log(CLV_LOG_ERROR, "clv-marker-into-second");

	std::string body_first, body_second;
	ASSERT_TRUE(ReadWholeFile(first, body_first));
	ASSERT_TRUE(ReadWholeFile(second, body_second));
	EXPECT_NE(body_first.find("clv-marker-into-first"), std::string::npos);
	EXPECT_EQ(body_first.find("clv-marker-into-second"), std::string::npos) << "换档没换干净，旧落点还在收";
	EXPECT_NE(body_second.find("clv-marker-into-second"), std::string::npos);
	EXPECT_EQ(body_second.find("clv-marker-into-first"), std::string::npos) << "新落点里不该有换档前的账";
#else
	EXPECT_EQ(CLV_LogSetStorageEx(first.c_str(), 64, 1), CLV_LOG_UNSUPPORTED_BACKEND);
	EXPECT_EQ(CLV_LogSetStorageEx(second.c_str(), 64, 1), CLV_LOG_UNSUPPORTED_BACKEND);
	ec.clear();
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(first), ec));
	ec.clear();
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(second), ec));
#endif
}

TEST(ClvLogger, ZeroMaxFilesMeansNoRotationAndAppend)
{
	/* max_files == 0 是一档合法取值（不轮转、一路追加），不是参数错误，也不是「关闭落盘」。
	 * null 侧同样判这一条：0 份数不能被判成参数不成立，只能回后端档。 */
	const std::string path = TempLogPath("no_rotate");
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(path), ec);

#if defined(CLV_LOGGER_USE_SPDLOG)
	ASSERT_EQ(CLV_LogSetStorageEx(path.c_str(), 64, 0), CLV_LOG_OK);
	CLV_Log(CLV_LOG_ERROR, "clv-marker-first-pass");
	// 同一个落点重设一次：basic 那档不 truncate，先前内容要留着
	ASSERT_EQ(CLV_LogSetStorageEx(path.c_str(), 64, 0), CLV_LOG_OK);
	CLV_Log(CLV_LOG_ERROR, "clv-marker-second-pass");

	std::string body;
	ASSERT_TRUE(ReadWholeFile(path, body));
	EXPECT_NE(body.find("clv-marker-first-pass"), std::string::npos) << "重设落点把已有内容清掉了";
	EXPECT_NE(body.find("clv-marker-second-pass"), std::string::npos);

	// 不轮转 ⇒ 盘上只有基础件一份。轮转件的命名法取自后端的 calc_filename：
	// "x.log" 的轮转件是 "x.1.log"，所以这里按同一形状拼出来再查它在不在。
	std::string rotated = path;
	const std::string::size_type dot = rotated.find_last_of('.');
	const std::string stem =
		(dot == std::string::npos || dot < rotated.find_last_of('/')) ? rotated : rotated.substr(0, dot);
	const std::string ext =
		(dot == std::string::npos || dot < rotated.find_last_of('/')) ? std::string() : rotated.substr(dot);
	rotated = stem + ".1" + ext;
	ec.clear();
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(rotated), ec))
		<< "max_files=0 不该产出轮转件：" << rotated;
#else
	EXPECT_EQ(CLV_LogSetStorageEx(path.c_str(), 64, 0), CLV_LOG_UNSUPPORTED_BACKEND)
		<< "0 份数是合法取值，不许判成参数不成立";
	EXPECT_NE(CLV_LogSetStorageEx(path.c_str(), 64, 0), CLV_LOG_INVALID_ARGUMENT);
	ec.clear();
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(path), ec));
#endif
}

TEST(ClvLogger, BackendNameMatchesStorageCapability)
{
	// 后端名永不为 NULL（契约），且与落盘口的回报分侧一致：叫 "spdlog" 的那把装得上，叫 "null" 的那把回 103。
	const char* name = CLV_LogBackendName();
	ASSERT_NE(name, nullptr);
	EXPECT_STREQ(name, strcmp(name, "spdlog") == 0 ? "spdlog" : "null") << "后端名只该是这两串之一，实测 = " << name;

	const std::string path = TempLogPath("name_probe");
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(path), ec);
	const CLV_LogError rc = CLV_LogSetStorageEx(path.c_str(), 64, 1);
	if (strcmp(name, "spdlog") == 0) EXPECT_EQ(rc, CLV_LOG_OK) << "名字说能落盘，装不上就是名字在撒谎";
	else EXPECT_EQ(rc, CLV_LOG_UNSUPPORTED_BACKEND) << "名字说落不了盘，却装上了";
}

TEST(ClvLogger, GetLevelTracksSetLevelOnlyWhereFilteringIsReal)
{
	/* 两把后端对「当前生效等级」的回答本来就不同（公开头写明：空后端不过滤，等级恒等于构造缺省）。
	 * 这里用 BackendName 在运行时判侧，不靠编译期宏 —— 同一条用例在两把构型里都跑、都有牙齿。 */
	const bool real = (strcmp(CLV_LogBackendName(), "spdlog") == 0);

	if (real)
	{
		CLV_LogSetLevel(CLV_LOG_DEBUG);
		EXPECT_EQ(CLV_LogGetLevel(), CLV_LOG_DEBUG);
		CLV_LogSetLevel(CLV_LOG_ERROR);
		EXPECT_EQ(CLV_LogGetLevel(), CLV_LOG_ERROR);
		// 越界 SetLevel 是 no-op：等级必须停在上一档，不许被改成越界值或复位
		CLV_LogSetLevel(static_cast<CLV_LogLevel>(42));
		EXPECT_EQ(CLV_LogGetLevel(), CLV_LOG_ERROR) << "越界那一次不许动等级";
	}
	else
	{
		CLV_LogSetLevel(CLV_LOG_DEBUG);
		EXPECT_EQ(CLV_LogGetLevel(), CLV_LOG_INFO) << "空后端不过滤，等级恒回构造缺省（契约写明的一条）";
		CLV_LogSetLevel(static_cast<CLV_LogLevel>(99));
		EXPECT_EQ(CLV_LogGetLevel(), CLV_LOG_INFO);
	}
	CLV_LogSetLevel(CLV_LOG_INFO);
	EXPECT_EQ(CLV_LogGetLevel(), CLV_LOG_INFO);
}

TEST(ClvLogger, OldStorageDoorIsReachableButReportsNothing)
{
	/* 老口保留：转调新口并把码丢掉，所以它在两档构型下都不可观测 —— 本条只钉「调得动、不崩、
	 * 后续照常」，不判成没成（「无回报」就是这只口的契约，见 CLV_Logger.h）。
	 * 判它真的转调到了新口，靠的是路径档那侧：老口给反斜杠路径时不该有任何崩溃或后续失效。 */
	const std::string path = TempLogPath("old_door");
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(path), ec);

	CLV_LogSetStorage(path.c_str(), 1024);
	CLV_LogSetStorage(nullptr, 0);	  // 老口没有码可判，越界入参只许被静默收下
	CLV_LogSetStorage("bad\\path.log", 1024);
	LogEveryLevel();

#if defined(CLV_LOGGER_USE_SPDLOG)
	std::string body;
	EXPECT_TRUE(ReadWholeFile(path, body)) << "老口已转调新口，spdlog 构型下应当真落盘";
#endif
	SUCCEED();
}
