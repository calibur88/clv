#include "CLV_Logger.h"
#include "backends/impl_logger.h"
#include "src/core/path_contract.h"

#include <memory>
#include <stddef.h>

// 后端由 CMake option 选：定义 CLV_LOGGER_USE_SPDLOG 用 spdlog 后端，否则用 null 后端。
#if defined(CLV_LOGGER_USE_SPDLOG)
	#include "backends/spdlog/logger.h"
#else
	#include "backends/null/logger.h"
#endif

namespace clv::core
{

	namespace
	{

		// 内部助手一律留在这个匿名命名空间里：本件只经 C ABI 出口对外，
		// 放在命名空间作用域会给静态件多带一个有外部链接的符号。
		std::unique_ptr<backend::Logger> MakeLogger()
		{
#if defined(CLV_LOGGER_USE_SPDLOG)
			return std::make_unique<::clv::backend::spdlog::SpdlogLogger>();
#else
			return std::make_unique<::clv::backend::null::NullLogger>();
#endif
		}

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

		/* 轮转份数的自钉上限。后端那份轮转件自己也有一条同值的上限检查，但那是抛异常的形式 ——
		 * 本口的形状是「问出参、不接异常」，所以在契约层先挡，越界的整次不装。 */
		constexpr size_t kMaxFilesCap = 200000u;

		/* rotate_size_kb 要乘 1024 换成字节数：乘得下才算参数合法，绝不回绕成一个小值把上限抹掉。 */
		bool RotateSizeBytesOK(size_t rotate_size_kb) noexcept { return rotate_size_kb <= SIZE_MAX / 1024u; }

		/* 后端返回 int，公开面返回 CLV_LogError。域外的值不投给调用方：收敛成打开失败档。 */
		CLV_LogError ToLogError(int code) noexcept
		{
			switch (code)
			{
			case CLV_LOG_OK:
			case CLV_LOG_INVALID_PATH:
			case CLV_LOG_UNSUPPORTED_BACKEND:
			case CLV_LOG_OPEN_FAILED:
			case CLV_LOG_INVALID_ARGUMENT: return static_cast<CLV_LogError>(code);
			default: return CLV_LOG_OPEN_FAILED;
			}
		}

	}	 // namespace

}	 // namespace clv::core

extern "C" void CLV_Log(CLV_LogLevel level, const char* msg)
{
	if (! clv::core::IsLevelValid(level)) return;	 // 契约：越界等级整条丢弃
	clv::core::Active().Log(static_cast<int>(level), msg);
}

extern "C" CLV_LogLevel CLV_LogGetLevel(void)
{
	// 等级由后端自己拿着：spdlog 那把回当前过滤档，空后端恒回构造缺省（契约见 CLV_Logger.h）。
	// 门面不再存一份，免得两处答案不同 —— 这一问的唯一答案就是后端的答复。
	return static_cast<CLV_LogLevel>(clv::core::Active().GetLevel());
}

extern "C" const char* CLV_LogBackendName(void) { return clv::core::Active().BackendName(); }

extern "C" void CLV_LogSetLevel(CLV_LogLevel level)
{
	if (! clv::core::IsLevelValid(level)) return;	 // 契约：越界 no-op，保持原等级
	clv::core::Active().SetLevel(static_cast<int>(level));
}

extern "C" CLV_LogError CLV_LogSetStorageEx(const char* filepath, size_t rotate_size_kb, size_t max_files)
{
	// 判定顺序与文件面同式：路径档 → 参数档 → 后端档 → 打开档。前三档都在任何后端代码之前，
	// 所以 null 后端与 spdlog 后端判出来的参数档完全一致，不会因选型漂。
	if (! clv::core::IsPathValid(filepath)) return CLV_LOG_INVALID_PATH;
	if (rotate_size_kb == 0u || ! clv::core::RotateSizeBytesOK(rotate_size_kb)) return CLV_LOG_INVALID_ARGUMENT;
	if (max_files > clv::core::kMaxFilesCap) return CLV_LOG_INVALID_ARGUMENT;

	return clv::core::ToLogError(clv::core::Active().SetStorage(filepath, rotate_size_kb, max_files));
}

extern "C" void CLV_LogSetStorage(const char* filepath, size_t rotate_size_kb)
{
	// 老口没有回报通道：转调新口，结果码就地丢弃。份数取 1（基础件之外最多再留一份轮转件）。
	(void) CLV_LogSetStorageEx(filepath, rotate_size_kb, 1u);
}

extern "C" const char* CLV_LogStrError(CLV_LogError err)
{
	switch (err)
	{
	case CLV_LOG_OK: return "OK";
	case CLV_LOG_INVALID_PATH: return "invalid path";
	case CLV_LOG_UNSUPPORTED_BACKEND: return "unsupported backend";
	case CLV_LOG_OPEN_FAILED: return "open failed";
	case CLV_LOG_INVALID_ARGUMENT: return "invalid argument";
	default: return "unknown";
	}
}
