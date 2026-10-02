/* container.c
 *
 * 用法：
 *   clv_demo_container                 写 + 读 + 逐项判定，文件落在当前目录 ./clv_demo_container.clv
 *   clv_demo_container <路径>           同上，改写入路径
 *   clv_demo_container info <路径>      只读并 dump 一个已有 .clv
 *
 * 视频流首包故意给源 dts = -40（B 帧前瞻的负初始 DTS）。写方要按首帧把整条流平移到 0 落盘，
 * 所以 [#04] 期望落盘 dts_delta = 0、pts_delta = 80（= 40 - (-40)，与平移量无关）。
 */
#include "CLV_Container.h"
#include "CLV_File.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
	// 示例是消费者侧代码，直接用 Win32 控制台 API；库内的「裸宏只出现在 platform.h」约束管库不管示例
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <windows.h>
#endif

#define kDefaultFile	"./clv_demo_container.clv"
#define kBaseTicks		1081080000ull /* 基础时间基 */
#define kVideoMaxPacket 4096u
#define kSubMaxPacket	1024u
#define kChunk			300u

#define kVideoFrames 2
#define kSubEvents	 2
#define kTotalFrames (kVideoFrames + kSubEvents)

/* 应用侧义务：库只按 UTF-8 字节串直投日志，不做代码页转换，所以控制台码页要自己设成 CP_UTF8，
   否则本文件的中文判定行与 dump 输出会按代码页 936 重解读成乱码。每个入口都要设一次。 */
static void SetupConsoleUtf8(void)
{
#if defined(_WIN32)
	if (SetConsoleOutputCP(CP_UTF8) == 0) printf("[warn] SetConsoleOutputCP(CP_UTF8) failed\n");
	SetConsoleCP(CP_UTF8);	  // 输入侧一并设上，保持与输出一致
#endif
}

static int g_item = 0;
static int g_pass = 0;
static int g_fail = 0;
static int g_manual = 0;

/* 源素材：视频两帧 + 字幕两事件，载荷是确定性图案，比对时不需要记住随机种子 */
static uint8_t g_video0[1000];
static uint8_t g_video1[200];
static uint8_t g_sub0[30];
static uint8_t g_sub1[20];

static const char kStyle[] = "[Script Info]\nTitle: clv demo\nScriptType: v4.00+\n";

static void FillPattern(uint8_t* p, size_t n, uint8_t seed)
{
	size_t i;
	for (i = 0; i < n; ++i) p[i] = (uint8_t) ((i * 37u + seed) & 0xFFu);
}

static void Check(const char* name, const char* expect, const char* actual, int ok)
{
	++g_item;
	printf("[#%02d] %-24s | expect: %-30s | actual: %-30s | %s\n", g_item, name, expect, actual, ok ? "PASS" : "FAIL");
	if (ok) ++g_pass;
	else ++g_fail;
}

static void Manual(const char* name)
{
	++g_manual;
	printf("[人工] %s\n", name);
}

static void DescribeStream(const CLV_StreamDescC* d, char* out, size_t cap)
{
	snprintf(out, cap, "id%u type%u tb%llu/%llu mps%u bd%u", (unsigned) d->stream_id, (unsigned) d->stream_type,
			 (unsigned long long) d->timebase_num, (unsigned long long) d->timebase_den, (unsigned) d->max_packet_size,
			 (unsigned) d->bit_depth);
}

static CLV_StreamDescC MakeStream(uint8_t type, uint8_t id, uint32_t max_packet)
{
	CLV_StreamDescC d;
	memset(&d, 0, sizeof(d));
	d.stream_id = id;
	d.stream_type = type;
	d.codec_id = (uint8_t) (type == 1 ? 1 : 0);
	d.codec_version = 1;
	d.timebase_num = 1;
	d.timebase_den = kBaseTicks; /* 视频与字幕共用基础时间基 */
	d.max_packet_size = max_packet;
	d.first_dts = 0;
	d.bit_depth = (type == 0) ? 8 : 0;
	return d;
}

