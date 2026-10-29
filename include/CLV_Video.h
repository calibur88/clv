/* CLV_Video.h
 *
 * CLV 自研视频流（`stream_type = 0` / `codec_id = 0`）编解码面的 C ABI 契约。
 * 编解两侧共用同一套有损重建闭环：编码端整数 Hadamard 8×8 变换 → 两级量化偏移叠加 →
 * 系数载体（块模式位图决定块是否存在，在场块的系数位宽由生效量化步长决定、不进码流）→
 * 反量化 → 反变换 → 加预测 → 夹取，并把自己的重建存为参考帧；解码端逐块走同一套重建。
 * 系数载体有两种写法：定长档与法 1 档（先写结束标志 EOB，再用位数递增的 Exp-Golomb 编码写幅值）；
 * 具体用哪一档由视频帧头第 5 到第 7 位声明，本库默认法 1。
 * 解码句柄七个入口：Open / Decode / NextOutput / Flush / Reset / StreamRejected / Close，
 * 覆盖帧头与片语法解析、致命码拒解、参考帧列表管理与出帧顺序。
 * 帧间预测按运动矢量取样，半像素用双线性平均（`b` / `c` / `d` 三式，舍入固定 `R = 0`），
 * 参考帧缓冲带一圈 1 像素边缘复制带，色度与亮度同核、矢量按 `(v + 1) >> 1` 换算；
 * B 帧一律 direct 且不带残差。故重建帧与源帧**不**逐字节相同，差异由量化与预测共同给出。
 * 函数名、结构体与出参形态都是对外 ABI 的一部分，改动即破坏调用方；
 * 错误码只在段尾追加，既有码值与语义不动。
 *
 * 面：不透明句柄 + 自由函数 + 整型错误码 + POD 描述符。零第三方类型、零平台类型。
 *
 * 与容器的分工：一帧的包外层、片序与重组都归容器层，本头只处理**一帧的完整 VOP 载荷**，
 * 以及调用方按宏块边界切好的**片数组**；片内的结构（片头、`first_mb`）只有视频层认识，
 * 容器不解释。载荷与帧缓冲都不跨句柄存活：帧缓冲由库拥有、引用计数管理，调用方不接管所有权，
 * 要留内容自己拷。取帧口交出帧时引用计数加一；调用方用毕调 CLV_VideoFrameFree 减一；
 * 计数归零的帧才回收进空闲池。空闲池的槽位连同像素缓冲一并留着复用，因此常驻量看的是
 * 「同时在场的帧数」而不是「解出过多少帧」：占用 ≤（参考缓冲 maxlen + 调用方未释放数）
 * × 每帧持有字节 + 基线，与帧数无关（基线 = 起池阈值那一档的在途帧与句柄自身）。
 *
 * 时间轴：入参与出参只带该流时间轴上的整数 ticks 与 `timebase_den`，库不拥有播放时钟，
 * 不做换算、不做调度。视频流的时间基是固定分母的大整数，禁止转浮点比较。
 *
 * 尺寸：原始显示尺寸 ∈ [1, 65535]，必须声明（0 不是「未声明」，而是打不开解码器）；
 * 补后尺寸 = 原始尺寸向上取整到 16 的倍数，
 * **不落字节**，一律由 `CLV_VideoCodedSize` 之类的接口按 32 位无符号推导
 * （原始值取 65521..65535 时补后为 65536，需 17 位，用 16 位容器算会回绕成 0）。
 * 补黑只补右与下，填充宏块照常编码；显示按原始尺寸裁回，原始尺寸未声明时不裁边（右/下露黑边）。
 * 补黑与裁回这两套入口库内不消费（解码出的帧仍带那一圈补齐），供调用方自备显示路径时取用；
 *   这一条是形状说明，不是「以后会消费」的临时状态。
 *
 * 量化：v1 只有一种反量化算法。量化步长 `q` 的合法域是**三段**，上界由 `quant_precision` 派生（`2^p − 1`，v1 的 p = 7 才是 127）：
 * `q = 0` = 无损档（块数据直载变换系数原值，不量化、不夹取）/ `1 ≤ q ≤ 2^p − 1` = 有损档 / 其余取值 = 域外不成立。
 * 帧级与宏块级偏移是带符号的定宽小整数，叠加后的有效步长按同一段上界夹住，不引入第二档逃生路径；
 * 不存在量化方法标志位——模式信号只有 `q` 本身这一个。
 *
 * 档次：基线流对应外部视频标准里 Simple 档那一组工具白名单（8-bit、逐行、矩形、无透明通道），B-VOP 是 CLV 私有扩展工具。
 * 隔行、灰度/任意形状、sprite、时域/空域可分级在 v1 一律不收：码流里出现即**拒解整条视频流**，其余流不受影响。
 * 位深这一档**以错误码为准，不写「N-bit 一律不收」这种笼统话**：`{8, 10, 12}` 之外的任意位深落 `CLV_VIDEO_BIT_DEPTH_UNSUPPORTED`；
 * 10 / 12 是**声明合法**（`extended_esc = 1` 的位深表上成立）而 v1 的像素电平只实现到 8-bit ⇒ 同样照拒解、同一个码。
 * 换句话说「表上合法」与「本层实现」是两件事，读者不要从位深表推出 10 / 12 能解码。
 *
 * 错误码：`CLV_VideoError` 是**独立枚举**，成功恒为 0（与既有公开面同值同义），
 * 失败码全部落在 64–95 段内。五面的错误码各自独立枚举：文件与容器从 1 顺排、音频 32 到 63、
 * 视频 64 到 95、节拍 96 起，不得跨头比对数值——各头的 1 语义无关。
 */
