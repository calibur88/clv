# 公共文件清单：不判断平台、不判断后端，与二者无关的源文件一次列全。
# 平台相关部分由 cmake/platform/<平台>.cmake 追加，后端部分由 cmake/backends/<后端>.cmake 追加。

set(CLV_SOURCES ${CLV_SOURCES}
    # clv（顶层身份）
    src/version.cpp

    # clv::core（调度层 + 平台无关工具）
    src/core/fileio.cpp
    src/core/logger.cpp
    src/core/memory.cpp
    src/core/crc32.cpp
    src/core/varint.cpp

    # clv::platform 公共部分（唯一选源处）
    platform/fileio/impl_fileio.cpp
)