/* ① 写：产出真文件，返回 0 表示写入路径全程无错 */
static int WriteDemoFile(const char* path, uint64_t* packets_out, uint64_t* size_out)
{
	CLV_WriteParams params;
	CLV_ContainerError err = CLV_CONTAINER_OK;
	CLV_ContainerWriter* w;
	CLV_StreamDescC video = MakeStream(0, 0, kVideoMaxPacket);
	CLV_StreamDescC sub = MakeStream(2, 1, kSubMaxPacket);
	uint64_t packets = 0;

	memset(&params, 0, sizeof(params));
	params.version_minor = 0;
	params.index_present = 1;
	params.globally_sorted = 0;
	params.streaming = 0;
	params.fragment_chunk_size = kChunk;

	w = CLV_ContainerWriterOpenFile(path, &params, &err);
	if (w == NULL)
	{
		printf("  写入打开失败：%s\n", CLV_ContainerStrError(err));
		return 1;
	}

	if (CLV_ContainerWriterAddStream(w, &video) != CLV_CONTAINER_OK ||
		CLV_ContainerWriterAddStream(w, &sub) != CLV_CONTAINER_OK)
	{
		printf("  AddStream 失败\n");
		CLV_ContainerWriterClose(w);
		return 1;
	}
	if (CLV_ContainerWriterAddExtBlock(w, 1, 4, 1, kStyle, strlen(kStyle)) != CLV_CONTAINER_OK)
	{
		printf("  样式扩展块写入失败\n");
		CLV_ContainerWriterClose(w);
		return 1;
	}

	/* 视频首帧：源 dts = -40，pts = +40（pts != dts），载荷 1000 B → 按 300 切四片 */
	if (CLV_ContainerWriterWriteFrame(w, 0, 1, -40, 40, g_video0, sizeof(g_video0)) != CLV_CONTAINER_OK ||
		CLV_ContainerWriterWriteFrame(w, 1, 0, 0, 0, g_sub0, sizeof(g_sub0)) != CLV_CONTAINER_OK ||
		CLV_ContainerWriterWriteFrame(w, 1, 0, 21621600, 21621600, g_sub1, sizeof(g_sub1)) != CLV_CONTAINER_OK ||
		CLV_ContainerWriterWriteFrame(w, 0, 0, 43243200, 43243200, g_video1, sizeof(g_video1)) != CLV_CONTAINER_OK)
	{
		printf("  WriteFrame 失败\n");
		CLV_ContainerWriterClose(w);
		return 1;
	}

	if (CLV_ContainerWriterFinish(w, &packets) != CLV_CONTAINER_OK)
	{
		printf("  Finish 失败\n");
		CLV_ContainerWriterClose(w);
		return 1;
	}
	CLV_ContainerWriterClose(w);

	*packets_out = packets;
	{
		CLV_File* f = CLV_OpenFile(path, CLV_FILE_MODE_READ);
		uint64_t size = 0;
		if (f != NULL)
		{
			CLV_FileSize(f, &size);
			CLV_CloseFile(f);
		}
		*size_out = size;
	}
	return 0;
}

/* 文件头 magic 只能从字节上看（公开 ABI 不暴露原始标签），所以用文件面读前 8 字节 */
static int ReadMagic(const char* path, char* out, size_t cap)
{
	CLV_File* f = CLV_OpenFile(path, CLV_FILE_MODE_READ);
	uint8_t bytes[8];
	size_t got = 0;

	if (cap < 9) return 1;
	if (f == NULL) return 1;
	if (CLV_ReadFile(f, bytes, sizeof(bytes), &got) != CLV_FILE_OK || got != sizeof(bytes))
	{
		CLV_CloseFile(f);
		return 1;
	}
	CLV_CloseFile(f);

	memcpy(out, bytes, 8);
	out[8] = '\0';
	return 0;
}

/* 把一条目的 dts 换算到全局 tick，用于跨流次序判定。
 * 先乘后除：dts × num × 1081080000 ÷ den。demo 的两条流都是 1/1081080000，量级安全；
 * 真正的通用换算在库里（128 位中间量），这里不做溢出防护。 */
