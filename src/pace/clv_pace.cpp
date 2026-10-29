/* 节器件面的门面 + 实现，同一份源：这里没有格式判断，也没有设备与线程。
 *
 * 唯一的对外依赖是调用方交进来的回调（排队字节数、可选的单调钟）：库不碰任何播放器库、不开线程、不睡、不画。
 * 判定只交出「等什么、等多久」，怎么等由调用方执行 —— 于是这条口径能在没有声卡与窗口的用例里逐条测，
 * 示例与用例读的是同一份数。
 *
 * 三个口径各管一件事，别并成一谈：
 *   主时钟 = 消费端报告的「已消费媒体时间」（`CLV_PaceNoteConsumedMediaTime`）。库不外推、不封顶、不阻尼，
 *     只在内部保证单调：回退的报告值不采信并计入 `clock_regress`。设备不报，钟就停在原地 —— 那是死队列
 *     保护层要报警的形状，不是库要替设备编一个进度；
 *   起播零点 = 放开设备那一刻的报告值，交出去的钟位减它 —— 从放开设备到第一帧出画之间有一段固定的启动
 *     开销（本机实测解码 + 首帧呈现约 20 ms），不减掉的话视频腿一开场就天生低这一截；
 *   墙钟 = 从起播零点起的真实时长，只用于死队列判定与「不许提前出画」的出点闸，不再给主钟封顶。
 *
 * 异常不出 C 边界：本层唯一的抛出源是句柄分配，只在 Open 收口成 CLV_PACE_OUT_OF_MEMORY；
 * catch 只接 std::bad_alloc，逻辑错误一律走返回码。
 */
#include "CLV_Pace.h"

#include "status.h"

#include <chrono>
#include <cstddef>
#include <new>

namespace
{
	using clv::pace::PaceErr;

	/* 公开枚举与内部枚举逐项同序，门面直接转值。顺序错位在这里编译期就断掉。 */
	static_assert(static_cast<int>(PaceErr::kInvalidHandle) == CLV_PACE_INVALID_HANDLE, "内部码与公开码同序");
	static_assert(static_cast<int>(PaceErr::kInvalidArgument) == CLV_PACE_INVALID_ARGUMENT, "内部码与公开码同序");
	static_assert(static_cast<int>(PaceErr::kValueRange) == CLV_PACE_VALUE_RANGE, "内部码与公开码同序");
	static_assert(static_cast<int>(PaceErr::kStateError) == CLV_PACE_STATE_ERROR, "内部码与公开码同序");
	static_assert(static_cast<int>(PaceErr::kClockSkew) == CLV_PACE_CLOCK_SKEW, "内部码与公开码同序");
	static_assert(static_cast<int>(PaceErr::kOutOfMemory) == CLV_PACE_OUT_OF_MEMORY, "内部码与公开码同序");

	/* 主钟一侧只读 reported / applied / max_report_step；字节账（fed / dropped / shadow_queued / pending_clear）
	 * 管水位、超扣与减账不变式，不进主钟；墙钟一侧（pass_start / last_now / now_seen）管出点闸与死队列计时。 */
	struct ClockState
	{
		unsigned long long fed_bytes = 0;
		unsigned long long dropped_bytes = 0;
		unsigned long long shadow_queued = 0;
		unsigned long long last_queued = 0;
		unsigned long long pending_clear = 0;
		unsigned long long reported_ticks = 0;
		unsigned long long applied_ticks = 0;
		unsigned long long max_report_step_ticks = 0;
		unsigned long long last_seen_report = 0;
		unsigned long long last_progress_ns = 0;
		unsigned long long pass_start_ns = 0;
		unsigned long long last_now_ns = 0;
		unsigned no_progress_streak = 0;
		unsigned dead_queue_count = 0;
		unsigned clock_regress = 0;
		unsigned jump_edge_count = 0;
		double origin_clock_s = 0.0;
		bool reported_valid = false;
		bool queue_reading_valid = false;
		bool jump_edge_pending = false;
		bool now_seen = false;
		bool origin_set = false;
		bool running = false;
		bool pass_started = false;
	};

}	 // namespace

struct CLV_Pace
{
	CLV_PaceParamsC cfg;
	ClockState st;
	unsigned long long bytes_per_second = 0;
	unsigned long long max_latency_bytes = 0;
	unsigned long long max_normal_chunk_bytes = 0;
	double period = 0.0;
	double half = 0.0;
	double bps = 0.0;
	double tick_to_sec = 1e-9; /* 报告时基换算：1 / (timebase_num/timebase_den) */
};

