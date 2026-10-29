/* clv_demo_subtitle —— 字幕管线 + 双轨播放：同一个 .clv 的视频 / 音频照常播，
 * 另一个 .clv 的字幕流经解析器与渲染器出 RGBA 字幕层，由 SDL 叠在视频上。
 *
 * 用法：clv_demo_subtitle.exe [av.clv] [sub.clv]
 *   默认 av  = ../assets/fixtures/demo_avs_360p_q32.clv（或 assets/ 下同名）
 *   默认 sub = ../assets/fixtures/subtitle_pipeline.clv（或 assets/ 下同名）
 *
 * player 逻辑只在本文件（demo 局部件）：公开头里没有 player。视频/音频/节拍三段与
 * `video.c` 同一形状（YUV 三平面直上纹理、设备取走的量按块报告给节器件当主时钟、`CLV_Pace` 判定）；
 * 本件多出来的一段是字幕：解析 → 按当前视频时间布局 → 渲染 RGBA 层 → 作为第二张
 * 纹理叠在视频上（straight alpha，混合交给 SDL）。库只出字幕层，混合归宿主。
 *
 * 退出：ESC / q / 关窗。空格 = 暂停 / 继续。默认播完一趟自动收，末行
 * `DEMO_SUBTITLE_DONE shown=… dropped=… frames=… blocks=… glyphs=…`，退出码 0 = 有过字幕帧且 dropped 为 0。
 */
#include "subtitle_player.h"

#include "CLV_Audio.h"
#include "CLV_Container.h"
#include "CLV_Pace.h"
#include "CLV_Subtitle.h"
#include "CLV_Video.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Player
{
	CLV_ContainerReader* rd;
	CLV_VideoDecoder* vd;
	CLV_AudioDecoder* ad;
	SDL_Window* win;
	SDL_Renderer* ren;
	SDL_Texture* tex;
	SDL_AudioStream* ast;
	CLV_SubtitleParser* sp;
	CLV_SubtitleRenderer* sr;
	SDL_Texture* subtex;
	uint8_t* subpix;
	/* 全局样式块缓冲：SetStyleBlock 交出去的是借用视图，parser 与 renderer 都要用到各自 Close 为止，
	 * 所以这份缓冲的寿命必须挂到播放器上，不能在 open_subtitles 里就放掉。 */
	uint8_t* style_block;
	uint32_t subw, subh;
	unsigned sub_shown, sub_blocks, sub_lines;
	unsigned v_pkts, a_pkts, s_pkts, presented;
	double wall_seconds_dbg;
	uint32_t sub_glyphs;
	int sub_visible;

	unsigned vid, aid;
	uint32_t vw, vh; /* 显示尺寸：纹理按它建，不是补后尺寸 */
	double fps;
	unsigned tb_num, tb_den;	  /* 视频流时间基：fps = tb_den / tb_num，节器件按有理数吃 */
	unsigned long long period_ns; /* 只用于等待期间的重复出画节奏；阈值本身全在节器件里 */
	unsigned rate, chans, bpf;	  /* bpf = 一帧音频样本的字节数 = channels × 2 */
	unsigned long long fed_bytes; /* 累计喂进音频流的字节；与仍排队的读数一起折算成报给节器件的主钟供数 */
	unsigned long long t0_ns, pass_t0_ns, v_dts;
	int eof_reached; /* 包区已尽，不再等钟 */
	CLV_Pace* pace;	 /* 主时钟与逐帧判定都从它问 */

	unsigned long long dec_ns, pres_ns, feed_ns;

	unsigned long long wait_ns, read_ns; /* 一帧时间去向：睡掉多少、读包花多少 */
	unsigned long long backlog_ns;		 // 背压闸里堵掉的墙钟，算进「其余」就等于没拆出来
	unsigned backlog_blocks;			 // 真堵过（进等待循环）的次数
	unsigned shown, dropped, repeated, late, in_band, a_decoded, a_bytes;
	unsigned hint_dropped, hint_duped; /* 执行侧的丢 / 重帧，与节器件判的 DROP 分开计 */
	uint32_t ratio_q16;				   /* 已落到音频流上的倍率（Q16），挪开一挡才重设 */
	int clock_warned;				   /* 主钟报告被拒只说一次 */
	int quit, paused, topmost;
	int probe; /* --probe：把趟首每一段与每帧的钟位拆开打点，偏差出在哪一段直接读出来 */
};

/* 趟首探针：一条打印 = 一段的累计耗时 + 本段耗时 + 那一刻音频流的三个量。
   「已消费」= 已喂 − 仍排队，是设备侧真取走的字节，也正是交给节器件当主钟的那一笔。主时钟的口径
   （只跟报告走、暂停期间不推进、从起播零点起算）全在库侧节器件里，这里不重读钟 —— 探针自己读一次钟
   就会把起播零点挪到探针的读数时刻上。 */
static void probe_step(struct Player* p, const char* label, unsigned long long t0, unsigned long long* plast)
{
	const unsigned long long now = (unsigned long long) SDL_GetTicksNS();
	if (p->probe)
	{
		const unsigned long long q = (unsigned long long) SDL_GetAudioStreamQueued(p->ast);
		const unsigned long long c = p->fed_bytes > q ? p->fed_bytes - q : 0ull;
		printf("[探针] %-12s 累计 %6.2f ms 本段 %6.2f ms | fed %8llu B 排队 %8llu B 已消费 %8llu B（钟位 %7.2f ms）\n",
			   label, (double) (now - t0) / 1e6, (double) (now - *plast) / 1e6, p->fed_bytes, q, c,
			   1000.0 * (double) c / ((double) p->rate * (double) p->bpf));
		fflush(stdout);
	}
	*plast = now;
}

/* 帧级探针：把节器件给出的判定与设备侧原始量并排打出来。 */
static void probe_frame(struct Player* p, const CLV_PaceDecisionC* d, double vpts_s)
{
	const unsigned long long now = (unsigned long long) SDL_GetTicksNS();
	const unsigned long long q = (unsigned long long) SDL_GetAudioStreamQueued(p->ast);
	const unsigned long long c = p->fed_bytes > q ? p->fed_bytes - q : 0ull;
	printf("[探针] f=%4u 趟龄 %8.2f ms | 视频钟 %8.2f 音频钟 %8.2f 差 %+7.2f | 视频−墙 %+7.2f 音频−墙 %+7.2f | 排队 "
		   "%8llu 已消费 %8llu（钟位 %8.2f ms）\n",
		   p->shown, (double) (now - p->pass_t0_ns) / 1e6, vpts_s * 1000.0, d->clock_seconds * 1000.0,
		   d->dev_seconds * 1000.0, vpts_s * 1000.0 - (double) (now - p->pass_t0_ns) / 1e6,
		   d->clock_seconds * 1000.0 - (double) (now - p->pass_t0_ns) / 1e6, q, c,
		   1000.0 * (double) c / ((double) p->rate * (double) p->bpf));
	fflush(stdout);
}