static uint64_t ToTicks(uint64_t dts, const CLV_StreamDescC* d)
{
	if (d == NULL || d->timebase_den == 0 || d->timebase_num == 0) return dts;
	return dts * d->timebase_num * kBaseTicks / d->timebase_den;
}

static void PrintStructure(CLV_ContainerReader* r, const char* path)
{
	CLV_FileHeadC head;
	CLV_ReaderStatsC stats;
	uint32_t streams = 0, entries = 0, blocks = 0, i;

	memset(&head, 0, sizeof(head));
	memset(&stats, 0, sizeof(stats));
	CLV_ContainerReaderGetHead(r, &head);
	CLV_ContainerReaderStreamCount(r, &streams);
	CLV_ContainerReaderIndexCount(r, &entries);
	CLV_ContainerReaderExtBlockCount(r, &blocks);
	CLV_ContainerReaderStats(r, &stats);

	printf("  文件            %s\n", path);
	printf("  结构版本        %u.%u   流数 %u   flags 0x%02X\n", (unsigned) head.version_major,
		   (unsigned) head.version_minor, (unsigned) head.stream_count, (unsigned) head.flags);
	printf("  duration_ticks  %llu (= %llu / %llu 秒)\n", (unsigned long long) head.duration_ticks,
		   (unsigned long long) head.duration_ticks, (unsigned long long) kBaseTicks);
	printf("  total_packets   %llu   index_offset %llu\n", (unsigned long long) head.total_packets,
		   (unsigned long long) head.index_offset);

	for (i = 0; i < streams; ++i)
	{
		CLV_StreamDescC d;
		char text[128];
		uint8_t usable = 0;
		memset(&d, 0, sizeof(d));
		CLV_ContainerReaderGetStream(r, i, &d, &usable);
		DescribeStream(&d, text, sizeof(text));
		printf("  流[%u]          %s  usable=%u\n", (unsigned) i, text, (unsigned) usable);
	}

	for (i = 0; i < blocks; ++i)
	{
		CLV_ExtBlockC b;
		memset(&b, 0, sizeof(b));
		CLV_ContainerReaderGetExtBlock(r, i, &b);
		printf("  扩展块[%u]      stream%u ext_type=%u ver=%u off=%llu size=%llu\n", (unsigned) i,
			   (unsigned) b.stream_id, (unsigned) b.ext_type, (unsigned) b.ext_version, (unsigned long long) b.offset,
			   (unsigned long long) b.size);
	}

	for (i = 0; i < entries; ++i)
	{
		CLV_IndexEntryC e;
		memset(&e, 0, sizeof(e));
		CLV_ContainerReaderGetIndexEntry(r, i, &e);
		printf("  索引[%u]        stream%u key=%u file_offset=%-8llu dts=%llu\n", (unsigned) i, (unsigned) e.stream_id,
			   (unsigned) e.is_keyframe, (unsigned long long) e.file_offset, (unsigned long long) e.dts);
	}

	printf("  读方统计        包 %llu 丢CRC %llu 丢字段 %llu 丢超限 %llu 丢无名流 %llu 重同步 %llu 残帧 %llu\n",
		   (unsigned long long) stats.packets_ok, (unsigned long long) stats.dropped_crc,
		   (unsigned long long) stats.dropped_field, (unsigned long long) stats.dropped_too_large,
		   (unsigned long long) stats.dropped_unknown_stream, (unsigned long long) stats.resync_count,
		   (unsigned long long) stats.frames_incomplete);
}

