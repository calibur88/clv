"""CLV 门面基准序列生成器（1080p / 30 fps；节目是六段各 150 帧 = 30 s，另有段 7 一只作渐变面补材）。

设计前提：
  · 自持、可无限复现 —— 只用 numpy（仓库既有依赖），不读任何外部素材、不取网络。
  · 输入完全已知 —— 每段的像素值由本文件的公式给出，断言可以逐值确定。
  · **按段出**（`--segment`），一段 150 帧 = 466 560 000 B；节目六段合 2 799 360 000 B 不落盘，
    跑完一段删一段。这一条的约束在内存（一段 1080p 全摊进内存是个位数 GB 级）。

节目六段内容（帧号 i = 0..149，段内；段号从 1 起，全局帧号 = (段序 − 1) × 150 + i）：
  1  0–5 s    静态彩条，垂直七色（100 % 满幅 R/G/B 组合），bt.709 有限范围换算
  2  5–10 s   16 级阶梯灰阶，Y = lvl × 17（lvl = 0..15，每级宽 120 列）
  3  10–15 s  空间频率扫描，垂直正弦条纹，周期从 64 px 线性扫到 2 px，相位 0
  4  15–20 s  棋盘格，格宽 40 × 40 像素，两色 Y = 235 / 16
  5  20–25 s  运动方块，200 × 200 像素，每帧右移 20 像素（越界回卷），底 Y = 16
  6  25–30 s  综合测试图：彩条带 + 灰阶带 + 棋盘带 + 中心红方块

补材（不排进 30 s 节目，也不与音频对齐）：
  7  渐变面：对角空间坡 + 时域电平坡，全整数算式。节目六段里没有平滑渐变形态的内容，
     这一段补上它，好让「同一量化档在平滑内容上取到多少」有一类可复现的对照素材。

色彩标记口径（写进 provenance）：全片按 **bt.709 / 有限范围(tv)** 生成 ——
  Y ∈ [16, 235]、Cb / Cr ∈ [16, 240]，中间电平 128。取整一律 floor(x + 0.5)（四舍五入离零）。
  4:2:0 色度按段直接给值：段 1 按条取彩条定值，其余各段（含段 7）两平面恒 128；
  不做由亮度平均出色度的下采样。

音频（30 s 单声道，48 kHz / 16 bit）：
  0–10 s   1 kHz 正弦，幅度 50 % 满刻度
  10–15 s  静音
  15–20 s  白噪声（确定性 PRNG：splitmix64，种子写死），峰值 30 % 满刻度
  20–25 s  440 Hz 正弦，幅度 80 % 满刻度
  25–30 s  1 kHz 正弦，幅度 0 → 满刻度线性渐强

用法：
  python scripts/gen_synth_sequence.py --segment 3 --out <目录>       # 出一段的 raw + 该段 SHA-256
  python scripts/gen_synth_sequence.py --segment 3 --stream          # 帧流走 stdout、SHA-256 走 stderr，一字节不落盘
  python scripts/gen_synth_sequence.py --segment 7 --frames 48 --stream   # 只出段内前 N 帧（N 取 1..150，默认整段 150）
  python scripts/gen_synth_sequence.py --audio --out <目录>          # 出整条音频 wav
  python scripts/gen_synth_sequence.py --manifest                    # 只出各段参数表与色条定值（不出图）

段号与帧数都在建目录、开文件之前判，判不过按参数错误退出，不留半截文件。
"""

import argparse
import hashlib
import math
import os
import struct
import sys

import numpy as np

W, H = 1920, 1080
FPS = 30  # 裸帧流不携带速率；这个常量由 manifest() 打出来，节目时长也按它折算，不在两处手写秒数
SEG_FRAMES = 150  # 一段 5 s（= SEG_FRAMES / FPS）
SEGS = 7
FRAME_BYTES = W * H * 3 // 2
SEG_BYTES = FRAME_BYTES * SEG_FRAMES

