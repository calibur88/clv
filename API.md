# CLV 公开接口（C ABI）

> 本文是 CLV 公开 C ABI 的权威说明：头文件分工、句柄与错误码约定、逐个函数的入参出参语义、可复制示例与验收步骤。  
> 文档版本：0.2.0-dev · 日期：2026-10-03 · 工程版本以 `include/CLV_Version.h` 的四个宏为单一来源。  
> 字节格式规则见 `docs/容器规范.md`；分层与依赖见 `ARCHITECTURE.md`；头文件里的注释与本表同为契约正文，两边不一致时以本文与头文件为准并回报缺陷。

---

## 1 形态与通用约定

| 项 | 约定 |
|---|---|
| 语言绑定 | 全部公开符号 `extern "C"`，C 与 C++ 都能直接调；C++ 只存在于实现层 |
| 符号前缀 | `CLV_`；可见性由 `CLV_Export.h` 的 `CLV_API` 决定：静态库下为空宏，`CLV_BUILD_SHARED` / `CLV_USE_SHARED` 切 `dllexport` / `dllimport` |
| 对象形态 | **不透明句柄**（`typedef struct CLV_X CLV_X;`）+ 自由函数 + 整型错误码；C++ 类型不跨边界，接口上没有异常，失败一律走返回码 |
| 错误码 | 返回整型枚举，`0` 恒为成功；构造类函数失败返回 `NULL`，细分码走出参 |
| 出参可为 NULL | 所有出参指针都可为 `NULL`，含义是「调用方不要该信息」 |
| 枚举值序即 ABI | 两套错误枚举的值序不可改：改序、插位、删位都是破坏性变更，新码只能追加在末尾 |
| 两套枚举互不相干 | `CLV_FileError` 与 `CLV_ContainerError` 独立编号，**不得跨头比对数值**（两边的 `1` 分别是「打开失败」与「无效句柄」） |
| 错误转文本 | `CLV_FileStrError` / `CLV_ContainerStrError` 对每个枚举值返回非空、静态存储期的 ASCII 串；未知值返回 `"unknown"` |
| 句柄线程约束 | 句柄不跨线程使用；跨实例、跨线程的并发语义未定义，调用方自行同步 |
| 句柄生命周期 | `Open` / `Create` 得到，`Close` 释放；对 `NULL` 句柄调用任何函数返回对应的 `INVALID_HANDLE`（`Close(NULL)` 是合法 no-op）；已关闭的句柄再使用是未定义行为 |
| 内存耗尽 | 全库对内存耗尽不设防：分配失败返回 `NULL` / `CLV_CONTAINER_OUT_OF_MEMORY`，不重试、不记日志；判空由调用方做 |
| 库内释放 | `new` / `delete` 都在本库模块内完成，调用方不需要也不能用自家的 `delete` / `free` 释放库返回的对象；`CLV_Alloc` 系列返回的内存用 `CLV_Free` |

### 1.1 路径契约