static int InfoMode(int argc, char** argv)
{
	CLV_ContainerError err = CLV_CONTAINER_OK;
	const char* path = (argc > 2) ? argv[2] : kDefaultFile;
	CLV_ContainerReader* r = CLV_ContainerReaderOpenFile(path, &err);
	CLV_FrameC frame;
	uint64_t frames = 0;
	uint8_t got = 0;

	if (r == NULL)
	{
		printf("info: 打不开或不是一份可解析的 CLV（%s）：%s\n", path, CLV_ContainerStrError(err));
		return 1;
	}

	printf("== CLV 结构 dump ==\n");
	PrintStructure(r, path);
	printf("  帧列表\n");
	for (;;)
	{
		memset(&frame, 0, sizeof(frame));
		if (CLV_ContainerReaderNextFrame(r, &frame, &got) != CLV_CONTAINER_OK || ! got) break;
		printf("  帧[%llu]        stream%u key=%u complete=%u dts_delta=%llu pts_delta=%lld 片 %llu/%llu 载荷 %llu B\n",
			   (unsigned long long) frames, (unsigned) frame.stream_id, (unsigned) frame.is_keyframe,
			   (unsigned) frame.complete, (unsigned long long) frame.dts_delta, (long long) frame.pts_delta,
			   (unsigned long long) frame.fragments_received, (unsigned long long) frame.fragments_expected,
			   (unsigned long long) frame.payload_size);
		++frames;
	}
	CLV_ContainerReaderClose(r);
	printf("== 共 %llu 帧 ==\n", (unsigned long long) frames);
	return 0;
}

