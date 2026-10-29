#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""随机交叉语料档生成器：Python 独立模型 ↔ C 实现 的双刃对照，三档产物 + 三道前置。

产物只落 `--out` 给的那个不入库目录，本件不写仓库里任何入库面：`--out` 解析后落在仓内时，
只有落在仓内那棵被 `.gitignore` 整目录排除的工作树下才放行，指进仓库根或任何入库面直接拒。
临时驱动件（C 源码 + 编译批 + 语料 + 结果日志）全部由本件现场生成到 `--out`，
只**链接已建好的静态库**，不碰构建系统（不跑 cmake / ninja / ctest，不改构建目录）。

三档都要现编临时驱动件，故都要给 `--lib` 与 `--thirdparty-lib`：

    # 随机交叉：编码参数组合 + 畸形域组合，绿向（模型重建 == C 解码输出）与红向（坏期望 != C 解码输出）
    .venv/Scripts/python.exe scripts/gen_fuzz_corpus.py --cross --out <临时目录> --lib <静态库> --thirdparty-lib <第三方库目录> --rounds 12

    # 畸形流鲁棒档：位翻转 / 截断 / 拼接 / 非法字段值 / varint 非最短形式喂解码器；不崩、不挂、错误码归到现行枚举三条判据都进退出码
    .venv/Scripts/python.exe scripts/gen_fuzz_corpus.py --fuzz --out <临时目录> --lib <静态库> --thirdparty-lib <第三方库目录> --cases 120

    # 坏期望三式：错一位系数 / 常数化 / 等于源，供断言敏感性抽检；基准取 C 侧解码输出（容器 kind 3）
    .venv/Scripts/python.exe scripts/gen_fuzz_corpus.py --mutate --out <临时目录> --lib <静态库> --thirdparty-lib <第三方库目录> --rounds 12

三道前置：--pre2 只出对齐表，不跑驱动件；--pre1 与 --pre3 要跑驱动件，故同样要给库路径。

    .venv/Scripts/python.exe scripts/gen_fuzz_corpus.py --pre1 --out <临时目录> --lib <静态库> --thirdparty-lib <第三方库目录> --allow-big   # 判据锚点对账
    .venv/Scripts/python.exe scripts/gen_fuzz_corpus.py --pre2 --out <临时目录>   # 参数覆盖对齐表
    .venv/Scripts/python.exe scripts/gen_fuzz_corpus.py --pre3 --out <临时目录> --lib <静态库> --thirdparty-lib <第三方库目录>   # 固定 seed 绿锚

满档那种 12 s 级大例属长跑档与前置 1：不进 `--cross` 门禁，但前置 1 的满档载荷锚点要它实跑，
所以前置 1 的规范敲法带 `--allow-big`；不带时那条锚算不出值，前置 1 判红——没跑全就不算过。

参数一览（除 `--out` 与档位开关外都可省，括号里是默认值）：`--rounds`（12，交叉档与敏感档的轮数，
直接按本参数取）、`--cases`（120，鲁棒档目标条数：底件数取 ⌈cases/3⌉，每底件三条手法，实产
3×⌈cases/3⌉ 条，cases 是 3 的倍数时正好相等；底件崩或容器读不出的会削减，真实条数看汇总行的 M=）、
`--seed-base`（20261007，各档种子派生的基数）、`--anchor-seeds`（前置 3 的固定 seed 表，不给用内置五个）、
`--budget-sec`（300，预算核对行里当门禁秒数用）、`--timeout-sec`（60，单子进程超时）、
`--big-timeout`（240，满档单例超时）、`--allow-big`（放进 4112×4080 那种大例）、`--repo`（脚本所在仓库根，
驱动件从这里取公开头）、`--lib`（已建好的静态库文件路径，本件不建库）、`--thirdparty-lib`（第三方库目录）、
`--extra-libs`（逗号分隔的附加库名，不带 `.lib` 的自动补后缀，按 `--thirdparty-lib` 目录解析）、
`--msvc-root` / `--msvc-version` / `--sdk-root` / `--sdk-ver`（工具链位置）、`--no-build`（复用已编好的驱动件）。

工具链位置也可以从环境变量给（脚本里不落任何本机路径）：`CLV_MSVC_ROOT`、`CLV_SDK_ROOT`、
`CLV_SDK_VERSION`。跑批前先 `set PYTHONIOENCODING=utf-8`。
子进程一律包超时（`--timeout-sec`，默认 60 s/例）：编码 / 解码任何一例超时、崩溃或起不来（含 `--no-build`
下可执行文件缺失）都记成该例的红，不把整批拖挂；鲁棒档因此对每条都记结果，前置与交叉档对每轮都记结果。

判定口径：两边都照规范写，Python 复用仓库里已有的规范模型；两侧是不同语言的两份实现，
Python 侧算式与 C 侧读取路径各写各的。取点由本件的 seed 决定，seed 落进用例名、随任务文件
一起交给库；独立性的论据是「不同语言的两份实现 + 各自独立写的算式」，不靠随机序列保密。
C 红 Py 红 = 一致（边界行为实现）；C 绿 Py 绿 = 一致（正常路径）；C 红 Py 绿 = Py 漏实现 / 绕过 /
错误处理缺失，或 C 过严；C 绿 Py 红 = Py 过严 / 误报，或 C 漏实现；两边同色而规范本身有歧义的，
按规范歧义记，不算实现红。绿向判据是逐字节全等，不放宽。seed 进用例名与每一行日志；
某轮「坏期望 == 实际输出」不停手，按 seed 固化成定例并记账。

预算核对：模型是 Python，单轮比 C 慢，日志按实测每例单价核对门禁秒数能容几轮；
轮数由 `--rounds` 直接给定，倒推值只作核对参考，不自动改轮数。
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import random
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cross_model as M  # noqa: E402

DRIVER_SRC_NAME = "cross_drv.c"
DRIVER_BAT_NAME = "cross_drv_build.bat"
DRIVER_EXE_NAME = "cross_drv.exe"

