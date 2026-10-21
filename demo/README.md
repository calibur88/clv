# CLV 功能展示（demo）

> 本目录放示例源码与本说明，**不放构建产物**（产物落 `build/` 与构建时创建的 `demo_local/`）。
> 当前文档版本：0.8.2；示例打印的工程版本是 `0.8.2-dev`。容器面 `CLV_Container.h` 的函数名 / 参数结构体 / 出参形态属对外 ABI，改动即破坏调用方；错误码只在枚举尾部追加、既有码值与语义不动；未冻结的是 `.clv` 的字节格式与语义。

**项目速记**：

- `CLV_Version`：版本查询，字符串带预发布后缀；
- `CLV_File`：文件读写，不透明句柄 + 三种打开模式 + 错误码出参；
- `CLV_Logger`：日志，UTF-8 字节串直投，带等级过滤；
- `CLV_Memory`：分配口，直通 C 运行时；
- `CLV_Container`：容器读写面（形状属对外 ABI，未冻结的是 `.clv` 的字节格式与语义）——写方 `OpenFile → AddStream → AddExtBlock → WriteFrame → Finish`，读方 `OpenFile → NextFrame` + 结构出参；
- 完整口径见仓库根 `README.md`；公开头共九份（`CLV_Audio.h`、`CLV_Container.h`、`CLV_Export.h`、`CLV_File.h`、`CLV_Logger.h`、`CLV_Memory.h`、`CLV_Pace.h`、`CLV_Version.h`、`CLV_Video.h`）；三份示例直接 include 用到其中八份，唯一没直接用到的是导出宏头 `CLV_Export.h`，它由各接口头自己带进来。`container.c` 的 `info` 子命令另用 `CLV_File.h`，`video.c` 还要链一份外部播放器库 SDL3。

## 示例数据（目录名）

| 文件夹 / 文件 | 内容 |
|---|---|
| `core.c` | C 程序，只用核心面的四个头（`CLV_File` / `CLV_Logger` / `CLV_Memory` / `CLV_Version`），按 `#01`~`#10` 编号逐条打印「预期 / 实测 / 判定」；可执行名 `clv_demo_core` |
| `container.c` | C 程序，容器面只用 `CLV_Container.h`（`info` 子命令读裸文件时另用了 `CLV_File.h` 的四个函数），现场写一个真 `.clv` 再读回逐条判定（含 `#04` 写方平移义务的可视行）；可执行名 `clv_demo_container` |
| `video.c` | C 程序，双轨播放器：`CLV_Container.h` + `CLV_Video.h` + `CLV_Audio.h` + `CLV_Pace.h` 四口同开，视频轨解到平面后经 SDL3 的 IYUV 纹理直上屏幕，音频轨灌 SDL3 音频流，两边按 pts 对齐；可执行名 `clv_demo_video`，需要外部依赖 SDL3 |
| `CMakeLists.txt` | demo 的构建入口，默认不建（`CLV_BUILD_DEMOS=OFF`）。产物落 `demo_local/bin/`（**每次构建前先按清单清空该目录**），并把 `core.c` / `container.c` / `video.c` / 本手册同步一份副本到 `demo_local/src/`；示例跑出来的文件落在 `demo_local/` 根上，不跟着 `bin/` 一起被清 |

三个可执行名与源文件名不同步（`clv_demo_core ← core.c`、`clv_demo_container ← container.c`、`clv_demo_video ← video.c`），配对写在 `CMakeLists.txt` 的 `add_executable` 里。

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
| 写后刷盘 | `WRITE` | 写 → `CLV_FlushFile` → 再以 `CLV_FileSize` 读回一致；只读句柄的 `Flush` 是 no-op 且返回 `CLV_FILE_OK` |
| Write 模式清空 | `WRITE` | 对齐 POSIX `"wb"`：已有内容不残留 |
| Append 模式追加 | `APPEND` | 对齐 POSIX `"ab"`：写入恒定落末尾，游标也在末尾 |
| Seek 随机读 | `READ` | 绝对偏移；Seek 到文件尾之后合法，其后 Read 返回 0 字节且不算失败 |