| 规则 | 行为 |
|---|---|
| 编码 | UTF-8 字节序，不含 BOM |
| 分隔符 | 只接受 `/`；`NULL`、空串、含 `\` 的路径在契约层拒收，返回 `CLV_FILE_INVALID_PATH` |
| 非法 UTF-8 | Windows 后端在转 UTF-16 处失败，返回 `CLV_FILE_OPEN_FAILED` |
| 长路径 | 超过系统默认上限不处理，由应用侧负责 |
| 从容器面进入 | 容器面不透传文件面的码值，路径非法在容器口统一收敛为 `CLV_CONTAINER_IO_FAILED` |

### 1.2 文本与编码

日志消息一律按 UTF-8 字节串透传：库不解析其中的 `{}`、`%s` 之类占位符，也不做编码转换。Windows 控制台要正确显示 UTF-8，由应用自行 `SetConsoleOutputCP(CP_UTF8)`，库不代劳。

---

## 2 头文件与能力域

| 头 | 能力域 | 公开符号规模 | 状态 |
|---|---|---|---|
| `include/CLV_Export.h` | `CLV_API` 可见性开关 | 1 宏 | 稳定 |
| `include/CLV_Version.h` | 版本查询 | 4 函数 + 4 宏 | 稳定 |
| `include/CLV_File.h` | 文件读写：句柄 + 三模式 + 11 错误码 | 10 函数 | 稳定（内存后端为占位） |
| `include/CLV_Logger.h` | 日志：四等级 + 过滤 | 3 函数 | 稳定（落盘为占位） |
| `include/CLV_Memory.h` | 分配口 | 3 函数 | 稳定（C 运行时直通） |
| `include/CLV_Container.h` | 容器读写面 | 19 函数 + 7 结构体 + 11 错误码 | **v0 实验性** |

---

## 3 `CLV_Version.h`

```c
const char* CLV_VersionString(void);   /* 形如 "0.0.2-dev" */
int         CLV_VersionMajor(void);
int         CLV_VersionMinor(void);
int         CLV_VersionPatch(void);
```

| 宏 | 含义 |
|---|---|
| `CLV_VERSION_MAJOR` / `MINOR` / `PATCH` | 三段数字，唯一来源 |
| `CLV_VERSION_STRING` | 带预发布后缀的完整串 |

四个函数与四个宏同源；`src/version.cpp` 读宏不写字面量。字符串含 `-dev` 后缀，三段数字只表达数值部分。容器格式版本（文件头 `version_major` / `version_minor`）与工程版本分轨，不经本头查询。

---

## 4 `CLV_File.h`

### 4.1 模式与语义

| 模式 | 建文件 | 已存在时 | 起始游标 | 对齐 POSIX |
|---|---|---|---|---|
| `CLV_FILE_MODE_READ` | 否 | 必须已存在 | 0 | `"rb"` |
| `CLV_FILE_MODE_WRITE` | 是 | 清空内容 | 0 | `"wb"` |
| `CLV_FILE_MODE_APPEND` | 是 | 保留内容 | 文件末尾 | `"ab"` |

| 行为 | 约定 |
|---|---|
| 短读 | **不是失败**：返回 `CLV_FILE_OK`，实际字节数经 `bytes_read` 给出；到文件尾时 `bytes_read` 为 0 |
| 短写 | 同理经 `bytes_written` 给出实际字节数 |
| Seek | 绝对偏移；Seek 到文件尾之后合法，其后 `Read` 返回 0 字节 |
| Size | 与游标无关，反映文件实际字节长度 |
| Flush | 只读句柄上是 no-op 且返回 `CLV_FILE_OK`，不因此报错 |
| 模式门控 | 只有 `Read` / `Write` 受模式门控：READ 句柄上 `Write` 记 `CLV_FILE_WRITE_FAILED`，WRITE / APPEND 句柄上 `Read` 记 `CLV_FILE_READ_FAILED`；`Seek` / `Size` / `Flush` 三种模式下都可用 |
| 大块请求 | Windows 后端的单次 Win32 调用长度上限是 32 位，后端自动分块吸收，调用方可一次请求 `size_t` 长度 |

### 4.2 函数

```c
CLV_File*      CLV_OpenFile(const char* path, CLV_FileMode mode);
CLV_File*      CLV_OpenFileEx(const char* path, CLV_FileMode mode, CLV_FileError* err);
CLV_FileError  CLV_ReadFile(CLV_File* h, void* buf, size_t size, size_t* bytes_read);
CLV_FileError  CLV_WriteFile(CLV_File* h, const void* data, size_t size, size_t* bytes_written);
CLV_FileError  CLV_SeekFile(CLV_File* h, uint64_t offset, uint64_t* new_position);
CLV_FileError  CLV_FileSize(CLV_File* h, uint64_t* size);
CLV_FileError  CLV_FlushFile(CLV_File* h);
void           CLV_CloseFile(CLV_File* h);
CLV_FileError  CLV_OpenMemoryFile(void* buffer, size_t size, CLV_File** out_handle);
const char*    CLV_FileStrError(CLV_FileError err);
```

`CLV_OpenFile` 失败只返回 `NULL`，忽略错误细分；要原因用 `CLV_OpenFileEx`：成功时返回非空句柄且 `*err == CLV_FILE_OK`，失败时返回 `NULL` 且 `*err` 是具体码。

`CLV_OpenMemoryFile` 是本期占位：声明与定义都在，返回 `CLV_FILE_UNSUPPORTED` 且 `*out_handle` 为 `NULL`。

### 4.3 `CLV_FileError`

| 值 | 名称 | 触发 |
|---|---|---|
| 0 | `CLV_FILE_OK` | 成功 |
| 1 | `CLV_FILE_OPEN_FAILED` | 打开失败（含路径字节序非法、文件不存在于 READ 模式） |
| 2 | `CLV_FILE_READ_FAILED` | 读失败或在不可读句柄上读 |
| 3 | `CLV_FILE_WRITE_FAILED` | 写失败或在不可写句柄上写 |
| 4 | `CLV_FILE_INVALID_HANDLE` | 句柄为 `NULL` |
| 5 | `CLV_FILE_INVALID_PATH` | 路径为 `NULL`、空串或含 `\` |
| 6 | `CLV_FILE_SEEK_FAILED` | 定位失败 |
| 7 | `CLV_FILE_SIZE_FAILED` | 取长度失败 |
| 8 | `CLV_FILE_FLUSH_FAILED` | 刷盘失败 |
| 9 | `CLV_FILE_UNSUPPORTED` | 未落地的能力（当前是内存后端） |
| 10 | `CLV_FILE_INVALID_ARGUMENT` | 模式值非法或 `size != 0` 而缓冲为 `NULL` |

---

## 5 `CLV_Logger.h`

```c
void CLV_Log(CLV_LogLevel level, const char* msg);
void CLV_LogSetLevel(CLV_LogLevel level);
void CLV_LogSetStorage(const char* filepath, size_t rotate_size_kb);
```

| 约定 | 行为 |
|---|---|
| 等级 | `CLV_LOG_DEBUG` / `INFO` / `WARN` / `ERROR` 四值 |
| `msg == NULL` | 整条丢弃，不记错误 |
| `level` 越界 | `CLV_Log` 整条丢弃；`CLV_LogSetLevel` 是 no-op，保持原等级 |
| 默认过滤等级 | `CLV_LOG_INFO`（构造后即生效） |
| 等级查询 | **无**查询接口，当前等级由调用方自己记账 |
| 错误通道 | **无**：日志失败不许影响宿主，后端内部吞掉落点问题 |
| 落盘 | `CLV_LogSetStorage` 是占位，调用无效果 |
| 后端 | 由构建选项决定；空后端（`CLV_LOGGER_USE_SPDLOG=OFF`）下所有调用都是丢弃 |

---

## 6 `CLV_Memory.h`

```c
void* CLV_Alloc(size_t size);
void* CLV_Realloc(void* p, size_t size);
void  CLV_Free(void* p);
```

| 约定 | 行为 |
|---|---|
| `CLV_Alloc` | 不清零；失败返回 `NULL` |
| `CLV_Realloc` | 语义同 C `realloc`：`p == NULL` 等价 `CLV_Alloc(size)`；`size == 0` 的行为不承诺 |
| `CLV_Free` | 接受 `NULL`（no-op）；同一指针重复 `Free` 是未定义行为 |
| 跨模块 | 指针可在同一 CRT 堆内跨模块边界传递；对齐要求按基础类型，不承诺超对齐 |

本期是 C 运行时分配器的直通，不带策略、不重试、不记日志。

---

## 7 `CLV_Container.h`

> **v0 实验性**：本头的形状（函数名、参数结构体、出参形态）在容器层开发期内可整体改；错误码在 ABI 冻结后只在枚举尾部追加，既有码值语义不动。

### 7.1 调用顺序

```text
写方：OpenFile → AddStream ×N → AddExtBlock ×M → WriteFrame ×… → Finish → Close
读方：OpenFile → GetHead / StreamCount / GetStream / ExtBlockCount / GetExtBlock
                → HasIndex / IndexCount / GetIndexEntry → NextFrame ×… → Stats → Close
