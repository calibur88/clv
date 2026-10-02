#include "gtest/gtest.h"

#if defined(_MSC_VER) && defined(_DEBUG)
	#include <crtdbg.h>
	#include <stdlib.h>	   // _set_error_mode / _set_abort_behavior 是 MSVC 扩展，只在 stdlib.h 里
#endif

int main(int argc, char** argv)
{
#if defined(_MSC_VER) && defined(_DEBUG)
	// 无人值守跑测试：调试 CRT 与 /RTC1 的检查失败一律改走 stderr。这些报告默认弹模态框，
	// 会把进程挂在那里等点击——ctest 与 gtest 发现阶段都因此超时。报告类型是单值不是掩码，逐个设。
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