# 音频
ARATE = 48000
ACH = 1  # 单声道（48 kHz / 1 ch 实测被 CLV 音频面接受，见 provenance 探针）
ABITS = 16
ASEC = 30
AFULL = 32767

# 彩条：七色 100 % 满幅 R/G/B，顺序与经典一致
BARS = [
    (255, 255, 255),  # 白
    (255, 255, 0),  # 黄
    (0, 255, 255),  # 青
    (0, 255, 0),  # 绿
    (255, 0, 255),  # 品
    (255, 0, 0),  # 红
    (0, 0, 255),  # 蓝
]

# bt.709 亮度系数（gamma 域加权，与 MPEG 系换算一致）
KR, KG, KB = 0.2126, 0.7152, 0.0722


def _r(x):
    """四舍五入离零：floor(x + 0.5)。"""
    return int(math.floor(x + 0.5))


def rgb2yuv(r, g, b):
    """有限范围 bt.709 换算。返回 (Y, Cb, Cr)，逐值可核。"""
    y_full = KR * r + KG * g + KB * b
    cb_full = (b - y_full) * 0.5 / (1.0 - KB)
    cr_full = (r - y_full) * 0.5 / (1.0 - KR)
    y = min(235, max(16, _r(16.0 + 219.0 * y_full / 255.0)))
    cb = min(240, max(16, _r(128.0 + 224.0 * cb_full / 255.0)))
    cr = min(240, max(16, _r(128.0 + 224.0 * cr_full / 255.0)))
    return y, cb, cr


BAR_TABLE = [
    (name, rgb, rgb2yuv(*rgb))
    for name, rgb in zip(
        ["white", "yellow", "cyan", "green", "magenta", "red", "blue"], BARS
    )
]


STATIC_SEGS = (1, 2, 3, 4, 6)
_static_planes = {}


def frame_yuv(seg, i):
    """返回一个 (Y, Cb, Cr) 三元组，元素是 uint8 ndarray。seg 从 1 起。

    与帧号无关的段（`STATIC_SEGS`）整段只算一次，同一段的后续帧复用同一份平面；
    复用方只读不写，所以拿到平面的调用不得就地改写它。
    """
    if seg in STATIC_SEGS:
        planes = _static_planes.get(seg)
        if planes is None:
            if seg == 1:
                planes = _bars()
            elif seg == 2:
                planes = _gray_steps()
            elif seg == 3:
                planes = _sweep()
            elif seg == 4:
                planes = _checker()
            else:
                planes = _composite()
            _static_planes[seg] = planes
        return planes
    if seg == 5:
        return _moving_block(i)
    if seg == 7:
        return _ramp(i)
    raise ValueError("segment out of range 1..%d, got %r" % (SEGS, seg))