```

| 顺序规则 | 违反时的码 |
|---|---|
| 全部 `AddStream` / `AddExtBlock` 必须在第一帧之前 | `CLV_CONTAINER_STATE_ERROR` |
| `Finish` 之后句柄不可再写，只能 `Close` | `CLV_CONTAINER_STATE_ERROR` |
| `WriteFrame` 必须按该流解码序（dts 非降）提交 | `CLV_CONTAINER_INVALID_ARGUMENT`，且不写坏文件 |
| 同一 `stream_id` 只能登记一条流 | `CLV_CONTAINER_INVALID_ARGUMENT` |
| `index` 出参不得超过对应计数 | `CLV_CONTAINER_INVALID_ARGUMENT` |

### 7.2 结构体

| 结构体 | 用途 |
|---|---|
| `CLV_StreamDescC` | 流描述符，字段与线上一一对应；`ext_offset` 不在此结构里（由写方布局算出） |
| `CLV_WriteParams` | 建写方时的布局选项：`version_minor` / `index_present` / `globally_sorted` / `streaming` / `fragment_chunk_size`；可为 `NULL` 取 v1 默认（有索引、不分片、非流式） |
| `CLV_FrameC` | 读回的整帧：流号、关键帧标志、`complete`、两个增量、片数、载荷指针与长度 |
| `CLV_ReaderStatsC` | 读方统计，见 §7.5 |
| `CLV_FileHeadC` | 文件头读回视图 |
| `CLV_IndexEntryC` | 索引条目读回视图 |
| `CLV_ExtBlockC` | 扩展块读回视图（含所属流号与绝对偏移） |

`CLV_StreamDescC` 的入参约束（不过即 `CLV_CONTAINER_VALUE_RANGE`，逐条规则见 `docs/容器规范.md` §4.2）：`stream_type` 只取 0 / 1 / 2；`timebase_num` / `timebase_den` 限 u32 非零；`first_dts` v1 必须 0；`max_packet_size` 不得 0；`bit_depth` 按流类型取合法集；`layer_id`、`vlc_table_id`、`codec_flags_1` v1 必须 0；音频流的 `sample_rate` 与 `channels` 不得 0。

### 7.3 写方函数

```c
CLV_ContainerWriter* CLV_ContainerWriterOpenFile(const char* path, const CLV_WriteParams* params,
                                                 CLV_ContainerError* err);
