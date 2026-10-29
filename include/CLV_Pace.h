/* CLV_Pace.h
 *
 * 呈现节器件（pacer）：把「音频侧消费量当主时钟、视频按 pts 追它」这套口径从播放器里抽出来。
 * 本头**不依赖任何播放器库**：设备侧只通过回调交进来（排队字节数、可选的单调钟），主钟的数值则由消费端报告。
 * 具体用哪个音频设备、怎么睡、怎么画，都由调用方决定。库自己既不碰线程也不碰设备。
 *
 * 主时钟口径（三句说完）：
 *   1) 主钟 = 消费端报告的「已消费媒体时间」（`CLV_PaceNoteConsumedMediaTime`）。报告值按建仓给的时基计，
 *      库内只保证它单调朝前：回退的报告值不采信；
 *   2) 库**不外推、不封顶、不阻尼** —— 钟位的精度就是消费端的报告粒度，块状取数的设备两次报告之间会跳一格，
 *      这是器件形状，不是要库补的洞；设备不报，钟就停在原地，死队列保护层只报警不替设备编进度；
 *   3) 交出去的钟位与墙钟都从「第一次放开设备的那一刻」起算，预灌段不计入 —— 预灌量算进已消费，
 *      钟会平白超前一段，实测形态是视频从第一帧起被一路判成迟到而连丢。
 *
 * 阈值只有 `period` 一个来源（= `fps_den / fps_num`），不给调用方随手调：
 *   对齐带 = |dev| ≤ period/2，等待阈 = dev > period/2，丢帧阈 = dev < −(period + 实测最大报告步长)。
 * 报告步长是量出来的器件形状：块状取数的设备一次报告就跳一整块，那一格不算迟到。
 * 背压、预灌、积压上界三个量是设备侧容量口径，可给；给 0 走库缺省（背压 1000 ms、预灌 200 ms、积压上界 2000 ms 当量字节）。
 * 背压缺省取在实测的音频排队自然峰值之上：这道闸拦的是「喂」，但单线程读包里一等等住整条腿，
 * 视频出点在等待里过期就是成串丢帧 —— 阈值低于自然峰值时，丢帧是闸造成的，不是素材或解码成本。
 */
#ifndef CLV_PACE_H
#define CLV_PACE_H

