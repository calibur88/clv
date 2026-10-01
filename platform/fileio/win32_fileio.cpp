// 自守卫：非本平台整文件编译为空单元。
#if defined(CLV_PLATFORM_WINDOWS)

	#include "win32_fileio.h"

	#include "CLV_File.h"
	#include "platform/platform.h"

	#include <limits>
	#include <string>

namespace clv::platform::win32
{

	namespace
	{

		// ReadFile / WriteFile 的长度参数是 32 位 DWORD，而契约给的是 64 位 size_t，
		// 大块请求必须分块，不许窄化截断。
		constexpr size_t kMaxChunk = 1U << 20U;

		// UTF-8 字节串 -> UTF-16。MB_ERR_INVALID_CHARS 让非法字节序在转换处就报错，
		// 而不是被系统按替换字符静默改写成一个别的文件。
		bool ToUtf16(const char* text, std::wstring& out)
		{
			const int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
			if (wide_len == 0) return false;

			out.assign(static_cast<size_t>(wide_len), L'\0');
			const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, &out[0], wide_len);
			if (converted == 0)
			{
				out.clear();
				return false;
			}
			return true;
		}

	}	 // namespace

	Win32FileIO::~Win32FileIO()
	{
		if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
	}

	int Win32FileIO::Open(const char* path, int mode)
	{
		// 分隔符判定在契约层（src/core/fileio.cpp），这里只挡空路径与非法编码
		if (path == nullptr || path[0] == '\0') return CLV_FILE_INVALID_PATH;

		std::wstring wide;
		if (! ToUtf16(path, wide)) return CLV_FILE_OPEN_FAILED;

		DWORD access = 0;
		DWORD disposition = 0;
		switch (mode)
		{
		case CLV_FILE_MODE_READ:
			access = GENERIC_READ;
			disposition = OPEN_EXISTING;
			break;
		case CLV_FILE_MODE_WRITE:
			access = GENERIC_WRITE;
			disposition = CREATE_ALWAYS;
			break;
		case CLV_FILE_MODE_APPEND:
			// FILE_APPEND_DATA 让写入恒定落末尾，即 POSIX O_APPEND 的语义；
			// FILE_READ_ATTRIBUTES 供 Size 查询用。
			access = FILE_APPEND_DATA | FILE_READ_ATTRIBUTES;
			disposition = OPEN_ALWAYS;
			break;
		default: return CLV_FILE_INVALID_ARGUMENT;
		}

		const HANDLE handle =
			CreateFileW(wide.c_str(), access, FILE_SHARE_READ, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return CLV_FILE_OPEN_FAILED;

		if (mode == CLV_FILE_MODE_APPEND)
		{
			// OPEN_ALWAYS 命中已有文件时游标在 0，把游标也落到末尾，Seek/写位置从打开起就一致
			if (SetFilePointerEx(handle, LARGE_INTEGER {}, nullptr, FILE_END) == 0)
			{
				CloseHandle(handle);
				return CLV_FILE_OPEN_FAILED;
			}
		}

		m_handle = handle;
		m_mode = mode;
		return CLV_FILE_OK;
	}

	int Win32FileIO::Read(void* buf, size_t size, size_t* bytes_read)
	{
		if (m_handle == INVALID_HANDLE_VALUE) return CLV_FILE_INVALID_HANDLE;
		if (m_mode != CLV_FILE_MODE_READ) return CLV_FILE_READ_FAILED;
		if (size > 0 && buf == nullptr) return CLV_FILE_INVALID_ARGUMENT;

		size_t total = 0;
		while (total < size)
		{
			const size_t remaining = size - total;
			const size_t want = remaining < kMaxChunk ? remaining : kMaxChunk;
			DWORD got = 0;
			if (ReadFile(m_handle, static_cast<char*>(buf) + total, static_cast<DWORD>(want), &got, nullptr) == 0)
			{
				if (bytes_read != nullptr) *bytes_read = total;
				return CLV_FILE_READ_FAILED;
			}
			total += got;
			if (got < want) break;	  // 短读即到了文件尾，不是失败
		}

		if (bytes_read != nullptr) *bytes_read = total;
		return CLV_FILE_OK;
	}

	int Win32FileIO::Write(const void* data, size_t size, size_t* bytes_written)
	{
		if (m_handle == INVALID_HANDLE_VALUE) return CLV_FILE_INVALID_HANDLE;
		if (m_mode != CLV_FILE_MODE_WRITE && m_mode != CLV_FILE_MODE_APPEND) return CLV_FILE_WRITE_FAILED;
		if (size > 0 && data == nullptr) return CLV_FILE_INVALID_ARGUMENT;

		size_t total = 0;
		while (total < size)
		{
			const size_t remaining = size - total;
			const size_t want = remaining < kMaxChunk ? remaining : kMaxChunk;
			DWORD put = 0;
			if (WriteFile(m_handle, static_cast<const char*>(data) + total, static_cast<DWORD>(want), &put, nullptr) ==
				0)
			{
				if (bytes_written != nullptr) *bytes_written = total;
				return CLV_FILE_WRITE_FAILED;
			}
			if (put == 0)
			{
				// 没推进就停手，否则整块写不进时会在这里转不出去
				if (bytes_written != nullptr) *bytes_written = total;
				return CLV_FILE_WRITE_FAILED;
			}
			total += put;
		}

		if (bytes_written != nullptr) *bytes_written = total;
		return CLV_FILE_OK;
	}

	int Win32FileIO::Seek(uint64_t offset, uint64_t* new_position)
	{
		if (m_handle == INVALID_HANDLE_VALUE) return CLV_FILE_INVALID_HANDLE;
		// SetFilePointerEx 的位移量是有符号 LARGE_INTEGER，超过 2^63-1 的偏移会被当成负数
		if (offset > static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max())) return CLV_FILE_SEEK_FAILED;

		LARGE_INTEGER target {};
		target.QuadPart = static_cast<LONGLONG>(offset);
		LARGE_INTEGER moved {};
		if (SetFilePointerEx(m_handle, target, &moved, FILE_BEGIN) == 0) return CLV_FILE_SEEK_FAILED;

		if (new_position != nullptr) *new_position = static_cast<uint64_t>(moved.QuadPart);
		return CLV_FILE_OK;
	}

	int Win32FileIO::Size(uint64_t* size)
	{
		if (m_handle == INVALID_HANDLE_VALUE) return CLV_FILE_INVALID_HANDLE;

		LARGE_INTEGER bytes {};
		if (GetFileSizeEx(m_handle, &bytes) == 0) return CLV_FILE_SIZE_FAILED;

		if (size != nullptr) *size = static_cast<uint64_t>(bytes.QuadPart);
		return CLV_FILE_OK;
	}

	int Win32FileIO::Flush()
	{
		if (m_handle == INVALID_HANDLE_VALUE) return CLV_FILE_INVALID_HANDLE;
		if (m_mode == CLV_FILE_MODE_READ) return CLV_FILE_OK;	 // 只读句柄没有写缓冲可刷

		return FlushFileBuffers(m_handle) == 0 ? CLV_FILE_FLUSH_FAILED : CLV_FILE_OK;
	}

}	 // namespace clv::platform::win32

#endif	  // CLV_PLATFORM_WINDOWS