/* 设备侧回调：把 SDL 音频流里还没播出去的字节数交出去；问不出返 0，不让节器件把失败读成零。 */
static uint8_t PaceQueued(void* user, uint64_t* out_queued)
{
	struct Player* p = (struct Player*) user;
	const int q = SDL_GetAudioStreamQueued(p->ast);
	if (q < 0) return 0;
	*out_queued = (uint64_t) q;
	return 1;
}

/* 主钟的供数口：设备取走的量（已喂 − 仍排队）换算成媒体时间。除法在前、余数补后，免得累计字节乘 1e9 撑破 64 位。
 * 排队量问不出就不报：一次问不出不是一个凭空的零。 */
static void pace_report_clock(struct Player* p)
{
	uint64_t q = 0;
	const unsigned long long bps = (unsigned long long) p->rate * p->bpf;
	if (! PaceQueued(p, &q) || bps == 0ull) return;
	const unsigned long long consumed = p->fed_bytes > q ? p->fed_bytes - q : 0ull;
	const unsigned long long ticks = (consumed / bps) * 1000000000ull + (consumed % bps) * 1000000000ull / bps;
	const CLV_PaceError e = CLV_PaceNoteConsumedMediaTime(p->pace, ticks);
	if (e != CLV_PACE_OK && ! p->clock_warned)
	{
		p->clock_warned = 1;
		fprintf(stderr, "  [警告] 主钟报告被拒：%s —— 报告值必须单调不减\n", CLV_PaceStrError(e));
	}
}

/* 倍率交给 SDL 的重采样比例（不动 PCM），丢 / 重帧交给视频腿：节器件只建议，不执行。 */
static void pace_apply_rate(struct Player* p, const CLV_PaceDecisionC* d)
{
	const uint32_t want = d->resample_ratio_q16;
	const uint32_t gap = want > p->ratio_q16 ? want - p->ratio_q16 : p->ratio_q16 - want;
	if (want != 0u && (p->ratio_q16 == 0u || gap >= 256u)) /* 小于 0.4 % 的抖动不值得重设一次流 */
	{
		p->ratio_q16 = want;
		if (! SDL_SetAudioStreamFrequencyRatio(p->ast, (float) want / 65536.0f))
			fprintf(stderr, "  [警告] 设重采样倍率：%s\n", SDL_GetError());
	}
}

/* 事件泵：退出与暂停。返回 1 = 该退出了。 */
static int pump_events(struct Player* p)
{
	SDL_Event ev;
	while (SDL_PollEvent(&ev))
	{
		if (ev.type == SDL_EVENT_QUIT) return 1;
		if (ev.type == SDL_EVENT_KEY_DOWN && (ev.key.key == SDLK_ESCAPE || ev.key.key == SDLK_Q)) return 1;
		if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_SPACE)
		{
			p->paused = ! p->paused;
			if (p->paused) SDL_PauseAudioStreamDevice(p->ast);
			else SDL_ResumeAudioStreamDevice(p->ast);
			/* 设备的放开与按住要同步告诉节器件：主钟只跟报告走，这个开关管的是起播零点与死队列计时。
			   按住期间没有报告是预期内的，不该被报成死队列。 */
			CLV_PaceSetDeviceRunning(p->pace, p->paused ? 0u : 1u);
			printf("[暂停] %s\n", p->paused ? "是" : "否");
			fflush(stdout);
		}
	}
	return 0;
}

static void paint(struct Player* p)
{
	SDL_RenderTexture(p->ren, p->tex, NULL, NULL);
	if (p->subtex && p->sub_visible) SDL_RenderTexture(p->ren, p->subtex, NULL, NULL);
	SDL_RenderPresent(p->ren);
}

static int feed_audio(struct Player* p, const uint8_t* payload, size_t size);

/* 供给腿往前走一格：音频进流、视频进解码器，不等出点。出画腿等的时候必须还能供数据 ——
 * 主钟只跟报告走，供给一停报告就停，等下去就是死锁。返回 0 = 读走一包，1 = 包区已尽，-1 = 该退。 */
static int pump_ahead(struct Player* p)
{
	CLV_FrameC f;
	uint8_t have = 0;
	const unsigned long long t = (unsigned long long) SDL_GetTicksNS();
	if (CLV_ContainerReaderNextFrame(p->rd, &f, &have) != CLV_CONTAINER_OK)
	{
		fprintf(stderr, "读包失败，停在这里\n");
		return -1;
	}
	p->read_ns += (unsigned long long) SDL_GetTicksNS() - t;
	if (! have)
	{
		p->eof_reached = 1;
		return 1;
	}
	if (f.stream_id == p->aid)
	{
		p->a_pkts++;
		return feed_audio(p, f.payload, f.payload_size) == 0 ? 0 : -1;
	}
	if (f.stream_id != p->vid) return 0;
	p->v_pkts++;
	p->v_dts += f.dts_delta;
	CLV_VideoFrameInfoC vi;
	SDL_zero(vi);
	const CLV_VideoError ve =
		CLV_VideoDecoderDecode(p->vd, f.payload, f.payload_size, f.fragments_expected ? f.fragments_expected : 1,
							   (int64_t) p->v_dts, (int64_t) (p->v_dts + (uint64_t) f.pts_delta), &vi);
	if (ve != CLV_VIDEO_OK)
	{
		fprintf(stderr, "视频解码失败（dts=%llu）：%s\n", p->v_dts, CLV_VideoStrError(ve));
		return -1;
	}
	return 0;
}

