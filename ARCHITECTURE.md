# CLV 整体架构

> 本文是 CLV 工程的权威架构说明。当前文档版本：0.8.1 · 日期：2026-10-21。文档版本与工程版本分轨维护，工程版本的单一来源是 `include/CLV_Version.h` 的四个宏。
> 面向读者：要改这个库的代码的人。使用者读 `README.md` 即可，本文解释**为什么这样分层**与**每层的硬约束**。

---

## 1. 项目定位

CLV = **C++ Lightweight Video**：自研轻量视频容器 + 自研视频编解码 + 自研字幕格式。当前落地的是地基层（文件、日志、内存、版本）、**容器读写面**（`src/container/` + `include/CLV_Container.h`，函数名 / 参数结构体 / 出参形态属对外 ABI，改动即破坏调用方；错误码只在枚举尾部追加、既有码值与语义不动；未冻结的是 `.clv` 的字节格式与语义）、**音频编解码闭环**（libFLAC 接到帧级）、**视频编解码**（解码句柄七入口全真 + 编码端有损重建闭环 + 无损档 + 帧间运动补偿与半像素插值）、**呈现节拍面**（`src/pace/` + `include/CLV_Pace.h`：主时钟口径与逐帧判定，不依赖任何播放器库）；**字幕格式尚未开工**。

三层物理结构：

1. **公开层**（`include/`）：C ABI 契约。不透明句柄 + 自由函数 + 整型错误码枚举；
2. **实现层**（`src/`、`platform/`、`backends/`）：C++ 编写，MSVC 风格；C++ 类型一律不进出参签名；
- 库本体引用的第三方头只出现在 `backends/`（日志后端取 spdlog、音频编解码后端取 libFLAC）；`tests/` 用 googletest；`demo/` 三份纯 C 示例，只有双轨播放器那一份链外部 SDL3（库本体零 SDL 依赖，见 §6「SDL3 的四层结论」）。两者都不在库目标之内。

**核心设计原则**：