static int RunChecks(const char* path, uint64_t written_packets, uint64_t file_size)
{
	CLV_ContainerError err = CLV_CONTAINER_OK;
	CLV_ContainerReader* r;
	CLV_FileHeadC head;
	CLV_ReaderStatsC stats;
	CLV_StreamDescC video, sub;
	CLV_ExtBlockC block;
	CLV_IndexEntryC entries[kTotalFrames];
	char expect[128], actual[160], magic[32];
	uint32_t streams = 0, entry_count = 0, block_count = 0, i;
	uint64_t last_ticks = 0, last_stream = 0, show_ticks[kTotalFrames];
	int sorted = 1, frame_count = 0, video0_ok = 0;
	uint8_t usable = 0, got = 0;
	CLV_FrameC frame;

	memset(&head, 0, sizeof(head));
	memset(&stats, 0, sizeof(stats));
	memset(&video, 0, sizeof(video));
	memset(&sub, 0, sizeof(sub));
	memset(&block, 0, sizeof(block));
	memset(entries, 0, sizeof(entries));
	memset(show_ticks, 0, sizeof(show_ticks));

	r = CLV_ContainerReaderOpenFile(path, &err);
	if (r == NULL)
	{
		printf("  读回打开失败：%s\n", CLV_ContainerStrError(err));
		return 1;
	}

	/* #01 magic + 结构版本 */
	CLV_ContainerReaderGetHead(r, &head);
	if (ReadMagic(path, magic, sizeof(magic)) != 0) magic[0] = '?';
	snprintf(expect, sizeof(expect), "CLVFILE / 1.0");
	snprintf(actual, sizeof(actual), "%.7s / %u.%u", magic, (unsigned) head.version_major,
			 (unsigned) head.version_minor);
	Check("文件头 magic/版本", expect, actual,
		  strncmp(magic, "CLVFILE", 7) == 0 && head.version_major == 1 && head.version_minor == 0);

	/* #02 两条流描述符读回 */
	CLV_ContainerReaderStreamCount(r, &streams);
	CLV_ContainerReaderGetStream(r, 0, &video, &usable);
	CLV_ContainerReaderGetStream(r, 1, &sub, &usable);
	snprintf(expect, sizeof(expect), "2 流 / id0 tb1/%llu / id1 type2", (unsigned long long) kBaseTicks);
	snprintf(actual, sizeof(actual), "%u 流 / id%u tb%llu/%llu / id%u type%u", (unsigned) streams,
			 (unsigned) video.stream_id, (unsigned long long) video.timebase_num,
			 (unsigned long long) video.timebase_den, (unsigned) sub.stream_id, (unsigned) sub.stream_type);
	Check("流描述符读回", expect, actual,
		  streams == 2 && video.stream_id == 0 && video.timebase_den == kBaseTicks && sub.stream_id == 1 &&
			  sub.stream_type == 2 && sub.max_packet_size == kSubMaxPacket);

	/* #03 字幕 ext_type=4 样式块逐字节 */
	CLV_ContainerReaderExtBlockCount(r, &block_count);
	CLV_ContainerReaderGetExtBlock(r, 0, &block);
	snprintf(expect, sizeof(expect), "ext4 样式 %llu B 逐字节等", (unsigned long long) strlen(kStyle));
	snprintf(actual, sizeof(actual), "ext%u 流%u %llu B %s", (unsigned) block.ext_type, (unsigned) block.stream_id,
			 (unsigned long long) block.size,
			 (block.size == strlen(kStyle) && memcmp(block.data, kStyle, block.size) == 0) ? "相同" : "不同");
	Check("字幕 ext_type=4 样式块", expect, actual,
		  block_count == 1 && block.ext_type == 4 && block.stream_id == 1 && block.size == strlen(kStyle) &&
			  memcmp(block.data, kStyle, block.size) == 0);

	/* #04 首包 dts 归零（写方平移义务） */
	memset(&frame, 0, sizeof(frame));
	CLV_ContainerReaderNextFrame(r, &frame, &got);
	snprintf(expect, sizeof(expect), "首帧 dts_delta = 0");
	snprintf(actual, sizeof(actual), "dts_delta = %llu (源 dts = -40)", (unsigned long long) frame.dts_delta);
	Check("首包 dts 归零", expect, actual, got && frame.dts_delta == 0);
	printf("      └─ 附加行：源首包 dts = -40 → 写方平移后落盘首包 dts = 0，pts_delta = %lld（= 40 - (-40)）\n",
		   (long long) frame.pts_delta);
	video0_ok = (frame.payload_size == sizeof(g_video0)) && memcmp(frame.payload, g_video0, sizeof(g_video0)) == 0 &&
				frame.fragments_received == 4 && frame.fragments_expected == 4 && frame.complete == 1;
	++frame_count;

	/* #05 分片重组完整帧字节一致 */
	snprintf(expect, sizeof(expect), "1000 B 四片重组一致");
	snprintf(actual, sizeof(actual), "%llu B 片%llu/%llu complete=%u", (unsigned long long) frame.payload_size,
			 (unsigned long long) frame.fragments_received, (unsigned long long) frame.fragments_expected,
			 (unsigned) frame.complete);
	Check("分片重组完整帧", expect, actual, video0_ok);

	/* 剩下的帧先读完，供后面几项用 */
	for (;;)
	{
		memset(&frame, 0, sizeof(frame));
		if (CLV_ContainerReaderNextFrame(r, &frame, &got) != CLV_CONTAINER_OK || ! got) break;
		++frame_count;
	}

	/* #06 索引条目数 == 帧数 */
	CLV_ContainerReaderIndexCount(r, &entry_count);
	CLV_ContainerReaderHasIndex(r, &got);
	snprintf(expect, sizeof(expect), "%d 条（每帧一条）", kTotalFrames);
	snprintf(actual, sizeof(actual), "%u 条，has_index=%u", (unsigned) entry_count, (unsigned) got);
	Check("索引条目数与帧数", expect, actual, got && entry_count == kTotalFrames && frame_count == kTotalFrames);

	/* #07 索引跨流统一时间轴升序 + 相等按 stream_id */
	for (i = 0; i < entry_count && i < kTotalFrames; ++i) CLV_ContainerReaderGetIndexEntry(r, i, &entries[i]);
	for (i = 0; i < entry_count && i < kTotalFrames; ++i)
	{
		const CLV_StreamDescC* d = (entries[i].stream_id == 0) ? &video : &sub;
		uint64_t ticks = ToTicks(entries[i].dts, d);
		show_ticks[i] = ticks;
		if (i == 0)
		{
			last_ticks = ticks;
			last_stream = entries[i].stream_id;
			continue;
		}
		if (ticks < last_ticks || (ticks == last_ticks && entries[i].stream_id < last_stream)) sorted = 0;
		last_ticks = ticks;
		last_stream = entries[i].stream_id;
	}
	snprintf(expect, sizeof(expect), "统一时间轴升序，相等按 stream_id");
	snprintf(actual, sizeof(actual), "%s（ticks %llu/%llu/%llu/%llu，流 %u/%u/%u/%u）", sorted ? "升序" : "有逆序",
			 (unsigned long long) show_ticks[0], (unsigned long long) show_ticks[1], (unsigned long long) show_ticks[2],
			 (unsigned long long) show_ticks[3], (unsigned) entries[0].stream_id, (unsigned) entries[1].stream_id,
			 (unsigned) entries[2].stream_id, (unsigned) entries[3].stream_id);
	Check("索引跨流统一时间轴", expect, actual, sorted && entries[0].stream_id == 0 && entries[1].stream_id == 1);

	/* #08 包数：文件头回填值 == 写入值 == 读方统计 */
	CLV_ContainerReaderStats(r, &stats);
	snprintf(expect, sizeof(expect), "%llu 包", (unsigned long long) written_packets);
	snprintf(actual, sizeof(actual), "头 %llu，读回成功 %llu", (unsigned long long) head.total_packets,
			 (unsigned long long) stats.packets_ok);
	Check("包数一致", expect, actual, head.total_packets == written_packets && stats.packets_ok == written_packets);

	/* #09 容错计数全零：干净文件不该出现丢包 / 重同步 / 残帧 */
	snprintf(expect, sizeof(expect), "全 0，未截断未放弃");
	snprintf(actual, sizeof(actual), "CRC%llu 字段%llu 超限%llu 无名流%llu 重同步%llu 残帧%llu 截断%u 放弃%u",
			 (unsigned long long) stats.dropped_crc, (unsigned long long) stats.dropped_field,
			 (unsigned long long) stats.dropped_too_large, (unsigned long long) stats.dropped_unknown_stream,
			 (unsigned long long) stats.resync_count, (unsigned long long) stats.frames_incomplete,
			 (unsigned) stats.truncated, (unsigned) stats.abandoned);
	Check("容错计数干净", expect, actual,
		  stats.dropped_crc == 0 && stats.dropped_field == 0 && stats.dropped_too_large == 0 &&
			  stats.dropped_unknown_stream == 0 && stats.resync_count == 0 && stats.frames_incomplete == 0 &&
			  stats.truncated == 0 && stats.abandoned == 0);

	PrintStructure(r, path);
	CLV_ContainerReaderClose(r);

	snprintf(expect, sizeof(expect), "> 1024 B");
	snprintf(actual, sizeof(actual), "%llu B", (unsigned long long) file_size);
	Check("文件大小合理", expect, actual, file_size > 1024);

	Manual("打开上面的 .clv（或跑 info 子命令）看结构是否顺眼");
	return 0;
}

