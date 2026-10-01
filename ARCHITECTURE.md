# CLV 整体架构

> 本文是 CLV 工程的权威架构说明。当前文档版本：0.1.0-dev（与工程版本 `0.0.1` 分轨维护）· 日期：2026-10-01。
> 面向读者：要改这个库的代码的人。使用者读 `README.md` 即可，本文解释**为什么这样分层**与**每层的硬约束**。

---

## 1. 项目定位

CLV = **C++ Lightweight Video**：自研轻量视频容器 + 自研视频编解码 + 自研字幕格式。当前落地的是地基层（文件、日志、内存、版本），容器 / 编解码 / 字幕尚未开工。

三层物理结构：

1. **公开层**（`include/`）：C ABI 契约。不透明句柄 + 自由函数 + 整型错误码枚举；
2. **实现层**（`src/`、`platform/`、`backends/`）：C++ 编写，MSVC 风格；C++ 类型一律不进出参签名；
3. **第三方层**（`third-party/`）：源码形态存放，只在 `backends/` 里被引用。

**核心设计原则**：

- **公开面是 C ABI**：CLV 是库不是应用，要能被不同编译器编出的宿主、不同 DLL 边界的宿主、以及非 C++ 语言绑定消费。因此公开头只用 C 语法，句柄不透明，错误用整型枚举——C++ 的类、模板、异常、`std::string` 都不出现在 `include/`。
- **C++ 只存在于实现层**：`clv::core` / `clv::platform` / `clv::backend` 三个命名空间各管一段，跨模块释放的风险靠「`new` / `delete` 都在本库模块内」这条约束消掉，而不是靠跨 ABI 边界的智能指针。
- **能力按「编译期唯一」还是「编译期可配置」分家**：平台绑定的能力（文件 IO）进 `platform/`，同一目标里只可能编进一份；可换后端的（日志）进 `backends/`，由构建开关选。这条分界决定了目录归属，不允许混放。
- **平台判定只有一个 `#if`**：CMake 的 `if(WIN32)` 是主判定，`platform/platform.h` 是裸机 / 直接编译场景的兜底；真正的选源 `#if` 全库只有 `platform/fileio/impl_fileio.cpp` 一处。每份平台源文件自带守卫，误加进别平台的构建时整文件为空。
- **降级与能力走状态，不走错误通道**：日志后端为空时 `CLV_Log` 静默丢弃并返回 OK，不报错；`CLV_OpenMemoryFile` 未落地时返回 `CLV_FILE_UNSUPPORTED`。理由是下一条。
- **日志失败不许影响宿主**：`CLV_Log` 没有返回值。日志是旁路能力，一旦给它错误通道，宿主就得为「写日志失败」写分支，这会把旁路变成主路。控制台编码同理——库只投 UTF-8 字节串，怎么显示是应用的事，这份义务由 `demo/clv_demo.c` 的 `SetupConsoleUtf8()` 演示，库不代劳。
- **不猜调用方的路径**：Windows 侧只走 W 系列 API（UTF-8 → UTF-16 → `CreateFileW`），但**不做分隔符改写、不加 `\\?\` 前缀**。改写分隔符会掩盖调用方的错误；长路径前缀涉及一整套语义（卷名、相对路径、尾点），v1 不承诺，交给应用侧预处理。
- **不越权承诺内存策略**：`CLV_Alloc` / `CLV_Realloc` / `CLV_Free` 直通 C 运行时，全库对内存耗尽不设防（失败返回 NULL，判空归调用方）。宿主注入分配器、静态池后端都还没做，见 §6。

---

## 2. 工程结构

```text
CLV/
├── include/                  # 公开 C ABI，平铺，CLV_ 前缀即分类
│   ├── CLV_Export.h          # CLV_API 符号可见性开关
│   ├── CLV_Version.h         # 版本查询
│   ├── CLV_File.h            # 文件读写（句柄 + 三模式 + 错误码）
│   ├── CLV_Logger.h          # 日志（等级 + 过滤）
│   └── CLV_Memory.h          # 分配口
│
├── src/                      # C ABI 的实现与平台无关工具
│   ├── version.cpp           # clv（顶层身份）
│   └── core/                 # clv::core（调度层 + 工具）
│       ├── fileio.cpp        # 路径契约判定、句柄转换、后端生命周期
│       ├── logger.cpp        # 日志后端选源
│       ├── memory.cpp        # malloc / realloc / free 直通
│       ├── crc32.{h,cpp}     # CRC-32/MPEG-2（实现层工具，不出 C ABI）
│       └── varint.{h,cpp}    # LEB128（同上）
│
├── platform/                 # clv::platform：编译期唯一、与平台绑定的能力
│   ├── platform.h            # 平台宏兜底推导（裸宏唯一出现处）
│   └── fileio/               # impl_fileio.{h,cpp} 唯一选源 + win32 / linux / macos 三份实现
│
├── backends/                 # clv::backend：编译期可配置多实现的能力
│   ├── impl_logger.h
│   ├── spdlog/               # 真实日志后端（第三方只在这里出现）
│   └── null/                 # 空后端（丢弃一切，无依赖场景用）
│
├── cmake/                    # sources.cmake 公共清单 + platform/<平台>.cmake + backends/<后端>.cmake
├── tests/                    # 单一可执行 clv_tests，只吃公开 C ABI
├── demo/                     # 示例源码 + 操作手册
├── script/                   # 工具入口（全 .py，不参与 CMake 构建）
└── third-party/              # 第三方源码：spdlog、googletest
```

| 目录 | 用途 | 是否入库 |
|---|---|---|
| `include/` `src/` `platform/` `backends/` `cmake/` `tests/` `demo/` `script/` `third-party/` | 源码与构建 | 是 |
| `build/` | CMake 产物、`build/_install`（第三方安装前缀） | 否 |
| `demo_local/` | 本地产物、示例源码副本、本地复检清单 | 否 |
| `compile_commands.json` | 移动到仓库根供 clangd 读取，命令里带本机绝对路径 | 否 |
| `.venv/`、`.vscode/`、`.cache/` | 环境与编辑器 | 否 |

易混淆对照：

| 一对目录 | 分界 |
|---|---|
| `platform/` vs `backends/` | 前者**编译期唯一**（一个目标里只可能有一份平台实现），后者**编译期可配置**（多份后端按开关选）。判据：能不能在同一份二进制里同时编进两份？能 → `backends/` |
| `src/core/` vs `platform/fileio/` | 前者做契约判定与调度（路径合法性、句柄转换、生命周期），后者只跟平台 API 打交道，不判公开契约 |
| `third-party/` vs `build/` | 前者放第三方**源码**，后者只放**产物**；第三方安装前缀是 `build/_install` |
| `demo/` vs `demo_local/` | 前者入库、只放源码与手册；后者不入库、放产物与本地复检记录 |

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

依赖单向，不得反向：`include → src/core → platform / backends`。

- `include/` 零依赖：只依赖 C 标准库头与自身；公开头之间只允许 `CLV_Export.h` 被其它头包含。
- `src/core/` 不直接调平台 API，也不直接 include 第三方头。
- `platform/` 与 `backends/` 不知道彼此存在，由 `src/core/` 组合。
- 实现层接口（`platform/fileio/impl_fileio.h`、`backends/impl_logger.h`）的返回值取值域与公开错误枚举**一致**，实现层不得自创语义、不得把系统错误码直接透出——映射责任在后端内部。
- 命名：公开符号 `CLV_*`（SDL 风格）；命名空间 `clv` / `clv::core` / `clv::platform::{win32,linux_,macos}` / `clv::backend::{spdlog,null}`。`linux_` 带尾下划线是因为 `linux` 是部分编译器的预定义宏，裸用会打架。

### 数据流

```
CLV_ReadFile(h, buf, size, &got)
  → src/core/fileio.cpp        句柄 → clv::platform::FileIO*（NULL 句柄在此挡下）
  → platform/fileio/win32_fileio.cpp
        模式门控 → 1 MiB 分块循环 ReadFile（短读即判到文件尾，break）
  → got 出参 + CLV_FileError
