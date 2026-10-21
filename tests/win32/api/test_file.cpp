#include "CLV_File.h"

#include "gtest/gtest.h"
#include <algorithm>
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

TEST(ClvFile, MemoryReadHandleSeesCapacityAndEndsCleanly)
{
	std::vector<uint8_t> buffer { 1, 2, 3, 4, 5 };
	CLV_File* h = nullptr;
	ASSERT_EQ(CLV_OpenMemoryFile(buffer.data(), buffer.size(), CLV_FILE_MODE_READ, &h), CLV_FILE_OK);
	ASSERT_NE(h, nullptr);

	uint64_t size = 0;
	EXPECT_EQ(CLV_FileSize(h, &size), CLV_FILE_OK);
	EXPECT_EQ(size, static_cast<uint64_t>(buffer.size())) << "只读句柄的 Size 是容量，与游标无关";

	uint8_t out[8] = { 0 };
	size_t got = 0;
	EXPECT_EQ(CLV_ReadFile(h, out, sizeof(out), &got), CLV_FILE_OK) << "短读不是失败";
	EXPECT_EQ(got, buffer.size());
	EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), out));

	got = 99;
	EXPECT_EQ(CLV_ReadFile(h, out, sizeof(out), &got), CLV_FILE_OK) << "到末尾再读是 OK + 0 字节";
	EXPECT_EQ(got, 0u);

	// 只读句柄上的写落在模式门控档，且文件面自此不再产出 UNSUPPORTED
	size_t written = 7;
	const CLV_FileError rc = CLV_WriteFile(h, "x", 1, &written);
	EXPECT_EQ(rc, CLV_FILE_WRITE_FAILED);
	EXPECT_NE(rc, CLV_FILE_UNSUPPORTED) << "值 9 在本次落地后是留白号位，任何入口都不该产出它";
	EXPECT_EQ(written, 0u);
	CLV_CloseFile(h);
}

TEST(ClvFile, MemoryOpenArgumentTiersAndEmptyBuffer)
{
	std::vector<uint8_t> buffer(4, 0);
	CLV_File* h = nullptr;
	// 模式在打开时定死一侧：这里取 READ，要写就另开一把 WRITE 句柄（与文件面同一条门控）。
	// 这里钉的是两件事：入参档的归属，以及文件面自此不再产出值 9。
	ASSERT_EQ(CLV_OpenMemoryFile(buffer.data(), buffer.size(), CLV_FILE_MODE_READ, &h), CLV_FILE_OK);
	ASSERT_NE(h, nullptr);
	CLV_CloseFile(h);

	EXPECT_EQ(CLV_OpenMemoryFile(buffer.data(), buffer.size(), CLV_FILE_MODE_READ, nullptr), CLV_FILE_INVALID_ARGUMENT)
		<< "出参为空属参数档";
	CLV_File* out = reinterpret_cast<CLV_File*>(0x1);
	EXPECT_EQ(CLV_OpenMemoryFile(nullptr, 3, CLV_FILE_MODE_READ, &out), CLV_FILE_INVALID_ARGUMENT) << "有容量却没缓冲";
	EXPECT_EQ(out, nullptr) << "失败路径必须置空，不许留野指针";

	// 容量 0 是合法的空缓冲区：三条读判都要有确定答案，不是 INVALID_ARGUMENT
	CLV_File* empty = nullptr;
	ASSERT_EQ(CLV_OpenMemoryFile(nullptr, 0, CLV_FILE_MODE_READ, &empty), CLV_FILE_OK);
	ASSERT_NE(empty, nullptr);
	uint64_t size = 99;
	EXPECT_EQ(CLV_FileSize(empty, &size), CLV_FILE_OK);
	EXPECT_EQ(size, 0u);
	uint8_t one = 0;
	size_t got = 99;
	EXPECT_EQ(CLV_ReadFile(empty, &one, 4, &got), CLV_FILE_OK)
		<< "给了缓冲就是一次合法短读：容量 0 ⇒ 完成量 0，不是失败";
	EXPECT_EQ(got, 0u);
	got = 99;
	EXPECT_EQ(CLV_ReadFile(empty, nullptr, 4, &got), CLV_FILE_INVALID_ARGUMENT)
		<< "非零 size 配空缓冲，与文件后端同落参数档";
	EXPECT_EQ(got, 0u);
	EXPECT_EQ(CLV_ReadFile(empty, nullptr, 0, &got), CLV_FILE_OK) << "size 为 0 时不碰指针，这一档仍是 OK + 0 字节";
	EXPECT_EQ(got, 0u);
	size_t wr = 99;
	EXPECT_EQ(CLV_WriteFile(empty, "x", 1, &wr), CLV_FILE_WRITE_FAILED);
	EXPECT_EQ(wr, 0u);
	CLV_CloseFile(empty);

	// 写侧的空指针档要用 WRITE 句柄来钉：READ 句柄上的写先被模式门控挡住，落不到参数档
	CLV_File* wn = nullptr;
	ASSERT_EQ(CLV_OpenMemoryFile(buffer.data(), buffer.size(), CLV_FILE_MODE_WRITE, &wn), CLV_FILE_OK);
	wr = 99;
	EXPECT_EQ(CLV_WriteFile(wn, nullptr, 1, &wr), CLV_FILE_INVALID_ARGUMENT) << "非零 size 配空指针，读写两侧同档";
	EXPECT_EQ(wr, 0u);
	wr = 99;
	EXPECT_EQ(CLV_WriteFile(wn, nullptr, 0, &wr), CLV_FILE_OK) << "size 为 0 时不碰指针，写侧也是 OK + 0 字节";
	EXPECT_EQ(wr, 0u);
	EXPECT_EQ(buffer[0], 0) << "两次没写成的调用不该动宿主缓冲一个字节";
	CLV_CloseFile(wn);
}