- **公开面是 C ABI**：CLV 是库不是应用，要能被不同编译器编出的宿主、不同 DLL 边界的宿主、以及非 C++ 语言绑定消费。因此公开头只用 C 语法，句柄不透明，错误用整型枚举——C++ 的类、模板、异常、`std::string` 都不出现在 `include/`。
- **C++ 只存在于实现层**：`clv::core` / `clv::platform` / `clv::backend` 三个命名空间各管一段，跨模块释放的风险靠「`new` / `delete` 都在本库模块内」这条约束消掉，而不是靠跨 ABI 边界的智能指针。
- **能力按「编译期唯一」还是「编译期可配置」分家**：平台绑定的能力（文件 IO）进 `platform/`，同一目标里只可能编进一份；可换后端的（日志、音频编解码）进 `backends/`，由构建开关选。这条分界决定了目录归属，不允许混放。
- **平台判定只有一个 `#if`**：CMake 的 `if(WIN32)` 是主判定，`platform/platform.h` 是裸机 / 直接编译场景的兜底；真正的选源 `#if` 全库只有 `platform/fileio/impl_fileio.cpp` 一处。每份平台源文件自带守卫，误加进别平台的构建时整文件为空。
- **降级与能力走状态，不走错误通道**：日志后端为空时 `CLV_Log` 静默丢弃并返回 OK，不报错；`CLV_OpenMemoryFile` 未落地时返回 `CLV_FILE_UNSUPPORTED`。理由是下一条。
- **日志失败不许影响宿主**：`CLV_Log` 没有返回值。日志是旁路能力，一旦给它错误通道，宿主就得为「写日志失败」写分支，这会把旁路变成主路。控制台编码同理——库只投 UTF-8 字节串，怎么显示是应用的事，这份义务由 `demo/core.c` / `demo/container.c` 各自的 `SetupConsoleUtf8()` 演示，库不代劳。
- **不猜调用方的路径**：Windows 侧只走 W 系列 API（UTF-8 → UTF-16 → `CreateFileW`），但**不做分隔符改写、不加 `\\?\` 前缀**。改写分隔符会掩盖调用方的错误；长路径前缀涉及一整套语义（卷名、相对路径、尾点），v1 不承诺，交给应用侧预处理。
- **不越权承诺内存策略**：`CLV_Alloc` / `CLV_Realloc` / `CLV_Free` 直通 C 运行时。分配口本身不设防（失败返回 NULL，不重试、不记日志，判空归调用方）；容器面、视频面、音频面、节拍面的入口在 `extern "C"` 边界上把分配失败收敛成各自的内存不足错误码后回报（`CLV_CONTAINER_OUT_OF_MEMORY` / `CLV_VIDEO_OUT_OF_MEMORY` / `CLV_AUDIO_OUT_OF_MEMORY` / `CLV_PACE_OUT_OF_MEMORY`），所以「调用方判空」只对直接用 `CLV_Alloc` 系列的口子成立。文件面不设这一档：`CLV_FileError` 里没有内存不足码，句柄分配走 `platform/fileio/impl_fileio.cpp` 的 `CreateFileIO()`。宿主注入分配器、静态池后端都还没做，见 §6。

---

## 2. 工程结构

```text
CLV/
├── include/                  # 公开 C ABI，平铺，CLV_ 前缀即分类
│   ├── CLV_Export.h          # CLV_EXPORT 符号可见性开关
│   ├── CLV_Version.h         # 版本查询
│   ├── CLV_File.h            # 文件读写（句柄 + 三模式 + 错误码）
│   ├── CLV_Logger.h          # 日志（等级 + 过滤）
│   ├── CLV_Memory.h          # 分配口
│   ├── CLV_Container.h       # 容器读写面（句柄 + 错误码 + 出参结构；形状属对外 ABI）
│   ├── CLV_Video.h           # 视频编解码面（解码七入口 + 编码端重建 + 帧与码流句柄）
│   ├── CLV_Audio.h           # 音频编解码面（FLAC 帧与承载块 + 编解码句柄 + 缓冲引用计数）
│   └── CLV_Pace.h            # 呈现节器件（主时钟、起播零点、两道闸；不依赖任何播放器库）
│
├── src/                      # C ABI 的实现与平台无关工具
│   ├── version.cpp           # clv（顶层身份）
│   └── core/                 # clv::core（调度层 + 工具）
│       ├── fileio.cpp        # 路径契约判定、句柄转换、后端生命周期
│       ├── logger.cpp        # 日志后端选源
│       ├── memory.cpp        # malloc / realloc / free 直通
│       ├── crc32.{h,cpp}     # CRC-32/MPEG-2（实现层工具，不出 C ABI）
│       └── varint.{h,cpp}    # LEB128 + zigzag + 最短形式判定（同上）
│
├── src/container/            # clv::container：容器读写核心（不碰平台 IO）
│   ├── byte_io.h             # 小端读写，读一律有界
│   ├── io.h                  # 字节源 / 汇抽象（内存实现供测试，文件实现由门面层适配）
│   ├── status.h              # 内部返回码，与公开枚举逐项同序
│   ├── timebase.{h,cpp}      # 统一时间轴比较（128 位整数，自带 64×64→128）
│   ├── packet.{h,cpp}        # 数据包编解码与逐档解析结果
│   ├── structure.{h,cpp}     # 定长结构编解码 + 值域校验
│   ├── frames.{h,cpp}        # 片序连续性与帧重组
│   ├── writer.{h,cpp}        # 布局、时间轴平移、分片、索引、回填、分段身份与文件级扩展块链
│   ├── reader.{h,cpp}        # 严格解析、容错处置、重同步、索引校验、段描述块与段观测计数
│   └── clv_container.cpp     # C ABI 门面（句柄转换 + 错误码收敛）
│
├── src/audio/                # clv::audio：FLAC 帧与元数据层 + C ABI 门面
│   ├── frame.{h,cpp}         # 帧头自核、块大小与采样率码表、子集上下界判据
│   ├── metadata.{h,cpp}      # 元数据块解析（STREAMINFO 位域）与流信息对账
│   └── clv_audio.cpp         # C ABI 门面：编解码闭环直调 backends/flac 的解析口，本文件 include backends/flac/flac_codec.h
│
├── src/video/                # clv::video：VOP 头 / 宏块 / 变换量化 / 运动补偿 + C ABI 门面
│   ├── headers.{h,cpp}       # VOP 头、片头、ext_type = 3 元数据块
│   ├── bits.{h,cpp}          # 位读写
│   ├── macroblock.{h,cpp}    # 宏块头两套语法 + 中值预测器状态（I/P 一套、B 前向 / 后向各一套）
│   ├── annex_b_tables_p.h    # 宏块头的三张变长码表（mcbpc 两张 + cbpy 一张），外部标准宏块层码表裁出的本库版本
│   ├── block.{h,cpp}         # 块系数载体：两档编码法（定宽直写 / EOB 截尾 + Exp-Golomb 幅值，帧头 3 bit 逐帧选定）+ 位宽表（帧内 / 帧间各七档）
│   ├── quant.{h,cpp}         # Hadamard 8×8、量化 / 反量化、有效量化步长
│   ├── reconstruct.{h,cpp}   # 块像素搬运 + 六块坐标 + 重建（**编解同式的唯一实现**）
│   ├── motion.{h,cpp}        # 运动补偿：半像素插值（亮度色度同核）、矢量换算、双向平均、运动估计
│   ├── direct_scale.{h,cpp}  # B-VOP direct 的矢量缩放（矢量域，向零截断，禁 >>）
│   ├── payload.{h,cpp}       # VOP 载荷与分片（片界靠 consumed 推，不靠额外字段）
│   ├── padding.{h,cpp}       # 补后尺寸推导（16 对齐）
│   ├── dpb.{h,cpp}           # 参考帧管理与显示序输出队列（持有只有一份账、记在帧对象上，本类经 FrameHolders 两个口加减）
│   ├── rgb.h + clv_rgb.cpp   # 帧平面 I420 → RGB24：AVX2 支与恒标量支同式，后者专供用例把两支逐字节对平
│   ├── status.h              # 视频层内部返回码，与公开枚举逐项同序（门面直接转值，顺序由 static_assert 钉住）
│   ├── encoder_hook.h        # 实现层测试钩子：取编码器自己的重建帧；不带导出宏、不进公开头、不进动态件导出表
│   └── clv_video.cpp         # C ABI 门面：编码端内部重建、解码端对称
│
├── src/pace/                 # clv::pace：呈现节器件（主时钟口径 + 逐帧判定 + 背压闸）
│   ├── status.h              # 节拍面内部返回码，与 CLV_PaceError 逐项同序（static_assert 钉住）
│   └── clv_pace.cpp          # C ABI 门面：不碰线程、不碰设备、不依赖任何播放器库，设备侧只经两个回调交进来
│
├── platform/                 # clv::platform：编译期唯一、与平台绑定的能力
│   ├── platform.h            # 平台宏兜底推导（裸宏唯一出现处）
│   └── fileio/               # impl_fileio.{h,cpp} 唯一选源 + win32 / linux / macos 三份实现
│
├── backends/                 # clv::backend：可替换实现（日志 + 音频编解码）
│   ├── impl_logger.h
│   ├── spdlog/               # 真实日志后端（第三方只在这里出现）
│   ├── null/                 # 空后端（丢弃一切，无依赖场景用）
│   └── flac/                 # 音频编解码后端：libFLAC 桥，全仓唯一含第三方 FLAC 头之处
│
├── cmake/                    # sources.cmake 公共清单 + platform/<平台>.cmake + backends/<后端>.cmake
├── tests/                    # 单一可执行 clv_tests；用例按平台目录 + 域两层存放：win32/<域>/（core /
│                             # container / audio / video / independent / api），linux/ 与 macos/ 暂无用例；
│                             # docs/ 一测试项一文档；independent/ 是独立验证层（期望不取自被测函数，
│                             # `ctest -R Independent` 可单跑）
├── demo/                     # 示例源码（core.c 地基层 + container.c 容器层 + video.c 双轨播放器）+ 操作手册；
│                             # demo 目标（CLV_BUILD_DEMOS）：三份纯 C 示例，只有 video.c 链外部 SDL3；库本体零 SDL 依赖
├── assets/                   # 入库测试素材：README + fixtures/ 三份自制确定性 .clv + 一份双轨演示件 .clv + 音视频素材
├── docs/                     # 格式规范正文（容器规范）
├── skills/                   # 项目级 Skill：与平台无关的唯一事实来源（各平台扫描目录里只放逐件链接，不入库）
├── scripts/                  # 仓库内所有 Python 工具的入口（不参与 CMake 构建）
└── third-party/              # 第三方源码：spdlog 与 googletest 纳入版本库；SDL3（仅 demo）与 flac（音频编解码后端）由本地拉取、不入库（`.gitignore` 已排除）
```

| 目录 | 用途 | 是否入库 |
|---|---|---|
| `include/` `src/` `platform/` `backends/` `cmake/` `tests/` `demo/` `assets/` `docs/` `skills/` `scripts/` `third-party/` | 源码、素材、文档与构建 | 是（`third-party/` 里入库的是 spdlog 与 googletest，SDL3 与 flac 两份由本地拉取、`.gitignore` 已排除） |
| `build/` | CMake 产物、`build/_install`（第三方安装前缀） | 否 |
| `demo_local/` | 本地产物区：`bin/` 只放可执行（每次构建先清空再投递）、`src/` 是 `demo/` 的同步副本、根上放示例跑出来的文件 | 否 |
| `compile_commands.json` | 移动到仓库根供 clangd 读取，命令里带本机绝对路径 | 否 |
| `.venv/`、`.vscode/`、`.cache/` | 环境与编辑器 | 否 |

易混淆对照：

| 一对目录 | 分界 |
|---|---|
| `platform/` vs `backends/` | 前者**编译期唯一**（一个目标里只可能有一份平台实现），后者**编译期可配置**（多份后端按开关选）。判据：能不能在同一份二进制里同时编进两份？能 → `backends/` |
| `src/core/` vs `platform/fileio/` | 前者做契约判定与调度（路径合法性、句柄转换、生命周期），后者只跟平台 API 打交道，不判公开契约 |
| `third-party/` vs `build/` | 前者放第三方**源码**，后者只放**产物**；第三方安装前缀是 `build/_install` |
| `skills/`（仓库根）vs `<平台>/skills/` | 两者末级同名（本仓这一侧一律用复数命名），**靠父路径区分，引用时写全路径**。前者是本仓 Skill 的**唯一事实来源**（入库、与平台无关，只放平台无关的内容）；后者只是某个平台自己的扫描入口目录，名字与层级由该平台自己的约定决定，不入库。入口里**逐件**放 `<平台>/skills/<name> -> ../../skills/<name>` 的链接（一件一链），**不做整目录链接**：整目录链过去，别的平台要加自己专有的 Skill 就只能往共同区里塞，共用面被污染。某件在平台目录里是**实体文件**时视为该平台的有意分叉，脚本不覆盖。由 `scripts/setup_skills.py` 现场重建；本仓同时接几家就建几个入口目录，不存在默认某一家 |
| `demo/` vs `demo_local/` | 前者入库、只放源码与手册；后者不入库、放构建投递的产物与示例跑出来的文件，整目录删掉可再生 |

---

## 3. 代码架构

```
宿主（C / C++ / 其它语言绑定）
  └─► include/CLV_*.h            C ABI 契约，extern "C"
        └─► src/core/*.cpp       clv::core —— 调度、契约判定、选源
              ├─► platform/fileio/impl_fileio.cpp   全库唯一平台 #if
              │     ├─► win32_fileio.cpp            CreateFileW / ReadFile / WriteFile
              │     ├─► linux_fileio.cpp            stdio
              │     └─► macos_fileio.cpp            stdio（iOS 在选源处 #error）
              └─► backends/impl_logger.h
                    ├─► spdlog/logger_backend.cpp   真实后端（第三方只在这里出现）
                    └─► null/logger_backend.cpp     空后端
```

依赖单向，不得反向：`include → src/core → platform / backends`。依赖单向指实现层不得反向依赖上层；公开头是各层共用的接口件，平台层与后端 include 公开头取枚举值不算反向依赖。

- `include/` 零依赖：只依赖 C 标准库头与自身；公开头之间只允许 `CLV_Export.h` 被其它头包含。
- `src/core/` 不直接调平台 API，也不直接 include 第三方头。
- `platform/` 与 `backends/` 不知道彼此存在，由 `src/core/` 组合。
- 实现层接口（`platform/fileio/impl_fileio.h`、`backends/impl_logger.h`）的返回值取值域与公开错误枚举**一致**，实现层不得自创语义、不得把系统错误码直接透出——映射责任在后端内部。
- 命名：公开符号 `CLV_*`（SDL 风格）；命名空间 `clv` / `clv::core` / `clv::platform::{win32,linux_,macos}` / `clv::backend::{spdlog,null}`。`linux_` 带尾下划线是因为 `linux` 是部分编译器的预定义宏，裸用会打架。
- 实现层命名全仓一套：类型与函数 `PascalCase`，数据成员一律尾下划线 `handle_`（不用 `m_` / `s_` 这类前缀记法），
  枚举值与文件级常量 `kXxx`。这一条同样约束 `platform/` 与 `backends/`，新增后端不得另立风格。

### 解码侧的并行度与出帧深度（读方内部实现，不进码流）

- **帧内分带**：一帧的宏块按行区间切成若干带，各带在自己的线程里做「取预测 + 重建 + 回写」。
  成立依据是走到这一步的读写集，四条都与素材无关：整帧语法已一次解析完（这一段不读比特）、
  帧内预测取常量电平不读邻块重建、帧间预测只读上一帧的重建、v1 无环路滤波与 OBMC。
  并行度是**编译期常量** `CLV_DEC_MB_THREADS`（出厂缺省 16；1 = 不切带），与 `CLV_COEF_CODING` 同款口径：
  不进公开入参、不接受环境变量改行为，出厂形态由构建面一次确定。
  它既不改码流也不改像素 —— 同一份字节在 1 档与多档下逐帧内容哈希同值，
  所以第三方不切带照样解同一份流，规范侧不必为它加任何语法，本能力只活在读方内部。
- **出帧深度** `CLV_VideoStreamParamsC::preroll_frames`：待输出队列攒到该值才吐第一帧，之后一进一出。
  地板 = `max_b_frames + 2`（重排本来就需要这么深），**只升不降** —— 降下去等于在显示序排好之前吐帧，
  直接违反「出帧按 PTS 升序」这条契约，故它不是一个可以往小调的旋钮。
- **两条不正交，读数必须同报**：深度的首帧代价 = `(N − 地板) × 每帧解码时间`，而分带正是压小
  「每帧解码时间」的那一项 ⇒ **帧数参数不得脱离并行度单独定档**。本机实测（1080p、绑 P 核）：
  同一档 N=6 相对 N=0 的首帧增量，在并行度 1 上是 +145.4 ms，在并行度 8 上只 +28.9 ms。

### 数据流

```
CLV_ReadFile(h, buf, size, &got)
  → src/core/fileio.cpp        句柄 → clv::platform::FileIO*（NULL 句柄在此挡下）
  → platform/fileio/win32_fileio.cpp
        模式门控 → 1 MiB 分块循环 ReadFile（短读即判到文件尾，break）
  → got 出参 + CLV_FileError
```

```
CLV_ContainerReaderNextFrame(r, &frame, &got)
  → src/container/clv_container.cpp   句柄转换；文件面失败收敛为 CLV_CONTAINER_IO_FAILED
  → src/container/reader.cpp          窗口读 → packet.cpp 逐档解析 → 容错处置 / 必要时重同步
  → src/container/frames.cpp          按 fragment_index 落位重组 → 整帧出参（载荷有效期到下次调用）
  →（多段句柄）段界出 got_frame = 0 时按清单轮转 reader：拆旧段 → 试开下一段 → 描述符比对 → 继续同一条取帧循环

CLV_Log(level, msg)
  → src/core/logger.cpp        等级过滤（越界整条丢弃）→ 函数内 static 的活跃后端
  → backends/spdlog 或 backends/null
  → 无返回值（日志失败不向调用方报告）
```

配置与状态：工程版本的单一来源是 `include/CLV_Version.h` 的四个宏，`src/version.cpp` 从它取；CMake 的 `project(... VERSION ...)` 另写同一组数字，两处一致性由版本用例钉住；构建期配置（后端、平台）不进运行期状态，运行期只有日志等级过滤一个可变量。

---

## 4. 构建与测试

```bash
python scripts/install_third_party.py                       # third-party 源码 -> build/_install
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug      # 必须在 vcvars64 的同一个进程里
cmake --build build
ctest --test-dir build --output-on-failure
python scripts/format_all.py                                # C/C++ 走 .clang-format，Python 走 black
```

- 构建流程：`cmake/sources.cmake` 列平台与后端无关的源文件；`cmake/platform/<平台>.cmake` 与 `cmake/backends/<后端>.cmake` 各补自己那份文件清单（只列文件，判定在根 `CMakeLists.txt`）；`move_compile_db` 目标把 `compile_commands.json` 从 `build/` 移到仓库根供 clangd 读。
- 库目标是两条产出路，同一份源：`clv_static`（STATIC，恒在）与 `clv`（SHARED，仅 `CLV_BUILD_SHARED=ON` 时）。链接分工定死——`clv_tests` 恒链静态件（它吃 `src/` 下的实现层头与不出 ABI 的内部钩子），三份示例在开关 ON 时链动态件、否则退回静态件，所以示例就是外部 ABI 消费者的验收面（`clv.dll` 随构建投递进 `demo_local/bin`，与 exe 同目录）；第三份示例 `clv_demo_video` 是同一条 ABI 上的双轨播放器，另链外部 SDL3、其 DLL 同样投递进 `bin`。公共配置（包含路径、平台宏、告警档、spdlog 与 FLAC 的 PRIVATE 链接）收在 `clv_configure_library_target()` 一处，两目标共用。
- 生成器固定 Ninja：Visual Studio 生成器是多配置，且缺省平台 win32 会与 64 位的第三方库打架。
- 第三方接入：通用依赖走 `find_package`（用 `build/_install`）优先、失败回退源码 `add_subdirectory`；**libFLAC 是例外，走 `find_path` + `find_library` 自建导入目标（唯一路，见 `README.md` 构建与验收节「libFLAC 怎么找」）**。
- 测试：`tests/` 编成单一可执行 `clv_tests`，用 GTest 但自带 `main`，用例经 `gtest_discover_tests` 注册进 CTest。例数只有一条口径：**以 `ctest -N` 的注册数为准，下表分域相加必须等于合计**（本条、下面的分域表、§6 的现状清单三处同一把尺，任何一处单独写数都会与另两处打架；数例数的谓词要含缩进与 `TEST_F`，只数顶格 `TEST(` 会少 42 条）。用例按「平台目录 + 域」两层存放：Windows 用例在 `tests/win32/<域>/`，`tests/linux/` 与 `tests/macos/` 暂无用例；测试项文档在 `tests/docs/`（一测试项一文档 + README 索引）。用例按「测的是哪一层」分域：`api` 域只吃公开 C ABI、不 include 实现层头；`core` 域直测不出 ABI 的内部工具（CRC-32/MPEG-2、LEB128）；`container` / `audio` / `video` 三个域两面混吃——句柄端到端走公开面，单元级直测直接 include `src/` 下的头，用于钉变换 / 量化 / 块载体 / 运动补偿 / 参考帧管理与后端接线这些不进 ABI 的行为；`tests/win32/independent/` 是独立验证层——期望值不取自被测函数本身，逐档爬到护栏并各配一个「已知会失败」的对照输入，`ctest -R Independent` 可单跑。

| 套件 | 领域 | 例数（`ctest -N` 注册数，分域相加 = 合计） |
|---|---|---|
| `core` | 路径与模式判定、CRC-32/MPEG-2、LEB128 与 zigzag、内存、版本 | 20 |
| `api` | 公开头端到端行为（打开 / 读写 / 游标 / 门控 / 错误码 / 日志等级）+ 容器面句柄与错误码 + 参数结构体自报尺寸 + 音频元数据解读面 + 节拍面 | 76 |
| `container` | 包编解码与容错分档、片序连续性与重组、读写全链路（内存源 / 汇）、索引排序与二分、分片接口、分段器（段描述块编解码与值域、多段清单续读、段观测计数、`WriteParams` 尺寸三档并存） | 77 |
| `audio` | 帧头 / 承载块 / 注释块结构层（含帧头编码号容量档与最短形式）、libFLAC 编解码闭环与逐帧块大小对账 | 39 |
| `video` | VOP 头与片头、宏块头 / B-VOP 头、块系数载体（两档编码法）、编码端有损重建闭环、运动补偿与半像素插值、direct 缩放、DPB 与输出序、补黑、多片载荷、帧引用计数与 RGB24 转换 | 142 |
| `independent` | 独立验证层：期望值不取自被测实现，逐档爬护栏 + 反向对照 | 21 |
| **合计** | 与 `ctest -N` 报出的注册数同值 | **375** |

`CLV_LOGGER_USE_SPDLOG` 的 ON 与 OFF 两种配置各跑一遍同一套用例。

---

## 5. 文档索引

| 文档 | 说明 |
|---|---|
| `README.md` | 使用者入口：安装、快速开始、公开接口、使用约定、构建与验收 |
| `API.md` | 公开 C ABI 逐函数说明：句柄与错误码约定、入参出参语义、最小示例 |
| `docs/容器规范.md` | `.clv` 字节格式的权威规范：布局、字段值域、时间模型、分片与容错 |
| `ARCHITECTURE.md` | 本文件：分层、依赖、数据流、构建组织、现状 |
| `CHANGE.log` | 版本演进与兼容声明 |
| `include/CLV_*.h` | 接口契约正文（头注释即规范） |
| `skills/` | 项目级 Skill：收口复扫 / 门禁跑批 / 交付记录 / Skill 生成器，协作方与 AI 都按它执行 |
| `assets/README.md` | 入库测试素材的登记件：`assets/fixtures/` 现有四份 `.clv`（三份自制确定性件 + 一份本库编码并封装的双轨演示件）、三支外部示例视频、外部音频原件与由它转出的 FLAC 件，另附来源件。构成、尺子分类、冻结字节长度与 CRC-32/MPEG-2 / SHA-256 冻结值**一律以该件为准**，本索引不重复列清单 |
| `tests/docs/` | 一测试项一文档：测什么 / 怎么测 / 判据 / 平台 / 现状 |
| `demo/README.md` | 示例库与验收步骤 |
| `人机协同工作流程.md` + 三份 `AI*.md` | **四份项目外自由文档**：入库，但不属本项目、不受本项目或任何角色管辖，也不参与本工程的口径裁定 |

---

## 6. 现状

**已实现**：C ABI 公开面九头；文件读写（三模式、1 MiB 分块、UTF-8 路径契约）；日志（spdlog 与空后端、等级过滤）；内存分配口；版本查询；实现层工具 CRC-32/MPEG-2、LEB128 + zigzag + 最短形式判定、128 位整数时间轴比较；容器读写面（定长结构、数据包编解码、分片重组、容错与重同步、索引构建与二分查询、分段容器的段描述块与多段清单续读、段间扩展块一致性查询、C ABI 门面），**形状属对外 ABI**；Ninja + x64 构建与 CTest 全量用例（例数以 `ctest -N` 注册数为准，分域口径见 §4 那张分域表，终值在收口时一次刷齐）；入库确定性测试素材三份与双轨演示件一份。音频编解码闭环（libFLAC 后端接线到帧级：帧头自核 + 流声明对账 + 一包一帧）；视频编解码的**解码句柄七入口全真**（吃码流吐帧 + DPB + 出帧序 + 拒解粘滞态）与**编码端有损重建闭环**（Hadamard8 → 量化 → 系数载体（定宽直写 / EOB+EG 两档，编码器侧按编译期选档）→ 编码器内部重建 → 存自己的参考帧；解码端走同一套反量化 / 反变换），**无损档已接**（`mode = LOSSLESS` + 码流里 `vop_quant = 0`，系数直载 15 bit 载体，重建与源逐字节全等），**帧间运动补偿与半像素插值两侧同式**；帧的借用走引用计数（取帧加一、`CLV_VideoFrameFree` 减一、计数归零才回收），帧平面 → RGB24 的转换口在库侧（AVX2 支与恒标量支逐字节同式）；**呈现节拍面**（`src/pace/` + `include/CLV_Pace.h`：主时钟口径 + 逐帧判定 + 背压闸，不依赖任何播放器库）；**三份纯 C 示例**（地基层、容器层、双轨播放器；播放器那一份链外部 SDL3，库本体零 SDL 依赖）。

**未落地 / 占位**：

| 项 | 状态 |
|---|---|
| B 帧残差通路 | 编码侧目前只产 `direct` + `not_coded`（不带残差、不带块数据）；解码侧四型语法（direct / interpolate / backward / forward）均已实现，`not_coded` 走整块复制 |
| 字幕格式 | 未开工 |
| `.clv` 字节格式与语义的对外发布 | 格式版本 2.0 已写进 `docs/容器规范.md`，但尚未随发布冻结；接口形状已属对外 ABI，不在未落地之列 |
| `CLV_OpenMemoryFile`（内存缓冲区后端） | 接口占位：有声明有定义，返回 `CLV_FILE_UNSUPPORTED` |
| `CLV_LogSetStorage`（日志落盘） | 接口占位：调用无效果 |
| 宿主注入后端与分配器路由 | 未做：后端由编译期选源，`CLV_Memory` 直通 C 运行时，库内部不经它 |
| 能力查询（当前生效后端 / 是否降级） | 未做：C ABI 上问不出后端状态 |
| SDL3 的四层结论 | ① 库本体源码**零** SDL include（`grep '#include *[<"]SDL' src include platform backends tests` = 0 命中），全仓唯一带 SDL 头的是 `demo/video.c`；② 链接面**一个** target 链 `SDL3::SDL3`（只有 `clv_demo_video`，库与测试都不链）；③ 构建面**只在开示例时相关**——SDL3 不进 `CLV_BUILD_DEMOS=OFF` 的查找，而 `ON` 时找不到就是配置期 `FATAL_ERROR`（有 target 真消费它，不再降级为 WARNING）；插桩构型不建它（`scripts/install_third_party.py` 的 `SANITIZE_SKIP`）；④ 文档面**已补**——本节与 `demo/README.md` 的播放器一节即口径。据此 SDL3 **不迁移**、不作库依赖，它只是示例的依赖 |
| 对齐分配 | 未加，等 SIMD 侧有需求再扩 |
| iOS | `platform.h` 留检测分支，选源处 `#error` 占位 |
| Linux / macOS 后端 | 代码按 POSIX 语义写好，**未经非 Windows 平台编译验证**，不作为可用承诺 |
| 容器面的非 Windows 验证 | 容器核心不碰平台 IO，但仍只在 MSVC x64 上编过、跑过 |
| sanitizer 覆盖面 | 内存安全（ASan）在 MSVC 构型下实测过，且**两档系数编码法各跑一遍**（定宽档与默认档，各自独立构建目录、插桩专用第三方前缀）：全量用例零命中、UAF 探针能自证 ⇒ 检测通路有效。两档都跑是必要的 —— 只跑默认档时，定宽档才走的分支（分档常量的 `#else` 侧、定宽载体的消费路径、"取错法必须被拒"那条断言）在 sanitizer 下等于没测。双档那两把的全量零命中是跑批本身的结果；**UAF 探针自证（证明检测通路真的有效，而不是"没报错等于没测到"）取自另一次实测** —— 两件事别并成一句读，任何一格都不替另一格背书。**泄漏与 UB 两格本工具链给不出**：MSVC ASan 明写 `detect_leaks is not supported on this platform`；`/fsanitize=undefined` 被 `D9002 忽略未知选项` 静默吞掉且返回 0（假绿），链接侧 `/fsanitize=address` 同类（`LNK4044`）。要这两格的数得换手段（CRT leak debug / Dr. Memory / clang-cl），属构建扩面 |