/* 当前视频时刻的字幕层：解析器里所有落在时间窗内的块各自布局，渲染器合成一张 RGBA 层。 */
static void subtitle_update(struct Player* p, double vpts_s)
{
	p->sub_visible = 0;
	if (! p->sp || ! p->sr || ! p->subpix) return;
	const int64_t t = (int64_t) (vpts_s * 1081080000.0 + 0.5);
	memset(p->subpix, 0, (size_t) p->subw * p->subh * 4u);
	uint32_t nb = 0;
	CLV_SubtitleParserBlockCount(p->sp, &nb);
	for (uint32_t bi = 0; bi < nb; ++bi)
	{
		CLV_SubtitleBlockC b;
		memset(&b, 0, sizeof(b));
		if (CLV_SubtitleParserBlockAt(p->sp, bi, &b) != CLV_SUBTITLE_OK) continue;
		if (b.displayable == 0u || b.line_count == 0u) continue;
		if (t < b.start_ticks || t >= b.end_ticks) continue;
		CLV_SubtitleLineC lines[64];
		uint32_t count = b.line_count > 64u ? 64u : b.line_count;
		memset(lines, 0, sizeof(lines));
		int ok = 1;
		for (uint32_t li = 0; li < count; ++li)
		{
			if (CLV_SubtitleParserLineAt(p->sp, b.first_line + li, &lines[li]) != CLV_SUBTITLE_OK)
			{
				ok = 0;
				break;
			}
		}
		if (! ok) continue;
		if (CLV_SubtitleRendererLayout(p->sr, &b, lines, count, NULL, t) != CLV_SUBTITLE_OK) continue;
	}
	if (CLV_SubtitleRendererRender(p->sr, p->subpix, (size_t) p->subw * 4u, p->subw, p->subh) != CLV_SUBTITLE_OK)
		return;
	uint32_t rects = 0;
	CLV_SubtitleRendererDirtyRects(p->sr, NULL, 0u, &rects);
	p->sub_visible = rects > 0u;
	if (p->sub_visible)
	{
		if (! SDL_UpdateTexture(p->subtex, NULL, p->subpix, (int) p->subw * 4u))
			fprintf(stderr, "  [警告] 更新字幕纹理：%s\n", SDL_GetError());
		p->sub_shown++;
	}
	CLV_SubtitleRenderStatsC rs;
	memset(&rs, 0, sizeof(rs));
	if (CLV_SubtitleRendererReport(p->sr, &rs) == CLV_SUBTITLE_OK) p->sub_glyphs = rs.glyphs;
}

/* 等到这一帧的出点：动作（等 / 出画 / 丢）由库侧节器件判，本函数只负责执行「等」。
   等待期间每周期重复呈现当前帧（窗口不能晾着），计 repeat。返回 1 = 调用方该退了。 */
static int pace_to(struct Player* p, double video_pts_s, CLV_PaceDecisionC* out)
{
	unsigned long long last_rep = 0;
	int guard = 0;
	for (;;)
	{
		if (pump_events(p)) return 1;
		pace_report_clock(p);
		const CLV_PaceError pe = CLV_PaceDecide(p->pace, video_pts_s, out);
		if (pe != CLV_PACE_OK && pe != CLV_PACE_CLOCK_SKEW)
		{
			fprintf(stderr, "  [警告] 节拍判定：%s —— 这一帧照常出，不叫节器件的故障变成停播\n", CLV_PaceStrError(pe));
			return 0;
		}
		if (out->action != CLV_PACE_ACT_WAIT) return 0;
		if (p->eof_reached) return 0; /* 包区已尽，钟不再前进：尾帧按出点直接出 */
		if (p->paused)
		{
			SDL_Delay(5);
			continue;
		}
		if (p->v_pkts < p->shown + p->dropped + 3u && pump_ahead(p) < 0) return 1;
		/* 等待分两类，执行方式必须不同（一律按周期重绘时，视频钟相对墙钟中位落后 23.9 ms，
		 *   滚出一整个周期后开始连丢）：
		 *   音频闸（视频超前于音频钟）=> 画面停在当前帧上，按周期重绘一次，计 repeat；
		 *   墙钟闸（还没到这一帧的出点）=> 什么都不画，睡到出点。 */
		const int audio_held = out->dev_seconds > out->period_seconds / 2.0;
		const unsigned long long now = (unsigned long long) SDL_GetTicksNS();
		if (audio_held && now - last_rep >= p->period_ns)
		{
			paint(p);
			p->repeated++;
			last_rep = now;
			printf("f=%4u 等待音频 dev=%+7.1fms repeat=%u\n", p->shown, out->dev_seconds * 1000.0, p->repeated);
			fflush(stdout);
		}
		unsigned long long ms = out->wait_ns / 1000000ull;
		if (ms < 1ull) ms = 1ull;
		if (ms > 5ull) ms = 5ull; /* 睡 5 ms 就回来重判：出点与音频闸都要有人盯 */
		p->wait_ns += ms * 1000000ull;
		SDL_Delay((Uint32) ms);
		if (++guard > 200000)
		{
			fprintf(stderr, "  [警告] 等待重判次数异常，按到点处理\n");
			out->action = CLV_PACE_ACT_PRESENT;
			return 0;
		}
	}
}

/* 把一帧 I420 平面直接推上纹理。返回本次耗时纳秒。
 * 这里的 f 只是借来的视图：平面指针有效到该帧最后一个引用被归还为止，归还在调用侧做，
 * 本函数既不 AddRef 也不归还 —— 一次持有只许有一个所有者。 */
static void shot(struct Player* p, double media_s)
{
	p->presented++;
	printf("[抽查] presented=%u shown=%u media=%.3fs fps=%.3f\n", p->presented, p->shown, media_s, p->fps);
	fflush(stdout);
	if (p->presented != 5u && p->presented != 45u && p->presented != 80u) return;
	SDL_Surface* s = SDL_RenderReadPixels(p->ren, NULL);
	if (s == NULL) return;
	SDL_CreateDirectory("demo_shots");
	char name[64];
	snprintf(name, sizeof(name), "demo_shots/sub_%04u.bmp", p->presented);
	if (SDL_SaveBMP(s, name)) printf("[截图] %s\n", name);
	SDL_DestroySurface(s);
	fflush(stdout);
}

static unsigned long long present(struct Player* p, CLV_VideoFrame* f, double media_s)
{
	CLV_VideoFrameDescC d;
	if (CLV_VideoFrameGetDesc(f, &d, NULL) != CLV_VIDEO_OK) return 0;
	const unsigned long long t = (unsigned long long) SDL_GetTicksNS();
	if (! SDL_UpdateYUVTexture(p->tex, NULL, d.plane[0], (int) d.stride[0], d.plane[1], (int) d.stride[1], d.plane[2],
							   (int) d.stride[2]))
	{
		fprintf(stderr, "  [警告] UpdateYUVTexture：%s\n", SDL_GetError());
	}
	subtitle_update(p, media_s);
	paint(p);
	shot(p, media_s);
	return (unsigned long long) SDL_GetTicksNS() - t;
}