def _bars():
    """垂直七色条：条号按 `col = x * 7 // W` 取，七段边界落在 x = 0, 275, 549, 823, 1098, 1372, 1646。

    1920 不整除 7（274.28…），七条宽度按边界差分别是 275 / 274 / 274 / 275 / 274 / 274 / 274，
    合计 1920：多出来的两列由 `x * 7 // 1920` 的整除点自然决定，落在第 1 与第 4 条上。
    """
    y = np.empty((H, W), dtype=np.uint8)
    cb = np.empty((H // 2, W // 2), dtype=np.uint8)
    cr = np.empty((H // 2, W // 2), dtype=np.uint8)
    ys = [b[2][0] for b in BAR_TABLE]
    cbs = [b[2][1] for b in BAR_TABLE]
    crs = [b[2][2] for b in BAR_TABLE]
    xs = np.arange(W)
    bar = (xs * 7) // W  # 每条实际宽 274 或 275 列
    y[:] = np.array(ys, dtype=np.uint8)[bar][None, :]
    xb = np.arange(W // 2)
    barb = (xb * 2 * 7) // W  # 色度列取该列左沿亮度所在条
    cb[:] = np.array(cbs, dtype=np.uint8)[barb][None, :]
    cr[:] = np.array(crs, dtype=np.uint8)[barb][None, :]
    return y, cb, cr


def _gray_steps():
    """16 级阶梯灰阶：Y = lvl × 17（lvl = 0..15 → 0, 17, …, 255），每级 120 列。
    彩条的 Y 域是 [16, 235]，本段按单内公式直给 lvl × 17，越出有限范围是刻意的（铺满 8-bit 全域）。"""
    lvl = (np.arange(W) * 16) // W
    y = np.array([l * 17 for l in lvl], dtype=np.uint8)
    y = np.repeat(y[None, :], H, axis=0)
    c = np.full((H // 2, W // 2), 128, dtype=np.uint8)
    return y, c, c.copy()


def _sweep():
    """空间频率扫描：垂直条纹，周期 T 沿 x 从 64 px 线性扫到 2 px（整段固定，与帧号无关），
    相位 0，Y = 128 + round(100 · sin(2π x / T))，夹到 [16, 235]。

    全件只有这里和音频的两条正弦支路走浮点：sin 的末位在不同指令集上不保证逐位一致，
    而取整用的 floor(x + 0.5) 会把落在半整数上的末位差放大成 1 字节差。本段与音频的哈希
    按同机同环境实测复现（连跑两遍逐字节一致），换机不做逐位承诺。
    """
    n = W - 1
    xs = np.arange(W, dtype=np.float64)
    period = 64.0 + (2.0 - 64.0) * (xs / n)  # x = 0 → 64 px，x = 1919 → 2 px
    val = 128.0 + 100.0 * np.sin(2.0 * math.pi * xs / period)
    yy = np.clip(np.floor(val + 0.5), 16, 235).astype(np.uint8)
    y = np.repeat(yy[None, :], H, axis=0)
    c = np.full((H // 2, W // 2), 128, dtype=np.uint8)
    return y, c, c.copy()


def _checker():
    """棋盘格 40 × 40：1080 / 40 = 27 格行、1920 / 40 = 48 格列，两色 Y = 235 / 16，色度 128。"""
    gy = (np.arange(H) // 40)[:, None]
    gx = (np.arange(W) // 40)[None, :]
    y = np.where(((gy + gx) & 1) == 0, 235, 16).astype(np.uint8)
    c = np.full((H // 2, W // 2), 128, dtype=np.uint8)
    return y, c, c.copy()


def _moving_block(i):
    """运动方块 200 × 200，底 Y = 16、块内 Y = 235；每帧右移 20 px，越界回卷：
    x0 = (20 · i) mod (1920 − 200) = (20 i) mod 1720，y0 = (1080 − 200) // 2 = 440。色度 128。
    """
    x0 = (20 * i) % (W - 200)
    y0 = (H - 200) // 2
    yy = np.full((H, W), 16, dtype=np.uint8)
    yy[y0 : y0 + 200, x0 : x0 + 200] = 235
    c = np.full((H // 2, W // 2), 128, dtype=np.uint8)
    return yy, c, c.copy()


def _composite():
    """综合测试图：三带 + 中心红方块。
    带 1 y ∈ [0, 270)   七色彩条（同段 1）
    带 2 y ∈ [270, 540)  16 级阶梯灰阶（同段 2）
    带 3 y ∈ [540, 1080) 40 × 40 棋盘（同段 4）
    中心红方块 200 × 200，左上 (860, 440)：R = 255 其余 0 ⇒ (Y, Cb, Cr) 取彩条里那条红的定值。"""
    y1, c1a, c1b = _bars()
    y2, c2a, c2b = _gray_steps()
    y3, c3a, c3b = _checker()
    y = np.empty((H, W), dtype=np.uint8)
    y[0:270] = y1[0:270]
    y[270:540] = y2[270:540]
    y[540:] = y3[540:]
    cb = np.empty((H // 2, W // 2), dtype=np.uint8)
    cr = np.empty((H // 2, W // 2), dtype=np.uint8)
    cb[0:135] = c1a[0:135]
    cb[135:270] = c2a[135:270]
    cb[270:] = c3a[270:]
    cr[0:135] = c1b[0:135]
    cr[135:270] = c2b[135:270]
    cr[270:] = c3b[270:]
    ry, rcb, rcr = BAR_TABLE[5][2]  # 红
    y[440:640, 860:1060] = ry
    cb[220:320, 430:530] = rcb
    cr[220:320, 430:530] = rcr
    return y, cb, cr


def _ramp(i):
    """渐变面：对角空间楼梯 + 整级均匀的时域推进，全整数算式，任何平台上逐值可复算。

    v(x, y) = (x + y) × 219 ÷ (W + H − 2)          ∈ [0, 219]（左上到右下的一条坡）
    t(i)    = i × 110 ÷ (SEG_FRAMES − 1)           ∈ [0, 110]（整段的时域推进，每帧 +0 或 +1，全帧同值）
    Y = 16 + v ÷ 2 + t（两次都是整除）              ∈ [16, 235]：16 在首帧左上取到、235 在末帧右下取到，不越有限范围
    色度两平面恒 128（本段只量亮度面的平滑内容）。

    时域项取整级均匀加、且加在整除之外：偏移一旦进被除数，整除就把它变成逐帧翻转的奇偶抖动
    （同一帧里一半像素 +1、另一半不动），量出来的是「坡 + 满幅 1 级 dither」而不是平滑渐变。

    帧号只在 0..SEG_FRAMES − 1 内有定义；再往上时域项一旦超过 110，亮度上界就被推出
    [16, 235]，转 8 位时静默回绕，所以下面在铺开平面之前先按时域项本身判住它。

    本段不排进 30 s 门面节目（节目仍是段 1..6），也不与音频对齐；它存在是因为节目六段里没有
    平滑渐变形态的内容，有了它才能对「同一量化档在平滑内容上取到多少」做可复现的对照。
    """
    t = (i * 110) // (SEG_FRAMES - 1)
    if t > 110:
        raise ValueError(
            "segment 7 frame index %r gives temporal term %d, above the 110 that keeps Y in [16, 235]"
            % (i, t)
        )
    xs = np.arange(W, dtype=np.int64)[None, :]
    ys = np.arange(H, dtype=np.int64)[:, None]
    v = ((xs + ys) * 219) // (W + H - 2)
    y = (16 + (v // 2) + t).astype(np.uint8)
    c = np.full((H // 2, W // 2), 128, dtype=np.uint8)
    return y, c, c.copy()


def pack_frame(y, cb, cr):
    return y.tobytes() + cb.tobytes() + cr.tobytes()


def write_segment(seg, out_dir, limit=None, to_stdout=False):
    """出一段到 <out_dir>/synth_seg<seg>.yuv；帧数取 limit（1..SEG_FRAMES），不给就是整段；
    to_stdout 时把帧流写到 stdout（给跑测架管道用）、SHA-256 与计数写到 stderr，一个字节不落盘。

    段号与帧数在建目录、开文件之前先判，判不过抛 ValueError：段号越界不会留下空文件，
    帧数 0 属于参数不成立、不等于「没给」（那由 limit=None 表示）。帧数上界一律取
    SEG_FRAMES：每段内容只定义了 SEG_FRAMES 帧，段 7 的时域项越界还会把亮度推出有限范围。
    """
    if not 1 <= seg <= SEGS:
        raise ValueError("segment out of range 1..%d, got %r" % (SEGS, seg))
    n_total = SEG_FRAMES if limit is None else limit
    if not 1 <= n_total <= SEG_FRAMES:
        raise ValueError(
            "frame count out of range 1..%d, got %r" % (SEG_FRAMES, n_total)
        )
    h = hashlib.sha256()
    path = None
    written = 0

    def pump(sink):
        nonlocal written
        for i in range(n_total):
            buf = pack_frame(*frame_yuv(seg, i))
            sink.write(buf)
            h.update(buf)
            written += 1

    # 文件走 with：写中途抛错也要关，别让落盘停在半开的句柄上；stdout 那条路不拥有它
    if to_stdout:
        pump(sys.stdout.buffer)
    else:
        os.makedirs(out_dir, exist_ok=True)
        path = os.path.join(out_dir, "synth_seg%d.yuv" % seg)
        with open(path, "wb") as f:
            pump(f)
    tail = "SEG %d frames=%d bytes=%d sha256=%s%s" % (
        seg,
        written,
        written * FRAME_BYTES,
        h.hexdigest(),
        "" if path is None else " path=%s" % path,
    )
    (print if not to_stdout else (lambda s: print(s, file=sys.stderr)))(tail)
    return path


def _splitmix64(x):
    x &= (1 << 64) - 1
    x = (x + 0x9E3779B97F4A7C15) & ((1 << 64) - 1)
    z = x
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & ((1 << 64) - 1)
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & ((1 << 64) - 1)
    return z ^ (z >> 31)


def build_audio():
    """30 s 单声道 16-bit，逐样本确定性生成（白噪声用 splitmix64，种子 0xC1F 写死）。

    白噪声那一支是全整数自算；两条正弦支路与段 3 的扫描同理，走浮点、按同机同环境复现。
    """
    n = ARATE * ASEC
    out = np.zeros(n, dtype=np.int16)
    t = np.arange(n, dtype=np.float64) / ARATE

    def sine(freq, amp, a, b):
        sl = slice(a, b)
        tt = t[sl]
        v = amp * AFULL * np.sin(2.0 * math.pi * freq * tt)
        out[sl] = np.clip(np.floor(v + 0.5), -AFULL, AFULL).astype(np.int16)

    seg = ARATE * 5
    sine(1000.0, 0.50, 0, 2 * seg)  # 0–10 s 1 kHz 50 %
    # 10–15 s 静音：out 已是 0
    a0, a1 = 3 * seg, 4 * seg  # 15–20 s 白噪声 30 %
    m = a1 - a0
    noise = np.empty(m, dtype=np.int16)
    for k in range(m):
        v = ((_splitmix64(0xC1F + k) >> 11) % (2 * AFULL + 1)) - AFULL
        noise[k] = _r(0.30 * v) if abs(v) > 1 else int(v)
    out[a0:a1] = noise
    sine(440.0, 0.80, 4 * seg, 5 * seg)  # 20–25 s 440 Hz 80 %
    a2, b2 = 5 * seg, n  # 25–30 s 1 kHz 0 → 满刻度渐强
    tt = t[a2:b2]
    ramp = np.arange(b2 - a2, dtype=np.float64) / (b2 - a2 - 1)
    v = AFULL * ramp * np.sin(2.0 * math.pi * 1000.0 * tt)
    out[a2:b2] = np.clip(np.floor(v + 0.5), -AFULL, AFULL).astype(np.int16)
    return out


def write_audio(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "synth_audio_48k_mono.wav")
    pcm = build_audio().tobytes()
    hdr = (
        b"RIFF"
        + struct.pack("<I", 36 + len(pcm))
        + b"WAVEfmt "
        + struct.pack(
            "<IHHIIHH",
            16,
            1,
            ACH,
            ARATE,
            ARATE * ACH * ABITS // 8,
            ACH * ABITS // 8,
            ABITS,
        )
        + b"data"
        + struct.pack("<I", len(pcm))
    )
    with open(path, "wb") as f:
        f.write(hdr + pcm)
    print(
        "AUDIO samples=%d rate=%d ch=%d bits=%d dur=%.3fs bytes=%d sha256=%s path=%s"
        % (
            len(pcm) // 2,
            ARATE,
            ACH,
            ABITS,
            len(pcm) / 2 / ARATE,
            len(hdr) + len(pcm),
            hashlib.sha256(hdr + pcm).hexdigest(),
            path,
        )
    )


def manifest():
    # 节目只排前六段（30 s）；段 7 是渐变面补材，不排进节目，所以两个总量分开报，
    # 别让"节目总字节"读起来像 35 s。秒数由帧数 ÷ FPS 现算，不手写；帧率也只有这一处读者。
    program_frames = SEG_FRAMES * (SEGS - 1)
    print(
        "W %d  H %d  FPS %d  SEG_FRAMES %d  FRAME_BYTES %d  SEG_BYTES %d"
        % (W, H, FPS, SEG_FRAMES, FRAME_BYTES, SEG_BYTES)
    )
    print(
        "PROGRAM_BYTES %d (%d segments x %d frames = %.1f s)  ALL_SEGS_BYTES %d (%d segments, including the segment 7 ramp)"
        % (
            SEG_BYTES * (SEGS - 1),
            SEGS - 1,
            SEG_FRAMES,
            program_frames / FPS,
            SEG_BYTES * SEGS,
            SEGS,
        )
    )
    print("BAR_TABLE (bt.709 tv, 100%% 满幅 RGB → Y/Cb/Cr):")
    for name, rgb, ycc in BAR_TABLE:
        print(
            "  %-8s RGB=%-11s Y=%3d Cb=%3d Cr=%3d"
            % (name, str(rgb), ycc[0], ycc[1], ycc[2])
        )
    print("SEG1 bars: bar(x) = x * 7 // 1920; 色度列 barb = x * 2 * 7 // 1920")
    print("SEG2 steps: lvl = x * 16 // 1920, Y = lvl * 17, chroma = 128")
    print(
        "SEG3 sweep : T(x) = 64 + (2 - 64) * x / 1919, Y = clip(128 + 100 sin(2 pi x / T), 16, 235), 相位 0"
    )
    print("SEG4 check : (x // 40 + y // 40) & 1 → Y = 235 / 16, chroma 128")
    print(
        "SEG5 block : x0 = (20 i) mod 1720, y0 = 440, 200x200 内 Y = 235 外 Y = 16, chroma 128"
    )
    print(
        "SEG6 comp  : 条带 [0,270) + 阶梯 [270,540) + 棋盘 [540,1080) + 红方块 y[440,640) x[860,1060)"
    )
    print(
        "SEG7 ramp  : v = (x + y) * 219 // (W + H - 2)，t = i * 110 // (SEG_FRAMES - 1)，"
        "Y = 16 + v // 2 + t，色度 128（渐变面，不在 30 s 节目内）"
    )
    print(
        "AUDIO      : 1k50%% | 静音 | 白噪 30%%(splitmix64 seed 0xC1F) | 440Hz 80%% | 1k 0→满刻度"
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--segment", type=int, help="which segment to emit (1..%d)" % SEGS)
    ap.add_argument("--audio", action="store_true")
    ap.add_argument("--manifest", action="store_true")
    ap.add_argument(
        "--stream",
        action="store_true",
        help="write the frame stream to stdout (pipe for the test harness), nothing on disk",
    )
    ap.add_argument(
        "--frames",
        type=int,
        help="emit only the first N frames of the segment (1..%d)" % SEG_FRAMES,
    )
    ap.add_argument("--out", default=".")
    a = ap.parse_args()
    if a.manifest:
        manifest()
        return 0
    if a.audio:
        write_audio(a.out)
        return 0
    if a.segment is None:
        ap.error("one of --segment N / --audio / --manifest is required")
    try:
        write_segment(a.segment, a.out, limit=a.frames, to_stdout=a.stream)
    except ValueError as exc:
        ap.error(str(exc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
