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
	SUCCEED();
}

TEST(ClvLogger, StorageSwitchIsAnInertPlaceholder)
{
	const std::string path = "/tmp/clv_logger_placeholder_should_not_be_created.log";
	CLV_LogSetStorage(path.c_str(), 1024);
	CLV_LogSetStorage(nullptr, 0);
	LogEveryLevel();

	std::error_code ec;
	EXPECT_FALSE(std::filesystem::exists(path, ec)) << "占位实现不许真的建文件";
	SUCCEED();
}