#ifndef CLV_VIDEO_H
#define CLV_VIDEO_H

#include "CLV_Export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct CLV_VideoDecoder CLV_VideoDecoder;
	typedef struct CLV_VideoEncoder CLV_VideoEncoder;
	typedef struct CLV_VideoFrame CLV_VideoFrame;
	typedef struct CLV_VideoBitstream CLV_VideoBitstream;

	/* 视频面错误码。0 恒为成功；失败码从 64 起，段内只追加不改序、不复用。
	 * 标了「拒解整条流」的码是致命的：句柄进入拒解态，此后除 Reset / Close 与观测口
	 * CLV_VideoDecoderStreamRejected 外的每次调用一律返回 CLV_VIDEO_STREAM_REJECTED，其余流不受影响。
	 * 建句柄时即命中的，CLV_VideoDecoderOpen 照常交出这个拒解态句柄，不返 NULL。 */
	typedef enum CLV_VideoError
	{
		CLV_VIDEO_OK = 0,

		CLV_VIDEO_INVALID_HANDLE = 64,	 /* 句柄为空或已关闭 */
		CLV_VIDEO_INVALID_ARGUMENT,		 /* 出参指针为空、数组与计数不匹配、片为空等入参不成立 */
		CLV_VIDEO_VALUE_RANGE,			 /* 字段值域不成立：尺寸越界、fcode 非 1..7、宏块数为 0 等 */
		CLV_VIDEO_PAYLOAD_TOO_SMALL,	 /* 载荷短于片头/帧头定长，或 varint 读不完 */
		CLV_VIDEO_HEADER_BAD,			 /* 帧头七字段不成立：类型非 I/P/B、reserved 位非 0 */
		CLV_VIDEO_QUANT_OUT_OF_RANGE,	 /* 编码器**声明侧**的量化步长越界：CLV_VideoEncoderOpen 的
							* `default_quant` 越出该流上界 [1, QuantCeiling(quant_precision)]
							* 即 [1, min(2^p - 1, 255)]，p 取 [3,9]；p = 7 时才是 [1,127]。
							* 本码只由这一条声明入口产 —— 逐调用侧的同名越界走 84。
							* 解码侧码流里的 q 不成立走 HEADER_BAD，与本码无关。 */
		CLV_VIDEO_BIT_DEPTH_UNSUPPORTED, /* 位深不在 v1 合法集（8 / 10 / 12；16 为结构预留）→ 拒解整条流 */
		CLV_VIDEO_TOOL_EXCLUDED,		 /* 码流出现 v1 不收的工具 → 拒解整条流 */
		CLV_VIDEO_TOOL_LIST_ABSENT,		 /* 扩展工具存在位为 1 但块内没有清单 → 拒解整条流 */
		/* 73 是留白号位，不复用（那一档没有产出点）；本条因此显式给值 */
		CLV_VIDEO_SLICE_HEADER_BAD = 74, /* 片头不成立：片数越上界、量化携带位与该片位置矛盾 */
		CLV_VIDEO_SLICE_SEQUENCE,		 /* 片序跳号，或本片的首宏块与上一片不衔接、倒退 */
		/* 76 号位：参考帧缺失。这条路只走 disposition（`CLV_VIDEO_DISP_DROP_REF_MISSING`：
		 * 丢这一帧、返回 `CLV_VIDEO_OK` 并计入统计），而一次调用只能出一个返回值，
		 * 丢帧那次的返回值已被 `OK` 占住 ⇒ 本码无产出点，号位不复用。
		 * 本库不承诺「错误码 + disposition 都走」。 */
		CLV_VIDEO_REF_FRAME_MISSING = 76, /*  请求的参考锚点不在参考缓冲里 */
		CLV_VIDEO_OUTPUT_TOO_SMALL,		  /* 调用方给的缓冲或行距不满足库给出的布局。
											 * 行距装不下这一形状现在由 `CLV_VideoFrameToRGB24` 判入参不成立，
											 * 公开入口没有别处产本码 ⇒ 本码无产出点，号位不复用。 */
		/* 78 是留白号位，不复用（那一档没有产出点）；本条因此显式给值 */
		CLV_VIDEO_STATE_ERROR = 79, /* 调用顺序不成立：已冲刷后继续喂、未加流就写等 */
		CLV_VIDEO_OUT_OF_MEMORY,	/* 库内分配失败 */
		/* 81 与 82 都是留白号位，不复用：两档语义都没有产出点，显式给值才接得上。
		 * 「熵解码 / 算术不自洽」这条语义要等熵解码支路自己能报不自洽时才成立，
		 * 届时在枚举尾另立新规，不占这两个旧号位。 */
		CLV_VIDEO_STREAM_REJECTED = 83, /* 该流已被致命码拒解，后续调用的统一返回 */
		/* 84 = 编码器**逐调用侧**的量化步长不成立：CLV_VideoEncoderEncode 的 q 越出
		 * [1, QuantCeiling(quant_precision)]，或无损模式带了非零 q（模式与参数矛盾，
		 * 既不静默编成最细有损档也不静默降级）。与 69 的差别在处置层：69 是句柄建不成，
		 * 本码只拒这一次调用，句柄、参考缓冲与后续调用都不受影响。 */
		CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL = 84,
		/* 85 = 载荷长度不符（含过长与过短）：递交进来的载荷字节数与该块规定的定长对不上。
		 * 过短那一支仍走 `CLV_VIDEO_PAYLOAD_TOO_SMALL`（那个码的公开定义钉死是「短于定长」），
		 * 过长那一支改判本码 —— 把「装得下多余字节」报成「太短」是反的。
		 * 号位在段尾追加，不复用 73 / 78 / 81 / 82 那几个留白位。 */
		CLV_VIDEO_PAYLOAD_LENGTH_MISMATCH = 85
	} CLV_VideoError;

	/* v1 只有一个取值。取值与线上语义绑定，扩格式时在段尾追加新值。 */
	typedef enum CLV_VideoPixelFormat
	{
		CLV_VIDEO_PIX_YUV420P = 0
	} CLV_VideoPixelFormat;

	/* 取值与 VOP 载荷里的帧类型字段同值：0 = I，1 = P，2 = B。 */
	typedef enum CLV_VideoFrameType
	{
		CLV_VIDEO_FRAME_I = 0,
		CLV_VIDEO_FRAME_P = 1,
		CLV_VIDEO_FRAME_B = 2
	} CLV_VideoFrameType;

	/* 一帧的处置结论。丢帧不是错误：句柄仍可用，时间轴照常推进。
	 * 本层不累计分档计数，公开面也没有统计口：处置只按当次调用回报，要分档就调用方自计。 */
	typedef enum CLV_VideoDisposition
	{
		CLV_VIDEO_DISP_OUTPUT = 0,		 /* 正常出帧 */
		CLV_VIDEO_DISP_DROP_REF_MISSING, /* 参考帧缺失 */
		CLV_VIDEO_DISP_DROP_DIRECT_TIME, /* 双向预测的时间权重不成立 */
		CLV_VIDEO_DISP_DELAYED			 /* 已解码，暂存待输出（显示序落后于解码序） */
	} CLV_VideoDisposition;

	/* 编码器元数据块（`ext_type = 3`，v1 载荷 8 字节）的读回视图。
	 * width / height 是补边前的原始显示尺寸，**不得为 0**（0 不是「未声明」，是值域不成立）。 */
	typedef struct CLV_VideoEncMetaC
	{
		uint8_t max_b_frames;
		uint8_t quant_precision;
		uint8_t ext_tools;	  // bit0 = 有扩展工具且清单载于本块；bit1-7 v1 = 0
		uint8_t reserved;
		uint16_t display_width;
		uint16_t display_height;
	} CLV_VideoEncMetaC;

	/* 编码模式选择。默认 0 = 现行为，存量调用零改动（把参数结构整体清零即得有损）。
	 *
	 * 模式只作用于编码侧。码流里不另设模式位：无损流把量化步长字段写成 0，
	 * 读方从流里读出 0 就知道本流的系数是整数变换结果原值直载，无需旁路标志。
	 * 因此量化步长的合法域是三段：**0 = 无损 / 1..2^quant_precision−1 = 有损 / 其余 = 域外非法**。
	 *
	 * LOSSLESS 下量化步长、帧级偏移、宏块级偏移都不参与算术（它们对有损流才有意义），
	 * 所以该模式要求传进来的 q 恰为 0；带非零 q 判参数矛盾并返回错误，既不静默降级也不静默忽略。
	 * `default_quant` 在任何模式下都只作声明值，不参与编码算术，也不参与模式判定。 */
	typedef enum CLV_VideoModeC
	{
		CLV_VIDEO_MODE_LOSSY = 0,
		CLV_VIDEO_MODE_LOSSLESS = 1
	} CLV_VideoModeC;

	/* 起池段（第一帧出队之前）的并行策略。帧内分带的并行度是编译期常量、不进公开面，
	 * 这里能调的只有「起池那几帧要不要也吃分带」这一档。 */
	typedef enum CLV_VideoPrefillModeC
	{
		CLV_PREFILL_DEFAULT = 0, /* 未声明 ⇒ 取库缺省档（缺省取哪一档由实测定，见 CLV_VideoDecoderOpen 侧） */
		CLV_PREFILL_SERIAL = 1,	 /* 起池段单线程：先把第一帧吐出去，之后各帧按并行度分带 */
		CLV_PREFILL_PARALLEL = 2 /* 起池段也分带：线程池的建池开销从第一帧就付 */
	} CLV_VideoPrefillModeC;

	/* 建解码器需要的流侧参数。来源：流描述符 + 该流的 `ext_type = 3` 块。
	 * 缺失该块时按缺省：max_b_frames = 0、quant_precision = 7、ext_tools = 0。
	 * 宽高必须声明：缺参数块或声明为 0 时打不开解码器（返回值域错误并给空指针）。
	 * 尺寸一律记**原始尺寸**，补后尺寸由库推导。
	 * `struct_size` 受理集两档：`0` ⇒ 按本布局受理，`sizeof(本结构)` ⇒ 显式声明本布局；
	 * **其余任何值判 `CLV_VIDEO_VALUE_RANGE`**，库不按字节长度猜版本。本结构没有「上一版尺寸」那一档
	 * （它的首字段 `bit_depth` 合法值非 0，跨版递交必然落进显式拒那一支）。完整混编口径见本文件
	 * `CLV_VideoEncParamsC` 头部那条，两处同式。 */
	typedef struct CLV_VideoStreamParamsC
	{
		uint32_t struct_size;		 // 0 或 sizeof(CLV_VideoStreamParamsC)，其余值判 CLV_VIDEO_VALUE_RANGE
		uint16_t bit_depth;			 // 8 / 10 / 12
		uint8_t quant_precision;	 // v1 = 7
		uint8_t max_b_frames;		 // 唯一权威声明：编码器不得超出，解码器一律以声明值为准
		uint8_t vlc_table_id;		 // v1 仅允许 0
		uint8_t extended_esc;		 // 0 且位深 10/12 → 拒解整条流
		uint8_t alpha_enabled;		 // v1 必须为 0
		uint8_t obmc_disable;		 // v1 = 1
		uint8_t layer_id;			 // v1 = 0；非 0 且不声明支持 → 拒解整条流
		uint16_t display_width;		 // 必须声明；0 = 打不开解码器（值域错误 + 空指针）
		uint16_t display_height;	 // 必须声明；0 = 打不开解码器（值域错误 + 空指针）
		const uint8_t* tool_list;	 // ext_tools bit0 = 1 时的工具清单，NULL = 清单缺失
		uint32_t tool_list_size;
		/* 出帧阈值（解码侧起池深度）：待输出队列攒到本字段值才吐第一帧，之后每进一帧吐一帧。
		 * **0 = 未声明** ⇒ 取 `max_b_frames + 2`，与既有行为一字不差（重排本来就需要这么深）。
		 * **只升不降**：低于该地板的取值按地板受理 —— 阈值低于地板会在显示序排好之前吐帧，
		 * 直接违反「出帧按 PTS 升序」这条契约，故本字段不是可以往小调的旋钮。
		 * 类型域 0..255 即合法域，不设业务上限：大 N 的内存与首帧代价由调用方自担
		 * （每多攒一帧 = 一整帧像素缓冲，按 4:2:0 的 1.5 字节 / 像素与本头的像素上界算）。
		 * 尾部追加字段：本结构体的尾部填充吸收它，`sizeof` 不变 ⇒ `struct_size` **无法**区分
		 * 新旧布局，判定只能落在取值上：填 0（含 memset 清零的存量写法）= 未声明 = 现状；
		 * 未清零的旧对象可能读到非 0 ⇒ 后果是这一路把队列攒深（首帧延迟与内存同时变大），
		 * **不会**改像素、不会改出帧序，但也不会报错——所以新代码一律显式填 `sizeof(*p)`
		 * 并把本字段显式置 0，不留隐式值。 */
		uint8_t preroll_frames;
		/* 起池段（第一帧出队之前）是否走帧内分带并行，取 `CLV_VideoPrefillModeC` 三档之一，
		 * 其余值不成立 ⇒ `CLV_VIDEO_VALUE_RANGE`（声明侧，句柄建不成）。
		 * 与上一字段同理：落在尾部填充里，`sizeof` 不变，故 0 = 未声明 = 库缺省档。 */
		uint8_t prefill_mode;
	} CLV_VideoStreamParamsC;

	/* 帧描述符：像素格式、尺寸、行距、平面指针同在一个 POD 里。
	 * 行距由库给出，每个平面按 32 字节对齐；平面指针指向库拥有的缓冲。
	 * 编码器输入与解码器输出共用同一布局，故指针是**可写的**：写进去即为本帧像素，
	 * 读出来即为解码像素；指针本身不拥有内存。
	 * coded_* 是补边后的编码尺寸，display_* 是原始显示尺寸；
	 * display_* 为 0 时库按 coded_* 给出（不裁边）。 */
	typedef struct CLV_VideoFrameDescC
	{
		CLV_VideoPixelFormat pixel_format;
		uint16_t bit_depth;
		uint32_t coded_width;
		uint32_t coded_height;
		uint32_t display_width;
		uint32_t display_height;
		uint32_t stride[3];
		uint8_t* plane[3];
	} CLV_VideoFrameDescC;

	/* 一次解码动作的结论。pts / dts 是该流时间轴上的整数 ticks。 */
	typedef struct CLV_VideoFrameInfoC
	{
		CLV_VideoFrameType type;
		CLV_VideoDisposition disposition;
		uint8_t is_keyframe;
		uint8_t quant_used;	   // 本帧帧头里的量化步长原值（`vop_quant`）。两级 dquant 偏移与夹取在量化那一步现算，
							   // 不回填到本字段；v1 编码侧写出的 `vop_dquant` 恒为 0，无损流本字段就是 0
		uint32_t macroblock_count;
		uint32_t slice_count;
		int64_t dts_ticks;
		int64_t pts_ticks;
		uint64_t timebase_den;
	} CLV_VideoFrameInfoC;

	/* 建编码器需要的参数。slice_mb_count = 0 表示整帧一片（调用方自己决定分片时用它；
	 * 非 0 时按宏块边界切，每片自带片头，片数超 65 535 判参数不成立）。
	 * intra_refresh_period 是建议值：每 N 帧或每 T 秒至少一个 I 帧。
	 *
	 * **`struct_size` 一族的混编口径（本头两处参数结构体同式，`CLV_Audio.h` 与 `CLV_Container.h` 同此）**：
	 * 该成员落在结构体**首位**，用途是让库在读任何后续字段之前先判「递交进来的对象是不是本库这套布局」。
	 * 合法值两档：**`sizeof(本结构)`**，或 **0**（未填 —— 等于「调用方与本库同批编译、按 memset 清零后逐字段赋值」的存量写法，
	 * 本头的字段名与各字段语义都没动，所以这类调用方不必改码）。
	 * **其余任何值一律显式拒**（视频面 `CLV_VIDEO_VALUE_RANGE`、音频面 `CLV_AUDIO_VALUE_RANGE`、容器面 `CLV_CONTAINER_INVALID_ARGUMENT`），
	 * 库不按字节长度去猜对方是哪一版布局。这条对混编有效是因为旧对象的头四字节天然非 0：
	 * 视频编码参数原首字段 `display_width` 合法域 [1, 65535]，解流参数原首字段 `bit_depth` = 8 / 10 / 12，
	 * 音频两侧原首字段 `sample_rate` 非 0 ⇒ 跨版本递交进来必落进「非 0 且不等于 sizeof」这一支被拒掉。
	 * **残余口登记（维持受理，不收紧）**：触发条件写死在这里 —— 调用方递交的是**旧布局的
	 * `CLV_WriteParams`**（其首四字节在原布局里是 `version_minor`，旧布局的合法值恰为 **0**）且其余字段也取默认值，
	 * 库就把那四个字节读成 `struct_size = 0`、按本布局受理，后续字段随之错位。这一支**不做防御**：
	 * 把 0 一并拒掉的代价是所有调用点补一行、外部调用方必须改码，属对既有调用契约的收紧，已判为不走。
	 * **新代码一律显式填 `sizeof(*p)`**（不是"建议"）—— 只有显式声明能挡住全部混编形状，含上面这个窄洞。 */
	typedef struct CLV_VideoEncParamsC
	{
		uint32_t struct_size;	   // 0 或 sizeof(本结构)，其余值判 CLV_VIDEO_VALUE_RANGE（口径见上方）
		uint16_t display_width;	   // 原始尺寸，[1, 65535]，0 不合法
		uint16_t display_height;
		uint16_t bit_depth;
		uint8_t quant_precision;	// v1 = 7
		uint8_t default_quant;		// 本流的量化步长声明；每次编码输入的量化档范围见同文件量化档说明处
		uint8_t max_b_frames;
		/* 每片宏块数：0 = 整帧一片。**只作编码器配置，不进码流**——码流里唯一真相是片头的 mb_run
		 * （该片实际宏块数），解码器对本字段完全无感知，改它的宽度只影响编码侧调用方。
		 * 为什么必须 16 位：帧的宏块数上界 = 补后像素上界 2^28 ÷ 256 = 1 048 576，「每片 256 ~ 65 535 个宏块」
		 * 是合法配置（8-bit 会把这一整段堵掉）；65 535 这个上限与分片数上限同值，是**编码侧配置的界**，
		 * 不是码流的界——片头的 mb_run 是 varint，本身装得下更大的值。 */
		uint16_t slice_mb_count;
		uint32_t intra_refresh_period;
		uint64_t timebase_den;
		/* 尾部追加字段。该结构体现在有 `struct_size` 自报成员（口径见本文件上方），
		 * 混编的判定顺序因此是两道：先 `struct_size` 判布局，再本字段与 `q` 联合判模式。
		 * 库的判定式是「本字段恰为 LOSSLESS 且 q 恰为 0」，旧对象即便侥幸过了布局这一道，
		 * 本字段读到不确定值最坏只让一次调用返回参数矛盾错误，不会把有损流悄悄编成无损流。
		 * 要真正用无损必须按本头重新编译调用方，并把 q 置 0。 */
		CLV_VideoModeC mode;
	} CLV_VideoEncParamsC;

	/* ---- 尺寸与元数据（无状态工具函数，句柄外可用）---- */

	/* 补后尺寸 = 原始尺寸向上取整到 16 的倍数。出参是 32 位无符号：
	 * 原始值 65521..65535 时补后为 65536，16 位容器存不下。
	 * 原始尺寸为 0 或两值任一越界返回 CLV_VIDEO_VALUE_RANGE。
	 * 补后宽 × 补后高越出一帧重建缓冲的上界（2^28 = 16 384 × 16 384 像素）同样判
	 * CLV_VIDEO_VALUE_RANGE —— 这个积在任何分配入口都分配不出来，判在这里而不是留到分配点；
	 * 拒时两个出参都不写，调用方传进来的值原样保留。 */
	CLV_EXPORT CLV_VideoError CLV_VideoCodedSize(uint16_t display_width, uint16_t display_height, uint32_t* coded_width,
												 uint32_t* coded_height);

	/* 解析 `ext_type = 3` 的 8 字节载荷（小端尺寸字段）。
	 * size < 8 返回 CLV_VIDEO_PAYLOAD_TOO_SMALL；size > 8 返回 CLV_VIDEO_PAYLOAD_LENGTH_MISMATCH
	 * （载荷长度不符，含过长与过短两档）；reserved / ext_tools bit1-7 非 0
	 * 返回 CLV_VIDEO_HEADER_BAD；quant_precision 越出 [3, 9] 或 width / height 为 0
	 * 返回 CLV_VIDEO_VALUE_RANGE。视频流缺该块时容器层把该流判不可用，本函数不参与那道判定。 */
	CLV_EXPORT CLV_VideoError CLV_VideoParseEncMeta(const uint8_t* data, size_t size, CLV_VideoEncMetaC* out);

	/* ---- 解码 ---- */

	/* 建解码器。两档失败分开：
	 * 入参不成立（尺寸 0、quant_precision 越出声明域 [3,9]）返回 NULL 并置 err；
	 * 流的声明本身落在致命档（位深非 8、v1 不收的工具特征位、声明了工具清单却没给内容）时
	 * 照常交出一个**已置拒解态**的句柄，`*err` 写命中的致命码，该句柄除 Reset / Close 与
	 * CLV_VideoDecoderStreamRejected 外每个调用都返回 CLV_VIDEO_STREAM_REJECTED；Reset 是该态的出口。 */
	CLV_EXPORT CLV_VideoDecoder* CLV_VideoDecoderOpen(const CLV_VideoStreamParamsC* params, CLV_VideoError* err);

	/* 喂入**一帧的完整载荷**（容器重组后的 VOP 字节流）。
	 * 本函数只解码，不决定何时出帧：出帧走 CLV_VideoDecoderNextOutput。
	 * info 可为 NULL。片缺失不到本层（容器层重组完才交进来），本层不设「片不完整」这一档返回码；
	 * 载荷读不满按 SLICE_HEADER_BAD / VALUE_RANGE 两档判。
	 *
	 * fragment_count = 本帧由几个片组成，取容器出参 `CLV_FrameC.fragments_expected`（uint64_t，
	 * 与线上 varint 同宽，不在这里收窄）。调用方契约三条：
	 *   1. 必须 > 0 且 ≤ 65535（分片数上限在本层兑现）；0 = 没等到末片就喂，
	 *      与 > 65535 一样判 CLV_VIDEO_VALUE_RANGE；
	 *   2. 片必须已按容器外层 fragment_index 升序拼好（容器读方保证，读侧有洞与重复都另计统计）；
	 *   3. 传错（与实际片数不符）一律必拒、不留未定义行为，但返回码分两档：声明 ≥ 2 时用片头在带的
	 *      分片数与声明值对账（该字段是分片数在码流里的唯一载体），不符判 CLV_VIDEO_VALUE_RANGE；
	 *      声明 = 1 时走单片解析路径，多片拼接的字节根本不构成一帧的单片载荷，按解析失败判
	 *      CLV_VIDEO_HEADER_BAD（该档没有片头可对账）。两档都是「拒这一帧」。
	 * = 1 时载荷是「VOP 头 + 宏块序列」且不含片头（单包帧不写片头）；> 1 时每片以片头起始，
	 * 首片的片头之后带完整 VOP 头。 */
	CLV_EXPORT CLV_VideoError CLV_VideoDecoderDecode(CLV_VideoDecoder* d, const uint8_t* payload, size_t size,
													 uint64_t fragment_count, int64_t dts_ticks, int64_t pts_ticks,
													 CLV_VideoFrameInfoC* info);

	/* 按显示序取下一帧（显示序 = PTS 升序）。有帧时 *got = 1 且 *out 有效，返回 OK；
	 * 暂无可出帧时 *got = 0 且 *out 为 NULL，返回 OK——这不是错误。
	 * out 不可为空：交不出句柄的场合不写计数出参。
	 * 取帧口交出帧时引用计数加一；调用方用毕调 CLV_VideoFrameFree 减一；计数归零的帧才回收进空闲池。
	 * Free 之后该帧指针失效，建议立即置空。
	 * 重置或关闭会使所有已交出的帧指针失效，此后再调 Free 返回参数不成立。 */
	CLV_EXPORT CLV_VideoError CLV_VideoDecoderNextOutput(CLV_VideoDecoder* d, CLV_VideoFrame** out, uint8_t* got);

	/* 流结束：把仍在队列里的帧按显示序放出来，取帧口的计数口径与本口同式。
	 * 冲刷之后本句柄进入 EOS 态，再喂新帧返回 CLV_VIDEO_STATE_ERROR，必须先 CLV_VideoDecoderReset 才恢复接收。 */
	CLV_EXPORT CLV_VideoError CLV_VideoDecoderFlush(CLV_VideoDecoder* d, CLV_VideoFrame** out, uint8_t* got);

	/* 定位 / 丢包重置：清空待输出队列、参考缓冲与本流的量化与预测状态。
	 * 重置会使所有已交出的帧指针失效，此后再调 Free 返回参数不成立。 */
	CLV_EXPORT CLV_VideoError CLV_VideoDecoderReset(CLV_VideoDecoder* d);

	/* 该流是否已被致命码拒解。拒解态下除重置、关闭、观测口自身外，其余入口一律返回拒解码
	 * （`CLV_VIDEO_STREAM_REJECTED`）。 */
	CLV_EXPORT CLV_VideoError CLV_VideoDecoderStreamRejected(const CLV_VideoDecoder* d, uint8_t* out);

	CLV_EXPORT void CLV_VideoDecoderClose(CLV_VideoDecoder* d);

	/* ---- 编码 ---- */

	CLV_EXPORT CLV_VideoEncoder* CLV_VideoEncoderOpen(const CLV_VideoEncParamsC* params, CLV_VideoError* err);

	/* 申请一块库拥有的帧缓冲，行距与对齐由库给出（每平面 32 字节）。
	 * 编码器输入与解码器输出共用同一布局。填充 plane 指针指向的内存即可。
	 * 用完 Release 或 Free（本帧不在解码器帧池里，计数归零即释放）。 */
	CLV_EXPORT CLV_VideoError CLV_VideoEncoderAllocFrame(CLV_VideoEncoder* e, CLV_VideoFrame** out);

	/* 编一帧，产出一段完整载荷与它按宏块边界的切片。
	 * type = I 时忽略参考；q 越出该流上界 [1, QuantCeiling(quant_precision)] 返回
	 * CLV_VIDEO_QUANT_OUT_OF_RANGE_CALL（84，上界由本流的量化精度决定，不是恒 127）；
	 * 无损模式下带非零 q 同样落 84。声明侧 default_quant 越界才是 69。
	 * 时间轴不后退，否则 CLV_VIDEO_STATE_ERROR。
	 * 产出的码流缓冲**归调用方所有**，用完 `CLV_VideoBitstreamRelease`；编码器不持有它。 */
	CLV_EXPORT CLV_VideoError CLV_VideoEncoderEncode(CLV_VideoEncoder* e, CLV_VideoFrame* frame,
													 CLV_VideoFrameType type, uint8_t q, int64_t dts_ticks,
													 int64_t pts_ticks, CLV_VideoBitstream** out);

	CLV_EXPORT void CLV_VideoEncoderClose(CLV_VideoEncoder* e);

	/* ---- 码流缓冲 ---- */

	/* 整帧载荷（单片即整帧全部字节）。data 只到该码流被 Release 为止有效。 */
	CLV_EXPORT CLV_VideoError CLV_VideoBitstreamGetBytes(const CLV_VideoBitstream* bs, const uint8_t** data,
														 size_t* size);

	/* 按宏块边界切好的片数与第 index 片。片数 ≥ 2 时逐片可直接喂容器
	 * CLV_ContainerWriterWriteFrameChunks；片数 = 1 时该片就是整帧载荷，不含片头。
	 * 每片的载荷自带片头（含该片首宏块序号），容器不解释。 */
	CLV_EXPORT CLV_VideoError CLV_VideoBitstreamSliceCount(const CLV_VideoBitstream* bs, uint32_t* out);
	CLV_EXPORT CLV_VideoError CLV_VideoBitstreamGetSlice(const CLV_VideoBitstream* bs, uint32_t index,
														 const uint8_t** data, size_t* size);

	/* 释放 CLV_VideoEncoderEncode 产出的码流缓冲（所有权在调用方）。
	 * 对 NULL 调用是 no-op；不得对同一指针 Release 两次，Release 后指针即失效。
	 * 编码器 Close 不会替你释放仍在你名下的码流。 */
	CLV_EXPORT void CLV_VideoBitstreamRelease(CLV_VideoBitstream* bs);

	/* ---- 帧缓冲与引用计数 ----
	 *
	 * 帧的归还有两扇门，走的是同一份计数：CLV_VideoFrameFree 带回报码，CLV_VideoFrameRelease 不带。
	 * 取帧口交出帧时引用计数加一；调用方用毕调 CLV_VideoFrameFree 减一；计数归零的帧才回收进空闲池。
	 * Free 之后该帧指针失效，建议立即置空。
	 * 重置或关闭会使所有已交出的帧指针失效，此后再调 Free 返回参数不成立。
	 * 这句在两扇门上都成立，可答的档只有一个：重置之后句柄还活着，失效的帧留着一个对象壳接住那次
	 * 归还；关闭连句柄一起没了，旧指针此后怎么读都不作数（与下面那句陈旧调用同属调用方未定义行为），
	 * 本头只承诺「关闭后不得再使用该句柄」，不承诺关闭后的归还能回报什么。
	 * 对同一帧重复调用返回参数不成立；跨复用后的陈旧调用属调用方未定义行为，本头不承诺幂等 ——
 *   引用计数这一套形状属长期承诺，不是「本版本如此、以后另改」的临时规定。
	 * 所有权：帧对象始终归库，交出去的只是带计数的借用句柄 —— 调用方既不接管内存也不共享所有权。
	 * C++ 侧包装层必须自持「已释放」标志（手动 Free 置位、析构时先查该位再调 C 口），
	 * 且包装类型要么禁拷贝、要么把转移语义写清：一次持有只许有一个所有者。
	 * 纯 C 调用方没有析构可兜底，仍须显式 Free。
	 * 编码器经 `CLV_VideoEncoderAllocFrame` 交出的帧不落在解码器帧池里，计数归零即释放该缓冲；
	 * 那一次交出同样带状态闸，Free 只受理一次，重复归还是参数不成立。 */

	/* 加引用，返回加完后的计数；对 NULL 调用返回 0 且不崩溃。
	 * 已作废的帧不复活：一律返回 0，不加计数。 */
	CLV_EXPORT uint32_t CLV_VideoFrameAddRef(CLV_VideoFrame* f);

	/* 减一个引用（不回报的那扇门），与 CLV_VideoFrameAddRef 配对；计数归零时该帧回收进空闲池。
	 * 取帧口交出的那一次还没还时，本口先消耗它（那道状态闸随之落下，此后再调 Free 判参数不成立）。
	 * 对 NULL 调用是 no-op；对已作废或计数已归零的帧不做任何事。 */
	CLV_EXPORT void CLV_VideoFrameRelease(CLV_VideoFrame* f);

	/* 还一帧给库（带回报的那扇门），减的是取帧口加上的那一次计数，只受理一次。
	 * 返回 CLV_VIDEO_INVALID_ARGUMENT（参数不成立）的三档：传入为空、该帧已被重置或关闭作废、
	 * 取帧那一次持有已经还过（同一帧重复归还）。成功归还返回 CLV_VIDEO_OK。
	 * 重复归还的判点落在「取帧那一次还没还」这道状态闸上，不落在这帧的计数是否归零上：
	 * 还在参考缓冲里的帧计数本来就大于 1，只判归零会让第二次归还把被引用的帧收走。
	 * 归还之后该帧指针失效，建议立即置空；同一帧在复用前的重复归还判 CLV_VIDEO_INVALID_ARGUMENT（见上），跨复用后的陈旧调用属调用方未定义行为。 */
	CLV_EXPORT CLV_VideoError CLV_VideoFrameFree(CLV_VideoFrame* f);

	/* 取描述符与信息。desc / info 可为 NULL 表示不要该项。
	 * 描述符里的平面指针指向库拥有的缓冲且可写，只到该帧最后一个引用被归还为止有效。
	 * 已作废的帧（重置或关闭之后）一律返回参数不成立且不写出参。
	 * 帧句柄是非 const 的：可写视图不能从 const 对象里取出来。 */
	CLV_EXPORT CLV_VideoError CLV_VideoFrameGetDesc(CLV_VideoFrame* f, CLV_VideoFrameDescC* desc,
													CLV_VideoFrameInfoC* info);

	/* 帧平面 → RGB24：输出是 `3 * width * height` 字节的紧密缓冲，一行 `width` 个像素、
	 * 每像素 R G B 三道、**行间不留填充**。`width` / `height` 取描述符的 `display_*`，
	 * 为 0 时取 `coded_*`（与出帧裁剪同一口径）。
	 * 色度按 2×2 最近邻放大：像素 (r, c) 取色度 (r / 2, c / 2)，不做插值 —— 这条决定了
	 * 本函数与任何双线性色彩转换**不逐字节可比**，要一致就得用同一式。
	 * 增益式（BT.601 整数近似，移位一律向下取整，负值按算术移位 = 向负无穷）：
	 *   R = clip(Y + (1436 (V − 128) ≫ 10))
	 *   G = clip(Y − ((731 (U − 128) + 1285 (V − 128)) ≫ 10))
	 *   B = clip(Y + (1814 (U − 128) ≫ 10))
	 * 界内乘加上界 256 052，int32 不溢出；夹取域 [0, 255] 与 8-bit 像素域同宽。
	 * 缓冲前面接一个 PPM 头（`P6\n<width> <height>\n255\n`）即是一份标准 P6 图像，
	 * 可直接作为显示侧的位图入参，不必再经一层色彩转换库。
	 * 只支持 8-bit YUV420P：位深不是 8 走 `CLV_VIDEO_BIT_DEPTH_UNSUPPORTED`；
	 * `desc` / `dst` 为空、像素格式不成立、平面指针缺失、尺寸为 0、行距装不下宽 ⇒ 入参不成立，
	 * 前三种走 `CLV_VIDEO_INVALID_ARGUMENT`、尺寸走 `CLV_VIDEO_VALUE_RANGE`，**这些场合都不写 `dst`**。
	 * 本函数不分配，故无 `CLV_VIDEO_OUT_OF_MEMORY` 一支。 */
	CLV_EXPORT CLV_VideoError CLV_VideoFrameToRGB24(const CLV_VideoFrameDescC* desc, uint8_t* dst);

	/* 每个枚举值都返回非空、静态存储期的 ASCII 串；未知值返回 "unknown"。 */
	CLV_EXPORT const char* CLV_VideoStrError(CLV_VideoError err);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_VIDEO_H
