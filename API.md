# CLV 公开接口（C ABI）

> 本文是 CLV 公开 C ABI 的权威说明：头文件分工、句柄与错误码约定、逐个函数的入参出参语义、可复制示例与验收步骤。  
> 文档版本：0.9.1 · 日期：2026-10-29 · 工程版本以 `include/CLV_Version.h` 的四个宏为单一来源。  
> **冻结声明**：既有公开面自 `0.8.2` 起冻结到 1.0；**字幕面自 0.9.0 追加，随 1.0 一并冻结**。冻结面 = 公开函数签名 + 结构体字段形状 + **七套**错误枚举（文件 / 容器 / 音频 / 视频 / 节拍 / 日志 / 字幕）的值序与段位，字幕段 106–117、118+ 留白不复用；未冻结 = `.clv` 的字节语义、平台支持范围、性能承诺。  
> 平台支持范围 = Windows x64 已验证；Linux / macOS 按 POSIX 语义实现但**未经非 Windows 编译验证**，不作可用承诺；iOS 只有 `platform/platform.h` 的检测分支。  
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
| 枚举值序即 ABI | 七套错误枚举的值序不可改：改序、插位、删位都是破坏性变更，新码只能追加在本面段位的末尾；退场码留下的号位不复用 |
| 七面错误码各自独立 | 七面的错误码各自独立枚举：文件与容器从 1 顺排、音频 32 到 63、视频 64 到 95、节拍 96 到 101、日志 102 到 105、字幕 106 到 117，不得跨头比对数值——各头的 `1` 语义无关 |
| 错误转文本 | 七个错误枚举各有一个转文本口：`CLV_FileStrError`、`CLV_ContainerStrError`、`CLV_VideoStrError`、`CLV_AudioStrError`、`CLV_PaceStrError`、`CLV_LogStrError`、`CLV_SubtitleStrError`；每个口对所属枚举的每个值返回非空、静态存储期的 ASCII 串，未知值与退场留白号位返回 `"unknown"` |
| 结构体自报尺寸 | **枚举式，不按字节长度猜版本**。容器 / 视频 / 音频三面的参数结构体受理集写死成三档：`0` ⇒ 按当前布局受理（存量 memset 写法的口子）、`上一版尺寸`（**只有容器面 `CLV_WriteParams` 有这一档** = 16，只认前六个字段）、`sizeof(本结构)` ⇒ 显式声明本布局；**其余任何值一律显式拒**（容器 `CLV_CONTAINER_INVALID_ARGUMENT`、视频 `CLV_VIDEO_VALUE_RANGE`、音频 `CLV_AUDIO_VALUE_RANGE`），库拿不到「尺寸像哪一版」这种推断当结论。字幕两面的参数结构体没有「上一版尺寸」这一档：`0` 与 `sizeof(本结构)` 受理，其余一律 `CLV_SUBTITLE_INVALID_ARGUMENT`（106）拒。两面各结构体的受理集在自己那一行里逐条写明，不只靠指路。**节拍面另有两档**：`CLV_PaceParamsC` / `CLV_PaceDecisionC` 的受理集是「`0`、上一版尺寸、本版尺寸、比本版更大」都收，**介于两版之间的显式长度（87 / 71）也在受理集内** —— 上一版 = 88 之前的 72 与 72 之前的 56，只有「非零且比上一版更小」判 `CLV_PACE_INVALID_ARGUMENT`（97）。与上面各面不同的是 `0` 的读法：节拍面把 `0` 当「长度信息缺失」，两侧都按最保守的上一版前缀处理（写侧不碰尾部、读侧新字段取缺省），因为按清零写的存量调用方可能只有前缀那么长；其余各面的 `0` 按本版布局受理。两面逐条写在 §8.1。 |
| 号位口径 | 七面的段位与留白：文件 **1–10**（值 9 自内存口落地起**无产出者**，留白不复用）｜容器 **1–13**（值 10 同样无产出者，不复用）｜**15–31 不预分配给任何面**（这一段没有主人，别从这儿挑码）｜音频 **32–63**（41 / 45 留白不复用）｜视频 **64–95**（73 / 76 / 77 / 78 / 81 / 82 无产出者、留白不复用；其中 76 / 77 的 `StrError` 仍有文案；已用到 85）｜节拍 **96–101**｜日志 **102–105**｜字幕 **106–117**（118+ 留白、不复用、不因任何退场而前移）。追加新码只允许落在**本面段尾的下一格**；留白号位永不重新投给新语义，因此既有码值不因某格退场而前移。各面 `StrError` 对留白号位照旧返回文案 |
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
| `CLV_Pace.h` | `CLV_PaceOpen` 的 `err`、`CLV_PaceStats` 的**全部四个**诊断出参 | `CLV_PaceClockSeconds` 的 `out_seconds`、`CLV_PaceDecide` 的 `out`、`CLV_PaceFeedAllowed` 的 `out_allowed`；`CLV_PaceDecide` 另按 `out->struct_size` 决定写多长，受理档见 §8.1（非零且小于上一版 56 才拒） |

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
| `include/CLV_Export.h` | `CLV_EXPORT` 可见性开关 + 能力位名 | 4 宏（1 可见性 + 3 能力位） | 稳定；能力位号段尾追加，`CLV_CAP_HOST_ALLOCATOR` 恒 0 且永不改义 |
| `include/CLV_Version.h` | 版本查询 + 本构建形状 | **5 函数** + 7 宏（4 版本宏 + 3 能力位） | 稳定；能力位段尾追加、已定义位不改义 |
| `include/CLV_File.h` | 文件读写：句柄 + 三模式 + 11 错误码 | 10 函数 | 稳定（文件后端与内存缓冲区后端均为实现态；值 9 是留白号位） |
| `include/CLV_Logger.h` | 日志：四等级 + 过滤 + 落盘 + 错误码 + 后端与等级查询 | **7 函数** + 2 枚举（等级 4 值、错误码 5 值 = `OK` 加 102 到 105） | 稳定；落盘口的形状与码值段尾追加同其余五面一条 |
| `include/CLV_Memory.h` | 分配口 | 3 函数 | 稳定（C 运行时直通）。**便利口不是注入点**：库内分配不经这三只口，宿主重载全局 `operator new` 只影响库内部那一路，两套不承诺同源；接管库内分配 = 未提供能力（bit2 恒 0）|
| `include/CLV_Container.h` | 容器读写面 | **25 函数**（写方 7 + 读方 17 + `StrError`）+ **10 结构体** + 14 错误码 | 函数名 / 参数结构体 / 出参形态属对外 ABI，改动即破坏调用方；错误码只在枚举尾部追加、既有码值与语义不动；未冻结的是 `.clv` 的字节格式与语义 |
| `include/CLV_Video.h` | 视频编解码面 | **23 函数**（无状态 2 + 解码 7 + 编码 4 + 码流 4 + 帧 5 + `StrError`）+ 5 结构体 + 6 枚举 | 编解两侧的**有损重建闭环已接**，**无损档已接**（编码侧 `CLV_VideoEncParamsC.mode`，无损的模式信号在码流里 = 帧头 / 片头的 `vop_quant = 0`，解码侧不接模式参数）；**运动补偿已接**（帧间按运动矢量取样，半像素双线性平均 + 参考缓冲边缘复制带；B 帧编码侧目前只产 direct + `not_coded`）；帧的借用与归还走引用计数（`CLV_VideoFrameFree`），逐帧像素出库走 `CLV_VideoFrameToRGB24`；函数级说明见本头每个导出声明上的注释 |
| `include/CLV_Audio.h` | 音频编解码面 | **28 函数**（无状态 5 + 承载块解读 4 + 解码 6 + 编码 6 + 缓冲 3 + 大块 3 + `StrError`）+ 10 结构体 + 3 枚举 + **16 个失败码**（计数一律不含 `OK`；段内 41 / 45 两格是退场留白、号位不复用，所以槽位 18 个、能拿到的失败码 16 个）| 编解码闭环已实现（libFLAC 后端接线到帧级：帧头自核 + 流声明对账 + 一包一帧）；**承载块内容的解读面已接**（Vorbis 注释键值与图标块字段，只读、不复制字节）；结构层与句柄面均为实现态；函数级说明见本头每个导出声明上的注释 |
| `include/CLV_Pace.h` | 呈现节拍面（主时钟、逐帧判定、积压水位与速率匹配建议、背压） | **12 函数**（建仓 1 + 拆仓 1 + 起趟与设备态 2 + 记账三口：喂入 / 报消费量 / 报减账 + 诊断查询 1 + 读钟 1 + 判定 1 + 背压 1 + `StrError`）+ 2 结构体 + 2 枚举（错误码 7 值 = `OK` 加 96 到 101 六个失败码）+ 2 回调类型 | 已实现：把「音频侧消费量当主时钟、视频按 pts 追它」这套口径从播放器里抽出来，**本头不依赖任何播放器库**，主钟的数值由消费端报告、库不外推不封顶不阻尼，库自己既不碰线程也不碰设备；结构体自报字段受理 `0` / 上一版 / 本版 / 更大四档，非零且小于上一版才拒，写侧按递交长度控长（见 §8.1）；逐函数说明见 §8 |

