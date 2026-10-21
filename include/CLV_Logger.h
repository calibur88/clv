/* CLV_Logger.h
 *
 * 日志的 C ABI 契约。消息一律按 UTF-8 字节串透传：库不解析其中的 '{}'、'%' 之类占位符，
 * 也不做编码转换。Windows 控制台要正确显示需应用自行调用 SetConsoleOutputCP(CP_UTF8)。
 *
 * - msg 为 NULL 时整条丢弃，不记错误（本契约无错误通道：日志失败不许影响宿主）。
 * - level 不在 CLV_LogLevel 四值内时整条丢弃。
 * - 过滤等级只丢不存：低于当前等级的事件直接丢弃。当前生效等级用 CLV_LogGetLevel 问。
 * - 构造后的默认过滤等级是 CLV_LOG_INFO。
 * - 等级值不在 CLV_LogLevel 四值内时 CLV_LogSetLevel 是 no-op，保持原等级。
 * - CLV_LogGetLevel 回答的是「当前设定并用于过滤的那一档」；空后端（见下）一律丢弃，
 *   它的等级恒等于构造缺省 CLV_LOG_INFO，SetLevel 在那把后端上不改变任何可观测行为。
 * - CLV_LogBackendName 返回本构建选中的后端名（ASCII 静态串，永不为 NULL）：
 *   "spdlog" 或 "null"。它与 CLV_Capabilities 的 bit1 同源于构建选项，不随调用改变。
 * - CLV_LogSetStorage 是**无回报**的老口：转调 CLV_LogSetStorageEx 并把结果码丢弃，调用方看不出
 *   成没成。要结果一律用 Ex 那一只。
 * - CLV_LogSetStorageEx 是本面唯一有回报的入口：它把落点问题按 CLV_LogError 回报，
 *   但仍然不抛异常、不改变 CLV_Log 的行为。
 * - 后端由构建选项决定（spdlog 或 null）；null 后端下所有调用都是丢弃，
 *   且 CLV_LogSetStorageEx 回 CLV_LOG_UNSUPPORTED_BACKEND（不假装成功）。
 *
 * 路径（只适用于落盘口）：与文件面同一把尺 —— UTF-8 字节串、只用 '/' 作分隔符；
 * NULL、空串、含 '\\' 的路径都判 CLV_LOG_INVALID_PATH。判定发生在后端之前，两族入口不会漂。
 * 与文件面不同的一处：本口不把 UTF-8 转成宽字符再打开，非 ASCII 路径能否落对由窄字符运行时决定，
 * 本 ABI 不作承诺。
 *
 * 落点形状：rotate_size_kb 是单份轮转上限（KB），0 判 CLV_LOG_INVALID_ARGUMENT（不解释成「无限」）；
 * 换算成字节时乘得下才算合法，越界同判参数档。
 * max_files 三档 —— 0 = 不轮转（基础件一路追加，不重置已有内容），N>0 = 轮转，且**基础件之外**最多
 * 再留 N 份轮转件（N=1 时盘上是 log 与 log.1 两份）；上限 200 000 由本库自钉，越界判
 * CLV_LOG_INVALID_ARGUMENT，不靠后端抛异常兜。
 * 再次调用换落点，不叠加；控制台输出与文件并存，改落点不会关掉控制台。
 *
 * 号位：CLV_LogError 从 102 起，是段尾追加（节拍面末码 101 的下一格），与文件 1–10、容器 1–13、
 * 音频 32–63、视频 64–95 各段的数值无关；字幕面从日志面末码的下一格起。
 * 各面错误码各自独立枚举，不得跨头比对数值。
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

	/* 日志面的错误码。OK 与其余五面同为 0；失败码只三格 + 一格里追加，取值域见抬头「号位」段。
	 * 判定顺序固定：路径档 → 参数档 → 后端档 → 打开档，前面几档与后端选型无关。 */
	typedef enum CLV_LogError
	{
		CLV_LOG_OK = 0,
		CLV_LOG_INVALID_PATH = 102,			  ///< 路径为 NULL、空串或含 '\\'
		CLV_LOG_UNSUPPORTED_BACKEND = 103,	  ///< 当前构建的后端落不了盘（null 后端）
		CLV_LOG_OPEN_FAILED = 104,			  ///< 路径合法但打不开：目录不存在、被占、权限不足
		CLV_LOG_INVALID_ARGUMENT = 105
	} CLV_LogError;

	CLV_EXPORT void CLV_Log(CLV_LogLevel level, const char* msg);
	CLV_EXPORT void CLV_LogSetLevel(CLV_LogLevel level);
	/* 当前设定并用于过滤的等级。构造后 = CLV_LOG_INFO；越界 SetLevel 不改变它。 */
	CLV_EXPORT CLV_LogLevel CLV_LogGetLevel(void);
	/* 本构建选中的后端名，ASCII 静态串、永不为 NULL："spdlog" 或 "null"。 */
	CLV_EXPORT const char* CLV_LogBackendName(void);
	/** 无回报的老口：转调 CLV_LogSetStorageEx 并丢弃结果码，max_files 固定取 1。 */
	CLV_EXPORT void CLV_LogSetStorage(const char* filepath, size_t rotate_size_kb);
	/** 落盘口，带回报。filepath 是 UTF-8 字节串且只用 '/'；rotate_size_kb 为 0 或 max_files 越 200 000 都判参数档。 */
	CLV_EXPORT CLV_LogError CLV_LogSetStorageEx(const char* filepath, size_t rotate_size_kb, size_t max_files);
	/* 文案是静态存储期的 ASCII 串；未知值返回 "unknown"。 */
	CLV_EXPORT const char* CLV_LogStrError(CLV_LogError err);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_LOGGER_H
