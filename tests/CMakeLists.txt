# tests/CMakeLists.txt —— 测试套（被根 CMakeLists add_subdirectory 纳入）
#
# 结构（按面分目录）：
#   tests/win32_main.cpp                 —— Windows 测试主入口（gtest + CRT 无人值守守卫）
#   tests/win32/<域>/core|container|audio|video|independent|api/*.cpp *.h
#   tests/linux/ tests/macos/            —— 平台目录，暂无用例
#   tests/docs/                          —— 测试项文档（一文件一项）+ README 索引
#
# 域口径（测的是哪一层，不按能不能碰 ABI 分）：
#   api         —— 只吃公开 C ABI，不 include 实现层头
#   core        —— 直测实现层内部工具（CRC-32/MPEG-2、LEB128），它们不出 ABI
#   container   —— 两面混吃：结构编解码走实现层，句柄端到端走公开面
#   audio / video —— 两面混吃：句柄端到端走公开面，单元级直测走 src 下的头
#   independent —— 期望值不取自被测实现的独立验证层，两面混吃
# 因此 clv_tests 必须拿到 ${CMAKE_SOURCE_DIR}/src 的包含路径。
#
# 恒链静态件 clv_static：本域吃 src 下的实现层头与不带导出宏的内部钩子
# （见 src/video/encoder_hook.h），那条路必须是静态件（动态那条的验收面在 demo）。

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF)
    add_subdirectory(${CMAKE_SOURCE_DIR}/third-party/googletest third-party/googletest EXCLUDE_FROM_ALL)
endif()

# ---------- 平台分派：与根 cmake/platform/*.cmake 同构（判定在此，清单在各平台件里只列文件） ----------
set(CLV_TEST_SOURCES "")
if(WIN32)
    include(win32/sources.cmake)
elseif(UNIX AND NOT APPLE)
    # include(linux/sources.cmake)   —— linux/ 暂无用例，接入时建该件并放开本行
elseif(APPLE)
    # include(macos/sources.cmake)   —— macos/ 暂无用例，接入时建该件并放开本行
endif()

if(CLV_TEST_SOURCES STREQUAL "")
    message(STATUS "CLV tests: 本平台（${CMAKE_SYSTEM_NAME}）暂无测试用例，跳过 clv_tests 目标")
    return()
endif()

add_executable(clv_tests ${CLV_TEST_SOURCES})

target_link_libraries(clv_tests PRIVATE clv_static GTest::gtest)
# CRC / varint / 容器结构编解码是实现层件，不经 C ABI 导出 ⇒ 单测直接吃 src 下的头。
target_include_directories(clv_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
# 入库素材的绝对路径（正斜杠，符合库的路径契约）；test_fixture_files.cpp 直接吃这三份文件。
target_compile_definitions(clv_tests PRIVATE CLV_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets/fixtures")
clv_apply_target_flags(clv_tests)

include(GoogleTest)
# 逐例超时统一 300 s 防挂死；满片档（65 535 片、载荷约 2.9e7 B）耗时最长，其余例在一秒内。
gtest_discover_tests(clv_tests PROPERTIES TIMEOUT 300)
