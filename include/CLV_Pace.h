/* CLV_Pace.h
 *
 * 呈现节器件（pacer）：把「音频设备侧消费量当主时钟、视频按 pts 追它」这套口径从播放器里抽出来。
 * 本头**不依赖任何播放器库**：设备侧只通过两个回调交进来（排队字节数、可选的单调钟），
 * 具体用哪个音频设备、怎么睡、怎么画，都由调用方决定。库自己既不碰线程也不碰设备。
 *
 * 主时钟口径（三句说完）：
 *   1) 已消费量 = 累计喂入字节 − 设备仍排队的字节；
 *   2) 设备在跑、而两次取数之间没有新消费时，按单调墙钟**线性外推**（设备放开后是匀速消费的，
 *      外推即真值；不外推的话读数颗粒等于设备的取数块长，本机实测约 100 ms，比一帧周期还粗）；
 *   3) 设备未放开（预灌期、暂停期）时**不外推**，钟位就停在实际已消费量上 —— 把预灌量算进已消费，
 *      钟会平白超前一段，实测形态是视频从第一帧起被一路判成迟到而连丢。
 *
 * 阈值只有 `period` 一个来源（= `fps_den / fps_num`），其余全是它的倍数，不给调用方随手调：
 *   对齐带 = |dev| ≤ period/2，等待阈 = dev > period/2，丢帧阈 = dev < −period。
 * 背压与预灌两个量是设备侧容量口径，可给；给 0 走库缺省（背压 1000 ms、预灌 200 ms 当量字节）。
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
		CLV_PACE_INVALID_ARGUMENT,	  /* 入参指针为空、参数结构体不合规、回调缺失 */
		CLV_PACE_VALUE_RANGE,		  /* 字段值域不成立：fps 分子分母为 0、字节率与采样参数自相矛盾等 */
		CLV_PACE_STATE_ERROR,		  /* 顺序不成立：未 BeginPass 就读钟 / 判定 / 置设备态 / 记喂入 */
		CLV_PACE_CLOCK_SKEW,		  /* 单调钟回退：本帧拿到的 now 比已登记的锚点还早，外推无从算起 */
		CLV_PACE_OUT_OF_MEMORY,		  /* 唯一的分配失败源是句柄本身：Open 判它并返 NULL */
	} CLV_PaceError;

	/* 设备侧回调：取「音频流里还没播出去的字节数」。返回 0 表示这一次问不出（设备已断等），
	 * 此时节器件沿用上一次登记过的已消费量，不把失败读成零。 */
	typedef uint8_t (*CLV_PaceQueuedFn)(void* user, uint64_t* out_queued_bytes);

	/* 可选的单调钟（纳秒）。给 NULL 时库内部用自己的单调钟。
	 * 由调用方供钟的唯一理由是可测：冒烟用例要拿假时钟走完整条时间轴。 */
	typedef uint64_t (*CLV_PaceNowFn)(void* user);

	/* 每份结构体的第一格恒为 `struct_size`，调用方填 `sizeof(*该结构)`：库据此认出调用方看到的是哪一版布局，
	 * 只认「不小于本版已知前缀」而不做等值判 —— 等值判会让尾部追加字段变成破坏性改动。
	 * 新字段一律尾部追加，不动已有格的位置与宽度；比本版小的 `struct_size` 判 `CLV_PACE_INVALID_ARGUMENT`。
	 *
	 * 本面的结构体自报字段只受理显式填入的本结构体长度，填 0 拒。理由：节拍面是新接口，没有按清零写的存量调用方；
	 * 「填 0 也受理」那一支原本是为兼容存量代码才留的，这里没有要兼容的对象，拒掉反而多一道布局校验。
	 * 因此本头是其余各头的例外：容器、视频、音频三头的同式口径（见 `CLV_Video.h` 参数结构体的混编说明）受理填 0，
	 * 本头不受理；比本版更大的显式长度仍按上面「不小于本版已知前缀」受理，被拒的只有 0 与小于本版的值。
	 * 从那些头迁过来的调用方，每一处都要显式填 `sizeof(*该结构)`。 */
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
		int32_t action;		/* 取 CLV_PaceActionC */
		uint8_t in_band;	/* |dev| ≤ period/2 */
		uint8_t late;		/* dev < 0（视频落后于音频钟） */
		uint8_t pad_;		/* 显式填充位，不留未初始化字节 */
		double dev_seconds; /* pts − 主时钟；正 = 视频早，负 = 视频迟 */
		double clock_seconds;
		double period_seconds;
		double wall_seconds; /* 本趟起播零点以来的真实时长：墙钟闸的当前值 */
		uint64_t wait_ns;	 /* 仅 WAIT 有效：等到落入对齐带所需的纳秒 */
	} CLV_PaceDecisionC;

	/* 建仓：按参数建一个节器件句柄。不成时返回 NULL 并把原因写进 *err（err 可空）。 */
	CLV_EXPORT CLV_Pace* CLV_PaceOpen(const CLV_PaceParamsC* params, CLV_PaceError* err);

	/* 拆仓。对 NULL 是 no-op；句柄拆后调用方不得再用。 */
	CLV_EXPORT void CLV_PaceClose(CLV_Pace* p);

	/* 起一趟：清累计喂入与实际已消费，并把外推锚点登记为「取钟那一刻」——
	 * 锚点若留在未初始化值上，第一次读钟会把整段程序运行时长外推进钟位。循环播放时每趟都要调。 */
	CLV_EXPORT CLV_PaceError CLV_PaceBeginPass(CLV_Pace* p);

	/* 设备是否放开：0 = 暂停/预灌（钟不推进），1 = 在播。BeginPass 之后初值为 0。 */
	CLV_EXPORT CLV_PaceError CLV_PaceSetDeviceRunning(CLV_Pace* p, uint8_t running);

	/* 记一笔喂进设备的字节（增量口径，可多次调）。 */
	CLV_EXPORT CLV_PaceError CLV_PaceNoteAudioFed(CLV_Pace* p, uint64_t bytes_added);

	/* 本趟主时钟当前秒数，**相对起播零点**（本趟第一次判定或第一次读钟的那一刻）。
	 * 未 BeginPass 就调判 CLV_PACE_STATE_ERROR。 */
	CLV_EXPORT CLV_PaceError CLV_PaceClockSeconds(CLV_Pace* p, double* out_seconds);

	/* 判这一帧：把 pts（秒）交进来，出动作与三个分类量。 */
	CLV_EXPORT CLV_PaceError CLV_PaceDecide(CLV_Pace* p, double video_pts_seconds, CLV_PaceDecisionC* out);

	/* 背压判定：*out_allowed = 0 表示设备里还堆着，先别喂。 */
	CLV_EXPORT CLV_PaceError CLV_PaceFeedAllowed(CLV_Pace* p, uint8_t* out_allowed);

	/* 每个错误码返回一句非空 ASCII 文案；未知值返回 "unknown"。 */
	CLV_EXPORT const char* CLV_PaceStrError(CLV_PaceError code);

#ifdef __cplusplus
}
#endif

#endif	  // CLV_PACE_H
