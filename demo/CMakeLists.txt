# demo 产物默认不构建，由 CLV_BUILD_DEMOS 显式打开。
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCLV_BUILD_DEMOS=ON -DCLV_LOGGER_USE_SPDLOG=ON
#   cmake --build build
#   cd demo_local && ./bin/clv_demo_core.exe && ./bin/clv_demo_container.exe
#
# 动态路（导出面的验收形）：上面那条配置再加 -DCLV_BUILD_SHARED=ON，clv.dll 随同一次刷新进 bin/，
# 四个示例照同一命令跑 —— 跑通即 dllexport / dllimport 那条路实测过（tests 恒链静态件，不受这个开关影响）。
#
# demo_local/ 由构建创建：bin/ 只放构建产物，每次构建先清空再整份拷贝；
# src/ 是 demo/ 的同步副本；示例跑出来的文件落在 demo_local/ 根上。
# 清空只动 bin/，示例跑出的文件与 src/ 副本都不受它影响。
# demo/ 只放源码与 README；demo_local/ 全部由构建与示例运行产出。
# 日志等级过滤那一项要看得见输出，所以产物必须用 spdlog 后端构建（CLV_LOGGER_USE_SPDLOG=ON）；
# 空后端下所有 CLV_Log 调用都是丢弃，core demo 的 #02 看不出过滤结果。

set(CLV_DEMO_LOCAL ${CMAKE_SOURCE_DIR}/demo_local)
set(CLV_DEMO_OUT ${CLV_DEMO_LOCAL}/bin)
set(CLV_DEMO_SRC ${CLV_DEMO_LOCAL}/src)
set(CLV_DEMO_OBJ ${CMAKE_CURRENT_BINARY_DIR}/clv_demo_bin)
set(CLV_DEMO_SNAPSHOT core.c container.c video.c subtitle.c subtitle_player.c subtitle_player.h README.md)

# 可执行名与源文件名不同步：clv_demo_core ← core.c，clv_demo_container ← container.c，
# clv_demo_video ← video.c，clv_demo_subtitle ← subtitle.c + subtitle_player.c
add_executable(clv_demo_core core.c)
add_executable(clv_demo_container container.c)
add_executable(clv_demo_video video.c)
add_executable(clv_demo_subtitle subtitle.c subtitle_player.c)

foreach(target clv_demo_core clv_demo_container clv_demo_video clv_demo_subtitle)
    # 四个 demo 都只链公开 ABI、不碰内部头；告警档走 clv_apply_target_flags，与主库同一套开关：
    # 恒带 /W4 /permissive-，/WX 只在 CLV_WARNINGS_AS_ERRORS=ON 时追加（该开关默认 OFF，默认不把警告当错误）。
    #
    # 示例按 C 编译：公开头要能被纯 C 消费者吃下。
    # CRT 不在这里指定：顶层 CMAKE_MSVC_RUNTIME_LIBRARY 把全仓库（含 gtest 与本目录产物）锁在同一套
    # 动态 CRT 上，这里跟着主库走即可，不加任何 CRT 相关的编译或链接选项。
    # 链接分工是定好的形状：demo 是本仓库天然的外部 ABI 消费者，开了动态就链 DLL，
    # 四个示例跑通即动态路验收（不需要另做 probe，也不动 tests）；没开就退回静态件。
    if(CLV_BUILD_SHARED)
        target_link_libraries(${target} PRIVATE clv)
    else()
        target_link_libraries(${target} PRIVATE clv_static)
    endif()
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    clv_apply_target_flags(${target})

    # 产物先落 build 里的这一份，再由下面的刷新步骤整份投到 demo_local/bin：
    # RUNTIME_OUTPUT_DIRECTORY 指这里，不指 demo_local/bin。
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CLV_DEMO_OBJ}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CLV_DEMO_OBJ}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CLV_DEMO_OBJ})

    # 建目录一次就够：make_directory 幂等，同一目标上多挂几条只是重复执行同样的命令
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CLV_DEMO_SRC}
        VERBATIM)
    foreach(src ${CLV_DEMO_SNAPSHOT})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/${src} ${CLV_DEMO_SRC}/${src}
            VERBATIM)
    endforeach()
endforeach()

# 播放器示例是 SDL3 在本仓库里唯一的消费者：显示腿走 SDL 的 IYUV 纹理、音频腿走 SDL 的设备流，
# 两条都直接吃公开 C ABI 交出来的字节。找不到 SDL3 就把配置判失败、不静默降级：
# 因为这时候「播放器 demo」是开了的，静默少一个目标比当场报错更难查。
target_link_libraries(clv_demo_video PRIVATE SDL3::SDL3)
target_link_libraries(clv_demo_subtitle PRIVATE SDL3::SDL3)

# SDL3 这份装的是动态件（_install/bin/SDL3.dll + lib/SDL3.lib 导入库），所以运行时也要它在 exe 同目录。
# 与上面 clv.dll 同一手法：投进 build 里的产物目录，跟着那次整份刷新一起进 demo_local/bin。
add_custom_command(TARGET clv_demo_video POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> ${CLV_DEMO_OBJ}
    VERBATIM)
add_custom_command(TARGET clv_demo_subtitle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> ${CLV_DEMO_OBJ}
    VERBATIM)

# 开了动态就把 clv.dll 投进 build 里的产物目录，让它跟着下面那次整份刷新一起进 demo_local/bin：
# Windows 按 exe 同目录找 DLL，bin 里少了它示例就起不来。链接关系已经保证 DLL 先于示例产出。
# 挂点为什么在 clv_demo_local 而不是某个 exe 的 POST_BUILD：exe 只按导入库判要不要重链，
# 而只改实现（导出面一字不动）时导入库哈希不变 ⇒ exe 不重链、那条拷贝整段不执行，
# bin 里留的就是上一轮的旧 DLL —— 实测一次改了库内缺省值，跑出来仍是旧缺省的行为。

# bin 是纯产物镜像：整份清掉再拷，所以改名、删目标都不会在 bin 里留下孤儿。
# 这个 rm -rf 的作用域就是上面 CLV_DEMO_OUT 这一个目录，示例生成的文件与 src/ 副本都不在里面。
add_custom_target(clv_demo_local ALL
    DEPENDS clv_demo_core clv_demo_container clv_demo_video clv_demo_subtitle)
if(CLV_BUILD_SHARED)
    add_custom_command(TARGET clv_demo_local POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:clv> ${CLV_DEMO_OBJ}
        VERBATIM)
endif()
add_custom_command(TARGET clv_demo_local POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CLV_DEMO_OUT}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CLV_DEMO_OBJ} ${CLV_DEMO_OUT}
    COMMENT "刷新 demo_local/bin（只放构建产物）"
    VERBATIM)
