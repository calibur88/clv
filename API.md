# CLV 公开接口（C ABI）

> 本文是 CLV 公开 C ABI 的权威说明：头文件分工、句柄与错误码约定、逐个函数的入参出参语义、可复制示例与验收步骤。  
> 文档版本：0.8.1 · 日期：2026-10-21 · 工程版本以 `include/CLV_Version.h` 的四个宏为单一来源。  
> 字节格式规则见 `docs/容器规范.md`；分层与依赖见 `ARCHITECTURE.md`；头文件里的注释与本表同为契约正文，两边不一致时以本文与头文件为准并回报缺陷。

---

## 1 形态与通用约定

| 项 | 约定 |
|---|---|
| 语言绑定 | 全部公开符号 `extern "C"`，C 与 C++ 都能直接调；C++ 只存在于实现层 |
| 符号前缀 | `CLV_`；可见性由 `CLV_Export.h` 的 `CLV_EXPORT` 决定：静态件下为空宏，构建动态件时切 `dllexport`、消费动态件时切 `dllimport`（两个组合位 `CLV_BUILD_DLL` / `CLV_USE_DLL` 由所链的目标自带，调用方不需要定义任何宏） |
| 对象形态 | **不透明句柄**（`typedef struct CLV_X CLV_X;`）+ 自由函数 + 整型错误码；C++ 类型不跨边界，接口上没有异常，失败一律走返回码 |
| 错误码 | 返回整型枚举，`0` 恒为成功；构造类函数失败返回 `NULL`，细分码走出参 |
| 出参可否为 NULL | **逐口定**，没有「所有出参都可为 `NULL`」这条通则：文件面六个出参全部可为 `NULL`，容器面、媒体面与节拍面的**主出参**（结构体与帧出参）为空即判对应的 `*_INVALID_ARGUMENT`；计数出参逐口不同，见下面「出参 NULL 的逐口口径」 |
| 枚举值序即 ABI | 五套错误枚举的值序不可改：改序、插位、删位都是破坏性变更，新码只能追加在本面段位的末尾；退场码留下的号位不复用 |
| 五面错误码各自独立 | 五面的错误码各自独立枚举：文件与容器从 1 顺排、音频 32 到 63、视频 64 到 95、节拍 96 起，不得跨头比对数值——各头的 `1` 语义无关 |
| 错误转文本 | 五个错误枚举各有一个转文本口：`CLV_FileStrError`、`CLV_ContainerStrError`、`CLV_VideoStrError`、`CLV_AudioStrError`、`CLV_PaceStrError`；每个口对所属枚举的每个值返回非空、静态存储期的 ASCII 串，未知值与退场留白号位返回 `"unknown"` |
| 句柄线程约束 | 句柄不跨线程使用；跨实例、跨线程的并发语义未定义，调用方自行同步 |
| 句柄生命周期 | `Open` / `Create` 得到，`Close` 释放；对 `NULL` 句柄调用任何函数返回对应的 `INVALID_HANDLE`（`Close(NULL)` 是合法 no-op）；已关闭的句柄再使用是未定义行为 |
| 内存耗尽 | **分配口本身**不设防：`CLV_Alloc` / `CLV_Realloc` 失败返回 `NULL`，不重试、不记日志，判空由调用方做。容器、视频、音频、节拍四面入口在 C ABI 边界上把分配失败收口成各自的具体码 —— `CLV_CONTAINER_OUT_OF_MEMORY` / `CLV_VIDEO_OUT_OF_MEMORY` / `CLV_AUDIO_OUT_OF_MEMORY` / `CLV_PACE_OUT_OF_MEMORY` |
| 库内释放 | `new` / `delete` 都在本库模块内完成，调用方不需要也不能用自家的 `delete` / `free` 释放库返回的对象；`CLV_Alloc` 系列返回的内存用 `CLV_Free` |

**出参 NULL 的逐口口径**——「传 `NULL` 表示不要该信息」只成立于下表点名的出参，**未点名的出参一律按不可为 `NULL` 使用**（为空即判该面的 `*_INVALID_ARGUMENT`）：

| 头 | 可为 `NULL` 的出参 | 为空即判 `*_INVALID_ARGUMENT` 的主出参 |
|---|---|---|
| `CLV_File.h` | **全部出参**：`err`、`bytes_read`、`bytes_written`、`new_position`、`size`、`out_handle` | 无——文件面的 `CLV_FILE_INVALID_ARGUMENT` 只判非法模式值与「`size != 0` 而缓冲为 `NULL`」两条 |
| `CLV_Container.h` | 两个 `OpenFile` 的 `err`、`Finish` 的 `packets_out`、`GetStream` 的 `usable`、`NextFrame` 的 `got_frame` | 其余主出参：`StreamCount` / `IndexCount` / `ExtBlockCount` / `HasIndex` 的计数与标志出参，`GetStream` / `NextFrame` / `Stats` / `GetHead` / `GetIndexEntry` / `GetExtBlock` 的结构体出参 |
| `CLV_Video.h` | 两个 `*Open` 的 `err`、`CLV_VideoDecoderDecode` 的 `info`、`CLV_VideoFrameGetDesc` 的 `desc` 与 `info`、`CLV_VideoDecoderNextOutput` 与 `CLV_VideoDecoderFlush` 的计数出参 `got` | `CLV_VideoCodedSize` 的 `coded_width` / `coded_height`、`CLV_VideoParseEncMeta` 的 `out`、取帧口与冲刷口的帧出参 `out`、`CLV_VideoFrameToRGB24` 的 `dst`、码流口的 `data` / `size` |
| `CLV_Audio.h` | 两个 `*Open` 的 `err`、`CLV_AudioDecoderDecodeFrame` 的 `info` | 无状态工具与句柄 / 缓冲口的主出参；`CLV_AudioSplitMetaBlock` 的三个出参按「至少给一个」判，三个全空才判 |
| `CLV_Pace.h` | `CLV_PaceOpen` 的 `err` | `CLV_PaceClockSeconds` 的 `out_seconds`、`CLV_PaceDecide` 的 `out`、`CLV_PaceFeedAllowed` 的 `out_allowed`；`CLV_PaceDecide` 另要求 `out->struct_size` 不小于 `sizeof(CLV_PaceDecisionC)`，填 0 与更小的显式长度都拒 |

