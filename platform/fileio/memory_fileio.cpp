#include "memory_fileio.h"

#include "CLV_File.h"

#include <cstring>
#include <new>

namespace clv::platform::membuf
{

	MemoryFileIO::MemoryFileIO(void* buffer, size_t capacity) noexcept:
		base_(static_cast<unsigned char*>(buffer)), capacity_(capacity)
	{
	}

	int MemoryFileIO::Open(const char* path, int mode)
	{
		(void) path;	// 契约第 1 点：缓冲区已在工厂给定，这里的 path 一律忽略
		switch (mode)
		{
		case CLV_FILE_MODE_READ:
		case CLV_FILE_MODE_WRITE: break;
		default:
			// APPEND 落不进内存句柄：没有「追加到末尾」这个概念，容量越界只会整次拒。
			// 取参数不成立档而不是 UNSUPPORTED —— 后者在本次落地后是文件面的留白号位，不留产出者。
			return CLV_FILE_INVALID_ARGUMENT;
		}
		mode_ = mode;
		pos_ = 0u;
		high_water_ = 0u;
		return CLV_FILE_OK;
	}

	int MemoryFileIO::Read(void* buf, size_t size, size_t* bytes_read)
	{
		// 完成量先落 0 再判档：门控挡住时能算出的完成量就是 0（与 Write 同一条，也与实现层契约同一条）
		if (bytes_read != nullptr) *bytes_read = 0u;
		if (mode_ != CLV_FILE_MODE_READ) return CLV_FILE_READ_FAILED;
		if (size > 0u && buf == nullptr) return CLV_FILE_INVALID_ARGUMENT;	  // 与文件后端同档
		const uint64_t available = pos_ < capacity_ ? static_cast<uint64_t>(capacity_) - pos_ : 0u;
		const uint64_t n = static_cast<uint64_t>(size) < available ? static_cast<uint64_t>(size) : available;
		if (buf != nullptr && n > 0u) std::memcpy(buf, base_ + pos_, static_cast<size_t>(n));
		pos_ += n;
		if (bytes_read != nullptr) *bytes_read = static_cast<size_t>(n);
		return CLV_FILE_OK;	   // 短读与读到末尾 0 字节都不是失败（与文件后端同式）
	}

	int MemoryFileIO::Write(const void* data, size_t size, size_t* bytes_written)
	{
		// 完成量先落 0 再判模式：实现层契约那条「失败时能算出多少写多少」在这一档算得出来（0），
		// 不能因为门控在先就把出参留着不动——留旧值会让调用方读到"写了 99 字节"这类假数。
		if (bytes_written != nullptr) *bytes_written = 0u;
		if (mode_ != CLV_FILE_MODE_WRITE) return CLV_FILE_WRITE_FAILED;
		if (size > 0u && data == nullptr) return CLV_FILE_INVALID_ARGUMENT;
		if (size == 0u) return CLV_FILE_OK;
		// 先判整次能否容下，再决定写不写：绝不做半次写，也绝不扩缓冲
		if (pos_ > capacity_ || static_cast<uint64_t>(size) > static_cast<uint64_t>(capacity_) - pos_)
			return CLV_FILE_WRITE_FAILED;
		std::memcpy(base_ + pos_, data, size);
		pos_ += static_cast<uint64_t>(size);
		if (pos_ > high_water_) high_water_ = pos_;
		if (bytes_written != nullptr) *bytes_written = size;
		return CLV_FILE_OK;
	}

	int MemoryFileIO::Seek(uint64_t offset, uint64_t* new_position)
	{
		pos_ = offset;	  // 越过容量的 Seek 合法（与文件后端「Seek 到文件尾之后不算失败」同一条），其后 Read 得 0 字节
		if (new_position != nullptr) *new_position = pos_;
		return CLV_FILE_OK;
	}

	int MemoryFileIO::Size(uint64_t* size)
	{
		if (size == nullptr) return CLV_FILE_SIZE_FAILED;
		*size = mode_ == CLV_FILE_MODE_READ ? static_cast<uint64_t>(capacity_) : high_water_;
		return CLV_FILE_OK;
	}

	int MemoryFileIO::Flush() { return CLV_FILE_OK; }	 // 内存句柄没有缓冲层可刷，两种模式下都是 no-op

	FileIO* CreateMemoryFileIO(void* buffer, size_t capacity)
	{
		return new (std::nothrow) MemoryFileIO(buffer, capacity);
	}

}	 // namespace clv::platform::membuf
