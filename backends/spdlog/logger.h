/* logger.h
 *
 * 日志后端的 spdlog 实现（clv::backend::spdlog）。
 *
 * m_impl 用 void* 而不是 shared_ptr 成员：spdlog 的头只出现在 .cpp 里，第三方不越过这一层。
 * 自持 logger 实例、不进 spdlog 注册表，因此 flush 策略必须走成员级 flush_on。
 */
#ifndef CLV_BACKEND_SPDLOG_LOGGER_H
#define CLV_BACKEND_SPDLOG_LOGGER_H

#include "../impl_logger.h"

namespace clv::backend::spdlog
{

	class SpdlogLogger final: public Logger
	{
		public:

		SpdlogLogger();
		~SpdlogLogger() override;

		SpdlogLogger(const SpdlogLogger&) = delete;
		SpdlogLogger& operator=(const SpdlogLogger&) = delete;

		void Log(int level, const char* msg) override;
		void SetLevel(int level) override;

		private:

		void* m_impl = nullptr;
	};

}	 // namespace clv::backend::spdlog

#endif	  // CLV_BACKEND_SPDLOG_LOGGER_H
