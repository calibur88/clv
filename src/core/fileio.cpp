#include "CLV_File.h"
#include "platform/fileio/impl_fileio.h"
#include "platform/fileio/memory_fileio.h"
#include "src/core/path_contract.h"

#include <cstdint>

namespace clv::core
{

	namespace
	{

		bool IsModeValid(CLV_FileMode mode) noexcept
		{
			switch (mode)
			{
			case CLV_FILE_MODE_READ:
			case CLV_FILE_MODE_WRITE:
			case CLV_FILE_MODE_APPEND: return true;
			}
			return false;
		}

		platform::FileIO* AsImpl(CLV_File* h) noexcept { return reinterpret_cast<platform::FileIO*>(h); }

		/* 后端各入口返回 int，公开面返回 CLV_FileError。域外的值不投给调用方：收敛成该操作自己的
		 * 失败档，out_of_domain 由调用点按操作给。 */
		CLV_FileError ToFileError(int code, CLV_FileError out_of_domain) noexcept
		{
			if (code < CLV_FILE_OK || code > CLV_FILE_INVALID_ARGUMENT) return out_of_domain;
			return static_cast<CLV_FileError>(code);
		}

	}	 // namespace

}	 // namespace clv::core

extern "C" CLV_File* CLV_OpenFileEx(const char* path, CLV_FileMode mode, CLV_FileError* err)
{
	auto fail = [err](CLV_FileError code) -> CLV_File*
	{
		if (err != nullptr) *err = code;
		return nullptr;
	};

	if (! clv::core::IsPathValid(path)) return fail(CLV_FILE_INVALID_PATH);
	if (! clv::core::IsModeValid(mode)) return fail(CLV_FILE_INVALID_ARGUMENT);

	clv::platform::FileIO* impl = clv::platform::CreateFileIO();
	if (impl == nullptr) return fail(CLV_FILE_OPEN_FAILED);

	const int code = impl->Open(path, static_cast<int>(mode));
	if (code != CLV_FILE_OK)
	{
		delete impl;
		// 后端只该回 CLV_FileError 的失败档；域外的返回值不投给调用方，一律收敛成打开失败
		return fail(clv::core::ToFileError(code, CLV_FILE_OPEN_FAILED));
	}

	if (err != nullptr) *err = CLV_FILE_OK;
	return reinterpret_cast<CLV_File*>(impl);
}

extern "C" CLV_File* CLV_OpenFile(const char* path, CLV_FileMode mode) { return CLV_OpenFileEx(path, mode, nullptr); }

extern "C" CLV_FileError CLV_ReadFile(CLV_File* h, void* buf, size_t size, size_t* bytes_read)
{
	if (h == nullptr) return CLV_FILE_INVALID_HANDLE;
	return clv::core::ToFileError(clv::core::AsImpl(h)->Read(buf, size, bytes_read), CLV_FILE_READ_FAILED);
}

extern "C" CLV_FileError CLV_WriteFile(CLV_File* h, const void* data, size_t size, size_t* bytes_written)
{
	if (h == nullptr) return CLV_FILE_INVALID_HANDLE;
	return clv::core::ToFileError(clv::core::AsImpl(h)->Write(data, size, bytes_written), CLV_FILE_WRITE_FAILED);
}

extern "C" CLV_FileError CLV_SeekFile(CLV_File* h, uint64_t offset, uint64_t* new_position)
{
	if (h == nullptr) return CLV_FILE_INVALID_HANDLE;
	return clv::core::ToFileError(clv::core::AsImpl(h)->Seek(offset, new_position), CLV_FILE_SEEK_FAILED);
}

extern "C" CLV_FileError CLV_FileSize(CLV_File* h, uint64_t* size)
{
	if (h == nullptr) return CLV_FILE_INVALID_HANDLE;
	return clv::core::ToFileError(clv::core::AsImpl(h)->Size(size), CLV_FILE_SIZE_FAILED);
}

extern "C" CLV_FileError CLV_FlushFile(CLV_File* h)
{
	if (h == nullptr) return CLV_FILE_INVALID_HANDLE;
	return clv::core::ToFileError(clv::core::AsImpl(h)->Flush(), CLV_FILE_FLUSH_FAILED);
}

extern "C" void CLV_CloseFile(CLV_File* h) { delete clv::core::AsImpl(h); }

extern "C" CLV_FileError CLV_OpenMemoryFile(void* buffer, size_t size, CLV_FileMode mode, CLV_File** out_handle)
{
	auto fail = [out_handle](CLV_FileError code) -> CLV_FileError
	{
		if (out_handle != nullptr) *out_handle = nullptr;
		return code;
	};

	// 出参档先判：句柄给不出去，与「句柄不成立」不是一回事
	if (out_handle == nullptr) return CLV_FILE_INVALID_ARGUMENT;
	// 容量 0 是合法的空缓冲区（Read 得 0 字节、Write 任何量都越界拒）；容量非零却没缓冲才是参数不成立
	if (buffer == nullptr && size > 0u) return fail(CLV_FILE_INVALID_ARGUMENT);
	// 模式在契约层就收成两档：APPEND 在内存句柄上没有对应概念，与越界值同档
	if (mode != CLV_FILE_MODE_READ && mode != CLV_FILE_MODE_WRITE) return fail(CLV_FILE_INVALID_ARGUMENT);

	clv::platform::FileIO* impl = clv::platform::membuf::CreateMemoryFileIO(buffer, size);
	if (impl == nullptr) return fail(CLV_FILE_OPEN_FAILED);

	// path 传 NULL 是这一族入口的形状：缓冲区已由工厂给定，后端契约里那一格忽略
	const int code = impl->Open(nullptr, static_cast<int>(mode));
	if (code != CLV_FILE_OK)
	{
		delete impl;
		return fail(clv::core::ToFileError(code, CLV_FILE_OPEN_FAILED));
	}
	*out_handle = reinterpret_cast<CLV_File*>(impl);
	return CLV_FILE_OK;
}

extern "C" const char* CLV_FileStrError(CLV_FileError err)
{
	switch (err)
	{
	case CLV_FILE_OK: return "OK";
	case CLV_FILE_OPEN_FAILED: return "open failed";
	case CLV_FILE_READ_FAILED: return "read failed";
	case CLV_FILE_WRITE_FAILED: return "write failed";
	case CLV_FILE_INVALID_HANDLE: return "invalid handle";
	case CLV_FILE_INVALID_PATH: return "invalid path";
	case CLV_FILE_SEEK_FAILED: return "seek failed";
	case CLV_FILE_SIZE_FAILED: return "size failed";
	case CLV_FILE_FLUSH_FAILED: return "flush failed";
	case CLV_FILE_UNSUPPORTED: return "unsupported";
	case CLV_FILE_INVALID_ARGUMENT: return "invalid argument";
	default: return "unknown";
	}
}
