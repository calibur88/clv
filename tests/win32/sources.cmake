# tests/win32/sources.cmake —— Windows 平台测试源清单
#
# 与根 cmake/platform/*.cmake 同构：只列文件，不做判断；平台分派在 tests/CMakeLists.txt。

set(CLV_TEST_SOURCES ${CLV_TEST_SOURCES}
    win32_main.cpp
    win32/core/test_crc32.cpp
    win32/core/test_varint.cpp
    win32/core/test_varint_signed.cpp
    win32/container/test_packet_frames.cpp
    win32/container/test_container_io.cpp
    win32/container/test_fixture_files.cpp
    win32/container/test_write_chunks.cpp
    win32/container/test_container_extchain_timebase.cpp
    win32/container/test_segments.cpp
    win32/audio/test_audio_framing.cpp
    win32/audio/test_audio_codec.cpp
    win32/audio/test_audio_blob_refs.cpp
    win32/video/test_video_headers.cpp
    win32/video/test_video_macroblock.cpp
    win32/video/test_video_motion.cpp
    win32/video/test_video_block.cpp
    win32/video/test_video_block_coef.cpp
    win32/video/test_video_payload.cpp
    win32/video/test_video_direct_scale.cpp
    win32/video/test_video_quant_dpb.cpp
    win32/video/test_video_slice_predictor.cpp
    win32/video/test_video_domain_edges.cpp
    win32/video/test_video_transform_lossless.cpp
    win32/video/test_video_params_and_rgb24.cpp
    win32/video/test_video_lossless_roundtrip.cpp
    win32/video/test_video_quality.cpp
    win32/video/test_video_rejected_state.cpp
    win32/independent/test_ind_container.cpp
    win32/independent/test_ind_audio.cpp
    win32/independent/test_ind_video.cpp
    win32/independent/test_ind_video_motion.cpp
    win32/api/test_file.cpp
    win32/api/test_logger.cpp
    win32/api/test_memory.cpp
    win32/api/test_version.cpp
    win32/api/test_container.cpp
    win32/api/test_video_tool_rejection.cpp
    win32/api/test_video_lossless_container.cpp
    win32/api/test_param_struct_layout.cpp
    win32/api/test_audio_metadata_view.cpp
    win32/api/test_pace.cpp
)