TEST(ClvFile, MemoryBufferStaysOwnedByCallerAcrossCloseAndHandlesAreIndependent)
{
	std::vector<uint8_t> buffer { 10, 20, 30, 40 };

	CLV_File* a = nullptr;
	CLV_File* b = nullptr;
	ASSERT_EQ(CLV_OpenMemoryFile(buffer.data(), buffer.size(), CLV_FILE_MODE_READ, &a), CLV_FILE_OK);
	ASSERT_EQ(CLV_OpenMemoryFile(buffer.data(), buffer.size(), CLV_FILE_MODE_READ, &b), CLV_FILE_OK);

	uint8_t one = 0;
	size_t got = 0;
	EXPECT_EQ(CLV_ReadFile(a, &one, 1, &got), CLV_FILE_OK);
	EXPECT_EQ(one, 10);
	EXPECT_EQ(CLV_ReadFile(b, &one, 1, &got), CLV_FILE_OK) << "两把句柄各记自己的游标，互不干涉";
	EXPECT_EQ(one, 10);

	CLV_CloseFile(a);
	CLV_CloseFile(b);

	// 关闭之后宿主仍可读写那块内存：库没有 free、也没有接管
	buffer[0] = 99;
	EXPECT_EQ(CLV_OpenMemoryFile(buffer.data(), buffer.size(), CLV_FILE_MODE_READ, &a), CLV_FILE_OK);
	ASSERT_NE(a, nullptr);
	EXPECT_EQ(CLV_ReadFile(a, &one, 1, &got), CLV_FILE_OK);
	EXPECT_EQ(one, 99) << "句柄关闭不该动宿主的缓冲";
	CLV_CloseFile(a);
}

