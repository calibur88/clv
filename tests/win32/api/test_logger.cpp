#include "CLV_Logger.h"

#include "gtest/gtest.h"
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace
{

	// 日志没有错误通道（契约：日志失败不许影响宿主），因此可断言的是：
	// 任何合法或非法入参都不许崩、不许改变后续调用的可用性。
	// 后端由构建选项决定（spdlog 或 null），断言必须对两者同时成立。
	//
	// **本件的档位定性 = 冒烟档**：公开面既不产出可读取的输出内容，也不提供当前生效等级的查询
	// （契约见 `CLV_Logger.h` 的「过滤等级只丢不存」「当前生效等级不提供查询接口」两句），
	// 所以下面以 SUCCEED 收尾的用例**不作为判据**，只钉「调得动、不崩、后续照常可用」这一条；
	// 输出的内容、条数、顺序与被过滤数量都不在本件的断言范围。
	// 过滤效果与格式位不吞的可见验证走 `demo/README.md` 的日志等级人工项（须用 spdlog 后端构建）。

	void LogEveryLevel()
	{
		CLV_Log(CLV_LOG_DEBUG, "debug line");
		CLV_Log(CLV_LOG_INFO, "info line");
		CLV_Log(CLV_LOG_WARN, "warn line");
		CLV_Log(CLV_LOG_ERROR, "error line");
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

TEST(ClvLogger, StorageSwitchIsAnInertPlaceholder)
{
	// 平台无关判据：文件名取自系统临时目录（Windows 与 POSIX 各自给出自己的 temp 路径），
	// 占位实现对任何入参都不落盘（契约见 `CLV_Logger.h`），所以这个名字对应的文件在调用前后都必须不在位。
	std::error_code ec;
	const auto dir = std::filesystem::temp_directory_path(ec);
	ASSERT_FALSE(ec) << "临时目录取不到，本条前提不成立：" << ec.message();
	const std::string path = dir.generic_string() + "/clv_logger_placeholder_" +
							 std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".log";
	std::filesystem::remove(std::filesystem::u8path(path), ec);	   // 同名残留先清掉，保证回查量的是本次调用

	CLV_LogSetStorage(path.c_str(), 1024);
	CLV_LogSetStorage(nullptr, 0);
	LogEveryLevel();

	ec.clear();
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(path), ec)) << "占位实现不许真的建文件";
}
