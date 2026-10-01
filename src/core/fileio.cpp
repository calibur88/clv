#include "CLV_File.h"
#include "platform/fileio/impl_fileio.h"

#include <cstdint>

namespace clv::core
{

	namespace
	{

		// 路径契约：UTF-8 字节序 + 只用 '/'。这条判定跨所有平台一致，发生在任何平台代码之前，
		// 因此后端拿到的路径已经不含反斜杠。
		bool IsPathValid(const char* path) noexcept
		{
			if (path == nullptr || path[0] == '\0') return false;
			for (const char* p = path; *p != '\0'; ++p)
				if (*p == '\\') return false;
			return true;
		}

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
		return fail(static_cast<CLV_FileError>(code));
	}

	if (err != nullptr) *err = CLV_FILE_OK;
	return reinterpret_cast<CLV_File*>(impl);
}

extern "C" CLV_File* CLV_OpenFile(const char* path, CLV_FileMode mode) { return CLV_OpenFileEx(path, mode, nullptr); }

extern "C" CLV_FileError CLV_ReadFile(CLV_File* h, void* buf, size_t size, size_t* bytes_read)
{
	if (h == nullptr) return CLV_FILE_INVALID_HANDLE;
	return static_cast<CLV_FileError>(clv::core::AsImpl(h)->Read(buf, size, bytes_read));
}

extern "C" CLV_FileError CLV_WriteFile(CLV_File* h, const void* data, size_t size, size_t* bytes_written)
{
	if (h == nullptr) return CLV_FILE_INVALID_HANDLE;
	return static_cast<CLV_FileError>(clv::core::AsImpl(h)->Write(data, size, bytes_written));
}

extern "C" CLV_FileError CLV_SeekFile(CLV_File* h, uint64_t offset, uint64_t* new_position)
{
	if (h == nullptr) return CLV_FILE_INVALID_HANDLE;
	return static_cast<CLV_FileError>(clv::core::AsImpl(h)->Seek(offset, new_position));
}

extern "C" CLV_FileError CLV_FileSize(CLV_File* h, uint64_t* size)
{
	if (h == nullptr) return CLV_FILE_INVALID_HANDLE;
	return static_cast<CLV_FileError>(clv::core::AsImpl(h)->Size(size));
}

extern "C" CLV_FileError CLV_FlushFile(CLV_File* h)
{
	if (h == nullptr) return CLV_FILE_INVALID_HANDLE;
	return static_cast<CLV_FileError>(clv::core::AsImpl(h)->Flush());
}

extern "C" void CLV_CloseFile(CLV_File* h) { delete clv::core::AsImpl(h); }

extern "C" CLV_FileError CLV_OpenMemoryFile(void* buffer, size_t size, CLV_File** out_handle)
{
	// 接口占位：内存后端待下一轮字幕 / 视频 / 音频层的消费点出现时落地
	(void) buffer;
	(void) size;
	if (out_handle != nullptr) *out_handle = nullptr;
	return CLV_FILE_UNSUPPORTED;
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