static int feed_audio(struct Player* p, const uint8_t* payload, size_t size)
{
	const unsigned long long t = (unsigned long long) SDL_GetTicksNS();
	CLV_AudioBuffer* buf = NULL;
	uint8_t got = 0;
	CLV_AudioFrameInfoC fi;
	SDL_zero(fi);
	const CLV_AudioError ae = CLV_AudioDecoderDecodeFrame(p->ad, payload, size, &buf, &got, &fi);
	if (ae != CLV_AUDIO_OK)
	{
		fprintf(stderr, "音频解码失败：%s\n", CLV_AudioStrError(ae));
		return 1;
	}
	if (got && buf)
	{
		CLV_AudioBufferDescC bd;
		if (CLV_AudioBufferGetDesc(buf, &bd) == CLV_AUDIO_OK && bd.data != NULL)
		{
			if (! SDL_PutAudioStreamData(p->ast, bd.data, (int) bd.size))
				fprintf(stderr, "灌音频失败：%s\n", SDL_GetError());
			p->fed_bytes += bd.size;
			CLV_PaceNoteAudioFed(p->pace, (uint64_t) bd.size); /* 只进积压账，主钟不看它 */
			pace_report_clock(p);
			p->a_bytes += (unsigned) bd.size;
			p->a_decoded++;
		}
		CLV_AudioBufferRelease(buf);
	}
	p->feed_ns += (unsigned long long) SDL_GetTicksNS() - t;
	return 0;
}

/* 喂音频后的背压闸：仍排队超过库侧阈值就等它降下来。这一步堵的是整条读包的腿，
   视频帧在解码器里等着，出来的时候出点已过期 ⇒ 成串丢帧。落账与打点都要留下这一段的墙钟。 */
static void drain_backlog(struct Player* p)
{
	uint8_t allowed = 1u;
	if (CLV_PaceFeedAllowed(p->pace, &allowed) != CLV_PACE_OK || allowed) return;
	const unsigned long long t0 = (unsigned long long) SDL_GetTicksNS();
	p->backlog_blocks++;
	while (! p->quit && CLV_PaceFeedAllowed(p->pace, &allowed) == CLV_PACE_OK && ! allowed)
	{
		/* 还堆着就等：阈值在库侧（缺省 1000 ms 当量，取在实测的排队自然峰值之上）。
		   等的这段设备仍在取数，不报就把主钟停在堵闸之前。 */
		pace_report_clock(p);
		SDL_Delay(2);
		if (pump_events(p)) return;
	}
	const unsigned long long d = (unsigned long long) SDL_GetTicksNS() - t0;
	p->backlog_ns += d;
	if (p->probe)
	{
		printf("[探针] 背压堵 f=%4u 本段 %6.2f ms 排队 %8llu B（第 %u 次）\n", p->shown, (double) d / 1e6,
			   (unsigned long long) SDL_GetAudioStreamQueued(p->ast), p->backlog_blocks);
		fflush(stdout);
	}
}

/* 一跑：重开读方、复位两条解码器、预灌音频、起钟。 */
static int start_pass(struct Player* p, const char* path)
{
	const unsigned long long pt0 = (unsigned long long) SDL_GetTicksNS();
	unsigned long long plast = pt0;
	probe_step(p, "进趟", pt0, &plast);
	if (p->rd)
	{
		CLV_ContainerReaderClose(p->rd);
		p->rd = NULL;
	}
	CLV_ContainerError ce = CLV_CONTAINER_OK;
	p->rd = CLV_ContainerReaderOpenFile(path, &ce);
	if (! p->rd)
	{
		fprintf(stderr, "打不开 %s：%s\n", path, CLV_ContainerStrError(ce));
		return 1;
	}
	probe_step(p, "重开读方", pt0, &plast);
	CLV_VideoDecoderReset(p->vd);
	CLV_AudioDecoderReset(p->ad);
	/* 上一趟残留的未播数据必须一起清掉：设备还在播它，而 fed_bytes 已归零，
	   换算出来的音频钟会整体前移一段实测 ~53 ms，视频就被判成迟到而连丢帧。 */
	SDL_ClearAudioStream(p->ast);
	/* 预灌期间设备不能还在跑：这一趟的时钟锚要等预灌结束才起，设备若已在播，
	   预灌那 ~50 ms 就被算进已消费量，音频钟平白超前一个丢帧阈，第一帧起就一路丢。
	   第 2 趟及以后尤其明显（设备从上一趟的 resume 状态直接继承）。 */
	SDL_PauseAudioStreamDevice(p->ast);
	probe_step(p, "复位+清流+暂停", pt0, &plast);
	p->shown = p->dropped = p->repeated = p->late = p->in_band = p->a_decoded = p->a_bytes = 0;
	p->dec_ns = p->pres_ns = p->feed_ns = 0;
	p->wait_ns = p->read_ns = p->backlog_ns = 0; /* 这几项必须随趟复位：第 2 趟起分母是本趟帧数、
	 分子一旦带着上一趟的量，「睡 / 读包 / 其余」三列直接读成假数 */
	p->backlog_blocks = 0;
	p->fed_bytes = 0;
	p->v_dts = 0ull;
	p->eof_reached = 0;
	p->hint_dropped = p->hint_duped = 0;
	p->clock_warned = 0;
	p->ratio_q16 = 0u; /* 上一趟的倍率不带进这一趟 */
	if (! SDL_SetAudioStreamFrequencyRatio(p->ast, 1.0f))
		fprintf(stderr, "  [警告] 复位重采样倍率：%s\n", SDL_GetError());
	if (CLV_PaceBeginPass(p->pace) != CLV_PACE_OK)
	{
		fprintf(stderr, "节器件起趟失败\n");
		return 1;
	}

	/* 预灌 200 ms */
	{
		const unsigned long long want = (unsigned long long) (0.2 * (double) p->rate) * p->bpf;
		int guard = 0;
		while (p->fed_bytes < want && guard++ < 8000)
		{
			CLV_FrameC f;
			uint8_t have = 0;
			if (CLV_ContainerReaderNextFrame(p->rd, &f, &have) != CLV_CONTAINER_OK || ! have) break;
			if (f.stream_id == p->aid)
			{
				p->a_pkts++;
				feed_audio(p, f.payload, f.payload_size);
			}
			else if (f.stream_id == p->vid)
			{
				p->v_pkts++;
				/* 预灌段里碰到的视频包照常喂解码器，dts 从 0 起累加 */
				CLV_VideoFrameInfoC vi;
				SDL_zero(vi);
				if (CLV_VideoDecoderDecode(p->vd, f.payload, f.payload_size,
										   f.fragments_expected ? f.fragments_expected : 1, 0, 0, &vi) != CLV_VIDEO_OK)
				{
					fprintf(stderr, "  [警告] 预灌段第 %d 个视频包解码失败，正片走到它时会再报一次\n", guard);
				}
			}
		}
		printf("[预灌] 音频 %llu B（约 %.0f ms）后放开设备\n", p->fed_bytes,
			   1000.0 * (double) p->fed_bytes / ((double) p->rate * (double) p->bpf));
	}
	fflush(stdout);
	probe_step(p, "预灌完", pt0, &plast);
	/* 起钟前先空跑一次呈现：首帧的纹理上传与渲染器初始化要 50 ms 量级，
	 * 把它算进第一帧的账会让音频钟凭空超前两帧，头几帧被误判成迟到而丢掉。 */
	paint(p);
	probe_step(p, "空跑呈现", pt0, &plast);
	SDL_ResumeAudioStreamDevice(p->ast);
	p->t0_ns = (unsigned long long) SDL_GetTicksNS();
	p->pass_t0_ns = p->t0_ns;
	CLV_PaceSetDeviceRunning(p->pace, 1u); /* 钟从这一刻起走；预灌段一直是 0，那段不计入已消费 */
	probe_step(p, "放开设备", pt0, &plast);
	return 0;
}

