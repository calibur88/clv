# CLV

> 本文是 CLV 仓库的入口说明。当前文档版本：0.8.1 · 抬头日期 2026-10-02，内容更新至 2026-10-21（容器格式版本升 2、分段容器落地与多段读口、`ext_type = 3` 对视频流转必选、入库素材第二次重锚）。

CLV = **C++ Lightweight Video**：自研轻量视频容器 + 自研视频编解码 + 自研字幕格式。
公开面是 **C ABI**：不透明句柄 + 自由函数 + 错误码枚举。
已落地能力：文件读写、日志、内存分配、版本查询，**容器读写面**（函数名 / 结构体 / 出参形态属对外 ABI，改动即破坏调用方；未冻结的是 `.clv` 的字节格式与语义；**分段容器已落地** —— 一个逻辑视频切成多个 `.clv`，段身份落 `ext_type = 7` 段描述块、入口在文件头 `file_ext_offset`，一个读方句柄按段清单连续出帧）、**视频编解码**（含帧间运动补偿与半像素插值）与**音频编解码闭环**（libFLAC）。字幕格式尚未开工。

## 安装

第三方源码在 `third-party/`（spdlog、googletest），先装到 `build/_install`：

```bash
python scripts/install_third_party.py          # 已装则跳过，--force 重装
```

协作方（人和 AI）在本仓干活要用的项目级 Skill 放 `skills/`，随仓库提交；各家 AI 平台自己的扫描目录只放逐件链接、不入库，
**克隆后跑一次** `python scripts/setup_skills.py` 现场重建（链接在 Windows 上记的是本机路径，入库必失效）。
哪一家、哪个路径由该平台自己的约定决定，细节见 `skills/project-skill-generator/SKILL.md`。

构建走 **Ninja + x64**，配置与编译必须在 `vcvars64.bat` 起来的同一个进程里跑：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

集成方式二选一：`add_subdirectory()` 引入源码，或直接链 `build/clv.lib` 并把 `include/` 加进头文件搜索路径。公开头共九份，全在 `include/`（核心与容器六份 + 媒体两份 + 节拍一份）。

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

逐个函数的签名、入参出参语义、错误码表与最小示例见 `API.md`；`.clv` 的字节格式规则见 `docs/容器规范.md`。

