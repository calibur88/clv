# demo 的构建入口：默认不建，要产物时显式打开。
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCLV_BUILD_DEMOS=ON -DCLV_LOGGER_USE_SPDLOG=ON
#   cmake --build build
#   cd demo_local && ./bin/clv_demo_core.exe && ./bin/clv_demo_container.exe
#
# demo_local/ 三块分工：bin/ 只放构建产物，每次构建由「先清空再拷贝」刷新一遍；
# src/ 是 demo/ 的同步副本；示例跑出来的文件落在 demo_local/ 根上。
# 三者里只有根上的留观文件与 src/ 副本是给人留的，清 bin 动不到它们。
# demo/ 本身只放源码与 README；整个 demo_local/ 不入库。
# 日志等级过滤那一项要看得见输出，所以产物必须用 spdlog 后端构建（CLV_LOGGER_USE_SPDLOG=ON）；
# 空后端下所有 CLV_Log 调用都是丢弃，core demo 的复检项 2 无从判定。

set(CLV_DEMO_LOCAL ${CMAKE_SOURCE_DIR}/demo_local)
set(CLV_DEMO_OUT ${CLV_DEMO_LOCAL}/bin)
set(CLV_DEMO_SRC ${CLV_DEMO_LOCAL}/src)
set(CLV_DEMO_OBJ ${CMAKE_CURRENT_BINARY_DIR}/clv_demo_bin)
set(CLV_DEMO_SNAPSHOT core.c container.c README.md)

# 可执行名与源文件名不同步：clv_demo_core ← core.c，clv_demo_container ← container.c
add_executable(clv_demo_core core.c)
add_executable(clv_demo_container container.c)

foreach(target clv_demo_core clv_demo_container)
    # 两个 demo 都只链公开 ABI、不碰内部头；与主库同口径 /W4 /WX——样板代码不带 warning。
    #
    # 示例按 C 编译：公开头要能被纯 C 消费者吃下。
    # CRT 运行时不单独指定：必须与 clv 库（以及它 PRIVATE 依赖的 spdlog）用同一套，
    # 这里若强设静态 CRT，链接期会因 /MDd 目标文件引用 __imp__ 符号而报 LNK2019。
    target_link_libraries(${target} PRIVATE clv)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    clv_apply_target_flags(${target})

    # 先落在 build 里，再由下面的刷新步骤整份投到 demo_local/bin：
    # 直接输出到 bin 的话，ninja 认为目标已最新时不会重链，清空步骤就把没有_dirty_的产物删没了
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CLV_DEMO_OBJ}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CLV_DEMO_OBJ}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CLV_DEMO_OBJ})

    # 源码副本跟着构建走，不靠人手 copy：贴进 demo_local 的代码与 demo/ 脱节就等于没有
    foreach(src ${CLV_DEMO_SNAPSHOT})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CLV_DEMO_SRC}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/${src} ${CLV_DEMO_SRC}/${src}
            VERBATIM)
    endforeach()
endforeach()

# bin 是纯产物镜像：整份清掉再拷，所以改名、删目标都不会在 bin 里留下孤儿。
# 这个 rm -rf 的作用域就是上面 CLV_DEMO_OUT 这一个目录，示例生成的文件与 src/ 副本都不在里面。
add_custom_target(clv_demo_local ALL
    DEPENDS clv_demo_core clv_demo_container)
add_custom_command(TARGET clv_demo_local POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CLV_DEMO_OUT}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CLV_DEMO_OBJ} ${CLV_DEMO_OUT}
    COMMENT "刷新 demo_local/bin（只放构建产物）"
    VERBATIM)
