#include "CLV_File.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace
{

	// 契约规定宿主只许传 '/'：std::filesystem 在 Windows 上给的是反斜杠，一律先转 generic。
	std::string TempPath(const char* name)
	{
		std::error_code ec;
		const auto dir = std::filesystem::temp_directory_path(ec);
		EXPECT_FALSE(ec) << ec.message();
		return dir.generic_string() + "/" + name;
	}

	class ScopedFile
	{
		public:

		explicit ScopedFile(const std::string& path): path_(path) {}

		~ScopedFile()
		{
			std::error_code ec;
			std::filesystem::remove(std::filesystem::u8path(path_), ec);
		}

		ScopedFile(const ScopedFile&) = delete;
		ScopedFile& operator=(const ScopedFile&) = delete;

		const char* c_str() const { return path_.c_str(); }

		private:

		std::string path_;
	};

	CLV_File* OpenOrThrow(const char* path, CLV_FileMode mode)
	{
		CLV_FileError err = CLV_FILE_OK;
		CLV_File* handle = CLV_OpenFileEx(path, mode, &err);
		EXPECT_NE(handle, nullptr) << CLV_FileStrError(err);
		return handle;
	}

}	 // namespace

TEST(ClvFile, WriteThenReadRoundTrips)
{
	const ScopedFile scoped(TempPath("clv_test_file_roundtrip.bin"));

	{
		CLV_File* w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_WRITE);
		ASSERT_NE(w, nullptr);
		const char text[] = "hello clv";
		size_t written = 0;
		ASSERT_EQ(CLV_WriteFile(w, text, sizeof(text) - 1, &written), CLV_FILE_OK);
		EXPECT_EQ(written, sizeof(text) - 1);
		ASSERT_EQ(CLV_FlushFile(w), CLV_FILE_OK);
		CLV_CloseFile(w);
	}

	CLV_File* r = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_READ);
	ASSERT_NE(r, nullptr);
	char buf[16] = {};
	size_t got = 0;
	ASSERT_EQ(CLV_ReadFile(r, buf, sizeof(buf), &got), CLV_FILE_OK);
	EXPECT_EQ(got, 9u);
	EXPECT_STREQ(buf, "hello clv");
	CLV_CloseFile(r);
}

TEST(ClvFile, WriteModeTruncatesExistingContent)
{
	const ScopedFile scoped(TempPath("clv_test_file_truncate.bin"));

	CLV_File* w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_WRITE);
	ASSERT_EQ(CLV_WriteFile(w, "0123456789", 10, nullptr), CLV_FILE_OK);
	CLV_CloseFile(w);

	w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_WRITE);
	ASSERT_EQ(CLV_WriteFile(w, "ab", 2, nullptr), CLV_FILE_OK);
	uint64_t size = 0;
	ASSERT_EQ(CLV_FileSize(w, &size), CLV_FILE_OK);
	EXPECT_EQ(size, 2u) << "WRITE 模式必须清空已有内容，POSIX \"wb\" 语义";
	CLV_CloseFile(w);
}

TEST(ClvFile, AppendModeKeepsOldContentAndWritesAtEnd)
{
	const ScopedFile scoped(TempPath("clv_test_file_append.bin"));

	CLV_File* w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_WRITE);
	ASSERT_EQ(CLV_WriteFile(w, "first", 5, nullptr), CLV_FILE_OK);
	CLV_CloseFile(w);

	// APPEND 模式没有读能力，长度要另开只读句柄查
	w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_APPEND);
	ASSERT_NE(w, nullptr);
	size_t written = 0;
	ASSERT_EQ(CLV_WriteFile(w, "-second", 7, &written), CLV_FILE_OK);
	CLV_CloseFile(w);

	CLV_File* r = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_READ);
	uint64_t size = 0;
	ASSERT_EQ(CLV_FileSize(r, &size), CLV_FILE_OK);
	EXPECT_EQ(size, 12u);

	char buf[16] = {};
	size_t got = 0;
	ASSERT_EQ(CLV_ReadFile(r, buf, sizeof(buf), &got), CLV_FILE_OK);
	EXPECT_EQ(got, 12u);
	buf[got] = '\0';
	EXPECT_STREQ(buf, "first-second");
	CLV_CloseFile(r);
}

