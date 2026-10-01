/* CLV_Logger.h
 *
 * 日志的 C ABI 契约。消息一律按 UTF-8 字节串透传：库不解析其中的 '{}'、'%' 之类占位符，
 * 也不做编码转换。Windows 控制台要正确显示由应用自行 SetConsoleOutputCP(CP_UTF8)，库不代劳。
 *
 * - msg 为 NULL 时整条丢弃，不记错误（本契约无错误通道：日志失败不许影响宿主）。
 * - level 不在 CLV_LogLevel 四值内时整条丢弃。
 * - 过滤等级只丢不存：低于当前等级的事件直接丢弃。当前生效等级不提供查询接口。
 * - 构造后的默认过滤等级是 CLV_LOG_INFO。
 * - 等级值不在 CLV_LogLevel 四值内时 CLV_LogSetLevel 是 no-op，保持原等级。
 * - CLV_LogSetStorage 本期是接口占位：声明与定义都在，调用无效果，落盘后端待下一轮。
 * - 后端由构建选项决定（spdlog 或 null）；null 后端下所有调用都是丢弃。
 */
#ifndef CLV_LOGGER_H
#define CLV_LOGGER_H

#include "CLV_Export.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef enum CLV_LogLevel
	{
		CLV_LOG_DEBUG,
		CLV_LOG_INFO,
		CLV_LOG_WARN,
		CLV_LOG_ERROR
	} CLV_LogLevel;

	CLV_API void CLV_Log(CLV_LogLevel level, const char* msg);
	CLV_API void CLV_LogSetLevel(CLV_LogLevel level);
	CLV_API void CLV_LogSetStorage(const char* filepath, size_t rotate_size_kb);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_LOGGER_H
