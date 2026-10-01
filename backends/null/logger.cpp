#include "logger.h"

namespace clv::backend::null
{

	void NullLogger::Log(int level, const char* msg)
	{
		(void) level;
		(void) msg;
	}

	void NullLogger::SetLevel(int level) { (void) level; }

}	 // namespace clv::backend::null
