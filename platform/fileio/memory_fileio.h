/* memory_fileio.h
 *
 * 内存缓冲区后端：把调用方给的一块缓冲区接进同一套 FileIO 虚表，公开面走 CLV_OpenMemoryFile。
 *
 * 与文件后端的三点不同，逐条都是公开契约（见 include/CLV_File.h「内存缓冲区」一节）：
 *   1. Open 的 path 入参忽略——缓冲区在工厂函数里就给定了，不在 Open 时给；
 *   2. 缓冲区全程归调用方：本类不分配、不 free、不 realloc，Close 只销毁本对象；
 *   3. 容量固定：Write 越出容量即整次拒，绝不扩，也不接受 APPEND。
 * 模式门控、短读语义、Seek 到末尾之后合法这三条与文件后端完全一致，不复用另一套说法。
 */
#ifndef CLV_PLATFORM_IMPL_MEMORY_FILEIO_H
#define CLV_PLATFORM_IMPL_MEMORY_FILEIO_H

#include "impl_fileio.h"

#include <cstddef>
#include <cstdint>

namespace clv::platform::membuf
{

	class MemoryFileIO final: public FileIO
	{
		public:

		MemoryFileIO(void* buffer, size_t capacity) noexcept;

		int Open(const char* path, int mode) override;
		int Read(void* buf, size_t size, size_t* bytes_read) override;
		int Write(const void* data, size_t size, size_t* bytes_written) override;
		int Seek(uint64_t offset, uint64_t* new_position) override;
		int Size(uint64_t* size) override;
		int Flush() override;

		private:

		unsigned char* base_ = nullptr;
		size_t capacity_ = 0u;
		uint64_t pos_ = 0u;
		/* 只读句柄的 Size 是容量；只写句柄的 Size 是被写覆盖过的最远字节数（高水位）。
		 * 两者都与会话游标无关，与文件后端那条「Size 与游标无关」同式。 */
		uint64_t high_water_ = 0u;
		int mode_ = -1;	   // -1 = 尚未 Open 成功
	};

	/* 产出一把内存句柄的裸指针，所有权交给 src/core/fileio.cpp，由 CLV_CloseFile 单点 delete
	 * （与 CreateFileIO 那条「new / delete 都在本库模块内」的规矩同一条路）。 */
	FileIO* CreateMemoryFileIO(void* buffer, size_t capacity);

}	 // namespace clv::platform::membuf

#endif	  // CLV_PLATFORM_IMPL_MEMORY_FILEIO_H