TEST(ClvFile, AppendHandlesSeekAndSizeLikeOtherModes)
{
	const ScopedFile scoped(TempPath("clv_test_file_append_seek.bin"));

	CLV_File* w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_WRITE);
	ASSERT_EQ(CLV_WriteFile(w, "0123456789", 10, nullptr), CLV_FILE_OK);
	CLV_CloseFile(w);

	w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_APPEND);
	uint64_t position = 0;
	// 契约：Seek / Size / Flush 三种模式下都可用，不受读写门控限制
	EXPECT_EQ(CLV_SeekFile(w, 3, &position), CLV_FILE_OK) << "APPEND 句柄要能 Seek";
	EXPECT_EQ(position, 3u);
	uint64_t size = 0;
	EXPECT_EQ(CLV_FileSize(w, &size), CLV_FILE_OK) << "APPEND 句柄要能取长度";
	EXPECT_EQ(size, 10u);
	EXPECT_EQ(CLV_FlushFile(w), CLV_FILE_OK);
	CLV_CloseFile(w);
}

TEST(ClvFile, ReadModeRequiresExistingFile)
{
	const std::string missing = TempPath("clv_test_file_not_here.bin");
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(missing), ec);

	CLV_FileError err = CLV_FILE_OK;
	EXPECT_EQ(CLV_OpenFileEx(missing.c_str(), CLV_FILE_MODE_READ, &err), nullptr);
	EXPECT_EQ(err, CLV_FILE_OPEN_FAILED);
}

TEST(ClvFile, BackslashPathIsRejectedAtContractLayer)
{
	std::error_code ec;
	const std::string dir = std::filesystem::temp_directory_path(ec).generic_string();
	ASSERT_FALSE(ec);
	const std::string backslashed = dir + "\\clv_test_file_backslash.bin";

	CLV_FileError err = CLV_FILE_OK;
	EXPECT_EQ(CLV_OpenFileEx(backslashed.c_str(), CLV_FILE_MODE_WRITE, &err), nullptr)
		<< "含反斜杠的路径要在契约层就拒，后端不做分隔符改写";
	EXPECT_EQ(err, CLV_FILE_INVALID_PATH);
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(backslashed)));
}

TEST(ClvFile, NullAndEmptyPathAreInvalidPath)
{
	CLV_FileError err = CLV_FILE_OK;
	EXPECT_EQ(CLV_OpenFileEx(nullptr, CLV_FILE_MODE_READ, &err), nullptr);
	EXPECT_EQ(err, CLV_FILE_INVALID_PATH);

	const std::string empty;
	EXPECT_EQ(CLV_OpenFileEx(empty.c_str(), CLV_FILE_MODE_WRITE, &err), nullptr);
	EXPECT_EQ(err, CLV_FILE_INVALID_PATH);
}

TEST(ClvFile, InvalidUtf8SequenceFailsToOpen)
{
	// 0xC3 后跟 0x28 不是合法 UTF-8：Windows 后端在 MultiByteToWideChar(MB_ERR_INVALID_CHARS) 处挡下
	std::string path = TempPath("clv_test_file_bad_utf8_");
	path.push_back(static_cast<char>(0xC3));
	path.push_back(static_cast<char>(0x28));
	path += ".bin";

	CLV_FileError err = CLV_FILE_OK;
	EXPECT_EQ(CLV_OpenFileEx(path.c_str(), CLV_FILE_MODE_WRITE, &err), nullptr);
	EXPECT_EQ(err, CLV_FILE_OPEN_FAILED);
}

TEST(ClvFile, NonAsciiUtf8PathRoundTrips)
{
	const ScopedFile scoped(TempPath("clv_test_file_中文路径.bin"));

	CLV_File* w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_WRITE);
	ASSERT_NE(w, nullptr) << "非 ASCII 路径必须落到这个名字上，不能被按代码页重解释成别的文件名";
	ASSERT_EQ(CLV_WriteFile(w, "utf8", 4, nullptr), CLV_FILE_OK);
	CLV_CloseFile(w);

	EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(std::string(scoped.c_str()))));

	CLV_File* r = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_READ);
	char buf[8] = {};
	size_t got = 0;
	ASSERT_EQ(CLV_ReadFile(r, buf, sizeof(buf), &got), CLV_FILE_OK);
	EXPECT_EQ(got, 4u);
	CLV_CloseFile(r);
}

TEST(ClvFile, OutOfRangeModeIsInvalidArgument)
{
	const ScopedFile scoped(TempPath("clv_test_file_bad_mode.bin"));
	CLV_FileError err = CLV_FILE_OK;
	EXPECT_EQ(CLV_OpenFileEx(scoped.c_str(), static_cast<CLV_FileMode>(99), &err), nullptr);
	EXPECT_EQ(err, CLV_FILE_INVALID_ARGUMENT);
}

