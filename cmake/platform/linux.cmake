# 只列文件，不判断平台：是否真正编译由源文件自守卫决定，这里与顶层 if(UNIX) 同源加载。

set(CLV_SOURCES ${CLV_SOURCES}
    platform/fileio/linux_fileio.cpp
)
