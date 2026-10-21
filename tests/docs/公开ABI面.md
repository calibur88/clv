# 测试项 · 公开 ABI 面（api）

| 栏 | 内容 |
|---|---|
| **测试项名** | 公开 ABI 面：只吃公开 C ABI 的端到端契约 |
| **测什么** | 版本双轨、内存分配器、日志、文件 IO、参数结构体布局、容器 / 视频 / 音频句柄端到端、元数据视图、视频 profile、无损容器、呈现节器件——全部不 include 实现层头，只调公开头声明的 C ABI。 |
| **怎么测** | 纯公开面调用（`include/CLV_*.h`）。`ParamStructLayout` 三例含「磁盘无残件」验证（写参数结构后确认不落地残留）。`PaceApiTest` 十六例（`TEST_F` 夹具）+ `PaceApiStandalone` 三例（共 19 例）走**假设备 + 假时钟**：排队字节数与单调钟都由用例供，一步一跳手动推进，所以外推、锚点复位、暂停不推进、背压阈值这些在真器件上要靠时序才能碰到的分支，都在这里逐条钉死。 |
| **测哪里** | `tests/win32/api/`（10 件：`test_file` 24 / `test_pace` 19 / `test_audio_metadata_view` 9 / `test_logger` 15 / `test_container` 6 / `test_memory` 4 / `test_version` 6 / `test_video_tool_rejection` 4 / `test_param_struct_layout` 3 / `test_video_lossless_container` 1，相加 = 91）。 |
| **判据** | 版本双轨字面量钉。`ParamStructLayout` 三例。官方 CC0 件三例见音频面文档。日志投递侧定性 = 冒烟档（不判内容 / 条数 / 顺序）；**落盘侧是实判**：路径档三种入参（`NULL` / 空串 / 含反斜杠）都判 `INVALID_PATH` 且不碰盘，参数档判 `rotate_size_kb == 0`、`SIZE_MAX` 一类乘不下的值与 `max_files > 200 000` 都归 `INVALID_ARGUMENT`，后端档按构型分侧（spdlog 侧装上后消息读得回、null 侧回 `UNSUPPORTED_BACKEND` 且不留件），换落点判「旧的不再收新的、新的不再收旧的」，`max_files = 0` 判不轮转且重设不 truncate（轮转件命名按后端 `calc_filename` 的形状拼出来查它在不在），`CLV_LogStrError` 逐格全覆盖 + 未知值 `"unknown"`，枚举行三件 `static_assert` 钉 `OK == 0` / 102–104 连续 / 末码 105。内存缓冲句柄（`ClvFile` 六例）钉五件事：所有权（`Close` 后宿主缓冲照常可读、库不接管）、`mode` 两档受理（`APPEND` 与越界值落 `INVALID_ARGUMENT`，且**不产出值 9**）、固定容量（写档越界整次拒 + 完成量写 0 + 游标不推进）、`Size` 两档读法（只读档 = 容量、只写档 = 高水位）、参数档与文件后端同式（非零 `size` 配空指针 ⇒ `INVALID_ARGUMENT`，读写两侧各钉一次；`size == 0` 不碰指针 ⇒ OK + 0）。节器件：阈值全是 `period` 的倍数（对齐带 ±半周 / 等待半周 / 丢帧一整周）逐档重算核对；起钟两处修正各一例（预灌期不推进钟、BeginPass 后首次读钟只登记锚点不外推）；容量缺省 1000 ms / 200 ms 当量各钉一次；冒烟判据 = 理想趟走完 30 帧 `dropped = 0`。 |
| **平台** | Win32 实测；Linux / macOS 暂无用例。 |
| **现状** | 通过（91 例，两档全跑，`TEST` 与 `TEST_F` 都数）。`CLV_COEF_CODING=0` 定长档构型下 `VideoToolRejection.FrameHeaderExtraSignallingBitsRejectOnlyThatFrameNotTheStream` 两档都实跑、不跳过：默认档把帧头选择器抹成 0、定长档抹成 1，两侧都是「这一帧被拒、且拒在帧头之后」—— 码 67 `CLV_VIDEO_PAYLOAD_TOO_SMALL`、拒在宏块载体读取段、只坏这一帧不进拒解态。 |
