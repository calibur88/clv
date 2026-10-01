# CLV 功能展示（demo）

> 本目录放示例源码与本说明，**不放构建产物**（产物落 `build/` 与本地 `demo_local/`）。
> 当前文档版本：0.1.0-dev；示例打印的工程版本是 `0.0.1-dev`。

**项目速记**：

- `CLV_Version`：版本查询，字符串带预发布后缀；
- `CLV_File`：文件读写，不透明句柄 + 三种打开模式 + 错误码出参；
- `CLV_Logger`：日志，UTF-8 字节串直投，带等级过滤；
- `CLV_Memory`：分配口，直通 C 运行时；
- 完整口径见仓库根 `README.md` 与 `include/` 下的五个公开头。

## 示例数据（目录名）

| 文件夹 / 文件 | 内容 |
|---|---|
| `clv_demo.c` | 单个 C 程序，只用 `include/` 下的五个公开头，按 `#01`~`#10` 编号逐条打印「预期 / 实测 / 判定」 |
| `CMakeLists.txt` | demo 的构建入口，默认不建（`CLV_BUILD_DEMOS=OFF`），产物固定输出到 `demo_local/bin/` |

## 预置内容（1 个程序，覆盖 10 个展示点）

- **身份**（1 项）：版本字符串与三段数字；
- **日志**（1 项）：等级过滤（设 `CLV_LOG_INFO` 后 DEBUG 被丢弃）；
- **文件写**（3 项）：写后刷盘、Write 模式建文件、Write 模式清空已有内容；
- **文件读写语义**（2 项）：Append 模式追加到末尾、Seek 绝对偏移后随机读；
- **错误与内存**（3 项）：`CLV_OpenFileEx` 失败时错误码被填、`CLV_Realloc` 保住前缀、UTF-8 中文路径建文件。

易混淆对照：

| 展示点 | 用的模式 | 契约要点 |
|---|---|---|
| 写后刷盘 | `WRITE` | 只读句柄的 `Flush` 是 no-op 且返回 `CLV_FILE_OK` |
| Write 模式清空 | `WRITE` | 对齐 POSIX `"wb"`：已有内容不残留 |
| Append 模式追加 | `APPEND` | 对齐 POSIX `"ab"`：写入恒定落末尾，游标也在末尾 |
| Seek 随机读 | `READ` | 绝对偏移；Seek 到文件尾之后合法，其后 Read 返回 0 字节且不算失败 |

## 可复制示例（按主题分节）

**版本查询**

```c
#include "CLV_Version.h"

printf("%s\n", CLV_VersionString());   /* 0.0.1-dev */
printf("%d.%d.%d\n", CLV_VersionMajor(), CLV_VersionMinor(), CLV_VersionPatch());   /* 0.0.1 */
```

命中：`0.0.1-dev` 与 `0.0.1` 各一行。

**打开文件并读取全部（含错误码出参）**

```c
#include "CLV_File.h"

CLV_FileError err = CLV_FILE_OK;
CLV_File* h = CLV_OpenFileEx("data/clip.bin", CLV_FILE_MODE_READ, &err);
if (h == NULL)
{
	printf("open failed: %s\n", CLV_FileStrError(err));
	return;
}

uint64_t size = 0;
CLV_FileSize(h, &size);
char buf[64];
size_t got = 0;
CLV_ReadFile(h, buf, sizeof(buf), &got);   /* 短读不是失败，实际字节数在 got */
CLV_CloseFile(h);
```

命中：路径不存在时打印 `open failed: open failed`，且 `h == NULL`。变体：把 `CLV_FILE_MODE_READ` 换成 `CLV_FILE_MODE_WRITE` 即「建文件或清空」，换成 `CLV_FILE_MODE_APPEND` 即「建文件并落到末尾」。

**随机读：绝对偏移 + 读一段**

```c
CLV_File* h = CLV_OpenFile("clv_demo_seek.bin", CLV_FILE_MODE_READ);
uint64_t position = 0;
CLV_SeekFile(h, 6, &position);          /* 绝对偏移，落点在 position 里 */
char buf[8] = {0};
size_t got = 0;
CLV_ReadFile(h, buf, 5, &got);          /* got == 5 */
CLV_CloseFile(h);
```

命中：`position == 6`、`got == 5`、`buf` 内容为 `CLVXY`（文件内容是 `123456CLVXY`）。

**日志与等级过滤**

```c
#include "CLV_Logger.h"

CLV_LogSetLevel(CLV_LOG_INFO);
CLV_Log(CLV_LOG_DEBUG, "this must be filtered out");   /* 不输出 */
CLV_Log(CLV_LOG_INFO, "this must appear");             /* 输出，{} 与 %s 按字面量 */
```

命中：只有一条 `[info]` 行，消息里的 `{}`、`%s` 不被当格式占位符解析。变体：`CLV_LOGGER_USE_SPDLOG=OFF` 时两条都不输出。

**控制台编码**

```c
#if defined(_WIN32)
	#include <windows.h>
#endif

static void SetupConsoleUtf8(void)
{
#if defined(_WIN32)
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
#endif
}

int main(void)
{
	SetupConsoleUtf8();
	/* ... */
}
```

命中：控制台上的中文提示与中文文件名正常显示。

**内存：分配 + 扩容**

```c
#include "CLV_Memory.h"

unsigned char* p = (unsigned char*)CLV_Alloc(8);
memcpy(p, "01234567", 8);
unsigned char* q = (unsigned char*)CLV_Realloc(p, 4096);   /* 前 8 字节原样保留 */
q[4095] = 7;
CLV_Free(q);
```

命中：`memcmp(q, "01234567", 8) == 0` 且新容量可写。变体：`CLV_Realloc(NULL, n)` 等价于 `CLV_Alloc(n)`；`CLV_Free(NULL)` 是合法 no-op。

## 操作

1. **适用范围**：示例只用 `include/` 下的五个公开头，程序按 C 编译。
2. **工程设置**：需要 CMake ≥ 3.16、Ninja、MSVC x64 环境；两个开关——`CLV_BUILD_DEMOS=ON` 建示例，`CLV_LOGGER_USE_SPDLOG=ON` 才有日志输出（`#02` 的判据要用它）。
3. **安装步骤**：先 `python script/install_third_party.py` 把第三方装到 `build/_install`，再配置与构建（命令见下面「开发」），产物落在 `demo_local/bin/clv_demo.exe`。
4. **验收**：切到 `demo_local/bin/` 目录执行 `clv_demo.exe`，临时文件建在当前工作目录。
5. **确认**：逐行对照 `#01`~`#10` 的 `expect` 与 `actual`，末行应打印「自动判定失败 0 项」，进程退出码为 0；`#02`（有没有 DEBUG 行）与 `#10`（中文名文件字形）含人工判定。
6. **清理**：程序自动删除 `demo_local/bin/` 下的 `clv_demo_*.bin`；`clv_demo_中文路径.txt` 由人看完手动删除。

## 开发

```bash
# 在 vcvars64 起来的 x64 开发环境里执行（cl 与 INCLUDE 必须同进程）
python script/install_third_party.py                 # 第三方源码 -> build/_install，已装则跳过
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCLV_BUILD_DEMOS=ON -DCLV_LOGGER_USE_SPDLOG=ON # 配置：开示例与真实日志后端
cmake --build build                                  # 产物：demo_local/bin/clv_demo.exe
cd demo_local/bin && ./clv_demo.exe; echo exit=$?    # 跑一遍，退出码 0 即自动项全过
```
