/* logger.h
 *
 * 日志后端的空实现（clv::backend::null）：所有调用都丢弃，不依赖任何第三方。
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
	};

}	 // namespace clv::backend::null

#endif	  // CLV_BACKEND_NULL_LOGGER_H