CLV_ContainerError   CLV_ContainerWriterAddStream(CLV_ContainerWriter* w, const CLV_StreamDescC* desc);
CLV_ContainerError   CLV_ContainerWriterAddExtBlock(CLV_ContainerWriter* w, uint8_t stream_id,
                                                    uint8_t ext_type, uint8_t ext_version,
                                                    const void* data, size_t size);
CLV_ContainerError   CLV_ContainerWriterWriteFrame(CLV_ContainerWriter* w, uint8_t stream_id,
                                                   uint8_t is_keyframe, int64_t dts, int64_t pts,
                                                   const void* payload, size_t size);
CLV_ContainerError   CLV_ContainerWriterFinish(CLV_ContainerWriter* w, uint64_t* packets_out);
void                 CLV_ContainerWriterClose(CLV_ContainerWriter* w);
```

| 函数 | 语义 |
|---|---|
| `OpenFile` | 建文件并写占位头（`CLV_FILE_MODE_WRITE`，已存在则清空）；失败返回 `NULL`，细分码走 `err`（可为 `NULL`） |
| `AddExtBlock` | 扩展块挂在指定流名下，必须该流已登记；`ext_type` / `ext_version` 由该流媒体层与调用方约定，容器层只搬运与校验 CRC；单块 `size` 上限 65535 字节 |
| `WriteFrame` | `dts` / `pts` 是**原始时间轴上的绝对值，允许为负**；写方按首帧 dts 把整条流平移到 0，读方不承担平移。`pts_delta` 恒等于 `pts − dts`。载荷为 0 长度时 `payload` 可为 `NULL`；超过该流 `max_packet_size` 拒（`TOO_LARGE`）；超过 `fragment_chunk_size` 自动切分片，每片自带完整外层头 |
| `Finish` | 写索引区与文件尾，回填文件头的 `duration_ticks` / `total_packets` / `index_offset`；`packets_out` 可为 `NULL` |

`CLV_CONTAINER_TOO_LARGE` 的三种触发：单帧载荷超该流 `max_packet_size`、流数超 255、扩展块数据超 65535；偏移超 u32（单文件 4 GiB）同样回这一码。

### 7.4 读方函数

```c
CLV_ContainerReader* CLV_ContainerReaderOpenFile(const char* path, CLV_ContainerError* err);
CLV_ContainerError   CLV_ContainerReaderStreamCount(const CLV_ContainerReader* r, uint32_t* out);
CLV_ContainerError   CLV_ContainerReaderGetStream(const CLV_ContainerReader* r, uint32_t index,
                                                  CLV_StreamDescC* out, uint8_t* usable);
