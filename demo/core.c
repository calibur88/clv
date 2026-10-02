/* core.c
 *
 * 用法：在 demo_local/ 下跑 ./bin/clv_demo_core，无参数。临时文件建在当前工作目录（路径都写成 ./xxx），跑完即删。
 * 退出码 0 = 全部自动判定项通过；1 = 有失败；人工判定项（第 2、10 项）不计入退出码。
 *
 * 控制台编码由本示例自己设（见 SetupConsoleUtf8）：库只保证按 UTF-8 字节串直投日志消息，
 * 不做代码页转换——不设的话 Windows 控制台按代码页 936 重解读，中文提示全成乱码。
 */
#include "CLV_File.h"
#include "CLV_Logger.h"
#include "CLV_Memory.h"
#include "CLV_Version.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
	// 示例是消费者侧代码，直接用 Win32 控制台 API；库内的「裸宏只出现在 platform.h」约束管库不管示例
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <windows.h>
#endif

/* 演示产物用 spdlog 后端构建时才看得到日志输出；空后端（构建选项 OFF）下第 2 项全程静默。 */
#define DEMO_LOG_LEVEL_TEXT "CLV_LOG_INFO"

static const char* const kFileWrite = "./clv_demo_write.bin";
static const char* const kFileTruncate = "./clv_demo_truncate.bin";
static const char* const kFileAppend = "./clv_demo_append.bin";
static const char* const kFileSeek = "./clv_demo_seek.bin";
static const char* const kFileMissing = "./clv_demo_definitely_not_here.bin";
/* 第 10 项：UTF-8 字节序的中文文件名，源码经 /utf-8 编译，故字面量就是 UTF-8 字节 */
static const char* const kFileUtf8Name = "./clv_demo_中文路径.txt";

static int g_failed = 0;
static int g_manual = 0;

/* 应用侧义务：库只按 UTF-8 字节串直投日志，不做代码页转换，所以控制台码页要自己设成 CP_UTF8，
   否则本文件的中文提示与第 10 项的文件名会按代码页 936 重解读成乱码。每个入口都要设一次。 */
static void SetupConsoleUtf8(void)
{
#if defined(_WIN32)
	if (SetConsoleOutputCP(CP_UTF8) == 0) printf("[warn] SetConsoleOutputCP(CP_UTF8) failed\n");
	SetConsoleCP(CP_UTF8);	  // 输入侧一并设上，保持与输出一致
#endif
}

static void Report(int item, const char* name, const char* expect, const char* actual, int passed)
{
	printf("[#%02d] %-26s | expect: %-34s | actual: %-26s | %s\n", item, name, expect, actual,
		   passed ? "PASS" : "FAIL");
	if (passed == 0) g_failed++;
}

static void Manual(int item, const char* name, const char* what)
{
	printf("[#%02d] %-26s | 人工判定：%s\n", item, name, what);
	g_manual++;
}

/* 写一个文件并关闭；返回是否成功。mode 决定是新建 / 清空 / 追加。 */
static int WriteAll(const char* path, CLV_FileMode mode, const void* data, size_t size)
{
	CLV_File* handle = CLV_OpenFile(path, mode);
	if (handle == NULL) return 0;

	const int ok = CLV_WriteFile(handle, data, size, NULL) == CLV_FILE_OK;
	CLV_CloseFile(handle);
	return ok;
}

/* 读整个文件到 buf（不含保证的 NUL 结尾由调用方给足空间），返回读到的字节数；打不开返回 0 且 *err 给出码。 */
static size_t ReadAll(const char* path, void* buf, size_t cap, CLV_FileError* err)
{
	CLV_File* handle = CLV_OpenFileEx(path, CLV_FILE_MODE_READ, err);
	if (handle == NULL) return 0;

	size_t got = 0;
	const int code = CLV_ReadFile(handle, buf, cap, &got);
	CLV_CloseFile(handle);
	if (code != CLV_FILE_OK) return 0;
	return got;
}

static uint64_t SizeOf(const char* path)
{
	CLV_File* handle = CLV_OpenFile(path, CLV_FILE_MODE_READ);
	if (handle == NULL) return 0;

	uint64_t size = 0;
	CLV_FileSize(handle, &size);
	CLV_CloseFile(handle);
	return size;
}

