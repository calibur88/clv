# CLV

> 本文是 CLV 仓库的入口说明。当前文档版本：0.1.0-dev · 日期：2026-10-02。

CLV = **C++ Lightweight Video**：自研轻量视频容器 + 自研视频编解码 + 自研字幕格式。
公开面是 **C ABI**：不透明句柄 + 自由函数 + 错误码枚举。
已落地能力：文件读写、日志、内存分配、版本查询，以及**容器读写面（v0 实验性）**。视频 / 音频编解码与字幕格式尚未开工。

## 安装

第三方源码在 `third-party/`（spdlog、googletest），先装到 `build/_install`：

```bash
python script/install_third_party.py          # 已装则跳过，--force 重装
```

构建走 **Ninja + x64**，配置与编译必须在 `vcvars64.bat` 起来的同一个进程里跑：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

集成方式二选一：`add_subdirectory()` 引入源码，或直接链 `build/clv.lib` 并把 `include/` 加进头文件搜索路径。公开头共六个，全在 `include/`。

## 快速开始

```c
#include <stdio.h>

#include "CLV_File.h"

int main(void)
{
	CLV_FileError err = CLV_FILE_OK;
	CLV_File* h = CLV_OpenFileEx("notes.txt", CLV_FILE_MODE_READ, &err);
	if (h == NULL)
	{
		printf("open failed: %s\n", CLV_FileStrError(err));
		return 1;
	}

	char buf[64];
	size_t got = 0;
	CLV_ReadFile(h, buf, sizeof(buf) - 1, &got);
	buf[got] = '\0';
	printf("%s", buf);

	CLV_CloseFile(h);
	return 0;
}
```

Windows 控制台上要正确显示中文，调用方先设码页：`SetConsoleOutputCP(CP_UTF8)` 与 `SetConsoleCP(CP_UTF8)`，做法见 `demo/core.c` 与 `demo/container.c`。

## 公开接口

| 头 | 函数 | 一句话 |
|---|---|---|
| `CLV_Version.h` | `CLV_VersionString` / `CLV_VersionMajor` / `CLV_VersionMinor` / `CLV_VersionPatch` | 版本查询，字符串带预发布后缀 |
| `CLV_File.h` | `CLV_OpenFile` / `CLV_OpenFileEx` / `CLV_ReadFile` / `CLV_WriteFile` / `CLV_SeekFile` / `CLV_FileSize` / `CLV_FlushFile` / `CLV_CloseFile` / `CLV_OpenMemoryFile` / `CLV_FileStrError` | 文件读写：三模式打开、顺序与随机读、取长度、刷盘、错误码文案 |
| `CLV_Logger.h` | `CLV_Log` / `CLV_LogSetLevel` / `CLV_LogSetStorage` | 日志：UTF-8 字节串直投 + 等级过滤 |
| `CLV_Memory.h` | `CLV_Alloc` / `CLV_Realloc` / `CLV_Free` | 分配口，直通 C 运行时，失败返回 NULL |
| `CLV_Export.h` | `CLV_API`（宏） | 静态库下为空；`CLV_BUILD_SHARED` / `CLV_USE_SHARED` 切 dllexport / dllimport |
| `CLV_Container.h` | 写方 6 个（`…WriterOpenFile` / `AddStream` / `AddExtBlock` / `WriteFrame` / `Finish` / `Close`）+ 读方 12 个（`…ReaderOpenFile` / `NextFrame` / `StreamCount` / `GetStream` / `Stats` / `HasIndex` / `IndexCount` / `GetHead` / `GetIndexEntry` / `ExtBlockCount` / `GetExtBlock` / `Close`）+ `CLV_ContainerStrError` | 容器读写：**v0 实验性**。不透明句柄 + 整帧进出，头注释即契约正文 |

## 使用约定

