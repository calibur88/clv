# CLV 测试套件

## 目录结构

```
tests/
  docs/                  测试项文档（一文件一项）+ 本索引
  win32_main.cpp         Windows 测试主入口（gtest + CRT 无人值守守卫）
  win32/                 Windows 平台测试源码，按面分目录
    core/ container/ audio/ video/ independent/ api/
  linux/                 Linux 平台测试（暂无用例）
  macos/                 macOS 平台测试（暂无用例）
  CMakeLists.txt         测试构建（由根 CMakeLists add_subdirectory 纳入）
```

## 测试项总表

| 测试项 | 面 | 例数 | 平台 | 现状 | 文档 |
|---|---|---|---|---|---|
| 核心工具（varint/CRC-32） | core | 20 | Win32 | 通过 | [核心工具.md](核心工具.md) |
| 容器格式（布局/读写/分片/索引/分段） | container | 77 | Win32 | 通过 | [容器格式.md](容器格式.md) |
| 音频帧与编解码 | audio | 39 | Win32 | 通过 | [音频帧.md](音频帧.md) |
| 视频编解码（含判据 1–4） | video | 142 | Win32 | 通过（真实素材档无素材时 Skip） | [视频编解码.md](视频编解码.md) |
| 独立验证层（梯子+反向对照） | independent | 21 | Win32 | 通过 | [独立验证层.md](独立验证层.md) |
| 公开 ABI 面 | api | 91 | Win32 | 通过 | [公开ABI面.md](公开ABI面.md) |
| 门禁（全量+双配置+探针） | gate | — | Win32/MSVC+Ninja | 见文档 | [门禁.md](门禁.md) |
| **合计** | | **390** | | | |

## 怎么跑

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug   # Windows 须在 vcvars64 同进程
cmake --build build --target clv_tests
ctest --test-dir build
```

## 例数口径

合计与分域都是 ctest 的**注册数**，按域逐件现数再相加（相加必须等于注册数，对不上就是数法错了）：
谓词 `^[[:space:]]*TEST\(` 或 `^[[:space:]]*TEST_F\(` —— 只认顶格的 `TEST(` 会漏掉带缩进的和命名空间里的
用例，`TEST_F` 那一族（节器件 16 例）不数就少一截。改例数时这一张表与 `门禁.md`、`CMakeLists.txt` 的
`CLV_EXPECTED_TESTS` 必须同批跟着实测走。