**ABI 覆盖（核心面）**：`include/CLV_File.h` 现有 **10 个** `CLV_EXPORT` 函数，`core.c` 真实调用其中 **9 个**
（`OpenFile` / `OpenFileEx` / `ReadFile` / `WriteFile` / `SeekFile` / `FileSize` / `FlushFile` / `CloseFile` / `FileStrError`）；
**未覆盖面 = `CLV_OpenMemoryFile`**。「写后刷盘」一项走两次 `CLV_FlushFile`：写完 `Close` 之前刷一次，再在只读句柄上刷一次、断言返回 `CLV_FILE_OK`（只读句柄的 `Flush` 是 no-op）；
落盘内容仍由 `CLV_FileSize` 读回比对。
`CLV_OpenMemoryFile` 已是实现态，但示例只演示文件句柄这一路：内存缓冲句柄的所有权与固定容量语义由测试侧覆盖，示例不重复一遍。
其余三个头：`CLV_Version.h` 四个函数全调，`CLV_Memory.h` 三个函数全调，`CLV_Logger.h` 现有 **5 个**函数，
`core.c` 调 `CLV_Log` / `CLV_LogSetLevel` 两只；**未覆盖面 = 落盘那三只**（`CLV_LogSetStorage` / `CLV_LogSetStorageEx` / `CLV_LogStrError`）——
示例把日志投递给控制台这一路演示到底，不去动落点：落盘会往盘上写件，示例的判据形态（重跑逐字节可比）不适合带一份变的文件。
清单写死在此，改函数时必须同步这一段。

## 可复制示例（按主题分节）

**版本查询**

```c
#include "CLV_Version.h"

printf("%s\n", CLV_VersionString());   /* 0.8.2-dev */
printf("%d.%d.%d\n", CLV_VersionMajor(), CLV_VersionMinor(), CLV_VersionPatch());   /* 0.8.2 */
```

命中：`0.8.2-dev` 与 `0.8.2` 各一行。

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

**用途**：容器层的人工验证示例——产出一个可打开、可 dump、可肉眼核对的真 `.clv`；自动化断言在 `tests/`。
文件写在当前工作目录（默认 `./clv_demo_container.clv`，可用 `argv[1]` 覆盖），写完留在原地不删。

写入的内容：视频流 `id0`（timebase 1/1081080000、`max_packet_size` 4096，挂 `ext_type = 3` 视频参数块：
8 字节载荷、显示尺寸 352×288、无 B 帧、`quant_precision = 7`）+ 字幕流 `id1`（`max_packet_size` 1024，挂 `ext_type = 4` 样式块）；
扩展块共 **2 条**（`ext_type = 3` 对视频流是 v2 起的必选项，查无该块的视频流进不了流表：只列出可用的流；描述符校验不通过的流在打开时被丢弃，不回报数量，也不占用编号）；
四帧，其中视频首帧**源 dts = −40**（B 帧前瞻）、载荷 1000 B 按 `fragment_chunk_size = 300` 切成 4 片。

| 判定项 | 考察点 |
|---|---|
| `#01` 文件头 magic / 版本 | 64 B 头写回后字段齐全 |
| `#02` 流描述符读回 | 双流、timebase、`stream_type` 逐条对得上 |
| `#03` 视频 `ext_type = 3` 参数块 + 字幕 `ext_type = 4` 样式块 | 扩展块**两条**：条数、类型、挂载流号与载荷逐字节等（参数块的 8 字节字段逐个对平）|
| `#04` 首包 dts 归零 | **写方平移义务**：源 dts −40 → 落盘 dts_delta 0，`pts_delta = 80`。附一行可视输出说明换算 |
| `#05` 分片重组完整帧 | 每片自带完整外层头 + 片序连续性，1000 B 四片重组 |
| `#06` 索引条目数与帧数 | 每帧一条索引 |
| `#07` 索引跨流统一时间轴 | 按统一时间轴升序，相等时按 `stream_id` tie-break |
| `#08` 包数一致 | 文件头 `total_packets` 与读方成功计数 |
| `#09` 容错计数干净 | CRC / 字段 / 超限 / 无名流 / 重同步 / 残帧 全 0 |
| `#10` 文件大小合理 | 大于 1024 B |

`#10` 之外另有一项**人工判定**：直接拿十六进制编辑器打开产出的 `.clv`，或跑 `info` 子命令。

