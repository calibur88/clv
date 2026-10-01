# spdlog 后端的文件清单。是否真正编译由源文件自守卫决定（CLV_LOGGER_USE_SPDLOG），
# 与 cmake/platform/*.cmake 一样：本文件只列文件，不做判断。

set(CLV_SOURCES ${CLV_SOURCES}
    backends/spdlog/logger.cpp
)