入参侧另有一条：`CLV_ContainerWriterOpenFile` 的 `params` 可为 `NULL`，取默认（有索引、不分片、非流式、不分段）。
各头函数上的注释与本表同为契约正文，逐口以两者共同点为准，对不上即回报缺陷。

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
| `include/CLV_Export.h` | `CLV_EXPORT` 可见性开关 | 1 宏 | 稳定 |
| `include/CLV_Version.h` | 版本查询 | 4 函数 + 4 宏 | 稳定 |
| `include/CLV_File.h` | 文件读写：句柄 + 三模式 + 11 错误码 | 10 函数 | 稳定（内存后端为占位） |
| `include/CLV_Logger.h` | 日志：四等级 + 过滤 | 3 函数 | 稳定（落盘为占位） |
| `include/CLV_Memory.h` | 分配口 | 3 函数 | 稳定（C 运行时直通） |
| `include/CLV_Container.h` | 容器读写面 | **25 函数**（写方 7 + 读方 17 + `StrError`）+ **10 结构体** + 14 错误码 | 函数名 / 参数结构体 / 出参形态属对外 ABI，改动即破坏调用方；错误码只在枚举尾部追加、既有码值与语义不动；未冻结的是 `.clv` 的字节格式与语义 |
| `include/CLV_Video.h` | 视频编解码面 | **23 函数**（无状态 2 + 解码 7 + 编码 4 + 码流 4 + 帧 5 + `StrError`）+ 5 结构体 + 6 枚举 | 编解两侧的**有损重建闭环已接**，**无损档已接**（编码侧 `CLV_VideoEncParamsC.mode`，无损的模式信号在码流里 = 帧头 / 片头的 `vop_quant = 0`，解码侧不接模式参数）；**运动补偿已接**（帧间按运动矢量取样，半像素双线性平均 + 参考缓冲边缘复制带；B 帧编码侧目前只产 direct + `not_coded`）；帧的借用与归还走引用计数（`CLV_VideoFrameFree`），逐帧像素出库走 `CLV_VideoFrameToRGB24`；函数级说明见本头每个导出声明上的注释 |
| `include/CLV_Audio.h` | 音频编解码面 | **28 函数**（无状态 5 + 承载块解读 4 + 解码 6 + 编码 6 + 缓冲 3 + 大块 3 + `StrError`）+ 10 结构体 + 3 枚举 + **16 个失败码**（计数一律不含 `OK`；段内 41 / 45 两格是退场留白、号位不复用，所以槽位 18 个、能拿到的失败码 16 个）| 编解码闭环已实现（libFLAC 后端接线到帧级：帧头自核 + 流声明对账 + 一包一帧）；**承载块内容的解读面已接**（Vorbis 注释键值与图标块字段，只读、不复制字节）；结构层与句柄面均为实现态；函数级说明见本头每个导出声明上的注释 |
| `include/CLV_Pace.h` | 呈现节拍面（主时钟、逐帧判定、背压） | **9 函数**（建仓 1 + 拆仓 1 + 趟与设备态 3 + 读钟 1 + 判定 1 + 背压 1 + `StrError`）+ 2 结构体 + 2 枚举（错误码 7 值 = `OK` 加 96 到 101 六个失败码）+ 2 回调类型 | 已实现：把「音频设备侧消费量当主时钟、视频按 pts 追它」这套口径从播放器里抽出来，**本头不依赖任何播放器库**，库自己既不碰线程也不碰设备；结构体自报字段只受理显式长度（填 0 拒；容器、视频、音频三头的同式口径受理填 0，本头不受理，理由见 §8.1）；逐函数说明见 §8 |

公开头共九份（CLV_Audio.h、CLV_Container.h、CLV_Export.h、CLV_File.h、CLV_Logger.h、CLV_Memory.h、CLV_Pace.h、CLV_Version.h、CLV_Video.h）；三份示例直接 include 用到其中八份，唯一没直接用到的是导出宏头 CLV_Export.h，它由各接口头自己带进来。

两份媒体面的函数名清单（与两份头的 `CLV_EXPORT` 声明逐字对齐，改函数必须同步这一段）：

- 视频：`CLV_VideoCodedSize`、`CLV_VideoParseEncMeta`、`CLV_VideoDecoderOpen`、`CLV_VideoDecoderDecode`、`CLV_VideoDecoderNextOutput`、`CLV_VideoDecoderFlush`、`CLV_VideoDecoderReset`、`CLV_VideoDecoderStreamRejected`、`CLV_VideoDecoderClose`、`CLV_VideoEncoderOpen`、`CLV_VideoEncoderAllocFrame`、`CLV_VideoEncoderEncode`、`CLV_VideoEncoderClose`、`CLV_VideoBitstreamGetBytes`、`CLV_VideoBitstreamSliceCount`、`CLV_VideoBitstreamGetSlice`、`CLV_VideoBitstreamRelease`、`CLV_VideoFrameAddRef`、`CLV_VideoFrameRelease`、`CLV_VideoFrameFree`、`CLV_VideoFrameGetDesc`、`CLV_VideoFrameToRGB24`、`CLV_VideoStrError`
- 音频：`CLV_AudioInspectFrame`、`CLV_AudioParseStreamInfo`、`CLV_AudioSplitMetaBlock`、`CLV_AudioClassifyMetaBlock`、`CLV_AudioStripChannelMask`、`CLV_AudioParseVorbisComment`、`CLV_AudioVorbisCommentEntryAt`、`CLV_AudioVorbisCommentFindKey`、`CLV_AudioParsePicture`、`CLV_AudioDecoderOpen`、`CLV_AudioDecoderDecodeFrame`、`CLV_AudioDecoderAddMetaBlock`、`CLV_AudioDecoderReset`、`CLV_AudioDecoderStats`、`CLV_AudioDecoderClose`、`CLV_AudioEncoderOpen`、`CLV_AudioEncoderWrite`、`CLV_AudioEncoderNextFrame`、`CLV_AudioEncoderFlush`、`CLV_AudioEncoderTakeStreamInfo`、`CLV_AudioEncoderClose`、`CLV_AudioBufferAddRef`、`CLV_AudioBufferRelease`、`CLV_AudioBufferGetDesc`、`CLV_AudioBlobAddRef`、`CLV_AudioBlobRelease`、`CLV_AudioBlobGetBytes`、`CLV_AudioStrError`

