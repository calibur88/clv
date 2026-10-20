/* 节器件面的公开面用例：只吃 include/CLV_Pace.h，不碰实现层头。
 *
 * 假设备 + 假时钟：设备按「每 100 ms 取走一整块」这种粗颗粒消费（本机真实设备的形状），
 * 时钟由测试供给，于是外推、暂停不推进、锚点复位这些口径都能 deterministic 地钉住。
 */
#include "CLV_Pace.h"

#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <string>

namespace
{

	struct FakeDevice
	{
		uint64_t now_ns = 0;
		uint64_t fed = 0;
		uint64_t played = 0;	// 已播出去的字节
		uint64_t bytes_per_second = 0;
		uint64_t pull_period_ns = 100'000'000ull;	 // 设备一次取走 100 ms 当量
		uint64_t next_pull_ns = 100'000'000ull;
		bool query_fails = false;

		uint64_t queued() const { return fed - played; }

		void AdvanceTo(uint64_t t_ns)
		{
			now_ns = t_ns;
			while (t_ns >= next_pull_ns)
			{
				const uint64_t chunk = bytes_per_second / 10ull;	// 100 ms 当量
				const uint64_t room = fed > played ? fed - played : 0ull;
				const uint64_t take = chunk < room ? chunk : room;
				played += take;
				next_pull_ns += pull_period_ns;
			}
		}
	};

	uint8_t QueryQueued(void* user, uint64_t* out)
	{
		auto* d = static_cast<FakeDevice*>(user);
		if (d->query_fails) return 0;
		*out = d->queued();
		return 1;
	}

	uint64_t NowNs(void* user) { return static_cast<FakeDevice*>(user)->now_ns; }

	/* 30 fps / 44.1 kHz / 2 ch / s16 的缺省形：与双轨示例件同一组数字，读数才对得上器件。 */
	CLV_PaceParamsC MakeParams(FakeDevice& d, uint32_t fps_num = 30, uint32_t fps_den = 1)
	{
		CLV_PaceParamsC p;
		std::memset(&p, 0, sizeof(p));
		p.struct_size = sizeof(p);
		p.fps_num = fps_num;
		p.fps_den = fps_den;
		p.sample_rate = 44100u;
		p.bytes_per_frame = 4u;
		d.bytes_per_second = 44100ull * 4ull;
		p.queued = &QueryQueued;
		p.now = &NowNs;
		p.queued_user = &d;
		p.now_user = &d;
		return p;
	}

	class PaceApiTest: public ::testing::Test
	{
		protected:

		FakeDevice dev;
		CLV_Pace* h = nullptr;

		void OpenOk()
		{
			CLV_PaceError e = CLV_PACE_OUT_OF_MEMORY;
			const CLV_PaceParamsC p = MakeParams(dev);
			h = CLV_PaceOpen(&p, &e);
			ASSERT_NE(h, nullptr) << CLV_PaceStrError(e);
			ASSERT_EQ(e, CLV_PACE_OK);
			ASSERT_EQ(CLV_PaceBeginPass(h), CLV_PACE_OK);
		}

		void FeedSeconds(double s)
		{
			const uint64_t bytes = static_cast<uint64_t>(s * static_cast<double>(dev.bytes_per_second));
			ASSERT_EQ(CLV_PaceNoteAudioFed(h, bytes), CLV_PACE_OK);
			dev.fed += bytes;
		}

		void SetUp() override {}

		void TearDown() override
		{
			if (h != nullptr)
			{
				CLV_PaceClose(h);
				h = nullptr;
			}
		}
	};

	// —— 建仓与入参拒绝档 ——

