# CLV 功能展示（demo）

> 本目录放示例源码与本说明，**不放构建产物**（产物落 `build/` 与本地 `demo_local/`）。
> 当前文档版本：0.2.0-dev；示例打印的工程版本是 `0.0.2-dev`。容器面 `CLV_Container.h` 仍标 v0 实验性，ABI 未冻结。

**项目速记**：

- `CLV_Version`：版本查询，字符串带预发布后缀；
- `CLV_File`：文件读写，不透明句柄 + 三种打开模式 + 错误码出参；
- `CLV_Logger`：日志，UTF-8 字节串直投，带等级过滤；
- `CLV_Memory`：分配口，直通 C 运行时；
- `CLV_Container`：**v0 实验性**，容器读写面——写方 `OpenFile → AddStream → AddExtBlock → WriteFrame → Finish`，读方 `OpenFile → NextFrame` + 结构出参；
- 完整口径见仓库根 `README.md` 与 `include/` 下的六个公开头。

## 示例数据（目录名）

| 文件夹 / 文件 | 内容 |
|---|---|
| `core.c` | C 程序，只用核心面的四个头（`CLV_File` / `CLV_Logger` / `CLV_Memory` / `CLV_Version`），按 `#01`~`#10` 编号逐条打印「预期 / 实测 / 判定」；可执行名 `clv_demo_core` |
| `container.c` | C 程序，容器面只用 `CLV_Container.h`（`info` 子命令读裸文件时另用了 `CLV_File.h` 的四个函数），现场写一个真 `.clv` 再读回逐条判定（含 `#04` 写方平移义务的可视行）；可执行名 `clv_demo_container` |
| `CMakeLists.txt` | demo 的构建入口，默认不建（`CLV_BUILD_DEMOS=OFF`）。产物落 `demo_local/bin/`（**每次构建前先按清单清空该目录**），并把 `core.c` / `container.c` / 本手册同步一份副本到 `demo_local/src/`；示例跑出来的文件落在 `demo_local/` 根上，不跟着 `bin/` 一起被清 |

两个可执行名与源文件名不同步（`clv_demo_core ← core.c`、`clv_demo_container ← container.c`），配对写在 `CMakeLists.txt` 的 `add_executable` 里。

**运行位置是 `demo_local/`，不是 `demo_local/bin/`**：可执行名带 `bin/` 前缀跑（`./bin/clv_demo_core.exe`），
示例自己生成的临时文件与 `.clv` 就落在 `demo_local/` 根上。代码里这些路径一律写成 `./xxx`，明确是「当前工作目录下的」；
放进 `bin/` 会被下一次构建开头的清空步骤连带删掉。

## 核心面示例（core.c，1 个程序 / 10 个展示点）

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

printf("%s\n", CLV_VersionString());   /* 0.0.2-dev */
printf("%d.%d.%d\n", CLV_VersionMajor(), CLV_VersionMinor(), CLV_VersionPatch());   /* 0.0.2 */
```

命中：`0.0.2-dev` 与 `0.0.2` 各一行。

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

## 容器层示例（container.c，1 个程序 / 10 项自动判定）

**用途**：容器层的人工验证示例——不替代 `tests/` 的自动化断言，而是产出一个可打开、可 dump、可肉眼核对的真 `.clv`。
文件写在当前工作目录（默认 `./clv_demo_container.clv`，可用 `argv[1]` 覆盖），写完留在原地给人看。

写入的内容：视频流 `id0`（timebase 1/1081080000、`max_packet_size` 4096）+ 字幕流 `id1`（`max_packet_size` 1024，挂 `ext_type = 4` 样式块）；
四帧，其中视频首帧**源 dts = −40**（B 帧前瞻）、载荷 1000 B 按 `fragment_chunk_size = 300` 切成 4 片。

| 判定项 | 考察点 |
|---|---|
| `#01` 文件头 magic / 版本 | 64 B 头写回后字段齐全 |
| `#02` 流描述符读回 | 双流、timebase、`stream_type` 逐条对得上 |
| `#03` 字幕 `ext_type = 4` 样式块 | 扩展块链写读逐字节等 |
| `#04` 首包 dts 归零 | **写方平移义务**：源 dts −40 → 落盘 dts_delta 0，`pts_delta = 80`。附一行可视输出说明换算 |
| `#05` 分片重组完整帧 | 每片自带完整外层头 + 片序连续性，1000 B 四片重组 |
| `#06` 索引条目数与帧数 | 每帧一条索引 |
| `#07` 索引跨流统一时间轴 | 按统一时间轴升序，相等时按 `stream_id` tie-break |
| `#08` 包数一致 | 文件头 `total_packets` 与读方成功计数 |
| `#09` 容错计数干净 | CRC / 字段 / 超限 / 无名流 / 重同步 / 残帧 全 0 |
| `#10` 文件大小合理 | 非零，供人工看一眼 |