容器面写方第七个是 `CLV_ContainerWriterWriteFrameChunks`（配 `CLV_Chunk`），它把分片边界交给调用方，容器不再按 `fragment_chunk_size` 二次切；原 `CLV_ContainerWriterWriteFrame` 不动。

音频面的只读帧户口 `CLV_AudioInspectFrame` 有**两个**失败出口，两种场合都不写 `*out`：

- `CLV_AUDIO_FRAME_HEADER_BAD`：帧头自身读不通（同步码不符、字段取保留值、帧尾校验不符）。
- `CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE`：帧头读得通，但块大小越出流式子集上界（块大小大于 16384；采样率不大于 48000 时上界收到 4608）。这一档与上一档是两个不同的失败原因，走的是同一只口。

入参不成立（`payload` 为空、`size` 为 0、`out` 为空）判 `CLV_AUDIO_INVALID_ARGUMENT`。

视频面的帧对象始终归库所有，取帧口交出去的只是带计数的借用句柄：取帧口交出帧时引用计数加一；调用方用毕调 `CLV_VideoFrameFree` 减一；计数归零的帧才回收进空闲池。Free 之后该帧指针失效，建议立即置空。重置或关闭会使所有已交出的帧指针失效，此后再调 Free 返回参数不成立。对同一帧重复调用返回参数不成立；跨复用后的陈旧调用属调用方未定义行为，本版本不做幂等保证。`CLV_VideoFrameAddRef` 与 `CLV_VideoFrameRelease` 是同一个计数上的两扇门（加计数并回报当前值 / 减计数不回报），对 `NULL` 分别是返回 0 与 no-op，都不崩溃。`CLV_VideoFrameToRGB24` 把 8-bit YUV420P 帧转成 `3 × 宽 × 高` 字节、行间不留填充的 RGB24 缓冲，色度按 2×2 最近邻放大而不做插值（增益式、移位与夹取口径见该口的导出注释；位深不是 8 走 `CLV_VIDEO_BIT_DEPTH_UNSUPPORTED`）。

---

## 3 `CLV_Version.h`

```c
const char* CLV_VersionString(void);   /* 形如 "0.8.1-dev" */
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
| 请求 0 字节 | 返回 `CLV_FILE_OK` 且 `*bytes_read` 置 0 ⇒ **不能用 0 字节读判文件尾**，剩余量走 `CLV_FileSize` |
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

`CLV_OpenMemoryFile` 是占位入口：声明与定义都在，任何入参都返回 `CLV_FILE_UNSUPPORTED` 且 `*out_handle` 置 `NULL`。

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

当前实现是 C 运行时分配器的直通，不带策略、不重试、不记日志。

---

## 7 `CLV_Container.h`

> **ABI 口径**：本头的函数名、参数结构体与出参形态属对外 ABI，改动即破坏调用方；错误码只在枚举尾部追加，既有码值语义不动。未冻结的是 `.clv` 的字节格式与语义，见 `docs/容器规范.md`。

### 7.1 调用顺序

```text
写方：OpenFile → AddStream ×N → AddExtBlock ×M → WriteFrame / WriteFrameChunks ×… → Finish → Close
读方（单段）：OpenFile → GetHead / StreamCount / GetStream / ExtBlockCount / GetExtBlock
                → HasIndex / IndexCount / GetIndexEntry → NextFrame ×… → Stats → Close
读方（多段）：OpenSegments（段清单 + auto_advance）→ NextFrame ×…（段界自动续读，跨段仍是一条帧序列）
                → SegStats / SegmentIndex / SegmentTotal → Close