int main(int argc, char** argv)
{
	const char* path = kDefaultFile;

	SetupConsoleUtf8();
	uint64_t packets = 0, size = 0;

	FillPattern(g_video0, sizeof(g_video0), 1);
	FillPattern(g_video1, sizeof(g_video1), 2);
	FillPattern(g_sub0, sizeof(g_sub0), 3);
	FillPattern(g_sub1, sizeof(g_sub1), 4);

	if (argc > 1 && strcmp(argv[1], "info") == 0) return InfoMode(argc, argv);
	if (argc > 1) path = argv[1];

	printf("== 容器层 demo：写 %s ==\n", path);
	if (WriteDemoFile(path, &packets, &size) != 0)
	{
		printf("== 写入阶段失败，退出 ==\n");
		return 1;
	}
	printf("  写入完成：%llu 包，文件 %llu B，视频首帧按 %u B 切成 4 片\n\n", (unsigned long long) packets,
		   (unsigned long long) size, (unsigned) kChunk);

	printf("== 读回并逐项判定 ==\n");
	if (RunChecks(path, packets, size) != 0) return 1;

	printf("\n=== 自动判定失败 %d 项，人工判定 %d 项 ===\n", g_fail, g_manual);
	printf("（共 %d 项自动判定，通过 %d 项）\n", g_item, g_pass);
	return g_fail == 0 ? 0 : 1;
}
