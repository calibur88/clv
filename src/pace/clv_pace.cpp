/* 节器件面的门面 + 实现，同一份源：这里没有格式判断，也没有设备与线程。
 *
 * 唯一的对外依赖是调用方交进来的回调（排队字节数、可选的单调钟）：库不碰任何播放器库、不开线程、不睡、不画。
 * 判定只交出「等什么、等多久」，怎么等由调用方执行 —— 于是这条口径能在没有声卡与窗口的用例里逐条测，
 * 示例与用例读的是同一份数。
 *
 * 三个口径各管一件事，别并成一谈：
 *   主时钟 = 设备已消费量（累计喂入 − 仍排队），两次取数之间按单调墙钟线性外推，外推顶到喂入上限为止；
 *   起播零点 = 本趟第一次判定那一刻的音频钟，交出去的钟位与趟龄都减它 —— 从放开设备到第一帧出画之间
 *     有一段固定的启动开销（本机实测解码 + 首帧呈现约 20 ms），不减掉的话视频腿一开场就天生低这一截，
 *     带内判定长期钉在带边上，攒到一整个周期外就开始连丢；
 *   墙钟 = 从起播零点起的真实时长，管的是「不许提前出画」。
 *
 * 异常不出 C 边界：本层唯一的抛出源是句柄分配，只在 Open 收口成 CLV_PACE_OUT_OF_MEMORY；
 * catch 只接 std::bad_alloc，逻辑错误一律走返回码。
 */
#include "CLV_Pace.h"

#include "status.h"

#include <chrono>
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

	/* 设备取数的颗粒比帧周期粗（本机实测约 100 ms 一跳），所以锚点之外要按墙钟外推。
	 * 各量的分工：fed_bytes 累计喂入（BeginPass 清零）；last_queued 最近一次问出来的仍排队字节
	 * （问不出就沿用，不把失败读成零）；anchor_* 上一次「设备真取走了新数据」的一刻与那时的已消费量；
	 * origin_* 本趟的起播零点，pass_start_ns 是它对应的墙钟时刻。 */
	struct ClockState
	{
		unsigned long long fed_bytes = 0;
		unsigned long long last_queued = 0;
		unsigned long long anchor_consumed = 0;
		unsigned long long anchor_now_ns = 0;
		unsigned long long pass_start_ns = 0;
		double origin_clock_s = 0.0;
		double last_abs_s = 0.0; /* 上一次的绝对钟位：主时钟只朝前走 */
		bool last_abs_valid = false;
		bool anchor_valid = false;
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
	double period = 0.0;
	double half = 0.0;
	double bps = 0.0;
};

namespace
{