```

| 顺序规则 | 违反时的码 |
|---|---|
| 全部 `AddStream` / `AddExtBlock` 必须在第一帧之前 | `CLV_CONTAINER_STATE_ERROR` |
| `Finish` 之后句柄不可再写，只能 `Close` | `CLV_CONTAINER_STATE_ERROR` |
| `WriteFrame` / `WriteFrameChunks` 必须按该流解码序（dts 非降）提交 | `CLV_CONTAINER_INVALID_ARGUMENT`，且不写坏文件 |
| 同一 `stream_id` 只能登记一条流 | `CLV_CONTAINER_INVALID_ARGUMENT` |
| `index` 出参不得超过对应计数 | `CLV_CONTAINER_INVALID_ARGUMENT` |

### 7.2 结构体

| 结构体 | 用途 |
|---|---|
| `CLV_StreamDescC` | 流描述符，字段与线上一一对应；`ext_offset` 不在此结构里（由写方布局算出） |
| `CLV_WriteParams` | 建写方时的布局选项：`struct_size` / `version_minor` / `index_present` / `globally_sorted` / `streaming` / `fragment_chunk_size`，v2 在尾部追加**分段身份**六项（`segmented` / `seg_flags` / `seg_index` / `seg_total` / `seg_start_ticks` / `seg_end_ticks`）；可为 `NULL` 取默认（有索引、不分片、非流式）。`struct_size` 三档受理 —— 留 0 = 存量写法照旧受理、填上一版尺寸（16）= 只认前六个字段、填 `sizeof(CLV_WriteParams)` = 显式声明本布局，**其余值判 `CLV_CONTAINER_INVALID_ARGUMENT` 且判在开文件之前**。只报旧尺寸的递交拿不到尾部六项 ⇒ 按不分段处理，库不会把那段字节当字段读，也不会悄悄产出自称分段的文件。**新代码一律显式填 `sizeof(*p)`**：`0` 那一支挡不住全部混编形状 —— 旧布局的首四字节是 `version_minor`（旧布局的合法取值恰为 0），「旧布局 + 全默认值」的递交会被读成 `struct_size = 0` 而按本布局受理，尾部六项随之错位；这一支不设防御，因为把 0 一并拒掉要求每个调用点补一行、外部调用方必须改码，那属对既有调用契约的破坏。完整口径见 `include/CLV_Video.h` 的 `struct_size` 混编说明（三头同式；视频与音频两侧原首字段合法值非 0，天然不落进这一支） |
| `CLV_Chunk` | `CLV_ContainerWriterWriteFrameChunks` 的一片输入：`data` + `size`，数组下标即片序；`data` 只需在调用期间有效，容器当场落盘 |
| `CLV_FrameC` | 读回的整帧：流号、关键帧标志、`complete`、两个增量、片数、载荷指针与长度 |
| `CLV_ReaderStatsC` | 读方统计，见 §7.5 |
| `CLV_FileHeadC` | 文件头读回视图 |
| `CLV_IndexEntryC` | 索引条目读回视图 |
| `CLV_ExtBlockC` | 扩展块读回视图（含所属流号与绝对偏移） |

`CLV_StreamDescC` 的入参约束（不过即 `CLV_CONTAINER_VALUE_RANGE`，逐条规则见 `docs/容器规范.md` 的流描述符一节）：`stream_type` 只取 0 / 1 / 2；`timebase_num` / `timebase_den` 限 u32 非零；`first_dts` 首段必须 0（分段容器的后续段取该段该流首帧的绝对 dts，见 `docs/容器规范.md` 的流描述符一节）；`max_packet_size` 不得 0；`bit_depth` 按流类型取合法集；`layer_id`、`vlc_table_id`、`codec_flags_1` v1 必须 0；音频流的 `sample_rate` 与 `channels` 不得 0。

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
CLV_ContainerError   CLV_ContainerWriterWriteFrameChunks(CLV_ContainerWriter* w, uint8_t stream_id,
                                                         const CLV_Chunk* chunks, uint32_t chunk_count,
                                                         int64_t dts, int64_t pts, uint8_t flags);
CLV_ContainerError   CLV_ContainerWriterFinish(CLV_ContainerWriter* w, uint64_t* packets_out);
void                 CLV_ContainerWriterClose(CLV_ContainerWriter* w);
```

| 函数 | 语义 |
|---|---|
| `OpenFile` | 建文件并写占位头（`CLV_FILE_MODE_WRITE`，已存在则清空）；失败返回 `NULL`，细分码走 `err`（可为 `NULL`） |
| `AddExtBlock` | 扩展块挂在指定流名下，必须该流已登记；`ext_type` / `ext_version` 由该流媒体层与调用方约定，容器层只搬运与校验 CRC；单块 `size` 上限 4 294 967 295 字节 |
| `WriteFrame` | `dts` / `pts` 是**原始时间轴上的绝对值，允许为负**；写方按首帧 dts 把整条流平移到 0，读方不承担平移。`pts_delta` 恒等于 `pts − dts`。载荷为 0 长度时 `payload` 可为 `NULL`；超过该流 `max_packet_size` 拒（`TOO_LARGE`）；超过 `fragment_chunk_size` 自动切分片，每片自带完整外层头 |
| `WriteFrameChunks` | 同上的一条帧，但**分片边界由调用方给定**：`chunks` 数组下标即片序，容器不按 `fragment_chunk_size` 二次切，一包一片按数组序落盘。`chunk_count = 1` 走单包帧路径（包外层不标分片位），`≥ 2` 时容器置 `is_fragment` 与末片标志——首片是帧首包并携带 `pts_delta`，同一帧各片带同一 `dts_delta`。`flags` 只用 bit0 = `is_keyframe`，bit1-7 必须为 0。`chunk_count = 0`、`chunks` 为 `NULL`、任一片 `size` 为 0 或 `data` 为 `NULL` 判 `INVALID_ARGUMENT`，任一片超该流 `max_packet_size` 判 `TOO_LARGE`；**两类失败都判在落盘之前，一片不写**。`dts` / `pts` 语义与 `WriteFrame` 相同，同样要求按该流解码序（dts 非降）提交 |
| `Finish` | 写索引区与文件尾，回填文件头的 `duration_ticks` / `total_packets` / `index_offset`；`packets_out` 可为 `NULL` |

`CLV_CONTAINER_TOO_LARGE` 的触发点全在写方侧，共六条：一，`WriteFrame` 的单帧载荷超该流 `max_packet_size`；二，`WriteFrameChunks` 的任一片载荷超该流 `max_packet_size`、各片相加溢出、或单帧合计超 1 073 741 824 字节 —— 该上界 = 宏块数上界（补后像素上界 2^28 ÷ 256 = 1 048 576 个宏块）× 每宏块线上最坏取整 1 024 字节 = 2^30，是一帧含其全部分片的字节上限，不是单包上限；三，`AddStream` 声明的 `max_packet_size` 超 8 388 567 字节 —— 该界 = 默认读方窗口 8 MiB 减外层最坏开销 41 字节，声明超过它就写得出读不回，故判在开流时而不是落盘后；四，登记的流数超 255；五，扩展块载荷超 `ext_len` 四字节可表达域 4 294 967 295；六，`Finish` 时索引条目数越出索引头 `entry_count` 的 u32 域 —— 越界即拒这次收尾、不做静默截断，判点排在编码与落盘之前，一片索引字节都不写。偏移类字段（`file_offset` / `ext_offset` / `next_ext_offset`）是 u64，不因单文件长度回这一码。读方不产这一码：读侧遇到超出接受上限的包走统计里的 `dropped_too_large`。

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
CLV_ContainerReader* CLV_ContainerReaderOpenSegments(const char* const* paths, uint32_t path_count,
												uint8_t auto_advance, CLV_ContainerError* err);
