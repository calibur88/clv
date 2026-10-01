cmake_minimum_required(VERSION 3.16)

project(clv VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CLV_BUILD_SHARED "Build CLV as a shared library" OFF)
option(CLV_LOGGER_USE_SPDLOG "Use the spdlog logger backend instead of the null one" OFF)
option(CLV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors for our own targets" OFF)
option(CLV_BUILD_DEMOS "Build the demo programs under demo/" OFF)

enable_testing()

# 编译警告与源文件编码。只作用于本仓库自己的 target，因此做成函数逐个施加，
# 不用目录级 add_compile_options（那样会波及 add_subdirectory 进来的第三方）。
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
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR}/_install)

if(CLV_LOGGER_USE_SPDLOG)
    find_package(spdlog QUIET)
    if(NOT spdlog_FOUND)
        set(SPDLOG_INSTALL OFF)
        add_subdirectory(third-party/spdlog third-party/spdlog EXCLUDE_FROM_ALL)
    endif()
endif()

# ---------- 文件清单 ----------
set(CLV_SOURCES "")
include(cmake/sources.cmake)

# ---------- 平台主判：唯一一处 CMake 平台判定 ----------
# 判编译器目标，同一个 if 块里产出两个动作：传宏 + 加载对应文件清单。
# 源文件侧 platform/platform.h 有同形兜底（没有 CMake 传宏时按裸宏推导），两者互斥。
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

add_library(clv ${CLV_SOURCES})

target_include_directories(clv
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(clv PRIVATE ${CLV_PLATFORM_DEFINE})
clv_apply_target_flags(clv)

if(CLV_BUILD_SHARED)
    target_compile_definitions(clv PRIVATE CLV_BUILD_SHARED)
    # 使用侧（含本仓库的测试）拿 dllimport 声明，避免自己再定一遍
    target_compile_definitions(clv INTERFACE $<BUILD_INTERFACE:CLV_USE_SHARED>)
    set_target_properties(clv PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# 后端选择属业务配置，与平台链路无关
if(CLV_LOGGER_USE_SPDLOG)
    target_compile_definitions(clv PRIVATE CLV_LOGGER_USE_SPDLOG)
    target_link_libraries(clv PRIVATE spdlog::spdlog)
endif()

add_subdirectory(tests)

# 编译数据库移动到仓库根（clangd 读根那一份；构建目录不留副本）
include(cmake/move_compile_db.cmake)
clv_add_move_compile_db_target()

if(CLV_BUILD_DEMOS)
    add_subdirectory(demo)
endif()
