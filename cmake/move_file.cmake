# 把一个文件从 FROM 移动到 TO（不是复制：构建目录里不留副本）。
# FROM 不存在时静默返回——编译数据库只在配置阶段生成，第二次构建时源文件已经不在原地，
# 少这一层判空会让构建失败。

if(EXISTS "${FROM}")
    get_filename_component(FROM_DIR "${TO}" DIRECTORY)
    file(MAKE_DIRECTORY "${FROM_DIR}")
    file(RENAME "${FROM}" "${TO}")
endif()
