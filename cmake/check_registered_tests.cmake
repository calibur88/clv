# 核对 ctest 的注册例数与账面期望值是否相等：不等就失败退出，构建当场红。
#
# 由根 CMakeLists.txt 挂在测试目标之后的一条构建步骤调用（配置阶段数不出来：注册清单是构建期
# 由测试可执行文件把用例列一遍再生成的）：
#   cmake -DCLV_EXPECTED_TESTS=<期望注册数> -DCLV_TESTS_BINARY_DIR=<tests 的构建目录>
#         -DCLV_TESTS_TARGET=<测试目标名> -P cmake/check_registered_tests.cmake
#
# 数的是注册数（ctest 报的那一圈用例条目），不是通过数：一条例跑失败、被判跳过都照样在清单里，
# 只有用例增删（或发现口径变了、测试目标没建起来）才会让这个数动。
# 本检查自己不往 ctest 挂注册，于是 `ctest -N` 的条数与期望值那一格始终同值。

foreach(CLV_REQUIRED_ARG CLV_EXPECTED_TESTS CLV_TESTS_BINARY_DIR CLV_TESTS_TARGET)
    if(NOT DEFINED ${CLV_REQUIRED_ARG})
        message(FATAL_ERROR "CLV: check_registered_tests 缺入参 -D${CLV_REQUIRED_ARG}")
    endif()
endforeach()

if(NOT CLV_EXPECTED_TESTS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "CLV: CLV_EXPECTED_TESTS 要是十进制整数，当前为 '${CLV_EXPECTED_TESTS}'")
endif()

set(CLV_CTEST_FILE "${CLV_TESTS_BINARY_DIR}/CTestTestfile.cmake")
if(NOT EXISTS "${CLV_CTEST_FILE}")
    message(FATAL_ERROR "CLV: 找不到 ${CLV_CTEST_FILE} —— tests/ 没进本次配置，或者构建目录不是这一份")
endif()

# 注册清单的文件名里带一段随发现参数变化的哈希，改一次发现选项或换一次构型就会留下旧件，
# 所以不猜文件名：顺 CTestTestfile.cmake 挂进来的那份 include 件取本次真正生效的清单。
file(READ "${CLV_CTEST_FILE}" CLV_CTEST_TEXT)
string(REGEX MATCHALL "include\\(\"[^\"]+\"\\)" CLV_INCLUDES "${CLV_CTEST_TEXT}")
set(CLV_TESTS_LIST "")
foreach(CLV_ONE_INCLUDE IN LISTS CLV_INCLUDES)
    string(REGEX REPLACE "^include\\(\"(.*)\"\\)$" "\\1" CLV_ONE_PATH "${CLV_ONE_INCLUDE}")
    if(NOT EXISTS "${CLV_ONE_PATH}")
        continue()
    endif()
    file(READ "${CLV_ONE_PATH}" CLV_ONE_TEXT)
    if(CLV_ONE_TEXT MATCHES "include\\(\"([^\"]*_tests\\.cmake)\"\\)")
        set(CLV_TESTS_LIST "${CMAKE_MATCH_1}")
    endif()
endforeach()

if(CLV_TESTS_LIST STREQUAL "" OR NOT EXISTS "${CLV_TESTS_LIST}")
    file(GLOB CLV_CANDIDATES LIST_DIRECTORIES false
         "${CLV_TESTS_BINARY_DIR}/${CLV_TESTS_TARGET}_*_tests.cmake")
    list(LENGTH CLV_CANDIDATES CLV_N_CANDIDATES)
    if(NOT CLV_N_CANDIDATES EQUAL 1)
        message(FATAL_ERROR
            "CLV: 定位不到本次构建的注册清单（在 ${CLV_TESTS_BINARY_DIR} 里数到 ${CLV_N_CANDIDATES} 份候选）。"
            "${CLV_TESTS_TARGET} 没构建时清单不会生成 —— 先把测试目标建出来再核这一条。")
    endif()
    list(GET CLV_CANDIDATES 0 CLV_TESTS_LIST)
endif()

file(READ "${CLV_TESTS_LIST}" CLV_LIST_TEXT)
string(REGEX MATCHALL "add_test\\(" CLV_ENTRIES "${CLV_LIST_TEXT}")
list(LENGTH CLV_ENTRIES CLV_REGISTERED)

if(NOT CLV_REGISTERED EQUAL CLV_EXPECTED_TESTS)
    message(FATAL_ERROR
        "CLV: ctest 注册例数 ${CLV_REGISTERED} ≠ 期望 ${CLV_EXPECTED_TESTS}（清单 ${CLV_TESTS_LIST}）。"
        "数的是注册数不是通过数；用例增删要同批改 CMakeLists.txt 里期望值那一格与 tests/docs/ 的对外计数。")
endif()

message(STATUS "CLV: ctest 注册例数 ${CLV_REGISTERED} = 期望 ${CLV_EXPECTED_TESTS}")