| 项 | 口径 |
|---|---|
| 路径 | UTF-8 字节序（不含 BOM）；分隔符只认 `/`；NULL、空串、含 `\` 的路径返回 `CLV_FILE_INVALID_PATH`；长路径不处理，由调用方预处理 |
| Windows 侧 | UTF-8 转 UTF-16 后走 W 系列 API；字节序不是合法 UTF-8 时返回 `CLV_FILE_OPEN_FAILED` |
| 三模式 | 语义逐一对齐 POSIX `rb` / `wb` / `ab`：READ 要求文件已存在、游标在 0；WRITE 建文件，已存在则清空；APPEND 建文件，写入恒落末尾 |
| 读与游标 | 短读返回 `CLV_FILE_OK`，实际字节数走 `bytes_read`；到文件尾读到 0 字节同样返回 `CLV_FILE_OK`；请求 0 字节返回 `CLV_FILE_OK` 且 `bytes_read` 为 0，**不用它判文件尾**；剩余字节用 `CLV_FileSize` 减调用方自持的游标 |
| Seek / Size / Flush | Seek 是绝对偏移，越过文件尾合法，其后 Read 返回 0 字节；Size 与游标无关；只读句柄的 Flush 返回 `CLV_FILE_OK` 且无效果 |
| 能力门控 | 只有 Read / Write 受打开模式限制，越界分别得 `WRITE_FAILED` / `READ_FAILED`；Seek / Size / Flush 三种模式下都可用 |
| 出参 | 出参指针可为 NULL；返回非 `CLV_FILE_OK` 时不写出参 |
| 错误表达 | 打开失败的细分走 `CLV_OpenFileEx` 的 `err` 出参（`CLV_OpenFile` 忽略它）；其余操作返回 `CLV_FileError`；文案走 `CLV_FileStrError`，返回静态存储期的 ASCII 串，未知值返回 `"unknown"` |
| 日志 | `CLV_Log` 无返回值，日志失败不向调用方报告；消息按 UTF-8 字节串直投，不解析 `{}` 与 `%s`；默认过滤等级 INFO；越界等级整条丢弃；`CLV_LogSetStorage` 当前无效果 |
| 内存 | 分配失败返回 NULL，判空由调用方做；`CLV_Free(NULL)` 是合法 no-op |
| 并发 | 只保证单实例单次调用原子；跨实例、跨线程的并发语义未定义，调用方自行同步 |
| 句柄 | 由 `CLV_CloseFile` 释放；对 NULL 句柄的任何调用返回 `CLV_FILE_INVALID_HANDLE`，`CLV_CloseFile(NULL)` 是合法 no-op；已关闭的句柄再使用是未定义行为 |
| 未就绪入口 | `CLV_OpenMemoryFile` 返回 `CLV_FILE_UNSUPPORTED` 且 `*out_handle` 为 NULL |
| 容器面错误码 | `CLV_ContainerError` 与 `CLV_FileError` 是**两套编号**，不得跨头比对数值；文件面失败在容器口统一收敛为 `CLV_CONTAINER_IO_FAILED`，不回传文件面码值 |
| 容器面时间轴 | `WriteFrame` 收该流原始时间轴上的绝对 dts / pts（可为负），写方按首帧平移到 0 落盘、读方不承担平移；`pts_delta` 是「本帧 PTS − 本帧 DTS」，用 zigzag 有符号 varint 承载，允许为负 |
| 容器面线程 | 句柄不跨线程使用，跨实例并发语义未定义 |

## 构建与验收

| 开关 | 默认 | 作用 |
|---|---|---|
| `CLV_BUILD_SHARED` | OFF | 建动态库（同时给使用侧带 `CLV_USE_SHARED`） |
| `CLV_LOGGER_USE_SPDLOG` | OFF | 日志后端选 spdlog；OFF 时用空后端，所有日志被丢弃 |
| `CLV_BUILD_DEMOS` | OFF | 建 `demo/` 的两个示例，可执行落 `demo_local/bin/`（每次构建先清空再投递），并把示例源码同步一份到 `demo_local/src/` |
| `CLV_WARNINGS_AS_ERRORS` | OFF | 自有 target 警告升级为错误 |

源文件编码由构建统一指定（MSVC `/utf-8`，其它 `-finput-charset=UTF-8`）。自有 target 的警告级别是 MSVC `/W4 /permissive-`，第三方不参与。

验收基线（Windows + MSVC + Ninja x64）：

| 项 | 结果 |
|---|---|
| clean 重建 | 0 warning |
| `ctest` | 89/89；`CLV_LOGGER_USE_SPDLOG` 的 ON 与 OFF 两种配置都跑 |
| 第三方接入 | `find_package`（用 `build/_install`）与源码 `add_subdirectory` 回退两条路都可用 |
| 格式自查 | `python script/format_all.py`（C/C++ 走仓库根 `.clang-format`，Python 走 black） |
| 示例 | `CLV_BUILD_DEMOS=ON` 构建后，在 `demo_local/` 下跑 `./bin/clv_demo_core.exe` 与 `./bin/clv_demo_container.exe`，退出码 0 表示自动判定项全过；人工项见 `demo/README.md` |

## 版本口径

| 轨道 | 值 | 出现位置 |
|---|---|---|
| 工程版本 | `0.0.1`，字符串 `0.0.1-dev` | `include/CLV_Version.h` 的四个宏（单一来源）、CMake `project(clv VERSION 0.0.1)`、`CLV_VersionString()` 的返回值 |
| 文档版本 | `0.1.0-dev` | 设计文档与本 README 的抬头声明，不进可执行代码 |

## 授权

仓库根 `LICENSE` 是本项目的许可条款全文：**非商业授权协议（非商业源码可见许可）**，非 OSI 开源许可。

- 非商业使用者（个人、教育机构、非营利组织及其他不以营利为目的的主体）拿到与 Apache-2.0 同范围的权利，但**不含**销售、再许可、商业集成、商业部署等商业权利；
- 商业用途需事先获得作者书面授权，联系方式见 `LICENSE` 第二条；免责声明见第三条；
- 许可只以根 `LICENSE` 为准，单个源码文件不带许可头；
- `third-party/` 下各库保留自己的许可证（spdlog 为 MIT、googletest 为 BSD-3-Clause），本许可不覆盖它们。

## 文档索引

| 文档 | 内容 |
|---|---|
| `README.md` | 本文件：安装、快速开始、公开接口、使用约定、构建与验收 |
| `ARCHITECTURE.md` | 工程结构、分层与数据流、构建系统组织方式、当前未落地项 |
| `CHANGE.log` | 版本演进记录与版本兼容声明 |
| `include/CLV_*.h` | 六个公开头的注释即接口契约正文（`CLV_Container.h` 标 v0 实验性） |
| `assets/README.md` | 入库测试素材的内容表、确定性要求与冻结值 |
| `demo/README.md` | 示例库：预置内容、可复制示例、运行与验收步骤 |
| `AI编写文档通用模板.md`、`AI编写项目架构通用模板.md`、`AI通用测试文档模板.md` | 协作方编写文档时使用的模板与红线 |