| `include/CLV_Subtitle.h` | 字幕面（解析器 + 渲染器） | **19 函数**（解析器 10 + 渲染器 8 + `StrError`）+ 9 结构体 + 7 枚举 + 2 不透明句柄（错误码 13 值 = `OK` 加 106 到 117 十二个失败码，118+ 留白不复用） | 解析器与载荷 / 样式块编解码为**真实现**；渲染器整体由 `CLV_BUILD_SUBTITLE_RENDERER` 决定编进与否，不在时渲染口如实回 `CLV_SUBTITLE_UNSUPPORTED`，能力位 bit4 与之一致（`CLV_Capabilities` 与 `CLV_SubtitleRendererOpen` 两处读同一开关）；逐函数说明与错误码触发表见 §11 |

公开头共十份（CLV_Audio.h、CLV_Container.h、CLV_Export.h、CLV_File.h、CLV_Logger.h、CLV_Memory.h、CLV_Pace.h、CLV_Subtitle.h、CLV_Version.h、CLV_Video.h）；四份示例直接 include 用到其中九份，唯一没直接用到的是导出宏头 CLV_Export.h，它由各接口头自己带进来。

三份媒体面的函数名清单（与三份头的 `CLV_EXPORT` 声明逐字对齐，改函数必须同步这一段）：

- 视频：`CLV_VideoCodedSize`、`CLV_VideoParseEncMeta`、`CLV_VideoDecoderOpen`、`CLV_VideoDecoderDecode`、`CLV_VideoDecoderNextOutput`、`CLV_VideoDecoderFlush`、`CLV_VideoDecoderReset`、`CLV_VideoDecoderStreamRejected`、`CLV_VideoDecoderClose`、`CLV_VideoEncoderOpen`、`CLV_VideoEncoderAllocFrame`、`CLV_VideoEncoderEncode`、`CLV_VideoEncoderClose`、`CLV_VideoBitstreamGetBytes`、`CLV_VideoBitstreamSliceCount`、`CLV_VideoBitstreamGetSlice`、`CLV_VideoBitstreamRelease`、`CLV_VideoFrameAddRef`、`CLV_VideoFrameRelease`、`CLV_VideoFrameFree`、`CLV_VideoFrameGetDesc`、`CLV_VideoFrameToRGB24`、`CLV_VideoStrError`
- 音频：`CLV_AudioInspectFrame`、`CLV_AudioParseStreamInfo`、`CLV_AudioSplitMetaBlock`、`CLV_AudioClassifyMetaBlock`、`CLV_AudioStripChannelMask`、`CLV_AudioParseVorbisComment`、`CLV_AudioVorbisCommentEntryAt`、`CLV_AudioVorbisCommentFindKey`、`CLV_AudioParsePicture`、`CLV_AudioDecoderOpen`、`CLV_AudioDecoderDecodeFrame`、`CLV_AudioDecoderAddMetaBlock`、`CLV_AudioDecoderReset`、`CLV_AudioDecoderStats`、`CLV_AudioDecoderClose`、`CLV_AudioEncoderOpen`、`CLV_AudioEncoderWrite`、`CLV_AudioEncoderNextFrame`、`CLV_AudioEncoderFlush`、`CLV_AudioEncoderTakeStreamInfo`、`CLV_AudioEncoderClose`、`CLV_AudioBufferAddRef`、`CLV_AudioBufferRelease`、`CLV_AudioBufferGetDesc`、`CLV_AudioBlobAddRef`、`CLV_AudioBlobRelease`、`CLV_AudioBlobGetBytes`、`CLV_AudioStrError`
- 字幕：`CLV_SubtitleStrError`、`CLV_SubtitleParserOpen`、`CLV_SubtitleParserFeed`、`CLV_SubtitleParserSetStyleBlock`、`CLV_SubtitleParserReset`、`CLV_SubtitleParserBlockCount`、`CLV_SubtitleParserBlockAt`、`CLV_SubtitleParserLineCount`、`CLV_SubtitleParserLineAt`、`CLV_SubtitleParserStats`、`CLV_SubtitleParserClose`、`CLV_SubtitleRendererOpen`、`CLV_SubtitleRendererSetStyleBlock`、`CLV_SubtitleRendererSetFontDir`、`CLV_SubtitleRendererLayout`、`CLV_SubtitleRendererDirtyRects`、`CLV_SubtitleRendererRender`、`CLV_SubtitleRendererReport`、`CLV_SubtitleRendererClose`