static int open_media(struct Player* p, const char* path)
{
	CLV_ContainerError ce = CLV_CONTAINER_OK;
	p->rd = CLV_ContainerReaderOpenFile(path, &ce);
	if (! p->rd)
	{
		fprintf(stderr, "打不开 %s：%s\n", path, CLV_ContainerStrError(ce));
		return 1;
	}
	uint32_t ns = 0;
	if (CLV_ContainerReaderStreamCount(p->rd, &ns) != CLV_CONTAINER_OK)
	{
		fprintf(stderr, "数不出流条数，读方状态不成立\n");
		return 1;
	}
	CLV_StreamDescC vdesc, adesc;
	SDL_zero(vdesc);
	SDL_zero(adesc);
	uint8_t have_v = 0, have_a = 0;
	for (uint32_t i = 0; i < ns; ++i)
	{
		CLV_StreamDescC d;
		uint8_t usable = 0;
		if (CLV_ContainerReaderGetStream(p->rd, i, &d, &usable) != CLV_CONTAINER_OK || ! usable) continue;
		if (d.stream_type == 0 && ! have_v)
		{
			vdesc = d;
			p->vid = d.stream_id;
			have_v = 1;
		}
		else if (d.stream_type == 1 && ! have_a)
		{
			adesc = d;
			p->aid = d.stream_id;
			have_a = 1;
		}
	}
	if (! have_v || ! have_a)
	{
		fprintf(stderr, "这个文件不是双轨（视频 %s / 音频 %s）—— 本示例要求两条流都在同一个文件里\n",
				have_v ? "在" : "缺", have_a ? "在" : "缺");
		return 1;
	}

	CLV_VideoEncMetaC em;
	SDL_zero(em);
	uint8_t streaminfo[64];
	size_t si_len = 0;
	uint32_t nx = 0;
	if (CLV_ContainerReaderExtBlockCount(p->rd, &nx) != CLV_CONTAINER_OK)
	{
		fprintf(stderr, "数不出扩展块条数，读方状态不成立\n");
		return 1;
	}
	for (uint32_t i = 0; i < nx; ++i)
	{
		CLV_ExtBlockC b;
		if (CLV_ContainerReaderGetExtBlock(p->rd, i, &b) != CLV_CONTAINER_OK) continue;
		if (b.stream_id == p->vid && b.ext_type == 3 && CLV_VideoParseEncMeta(b.data, b.size, &em) != CLV_VIDEO_OK)
			fprintf(stderr, "  [警告] 编码元数据块读不成，建视频解码器时要它\n");
		if (b.stream_id == p->aid && b.ext_type == 5 && b.size <= sizeof(streaminfo))
		{
			memcpy(streaminfo, b.data, b.size);
			si_len = b.size;
		}
	}
	if (! em.display_width || ! si_len)
	{
		fprintf(stderr, "缺视频参数块（ext_type=3）或 FLAC 流信息块（ext_type=5），建不了句柄\n");
		return 1;
	}
	p->vw = em.display_width;
	p->vh = em.display_height;

	CLV_VideoStreamParamsC vp;
	SDL_zero(vp);
	vp.struct_size = sizeof(vp);
	vp.bit_depth = 8;
	vp.quant_precision = em.quant_precision;
	vp.max_b_frames = em.max_b_frames;
	vp.display_width = em.display_width;
	vp.display_height = em.display_height;
	vp.obmc_disable = 1;
	vp.preroll_frames = 0; /* 0 = 未声明 ⇒ 取地板 max_b_frames + 2 */
	vp.prefill_mode = CLV_PREFILL_DEFAULT;
	CLV_VideoError ve = CLV_VIDEO_OK;
	p->vd = CLV_VideoDecoderOpen(&vp, &ve);
	if (! p->vd)
	{
		fprintf(stderr, "视频句柄建不起来：%s\n", CLV_VideoStrError(ve));
		return 1;
	}

	CLV_AudioStreamInfoC asi;
	SDL_zero(asi);
	if (CLV_AudioParseStreamInfo(streaminfo, si_len, &asi) != CLV_AUDIO_OK)
		fprintf(stderr, "  [警告] STREAMINFO 解析不成，声道数/采样率退回流描述符的值\n");
	CLV_AudioStreamParamsC asp;
	SDL_zero(asp);
	asp.struct_size = sizeof(asp);
	asp.sample_rate = asi.sample_rate ? asi.sample_rate : adesc.sample_rate;
	asp.channels = asi.channels ? asi.channels : adesc.channels;
	asp.bit_depth = asi.bit_depth ? asi.bit_depth : adesc.bit_depth;
	asp.max_packet_size = adesc.max_packet_size;
	CLV_AudioError ae = CLV_AUDIO_OK;
	p->ad = CLV_AudioDecoderOpen(&asp, streaminfo, si_len, &ae);
	if (! p->ad)
	{
		fprintf(stderr, "音频句柄建不起来：%s\n", CLV_AudioStrError(ae));
		return 1;
	}
	p->rate = asp.sample_rate;
	p->chans = asp.channels;
	p->bpf = asp.channels * 2u; /* 本示例的音频腿是 s16 */

	/* 帧率不取自流时间基：时间基可能只是全局 tick（1/1081080000），不是帧间隔。
	 * 扫包读前两个不同的视频 PTS，按时间基折成秒，取倒数。 */
	double fps = 30.0;
	{
		int64_t dts = 0;
		int64_t last_pts = 0;
		int have_last = 0;
		CLV_FrameC q;
		uint8_t g = 0;
		while (CLV_ContainerReaderNextFrame(p->rd, &q, &g) == CLV_CONTAINER_OK && g)
		{
			dts += (int64_t) q.dts_delta;
			if (q.stream_id != p->vid) continue;
			const int64_t pts = dts + q.pts_delta;
			if (have_last && pts > last_pts)
			{
				const long double sec = (long double) (pts - last_pts) * (long double) vdesc.timebase_num /
										(long double) vdesc.timebase_den;
				if (sec > 0.0L) fps = (double) (1.0L / sec);
				break;
			}
			last_pts = pts;
			have_last = 1;
		}
		CLV_ContainerReaderClose(p->rd);
		p->rd = NULL;
	}
	if (fps < 1.0 || fps > 240.0) fps = 30.0;
	p->fps = fps;
	p->tb_num = 1000u;
	p->tb_den = (unsigned) (fps * 1000.0 + 0.5);
	p->period_ns = (unsigned long long) (1e9 / p->fps); /* 只用于等待期间的重复出画节奏 */
	printf("[流] 视频 id=%u %ux%u fps=%.6g   音频 id=%u %u Hz / %u ch / %u bit\n", p->vid, p->vw, p->vh, p->fps, p->aid,
		   p->rate, p->chans, asp.bit_depth);
	printf("[口径] 周期 %.1f ms  对齐带 ±%.1f ms  丢帧阈 −%.1f ms  背压与预灌取库缺省  主时钟=音频（节器件）\n",
		   (double) p->period_ns / 1e6, (double) p->period_ns / 2e6, (double) p->period_ns / 1e6);
	return 0;
}