# 临时驱动件的源码内嵌在这里，运行前写到产物目录再编（只链接已建好的库）。
DRIVER_C_SOURCE = r"""
/* cross_drv.c — 随机交叉语料档的临时驱动件（只链接已建好的静态库，不碰构建系统）。
 *
 * 用法：cross_drv <mode> <dir> <case>
 *   mode = enc  : 读 <dir>/<case>.job 与 <dir>/<case>.src，逐帧编码 + 喂解码器，写 <dir>/<case>.out 容器
 *   mode = rob  : 读 <dir>/<case>.job 与 <dir>/<case>.pay，把（可能已畸形的）载荷喂解码器，回报错误码与出帧数
 *
 * 容器（小端）：magic "CFXC" + u32 ver + u32 n_entries，随后 n_entries 个
 *   u32 kind + u32 index + u32 len + len 字节。
 *   kind: 1=整帧载荷 3=解码输出帧
 *   这条旁路容器不是 .clv 文件；magic 取 "CFXC" 是刻意避开真格式扩展块的那个四字符标识，
 *   免得产物目录里的 .out 被按容器解析器读。ver 是本容器自己的版本轴，与视频语法版本无关。
 *
 * 本驱动件只跑视频侧：mode 只有 enc / rob 两档，音频对照不在本件范围内；
 * 音频侧的算式与非法值判据都由模型层的帧头解析自核。
 *
 * stdout 只打 ASCII 前缀 + 数值，对照件按行读；所有子进程调用都包超时。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CLV_Video.h"

#define MAX_FRAMES 64 /* 一例最多喂 64 帧：GOP 计划按 max_b×组数派生，取到这个数已是长例 */
#define VIDEO_PLANE_BYTES(w, h, pl) ((pl) == 0 ? (size_t) (w) * (h) : (size_t) ((w) / 2u) * ((h) / 2u))

typedef struct
{
	char name[64];
	long w, h, dw, dh, qp, maxb, slice_mb, q;
	int nframes;
	int types[MAX_FRAMES];
	long long dts[MAX_FRAMES];
	long long pts[MAX_FRAMES];
	long frag;
} Job;

static FILE *g_out = NULL;
static uint32_t g_entries = 0u;

static void w32(uint32_t v)
{
	fputc((int) (v & 0xFFu), g_out);
	fputc((int) ((v >> 8) & 0xFFu), g_out);
	fputc((int) ((v >> 16) & 0xFFu), g_out);
	fputc((int) ((v >> 24) & 0xFFu), g_out);
}

static void put_blob(uint32_t kind, uint32_t index, const uint8_t *p, size_t n)
{
	w32(kind);
	w32(index);
	w32((uint32_t) n);
	if (n != 0u && p != NULL) fwrite(p, 1, n, g_out);
	++g_entries;
}

/* 收尾：把条目数回填到头 8..11 字节。 */
static void finish_out(void)
{
	if (g_out == NULL) return;
	fflush(g_out);
	if (fseek(g_out, 8L, SEEK_SET) == 0) w32(g_entries);
	g_out = NULL; /* 只回填一次；置空后重复调用直接返回 */
}

static void copy_str(char *dst, const char *src, size_t n)
{
	size_t i = 0u;
	for (; i + 1 < n && src[i] != '\0'; ++i) dst[i] = src[i];
	dst[i] = '\0';
}

static void trim(char *s)
{
	size_t l = strlen(s);
	while (l > 0u && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ')) s[--l] = '\0';
}

static int load_job(Job *j, const char *path)
{
	FILE *f;
	char line[512];
	if (path == NULL || j == NULL) return -1;
	f = fopen(path, "r");
	if (f == NULL) return -1;
	memset(j, 0, sizeof(*j));
	copy_str(j->name, "case", sizeof(j->name));
	j->qp = 7;
	while (fgets(line, (int) sizeof(line), f) != NULL)
	{
		char *sp = strchr(line, ' ');
		char key[64];
		char *rest;
		if (sp == NULL) continue;
		*sp = '\0';
		copy_str(key, line, sizeof(key));
		rest = sp + 1;
		trim(rest);
		if (strcmp(key, "frames") == 0)
		{
			char *tok = strtok(rest, " ");
			j->nframes = 0;
			while (tok != NULL && j->nframes < MAX_FRAMES)
			{
				j->types[j->nframes] = atoi(tok);
				++j->nframes;
				tok = strtok(NULL, " ");
			}
		}
		else if (strcmp(key, "dts") == 0 || strcmp(key, "pts") == 0)
		{
			long long *arr = (key[0] == 'd') ? j->dts : j->pts;
			char *tok = strtok(rest, " ");
			int i = 0;
			while (tok != NULL && i < MAX_FRAMES) arr[i++] = atoll(tok), tok = strtok(NULL, " ");
		}
		else if (strcmp(key, "name") == 0) copy_str(j->name, rest, sizeof(j->name));
		else if (strcmp(key, "w") == 0) j->w = atol(rest);
		else if (strcmp(key, "h") == 0) j->h = atol(rest);
		else if (strcmp(key, "dw") == 0) j->dw = atol(rest);
		else if (strcmp(key, "dh") == 0) j->dh = atol(rest);
		else if (strcmp(key, "qp") == 0) j->qp = atol(rest);
		else if (strcmp(key, "maxb") == 0) j->maxb = atol(rest);
		else if (strcmp(key, "slice_mb") == 0) j->slice_mb = atol(rest);
		else if (strcmp(key, "q") == 0) j->q = atol(rest);
		else if (strcmp(key, "frag") == 0) j->frag = atol(rest);
		else
		{
			/* 认不出的键要喊出来：静默忽略会让任务文件里的错键变成隐形缺参 */
			printf("KEYWARN key=%s\n", key);
		}
	}
	fclose(f);
	if (j->frag <= 0) j->frag = 1; /* 没给 frag 键的场合按单片；rob 档总是显式给片数 */
	if (j->nframes == 0) j->nframes = 1;
	return 0;
}

static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf = NULL;
	long sz = 0;
	if (f == NULL) return NULL;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (uint8_t *) malloc((size_t) sz + 1u);
	if (buf == NULL)
	{
		fclose(f);
		return NULL;
	}
	if (sz > 0 && fread(buf, 1, (size_t) sz, f) != (size_t) sz)
	{
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (size_t) sz;
	return buf;
}

/* 行连续存放的三平面 ⇄ 库给的 32 字节对齐行距。 */
static void fill_frame(CLV_VideoFrame *fr, const uint8_t *src, uint32_t w, uint32_t h)
{
	CLV_VideoFrameDescC d;
	size_t off = 0u;
	memset(&d, 0, sizeof(d));
	if (CLV_VideoFrameGetDesc(fr, &d, NULL) != CLV_VIDEO_OK) return;
	for (int pl = 0; pl < 3; ++pl)
	{
		const uint32_t pw = (pl == 0) ? w : w / 2u;
		const uint32_t ph = (pl == 0) ? h : h / 2u;
		for (uint32_t r = 0u; r < ph; ++r)
		{
			memcpy(d.plane[pl] + (size_t) r * d.stride[pl], src + off, pw);
			off += pw;
		}
	}
}

static size_t dump_frame(CLV_VideoFrame *fr, uint32_t w, uint32_t h, uint32_t index, long long *pts_out)
{
	CLV_VideoFrameDescC d;
	CLV_VideoFrameInfoC inf;
	size_t total = 0u;
	uint8_t *tmp;
	memset(&d, 0, sizeof(d));
	memset(&inf, 0, sizeof(inf));
	if (CLV_VideoFrameGetDesc(fr, &d, &inf) != CLV_VIDEO_OK) return 0u;
	for (int pl = 0; pl < 3; ++pl) total += VIDEO_PLANE_BYTES(w, h, pl);
	tmp = (uint8_t *) malloc(total);
	if (tmp == NULL) return 0u;
	{
		size_t off = 0u;
		for (int pl = 0; pl < 3; ++pl)
		{
			const uint32_t pw = (pl == 0) ? w : w / 2u;
			const uint32_t ph = (pl == 0) ? h : h / 2u;
			for (uint32_t r = 0u; r < ph; ++r)
			{
				memcpy(tmp + off, d.plane[pl] + (size_t) r * d.stride[pl], pw);
				off += pw;
			}
		}
		put_blob(3u, index, tmp, total);
	}
	free(tmp);
	if (pts_out != NULL) *pts_out = inf.pts_ticks;
	return total;
}

/* ---- mode = enc ---- */

static int mode_enc(const char *dir, Job *j)
{
	const uint32_t w = (uint32_t) j->w, h = (uint32_t) j->h;
	char path[1024];
	size_t srclen = 0u, per = 0u, off = 0u;
	uint8_t *src = NULL;
	CLV_VideoEncParamsC ep;
	CLV_VideoStreamParamsC dp;
	CLV_VideoEncoder *e = NULL;
	CLV_VideoDecoder *d = NULL;
	CLV_VideoError err = CLV_VIDEO_OK;
	int rc = 0, k;
	int emitted = 0;

	snprintf(path, sizeof(path), "%s/%s.src", dir, j->name);
	src = read_file(path, &srclen);
	if (src == NULL)
	{
		printf("ERR nosrc\n");
		return 2;
	}
	snprintf(path, sizeof(path), "%s/%s.out", dir, j->name);
	g_out = fopen(path, "wb");
	if (g_out == NULL)
	{
		printf("ERR noout\n");
		return 2;
	}
	fwrite("CFXC", 1, 4, g_out);
	w32(1u); /* 旁路容器自身的版本轴，与 .clv 容器版本、视频语法版本都不同轴 */
	w32(0u);

	memset(&ep, 0, sizeof(ep));
	ep.display_width = (uint16_t) j->dw;
	ep.display_height = (uint16_t) j->dh;
	ep.bit_depth = 8u;
	ep.quant_precision = (uint8_t) j->qp;
	ep.default_quant = (uint8_t) j->q;
	ep.max_b_frames = (uint8_t) j->maxb;
	ep.slice_mb_count = (uint16_t) j->slice_mb;
	/* 时基分母取 1001 × 1 080 000 = 1 081 080 000：24 / 25 / 30 / 50 / 60 与 24 000 / 30 000
	 * （NTSC 系那两个分子）都能整除它。本件的 dts / pts 只走 0..帧数−1 的小整数，
	 * 两侧同值即可，取这个数是为了让换帧率时不必重算期望的 PTS 差。 */
	ep.timebase_den = 1081080000ull;
	e = CLV_VideoEncoderOpen(&ep, &err);
	if (e == NULL)
	{
		printf("ENC_OPEN err=%d\n", (int) err);
		return 3;
	}
	memset(&dp, 0, sizeof(dp));
	dp.bit_depth = 8u;
	dp.quant_precision = (uint8_t) j->qp;
	dp.max_b_frames = (uint8_t) j->maxb;
	dp.obmc_disable = 1u;
	dp.display_width = (uint16_t) j->dw;
	dp.display_height = (uint16_t) j->dh;
	d = CLV_VideoDecoderOpen(&dp, &err);
	if (d == NULL)
	{
		printf("DEC_OPEN err=%d\n", (int) err);
		return 3;
	}

	per = VIDEO_PLANE_BYTES(w, h, 0) + VIDEO_PLANE_BYTES(w, h, 1) + VIDEO_PLANE_BYTES(w, h, 2);
	for (k = 0; k < j->nframes; ++k)
	{
		CLV_VideoFrame *fr = NULL;
		CLV_VideoBitstream *bs = NULL;
		const uint8_t *data = NULL;
		size_t size = 0u;
		uint32_t ns = 1u;
		if (CLV_VideoEncoderAllocFrame(e, &fr) != CLV_VIDEO_OK || fr == NULL)
		{
			rc = 4;
			break;
		}
		if (srclen < off + per)
		{
			rc = 5;
			break;
		}
		fill_frame(fr, src + off, w, h);
		off += per;
		err = CLV_VideoEncoderEncode(e, fr, (CLV_VideoFrameType) j->types[k], (uint8_t) j->q, j->dts[k], j->pts[k], &bs);
		if (err != CLV_VIDEO_OK)
		{
			printf("ENC_FAIL k=%d type=%d err=%d\n", k, j->types[k], (int) err);
			rc = 6;
			break;
		}
		CLV_VideoBitstreamGetBytes(bs, &data, &size);
		CLV_VideoBitstreamSliceCount(bs, &ns);
		put_blob(1u, (uint32_t) k, data, size);
		printf("FRAME k=%d type=%d pay=%llu slices=%u\n", k, j->types[k], (unsigned long long) size, ns);
		err = CLV_VideoDecoderDecode(d, data, size, (uint64_t) ns, j->dts[k], j->pts[k], NULL);
		if (err != CLV_VIDEO_OK) printf("DEC_FAIL k=%d err=%d\n", k, (int) err);
		CLV_VideoBitstreamRelease(bs);
		CLV_VideoFrameRelease(fr);
	}
	for (;;)
	{
		CLV_VideoFrame *got = NULL;
		uint8_t flag = 0u;
		long long pts = 0;
		if (CLV_VideoDecoderNextOutput(d, &got, &flag) != CLV_VIDEO_OK) break;
		if (flag == 0u || got == NULL) break;
		dump_frame(got, w, h, (uint32_t) emitted, &pts);
		printf("OUT k=%d pts=%lld\n", emitted, pts);
		++emitted;
		CLV_VideoFrameRelease(got);
	}
	for (;;)
	{
		CLV_VideoFrame *got = NULL;
		uint8_t flag = 0u;
		long long pts = 0;
		if (CLV_VideoDecoderFlush(d, &got, &flag) != CLV_VIDEO_OK) break;
		if (flag == 0u || got == NULL) break;
		dump_frame(got, w, h, (uint32_t) emitted, &pts);
		printf("OUT k=%d pts=%lld\n", emitted, pts);
		++emitted;
		CLV_VideoFrameRelease(got);
	}
	printf("EMITTED %d frames=%d rc=%d\n", emitted, j->nframes, rc);
	finish_out();
	CLV_VideoDecoderClose(d);
	CLV_VideoEncoderClose(e);
	free(src);
	return rc;
}

/* ---- mode = rob：坏流喂解码，三条判据都由对照件收进退出码（不崩 / 不挂 / 错误码落在现行枚举） ---- */

static int mode_rob(const char *dir, Job *j)
{
	char path[1024];
	size_t len = 0u;
	uint8_t *pay = NULL;
	CLV_VideoStreamParamsC dp;
	CLV_VideoDecoder *d = NULL;
	CLV_VideoError err = CLV_VIDEO_OK;
	CLV_VideoFrameInfoC inf;
	uint32_t out_count = 0u;

	snprintf(path, sizeof(path), "%s/%s.pay", dir, j->name);
	pay = read_file(path, &len);
	if (pay == NULL)
	{
		printf("ROB nopay\n");
		return 2;
	}
	/* 畸形手法（截断、翻位、拼接）全在对照件侧做完，驱动件只负责把 .pay 原样喂进去：
	 * 这里不再二次改字节，否则一条例的真实手法就说不清了。 */

	memset(&dp, 0, sizeof(dp));
	dp.bit_depth = 8u;
	dp.quant_precision = (uint8_t) j->qp;
	dp.max_b_frames = (uint8_t) j->maxb;
	dp.obmc_disable = 1u;
	dp.display_width = (uint16_t) j->dw;
	dp.display_height = (uint16_t) j->dh;
	d = CLV_VideoDecoderOpen(&dp, &err);
	if (d == NULL)
	{
		printf("ROB openfail err=%d\n", (int) err);
		free(pay);
		return 3;
	}
	memset(&inf, 0, sizeof(inf));
	err = CLV_VideoDecoderDecode(d, pay, len, (uint64_t) j->frag, j->dts[0], j->pts[0], &inf);
	printf("ROB err=%d disp=%d mb=%u slices=%u\n", (int) err, (int) inf.disposition, inf.macroblock_count,
		   inf.slice_count);
	for (;;)
	{
		CLV_VideoFrame *got = NULL;
		uint8_t flag = 0u;
		if (CLV_VideoDecoderNextOutput(d, &got, &flag) != CLV_VIDEO_OK) break;
		if (flag == 0u || got == NULL) break;
		++out_count;
		CLV_VideoFrameRelease(got);
	}
	{
		CLV_VideoFrame *got = NULL;
		uint8_t flag = 0u;
		if (CLV_VideoDecoderFlush(d, &got, &flag) == CLV_VIDEO_OK && flag == 1u && got != NULL)
		{
			++out_count;
			CLV_VideoFrameRelease(got);
		}
	}
	{
		uint8_t rej = 0u;
		printf("ROB out=%u rejected=%u\n", out_count, (CLV_VideoDecoderStreamRejected(d, &rej) == CLV_VIDEO_OK) ? rej : 0u);
	}
	CLV_VideoDecoderClose(d);
	free(pay);
	return 0;
}

int main(int argc, char **argv)
{
	Job j;
	char path[1024];
	int rc;
	setvbuf(stdout, NULL, _IONBF, 0); /* 崩在半路也要看得见已打到哪一行 */
	if (argc < 4)
	{
		printf("USAGE cross_drv <mode> <dir> <case>\n");
		return 1;
	}
	snprintf(path, sizeof(path), "%s/%s.job", argv[2], argv[3]);
	if (load_job(&j, path) != 0)
	{
		printf("ERR nojob\n");
		return 1;
	}
	if (strcmp(argv[1], "enc") == 0) rc = mode_enc(argv[2], &j);
	else if (strcmp(argv[1], "rob") == 0) rc = mode_rob(argv[2], &j);
	else
	{
		printf("ERR badmode\n");
		rc = 1;
	}
	fflush(stdout);
	return rc;
}
"""

# 预算核对行的两个常数：--budget-sec 的默认门禁秒数，与编译 + 启动 + 写日志的固定开销余量。
# 只用来在日志里核对「实测单价能容几轮」，轮数本身由 --rounds 给定，这里不回写。
GATE_DEFAULT_SEC = 300.0
SAFETY_MARGIN_SEC = 40.0  # 编译 + 启动 + 写日志的固定开销余量

# 鲁棒档的种子派生常数：
# MUT_SEED_MIX 取 7919（第 1000 个素数）做底件序号的乘数，素数步长让不同底件的种子区间互不重叠；
# MUT_SEED_BITS 取 30，把种子收在十进制短数域内，用例名与日志里的种子始终是可抄回的 30 位内整数。
MUT_SEED_MIX = 7919
MUT_SEED_BITS = 30

# 前置 3 的固定 seed 表：这五个点只求每次取到同一例（绿锚要能跨轮比），不参与随机序列；
# 要换点用 --anchor-seeds 传，别改这张表——改了以后各轮的绿锚就不可比。
PRE3_ANCHOR_SEEDS = [20261007, 1000003, 7777771, 4242, 987654321]

# 图样维：都是「喂进去的素材」，不是期望值；0 / 1 两档特意取判据锚点用的那两种写法。
PATTERNS = {
    0: "hf",  # r*37 + c*91 + ((r^c)*13) + pl*61（判据锚点件里的高频面）
    1: "ramp",  # pl*61 + r*7 + c*3（满档载荷件里的那种面）
    2: "flat",  # 每平面一个常数（全零块路径）
    3: "prng",  # 逐像素伪随机（seed 决定，两档交叉都靠它取不同的点）
    4: "blocks",  # 4×4 方块棋盘（高频 + 位图稀疏）
    5: "tone",  # 缓慢坡度（几乎全零高频系数）
}

# 尺寸档：显示尺寸 ∈ [1, 65535]，补后 = 上取整到 16 的倍数。
# 这一张表本身就是采样前的宏块数护栏：表内最大一档 176×144 = 11×9 = 99 宏块，
# 门禁档能抽到的尺寸因此恒在宏块上界之内；更大的 4112×4080 只在 --allow-big 的池子里追加。
SIZES = [
    (16, 16),
    (48, 32),
    (32, 16),
    (64, 64),
    (176, 144),
    (80, 48),
    (35, 21),  # 非 16 倍数 → 走补边
    (16, 32),
]

# --allow-big 追加的那一档大例：4112×4080 = 257×255 = 65 535 宏块，正好是片数字段的取值上界，
# 每片 1 宏块时片数打到上限，满档载荷长度也只有这一档能量到。
BIG_SIZE = (4112, 4080)


# ---------------------------------------------------------------- 工具链与驱动件

MSVC_ROOT_ENV = "CLV_MSVC_ROOT"
SDK_ROOT_ENV = "CLV_SDK_ROOT"
SDK_VERSION_ENV = "CLV_SDK_VERSION"


def version_tuple(name: str) -> tuple:
    """版本目录名里的数字段按整数比：14.51.36110 ⇒ (14, 51, 36110)。非数字段跳过。"""
    return tuple(int(p) for p in name.split(".") if p.isdigit())


def newest_msvc_dir(root: str) -> str:
    """编译器版本目录取「版本最新」的那个，按版本元组比而不是按字典序。

    候选判据 = 该目录下有 lib/x64/libcpmt.lib（静态运行库构件在，才算一个能拿来链的编译器版本）；
    字典序会把 14.51 排在 14.9 之前，取到的就是旧版本目录，所以必须拆成数字段比。
    目录不存在或一个候选都没有都返空串，由调用方走友好退出。
    """
    if not pathlib.Path(root).is_dir():
        return ""
    cands = [
        d.name
        for d in pathlib.Path(root).iterdir()
        if (d / "lib" / "x64" / "libcpmt.lib").is_file()
    ]
    if not cands:
        return ""
    return max(cands, key=version_tuple)


def toolchain_paths(args) -> dict:
    """工具链位置只从环境变量 / 命令行取，脚本里不落任何本机路径。"""
    root = args.msvc_root or os.environ.get(MSVC_ROOT_ENV, "")
    sdk = args.sdk_root or os.environ.get(SDK_ROOT_ENV, "")
    ver = args.sdk_ver or os.environ.get(SDK_VERSION_ENV, "")
    if not root or not sdk or not ver:
        raise SystemExit(
            "缺工具链位置：请设 %s / %s / %s 或用 --msvc-root / --sdk-root / --sdk-ver 传"
            % (MSVC_ROOT_ENV, SDK_ROOT_ENV, SDK_VERSION_ENV)
        )
    msvc_ver = args.msvc_version or newest_msvc_dir(root)
    if not msvc_ver:
        raise SystemExit("MSVC 版本目录没找到：%s" % root)
    return {
        "msvc": str(pathlib.Path(root) / msvc_ver),
        "sdk": sdk,
        "sdk_ver": ver,
    }