namespace
{
	static_assert(sizeof(CLV_PaceParamsC) == 88u, "本版布局尺寸就是受理判定的尺子，尾部追加字段时必须同步上一版常量");
	static_assert(sizeof(CLV_PaceDecisionC) == 72u, "同上：写侧按这个长度决定能写到哪一停");
	/* 前缀的偏移同样是公开承诺：旧尺寸调用方按它读，发行文档那张表也按它写。 */
	static_assert(offsetof(CLV_PaceDecisionC, struct_size) == 0u, "自报长度恒在第一格");
	static_assert(offsetof(CLV_PaceDecisionC, action) == 4u, "动作紧随自报长度");
	static_assert(offsetof(CLV_PaceDecisionC, in_band) == 8u, "三个分类字节排在动作之后");
	static_assert(offsetof(CLV_PaceDecisionC, late) == 9u, "迟到位紧随带内位");
	static_assert(offsetof(CLV_PaceDecisionC, pad_) == 10u, "显式填充位是本面唯一的那个 pad");
	static_assert(offsetof(CLV_PaceDecisionC, dev_seconds) == 16u, "四个 double 从 8 字节边界起排");
	static_assert(offsetof(CLV_PaceDecisionC, clock_seconds) == 24u, "钟位紧随偏差");
	static_assert(offsetof(CLV_PaceDecisionC, period_seconds) == 32u, "帧周期紧随钟位");
	static_assert(offsetof(CLV_PaceDecisionC, wall_seconds) == 40u, "墙龄紧随帧周期");
	static_assert(offsetof(CLV_PaceDecisionC, wait_ns) == 48u, "等待量是旧布局的末格");
	/* 尾部偏移是公开承诺：旧尺寸调用方的前缀边界、新字段可读起点都按这几个数判。 */
	static_assert(offsetof(CLV_PaceDecisionC, watermark) == 56u, "水位量起点 = 旧布局末尾");
	static_assert(offsetof(CLV_PaceDecisionC, queue_ms) == 60u, "积压毫秒紧跟水位");
	static_assert(offsetof(CLV_PaceDecisionC, resample_ratio_q16) == 64u, "倍率排在两个 u8 之前，中段不留占位");
	static_assert(offsetof(CLV_PaceDecisionC, device_jump) == 68u, "边沿位在倍率之后");
	static_assert(offsetof(CLV_PaceDecisionC, drop_dup_hint) == 69u, "提示紧随边沿；70–71 是隐式填充，库不写它");
	const unsigned long long kParamsPrevSize = 72ull; /* 尾部追加积压上界与时基之前的布局 */
	const unsigned kDecisionPrevSize = 56u;			  /* 尾部追加水位量之前的布局 */
	const unsigned long long kFpsLimit = 1000000ull; /* 分子分母的上界：下面那条 `base × 分母` 要在 64 位里可证不溢出 */

