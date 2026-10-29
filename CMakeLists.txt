cmake_minimum_required(VERSION 3.16)

project(clv VERSION 0.9.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CLV_BUILD_SHARED "Build CLV as a shared library" OFF)
option(CLV_LOGGER_USE_SPDLOG "Use the spdlog logger backend instead of the null one" OFF)
option(CLV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors for our own targets" OFF)
option(CLV_BUILD_DEMOS "Build the demo programs under demo/" OFF)
option(CLV_BUILD_SUBTITLE_RENDERER "Build the subtitle renderer (needs the text stack)" ON)
if(CLV_BUILD_SUBTITLE_RENDERER)
    add_compile_definitions(CLV_SUBTITLE_RENDERER_ENABLED=1)
else()
    add_compile_definitions(CLV_SUBTITLE_RENDERER_ENABLED=0)
endif()

# 系数编码法：编码器写出哪一法。0 = 定宽直写（零也算载荷），1 = EOB 截尾 + Exp-Golomb 幅值。
# 这一档必须按「仓」定而不是按「target」定：块层的编码器侧取的是编译期常量，
# 库与 tests / demo 的每个 TU 都要看到同一个值，两侧分岔就会编出与使用侧预期不同的流。
set(CLV_COEF_CODING "1" CACHE STRING "Coefficient coding the encoder writes: 0 = fixed width, 1 = EOB + Exp-Golomb")
if(NOT CLV_COEF_CODING MATCHES "^[01]$")
    message(FATAL_ERROR "CLV: CLV_COEF_CODING 只认 0（定宽）或 1（EOB+EG），当前为 '${CLV_COEF_CODING}'")
endif()
add_compile_definitions(CLV_COEF_CODING=${CLV_COEF_CODING})

# 帧内分带的并行度：编译期常量，与 CLV_COEF_CODING 同款口径（仓域 add_compile_definitions ⇒
# 库、测试、示例看到同一个值）。分带是读方内部实现，不进码流、不进公开入参，
# 也不接受环境变量改行为 —— 出厂形态必须由构建面确定。1 = 不切带。
set(CLV_DEC_MB_THREADS "16" CACHE STRING "Macroblock-band decode parallelism baked into the library (1 = serial; 16 = 出厂缺省)")
if(NOT CLV_DEC_MB_THREADS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "CLV: CLV_DEC_MB_THREADS 必须是 ≥ 1 的十进制整数（1 = 不切带），当前为 '${CLV_DEC_MB_THREADS}'")
endif()
add_compile_definitions(CLV_DEC_MB_THREADS=${CLV_DEC_MB_THREADS})

# 全量 ctest 的期望注册例数。这一格由 cmake/check_registered_tests.cmake 在每次构建里核对，
# 数不到这个值就构建失败（挂点见 add_subdirectory(tests) 之后那段）。
# 判读口径：数的是**注册数**（`ctest -N` 报出的条数），不是通过数 —— 一条例跑失败或被判跳过都不改它，
# 只有用例增删、或测试目标没建起来才会让它动；换构型（含 CLV_COEF_CODING 两档、Debug/Release、ASan）不改这个数字。
# 例数变了就同批改这一格与 `tests/docs/` 的对外计数。
set(CLV_EXPECTED_TESTS 431)
message(STATUS "CLV: 期望 ctest 注册例数 = ${CLV_EXPECTED_TESTS}")

enable_testing()

# CRT 一致性：MSVC 下全仓库（含 tests/ 拉进来的 gtest 与 demo 产物）锁动态 CRT，
# Debug 用 /MDd 系、其余配置用 /MD 系，不留给默认值按配置浮动。
# tests/CMakeLists.txt 的 gtest_force_shared_crt=ON 与这里的 Debug/DLL 取值是一对，改任一侧都要核另一侧。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# 编译警告与源文件编码，只作用于本仓库自己的 target：
# 用函数逐个施加，不用目录级 add_compile_options（第三方 target 不继承这些开关）。
# 本仓库的中文注释一律是无 BOM 的 UTF-8，编码由编译器开关指定，不靠文件带 BOM。
function(clv_apply_target_flags target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /permissive-)
        if(CLV_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -finput-charset=UTF-8 -Wall -Wextra)
        if(CLV_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ---------- 第三方：装了就用包，没装就吃源码 ----------
# 第三方只装进主构建目录的 _install（scripts/install_third_party.py 的固定落点），
# 双配置目录（build/offcfg 等）共用同一份，故两个前缀都挂上。
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR}/_install ${CMAKE_SOURCE_DIR}/build/_install)

