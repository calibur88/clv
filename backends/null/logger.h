/* logger.h
 *
 * 日志后端的空实现（clv::backend::null）：所有调用都丢弃，不依赖任何第三方。
 * 落盘这一档不假装成功 —— SetStorage 一律回 CLV_LOG_UNSUPPORTED_BACKEND。
 */
#ifndef CLV_BACKEND_NULL_LOGGER_H
#define CLV_BACKEND_NULL_LOGGER_H

#include "../impl_logger.h"

namespace clv::backend::null
{

	class NullLogger final: public Logger
	{
		public:

		void Log(int level, const char* msg) override;
		void SetLevel(int level) override;
		int GetLevel() const override;
		const char* BackendName() const override;
		int SetStorage(const char* filepath, size_t rotate_size_kb, size_t max_files) override;
	};

}	 // namespace clv::backend::null

#endif	  // CLV_BACKEND_NULL_LOGGER_H
