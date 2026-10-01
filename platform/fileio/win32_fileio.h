/* win32_fileio.h
 *
 * IFileIO 的 Windows 实现：Win32 文件 API（CreateFileW / ReadFile / WriteFile /
 * SetFilePointerEx / GetFileSizeEx / FlushFileBuffers）。
 *
 * 路径一律走 W 系列：入参是 UTF-8 字节串，先 MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS)
 * 转 UTF-16 再交给 CreateFileW。窄版 A 系列按进程 ANSI 代码页重解释字节，会把非 ASCII 路径
 * 落成别的文件名，因此禁用。分隔符不做任何改写（CreateFileW 原生认 '/'）。
 */
#ifndef CLV_PLATFORM_WIN32_FILEIO_H
#define CLV_PLATFORM_WIN32_FILEIO_H

#include "impl_fileio.h"
#include "platform/platform.h"

#if ! defined(CLV_PLATFORM_WINDOWS)
	#error "CLV: win32_fileio.h included on non-Windows platform"
#endif

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
// min / max 宏会污染同翻译单元里的模板与实参比较，只保留显式比较写法。
#ifndef NOMINMAX
	#define NOMINMAX
#endif
#include <cstdint>
#include <windows.h>

namespace clv::platform::win32
{

	class Win32FileIO final: public FileIO
	{
		public:

		Win32FileIO() = default;
		~Win32FileIO() override;

		Win32FileIO(const Win32FileIO&) = delete;
		Win32FileIO& operator=(const Win32FileIO&) = delete;

		int Open(const char* path, int mode) override;
		int Read(void* buf, size_t size, size_t* bytes_read) override;
		int Write(const void* data, size_t size, size_t* bytes_written) override;
		int Seek(uint64_t offset, uint64_t* new_position) override;
		int Size(uint64_t* size) override;
		int Flush() override;

		private:

		HANDLE m_handle = INVALID_HANDLE_VALUE;
		int m_mode = -1;	// 打开时定死的能力侧：Read/Write 门控与 Flush 的 no-op 判定用它
	};

}	 // namespace clv::platform::win32

#endif	  // CLV_PLATFORM_WIN32_FILEIO_H