CLV_ContainerError   CLV_ContainerReaderSegmentIndex(const CLV_ContainerReader* r, uint64_t* out);
CLV_ContainerError   CLV_ContainerReaderSegmentTotal(const CLV_ContainerReader* r, uint64_t* out);
CLV_ContainerError   CLV_ContainerReaderSegStats(const CLV_ContainerReader* r, CLV_ReaderSegStatsC* out);
CLV_ContainerError   CLV_ContainerReaderSegExtCheck(CLV_ContainerReader* r, CLV_ReaderSegExtCheckC* out);
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
| `OpenFile` | 读文件头与描述符表；头本身不成立返回 `NULL` + `CLV_CONTAINER_BAD_STRUCTURE`，字段值域不成立返回 `NULL` + `CLV_CONTAINER_VALUE_RANGE`。描述符逐条校验，校验不过的不判整文件损坏：只列出可用的流；描述符校验不通过的流在打开时被丢弃，不回报数量，也不占用编号 |
| `GetStream` | 只列出可用的流；描述符校验不通过的流在打开时被丢弃，不回报数量，也不占用编号。`index` 是可用流列表的下标（不是 `stream_id`），越出列表判 `CLV_CONTAINER_INVALID_ARGUMENT`；表内每条都已通过校验，`usable` 因此恒取 1，该出参可为 `NULL` 表示不要 |
| `NextFrame` | 产出一帧重组好的整帧；`*got_frame = 1` 时 `*out` 有效，包区走完时 `*got_frame = 0` 且返回 `OK`（**不视为错误**）。句柄内部推进，读方按 `dts_delta` 自行累加绝对 dts。多段句柄下清单走完而最后一个接缝有事件时，返回 `CLV_CONTAINER_SEGMENT_MISMATCH` 或 `CLV_CONTAINER_SEGMENT_ORDER` 且 `*got_frame` 仍为 0：已交出的帧不回收，段级计数查 `SegStats` |
| `OpenSegments` | 打开一组段（一个逻辑视频的多个 `.clv`）：`paths[i]` 按段序给、`path_count >= 1`，库不猜文件名也不列目录。`auto_advance = 1` 时本段包区走完自动接下一段，调用方拿到的仍是一条连续帧序列（`CLV_FrameC` 形状不变）；= 0 时本段读完即 `*got_frame = 0`，与单段同形。`paths` / `path_count` / 其中任一 `paths[i]` 为 `NULL` 判 `INVALID_ARGUMENT`；`paths[0]` 打不开与 `OpenFile` 同档。**段界处段首帧的 `dts_delta` 按该段自己的起点算**（后续段不平移 ⇒ 那个增量就是它的绝对 dts），跨段一个累加器加到底不成立，连续时间轴要用 `first_dts` 逐段起算。句柄轮转只换内部 reader：`Stats` / `GetHead` / 索引与扩展块视图反映**当前段**，跨段累计只由 `SegStats` 承担 |
| `SegmentIndex` / `SegmentTotal` | 当前段头里声明的 `seg_index` 与 `seg_total`；本文件不是分段容器时分别返回 0 与全 1（= 未知），并回报 `OK`，不当错误。段号以段块里的声明为权威，与清单下标不符时 `SegmentIndex` 给的是头里的值 |
| `SegExtCheck` | 段间扩展块一致性的**主动查询**：按 `(stream_id, ext_type)` 与基准段逐字节比版本号与载荷，出「扫过段数 / 读不开段数 / 比对条数 / 一侧缺块条数 / 内容不同条数 / 不一致段数 / 首处不一致（段号 + 流号 + 块类型 + 原因）」，并把两个计数落进 `SegStats`。本版**不因不一致而拒接**，返回码仍是 `OK`。返回档四条：句柄为空判 `CLV_CONTAINER_INVALID_HANDLE`；`out` 为空判 `CLV_CONTAINER_INVALID_ARGUMENT`；**清单里每一段都打不开**判 `CLV_CONTAINER_IO_FAILED`，此时不写 `out`、落进 `SegStats` 的两档计数也不动；库内分配失败判 `CLV_CONTAINER_OUT_OF_MEMORY`，此时两档计数同样不动 |
| `SegStats` | 分段观测计数（`CLV_ReaderSegStatsC`，见 §7.5）；段标识落在文件头 CRC 覆盖之外，失效的实测主形态是静默丢失 ⇒ 缺失 / 整段拒接 / 段号错位 / 声明有间断各自是可查的数 |
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
| `abandoned` | 重同步失败后放弃剩余包区时为 1 |

分段观测面 `CLV_ReaderSegStatsC`（新结构，不动 `CLV_ReaderStatsC` 的尺寸）：
`block_missing`（声明了 `segmented` 却没拿到可用段描述块）/ `block_bad`（块读得进但解析不成立）/ `discontinuities`（段块 bit2 声明与上一段之间有时间断）/ `video_stream_no_param_block`（视频流缺 `ext_type = 3` 而在打开时被丢弃的条数）/ `segments_read`（本句柄走过的段数，含当前段）/ `segments_missing`（清单里接不上因而跳过的段数）/ `segments_mismatched`（描述符与首段逐字段对不上、被整段拒接的段数）/ `segments_reordered`（段块 `seg_index` 与清单位置不符、按头里段号记账的段数，续读不中断）/ `last_seg_index`（最后一次读到的段号）。前四档 = 已走过各段结转 + 当前段现值，后五档在句柄存活期内跨段不重置。两类分段事件各自对应一个收束码：`segments_mismatched` → `CLV_CONTAINER_SEGMENT_MISMATCH`、`segments_reordered` → `CLV_CONTAINER_SEGMENT_ORDER`，一格一因，不再合计数。

处置分档与「永不判整文件损坏」的总条见 `docs/容器规范.md` 的容错与重同步一节。