static void CheckVersion(void)
{
	printf("CLV version string : %s\n", CLV_VersionString());
	printf("CLV version triple : %d.%d.%d\n", CLV_VersionMajor(), CLV_VersionMinor(), CLV_VersionPatch());
	Report(1, "版本字符串", CLV_VERSION_STRING, CLV_VersionString(),
		   strcmp(CLV_VersionString(), "0.0.1-dev") == 0 && CLV_VersionMajor() == 0 && CLV_VersionMinor() == 0 &&
			   CLV_VersionPatch() == 1);
}

static void CheckLogLevelFilter(void)
{
	CLV_LogSetLevel(CLV_LOG_INFO);
	/* 库写 stdout 与自己 printf 的缓冲不同步：标记行打完立刻冲一道，日志才肯落在标记之间 */
	printf("--- 以下两行由库写出，DEBUG 那行不该出现 ---\n");
	fflush(stdout);
	CLV_Log(CLV_LOG_DEBUG, "clv_demo: this DEBUG line must be filtered out");
	CLV_Log(CLV_LOG_INFO, "clv_demo: this INFO line must appear");
	printf("--- 以上两行之间只应有一条 INFO ---\n");
	fflush(stdout);
	Manual(2, "日志级别过滤", "过滤等级设为 " DEMO_LOG_LEVEL_TEXT "，上面应只有 INFO 行、没有 DEBUG 行");
}

static void CheckWriteThenFlush(void)
{
	static const char payload[] = "clv flush check";
	const size_t len = sizeof(payload) - 1;

	int ok = WriteAll(kFileWrite, CLV_FILE_MODE_WRITE, payload, len);
	const uint64_t size = ok ? SizeOf(kFileWrite) : 0;

	char expect[32];
	char actual[32];
	sprintf(expect, "size == %lu", (unsigned long) len);
	sprintf(actual, "size == %lu", (unsigned long) size);
	Report(3, "写后刷盘", expect, actual, ok && size == len);
}

static void CheckWriteCreatesFile(void)
{
	remove(kFileTruncate); /* 保证起点是「不存在」 */

	int ok = WriteAll(kFileTruncate, CLV_FILE_MODE_WRITE, "fresh", 5);
	const uint64_t size = ok ? SizeOf(kFileTruncate) : 0;

	char actual[32];
	sprintf(actual, "created=%d size=%lu", ok, (unsigned long) size);
	Report(4, "Write 模式建文件", "不存在的路径能建起来", actual, ok && size == 5);
}

static void CheckWriteTruncates(void)
{
	int ok = WriteAll(kFileTruncate, CLV_FILE_MODE_WRITE, "01234567890123456789", 20);
	ok = ok && WriteAll(kFileTruncate, CLV_FILE_MODE_WRITE, "short", 5);
	const uint64_t size = ok ? SizeOf(kFileTruncate) : 0;

	char buf[32];
	memset(buf, 0, sizeof(buf));
	const size_t got = ReadAll(kFileTruncate, buf, sizeof(buf) - 1, NULL);

	char actual[48];
	sprintf(actual, "size=%lu content=%s", (unsigned long) size, buf);
	Report(5, "Write 模式清空", "size=5 content=short（旧内容不残留）", actual,
		   ok && size == 5 && got == 5 && strcmp(buf, "short") == 0);
}

static void CheckAppendKeepsContent(void)
{
	int ok = WriteAll(kFileAppend, CLV_FILE_MODE_WRITE, "first", 5);
	ok = ok && WriteAll(kFileAppend, CLV_FILE_MODE_APPEND, "-second", 7);

	char buf[32];
	memset(buf, 0, sizeof(buf));
	const size_t got = ReadAll(kFileAppend, buf, sizeof(buf) - 1, NULL);

	char actual[48];
	sprintf(actual, "size=%lu content=%s", (unsigned long) SizeOf(kFileAppend), buf);
	Report(6, "Append 模式追加", "size=12 content=first-second", actual,
		   ok && got == 12 && strcmp(buf, "first-second") == 0);
}