**子命令 `info <path>`**：只读并 dump 结构（头 / 描述符 / 扩展块 / 索引 / 读方统计 / 帧列表），不写文件。
它同时是 `assets/fixtures/` 三份素材的交叉验证入口——那三份由 `scripts/gen_fixtures.py`（独立 Python 实现）写出，
能被本库的读方正确解析，说明两侧都没偏离规范：

```bash
# info 是只读子命令，不写文件，所以从仓库根跑没问题
<构建产物目录>/clv_demo_container.exe info assets/fixtures/neg_dts.clv
```

**路径口径**：`CLV_ContainerWriterOpenFile` / `CLV_ContainerReaderOpenFile` 收的是项目路径契约——UTF-8、**只用正斜杠 `/`**，
出现反斜杠即拒绝。容器面把文件层失败统一收敛为 `CLV_CONTAINER_IO_FAILED`，不回传文件面码值，
所以写错分隔符时只会看到 `io failed`，**不会有一个专门表示路径非法的错误码**。路径一律写成 `assets/fixtures/x.clv` 这种正斜杠形式。

**ABI 覆盖**：`include/CLV_Container.h` 现有 **25 个** `CLV_EXPORT` 函数（写方 7 / 读方 17 / `StrError` 1），本示例真实调用其中 **19 个**；
**未覆盖面 = 6 个口**：`CLV_ContainerWriterWriteFrameChunks`（分片写入入口，由 `tests/win32/container/test_write_chunks.cpp` 按五条正路 + 六条拒绝路覆盖）与多段读五口 `CLV_ContainerReaderOpenSegments` / `SegmentIndex` / `SegmentTotal` / `SegStats` / `SegExtCheck`（由 `tests/win32/container/test_segments.cpp` 覆盖）；示例未走这六个口（25 = 19 + 6）。
清单写死在此，改函数时必须同步这一段：只核对总数不够，同一个数字可以对应不同的函数集。

## 双轨播放器示例（video.c，1 个程序 / 一条同步判据）

放一个**同时含视频轨与音频轨**的 `.clv` 进去，两条轨一起出：画面直上窗口、声音直出设备，两边按 pts 对齐。
它演示的是读写面之外的第三件事——**公开头给的出参够不够一个真实播放器用**：视频解到 `CLV_VideoFrameDescC` 的三个平面、
音频解到 `CLV_AudioBufferGetDesc` 的 PCM，都不经本库转成 RGB。

- **依赖**：SDL3（`python scripts/install_third_party.py` 装进 `build/_install`）。配置期开了 `CLV_BUILD_DEMOS=ON` 而找不到 SDL3 会直接失败，不再静默跳过；`SDL3.dll` 随构建投进 `demo_local/bin/`，与 exe 同目录。
- **画面路**：纹理格式 `SDL_PIXELFORMAT_IYUV`，`SDL_UpdateYUVTexture` 把解码器给的三平面与三个行距直投渲染器——库里已有 `CLV_VideoFrameToRGB24`，本示例**故意不用它**，用它就是另一条路。
- **时钟口径**：不在本示例里写，全在库侧节器件（`include/CLV_Pace.h` / `src/pace/`）——主时钟取音频设备侧的已消费量（`SDL_GetAudioStreamQueued` 反推），锚点之外按单调墙钟线性外推；四个阈值都是 `1/fps` 的倍数：对齐带 ±半周期、等待阈半周期、丢帧阈一整个周期，另两道容量缺省 = 背压 1000 ms、预灌 200 ms 当量字节。示例只把设备的排队量与放开/按住两件事告诉库，然后执行它交回的「等 / 出画 / 丢」。
  背压缺省为什么是 1000 ms 而不是更小的数：喂闸一等等住的是整条读包腿，实测一份素材的音频排队自然峰值 598 ms，500 ms 的阈值正卡在峰值上，一堵 90~114 ms 醒来连丢三帧 —— 阈值低于自然峰值时丢帧是闸造成的，跟解码成本无关。