段间扩展块一致性另加两档（由 `SegExtCheck` 写入，句柄存活期内不重置）：`segments_ext_scanned`（查过的段数，**含基准段；0 = 从没查过**）与 `segments_ext_inconsistent`（其中与基准段内容不一致的段数）。成对给而不是一格，是因为只有一格时 0 会被读成「通过」，那是判据空转的一种。查询口本身在 `NextFrame` 通路之外：不动段游标、不回收已交出的帧。

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
| 10 | `CLV_CONTAINER_UNSUPPORTED` | 容器面**不返回**这一码。它是段内预留位：内部枚举没有对应项（内部面的码只到「调用顺序不成立」那一档，分配失败一类由门面直接按公开码回报），既不是同序占位，也没有产点 |
| 11 | `CLV_CONTAINER_EXT_CHAIN_TOO_LONG` | 读方走某条流的扩展块链，逐块成立、链路向前，却在跳数上限处仍没走到链尾：**整次打开失败**，`CLV_ContainerReaderOpenFile` 返回 NULL 并把本码写入 `err`。上限是实现侧护栏常量，不写入格式规范正文 |
| 12 | `CLV_CONTAINER_SEGMENT_MISMATCH` | 分段容器：这一段自己的流描述符与前段逐字段比不下来 ⇒ 该段不可接入，不试解它的包；已交出的帧不回收，其余段照常 |
| 13 | `CLV_CONTAINER_SEGMENT_ORDER` | 清单给的位置与该段头里的 `seg_index` 对不上：以头里的段号为权威放置该段（文件名与清单序只是线索），走观测口回报并计数，**不判损坏、不停读** |

---

## 8 `CLV_Pace.h`

> **能力域口径**：本面把「音频设备侧消费量当主时钟、视频按 pts 追它」这套口径从播放器里抽出来。**不依赖任何播放器库**：设备侧只通过两个回调交进来（排队字节数、可选的单调钟），具体用哪个音频设备、怎么睡、怎么画都由调用方决定；库自己既不碰线程也不碰设备。

### 8.1 调用顺序

```text
Open → BeginPass →（逐帧循环）NoteAudioFed / SetDeviceRunning / ClockSeconds / Decide / FeedAllowed → Close
```

| 顺序规则 | 违反时的码或后果 |
|---|---|
| 未 `BeginPass` 就读钟、判定、置设备态、记喂入 | `CLV_PACE_STATE_ERROR`（`FeedAllowed` 不在这一条上，它只读当前排队量，未起趟也返回 `OK`） |
| 每趟播放都要重新 `BeginPass` | 不清账会把上一趟的累计喂入、外推锚点与本趟零点带进这一趟 |
| 本趟零点只定一次：先发生的那一步定它 —— `SetDeviceRunning` 把设备放开的那一刻，或第一次读钟 / 判定的那一刻 | 零点若留给「第一次判定」，设备从放开起就开始计位，而第一帧还要解码十几到几十毫秒，这一截会长期算进钟位，落在对齐带里的出点永远等不到；恢复播放重定零点则会让暂停前后之间钟自己归零 |
| 主时钟单调：取位只认更大的那个值 | 设备按整块取数，重锚会把已外推的相位丢掉；不钉住单调时 `dev` 会呈锯齿冲高，攒久了跨过丢帧阈 |

**结构体自报字段**：本面的结构体自报字段只受理显式填入的本结构体长度，填 0 拒。理由：节拍面是新接口，没有按清零写的存量调用方；「填 0 也受理」那一支原本是为兼容存量代码才留的，这里没有要兼容的对象，拒掉反而多一道布局校验。因此本头是其余各头的例外：容器、视频、音频三头的同式口径受理填 0，本头不受理；比本版更大的显式长度仍按「不小于本版已知前缀」受理，被拒的只有 0 与小于本版的值。从那些头迁过来的调用方，每一处都要显式填 `sizeof(*该结构)`。

### 8.2 函数

```c
CLV_Pace*        CLV_PaceOpen(const CLV_PaceParamsC* params, CLV_PaceError* err);
void             CLV_PaceClose(CLV_Pace* p);
CLV_PaceError    CLV_PaceBeginPass(CLV_Pace* p);
CLV_PaceError    CLV_PaceSetDeviceRunning(CLV_Pace* p, uint8_t running);
CLV_PaceError    CLV_PaceNoteAudioFed(CLV_Pace* p, uint64_t bytes_added);
CLV_PaceError    CLV_PaceClockSeconds(CLV_Pace* p, double* out_seconds);
CLV_PaceError    CLV_PaceDecide(CLV_Pace* p, double video_pts_seconds, CLV_PaceDecisionC* out);
CLV_PaceError    CLV_PaceFeedAllowed(CLV_Pace* p, uint8_t* out_allowed);
const char*      CLV_PaceStrError(CLV_PaceError code);
```

| 函数 | 语义 |
|---|---|
| `Open` | 按参数建仓；不成返回 `NULL` 并把原因写进 `err`（`err` 可为 `NULL`）。判定顺序：`params` 为空、`struct_size` 小于 `sizeof(CLV_PaceParamsC)`、`queued` 回调缺失 ⇒ `INVALID_ARGUMENT`；四个非零字段（`fps_num` / `fps_den` / `sample_rate` / `bytes_per_frame`）任一为 0，或 `sample_rate` 大于 16 777 216、`bytes_per_frame` 大于 256 ⇒ `VALUE_RANGE`；句柄本身分配失败 ⇒ `OUT_OF_MEMORY` |
| `Close` | 拆仓；对 `NULL` 是 no-op。句柄拆后调用方不得再用 |
| `BeginPass` | 起一趟：累计喂入、实际已消费、外推锚点、本趟零点与墙钟起点全部作废；设备态初值为「未放开」（`running` = 0，钟不推进） |
| `SetDeviceRunning` | 声明设备是否在播：`running` 非 0 = 放开、0 = 暂停或预灌。放开与按住都作废外推锚点（暂停期间设备没消费，留着旧锚会把整段暂停时长外推进钟位） |
| `NoteAudioFed` | 记一笔喂进设备的字节，**增量口径**，可多次调；已消费量 = 累计喂入 − 设备仍排队的字节 |
| `ClockSeconds` | 本趟主时钟当前秒数，**相对起播零点**（本趟第一次读钟或第一次判定的那一刻）。拿不到新取位且设备在跑时按单调钟线性外推，外推顶到「已喂入」为止 |
| `Decide` | 判这一帧：入参是本帧 pts（秒），出动作与分类量。`out->struct_size` 必须显式填 `sizeof(CLV_PaceDecisionC)`，填 0 与更小的值判 `INVALID_ARGUMENT`。返回 `CLV_PACE_CLOCK_SKEW` 时 `out` 仍被写入，那一帧只报实际已消费量、不做外推 |
| `FeedAllowed` | 背压判定：`*out_allowed` 为 0 表示设备里还堆着，先别喂。阈值 = `backpressure_bytes` |
| `StrError` | 每个枚举值返回非空、静态存储期的 ASCII 串；未知值返回 `"unknown"` |