CLV_ContainerError   CLV_ContainerReaderNextFrame(CLV_ContainerReader* r, CLV_FrameC* out,
                                                  uint8_t* got_frame);
CLV_ContainerError   CLV_ContainerReaderStats(const CLV_ContainerReader* r, CLV_ReaderStatsC* out);
CLV_ContainerError   CLV_ContainerReaderHasIndex(const CLV_ContainerReader* r, uint8_t* out);
CLV_ContainerError   CLV_ContainerReaderIndexCount(const CLV_ContainerReader* r, uint32_t* out);
CLV_ContainerError   CLV_ContainerReaderGetHead(const CLV_ContainerReader* r, CLV_FileHeadC* out);
CLV_ContainerError   CLV_ContainerReaderGetIndexEntry(const CLV_ContainerReader* r, uint32_t index,
                                                      CLV_IndexEntryC* out);
CLV_ContainerError   CLV_ContainerReaderExtBlockCount(const CLV_ContainerReader* r, uint32_t* out);
CLV_ContainerError   CLV_ContainerReaderGetExtBlock(const CLV_ContainerReader* r, uint32_t index,
                                                    CLV_ExtBlockC* out);
void                 CLV_ContainerReaderClose(CLV_ContainerReader* r);
```

| 项 | 语义 |
|---|---|
| `OpenFile` | 读文件头与描述符表；头本身不成立返回 `NULL` + `CLV_CONTAINER_BAD_STRUCTURE`，字段值域不成立返回 `NULL` + `CLV_CONTAINER_VALUE_RANGE`。描述符逐条校验，不过的流标记为不可用而**不判整文件损坏** |
| `GetStream` | `index` 是描述符表下标（不是 `stream_id`）；`usable` 出参给该流是否可用（0 / 1） |
| `NextFrame` | 产出一帧重组好的整帧；`*got_frame = 1` 时 `*out` 有效，包区走完时 `*got_frame = 0` 且返回 `OK`（**不视为错误**）。句柄内部推进，读方按 `dts_delta` 自行累加绝对 dts |
| 载荷有效期 | `CLV_FrameC::payload` 与 `CLV_ExtBlockC::data` 指向句柄内部缓冲，只到下一次 `NextFrame`（或 `Close`）为止；要留内容必须自己拷出。`CLV_FileHeadC` / `CLV_IndexEntryC` 是值拷贝，取回即稳定 |
| `HasIndex` | 索引区存在**且**通过读方的排序与范围校验才返回 1；判不可用不代表文件损坏 |
| `GetExtBlock` | 按扩展块枚举下标取回，含所属 `stream_id`、`ext_type`、`ext_version`、绝对 `offset` 与载荷 |

### 7.5 读方统计

| 字段 | 含义 |
|---|---|
| `packets_ok` | 解析成立的包数 |
| `dropped_crc` | CRC 不过被丢弃的包数 |
| `dropped_field` | 字段区不成立（含非最短形式、`payload_size` 仲裁式不符）的包数 |
| `dropped_too_large` | 超出接受上限但跳得动而被跳过的包数 |
| `dropped_unknown_stream` | CRC 成立但 `stream_id` 不在白名单内的包数 |
| `resync_count` | 进入重同步扫描的次数 |
| `resync_bytes_scanned` | 重同步扫描的字节总数 |
| `resync_failed` | 扫描窗口内没找回包头的次数 |
| `frames_ok` | 完整出帧数 |
| `frames_incomplete` | 残帧出帧数（`complete = 0`） |
| `fragment_gaps` | 片序跳变与末片声明总数不符的计数 |
| `missing_first_packets` | 帧首包缺失导致残帧丢弃的次数 |
| `duplicate_fragments` | 同帧重复 `fragment_index` 被忽略的次数 |
| `ext_blocks_bad` | 扩展块链里 CRC 或布局不成立的块数 |
| `truncated` | 包区末尾不足一个包（0 / 1） |
| `abandoned` | 重同步失败、包区剩余内容未读完（0 / 1） |

处置分档与「永不判整文件损坏」的总条见 `docs/容器规范.md` §7。

### 7.6 `CLV_ContainerError`

| 值 | 名称 | 触发 |
|---|---|---|
| 0 | `CLV_CONTAINER_OK` | 成功 |
| 1 | `CLV_CONTAINER_INVALID_HANDLE` | 句柄为 `NULL` 或内部对象缺失 |
| 2 | `CLV_CONTAINER_INVALID_ARGUMENT` | 入参不成立（值域之外的组合、乱序提交、重复流号、下标越界） |
| 3 | `CLV_CONTAINER_IO_FAILED` | 文件面任何失败在此收敛，不回传文件面码值（含路径非法） |
| 4 | `CLV_CONTAINER_BAD_STRUCTURE` | magic 或 CRC 不成立 |
| 5 | `CLV_CONTAINER_VALUE_RANGE` | 字段值域校验不过 |
| 6 | `CLV_CONTAINER_TOO_LARGE` | 超写方侧上限（见 §7.3） |
| 7 | `CLV_CONTAINER_STREAM_NOT_FOUND` | 引用的流号未登记 |
| 8 | `CLV_CONTAINER_STATE_ERROR` | 调用顺序不成立 |
| 9 | `CLV_CONTAINER_OUT_OF_MEMORY` | 建句柄或建内部对象时分配失败（`OpenFile` 类函数） |
| 10 | `CLV_CONTAINER_UNSUPPORTED` | 本期容器面**不返回**这一码；值占位以与内部枚举逐项同序 |

---

## 8 最小示例

```c
#include "CLV_Container.h"