if(CLV_LOGGER_USE_SPDLOG)
    find_package(spdlog QUIET)
    if(NOT spdlog_FOUND)
        set(SPDLOG_INSTALL OFF)
        add_subdirectory(third-party/spdlog third-party/spdlog EXCLUDE_FROM_ALL)
    endif()
endif()

# libFLAC 是音频层的硬依赖：编解码闭环直接链它，定位不到即配置失败。
find_package(FLAC QUIET)
# 直接定位路的候选前缀按顺序试：跟着 -B 走的「当前构建目录/_install」优先，
# 再试 scripts/install_third_party.py 的固定落点 build/_install。
# 只认后者时，换个构建目录配置（-B build/xxx 之类）就定位不到，故两条都挂。
foreach(CLV_FLAC_PREFIX IN ITEMS ${CMAKE_BINARY_DIR}/_install ${CMAKE_SOURCE_DIR}/build/_install)
    if(NOT FLAC_FOUND AND EXISTS ${CLV_FLAC_PREFIX}/include/FLAC/stream_encoder.h)
        # vendored 安装的 FLAC config 包会无条件 find_dependency(Ogg)，而本仓库不构建 ogg
        # （install_third_party.py 只装 libFLAC）。这条退回直接定位头与库，自建导入目标。
        # 每个前缀都 unset 后重查：缓存会把两个前缀的结果混成一对。
        unset(CLV_FLAC_INCLUDE_DIR CACHE)
        unset(CLV_FLAC_LIBRARY CACHE)
        find_path(CLV_FLAC_INCLUDE_DIR NAMES FLAC/stream_encoder.h
                  PATHS ${CLV_FLAC_PREFIX}/include)
        find_library(CLV_FLAC_LIBRARY NAMES FLAC libFLAC
                     PATHS ${CLV_FLAC_PREFIX}/lib)
        if(CLV_FLAC_INCLUDE_DIR AND CLV_FLAC_LIBRARY AND NOT TARGET FLAC::FLAC)
            add_library(FLAC::FLAC UNKNOWN IMPORTED)
            set_target_properties(FLAC::FLAC PROPERTIES
                IMPORTED_LOCATION "${CLV_FLAC_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${CLV_FLAC_INCLUDE_DIR}"
                # FLAC.lib 是静态件；export.h 只在定义了 FLAC__NO_DLL 时才把 FLAC_API
                # 退成空修饰，否则按 dllimport 走，链接期全是 __imp_FLAC__* 未解析。
                INTERFACE_COMPILE_DEFINITIONS "FLAC__NO_DLL")
            set(FLAC_FOUND TRUE)
            message(STATUS "CLV: FLAC 走直接定位（config 包要求 Ogg，本仓库不建 ogg）: ${CLV_FLAC_LIBRARY}")
        endif()
    endif()
endforeach()
if(NOT FLAC_FOUND)
    # libFLAC 定位的唯一路 = 上面的 find_path + find_library 自建导入目标。
    # 不补建 ogg（上游 config 包要求的 Ogg 不是本仓库的依赖）、
    # 不把 vendored 源码放回 third-party/flac（该目录不参与构建）。
    # 换路须同步改 README.md 构建与验收节的「路线锁定」。
    message(FATAL_ERROR
        "CLV: 找不到 libFLAC —— 自建导入目标未生效。先跑 scripts/install_third_party.py 把 FLAC 装进 build/_install，"
        "再确认 build/_install/include/FLAC/stream_encoder.h 与 build/_install/lib/FLAC.lib 都在。"
        "不要改判定逻辑去换路（见 README.md 构建与验收节「路线锁定」）。")