容器面写方第七个是 `CLV_ContainerWriterWriteFrameChunks`（配 `CLV_Chunk`），它把分片边界交给调用方，容器不再按 `fragment_chunk_size` 二次切；原 `CLV_ContainerWriterWriteFrame` 不动。

音频面的只读帧户口 `CLV_AudioInspectFrame` 有**两个**失败出口，两种场合都不写 `*out`：

- `CLV_AUDIO_FRAME_HEADER_BAD`：帧头自身读不通（同步码不符、字段取保留值、帧尾校验不符）。
- `CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE`：帧头读得通，但块大小越出流式子集上界（块大小大于 16384；采样率不大于 48000 时上界收到 4608）。这一档与上一档是两个不同的失败原因，走的是同一只口。

入参不成立（`payload` 为空、`size` 为 0、`out` 为空）判 `CLV_AUDIO_INVALID_ARGUMENT`。

视频面的帧对象始终归库所有，取帧口交出去的只是带计数的借用句柄：取帧口交出帧时引用计数加一；调用方用毕调 `CLV_VideoFrameFree` 减一；计数归零的帧才回收进空闲池。Free 之后该帧指针失效，建议立即置空。重置或关闭会使所有已交出的帧指针失效，此后再调 Free 返回参数不成立。对同一帧重复调用返回参数不成立；跨复用后的陈旧调用属调用方未定义行为，本 ABI 不承诺幂等。`CLV_VideoFrameAddRef` 与 `CLV_VideoFrameRelease` 是同一个计数上的两扇门（加计数并回报当前值 / 减计数不回报），对 `NULL` 分别是返回 0 与 no-op，都不崩溃。`CLV_VideoFrameToRGB24` 把 8-bit YUV420P 帧转成 `3 × 宽 × 高` 字节、行间不留填充的 RGB24 缓冲，色度按 2×2 最近邻放大而不做插值（增益式、移位与夹取口径见该口的导出注释；位深不是 8 走 `CLV_VIDEO_BIT_DEPTH_UNSUPPORTED`）。

**1.0 承诺集**（清单式；这五格是「形状」而非「本版实现细节」，到 1.0 不改，改它算破坏性变更）：

1. 取帧口交出帧时引用计数**加一**；
2. `CLV_VideoFrameFree` **减一**（它是归还口，不是 `free` 语义的释放口）；
3. 计数**归零才回收**进空闲池 —— 「还有一处没归还」的帧不会被回收，可回收不等于全回收；
4. 对同一帧**重复归还**判 `CLV_VIDEO_INVALID_ARGUMENT`（值 65），且不改变计数；
5. **重置或关闭作废之后**再归还同一格，同样判 65（关闭那一侧只承诺「句柄不得再用」，不承诺归还能回报什么）。

同一承诺集在 `include/CLV_Video.h` 的该口注释里逐字对得上；两处的措辞都不带「本版本」这类时效限定。

---

## 3 `CLV_Version.h`

```c
const char* CLV_VersionString(void);   /* 形如 "0.9.1-dev" */
int         CLV_VersionMajor(void);
int         CLV_VersionMinor(void);
int         CLV_VersionPatch(void);
uint32_t    CLV_Capabilities(void);
```

| 宏 | 含义 |
|---|---|
| `CLV_VERSION_MAJOR` / `MINOR` / `PATCH` | 三段数字，唯一来源 |
| `CLV_VERSION_STRING` | 带预发布后缀的完整串 |
| `CLV_CAP_MEMORY_FILE`（bit0） | 内存缓冲句柄可用。`CLV_OpenMemoryFile` 是实现态 ⇒ 本构建恒置 1 |
| `CLV_CAP_LOG_STORAGE`（bit1） | 日志可落盘。spdlog 后端置 1，空后端置 0 |
| `CLV_CAP_HOST_ALLOCATOR`（bit2） | **恒 0，且那一格永不赋予新义**：宿主注入分配器不在本 ABI 的能力范围内 |

四个版本函数与四个版本宏同源；`src/version.cpp` 读宏不写字面量。字符串含 `-dev` 后缀，三段数字只表达数值部分。容器格式版本（文件头 `version_major` / `version_minor`）与工程版本分轨，不经本头查询。