TEST(ClvFile, MemoryWriteHandleKeepsCapacityFixedAndReportsHighWater)
{
	std::vector<uint8_t> buffer(8, 0xAA);
	CLV_File* w = nullptr;
	ASSERT_EQ(CLV_OpenMemoryFile(buffer.data(), buffer.size(), CLV_FILE_MODE_WRITE, &w), CLV_FILE_OK);
	ASSERT_NE(w, nullptr);

	uint64_t size = 99;
	ASSERT_EQ(CLV_FileSize(w, &size), CLV_FILE_OK);
	EXPECT_EQ(size, 0u) << "写档句柄的 Size 是高水位，刚打开时是 0";

	size_t written = 99;
	EXPECT_EQ(CLV_WriteFile(w, "abc", 3, &written), CLV_FILE_OK);
	EXPECT_EQ(written, 3u);
	EXPECT_EQ(buffer[0], 'a');
	EXPECT_EQ(buffer[2], 'c');

	// 越界那一次整次拒：既不半写，也不扩缓冲
	size_t refused = 99;
	const CLV_FileError rc = CLV_WriteFile(w, "0123456789", 10, &refused);
	EXPECT_EQ(rc, CLV_FILE_WRITE_FAILED);
	EXPECT_EQ(refused, 0u) << "越界那次的完成量是已知的 0，不许留旧值";
	const CLV_FileError rc2 = CLV_WriteFile(w, "ab", 2, &refused);
	EXPECT_EQ(rc2, CLV_FILE_OK) << "游标仍在 3，容量内就该写得进";
	EXPECT_EQ(refused, 2u);

	ASSERT_EQ(CLV_FileSize(w, &size), CLV_FILE_OK);
	EXPECT_EQ(size, 5u) << "高水位 = 被写覆盖过的最远字节数";
	CLV_CloseFile(w);

	// 关闭之后由只读句柄读回：宿主缓冲没被库接管
	CLV_File* r = nullptr;
	ASSERT_EQ(CLV_OpenMemoryFile(buffer.data(), buffer.size(), CLV_FILE_MODE_READ, &r), CLV_FILE_OK);
	uint8_t out[3] = { 0 };
	size_t got = 0;
	EXPECT_EQ(CLV_ReadFile(r, out, sizeof(out), &got), CLV_FILE_OK);
	EXPECT_EQ(got, 3u);
	EXPECT_EQ(out[0], 'a');
	EXPECT_EQ(out[1], 'b');
	EXPECT_EQ(out[2], 'c');
	CLV_CloseFile(r);
}

TEST(ClvFile, MemoryOpenRejectsAppendAndForeignModesWithoutProducingUnsupported)
{
	std::vector<uint8_t> buffer(4, 0);
	CLV_File* h = reinterpret_cast<CLV_File*>(0x1);
	const CLV_FileError rc = CLV_OpenMemoryFile(buffer.data(), buffer.size(), CLV_FILE_MODE_APPEND, &h);
	EXPECT_EQ(rc, CLV_FILE_INVALID_ARGUMENT) << "APPEND 在内存句柄上没有对应概念，落参数档";
	EXPECT_NE(rc, CLV_FILE_UNSUPPORTED) << "值 9 自此无产出者（留白不复用）";
	EXPECT_EQ(h, nullptr);

	h = reinterpret_cast<CLV_File*>(0x1);
	EXPECT_EQ(CLV_OpenMemoryFile(buffer.data(), buffer.size(), static_cast<CLV_FileMode>(99), &h),
			  CLV_FILE_INVALID_ARGUMENT)
		<< "越界模式同档";
	EXPECT_EQ(h, nullptr);
}

TEST(ClvFile, ModeGatedFailureOnFileHandleWritesZeroOutParam)
{
	const std::string path = TempPath("clv_file_mode_gate.txt");
	std::error_code ec;
	std::filesystem::remove(path, ec);

	{
		CLV_File* w = CLV_OpenFile(path.c_str(), CLV_FILE_MODE_WRITE);
		ASSERT_NE(w, nullptr);
		CLV_CloseFile(w);
	}
	CLV_FileError err = CLV_FILE_INVALID_HANDLE;
	CLV_File* r = CLV_OpenFileEx(path.c_str(), CLV_FILE_MODE_READ, &err);
	ASSERT_NE(r, nullptr);
	ASSERT_EQ(err, CLV_FILE_OK);

	size_t written = 99;
	EXPECT_EQ(CLV_WriteFile(r, "x", 1, &written), CLV_FILE_WRITE_FAILED);
	EXPECT_EQ(written, 0u) << "门控挡住时的完成量是已知的 0：文件后端与内存后端同一条";

	uint64_t size = 99;
	EXPECT_EQ(CLV_FileSize(r, &size), CLV_FILE_OK);
	EXPECT_EQ(size, 0u);
	CLV_CloseFile(r);
	std::filesystem::remove(path, ec);
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