static int open_subtitles(struct Player* p, const char* path)
{
	CLV_ContainerError ce = CLV_CONTAINER_OK;
	CLV_ContainerReader* rd = CLV_ContainerReaderOpenFile(path, &ce);
	if (! rd)
	{
		fprintf(stderr, "打不开字幕文件 %s：%s\n", path, CLV_ContainerStrError(ce));
		return 1;
	}
	uint32_t ns = 0;
	CLV_ContainerReaderStreamCount(rd, &ns);
	CLV_StreamDescC sdesc;
	SDL_zero(sdesc);
	uint8_t have_s = 0;
	for (uint32_t i = 0; i < ns; ++i)
	{
		CLV_StreamDescC d;
		uint8_t usable = 0;
		if (CLV_ContainerReaderGetStream(rd, i, &d, &usable) != CLV_CONTAINER_OK || ! usable) continue;
		if (d.stream_type == 2)
		{
			sdesc = d;
			have_s = 1;
			break;
		}
	}
	if (! have_s)
	{
		fprintf(stderr, "字幕文件里没有 stream_type = 2 的流\n");
		CLV_ContainerReaderClose(rd);
		return 1;
	}

	int64_t dts = 0;
	uint8_t* block = NULL;
	size_t block_size = 0;
	uint32_t nx = 0;
	CLV_ContainerReaderExtBlockCount(rd, &nx);
	for (uint32_t i = 0; i < nx; ++i)
	{
		CLV_ExtBlockC b;
		memset(&b, 0, sizeof(b));
		if (CLV_ContainerReaderGetExtBlock(rd, i, &b) != CLV_CONTAINER_OK) continue;
		if (b.stream_id != sdesc.stream_id || b.ext_type != 4 || b.data == NULL || b.size == 0u) continue;
		block = (uint8_t*) malloc(b.size);
		if (block == NULL) break;
		memcpy(block, b.data, b.size);
		block_size = b.size;
		break;
	}

	CLV_SubtitleError se = CLV_SUBTITLE_OK;
	CLV_SubtitleParserParamsC pp;
	SDL_zero(pp);
	pp.struct_size = sizeof(pp);
	CLV_SubtitleRendererParamsC rp;
	SDL_zero(rp);
	rp.struct_size = sizeof(rp);
	rp.width = p->vw;
	rp.height = p->vh;
	rp.metric_entry = CLV_SUBTITLE_METRIC_HB_OT_FONT;
	p->sp = CLV_SubtitleParserOpen(&pp, &se);
	p->sr = CLV_SubtitleRendererOpen(&rp, &se);
	if (! p->sp || ! p->sr)
	{
		fprintf(stderr, "字幕句柄建不起来：%s\n", CLV_SubtitleStrError(se));
		free(block);
		CLV_ContainerReaderClose(rd);
		return 1;
	}
	/* 字体目录按运行位置给两种相对写法；不存在的目录由 CAL 报 UNSUPPORTED，不影响其它目录。 */
	CLV_SubtitleRendererSetFontDir(p->sr, "../assets/fonts/NotoSansCJKsc");
	CLV_SubtitleRendererSetFontDir(p->sr, "../assets/fonts/noto-emoji");
	CLV_SubtitleRendererSetFontDir(p->sr, "assets/fonts/NotoSansCJKsc");
	CLV_SubtitleRendererSetFontDir(p->sr, "assets/fonts/noto-emoji");
	if (block != NULL)
	{
		CLV_SubtitleParserSetStyleBlock(p->sp, block, block_size);
		CLV_SubtitleRendererSetStyleBlock(p->sr, block, block_size);
		p->style_block = block; /* 所有权移交播放器，销毁段在两侧 Close 之后再放 */
		block = NULL;
	}

	CLV_FrameC f;
	memset(&f, 0, sizeof(f));
	uint8_t got = 0;
	while (CLV_ContainerReaderNextFrame(rd, &f, &got) == CLV_CONTAINER_OK && got)
	{
		/* dts_delta 是**本流**相邻帧的增量：只对字幕流累加，混进视频包的增量会把包时间翻倍。 */
		if (f.stream_id != sdesc.stream_id || f.payload_size == 0u) continue;
		dts += (int64_t) f.dts_delta;
		p->s_pkts++;
		const long double sec =
			(long double) (dts + f.pts_delta) * (long double) sdesc.timebase_num / (long double) sdesc.timebase_den;
		const int64_t ticks = (int64_t) (sec * 1081080000.0L + 0.5L);
		CLV_SubtitleParserFeed(p->sp, f.payload, f.payload_size, ticks);
	}
	CLV_ContainerReaderClose(rd);

	uint32_t nb = 0;
	CLV_SubtitleParserBlockCount(p->sp, &nb);
	p->sub_blocks = nb;
	uint32_t nl = 0;
	CLV_SubtitleParserLineCount(p->sp, &nl);
	p->sub_lines = nl;
	printf("[字幕] %u 块 / %u 行（%s）\n", nb, nl, path);
	return 0;
}

