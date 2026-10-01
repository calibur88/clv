/* CLV_File.h
 *
 * 文件读写的 C ABI 契约。句柄不透明，能力由 CLV_FileMode 在打开时定死一侧。
 *
 * 路径：
 * - UTF-8 字节序（不含 BOM）。
 * - 只接受 '/' 作分隔符。NULL、空串、含 '\\' 的路径都在契约层拒收，返回 CLV_FILE_INVALID_PATH。
 * - 字节序不是合法 UTF-8 时，Windows 后端在转 UTF-16 处失败，返回 CLV_FILE_OPEN_FAILED。
 * - 长路径（超过系统默认上限）不处理，由应用侧负责。
 *
 * 打开模式，语义逐一对齐 POSIX 的 "rb" / "wb" / "ab"：
 * - READ   文件必须已存在，游标在 0。
 * - WRITE  建文件；已存在则清空内容。
 * - APPEND 建文件；已存在则写入恒定落到末尾。
 *
 * 读写与游标：
 * - 短读不是失败：返回 CLV_FILE_OK，实际字节数经 bytes_read 出参给出；到文件尾时 bytes_read 为 0。
 * - 出参指针可为 NULL，表示调用方不要该信息。
 * - Seek 是绝对偏移。Seek 到文件尾之后合法，其后 Read 返回 0 字节。
 * - Size 与游标无关，反映文件的实际字节长度。
 * - Flush 对只读句柄是 no-op（无写缓冲可刷）并返回 CLV_FILE_OK，不因此报错。
 * - 只有 Read / Write 受模式门控：READ 句柄上 Write 记 CLV_FILE_WRITE_FAILED，
 *   WRITE / APPEND 句柄上 Read 记 CLV_FILE_READ_FAILED。Seek / Size / Flush 三种模式下都可用。
 *
 * 其它：
 * - 跨实例、跨线程的并发访问语义未定义，调用方自行同步。
 * - 句柄由 CLV_CloseFile 释放；对 NULL 句柄调用任何函数都返回 CLV_FILE_INVALID_HANDLE，
 *   CLV_CloseFile(NULL) 是合法 no-op。已关闭的句柄再使用是未定义行为。
 * - CLV_OpenMemoryFile 本期是接口占位，声明与定义都在，实现待内存后端落地。
 */
#ifndef CLV_FILE_H
#define CLV_FILE_H

#include "CLV_Export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct CLV_File CLV_File;

	typedef enum CLV_FileMode
	{
		CLV_FILE_MODE_READ,
		CLV_FILE_MODE_WRITE,
		CLV_FILE_MODE_APPEND
	} CLV_FileMode;

	/* 取值域同时是实现层（platform/fileio/impl_fileio.h）的返回值域，实现层不得自创语义。
 * 0~4 与既有码值不变，新码只追加在后面。 */
	typedef enum CLV_FileError
	{
		CLV_FILE_OK = 0,
		CLV_FILE_OPEN_FAILED,
		CLV_FILE_READ_FAILED,
		CLV_FILE_WRITE_FAILED,
		CLV_FILE_INVALID_HANDLE,
		CLV_FILE_INVALID_PATH,
		CLV_FILE_SEEK_FAILED,
		CLV_FILE_SIZE_FAILED,
		CLV_FILE_FLUSH_FAILED,
		CLV_FILE_UNSUPPORTED,
		CLV_FILE_INVALID_ARGUMENT
	} CLV_FileError;

	/* 打开。失败返回 NULL；错误细分走 Ex 的 err 出参，本函数忽略之。 */
	CLV_API CLV_File* CLV_OpenFile(const char* path, CLV_FileMode mode);

	/* 打开。成功时 *out_handle 非空且返回 CLV_FILE_OK；失败时 *out_handle 为 NULL 且返回具体码。
 * err 出参可为 NULL。 */
	CLV_API CLV_File* CLV_OpenFileEx(const char* path, CLV_FileMode mode, CLV_FileError* err);

	CLV_API CLV_FileError CLV_ReadFile(CLV_File* h, void* buf, size_t size, size_t* bytes_read);
	CLV_API CLV_FileError CLV_WriteFile(CLV_File* h, const void* data, size_t size, size_t* bytes_written);
	CLV_API CLV_FileError CLV_SeekFile(CLV_File* h, uint64_t offset, uint64_t* new_position);
	CLV_API CLV_FileError CLV_FileSize(CLV_File* h, uint64_t* size);
	CLV_API CLV_FileError CLV_FlushFile(CLV_File* h);
	CLV_API void CLV_CloseFile(CLV_File* h);

	/* 内存缓冲区后端的入口（本期占位）。返回 CLV_FILE_UNSUPPORTED 且 *out_handle 为 NULL。 */
	CLV_API CLV_FileError CLV_OpenMemoryFile(void* buffer, size_t size, CLV_File** out_handle);

	/* 每个枚举值都返回非空、静态存储期的 ASCII 串；未知值返回 "unknown"。 */
	CLV_API const char* CLV_FileStrError(CLV_FileError err);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_FILE_H