TEST(ClvFile, ShortReadAtEndOfFileIsNotFailure)
{
	const ScopedFile scoped(TempPath("clv_test_file_short_read.bin"));

	CLV_File* w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_WRITE);
	ASSERT_EQ(CLV_WriteFile(w, "abc", 3, nullptr), CLV_FILE_OK);
	CLV_CloseFile(w);

	CLV_File* r = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_READ);
	char buf[16] = {};
	size_t got = 0;
	ASSERT_EQ(CLV_ReadFile(r, buf, sizeof(buf), &got), CLV_FILE_OK) << "短读不是失败";
	EXPECT_EQ(got, 3u);

	got = 123;
	ASSERT_EQ(CLV_ReadFile(r, buf, sizeof(buf), &got), CLV_FILE_OK);
	EXPECT_EQ(got, 0u) << "文件尾之后读到 0 字节，仍算成功";
	CLV_CloseFile(r);
}

TEST(ClvFile, ReadAndWriteAreGatedByOpenMode)
{
	const ScopedFile scoped(TempPath("clv_test_file_mode_gate.bin"));

	CLV_File* w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_WRITE);
	char buf[4] = {};
	size_t got = 0;
	EXPECT_EQ(CLV_ReadFile(w, buf, sizeof(buf), &got), CLV_FILE_READ_FAILED) << "WRITE 句柄不许读";
	CLV_CloseFile(w);

	CLV_File* r = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_READ);
	EXPECT_EQ(CLV_WriteFile(r, "x", 1, nullptr), CLV_FILE_WRITE_FAILED) << "READ 句柄不许写";
	EXPECT_EQ(CLV_FlushFile(r), CLV_FILE_OK) << "只读句柄没有写缓冲可刷，Flush 是 no-op";
	CLV_CloseFile(r);
}

TEST(ClvFile, SeekIsAbsoluteAndSizeIgnoresCursor)
{
	const ScopedFile scoped(TempPath("clv_test_file_seek.bin"));

	CLV_File* w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_WRITE);
	ASSERT_EQ(CLV_WriteFile(w, "0123456789", 10, nullptr), CLV_FILE_OK);
	CLV_CloseFile(w);

	CLV_File* r = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_READ);
	uint64_t position = 99;
	ASSERT_EQ(CLV_SeekFile(r, 7, &position), CLV_FILE_OK);
	EXPECT_EQ(position, 7u);

	// Size 与游标无关
	uint64_t size = 0;
	ASSERT_EQ(CLV_FileSize(r, &size), CLV_FILE_OK);
	EXPECT_EQ(size, 10u);

	char buf[4] = {};
	size_t got = 0;
	ASSERT_EQ(CLV_ReadFile(r, buf, 2, &got), CLV_FILE_OK);
	EXPECT_EQ(got, 2u);
	buf[2] = '\0';
	EXPECT_STREQ(buf, "78");

	// 回到 0 再读一次，验证绝对偏移
	ASSERT_EQ(CLV_SeekFile(r, 0, &position), CLV_FILE_OK);
	ASSERT_EQ(CLV_ReadFile(r, buf, 2, &got), CLV_FILE_OK);
	buf[2] = '\0';
	EXPECT_STREQ(buf, "01");
	CLV_CloseFile(r);
}

TEST(ClvFile, SeekBeyondEndIsAllowedAndReadsZero)
{
	const ScopedFile scoped(TempPath("clv_test_file_seek_past.bin"));

	CLV_File* w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_WRITE);
	ASSERT_EQ(CLV_WriteFile(w, "abc", 3, nullptr), CLV_FILE_OK);
	CLV_CloseFile(w);

	CLV_File* r = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_READ);
	uint64_t position = 0;
	ASSERT_EQ(CLV_SeekFile(r, 1024, &position), CLV_FILE_OK) << "Seek 到文件尾之后合法（POSIX 同语义）";
	EXPECT_EQ(position, 1024u);

	char buf[4] = {};
	size_t got = 7;
	ASSERT_EQ(CLV_ReadFile(r, buf, sizeof(buf), &got), CLV_FILE_OK);
	EXPECT_EQ(got, 0u);
	CLV_CloseFile(r);
}