static int open_sdl(struct Player* p)
{
	if (! SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
	{
		fprintf(stderr, "SDL_Init：%s\n", SDL_GetError());
		return 1;
	}
	if (! SDL_CreateWindowAndRenderer("CLV 双轨播放器", (int) p->vw, (int) p->vh, 0, &p->win, &p->ren))
	{
		fprintf(stderr, "建窗口/渲染器：%s\n", SDL_GetError());
		return 1;
	}
	p->tex = SDL_CreateTexture(p->ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, (int) p->vw, (int) p->vh);
	if (! p->tex)
	{
		fprintf(stderr, "建 IYUV 纹理：%s\n", SDL_GetError());
		return 1;
	}
	p->subw = p->vw;
	p->subh = p->vh;
	p->subtex =
		SDL_CreateTexture(p->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, (int) p->subw, (int) p->subh);
	if (! p->subtex)
	{
		fprintf(stderr, "建字幕 RGBA 纹理：%s\n", SDL_GetError());
		return 1;
	}
	SDL_SetTextureBlendMode(p->subtex, SDL_BLENDMODE_BLEND);
	p->subpix = (uint8_t*) malloc((size_t) p->subw * p->subh * 4u);
	if (! p->subpix)
	{
		fprintf(stderr, "字幕像素缓冲分配失败\n");
		return 1;
	}
	SDL_AudioSpec spec;
	SDL_zero(spec);
	spec.channels = (int) p->chans;
	spec.freq = (int) p->rate;
	spec.format = SDL_AUDIO_S16;
	p->ast = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	if (! p->ast)
	{
		fprintf(stderr, "打不开音频设备：%s —— 双轨要求两轨同时出，本示例不退成单轨\n", SDL_GetError());
		return 1;
	}
	CLV_PaceParamsC pp;
	SDL_zero(pp);
	pp.struct_size = sizeof(pp);
	pp.fps_num = (uint32_t) p->tb_den;
	pp.fps_den = (uint32_t) p->tb_num;
	pp.sample_rate = (uint32_t) p->rate;
	pp.bytes_per_frame = (uint32_t) p->bpf;
	pp.queued = &PaceQueued;
	pp.queued_user = p; /* 库缺省背压 1000 ms、预灌 200 ms 当量，两个都给 0 走缺省 */
	CLV_PaceError pe = CLV_PACE_OK;
	p->pace = CLV_PaceOpen(&pp, &pe);
	if (! p->pace)
	{
		fprintf(stderr, "节器件建不起来：%s\n", CLV_PaceStrError(pe));
		return 1;
	}

	printf("[SDL] renderer=%s  纹理=IYUV（三平面直上，不经 RGB）  音频设备=%u  %u Hz / %u ch / s16\n",
		   SDL_GetRendererName(p->ren), (unsigned) SDL_GetAudioStreamDevice(p->ast), p->rate, p->chans);
	if (p->topmost)
	{
		SDL_SetWindowAlwaysOnTop(p->win, true);
		SDL_RaiseWindow(p->win);
	}
	int wx = 0, wy = 0, ww = 0, wh = 0;
	SDL_GetWindowPosition(p->win, &wx, &wy);
	SDL_GetWindowSize(p->win, &ww, &wh);
	printf("[窗口] x=%d y=%d w=%d h=%d topmost=%d\n", wx, wy, ww, wh, p->topmost);
	return 0;
}

int clv_demo_subtitle_run(const char* av_path, const char* sub_path)
{
	const char* av_default = "../assets/fixtures/demo_avs_360p_q32.clv";
	if (av_path == NULL) av_path = av_default;
	/* 只给一份文件时，字幕也从这份文件里找（三轨文件的情形）；第二参数可另指字幕文件。 */
	if (sub_path == NULL) sub_path = av_path;
	struct Player p;
	SDL_zero(p);
	if (open_media(&p, av_path) != 0 && open_media(&p, "assets/fixtures/demo_avs_360p_q32.clv") != 0) return 2;
	if (open_subtitles(&p, sub_path) != 0 && open_subtitles(&p, "assets/fixtures/subtitle_pipeline.clv") != 0) return 2;
	if (open_sdl(&p) != 0) return 2;

	int pass = 0;
	do
	{
		pass++;
		printf("\n=== 第 %d 趟 ===\n", pass);
		if (start_pass(&p, av_path) != 0) break;

		int done = 0;
		while (! done && ! p.quit)
		{
			CLV_FrameC f;
			uint8_t have = 0;
			const unsigned long long t_read0 = (unsigned long long) SDL_GetTicksNS();
			const CLV_ContainerError rdce = CLV_ContainerReaderNextFrame(p.rd, &f, &have);
			p.read_ns += (unsigned long long) SDL_GetTicksNS() - t_read0;
			if (rdce != CLV_CONTAINER_OK)
			{
				fprintf(stderr, "读包失败，停在这里\n");
				break;
			}
			if (! have)
			{
				p.eof_reached = 1;
				for (;;)
				{
					CLV_VideoFrame* fr = NULL;
					uint8_t got = 0;
					if (CLV_VideoDecoderFlush(p.vd, &fr, &got) != CLV_VIDEO_OK || ! got) break;
					/* 包区已尽：剩下的都是解码器里的尾帧。音频钟此时不再前进，
					 * 再按节拍等会停在这一帧上 ⇒ 尾帧直接出，不让收尾挂住窗口。 */
					p.pres_ns += present(&p, fr, (double) p.shown / p.fps);
					p.shown++;
					(void) CLV_VideoFrameFree(fr);
					fr = NULL;
				}
				done = 1;
				break;
			}
			if (f.stream_id == p.aid)
			{
				p.a_pkts++;
				feed_audio(&p, f.payload, f.payload_size);
				drain_backlog(&p);
				continue;
			}
			if (f.stream_id != p.vid) continue;
			p.v_pkts++;

			p.v_dts += f.dts_delta;
			const unsigned long long t = (unsigned long long) SDL_GetTicksNS();
			CLV_VideoFrameInfoC vi;
			SDL_zero(vi);
			const CLV_VideoError ve =
				CLV_VideoDecoderDecode(p.vd, f.payload, f.payload_size, f.fragments_expected ? f.fragments_expected : 1,
									   (int64_t) p.v_dts, (int64_t) (p.v_dts + (uint64_t) f.pts_delta), &vi);
			if (ve != CLV_VIDEO_OK)
			{
				fprintf(stderr, "视频解码失败（dts=%llu）：%s\n", p.v_dts, CLV_VideoStrError(ve));
				p.quit = 1;
				break;
			}
			p.dec_ns += (unsigned long long) SDL_GetTicksNS() - t;

			for (;;)
			{
				CLV_VideoFrame* fr = NULL;
				uint8_t got = 0;
				if (CLV_VideoDecoderNextOutput(p.vd, &fr, &got) != CLV_VIDEO_OK || ! got) break;
				const double vc = (double) p.shown / p.fps;
				CLV_PaceDecisionC dec;
				SDL_zero(dec);
				dec.struct_size = sizeof(dec);
				if (pace_to(&p, vc, &dec))
				{
					(void) CLV_VideoFrameFree(fr);
					fr = NULL;
					p.quit = 1;
					break;
				}
				pace_apply_rate(&p, &dec); /* 建议倍率先落到音频流，再谈这一帧画不画 */
				/* 节器件判的 DROP 这一路不执行（字幕要连着走）；执行侧的丢只削视频腿一格，shown 照常推进 */
				if (dec.drop_dup_hint == 1u)
				{
					p.hint_dropped++;
					(void) CLV_VideoFrameFree(fr);
					fr = NULL;
					p.shown++;
					if (pump_events(&p))
					{
						p.quit = 1;
						break;
					}
					continue;
				}
				if (dec.in_band) p.in_band++;
				else if (dec.late) p.late++;
				const unsigned long long tp = (unsigned long long) SDL_GetTicksNS();
				present(&p, fr, vc);
				p.pres_ns += (unsigned long long) SDL_GetTicksNS() - tp;
				if (dec.drop_dup_hint == 2u)
				{
					paint(&p); /* 提示级的重帧：同一帧多占一拍 */
					p.hint_duped++;
				}
				const CLV_VideoError fe = CLV_VideoFrameFree(fr);
				fr = NULL;
				if (fe != CLV_VIDEO_OK) fprintf(stderr, "  [警告] 归还帧失败：%s\n", CLV_VideoStrError(fe));
				p.shown++;
				if (pump_events(&p))
				{
					p.quit = 1;
					break;
				}
			}
		}

		const double wall = ((double) SDL_GetTicksNS() - (double) p.t0_ns) / 1e9;
		p.wall_seconds_dbg = wall;
		printf("\n--- 第 %d 趟收口 ---\n", pass);
		printf("视频 %u 帧（丢 %u / 重复 %u）  字幕帧 %u  字幕块 %u  墙钟 %.2f s\n", p.shown, p.dropped, p.repeated,
			   p.sub_shown, p.sub_blocks, wall);
		printf("速率匹配：建议倍率末值 %.4f（Q16 %u）  提示级丢 %u 帧 / 重帧 %u 次  对齐带内 %u / 带外迟到 %u\n",
			   (double) p.ratio_q16 / 65536.0, p.ratio_q16, p.hint_dropped, p.hint_duped, p.in_band, p.late);
		{
			uint32_t dead = 0u, regress = 0u, jumps = 0u;
			uint64_t dropb = 0ull;
			if (CLV_PaceStats(p.pace, &dead, &regress, &jumps, &dropb) == CLV_PACE_OK)
			{
				printf("旁路诊断：死队列报警 %u 次  回退拒收 %u 次  设备跳变边沿 %u 次  自报减账 %llu B\n", dead,
					   regress, jumps, (unsigned long long) dropb);
			}
		}
		if (p.quit) break;
	} while (0);

	if (p.pace) CLV_PaceClose(p.pace);
	if (p.ast) SDL_DestroyAudioStream(p.ast);
	if (p.subtex) SDL_DestroyTexture(p.subtex);
	if (p.tex) SDL_DestroyTexture(p.tex);
	if (p.ren) SDL_DestroyRenderer(p.ren);
	if (p.win) SDL_DestroyWindow(p.win);
	if (p.sr) CLV_SubtitleRendererClose(p.sr);
	if (p.sp) CLV_SubtitleParserClose(p.sp);
	free(p.style_block); /* 借用视图的用方已 Close，这份缓冲才轮到放 */
	if (p.ad) CLV_AudioDecoderClose(p.ad);
	if (p.vd) CLV_VideoDecoderClose(p.vd);
	if (p.rd) CLV_ContainerReaderClose(p.rd);
	free(p.subpix);
	SDL_Quit();
	printf("[核对] packets v/a/s=%u/%u/%u  blocks=%u lines=%u  video presented=%u  subtitle frames=%u "
		   "glyphs=%u  wall=%.2fs\n",
		   p.v_pkts, p.a_pkts, p.s_pkts, p.sub_blocks, p.sub_lines, p.presented, p.sub_shown, p.sub_glyphs,
		   (double) p.wall_seconds_dbg);
	printf("DEMO_SUBTITLE_DONE shown=%u dropped=%u frames=%u blocks=%u glyphs=%u\n", p.sub_shown, p.dropped, p.shown,
		   p.sub_blocks, p.sub_glyphs);
	return (p.sub_shown > 0u && p.dropped == 0u) ? 0 : 1;
}
