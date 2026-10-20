# 测试项 · 公开 ABI 面（api）

| 栏 | 内容 |
|---|---|
| **测试项名** | 公开 ABI 面：只吃公开 C ABI 的端到端契约 |
| **测什么** | 版本双轨、内存分配器、日志、文件 IO、参数结构体布局、容器 / 视频 / 音频句柄端到端、元数据视图、视频 profile、无损容器、呈现节器件——全部不 include 实现层头，只调公开头声明的 C ABI。 |
| **怎么测** | 纯公开面调用（`include/CLV_*.h`）。`ParamStructLayout` 三例含「磁盘无残件」验证（写参数结构后确认不落地残留）。`PaceApiTest` 十六例（`TEST_F` 夹具）+ `PaceApiStandalone` 三例（共 19 例）走**假设备 + 假时钟**：排队字节数与单调钟都由用例供，一步一跳手动推进，所以外推、锚点复位、暂停不推进、背压阈值这些在真器件上要靠时序才能碰到的分支，都在这里逐条钉死。 |
| **测哪里** | `tests/win32/api/`（10 件：`test_file` 19 / `test_pace` 19 / `test_audio_metadata_view` 9 / `test_logger` 7 / `test_container` 6 / `test_memory` 4 / `test_version` 4 / `test_video_tool_rejection` 4 / `test_param_struct_layout` 3 / `test_video_lossless_container` 1，相加 = 76）。 |
| **判据** | 版本双轨字面量钉。`ParamStructLayout` 三例。官方 CC0 件三例见音频面文档。日志面定性 = 冒烟档，不断输出内容 / 条数 / 顺序。节器件：阈值全是 `period` 的倍数（对齐带 ±半周 / 等待半周 / 丢帧一整周）逐档重算核对；起钟两处修正各一例（预灌期不推进钟、BeginPass 后首次读钟只登记锚点不外推）；容量缺省 1000 ms / 200 ms 当量各钉一次；冒烟判据 = 理想趟走完 30 帧 `dropped = 0`。 |
| **平台** | Win32 实测；Linux / macOS 暂无用例。 |
| **现状** | 通过（76 例，两档全跑，`TEST` 与 `TEST_F` 都数）。`CLV_COEF_CODING=0` 定长档构型下 `VideoToolRejection.FrameHeaderExtraSignallingBitsRejectOnlyThatFrameNotTheStream` 两档都实跑、不跳过：默认档把帧头选择器抹成 0、定长档抹成 1，两侧都是「这一帧被拒、且拒在帧头之后」—— 码 67 `CLV_VIDEO_PAYLOAD_TOO_SMALL`、拒在宏块载体读取段、只坏这一帧不进拒解态。 |