endif()

# 字幕文本栈（simdutf / utf8proc / SheenBidi / HarfBuzz / FreeType）：CAL 与字体解析的硬依赖。
# 只被 backends/text 与 platform/font 包含，故在库函数里 PRIVATE 链接；使用侧不需要自己找。
# 头与库都是静态件，装在 install_third_party.py 的固定落点 _install 下。
set(CLV_TEXT_PREFIX "")
foreach(CLV_PREFIX IN ITEMS ${CMAKE_BINARY_DIR}/_install ${CMAKE_SOURCE_DIR}/build/_install)
    if(EXISTS ${CLV_PREFIX}/include/simdutf.h AND EXISTS ${CLV_PREFIX}/include/harfbuzz/hb.h
       AND EXISTS ${CLV_PREFIX}/include/freetype2/ft2build.h
       AND EXISTS ${CLV_PREFIX}/include/SheenBidi/SheenBidi.h)
        set(CLV_TEXT_PREFIX ${CLV_PREFIX})
        break()
    endif()
endforeach()
if(NOT CLV_TEXT_PREFIX)
    message(FATAL_ERROR
        "CLV: 找不到字幕文本栈（simdutf / utf8proc / SheenBidi / HarfBuzz / FreeType）。"
        "先跑 scripts/install_third_party.py 把它们装进 build/_install，"
        "再确认 include/simdutf.h、include/harfbuzz/hb.h、include/freetype2/ft2build.h、"
        "include/SheenBidi/SheenBidi.h 都在。")
endif()
find_library(CLV_SIMDUTF_LIBRARY NAMES simdutf PATHS ${CLV_TEXT_PREFIX}/lib NO_DEFAULT_PATH)
find_library(CLV_UTF8PROC_LIBRARY NAMES utf8proc_static utf8proc PATHS ${CLV_TEXT_PREFIX}/lib NO_DEFAULT_PATH)
find_library(CLV_SHEENBIDI_LIBRARY NAMES SheenBidi PATHS ${CLV_TEXT_PREFIX}/lib NO_DEFAULT_PATH)
find_library(CLV_HARFBUZZ_LIBRARY NAMES harfbuzz PATHS ${CLV_TEXT_PREFIX}/lib NO_DEFAULT_PATH)
# 调试构型装出来的是 freetyped / libpng16_staticd / zlibstaticd，发布构型没有 d 尾；两种名都试。
find_library(CLV_FREETYPE_LIBRARY NAMES freetyped freetype PATHS ${CLV_TEXT_PREFIX}/lib NO_DEFAULT_PATH)
find_library(CLV_PNG_LIBRARY NAMES libpng16_staticd libpng16_static libpng16
             PATHS ${CLV_TEXT_PREFIX}/lib NO_DEFAULT_PATH)
find_library(CLV_ZLIB_LIBRARY NAMES zlibstaticd zlibstatic zlib PATHS ${CLV_TEXT_PREFIX}/lib NO_DEFAULT_PATH)
foreach(CLV_TEXT_VAR IN ITEMS CLV_SIMDUTF_LIBRARY CLV_UTF8PROC_LIBRARY CLV_SHEENBIDI_LIBRARY
        CLV_HARFBUZZ_LIBRARY CLV_FREETYPE_LIBRARY CLV_PNG_LIBRARY CLV_ZLIB_LIBRARY)
    if(NOT ${CLV_TEXT_VAR})
        message(FATAL_ERROR "CLV: 文本栈缺件 ${CLV_TEXT_VAR}（前缀 ${CLV_TEXT_PREFIX}）")
    endif()
endforeach()
set(CLV_TEXT_INCLUDES
    ${CLV_TEXT_PREFIX}/include
    ${CLV_TEXT_PREFIX}/include/harfbuzz
    ${CLV_TEXT_PREFIX}/include/freetype2
    ${CLV_TEXT_PREFIX}/include/SheenBidi)
