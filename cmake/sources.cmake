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

    # clv::container（容器层：结构 / 包 / 容错 / 写读 + C ABI 门面）
    src/container/packet.cpp
    src/container/structure.cpp
    src/container/timebase.cpp
    src/container/frames.cpp
    src/container/writer.cpp
    src/container/reader.cpp
    src/container/clv_container.cpp

    # clv::audio（FLAC 帧与元数据层 + C ABI 门面）
    src/audio/frame.cpp
    src/audio/metadata.cpp
    backends/flac/flac_codec.cpp
    src/audio/clv_audio.cpp

    # clv::video（VOP 头与片头 / 宏块头与宏块序列 / 变换量化 / 参考帧管理 + C ABI 门面）
    src/video/headers.cpp
    src/video/bits.cpp
    src/video/macroblock.cpp
    src/video/block.cpp
    src/video/reconstruct.cpp
    src/video/payload.cpp
    src/video/direct_scale.cpp
    src/video/padding.cpp
    src/video/quant.cpp
    src/video/dpb.cpp
    src/video/motion.cpp
    src/video/clv_rgb.cpp
    src/video/clv_video.cpp

    # clv::pace（呈现节器件：主时钟口径与逐帧判定，不依赖任何播放器库，不开线程不睡）
    src/pace/clv_pace.cpp

    # clv::platform 公共部分（唯一选源处 + 与平台无关的内存缓冲区后端）
    platform/fileio/impl_fileio.cpp
    platform/fileio/memory_fileio.cpp
)
