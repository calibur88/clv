// 平台判定先取兜底推导件：CMake 没传 CLV_PLATFORM_* 宏时（手编 / IDE 索引 / clang-tidy）由它按
// 编译器目标宏推导，守卫与 impl_fileio.cpp 的分派才会判到同一个平台。
#include "platform/platform.h"

// 自守卫：非本平台整文件编译为空单元。
#if defined(CLV_PLATFORM_MACOS)

	#include "CLV_File.h"
	#include "macos_fileio.h"

	#include <limits>

namespace clv::platform::macos
{

	namespace
	{

		const char* ModeString(int mode)
		{
			switch (mode)
			{
			case CLV_FILE_MODE_READ: return "rb";
			case CLV_FILE_MODE_WRITE: return "wb";
			case CLV_FILE_MODE_APPEND: return "ab";
			default: return nullptr;
			}
		}

	}	 // namespace

	MacosFileIO::~MacosFileIO()
	{
		if (fp_ != nullptr) std::fclose(fp_);
	}

	int MacosFileIO::Open(const char* path, int mode)
	{
		if (path == nullptr || path[0] == '\0') return CLV_FILE_INVALID_PATH;

		const char* mode_string = ModeString(mode);
		if (mode_string == nullptr) return CLV_FILE_INVALID_ARGUMENT;

		FILE* fp = std::fopen(path, mode_string);
		if (fp == nullptr) return CLV_FILE_OPEN_FAILED;

		fp_ = fp;
		mode_ = mode;
		return CLV_FILE_OK;
	}

	int MacosFileIO::Read(void* buf, size_t size, size_t* bytes_read)
	{
		if (fp_ == nullptr) return CLV_FILE_INVALID_HANDLE;
		if (mode_ != CLV_FILE_MODE_READ) return CLV_FILE_READ_FAILED;
		if (size > 0 && buf == nullptr) return CLV_FILE_INVALID_ARGUMENT;

		const size_t got = std::fread(buf, 1, size, fp_);
		if (bytes_read != nullptr) *bytes_read = got;
		if (got < size && std::ferror(fp_) != 0) return CLV_FILE_READ_FAILED;
		return CLV_FILE_OK;	   // 短读且无错就是到了文件尾
	}

	int MacosFileIO::Write(const void* data, size_t size, size_t* bytes_written)
	{
		if (fp_ == nullptr) return CLV_FILE_INVALID_HANDLE;
		if (mode_ != CLV_FILE_MODE_WRITE && mode_ != CLV_FILE_MODE_APPEND) return CLV_FILE_WRITE_FAILED;
		if (size > 0 && data == nullptr) return CLV_FILE_INVALID_ARGUMENT;

		const size_t put = std::fwrite(data, 1, size, fp_);
		if (bytes_written != nullptr) *bytes_written = put;
		return put < size ? CLV_FILE_WRITE_FAILED : CLV_FILE_OK;
	}

	int MacosFileIO::Seek(uint64_t offset, uint64_t* new_position)
	{
		if (fp_ == nullptr) return CLV_FILE_INVALID_HANDLE;

		// fseek 的位移量是 long：本平台是 64 位，但超过 LONG_MAX 的偏移窄化后会被当负数用，
		// 越界一律判失败，不交给窄化后的静默错位。
		if (offset > static_cast<uint64_t>(std::numeric_limits<long>::max())) return CLV_FILE_SEEK_FAILED;
		if (std::fseek(fp_, static_cast<long>(offset), SEEK_SET) != 0) return CLV_FILE_SEEK_FAILED;

		const long position = std::ftell(fp_);
		if (position < 0) return CLV_FILE_SEEK_FAILED;
		if (new_position != nullptr) *new_position = static_cast<uint64_t>(position);
		return CLV_FILE_OK;
	}

	int MacosFileIO::Size(uint64_t* size)
	{
		if (fp_ == nullptr) return CLV_FILE_INVALID_HANDLE;

		const long saved = std::ftell(fp_);
		if (saved < 0) return CLV_FILE_SIZE_FAILED;
		if (std::fseek(fp_, 0, SEEK_END) != 0) return CLV_FILE_SIZE_FAILED;

		const long bytes = std::ftell(fp_);
		const int restored = std::fseek(fp_, saved, SEEK_SET);
		if (bytes < 0) return CLV_FILE_SIZE_FAILED;
		if (restored != 0) return CLV_FILE_SIZE_FAILED;

		if (size != nullptr) *size = static_cast<uint64_t>(bytes);
		return CLV_FILE_OK;
	}

	int MacosFileIO::Flush()
	{
		if (fp_ == nullptr) return CLV_FILE_INVALID_HANDLE;
		if (mode_ == CLV_FILE_MODE_READ) return CLV_FILE_OK;

		return std::fflush(fp_) == 0 ? CLV_FILE_OK : CLV_FILE_FLUSH_FAILED;
	}

}	 // namespace clv::platform::macos

#endif	  // CLV_PLATFORM_MACOS