| 头 | 函数 | 一句话 |
|---|---|---|
| `CLV_Version.h` | `CLV_VersionString` / `CLV_VersionMajor` / `CLV_VersionMinor` / `CLV_VersionPatch` | 版本查询，字符串带预发布后缀 |
| `CLV_File.h` | `CLV_OpenFile` / `CLV_OpenFileEx` / `CLV_ReadFile` / `CLV_WriteFile` / `CLV_SeekFile` / `CLV_FileSize` / `CLV_FlushFile` / `CLV_CloseFile` / `CLV_OpenMemoryFile` / `CLV_FileStrError` | 文件读写：三模式打开、顺序与随机读、取长度、刷盘、错误码文案 |
| `CLV_Logger.h` | `CLV_Log` / `CLV_LogSetLevel` / `CLV_LogSetStorage` | 日志：UTF-8 字节串直投 + 等级过滤 |
| `CLV_Memory.h` | `CLV_Alloc` / `CLV_Realloc` / `CLV_Free` | 分配口，直通 C 运行时，失败返回 NULL |
| `CLV_Export.h` | `CLV_EXPORT`（宏） | 静态件下为空；构建动态件时 `CLV_BUILD_DLL` 切 dllexport、消费动态件时 `CLV_USE_DLL` 切 dllimport，两个位都由所链目标自带 |
| `CLV_Container.h` | 写方 7 个（`…WriterOpenFile` / `AddStream` / `AddExtBlock` / `WriteFrame` / `WriteFrameChunks` / `Finish` / `Close`）+ 读方 17 个（`…ReaderOpenFile` / `NextFrame` / `StreamCount` / `GetStream` / `Stats` / `HasIndex` / `IndexCount` / `GetHead` / `GetIndexEntry` / `ExtBlockCount` / `GetExtBlock` / `Close`，另有多段读五口 `…ReaderOpenSegments` / `SegmentIndex` / `SegmentTotal` / `SegStats` / `SegExtCheck`）+ `CLV_ContainerStrError` = **25** | 容器读写：**形状属对外 ABI**（改动即破坏调用方），未冻结的是 `.clv` 的字节格式与语义。不透明句柄 + 整帧或调用方给定的分片进出；分段容器已落地（`ext_type = 7` 段描述块 + 段清单连续出帧 + 段级观测口），头注释即契约正文 |
| `CLV_Video.h` | 无状态 2（`CLV_VideoCodedSize` / `ParseEncMeta`）+ 解码 7（`DecoderOpen` / `Decode` / `NextOutput` / `Flush` / `Reset` / `StreamRejected` / `Close`）+ 编码 4（`EncoderOpen` / `AllocFrame` / `Encode` / `Close`）+ 码流 4（`BitstreamGetBytes` / `BitstreamSliceCount` / `BitstreamGetSlice` / `BitstreamRelease`）+ 帧 5（`FrameAddRef` / `FrameRelease` / `FrameFree` / `FrameGetDesc` / `FrameToRGB24`）+ `StrError` = **23** | 视频编解码面：**编解两侧的有损重建闭环已接**，**无损档已接**（`mode = LOSSLESS` + 码流里 `vop_quant = 0`，系数直载、重建与源逐字节全等）（编码端变换量化 → 系数载体 → 内部重建并自存参考帧；解码端句柄七入口全真）；系数载体有两种写法：定长档与法 1 档（先写结束标志 EOB，再用位数递增的 Exp-Golomb 编码写幅值）；具体用哪一档由视频帧头第 5 到第 7 位声明，本库默认法 1。**运动补偿已接**（帧间按运动矢量取样，半像素双线性平均 + 参考缓冲边缘复制带；B 帧编码侧目前只产 direct + `not_coded`）。码流缓冲归调用方、用完自己 `Release` |
| `CLV_Audio.h` | 无状态 5（`AudioInspectFrame` / `ParseStreamInfo` / `SplitMetaBlock` / `ClassifyMetaBlock` / `StripChannelMask`）+ 解码 6（`DecoderOpen` / `DecodeFrame` / `AddMetaBlock` / `Reset` / `Stats` / `Close`）+ 编码 6（`EncoderOpen` / `Write` / `NextFrame` / `Flush` / `TakeStreamInfo` / `Close`）+ 缓冲 3（`BufferAddRef` / `BufferRelease` / `BufferGetDesc`）+ 元数据块 3（`BlobAddRef` / `BlobRelease` / `BlobGetBytes`）+ 承载块解读 4（`ParseVorbisComment` / `VorbisCommentEntryAt` / `CLV_AudioVorbisCommentFindKey` / `ParsePicture`）+ `StrError` = **28** | 音频面：编解码闭环已实现（libFLAC 后端接线到帧级：帧头自核 + 流声明对账 + 一包一帧）；结构层（帧头 / 承载块 / 注释块）与句柄面均为实现态 |
| `CLV_Pace.h` | 建仓与逐帧判定 8（`CLV_PaceOpen` / `Close` / `BeginPass` / `SetDeviceRunning` / `NoteAudioFed` / `ClockSeconds` / `Decide` / `FeedAllowed`）+ `StrError` = **9** | 播放节奏：以音频设备侧的已消费量为主时钟，按这一帧的 pts 判它该等、该出还是该丢（`Decide` 交回等待 / 出画 / 丢帧三档），并给出还能不能继续喂音频；设备侧只经两个回调交进来，本头不依赖任何播放器库，失败码从 96 起独立成段 |

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
| 错误码编号 | 五面的错误码各自独立枚举：文件与容器从 1 顺排、音频 32 到 63、视频 64 到 95、节拍 96 起，不得跨头比对数值——各头的 1 语义无关；文件面失败在容器口统一收敛为 `CLV_CONTAINER_IO_FAILED`，不回传文件面码值 |
| 容器面时间轴 | `WriteFrame` 收该流原始时间轴上的绝对 dts / pts（可为负），写方按首帧平移到 0 落盘、读方不承担平移；`pts_delta` 是「本帧 PTS − 本帧 DTS」，用 zigzag 有符号 varint 承载，允许为负 |
| 容器面线程 | 句柄不跨线程使用，跨实例并发语义未定义 |

## 构建与验收

