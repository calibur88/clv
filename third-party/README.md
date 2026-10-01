# third-party

第三方**源码**存放处（纳入版本库）。`build/` 只放产物与构建中间物。

| 库 | 版本 | 许可证 | 说明 |
|---|---|---|---|
| spdlog | v1.17.0 | MIT | GitHub `gabime/spdlog` Releases tag v1.17.0；解包一层去壳，无嵌套 `.git` |
| googletest | v1.18.0 | BSD-3-Clause | GitHub `google/googletest` Releases tag v1.18.0；同上 |

各自的 `LICENSE` 文件保持原样随源码存放。

## 构建流程（谁在哪儿）

~~~
third-party/<lib>            源码（追踪）
   │ cmake -S
   ▼
build/<lib>-build            该库的构建中间物（不追踪）
   │ cmake --install
   ▼
build/_install               安装前缀：头 + 库 + <pkg>Config.cmake（不追踪）
   │ 顶层 CMakeLists 把 build/_install 追加进 CMAKE_PREFIX_PATH
   ▼
find_package(spdlog REQUIRED) / find_package(GTest CONFIG REQUIRED)
~~~

一条命令装第三方：`python script/install_third_party.py`（已装则跳过，`--force` 重装）。

## 为什么用 find_package 而不是 add_subdirectory

`find_package(CONFIG)` 让第三方在自己的构建树里编译，**不继承 CLV 的编译选项与警告开关**，也不会把它们的安装/导出规则混进本项目的目标图；同时保证 `CMAKE_CXX_STANDARD 17` 只在 CLV 侧生效（spdlog 在其 `CMakeLists.txt` 中于标准未定义时会把自己设成 C++20，顶层必须先 cache 锁定 17）。

若将来决定改为源码内置（`add_subdirectory(third-party/spdlog)`），顺序必须是：先 `set(CMAKE_CXX_STANDARD 17 CACHE STRING "" FORCE)`，再 `add_subdirectory`。