# HarfBuzz 链 FreeType；FreeType 开了 PNG（彩色 emoji），故再链 libpng 与 zlib。
set(CLV_TEXT_LIBS
    ${CLV_HARFBUZZ_LIBRARY}
    ${CLV_FREETYPE_LIBRARY}
    ${CLV_PNG_LIBRARY}
    ${CLV_ZLIB_LIBRARY}
    ${CLV_SIMDUTF_LIBRARY}
    ${CLV_UTF8PROC_LIBRARY}
    ${CLV_SHEENBIDI_LIBRARY})

# SDL3 只被 demo 播放器用，库本身不依赖它：不开 CLV_BUILD_DEMOS 就不去找。
if(CLV_BUILD_DEMOS)
    find_package(SDL3 QUIET)
    if(NOT SDL3_FOUND)
        # 两份播放器示例（demo/video.c 与 demo/subtitle_player.c）链 SDL3::SDL3 —— 有 target 消费它之后，缺它就判配置失败，
        # 不降级成 WARNING：降级会让缺依赖的配置照样过，到编译 demo 时才炸，报的还是链接错。
        message(FATAL_ERROR
            "CLV: 开了 CLV_BUILD_DEMOS 但找不到 SDL3 —— 两份播放器示例（demo/video.c 与 demo/subtitle_player.c）的显示腿与音频腿都要它。"
            "先跑 scripts/install_third_party.py 把它装进 build/_install（找到的应是 "
            "build/_install/cmake/SDL3Config.cmake），再配置。库本体不依赖 SDL3：只要 ctest 就关掉本开关。")
    endif()
endif()

# ---------- 文件清单 ----------
set(CLV_SOURCES "")
include(cmake/sources.cmake)

# ---------- 平台主判：根件内唯一一处 CMake 平台判定 ----------
# 判编译器目标，同一个 if 块里产出两个动作：传宏 + 加载对应文件清单。
# 源文件侧 platform/platform.h 有同形兜底（没有 CMake 传宏时按裸宏推导），两者互斥。
# tests/CMakeLists.txt 另有一处同形判定（只分派测试用例清单），两处判的是各自的清单，改平台分支要一起核。
if(WIN32)
    set(CLV_PLATFORM_DEFINE CLV_PLATFORM_WINDOWS=1)
    include(cmake/platform/win32.cmake)
elseif(APPLE)
    set(CLV_PLATFORM_DEFINE CLV_PLATFORM_MACOS=1)
    include(cmake/platform/macos.cmake)
elseif(UNIX)
    set(CLV_PLATFORM_DEFINE CLV_PLATFORM_LINUX=1)
    include(cmake/platform/linux.cmake)
else()
    message(FATAL_ERROR "CLV: unsupported platform")
endif()
# -------------------------------------------------

# ---------- 后端文件清单 ----------
# 两套后端都挂上，是否真编译由源文件自守卫筛（CLV_LOGGER_USE_SPDLOG）；
# 这里与 cmake/platform/*.cmake 同构：只列文件，不做判断。
include(cmake/backends/spdlog.cmake)
include(cmake/backends/null.cmake)

