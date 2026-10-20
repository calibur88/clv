/* log.h
 *
 * 内部层投递日志的薄封装：把一条消息转投公开口 CLV_Log，用这一处的件不需要知道日志后端在哪、
 * 也不需要自己拼等级枚举值。消息按 UTF-8 字节串直投、不进格式化，所以 '%' 一类字符没有特殊含义。
 *
 * 库里投日志只有这一条路：其余件不直接 include 公开日志头自己转投 `CLV_Log`。
 * 公开日志头仍被两处直接 include，但都不是转投路 —— `src/core/logger.cpp` 是那张口本体，
 * `backends/spdlog/logger.cpp` 只对接等级枚举。新入口要么走本件这两个封装，要么就不得投日志。
 *
 * 等级只有 warn 与 error 两档对外部诊断有意义：丢包、丢帧、丢块这类「局部不成立但照常继续」
 * 走 LogWarn；一条流被判不可用、结构走到无法继续的境地才走 LogError。
 */
#ifndef CLV_CORE_LOG_H
#define CLV_CORE_LOG_H

#include "CLV_Logger.h"

namespace clv
{
	namespace core
	{

		inline void LogWarn(const char* msg) { CLV_Log(CLV_LOG_WARN, msg); }

		inline void LogError(const char* msg) { CLV_Log(CLV_LOG_ERROR, msg); }

	}	 // namespace core
}	 // namespace clv

#endif	  // CLV_CORE_LOG_H
