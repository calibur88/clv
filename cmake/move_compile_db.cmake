# 把构建目录里的 compile_commands.json 移到仓库根，全仓只留根那一份。
#
# clangd 的 --compile-commands-dir 实测不吃构建目录里的数据库（它要的是一个目录级
# compile_commands.json 位置），因此沿用「移到根」的做法：命令行读根这一份，
# 构建目录不留副本（见 cmake/move_file.cmake）。

function(clv_add_move_compile_db_target)
    add_custom_target(move_compile_db ALL
        COMMAND ${CMAKE_COMMAND}
                -DFROM=${CMAKE_BINARY_DIR}/compile_commands.json
                -DTO=${CMAKE_SOURCE_DIR}/compile_commands.json
                -P ${CMAKE_SOURCE_DIR}/cmake/move_file.cmake
        COMMENT "clv: 把编译数据库移动到仓库根"
        VERBATIM)
endfunction()
