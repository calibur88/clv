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
 * - `buf` / `data` 为 NULL 而 `size > 0` 时是参数档：返回 `CLV_FILE_INVALID_ARGUMENT`，
 *   该次的完成量出参写 0。文件句柄与内存句柄同式；`size == 0` 时不碰指针，两档都返回 `CLV_FILE_OK`。
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
 * 内存缓冲区（CLV_OpenMemoryFile）：
 * - 所有权：缓冲区全程归调用方。库不分配、不 free、不 realloc，也不接管；`CLV_CloseFile` 只销毁
 *   库内的游标对象，宿主那块缓冲在关闭之后依旧有效。
 * - 入参档：`out_handle == NULL` ⇒ `CLV_FILE_INVALID_ARGUMENT`（这一档没有句柄可判，落参数档不落句柄档）；
 *   `buffer == NULL` 且 `size > 0` ⇒ `CLV_FILE_INVALID_ARGUMENT`；`size == 0` 合法，是一份空缓冲
 *   （`Size` 得 0、递进合法缓冲的 `Read` 得 OK + 0 字节、任何非零 `Write` 都越界拒）。
 * - 模式：`mode` 只受理 `CLV_FILE_MODE_READ` 与 `CLV_FILE_MODE_WRITE` 两档，打开时定死一侧（与文件面同式）。
 *   `APPEND` 在这里没有对应概念——容量由调用方给定，不存在「追加到末尾」——与越界模式同档回 `CLV_FILE_INVALID_ARGUMENT`。
 * - 容量固定：写档越出 `size` 即**整次拒**并回 `CLV_FILE_WRITE_FAILED`、`*bytes_written` 写 0，绝不半次写、
 *   绝不扩、绝不 realloc。要写更多只能由调用方换一块更大的缓冲重开句柄。
 * - 模式门控、短读语义、`Seek` 越过末尾合法这三条与文件后端完全一致，不复用另一套说法：
 *   `READ` 句柄上 `Write` 记 `WRITE_FAILED`，`Seek` / `Size` / `Flush` 不受门控，`Flush` 两种模式下都是 no-op 回 OK。
 * - `Size` 与游标无关：只读句柄给容量，只写句柄给**被写覆盖过的最远字节数**（高水位）——
 *   这样「往内存写一份、再用另一把只读句柄读回」不需要调用方自己记长度。
 * - 跨实例、跨线程并发同样不承诺；两把句柄指向同一缓冲时各自记游标，互不干涉，写读重叠区的数据竞争由调用方负责。
 *
 * 号位：本入口落地后 `CLV_FILE_UNSUPPORTED`（值 9）在文件面**不再有任何产出者**——它不是被别的码取代，
 * 而是这一族的入口已经实现。号位按「留白不复用」处理：后续新码一律段尾追加，9 不再投给任何新语义，
 * 因此既有数值不因这一格的退场而前移；`CLV_FileStrError(9)` 仍返非空 ASCII 串。
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
	CLV_EXPORT CLV_File* CLV_OpenFile(const char* path, CLV_FileMode mode);

	/* 打开。成功返回非空句柄且（`err` 非空时）`*err == CLV_FILE_OK`；失败返回 NULL 并写入具体码。
	 * 句柄走返回值本身，本函数没有句柄出参；`err` 可为 NULL。 */
	CLV_EXPORT CLV_File* CLV_OpenFileEx(const char* path, CLV_FileMode mode, CLV_FileError* err);

	/* 读。短读返回 CLV_FILE_OK，实际读到的字节数走 bytes_read；到文件尾读到 0 字节同样返回 CLV_FILE_OK；
	 * 请求 0 字节也返回 CLV_FILE_OK 且 *bytes_read 置 0 —— 所以本函数不能用 0 字节判文件尾，剩余量走 CLV_FileSize。 */
	CLV_EXPORT CLV_FileError CLV_ReadFile(CLV_File* h, void* buf, size_t size, size_t* bytes_read);
	CLV_EXPORT CLV_FileError CLV_WriteFile(CLV_File* h, const void* data, size_t size, size_t* bytes_written);
	CLV_EXPORT CLV_FileError CLV_SeekFile(CLV_File* h, uint64_t offset, uint64_t* new_position);
	CLV_EXPORT CLV_FileError CLV_FileSize(CLV_File* h, uint64_t* size);
	CLV_EXPORT CLV_FileError CLV_FlushFile(CLV_File* h);
	CLV_EXPORT void CLV_CloseFile(CLV_File* h);

	/* 把调用方给定的缓冲区开成只读或固定容量只写的句柄，六条契约见本头抬头「内存缓冲区」一节。
	 * 成功回 CLV_FILE_OK 且 *out_handle 有效；入参不成立（含 mode 取 APPEND 或越界值）回
	 * CLV_FILE_INVALID_ARGUMENT 并把 *out_handle 置 NULL。缓冲区始终归调用方，Close 不动它。 */
	CLV_EXPORT CLV_FileError CLV_OpenMemoryFile(void* buffer, size_t size, CLV_FileMode mode, CLV_File** out_handle);

	/* 每个枚举值都返回非空、静态存储期的 ASCII 串；未知值返回 "unknown"。 */
	CLV_EXPORT const char* CLV_FileStrError(CLV_FileError err);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_FILE_H
