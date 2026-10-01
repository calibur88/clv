# 空后端的文件清单。该后端无第三方依赖、不需要开关，始终参与编译。

set(CLV_SOURCES ${CLV_SOURCES}
    backends/null/logger.cpp
)