	unsigned long long NowNs(const CLV_Pace& s)
	{
		if (s.cfg.now != nullptr) return static_cast<unsigned long long>(s.cfg.now(s.cfg.now_user));
		return static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch() /
											   std::chrono::nanoseconds(1));
	}

	bool QueryQueued(CLV_Pace& s, unsigned long long& out)
	{
		unsigned long long v = 0;
		if (s.cfg.queued == nullptr || s.cfg.queued(s.cfg.queued_user, &v) == 0) return false;
		out = v;
		s.st.last_queued = v;
		return true;
	}

	unsigned long long ConsumedBytes(CLV_Pace& s)
	{
		unsigned long long q = s.st.last_queued;
		(void) QueryQueued(s, q);
		return s.st.fed_bytes > q ? s.st.fed_bytes - q : 0ull;
	}

	/* 绝对钟位。两处起钟修正都落在这一层：
	 *   设备未放开 ⇒ 不外推，钟停在已消费量上（预灌期那段不计入已消费）
	 *   取到新消费 ⇒ 以这一刻重设锚点
	 *   锚点尚未登记 ⇒ 也走重设，绝不允许拿 now − 0 当外推时长（那会把整段程序运行时间灌进钟位） */
	PaceErr ClockAbsRaw(CLV_Pace& s, double& out_seconds)
	{
		if (! s.st.pass_started) return PaceErr::kStateError;
		const unsigned long long now = NowNs(s);
		const unsigned long long consumed = ConsumedBytes(s);

		if (! s.st.running || ! s.st.anchor_valid || consumed != s.st.anchor_consumed)
		{
			s.st.anchor_consumed = consumed;
			s.st.anchor_now_ns = now;
			s.st.anchor_valid = true;
			out_seconds = static_cast<double>(consumed) / s.bps;
			return PaceErr::kOk;
		}

		if (now < s.st.anchor_now_ns)
		{
			/* 调用方供的钟回退了：外推没有起算点，重登记锚点并只报实际已消费量。 */
			s.st.anchor_consumed = consumed;
			s.st.anchor_now_ns = now;
			out_seconds = static_cast<double>(consumed) / s.bps;
			return PaceErr::kClockSkew;
		}

		/* 外推顶到「已喂入」为止：设备播不出还没交给它的数据。放开这个上限，等待期间的钟会一路冲到并不存在
		 * 的数据上，下一次真实取位又把钟猛拉回来，dev 一步跨出丢帧阈 —— 假设备用例里 30 帧丢 20 帧就是这么来的。 */
		double est = static_cast<double>(consumed) + static_cast<double>(now - s.st.anchor_now_ns) * 1e-9 * s.bps;
		if (est > static_cast<double>(s.st.fed_bytes)) est = static_cast<double>(s.st.fed_bytes);
		out_seconds = est / s.bps;
		return PaceErr::kOk;
	}

	/* 交出去的钟位一律相对本趟起播零点：第一次调用（读钟或判定都行）把零点定在那一刻的绝对钟位，
	 * 并把墙钟起点同步到同一时刻。此后 dev 量的就只是两轨之间的漂移，不含「放开设备到第一帧出画」那段启动开销。 */
	PaceErr ClockRel(CLV_Pace& s, double& out_seconds)
	{
		double abs_s = 0.0;
		const PaceErr e = ClockAbsRaw(s, abs_s);
		/* 主时钟单调：设备按整块取数（本机 100 ms 一块），取数那一刻重锚会把已外推的相位丢掉，
		 * 钟就呈锯齿状往回跳 —— 每一跳都把 dev 顶成正的「视频超前」，于是每隔两三帧白等一次，
		 * 攒久了视频腿整体落后墙钟二十来毫秒，最后跨过丢帧阈。取位只认更大的那个值。 */
		if (s.st.last_abs_valid && abs_s < s.st.last_abs_s) abs_s = s.st.last_abs_s;
		s.st.last_abs_s = abs_s;
		s.st.last_abs_valid = true;
		if (e != PaceErr::kOk && e != PaceErr::kClockSkew) return e;
		if (! s.st.origin_set)
		{
			s.st.origin_set = true;
			s.st.origin_clock_s = abs_s;
			s.st.pass_start_ns = NowNs(s);
		}
		out_seconds = abs_s - s.st.origin_clock_s;
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
		/* 只认「不小于本版已知布局」：等值判会把尾部追加字段变成破坏性改动。 */
		if (params->struct_size < sizeof(CLV_PaceParamsC))
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

		handle->cfg = *params;
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

		if (err != nullptr) *err = CLV_PACE_OK;
		return handle;
	}

	CLV_EXPORT void CLV_PaceClose(CLV_Pace* p) { delete p; }

	/* 起一趟：累计喂入、外推锚点、起播零点与墙钟起点全部作废。循环播放时每趟都要调，
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
			p->st.origin_clock_s = static_cast<double>(ConsumedBytes(*p)) / p->bps;
			p->st.origin_set = true;
		}
		/* 放开与按住都作废外推锚点：暂停期间设备没消费，留着旧锚会把整段暂停时长外推进钟位。 */
		p->st.anchor_valid = false;
		return CLV_PACE_OK;
	}

	CLV_EXPORT CLV_PaceError CLV_PaceNoteAudioFed(CLV_Pace* p, uint64_t bytes_added)
	{
		if (p == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidHandle);
		if (! p->st.pass_started) return static_cast<CLV_PaceError>(PaceErr::kStateError);
		p->st.fed_bytes += bytes_added;
		return CLV_PACE_OK;
	}

	CLV_EXPORT CLV_PaceError CLV_PaceClockSeconds(CLV_Pace* p, double* out_seconds)
	{
		if (p == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidHandle);
		if (out_seconds == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidArgument);
		return static_cast<CLV_PaceError>(ClockRel(*p, *out_seconds));
	}

	CLV_EXPORT CLV_PaceError CLV_PaceDecide(CLV_Pace* p, double video_pts_seconds, CLV_PaceDecisionC* out)
	{
		if (p == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidHandle);
		if (out == nullptr) return static_cast<CLV_PaceError>(PaceErr::kInvalidArgument);
		if (out->struct_size < sizeof(CLV_PaceDecisionC)) return static_cast<CLV_PaceError>(PaceErr::kInvalidArgument);

		double clock = 0.0;
		const PaceErr e = ClockRel(*p, clock);
		if (e != PaceErr::kOk && e != PaceErr::kClockSkew) return static_cast<CLV_PaceError>(e);

		const double dev = video_pts_seconds - clock;
		out->dev_seconds = dev;
		out->clock_seconds = clock;
		out->period_seconds = p->period;
		out->in_band = (dev <= p->half && dev >= -p->half) ? 1u : 0u;
		out->wall_seconds = 0.0; /* 早于两道闸算出的值先给个确定的初值 */
		out->late = dev < 0.0 ? 1u : 0u;
		out->pad_ = 0u;

		/* 两道闸，各管一头：
		 *   墙钟闸（pts 还没到出点）⇒ 不许提前出画，否则视频计数跑在真实时间前面，整段先冲高再被甩下；
		 *   音频闸（dev > 0）⇒ 视频超前于音频钟，要压住；追到带心而不是带边，带边只用于统计 in_band。
		 * 等待取两者的较大值；出点已过而音频钟仍追不上时按出点出画，不拿一条追不上的钟把画面按住整段。 */
		const double wall_s = static_cast<double>(NowNs(*p) - p->st.pass_start_ns) * 1e-9;
		const double deadline_wait = video_pts_seconds > wall_s ? video_pts_seconds - wall_s : 0.0;
		const double audio_wait = dev > p->half ? dev - p->half : 0.0;
		out->wall_seconds = wall_s;
		if (dev < -p->period)
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
		return static_cast<CLV_PaceError>(e);
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