```

```
CLV_Log(level, msg)
  → src/core/logger.cpp        等级过滤（越界整条丢弃）→ 函数内 static 的活跃后端
  → backends/spdlog 或 backends/null
  → 无返回值（日志失败不向调用方报告）
```

配置与状态：工程版本的单一来源是 `include/CLV_Version.h` 的四个宏，`src/version.cpp` 与 CMake `project(... VERSION ...)` 都从它取；构建期配置（后端、平台）不进运行期状态，运行期只有日志等级过滤一个可变量。

---

## 4. 构建与测试

```bash
python script/install_third_party.py                       # third-party 源码 -> build/_install
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug      # 必须在 vcvars64 的同一个进程里
cmake --build build
ctest --test-dir build --output-on-failure
python script/format_all.py                                # C/C++ 走 .clang-format，Python 走 black
```

- 构建流程：`cmake/sources.cmake` 列平台与后端无关的源文件；`cmake/platform/<平台>.cmake` 与 `cmake/backends/<后端>.cmake` 各补自己那份文件清单（只列文件，判定在根 `CMakeLists.txt`）；`move_compile_db` 目标把 `compile_commands.json` 从 `build/` 移到仓库根供 clangd 读。
- 生成器固定 Ninja：Visual Studio 生成器是多配置，且缺省平台 win32 会与 64 位的第三方库打架。
- 第三方接入两条路：`find_package`（用 `build/_install`）优先，失败回退源码 `add_subdirectory`。
- 测试：`tests/` 编成单一可执行 `clv_tests`，用 GTest 但自带 `main`，用例经 `gtest_discover_tests` 注册进 CTest；测试只吃公开 C ABI，不 include 实现层头。

| 套件 | 领域 | 例数 |
|---|---|---|
| `core` | 路径与模式判定、CRC、varint、内存、版本 | 15 |
| `api` | 五个公开头的端到端行为（打开 / 读写 / 游标 / 门控 / 错误码 / 日志等级） | 35 |

`CLV_LOGGER_USE_SPDLOG` 的 ON 与 OFF 两种配置各跑一遍同一套用例。

---

## 5. 文档索引

| 文档 | 说明 |
|---|---|
| `README.md` | 使用者入口：安装、快速开始、公开接口、使用约定、构建与验收 |
| `ARCHITECTURE.md` | 本文件：分层、依赖、数据流、构建组织、现状 |
| `CHANGE.log` | 版本演进与兼容声明 |
| `include/CLV_*.h` | 接口契约正文（头注释即规范） |
| `demo/README.md` | 示例库与验收步骤 |
| `AI编写文档通用模板.md` 等三份 | 协作方编写文档的模板与红线 |

---

## 6. 现状

**已实现**：C ABI 公开面五头；文件读写（三模式、1 MiB 分块、UTF-8 路径契约）；日志（spdlog 与空后端、等级过滤）；内存分配口；版本查询；实现层工具 CRC-32/MPEG-2 与无符号 LEB128；Ninja + x64 构建与 50 例 CTest。

**未落地 / 占位**：

| 项 | 状态 |
|---|---|
| 容器解析、视频 / 音频编解码、字幕格式 | 未开工 |
| `CLV_OpenMemoryFile`（内存缓冲区后端） | 接口占位：有声明有定义，返回 `CLV_FILE_UNSUPPORTED` |
| `CLV_LogSetStorage`（日志落盘） | 接口占位：调用无效果 |
| 宿主注入后端与分配器路由 | 未做：后端由编译期选源，`CLV_Memory` 直通 C 运行时，库内部不经它 |
| 能力查询（当前生效后端 / 是否降级） | 未做：C ABI 上问不出后端状态 |
| 对齐分配 | 未加，等 SIMD 侧有需求再扩 |
| iOS | `platform.h` 留检测分支，选源处 `#error` 占位 |
| Linux / macOS 后端 | 代码按 POSIX 语义写好，**未经非 Windows 平台编译验证**，不作为可用承诺 |
| `CLV_BUILD_SHARED` | 开关存在但从未构建过，dllexport 路径与跨 DLL 释放未实测 |
| zigzag / 跨编译器 128 位乘除 | 容器层需要，尚未落地 |
