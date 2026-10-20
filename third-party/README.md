# third-party

第三方**源码**存放处：spdlog 与 googletest 纳入版本库，SDL3 与 flac 由本地拉取、被 `.gitignore` 排除。`build/` 只放产物与构建中间物。

| 库 | 版本 | 许可证 | 说明 |
|---|---|---|---|
| spdlog | v1.17.0 | MIT | GitHub `gabime/spdlog` Releases tag v1.17.0；解包一层去壳，无嵌套 `.git` |
| googletest | v1.18.0 | BSD-3-Clause | GitHub `google/googletest` Releases tag v1.18.0；同上 |

各自的 `LICENSE` 文件保持原样随源码存放。

## 构建流程（谁在哪儿）

~~~
third-party/<lib>            源码（spdlog / googletest 在版本库里，SDL3 / flac 本地拉取）
   │ cmake -S
   ▼
build/<lib>-build            该库的构建中间物（不追踪）
   │ cmake --install
   ▼
build/_install               安装前缀：头 + 库 + <pkg>Config.cmake（不追踪）
   │ 顶层 CMakeLists 把 build/_install 追加进 CMAKE_PREFIX_PATH
   ▼
find_package(<pkg> QUIET) —— 命中就用安装树里的包，未命中走下面的退回路径
~~~

一条命令装第三方：`python script/install_third_party.py`（已装则跳过，`--force` 重装）。

## 包模式优先，源码内建或直接定位作退回

包模式命中时第三方在自己的构建树里编译，**不继承 CLV 的编译选项与警告开关**，也不会把它们的安装/导出规则混进本项目的目标图；CLV 侧的告警开关由 `clv_apply_target_flags` 逐个 target 施加，不用目录级 `add_compile_options`。

退回路径：spdlog 与 googletest 未命中时 `add_subdirectory(third-party/<lib> … EXCLUDE_FROM_ALL)` 源码内建——顶层在引入第三方之前已 `set(CMAKE_CXX_STANDARD 17)`，spdlog 的标准未定义分支（`elseif(NOT CMAKE_CXX_STANDARD)`，设成 C++11；开 `SPDLOG_USE_STD_FORMAT` 才设 C++20）因此不触发。FLAC 未命中时直接 `find_path` + `find_library` 自建 `FLAC::FLAC` 导入目标。SDL3 没有退回路径，只在 `CLV_BUILD_DEMOS=ON` 时需要。
