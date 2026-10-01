/* impl_logger.h
 *
 * 后端接口契约（clv::backend）：C ABI 之下的实现层抽象，不出现在公开头里。
 *
 * - level 的取值域与 CLV_LogLevel 一致，msg 是 UTF-8 字节串。后端不得解析 msg 里的格式占位符，
 *   也不做编码转换。
 * - msg 为 NULL 时整条丢弃。
 * - SetLevel 的取值域同样是 CLV_LogLevel；越界是 no-op，保持原过滤等级。
 * - 构造后的默认过滤等级是 CLV_LOG_INFO；无等级查询接口，当前等级由宿主自己记账。
 * - 日志失败不许影响宿主：后端内部吞掉落点问题，不向上抛、不返回错误码。
 */
#ifndef CLV_BACKEND_IMPL_LOGGER_H
#define CLV_BACKEND_IMPL_LOGGER_H

namespace clv::backend
{

	class Logger
	{
		public:

		virtual ~Logger() = default;

		virtual void Log(int level, const char* msg) = 0;
		virtual void SetLevel(int level) = 0;
	};

}	 // namespace clv::backend

#endif	  // CLV_BACKEND_IMPL_LOGGER_H