| 开关 | 默认 | 作用 |
|---|---|---|
| `CLV_BUILD_SHARED` | OFF | 另建动态件 `clv`（静态件 `clv_static` 恒在）；ON 时三份示例链动态件，跑通即动态路实测 |
| `CLV_LOGGER_USE_SPDLOG` | OFF | 日志后端选 spdlog；OFF 时用空后端，所有日志被丢弃 |
| `CLV_BUILD_DEMOS` | OFF | 建 `demo/` 的三个示例，可执行落 `demo_local/bin/`（每次构建先清空再投递），并把示例源码同步一份到 `demo_local/src/`；播放器示例另需 SDL3，找不到时配置期直接失败 |
| `CLV_WARNINGS_AS_ERRORS` | OFF | 自有 target 警告升级为错误 |

源文件编码由构建统一指定（MSVC `/utf-8`，其它 `-finput-charset=UTF-8`）。自有 target 的警告级别是 MSVC `/W4 /permissive-`，第三方不参与。

验收基线（Windows + MSVC + Ninja x64）：

| 项 | 结果 |
|---|---|
| clean 重建 | 0 warning |
| `ctest` | 注册 **375** 例、0 失败；其中 1 例（`VideoQualityRealMedia.…`）在没有真素材环境变量时按设计 `GTEST_SKIP`，不判红。`CLV_LOGGER_USE_SPDLOG` 的 ON 与 OFF 两种配置都跑 |
| 第三方接入 | `find_package`（用 `build/_install`）与源码 `add_subdirectory` 回退两条路都可用；**libFLAC 是例外**，走自建导入目标，见下一节 |
| 格式自查 | `python scripts/format_all.py`（C/C++ 走仓库根 `.clang-format`，Python 走 black） |
| 示例 | `CLV_BUILD_DEMOS=ON` 构建后，在 `demo_local/` 下跑 `./bin/clv_demo_core.exe`、`./bin/clv_demo_container.exe` 与 `./bin/clv_demo_video.exe <一份双流 .clv>`，前两个退出码 0 表示自动判定项全过；第三份是双轨播放器（另需外部 SDL3），判据是退出码 0 且末行 `DEMO_VIDEO_DONE shown=… dropped=… repeated=…` 打出来——该行只在两轨都跑完时打印，`shown` 等于流里的帧数、`dropped` 为 0；人工项见 `demo/README.md` |

### libFLAC 怎么找（唯一路，锁定）

`find_package` 优先、源码 `add_subdirectory` 回退之外，**libFLAC 走第三条路，并且它是唯一路**：
vendored 安装的 FLAC config 包（`build/_install/lib/cmake/FLAC/flac-config.cmake`）无条件 `find_dependency(Ogg)`，
而本仓库从不出 ogg（`scripts/install_third_party.py` 只出 libFLAC），CONFIG 模式必失败；故根 `CMakeLists.txt` 里那段
`find_package(FLAC QUIET)` 之后的退回路改用 `find_path` + `find_library` 自建 `FLAC::FLAC` 导入目标——**引用本仓库的行号一律不写**，
那段代码随改动整批漂移，点名符号比点名行号靠得住。

```cmake
# 候选前缀按 foreach 过两遍：${CMAKE_BINARY_DIR}/_install 与 ${CMAKE_SOURCE_DIR}/build/_install，
# 每个前缀进来前先 unset 两个缓存项——不 unset 会把两处的结果混成一对。
find_path(CLV_FLAC_INCLUDE_DIR NAMES FLAC/stream_encoder.h PATHS ${CLV_FLAC_PREFIX}/include)
find_library(CLV_FLAC_LIBRARY NAMES FLAC libFLAC PATHS ${CMAKE_SOURCE_DIR}/build/_install/lib)
add_library(FLAC::FLAC UNKNOWN IMPORTED)
set_target_properties(FLAC::FLAC PROPERTIES
    IMPORTED_LOCATION             "${CLV_FLAC_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CLV_FLAC_INCLUDE_DIR}"
    INTERFACE_COMPILE_DEFINITIONS "FLAC__NO_DLL")   # 静态件；缺它则 __imp_FLAC__* 未解析
```