	unsigned long long NowNs(const CLV_Pace& s)
	{
		if (s.cfg.now != nullptr) return static_cast<unsigned long long>(s.cfg.now(s.cfg.now_user));
		return static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch() /
											   std::chrono::nanoseconds(1));
	}

	bool QueryQueued(CLV_Pace& s, unsigned long long& out)
	{
		unsigned long long v = 0;
		if (s.cfg.queued == nullptr || s.cfg.queued(s.cfg.queued_user, &v) == 0)
		{
			s.st.queue_reading_valid = false;
			return false;
		}
		s.st.queue_reading_valid = true;
		out = v;
		s.st.last_queued = v;
		return true;
	}

	/* 影子对平：设备读数是事实，但报过账的那批还没从设备侧消失时读数里仍然含着它，抵掉才是真积压 ——
	 * 加上去就把同一批字节算两遍。抵不掉的差额只能是设备自己清的，交一次边沿，主钟不动。 */
	void PollQueue(CLV_Pace& s)
	{
		const unsigned long long prev = s.st.last_queued;
		unsigned long long raw = prev;
		if (! QueryQueued(s, raw)) return;	  // 问不出就沿上次的读数，水位另判 UNKNOWN
		const unsigned long long fall = prev > raw ? prev - raw : 0ull;
		const unsigned long long absorb = fall < s.st.pending_clear ? fall : s.st.pending_clear;
		s.st.pending_clear -= absorb;
		s.st.shadow_queued = raw > s.st.pending_clear ? raw - s.st.pending_clear : 0ull;
		if (fall - absorb > s.cfg.backpressure_bytes && fall - absorb > s.max_normal_chunk_bytes)
		{
			s.st.jump_edge_pending = true;
			s.st.jump_edge_count++;
		}
	}

	/* 基准在前才有差值：钟回退过就取 0，而不是让无符号相减回绕成几百年 */
	unsigned long long DeltaNs(unsigned long long now_ns, unsigned long long base_ns)
	{
		return now_ns > base_ns ? now_ns - base_ns : 0ull;
	}

	/* 取一次墙钟。回退 ⇒ 把墙钟基准挪到这一刻并返回 CLOCK_SKEW，交出去的钟位本身不受影响：
	 * 主钟只跟消费端的报告值走，墙钟只管出点闸与死队列计时。 */
	PaceErr TakeNow(CLV_Pace& s, unsigned long long& out_ns)
	{
		const unsigned long long now = NowNs(s);
		const PaceErr e = (s.st.now_seen && now < s.st.last_now_ns) ? PaceErr::kClockSkew : PaceErr::kOk;
		if (e == PaceErr::kClockSkew)
		{
			s.st.pass_start_ns = now;
			s.st.last_progress_ns = now;
		}
		s.st.now_seen = true;
		s.st.last_now_ns = now;
		out_ns = now;
		return e;
	}

	/* 绝对钟位 = 消费端报告的已消费媒体时间，库内只保证单调。
	 * 不封顶、不外推、不阻尼：设备不报，钟就停 —— 死队列保护会报警，库不替设备编进度。 */
	PaceErr ClockAbsRaw(CLV_Pace& s, double& out_seconds)
	{
		if (! s.st.pass_started) return PaceErr::kStateError;
		PollQueue(s); /* 影子对平与跳变边沿在这一拍走一遍：水位、闸门与超扣校验读的都是这套账 */
		if (s.st.reported_ticks > s.st.applied_ticks) s.st.applied_ticks = s.st.reported_ticks;
		out_seconds = static_cast<double>(s.st.applied_ticks) * s.tick_to_sec;
		return PaceErr::kOk;
	}

	/* 交出去的钟位一律相对本趟起播零点：零点取「放开设备那一刻的报告值」，与主钟同一条源，
	 * 不掺字节账 —— 两条不同源时，相对钟位会算出负值。 */
	PaceErr ClockRel(CLV_Pace& s, unsigned long long now_ns, double& out_seconds)
	{
		double abs_s = 0.0;
		const PaceErr e = ClockAbsRaw(s, abs_s);
		if (e != PaceErr::kOk) return e;
		if (! s.st.origin_set)
		{
			s.st.origin_set = true;
			s.st.origin_clock_s = abs_s;
			s.st.pass_start_ns = now_ns;
			s.st.last_progress_ns = now_ns;
		}
		out_seconds = abs_s - s.st.origin_clock_s;
		if (out_seconds < 0.0) out_seconds = 0.0;
		return e;
	}

}	 // namespace