### 8.3 结构体

`CLV_PaceParamsC`（建仓入参）：

| 字段 | 含义 |
|---|---|
| `struct_size` | 显式填 `sizeof(CLV_PaceParamsC)`，见 §8.1 |
| `fps_num` / `fps_den` | 帧率分子与分母，都非 0；帧周期 `period = fps_den / fps_num` 秒 |
| `sample_rate` | 音频采样率，非 0 |
| `bytes_per_frame` | 一个音频样本帧的字节数 = 声道数 × 每样本字节数，非 0 |
| `backpressure_bytes` | 排队超过它就停止喂音频；0 = 库缺省一秒音频当量（缺省取在实测的音频排队自然峰值之上：这道闸拦的是「喂」，而单线程读包里一等等住的是整条腿，视频出点在等待里过期就是成串丢帧） |
| `preroll_bytes` | 预灌目标，供调用方自查；0 = 库缺省 200 ms 当量。节器件自己不做预灌 |
| `queued` | 必填回调：取「音频流里还没播出去的字节数」，返回 0 表示这一次问不出（设备已断等），此时节器件沿用上一次登记的已消费量，不把失败读成零 |
| `now` | 可选的单调钟（纳秒）；`NULL` = 库内部用自己的单调钟。由调用方供钟的唯一理由是可测：用例要拿假时钟走完整条时间轴 |
| `queued_user` / `now_user` | 随各自回调透传，可为 `NULL` |

`CLV_PaceDecisionC`（判定出参）：

| 字段 | 含义 |
|---|---|
| `struct_size` | 显式填 `sizeof(CLV_PaceDecisionC)` |
| `action` | 取 `CLV_PaceActionC`：`WAIT` = 视频超前，等 `wait_ns` 再重判；`PRESENT` = 落在对齐带内，或迟到但未过丢帧阈；`DROP` = 迟到超过一整个帧周期 |
| `in_band` | dev 的绝对值 ≤ period/2 |
| `late` | dev 小于 0（视频落后于音频钟） |
| `pad_` | 显式填充位，不留未初始化字节 |
| `dev_seconds` | pts − 主时钟；正 = 视频早，负 = 视频迟 |
| `clock_seconds` / `period_seconds` | 本趟主时钟当前秒数与帧周期 |
| `wall_seconds` | 本趟起播零点以来的真实时长（墙钟闸的当前值） |
| `wait_ns` | 仅 `WAIT` 有效：等到落入对齐带所需的纳秒 |

阈值只有 `period` 一个来源，其余全是它的倍数，不给调用方随手调：对齐带 = dev 的绝对值 ≤ period/2，等待阈 = dev > period/2，丢帧阈 = dev < −period。`WAIT` 的等待量取两道闸的较大值：墙钟闸（pts 还没到出点，不许提前出画）与音频闸（视频超前于音频钟，追到带心而不是带边）；出点已过而音频钟追不上时按出点出画。

### 8.4 `CLV_PaceError`

| 值 | 名称 | 触发 |
|---|---|---|
| 0 | `CLV_PACE_OK` | 成功 |
| 96 | `CLV_PACE_INVALID_HANDLE` | 句柄为空或已关闭 |
| 97 | `CLV_PACE_INVALID_ARGUMENT` | 入参指针为空、参数结构体或出参结构体的自报长度不合规、回调缺失 |
| 98 | `CLV_PACE_VALUE_RANGE` | 字段值域不成立：fps 分子分母为 0、字节率与采样参数自相矛盾等 |
| 99 | `CLV_PACE_STATE_ERROR` | 顺序不成立：未 `BeginPass` 就读钟 / 判定 / 置设备态 / 记喂入 |
| 100 | `CLV_PACE_CLOCK_SKEW` | 单调钟回退：本帧拿到的 `now` 比已登记的锚点还早，外推无从算起 |
| 101 | `CLV_PACE_OUT_OF_MEMORY` | 唯一的分配失败源是句柄本身：`Open` 判它并返回 `NULL` |

---

## 9 最小示例

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

## 10 构建开关对调用方可见的差异

| 开关 | 默认 | 对公开面的影响 |
|---|---|---|
| `CLV_BUILD_SHARED` | OFF | ON 时**另产**动态件 `clv`（`clv.dll` + 导入库），静态件 `clv_static` 恒在、`clv_tests` 只链它；三份示例在 ON 时改链动态件。
  OFF 时只有 `clv_static`，与既往形态一致。两目标输出名错开（`clv_static.lib` / `clv.dll` + `clv.lib`），实测并存不互撞 |