	TEST_F(PaceApiTest, OpenRejectsMissingOrUndersizedStruct)
	{
		CLV_PaceError e = CLV_PACE_OK;
		EXPECT_EQ(CLV_PaceOpen(nullptr, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_INVALID_ARGUMENT);

		CLV_PaceParamsC p = MakeParams(dev);
		p.struct_size = sizeof(p) - 1u;
		EXPECT_EQ(CLV_PaceOpen(&p, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_INVALID_ARGUMENT);

		/* 尾部追加的兼容方向：来客见到更大的布局要照收，不拿等值判把追加变成破坏。 */
		p.struct_size = sizeof(p) + 16u;
		CLV_Pace* h2 = CLV_PaceOpen(&p, &e);
		ASSERT_NE(h2, nullptr);
		EXPECT_EQ(e, CLV_PACE_OK);
		CLV_PaceClose(h2);
	}

	TEST_F(PaceApiTest, OpenRequiresDeviceQueryCallback)
	{
		CLV_PaceParamsC p = MakeParams(dev);
		p.queued = nullptr;
		CLV_PaceError e = CLV_PACE_OK;
		EXPECT_EQ(CLV_PaceOpen(&p, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_INVALID_ARGUMENT);
	}

	TEST_F(PaceApiTest, OpenRejectsZeroAndAbsurdRates)
	{
		CLV_PaceError e = CLV_PACE_OK;

		CLV_PaceParamsC p = MakeParams(dev);
		p.fps_num = 0u;
		EXPECT_EQ(CLV_PaceOpen(&p, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_VALUE_RANGE);

		p = MakeParams(dev);
		p.fps_den = 0u;
		EXPECT_EQ(CLV_PaceOpen(&p, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_VALUE_RANGE);

		p = MakeParams(dev);
		p.sample_rate = 0u;
		EXPECT_EQ(CLV_PaceOpen(&p, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_VALUE_RANGE);

		p = MakeParams(dev);
		p.bytes_per_frame = 0u;
		EXPECT_EQ(CLV_PaceOpen(&p, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_VALUE_RANGE);

		p = MakeParams(dev);
		p.sample_rate = 0x2000000u;	   // 采样率上界 = 0x1000000：src/pace/clv_pace.cpp 的 CLV_PaceOpen 入口把
		// sample_rate > 0x1000000 与 bytes_per_frame > 0x100 两档都判值域不成立，这里取第一档的越界值
		EXPECT_EQ(CLV_PaceOpen(&p, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_VALUE_RANGE);
	}

	TEST(PaceApiStandalone, NullHandleIsRejectedAtEveryEntry)
	{
		double sec = 12345.0;
		uint8_t allowed = 7u;
		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		EXPECT_EQ(CLV_PaceBeginPass(nullptr), CLV_PACE_INVALID_HANDLE);
		EXPECT_EQ(CLV_PaceSetDeviceRunning(nullptr, 1u), CLV_PACE_INVALID_HANDLE);
		EXPECT_EQ(CLV_PaceNoteAudioFed(nullptr, 1u), CLV_PACE_INVALID_HANDLE);
		EXPECT_EQ(CLV_PaceClockSeconds(nullptr, &sec), CLV_PACE_INVALID_HANDLE);
		EXPECT_EQ(CLV_PaceDecide(nullptr, 0.0, &d), CLV_PACE_INVALID_HANDLE);
		EXPECT_EQ(CLV_PaceFeedAllowed(nullptr, &allowed), CLV_PACE_INVALID_HANDLE);
		CLV_PaceClose(nullptr);	   // no-op，不得崩
	}

	TEST(PaceApiStandalone, OutPointersMustNotBeNull)
	{
		FakeDevice dev;
		CLV_PaceParamsC p = MakeParams(dev);
		CLV_PaceError e = CLV_PACE_OK;
		CLV_Pace* h = CLV_PaceOpen(&p, &e);
		ASSERT_NE(h, nullptr);
		ASSERT_EQ(CLV_PaceBeginPass(h), CLV_PACE_OK);
		double* null_sec = nullptr;
		EXPECT_EQ(CLV_PaceClockSeconds(h, null_sec), CLV_PACE_INVALID_ARGUMENT);
		uint8_t allowed = 0u;
		EXPECT_EQ(CLV_PaceFeedAllowed(h, nullptr), CLV_PACE_INVALID_ARGUMENT);
		EXPECT_EQ(allowed, 0u);
		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		EXPECT_EQ(CLV_PaceDecide(h, 0.0, nullptr), CLV_PACE_INVALID_ARGUMENT);
		d.struct_size = sizeof(d) - 1u;
		EXPECT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_INVALID_ARGUMENT);
		CLV_PaceClose(h);
	}

	// —— 主时钟三条口径 ——

	TEST_F(PaceApiTest, ClockNeedsAPassStartFirst)
	{
		CLV_PaceError e = CLV_PACE_OUT_OF_MEMORY;
		const CLV_PaceParamsC p = MakeParams(dev);
		h = CLV_PaceOpen(&p, &e);
		ASSERT_NE(h, nullptr);
		double sec = 5.0;
		EXPECT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_STATE_ERROR);
		EXPECT_EQ(CLV_PaceNoteAudioFed(h, 1000u), CLV_PACE_STATE_ERROR);
		EXPECT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_STATE_ERROR);
		ASSERT_EQ(CLV_PaceBeginPass(h), CLV_PACE_OK);
		EXPECT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		EXPECT_DOUBLE_EQ(sec, 0.0);
	}

	/* 预灌期设备没放开，钟必须停在原位 —— 把预灌量算进已消费，实测形态就是「第一帧起一路被判定迟到」。 */
	TEST_F(PaceApiTest, PrerollWindowDoesNotAdvanceClock)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(0.279);
		dev.AdvanceTo(5'000'000'000ull);	// 假时钟走过 5 s，设备 paused ⇒ 一次都不取
		double sec = -1.0;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 0u), CLV_PACE_OK);
		for (int i = 0; i < 4; ++i)
		{
			ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
			EXPECT_DOUBLE_EQ(sec, 0.0);
			dev.AdvanceTo(5'000'000'000ull + static_cast<uint64_t>(i) * 100'000'000ull);
		}
	}

	/* 锚点要从「取钟那一刻」起算。假时钟起点故意放到 5 s —— 拿 now − 0 外推的话，
	 * 第一次读钟就会一口气顶到累计喂入的上限，这正是不显式初始化锚点的实测形态。 */
	TEST_F(PaceApiTest, FirstClockReadAnchorsNowNotEpoch)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(0.279);
		dev.now_ns = 5'000'000'000ull;
		dev.next_pull_ns = dev.now_ns + dev.pull_period_ns;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		double sec = -1.0;
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		EXPECT_DOUBLE_EQ(sec, 0.0);
	}

	/* 设备取数比帧周期粗（100 ms 一跳），两次取数之间按墙钟线性外推：
	 * 这一条不成立的话，读数会在 0 → 100 ms 之间跳，把不该等的帧判成超前。 */
	TEST_F(PaceApiTest, ExtrapolatesBetweenCoarseDevicePulls)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.0);
		dev.now_ns = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);

		double sec = -1.0;
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);	  // 锚在这里登记，consumed = 0
		EXPECT_DOUBLE_EQ(sec, 0.0);

