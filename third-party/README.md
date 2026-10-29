# third-party

第三方**源码**存放处：spdlog 与 googletest 纳入版本库，SDL3、flac、字幕层的文本栈五件、以及彩色位图解码链的 zlib 与 libpng 由本地拉取、被 `.gitignore` 排除。`build/` 只放产物与构建中间物。

| 库 | 版本 | 许可证 | 说明 |
|---|---|---|---|
| spdlog | v1.17.0 | MIT | GitHub `gabime/spdlog` Releases tag v1.17.0；解包一层去壳，无嵌套 `.git` |
| googletest | v1.18.0 | BSD-3-Clause | GitHub `google/googletest` Releases tag v1.18.0；同上 |
| simdutf | 9.1.2 | MIT | 字幕 CAL：整包 UTF-8 验证 |
| utf8proc | 2.12.0 | MIT | 字幕 CAL：NFC / 码点分类（Unicode 18.0.0） |
| SheenBidi | 3.0.0 | Apache-2.0 | 字幕 CAL：UAX #9 双向文本 |
| HarfBuzz | 14.4.0 | MIT | 字幕 CAL：文本整形 |
| FreeType | 2.14.3 | FTL | 字幕 CAL：face / 字形栅格 |
| zlib | 1.3.1 | zlib（BSD 风格） | 彩色 emoji 解码链：libpng 的依赖（SourceForge / GitHub 快照，本地拉取） |
| libpng | 1.6.44 | libpng 许可（MIT 风格，含 PNG 参考实现声明） | 彩色 emoji 解码链：FreeType `FT_CONFIG_OPTION_USE_PNG` 的提供方 |

各自的 `LICENSE` / `COPYING` 文件保持原样随源码存放；安装时由脚本另投一份到 `build/_install/licenses/<pkg>/`。

## 构建流程（谁在哪儿）

~~~text
third-party/<lib>            源码（spdlog / googletest 在版本库里，SDL3 / flac / 文本栈五件 / zlib / libpng 本地拉取）
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

一条命令装第三方：`python scripts/install_third_party.py`（已装则跳过，`--force` 重装）。

文本栈五件当前只为字幕层准备：安装脚本会把它们装进 `build/_install`，但库本体与测试还没有 `find_package` / 链接它们；等字幕层进主树时再接消费者。

## 包模式优先，源码内建或直接定位作退回

包模式命中时第三方在自己的构建树里编译，**不继承 CLV 的编译选项与警告开关**，也不会把它们的安装/导出规则混进本项目的目标图；CLV 侧的告警开关由 `clv_apply_target_flags` 逐个 target 施加，不用目录级 `add_compile_options`。

退回路径：spdlog 与 googletest 未命中时 `add_subdirectory(third-party/<lib> … EXCLUDE_FROM_ALL)` 源码内建——顶层在引入第三方之前已 `set(CMAKE_CXX_STANDARD 17)`，spdlog 的标准未定义分支（`elseif(NOT CMAKE_CXX_STANDARD)`，设成 C++11；开 `SPDLOG_USE_STD_FORMAT` 才设 C++20）因此不触发。FLAC 未命中时直接 `find_path` + `find_library` 自建 `FLAC::FLAC` 导入目标。SDL3 没有退回路径，只在 `CLV_BUILD_DEMOS=ON` 时需要。
