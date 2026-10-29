/* 节器件面的公开面用例：只吃 include/CLV_Pace.h，不碰实现层头。
 * 假设备 + 假墙钟 + 假消费端（报告值由用例给出），于是没有声卡与窗口也能把主钟口径逐条钉死。
 */
#include "CLV_Pace.h"

#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <string>

namespace
{

	/* 取数块 = 100 ms 当量，比帧周期粗：报告之间会跳一格就是这么来的 */
	struct FakeDevice
	{
		uint64_t now_ns = 0;
		uint64_t fed = 0;		// 设备侧累计收到的字节
		uint64_t played = 0;	// 已经被设备取走的字节
		uint64_t bytes_per_second = 0;
		uint64_t pull_period_ns = 100'000'000ull;
		uint64_t next_pull_ns = 100'000'000ull;
		bool query_fails = false;
		bool consumes = true;		   // false = 设备一格也不取（死队列形状）
		uint64_t stall_from_ns = 0;	   // 落在这个区间里的取数点一律推到区间末尾：设备停一格的形状
		uint64_t stall_to_ns = 0;

		uint64_t queued() const { return fed - played; }

		uint64_t consumed_ticks() const
		{
			return bytes_per_second == 0ull ? 0ull : played * 1'000'000'000ull / bytes_per_second;
		}

