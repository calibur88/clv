#include "impl_fileio.h"

#include "platform/platform.h"

// ---------- 选源：全库唯一一处平台分派 ----------
// 判 CLV_PLATFORM_*，不碰裸宏。
#if defined(CLV_PLATFORM_WINDOWS)
	#include "win32_fileio.h"
	#define CLV_FILEIO_IMPL ::clv::platform::win32::Win32FileIO
#elif defined(CLV_PLATFORM_LINUX)
	#include "linux_fileio.h"
	#define CLV_FILEIO_IMPL ::clv::platform::linux_::LinuxFileIO
#elif defined(CLV_PLATFORM_MACOS)
	#include "macos_fileio.h"
	#define CLV_FILEIO_IMPL ::clv::platform::macos::MacosFileIO
#elif defined(CLV_PLATFORM_IOS)
	// 占位：iOS 的平台判定分支保留在 platform.h，实现待移动端排期。
	#error "CLV: iOS fileio not implemented"
#else
	#error "CLV: no fileio backend registered for this platform"
#endif
// --------------------------------------------

namespace clv::platform
{

	FileIO* CreateFileIO() { return new CLV_FILEIO_IMPL(); }

}	 // namespace clv::platform