- **定位逻辑**：`NAMES` 里的 `FLAC` / `libFLAC` 分别是 MSVC 与类 Unix 的库文件名；`PATHS` 只指 `build/_install`
  （`scripts/install_third_party.py` 的安装前缀），**不搜系统目录**——免得在别的机器上误捡到系统里另一份版本不受控的 FLAC。
- **`FLAC__NO_DLL` 是硬要求，不是建议**：`build/_install/lib/FLAC.lib` 是**静态件**（实测 2 338 352 B、无 `__imp_FLAC__` 导入桩），
  `FLAC/export.h` 只在定义了该宏时才把 `FLAC_API` 从 `dllimport` 退成空修饰；不给这个宏，链接期就是一批 `__imp_FLAC__*` 未解析。
- **换机复现**：`python scripts/install_third_party.py` → `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug`
  （须在 `vcvars64.bat` 起来的同一进程里）。配置期会打一行
  `CLV: FLAC 走直接定位（config 包要求 Ogg，本仓库不建 ogg）: <FLAC.lib 路径>`，见到它即自建导入目标生效；
  若报 `CLV: 找不到 libFLAC`，是 `build/_install` 里没装 FLAC，补跑装好即可，别去改判定逻辑。
- **路线锁定**：自建导入目标是 libFLAC 定位的**唯一路**，另两条已排除——**不补建 ogg**（只为让上游 config 包能用，
  会引入本仓库根本不需要的依赖）、**不把 vendored 源码放回 `third-party/flac`**。后续不得擅自换路；确要换，先改本节并在 `CHANGE.log` 记条目。

## 版本口径

| 轨道 | 值 | 出现位置 |
|---|---|---|
| 工程版本 | `0.8.1`，字符串 `0.8.1-dev` | `include/CLV_Version.h` 的四个宏（单一来源）、CMake `project(clv VERSION 0.8.1)`、`CLV_VersionString()` 的返回值 |
| 文档版本 | `0.8.1` | 设计文档与本 README 的抬头声明，不进可执行代码；与工程版本可以同值，但两条轨道的来源与变更时机各自独立 |
| 容器格式版本 | `2` / `0`（`version_major` / `version_minor`） | `.clv` 文件头偏移 8 / 10，读方据此判能否解析 |

**格式变更须伴随版本升号**：改动落到线上字节（布局、字段宽度、偏移口径、取值域）时，**必须同轮**抬 `version_major`（只澄清语义、补齐定义而不动布局才抬 `version_minor`），并随之重锚入库素材与规范正文。反面就是 2026-10-11 到 2026-10-12 之间那段：布局已变而 `version_major` 仍写 1，两份文件字节上看不出归属。
三条轨道之间**没有换算关系**：抬工程版本既不触发容器格式版本、也不触发入库素材重跑（重跑判据只看容器格式版本与结构口径）。

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
| `API.md` | 公开 C ABI 逐函数说明：句柄与错误码约定、入参出参语义、最小示例、验收清单 |
| `docs/容器规范.md` | `.clv` 字节格式规范：布局、字段值域、时间模型、分片规则与容错口径 |
| `ARCHITECTURE.md` | 工程结构、分层与数据流、构建系统组织方式、当前未落地项与已落地范围 |
| `CHANGE.log` | 版本演进记录与版本兼容声明 |
| `include/CLV_*.h` | 九个公开头的注释即接口契约正文：函数名 / 参数结构体 / 出参形态属对外 ABI，改动即破坏调用方；未冻结的是各面的字节格式与码流语义 |
| `assets/README.md` | 入库测试素材的内容表、确定性要求与冻结值 |
| `skills/` | 项目级 Skill（与平台无关的唯一事实来源）：收口复扫 / 门禁跑批 / 交付记录 / Skill 生成器；各平台扫描目录里只放逐件链接，克隆后由 `scripts/setup_skills.py` 重建 |
| `demo/README.md` | 示例库：预置内容、可复制示例、运行与验收步骤 |
| `人机协同工作流程.md`、`AI项目文档通用生成模板.md`、`AI分层架构通用规范.md`、`AI技术债审查规范.md` | **四份项目外自由文档**：随仓库一起入库，但不属本项目，也不受本项目的注释纪律与版本口径管辖；内容是各自通用的协作流程、文档模板与审查规范，版本演进由该件自己头部声明 |