`#10` 之外另有一项**人工判定**：直接拿十六进制编辑器打开产出的 `.clv`，或跑 `info` 子命令。

**子命令 `info <path>`**：只读并 dump 结构（头 / 描述符 / 扩展块 / 索引 / 读方统计 / 帧列表），不写文件。
它同时是 `assets/fixtures/` 三份素材的交叉验证入口——那三份由 `script/gen_fixtures.py`（独立 Python 实现）写出，
能被本库的读方正确解析，说明两侧都没偏离规范：

```bash
# info 是只读子命令，不写文件，所以从仓库根跑没问题
./demo_local/bin/clv_demo_container.exe info assets/fixtures/neg_dts.clv
```

**路径口径**：`CLV_ContainerWriterOpenFile` / `CLV_ContainerReaderOpenFile` 收的是项目路径契约——UTF-8、**只用正斜杠 `/`**，
出现反斜杠即拒绝。容器面把文件层失败统一收敛为 `CLV_CONTAINER_IO_FAILED`，不回传文件面码值，
所以写错分隔符时只会看到 `io failed`，**不会有一个专门表示路径非法的错误码**。路径一律写成 `assets/fixtures/x.clv` 这种正斜杠形式。

**ABI 覆盖**：`include/CLV_Container.h` 的 19 个 `CLV_API` 函数在本示例里**全部被真实调用过一次**（写 6 / 读 12 / `StrError` 1），
可作为 ABI 冻结前核覆盖缺口的依据。

## 操作

1. **适用范围**：两个示例都只用 `include/` 下的公开头（核心面四个 + 容器面一个），程序按 C 编译。
2. **工程设置**：需要 CMake ≥ 3.16、Ninja、MSVC x64 环境；两个开关——`CLV_BUILD_DEMOS=ON` 建示例，`CLV_LOGGER_USE_SPDLOG=ON` 才有日志输出（`clv_demo_core` 的 `#02` 判据要用它）。
3. **安装步骤**：先 `python script/install_third_party.py` 把第三方装到 `build/_install`，再配置与构建（命令见下面「开发」），产物落在 `demo_local/bin/` 下的 `clv_demo_core.exe` 与 `clv_demo_container.exe`。
4. **验收**：在 `demo_local/` 目录下依次执行 `./bin/clv_demo_core.exe` 与 `./bin/clv_demo_container.exe`，它们生成的文件都建在当前工作目录（也就是 `demo_local/` 根）。
5. **确认**：逐行对照两个程序各自 `#01`~`#10` 的 `expect` 与 `actual`，末行都应是「自动判定失败 0 项」，退出码 0。
   核心面的 `#02`（有没有 DEBUG 行）与 `#10`（中文名文件字形）含人工判定；容器面的 `#04` 会额外打印一行平移算式（源 dts −40 → 落盘 0，`pts_delta = 80`），`#10` 之外的「结构是否顺眼」为人工判定。
6. **清理**：`clv_demo_core` 自动删除自己建的 `./clv_demo_*.bin`，但 `./clv_demo_中文路径.txt` 留着——中文文件名本身要给人核对字形，看完手动删；
   `clv_demo_container` 写完的 `./clv_demo_container.clv` 也留着给人开，不自动删。`demo_local/bin/` 由构建每次清空，`src/` 副本与这两个留观文件都不受清空影响。整个 `demo_local/` 不入库，删掉可再生。

## 开发

```bash
# 在 vcvars64 起来的 x64 开发环境里执行（cl 与 INCLUDE 必须同进程）
python script/install_third_party.py                 # 第三方源码 -> build/_install，已装则跳过
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCLV_BUILD_DEMOS=ON -DCLV_LOGGER_USE_SPDLOG=ON # 配置：开示例与真实日志后端
cmake --build build                                  # 先清空 demo_local/bin 再生成两个 exe，并同步 src/ 副本
cd demo_local
./bin/clv_demo_core.exe; echo exit=$?                # 退出码 0 即自动项全过
./bin/clv_demo_container.exe; echo exit=$?           # 同上，跑出来的 .clv 落在 demo_local/ 根
```

两个 demo 目标都在仓库统一的告警档（`/W4 /permissive-`，且 `CLV_WARNINGS_AS_ERRORS=ON` 时 `/WX`）下编译，
与库和测试同一标准，不允许「示例豁免告警」。
