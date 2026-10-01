#include "CLV_Logger.h"
#include "backends/impl_logger.h"

#include <memory>

// 后端选择属业务配置（CMake option），与平台链路无关。
#if defined(CLV_LOGGER_USE_SPDLOG)
	#include "backends/spdlog/logger.h"
#else
	#include "backends/null/logger.h"
#endif

namespace clv::core
{

	std::unique_ptr<backend::Logger> MakeLogger()
	{
#if defined(CLV_LOGGER_USE_SPDLOG)
		return std::make_unique<::clv::backend::spdlog::SpdlogLogger>();
#else
		return std::make_unique<::clv::backend::null::NullLogger>();
#endif
	}

	namespace
	{

		// 函数内 static：初始化时机在使用点，避开跨翻译单元静态初始化顺序与退出期析构竞态。
		// C++11 起局部 static 的初始化是线程安全的。
		backend::Logger& Active()
		{
			static std::unique_ptr<backend::Logger> logger = MakeLogger();
			return *logger;
		}

		bool IsLevelValid(CLV_LogLevel level) noexcept
		{
			switch (level)
			{
			case CLV_LOG_DEBUG:
			case CLV_LOG_INFO:
			case CLV_LOG_WARN:
			case CLV_LOG_ERROR: return true;
			}
			return false;
		}

	}	 // namespace

}	 // namespace clv::core

extern "C" void CLV_Log(CLV_LogLevel level, const char* msg)
{
	if (! clv::core::IsLevelValid(level)) return;	 // 契约：越界等级整条丢弃
	clv::core::Active().Log(static_cast<int>(level), msg);
}

extern "C" void CLV_LogSetLevel(CLV_LogLevel level)
{
	if (! clv::core::IsLevelValid(level)) return;	 // 契约：越界 no-op，保持原等级
	clv::core::Active().SetLevel(static_cast<int>(level));
}

extern "C" void CLV_LogSetStorage(const char* filepath, size_t rotate_size_kb)
{
	// 接口占位：落盘后端待下一轮，调用无效果
	(void) filepath;
	(void) rotate_size_kb;
}