TEST(ClvFile, LargeTransferCrossesTheInternalChunking)
{
	const ScopedFile scoped(TempPath("clv_test_file_chunked.bin"));
	constexpr size_t kBytes = 3u << 20u;	// 3 MiB，跨过实现里的 1 MiB 分块边界
	std::vector<uint8_t> payload(kBytes);
	for (size_t i = 0; i < kBytes; ++i) payload[i] = static_cast<uint8_t>(i * 31u + 7u);

	CLV_File* w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_WRITE);
	size_t written = 0;
	ASSERT_EQ(CLV_WriteFile(w, payload.data(), payload.size(), &written), CLV_FILE_OK);
	EXPECT_EQ(written, kBytes) << "大块写入不许被平台 API 的宽度限制截断";
	CLV_CloseFile(w);

	CLV_File* r = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_READ);
	std::vector<uint8_t> read_back(kBytes, 0);
	size_t got = 0;
	ASSERT_EQ(CLV_ReadFile(r, read_back.data(), read_back.size(), &got), CLV_FILE_OK);
	EXPECT_EQ(got, kBytes);
	EXPECT_EQ(read_back, payload);
	CLV_CloseFile(r);
}

TEST(ClvFile, NullOutPointersAreAccepted)
{
	const ScopedFile scoped(TempPath("clv_test_file_null_out.bin"));

	CLV_File* w = OpenOrThrow(scoped.c_str(), CLV_FILE_MODE_WRITE);
	ASSERT_EQ(CLV_WriteFile(w, "abc", 3, nullptr), CLV_FILE_OK);
	ASSERT_EQ(CLV_SeekFile(w, 1, nullptr), CLV_FILE_OK);
	ASSERT_EQ(CLV_FileSize(w, nullptr), CLV_FILE_OK);
	CLV_CloseFile(w);
}

TEST(ClvFile, NullHandleIsRejectedByEveryFunction)
{
	size_t got = 0;
	uint64_t value = 0;
	EXPECT_EQ(CLV_ReadFile(nullptr, nullptr, 0, &got), CLV_FILE_INVALID_HANDLE);
	EXPECT_EQ(CLV_WriteFile(nullptr, "x", 1, nullptr), CLV_FILE_INVALID_HANDLE);
	EXPECT_EQ(CLV_SeekFile(nullptr, 0, &value), CLV_FILE_INVALID_HANDLE);
	EXPECT_EQ(CLV_FileSize(nullptr, &value), CLV_FILE_INVALID_HANDLE);
	EXPECT_EQ(CLV_FlushFile(nullptr), CLV_FILE_INVALID_HANDLE);

	// 关空句柄是合法 no-op，不许崩
	CLV_CloseFile(nullptr);
}

TEST(ClvFile, MemoryFileBackendIsAnUnimplementedPlaceholder)
{
	std::vector<uint8_t> buffer(16, 0);
	CLV_File* handle = reinterpret_cast<CLV_File*>(0x1);
	EXPECT_EQ(CLV_OpenMemoryFile(buffer.data(), buffer.size(), &handle), CLV_FILE_UNSUPPORTED);
	EXPECT_EQ(handle, nullptr) << "占位实现必须把出参置空，不许留野指针";

	CLV_File* null_out = nullptr;
	EXPECT_EQ(CLV_OpenMemoryFile(nullptr, 0, nullptr), CLV_FILE_UNSUPPORTED);
	EXPECT_EQ(CLV_OpenMemoryFile(nullptr, 0, &null_out), CLV_FILE_UNSUPPORTED);
}

TEST(ClvFile, StrErrorCoversEveryCode)
{
	const CLV_FileError codes[] = { CLV_FILE_OK,		   CLV_FILE_OPEN_FAILED,	 CLV_FILE_READ_FAILED,
									CLV_FILE_WRITE_FAILED, CLV_FILE_INVALID_HANDLE,	 CLV_FILE_INVALID_PATH,
									CLV_FILE_SEEK_FAILED,  CLV_FILE_SIZE_FAILED,	 CLV_FILE_FLUSH_FAILED,
									CLV_FILE_UNSUPPORTED,  CLV_FILE_INVALID_ARGUMENT };

	for (const CLV_FileError code: codes)
	{
		const char* text = CLV_FileStrError(code);
		ASSERT_NE(text, nullptr);
		EXPECT_NE(std::string(text), "unknown") << "枚举值 " << static_cast<int>(code) << " 没有专属文案";
	}

	EXPECT_STREQ(CLV_FileStrError(static_cast<CLV_FileError>(999)), "unknown");
}
