// 自守卫：未启用 spdlog 后端时整文件编译为空单元（两套后端都进 target，靠这里筛）。
#if defined(CLV_LOGGER_USE_SPDLOG)

	#include "logger.h"

	#include "CLV_Logger.h"

	#include <algorithm>
	#include <memory>
	#include <spdlog/sinks/basic_file_sink.h>
	#include <spdlog/sinks/rotating_file_sink.h>
	#include <spdlog/sinks/stdout_color_sinks.h>
	#include <spdlog/spdlog.h>
	#include <string>
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

		// 反方向只回契约收的那四档。后端还可能停在 trace / critical / off —— 那些档本面从没设过
		// （SetLevel 的入参先判过域），真落到这里按构造缺省答，不自创等级值。
		int FromSpdlogLevel(::spdlog::level::level_enum level) noexcept
		{
			switch (level)
			{
			case ::spdlog::level::debug: return CLV_LOG_DEBUG;
			case ::spdlog::level::info: return CLV_LOG_INFO;
			case ::spdlog::level::warn: return CLV_LOG_WARN;
			case ::spdlog::level::err: return CLV_LOG_ERROR;
			default: return CLV_LOG_INFO;
			}
		}

	}	 // namespace

	// pImpl 本体：自持一个不注册到 spdlog 注册表的 logger 实例，宿主看不到 spdlog 类型。
	struct SpdlogLogger::Impl
	{
		std::shared_ptr<::spdlog::logger> logger;
		// 当前那份文件 sink：再次 SetStorage 换掉它，不叠加。控制台 sink 恒在，不归这里管。
		std::shared_ptr<::spdlog::sinks::sink> file_sink;
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

	int SpdlogLogger::GetLevel() const { return FromSpdlogLevel(impl_->logger->level()); }

	const char* SpdlogLogger::BackendName() const { return "spdlog"; }

	int SpdlogLogger::SetStorage(const char* filepath, size_t rotate_size_kb, size_t max_files)
	{
		// 入参三档（路径 / rotate 非 0 / 上限）都由门面判过，这里只管「装不装得上」。
		// spdlog 打开失败是抛 spdlog_ex，不是返回错误 —— 抓住它换成自己的码，绝不让它穿到调用方。
		std::shared_ptr<::spdlog::sinks::sink> fresh;
		try
		{
			if (max_files == 0u)
			{
				// 不轮转：单份一路追加（truncate 缺省 false，本口不重置已有内容）
				fresh = std::make_shared<::spdlog::sinks::basic_file_sink_mt>(std::string(filepath), false);
			}
			else
			{
				const std::size_t max_size = rotate_size_kb * 1024u;	// 溢出在门面挡下，这里乘得下
				fresh = std::make_shared<::spdlog::sinks::rotating_file_sink_mt>(std::string(filepath), max_size,
																				 max_files);
			}
		}
		catch (const ::spdlog::spdlog_ex&)
		{
			return CLV_LOG_OPEN_FAILED;
		}
		catch (const std::exception&)
		{
			return CLV_LOG_OPEN_FAILED;
		}

		auto& sinks = impl_->logger->sinks();
		if (impl_->file_sink)
		{
			// 换落点：先摘旧的那份文件 sink，控制台 sink 留着（并存）
			sinks.erase(std::remove(sinks.begin(), sinks.end(), impl_->file_sink), sinks.end());
		}
		sinks.push_back(fresh);
		impl_->file_sink = fresh;
		impl_->logger->flush();	   // 换档这一刻把已在缓冲里的消息落干净，别让新旧两份混着收
		return CLV_LOG_OK;
	}

}	 // namespace clv::backend::spdlog

#endif	  // CLV_LOGGER_USE_SPDLOG