		dev.AdvanceTo(40'000'000ull);	 // 还没到设备的取数点：队列没动，靠外推
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		EXPECT_NEAR(sec, 0.040, 1e-9);

		dev.AdvanceTo(100'000'000ull);	  // 取数发生：consumed 跳到 100 ms 当量，钟位由实测给出
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		EXPECT_NEAR(sec, 0.100, 1e-9);

		dev.AdvanceTo(150'000'000ull);
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		EXPECT_NEAR(sec, 0.150, 1e-9);
	}

	/* 外推不许超过累计喂入量：设备不可能播出还没交给它的数据。 */
	TEST_F(PaceApiTest, ExtrapolationIsCappedByFedBytes)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(0.05);
		dev.now_ns = 0ull;
		dev.next_pull_ns = 10'000'000'000ull;	 // 设备很久不来取，全程靠外推
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);

		double sec = -1.0;
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);	  // 登记锚点
		EXPECT_DOUBLE_EQ(sec, 0.0);

		dev.AdvanceTo(3'000'000'000ull);	// 墙钟走过 3 s，但只喂过 50 ms 当量
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		EXPECT_NEAR(sec, 0.050, 1e-9);
	}

	/* 暂停 → 恢复不能把暂停那段时间算进钟。 */
	TEST_F(PaceApiTest, PauseResumeDoesNotChargePausedTime)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.0);
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		double sec = -1.0;
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK); /* 这一次读只定起播零点，读数恒为 0 */
		ASSERT_DOUBLE_EQ(sec, 0.0);
		dev.AdvanceTo(200'000'000ull);
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		const double before = sec;
		ASSERT_GT(before, 0.0);

		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 0u), CLV_PACE_OK);
		dev.next_pull_ns = 100'000'000'000ull; /* 真设备暂停时不再取数，假设备要照做 */
		dev.AdvanceTo(3'000'000'000ull);	   // 暂停期走过 2.8 s
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		const double paused = sec;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		EXPECT_NEAR(sec, paused, 1e-9);	   // 恢复后不跳，把暂停那段整段剔掉
	}

	/* 调用方供的钟回退：报 CLOCK_SKEW，同时重登记锚点、只给实际已消费量，不能给出一段倒退的外推值。 */
	TEST_F(PaceApiTest, ClockSkewReanchorsAndReports)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.0);
		dev.now_ns = 1'000'000'000ull;
		dev.next_pull_ns = 1'100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		double sec = -1.0;
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);

		dev.now_ns = 900'000'000ull;	// 往回走
		EXPECT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_CLOCK_SKEW);
		EXPECT_NEAR(sec, 0.0, 1e-9);

		dev.now_ns = 1'000'000'000ull;
		EXPECT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);	  // 重登记后照常
	}

	/* 问不出排队量 ≠ 排队为零：沿用上一次读数，钟不能因此跳一格。 */
	TEST_F(PaceApiTest, FailedQueueQueryKeepsLastReading)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.0);
		dev.now_ns = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		double a = -1.0, b = -1.0;
		dev.AdvanceTo(50'000'000ull);
		ASSERT_EQ(CLV_PaceClockSeconds(h, &a), CLV_PACE_OK);
		dev.query_fails = true;
		dev.AdvanceTo(80'000'000ull);
		ASSERT_EQ(CLV_PaceClockSeconds(h, &b), CLV_PACE_OK);
		EXPECT_NEAR(b, a + 0.030, 1e-9);	// 只按墙钟外推，不把队列读成零而倒退
		dev.query_fails = false;
	}

	// —— 逐帧判定与阈值 ——

	TEST_F(PaceApiTest, ThresholdsAreMultiplesOfOnePeriod)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(2.0);
		dev.now_ns = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);

		const double period = 1.0 / 30.0;
		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);

		/* 两道闸各测一次。这里先把墙钟推到这一帧的出点上，让 deadline_wait 归零，
		 * 于是 wait_ns 只剩音频闸那一份 = dev − period/2。 */
		dev.AdvanceTo(static_cast<unsigned long long>(0.9 * period * 1e9));
		ASSERT_EQ(CLV_PaceDecide(h, 0.9 * period, &d), CLV_PACE_OK);
		EXPECT_EQ(d.action, CLV_PACE_ACT_WAIT);
		EXPECT_NEAR(d.dev_seconds, 0.9 * period, 1e-9);
		/* wait_ns 就是两道闸的较大值，两个量都能从出参里读出来复算，不留"期望值靠猜"的余地 */
		{
			const double deadline = 0.9 * period - d.wall_seconds > 0.0 ? 0.9 * period - d.wall_seconds : 0.0;
			const double audio =
				d.dev_seconds - d.period_seconds / 2.0 > 0.0 ? d.dev_seconds - d.period_seconds / 2.0 : 0.0;
			const double want = deadline > audio ? deadline : audio;
			EXPECT_NEAR(static_cast<double>(d.wait_ns) / 1e9, want, 1e-6);
			EXPECT_GE(static_cast<double>(d.wait_ns) / 1e9, audio - 1e-6);
			EXPECT_GE(static_cast<double>(d.wait_ns) / 1e9, deadline - 1e-6);
		}
		EXPECT_EQ(d.in_band, 0u);
		EXPECT_EQ(d.late, 0u);
		EXPECT_NEAR(d.period_seconds, period, 1e-12);
		/* 墙钟闸单独钉一次：挑一个远在未来出点的 pts，此时 deadline_wait 必然盖过音频闸。 */
		CLV_PaceDecisionC w;
		std::memset(&w, 0, sizeof(w));
		w.struct_size = sizeof(w);
		const double far_pts = 10.0 * period;
		ASSERT_EQ(CLV_PaceDecide(h, far_pts, &w), CLV_PACE_OK);
		EXPECT_EQ(w.action, CLV_PACE_ACT_WAIT);
		/* 两道闸谁占大不由测试预设，按出参复算取大者再比 */
		{
			const double dl = far_pts - w.wall_seconds > 0.0 ? far_pts - w.wall_seconds : 0.0;
			const double au =
				w.dev_seconds - w.period_seconds / 2.0 > 0.0 ? w.dev_seconds - w.period_seconds / 2.0 : 0.0;
			EXPECT_NEAR(static_cast<double>(w.wait_ns) / 1e9, dl > au ? dl : au, 1e-6);
		}

		// 带内与迟到档都要「出点已过」才轮到音频闸说话，所以先把墙钟推远，再拿当刻的钟位回推 pts。
		dev.AdvanceTo(500'000'000ull);
		CLV_PaceDecisionC base;
		std::memset(&base, 0, sizeof(base));
		base.struct_size = sizeof(base);
		ASSERT_EQ(CLV_PaceDecide(h, 0.0, &base), CLV_PACE_OK);
		const double c0 = base.clock_seconds;
		EXPECT_LE(base.wall_seconds, c0 + period); /* 墙钟与主钟同量级，下面的 pts 才不会撞墙钟闸 */

		// 迟 quarter（带内）⇒ 呈现且算带内
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		ASSERT_EQ(CLV_PaceDecide(h, c0 - period / 4.0, &d), CLV_PACE_OK);
		EXPECT_EQ(d.action, CLV_PACE_ACT_PRESENT);
		EXPECT_EQ(d.in_band, 1u);
		EXPECT_NEAR(d.dev_seconds, -period / 4.0, 1e-6);

		// 迟 0.6 period ⇒ 仍未过丢帧阈，呈现但记 late
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		ASSERT_EQ(CLV_PaceDecide(h, c0 - 0.6 * period, &d), CLV_PACE_OK);
		EXPECT_EQ(d.action, CLV_PACE_ACT_PRESENT);
		EXPECT_EQ(d.in_band, 0u);
		EXPECT_EQ(d.late, 1u);

		// 迟过一整个 period ⇒ 丢
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		ASSERT_EQ(CLV_PaceDecide(h, c0 - 1.1 * period, &d), CLV_PACE_OK);
		EXPECT_EQ(d.action, CLV_PACE_ACT_DROP);
		EXPECT_EQ(d.wait_ns, 0ull);
	}

	/* 冒烟判据：起钟 → 一帧帧喂 → 走完 30 帧，drop 计数为零。 */
	TEST_F(PaceApiTest, SmoothPassDropsNothing)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.2);	 // 够 30 帧整段播
		dev.now_ns = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);

		const double period = 1.0 / 30.0;
		uint64_t presented = 0, dropped = 0, waited = 0;
		for (uint32_t i = 0; i < 30u; ++i)
		{
			CLV_PaceDecisionC d;
			std::memset(&d, 0, sizeof(d));
			d.struct_size = sizeof(d);
			const double pts = i * period;
			/* 假设备与假时钟同速：墙钟走到这一帧的出点，判定应当直接给 PRESENT。
			 * 这里不等出点就判一次，是为了把「等待不该吞帧」这条钉住。 */
			ASSERT_EQ(CLV_PaceDecide(h, pts, &d), CLV_PACE_OK);
			if (d.action == CLV_PACE_ACT_WAIT)
			{
				waited++;
				dev.AdvanceTo(dev.now_ns + d.wait_ns);
				ASSERT_EQ(CLV_PaceDecide(h, pts, &d), CLV_PACE_OK);
			}
			dev.AdvanceTo(static_cast<unsigned long long>((i + 1) * period * 1e9));
			if (d.action == CLV_PACE_ACT_DROP) dropped++;
			else presented++;
		}
		EXPECT_EQ(dropped, 0ull);
		EXPECT_EQ(presented, 30ull);
		/* 等待次数不是判据：设备按整块取数（这里 100 ms 一块），块边界附近必然出现
		 * 「钟还没追到出点」的短暂等待，判据只看没有丢帧、帧数出满。 */
		EXPECT_LE(waited, 30ull * 3ull);
	}

	// —— 背压 ——

	TEST_F(PaceApiTest, BackpressureDefaultsToOneSecond)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		uint8_t allowed = 9u;
		FeedSeconds(0.8);
		ASSERT_EQ(CLV_PaceFeedAllowed(h, &allowed), CLV_PACE_OK);
		EXPECT_EQ(allowed, 1u);	   // 800 ms 当量 < 1000 ms

		FeedSeconds(0.4);
		ASSERT_EQ(CLV_PaceFeedAllowed(h, &allowed), CLV_PACE_OK);
		EXPECT_EQ(allowed, 0u);	   // 1200 ms 当量 > 1000 ms
	}

	TEST_F(PaceApiTest, BackpressureIsOverridableByParam)
	{
		FakeDevice d;
		CLV_PaceParamsC p = MakeParams(d);
		p.backpressure_bytes = 1000ull;
		CLV_PaceError e = CLV_PACE_OK;
		CLV_Pace* h2 = CLV_PaceOpen(&p, &e);
		ASSERT_NE(h2, nullptr);
		ASSERT_EQ(CLV_PaceBeginPass(h2), CLV_PACE_OK);
		d.fed = 500ull;
		uint8_t allowed = 9u;
		ASSERT_EQ(CLV_PaceFeedAllowed(h2, &allowed), CLV_PACE_OK);
		EXPECT_EQ(allowed, 1u);
		d.fed = 1500ull;
		ASSERT_EQ(CLV_PaceFeedAllowed(h2, &allowed), CLV_PACE_OK);
		EXPECT_EQ(allowed, 0u);
		CLV_PaceClose(h2);
	}

	/* 每趟复位：累计喂入与锚点都清零，第二趟不能继承第一趟的钟位。 */
	TEST_F(PaceApiTest, BeginPassClearsPreviousPassState)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.0);
		dev.now_ns = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		double sec = -1.0;
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK); /* 定零点 */
		dev.AdvanceTo(100'000'000ull);
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		ASSERT_GT(sec, 0.0);

		dev.fed = 0ull;
		dev.played = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		dev.now_ns = 0ull;
		ASSERT_EQ(CLV_PaceBeginPass(h), CLV_PACE_OK);
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		EXPECT_DOUBLE_EQ(sec, 0.0);
		uint8_t allowed = 9u;
		ASSERT_EQ(CLV_PaceFeedAllowed(h, &allowed), CLV_PACE_OK);
		EXPECT_EQ(allowed, 1u);
	}

	// —— 文案表 ——

	TEST(PaceApiStandalone, StrErrorCoversEveryCodeAndUnknown)
	{
		const CLV_PaceError codes[] = { CLV_PACE_OK,		   CLV_PACE_INVALID_HANDLE, CLV_PACE_INVALID_ARGUMENT,
										CLV_PACE_VALUE_RANGE,  CLV_PACE_STATE_ERROR,	CLV_PACE_CLOCK_SKEW,
										CLV_PACE_OUT_OF_MEMORY };
		for (const CLV_PaceError c: codes)
		{
			const std::string s = CLV_PaceStrError(c);
			EXPECT_FALSE(s.empty()) << static_cast<int>(c);
			for (const char ch: s)
			{
				EXPECT_GE(static_cast<unsigned char>(ch), 0x20) << "文案要全是可打印 ASCII";
				EXPECT_LT(static_cast<unsigned char>(ch), 0x80);
			}
		}
		/* 本枚举未登记的值一律返回 unknown：段首下界之外取 95，段尾上界之外取 102 与远值 9999 */
		EXPECT_STREQ(CLV_PaceStrError(static_cast<CLV_PaceError>(95)), "unknown");
		EXPECT_STREQ(CLV_PaceStrError(static_cast<CLV_PaceError>(102)), "unknown");
		EXPECT_STREQ(CLV_PaceStrError(static_cast<CLV_PaceError>(9999)), "unknown");
	}

}	 // namespace