# 批里到处是 cmd 的 %VAR% 形态，故模板用 @token@ 占位再做整词替换，不走 % 格式化。
DRIVER_BAT = """@echo off
REM Cross-corpus driver build. ASCII only. Generated by the corpus script.
REM Env: INCLUDE / LIB / PATH assembled from the toolchain roots (no reg.exe lookup).
setlocal
set "MSVCROOT=@MSVC@"
set "SDKROOT=@SDK@"
set "SDKV=@SDKVER@"

set "INCLUDE=%MSVCROOT%\\include;%SDKROOT%\\Include\\%SDKV%\\ucrt;%SDKROOT%\\Include\\%SDKV%\\shared;%SDKROOT%\\Include\\%SDKV%\\um;%SDKROOT%\\Include\\%SDKV%\\winrt;%SDKROOT%\\Include\\%SDKV%\\cppwinrt"
set "LIB=%MSVCROOT%\\lib\\x64;%SDKROOT%\\Lib\\%SDKV%\\ucrt\\x64;%SDKROOT%\\Lib\\%SDKV%\\um\\x64"
set "PATH=%MSVCROOT%\\bin\\Hostx64\\x64;%SDKROOT%\\bin\\%SDKV%\\x64;%SDKROOT%\\Redist\\ucrt\\DLLs\\x64;%PATH%"

cd /d "%~dp0"
REM CRT flag must match the build config of the library given by --lib.
cl /nologo /W3 /utf-8 @CRT@ /std:c11 /I "@INC@" @SRC@ ^
   /link "@LIB@" @EXTRALIBS@ ^
   kernel32.lib user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib > @LOG@ 2>&1
echo BUILD_EXIT=%ERRORLEVEL%
endlocal
exit /b 0
"""

DRIVER_BAT_TOKENS = (
    "MSVC",
    "SDK",
    "SDKVER",
    "INC",
    "SRC",
    "LIB",
    "CRT",
    "EXTRALIBS",
    "LOG",
)

# 库路径里的 Release 系构型词（比对前一律小写、分隔符归一）：命中任一条就配 /MD，一条都不中按 /MDd。
RELEASE_LIB_MARKERS = ("release", "relwithdebinfo", "minsizerel")


def crt_opt_for(lib_path) -> str:
    """CRT 选项跟着 --lib 的构型走：给 Release 库链 Debug CRT（/MDd）就是 CRT 错配。

    判据只取库路径里的构型词，不猜库本身、也不认具体目录名：路径小写、分隔符归一后
    命中 RELEASE_LIB_MARKERS 任一条（`/Release`、`\\Release`、`_release` 这些写法都归到
    release 那一档）判 Release 构型 ⇒ /MD；命中不到的一律按 Debug 构型 ⇒ /MDd，
    即默认档就是 /MDd。库路径里没有构型词时落回 /MDd，所以把库放在带构型名的子目录里
    （build/<配置>/Release/…）即可，别指望脚本从别的目录名猜构型。

    只有这两态：本件配的是动态运行库（/MD 与 /MDd），静态运行库构型（/MT、/MTd）不在判据里，
    也不试图识别——本仓的库构型不出静态运行库那一档，真拿一条 /MT 构型的库来链会在链接期
    撞 LNK2038（RuntimeLibrary 不一致），那属本件不支持面，不靠路径词兜。
    """
    p = str(lib_path).lower().replace("\\", "/")
    return "/MD" if any(m in p for m in RELEASE_LIB_MARKERS) else "/MDd"


def render_driver_bat(values: dict) -> str:
    text = DRIVER_BAT
    for key in DRIVER_BAT_TOKENS:
        text = text.replace("@%s@" % key, str(values[key]))
    return text


def write_driver(out: pathlib.Path, args) -> pathlib.Path:
    src = out / DRIVER_SRC_NAME
    src.write_text(DRIVER_C_SOURCE, encoding="utf-8", newline="\n")
    tp = toolchain_paths(args)
    repo = str(pathlib.Path(args.repo))
    lib = str(pathlib.Path(args.lib).resolve())
    tpdir = str(pathlib.Path(args.thirdparty_lib).resolve())
    extras = " ".join(
        '"%s"' % (pathlib.Path(tpdir) / (n if n.endswith(".lib") else n + ".lib"))
        for n in args.extra_libs.split(",")
        if n
    )
    bat = out / DRIVER_BAT_NAME
    bat.write_text(
        render_driver_bat(
            {
                "MSVC": tp["msvc"],
                "SDK": tp["sdk"],
                "SDKVER": tp["sdk_ver"],
                "INC": str((pathlib.Path(repo) / "include").resolve()),
                "SRC": DRIVER_SRC_NAME,
                "LIB": lib,
                "CRT": crt_opt_for(lib),
                "EXTRALIBS": extras,
                "LOG": "cross_drv_build.log",
            }
        ),
        encoding="ascii",
        newline="\r\n",
    )
    return bat


class _Done:
    """Popen 读完后给调用方留 returncode / stdout 两个字段。"""

    def __init__(self, code: int, text: str):
        self.returncode = code
        self.stdout = text


def _kill_tree(pid: int) -> None:
    """超时的子进程要连子进程一起杀：cmd.exe 被打断后它派生的 cl.exe 还活着，
    并且仍握着构建日志的写句柄 —— Windows 上这会让文件删不掉、空间不释放。

    这里的 20 s 是 taskkill 自身的上限：它只在本机做进程树终止（不走网络、不等退出），
    正常一瞬完成；给到 20 s 是让极端情况（句柄释放排队）也能走完，而不是把它当例超时用，
    例的超时另由 --timeout-sec 管。
    """
    subprocess.run(  # noqa: S603
        ["taskkill.exe", "/F", "/T", "/PID", str(pid)],
        capture_output=True,
        timeout=20.0,
        check=False,
    )


def parse_build_exit(text: str):
    """从批处理 stdout 里取 cl 的退出码：只认 `BUILD_EXIT=<整数>` 这一枚标记，取不到返 None。

    批处理末尾固定 `exit /b 0`（cmd 的返回码没有信息量），所以成功与否看这里解析出来的码，
    不看日志里有没有某个词——编译器换了告警文案也不会把一次失败读成成功。
    """
    for tok in text.split():
        if tok.startswith("BUILD_EXIT="):
            try:
                return int(tok.split("=", 1)[1])
            except ValueError:
                return None
    return None


