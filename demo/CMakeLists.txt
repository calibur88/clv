# demo 的构建入口：默认不建，要产物时显式打开。
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCLV_BUILD_DEMOS=ON -DCLV_LOGGER_USE_SPDLOG=ON
#   cmake --build build
#
# 产物固定落在 demo_local/bin/（该目录不入库）。demo/ 本身只放源码与 README，不留构建产物。
# 日志等级过滤那一项要看得见输出，所以产物必须用 spdlog 后端构建（CLV_LOGGER_USE_SPDLOG=ON）；
# 空后端下所有 CLV_Log 调用都是丢弃，复检项 2 无从判定。

add_executable(clv_demo clv_demo.c)

# 示例按 C 编译：公开头要能被纯 C 消费者吃下。
# CRT 运行时不单独指定：必须与 clv 库（以及它 PRIVATE 依赖的 spdlog）用同一套，
# 这里若强设静态 CRT，链接期会因 /MDd 目标文件引用 __imp__ 符号而报 LNK2019。
target_link_libraries(clv_demo PRIVATE clv)

target_compile_definitions(clv_demo PRIVATE _CRT_SECURE_NO_WARNINGS)
clv_apply_target_flags(clv_demo)

set(CLV_DEMO_OUT ${CMAKE_SOURCE_DIR}/demo_local/bin)
set_target_properties(clv_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CLV_DEMO_OUT}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CLV_DEMO_OUT}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CLV_DEMO_OUT}
)
