// 自守卫：未启用 spdlog 后端时整文件编译为空单元（两套后端都进 target，靠这里筛）。
#if defined(CLV_LOGGER_USE_SPDLOG)

	#include "logger.h"

	#include "CLV_Logger.h"

	#include <memory>
	#include <spdlog/sinks/stdout_color_sinks.h>
	#include <spdlog/spdlog.h>
	#include <string_view>

namespace clv::backend::spdlog
{

	namespace
	{

		::spdlog::level::level_enum ToSpdlogLevel(int level)
		{
			switch (level)
			{
			case CLV_LOG_DEBUG: return ::spdlog::level::debug;
			case CLV_LOG_INFO: return ::spdlog::level::info;
			case CLV_LOG_WARN: return ::spdlog::level::warn;
			case CLV_LOG_ERROR: return ::spdlog::level::err;
			default: return ::spdlog::level::off;
			}
		}

	}	 // namespace

	// pImpl 本体：自持一个不注册到 spdlog 注册表的 logger 实例，宿主看不到 spdlog 类型。
	struct SpdlogLogger::Impl
	{
		std::shared_ptr<::spdlog::logger> logger;
	};

	SpdlogLogger::SpdlogLogger(): impl_(std::make_unique<Impl>())
	{
		auto sink = std::make_shared<::spdlog::sinks::stdout_color_sink_mt>();
		impl_->logger = std::make_shared<::spdlog::logger>(std::string("clv"), sink);

		// 契约规定的构造后默认过滤等级
		impl_->logger->set_level(::spdlog::level::info);
		// ::spdlog::flush_on 只作用于注册表里的 logger，自持实例必须走成员级
		impl_->logger->flush_on(::spdlog::level::warn);
	}

	SpdlogLogger::~SpdlogLogger()
	{
		if (impl_ && impl_->logger) impl_->logger->flush();
	}

	void SpdlogLogger::Log(int level, const char* msg)
	{
		if (msg == nullptr) return;

		const ::spdlog::level::level_enum spdlog_level = ToSpdlogLevel(level);
		if (spdlog_level == ::spdlog::level::off) return;

		// string_view 重载不经过格式化：消息里的 "{}" / "%" 都按字面量输出，不会被当占位符吃掉
		impl_->logger->log(spdlog_level, ::spdlog::string_view_t(msg));
	}

	void SpdlogLogger::SetLevel(int level)
	{
		const ::spdlog::level::level_enum spdlog_level = ToSpdlogLevel(level);
		if (spdlog_level == ::spdlog::level::off) return;	 // 越界 no-op，保持原等级

		impl_->logger->set_level(spdlog_level);
	}

}	 // namespace clv::backend::spdlog

#endif	  // CLV_LOGGER_USE_SPDLOG