# ---------- 库目标：静态常备，动态按开关 ----------
# 两个目标是同一份源的两条产出路，公共配置收进函数，避免两边各写一遍再漂移。
#
# clv_static 一直在，且是唯一给 tests 用的那条：测试吃 src 下的实现层头与不带导出宏的内部
# 钩子（见 src/video/encoder_hook.h），那条路必须是静态件。
# clv（SHARED）只在 CLV_BUILD_SHARED=ON 时产出；两目标输出名天然错开
# （clv_static.lib 与 clv.dll + 导入库 clv.lib），实测两件并存不互撞。
function(clv_configure_library_target target)
    target_include_directories(${target}
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(${target} PRIVATE ${CLV_PLATFORM_DEFINE})
    clv_apply_target_flags(${target})
    # 后端选择属业务配置，与平台链路无关
    if(CLV_LOGGER_USE_SPDLOG)
        target_compile_definitions(${target} PRIVATE CLV_LOGGER_USE_SPDLOG)
        target_link_libraries(${target} PRIVATE spdlog::spdlog)
    endif()
    # 音频编解码闭环的后端：libFLAC 只被 backends/flac/flac_codec.cpp 一个文件包含，
    # 所以是 PRIVATE 链接 —— 使用侧不需要一起找 FLAC。
    target_link_libraries(${target} PRIVATE FLAC::FLAC)
    # 字幕文本栈：同样只有 backends/text 与 platform/font 包含，PRIVATE 链接。
    target_include_directories(${target} PRIVATE ${CLV_TEXT_INCLUDES})
    target_compile_definitions(${target} PRIVATE SB_CONFIG_EXPERIMENTAL_TEXT_API=1 UTF8PROC_STATIC)
    target_link_libraries(${target} PRIVATE ${CLV_TEXT_LIBS})
    if(WIN32)
        # DirectWrite 系统字体解析
        target_link_libraries(${target} PRIVATE dwrite ole32)
    endif()
endfunction()

add_library(clv_static STATIC ${CLV_SOURCES})
clv_configure_library_target(clv_static)

if(CLV_BUILD_SHARED)
    add_library(clv SHARED ${CLV_SOURCES})
    clv_configure_library_target(clv)
    # 导出宏 CLV_EXPORT 的两侧都随目标走：构建侧出 dllexport / 可见性默认，
    # 使用侧出 dllimport ⇒ 消费者不需要自己定义任何宏，链哪个目标就拿哪个声明。
    target_compile_definitions(clv PRIVATE CLV_BUILD_DLL)
    target_compile_definitions(clv INTERFACE $<BUILD_INTERFACE:CLV_USE_DLL>)
    if(NOT WIN32)
        # 默认隐藏 + CLV_EXPORT 显式放行，公开面才是真的那一圈符号
        target_compile_options(clv PRIVATE -fvisibility=hidden)
    endif()
    set_target_properties(clv PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

add_subdirectory(tests)

# ---------- 注册例数闸：把上面那一格接到构建上 ----------
# gtest 的注册发生在构建期：gtest_discover_tests 跑一遍测试可执行文件列用例，再把清单写进
# tests 的构建目录，配置阶段数不出来 —— 所以这道闸做成构建步骤，排在测试目标之后。
# 只吃 cmake -P：不依赖本机脚本解释器、不写绝对路径，换构型（含 CLV_COEF_CODING 两档）跑的是同一条比较。
# 闸自己不往 ctest 挂注册，于是 `ctest -N` 的条数与 CLV_EXPECTED_TESTS 那一格同值；
# 数的是注册数不是通过数，不等就是 FATAL_ERROR，构建失败。
if(TARGET clv_tests)
    # 注册清单的落点 = add_subdirectory(tests) 默认开出来的那个子目录构建目录
    set(CLV_TESTS_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/tests)
    add_custom_target(clv_check_registered_tests ALL
        COMMAND ${CMAKE_COMMAND}
                -DCLV_EXPECTED_TESTS=${CLV_EXPECTED_TESTS}
                -DCLV_TESTS_BINARY_DIR=${CLV_TESTS_BINARY_DIR}
                -DCLV_TESTS_TARGET=clv_tests
                -P ${CMAKE_SOURCE_DIR}/cmake/check_registered_tests.cmake
        DEPENDS clv_tests
        COMMENT "clv: 核对 ctest 注册例数 = ${CLV_EXPECTED_TESTS}"
        VERBATIM)
endif()

# 编译数据库移动到仓库根（clangd 读根那一份；构建目录不留副本）
include(cmake/move_compile_db.cmake)
clv_add_move_compile_db_target()

if(CLV_BUILD_DEMOS)
    add_subdirectory(demo)
endif()
