/* impl_logger.h
 *
 * 后端接口契约（clv::backend）：C ABI 之下的实现层抽象，不出现在公开头里。
 *
 * - level 的取值域与 CLV_LogLevel 一致，msg 是 UTF-8 字节串。后端不得解析 msg 里的格式占位符，
 *   也不做编码转换。
 * - msg 为 NULL 时整条丢弃。
 * - SetLevel 的取值域同样是 CLV_LogLevel；越界是 no-op，保持原过滤等级。
 * - 构造后的默认过滤等级是 CLV_LOG_INFO；当前等级由后端自己拿着，公开侧的查询口在能力查询那一族。
 * - 日志失败不许影响宿主：SetStorage 之外的入口不回报、不抛；SetStorage 可以回报（取值域同
 *   CLV_LogError），但回报只到门面为止 —— 门面把码投给 CLV_LogSetStorageEx 的调用方，
 *   CLV_Log 那条路仍然一条都不碰。
 * - SetStorage 收到的入参已由门面判过：路径合法、rotate_size_kb 非 0、max_files 不越上限。
 *   后端只判「我这把后端落不落得了盘」与「打开成不成」。
 */
#ifndef CLV_BACKEND_IMPL_LOGGER_H
#define CLV_BACKEND_IMPL_LOGGER_H

#include <stddef.h>

namespace clv::backend
{

	class Logger
	{
		public:

		virtual ~Logger() = default;

		virtual void Log(int level, const char* msg) = 0;
		virtual void SetLevel(int level) = 0;
		/** 当前用于过滤的那一档，取值域同 CLV_LogLevel。空后端不过滤，恒回构造缺省。 */
		virtual int GetLevel() const = 0;
		/** 后端名，ASCII 静态串、不得为 NULL；与构建选项同源。 */
		virtual const char* BackendName() const = 0;
		/** 换落点：把 filepath 那份 sink 装上（已装过的换掉，不叠加），控制台 sink 保留。
		 *  返回值取值域同 CLV_LogError：0 成功，103 本后端落不了盘，104 打开失败。 */
		virtual int SetStorage(const char* filepath, size_t rotate_size_kb, size_t max_files) = 0;
	};

}	 // namespace clv::backend

#endif	  // CLV_BACKEND_IMPL_LOGGER_H