#include <stdio.h>
#include <string.h>

#define TB_NUM 1u
#define TB_DEN 1081080000u

int main(void)
{
    static const uint8_t frame0[64] = {0};
    static const uint8_t frame1[48] = {0};
    CLV_StreamDescC desc;
    CLV_WriteParams params;
    CLV_ContainerError err;
    CLV_ContainerWriter* w;
    CLV_ContainerReader* r;
    CLV_FrameC f;
    uint64_t packets = 0;
    uint8_t got = 0;

    memset(&desc, 0, sizeof desc);
    desc.stream_id = 0;
    desc.stream_type = 0;                    /* 视频 */
    desc.codec_id = 0;
    desc.codec_version = 1;
    desc.timebase_num = TB_NUM;
    desc.timebase_den = TB_DEN;
    desc.max_packet_size = 4096;
    desc.bit_depth = 8;                      /* first_dts / layer_id / vlc_table_id 留 0 */

    memset(&params, 0, sizeof params);
    params.version_minor = 0;
    params.index_present = 1;
    params.fragment_chunk_size = 0;          /* 整帧一包 */

    w = CLV_ContainerWriterOpenFile("./out.clv", &params, &err);
    if (w == NULL)
    {
        printf("建文件失败: %s\n", CLV_ContainerStrError(err));
        return 1;
    }
    if (CLV_ContainerWriterAddStream(w, &desc) != CLV_CONTAINER_OK ||
        CLV_ContainerWriterWriteFrame(w, 0, 1, 0, 0, frame0, sizeof frame0) != CLV_CONTAINER_OK ||
        CLV_ContainerWriterWriteFrame(w, 0, 0, 45045000, 45045000, frame1, sizeof frame1) != CLV_CONTAINER_OK ||
        CLV_ContainerWriterFinish(w, &packets) != CLV_CONTAINER_OK)
    {
        CLV_ContainerWriterClose(w);
        return 1;
    }
    CLV_ContainerWriterClose(w);
    printf("写入 %llu 包\n", (unsigned long long) packets);

    r = CLV_ContainerReaderOpenFile("./out.clv", &err);
    if (r == NULL)
    {
        printf("打开失败: %s\n", CLV_ContainerStrError(err));
        return 1;
    }
    while (CLV_ContainerReaderNextFrame(r, &f, &got) == CLV_CONTAINER_OK && got)
    {
        /* payload 只到下一次 NextFrame 为止有效，要留内容自己拷 */
        printf("帧 dts_delta=%llu pts_delta=%lld 载荷=%u 字节 complete=%u\n",
               (unsigned long long) f.dts_delta, (long long) f.pts_delta,
               (unsigned) f.payload_size, (unsigned) f.complete);
    }
    CLV_ContainerReaderClose(r);
    return 0;
}
```

三个必须点：路径用 `/`；`WriteFrame` 按解码序提交；取走的载荷要留就拷。

---

## 9 构建开关对调用方可见的差异

| 开关 | 默认 | 对公开面的影响 |
|---|---|---|
| `CLV_BUILD_SHARED` | OFF | ON 时产出动态库，`CLV_API` 切 `dllexport`；OFF 时静态库，`CLV_API` 为空 |
| `CLV_USE_SHARED` | OFF | 使用方按动态方式声明导入 |
| `CLV_LOGGER_USE_SPDLOG` | OFF | OFF 时链接空后端，`CLV_Log` 与 `CLV_LogSetLevel` 全部丢弃，日志类验收必须用 ON 的构建 |
| `CLV_BUILD_DEMOS` | OFF | ON 时构建 `demo/` 两份示例 |
| `CLV_WARNINGS_AS_ERRORS` | OFF | 自有 target 的 `/W4 /permissive-` 升级为错误 |

CRT 不能混用：调用方的运行时必须与库一致（静态 `/MT*` 与动态 `/MD*` 不可混链）。

---

## 10 占位与未落地

| 符号 | 现状 |
|---|---|
| `CLV_OpenMemoryFile` | 声明与定义都在，返回 `CLV_FILE_UNSUPPORTED`、`*out_handle` 为 `NULL`，等内存后端落地 |
| `CLV_LogSetStorage` | 调用无效果，等落盘日志后端落地 |
| 容器面全部符号 | **v0 实验性**，形状可在容器层开发期内整体改动 |
| 单文件超 4 GiB | 只检测 + 报错，分段容器不在本期范围 |
| 跨线程与跨实例并发 | 语义未定义，不由本 ABI 承诺 |

---

## 11 验收清单

1. 按 §2 表逐个头文件包含，确认 C 与 C++ 两种编译方式都能通过。
2. 打印 `CLV_VersionString()` 与三段数字，核对与 `include/CLV_Version.h` 的宏同源。
3. 跑 `CLV_BUILD_DEMOS=ON` 构建出的两份示例：`clv_demo_core` 与 `clv_demo_container`，退出码 0 表示自动判定项全过；逐项预期见 `demo/README.md`。
4. 对每个错误枚举调一次 `StrError`，确认每个值都返回非空 ASCII 串、未知值返回 `"unknown"`（对应 `tests/` 里的两条用例）。
5. 逐函数验 `NULL` 入参：句柄为 `NULL` 回 `INVALID_HANDLE`，出参为 `NULL` 回 `INVALID_ARGUMENT`，都不许崩。
6. 用 `CLV_FILE_INVALID_PATH` 与容器口的 `CLV_CONTAINER_IO_FAILED` 核对 §1.1 的路径契约与码值收敛。
7. 用 `assets/fixtures/` 三份素材核 §7.5 的统计口径：正常读回时全部计数为 0。
8. 跑 `ctest`，确认 89 例全过，其中只吃公开 C ABI 的 api 面 40 例是本文的机器可读对账。
