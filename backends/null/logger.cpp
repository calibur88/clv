#include "logger.h"

#include "CLV_Logger.h"

namespace clv::backend::null
{

	void NullLogger::Log(int level, const char* msg)
	{
		(void) level;
		(void) msg;
	}

	void NullLogger::SetLevel(int level) { (void) level; }

	int NullLogger::GetLevel() const
	{
		// 空后端不过滤：任何东西都不落、不排，所以「当前生效等级」这一问在它这里没有可变状态可答。
		// 如实回答 = 恒回构造缺省，SetLevel 不改变它 —— 这一条同时写进公开头与用例，不留成猜测。
		return CLV_LOG_INFO;
	}

	const char* NullLogger::BackendName() const { return "null"; }

	int NullLogger::SetStorage(const char* filepath, size_t rotate_size_kb, size_t max_files)
	{
		(void) filepath;
		(void) rotate_size_kb;
		(void) max_files;
		return CLV_LOG_UNSUPPORTED_BACKEND;	   // 空后端落不了盘，也不假装成功
	}

}	 // namespace clv::backend::null