- **单轨不算过**：只出画面或只出声音都判负，末行 `DEMO_VIDEO_DONE shown=… dropped=… repeated=…` 只在两轨都跑完时打印，`shown` 应等于流里的帧数、`dropped` 为 0。
- **每帧一行读数**：`f`（帧号）/ `vpts`（视频 pts）/ `aclock`（音频钟）/ `dev`（两者之差，正 = 视频早、负 = 视频迟）/ `asample`（音频样本位置）/ `dec` `pres`（解码与呈现两腿的累计均值）/ `drop` `repeat`（丢帧与等待期重复计数）。收口行另给对齐带内 / 带外 / 丢帧的三档分布与末帧双钟读数。
- **交互**：`ESC` 或 `q` 退出，`SPACE` 暂停续播；`--loop` 循环播放（每趟重开读方、复位两条解码器、清空并暂停音频流，预灌完再放开设备），`--passes N` 跑满 N 趟自己收（循环满 N 趟的验收形要能不碰窗口就停下），`--topmost` 把窗口置前（截图取证用）。
- **循环验收形**：**构型钉 Release**（`-DCMAKE_BUILD_TYPE=Release`，其余同门禁 A 构型）、不绑核、`--passes 2`，逐趟核对三件事——`呈现 N 帧` 的 N 等于流内帧数、`丢 0`、时间去向行的 `背压 0.00 … 背压共堵 0 次`（一堵就说明阈值卡在素材的自然峰值之下，见上一条）。第 2 趟的读数要与第 1 趟同形（每趟各自复位计数器，跨趟混账在这行会读成假数）。
  **同步判据分主辅两条**：主判据 `dropped = 0`；辅判据 **`|dev| ≤ 一帧周期`**（30 fps ⇒ 33.3 ms），`|dev|` 超出一整个周期即 FAIL。
  辅判据为什么定在周期而不是更紧：`dev` 是「视频 pts − 音频钟」，单线程播放器出画点的粒度 = 睡眠量子（本机 1~5 ms），
  再叠上设备取数一跳约 100 ms 的网格；实测两趟 `dev` 落在 −10.0 ~ −0.4 ms、中位 3.30 ms，全部在周期以内。
  把辅判据钉到 ±1 ms 会把它变成睡眠粒度的读数器，量的不再是同步好坏。
  本机实测（Release，`--passes 2`）：第 1 趟 解码 10.79 / 呈现 1.25 / 睡 17.53 / 读包 1.36 / 喂音频 0.06 / 背压 0.00 / 其余 2.04 ms/帧，第 2 趟 10.93 / 1.34 / 17.20 / 1.42 / 0.06 / 0.00 / 2.09，两趟各 105 帧、丢 0、背压堵 0 次。
  **同一份素材在 Debug 构型下这条判据不成立**：实测解码腿 86.84 / 89.44 ms/帧（帧预算 33.33 ms），105 帧里丢 101 / 102，退出码仍 0、音轨照常播 —— 这是**吞吐不达**而不是同步口径错，节器件在追不上时按丢帧降级正是它的职责。
  所以这一条判据离开构型就没有意义，读它之前先读构建行。
- **`--probe` 打点**：同步偏差要归因时用这条，别靠猜。它另打两类读数——① 趟首五段（进趟 / 重开读方 / 复位+清流+暂停 / 预灌完 / 空跑呈现 / 放开设备）各自的累计与本段耗时，加那一刻音频流的排队、已消费与外推锚龄；② 每帧同样的四个量并加趟龄。
  「钟位凭空超前」由此分成两种，一眼分开：**已消费本身就大** = 设备真播掉了这么多（起钟太晚）；**已消费小而锚龄大** = 线性外推把它算多了（锚点没跟上一趟的起点）。这两列要成对打才有用：钟位是「已消费量」与「锚点之后的线性外推量」两项之和，单给一项就分不开这两种成因。节器件的口径写在 `include/CLV_Pace.h` 的头注里——预灌期与暂停期不外推、那一段也不计入已消费，成对读数正是把这两条约束各自核一遍。
- **它不写文件**：跑完不在 `demo_local/` 根留任何东西，清账口径只涉及前两个示例。

## 操作