extern "C"
{

	CLV_EXPORT CLV_Pace* CLV_PaceOpen(const CLV_PaceParamsC* params, CLV_PaceError* err)
	{
		auto report = [err](PaceErr e)
		{
			if (err != nullptr) *err = static_cast<CLV_PaceError>(e);
		};

		if (params == nullptr)
		{
			report(PaceErr::kInvalidArgument);
			return nullptr;
		}
		/* 三档受理：0、上一版尺寸、本版尺寸都收；非零且比上一版小才拒。
		 * 按长度逐字段判读，不整块拷贝 —— 整块拷会读进旧调用方 buffer 之外的字节。 */
		if (params->struct_size != 0u && params->struct_size < kParamsPrevSize)
		{
			report(PaceErr::kInvalidArgument);
			return nullptr;
		}
		if (params->queued == nullptr)
		{
			report(PaceErr::kInvalidArgument);
			return nullptr;
		}
		if (params->fps_num == 0u || params->fps_den == 0u || params->sample_rate == 0u ||
			params->bytes_per_frame == 0u)
		{
			report(PaceErr::kValueRange);
			return nullptr;
		}
		if (params->sample_rate > 0x1000000u || params->bytes_per_frame > 0x100u)
		{
			report(PaceErr::kValueRange);
			return nullptr;
		}
		/* 分子分母都不许越过 1e6：`base × 分母` 要在 64 位里可证不溢出。
		 * timebase 是本版尾部追加的字段，递交长度不够时不去读它——读就越界。 */
		if (params->fps_num > kFpsLimit || params->fps_den > kFpsLimit)
		{
			report(PaceErr::kValueRange);
			return nullptr;
		}

		CLV_Pace* handle = nullptr;
		try
		{
			handle = new CLV_Pace();
		}
		catch (const std::bad_alloc&)
		{
			report(PaceErr::kOutOfMemory);
			return nullptr;
		}

		handle->cfg.struct_size = sizeof(CLV_PaceParamsC);
		handle->cfg.fps_num = params->fps_num;
		handle->cfg.fps_den = params->fps_den;
		handle->cfg.sample_rate = params->sample_rate;
		handle->cfg.bytes_per_frame = params->bytes_per_frame;
		handle->cfg.backpressure_bytes = params->backpressure_bytes;
		handle->cfg.preroll_bytes = params->preroll_bytes;
		handle->cfg.queued = params->queued;
		handle->cfg.now = params->now;
		handle->cfg.queued_user = params->queued_user;
		handle->cfg.now_user = params->now_user;
		/* 尾部新字段：只有递交长度够时才读，否则取库内缺省。 */
		handle->cfg.max_latency_bytes = 0ull;
		handle->cfg.timebase_num = 0u;
		handle->cfg.timebase_den = 0u;
		if (params->struct_size >= sizeof(CLV_PaceParamsC))
		{
			handle->cfg.max_latency_bytes = params->max_latency_bytes;
			handle->cfg.timebase_num = params->timebase_num;
			handle->cfg.timebase_den = params->timebase_den;
		}
		if (handle->cfg.timebase_num == 0u || handle->cfg.timebase_den == 0u)
		{
			handle->cfg.timebase_num = 1000000000u; /* 缺省时基 = 纳秒，与内部单调钟同一把尺 */
			handle->cfg.timebase_den = 1u;
		}
		/* 时基要能整除，并且 `时基 × 帧率分母` 在 64 位里可证不溢出：这才是真正需要防的乘法。
		 * 不去给时基本身设绝对上限——纳秒时基就是 1e9，任何小于它的常数都会把缺省值判死。 */
		const unsigned long long tb = handle->cfg.timebase_num / handle->cfg.timebase_den;
		if (handle->cfg.timebase_den > kFpsLimit || handle->cfg.timebase_num % handle->cfg.timebase_den != 0u ||
			tb > 0xFFFFFFFFFFFFFFFFull / (kFpsLimit * 2ull))
		{
			delete handle;
			report(PaceErr::kValueRange);
			return nullptr;
		}
		handle->tick_to_sec =
			static_cast<double>(handle->cfg.timebase_den) / static_cast<double>(handle->cfg.timebase_num);
		handle->bytes_per_second = static_cast<unsigned long long>(params->sample_rate) * params->bytes_per_frame;
		handle->bps = static_cast<double>(handle->bytes_per_second);
		handle->period = static_cast<double>(params->fps_den) / static_cast<double>(params->fps_num);
		handle->half = handle->period / 2.0;
		/* 背压缺省 = 1000 ms 当量。这道闸拦住的是「喂」，可单线程读包里一等等住的是整条腿：视频帧的
		 * 出点在等待里过期，醒来就是成串丢帧。实测一份 30 fps / 44.1 kHz 双轨素材的音频排队自然峰值
		 * 598 ms（音频包成串到达，不按匀速来），500 ms 的阈值正卡在峰值上 —— 一堵 90~114 ms，连丢三帧。
		 * 缺省要落在自然峰值之上；它仍是内存上界，只是把上界放到一秒钟的音频当量。 */
		if (handle->cfg.backpressure_bytes == 0ull) handle->cfg.backpressure_bytes = handle->bytes_per_second;
		if (handle->cfg.preroll_bytes == 0ull) handle->cfg.preroll_bytes = handle->bytes_per_second / 5ull;
		handle->max_latency_bytes = handle->cfg.max_latency_bytes != 0ull ?
										handle->cfg.max_latency_bytes :
										handle->bytes_per_second * 2ull;   /* 缺省 2000 ms 当量 */
		handle->max_normal_chunk_bytes = handle->bytes_per_second / 10ull; /* 一记取数块 = 100 ms 当量 */

		if (err != nullptr) *err = CLV_PACE_OK;
		return handle;
	}

	CLV_EXPORT void CLV_PaceClose(CLV_Pace* p) { delete p; }

	/* 起一趟：累计喂入、影子排队账、报告值、起播零点与墙钟起点全部作废。循环播放时每趟都要调，
	 * 否则第二趟会继承第一趟的残留（实测形态：钟位凭空超前 ~53 ms，从第一帧起一路被判定迟到而连丢）。 */
	CLV_EXPORT CLV_PaceError CLV_PaceBeginPass(CLV_Pace* p)
	{
		if (p == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidHandle);
		p->st = ClockState {};
		p->st.pass_started = true;
		return CLV_PACE_OK;
	}

	CLV_EXPORT CLV_PaceError CLV_PaceSetDeviceRunning(CLV_Pace* p, uint8_t running)
	{
		if (p == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidHandle);
		if (! p->st.pass_started) return static_cast<CLV_PaceError>(PaceErr::kStateError);
		const bool was_running = p->st.running;
		p->st.running = running != 0u;
		/* 放开设备的那一刻就是本趟的统一基准：墙钟起点与音频钟零点都定在这里。
		 * 把零点留给「第一次判定」会留下一个真实的错位 —— 设备从放开起就开始计位，
		 * 而第一帧还要解码十几到几十毫秒，于是钟位长期高于墙钟这一截，带内档永远等不到出点。 */
		/* 零点每趟只定一次：恢复播放不能重定，否则暂停前后之间钟会自己归零。 */
		if (p->st.running && ! was_running && ! p->st.origin_set)
		{
			p->st.pass_start_ns = NowNs(*p);
			/* 零点与主钟同一条源：取此刻的报告值。换成别的量起算，相对钟位就可能被判成负。 */
			p->st.origin_clock_s = static_cast<double>(p->st.applied_ticks) * p->tick_to_sec;
			p->st.origin_set = true;
			p->st.last_progress_ns = p->st.pass_start_ns;
		}
		/* 这个开关不改钟位：只管起播零点定在第一次放开的那一刻，以及死队列计时只在放开时走。 */
		return CLV_PACE_OK;
	}

	CLV_EXPORT CLV_PaceError CLV_PaceNoteAudioFed(CLV_Pace* p, uint64_t bytes_added)
	{
		if (p == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidHandle);
		if (! p->st.pass_started) return static_cast<CLV_PaceError>(PaceErr::kStateError);
		p->st.fed_bytes += bytes_added;
		p->st.shadow_queued += bytes_added; /* 交进流里的字节先记在影子账上，等设备的排队读数来对平 */
		return CLV_PACE_OK;
	}

	CLV_EXPORT CLV_PaceError CLV_PaceNoteConsumedMediaTime(CLV_Pace* p, uint64_t media_ticks)
	{
		if (p == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidHandle);
		if (! p->st.pass_started) return static_cast<CLV_PaceError>(PaceErr::kStateError);
		if (p->st.reported_valid && media_ticks < p->st.reported_ticks)
		{
			p->st.clock_regress++; /* 不采信，只记一笔拒收 */
			return static_cast<CLV_PaceError>(PaceErr::kStateError);
		}
		if (p->st.reported_valid && media_ticks > p->st.reported_ticks)
		{
			const unsigned long long step = media_ticks - p->st.reported_ticks;
			if (step > p->st.max_report_step_ticks) p->st.max_report_step_ticks = step;
		}
		p->st.reported_ticks = media_ticks;
		p->st.reported_valid = true;
		return CLV_PACE_OK;
	}

	/* 减账口：先报后清。校验取影子排队量，与设备清队的先后无关。 */
	CLV_EXPORT CLV_PaceError CLV_PaceNoteAudioDropped(CLV_Pace* p, uint64_t bytes_dropped)
	{
		if (p == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidHandle);
		if (! p->st.pass_started) return static_cast<CLV_PaceError>(PaceErr::kStateError);
		(void) PollQueue(*p);
		if (bytes_dropped > p->st.shadow_queued) return static_cast<CLV_PaceError>(PaceErr::kInvalidArgument);
		p->st.fed_bytes -= bytes_dropped;
		p->st.shadow_queued -= bytes_dropped;
		p->st.dropped_bytes += bytes_dropped;
		p->st.pending_clear += bytes_dropped; /* 等设备的排队读数掉下来时抵掉，不重复计成已消费 */
		return CLV_PACE_OK;
	}

	/* 诊断量一次交出：死队列报警次数、回退拒收次数、跳变边沿次数、自报减账累计。
	 * 这些都是旁路量，不参与主钟与判定。 */
	CLV_EXPORT CLV_PaceError CLV_PaceStats(CLV_Pace* p, uint32_t* out_dead_queue_count, uint32_t* out_clock_regress,
										   uint32_t* out_jump_edges, uint64_t* out_dropped_bytes)
	{
		if (p == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidHandle);
		if (out_dead_queue_count != nullptr) *out_dead_queue_count = p->st.dead_queue_count;
		if (out_clock_regress != nullptr) *out_clock_regress = p->st.clock_regress;
		if (out_jump_edges != nullptr) *out_jump_edges = p->st.jump_edge_count;
		if (out_dropped_bytes != nullptr) *out_dropped_bytes = p->st.dropped_bytes;
		return CLV_PACE_OK;
	}

	CLV_EXPORT CLV_PaceError CLV_PaceClockSeconds(CLV_Pace* p, double* out_seconds)
	{
		if (p == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidHandle);
		if (out_seconds == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidArgument);
		unsigned long long now_ns = 0ull;
		const PaceErr sk = TakeNow(*p, now_ns);
		double sec = 0.0;
		const PaceErr e = ClockRel(*p, now_ns, sec);
		if (e != PaceErr::kOk) return static_cast<CLV_PaceError>(e);
		*out_seconds = sec;
		return static_cast<CLV_PaceError>(sk);
	}

	CLV_EXPORT CLV_PaceError CLV_PaceDecide(CLV_Pace* p, double video_pts_seconds, CLV_PaceDecisionC* out)
	{
		if (p == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidHandle);
		if (out == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidArgument);
		/* 三档受理 + 按长度控写：非零且比上一版小才拒；旧尺寸只写前缀，尾部一个字节都不碰。 */
		if (out->struct_size != 0u && out->struct_size < kDecisionPrevSize)
			return static_cast<CLV_PaceError>(PaceErr::kInvalidArgument);
		const bool write_tail = out->struct_size >= sizeof(CLV_PaceDecisionC);

		/* 一拍只取一次钟：出点闸与死队列计时用的是同一个 now，取两次就会把这一拍的间隔算进出点里。 */
		unsigned long long now_ns = 0ull;
		const PaceErr skew = TakeNow(*p, now_ns);
		double clock = 0.0;
		const PaceErr ce = ClockRel(*p, now_ns, clock);
		if (ce != PaceErr::kOk) return static_cast<CLV_PaceError>(ce);

		const double dev = video_pts_seconds - clock;
		out->dev_seconds = dev;
		out->clock_seconds = clock;
		out->period_seconds = p->period;
		out->in_band = (dev <= p->half && dev >= -p->half) ? 1u : 0u;
		out->wall_seconds = 0.0; /* 早于两道闸算出的值先给个确定的初值 */
		out->late = dev < 0.0 ? 1u : 0u;
		out->pad_ = 0u;

		/* 死队列保护：设备在播而连续 3 次判定或 500 ms 无进展（取先到）⇒ 计数，不自动重锚。
		 * 「一条都没报」与「报同一个值」是同一个形状；按住的那段不计，那时候没有该有进展的预期。 */
		if (p->st.running && p->st.reported_ticks == p->st.last_seen_report)
		{
			if (++p->st.no_progress_streak >= 3u ||
				(p->st.last_progress_ns != 0ull && DeltaNs(now_ns, p->st.last_progress_ns) >= 500000000ull))
			{
				p->st.dead_queue_count++;
				p->st.no_progress_streak = 0u;
				p->st.last_progress_ns = now_ns;
			}
		}
		else
		{
			p->st.last_seen_report = p->st.reported_ticks;
			p->st.no_progress_streak = 0u;
			p->st.last_progress_ns = now_ns;
		}

		/* 两道闸，各管一头：
		 *   墙钟闸（pts 还没到出点）⇒ 不许提前出画，否则视频计数跑在真实时间前面，整段先冲高再被甩下；
		 *   音频闸（dev > 0）⇒ 视频超前于主钟，要压住；追到带心而不是带边，带边只用于统计 in_band。
		 * 等待取两者的较大值；出点已过而主钟仍追不上时按出点出画，不拿一条追不上的钟把画面按住整段。 */
		const double wall_s = static_cast<double>(DeltaNs(now_ns, p->st.pass_start_ns)) * 1e-9;
		const double deadline_wait = video_pts_seconds > wall_s ? video_pts_seconds - wall_s : 0.0;
		const double audio_wait = dev > p->half ? dev - p->half : 0.0;
		out->wall_seconds = wall_s;
		/* 丢帧阈 = 一个帧周期 + 实测最大报告步长：块状取数的设备两次报告之间本就跳一格，
		 * 那一格不是迟到。步长是量出来的器件形状，不是拍的常数。 */
		const double drop_thresh = p->period + static_cast<double>(p->st.max_report_step_ticks) * p->tick_to_sec;
		if (dev < -drop_thresh)
		{
			out->action = CLV_PACE_ACT_DROP;
			out->wait_ns = 0ull;
		}

		else if (deadline_wait > 0.0 || audio_wait > 0.0)
		{
			const double w = deadline_wait > audio_wait ? deadline_wait : audio_wait;
			out->action = CLV_PACE_ACT_WAIT;
			out->wait_ns = static_cast<uint64_t>(w * 1e9);
		}
		else
		{
			out->action = CLV_PACE_ACT_PRESENT;
			out->wait_ns = 0ull;
		}

		if (write_tail)
		{
			const unsigned long long q = p->st.shadow_queued;
			const double q_ms = p->bps > 0.0 ? static_cast<double>(q) * 1000.0 / p->bps : 0.0;
			const bool unknown = ! p->st.queue_reading_valid; /* 没给回调与这一次问不出，都算问不出 */
			out->watermark =
				unknown ? 3u : (q > p->max_latency_bytes ? 2u : (q > p->max_latency_bytes / 2ull ? 1u : 0u));
			out->queue_ms =
				unknown ? 0u : static_cast<uint32_t>(q_ms < 0.0 ? 0.0 : (q_ms > 4294967.0 ? 4294967.0 : q_ms));
			out->device_jump = p->st.jump_edge_pending ? 1u : 0u;
			p->st.jump_edge_pending = false; /* 边沿只交一次 */
			/* 速率匹配提示按内容偏差给：视频落后主钟超过一周期 ⇒ 丢；超前超过两周期 ⇒ 重帧。 */
			out->drop_dup_hint = dev < -p->period ? 1u : (dev > 2.0 * p->period ? 2u : 0u);
			/* 尾部对齐用的两个字节是隐式填充：库不写它，也不承诺它的值。 */
			/* 建议倍率 = 队列偏差的比例校正，钳在 ±5 %：积压高于带心就催快，低于就压慢。 */
			const double target_bytes = static_cast<double>(p->max_latency_bytes) / 2.0;
			double cmd = 1.0 + (target_bytes > 0.0 ? (q - target_bytes) / target_bytes * 0.1 : 0.0);
			if (cmd < 0.95) cmd = 0.95;
			if (cmd > 1.05) cmd = 1.05;
			out->resample_ratio_q16 = static_cast<uint32_t>(cmd * 65536.0 + 0.5);
		}
		return static_cast<CLV_PaceError>(skew);
	}

	CLV_EXPORT CLV_PaceError CLV_PaceFeedAllowed(CLV_Pace* p, uint8_t* out_allowed)
	{
		if (p == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidHandle);
		if (out_allowed == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidArgument);
		unsigned long long q = p->st.last_queued;
		(void) QueryQueued(*p, q);
		*out_allowed = q <= p->cfg.backpressure_bytes ? 1u : 0u;
		return CLV_PACE_OK;
	}

	CLV_EXPORT const char* CLV_PaceStrError(CLV_PaceError code)
	{
		switch (code)
		{
		case CLV_PACE_OK: return "ok";
		case CLV_PACE_INVALID_HANDLE: return "invalid handle";
		case CLV_PACE_INVALID_ARGUMENT: return "invalid argument";
		case CLV_PACE_VALUE_RANGE: return "value out of range";
		case CLV_PACE_STATE_ERROR: return "state error";
		case CLV_PACE_CLOCK_SKEW: return "clock skew";
		case CLV_PACE_OUT_OF_MEMORY: return "out of memory";
		default: return "unknown";
		}
	}

}	 // extern "C"
