/* linux_fileio.h
 *
 * IFileIO 的 Linux 实现：stdio（fopen / fread / fwrite / fseek / ftell / fflush）。
 * 路径按字节序直传，不做编码转换——POSIX 侧文件名本就是字节串，UTF-8 天然成立。
 */
#ifndef CLV_PLATFORM_LINUX_FILEIO_H
#define CLV_PLATFORM_LINUX_FILEIO_H

#include "impl_fileio.h"
#include "platform/platform.h"

#if ! defined(CLV_PLATFORM_LINUX)
	#error "CLV: linux_fileio.h included on non-Linux platform"
#endif

#include <cstdio>

namespace clv::platform::linux_
{
	// 尾下划线避 GNU / Clang 非严格模式的预定义宏 linux。
	class LinuxFileIO final: public FileIO
	{
		public:

		LinuxFileIO() = default;
		~LinuxFileIO() override;

		LinuxFileIO(const LinuxFileIO&) = delete;
		LinuxFileIO& operator=(const LinuxFileIO&) = delete;

		int Open(const char* path, int mode) override;
		int Read(void* buf, size_t size, size_t* bytes_read) override;
		int Write(const void* data, size_t size, size_t* bytes_written) override;
		int Seek(uint64_t offset, uint64_t* new_position) override;
		int Size(uint64_t* size) override;
		int Flush() override;

		private:

		FILE* fp_ = nullptr;
		int mode_ = -1;
	};

}	 // namespace clv::platform::linux_

#endif	  // CLV_PLATFORM_LINUX_FILEIO_H
