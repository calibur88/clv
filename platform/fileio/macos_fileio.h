/* macos_fileio.h
 *
 * IFileIO 的 macOS 实现：stdio，与 Linux 后端同构但物理隔离（两份文件各自演进，不共享头）。
 * 路径按字节序直传，不做编码转换，也不做 Unicode 正规化——文件系统侧的 NFD 正规化不属本库承诺。
 */
#ifndef CLV_PLATFORM_MACOS_FILEIO_H
#define CLV_PLATFORM_MACOS_FILEIO_H

#include "impl_fileio.h"
#include "platform/platform.h"

#if ! defined(CLV_PLATFORM_MACOS)
	#error "CLV: macos_fileio.h included on non-macOS platform"
#endif

#include <cstdio>

namespace clv::platform::macos
{

	class MacosFileIO final: public FileIO
	{
		public:

		MacosFileIO() = default;
		~MacosFileIO() override;

		MacosFileIO(const MacosFileIO&) = delete;
		MacosFileIO& operator=(const MacosFileIO&) = delete;

		int Open(const char* path, int mode) override;
		int Read(void* buf, size_t size, size_t* bytes_read) override;
		int Write(const void* data, size_t size, size_t* bytes_written) override;
		int Seek(uint64_t offset, uint64_t* new_position) override;
		int Size(uint64_t* size) override;
		int Flush() override;

		private:

		FILE* m_fp = nullptr;
		int m_mode = -1;
	};

}	 // namespace clv::platform::macos

#endif	  // CLV_PLATFORM_MACOS_FILEIO_H
