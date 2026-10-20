# 把一个文件从 FROM 移动到 TO：移动而非复制，源位置不留副本。
# FROM 不存在时静默返回：编译数据库只在配置阶段生成，
# 第二次构建时 FROM 已不在原地。

if(EXISTS "${FROM}")
    # 取的是 TO 的父目录，变量名别写成 FROM_DIR —— 名实不符会让下次改这段的人以为它在建源目录
    get_filename_component(TO_DIR "${TO}" DIRECTORY)
    file(MAKE_DIRECTORY "${TO_DIR}")
    file(RENAME "${FROM}" "${TO}")
endif()