| `CLV_BUILD_DLL` / `CLV_USE_DLL` | —— | **不是 CMake 开关**，是导出宏的两个组合位：前者由动态目标的 PRIVATE 带、后者由同一目标的 INTERFACE 带 ⇒ 调用方链上目标就拿到对应声明，不需要自己定义 |
| `CLV_LOGGER_USE_SPDLOG` | OFF | OFF 时链接空后端，`CLV_Log` 与 `CLV_LogSetLevel` 全部丢弃，日志类验收必须用 ON 的构建 |
| `CLV_BUILD_DEMOS` | OFF | ON 时构建 `demo/` 三份示例（第三份是双轨播放器，另需外部 SDL3；缺它就是配置期硬失败） |
| `CLV_WARNINGS_AS_ERRORS` | OFF | 自有 target 的 `/W4 /permissive-` 升级为错误 |

CRT 不能混用：调用方的运行时必须与库一致（静态 `/MT*` 与动态 `/MD*` 不可混链）。

---

## 11 占位与未落地

| 符号 | 现状 |
|---|---|
| `CLV_OpenMemoryFile` | 声明与定义都在，返回 `CLV_FILE_UNSUPPORTED`、`*out_handle` 为 `NULL`，内存后端尚未实现 |
| `CLV_LogSetStorage` | 调用无效果，落盘日志后端尚未实现 |
| 视频编码句柄 | 真实现：`CLV_VideoEncoderOpen` / `CLV_VideoEncoderAllocFrame` / `CLV_VideoEncoderEncode` / `CLV_VideoEncoderClose` 吃源帧吐载荷。帧间预测的参考是**编码器自己产出的重建帧（闭环）**：参考缓冲由编码器自管，调用方既不提供也无法替换，喂进来的原始帧只作残差比对的被减数。v1 无环路滤波，进参考的就是未滤波的重建；同一条件下编码器内部重建与解码器输出逐字节相同，故此闭环不引入两侧漂移 |
| 视频解码句柄 | 真实现：`CLV_VideoDecoderDecode` / `NextOutput` / `Flush` / `StreamRejected` 吃码流吐帧，自管参考缓冲与待输出队列，出帧按显示序，致命码拒解后粘滞。编码端同样真实现（产 VOP 载荷与宏块序列，做内部重建、运动估计与参考帧自管）。系数载体有两种写法：定长档与法 1 档（先写结束标志 EOB，再用位数递增的 Exp-Golomb 编码写幅值）；具体用哪一档由视频帧头第 5 到第 7 位声明，本库默认法 1。**目前只限两项**：B 帧编码侧一律 direct + `not_coded`，不带残差与块数据；10 / 12-bit 的像素电平未定义 |
| 音频句柄 | 编解码闭环已实现：`CLV_AudioDecoderOpen` / `CLV_AudioDecoderDecodeFrame` / `CLV_AudioEncoderOpen` / `CLV_AudioEncoderWrite` / `CLV_AudioEncoderNextFrame` / `CLV_AudioEncoderFlush` 等全部接线到 libFLAC 后端（帧头自核 + 流声明对账 + 一包一帧）；无状态的帧头自查 / 承载块拆分 / 掩码剥离 / 流信息解析同为实现态 |
| 容器面全部符号 | 形状属对外 ABI（改动即破坏调用方）；未冻结的是 `.clv` 的字节格式与语义 |
| 单文件长度 | 结构不设上限（偏移类字段是 u64）；分段容器已落地（`flags` bit3 + `file_ext_offset` + `ext_type = 7` 段描述块 + 多段读口），段索引表（`ext_type = 8`）值已留、未实现 |
| 跨线程与跨实例并发 | 语义未定义，不由本 ABI 承诺 |

---

## 12 验收清单

1. 按 §2 表逐个头文件包含，确认 C 与 C++ 两种编译方式都能通过。
2. 打印 `CLV_VersionString()` 与三段数字，核对与 `include/CLV_Version.h` 的宏同源。
3. 跑 `CLV_BUILD_DEMOS=ON` 构建出的三份示例：`clv_demo_core` 与 `clv_demo_container` 退出码 0 表示自动判定项全过；`clv_demo_video` 吃一份双流 `.clv`，两轨同时出（末行 `DEMO_VIDEO_DONE` 打印且 `dropped = 0`），只出单轨不算过；逐项预期见 `demo/README.md`。
4. 对每个错误枚举调一次 `StrError`，确认每个值都返回非空 ASCII 串、未知值与退场留白号位返回 `"unknown"`。五个错误枚举（文件、容器、视频、音频、节拍）各有一条全枚举遍历用例，容器分段的两个收束码另有一条，共六条：`ClvFile.StrErrorCoversEveryCode`、`ApiContainer.StrErrorCoversEveryCode`、`VideoHeaders.PublicSurfaceStaysInsideVideoSegment`、`AudioFraming.StrErrorNamesEveryDefinedCode`、`PaceApiStandalone.StrErrorCoversEveryCodeAndUnknown`、`Segments.StrErrorCoversBothSegmentCodesWithText`。
5. 逐函数验 `NULL` 入参：句柄为 `NULL` 回 `INVALID_HANDLE`，出参为 `NULL` 回 `INVALID_ARGUMENT`，都不许崩。
6. 用 `CLV_FILE_INVALID_PATH` 与容器口的 `CLV_CONTAINER_IO_FAILED` 核对 §1.1 的路径契约与码值收敛。
7. 用 `assets/fixtures/` 三份素材核 §7.5 的统计口径：正常读回时全部计数为 0。
8. 跑 `ctest`，确认 **375 例**全过；其中只吃公开 C ABI 的 `api` 面 76 例是本文的机器可读对账。`core` 域直测不出 ABI 的内部工具（CRC-32/MPEG-2、LEB128 与 zigzag），`container` / `audio` / `video` 三个域两面混吃：句柄端到端走公开面，单元级用例直接 include `src/` 下的实现层头，用来钉变换 / 量化 / 块载体 / 运动补偿 / 参考帧管理这些不进 ABI 的行为；`clv_tests` 因此恒链静态件 `clv_static`，实现层符号不带导出宏、不进动态件的导出表。`ctest -R Independent` 单跑独立验证层那 21 例，它们与调用方契约无关，是仓内的边界与分辨力检查。