#include "CLV_Export.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct CLV_Pace CLV_Pace;

	/* 节器件错误码。0 恒为成功；失败码从 96 起（承 FILE / CONTAINER / AUDIO 32.. / VIDEO 64..84 之后的新段），
	 * 段内只追加不改序、不复用。 */
	typedef enum CLV_PaceError
	{
		CLV_PACE_OK = 0,

		CLV_PACE_INVALID_HANDLE = 96, /* 句柄为空或已关闭 */
		CLV_PACE_INVALID_ARGUMENT,	  /* 入参指针为空、参数结构体不合规、回调缺失、减账报得比还排着的多 */
		CLV_PACE_VALUE_RANGE,		  /* 字段值域不成立：fps 分子分母为 0 或越界、采样参数自相矛盾、时基不整除 */
		CLV_PACE_STATE_ERROR,		  /* 顺序不成立：未 BeginPass 就读钟 / 判定 / 置设备态 / 记喂入 / 报消费量 */
		CLV_PACE_CLOCK_SKEW,		  /* 调用方供的单调钟回退：经过时长没有起算点，库重登记基准并只报这一次 */
		CLV_PACE_OUT_OF_MEMORY,		  /* 唯一的分配失败源是句柄本身：Open 判它并返 NULL */
	} CLV_PaceError;

	/* 设备侧回调：取「音频流里还没播出去的字节数」。返回 0 表示这一次问不出（设备已断等），
	 * 此时节器件沿用上一次登记过的读数、把积压水位报成 UNKNOWN，不把失败读成零。 */
	typedef uint8_t (*CLV_PaceQueuedFn)(void* user, uint64_t* out_queued_bytes);

	/* 可选的单调墙钟（纳秒）：只管「不许提前出画」那道闸与死队列计时，不参与主钟的数值。
	 * 给 NULL 时库内部用自己的单调钟。由调用方供钟的唯一理由是可测：冒烟用例要拿假时钟走完整条时间轴。 */
	typedef uint64_t (*CLV_PaceNowFn)(void* user);

	/* 每份结构体的第一格恒为 `struct_size`，调用方填 `sizeof(*该结构)`：库据此认出调用方看到的是哪一版布局。
	 * 受理三档：填 0、填上一版尺寸、填本版尺寸都收；填比上一版更小的非零值判 `CLV_PACE_INVALID_ARGUMENT`。
	 * 写侧按 `struct_size` 控制写入长度：旧尺寸只写前缀，绝不越界碰调用方 buffer 的尾部；
	 * 读侧逐字段按长度判读，缺失的新字段取库内缺省值，不整块拷贝调用方给的字节。
	 * 填 0 是「长度信息缺失」那一档，两侧都按最保守的上一版前缀处理 —— 存量按清零写的调用方可能只有前缀那么长。
	 * 比本版更大的显式长度照常受理（向前兼容更新版的调用方）。
	 * 「比上一版大、又比本版小」的显式长度（如 `ParamsC` 的 87、`DecisionC` 的 71）同样受理，并按「不足本版」处理：
	 * 尾部新字段不读也不写 —— 递交长度不足以容纳本版布局时，读或写尾部都会越进调用方没给的空间。
	 * 新字段一律尾部追加，不动已有格的位置与宽度。 */
	typedef struct CLV_PaceParamsC
	{
		uint32_t struct_size;
		uint32_t fps_num;			 /* 帧率分子，非 0 */
		uint32_t fps_den;			 /* 帧率分母，非 0；period = fps_den / fps_num 秒 */
		uint32_t sample_rate;		 /* 音频采样率，非 0 */
		uint32_t bytes_per_frame;	 /* 一个音频样本帧的字节数 = 声道数 × 每样本字节数，非 0 */
		uint64_t backpressure_bytes; /* 排队超过它就停止喂音频；0 = 库缺省 1000 ms 当量 */
		uint64_t preroll_bytes;		 /* 预灌目标，供调用方自查；0 = 库缺省 200 ms 当量。节器件自己不做预灌 */
		CLV_PaceQueuedFn queued;	 /* 必填 */
		CLV_PaceNowFn now;			 /* 可空 = 库内部单调钟 */
		void* queued_user;			 /* 随 queued 透传，可空 */
		void* now_user;				 /* 随 now 透传，可空 */
		uint64_t
			max_latency_bytes; /* 积压上界，水位按它分档；0 = 库缺省 2000 ms 当量 = 2 × sample_rate × bytes_per_frame */
		uint32_t timebase_num; /* 消费端报告媒体时间用的时基分子；0 = 缺省 1e9（纳秒，与内部单调钟同一把尺） */
		uint32_t
			timebase_den; /* 时基分母，0 同上；须整除分子，且分母与 fps 分子分母都不越过 1e6（越界判 VALUE_RANGE） */
	} CLV_PaceParamsC;

	/* 一次呈现判定的动作。 */
	typedef enum CLV_PaceActionC
	{
		CLV_PACE_ACT_WAIT = 0, /* 视频超前：等 wait_ns 再重判 */
		CLV_PACE_ACT_PRESENT,  /* 落在对齐带内，或迟到但未过丢帧阈 */
		CLV_PACE_ACT_DROP,	   /* 迟到超过一整个帧周期 */
	} CLV_PaceActionC;

	typedef struct CLV_PaceDecisionC
	{
		uint32_t struct_size;
		int32_t action;		  /* 取 CLV_PaceActionC */
		uint8_t in_band;	  /* |dev| ≤ period/2 */
		uint8_t late;		  /* dev < 0（视频落后于音频钟） */
		uint8_t pad_;		  /* 显式填充位，不留未初始化字节 */
		double dev_seconds;	  /* pts − 主时钟；正 = 视频早，负 = 视频迟 */
		double clock_seconds; /* 本趟主钟位，同样相对起播零点 */
		double period_seconds;
		double wall_seconds;		 /* 本趟起播零点以来的真实时长：「不许提前出画」那道闸的当前值 */
		uint64_t wait_ns;			 /* 仅 WAIT 有效：等到落入对齐带所需的纳秒 */
		uint32_t watermark;			 /* 积压水位：0=HEALTHY 1=PRESSURE 2=OVERFLOW 3=UNKNOWN（排队读数问不出） */
		uint32_t queue_ms;			 /* 当前积压毫秒，向下取整；UNKNOWN 时 0 */
		uint32_t resample_ratio_q16; /* 建议重采样倍率，Q16：65536 = 1.0；钳在 [0.95,1.05] = [62259,68813] */
		uint8_t device_jump;		 /* 设备跳变边沿：命中那次置 1，下一次判定清零，只持续一边沿 */
		uint8_t drop_dup_hint;		 /* 消费端速率匹配提示：0=正常 1=丢帧 2=重帧 */
	} CLV_PaceDecisionC;

	/* 建仓：按参数建一个节器件句柄。不成时返回 NULL 并把原因写进 *err（err 可空）。 */
	CLV_EXPORT CLV_Pace* CLV_PaceOpen(const CLV_PaceParamsC* params, CLV_PaceError* err);

	/* 拆仓。对 NULL 是 no-op；句柄拆后调用方不得再用。 */
	CLV_EXPORT void CLV_PaceClose(CLV_Pace* p);

	/* 起一趟：累计喂入、影子排队账、消费端报告值、起播零点与墙钟基准全部作废。循环播放时每趟都要调，
	 * 否则第二趟会继承第一趟的账 —— 实测形态是钟位凭空超前一段，从第一帧起一路被判定迟到而连丢。 */
	CLV_EXPORT CLV_PaceError CLV_PaceBeginPass(CLV_Pace* p);

	/* 设备是否放开：0 = 暂停/预灌，1 = 在播。BeginPass 之后初值为 0。
	 * 它不改钟位（主钟只跟报告走），管两件事：起播零点定在第一次放开的那一刻，死队列计时只在放开时走。 */
	CLV_EXPORT CLV_PaceError CLV_PaceSetDeviceRunning(CLV_Pace* p, uint8_t running);

	/* 记一笔喂进设备的字节（增量口径，可多次调）。它只进积压账，不进主钟。 */
	CLV_EXPORT CLV_PaceError CLV_PaceNoteAudioFed(CLV_Pace* p, uint64_t bytes_added);

	/* 主钟的供数口：消费端报告「已经消费掉的媒体时间」，单位 = 建仓时给的时基（缺省纳秒）。
	 * 主钟就等于这个报告值：不外推、不封顶、不阻尼；库内自证单调 —— 回退的报告值不采信，
	 * 且返回 `CLV_PACE_STATE_ERROR`；未 BeginPass 同样判 `CLV_PACE_STATE_ERROR`。
	 * 精度就是消费端的报告粒度（块边界）：块状取数的设备两次报告之间会跳一格，这是器件形状。 */
	CLV_EXPORT CLV_PaceError CLV_PaceNoteConsumedMediaTime(CLV_Pace* p, uint64_t media_ticks);

	/* 本趟主时钟当前秒数，**相对起播零点**（第一次放开设备的那一刻）。
	 * 未 BeginPass 就调判 `CLV_PACE_STATE_ERROR`；调用方供的钟回退那一次判 `CLV_PACE_CLOCK_SKEW`。 */
	CLV_EXPORT CLV_PaceError CLV_PaceClockSeconds(CLV_Pace* p, double* out_seconds);

	/* 减账口：调用方在把陈旧积压丢掉之前报一笔「交给设备前就丢弃的字节」。
	 * 必须先报后清：报账时按影子排队量校验，`bytes > queued` 判 `CLV_PACE_INVALID_ARGUMENT` 且三账不动；
	 * 未 BeginPass 判 `CLV_PACE_STATE_ERROR`。不变式：`fed′ = fed − bytes`、`queued′ = queued − bytes`、
	 * 已消费量不变、`queued′ ≥ 0`。设备自己跳变（重置/清空）造成的丢不由这里报。 */
	CLV_EXPORT CLV_PaceError CLV_PaceNoteAudioDropped(CLV_Pace* p, uint64_t bytes_dropped);

	/* 诊断量：死队列报警次数、主钟回退拒收次数、设备跳变边沿次数、自报减账累计。
	 * 四个都可空（只取要看的）。它们不参与主钟与判定，是给调用方与用例取证用的旁路量。 */
	CLV_EXPORT CLV_PaceError CLV_PaceStats(CLV_Pace* p, uint32_t* out_dead_queue_count, uint32_t* out_clock_regress,
										   uint32_t* out_jump_edges, uint64_t* out_dropped_bytes);

	/* 判这一帧：把 pts（秒）交进来，出动作、对齐带与迟到分类、积压水位与速率匹配建议。
	 * 出参按 `struct_size` 决定写多长：递交旧尺寸时只写前缀，尾部一个字节都不碰。 */
	CLV_EXPORT CLV_PaceError CLV_PaceDecide(CLV_Pace* p, double video_pts_seconds, CLV_PaceDecisionC* out);

	/* 背压判定：*out_allowed = 0 表示设备里还堆着，先别喂。 */
	CLV_EXPORT CLV_PaceError CLV_PaceFeedAllowed(CLV_Pace* p, uint8_t* out_allowed);

	/* 每个错误码返回一句非空 ASCII 文案；未知值返回 "unknown"。 */
	CLV_EXPORT const char* CLV_PaceStrError(CLV_PaceError code);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_PACE_H