		void AdvanceTo(uint64_t t_ns)
		{
			if (t_ns < now_ns) t_ns = now_ns;	 // 墙钟只朝前走：推进量倒回去时按原地不动，回退那一档另有专门的用例
			now_ns = t_ns;
			while (consumes && t_ns >= next_pull_ns)
			{
				if (next_pull_ns >= stall_from_ns && next_pull_ns < stall_to_ns)
				{
					next_pull_ns = stall_to_ns;
					continue;
				}
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

		/* 消费端报告口 */
		void Report() { ASSERT_EQ(CLV_PaceNoteConsumedMediaTime(h, dev.consumed_ticks()), CLV_PACE_OK); }

		void AdvanceAndReport(uint64_t t_ns)
		{
			dev.AdvanceTo(t_ns);
			Report();
		}

		double Clock()
		{
			double sec = -1.0;
			EXPECT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
			return sec;
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

	/* 等值判会把尾部追加变成破坏性改动：0 / 上一版 / 本版 / 更大都要收，只拒「非零且比上一版小」。 */
	TEST_F(PaceApiTest, OpenAcceptsZeroPrevAndCurrentSizes)
	{
		CLV_PaceError e = CLV_PACE_OK;
		EXPECT_EQ(CLV_PaceOpen(nullptr, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_INVALID_ARGUMENT);

		CLV_PaceParamsC p = MakeParams(dev);
		for (const uint32_t sz: { 0u, 72u, static_cast<uint32_t>(sizeof(p)), static_cast<uint32_t>(sizeof(p) + 16u) })
		{
			p.struct_size = sz;
			CLV_Pace* h2 = CLV_PaceOpen(&p, &e);
			ASSERT_NE(h2, nullptr) << "尺寸 " << sz << " 该受理：" << CLV_PaceStrError(e);
			EXPECT_EQ(e, CLV_PACE_OK);
			CLV_PaceClose(h2);
		}

		/* 非零且比上一版小：前缀都不完整 */
		p.struct_size = 71u;
		EXPECT_EQ(CLV_PaceOpen(&p, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_INVALID_ARGUMENT);
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
		p.sample_rate = 0x2000000u;	   // 采样率上界 = 0x1000000：建仓入口把
		// sample_rate > 0x1000000 与 bytes_per_frame > 0x100 两档都判值域不成立，这里取第一档的越界值
		EXPECT_EQ(CLV_PaceOpen(&p, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_VALUE_RANGE);

		/* 界写在会进乘法的那一侧：真正要防的是时基 × 帧率分母溢出 */
		p = MakeParams(dev);
		p.fps_num = 1'000'001u;
		EXPECT_EQ(CLV_PaceOpen(&p, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_VALUE_RANGE);

		/* 不整除就拒；缺省那把 1e9 不许被任何常数判死 */
		p = MakeParams(dev);
		p.timebase_num = 1'000'000'000u;
		p.timebase_den = 3u;
		EXPECT_EQ(CLV_PaceOpen(&p, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_VALUE_RANGE);

		/* 这一档只违分母界，整除成立 */
		p = MakeParams(dev);
		p.timebase_num = 2'000'002u;
		p.timebase_den = 1'000'001u;
		EXPECT_EQ(CLV_PaceOpen(&p, &e), nullptr);
		EXPECT_EQ(e, CLV_PACE_VALUE_RANGE);

		p = MakeParams(dev);
		p.timebase_num = 1'000'000'000u;
		p.timebase_den = 1u;
		CLV_Pace* h2 = CLV_PaceOpen(&p, &e);
		EXPECT_NE(h2, nullptr) << CLV_PaceStrError(e);
		if (h2 != nullptr) CLV_PaceClose(h2);
	}

	TEST(PaceApiStandalone, NullHandleIsRejectedAtEveryEntry)
	{
		double sec = 12345.0;
		uint8_t allowed = 7u;
		uint32_t dead = 7u, regress = 7u, jumps = 7u;
		uint64_t dropped = 7u;
		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		EXPECT_EQ(CLV_PaceBeginPass(nullptr), CLV_PACE_INVALID_HANDLE);
		EXPECT_EQ(CLV_PaceSetDeviceRunning(nullptr, 1u), CLV_PACE_INVALID_HANDLE);
		EXPECT_EQ(CLV_PaceNoteAudioFed(nullptr, 1u), CLV_PACE_INVALID_HANDLE);
		EXPECT_EQ(CLV_PaceNoteConsumedMediaTime(nullptr, 1u), CLV_PACE_INVALID_HANDLE);
		EXPECT_EQ(CLV_PaceNoteAudioDropped(nullptr, 1u), CLV_PACE_INVALID_HANDLE);
		EXPECT_EQ(CLV_PaceStats(nullptr, &dead, &regress, &jumps, &dropped), CLV_PACE_INVALID_HANDLE);
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
		/* 比上一版 56 还小才拒 */
		d.struct_size = sizeof(d) - 17u;
		EXPECT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_INVALID_ARGUMENT);
		CLV_PaceClose(h);
	}

	// —— 主时钟：消费端报告驱动，库不外推 ——

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
		EXPECT_EQ(CLV_PaceNoteConsumedMediaTime(h, 1000u), CLV_PACE_STATE_ERROR);
		EXPECT_EQ(CLV_PaceNoteAudioDropped(h, 1000u), CLV_PACE_STATE_ERROR);
		ASSERT_EQ(CLV_PaceBeginPass(h), CLV_PACE_OK);
		EXPECT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		EXPECT_DOUBLE_EQ(sec, 0.0);
	}

	/* 预灌段一条报告都没有，钟停在原位；按住的那段也不算死队列 —— 那时候没有「该有进展」的预期。 */
	TEST_F(PaceApiTest, PrerollWindowDoesNotAdvanceClock)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(0.279);
		dev.consumes = false;
		dev.AdvanceTo(5'000'000'000ull);	// 假时钟走过 5 s，设备 paused ⇒ 一次都不取
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 0u), CLV_PACE_OK);
		for (int i = 0; i < 4; ++i)
		{
			double sec = -1.0;
			ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
			EXPECT_DOUBLE_EQ(sec, 0.0);
			dev.AdvanceTo(5'000'000'000ull + static_cast<uint64_t>(i) * 100'000'000ull);
		}
		uint32_t dead = 999u;
		ASSERT_EQ(CLV_PaceStats(h, &dead, nullptr, nullptr, nullptr), CLV_PACE_OK);
		EXPECT_EQ(dead, 0u) << "预灌段不该报死队列";
	}

	/* 假钟起点放到 5 s：主钟只跟报告走，程序已经跑了多久进不了钟位。 */
	TEST_F(PaceApiTest, FirstClockReadIsNotAnEpoch)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(0.279);
		dev.now_ns = 5'000'000'000ull;
		dev.next_pull_ns = dev.now_ns + dev.pull_period_ns;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		EXPECT_DOUBLE_EQ(Clock(), 0.0);
		AdvanceAndReport(dev.now_ns + 100'000'000ull);	  // 设备取走一格并报告 ⇒ 钟跳一格
		EXPECT_NEAR(Clock(), 0.100, 1e-9);
	}

	/* 报告粒度就是钟的精度：设备一格没取，墙钟走过多少都不推进。 */
	TEST_F(PaceApiTest, ClockFollowsReportsWithoutExtrapolation)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.0);
		dev.now_ns = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		Report();
		EXPECT_DOUBLE_EQ(Clock(), 0.0);

		dev.AdvanceTo(40'000'000ull);	 // 还没到设备的取数点：没有新报告，钟不动
		Report();
		EXPECT_DOUBLE_EQ(Clock(), 0.0);

		AdvanceAndReport(100'000'000ull);	 // 取数发生并报告：钟位跳到 100 ms 当量
		EXPECT_NEAR(Clock(), 0.100, 1e-9);

		dev.AdvanceTo(150'000'000ull);	  // 块边界之间：报告值没变，钟位就停在这一格
		Report();
		EXPECT_NEAR(Clock(), 0.100, 1e-9);

		AdvanceAndReport(200'000'000ull);
		EXPECT_NEAR(Clock(), 0.200, 1e-9);
	}

	/* 一条都不报：钟停在原地，死队列计数报警。 */
	TEST_F(PaceApiTest, ClockStopsWhenNothingIsReported)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(0.05);
		dev.now_ns = 0ull;
		dev.next_pull_ns = 10'000'000'000ull;	 // 设备很久不来取
		dev.consumes = false;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);

		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		for (int i = 0; i < 4; ++i)
		{
			dev.AdvanceTo(static_cast<uint64_t>(i) * 1'000'000'000ull);
			ASSERT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_OK);
			EXPECT_DOUBLE_EQ(d.clock_seconds, 0.0);
		}
		uint32_t dead = 0u, regress = 0u;
		ASSERT_EQ(CLV_PaceStats(h, &dead, &regress, nullptr, nullptr), CLV_PACE_OK);
		EXPECT_GT(dead, 0u);
		EXPECT_EQ(regress, 0u);
	}