`CLV_Capabilities` 回答的是**这一份构建编进了什么**：位名在 `CLV_Export.h`，返回值不随任何调用改变（没有任何入口能把一位从 0 翻成 1，也没有入口让句柄「暂时不支持」）。3 号位以上未分配，读到 0 不携带信息——不要据此推断「以后会是 1」。判据侧的用法是把 bit1 与 `CLV_LogBackendName()` 对平：两处读同一个构建选项，等式不成立即为漂。

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
CLV_FileError  CLV_OpenMemoryFile(void* buffer, size_t size, CLV_FileMode mode, CLV_File** out_handle);
const char*    CLV_FileStrError(CLV_FileError err);
```

`CLV_OpenFile` 失败只返回 `NULL`，忽略错误细分；要原因用 `CLV_OpenFileEx`：成功时返回非空句柄且 `*err == CLV_FILE_OK`，失败时返回 `NULL` 且 `*err` 是具体码。

`CLV_OpenMemoryFile` 把调用方给的那块内存包成句柄：缓冲区全程归调用方，库不分配、不 `free`、不 `realloc`，`CLV_CloseFile` 只销毁库内的游标对象。`mode` 只受理 `READ` 与 `WRITE` 两档（`APPEND` 在这一族里没有对应概念，与越界值同落 `CLV_FILE_INVALID_ARGUMENT`）；容量固定，写档越出 `size` 整次拒并回 `CLV_FILE_WRITE_FAILED`、`*bytes_written` 写 0，绝不半次写、绝不扩。`Size` 在只读档给容量、只写档给被写覆盖过的最远字节数。短读语义、模式门控、`Seek` 越过末尾合法这三条与文件后端一致。`out_handle == NULL`、或 `buffer == NULL` 而 `size > 0` 都回 `CLV_FILE_INVALID_ARGUMENT` 并把 `*out_handle` 置 `NULL`；`size == 0` 是一份合法的空缓冲。本口自 `[0.8.2]` 起带 `mode` 入参，**形状相对 `0.0.1` 起的三参版本是破坏性变更**。

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
| 9 | `CLV_FILE_UNSUPPORTED` | **留白号位，不再有产出者**：文件面每一族入口都已实现，值 9 不投给任何新语义，后续新码一律段尾追加（`CLV_FileStrError(9)` 仍返非空文案，既有码值不前移） |
| 10 | `CLV_FILE_INVALID_ARGUMENT` | 模式值非法，或 `size != 0` 而缓冲指针为 `NULL`（读写两侧、文件句柄与内存句柄同式）；`CLV_OpenMemoryFile` 的 `out_handle == NULL`、`APPEND` 落内存句柄也归这一档 |

---

## 5 `CLV_Logger.h`

```c
void CLV_Log(CLV_LogLevel level, const char* msg);
void CLV_LogSetLevel(CLV_LogLevel level);
CLV_LogLevel CLV_LogGetLevel(void);
const char*  CLV_LogBackendName(void);
void CLV_LogSetStorage(const char* filepath, size_t rotate_size_kb);
CLV_LogError CLV_LogSetStorageEx(const char* filepath, size_t rotate_size_kb, size_t max_files);
const char*  CLV_LogStrError(CLV_LogError err);
```

| 约定 | 行为 |
|---|---|
| 等级 | `CLV_LOG_DEBUG` / `INFO` / `WARN` / `ERROR` 四值 |
| `msg == NULL` | 整条丢弃，不记错误 |
| `level` 越界 | `CLV_Log` 整条丢弃；`CLV_LogSetLevel` 是 no-op，保持原等级 |
| 默认过滤等级 | `CLV_LOG_INFO`（构造后即生效） |
| 等级查询 | `CLV_LogGetLevel` 回「当前设定并用于过滤的那一档」，构造后 = `CLV_LOG_INFO`；空后端不过滤，它在回这一问时**恒等于构造缺省**（`SetLevel` 不改变任何可观测行为） |
| 错误通道 | 投递侧**无**（日志失败不许影响宿主）；落盘侧有，走 `CLV_LogError`——本面唯一有回报的入口是 `CLV_LogSetStorageEx` |
| 落盘 | `CLV_LogSetStorageEx` 装上文件落点：`rotate_size_kb` 是单份轮转上限（KB，0 判参数档），`max_files` = 0 不轮转（一路追加、不重置已有内容）、`N>0` 时基础件之外最多再留 N 份轮转件，上限 200 000 自钉。再次调用**换**落点不叠加，控制台输出与文件并存 |
| 老落盘口 | `CLV_LogSetStorage` 转调新口并把结果码丢掉（`max_files` 固定取 1）：**无回报**，调用方看不出成没成。要结果一律用 Ex 那一只 |
| 路径 | 与文件面同一把尺：UTF-8 字节串、只用 `/`；`NULL`、空串、含 `\` 都判 `CLV_LOG_INVALID_PATH`，判定在任何后端之前。与文件面不同：本口不转宽字符，非 ASCII 路径能否落对由窄字符运行时决定，不作承诺 |
| 后端 | 由构建选项决定；`CLV_LogBackendName` 回 `spdlog` 或 `null`（ASCII 静态串、永不为 NULL），与 `CLV_Capabilities` 的 bit1 同源。空后端（`CLV_LOGGER_USE_SPDLOG=OFF`）下投递一律丢弃，`CLV_LogSetStorageEx` 回 `CLV_LOG_UNSUPPORTED_BACKEND`，不假装成功 |

### 5.1 `CLV_LogError`

| 值 | 名称 | 触发 |
|---|---|---|
| 0 | `CLV_LOG_OK` | 成功 |
| 102 | `CLV_LOG_INVALID_PATH` | 路径为 `NULL`、空串或含 `\` |
| 103 | `CLV_LOG_UNSUPPORTED_BACKEND` | 当前构建的后端落不了盘（空后端） |
| 104 | `CLV_LOG_OPEN_FAILED` | 路径合法但装不上落点：目录不存在、被占、权限不足 |
| 105 | `CLV_LOG_INVALID_ARGUMENT` | `rotate_size_kb == 0`、乘 1024 越界，或 `max_files > 200 000` |

段位：日志面从 102 起（节拍面末码 101 的下一格），末码停在 105；字幕面从 106 起。判定顺序固定为路径档 → 参数档 → 后端档 → 打开档，前三档与后端选型无关。

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

> **能力域口径**：本面把「音频侧消费量当主时钟、视频按 pts 追它」这套口径从播放器里抽出来。**不依赖任何播放器库**：设备侧只通过两个回调交进来（排队字节数、可选的单调墙钟），主钟的数值由消费端报告；具体用哪个音频设备、怎么睡、怎么画都由调用方决定，库自己既不碰线程也不碰设备。
>
> 主时钟三句话：① 主钟 = 消费端报告的「已消费媒体时间」，单位 = 建仓给的时基（缺省纳秒）；② 库**不外推、不封顶、不阻尼**，只在内部保证单调 —— 回退的报告值不采信，钟位的精度就是消费端的报告粒度，块状取数的设备两次报告之间会跳一格，那是器件形状不是要库补的洞；③ 设备不报，钟就停在原地，死队列保护层只报警、不替设备编进度。

### 8.1 调用顺序

```text
Open → BeginPass →（逐帧循环）NoteAudioFed / NoteConsumedMediaTime / SetDeviceRunning / ClockSeconds / Decide / FeedAllowed / Stats → Close
```

| 顺序规则 | 违反时的码或后果 |
|---|---|
| 未 `BeginPass` 就读钟、判定、置设备态、记喂入、报消费量、报减账 | `CLV_PACE_STATE_ERROR`（`FeedAllowed` 与 `Stats` 不在这一条上：前者只读当前排队量，后者只读已累计的诊断量，未起趟也返回 `OK`） |
| 每趟播放都要重新 `BeginPass` | 不清账会把上一趟的累计喂入、影子排队账、消费端报告值与本趟零点带进这一趟 |
| 本趟零点只定一次：`SetDeviceRunning` 第一次把设备放开的那一刻定它 | 零点若留给「第一次判定」，设备从放开起就开始计位，而第一帧还要解码十几到几十毫秒，这一截会长期算进钟位，落在对齐带里的出点永远等不到；恢复播放重定零点则会让暂停前后之间钟自己归零 |
| 报告值单调不减 | 回退的报告值整口拒：返回 `CLV_PACE_STATE_ERROR`、账一格不动，只在诊断量里记一笔「回退拒收次数」 |
| 消费端报告是主钟唯一的来源 | 没报告就没有钟位：库不拿墙钟、喂入量或排队读数替设备编一个进度。设备在播而连续 3 次判定报告无进展、或 500 ms 无进展（取先到）⇒ 死队列计数 +1，**不自动重锚**；设备按住的那段不计（那时候没有「该有进展」这个预期） |

**结构体自报字段**：`struct_size` 受理三档 —— 填 `0`、填上一版尺寸、填本版尺寸都收，填得比本版更大的显式长度照常受理（向前兼容更新版的调用方）；只有「非零且比上一版还小」判 `CLV_PACE_INVALID_ARGUMENT`。写侧按递交长度控制写入长度：递交旧尺寸时只写前缀，绝不越界碰调用方 buffer 的尾部；读侧逐字段按长度判读，递交长度不够的新字段取库内缺省值，不整块拷贝调用方给的字节。填 `0` 是「长度信息缺失」那一档，两侧都按最保守的上一版前缀处理 —— 存量按清零写的调用方可能只有前缀那么长。**「比上一版大、又比本版小」的显式长度**（`ParamsC` 的 87、`DecisionC` 的 71）同样受理，并按「不足本版」处理：尾部新字段不读也不写（读或写都会越进调用方没给的空间），因此这两档拿到的时基与积压上界是库缺省、交出的判定不含尾部五格。比本版更大的显式长度照常受理。本版与上一版的尺寸：`CLV_PaceParamsC` 88 / 72，`CLV_PaceDecisionC` 72 / 56。

### 8.2 函数

```c
CLV_Pace*        CLV_PaceOpen(const CLV_PaceParamsC* params, CLV_PaceError* err);
void             CLV_PaceClose(CLV_Pace* p);
CLV_PaceError    CLV_PaceBeginPass(CLV_Pace* p);
CLV_PaceError    CLV_PaceSetDeviceRunning(CLV_Pace* p, uint8_t running);
CLV_PaceError    CLV_PaceNoteAudioFed(CLV_Pace* p, uint64_t bytes_added);
CLV_PaceError    CLV_PaceNoteConsumedMediaTime(CLV_Pace* p, uint64_t media_ticks);
CLV_PaceError    CLV_PaceNoteAudioDropped(CLV_Pace* p, uint64_t bytes_dropped);
CLV_PaceError    CLV_PaceStats(CLV_Pace* p, uint32_t* out_dead_queue_count, uint32_t* out_clock_regress,
                               uint32_t* out_jump_edges, uint64_t* out_dropped_bytes);
