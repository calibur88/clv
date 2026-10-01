/* impl_fileio.h
 *
 * 实现层接口契约（clv::platform）：C ABI 之下的平台侧抽象，不出现在公开头里。
 *
 * - 全部方法返回 int，取值域与 CLV_FileError 一致（0=OK，其余为具体失败码）。平台后端不得自创
 *   返回值语义，也不得把系统错误码直接透出——映射责任在后端内部。
 * - Open 的 mode 取值域与 CLV_FileMode 一致；越界返回 CLV_FILE_INVALID_ARGUMENT。
 * - Read / Write 的 size 是实现层内部要消化的请求量：平台 API 的宽度限制（如 Win32 的 DWORD
 *   长度参数）由后端分块吸收，不许把截断后的字节数当成功报出去。
 * - 出参指针可为 NULL。失败时后端仍要写实际完成量（能算出多少写多少），算不出就不动出参。
 * - Seek 是绝对偏移，Seek 到文件尾之后不算失败。Size 与游标无关。Flush 对只读句柄是 no-op
 *   并返回 OK。Read / Write 受模式门控，Seek / Size / Flush 不受。
 *
 * 实例由 CreateFileIO() 产出裸指针，由 src/core/fileio.cpp 在 CLV_CloseFile 里 delete：
 * new / delete 都在本库模块内，不跨 ABI 边界。
 */
#ifndef CLV_PLATFORM_IMPL_FILEIO_H
#define CLV_PLATFORM_IMPL_FILEIO_H

#include <cstddef>
#include <cstdint>

namespace clv::platform
{

	class FileIO
	{
		public:

		virtual ~FileIO() = default;

		virtual int Open(const char* path, int mode) = 0;
		virtual int Read(void* buf, size_t size, size_t* bytes_read) = 0;
		virtual int Write(const void* data, size_t size, size_t* bytes_written) = 0;
		virtual int Seek(uint64_t offset, uint64_t* new_position) = 0;
		virtual int Size(uint64_t* size) = 0;
		virtual int Flush() = 0;
	};

	// 选源入口。定义在 impl_fileio.cpp，全库唯一一处平台分派。
	FileIO* CreateFileIO();

}	 // namespace clv::platform

#endif	  // CLV_PLATFORM_IMPL_FILEIO_H