	TEST_F(PaceApiTest, PausedTimeIsNotChargedToClock)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.0);
		dev.now_ns = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		Report();
		AdvanceAndReport(200'000'000ull);
		const double before = Clock();
		ASSERT_GT(before, 0.0);

		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 0u), CLV_PACE_OK);
		dev.consumes = false;
		dev.AdvanceTo(3'000'000'000ull);	// 暂停期走过 2.8 s，一条报告都没有
		Report();
		const double paused = Clock();
		EXPECT_NEAR(paused, before, 1e-9);

		uint32_t dead = 0u;
		ASSERT_EQ(CLV_PaceStats(h, &dead, nullptr, nullptr, nullptr), CLV_PACE_OK);
		EXPECT_EQ(dead, 0u) << "按住的那段不算死队列";

		dev.consumes = true;
		dev.next_pull_ns = 3'100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		EXPECT_NEAR(Clock(), paused, 1e-9);	   // 恢复本身不动钟位：零点每趟只定一次
		dev.AdvanceTo(3'200'000'000ull);
		Report();
		EXPECT_GT(Clock(), paused);
	}

	/* 供的墙钟回退：重登记基准、这一拍判 CLOCK_SKEW；关键是相减不许回绕成一段巨长的「已经过」。 */
	TEST_F(PaceApiTest, CallerClockSkewReanchorsAndReports)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.0);
		dev.now_ns = 1'000'000'000ull;
		dev.next_pull_ns = 1'100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		Report();
		double sec = -1.0;
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);

		dev.now_ns = 900'000'000ull;	// 往回走
		EXPECT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_CLOCK_SKEW);
		EXPECT_DOUBLE_EQ(sec, 0.0);	   // 钟位只跟报告走，与墙钟回退无关

		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		dev.now_ns = 901'000'000ull;
		ASSERT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_OK);
		EXPECT_LT(d.wall_seconds, 1.0) << "回退后的墙龄必须从小值重新起算，不能回绕";

		dev.now_ns = 890'000'000ull;	// 再退一次：判定口也得把这个边沿交出来，只交一拍
		ASSERT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_CLOCK_SKEW);
		dev.now_ns = 891'000'000ull;
		ASSERT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_OK);

		dev.now_ns = 1'000'000'000ull;
		EXPECT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
	}

	/* 报告值回退一口拒掉、账不动，只记一笔回退拒收。 */
	TEST_F(PaceApiTest, BackwardReportIsRejectedAndCounted)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.0);
		dev.consumes = false;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		ASSERT_EQ(CLV_PaceNoteConsumedMediaTime(h, 200'000'000ull), CLV_PACE_OK);
		EXPECT_DOUBLE_EQ(Clock(), 0.200);
		EXPECT_EQ(CLV_PaceNoteConsumedMediaTime(h, 100'000'000ull), CLV_PACE_STATE_ERROR);
		EXPECT_DOUBLE_EQ(Clock(), 0.200) << "拒收就不能改账";
		uint32_t regress = 0u, dead = 0u;
		ASSERT_EQ(CLV_PaceStats(h, &dead, &regress, nullptr, nullptr), CLV_PACE_OK);
		EXPECT_GT(regress, 0u);
	}

	/* 问不出排队量 ≠ 排队为零：沿用上一次读数，水位改判 UNKNOWN，主钟照旧跟报告走。 */
	TEST_F(PaceApiTest, FailedQueueQueryReportsUnknownWatermark)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.0);
		dev.now_ns = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);

		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		AdvanceAndReport(100'000'000ull);
		ASSERT_EQ(CLV_PaceDecide(h, 0.100, &d), CLV_PACE_OK);
		EXPECT_NE(d.watermark, 3u);
		EXPECT_GT(d.queue_ms, 0u);

		dev.query_fails = true;
		dev.consumes = false;	 // 真设备断了：不再取数，也不再回话
		dev.AdvanceTo(150'000'000ull);
		Report();
		ASSERT_EQ(CLV_PaceDecide(h, 0.150, &d), CLV_PACE_OK);
		EXPECT_EQ(d.watermark, 3u);
		EXPECT_EQ(d.queue_ms, 0u);
		EXPECT_NEAR(d.clock_seconds, 0.100, 1e-9);	  // 钟位由报告给出，与问没问出排队量无关
		dev.query_fails = false;
		dev.consumes = true;
		AdvanceAndReport(200'000'000ull);
		ASSERT_EQ(CLV_PaceDecide(h, 0.200, &d), CLV_PACE_OK);
		EXPECT_EQ(d.watermark, 0u) << "重新问得出就回到正常档";
	}

	// —— 逐帧判定与阈值 ——

	/* 丢帧阈 = 一个帧周期 + 实测最大报告步长：100 ms 一跳那一格不是迟到，旧的「一整个 period」会把它判成丢帧。 */
	TEST_F(PaceApiTest, ThresholdsAreMultiplesOfOnePeriod)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(2.0);
		dev.now_ns = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		Report();

		const double period = 1.0 / 30.0;
		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);

		dev.AdvanceTo(static_cast<uint64_t>(0.9 * period * 1e9));
		Report();
		ASSERT_EQ(CLV_PaceDecide(h, 0.9 * period, &d), CLV_PACE_OK);
		EXPECT_EQ(d.action, CLV_PACE_ACT_WAIT);
		EXPECT_NEAR(d.dev_seconds, 0.9 * period, 1e-9);
		{
			const double deadline = 0.9 * period - d.wall_seconds > 0.0 ? 0.9 * period - d.wall_seconds : 0.0;
			const double audio =
				d.dev_seconds - d.period_seconds / 2.0 > 0.0 ? d.dev_seconds - d.period_seconds / 2.0 : 0.0;
			const double want = deadline > audio ? deadline : audio;
			EXPECT_NEAR(static_cast<double>(d.wait_ns) / 1e9, want, 1e-6);
			EXPECT_GT(audio, 0.0) << "钟位在 0，这一帧的偏差由音频闸给出";
		}
		EXPECT_EQ(d.in_band, 0u) << "超前半个周期以上就不在带内";
		EXPECT_EQ(d.late, 0u);
		EXPECT_NEAR(d.period_seconds, period, 1e-12);

		/* 两格报告、每格都是 100 ms 步长；出点也过了，才轮到带内 / 迟到 / 丢三档说话。 */
		AdvanceAndReport(100'000'000ull);
		AdvanceAndReport(200'000'000ull);
		CLV_PaceDecisionC base;
		std::memset(&base, 0, sizeof(base));
		base.struct_size = sizeof(base);
		ASSERT_EQ(CLV_PaceDecide(h, 0.200, &base), CLV_PACE_OK);
		const double c0 = base.clock_seconds;
		ASSERT_NEAR(c0, 0.200, 1e-9);

		// 迟 quarter（带内）⇒ 呈现且算带内
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		ASSERT_EQ(CLV_PaceDecide(h, c0 - period / 4.0, &d), CLV_PACE_OK);
		EXPECT_EQ(d.action, CLV_PACE_ACT_PRESENT);
		EXPECT_EQ(d.in_band, 1u);
		EXPECT_NEAR(d.dev_seconds, -period / 4.0, 1e-6);

		// 迟 1.1 period：旧阈（一整个 period）会判丢，报告步长 100 ms 在位 ⇒ 仍算带外迟到，呈现
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		ASSERT_EQ(CLV_PaceDecide(h, c0 - 1.1 * period, &d), CLV_PACE_OK);
		EXPECT_EQ(d.action, CLV_PACE_ACT_PRESENT);
		EXPECT_EQ(d.in_band, 0u);
		EXPECT_EQ(d.late, 1u);

		// 迟过「period + 报告步长」⇒ 丢
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		ASSERT_EQ(CLV_PaceDecide(h, c0 - 0.100 - 1.1 * period, &d), CLV_PACE_OK);
		EXPECT_EQ(d.action, CLV_PACE_ACT_DROP);
		EXPECT_EQ(d.wait_ns, 0ull);
	}

	/* 冒烟判据：起钟 → 一帧帧报告 → 走完 30 帧，drop 计数为零。 */
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
			dev.AdvanceTo(static_cast<uint64_t>(pts * 1e9));
			Report();
			ASSERT_EQ(CLV_PaceDecide(h, pts, &d), CLV_PACE_OK);
			if (d.action == CLV_PACE_ACT_WAIT)
			{
				waited++;
				dev.AdvanceTo(dev.now_ns + d.wait_ns);
				Report();
				ASSERT_EQ(CLV_PaceDecide(h, pts, &d), CLV_PACE_OK);
			}
			dev.AdvanceTo(static_cast<unsigned long long>((i + 1) * period * 1e9));
			Report();
			if (d.action == CLV_PACE_ACT_DROP) dropped++;
			else presented++;
		}
		EXPECT_EQ(dropped, 0ull);
		EXPECT_EQ(presented, 30ull);
		/* 等待次数不是判据：设备按整块取数（这里 100 ms 一块），块边界附近必然出现
		 * 「钟还没追到出点」的短暂等待，判据只看没有丢帧、帧数出满。 */
		EXPECT_LE(waited, 30ull * 3ull);
	}

	// —— 递交长度控制写入：旧尺寸调用方的尾部一格都不碰 ——

	/* 递交上一版（56）时 56 之后必须一字不动 —— 128 B 垫子填满哨兵逐字节回查，越界写只有这么量才看得见。 */
	TEST_F(PaceApiTest, PrevSizeDecisionWritesOnlyPrefix)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.0);
		dev.consumes = false;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		ASSERT_EQ(CLV_PaceNoteConsumedMediaTime(h, 100'000'000ull), CLV_PACE_OK);

		alignas(CLV_PaceDecisionC) unsigned char buf[128];
		std::memset(buf, 0xA5, sizeof(buf));
		auto* d = reinterpret_cast<CLV_PaceDecisionC*>(buf);
		d->struct_size = 56u;	 // 上一版尺寸：只写前缀
		ASSERT_EQ(CLV_PaceDecide(h, 0.100, d), CLV_PACE_OK);
		for (size_t i = 56; i < sizeof(buf); ++i) EXPECT_EQ(buf[i], 0xA5u) << "旧尺寸递交却在偏移 " << i << " 写了字节";

		std::memset(buf, 0xA5, sizeof(buf));
		d = reinterpret_cast<CLV_PaceDecisionC*>(buf);
		d->struct_size = sizeof(CLV_PaceDecisionC);
		ASSERT_EQ(CLV_PaceDecide(h, 0.100, d), CLV_PACE_OK);
		EXPECT_NE(buf[56], 0xA5u) << "本版尺寸该写水位，尾部没动";
		EXPECT_EQ(buf[70], 0xA5u) << "70–71 是隐式填充：库不写它，也不承诺它的值";

		std::memset(buf, 0xA5, sizeof(buf));
		d = reinterpret_cast<CLV_PaceDecisionC*>(buf);
		d->struct_size = 71u;	 // 中间档：比上一版 56 大、比本版 72 小 ⇒ 受理，但仍只按前缀写
		ASSERT_EQ(CLV_PaceDecide(h, 0.100, d), CLV_PACE_OK);
		for (size_t i = 56; i < sizeof(buf); ++i) EXPECT_EQ(buf[i], 0xA5u) << "递交 71 却在偏移 " << i << " 写了字节";

		std::memset(buf, 0xA5, sizeof(buf));
		d = reinterpret_cast<CLV_PaceDecisionC*>(buf);
		d->struct_size = 0u;	// 填 0 也受理，但长度信息缺失 ⇒ 只按上一版前缀写，绝不越进调用方可能只有的旧布局
		ASSERT_EQ(CLV_PaceDecide(h, 0.100, d), CLV_PACE_OK);
		for (size_t i = 56; i < sizeof(buf); ++i)
			EXPECT_EQ(buf[i], 0xA5u) << "填 0 的递交却在偏移 " << i << " 写了字节";
	}

	/* 递交旧尺寸时不读尾部：时基落回纳秒缺省，1e9 tick 的报告正好换成 1.0 s —— 真去读哨兵字节就出不来这个数。 */
	TEST_F(PaceApiTest, PrevSizeParamsReadsDefaults)
	{
		alignas(CLV_PaceParamsC) unsigned char buf[128];
		std::memset(buf, 0xA5, sizeof(buf));
		auto* src = reinterpret_cast<CLV_PaceParamsC*>(buf);
		src->struct_size = 72u;	   // 上一版尺寸
		src->fps_num = 30u;
		src->fps_den = 1u;
		src->sample_rate = 44100u;
		src->bytes_per_frame = 4u;
		src->backpressure_bytes = 0ull;
		src->preroll_bytes = 0ull;
		src->queued = &QueryQueued;
		src->now = &NowNs;
		src->queued_user = &dev;
		src->now_user = &dev;
		dev.bytes_per_second = 44100ull * 4ull;
		unsigned char snap[128];
		std::memcpy(snap, buf, sizeof(snap));

		CLV_PaceError e = CLV_PACE_OK;
		CLV_Pace* h2 = CLV_PaceOpen(src, &e);
		ASSERT_NE(h2, nullptr) << CLV_PaceStrError(e);
		/* 建仓口对参数缓冲只读不写：递交旧尺寸时 128 B 垫子必须一字不动。 */
		for (size_t i = 0; i < sizeof(snap); ++i) EXPECT_EQ(buf[i], snap[i]) << "Open 改写了调用方参数缓冲的偏移 " << i;
		ASSERT_EQ(CLV_PaceBeginPass(h2), CLV_PACE_OK);
		dev.consumes = false;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h2, 1u), CLV_PACE_OK);
		ASSERT_EQ(CLV_PaceNoteConsumedMediaTime(h2, 1'000'000'000ull), CLV_PACE_OK);
		double sec = -1.0;
		ASSERT_EQ(CLV_PaceClockSeconds(h2, &sec), CLV_PACE_OK);
		EXPECT_NEAR(sec, 1.0, 1e-9) << "旧尺寸递交时时基该落回纳秒缺省";
		CLV_PaceClose(h2);
	}

	// —— 积压水位、速率匹配与诊断量 ——

	/* 水位按积压上界分档：过半是 PRESSURE，越界是 OVERFLOW。 */
	TEST_F(PaceApiTest, WatermarkTiersFollowBacklogBound)
	{
		FakeDevice d;
		CLV_PaceParamsC p = MakeParams(d);
		p.max_latency_bytes = 400'000ull;	 // 约 2.27 s 当量，三档都好摆
		CLV_PaceError e = CLV_PACE_OK;
		CLV_Pace* h2 = CLV_PaceOpen(&p, &e);
		ASSERT_NE(h2, nullptr) << CLV_PaceStrError(e);
		ASSERT_EQ(CLV_PaceBeginPass(h2), CLV_PACE_OK);
		d.consumes = false;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h2, 1u), CLV_PACE_OK);

		CLV_PaceDecisionC dec;
		std::memset(&dec, 0, sizeof(dec));
		dec.struct_size = sizeof(dec);

		struct Want
		{
			uint64_t bytes;
			uint32_t tier;
		};

		for (const Want w: {
				 Want { 60'000ull,	0u },
				  Want { 250'000ull, 1u },
					Want { 450'000ull, 2u }
		   })
		{
			uint64_t add = w.bytes > d.fed ? w.bytes - d.fed : 0ull;
			if (add != 0ull)
			{
				ASSERT_EQ(CLV_PaceNoteAudioFed(h2, add), CLV_PACE_OK);
				d.fed += add;
			}
			ASSERT_EQ(CLV_PaceDecide(h2, 0.0, &dec), CLV_PACE_OK);
			EXPECT_EQ(dec.watermark, w.tier) << w.bytes << " B 当量";
			EXPECT_NEAR(static_cast<double>(dec.queue_ms),
						1000.0 * static_cast<double>(d.fed - d.played) / static_cast<double>(d.bytes_per_second), 1.0);
		}
		CLV_PaceClose(h2);
	}

	/* 倍率钳在 ±5 %：积压到顶催快、流空了压慢，两头都越不出那对界。 */
	TEST_F(PaceApiTest, ResampleRatioStaysClamped)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		dev.consumes = false;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		FeedSeconds(10.0);	  // 远超缺省 2000 ms 的积压上界

		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		ASSERT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_OK);
		EXPECT_EQ(d.watermark, 2u);
		EXPECT_EQ(d.resample_ratio_q16, 68813u) << "催快顶到 +5 %";

		/* 设备把流取空：积压归零 ⇒ 顶到 −5 %。 */
		dev.consumes = true;
		dev.next_pull_ns = 1ull;
		dev.AdvanceTo(20'000'000'000ull);
		Report();
		ASSERT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_OK);
		EXPECT_EQ(d.queue_ms, 0u);
		EXPECT_EQ(d.resample_ratio_q16, 62259u) << "压慢顶到 −5 %";
	}

	/* 提示只看内容偏差：落后一周期报丢、超前两周期报重。 */
	TEST_F(PaceApiTest, DropDupHintFollowsContentOffset)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		dev.consumes = false;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		ASSERT_EQ(CLV_PaceNoteConsumedMediaTime(h, 1'000'000'000ull), CLV_PACE_OK);

		const double period = 1.0 / 30.0;
		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		dev.next_pull_ns = 100'000'000'000ull;
		dev.now_ns = 1'000'000'000ull;

		ASSERT_EQ(CLV_PaceDecide(h, 1.0, &d), CLV_PACE_OK);
		EXPECT_EQ(d.drop_dup_hint, 0u);
		ASSERT_EQ(CLV_PaceDecide(h, 1.0 - 1.1 * period, &d), CLV_PACE_OK);
		EXPECT_EQ(d.drop_dup_hint, 1u) << "落后超过一周期该报丢";
		ASSERT_EQ(CLV_PaceDecide(h, 1.0 + 2.1 * period, &d), CLV_PACE_OK);
		EXPECT_EQ(d.drop_dup_hint, 2u) << "超前超过两周期该报重帧";
	}

	/* 设备自己清队：库交一次跳变边沿、不动任何账，边沿只持续一次判定。 */
	TEST_F(PaceApiTest, DeviceJumpEdgeLastsOneDecision)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(6.0);	 // 远超背压当量，清掉的量才够判成「不是正常取数」
		dev.consumes = false;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		ASSERT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_OK);
		ASSERT_EQ(d.device_jump, 0u);

		dev.played = dev.fed;	 // 设备自己把队列清了
		ASSERT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_OK);
		EXPECT_EQ(d.device_jump, 1u);
		ASSERT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_OK);
		EXPECT_EQ(d.device_jump, 0u) << "边沿只交一次";

		double sec = -1.0;
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		EXPECT_DOUBLE_EQ(sec, 0.0) << "跳变不动主钟：主钟只跟报告走";

		uint32_t jumps = 0u, dead = 0u;
		uint64_t dropped = 999ull;
		ASSERT_EQ(CLV_PaceStats(h, &dead, nullptr, &jumps, &dropped), CLV_PACE_OK);
		EXPECT_EQ(jumps, 1u);
		EXPECT_EQ(dropped, 0ull) << "设备跳变不由减账口报";
	}

	/* 先报后清：报得比还排着的多就整口拒、账不动；报得下的那笔等设备读数掉下来时抵掉。 */
	TEST_F(PaceApiTest, DroppedBytesAreReportedBeforeClearing)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.0);
		const uint64_t all = dev.fed;
		dev.consumes = false;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);

		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		ASSERT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_OK);
		const uint32_t q_before = d.queue_ms;

		EXPECT_EQ(CLV_PaceNoteAudioDropped(h, all + 1ull), CLV_PACE_INVALID_ARGUMENT);
		ASSERT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_OK);
		EXPECT_EQ(d.queue_ms, q_before) << "被拒的报账不能动水位";

		ASSERT_EQ(CLV_PaceNoteAudioDropped(h, all / 2ull), CLV_PACE_OK);
		uint64_t dropped = 0ull;
		ASSERT_EQ(CLV_PaceStats(h, nullptr, nullptr, nullptr, &dropped), CLV_PACE_OK);
		EXPECT_EQ(dropped, all / 2ull);

		ASSERT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_OK);
		EXPECT_NEAR(static_cast<double>(d.queue_ms),
					1000.0 * static_cast<double>(all) / static_cast<double>(dev.bytes_per_second) / 2.0, 2.0);

		dev.fed -= all / 2ull;	  // 调用方真把陈旧积压清了，设备读数跟着掉
		ASSERT_EQ(CLV_PaceDecide(h, 0.0, &d), CLV_PACE_OK);
		EXPECT_NEAR(static_cast<double>(d.queue_ms),
					1000.0 * static_cast<double>(all) / static_cast<double>(dev.bytes_per_second) / 2.0, 2.0);
	}

	TEST_F(PaceApiTest, StatsAcceptsNullOutputs)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		EXPECT_EQ(CLV_PaceStats(h, nullptr, nullptr, nullptr, nullptr), CLV_PACE_OK);
		uint32_t dead = 0u;
		ASSERT_EQ(CLV_PaceStats(h, &dead, nullptr, nullptr, nullptr), CLV_PACE_OK);
		EXPECT_EQ(dead, 0u);
	}

	/* 递交尺寸落在「大于上一版、小于本版」的中间档（87 = 88−1）：受理，但尾部三格不读，取库内缺省。
	 * 这一档既不是「比上一版小」的拒收面，也不是「够读尾部」的受理面，边界必须写死。 */
	TEST_F(PaceApiTest, MidSizeParamsAcceptsAndIgnoresTail)
	{
		alignas(CLV_PaceParamsC) unsigned char buf[128];
		std::memset(buf, 0xA5, sizeof(buf));
		auto* src = reinterpret_cast<CLV_PaceParamsC*>(buf);
		src->struct_size = sizeof(CLV_PaceParamsC) - 1u;	// 87：比上一版大、比本版小
		src->fps_num = 30u;
		src->fps_den = 1u;
		src->sample_rate = 44100u;
		src->bytes_per_frame = 4u;
		src->queued = &QueryQueued;
		src->now = &NowNs;
		src->queued_user = &dev;
		src->now_user = &dev;
		dev.bytes_per_second = 44100ull * 4ull;
		dev.consumes = false;
		unsigned char snap[128];
		std::memcpy(snap, buf, sizeof(snap));

		CLV_PaceError e = CLV_PACE_OK;
		CLV_Pace* h2 = CLV_PaceOpen(src, &e);
		ASSERT_NE(h2, nullptr) << CLV_PaceStrError(e);
		/* 写侧同样不越界：递交 87 时库若按本版长度回写，就会碰掉调用方没给的那些格。 */
		for (size_t i = 0; i < sizeof(snap); ++i) EXPECT_EQ(buf[i], snap[i]) << "Open 改写了调用方参数缓冲的偏移 " << i;
		ASSERT_EQ(CLV_PaceBeginPass(h2), CLV_PACE_OK);
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h2, 1u), CLV_PACE_OK);
		/* 时基取缺省纳秒：报 1e9 tick 就是 1.0 s。若真去读那 8 个尾字节，读到的会是哨兵垃圾。 */
		ASSERT_EQ(CLV_PaceNoteConsumedMediaTime(h2, 1'000'000'000ull), CLV_PACE_OK);
		double sec = -1.0;
		ASSERT_EQ(CLV_PaceClockSeconds(h2, &sec), CLV_PACE_OK);
		EXPECT_NEAR(sec, 1.0, 1e-9);
		/* 积压上界同样取缺省 2000 ms 当量：0.279 s 的积压该落在 HEALTHY 档，而不是被哨兵当成极小的上界。 */
		CLV_PaceNoteAudioFed(h2, 49'152ull);
		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		ASSERT_EQ(CLV_PaceDecide(h2, 1.0, &d), CLV_PACE_OK);
		EXPECT_EQ(d.watermark, 0u);
		CLV_PaceClose(h2);
	}

	// —— 每趟复位 ——

	TEST_F(PaceApiTest, BeginPassClearsPreviousPassState)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(1.0);
		dev.now_ns = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		Report();
		dev.AdvanceTo(200'000'000ull);
		Report();
		const double first = Clock();
		ASSERT_GT(first, 0.0);
		uint32_t dead_before = 0u;
		ASSERT_EQ(CLV_PaceStats(h, &dead_before, nullptr, nullptr, nullptr), CLV_PACE_OK);

		dev.fed = 0ull;
		dev.played = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		dev.now_ns = 0ull;
		ASSERT_EQ(CLV_PaceBeginPass(h), CLV_PACE_OK);
		EXPECT_DOUBLE_EQ(Clock(), 0.0);
		uint32_t dead = 999u, regress = 999u, jumps = 999u;
		uint64_t dropped = 999ull;
		ASSERT_EQ(CLV_PaceStats(h, &dead, &regress, &jumps, &dropped), CLV_PACE_OK);
		EXPECT_EQ(dead, 0u);
		EXPECT_EQ(regress, 0u);
		EXPECT_EQ(jumps, 0u);
		EXPECT_EQ(dropped, 0ull);
		uint8_t allowed = 9u;
		ASSERT_EQ(CLV_PaceFeedAllowed(h, &allowed), CLV_PACE_OK);
		EXPECT_EQ(allowed, 1u);
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

	// —— 设备形状三格：判据都是「出满、零丢、主钟单调」 ——

	/* 走一遍帧序列：按判定给的时长推进墙钟，并把设备取走的量报进去。hold_limit 越界就当卡死登记。 */
	struct PassResult
	{
		uint64_t presented = 0;
		uint64_t dropped = 0;
		uint64_t waits = 0;
		uint64_t holds = 0;	   // 某一帧的等待重判次数峰值
		bool hung = false;
	};

	PassResult RunFrames(CLV_Pace* h, FakeDevice& dev, uint32_t frames, double period, uint64_t hold_limit)
	{
		PassResult r;
		const uint64_t period_ns = static_cast<uint64_t>(period * 1e9);
		for (uint32_t i = 0; i < frames; ++i)
		{
			const double pts = i * period;
			CLV_PaceDecisionC d;
			std::memset(&d, 0, sizeof(d));
			d.struct_size = sizeof(d);
			uint64_t holds = 0;
			for (;;)
			{
				if (CLV_PaceNoteConsumedMediaTime(h, dev.consumed_ticks()) != CLV_PACE_OK)
				{
					r.hung = true;
					break;
				}
				if (CLV_PaceDecide(h, pts, &d) != CLV_PACE_OK)
				{
					r.hung = true;
					break;
				}
				if (d.action != CLV_PACE_ACT_WAIT) break;
				if (++holds > hold_limit)
				{
					r.hung = true;
					break;
				}
				r.waits++;
				dev.AdvanceTo(dev.now_ns + d.wait_ns + 1ull);
			}
			if (r.hung) break;
			if (d.action == CLV_PACE_ACT_DROP) r.dropped++;
			else r.presented++;
			r.holds = holds > r.holds ? holds : r.holds;
			dev.AdvanceTo(dev.now_ns + period_ns);	  // 这一帧在屏幕上占一个周期
		}
		return r;
	}

	TEST_F(PaceApiTest, ChunkedDevicePullsPresentEveryFrame)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(4.0);	 // 90 帧 = 3 s，供料要盖过内容时长
		dev.now_ns = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		const PassResult r = RunFrames(h, dev, 90u, 1.0 / 30.0, 300ull);
		EXPECT_FALSE(r.hung);
		EXPECT_EQ(r.presented, 90ull);
		EXPECT_EQ(r.dropped, 0ull);
	}

	/* 停一格 1 s：主钟停在原地、按出点等，恢复后一格不丢，停播段要报死队列。 */
	TEST_F(PaceApiTest, DeviceStallForOneSecondDropsNothing)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(6.0);
		dev.now_ns = 0ull;
		dev.next_pull_ns = 100'000'000ull;
		dev.stall_from_ns = 1'000'000'000ull;
		dev.stall_to_ns = 2'000'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		const PassResult r = RunFrames(h, dev, 90u, 1.0 / 30.0, 4000ull);
		EXPECT_FALSE(r.hung);
		EXPECT_EQ(r.presented, 90ull);
		EXPECT_EQ(r.dropped, 0ull);
		uint32_t dead = 0u;
		ASSERT_EQ(CLV_PaceStats(h, &dead, nullptr, nullptr, nullptr), CLV_PACE_OK);
		EXPECT_GT(dead, 0u) << "停播段就是死队列形状，保护层要报出来";
	}

	/* 慢消费（取数节奏慢一成）：队列有界不增长、一格不丢、主钟全程单调。 */
	TEST_F(PaceApiTest, SlowConsumerKeepsQueueBounded)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(4.0);
		dev.now_ns = 0ull;
		dev.pull_period_ns = 111'111'111ull;	// 0.9× 的取数节奏：每格还是 100 ms 当量
		dev.next_pull_ns = dev.pull_period_ns;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		const PassResult r = RunFrames(h, dev, 90u, 1.0 / 30.0, 4000ull);
		EXPECT_FALSE(r.hung);
		EXPECT_EQ(r.presented, 90ull);
		EXPECT_EQ(r.dropped, 0ull);
		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		dev.AdvanceTo(4'000'000'000ull);
		ASSERT_EQ(CLV_PaceNoteConsumedMediaTime(h, dev.consumed_ticks()), CLV_PACE_OK);
		ASSERT_EQ(CLV_PaceDecide(h, 3.0, &d), CLV_PACE_OK);
		EXPECT_LT(d.queue_ms, 2'000u) << "积压不许发散：上界是 2000 ms 当量";
		uint32_t regress = 0u;
		ASSERT_EQ(CLV_PaceStats(h, nullptr, &regress, nullptr, nullptr), CLV_PACE_OK);
		EXPECT_EQ(regress, 0u) << "主钟全程单调";
	}

	// —— 死队列专档：允许钟冻住，判据是报警、不崩、不丢 ——

	/* 设备一格不取、一条都不报：主钟停在 0、计数报警、只出第一帧。 */
	TEST_F(PaceApiTest, DeadQueueHoldsClockAndAlarms)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(2.0);
		dev.consumes = false;
		dev.now_ns = 0ull;
		dev.next_pull_ns = 10'000'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);
		const PassResult r = RunFrames(h, dev, 90u, 1.0 / 30.0, 300ull);
		EXPECT_TRUE(r.hung) << "设备不报，钟就停：这一格本就走不完";
		EXPECT_EQ(r.dropped, 0ull);
		EXPECT_EQ(r.presented, 1ull);
		double sec = -1.0;
		ASSERT_EQ(CLV_PaceClockSeconds(h, &sec), CLV_PACE_OK);
		EXPECT_DOUBLE_EQ(sec, 0.0);
		uint32_t dead = 0u, regress = 999u;
		ASSERT_EQ(CLV_PaceStats(h, &dead, &regress, nullptr, nullptr), CLV_PACE_OK);
		EXPECT_GT(dead, 0u);
		EXPECT_EQ(regress, 0u);
	}

	/* 读数报成零：旧口径会把它读成全已消费而一路丢到底；现在没有报告就没有钟位 —— 报警、不丢帧。 */
	TEST_F(PaceApiTest, DeadQueueReportingZeroAlarms)
	{
		ASSERT_NO_FATAL_FAILURE(OpenOk());
		FeedSeconds(2.0);
		dev.consumes = false;
		dev.played = dev.fed;	 // 排队读数顶到 0
		dev.now_ns = 0ull;
		dev.next_pull_ns = 10'000'000'000ull;
		ASSERT_EQ(CLV_PaceSetDeviceRunning(h, 1u), CLV_PACE_OK);

		/* 不替消费端编报告：这一档要看的就是零报告 */
		const double period = 1.0 / 30.0;
		CLV_PaceDecisionC d;
		std::memset(&d, 0, sizeof(d));
		d.struct_size = sizeof(d);
		uint64_t presented = 0, dropped = 0;
		for (uint32_t i = 0; i < 6u; ++i)
		{
			dev.AdvanceTo(static_cast<uint64_t>(i) * static_cast<uint64_t>(period * 1e9));
			ASSERT_EQ(CLV_PaceDecide(h, i * period, &d), CLV_PACE_OK);
			EXPECT_DOUBLE_EQ(d.clock_seconds, 0.0) << "读数掉到零不等于消费掉，钟位不能被它推走";
			if (d.action == CLV_PACE_ACT_DROP) dropped++;
			else if (d.action == CLV_PACE_ACT_PRESENT) presented++;
		}
		EXPECT_EQ(dropped, 0ull);
		EXPECT_EQ(presented, 1ull);
		EXPECT_EQ(d.queue_ms, 0u);
		uint32_t dead = 0u;
		uint64_t dropped_bytes = 999ull;
		ASSERT_EQ(CLV_PaceStats(h, &dead, nullptr, nullptr, &dropped_bytes), CLV_PACE_OK);
		EXPECT_GT(dead, 0u);
		EXPECT_EQ(dropped_bytes, 0ull) << "读数掉到零不是调用方报过的丢";
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