def build_driver(out: pathlib.Path, args, log) -> bool:
    """编驱动件：cl 退出码为 0 且产物 exe 真在，两样都成立才算建成。"""
    bat = out / DRIVER_BAT_NAME
    exe = out / DRIVER_EXE_NAME
    t0 = time.monotonic()
    try:
        p = subprocess.Popen(  # noqa: S603
            ["cmd.exe", "/c", str(bat)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="strict",
        )
    except OSError as exc:
        log("BUILD cmd.exe 起不来：%s" % type(exc).__name__)
        return False
    try:
        stdout_txt = p.communicate(timeout=args.timeout_sec)[0]
    except subprocess.TimeoutExpired:
        _kill_tree(p.pid)
        p.communicate()  # 收尾并关闭两条管道
        log("BUILD 超时 %.1f s，进程树已杀" % (time.monotonic() - t0))
        return False
    secs = time.monotonic() - t0
    if stdout_txt is None:
        log("BUILD stdout=None（读取失败）")
        return False
    build_exit = parse_build_exit(stdout_txt)
    have_exe = exe.is_file()
    ok = build_exit == 0 and have_exe
    # 失败回显只截末 2000 字符：cl 的错误在末尾汇总，全量会把跑批日志挤爆；完整日志留在产物目录的编译日志文件里
    tail = (
        (out / "cross_drv_build.log").read_text(encoding="gbk", errors="replace")
        if not ok
        else ""
    )
    log(
        "BUILD ok=%s cl退出码=%s 产物在=%s 用时 %.1f s%s"
        % (ok, build_exit, have_exe, secs, "" if ok else "\n" + tail[-2000:])
    )
    return ok


def run_driver(out: pathlib.Path, mode: str, case: str, timeout: float):
    """跑一例：返回 (rc, stdout 行, 用时)。超时 / 崩溃 / 进程起不来都返 rc=None，
    由调用方记成该例的红——一例起不来不许把整批带下去。"""
    exe = str(out / DRIVER_EXE_NAME)
    t0 = time.monotonic()
    try:
        p = subprocess.Popen(  # noqa: S603
            [exe, mode, str(out), case],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="strict",
        )
    except OSError as exc:
        # --no-build 复用不到可执行件、或路径被占用时的启动失败：留一行机器可读标记再记红
        return None, ["EXEC_FAIL", type(exc).__name__], time.monotonic() - t0
    try:
        stdout_txt = p.communicate(timeout=timeout)[0]
    except subprocess.TimeoutExpired:
        _kill_tree(p.pid)
        p.communicate()  # 收尾并关闭两条管道
        return None, ["TIMEOUT"], time.monotonic() - t0
    r = _Done(p.returncode, stdout_txt)
    if r.stdout is None:
        return None, ["STDOUT_NONE"], time.monotonic() - t0
    return (
        r.returncode,
        [ln for ln in r.stdout.splitlines() if ln.strip()],
        time.monotonic() - t0,
    )


# ---------------------------------------------------------------- 容器与 job


def read_container(path: pathlib.Path):
    """驱动件旁路容器 → {(kind, index): bytes}；读不出返回 None，由调用方记成该例的红。

    容器 = magic 'CFXC' + ver + n_entries + 条目，不是 .clv 文件。文件读不到 / 头不符 /
    条目数与实际吃到的不等（崩在半路留下的截断容器）都算单例失败：这里不抛、不退出，
    只记该例的红、不把整批拖挂。n_entries = 0 是合法的 12 字节空容器，返回空表；
    空表没有可比对象，判绿面（compare_case）对它一律判失败。
    """
    try:
        blob = path.read_bytes()
    except OSError:
        return None
    if len(blob) < 12 or blob[:4] != b"CFXC":
        return None
    entries = int.from_bytes(blob[8:12], "little")
    pos, out, seen = 12, {}, 0
    while pos + 12 <= len(blob) and seen < entries:
        kind = int.from_bytes(blob[pos : pos + 4], "little")
        index = int.from_bytes(blob[pos + 4 : pos + 8], "little")
        n = int.from_bytes(blob[pos + 8 : pos + 12], "little")
        pos += 12
        out[(kind, index)] = blob[pos : pos + n]
        pos += n
        seen += 1
    return None if seen != entries else out


def write_job(path: pathlib.Path, fields: dict) -> None:
    lines = []
    for k, v in fields.items():
        if isinstance(v, (list, tuple)):
            lines.append("%s %s" % (k, " ".join(str(x) for x in v)))
        else:
            lines.append("%s %s" % (k, v))
    path.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\r\n")


# ---------------------------------------------------------------- 素材生成


def make_plane_bytes(w: int, h: int, kind: str, seed: int, display=None) -> bytes:
    """按图样生成三平面（行连续、无行距填充）；显示区之外的补边按黑电平填（luma 16 / chroma 128）。

    这里落的数都只是「喂进去的素材」，不是期望值：C 侧读的就是这份字节，两侧不必同算法。
    prng 图样用线性同余 state = 1103515245 × state + 12345（mod 2^31）——ISO C rand() 那一组
    公认常数，取它是为了让伪随机图样在任何 Python 版本上重算都逐字节复现，不靠随机模块的序列；
    掩码 0x7FFFFFFF = 2^31−1 就是该同余的模，`or 1` 是给 seed = 0 的场合留的非零种子
    （该同余在 state = 0 上会原地不动，整张图就成了一色）。
    """
    dw, dh = display if display else (w, h)
    state = seed & 0x7FFFFFFF or 1
    out = bytearray()
    for pl in range(3):
        pw = w if pl == 0 else w // 2
        ph = h if pl == 0 else h // 2
        dwx = dw if pl == 0 else dw // 2
        dhy = dh if pl == 0 else dh // 2
        black = M.BLACK_LUMA if pl == 0 else M.BLACK_CHROMA
        for y in range(ph):
            for x in range(pw):
                if x >= dwx or y >= dhy:
                    out.append(black)
                    continue
                if kind == "hf":
                    v = y * 37 + x * 91 + ((y ^ x) * 13) + pl * 61
                elif kind == "ramp":
                    v = pl * 61 + y * 7 + x * 3
                elif kind == "flat":
                    v = 7 * (pl + 3)
                elif kind == "prng":
                    state = (1103515245 * state + 12345) & 0x7FFFFFFF
                    v = (state >> 8) & 0xFF
                elif kind == "blocks":
                    v = ((((x // 4) ^ (y // 4) ^ pl) & 1) * 190) + 33
                else:  # tone
                    v = (y * 3 + x * 5 + pl * 11) // 4
                out.append(v & 0xFF)
    return bytes(out)


def plane_points(w: int, h: int) -> int:
    """4:2:0 三平面的点数 = 亮度 w×h + 两枚半分辨率色度面 (w//2)×(h//2)。

    补后整帧与显示有效区都用这一式取数，只是代进去的宽高不同（整帧代补后尺寸、
    有效区代显示尺寸），比对与报数不许在同一行里混两轴的点数。
    """
    return w * h + 2 * (w // 2) * (h // 2)


def vop_head_pos(payload: bytes, nfrag: int):
    """定位 VOP 头起点（载荷内的字节下标）；定位不出来返 None，调用方按算不出处理。

    一帧载荷只有一套头布局，差别在片数：声明一片时载荷首字节就是 VOP 头；声明多片时
    首片先带定长 9 字节的片头（frame_id 4 + fragment_count 2 + flags 1 + vop_type 1 + vop_quant 1），
    片头后还有 first_mb 与 mb_run 两枚 varint，走完才到 VOP 头。本式按片数走这一条路径，
    变异旋钮与统计都从它取偏移，不再另设「帧头从首字节起」的第二套口径。
    """
    if nfrag > 1:
        pos = M.SLICE_FIXED_WITH_QUANT
        if len(payload) < pos + 2:
            return None
        br = M.BitReader(payload, pos * 8)
        try:
            br.read_varint()  # first_mb
            br.read_varint()  # mb_run
        except M.ModelError:
            return None
        pos = br.pos // 8
    else:
        pos = 0
    if len(payload) < pos + M.VOP_FIXED_BYTES + 1:
        return None  # VOP 头 = 6 字节定长 + 至少 1 字节 mb_count
    return pos


def fuzz_row_seed(seed_base: int, base_index: int, form_index: int) -> int:
    """鲁棒档一条例的种子 = ((seed_base + 底件序号) × MUT_SEED_MIX + 手法序号) 收进 MUT_SEED_BITS 位。

    按序号派生而不是从一条共享随机序列里连续取数：底件崩没崩、容器读没读出来都不改
    后面例的种子与手法，语料因此是输入参数的纯函数，同参数重跑逐字节对得上。
    """
    return ((seed_base + base_index) * MUT_SEED_MIX + form_index) & (
        (1 << MUT_SEED_BITS) - 1
    )


def case_seed(case: dict) -> int:
    return case.get("seed", 0) + case.get("idx", 0)


def gop_plan(max_b: int, groups: int) -> list:
    """解码序（= DTS 升序）的帧型与 PTS：锚点先行，B 挂在两个锚点之间。

    锚点 k 的 PTS = k × (max_b + 1)；组内 B 的 PTS = (k−1) × (max_b+1) + i，
    于是每个 B 都满足 0 < TRB < TRD 的帧级判据（TRB = 本帧 PTS − 前向 PTS、TRD = 后向 − 前向）。
    """
    g = max_b + 1
    plan = [(0, 0)]  # (vop_type, pts)：首锚点必是 I
    for k in range(1, groups):
        plan.append((1, k * g))  # P 锚点
        for i in range(1, max_b + 1):
            plan.append((2, (k - 1) * g + i))  # B
    return plan


def make_case(seed: int, idx: int, allow_big: bool = False) -> dict:
    """随机交叉的一例：尺寸 × q 档 × 图样 × I/P/B × 宏块布局（每片宏块数）。

    rng 用 (seed << 8) ^ idx 播种：左移 8 位让 seed 落在高位、idx 落在低位，同一 seed 的
    不同轮次因此不会共用同一子序列的前缀。
    """
    rng = random.Random((seed << 8) ^ idx)
    # 宏块数护栏就在采样之前：非大例档只从 SIZES 里取（表内最大 176×144 = 99 宏块），
    # 大例档才把 BIG_SIZE 追加进池子；采样后不再按宏块数事后改写尺寸。
    sizes = SIZES if not allow_big else SIZES + [BIG_SIZE]
    dw, dh = rng.choice(sizes)
    w, h = M.coded_size(dw), M.coded_size(dh)
    total_mb = (w // 16) * (h // 16)
    max_b = rng.choice([0, 0, 1, 2])
    # max_b = 0 时组数可取 1..3（没有 B 就无所谓锚点间距）；带 B 时至少两组，否则 B 缺后向参考。
    groups = rng.choice([1, 2, 3]) if max_b == 0 else rng.choice([2, 3])
    # 切片维：整帧一片（0）恒在；宏块数 ≥ 2 时再加三种切法——一片一宏块、n//2、n//3（向下取整）。
    # 向下取整后 2 ≤ n < 6 会出现重复值（如 n = 2 时 n//2 与 n//3 都归到 1），去重后可能只剩
    # 「整帧一片 + 一片一宏块」两档；n = 1 时任何分片写法都与整帧一片同形，只留 0 一档。
    slice_choices = [0]
    if total_mb >= 2:
        slice_choices = sorted(set([0, 1, total_mb // 2, total_mb // 3]))
    plan = gop_plan(max_b, groups)
    return {
        "case": "s%d_%03d" % (seed, idx),
        "seed": seed,
        "idx": idx,
        "dw": dw,
        "dh": dh,
        "w": w,
        "h": h,
        "qp": 7,
        "q": rng.choice([1, 2, 3, 8, 12, 24, 32, 33, 47, 64, 96, 127]),
        "pattern": rng.choice(sorted(PATTERNS)),
        "maxb": max_b,
        "slice_mb": rng.choice(slice_choices),
        "plan": plan,
        "total_mb": total_mb,
    }


def emit_case_files(out: pathlib.Path, case: dict) -> None:
    src = bytearray()
    for _ in case["plan"]:
        src += make_plane_bytes(
            case["w"],
            case["h"],
            PATTERNS[case["pattern"]],
            case_seed(case),
            (case["dw"], case["dh"]),
        )
    (out / (case["case"] + ".src")).write_bytes(bytes(src))
    dts = list(range(len(case["plan"])))
    write_job(
        out / (case["case"] + ".job"),
        {
            "name": case["case"],
            "w": case["w"],
            "h": case["h"],
            "dw": case["dw"],
            "dh": case["dh"],
            "qp": case["qp"],
            "q": case["q"],
            "maxb": case["maxb"],
            "slice_mb": case["slice_mb"],
            "frames": [t for t, _ in case["plan"]],
            "dts": dts,
            "pts": [p for _, p in case["plan"]],
        },
    )


# ---------------------------------------------------------------- 对照核心


def model_frames(case: dict, container: dict):
    """模型侧：逐帧喂自己解析出来的载荷，返回 [(pts, Plane)] 的出帧序与逐帧错误码。

    片数一律走 nfrag_for(case)，与变异后重解、任务文件里的 frag 字段同一条算式。
    """
    dec = M.ModelDecoder(case["w"], case["h"], case["qp"], case["maxb"])
    errs = []
    emitted = []
    nfrag = nfrag_for(case)
    for k, (vtype, pts) in enumerate(case["plan"]):
        pay = container.get((1, k))
        if pay is None:
            errs.append((k, "NOPAY"))
            continue
        code, pops, disp = dec.decode(pay, nfrag, k, pts)
        errs.append((k, code if code else disp))
        for ppts, sid in pops:
            emitted.append((ppts, dec.frames[sid]))
    for ppts, sid in dec.flush():
        emitted.append((ppts, dec.frames[sid]))
    return emitted, errs


def compare_case(case: dict, container: dict) -> dict:
    """绿向：Python 独立算的重建 == C 解码输出（补后整帧逐字节）。

    判绿必须先有可比对象：空容器（0 条目）、C 侧零出帧、某条输出长度为 0 都属于没有可比内容，
    一律判失败，不许按「差集为空」通过。点数一律按补后整帧给（比的就是整帧全平面字节），
    不与显示尺寸的有效区点数混在同一条读数里。
    """
    res = {"green": None, "detail": ""}
    outc = sum(1 for (kind, _i) in container if kind == 3)
    payc = sum(1 for (kind, _i) in container if kind == 1)
    if payc == 0 or outc == 0:
        res["green"] = False
        res["detail"] = "无可比内容：载荷 %d 条 / C 出帧 %d 条 ⇒ 判失败不判绿" % (
            payc,
            outc,
        )
        return res
    emitted, errs = model_frames(case, container)
    if len(emitted) != outc:
        res["green"] = False
        res["detail"] = "出帧数不等：模型 %d / C %d，逐帧判定 %s" % (
            len(emitted),
            outc,
            errs,
        )
        return res
    diffs = []
    total_points = plane_points(case["w"], case["h"])
    for k in range(outc):
        c_out = container.get((3, k), b"")
        m_bytes = b"".join(emitted[k][1].planes)
        same = sum(1 for a, b in zip(c_out, m_bytes) if a == b)
        diffs.append((k, same, len(c_out), len(m_bytes)))
    bad = [d for d in diffs if d[2] == 0 or d[1] != d[2] or d[2] != d[3]]
    res["green"] = not bad
    res["detail"] = "全等帧 %d/%d（每帧补后整帧 %d 点，三平面逐字节）" % (
        len(diffs) - len(bad),
        len(diffs),
        total_points,
    )
    if bad:
        k, same, n, mn = bad[0]
        res["detail"] += "：首处不等 k=%d 同点 %d / C %d B / Py %d B" % (k, same, n, mn)
    return res


def source_vs_model(case: dict, container: dict) -> dict:
    """源 vs 模型重建的差异点数与总绝对差（这两条度量由模型自己算，不取库的读数）。

    素材字节与平面偏移都只算一次后复用：它们与帧号无关，逐帧重算是白跑。
    """
    out = {}
    src = make_plane_bytes(
        case["w"],
        case["h"],
        PATTERNS[case["pattern"]],
        case_seed(case),
        (case["dw"], case["dh"]),
    )
    offs = pl_offsets(case)
    nfrag = nfrag_for(case)
    for k in range(len(case["plan"])):
        pay = container.get((1, k))
        if pay is None:
            continue
        d = M.ModelDecoder(case["w"], case["h"], case["qp"], case["maxb"])
        # 单帧序列：把该帧之前的帧先喂进去，保证参考帧状态一致；缺任何一条参考载荷就算不出这一帧
        broken_ref = False
        for j in range(k):
            pj = container.get((1, j))
            if pj is None:
                broken_ref = True
                break
            d.decode(pj, nfrag, j, case["plan"][j][1])
        if broken_ref:
            continue
        _, pops, _ = d.decode(pay, nfrag, k, case["plan"][k][1])
        for pp, sid in d.flush():
            pops.append((pp, sid))
        if not pops:
            continue
        plane = d.frames[pops[-1][1]]
        vals = plane.valid_pixels(case["dw"], case["dh"])
        # 有效区逐点：源按显示尺寸从补后整帧里重取
        src_valid = []
        for pl in range(3):
            pw = case["dw"] if pl == 0 else case["dw"] // 2
            ph = case["dh"] if pl == 0 else case["dh"] // 2
            off = offs[pl]
            roww = case["w"] if pl == 0 else case["w"] // 2
            for y in range(ph):
                src_valid += list(src[off + y * roww : off + y * roww + pw])
        diff = sum(1 for a, b in zip(src_valid, vals) if a != b)
        tot = sum(abs(a - b) for a, b in zip(src_valid, vals))
        out[k] = {
            "diff": diff,
            "abs": tot,
            "same": len(vals) - diff,
        }
    return out


def nfrag_for(case: dict) -> int:
    if not case["slice_mb"]:
        return 1
    return max(1, (case["total_mb"] + case["slice_mb"] - 1) // case["slice_mb"])


def pl_offsets(case: dict) -> list:
    w, h = case["w"], case["h"]
    y = w * h
    c = (w // 2) * (h // 2)
    return [0, y, y + c]


# ---------------------------------------------------------------- 红向（坏期望三式）


def first_block_bit_pos(pay: bytes, case: dict, nfrag: int):
    """首块数据的起始位（比特轴）；取不到返 None。

    落点由模型的 walk_payload 交回，与读方解析同一条位轴：件内不再另立第二套头布局口径，
    变异旋钮与统计都从这一条轴定位。只挑真带块数据的宏块（not_coded 与 block_size = 0 的
    宏块没有块数据可翻，落点会打到下一宏块的头上）。
    """
    try:
        res = M.walk_payload(pay, case["qp"], case["w"] // M.MB_SIZE, nfrag)
    except M.ModelError:
        return None
    for mb, start in zip(res.mbs, res.blocks):
        if start is not None and mb.block_size > 0:
            return start
    return None


def bad_expectations(case: dict, container: dict, emitted, kind: str) -> dict:
    """坏期望三式：错一位系数 / 常数化 / 等于源。基准一律取 C 侧解码输出（容器 kind 3 的首帧），
    不与模型自己重建的字节对照——模型对模型只测得出模型的敏感度，测不出断言对库输出的分辨力。

    返回的 status 四档：
      ok     —— 比过了，differs 就是结论（True = 该式与库输出不等，断言有分辨力）
      skip   —— 该式在本例结构性无分辨力，记 reason 并落件，既不算命中也不算不敏感
      reject —— 模型直接拒收翻位后的码流（判不成立或丢帧），单列一档：没有坏期望字节可比，
                不许当成断言敏感
      unknown —— 没做成比较（模型无出帧 / C 侧首帧输出取不到 / 载荷或头短到无处落位 / 位翻不到位）。
                判失败：没比成的这一式就是没测，不许按默认值记成敏感
    """
    res = {"status": "unknown", "reason": ""}
    if not emitted:
        res["reason"] = "no_model_output"
        return res
    lib = container.get((3, 0))
    if not lib:
        # 空容器 / 零出帧 / 首帧长度为 0：没有基准字节，任何一式的比较都是空的
        res["reason"] = "no_c_output"
        return res
    if kind == "onecoef":
        pay = container.get((1, 0))
        if pay is None:
            res["reason"] = "no_payload"
            return res
        nfrag = nfrag_for(case)
        bit = first_block_bit_pos(pay, case, nfrag)
        mutated = bytearray(pay)
        flipped = -1
        rebuilt = None
        rejected = False
        try:
            flipped = flip_first_coef_bit(mutated, bit)
            if flipped >= 0:
                d2 = M.ModelDecoder(case["w"], case["h"], case["qp"], case["maxb"])
                _code, pops, _disp = d2.decode(
                    bytes(mutated), nfrag, 0, case["plan"][0][1]
                )
                if pops:
                    rebuilt = b"".join(d2.frames[pops[-1][1]].planes)
                else:
                    rejected = True
        except (M.ModelError, IndexError, ValueError, KeyError):
            rejected = True
        if flipped < 0:
            res["reason"] = "no_coef_bit_to_flip"
            return res
        if rejected:
            return {"status": "reject", "reason": "model_rejects_mutated_stream"}
        return {"status": "ok", "differs": rebuilt != lib}
    if kind == "const":
        differs = lib != bytes([lib[0]]) * len(lib)
        res = {"status": "ok", "differs": differs}
        if not differs:
            # 显式跳过判据一：库输出本就是同一字节的全常数串，常数化式没有分辨力
            res = {"status": "skip", "reason": "output_already_constant"}
        return res
    # 等于源式：把源像素按库输出的长度截齐再比（本档是有损的，重建不等于源才算断言有牙）
    src = make_plane_bytes(
        case["w"],
        case["h"],
        PATTERNS[case["pattern"]],
        case_seed(case),
        (case["dw"], case["dh"]),
    )
    differs = src[: len(lib)] != lib
    if not differs:
        # 显式跳过判据二：该帧库输出与源像素前缀逐等，等于源式没有分辨力
        return {"status": "skip", "reason": "reconstruction_equals_source"}
    return {"status": "ok", "differs": differs}


def flip_first_coef_bit(mutated: bytearray, bit_pos) -> int:
    """把首块数据的第一位翻掉（定长法 ⇒ 第一系数的最高位；变长法 ⇒ 段首位），成功返该位点。

    bit_pos 由 first_block_bit_pos 在翻之前按码流走出来定位；位点缺失或落在载荷外一律返 -1
    （合法位点是 ≥ 0 的比特下标，不会与 -1 撞值），调用方必须判它——把没翻动的载荷照常喂下去，
    得到的「不敏感」是自造的假读数。
    """
    if bit_pos is None or bit_pos < 0 or bit_pos // 8 >= len(mutated):
        return -1
    mutated[bit_pos // 8] ^= 0x80 >> (bit_pos % 8)
    return bit_pos


# ---------------------------------------------------------------- 畸形档（--fuzz）


def fuzz_streams(
    out: pathlib.Path,
    cases: list,
    container_of: dict,
    seed_base: int,
    log,
    timeout: float,
) -> list:
    """按五种畸形手法生成坏载荷，逐个喂解码器；不崩 / 不挂 / 错误码落在现行枚举这三条都逐例记账，
    由调用方（tier_fuzz）一并收进退出码。

    底件容器读不出的那些例不在 container_of 里，逐例跳过即可，不整批停手。
    手法与种子按（底件序号, 手法序号）派生（见 fuzz_row_seed），先算后用：某条底件没活下来
    只影响它自己那三条写不写出来，不会把后面所有例的手法与种子整体前移。
    """
    rows = []
    codes = set(M.VIDEO_ERROR_CODES)
    for ci, case in enumerate(cases):
        cont = container_of.get(case["case"])
        if cont is None:
            continue
        base = cont.get((1, 0))
        if base is None:
            continue
        nfrag = nfrag_for(case)
        # 声明片数一律给底件的实产片数；非法片数（0 / 越出上限）那一档由模型层的值域判据自核，
        # 本档不产那种任务文件——驱动件收到非正片数也只能按一片喂，测不到判点。
        vop = vop_head_pos(base, nfrag)
        for m in range(3):
            seed = fuzz_row_seed(seed_base, ci, m)
            mutated = mutate_payload(base, random.Random(seed), vop)
            if mutated is None:
                # 载荷短到任何一种手法都落不下点：记一行跳过，不留进名册
                log(
                    "FUZZ seed=%s base=%s form=%d SKIP=too_short len=%d"
                    % (seed, case["case"], m, len(base))
                )
                continue
            kind, blob = mutated
            name = "f%d_%d_%d_%d" % (case["seed"], ci, m, seed)
            (out / (name + ".pay")).write_bytes(blob)
            write_job(
                out / (name + ".job"),
                {
                    "name": name,
                    "dw": case["dw"],
                    "dh": case["dh"],
                    "w": case["w"],
                    "h": case["h"],
                    "qp": case["qp"],
                    "maxb": case["maxb"],
                    "frag": nfrag,
                    "dts": [0],
                    "pts": [0],
                },
            )
            # 鲁棒档的子进程超时同样吃 --timeout-sec（默认 60 s/例）
            rc, lines, secs = run_driver(out, "rob", name, timeout)
            err = None
            for ln in lines:
                # 驱动件一行报多个字段：`ROB err=<码> disp=<处置> mb=<宏块数> slices=<片数>`
                if ln.startswith("ROB "):
                    tok = next((w for w in ln.split() if w.startswith("err=")), None)
                    if tok is not None:
                        err = int(tok.split("=", 1)[1])
            code_name = next(
                (k for k, v in M.VIDEO_ERROR_CODES.items() if v == err), None
            )
            # 挂（进程没跑完：超时 / 起不来 / stdout 读不到）与崩（跑完了但退出码不在现行
            # 返回档里，或落在 Windows 异常码段）分两档记，两条互斥；挂的例本来就没有退出码。
            # 现行返回档 = 驱动件 main 与 mode_enc / mode_rob 实际会回的那些码（0 正常，
            # 1 用法或任务文件读不出，2 输入文件缺，3 打开编解码器失败，4..6 编码循环里的
            # 分配 / 素材不足 / 编码失败），其余任意值都按「跑崩了」判。
            hang = rc is None or "TIMEOUT" in lines
            crash = not hang and (
                (rc not in (0, 1, 2, 3, 4, 5, 6) and rc < 0) or rc >= 0xC0000000
            )
            rows.append(
                {
                    "case": name,
                    "seed": seed,
                    "how": kind,
                    "len": len(blob),
                    "rc": rc,
                    "err": err,
                    "err_name": code_name,
                    "in_enum": code_name in codes,
                    "crash": crash,
                    "hang": hang,
                    # 帧头标志字节 = VOP 头第 5 字节（偏移按 vop_head_pos 定位）；插入类手法会让
                    # 该字节位移，所以这一列只作观测计数，改动数按「同位不同值」的下界报。
                    "flags_byte_touched": (
                        vop is not None
                        and len(base) > vop + 4
                        and len(blob) > vop + 4
                        and blob[vop + 4] != base[vop + 4]
                    ),
                }
            )
            log(
                "FUZZ seed=%s case=%s how=%s len=%d rc=%s err=%s(%s) enum=%s crash=%s hang=%s %.2fs"
                % (
                    seed,
                    name,
                    kind,
                    len(blob),
                    rc,
                    err,
                    code_name,
                    code_name in codes,
                    crash,
                    hang,
                    secs,
                )
            )
    return rows


def mutate_payload(base: bytes, rng: random.Random, vop):
    """五类畸形手法：位翻转 / 截断 / 拼接 / 非法字段值 / varint 非最短形式。

    少于两字节的载荷任何一手都落不下点，返 None 由调用方记跳过。
    非法字段值那一手的五个写点按 VOP 头字段定位（vop 由 vop_head_pos 给，那条轴与读方解析同一条）；
    VOP 头定位不出、或载荷短到写点会越出尾界时退回位翻转——把坏值写在片头上打不中帧头字段，不算这一手。
    """
    if len(base) < 2:
        return None
    b = bytearray(base)
    pick = rng.choice(["bitflip", "trunc", "splice", "illegal", "varint"])
    # 非法字段值那一手要按 VOP 头字段整段写到位（最长落到 vop + 定长 6 + mb_count 那一字节）：
    # VOP 头定位不出、或载荷短到写点会越出尾界时，一律退回位翻转，不靠调用方保证 vop 与长度同形。
    if pick == "illegal" and (vop is None or len(base) < vop + M.VOP_FIXED_BYTES + 1):
        pick = "bitflip"
    if pick == "bitflip":
        # 每例翻 1..3 位：单点与稀疏多点是两类不同的落位，都在这手里取到；翻更多位与截断手法的
        # 覆盖重合（截断已经在改结构长度），故上界取 3 不再放大。
        for _ in range(rng.randint(1, 3)):
            i = rng.randrange(len(b))
            # 1 << 0..7：八个位各当过一次落点，含最高位（varint 的续接位就在最高位）
            b[i] ^= 1 << rng.randrange(8)
    elif pick == "trunc":
        b = b[: max(1, rng.randrange(1, len(b)))]
    elif pick == "splice":
        cut = rng.randrange(1, len(b))
        # 取段上界 24 B：这手的目的是把中段内容重复插回、制造码流错位，不是放大载荷尺寸；
        # 取更多只是把同一处错位摊成更大的一片，覆盖面不增、单例却变重。
        take = rng.randint(1, min(24, len(base)))  # 下界钳到载荷长度内，短载荷上不越界
        mid = len(base) // 2
        b = b[:cut] + bytes(base[mid : mid + take]) + b[cut:]
    elif pick == "varint":
        # 在 VOP 头 mb_count 那枚 varint 上插一节续接位（超长编码 / 非最短形式）
        i = vop + M.VOP_FIXED_BYTES if vop is not None else 0
        if i >= len(b):
            i = 0
        # 插 1..12 节：每节贡献 7 位且都带续接位，12 节已把该字段撑到远超任何合法取值；
        # 再插只是同形放大，读方在第一处就该判非最短形式。
        b = b[: i + 1] + bytes([0x80] * rng.randint(1, 12)) + b[i + 1 :]
    else:
        # 非法字段值五手：帧型 / 两个 fcode / flags 保留位与系数编码法 / dquant 保留位 / 宏块数为 0
        slot = rng.randrange(5)
        if slot == 0:
            b[vop] = rng.choice([3, 7, 0xFF])
        elif slot == 1:
            b[vop + 2] = 0 if rng.random() < 0.5 else 8
            b[vop + 3] = 0
        elif slot == 2:
            b[vop + 4] = rng.choice([0x08, 0x10, 0x80, 0xFF])
        elif slot == 3:
            b[vop + 5] = rng.choice([0x20, 0x80, 0xFF])
        else:
            b[vop + M.VOP_FIXED_BYTES] = 0x00
    return pick, bytes(b)


# ---------------------------------------------------------------- 非法值清单产物


def illegal_value_report() -> list:
    """非法值清单：逐条对规范核，标明出处与算式。层标签实产八种：音频·帧头层、音频·流式子集层、
    视频·VOP 头层、视频·分片层、视频·宏块层、视频·direct、视频·位深、视频·表号——
    前两种取自模型带的两张音频表，后六种由本件下面这些行给出。
    音频两层与视频后几层都只登记判据：本件不产音频语料，视频侧也只把坏值打在帧头字段与片头上，
    宏块层以下的档位由模型层的解析自核。

    关键口径：块大小码 0b1111 → 32768 在**帧头层是合法值**（块大小码表的 0b1000..0b1111 都是 2^v 定值码），
    只在子集层越界；Forbidden / Reserved 位落在采样率、位深、声道那三张表上，与块大小表无关。
    版本词说明：下面写的「视频语法第 1 版」是语法自己的版本轴，与容器 .clv 的版本号不同轴、不同值。
    """
    rows = []
    for what, why in M.AUDIO_HEADER_ILLEGAL:
        rows.append(("音频·帧头层", what, why))
    for what, why in M.AUDIO_SUBSET_ILLEGAL:
        rows.append(("音频·流式子集层", what, why))
    rows += [
        ("视频·VOP 头层", "vop_type ∉ {0,1,2}", "帧型字段只有 I/P/B 三个取值"),
        (
            "视频·VOP 头层",
            "vop_quant ∉ [1, 2^quant_precision − 1]",
            "量化步长上界由 quant_precision 派生",
        ),
        (
            "视频·VOP 头层",
            "vop_fcode_* = 0 或 > 7",
            "两个 fcode 承载规范原值 1..7，0 非法",
        ),
        (
            "视频·VOP 头层",
            "vop_flags 的 bit3 / bit4 非 0",
            "视频语法第 1 版（语法自身的版本轴，与容器版本无关）里顶场优先与隔行位恒 0，这两位是保留位",
        ),
        (
            "视频·VOP 头层",
            "vop_flags 的 bit5-7 ∈ 2..7（系数编码法未赋法）",
            "bit5-7 = 系数编码法，0（定长直写）与 1（EOB+EG）都是合法值；"
            "未赋法的值判帧头不成立，两把库同判（读方按码流选法）",
        ),
        ("视频·VOP 头层", "vop_dquant 的 bit5-7 非 0", "5 bit 带符号偏移，域 −16..+15"),
        ("视频·VOP 头层", "mb_count = 0", "宏块数不得为 0"),
        ("视频·VOP 头层", "varint 非最短形式", "最短形式之外的编码按码流不成立"),
        (
            "视频·分片层",
            "片数 = 0 或 > 65535",
            "分片数上限的唯一载体在片头 fragment_count；本件鲁棒档的声明片数一律取底件实产片数，"
            "这两档只在模型层的值域判据里自核，不成语料",
        ),
        ("视频·分片层", "mb_run = 0", "空片：写方不得产出、读方读到即拒该帧"),
        ("视频·分片层", "first_mb ≠ 前片累加", "片必须按宏块光栅序首尾相接"),
        (
            "视频·分片层",
            "各片 mb_run 累加 ≠ 首片 mb_count",
            "全帧宏块数只由首片 VOP 头说",
        ),
        (
            "视频·分片层",
            "解完后字节未吃光 / 提前耗尽",
            "不多不少；前者码流不成立、后者载荷过短",
        ),
        ("视频·宏块层", "not_coded = 1 出现在 I-VOP", "I-VOP 没有 skip 语义"),
        (
            "视频·宏块层",
            "mcbpc 命中 mb_type = 2（inter4v）或 Stuffing 码",
            "排除集：识别成被排除的工具，拒该帧",
        ),
        (
            "视频·宏块层",
            "mcbpc 落在两张表外（含被裁掉的 +q 行）",
            "表外码字判码流不成立，与上一条分档",
        ),
        (
            "视频·宏块层",
            "B 侧 mb_type ∈ 4..255",
            "自研语法只给 direct / interpolate / backward / forward",
        ),
        ("视频·宏块层", "B 侧 cbpy > 63", "6 bit 全位图，值域 0..63"),
        ("视频·宏块层", "block_size = 0 而块模式非全零", "有残差就至少有一块数据"),
        (
            "视频·宏块层",
            "【法 0】block_size ≠ 在场块数 × (64 × bits(q') ÷ 8)",
            "冗余自校验：定长直写的字节数唯一决定",
        ),
        (
            "视频·宏块层",
            "【法 1】block_size 越过「在场块数 × block_bytes_max(q', intra, 法1)」，或有在场块却为 0",
            "变长只卡上界：单块最坏 = (1 + 6 + 64 × (EG(QF_max) 码长 + 1) + 7) ÷ 8 字节；"
            "位图全零的宏块（整块跳过）只接受 size = 0，沿用定宽等式会把稠密内容判成假不成立",
        ),
        (
            "视频·宏块层",
            "|QF| > QF_max(q')",
            "码流系数越窗判取值不成立（两法同一条，法 1 里分母是 EG 的 level 上界）",
        ),
        (
            "视频·宏块层",
            "【法 0】位图为 1 的块 64 系数全零",
            "全零块的位必须写 0；本法没有能表达「在场且整块全零」的码字，读到即不成立",
        ),
        (
            "视频·宏块层",
            "【法 1】eob 位上的系数为零",
            "本法把「在场且整块全零」写成 all_zero = 1，是合法码字；eob 位上的系数为零与该位的定义自相矛盾 ⇒ 码流不成立",
        ),
        (
            "视频·宏块层",
            "【法 1】EG 前导零数 > 24（level 读到读不尽）",
            "前导零到上限 24 之后仍为 0 即判载荷过短，不把坏码流读成更大的坏值",
        ),
        (
            "视频·宏块层",
            "【法 1】块数据解完剩余 ≥ 8 位",
            "块边界自终止，段末只允许 0..7 位补齐；剩整字节 = 与 block_size 说谎同档",
        ),
        (
            "视频·direct",
            "TRB > TRD",
            "该调用判参数不成立、不做算术（TRB = TRD 不判拒）",
        ),
        (
            "视频·direct",
            "非 0 < TRB < TRD 的帧级时间",
            "整帧跳过，与宏块式入口检查分属两层",
        ),
        (
            "视频·位深",
            "bit_depth ∉ {8,10,12}（16 结构预留）",
            "视频语法第 1 版拒绝 16（语法版本轴，与容器版本无关）",
        ),
        ("视频·位深", "位深 10/12 而扩展标志未置", "拒解整条流"),
        (
            "视频·表号",
            "vlc_table_id ≥ 1",
            "视频语法第 1 版只允许规范表 0（语法版本轴，与容器版本无关）",
        ),
    ]
    return rows


# ---------------------------------------------------------------- 三档 / 三道前置


class Log:
    def __init__(self, path: pathlib.Path, echo=True):
        self.fh = open(path, "w", encoding="utf-8", newline="\n")
        self.echo = echo

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def __call__(self, *a) -> None:
        s = " ".join(str(x) for x in a)
        self.fh.write(s + "\n")
        self.fh.flush()
        if self.echo:
            try:
                print(s)
            except UnicodeEncodeError:
                print(s.encode("ascii", "replace").decode("ascii"))

    def close(self):
        self.fh.close()


def tier_cross(out: pathlib.Path, args, log: Log) -> int:
    t_c = t_p = 0.0
    greens = reds = 0
    bad_green = []
    unknown = []  # 红向里没做成比较的那些式（空输出 / 空容器 / 翻不到位），判红
    rejects = {"onecoef": 0, "const": 0, "source": 0}
    rounds = args.rounds
    for r in range(rounds):
        seed = args.seed_base + r
        case = make_case(seed, r, allow_big=args.allow_big)
        emit_case_files(out, case)
        t0 = time.monotonic()
        rc, lines, secs = run_driver(out, "enc", case["case"], args.timeout_sec)
        t_c += secs
        crash = rc is None or "TIMEOUT" in lines
        cont_path = out / (case["case"] + ".out")
        if crash or not cont_path.is_file():
            reds += 1
            bad_green.append((case["case"], "C 侧崩 / 超时 / 起不来 rc=%s" % rc))
            log(
                "CROSS seed=%s case=%s GREEN=NA C-crash rc=%s lines=%s"
                % (seed, case["case"], rc, lines)
            )
            continue
        cont = read_container(cont_path)
        if cont is None:
            # 崩在半路 / 输出截断的坏容器：只记这一例红，后面的例照跑
            reds += 1
            bad_green.append((case["case"], "容器读不出（输出截断 / 崩在半路）"))
            log("CROSS seed=%s case=%s GREEN=NA bad-container" % (seed, case["case"]))
            continue
        t1 = time.monotonic()
        cmp_res = compare_case(case, cont)
        t_p += time.monotonic() - t1
        if cmp_res["green"]:
            greens += 1
        else:
            reds += 1
            bad_green.append((case["case"], cmp_res["detail"]))
        log(
            "CROSS seed=%s case=%s %dx%d q=%d pat=%s maxb=%d slice_mb=%d frags=%d pay=%d "
            "GREEN=%s %s Cc=%.2fs Py=%.2fs"
            % (
                seed,
                case["case"],
                case["dw"],
                case["dh"],
                case["q"],
                PATTERNS[case["pattern"]],
                case["maxb"],
                case["slice_mb"],
                nfrag_for(case),
                sum(len(v) for (k, _), v in cont.items() if k == 1),
                cmp_res["green"],
                cmp_res["detail"],
                secs,
                time.monotonic() - t1,
            )
        )
        # 红向：坏期望三式各自必须与 C 侧解码输出不等
        emitted, _errs = model_frames(case, cont)
        for kind in ("onecoef", "const", "source"):
            t2 = time.monotonic()
            bad = bad_expectations(case, cont, emitted, kind)
            t_p += time.monotonic() - t2
            status = bad["status"]
            reason = bad.get("reason", "")
            if status == "skip":
                # 显式跳过：不是放行，是该式在本例上结构性无分辨力，记因并落件
                log(
                    "MUT seed=%s case=%s form=%s SKIP(reason=%s)"
                    % (seed, case["case"], kind, reason)
                )
                (out / ("skipped_%s_%s.json" % (case["case"], kind))).write_text(
                    json.dumps(
                        {"case": case, "form": kind, "skip_reason": reason}, default=str
                    ),
                    encoding="utf-8",
                    newline="\n",
                )
                continue
            if status == "unknown":
                # 没做成比较的这一式就是没测：记进判红清单，不许按默认值算成敏感命中
                unknown.append((case["case"], kind, reason))
                log(
                    "MUT seed=%s case=%s form=%s UNKNOWN(reason=%s) 判红"
                    % (seed, case["case"], kind, reason)
                )
                continue
            if status == "reject":
                # 模型把翻位后的码流直接拒收：没有期望字节可比，单列一档，既不计敏感也不计不敏感
                rejects[kind] += 1
                log(
                    "MUT seed=%s case=%s form=%s REJECT(模型拒收该码流)"
                    % (seed, case["case"], kind)
                )
                continue
            hit = bad["differs"]
            tail = "" if hit else " 已固化成定例并落件"
            log(
                "MUT seed=%s case=%s form=%s %s%s"
                % (
                    seed,
                    case["case"],
                    kind,
                    "SENSITIVE" if hit else "INSENSITIVE",
                    tail,
                )
            )
            if not hit:
                (out / ("insensitive_%s_%s.json" % (case["case"], kind))).write_text(
                    json.dumps({"case": case, "form": kind}, default=str),
                    encoding="utf-8",
                    newline="\n",
                )
    total = t_c + t_p
    log(
        "CROSS 汇总：绿向 %d / %d，红向（坏期望 != C 侧解码输出）见 MUT 行，"
        "模型拒收 %d 式 / 未做成比较 %d 式；C 侧累计 %.1fs，Py 侧累计 %.1fs"
        % (greens, rounds, sum(rejects.values()), len(unknown), t_c, t_p)
    )
    if bad_green:
        log("CROSS 绿向不等清单（按 seed 可重放）：")
        for name, detail in bad_green[:20]:
            log("  %s :: %s" % (name, detail))
    if unknown:
        log("CROSS 红向未做成比较清单（没比成 = 没测，判红）：")
        for name, form, reason in unknown[:20]:
            log("  %s form=%s :: %s" % (name, form, reason))
    if rounds:
        # 实测每例单价 = C 侧累计 + 模型侧累计（绿向 + 三式红向）÷ 轮数；轮数按 --rounds 取，不自动改
        price = total / rounds
        derived = max(1, int((args.budget_sec - SAFETY_MARGIN_SEC) / max(0.001, price)))
        log(
            "预算核对：实测每例单价 %.2fs =（C %.1fs + Py %.1fs）÷ %d 轮 ⇒ "
            "(门禁 %.0fs − 固定余量 %.0fs) ÷ %.2fs 能容 %d 轮；本轮轮数按 --rounds 实取 %d，wall %.1fs"
            % (
                price,
                t_c,
                t_p,
                rounds,
                args.budget_sec,
                SAFETY_MARGIN_SEC,
                price,
                derived,
                rounds,
                total,
            )
        )
    return 0 if (not bad_green and not unknown) else 1


def tier_fuzz(out: pathlib.Path, args, log: Log) -> int:
    # 底件数取 ⌈--cases / 3⌉：每底件三条手法，cases 是 3 的倍数时实产条数正好等于它
    nbases = max(1, (args.cases + 2) // 3)
    picked = [make_case(args.seed_base + r, r) for r in range(nbases)]
    container_of = {}
    for case in picked:
        emit_case_files(out, case)
        rc, _lines, _secs = run_driver(out, "enc", case["case"], args.timeout_sec)
        p = out / (case["case"] + ".out")
        if rc is None or not p.is_file():
            log("FUZZ base skip case=%s rc=%s" % (case["case"], rc))
            continue
        cont = read_container(p)
        if cont is None:
            # 坏容器（崩在半路 / 输出截断）只跳过这一条底件，其余底件照跑
            log("FUZZ base skip case=%s 容器读不出" % case["case"])
            continue
        container_of[case["case"]] = cont
    rows = fuzz_streams(
        out, picked, container_of, args.seed_base, log, args.timeout_sec
    )
    if not rows:
        # 一条读数都没有 = 底件全没跑通 / 驱动件起不来 / 全是落不下点的短载荷：
        # 本档等于整档没测，判红，不许按「没有崩的例」放行
        log(
            "FUZZ 判红：%d 条底件没有任何畸形例读数（底件没跑通 / 驱动件起不来）"
            % len(picked)
        )
        return 1
    crashed = [r for r in rows if r["crash"]]
    hang = [r for r in rows if r["hang"]]
    outside = [r for r in rows if r["err"] is not None and not r["in_enum"]]
    # 既没崩也没挂、却没报出错误码读数的例（驱动件走的是早退档，stdout 里没有 ROB err= 那一行）
    # 判据三无从可判 ⇒ 按「没测」判红：空读数不算通过，否则「没有崩的例」就是自造的假绿。
    no_read = [r for r in rows if r["err"] is None and not r["crash"] and not r["hang"]]
    flags_touched = sum(1 for r in rows if r["flags_byte_touched"])
    log(
        "FUZZ 汇总：M=%d 条（存活底件 %d 条 × 三条手法；短载荷落不下点的逐手法记了 SKIP 行），"
        "崩 %d / 挂 %d（两档互斥：挂 = 进程没跑完，崩 = 跑完了但退出码不在现行返回档里）"
        " / 错误码不在现行枚举 %d / 没报出错误码读数 %d / 帧头 flags 字节（VOP 头第 5 字节，同位不同值）%d 条"
        "（flags 被动过 ⇒ 含系数编码法被切档或切到未赋法，读方按码流选法，本档只计数不判对错）；"
        "码值分布 %s；三条判据（崩 / 挂 / 码在枚举内）都进退出码，读不出码值的例按没测判红"
        % (
            len(rows),
            len(container_of),
            len(crashed),
            len(hang),
            len(outside),
            len(no_read),
            flags_touched,
            sorted({r["err_name"] for r in rows if r["err_name"]}),
        )
    )
    for r in outside[:20]:
        log(
            "FUZZ 判红 case=%s 错误码 %s 不在现行枚举 rc=%s"
            % (r["case"], r["err"], r["rc"])
        )
    for r in no_read[:20]:
        log(
            "FUZZ 判红 case=%s 没报出错误码读数 rc=%s（判据三无读数可判 ⇒ 按没测判红）"
            % (r["case"], r["rc"])
        )
    # 名册不落任何耗时 / 墙钟字段：逐例用时只进日志行，名册字节必须是输入的纯函数，跨轮可逐字对平
    with open(out / "fuzz_roster.json", "w", encoding="utf-8", newline="\n") as fh:
        json.dump(rows, fh, ensure_ascii=False, indent=1)
    return 1 if (crashed or hang or outside or no_read) else 0


def tier_mutate(out: pathlib.Path, args, log: Log) -> int:
    """断言敏感性抽检：三式坏期望各自必须与 C 侧解码输出不等；撞车的按 seed 固化成定例。

    只有真比成了的式子才进「命中 / 不敏感」两档；模型拒收翻位码流的单列一档（没有期望字节可比）；
    结构性无分辨力的记 SKIP；没做成比较的（空输出 / 空容器 / 翻不到位 / 底件起不来）一律记 UNKNOWN
    并判红。一档全跳光、一式没比成都不许返成功。
    """
    hit = {"onecoef": 0, "const": 0, "source": 0}
    ins = {"onecoef": 0, "const": 0, "source": 0}
    skips = {"onecoef": 0, "const": 0, "source": 0}
    rejects = {"onecoef": 0, "const": 0, "source": 0}
    unknown = []
    judged = 0
    for r in range(args.rounds):
        # 本档种子 = seed_base + MUT_SEED_MIX × (轮次 + 1)，与交叉档那条 seed_base + r 的序列错开
        seed = args.seed_base + MUT_SEED_MIX * (r + 1)
        case = make_case(seed, r)
        emit_case_files(out, case)
        rc, _lines, _secs = run_driver(out, "enc", case["case"], args.timeout_sec)
        p = out / (case["case"] + ".out")
        if rc is None or not p.is_file():
            unknown.append((case["case"], "-", "C 侧崩 / 超时 / 起不来 rc=%s" % rc))
            log("MUTATE seed=%s case=%s UNKNOWN(base) rc=%s" % (seed, case["case"], rc))
            continue
        cont = read_container(p)
        if cont is None:
            # 坏容器（崩在半路 / 输出截断）只跳这一例，与上面 rc 缺失同一档处理
            unknown.append((case["case"], "-", "容器读不出（输出截断 / 崩在半路）"))
            log(
                "MUTATE seed=%s case=%s UNKNOWN(base) bad-container"
                % (seed, case["case"])
            )
            continue
        emitted, _ = model_frames(case, cont)
        for kind in ("onecoef", "const", "source"):
            b = bad_expectations(case, cont, emitted, kind)
            status = b["status"]
            reason = b.get("reason", "")
            if status == "skip":
                skips[kind] += 1
                log(
                    "MUTATE seed=%s case=%s form=%s SKIP(reason=%s) 记因并落件"
                    % (seed, case["case"], kind, reason)
                )
                (out / ("skipped_%s_%s.json" % (case["case"], kind))).write_text(
                    json.dumps(
                        {
                            "seed": seed,
                            "case": case["case"],
                            "form": kind,
                            "skip_reason": reason,
                        },
                        default=str,
                    ),
                    encoding="utf-8",
                    newline="\n",
                )
                continue
            if status == "reject":
                rejects[kind] += 1
                log(
                    "MUTATE seed=%s case=%s form=%s REJECT(模型拒收该码流) 不计敏感命中"
                    % (seed, case["case"], kind)
                )
                continue
            if status == "unknown":
                unknown.append((case["case"], kind, reason))
                log(
                    "MUTATE seed=%s case=%s form=%s UNKNOWN(reason=%s) 判红"
                    % (seed, case["case"], kind, reason)
                )
                continue
            judged += 1
            if b["differs"]:
                hit[kind] += 1
                log(
                    "MUTATE seed=%s case=%s form=%s SENSITIVE"
                    % (seed, case["case"], kind)
                )
            else:
                ins[kind] += 1
                log(
                    "MUTATE seed=%s case=%s form=%s INSENSITIVE 记账并固化"
                    % (seed, case["case"], kind)
                )
                (out / ("insensitive_%s_%s.json" % (case["case"], kind))).write_text(
                    json.dumps(
                        {"seed": seed, "case": case["case"], "form": kind}, default=str
                    ),
                    encoding="utf-8",
                    newline="\n",
                )
    log(
        "MUTATE 汇总：比成 %d 式，三式命中 %s，不敏感 %s，结构性跳过 %s，模型拒收 %s，未做成比较 %d 式"
        "（不敏感不等于放行；结构性跳过 = 判据式在本例上无分辨力，逐条落 skipped_*.json 带原因；"
        "未做成比较 = 没测，判红）" % (judged, hit, ins, skips, rejects, len(unknown))
    )
    for name, form, reason in unknown[:20]:
        log("MUTATE 判红 case=%s form=%s :: %s" % (name, form, reason))
    return 0 if (judged and not unknown) else 1


def pre2_coverage(out: pathlib.Path, log: Log) -> int:
    """参数覆盖对齐：模型层覆盖的维 vs --cross 的采样空间；超出的一律显式跳过并记账。

    表的第二列是「模型层覆盖」——模型按规范判到哪儿，不等于本件的语料打到哪儿：
    本件只出视频码流语料，音频两行没有语料，宏块层以下的视频行也只由模型解析自核。
    """
    dims = [
        (
            "尺寸（显示 × 补后）",
            "覆盖：任意 [1,65535]，补后 = 上取整到 16",
            "采样集 %s（--allow-big 再追加 %s）" % (SIZES, BIG_SIZE),
            "无跳过",
        ),
        (
            "q 档",
            "覆盖：q ∈ [1, 2^qp − 1]，位宽由两张表算式给",
            "qp = 7 ⇒ [1,127]",
            "qp ∈ {8,9} 显式跳过并记账（上界 255 后位宽沿用末档，属规范未定段）",
        ),
        ("图样", "覆盖：6 种（hf/ramp/flat/prng/blocks/tone）", "全部进采样", "无跳过"),
        (
            "帧型 I/P/B",
            "覆盖：I 全帧内、P 读码流矢量、B 走 direct 双向平均",
            "GOP 计划给 0..2 连续 B",
            "B 带残差的形态 C 编码器不产出 ⇒ 只在解析层覆盖，绿向不采样",
        ),
        (
            "宏块布局（每片宏块数）",
            "覆盖：整帧一片 + 最多三种切法（n ≥ 6 时三种切法互异；2 ≤ n < 6 去重后只剩一种；n = 1 只有整帧一片）",
            "slice_mb ∈ {0, 1, n//2, n//3}（向下取整后去重）",
            "无跳过",
        ),
        (
            "分片数",
            "覆盖：整帧一片到每片 1 宏块；片数上界那档只在 --allow-big 的满档例里打到",
            "nfrag = ⌈总宏块数 ÷ slice_mb⌉，slice_mb = 0 时按一片",
            "满档不进 --cross 门禁（单例 12 s 级）；声明片数的非法档（0 与越出 65535）只在模型层"
            "的值域判据里自核，鲁棒档的 frag 一律给底件实产片数",
        ),
        (
            "块模式位图",
            "覆盖：64 种取值全（解析层）；绿向按实际产出",
            "全零块 / 部分在场",
            "无跳过",
        ),
        (
            "量化偏移",
            "覆盖：帧级 d_vop ∈ [−16,15]、宏块级 −2..+1（解析层）",
            "编码侧恒 0",
            "C 编码器不写非零偏移 ⇒ 绿向不采样，只在畸形档验证叠加式",
        ),
        (
            "系数编码法",
            "覆盖：法 0（定长直写）与法 1（EOB 截尾 + Exp-Golomb 幅值）；"
            "写方哪一法 = 构建期，读方哪一法 = 码流 bit5-7，两档解码共用一把库",
            "两把库（默认档 / 定宽档）各进 --cross 一遍，每遍轮数按 --rounds 取（默认 12）"
            "⇒ 双轴合计 2×--rounds",
            "无跳过；未赋法的值（2..7）只在畸形档测拒",
        ),
        (
            "音频：采样率 / 声道 / 位深 / 块大小",
            "模型层覆盖：帧头三张表 + 子集上界（本件不产音频语料，只登记判据）",
            "—（无音频采样）",
            "位深 ∉ {16,24} 子集拒 ⇒ 只做红向；本件无音频例，该判据由模型层的两张表自核",
        ),
        (
            "音频：帧内子语法",
            "不覆盖（CLV 层不解子帧头，由后端兜底）",
            "—",
            "显式跳过并记账：预测阶 / Rice 阶不进本模型",
        ),
    ]
    with open(out / "coverage_align.tsv", "w", encoding="utf-8", newline="\n") as fh:
        fh.write(
            "维\t模型覆盖（模型层判据覆盖面，不等于本件语料覆盖）\t采样空间\t跳过与记账\n"
        )
        for row in dims:
            fh.write("\t".join(row) + "\n")
    log(
        "PRE2 覆盖对齐表写出：维 %d 条，其中显式跳过 %d 条（见 coverage_align.tsv）"
        % (len(dims), sum(1 for d in dims if "跳过" in d[3]))
    )
    return 0


def pre1_anchor(out: pathlib.Path, args, log: Log) -> int:
    """前置 1：把本模型算出的读数逐条与期望值对账。

    每条读数写成（说明 / 期望值 / 实得值）。期望值一律优先给派生式：点数走 plane_points，
    满档载体长度走「单块字节数 × 块数 + 头侧字节数」那条算式；只有派生不出来的格子取实测钉值——
    「源 vs 重建」的差异点数与总绝对差、以及法 1 的满档整段载体长度。
    系数编码法只换载体、不动像素，所以按像素差量的那几格在法 0 / 法 1 两档逐格同值，
    一份钉值对两档同时成立；满档那格量的就是载体长度本身，两档各钉一个值。
    任何一格算不出（容器读不出 / 载荷没产出 / 大例没实跑）都记 MISS 判红，不设「跳过算通过」那一档。
    """
    anchors = []
    # 单帧 I 例：48×32 高频面、q = 24、整帧一片
    spec = {
        "case": "apre_i_q24",
        "dw": 48,
        "dh": 32,
        "w": 48,
        "h": 32,
        "qp": 7,
        "q": 24,
        "pattern": 0,
        "maxb": 0,
        "slice_mb": 0,
        "plan": [(0, 0)],
        "total_mb": 6,
    }
    base_points = plane_points(spec["dw"], spec["dh"])  # 48×32 ⇒ 1536 + 2×384
    emit_case_files(out, spec)
    rc, _l, _s = run_driver(out, "enc", spec["case"], args.timeout_sec)
    if rc is None:
        log("PRE1 驱动件未跑通 rc=%s" % rc)
        return 1
    cont = read_container(out / (spec["case"] + ".out"))
    if cont is None:
        # 坏容器（崩在半路 / 输出截断）让本组读数算不出：留 None，逐条记 MISS 后继续跑其余件
        log("PRE1 对账件容器读不出 case=%s，该组三条读数算 MISS" % spec["case"])
        cont = {}
    emitted, errs = model_frames(spec, cont)
    vals = source_vs_model(spec, cont)
    v0 = vals.get(0, {})
    c0 = cont.get((3, 0))
    same_c = (
        sum(1 for a, b in zip(c0, b"".join(emitted[0][1].planes)) if a == b)
        if (emitted and c0)
        else None
    )
    anchors.append(
        (
            "补后整帧 %d 点全等（编码器内部重建 == 解码输出）" % base_points,
            base_points,
            same_c,
        )
    )
    anchors.append(
        (
            "源 vs 重建的差异点（48×32 高频面、q=24；钉值取实测）",
            1388,
            v0.get("diff"),
        )
    )
    anchors.append(("源 vs 重建的总绝对差（同上；钉值取实测）", 2215, v0.get("abs")))
    # q 档横扫：三档各自的相等点与总绝对差（相等点钉的是实测，两法同值）
    SAME_BY_Q = {8: 1279, 32: 256, 96: 173}
    ABS_BY_Q = {8: 1222, 32: 5898, 96: 11361}
    sweep = {}
    for q in (8, 32, 96):
        c2 = dict(spec)
        c2["case"] = "apre_q%d" % q
        c2["q"] = q
        emit_case_files(out, c2)
        run_driver(out, "enc", c2["case"], args.timeout_sec)
        # 读不出就是读不出：该档读数留 None（记 MISS），本批其余档照扫
        k2 = read_container(out / (c2["case"] + ".out")) or {}
        d2 = source_vs_model(c2, k2).get(0, {})
        sweep[q] = d2
        anchors.append(
            (
                "q=%d 档相等点（三档 1279/256/173 之一）" % q,
                SAME_BY_Q[q],
                d2.get("same"),
            )
        )
    for q in (8, 32, 96):
        anchors.append(
            ("q=%d 档总绝对差" % q, ABS_BY_Q[q], sweep.get(q, {}).get("abs"))
        )
    # 真位移例：64×64、I 后接 P，第二帧是同一图样右移 4 像素（色度移 2 像素）
    disp = {
        "case": "apre_disp64",
        "dw": 64,
        "dh": 64,
        "w": 64,
        "h": 64,
        "qp": 7,
        "q": 8,
        "pattern": 0,
        "maxb": 0,
        "slice_mb": 0,
        "plan": [(0, 0), (1, 1)],
        "total_mb": 16,
    }
    disp_points = plane_points(disp["dw"], disp["dh"])  # 64×64 ⇒ 4096 + 2×1024
    # 真位移的第二帧 = 图案右移 4 像素（亮度）、色度右移 2 像素，左边界复制
    src = bytearray()
    src += make_plane_bytes(64, 64, "hf", 0)
    src += shifted_pattern_bytes()
    (out / (disp["case"] + ".src")).write_bytes(bytes(src))
    emit_case_files_keep_src(out, disp)
    rc, _l, _s = run_driver(out, "enc", disp["case"], args.timeout_sec)
    cont2 = read_container(out / (disp["case"] + ".out")) or {}
    e2, _er2 = model_frames(disp, cont2)
    # 容器读不出时 same2 留 None（记 MISS），不当成算得出但不等
    same2 = None if not cont2 else 0
    if len(e2) >= 2 and cont2.get((3, 1)) is not None:
        same2 = sum(
            1 for a, b in zip(cont2[(3, 1)], b"".join(e2[1][1].planes)) if a == b
        )
    elif len(e2) == 1 and cont2.get((3, 0)) is not None:
        same2 = sum(
            1 for a, b in zip(cont2[(3, 0)], b"".join(e2[0][1].planes)) if a == b
        )
    anchors.append(
        (
            "真位移端到端 %d 点逐字节全等" % disp_points,
            disp_points,
            same2,
        )
    )
    # 满档载荷字节数：构型（系数编码法）从产出的码流里读，不靠命令行加旗标
    bits24 = M.bits_per_coef(24, True)
    single_block = M.block_bytes(24, True)
    # 满档 = 每片 1 宏块，宏块数与片数同时打到上界 M.MAX_FRAGMENT_COUNT = 65 535 = 257×255
    full_mb_count = M.MAX_FRAGMENT_COUNT
    carrier = full_mb_count * M.BLOCKS_PER_MB * single_block
    # 头侧字节 = 分片头 + 宏块头与 VOP 头：两项都是从满档整段实测读数里减掉载体项得到的
    SLICE_HEAD_BYTES = 704374
    MB_AND_VOP_HEAD_BYTES = 262149
    head_bytes = SLICE_HEAD_BYTES + MB_AND_VOP_HEAD_BYTES
    identity = carrier + head_bytes
    single_block_max1 = M.block_bytes_max(24, True, M.COEF_EOB_EG)
    carrier1_worst = full_mb_count * M.BLOCKS_PER_MB * single_block_max1
    # 满档载体长度分两档钉：法 0 = 「载体 + 头侧常数」这条算式；法 1 的载体随内容走 ⇒ 只钉整段实测长度
    FULL_PAYLOAD_COEF1 = 9293554
    FULL_PAYLOAD_BY_COEF_METHOD = {0: identity, 1: FULL_PAYLOAD_COEF1}
    big = {
        "case": "apre_full",
        "dw": BIG_SIZE[0],
        "dh": BIG_SIZE[1],
        "w": BIG_SIZE[0],
        "h": BIG_SIZE[1],
        "qp": 7,
        "q": 24,
        "pattern": 1,
        "maxb": 0,
        "slice_mb": 1,
        "plan": [(0, 0)],
        "total_mb": full_mb_count,
    }
    pay_bytes = None
    coef_method = None
    vop_off_full = None
    if args.allow_big:
        emit_case_files(out, big)
        rc, lines, secs = run_driver(out, "enc", big["case"], args.big_timeout)
        for ln in lines:
            if ln.startswith("FRAME"):
                pay_bytes = int(ln.split("pay=")[1].split()[0])
        full_cont = read_container(out / (big["case"] + ".out"))
        full_base = full_cont.get((1, 0)) if full_cont else None
        # 偏移按 vop_head_pos 定位（多片时先走掉 9 字节定长片头与 first_mb / mb_run 两枚 varint）；
        # 系数编码法就在 VOP 头第 5 字节的 bit5-7
        vop_off_full = vop_head_pos(full_base, nfrag_for(big)) if full_base else None
        if vop_off_full is not None and len(full_base) > vop_off_full + 4:
            coef_method = (full_base[vop_off_full + 4] >> 5) & 0x07
        want_full = FULL_PAYLOAD_BY_COEF_METHOD.get(coef_method)
        anchors.append(
            (
                "满档载荷 构型=法 %s ⇒ %s B" % (coef_method, want_full),
                want_full,
                pay_bytes,
            )
        )
        log(
            "PRE1 满档单例 wall %.1f s，VOP 头偏移 = %s，构型从码流读 = 法 %s"
            % (secs, vop_off_full, coef_method)
        )
    else:
        anchors.append(
            (
                "满档载荷（本轮没给 --allow-big，无读数 ⇒ 判红：规范敲法带这个开关）",
                identity,
                None,
            )
        )
    ok = True
    for name, want, got in anchors:
        mark = "MATCH" if got == want else "MISS"
        if mark != "MATCH":
            ok = False
        log("PRE1 %s %s：期望 %s / 模型 %s" % (mark, name, want, got))
    log(
        "PRE1 算式核对【法 0】：bits(24,帧内) = %d ⇒ 单块 64 × %d ÷ 8 = %d B ⇒ %d × %d × %d = %d，"
        "+ 头侧 %d = %d（头侧 = 分片头 %d + 宏块头与 VOP 头 %d，两项系实测相减所得）"
        % (
            bits24,
            bits24,
            single_block,
            full_mb_count,
            M.BLOCKS_PER_MB,
            single_block,
            carrier,
            head_bytes,
            identity,
            SLICE_HEAD_BYTES,
            MB_AND_VOP_HEAD_BYTES,
        )
    )
    log(
        "PRE1 算式核对【法 1】：单块最坏 block_bytes_max(24,帧内) = %d B ⇒ 最坏载体 %d × %d × %d = %d B；"
        "实际载体随内容走 ⇒ 本法不拆头侧，只钉满档单例的整段实测长度 %d B"
        % (
            single_block_max1,
            full_mb_count,
            M.BLOCKS_PER_MB,
            single_block_max1,
            carrier1_worst,
            FULL_PAYLOAD_COEF1,
        )
    )
    log("PRE1 逐帧判定（%s）：%s" % (spec["case"], errs))
    return 0 if ok else 1


def emit_case_files_keep_src(out: pathlib.Path, case: dict) -> None:
    """只为对账件服务：已经手写好 .src 的场合，只补 job 文件。"""
    dts = list(range(len(case["plan"])))
    write_job(
        out / (case["case"] + ".job"),
        {
            "name": case["case"],
            "w": case["w"],
            "h": case["h"],
            "dw": case["dw"],
            "dh": case["dh"],
            "qp": case["qp"],
            "q": case["q"],
            "maxb": case["maxb"],
            "slice_mb": case["slice_mb"],
            "frames": [t for t, _ in case["plan"]],
            "dts": dts,
            "pts": [p for _, p in case["plan"]],
        },
    )


def shifted_pattern_bytes() -> bytes:
    """真位移锚点件的第二帧：图案右移 4 像素（色度按 4:2:0 减半移 2 像素），左边界复制第 0 列。"""
    out = bytearray()
    for pl in range(3):
        half = pl != 0
        shift = 2 if half else 4
        pw = 32 if half else 64
        ph = 32 if half else 64
        for y in range(ph):
            for x in range(pw):
                sx = x - shift
                if sx < 0:
                    sx = 0
                out.append((y * 37 + sx * 91 + ((y ^ sx) * 13) + pl * 61) & 0xFF)
    return bytes(out)


def pre3_green_anchor(out: pathlib.Path, args, log: Log) -> int:
    """前置 3：正式 sweep 前抽固定 seed 做绿向锚定（C 与 Python 逐字节对上）。"""
    seeds = list(args.anchor_seeds or PRE3_ANCHOR_SEEDS)
    bad = []
    for i, seed in enumerate(seeds):
        case = make_case(seed, i)
        emit_case_files(out, case)
        rc, _lines, secs = run_driver(out, "enc", case["case"], args.timeout_sec)
        p = out / (case["case"] + ".out")
        if rc is None or not p.is_file():
            bad.append((seed, case["case"], "driver rc=%s" % rc))
            log("PRE3 seed=%s case=%s ANCHOR=CRASH rc=%s" % (seed, case["case"], rc))
            continue
        cont = read_container(p)
        if cont is None:
            # 坏容器（崩在半路 / 输出截断）只记这一锚失败，其余固定 seed 照跑
            bad.append((seed, case["case"], "容器读不出（输出截断 / 崩在半路）"))
            log("PRE3 seed=%s case=%s ANCHOR=BAD-CONTAINER" % (seed, case["case"]))
            continue
        r = compare_case(case, cont)
        log(
            "PRE3 seed=%s case=%s %dx%d q=%d pat=%s maxb=%d GREEN=%s %s (%.2fs)"
            % (
                seed,
                case["case"],
                case["dw"],
                case["dh"],
                case["q"],
                PATTERNS[case["pattern"]],
                case["maxb"],
                r["green"],
                r["detail"],
                secs,
            )
        )
        if not r["green"]:
            bad.append((seed, case["case"], r["detail"]))
    log(
        "PRE3 汇总：%d 个固定 seed，绿锚不过 %d 个%s"
        % (len(seeds), len(bad), "（先修模型边界，别当实现红）" if bad else "")
    )
    return 1 if bad else 0


def write_seeds(out: pathlib.Path, args, log: Log) -> None:
    """逐档写种子清单与可重放串：每档各按自己的派生式列，不再统一写成交叉档那一条。

    交叉档 seed = seed_base + 轮次；鲁棒档底件 seed = seed_base + 底件序号，逐条例的手法种子
    按 fuzz_row_seed 派生；敏感档 seed = seed_base + MUT_SEED_MIX × (轮次 + 1)；
    前置 1 用写死的对账件（无 seed），前置 2 只出对齐表，前置 3 用固定 seed 表。
    """
    nbases = max(1, (args.cases + 2) // 3)
    cross_seeds = [args.seed_base + r for r in range(args.rounds)]
    fuzz_base_seeds = [args.seed_base + r for r in range(nbases)]
    row_seeds = [
        fuzz_row_seed(args.seed_base, ci, m) for ci in range(nbases) for m in range(3)
    ]
    mutate_seeds = [args.seed_base + MUT_SEED_MIX * (r + 1) for r in range(args.rounds)]
    anchor_seeds = list(args.anchor_seeds or PRE3_ANCHOR_SEEDS)
    # 重放串只落占位符，不落本机库路径：清单字节因此与机器无关，可跨轮跨机逐字对平
    libpart = " --lib <静态库> --thirdparty-lib <第三方库目录>"
    roster = {
        "seed_base": args.seed_base,
        "cross": {
            "rounds": args.rounds,
            "seeds": cross_seeds,
            "replay": "python scripts/gen_fuzz_corpus.py --cross --out <同一临时目录>%s "
            "--seed-base %d --rounds %d" % (libpart, args.seed_base, args.rounds),
        },
        "fuzz": {
            "cases_arg": args.cases,
            "base_count": nbases,
            "base_seeds": fuzz_base_seeds,
            "row_seed_formula": "((seed_base + 底件序号) × %d + 手法序号) mod 2^%d"
            % (MUT_SEED_MIX, MUT_SEED_BITS),
            "row_seeds": row_seeds,
            "replay": "python scripts/gen_fuzz_corpus.py --fuzz --out <同一临时目录>%s "
            "--seed-base %d --cases %d" % (libpart, args.seed_base, args.cases),
        },
        "mutate": {
            "rounds": args.rounds,
            "seed_formula": "seed_base + %d × (轮次 + 1)" % MUT_SEED_MIX,
            "seeds": mutate_seeds,
            "replay": "python scripts/gen_fuzz_corpus.py --mutate --out <同一临时目录>%s "
            "--seed-base %d --rounds %d" % (libpart, args.seed_base, args.rounds),
        },
        "pre1": {"seeds": [], "note": "对账件的尺寸与 q 档写死在件里，无 seed 可重放"},
        "pre2": {"seeds": [], "note": "只出覆盖对齐表，不取 seed"},
        "pre3": {
            "seeds": anchor_seeds,
            "replay": "python scripts/gen_fuzz_corpus.py --pre3 --out <同一临时目录>%s "
            "--anchor-seeds %s" % (libpart, " ".join(str(s) for s in anchor_seeds)),
        },
    }
    (out / "seed_roster.json").write_text(
        json.dumps(roster, ensure_ascii=False, indent=1, sort_keys=True),
        encoding="utf-8",
        newline="\n",
    )
    log(
        "SEED 清单写出：seed_base=%d，交叉 %d 个 / 鲁棒底件 %d 个（逐手法 %d 个）/ 敏感 %d 个 / 前置 3 %d 个"
        "（用例名里带 seed，红了按各档那条 replay 重放；seed_roster.json 逐字节由参数决定）"
        % (
            args.seed_base,
            len(cross_seeds),
            nbases,
            len(row_seeds),
            len(mutate_seeds),
            len(anchor_seeds),
        )
    )


def write_illegal(out: pathlib.Path, log: Log) -> None:
    rows = illegal_value_report()
    path = out / "illegal_values.tsv"
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("层\t非法值\t判据内容\n")
        for a, b, c in rows:
            fh.write("%s\t%s\t%s\n" % (a, b, c))
    log(
        "非法值清单写出：%d 条（音频帧头层 %d / 子集层 %d / 视频层 %d）；块大小码 0b1111 → 32768 "
        "在帧头层是**合法**值，本清单不把它当坏值"
        % (
            len(rows),
            len(M.AUDIO_HEADER_ILLEGAL),
            len(M.AUDIO_SUBSET_ILLEGAL),
            len(rows) - len(M.AUDIO_HEADER_ILLEGAL) - len(M.AUDIO_SUBSET_ILLEGAL),
        )
    )


# ---------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(description="随机交叉语料档生成器（三档 + 三道前置）")
    ap.add_argument("--out", required=True, help="产物目录（不入库的临时目录）")
    grp = ap.add_mutually_exclusive_group(required=True)
    grp.add_argument(
        "--fuzz",
        action="store_true",
        help="畸形流鲁棒档：位翻转 / 截断 / 拼接 / 非法字段值 / varint 非最短形式",
    )
    grp.add_argument(
        "--mutate", action="store_true", help="坏期望三式：错一位系数 / 常数化 / 等于源"
    )
    grp.add_argument(
        "--cross",
        action="store_true",
        help="随机交叉档：编码参数组合 + 畸形域组合，绿向 + 红向",
    )
    grp.add_argument("--pre1", action="store_true", help="前置 1：判据锚点对账")
    grp.add_argument("--pre2", action="store_true", help="前置 2：参数覆盖对齐表")
    grp.add_argument("--pre3", action="store_true", help="前置 3：固定 seed 绿锚")
    ap.add_argument(
        "--rounds",
        type=int,
        default=12,
        help="交叉档 / 敏感档的轮数：直接按本参数取，不由预算倒推（日志只核对单价能容几轮）",
    )
    ap.add_argument(
        "--cases",
        type=int,
        default=120,
        help="鲁棒档目标条数：底件数取 ⌈cases/3⌉，每底件三条手法 ⇒ 实产 3×⌈cases/3⌉ 条"
        "（cases 为 3 的倍数时正好相等）；底件崩或容器读不出会削减，真实条数看汇总行 M=",
    )
    ap.add_argument(
        "--seed-base", type=int, default=20261007, help="各档种子派生的基数"
    )
    ap.add_argument(
        "--anchor-seeds",
        type=int,
        nargs="*",
        default=None,
        help="前置 3 用的固定 seed 列表（不给用内置的 %s）" % (PRE3_ANCHOR_SEEDS,),
    )
    ap.add_argument(
        "--budget-sec",
        type=float,
        default=GATE_DEFAULT_SEC,
        help="预算核对行里当门禁秒数用的预算值",
    )
    ap.add_argument("--timeout-sec", type=float, default=60.0, help="单子进程超时")
    ap.add_argument("--big-timeout", type=float, default=240.0, help="满档单例的超时")
    ap.add_argument(
        "--allow-big",
        action="store_true",
        help="放进 4112×4080 那种满档大例（长跑档与前置 1 用）",
    )
    ap.add_argument(
        "--repo",
        default=str(pathlib.Path(__file__).resolve().parent.parent),
        help="驱动件取公开头的仓库根",
    )
    ap.add_argument(
        "--lib", default=None, help="已建好的静态库文件路径（本件不建库，只链接）"
    )
    ap.add_argument(
        "--thirdparty-lib", default=None, help="第三方库目录（附加库从这里解析）"
    )
    ap.add_argument(
        "--extra-libs",
        dest="extra_libs",
        default="",
        help="逗号分隔的附加库名；不带 .lib 的自动补后缀，按 --thirdparty-lib 目录解析",
    )
    ap.add_argument("--msvc-root", default=None, help="MSVC 根（也可走环境变量）")
    ap.add_argument("--msvc-version", default=None, help="指定编译器版本目录名")
    ap.add_argument("--sdk-root", default=None, help="Windows SDK 根（也可走环境变量）")
    ap.add_argument("--sdk-ver", default=None, help="SDK 版本目录名（也可走环境变量）")
    ap.add_argument("--no-build", action="store_true", help="复用已编好的驱动件")
    args = ap.parse_args()
    # 要现编驱动件的模式才要库路径；--pre2 只出对齐表，--no-build 复用已编好的件，两样都可省
    if not args.pre2 and not args.no_build:
        if args.lib is None or args.thirdparty_lib is None:
            ap.error("必须给 --lib 与 --thirdparty-lib（已建好的库，本件不构建）")
    out = pathlib.Path(args.out)
    forbid_out_inside_repo(out, args.repo)
    out.mkdir(parents=True, exist_ok=True)
    with Log(out / "cross_run.log") as log:
        if args.no_build and not args.pre2 and not (out / DRIVER_EXE_NAME).is_file():
            # 起不来的那一档整档记红：逐例都会看到 rc=None，没有一例能被算成通过
            log(
                "BUILD 跳过且驱动件缺失（%s）：本档每一例都记红，不会有任何读数被算成通过"
                % DRIVER_EXE_NAME
            )
        return _dispatch(out, args, log)


def forbid_out_inside_repo(out: pathlib.Path, repo: str) -> None:
    """产物目录只准落不入库面：落在仓内时只放行 .gitignore 整目录排除的那棵工作树。

    判点用解析后的绝对路径比，不接受 .. 绕回仓内的写法；指进仓库根或任何入库面直接用法错误退出，
    免得语料、驱动件源码与日志被当成资产收进版本库。
    """
    resolved = out.resolve()
    root = pathlib.Path(repo).resolve()
    try:
        rel = resolved.relative_to(root)
    except ValueError:
        return  # 根本不在仓内：临时目录、盘外目录都放行
    parts = rel.parts
    if parts and parts[0] == "temp":
        return
    raise SystemExit(
        "产物目录 %s 落在仓库入库面里（仓库根 %s）：只准落仓外临时目录，或仓内被 .gitignore 整目录排除的工作树"
        % (resolved, root)
    )


def _dispatch(out: pathlib.Path, args, log) -> int:
    write_illegal(out, log)
    write_seeds(out, args, log)
    if not args.no_build:
        write_driver(out, args)
        if not build_driver(out, args, log):
            return 2
    if args.cross:
        rc = tier_cross(out, args, log)
    elif args.fuzz:
        rc = tier_fuzz(out, args, log)
    elif args.mutate:
        rc = tier_mutate(out, args, log)
    elif args.pre1:
        rc = pre1_anchor(out, args, log)
    elif args.pre2:
        rc = pre2_coverage(out, log)
    else:
        rc = pre3_green_anchor(out, args, log)
    return rc


if __name__ == "__main__":
    sys.exit(main())
