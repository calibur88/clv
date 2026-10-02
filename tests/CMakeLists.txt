# 单测：只走公开 C ABI，与宿主同一张脸。第三方依赖不外泄给测试。

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF)
    add_subdirectory(${CMAKE_SOURCE_DIR}/third-party/googletest third-party/googletest EXCLUDE_FROM_ALL)
endif()

add_executable(clv_tests
    main.cpp
    core/test_crc32.cpp
    core/test_varint.cpp
    core/test_varint_signed.cpp
    container/test_packet_frames.cpp
    container/test_container_io.cpp
    api/test_file.cpp
    api/test_logger.cpp
    api/test_memory.cpp
    api/test_version.cpp
    api/test_container.cpp
)

target_link_libraries(clv_tests PRIVATE clv GTest::gtest)
# crc32 / varint / container 是实现层件，不经 C ABI 导出，单测直接吃 src 下的头
target_include_directories(clv_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
clv_apply_target_flags(clv_tests)

include(GoogleTest)
gtest_discover_tests(clv_tests)