static void CheckSeekRandomRead(void)
{
	/* 偏移 6 起读 5 字节，前三个字节必须是 CLV，即读出 "CLVXY" */
	static const char payload[] = "123456CLVXY";
	int ok = WriteAll(kFileSeek, CLV_FILE_MODE_WRITE, payload, sizeof(payload) - 1);

	CLV_File* handle = CLV_OpenFile(kFileSeek, CLV_FILE_MODE_READ);
	if (handle == NULL)
	{
		Report(7, "Seek 随机读", "offset=6 len=5 -> CLVXY", "open failed", 0);
		return;
	}

	uint64_t position = 0;
	ok = ok && CLV_SeekFile(handle, 6, &position) == CLV_FILE_OK && position == 6;

	char buf[8];
	memset(buf, 0, sizeof(buf));
	size_t got = 0;
	ok = ok && CLV_ReadFile(handle, buf, 5, &got) == CLV_FILE_OK && got == 5;
	CLV_CloseFile(handle);

	buf[got] = '\0';
	char actual[48];
	sprintf(actual, "pos=%lu read=%s", (unsigned long) position, buf);
	Report(7, "Seek 随机读", "pos=6 read=CLVXY", actual, ok && strcmp(buf, "CLVXY") == 0);
}

static void CheckOpenFailureReportsError(void)
{
	remove(kFileMissing);

	CLV_FileError err = CLV_FILE_OK;
	CLV_File* handle = CLV_OpenFileEx(kFileMissing, CLV_FILE_MODE_READ, &err);

	char actual[48];
	sprintf(actual, "handle=%s err=%s", handle == NULL ? "NULL" : "non-null", CLV_FileStrError(err));
	Report(8, "OpenEx 失败上报", "handle=NULL err=open failed", actual, handle == NULL && err == CLV_FILE_OPEN_FAILED);
}

static void CheckReallocSemantics(void)
{
	static const char seed[] = "01234567"; /* 8 字节，不含结尾 */

	unsigned char* block = (unsigned char*) CLV_Alloc(8);
	int ok = block != NULL;
	if (ok) memcpy(block, seed, 8);

	unsigned char* grown = (unsigned char*) CLV_Realloc(block, 4096);
	ok = ok && grown != NULL;
	if (ok)
	{
		ok = ok && memcmp(grown, seed, 8) == 0; /* 扩容必须保住原前缀 */
		grown[4095] = 7;						/* 新容量真的可用 */
	}
	if (grown != NULL) CLV_Free(grown);

	Report(9, "Realloc 语义", "前 8 字节不变且可写 4096", ok ? "前缀保留 + 新容量可用" : "失败", ok);
}

static void CheckUtf8Path(void)
{
	/* 契约：路径是 UTF-8 字节串。Windows 后端转 UTF-16 走 W 系列，中文名才不会被按代码页重解释。
	   这里刻意不删该文件，留给人对着目录看文件名是否正确；README 的清理步骤负责删。 */
	int ok = WriteAll(kFileUtf8Name, CLV_FILE_MODE_WRITE, "utf8", 4);
	const uint64_t size = ok ? SizeOf(kFileUtf8Name) : 0;

	char actual[32];
	sprintf(actual, "size=%lu", (unsigned long) size);
	Report(10, "中文路径（UTF-8）", "能建文件且大小=4；文件名见目录", actual, ok && size == 4);

	printf("[#10] 中文路径（UTF-8）        | 人工判定：到当前目录看 clv_demo_中文路径.txt 的字形，不乱码即通过\n");
	printf("       注：本示例已在开头 SetConsoleOutputCP(CP_UTF8)，所以这里控制台上的中文名也该是正的；\n");
	printf("           换到你自己的程序里若又变乱码，那是应用侧没设码页，不是库把路径改坏了。\n");
	g_manual++;
}

static void CleanupScratch(void)
{
	remove(kFileWrite);
	remove(kFileTruncate);
	remove(kFileAppend);
	remove(kFileSeek);
	remove(kFileMissing);
	/* kFileUtf8Name 不删：中文文件名本身要留在目录里核对字形 */
}

int main(void)
{
	SetupConsoleUtf8();

	printf("=== CLV demo / 人工复检（10 项）===\n");
	printf("工作目录下的临时文件：clv_demo_*.bin 跑完自动删，中文名的留在原地待清理\n\n");

	CheckVersion();
	printf("\n");
	CheckLogLevelFilter();
	printf("\n");
	CheckWriteThenFlush();
	CheckWriteCreatesFile();
	CheckWriteTruncates();
	CheckAppendKeepsContent();
	CheckSeekRandomRead();
	CheckOpenFailureReportsError();
	CheckReallocSemantics();
	CheckUtf8Path();

	CleanupScratch();

	printf("\n=== 自动判定失败 %d 项，人工判定 %d 项 ===\n", g_failed, g_manual);
	if (g_failed != 0) return 1;
	return 0;
}