CLV_PaceError    CLV_PaceClockSeconds(CLV_Pace* p, double* out_seconds);
CLV_PaceError    CLV_PaceDecide(CLV_Pace* p, double video_pts_seconds, CLV_PaceDecisionC* out);
CLV_PaceError    CLV_PaceFeedAllowed(CLV_Pace* p, uint8_t* out_allowed);
const char*      CLV_PaceStrError(CLV_PaceError code);
```

| 函数 | 语义 |
|---|---|
| `Open` | 按参数建仓；不成返回 `NULL` 并把原因写进 `err`（`err` 可为 `NULL`）。判定顺序：`params` 为空、`struct_size` 非零且小于上一版尺寸 72、`queued` 回调缺失 ⇒ `INVALID_ARGUMENT`；四个非零字段（`fps_num` / `fps_den` / `sample_rate` / `bytes_per_frame`）任一为 0，或 `sample_rate` 大于 16 777 216、`bytes_per_frame` 大于 256、`fps_num` 或 `fps_den` 大于 1 000 000 ⇒ `VALUE_RANGE`；时基（递交长度够时才读）不整除、分母大于 1 000 000，或「时基 × 1 000 000」在 64 位里可证会溢出 ⇒ `VALUE_RANGE`；句柄本身分配失败 ⇒ `OUT_OF_MEMORY`。缺省时基是 1e9（纳秒），与内部单调钟同一把尺 —— 值域判只约束会进乘法的那一侧，任何小于 1e9 的绝对上限都会把这个缺省值判死 |
| `Close` | 拆仓；对 `NULL` 是 no-op。句柄拆后调用方不得再用 |
| `BeginPass` | 起一趟：累计喂入、影子排队账、消费端报告值、本趟零点与墙钟基准、四个诊断量全部作废；设备态初值为「未放开」（`running` = 0） |
| `SetDeviceRunning` | 声明设备是否在播：`running` 非 0 = 放开、0 = 暂停或预灌。它**不改钟位**（主钟只跟报告走），管两件事：起播零点定在第一次放开的那一刻，死队列计时只在放开时走 |
| `NoteAudioFed` | 记一笔喂进设备的字节，**增量口径**，可多次调。它只进积压账（影子排队量与超扣校验），不进主钟 |
| `NoteConsumedMediaTime` | 主钟的供数口：消费端报告「已经消费掉的媒体时间」，单位 = 建仓给的时基。库内自证单调：回退的报告值判 `STATE_ERROR` 且不改账；未 `BeginPass` 同判 `STATE_ERROR`。报告粒度就是钟的精度 |
| `NoteAudioDropped` | 减账口：调用方在把陈旧积压丢掉**之前**报一笔「交给设备前就丢弃的字节」。先报后清：报账时按影子排队量校验，`bytes > queued` 判 `INVALID_ARGUMENT` 且三账不动；未 `BeginPass` 判 `STATE_ERROR`。不变式：`fed′ = fed − bytes`、`queued′ = queued − bytes`、主钟不变、`queued′ ≥ 0`。设备自己跳变（重置 / 清空）造成的丢不由这里报 |
| `Stats` | 诊断量：死队列报警次数、主钟回退拒收次数、设备跳变边沿次数、自报减账累计字节。四个出参都可为 `NULL`（只取要看的）。它们不参与主钟与判定，是给调用方与用例取证用的旁路量 |
| `ClockSeconds` | 本趟主时钟当前秒数，**相对起播零点**。等于报告值减零点，只做单调钳。调用方供的墙钟回退那一次返回 `CLV_PACE_CLOCK_SKEW`（钟位本身不受墙钟影响） |
| `Decide` | 判这一帧：入参是本帧 pts（秒），出动作、对齐带与迟到分类、积压水位与速率匹配建议。出参按 `struct_size` 决定写多长，递交旧尺寸时尾部一个字节都不碰。返回 `CLV_PACE_CLOCK_SKEW` 时 `out` 仍按上面这条规则写入 —— 那是「墙钟回退、基准已重登记」这一拍的边沿，只持续一次 |
| `FeedAllowed` | 背压判定：`*out_allowed` 为 0 表示设备里还堆着，先别喂。阈值 = `backpressure_bytes`，读的是设备自己的排队读数（问不出就沿上一次读数） |
| `StrError` | 每个枚举值返回非空、静态存储期的 ASCII 串；未知值返回 `"unknown"` |

### 8.3 结构体

`CLV_PaceParamsC`（建仓入参，本版 88 字节）：

| 字段 | 含义 |
|---|---|
| `struct_size` | 显式填 `sizeof(CLV_PaceParamsC)`，受理档见 §8.1 |
| `fps_num` / `fps_den` | 帧率分子与分母，都非 0 且不大于 1 000 000；帧周期 `period = fps_den / fps_num` 秒 |
| `sample_rate` | 音频采样率，非 0 且不大于 16 777 216 |
| `bytes_per_frame` | 一个音频样本帧的字节数 = 声道数 × 每样本字节数，非 0 且不大于 256 |
| `backpressure_bytes` | 排队超过它就停止喂音频；0 = 库缺省一秒音频当量（缺省取在实测的音频排队自然峰值之上：这道闸拦的是「喂」，而单线程读包里一等等住的是整条腿，视频出点在等待里过期就是成串丢帧） |
| `preroll_bytes` | 预灌目标，供调用方自查；0 = 库缺省 200 ms 当量。节器件自己不做预灌 |
| `queued` | 必填回调：取「音频流里还没播出去的字节数」，返回 0 表示这一次问不出（设备已断等），此时节器件沿用上一次登记的读数、把积压水位报成 `UNKNOWN`，不把失败读成零 |
| `now` | 可选的单调墙钟（纳秒）：只管「不许提前出画」那道闸与死队列计时，不参与主钟的数值；`NULL` = 库内部用自己的单调钟。由调用方供钟的唯一理由是可测 |
| `queued_user` / `now_user` | 随各自回调透传，可为 `NULL` |
| `max_latency_bytes` | 积压上界，水位按它分档；0 = 库缺省 2000 ms 当量 = 2 × `sample_rate` × `bytes_per_frame` |
| `timebase_num` / `timebase_den` | 消费端报告媒体时间用的时基，`0` = 缺省 1e9 / 1（纳秒）。分母不大于 1 000 000、须整除分子，且「时基 × 1 000 000」可证不溢出；递交长度不足 88 时这两个字段不读，取缺省 |

`CLV_PaceDecisionC`（判定出参，本版 72 字节；上一版 56 字节 = 到 `wait_ns` 为止）：

| 字段 | 偏移 | 含义 |
|---|---|---|
| `struct_size` | 0 | 受理档见 §8.1；填 0 与递交 56 都只写前缀 |
| `action` | 4 | 取 `CLV_PaceActionC`：`WAIT` = 视频超前或还没到出点，等 `wait_ns` 再重判；`PRESENT` = 落在对齐带内，或迟到但未过丢帧阈；`DROP` = 迟到超过「一个帧周期 + 实测最大报告步长」 |
| `in_band` | 8 | dev 的绝对值 ≤ period/2 |
| `late` | 9 | dev 小于 0（视频落后于音频钟） |
| `pad_` | 10 | 显式填充位，不留未初始化字节 |
| `dev_seconds` | 16 | pts − 主时钟；正 = 视频早，负 = 视频迟 |
| `clock_seconds` | 24 | 本趟主时钟当前秒数（同样相对起播零点） |
| `period_seconds` | 32 | 帧周期 |
| `wall_seconds` | 40 | 本趟起播零点以来的真实时长（「不许提前出画」那道闸的当前值） |
| `wait_ns` | 48 | 仅 `WAIT` 有效：等到落入对齐带所需的纳秒 |
| `watermark` | 56 | 积压水位：0 = HEALTHY、1 = PRESSURE（超上界一半）、2 = OVERFLOW（超上界）、3 = UNKNOWN（排队读数问不出）。未 `BeginPass` 时整口返回 `STATE_ERROR` 且不写出参 |
| `queue_ms` | 60 | 当前积压毫秒，向下取整；`UNKNOWN` 时 0 |
| `resample_ratio_q16` | 64 | 建议重采样倍率，Q16：65536 = 1.0，钳在 ±5 % = [62259, 68813]。由消费端执行，库不碰音频数据 |
| `device_jump` | 68 | 设备跳变边沿：命中那次置 1，下一次判定清零，只持续一边沿 |
| `drop_dup_hint` | 69 | 速率匹配提示：0 = 正常、1 = 丢一帧、2 = 重一帧 |

70–71 两字节是尾部对齐用的隐式填充：库不写它，也不承诺它的值。

阈值只有 `period` 一个来源，不给调用方随手调：对齐带 = dev 的绝对值 ≤ period/2，等待阈 = dev > period/2，丢帧阈 = dev < −(period + 实测最大报告步长)。报告步长是从消费端的报告序列里量出来的器件形状 —— 块状取数的设备一次报告就跳一整块，那一格不算迟到；它不是拍的常数。`WAIT` 的等待量取两道闸的较大值：墙钟闸（pts 还没到出点，不许提前出画）与音频闸（视频超前于音频钟，追到带心而不是带边）；出点已过而音频钟追不上时按出点出画。

### 8.4 `CLV_PaceError`

| 值 | 名称 | 触发 |
|---|---|---|
| 0 | `CLV_PACE_OK` | 成功 |
| 96 | `CLV_PACE_INVALID_HANDLE` | 句柄为空或已关闭 |
| 97 | `CLV_PACE_INVALID_ARGUMENT` | 入参指针为空、参数结构体或出参结构体的自报长度非零且小于上一版、回调缺失、减账报得比还排着的多 |
| 98 | `CLV_PACE_VALUE_RANGE` | 字段值域不成立：fps 分子分母为 0 或越过 1 000 000、采样率或每帧字节数越界、时基不整除或分母越界或换算可证溢出 |
| 99 | `CLV_PACE_STATE_ERROR` | 顺序不成立：未 `BeginPass` 就读钟 / 判定 / 置设备态 / 记喂入 / 报消费量 / 报减账；报告值回退 |
| 100 | `CLV_PACE_CLOCK_SKEW` | 调用方供的单调墙钟回退：经过时长没有起算点，库把墙钟基准重登记到这一刻并只在这一拍返回它，钟位与出画判定都不因此改动 |
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
| `CLV_BUILD_SHARED` | OFF | ON 时**另产**动态件 `clv`（`clv.dll` + 导入库），静态件 `clv_static` 恒在、`clv_tests` 只链它；四份示例在 ON 时改链动态件。 OFF 时只有 `clv_static`，与既往形态一致。两目标输出名错开（`clv_static.lib` / `clv.dll` + `clv.lib`），实测并存不互撞 |
| `CLV_BUILD_DLL` / `CLV_USE_DLL` | —— | **不是 CMake 开关**，是导出宏的两个组合位：前者由动态目标的 PRIVATE 带、后者由同一目标的 INTERFACE 带 ⇒ 调用方链上目标就拿到对应声明，不需要自己定义 |
| `CLV_LOGGER_USE_SPDLOG` | OFF | OFF 时链接空后端，`CLV_Log` 与 `CLV_LogSetLevel` 全部丢弃，日志类验收必须用 ON 的构建 |
| `CLV_BUILD_DEMOS` | OFF | ON 时构建 `demo/` 四份示例（第三份是双轨播放器、第四份是字幕三轨播放器，那两份各需外部 SDL3；缺它就是配置期硬失败） |
| `CLV_BUILD_SUBTITLE_RENDERER` | ON | OFF 时渲染器不进库：八个渲染口一律回 `CLV_SUBTITLE_UNSUPPORTED`，能力位 bit4 落 0，`CLV_SubtitleStrError` 与解析器一族不受影响。公开头与函数名两档都在，问得出差别 |
| `CLV_WARNINGS_AS_ERRORS` | OFF | 自有 target 的 `/W4 /permissive-` 升级为错误 |

CRT 不能混用：调用方的运行时必须与库一致（静态 `/MT*` 与动态 `/MD*` 不可混链）。

---

## 11 `CLV_Subtitle.h`（自 0.9.0 追加）

字幕面只有两件：**解析器**与**渲染器**。**不含播放器句柄**、不含播放时钟 / 设备回调 / 音视频混合——
与视频帧的混合归宿主。本面自 0.9.0 追加，随 1.0 一并冻结。

**错误码（段 106–117；`0 = OK`；118+ 留白、不复用、不因任何退场而前移）**

| 值 | 名 | 触发 |
|---|---|---|
| 106 | `CLV_SUBTITLE_INVALID_ARGUMENT` | 出参为空、数组/计数不匹配、参数非法 |
| 107 | `CLV_SUBTITLE_INVALID_HANDLE` | 句柄为空或已关闭 |
| 108 | `CLV_SUBTITLE_INVALID_UTF8` | 包 payload 非法 UTF-8（丢整包并计数） |
| 109 | `CLV_SUBTITLE_PACKET_HEADER_BAD` | 包定界 / 下界 / 尾随 / 坏 flags / 域判不成立 |
| 110 | `CLV_SUBTITLE_PACKET_TOO_LARGE` | 头长 + 文本长超上限 |
| 111 | `CLV_SUBTITLE_OUT_OF_MEMORY` | 库内分配失败 |
| 112 | `CLV_SUBTITLE_STATE` | **本版无产出者**（未 Feed 时块计数为 0；越界取块 / 取行回 106，空句柄回 107）；号位留白不复用，`StrError` 照旧回文案 |
| 113 | `CLV_SUBTITLE_BACKEND_FAILURE` | **本版无产出者**（CAL / 平台后端失败时该块不出图，动作口仍回 `OK`）；号位留白不复用 |
| 114 | `CLV_SUBTITLE_RENDER_FAILED` | **本版无产出者**（布局 / 栅格 / 合成失败按 113 那一档不出图）；号位留白不复用 |
| 115 | `CLV_SUBTITLE_UNSUPPORTED` | 渲染器未编入、保留能力、未实现语法 |
| 116 | `CLV_SUBTITLE_OUTPUT_TOO_SMALL` | 输出缓冲 / stride 不足，或数组容量不足 |
| 117 | `CLV_SUBTITLE_STYLE_BLOCK_BAD` | 样式块的长度 / 版本 / flags / 域判不成立 |

`CLV_SubtitleStrError` 对每个值返回非空静态 ASCII；未知值返回 `"unknown"`，永不返回 NULL。

**三条约定的形状**

- `struct_size`：`0` 按本布局受理，`sizeof(本结构)` 显式声明本布局，其余显式拒（106）。
- 输出数组两趟：`capacity = 0` 只写总数；容量不足返 116 **并写实际总数**。
- 句柄单线程使用；库内 OOM 在 C 边界收敛成 111（只接 `std::bad_alloc`）。

**函数清单**

- 解析器：`CLV_SubtitleParserOpen / Feed / Reset / BlockCount / BlockAt / LineCount / LineAt / Stats / Close`
- 渲染器：`CLV_SubtitleRendererOpen / SetStyleBlock / SetFontDir / Layout / DirtyRects / Render / Report / Close`；`Layout` 收块、该块的文本行数组、可选样式覆盖与 `time_ticks`；`SetFontDir` 可多次调用，按加入顺序扫描
- 默认样式：`CLV_SubtitleParserParamsC.parser_default`（覆盖链第 3 档）与 `CLV_SubtitleRendererParamsC.renderer_default`（第 1 档）均可为 NULL；全局样式块经 `SetStyleBlock` 进第 2 档。覆盖链与 `style_source` 的逐档定义见 `docs/字幕规范.md` 的默认样式与覆盖级一节
- 缓冲所有权：两处 `SetStyleBlock`（解析器与渲染器）**都不复制载荷**——库只记指针与长度，块内字族串按 (ptr, len) 出借、要到 `Layout` 期才取。所以调用方给的 `block` 必须**存活到该句柄 `Close` 或下一次 `SetStyleBlock` 覆盖之后**；存续期内改动或释放它，之后的解析与排字会读到失效内存。这与 `CLV_OpenMemoryFile` 的「缓冲区全程归调用方」同一条纪律，也与视频帧「句柄归库、交出的是计数借用」是**两种不同的**所有权形状，别互相套

**最小示例**

```c
CLV_SubtitleError err = CLV_SUBTITLE_OK;
CLV_SubtitleParserParamsC pp = {0};          /* struct_size = 0 按本布局受理 */
CLV_SubtitleParser* p = CLV_SubtitleParserOpen(&pp, &err);
if (p == NULL) { printf("%s", CLV_SubtitleStrError(err)); return 1; }
CLV_SubtitleParserFeed(p, payload, payload_size);   /* 容器 stream_type = 2、ext_type = 4 的载荷 */
uint32_t blocks = 0;
CLV_SubtitleParserBlockCount(p, &blocks);
CLV_SubtitleParserClose(p);
```

**能力位**：bit3 = 解析/编码在（恒 1）；bit4 = 渲染器在（`CLV_BUILD_SUBTITLE_RENDERER` ON ⇒ 1 / OFF ⇒ 0，OFF 时渲染口返 `UNSUPPORTED`）。

---

## 12 占位与未落地

| 符号 | 现状 |
|---|---|
| 视频编码句柄 | 真实现：`CLV_VideoEncoderOpen` / `CLV_VideoEncoderAllocFrame` / `CLV_VideoEncoderEncode` / `CLV_VideoEncoderClose` 吃源帧吐载荷。帧间预测的参考是**编码器自己产出的重建帧（闭环）**：参考缓冲由编码器自管，调用方既不提供也无法替换，喂进来的原始帧只作残差比对的被减数。v1 无环路滤波，进参考的就是未滤波的重建；同一条件下编码器内部重建与解码器输出逐字节相同，故此闭环不引入两侧漂移 |
| 视频解码句柄 | 真实现：`CLV_VideoDecoderDecode` / `NextOutput` / `Flush` / `StreamRejected` 吃码流吐帧，自管参考缓冲与待输出队列，出帧按显示序，致命码拒解后粘滞。编码端同样真实现（产 VOP 载荷与宏块序列，做内部重建、运动估计与参考帧自管）。系数载体有两种写法：定长档与法 1 档（先写结束标志 EOB，再用位数递增的 Exp-Golomb 编码写幅值）；具体用哪一档由视频帧头第 5 到第 7 位声明，本库默认法 1。**目前只限两项**：B 帧编码侧一律 direct + `not_coded`，不带残差与块数据；10 / 12-bit 的像素电平未定义 |
| 音频句柄 | 编解码闭环已实现：`CLV_AudioDecoderOpen` / `CLV_AudioDecoderDecodeFrame` / `CLV_AudioEncoderOpen` / `CLV_AudioEncoderWrite` / `CLV_AudioEncoderNextFrame` / `CLV_AudioEncoderFlush` 等全部接线到 libFLAC 后端（帧头自核 + 流声明对账 + 一包一帧）；无状态的帧头自查 / 承载块拆分 / 掩码剥离 / 流信息解析同为实现态 |
| 字幕面 | 解析器与载荷外层 / 全局样式块编解码为真实现；渲染器（布局 / 折行避头尾 / 字体四档回退 / 滚动 / 描边 / 合成 / 脏矩形）接线到文本栈五件与系统字体解析，随 `CLV_BUILD_SUBTITLE_RENDERER` 编进 —— OFF 那档八个渲染口如实回 `CLV_SUBTITLE_UNSUPPORTED`，是构型不是缺口。本版一并写死不做的：v0.2 预留语法按未知语法忽略处理；多字幕流之间的层叠关系不定义（归宿主）；颜色管理不做（RGBA8 straight alpha + sRGB 假设，不做 HDR / 广色域 / 预乘转换）；屏幕合成与上屏不做（库只出字幕层）；像素级 golden 图件未建（布局 / 包围盒 / 覆盖率三类断言已实测，跨平台像素一致不作承诺）|
| 容器面全部符号 | 形状属对外 ABI（改动即破坏调用方）；未冻结的是 `.clv` 的字节格式与语义 |
| 单文件长度 | 结构不设上限（偏移类字段是 u64）；分段容器已落地（`flags` bit3 + `file_ext_offset` + `ext_type = 7` 段描述块 + 多段读口），段索引表（`ext_type = 8`）值已留、未实现 |
| 跨线程与跨实例并发 | 语义未定义，不由本 ABI 承诺 |
| 宿主分配器 | **判「不做注入」，只把边界写死**，五条：① `CLV_Alloc` / `CLV_Realloc` / `CLV_Free` 是给调用方的便利口，不是注入点，库里没有任何一处把它们当钩子用；② 库内分配不经这三只口 —— 实现层走标准库分配器（STL 容器与智能指针经 `std::allocator` → 全局 `operator new`），所以宿主重载全局 `operator new` 只影响库内部那一路，与 `CLV_Alloc` 拿到的内存**不承诺同源**；③ 三只口继续走 C 运行时 `malloc` / `realloc` / `free`（C++ 分配函数不带尺寸查询，`CLV_Realloc` 的语义因此做不到由 `operator new` 承担），失败行为 = 返 `NULL`、不抛、不重试、不记日志，判空由调用方做 —— 这是本库**唯一**一处「分配失败以 NULL 出现在调用方手里」，四面入口已在 `extern "C"` 边界接 `std::bad_alloc` 返各面 `*_OUT_OF_MEMORY`，两档形态刻意分开；④ 要接管库内分配 = 未提供能力，`CLV_Capabilities` 的 bit2 恒 0 且那一格永不赋予新义；⑤ 跨模块传递与释放需宿主自行保证同一 CRT 实例（`/MT*` 与 `/MD*` 不可混链），本 ABI 不作承诺也不检测 |

---

## 13 验收清单

1. 按 §2 表逐个头文件包含，确认 C 与 C++ 两种编译方式都能通过。
2. 打印 `CLV_VersionString()` 与三段数字，核对与 `include/CLV_Version.h` 的宏同源。
3. 跑 `CLV_BUILD_DEMOS=ON` 构建出的四份示例：`clv_demo_core` 与 `clv_demo_container` 退出码 0 表示自动判定项全过；`clv_demo_video` 吃一份双流 `.clv`，两轨同时出（末行 `DEMO_VIDEO_DONE` 打印且 `dropped = 0`），只出单轨不算过；`clv_demo_subtitle` 吃一份三轨 `.clv`，除视频与音频照常外还要看到字幕层（末行 `DEMO_SUBTITLE_DONE` 打印、`dropped = 0` 且 `shown` 为正）；逐项预期见 `demo/README.md`。
4. 对每个错误枚举调一次 `StrError`，确认每个值都返回非空 ASCII 串、未知值与退场留白号位返回 `"unknown"`。七个错误枚举（文件、容器、视频、音频、节拍、日志、字幕）各有一条全枚举遍历用例，容器分段的两个收束码另有一条，共八条：`ClvFile.StrErrorCoversEveryCode`、`ApiContainer.StrErrorCoversEveryCode`、`VideoHeaders.PublicSurfaceStaysInsideVideoSegment`、`AudioFraming.StrErrorNamesEveryDefinedCode`、`PaceApiStandalone.StrErrorCoversEveryCodeAndUnknown`、`ClvLogger.StrErrorCoversEveryCode`、`SubtitleApi.StrErrorCoversEveryCodeAndUnknown`、`Segments.StrErrorCoversBothSegmentCodesWithText`。
5. 逐函数验 `NULL` 入参：句柄为 `NULL` 回 `INVALID_HANDLE`，出参为 `NULL` 回 `INVALID_ARGUMENT`，都不许崩。
6. 用 `CLV_FILE_INVALID_PATH` 与容器口的 `CLV_CONTAINER_IO_FAILED` 核对 §1.1 的路径契约与码值收敛。
7. 用 `assets/fixtures/` 三份素材核 §7.5 的统计口径：正常读回时全部计数为 0。
8. 跑 `ctest`，确认 **431 例**全过；其中只吃公开 C ABI 的 `api` 面 110 例是本文的机器可读对账。`core` 域直测不出 ABI 的内部工具（CRC-32/MPEG-2、LEB128 与 zigzag），`container` / `audio` / `video` / `subtitle` 四个域两面混吃：句柄端到端走公开面，单元级用例直接 include `src/` 下的实现层头，用来钉变换 / 量化 / 块载体 / 运动补偿 / 参考帧管理 / 载荷外层 / 覆盖链这些不进 ABI 的行为；`clv_tests` 因此恒链静态件 `clv_static`，实现层符号不带导出宏、不进动态件的导出表。`ctest -R Independent` 单跑独立验证层那 21 例，它们与调用方契约无关，是仓内的边界与分辨力检查。