1. **适用范围**：三个示例都只用 `include/` 下的公开头——公开头共九份（`CLV_Audio.h`、`CLV_Container.h`、`CLV_Export.h`、`CLV_File.h`、`CLV_Logger.h`、`CLV_Memory.h`、`CLV_Pace.h`、`CLV_Version.h`、`CLV_Video.h`）；三份示例直接 include 用到其中八份，唯一没直接用到的是导出宏头 `CLV_Export.h`，它由各接口头自己带进来。程序按 C 编译；播放器额外链一份外部 SDL3。
2. **工程设置**：需要 CMake ≥ 3.16、Ninja、MSVC x64 环境；两个开关——`CLV_BUILD_DEMOS=ON` 建示例，`CLV_LOGGER_USE_SPDLOG=ON` 才有日志输出（`clv_demo_core` 的 `#02` 日志过滤项要用它）。开了 `CLV_BUILD_DEMOS=ON` 就必须能找到 SDL3，找不到是配置期硬失败。
3. **安装步骤**：先 `python scripts/install_third_party.py` 把第三方装到 `build/_install`，再配置与构建（命令见下面「开发」），产物落在 `demo_local/bin/` 下的 `clv_demo_core.exe`、`clv_demo_container.exe` 与 `clv_demo_video.exe`（播放器旁边还有同目录的 `SDL3.dll`，Windows 按 exe 同目录找 DLL）。
4. **验收**：在 `demo_local/` 目录下依次执行 `./bin/clv_demo_core.exe` 与 `./bin/clv_demo_container.exe`，它们生成的文件都建在当前工作目录（也就是 `demo_local/` 根）；播放器要吃一份**双流** `.clv`，直接跑 `./bin/clv_demo_video.exe <文件.clv>`，它自己不生成文件。
5. **确认**：逐行对照前两个程序各自 `#01`~`#10` 的 `expect` 与 `actual`，末行都应是「自动判定失败 0 项」，退出码 0。
   核心面的 `#02`（有没有 DEBUG 行）与 `#10`（中文名文件字形）含人工判定；容器面的 `#04` 会额外打印一行平移算式（源 dts −40 → 落盘 0，`pts_delta = 80`），`#10` 之外的「结构是否顺眼」为人工判定。
   播放器的人工判定是「画面在动、声音在响、口型与画面对得上」，自动判据是末行 `DEMO_VIDEO_DONE` 的 `shown` 等于流内帧数且 `dropped = 0`，退出码 0
   —— 这条判据**钉在 Release 构型**（Debug 下解码腿 86.8~89.4 ms/帧，丢帧属吞吐不达，见「循环验收形」）。
6. **清理**：`clv_demo_core` 自动删除自己建的 `./clv_demo_*.bin`，但 `./clv_demo_中文路径.txt` 留着——中文文件名本身要对着目录核对字形，确认后手动删；
   `clv_demo_container` 写完的 `./clv_demo_container.clv` 也留着，不自动删。`demo_local/bin/` 由构建每次清空，`src/` 副本与这两个示例产出文件都不受清空影响。`demo_local/` 全部由构建与示例运行产出，删掉可再生。

## 开发

```bash
# 在 vcvars64 起来的 x64 开发环境里执行（cl 与 INCLUDE 必须同进程）
python scripts/install_third_party.py                 # 第三方源码 -> build/_install，已装则跳过
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCLV_BUILD_DEMOS=ON -DCLV_LOGGER_USE_SPDLOG=ON # 配置：开示例与真实日志后端
cmake --build build                                  # 先清空 demo_local/bin 再生成三个 exe，并同步 src/ 副本
cd demo_local
./bin/clv_demo_core.exe; echo exit=$?                # 退出码 0 即自动项全过
./bin/clv_demo_container.exe; echo exit=$?           # 同上，跑出来的 .clv 落在 demo_local/ 根
./bin/clv_demo_video.exe <一份双流.clv>; echo exit=$? # 窗口出画面 + 设备出声，末行 DEMO_VIDEO_DONE 才算跑完
```

播放器的**同步判据要在 Release 构型下读**（Debug 的解码腿追不上 30 fps，见上一节「循环验收形」）：

```bash
cmake -S . -B build/rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCLV_BUILD_DEMOS=ON -DCLV_LOGGER_USE_SPDLOG=ON -DCLV_WARNINGS_AS_ERRORS=ON
cmake --build build/rel
cd demo_local && ./bin/clv_demo_video.exe ../assets/fixtures/demo_av_1080p_q32.clv --passes 2 --probe
```

三个 demo 目标都在仓库统一的告警档（`/W4 /permissive-`，且 `CLV_WARNINGS_AS_ERRORS=ON` 时 `/WX`）下编译，
与库和测试同一标准，不允许「示例豁免告警」。
