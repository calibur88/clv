#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""随机交叉语料档的独立模型层：照规范写的视频重建模型 + FLAC 帧头模型。

本件是「双刃对照」里的 Python 一侧，不参与被测：只拿规范里的算式与语法自己算一遍期望值。
变换 / 量化 / 反量化直接复用 gen_video_payload_fixtures.py 里那套按规范写的算式（正变换按
H X Hᵀ 的矩阵乘法直算、量化按电平集穷举取最近、载体按 1 位符号 + bits−1 位幅值定长直写），
本件补上它没有的反变换、四级饱和域、运动补偿、宏块头语法与 FLAC 帧头。

本件只做解码侧：期望值一律由读侧模型给出，载荷字节的产出归 gen_video_payload_fixtures.py
与语料生成件，这里不另起一条编码链（另起就有两份实现，改语法时只会对平自己）。

自检（无副作用，不写文件）：

    .venv/Scripts/python.exe scripts/cross_model.py --selfcheck

实现的规则内容与算式：

* 正变换 T = H X Hᵀ，H 是 8×8 Sylvester Hadamard（元素 ±1）、全程整数、不归一。
* 反变换 Y = (H F′ H) ÷ 64，输出舍入只有一条式：Y ≥ 0 时 floor(Y + 0.5)，否则 ceil(Y − 0.5)
  （四舍五入、半程离零；不是截断、不是就近偶）。Y 恒是「整数 ÷ 64」，64 = 2^6 精确可表示，
  故本件用整数式 (s + 32) // 64 与 −((−s + 32) // 64) 实现同一条式，自检里与浮点式逐点对拍。
* 电平集 {0} ∪ {±((2n+1)q' − δ)}，δ = 0（q' 奇）/ 1（q' 偶）；反量化 |F''| = (2|QF|+1)q' − δ。
* 有损流的两级量化偏移叠加 q' = clamp(q + d_vop + d_mb, 1, 2^quant_precision − 1)，上界再收口到 8 位载体。
* 量化步长 q 是三段域：0 = 无损 / 1..2^quant_precision − 1 = 有损 / 其余判帧头不成立。无损流里
  q' 恒为 0（帧级与宏块级偏移都不进算术），载体直载整数变换原值、反量化是恒等映射，
  位宽 1 位符号 + 14 位幅值，|系数| 上界就是第二级饱和域的 T_max 两档（帧内 8192 / 帧间 16320）。
* 每系数位宽 bits(q') = ceil(log2(QF_max(q') + 1)) + 1，QF_max = max over |T| ≤ T_max 的
  |QuantizeValue(T, q')|；T_max 帧内 8192 = 64 × 128、帧间 16320 = 64 × 255。两张有损表与无损
  那一行都从 src/video/block.cpp 现解析（件内不抄第二份）；末档起点覆盖有损段的整个可表达域，
  故位宽只归一下界。
* 四级饱和域：码流系数 |QF| ≤ QF_max(q')（越界判取值不成立）→ 反量化后系数按块类型夹两档
  （帧内 ±64 × 2^(bpp−1) = ±8192、帧间 ±64 × (2^bpp − 1) = ±16320，即 T_max 两档）
  → 反变换后残差夹 [−2^bpp, 2^bpp − 1] → 重建夹 [0, 2^bpp − 1]。
  系数溢出一律夹取不拒解；只有码流字段越界才拒。
* 半像素插值 b = (A+B+1)>>1、c = (A+C+1)>>1、d = (A+B+C+D+2)>>2；双向平均 P = (Pf+Pb+1)>>1；
  色度矢量 = round_half(v) = (v+1)>>1，与亮度同核、只是换算与块尺寸不同。
* 受限边界：取样坐标钳到最外像素（一圈 1 像素复制带的实现形式），对任意远坐标都有定义。
* 运动矢量预测 = 三候选（左 / 上 / 右上）逐分量取中值；无效候选（越界、缺位、邻居不带该方向）
  置零后仍参与中值；预测器状态跨宏块延续、在片界归零；B 侧前向 / 后向各一套、互不干涉。
* direct 缩放 MVF = (TRB × MV_ref) ÷ TRD、MVB = MVF − MV_ref：MV_ref 取**后向参考帧**同位宏块
  的矢量（槽位号给，取不到按零矢量），除法是**向零截断**的整数除法
  （矢量域不许用右移代替），乘除按 64 位有符号做，全式只舍入一次，结果落回 32 位有符号饱和域。
  TRB > TRD 判参数不成立、不做算术；TRD > 0 且 0 < TRB < TRD 是帧级判据，不满足即整帧跳过。
* 帧内块的预测块恒为常量 128，重建 d = clamp(128 + f, 0, 2^bpp − 1)；帧间 d = clamp(p + f, ...)。
  同位块矢量取不到的（前向参考是 I 帧 / 同位宏块是 intra / 同位宏块 skip / 缺位）一律按零矢量。
* 补边填黑：8-bit 有限范围 luma = 16、chroma = 128。
* 块模式位图：bit5..bit2 = 亮度块 0..3（左上起、行优先）、bit1 = Cb、bit0 = Cr；I / P 侧合成式
  (cbpy << 2) | cbpc，B 侧 cbpy 字段本身是 6 bit 全位图。位为 0 的块不占字节，
  单宏块块数据字节数 = 在场块数 × (64 × bits(q') ÷ 8)；位为 1 而 64 系数全零判取值不成立。
* 宏块头字段序：I / P = not_coded(1 bit) → mcbpc → cbpy → dquant(2 bit) →〔motion_vector()〕
  → block_size(varint) → 块数据；B = not_coded(8 bit) → mb_type(8 bit) → mvd[] → cbpy(varint)
  → block_size(varint) → 块数据。宏块级 dquant 的 2 bit 是二补码：00 = 0、01 = +1、10 = −2、11 = −1。
  not_coded = 1 在 I / P 侧跳过 mcbpc 及其后全部字段（I-VOP 没有 skip 语义，读到即码流不成立）；
  在 B 侧只决定接不接块数据，其余字段照旧在场。
* 帧间宏块的 motion_vector() = 两条 zigzag varint 残差，绝对矢量 = 预测值 + 残差（先在 64 位里
  定界再落回 32 位，越界取 INT32_MAX / INT32_MIN，不静默回绕）。帧内与 not_coded 宏块都不带它。
* 分片：按宏块边界切、每片从宏块边界起始；片数 ≤ 65 535；mb_run ≥ 1；单片帧不发片头；
  首片带完整 VOP 头。四条校验 = 片头分片数与声明对账、各片 mb_run 累加等于首片 mb_count、
  本片 first_mb 等于前片累加、全部片解完后字节整段吃光。

FLAC 帧头（位域布局按库内 src/audio/frame.h 的头注释；四张码表的**取值**在
src/audio/frame.cpp —— 采样率表在 RateFromCode，块大小 / 声道 / 位深三张表在 ParseFrameHead
里各自的分支，非法值只从表里逐条取）：

    byte0    同步码高 8 位 = 0xFF
    byte1    同步码低 6 位 + 保留位（必须 0）+ 块策略位（0 = 固定块，1 = 变长块）
    byte2    块大小码 4 位 / 采样率码 4 位
    byte3    声道码 4 位 / 位深码 3 位 / 保留位（必须 0）
    ...      扩展 UTF-8 编码号（最短形式：首字节的连续 1 个数 = 总字节数 1..7，
             值既不得越出该字节形态的上界，也不得落进更短一档的量程）
    [extra]  块大小码 0b0110 → 8 位、0b0111 → 16 位（存储值 + 1）；
             采样率码 0b1100 → 8 位（kHz）、0b1101 → 16 位（Hz）、0b1110 → 16 位（Hz ÷ 10）
    byte     帧头 CRC-8：多项式 0x07、初值 0、不反射、无收尾异或，覆盖帧头前置字节
    帧尾     CRC-16：多项式 0x8005、初值 0、不反射、无收尾异或，大端 2 字节，覆盖整帧除自身

* 块大小码是**块大小码表**：0b0000 保留、0b0001 = 192、0b0010..0b0101 = 144 × 2^v、
  0b0110 / 0b0111 = 非常规块大小（存储值 + 1）、0b1000..0b1111 = 2^v（v 就是那 4 位码值）
  ⇒ 256 / 512 / 1024 / 2048 / 4096 / 8192 / 16384 / **32768**。
  **0b1111 → 32768 在帧头层是合法值**，它只是越流式子集，不是坏码。
* 采样率码是**采样率码表**：0b0000 = 只在流信息里给、0b0001..0b1011 定值、0b1100..0b1110 走 extra、
  **0b1111 才是 Forbidden**——禁用位落在采样率表，与块大小表无关。
* 位深码：0b000 = 只在流信息里给、0b001 = 8、0b010 = 12、**0b011 = Reserved**、0b100 = 16、
  0b101 = 20、0b110 = 24、0b111 = 32。
* 声道码：0b0000..0b0111 = 1..8 声道、0b1000..0b1010 = 三种 2 声道去相关、
  **0b1011..0b1111 = Reserved**。
* 帧尾 CRC-16 / 帧头 CRC-8 是两条独立校验；越出子集（块大小 > 16384，或采样率 ≤ 48000 时
  块大小 > 4608）属子集层判定，与帧头层的保留 / 禁用码分属两层。
"""

from __future__ import annotations

import argparse
import math
import pathlib
import re
import sys
from dataclasses import dataclass, field

# 变换 / 量化 / 载体那套算式复用仓库里已有的规范模型件，不另起炉灶。本件与它同目录：直接运行
# 本件、或从同目录的脚本 import 本件，解释器都已把该目录放进 sys.path，故不在导入时改宿主的模块
# 搜索路径（insert(0) 会让同名件被本目录顶掉，波及只是 import 本件的其它脚本）。
# 位宽档表不在本件手抄：随那份件一起从 src/video/block.cpp 现解析（见 width_tables）。
from gen_video_payload_fixtures import (
    CONST_SOURCES,
    INTRA_PRED,
    T_MAX_INTER,
    T_MAX_INTRA,
    constant_resolver,
    dequantize_value,
    hadamard8,
    quantize_exhaustive,
    quantize_value,
    read_width_tables,
)


def configure_streams() -> None:
    """把 stdout / stderr 钉成 UTF-8，只由本件的入口调用。

    自检与报告里带 ± / ÷ / ≤ 这类非 ASCII 字符，Windows 控制台默认码页（GBK）会在 print 处抛
    UnicodeEncodeError，整条自检跑不完，所以要与码页设置脱钩。写在导入路径上会把「只是
    import 本件」的宿主进程的两条流一并改掉，故交回入口自己决定。
    """
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")


# ---------------------------------------------------------------- 常量

BLOCK_COEFS = 64
BLOCKS_PER_MB = 6
MB_SIZE = 16
MAX_FRAGMENT_COUNT = 65535

BLACK_LUMA = 16  # 补边填黑：8-bit 有限范围的 luma 黑电平
BLACK_CHROMA = 128  # 同一条的 chroma 黑电平

# 四级饱和域中后三级（bpp = 8）：反量化后系数、反变换后残差、重建样本。
# 第二级按块类型取两档，取值即 gen_video_payload_fixtures 的 T_MAX_*（正变换不归一，
# 块和上界 = 系数个数 × 变换输入幅度上界）：帧内 64 × 128 = 8192、帧间 64 × 255 = 16320。
COEF_LO_INTRA, COEF_HI_INTRA = -T_MAX_INTRA, T_MAX_INTRA
COEF_LO_INTER, COEF_HI_INTER = -T_MAX_INTER, T_MAX_INTER
RES_LO, RES_HI = -(2**8), 2**8 - 1
SAMPLE_LO, SAMPLE_HI = 0, 2**8 - 1

INT32_MIN, INT32_MAX = -(2**31), 2**31 - 1

VOP_DQUANT_MIN, VOP_DQUANT_MAX = -16, 15  # 帧级 dquant 的 5 bit 带符号域
MB_DQUANT_TABLE = {0: 0, 1: 1, 2: -2, 3: -1}  # 宏块级 2 bit 二补码

QUANT_PRECISION_MIN, QUANT_PRECISION_MAX = 3, 9

VOP_FIXED_BYTES = 6  # VOP 头定长部分（不含 mb_count 的 varint）
SLICE_HEAD_PREFIX_BYTES = (
    8
    # 片头定长前缀，首片与后续片共用同一段：frame_id 4 + fragment_count 2 + flags 1 + vop_type 1
)
SLICE_FIXED_WITH_QUANT = 9  # 首片 = 上面那段前缀再加一枚 vop_quant

# ---------------------------------------------------------------- 量化步长与位宽

# 量化步长的三段域（依据 src/video/quant.h 的 kQuantLossless 与 QuantInStreamDomain，
# 以及 src/video/headers.h 帧头注里 vop_quant 那一行的分段写法）：
# 0 = 无损 / 1..QuantCeiling = 有损 / 其余不成立。0 不是「未声明」，它自己就是一档模式。
QUANT_LOSSLESS = 0

# 无损档的载体直载整数变换原值：位宽 = 1 位符号 + ⌈log2(上界 + 1)⌉ 位幅值，两档同宽。
# 上界即第二级饱和域的 T_max 两档（帧内 64 × 128 = 8192、帧间 64 × 255 = 16320），
# 幅值位数取 16320 与 8192 共同的 ⌈log2(界 + 1)⌉ = 14，不另立字面量。
# 这两格是本件按算式给的期望值，取档实际走解析出来的无损行（自检逐点对拍二者同值）。
LOSSLESS_MAG_BITS = T_MAX_INTER.bit_length()  # 14
LOSSLESS_BITS = 1 + LOSSLESS_MAG_BITS  # 15


def quant_ceiling(quant_precision: int) -> int:
    """q / q' 的上界 = 2^quant_precision − 1：入参先夹进声明域 [3, 9]，再收口到 8 位载体。"""
    p = max(QUANT_PRECISION_MIN, min(QUANT_PRECISION_MAX, quant_precision))
    return min(255, (1 << p) - 1)


def quant_in_stream_domain(q: int, quant_precision: int) -> bool:
    """码流里量化步长的三段域判据（帧头与片头共用，与库侧同名判据同式）。"""
    return q == QUANT_LOSSLESS or (1 <= q <= quant_ceiling(quant_precision))


def effective_quant(q: int, d_vop: int, d_mb: int, quant_precision: int) -> int:
    """本片段生效的量化步长。

    无损流（q = 0）原样给出 0：量化档、帧级偏移、宏块级偏移只在有损式里才有意义，
    无损流里三者都不参与算术，夹取与偏移一律不走（把偏移折进 0 就会把无损流按最细有损档解）。
    有损流才做两级偏移叠加：q' = clamp(q + d_vop + d_mb, 1, 2^quant_precision − 1)。
    """
    if q == QUANT_LOSSLESS:
        return QUANT_LOSSLESS
    return max(1, min(quant_ceiling(quant_precision), q + d_vop + d_mb))


def coded_size(orig: int) -> int:
    """补后尺寸 = 原始尺寸向上取整到 16 的倍数（原始 65 521..65 535 时补后 65 536，需 32 位容器）。"""
    return (orig + 15) & ~15


_WIDTH_TABLES = None


def width_tables():
    """两张有损档表与无损那一档都从 src/video/block.cpp 现解析，件内不留手抄副本。

    位宽与 |QF| 上界是一档一个值的格式事实，权威在库侧那三张表；本件抄一份就会随库侧改档落后，
    而落后之后的错位只会让「位宽与算式自洽」那类自检变成对抄件自证。解析放在首次取用时做：
    import 本件不做文件 I/O，引用本件的脚本各自有自己的前置。
    """
    global _WIDTH_TABLES
    if _WIDTH_TABLES is None:
        _WIDTH_TABLES = read_width_tables(constant_resolver(CONST_SOURCES))
    return _WIDTH_TABLES


def bits_per_coef(q: int, intra: bool) -> int:
    """每系数位宽（含 1 位符号）：q = 0 取无损那一档，q ≥ 1 取起点不超过它的最后一档。"""
    return width_tables().bits_of(q, intra)


def qf_window(q: int, intra: bool) -> int:
    """|码流系数| 的窗口上界：有损档取该档的 QF_max，无损档取变换原值的界 T_max。"""
    return width_tables().row_of(q, intra).qf_max


def block_bytes(q: int, intra: bool) -> int:
    """单块字节数 = 64 × bits(q') ÷ 8，恒为整字节（只属于定长档那一法）。"""
    return BLOCK_COEFS * bits_per_coef(q, intra) // 8


def dequantize_coef(v: int, q: int) -> int:
    """反量化：有损档按电平集取档，无损档（q = 0）是恒等映射 —— 载体里装的就是变换原值。

    电平集式对 q = 0 没有定义（(2|QF|+1) × 0 − δ 会给出 −δ），所以无损支必须绕开电平集，
    与库侧 DequantizeValue 的恒等分支同式。
    """
    return v if q == QUANT_LOSSLESS else dequantize_value(v, q)


# ---------------------------------------------------------------- 系数编码法（vop_flags bit5-7）
#
# 0 = 定长直写，1 = EOB 截尾 + Exp-Golomb(order-0) 幅值；2..7 未赋法，帧头即判不成立。
# 本区两法的字段与算式与库内那一份实现逐条同构：`all_zero` 1 bit、`eob` 6 bit、幅值走 Exp-Golomb order-0、
# 非零幅值再跟 1 位符号；扫描序取自然行序（不引入 zigzag），故 `eob` 就是该顺序里的下标。

COEF_FIXED = 0
COEF_EOB_EG = 1
COEF_CODING_MAX = 1

EOB_FIELD_BITS = 6  # eob 恰好铺满下标 0..63 ⇒ 越界在字段宽度上不可能出现
EG_MAX_PREFIX = 24  # 前导零到 24 时值已近 2^24，远超任一档 QF_max；到顶即判读不尽


def eg_bits(v: int) -> int:
    """Exp-Golomb order-0 的码字长 = 2L + 1，L = floor(log2(v + 1))。无参数、无表、自分隔。"""
    x, L = v + 1, 0
    while x > 1:
        x >>= 1
        L += 1
    return 2 * L + 1


def write_eg(bw: "BitWriter", v: int) -> None:
    x = v + 1
    L = x.bit_length() - 1
    bw.write(0, L)
    bw.write(1, 1)
    if L:
        bw.write(x & ((1 << L) - 1), L)


def read_eg(br: BitReader) -> int:
    L = 0
    while True:
        if br.read_bit():
            break
        if L >= EG_MAX_PREFIX:
            raise ModelError(E_PAYLOAD_TOO_SMALL, f"EG 前导零越过上限 {EG_MAX_PREFIX}")
        L += 1
    if L == 0:
        return 0
    return ((1 << L) | br.read(L)) - 1


def block_bits_max(q: int, intra: bool, coding: int) -> int:
    """单块最坏比特数：法 1 按满幅系数算（EG(QF_max) 比定宽 bits 还长），法 0 就是定宽。"""
    if coding == COEF_FIXED:
        return BLOCK_COEFS * bits_per_coef(q, intra)
    return 1 + EOB_FIELD_BITS + BLOCK_COEFS * (eg_bits(qf_window(q, intra)) + 1)


def block_bytes_max(q: int, intra: bool, coding: int) -> int:
    return (block_bits_max(q, intra, coding) + 7) // 8


def present_block_count(pattern: int) -> int:
    return bin(pattern & 0x3F).count("1")


# ---------------------------------------------------------------- 判定档位名
#
# 名字取自规范里的处置口径，只列本件（读侧模型）自己会产的档：自检按标签断言判点，
# 语料侧只看红绿、不按标签取档。编码侧的三档（声明侧量化越界、逐调用侧量化越界、载荷长度不符）
# 本件不产，号位只在下面那张码表里登记，这里不设标签 —— 留一个没有产出点的标签，
# 断言就会对着一个永不成立的分支空转。

E_VALUE_RANGE = "VALUE_RANGE"
E_PAYLOAD_TOO_SMALL = "PAYLOAD_TOO_SMALL"
E_HEADER_BAD = "HEADER_BAD"
E_SLICE_HEADER_BAD = "SLICE_HEADER_BAD"
E_SLICE_SEQUENCE = "SLICE_SEQUENCE"
E_TOOL_EXCLUDED = "TOOL_EXCLUDED"
E_INVALID_ARGUMENT = "INVALID_ARGUMENT"

# 手抄的码名 → 码值表：本件与语料对照件都按名字取档，值必须与 include/CLV_Video.h 逐格相同。
# 抄件不会随公开头的退场或改值自己同步，公开头改了而本表没跟上就会整体错位，而错位会让「查得到名字就算合法」
# 那一类判据恒真、判据空转 ⇒ 对平不靠人记，由 video_error_code_diff 现算公开头再比（自检逐条跑）。
# 本表与公开头分处两个文件、没有任何机制把它们绑在一起动：公开头在段尾追加、号位留白或改值时，
# 本表必须跟着改同一格，两表各自被不同件读走（本件按名取值、语料对照件按值查名），
# 只改一侧就是让另一侧的判据对着一个不存在的号位空转。
VIDEO_ERROR_CODES = {
    "OK": 0,
    "INVALID_HANDLE": 64,
    "INVALID_ARGUMENT": 65,
    "VALUE_RANGE": 66,
    "PAYLOAD_TOO_SMALL": 67,
    "HEADER_BAD": 68,
    "QUANT_OUT_OF_RANGE": 69,
    "BIT_DEPTH_UNSUPPORTED": 70,
    "TOOL_EXCLUDED": 71,
    "TOOL_LIST_ABSENT": 72,
    "SLICE_HEADER_BAD": 74,
    "SLICE_SEQUENCE": 75,
    "REF_FRAME_MISSING": 76,
    "OUTPUT_TOO_SMALL": 77,
    "STATE_ERROR": 79,
    "OUT_OF_MEMORY": 80,
    "STREAM_REJECTED": 83,
    "QUANT_OUT_OF_RANGE_CALL": 84,
    "PAYLOAD_LENGTH_MISMATCH": 85,
    # 73 / 78 / 81 / 82 是留白号位，公开头里同样没有这些成员，本表因此不列：
    # 鲁棒档的 `in_enum` 判据靠这几格查不到名字来咬住错值，列了就等于判据空转。
    # 段尾追加的新码只改本表最后一格；号位与顺序以公开头为准，本表不自行排号。
}

# 公开头相对本件的位置：本件在 scripts/，头在同级仓库根的 include/。
VIDEO_ERROR_HEADER_REL = pathlib.PurePosixPath("include", "CLV_Video.h")

_ENUM_MEMBER_RE = re.compile(r"CLV_VIDEO_([A-Z0-9_]+)(?:\s*=\s*([0-9]+))?")


def video_error_header_path() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parent.parent / VIDEO_ERROR_HEADER_REL


def strip_c_comments(text: str) -> str:
    """剥掉块注释与行注释，注释所在行留出空白（行结构不塌）。"""
    text = re.sub(r"/\*.*?\*/", lambda m: " " * len(m.group()), text, flags=re.S)
    return re.sub(r"//[^\n]*", lambda m: " " * len(m.group()), text)


def parse_video_error_codes(header_path: pathlib.Path) -> dict[str, int]:
    """从公开头独立解析 CLV_VideoError 的「名 → 值」。

    隐式顺排与显式给值两式都按 C 的枚举规则现算，不抄头里的注释数字：注释里满是
    「73 是留白号位」这类号位叙述，那些数字不能被当成成员值。
    顺序必须是**先整体剥注释、再切枚举体**：号位叙述与合法集写法（`{8, 10, 12}` 这一类花括号）
    都写在注释里，先切后剥会让注释中的右花括号把枚举体截短、尾部成员无声消失，差集只剩下
    「抄件多写了几格、头里查不到」这种把人往反方向带的读数。成员名与 `= 值` 允许折行（正则按
    空白跨行匹配），显式给值与顺排两式同一条走。
    剥完注释后的第一个右花括号就是本枚举的终点，其后必须紧跟 typedef 名 —— 截断要响，
    不能只让差集去猜。
    """
    text = strip_c_comments(header_path.read_text(encoding="utf-8"))
    marker = "typedef enum CLV_VideoError"
    if marker not in text:
        raise SystemExit(f"公开头里找不到枚举起点：{header_path}")
    body, _, tail = text.split(marker, 1)[1].partition("}")
    if not re.match(r"\s*CLV_VideoError\b", tail):
        raise SystemExit(
            f"公开头的枚举体没有以 CLV_VideoError 收尾（枚举体被截短，终点花括号不在位）：{header_path}"
        )
    codes: dict[str, int] = {}
    value = -1
    for name, explicit in _ENUM_MEMBER_RE.findall(body):
        value = int(explicit) if explicit else value + 1
        codes[name] = value
    if not codes:
        raise SystemExit(f"公开头枚举体解析到零个成员：{header_path}")
    return codes


def video_error_code_diff(
    manual: dict[str, int] | None = None, header_path: pathlib.Path | None = None
) -> tuple[list, list]:
    """双向差集：(手抄表独有或值不符的名, 公开头独有或值不符的名)。两列都空才算对平。

    「名同值异」两列都进，这样改值与增删号位落在哪一侧都看得见；只比对键集合会把
    错位（全体顺移一格）读成对平。
    """
    manual = VIDEO_ERROR_CODES if manual is None else manual
    parsed = parse_video_error_codes(
        header_path if header_path is not None else video_error_header_path()
    )
    only_manual = [
        f"{name}={value}(头={parsed.get(name)})"
        for name, value in manual.items()
        if parsed.get(name) != value
    ]
    only_header = [
        f"{name}={value}(抄={manual.get(name)})"
        for name, value in parsed.items()
        if manual.get(name) != value
    ]
    return only_manual, only_header


class ModelError(Exception):
    def __init__(self, code: str, detail: str = ""):
        super().__init__(f"{code}: {detail}")
        self.code = code
        self.detail = detail


# ---------------------------------------------------------------- 位流与 varint


def shortest_varint_len(value: int) -> int:
    n, v = 1, value
    while v >= 0x80:
        v >>= 7
        n += 1
    return n


def encode_varint(value: int) -> bytes:
    out = bytearray()
    while True:
        if value < 0x80:
            out.append(value)
            return bytes(out)
        out.append((value & 0x7F) | 0x80)
        value >>= 7


def encode_zigzag(value: int) -> bytes:
    """(n << 1) ^ (n >> 63)，n 按有符号 64 位；线上存的是 zigzag 后的无符号值。"""
    return encode_varint(((value << 1) ^ (value >> 63)) & ((1 << 64) - 1))


class BitReader:
    """MSB 先出的位读件：变长码、块数据、varint 都从同一条位轴上取。

    end_bit 给读件封口（块数据的读界 = 宏块头的 block_size，与库侧「块数据是一段独立字节」同构）；
    不给就是读到载荷末尾。封口之后 bits_left 与越界判定都只算到封口位。
    """

    __slots__ = ("data", "pos", "end")

    def __init__(self, data: bytes, pos_bit: int = 0, end_bit: int = None):
        self.data = data
        self.pos = pos_bit
        cap = len(data) * 8
        self.end = cap if end_bit is None else min(end_bit, cap)

    def bits_left(self) -> int:
        return self.end - self.pos

    def read_bit(self) -> int:
        if self.pos >= self.end:
            raise ModelError(E_PAYLOAD_TOO_SMALL, "位读越出载荷")
        b = (self.data[self.pos >> 3] >> (7 - (self.pos & 7))) & 1
        self.pos += 1
        return b

    def read(self, n: int) -> int:
        v = 0
        for _ in range(n):
            v = (v << 1) | self.read_bit()
        return v

    def read_varint(self) -> int:
        """无符号 LEB128：每字节低 7 位数据、高 1 位续接；非最短形式按码流不成立。"""
        scratch = []
        for _ in range(10):
            scratch.append(self.read(8))
            if (scratch[-1] & 0x80) == 0:
                v = 0
                for i, b in enumerate(scratch):
                    v |= (b & 0x7F) << (7 * i)
                if v >= 1 << 64:
                    raise ModelError(E_HEADER_BAD, "varint 值越出 64 位量程")
                if len(scratch) != shortest_varint_len(v):
                    raise ModelError(
                        E_HEADER_BAD, f"varint 非最短形式（{bytes(scratch).hex()}）"
                    )
                return v
        raise ModelError(E_HEADER_BAD, "varint 越过 10 字节仍无终止位")

    def read_zigzag(self) -> int:
        raw = self.read_varint()
        return (raw >> 1) ^ -(raw & 1)

    def skip_bytes(self, n: int) -> None:
        if self.bits_left() < n * 8:
            raise ModelError(E_PAYLOAD_TOO_SMALL, "块数据越出载荷")
        self.pos += n * 8


class BitWriter:
    """与 BitReader 同轴的写件（自检那两组载体往返用例用）。"""

    def __init__(self):
        self.bits: list[int] = []

    def write(self, value: int, n: int) -> None:
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def write_bytes(self, data) -> None:
        for b in data:
            self.write(b, 8)

    def write_varint(self, value: int) -> None:
        self.write_bytes(encode_varint(value))

    def align(self) -> None:
        while len(self.bits) % 8:
            self.bits.append(0)

    def to_bytes(self) -> bytes:
        self.align()
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            v = 0
            for b in self.bits[i : i + 8]:
                v = (v << 1) | b
            out.append(v)
        return bytes(out)


# ---------------------------------------------------------------- 宏块头码表
#
# mcbpc 一行同时给「宏块类型 + 色度块模式 cbpc」，底本是库内 src/video/annex_b_tables_p.h 那两张表裁前的
# I-VOP / P-VOP 两张表**删去全部 +q 行**（CLV 的宏块头无条件带 2 bit 量化偏移，+q 与不带 q
# 的差别不复存在，留着只会让同一宏块有两种写法）。mb_type = 2（inter4v）与 Stuffing 码字
# 000000001 留在表里专供识别成「被排除的工具」，表外码字判另一档错，两条失败路径不得混。
# cbpc 列按位图合成位序读（bit1 = Cb、bit0 = Cr、00 = 两色度块都不在场），不沿用底本原列的
# 4:2:0 色度块模式码语义（原列 00 = 两块都在场、11 = 无）。
# cbpy 表 = 同一批码表里的「四块全不透明」一张：帧内宏块查 intra 列、帧间宏块查 inter 列，
# 两列逐行互为按位取反。码字按 MSB 先出匹配，前缀自由、命中即定，不需要回溯。

MB_TYPE_INTER = 0
MB_TYPE_INTER4V = 2
MB_TYPE_INTRA = 3
MB_TYPE_STUFFING = -1

VOP_I, VOP_P, VOP_B = 0, 1, 2

# (码长, 码字, mb_type, cbpc)
MCBPC_I_VOP = [
    (1, 0x001, MB_TYPE_INTRA, 0),
    (3, 0x001, MB_TYPE_INTRA, 1),
    (3, 0x002, MB_TYPE_INTRA, 2),
    (3, 0x003, MB_TYPE_INTRA, 3),
    (9, 0x001, MB_TYPE_STUFFING, 0),
]
MCBPC_P_VOP = [
    (1, 0x001, MB_TYPE_INTER, 0),
    (4, 0x003, MB_TYPE_INTER, 1),
    (4, 0x002, MB_TYPE_INTER, 2),
    (6, 0x005, MB_TYPE_INTER, 3),
    (3, 0x002, MB_TYPE_INTER4V, 0),
    (7, 0x005, MB_TYPE_INTER4V, 1),
    (7, 0x004, MB_TYPE_INTER4V, 2),
    (8, 0x005, MB_TYPE_INTER4V, 3),
    (5, 0x003, MB_TYPE_INTRA, 0),
    (8, 0x004, MB_TYPE_INTRA, 1),
    (8, 0x003, MB_TYPE_INTRA, 2),
    (7, 0x003, MB_TYPE_INTRA, 3),
    (9, 0x001, MB_TYPE_STUFFING, 0),
]
# 底本里被删掉的 +q 行：不属于 CLV 码流语法，逐个喂回解码侧必须判表外码字，不得静默命中别的码。
MCBPC_I_Q_ROWS = [
    (4, 0x001, 4, 0),
    (6, 0x001, 4, 1),
    (6, 0x002, 4, 2),
    (6, 0x003, 4, 3),
]
MCBPC_P_Q_ROWS = [
    (3, 0x003, 1, 0),
    (7, 0x007, 1, 1),
    (7, 0x006, 1, 2),
    (9, 0x005, 1, 3),
    (6, 0x004, 4, 0),
    (9, 0x004, 4, 1),
    (9, 0x003, 4, 2),
    (9, 0x002, 4, 3),
]

# (码长, 码字, 帧内块模式, 帧间块模式)
CBPY_TABLE = [
    (4, 0x003, 0x0, 0xF),
    (5, 0x005, 0x1, 0xE),
    (5, 0x004, 0x2, 0xD),
    (4, 0x009, 0x3, 0xC),
    (5, 0x003, 0x4, 0xB),
    (4, 0x007, 0x5, 0xA),
    (6, 0x002, 0x6, 0x9),
    (4, 0x00B, 0x7, 0x8),
    (5, 0x002, 0x8, 0x7),
    (6, 0x003, 0x9, 0x6),
    (4, 0x005, 0xA, 0x5),
    (4, 0x00A, 0xB, 0x4),
    (4, 0x004, 0xC, 0x3),
    (4, 0x008, 0xD, 0x2),
    (4, 0x006, 0xE, 0x1),
    (2, 0x003, 0xF, 0x0),
]

MCBPC_MAX_BITS = 9
CBPY_MAX_BITS = 6

BVOP_DIRECT, BVOP_INTERPOLATE, BVOP_BACKWARD, BVOP_FORWARD = 0, 1, 2, 3
BVOP_MB_TYPE_MAX = 3


def bvop_mvd_count(mb_type: int) -> int:
    return {
        BVOP_DIRECT: 0,
        BVOP_INTERPOLATE: 2,
        BVOP_BACKWARD: 1,
        BVOP_FORWARD: 1,
    }.get(mb_type, 0)


def _find_mcbpc(vop_type: int, length: int, value: int):
    table = MCBPC_I_VOP if vop_type == VOP_I else MCBPC_P_VOP
    for row in table:
        if row[0] == length and row[1] == value:
            return row
    return None


def _find_cbpy(length: int, value: int):
    for row in CBPY_TABLE:
        if row[0] == length and row[1] == value:
            return row
    return None


def find_cbpy_row(intra: bool, pattern: int):
    for row in CBPY_TABLE:
        if (row[2] if intra else row[3]) == pattern:
            return row
    return None


def find_mcbpc_row(vop_type: int, mb_type: int, cbpc: int):
    table = MCBPC_I_VOP if vop_type == VOP_I else MCBPC_P_VOP
    for row in table:
        if row[2] == mb_type and row[3] == cbpc:
            return row
    return None


def read_vlc(br: BitReader, max_bits: int, finder) -> tuple:
    value = 0
    for length in range(1, max_bits + 1):
        value = (value << 1) | br.read_bit()
        hit = finder(length, value)
        if hit is not None:
            return hit
    raise ModelError(E_HEADER_BAD, f"{max_bits} 位内未命中码表（表外码字）")


# ---------------------------------------------------------------- 反变换与重建


def hadamard_matrix() -> list[list[int]]:
    """H[i][j] = (−1)^popcount(i & j)：8×8 Sylvester Hadamard，元素 ±1。"""
    return [[1 - 2 * ((i & j).bit_count() & 1) for j in range(8)] for i in range(8)]


def inverse_hadamard8_int(f2: list[int]) -> list[int]:
    """反变换的整数分子 S = H F′ Hᵀ（Y = S ÷ 64 才是规范说的那一步）。

    与正变换同一张 H、同一种写法（矩阵乘法直算，不用蝶形 / 原位 fast WHT）——
    编解两侧「同式」这条硬约束在模型里也只有一个来源。
    """
    h = hadamard_matrix()
    fm = [f2[r * 8 : r * 8 + 8] for r in range(8)]
    a = [
        [sum(h[i][k] * fm[k][j] for k in range(8)) for j in range(8)] for i in range(8)
    ]
    s = [[sum(a[i][k] * h[j][k] for k in range(8)) for j in range(8)] for i in range(8)]
    return [s[r][c] for r in range(8) for c in range(8)]


def round_residual_int(s: int) -> int:
    """规范唯一那条舍入式的整数形态：Y = s ÷ 64，Y ≥ 0 → floor(Y + 0.5)，否则 ceil(Y − 0.5)。

    64 = 2^6 ⇒ 与 (s + 32) // 64 与 −((−s + 32) // 64) 逐点同值（自检里与浮点式对拍）。
    """
    if s >= 0:
        return (s + 32) // 64
    return -((-s + 32) // 64)


def round_residual_float(y: float) -> int:
    """规范原式（浮点形态），只用于与整数式逐点对拍。"""
    return math.floor(y + 0.5) if y >= 0 else math.ceil(y - 0.5)


def clamp(value: int, lo: int, hi: int) -> int:
    return lo if value < lo else (hi if value > hi else value)


def read_present_blocks(
    br: BitReader, pattern: int, q: int, intra: bool, coding: int
) -> list[list[int]]:
    """按位图依次读在场块（块序 = 位序 bit5 → bit0），返回 6 块系数（不在场的块全零）。

    法 0 = 定长直写，零照样占位；法 1 = all_zero / eob / EG 幅值 + 符号，块边界自终止。
    两法的读界都由调用方封在 br 里（= 宏块头的 block_size），本法解完后剩余不足一字节。
    """
    cap = qf_window(q, intra)
    rows: list[list[int]] = [[0] * BLOCK_COEFS for _ in range(BLOCKS_PER_MB)]
    if coding == COEF_EOB_EG:
        for bit in range(5, -1, -1):
            if (pattern & (1 << bit)) == 0:
                continue
            idx = 5 - bit
            if br.read(1):
                # 本法把「在场且整块全零」写成合法码字，解回来是零系数而不是失败
                continue
            eob = br.read(EOB_FIELD_BITS)
            for k in range(eob + 1):
                mag = read_eg(br)
                sign = br.read(1) if mag else 0
                if mag > cap:
                    raise ModelError(E_VALUE_RANGE, f"|QF| = {mag} 越出窗口上界 {cap}")
                rows[idx][k] = -mag if sign else mag
            if rows[idx][eob] == 0:
                raise ModelError(
                    E_VALUE_RANGE, f"eob = {eob} 位上的系数为零，与 eob 的定义自相矛盾"
                )
        return rows
    bits = bits_per_coef(q, intra)
    for bit in range(5, -1, -1):
        if (pattern & (1 << bit)) == 0:
            continue
        idx = 5 - bit
        nonzero = False
        for k in range(BLOCK_COEFS):
            sign = br.read_bit()
            mag = br.read(bits - 1)
            if mag > cap:
                raise ModelError(E_VALUE_RANGE, f"|QF| = {mag} 越出窗口上界 {cap}")
            v = -mag if sign else mag
            rows[idx][k] = v
            if v:
                nonzero = True
        if not nonzero:
            raise ModelError(E_VALUE_RANGE, "位为 1 的块 64 个系数全零")
    return rows


def reconstruct_block(qf: list[int], q: int, intra: bool, pred: list[int]) -> list[int]:
    """反量化 → 第二级夹取（帧内 ±8192 / 帧间 ±16320）→ 反变换 → 舍入 → 第三级夹取 → 加预测 → 第四级夹取。

    无损档（q = 0）在第一级是恒等映射，反变换拿到的就是码流里的变换原值，
    于是这一支名实相符地是「直载 → 反变换」：残差 = 原值，除 64 那一步由 H Hᵀ = 8I 的两遍乘加抵掉。
    """
    lo, hi = (COEF_LO_INTRA, COEF_HI_INTRA) if intra else (COEF_LO_INTER, COEF_HI_INTER)
    f2 = [clamp(dequantize_coef(v, q), lo, hi) for v in qf]
    s = inverse_hadamard8_int(f2)
    out = []
    for i in range(BLOCK_COEFS):
        f = clamp(round_residual_int(s[i]), RES_LO, RES_HI)
        out.append(clamp(pred[i] + f, SAMPLE_LO, SAMPLE_HI))
    return out


# ---------------------------------------------------------------- 平面与取块


@dataclass
class Plane:
    """一帧三平面、行连续存放（无行距填充）；色度两个轴都是 1/2。"""

    width: int
    height: int
    planes: list = field(default_factory=list)

    @staticmethod
    def new(width: int, height: int) -> "Plane":
        w2, h2 = width // 2, height // 2
        return Plane(
            width,
            height,
            [
                bytearray([BLACK_LUMA]) * (width * height),
                bytearray([BLACK_CHROMA]) * (w2 * h2),
                bytearray([BLACK_CHROMA]) * (w2 * h2),
            ],
        )

    def pw(self, plane: int) -> int:
        return self.width if plane == 0 else self.width // 2

    def ph(self, plane: int) -> int:
        return self.height if plane == 0 else self.height // 2

    def at(self, plane: int, x: int, y: int) -> int:
        return self.planes[plane][y * self.pw(plane) + x]

    def put_row(self, plane: int, x: int, y: int, vals) -> None:
        base = y * self.pw(plane) + x
        self.planes[plane][base : base + len(vals)] = bytes(vals)

    def valid_pixels(self, display_w: int, display_h: int) -> list[int]:
        """有效区逐点读数：Y 全幅 + 两色度各 1/4 幅（比较一律只比有效区）。"""
        out = []
        for pl in range(3):
            pw = display_w if pl == 0 else display_w // 2
            ph = display_h if pl == 0 else display_h // 2
            for y in range(ph):
                out += list(self.planes[pl][y * self.pw(pl) : y * self.pw(pl) + pw])
        return out


def block_geom(mb_x: int, mb_y: int, idx: int) -> tuple[int, int, int]:
    """六块的 (平面, 左上 x, 左上 y)：亮度四块左上起、行优先，与位图 bit5..bit2 同一序。"""
    if idx < 4:
        return 0, mb_x * MB_SIZE + (idx % 2) * 8, mb_y * MB_SIZE + (idx // 2) * 8
    return (1 if idx == 4 else 2), mb_x * 8, mb_y * 8


def scatter_block(frame: Plane, plane: int, px: int, py: int, vals) -> None:
    for r in range(8):
        frame.put_row(plane, px, py + r, vals[r * 8 : r * 8 + 8])


# ---------------------------------------------------------------- 运动补偿


def sample_replicated(frame: Plane, plane: int, x: int, y: int) -> int:
    """一圈 1 像素复制带的实现形式：坐标钳到最外像素，带内取实际像素、带外取最外像素。"""
    return frame.at(
        plane,
        min(max(x, 0), frame.pw(plane) - 1),
        min(max(y, 0), frame.ph(plane) - 1),
    )


def _floor_div2(v: int) -> int:
    return v // 2 if v >= 0 else -((-v + 1) // 2)


def round_half(v: int) -> int:
    """色度矢量换算：round_half(v) = (v + 1) >> 1。"""
    return _floor_div2(v + 1)


def predict_block(
    ref: Plane, plane: int, px: int, py: int, mv: tuple[int, int]
) -> list[int]:
    """半像素取样三式 b / c / d；整像素位置直接取 A。"""
    out = []
    for r in range(8):
        for c in range(8):
            hx = 2 * (px + c) + mv[0]
            hy = 2 * (py + r) + mv[1]
            ix, iy = _floor_div2(hx), _floor_div2(hy)
            hx_half = (hx - 2 * ix) != 0
            hy_half = (hy - 2 * iy) != 0
            a = sample_replicated(ref, plane, ix, iy)
            if not hx_half and not hy_half:
                v = a
            elif hx_half and not hy_half:
                v = (a + sample_replicated(ref, plane, ix + 1, iy) + 1) >> 1
            elif not hx_half and hy_half:
                v = (a + sample_replicated(ref, plane, ix, iy + 1) + 1) >> 1
            else:
                b = sample_replicated(ref, plane, ix + 1, iy)
                cc = sample_replicated(ref, plane, ix, iy + 1)
                dd = sample_replicated(ref, plane, ix + 1, iy + 1)
                v = (a + b + cc + dd + 2) >> 2
            out.append(v)
    return out


def predict_macroblock(
    ref: Plane, mb_x: int, mb_y: int, mv: tuple[int, int]
) -> list[list[int]]:
    """同核、不同换算与尺寸：亮度用宏块矢量，色度用 round_half 后的矢量，块尺寸都是 8×8。"""
    cmv = (round_half(mv[0]), round_half(mv[1]))
    out = []
    for idx in range(BLOCKS_PER_MB):
        plane, px, py = block_geom(mb_x, mb_y, idx)
        out.append(predict_block(ref, plane, px, py, mv if plane == 0 else cmv))
    return out


def bidirectional_average(pf: list[int], pb: list[int]) -> list[int]:
    """双向平均 P = (Pf + Pb + 1) >> 1，逐像素。"""
    return [(a + b + 1) >> 1 for a, b in zip(pf, pb)]


def fill_block(level: int) -> list[int]:
    return [level] * BLOCK_COEFS


# ---------------------------------------------------------------- 预测器与矢量


def med3(x: int, y: int, z: int) -> int:
    if x <= y:
        return y if y <= z else (z if x <= z else x)
    return x if x <= z else (z if y <= z else y)


def median_predictor(
    a: tuple[int, int], b: tuple[int, int], c: tuple[int, int]
) -> tuple[int, int]:
    """三候选逐分量取中值；无效候选（越界、缺位、邻居不带该方向）由调用方先置零再参与。"""
    return med3(a[0], b[0], c[0]), med3(a[1], b[1], c[1])


def candidate_indices(mb_width: int, i: int) -> list[int]:
    """三候选的位置：左、上、右上；越界交回 −1（调用方按零矢量处理）。"""
    col = i % mb_width
    above = i >= mb_width
    return [
        i - 1 if col > 0 else -1,
        i - mb_width if above else -1,
        (i - mb_width + 1) if (above and col + 1 < mb_width) else -1,
    ]


def saturate_component(v: int) -> int:
    """落回 32 位有符号的唯一收口式：越上界取 INT32_MAX、越下界取 INT32_MIN，不静默回绕。"""
    return INT32_MAX if v > INT32_MAX else (INT32_MIN if v < INT32_MIN else v)


def add_mv_component(base: int, delta: int) -> int:
    """预测值（32 位）+ 线上残差（64 位）→ 绝对矢量：先在 64 位里定界再落回 32 位。"""
    if delta >= INT32_MAX - base:
        return INT32_MAX
    if delta <= INT32_MIN - base:
        return INT32_MIN
    return base + delta


def trunc_div(a: int, b: int) -> int:
    """向零截断的整数除法：矢量域不许用右移代替（右移是向下取整，负矢量会差一格）。"""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def direct_scale(mv_ref: tuple[int, int], trb: int, trd: int):
    """direct 的纯缩放：MVF = (TRB × MV_ref) ÷ TRD、MVB = MVF − MV_ref，不带残差。

    除法是向零截断的整数除法；乘除一律 64 位有符号、全式只舍入一次。
    TRD ≤ 0 或 TRB > TRD 判参数不成立、不做算术；TRB = TRD 不在判拒之列。

    MVB 的被减数是**未夹饱和**的那 64 位缩放值，先减后夹：两个分量各自从同一个 64 位值落回
    32 位饱和域（依据 src/video/direct_scale.cpp 的 DirectScale —— 它对 forward 与 backward
    分别调 SaturateVectorComponent，backward 那一支的入参是 mf_x − mv_ref 的 64 位差）。
    拿夹过的 MVF 作被减数，在 MV_ref 触到饱和时就会与库侧差一个饱和残量。
    帧级判据 0 ≤ TRB ≤ TRD 成立时 |缩放值| ≤ |MV_ref| ≤ 32 位上界，饱和触不到、两支同值
    （自检逐档核这条不变式）；这一道对齐管的是判据被绕过的那些入参。
    """
    if trd <= 0 or trb > trd:
        raise ModelError(E_VALUE_RANGE, f"direct 时间权重不成立：trb={trb} trd={trd}")

    scaled = tuple(
        trunc_div(trb * v, trd) for v in mv_ref
    )  # 64 位有符号、尚未落回 32 位
    mf = tuple(saturate_component(x) for x in scaled)
    mb = tuple(saturate_component(x - r) for x, r in zip(scaled, mv_ref))
    return mf, mb


# ---------------------------------------------------------------- 语法结构


@dataclass
class VopHead:
    type: int = 0
    quant: int = 0
    fcode_forward: int = 0
    fcode_backward: int = 0
    intra_dc_vlc_thr: int = 0
    coef_coding: int = COEF_FIXED
    dquant: int = 0
    mb_count: int = 0


@dataclass
class SliceHead:
    frame_id: int = 0
    fragment_count: int = 0
    vop_type: int = 0
    vop_quant: int = 0
    first_mb: int = 0
    mb_run: int = 0
    size: int = 0


@dataclass
class MbIp:
    not_coded: bool = False
    mb_type: int = MB_TYPE_INTER
    cbpc: int = 0
    cbpy: int = 0
    dquant: int = 0
    mv: tuple[int, int] = (0, 0)
    block_size: int = 0


@dataclass
class MbB:
    not_coded: bool = False
    mb_type: int = BVOP_DIRECT
    forward: tuple[int, int] = (0, 0)
    backward: tuple[int, int] = (0, 0)
    cbpy: int = 0
    block_size: int = 0


def parse_vop_head(data: bytes, pos: int, quant_precision: int) -> tuple[VopHead, int]:
    """VOP 头 = 6 字节定长 + mb_count 的 varint，逐字段按值域判。

    type ∈ {0,1,2}；quant 是三段域（0 = 无损 / 1..2^quant_precision − 1 = 有损 / 其余不成立）；
    两个 fcode ∈ [1,7]（0 非法）；
    vop_flags 的 bit3（顶场优先）/ bit4（隔行扫描）恒 0，bit5-7 = 系数编码法（只认 0 与 1）；
    vop_dquant 的 bit5-7 恒 0、bit0-4 是 5 bit 补码（−16..+15）；mb_count = 0 非法。
    """
    if len(data) - pos < VOP_FIXED_BYTES + 1:
        raise ModelError(E_PAYLOAD_TOO_SMALL, "VOP 头短于 6 字节定长 + 至少 1 字节计数")
    h = VopHead()
    h.type, h.quant = data[pos], data[pos + 1]
    h.fcode_forward, h.fcode_backward = data[pos + 2], data[pos + 3]
    flags, dqs = data[pos + 4], data[pos + 5]
    if h.type > 2:
        raise ModelError(E_HEADER_BAD, f"帧类型 {h.type} 非 I/P/B")
    if not quant_in_stream_domain(h.quant, quant_precision):
        # 只有落在三段域之外才判帧头不成立（库侧 payload.cpp 的 DecodeVopHeader 失败即 kHeaderBad，
        # 判据是 quant.h 的 QuantInStreamDomain）；0 = 无损是合法取值，不是坏值，
        # 把它当坏值会让无损流整条解不出来。
        # 69 只由编码器声明侧 default_quant 越界产出，两者不混
        raise ModelError(
            E_HEADER_BAD,
            f"帧头 vop_quant = {h.quant} 不在三段域（0 = 无损 / 1..{quant_ceiling(quant_precision)} = 有损）",
        )
    if not 1 <= h.fcode_forward <= 7 or not 1 <= h.fcode_backward <= 7:
        raise ModelError(
            E_HEADER_BAD, f"fcode = {h.fcode_forward}/{h.fcode_backward} 非 1..7"
        )
    if flags & 0x18:
        raise ModelError(E_HEADER_BAD, "vop_flags 的 bit3-4 非 0")
    if (flags >> 5) & 0x07 > 1:
        # bit5-7 = 系数编码法，只认 0（定宽）与 1（EOB+EG）；未赋法的值判帧头不成立
        raise ModelError(E_HEADER_BAD, "vop_flags 的系数编码法未赋法")
    if dqs & 0xE0:
        raise ModelError(E_HEADER_BAD, "vop_dquant 的 bit5-7 非 0")
    d = dqs & 0x1F
    h.dquant = d - 32 if d & 0x10 else d
    if not VOP_DQUANT_MIN <= h.dquant <= VOP_DQUANT_MAX:
        raise ModelError(E_VALUE_RANGE, f"d_vop = {h.dquant} 越出 −16..+15")
    h.intra_dc_vlc_thr = flags & 0x07
    h.coef_coding = (flags >> 5) & 0x07
    br = BitReader(data, (pos + VOP_FIXED_BYTES) * 8)
    h.mb_count = br.read_varint()
    if h.mb_count == 0:
        raise ModelError(E_VALUE_RANGE, "mb_count = 0")
    return h, br.pos // 8


def parse_slice_head(
    data: bytes, pos: int, quant_precision: int, first: bool, last: bool
) -> SliceHead:
    """片头 = frame_id(4) + fragment_count(2) + flags(1) + vop_type(1) [+vop_quant(1)] + 两枚 varint。

    判据：fragment_count ≥ 2（单片帧不发片头）、flags 的 bit3-7 为 0、vop_type ≤ 2、
    首末片位与量化携带位由传入片数给权威、has_vop_quant 必须等于 is_first、
    携带的 vop_quant 与帧头同一段三段域、mb_run = 0 是空片 → 值域不成立。
    """
    need = SLICE_HEAD_PREFIX_BYTES + 1
    if len(data) - pos < need:
        raise ModelError(E_PAYLOAD_TOO_SMALL, "片头短于 8 字节定长 + 至少 1 字节变量")
    frame_id = int.from_bytes(data[pos : pos + 4], "little")
    fragment_count = int.from_bytes(data[pos + 4 : pos + 6], "little")
    flags, vop_type = data[pos + 6], data[pos + 7]
    if flags & 0xF8:
        raise ModelError(E_SLICE_HEADER_BAD, "片头 flags 的 bit3-7 非 0")
    if fragment_count < 2:
        raise ModelError(E_SLICE_HEADER_BAD, "单片帧不该有片头")
    if vop_type > 2:
        raise ModelError(E_SLICE_HEADER_BAD, f"片头帧类型 {vop_type} 非法")
    is_first, is_last, has_quant = (
        bool(flags & 0x02),
        bool(flags & 0x01),
        bool(flags & 0x04),
    )
    if is_first != first or is_last != last:
        raise ModelError(E_SLICE_HEADER_BAD, "片头首末位与传入片数矛盾")
    if has_quant != is_first:
        raise ModelError(E_SLICE_HEADER_BAD, "量化携带位与该片位置矛盾")
    p = pos + SLICE_HEAD_PREFIX_BYTES
    vop_quant = 0
    if has_quant:
        if len(data) - p < 1:
            raise ModelError(E_PAYLOAD_TOO_SMALL, "片头带量化位但载荷不足")
        vop_quant = data[p]
        if not quant_in_stream_domain(vop_quant, quant_precision):
            # 片头携带的 vop_quant 与帧头同域（三段），0 = 无损同样合法
            raise ModelError(
                E_SLICE_HEADER_BAD,
                f"片头 vop_quant = {vop_quant} 不在三段域（0 = 无损 / "
                f"1..{quant_ceiling(quant_precision)} = 有损）",
            )
        p += 1
    br = BitReader(data, p * 8)
    first_mb = br.read_varint()
    mb_run = br.read_varint()
    if mb_run == 0:
        raise ModelError(E_VALUE_RANGE, "mb_run = 0 是空片")
    # size = 本片头吃掉的字节数（定长 + 两枚 varint，恒整字节）
    return SliceHead(
        frame_id,
        fragment_count,
        vop_type,
        vop_quant,
        first_mb,
        mb_run,
        br.pos // 8 - pos,
    )


def parse_mb_ip(br: BitReader, vop_type: int, pred: tuple[int, int]) -> MbIp:
    """I / P 宏块头：not_coded → mcbpc → cbpy → dquant →〔motion_vector()〕→ block_size。"""
    mb = MbIp()
    if br.read_bit():
        if vop_type == VOP_I:
            raise ModelError(E_HEADER_BAD, "I-VOP 没有 skip 语义，not_coded = 1 不成立")
        mb.not_coded = True
        return mb
    _, _, mb_type, cbpc = read_vlc(
        br, MCBPC_MAX_BITS, lambda n, v: _find_mcbpc(vop_type, n, v)
    )
    if mb_type in (MB_TYPE_STUFFING, MB_TYPE_INTER4V):
        raise ModelError(E_TOOL_EXCLUDED, f"排除集：mcbpc 行的 mb_type = {mb_type}")
    crow = read_vlc(br, CBPY_MAX_BITS, _find_cbpy)
    intra = mb_type == MB_TYPE_INTRA
    mb.mb_type = mb_type
    mb.cbpc = cbpc
    mb.cbpy = crow[2] if intra else crow[3]
    mb.dquant = MB_DQUANT_TABLE[br.read(2)]
    if not intra:
        mb.mv = (
            add_mv_component(pred[0], br.read_zigzag()),
            add_mv_component(pred[1], br.read_zigzag()),
        )
    mb.block_size = br.read_varint()
    if mb.block_size > br.bits_left() // 8:
        raise ModelError(
            E_PAYLOAD_TOO_SMALL, f"block_size = {mb.block_size} 越出剩余载荷"
        )
    if mb.block_size == 0 and (mb.cbpc | mb.cbpy) != 0:
        raise ModelError(E_VALUE_RANGE, "block_size = 0 而块模式非全零")
    return mb


def parse_mb_b(br: BitReader, pred_f: tuple[int, int], pred_b: tuple[int, int]) -> MbB:
    """B 宏块头：not_coded(8) → mb_type(8) → mvd[]（个数由 mb_type 推）→ cbpy(6 bit 全位图) → block_size。"""
    mb = MbB()
    nc = br.read(8)
    if nc > 1:
        raise ModelError(E_HEADER_BAD, f"B 侧 not_coded 字段 = {nc} 非 0/1")
    mt = br.read(8)
    if mt > BVOP_MB_TYPE_MAX:
        raise ModelError(E_TOOL_EXCLUDED, f"B 侧 mb_type = {mt} 落在保留值 4..255")
    mb.not_coded, mb.mb_type = bool(nc), mt
    n = bvop_mvd_count(mt)
    if n >= 2:  # interpolate：两个残差按前向、后向的顺序排
        mb.forward = (
            add_mv_component(pred_f[0], br.read_zigzag()),
            add_mv_component(pred_f[1], br.read_zigzag()),
        )
        mb.backward = (
            add_mv_component(pred_b[0], br.read_zigzag()),
            add_mv_component(pred_b[1], br.read_zigzag()),
        )
    elif n == 1 and mt == BVOP_BACKWARD:
        mb.backward = (
            add_mv_component(pred_b[0], br.read_zigzag()),
            add_mv_component(pred_b[1], br.read_zigzag()),
        )
    elif n == 1:
        mb.forward = (
            add_mv_component(pred_f[0], br.read_zigzag()),
            add_mv_component(pred_f[1], br.read_zigzag()),
        )
    cbpy = br.read_varint()
    if cbpy > 0x3F:
        raise ModelError(E_VALUE_RANGE, f"B 侧块模式 {cbpy} 越出 0..63")
    if mb.not_coded and cbpy != 0:
        raise ModelError(E_HEADER_BAD, "not_coded = 1 而 cbpy 非 0")
    mb.cbpy = cbpy
    if not mb.not_coded:
        mb.block_size = br.read_varint()
        if mb.block_size > br.bits_left() // 8:
            raise ModelError(
                E_PAYLOAD_TOO_SMALL, f"block_size = {mb.block_size} 越出剩余载荷"
            )
        if mb.block_size == 0 and cbpy != 0:
            raise ModelError(E_VALUE_RANGE, "block_size = 0 而块模式非全零")
    return mb


@dataclass
class WalkResult:
    head: VopHead
    mbs: list
    patterns: list
    consumed: int
    mv_field: list
    blocks: list  # 每宏块的块数据起始位（not_coded 的宏块为 None），下标与 mbs 对齐
    data: bytes


def walk_payload(
    data: bytes, quant_precision: int, mb_width: int, fragment_count: int
) -> WalkResult:
    """一帧完整载荷的独立解析：四条片校验 + 逐宏块定界，块数据的位起始位置一并交出。

    单趟走完：片头 → （首片的 VOP 头）→ 该片宏块序列 → 下一片片头，位置由位消费点推出，
    不另立第二套定界口径。
    """
    if fragment_count == 0 or fragment_count > MAX_FRAGMENT_COUNT:
        raise ModelError(
            E_VALUE_RANGE, f"声明片数 {fragment_count} 越出 1..{MAX_FRAGMENT_COUNT}"
        )
    if not data:
        raise ModelError(E_INVALID_ARGUMENT, "空载荷")
    mbs: list = []
    patterns: list[int] = []
    mv_field: list = []
    blocks: list = []
    if fragment_count == 1:
        head, pos = parse_vop_head(data, 0, quant_precision)
        used = _walk_run(
            data,
            pos,
            head.type,
            mb_width,
            head.mb_count,
            mbs,
            patterns,
            mv_field,
            blocks,
        )
        # 解满 mb_count 后仍有剩余整字节 = 实际不止一片而调用方报了一片 → 码流不成立
        if pos + used != len(data):
            raise ModelError(
                E_VALUE_RANGE,
                f"解满 {head.mb_count} 个宏块后仍有 {len(data) - pos - used} 个整字节",
            )
    else:
        pos, total, head = 0, 0, None
        for i in range(fragment_count):
            sh = parse_slice_head(
                data, pos, quant_precision, i == 0, i == fragment_count - 1
            )
            # 校验 1：片头在带的分片数 == 调用方声明的片数
            if sh.fragment_count != fragment_count:
                raise ModelError(
                    E_VALUE_RANGE,
                    f"片头分片数 {sh.fragment_count} 与声明 {fragment_count} 不符",
                )
            pos += sh.size
            if i == 0:
                # parse_vop_head 交回的是「解完后到的绝对字节位」，不是本片吃掉的字节数
                head, pos = parse_vop_head(data, pos, quant_precision)
            # 校验 3：本片 first_mb == 前片累加值（首片为 0），即片按宏块光栅序首尾相接
            if sh.first_mb != total:
                raise ModelError(
                    E_SLICE_SEQUENCE,
                    f"first_mb = {sh.first_mb} 与前片累加 {total} 不接",
                )
            used = _walk_run(
                data,
                pos,
                head.type,
                mb_width,
                sh.mb_run,
                mbs,
                patterns,
                mv_field,
                blocks,
            )
            pos += used
            total += sh.mb_run
        # 校验 2：各片 mb_run 累加 == 首片 VOP 头的 mb_count
        if total != head.mb_count:
            raise ModelError(
                E_VALUE_RANGE,
                f"各片 mb_run 累加 {total} 与首片 mb_count {head.mb_count} 不符",
            )
        # 校验 4：全部片解完后字节整段吃光（不多不少）
        if pos != len(data):
            raise ModelError(E_VALUE_RANGE, f"全部片解完后剩 {len(data) - pos} 字节")
        head.mb_count = total
    return WalkResult(head, mbs, patterns, len(data), mv_field, blocks, data)


def _walk_run(
    data, start, vop_type, mb_width, count, mbs, patterns, mv_field, blocks
) -> int:
    """解一段宏块序列（一片或整帧），回报吃掉的字节数（向上取整，尾位是补齐位）。

    预测器状态在本段内新建、段末即弃 —— 多片路径逐段调用，等价于「片界归零」。
    blocks 与 mbs 同下标：not_coded 的宏块没有块数据，交回 None。
    """
    br = BitReader(data, start * 8)
    if vop_type == VOP_B:
        state: list = []
        for i in range(count):
            idxs = candidate_indices(mb_width, i)

            def pick(k: int, fwd: bool, st=state):
                if k < 0 or k >= len(st):
                    return (0, 0)
                f, b, has = st[k]
                if not (has & (1 if fwd else 2)):
                    return (0, 0)
                return f if fwd else b

            pred_f = median_predictor(*[pick(k, True) for k in idxs])
            pred_b = median_predictor(*[pick(k, False) for k in idxs])
            mb = parse_mb_b(br, pred_f, pred_b)
            mbs.append(mb)
            patterns.append(mb.cbpy & 0x3F)
            fwd_ok = mb.mb_type in (BVOP_INTERPOLATE, BVOP_FORWARD)
            back_ok = mb.mb_type in (BVOP_INTERPOLATE, BVOP_BACKWARD)
            state.append(
                (mb.forward, mb.backward, (1 if fwd_ok else 0) | (2 if back_ok else 0))
            )
            # 同位宏块的缩放源：带前向就存前向，只带后向存后向，direct / 取不到的存零
            mv_field.append(
                mb.forward if fwd_ok else (mb.backward if back_ok else (0, 0))
            )
            if mb.not_coded:
                blocks.append(None)
            else:
                blocks.append(br.pos)
                br.skip_bytes(mb.block_size)
    else:
        state_ip: list = []
        for i in range(count):
            idxs = candidate_indices(mb_width, i)

            def pick_ip(k: int, st=state_ip):
                if k < 0 or k >= len(st):
                    return (0, 0)
                mv, has = st[k]
                return mv if has else (0, 0)

            pred = median_predictor(*[pick_ip(k) for k in idxs])
            mb = parse_mb_ip(br, vop_type, pred)
            mbs.append(mb)
            intra = mb.mb_type == MB_TYPE_INTRA
            patterns.append(((mb.cbpy << 2) | (mb.cbpc & 0x03)) & 0x3F)
            # 帧内宏块压根没有矢量（候选无效）；not_coded 有「零矢量」这一档，照样参与预测器
            state_ip.append(((0, 0) if intra else mb.mv, 0 if intra else 1))
            mv_field.append((0, 0) if intra else mb.mv)
            if mb.not_coded:
                blocks.append(None)
            else:
                blocks.append(br.pos)
                br.skip_bytes(mb.block_size)
    return (br.pos - start * 8 + 7) // 8


# ---------------------------------------------------------------- 帧重建模型


class ModelDecoder:
    """照规范写的视频解码模型：吃一帧的完整载荷、出重建帧。

    参考缓冲只存非 B 帧、容量恒为 2、满则驱逐解码序最旧的一帧（禁止显示序参与驱逐）；
    待输出队列阈值 = max_b_frames + 2，达到即弹出 PTS 最小的一帧（弹出只出队、不驱逐参考帧）；
    流末按 PTS 升序清空。前向参考 = PTS 小于当前且最近者，后向 = PTS 大于当前且最近者。
    B 不进参考缓冲；同位块矢量取不到的一律按零。
    """

    DPB_CAPACITY = 2

    def __init__(self, coded_w, coded_h, quant_precision=7, max_b_frames=0):
        self.w, self.h = coded_w, coded_h
        self.mb_width = coded_w // MB_SIZE
        self.mb_height = coded_h // MB_SIZE
        self.qp = quant_precision
        self.max_b_frames = max_b_frames
        self.frames: dict[int, Plane] = {}
        self.slot_pts: dict[int, int] = {}
        self.slot_mv: dict[int, list] = {}
        self.dpb: list[int] = []
        self.queue: list[tuple[int, int]] = []
        self.next_id = 0

    def decode(self, data: bytes, fragment_count: int, dts: int, pts: int):
        """返回 (错误码或 None, 弹出的 (pts, id) 列表, 处置标签)。"""
        if not data:
            return E_INVALID_ARGUMENT, [], "invalid_argument"
        try:
            res = walk_payload(data, self.qp, self.mb_width, fragment_count)
        except ModelError as exc:
            return exc.code, [], "dropped"
        head = res.head
        if head.mb_count != self.mb_width * self.mb_height:
            return E_VALUE_RANGE, [], "dropped"
        fwd, back = self._pick(pts, False), self._pick(pts, True)
        t = head.type
        if (t == VOP_P and fwd < 0) or (t == VOP_B and (fwd < 0 or back < 0)):
            return None, [], "drop_ref_missing"
        trb = trd = 0
        if t == VOP_B:
            trb = pts - self.slot_pts[fwd]
            trd = self.slot_pts[back] - self.slot_pts[fwd]
            if trd <= 0 or trb <= 0 or trb >= trd:
                return None, [], "drop_direct_time"
        try:
            frame = self._reconstruct(res, fwd, back, trb, trd)
        except ModelError as exc:
            return exc.code, [], "dropped"
        sid = self.next_id
        self.next_id += 1
        self.frames[sid] = frame
        self.slot_pts[sid] = pts
        self.slot_mv[sid] = res.mv_field
        emitted = []
        if t != VOP_B:
            if len(self.dpb) >= self.DPB_CAPACITY:
                self.dpb.remove(0)
            self.dpb.append(sid)
        self.queue.append((pts, sid))
        self.queue.sort(key=lambda it: (it[0], it[1]))
        if len(self.queue) >= self.max_b_frames + 2:
            emitted.append(self.queue.pop(0))
        return None, emitted, "output"

    def flush(self):
        out = list(self.queue)
        self.queue.clear()
        return out

    def _pick(self, pts: int, backward: bool) -> int:
        best, gap = -1, None
        for sid in self.dpb:
            tp = self.slot_pts[sid]
            if (tp - pts) * (1 if backward else -1) < 0:
                continue
            d = abs(tp - pts)
            if gap is None or d < gap:
                best, gap = sid, d
        return best

    def _reconstruct(
        self, res: WalkResult, fwd: int, back: int, trb: int, trd: int
    ) -> Plane:
        head = res.head
        frame = Plane.new(self.w, self.h)
        fwd_frame = self.frames.get(fwd) if fwd >= 0 else None
        back_frame = self.frames.get(back) if back >= 0 else None
        data = res.data
        for i, mb in enumerate(res.mbs):
            mx, my = i % self.mb_width, i // self.mb_width
            if head.type == VOP_B:
                qq = effective_quant(head.quant, head.dquant, 0, self.qp)
                intra = False
                pred6 = self._pred_b(
                    mb, mx, my, fwd_frame, back_frame, back, trb, trd, i
                )
            else:
                intra = mb.mb_type == MB_TYPE_INTRA
                if intra:
                    pred6 = [fill_block(INTRA_PRED) for _ in range(BLOCKS_PER_MB)]
                elif fwd_frame is None:
                    raise ModelError(E_VALUE_RANGE, "帧间宏块没有前向参考")
                else:
                    # not_coded 的宏块不带 motion_vector()，矢量按零处理
                    pred6 = predict_macroblock(
                        fwd_frame, mx, my, (0, 0) if mb.not_coded else mb.mv
                    )
                qq = effective_quant(head.quant, head.dquant, mb.dquant, self.qp)
            if mb.not_coded:
                # 无残差 ⇒ 重建就是预测块本身
                for idx in range(BLOCKS_PER_MB):
                    pl, px, py = block_geom(mx, my, idx)
                    scatter_block(frame, pl, px, py, pred6[idx])
                continue
            pattern = res.patterns[i]
            coding = head.coef_coding
            present = present_block_count(pattern)
            bmax = block_bytes_max(qq, intra, coding)
            want = present * bmax
            if coding == COEF_FIXED:
                if want != mb.block_size:
                    raise ModelError(
                        E_VALUE_RANGE,
                        f"block_size {mb.block_size} 与位图算式 {present} × "
                        f"{block_bytes(qq, intra)} = {want} 不符",
                    )
            elif want == 0:
                # 位图全零的宏块（整块跳过）本法合法长度就是 0，别的值一律不成立
                if mb.block_size != 0:
                    raise ModelError(
                        E_VALUE_RANGE, f"位图全零而 block_size = {mb.block_size}"
                    )
            elif mb.block_size == 0 or mb.block_size > want:
                raise ModelError(
                    E_VALUE_RANGE,
                    f"block_size {mb.block_size} 越出本法上界 {present} × {bmax} = {want}",
                )
            bit_pos = res.blocks[i]
            br = BitReader(data, bit_pos, end_bit=bit_pos + mb.block_size * 8)
            rows = read_present_blocks(br, pattern, qq, intra, coding)
            left = br.bits_left()
            # 定宽不该有剩余位；变长块边界自终止，末尾只允许 0..7 位补齐，剩整字节 = block_size 说谎
            if coding == COEF_FIXED:
                if left != 0:
                    raise ModelError(E_VALUE_RANGE, f"定宽载体解完仍剩 {left} 位")
            elif left >= 8:
                raise ModelError(E_VALUE_RANGE, f"变长载体解完仍剩 {left} 位")
            for idx in range(BLOCKS_PER_MB):
                pl, px, py = block_geom(mx, my, idx)
                scatter_block(
                    frame,
                    pl,
                    px,
                    py,
                    reconstruct_block(rows[idx], qq, intra, pred6[idx]),
                )
        return frame

    def _pred_b(
        self, mb, mx, my, fwd_frame, back_frame, back_ref, trb, trd, i
    ) -> list[list[int]]:
        if mb.mb_type == BVOP_DIRECT:
            # back_ref 是后向参考的槽位号：同位宏块的矢量记在那一张槽的 mv 表里
            ref_mvs = self.slot_mv.get(back_ref, [])
            # 同位块取不到的（前向参考是 I 帧 / intra / skip / 缺位）一律按零矢量
            mv_ref = ref_mvs[i] if i < len(ref_mvs) else (0, 0)
            mf, mbv = direct_scale(mv_ref, trb, trd)
            a = predict_macroblock(fwd_frame, mx, my, mf)
            b = predict_macroblock(back_frame, mx, my, mbv)
            return [bidirectional_average(x, y) for x, y in zip(a, b)]
        if mb.mb_type == BVOP_BACKWARD:
            return predict_macroblock(back_frame, mx, my, mb.backward)
        if mb.mb_type == BVOP_FORWARD:
            return predict_macroblock(fwd_frame, mx, my, mb.forward)
        a = predict_macroblock(fwd_frame, mx, my, mb.forward)
        b = predict_macroblock(back_frame, mx, my, mb.backward)
        return [bidirectional_average(x, y) for x, y in zip(a, b)]


# ---------------------------------------------------------------- 载体写入件


def _block_bytes(
    pattern: int, qf_rows: list[list[int]], q: int, intra: bool, coding: int
) -> bytes:
    """一个宏块的块数据字节：本 MB 全部在场块写完统一到字节边界（位状态不跨宏块、不跨片）。

    只由自检的载体往返用例调用：本件是读侧模型，产字节只为造出读得回去的输入，
    进期望值文件的字节由载荷生成件产（两处各写一份就会只对平自己）。
    """
    bw = BitWriter()
    for idx in range(BLOCKS_PER_MB):
        if (pattern & (1 << (5 - idx))) == 0:
            continue
        if coding == COEF_EOB_EG:
            nz = [k for k, v in enumerate(qf_rows[idx]) if v]
            if not nz:
                bw.write(1, 1)  # all_zero：整块只花这一 bit
                continue
            bw.write(0, 1)
            eob = nz[-1]  # 自然行序里最后一个非零系数的下标，其后槽一概不写
            bw.write(eob, EOB_FIELD_BITS)
            for k in range(eob + 1):
                v = qf_rows[idx][k]
                write_eg(bw, abs(v))
                if v:  # 零槽占一格 EG(0) = 1 bit，但不带符号位
                    bw.write(1 if v < 0 else 0, 1)
        else:
            bits = bits_per_coef(q, intra)
            for v in qf_rows[idx]:
                bw.write(1 if v < 0 else 0, 1)
                bw.write(abs(v), bits - 1)
    return bw.to_bytes()


# ---------------------------------------------------------------- FLAC 帧头模型（音频侧）

FLAC_SYNC_HIGH = 0xFF
SAMPLERATE_CODES = {
    1: 88200,
    2: 176400,
    3: 192000,
    4: 8000,
    5: 16000,
    6: 22050,
    7: 24000,
    8: 32000,
    9: 44100,
    10: 48000,
    11: 96000,
}
BITS_CODES = {0: 0, 1: 8, 2: 12, 4: 16, 5: 20, 6: 24, 7: 32}  # 0b011 = Reserved 单独判

# 扩展 UTF-8 编码号每一档（lead 个字节）可载的最大值，下标就是 lead。
# 位宽 = 首字节的 (7 − lead) 位载荷 + 每个续字节 6 位 ⇒ (7 − lead) + 6 × (lead − 1)；
# lead = 1 是 0xxxxxxx 共 7 位，lead = 0 供下界判比较用（任何值都大于 0，单字节档没有更短一档）。
EXT_UTF8_CAPS = (0, 0x7F) + tuple(
    (1 << (7 - lead + 6 * (lead - 1))) - 1 for lead in range(2, 8)
)

# 帧头层的非法值：逐条取自码表本身的 Reserved / Forbidden 行与解析里的判点，不凭直觉加。
# 出处：四张码表的**取值**在 src/audio/frame.cpp（采样率表在 RateFromCode，块大小 / 声道 /
# 位深三张表在 ParseFrameHead 的分支），编码号的量程在 ReadCodedNumber 与 ExtUtf8Cap；
# 位域布局在 src/audio/frame.h 的头注释。
AUDIO_HEADER_ILLEGAL = [
    ("byte1 的同步码后保留位 = 1", "该位必须为 0（区别于元数据块的 14+1 位同步形态）"),
    ("块大小码 0b0000", "块大小码表里唯一的 Reserved 行"),
    ("采样率码 0b1111", "采样率码表里唯一的 Forbidden 行"),
    ("位深码 0b011", "位深码表里的 Reserved 行"),
    ("声道码 0b1011..0b1111", "声道码表的 Reserved 段"),
    ("byte3 的位深后保留位 = 1", "位深之后那一必须为 0"),
    (
        "编码号首字节 0xFF",
        "连续 1 达 8 位即不分配形态；0xFE 是合法的 7 字节形态，不属非法",
    ),
    ("编码号首字节落在 0x80..0xBF", "那是续接位形态，单独作首字节不成立"),
    (
        "编码号不是最短形式",
        "上下两界都判，与库侧 ReadCodedNumber 同两条：值落得进更短一档就是多装了一个字节；"
        "越出本档量程即越界（本档载荷位宽由掩码铺满，自检逐档核 cap 恰是可达最大值，"
        "所以上那一判在合法形态里咬不到，留着是对齐实码的判点而非凑数）",
    ),
    ("帧头 CRC-8 不符", "帧头校验独立于帧尾校验，多项式 0x07"),
    ("帧尾 CRC-16 不符", "帧尾校验覆盖整帧除自身，多项式 0x8005"),
]
# 流式子集层（与帧头层分属两层，判定对象不同）：块大小 > 16384 拒；采样率 ≤ 48000 时
# 块大小 > 4608 拒；位深只收 16 / 24；声道 ≤ 8；预测阶 ≤ 12、Rice 分块阶 ≤ 8 由后端兜底。
# 关键区分：块大小码 0b1111 → 32768 在**帧头层是合法值**，只在子集层越界；
# 采样率码 0b1111 的 Forbidden 落在采样率上，与块大小码无关，不得混到块大小那一档去。
# 16384 那一档是全采样率通用的上界（src/audio/frame.cpp 的 StreamableSubsetOk 先判它，
# 再看低采样率附加上限），不随采样率分档，故高采样率那边不再另立一条。
AUDIO_SUBSET_ILLEGAL = [
    ("块大小 > 16384", "子集上限；32768 在帧头层合法、在子集层越界"),
    ("采样率 ≤ 48000 且块大小 > 4608", "子集的低采样率附加上限"),
    ("位深 ∈ {8, 12, 20, 32}", "子集只收 16 / 24"),
    ("声道 > 8", "子集上限"),
]


def crc8_flac(data: bytes) -> int:
    """帧头 CRC-8：多项式 0x07、初值 0、不反射、无收尾异或。"""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def crc16_flac(data: bytes) -> int:
    """帧尾 CRC-16：多项式 0x8005、初值 0、不反射、无收尾异或。"""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = (
                ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
            )
    return crc


@dataclass
class FlacHead:
    blocksize: int = 0
    sample_rate: int = 0
    channels: int = 0
    bit_depth: int = 0
    coded_number: int = 0
    blocking_strategy: int = 0
    header_size: int = 0
    bs_code: int = 0
    sr_code: int = 0
    ch_code: int = 0
    bits_code: int = 0


def parse_flac_head(data: bytes) -> FlacHead:
    """解 FLAC 帧头并自核：同步码、两个保留位、码表的保留 / 禁用值、编码号、帧头 CRC-8。"""
    if len(data) < 7:
        raise ModelError("FRAME_HEADER_BAD", "帧头短于 7 字节下限")
    if data[0] != FLAC_SYNC_HIGH or (data[1] & 0xFE) != 0xF8:
        raise ModelError(
            "FRAME_HEADER_BAD", "同步码不符（byte1 的 bit7..bit1 应为 1111100）"
        )
    if (data[1] >> 1) & 1:
        raise ModelError("FRAME_HEADER_BAD", "同步码尾位（byte1 bit1）非 0")
    bs_code, sr_code = data[2] >> 4, data[2] & 0x0F
    ch_code, bits_code = data[3] >> 4, (data[3] >> 1) & 0x07
    if data[3] & 1:
        raise ModelError("FRAME_HEADER_BAD", "位深后的保留位非 0")
    if bs_code == 0:
        raise ModelError("FRAME_HEADER_BAD", "块大小码 0b0000 是保留值")
    if sr_code == 15:
        raise ModelError("FRAME_HEADER_BAD", "采样率码 0b1111 是禁用值")
    if bits_code == 3:
        raise ModelError("FRAME_HEADER_BAD", "位深码 0b011 是 Reserved")
    if ch_code >= 11:
        raise ModelError(
            "FRAME_HEADER_BAD", f"声道码 {ch_code} 落在 Reserved 0b1011..0b1111"
        )
    pos = 4
    h = FlacHead(bs_code=bs_code, sr_code=sr_code, ch_code=ch_code, bits_code=bits_code)
    h.blocking_strategy = data[1] & 1
    h.coded_number, used = _utf8_coded_number(data, pos)
    pos += used
    if bs_code in (6, 7):
        extra = 1 if bs_code == 6 else 2
        if len(data) - pos < extra:
            raise ModelError("FRAME_HEADER_BAD", "非常规块大小的 extra 字节不够")
        h.blocksize = int.from_bytes(data[pos : pos + extra], "big") + 1
        pos += extra
    elif bs_code == 1:
        h.blocksize = 192
    elif 2 <= bs_code <= 5:
        h.blocksize = 144 << bs_code  # 块大小码表：144 × 2^v
    else:
        h.blocksize = 1 << bs_code  # 块大小码表：0b1000..0b1111 → 2^v，末档含 32768
    rate = SAMPLERATE_CODES.get(sr_code, 0)
    if 12 <= sr_code <= 14:
        extra = 1 if sr_code == 12 else 2
        if len(data) - pos < extra:
            raise ModelError("FRAME_HEADER_BAD", "非常规采样率的 extra 字节不够")
        stored = int.from_bytes(data[pos : pos + extra], "big")
        pos += extra
        rate = (
            stored * 1000
            if sr_code == 12
            else (stored if sr_code == 13 else stored * 10)
        )
        if rate == 0:
            raise ModelError("FRAME_HEADER_BAD", "非常规采样率算出 0")
    h.sample_rate = rate
    if pos >= len(data):
        raise ModelError("FRAME_HEADER_BAD", "帧头没有 CRC-8 的位置")
    if crc8_flac(data[:pos]) != data[pos]:
        raise ModelError(
            "FRAME_HEADER_BAD", f"帧头 CRC-8 不符：算得 {crc8_flac(data[:pos])}"
        )
    pos += 1
    h.channels = ch_code + 1 if ch_code <= 7 else 2  # 三种去相关都是 2 声道
    h.bit_depth = BITS_CODES.get(bits_code, 32)
    h.header_size = pos
    if h.header_size > len(data) - 2:
        raise ModelError("FRAME_HEADER_BAD", "样本区容不下帧尾 2 字节校验")
    return h


def _utf8_coded_number(data: bytes, pos: int) -> tuple[int, int]:
    """扩展 UTF-8 编码号：首字节连续 1 的个数 = 总字节数（2..7），1 之后必须紧跟 0。

    10xxxxxx 单独作首字节非法（续接位形态）。最短形式两侧都判：值越出本档可表达域即不成立，
    落得进更短一档也不成立（库侧 frame.cpp 的 ReadCodedNumber 就是上下两判，量程按档派生）。
    量程只随字节形态走，不随块策略再加第二道界 —— 固定块流的帧号并没有一条自己的上界，
    模型自加的界只会比实码严，把库侧收得下的码流判成坏帧。
    """
    if pos >= len(data):
        raise ModelError("FRAME_HEADER_BAD", "编码号缺首字节")
    b0 = data[pos]
    if b0 < 0x80:
        return b0, 1
    lead = 0
    for bit in range(7, -1, -1):
        if (b0 & (1 << bit)) == 0:
            break
        lead += 1
    if lead < 2 or lead > 7:
        raise ModelError(
            "FRAME_HEADER_BAD", f"编码号首字节 {b0:#02x} 的连续 1 形态非法"
        )
    if len(data) - pos < lead:
        raise ModelError("FRAME_HEADER_BAD", "编码号续接字节不够")
    v = b0 & ((1 << (7 - lead)) - 1)
    for k in range(1, lead):
        b = data[pos + k]
        if (b & 0xC0) != 0x80:
            raise ModelError("FRAME_HEADER_BAD", "编码号续接字节高位不对")
        v = (v << 6) | (b & 0x3F)
    if v > EXT_UTF8_CAPS[lead]:
        raise ModelError(
            "FRAME_HEADER_BAD",
            f"编码号 {v} 越出 {lead} 字节形态的量程 {EXT_UTF8_CAPS[lead]}",
        )
    if v <= EXT_UTF8_CAPS[lead - 1]:
        raise ModelError(
            "FRAME_HEADER_BAD",
            f"编码号 {v} 落得进 {lead - 1} 字节形态的量程：多装了字节（非最短形式）",
        )
    return v, lead


def flac_frame_crc_ok(frame: bytes) -> bool:
    """帧尾 CRC-16 复核：覆盖 [0, len−2)，末 2 字节为大端校验值。"""
    if len(frame) < 3:
        return False
    return crc16_flac(frame[:-2]) == int.from_bytes(frame[-2:], "big")


# ---------------------------------------------------------------- 自检


def selfcheck() -> int:
    lines = []
    # 0) 视频错误码抄件对公开头：现算双向差集，两列都空才算对平。
    #    抄件落后一轮就整体错位，而「查得到名字就算合法」那一类判据会跟着恒真、判据空转，
    #    所以这一项不看人记没记，每次自检都从公开头现解析一遍。
    only_manual, only_header = video_error_code_diff()
    if only_manual or only_header:
        raise SystemExit(
            f"视频错误码抄件与公开头不对平：抄件侧 {only_manual} / 公开头侧 {only_header}"
        )
    header_codes = parse_video_error_codes(video_error_header_path())
    touched = [
        n
        for n in (73, 78, 81, 82)
        if n in header_codes.values() or n in VIDEO_ERROR_CODES.values()
    ]
    if touched:
        raise SystemExit(f"留白号位被列进表了：{touched}")
    if VIDEO_ERROR_CODES["PAYLOAD_LENGTH_MISMATCH"] != max(header_codes.values()):
        raise SystemExit("段尾追加的那一格不在枚举末尾：抄件与头的段位口径不符")
    lines.append(
        f"视频错误码：抄件 {len(VIDEO_ERROR_CODES)} 格 对 公开头现解析 {len(header_codes)} 格，"
        f"双向差集 {len(only_manual)} / {len(only_header)}；"
        f"留白号位 73 / 78 / 81 / 82 两张表都查不到名字（in_enum 判据靠这一格咬住错值）"
    )
    # 1) 舍入式的整数形态与浮点式逐点对拍（覆盖半程两侧与负值）
    bad = 0
    for s in range(-20000, 20001):
        if round_residual_int(s) != round_residual_float(s / 64.0):
            bad += 1
    lines.append(
        f"舍入式整数 / 浮点两形态对拍：s ∈ [−20000, 20000] 共 40001 点，不一致 {bad} 点"
    )
    # 2) 量化：穷举版与捷径版一致（复用件里那两个函数）
    m = 0
    for q in range(1, 128, 5):
        for a in (
            0,
            1,
            2,
            23,
            24,
            35,
            36,
            47,
            48,
            71,
            119,
            1023,
            4095,
            8192,
            16319,
            16320,
        ):
            for s in (1, -1):
                if quantize_value(s * a, q) != quantize_exhaustive(s * a, q):
                    m += 1
    lines.append(f"量化捷径版 / 穷举版对拍：不一致 {m} 点")
    # 3) 位宽档表：本件取档与库侧那三张表（现解析，不抄）逐档一致，且 QF_max 与位宽算式自洽
    tables = width_tables()
    for label, rows, tmax, intra_flag in (
        ("帧内", tables.intra, T_MAX_INTRA, True),
        ("帧间", tables.inter, T_MAX_INTER, False),
    ):
        for row in rows:
            start = row.start
            got_bits, got_cap = (
                bits_per_coef(start, intra_flag),
                qf_window(start, intra_flag),
            )
            scan = max(abs(quantize_value(t, start)) for t in range(-tmax, tmax + 1))
            calc = int(math.ceil(math.log2(scan + 1))) + 1 if scan else 1
            if (got_bits, got_cap, scan, calc) != (
                row.bits,
                row.qf_max,
                row.qf_max,
                row.bits,
            ):
                raise SystemExit(
                    f"{label}表 q'={start}：本件 ({got_bits},{got_cap}) / 库侧 ({row.bits},{row.qf_max}) / 全域扫 {scan}"
                )
    lines.append(
        f"位宽表：从 src/video/block.cpp 现解析 帧内 {len(tables.intra)} 档 + 帧间 "
        f"{len(tables.inter)} 档 + 无损 2 档，位宽与 |QF| 上界逐档与全域扫出的算式自洽"
    )
    # 3b) 无损档（q = 0）：三段域放行、两级偏移都不折档、载体直载 + 反变换逐系数精确复原
    if (
        bits_per_coef(QUANT_LOSSLESS, True) != LOSSLESS_BITS
        or bits_per_coef(QUANT_LOSSLESS, False) != LOSSLESS_BITS
    ):
        raise SystemExit("无损档每系数位宽不是 15")
    if (qf_window(QUANT_LOSSLESS, True), qf_window(QUANT_LOSSLESS, False)) != (
        T_MAX_INTRA,
        T_MAX_INTER,
    ):
        raise SystemExit("无损档的窗口上界不是 T_max 两档")
    if BLOCK_COEFS * LOSSLESS_BITS % 8:
        raise SystemExit("无损档单块不是整字节")
    folded, off_groups = 0, 0
    for d_vop in range(VOP_DQUANT_MIN, VOP_DQUANT_MAX + 1):
        for d_mb in MB_DQUANT_TABLE.values():
            off_groups += 1
            if effective_quant(QUANT_LOSSLESS, d_vop, d_mb, 7) != QUANT_LOSSLESS:
                folded += 1
    if folded:
        raise SystemExit(f"无损流被两级偏移折进有损档：{folded} / {off_groups} 组")
    if effective_quant(1, VOP_DQUANT_MIN, -2, 7) != 1:
        raise SystemExit("有损档的下界夹取不再是 1")
    head_bad = 0
    for bad_q in (quant_ceiling(7) + 1, 200, 255):
        try:
            parse_vop_head(bytes([VOP_I, bad_q, 1, 1, 0, 0]) + encode_varint(1), 0, 7)
            raise SystemExit(f"帧头未判越出三段域的 q：{bad_q}")
        except ModelError as exc:
            if exc.code != E_HEADER_BAD:
                raise
            head_bad += 1
    if (
        parse_vop_head(
            bytes([VOP_I, QUANT_LOSSLESS, 1, 1, 0, 0]) + encode_varint(1), 0, 7
        )[0].quant
        != QUANT_LOSSLESS
    ):
        raise SystemExit("帧头不把 q = 0 当三段域里的无损那一档")
    slice_back, slice_rej = 0, 0
    for vq in (QUANT_LOSSLESS, 1, quant_ceiling(7), quant_ceiling(7) + 1, 200):
        blob = (1).to_bytes(4, "little") + (2).to_bytes(2, "little")
        blob += bytes([0x06, VOP_I, vq])  # 首片：bit2 携带量化 + bit1 首片
        blob += encode_varint(0) + encode_varint(1)
        try:
            got_q = parse_slice_head(blob, 0, 7, True, False).vop_quant
        except ModelError as exc:
            if exc.code != E_SLICE_HEADER_BAD:
                raise
            slice_rej += 1
            continue
        if got_q != vq:
            raise SystemExit(f"片头携带的 vop_quant 读回不符：{vq} != {got_q}")
        slice_back += 1
    if (slice_back, slice_rej) != (3, 2):
        raise SystemExit(
            f"片头量化域判点数量不符：放行 {slice_back} / 判拒 {slice_rej}"
        )
    xs = [
        ((i * 37) % 255) - 127 for i in range(BLOCK_COEFS)
    ]  # 残差 −127..127，不触第三级夹取
    ts = hadamard8(xs)
    if max(abs(t) for t in ts) > T_MAX_INTRA:
        raise SystemExit("无损自检样本的变换幅值越过帧内 T_max")
    lpattern = 0x20
    lrows = [[0] * BLOCK_COEFS for _ in range(BLOCKS_PER_MB)]
    lrows[0] = ts
    lgot = read_present_blocks(
        BitReader(_carrier_bytes(lrows, lpattern, LOSSLESS_MAG_BITS)),
        lpattern,
        QUANT_LOSSLESS,
        True,
        COEF_FIXED,
    )
    if lgot[0] != ts:
        raise SystemExit("无损载体往返不等：直载的变换原值没被原样读回")
    rebuilt = reconstruct_block(
        lgot[0], QUANT_LOSSLESS, True, [INTRA_PRED] * BLOCK_COEFS
    )
    if rebuilt != [INTRA_PRED + x for x in xs]:
        raise SystemExit("无损反变换不精确：重建不等于帧内预测电平 + 原残差")
    lines.append(
        f"无损档（q = 0）：位宽 {LOSSLESS_BITS}、窗口 = T_max 两档、单块 {block_bytes(0, True)} B；"
        f"两级偏移 {off_groups} 组都不折档；帧头判拒越域 {head_bad} 格、片头放行 {slice_back} / "
        f"判拒 {slice_rej}；直载 + 反变换 {BLOCK_COEFS} 系数逐点精确复原"
    )
    # 3c) 一帧完整的无损 I 走过整条读侧（帧头 → 宏块头 → 载体 → 重建），逐点对比残差
    frame_x = [
        ((i * 29) % 201) - 100 for i in range(BLOCK_COEFS)
    ]  # 每块同一份残差，−100..100 不触第三级夹取
    frame_t = hadamard8(frame_x)
    lpattern_all = 0x3F  # 六块全在场
    lrows_all = [list(frame_t) for _ in range(BLOCKS_PER_MB)]
    lbody = _block_bytes(lpattern_all, lrows_all, QUANT_LOSSLESS, True, COEF_FIXED)
    lrow = find_mcbpc_row(VOP_I, MB_TYPE_INTRA, lpattern_all & 0x03)
    lcrow = find_cbpy_row(True, lpattern_all >> 2)
    if lrow is None or lcrow is None:
        raise SystemExit("无损整帧用例的 mcbpc / cbpy 行取不到")
    lbw = BitWriter()
    lbw.write(0, 1)  # not_coded
    lbw.write(lrow[1], lrow[0])
    lbw.write(lcrow[1], lcrow[0])
    lbw.write(0, 2)  # dquant = 00
    lbw.write_varint(len(lbody))
    lbw.write_bytes(lbody)
    lpayload = bytes([VOP_I, QUANT_LOSSLESS, 1, 1, 0, 0]) + encode_varint(1)
    lpayload += lbw.to_bytes()
    ldec = ModelDecoder(MB_SIZE, MB_SIZE, 7, 0)
    lerr, _, ltag = ldec.decode(lpayload, 1, 0, 0)
    if lerr is not None or ltag != "output":
        raise SystemExit(f"无损 I 帧整帧解不出：err={lerr} tag={ltag}")
    lplane = next(iter(ldec.frames.values()))
    for lidx, lpw, lph in (
        (0, MB_SIZE, MB_SIZE),
        (1, MB_SIZE // 2, MB_SIZE // 2),
        (2, MB_SIZE // 2, MB_SIZE // 2),
    ):
        want = [
            INTRA_PRED + frame_x[(y % 8) * 8 + (x % 8)]
            for y in range(lph)
            for x in range(lpw)
        ]
        if list(lplane.planes[lidx]) != want:
            raise SystemExit(f"无损整帧第 {lidx} 平面不等：重建不是预测电平 + 原残差")
    lines.append(
        f"无损整帧：16x16 一宏块的 I 帧（{len(lpayload)} B 载荷）走完帧头 → 宏块头 → 载体 → 重建，"
        "亮度与两个色度平面逐点等于预测电平 128 + 原残差"
    )
    # 4) varint / zigzag 往返
    for v in (0, 1, 127, 128, 432, 16383, 16384, 65535, 2**31, 2**63 - 1):
        if BitReader(encode_varint(v)).read_varint() != v or len(
            encode_varint(v)
        ) != shortest_varint_len(v):
            raise SystemExit(f"varint 往返失败：{v}")
    for v in (-(2**31), -300, -1, 0, 1, 300, 2**31 - 1):
        if BitReader(encode_zigzag(v)).read_zigzag() != v:
            raise SystemExit(f"zigzag 往返失败：{v}")
    lines.append("varint / zigzag 往返：全部命中最短形式")
    # 5) 两张 mcbpc 表与 cbpy 表逐行按自身长度命中：码字集前缀自由，不得被同表他行遮蔽
    checked = 0
    for vt, table in ((VOP_I, MCBPC_I_VOP), (VOP_P, MCBPC_P_VOP)):
        for length, code, mb_type, cbpc in table:
            row = read_vlc(
                BitReader(_pad(code, length)),
                MCBPC_MAX_BITS,
                lambda n, v, t=vt: _find_mcbpc(t, n, v),
            )
            if row != (length, code, mb_type, cbpc):
                raise SystemExit(
                    f"mcbpc 命中错位：帧型 {vt} 码 {length}/{code} 命中 {row}"
                )
            checked += 1
    for length, code, intra_v, inter_v in CBPY_TABLE:
        row = read_vlc(BitReader(_pad(code, length)), CBPY_MAX_BITS, _find_cbpy)
        if row != (length, code, intra_v, inter_v):
            raise SystemExit(f"cbpy 命中错位：码 {length}/{code} 命中 {row}")
        checked += 1
    lines.append(f"mcbpc / cbpy 码表：{checked} 行逐行按自身长度命中，无被同表他行遮蔽")
    # 6) +q 行必须判表外码字（各表只判自己那一张：I 帧表与 P 帧表的码字集互不隶属）
    for vt, rows in ((VOP_I, MCBPC_I_Q_ROWS), (VOP_P, MCBPC_P_Q_ROWS)):
        for length, code, _, _ in rows:
            try:
                read_vlc(
                    BitReader(_pad(code, length)),
                    MCBPC_MAX_BITS,
                    lambda n, v, t=vt: _find_mcbpc(t, n, v),
                )
                raise SystemExit(
                    f"+q 行被命中（应判表外）：帧型 {vt} 码 {length}/{code}"
                )
            except ModelError:
                pass
    lines.append(
        f"+q 行 {len(MCBPC_I_Q_ROWS) + len(MCBPC_P_Q_ROWS)} 条：各按自己那张 mcbpc 表判表外码字"
    )
    # 7) 矢量域截断 vs 右移不等价：−7/4 必须是 −1 而不是 −2
    mf, mbv = direct_scale((-7, 7), 1, 4)
    if mf != (-1, 1) or mbv != (6, -6):
        raise SystemExit(f"direct 截断口径错：{mf} {mbv}")
    # 7b) MVB 的被减数是未夹饱和的缩放值（先减后夹，与 DirectScale 同序）。
    #     两判分开放：① 合法时间权重下缩放值恒落在 32 位域内，饱和收口点触不到，
    #     于是先减后夹与先夹后减在可达域内同值 ⇒ 这一格对齐不动任何期望值；
    #     ② 拿一个越出 32 位域的 MV_ref 当构造输入，两种写法就此分叉，
    #     这一例才是「按库侧口径改」的判别读数（实跑复现由主窗口的用例给）。
    sat_hits, probed = 0, 0
    for v in (INT32_MIN, INT32_MIN + 1, -7, 0, 7, INT32_MAX - 1, INT32_MAX):
        for trb, trd in ((0, 1), (1, 1), (1, 4), (3, 4), (7, 8), (1, 3), (127, 127)):
            scaled = trunc_div(trb * v, trd)
            probed += 1
            if scaled != saturate_component(scaled):
                sat_hits += 1
            got_f, got_b = direct_scale((v, 0), trb, trd)
            if got_f != (saturate_component(scaled), 0) or got_b != (
                saturate_component(scaled - v),
                0,
            ):
                raise SystemExit(f"direct 分量口径不符：v={v} trb={trb} trd={trd}")
    if sat_hits:
        raise SystemExit(f"合法时间权重下竟触到饱和收口：{sat_hits} / {probed} 组")
    probe_f, probe_b = direct_scale(
        (2**31, 0), 2, 2
    )  # 越出 32 位域的构造输入，只为让两支分叉
    if probe_f != (INT32_MAX, 0) or probe_b != (0, 0):
        raise SystemExit(f"direct 的 MVB 还拿夹过的 MVF 作被减数：{probe_f} {probe_b}")
    lines.append(
        f"direct：向零截断（−7 ÷ 4 = −1，右移给 −2）；域内 {probed} 组饱和收口零触发（先减后夹"
        f"与先夹后减同值），越域构造例 {(INT32_MAX, 0)} / {(0, 0)} 坐实 MVB 用未夹的缩放值"
    )
    # 8) 补后尺寸与位宽算式的整字节性（无损那一档同一条算式）
    for q in range(0, 256):
        for intra in (True, False):
            if BLOCK_COEFS * bits_per_coef(q, intra) % 8:
                raise SystemExit(f"单块非整字节：q'={q} intra={intra}")
    lines.append("q' = 0..255（含无损档）两表：64 × bits(q') 恒为 8 的倍数")
    # 9) 载体往返：写读同轴、逐系数复原；幅值域比窗口宽的那些档，越窗例必须判取值不成立
    rt, over = 0, 0
    for q in (1, 12, 24, 33, 65, 127, 200):
        for intra in (True, False):
            bits, cap = bits_per_coef(q, intra), qf_window(q, intra)
            mag_bits = bits - 1
            pattern = 0x3F if (q % 2) else 0x15
            rows = []
            for idx in range(BLOCKS_PER_MB):
                if (pattern & (1 << (5 - idx))) == 0:
                    rows.append([0] * BLOCK_COEFS)
                    continue
                rows.append(
                    [
                        (
                            cap
                            if (k + idx) % 3 == 0
                            else (-(cap // 2) if (k + idx) % 3 == 1 else 1)
                        )
                        for k in range(BLOCK_COEFS)
                    ]
                )
            got = read_present_blocks(
                BitReader(_carrier_bytes(rows, pattern, mag_bits)),
                pattern,
                q,
                intra,
                COEF_FIXED,
            )
            if got != rows:
                raise SystemExit(f"载体往返不等：q'={q} intra={intra}")
            rt += 1
            if (1 << mag_bits) - 1 > cap:  # 幅值域宽于窗口的那些档才谈得上越窗
                bad = [r[:] for r in rows]
                first = 5 - next(b for b in range(5, -1, -1) if pattern & (1 << b))
                bad[first][0] = (1 << mag_bits) - 1  # 幅值域上界：本档它严格大于窗口
                over += 1
                try:
                    read_present_blocks(
                        BitReader(_carrier_bytes(bad, pattern, mag_bits)),
                        pattern,
                        q,
                        intra,
                        COEF_FIXED,
                    )
                    raise SystemExit(f"越窗未判拒：q'={q} intra={intra}")
                except ModelError as exc:
                    if exc.code != E_VALUE_RANGE:
                        raise
    lines.append(
        f"载体往返：{rt} 组（q' × 帧内 / 帧间）逐系数复原；越窗 {over} 组全部判取值不成立"
    )
    # 9b) 法 1 载体往返 + 四条判据：EG 码长算式、all_zero 合法、eob 位为零 / 越窗判拒、封口上界恒宽于定宽
    eg_bad = 0
    for v in (0, 1, 2, 3, 4, 7, 8, 15, 16, 255, 4095, 8159, 8160, (1 << 24) - 1):
        bw = BitWriter()
        write_eg(bw, v)
        n = len(bw.bits)  # 必须在补齐之前取，否则量到的是补后的长度
        if (
            read_eg(BitReader(bw.to_bytes())) != v
            or n != eg_bits(v)
            or n != 2 * (v + 1).bit_length() - 1
        ):
            eg_bad += 1
    if eg_bad:
        raise SystemExit(f"EG 往返或码长算式不符：{eg_bad} 点")
    prefix_rej = 0
    z = BitWriter()
    z.write(0, EG_MAX_PREFIX + 1)  # 前导零到上限之后还来 0 = 读不尽
    z.write(1, 1)
    try:
        read_eg(BitReader(z.to_bytes()))
        raise SystemExit("EG 前导零上限未判拒")
    except ModelError as exc:
        if exc.code != E_PAYLOAD_TOO_SMALL:
            raise
        prefix_rej += 1
    rt1, rej1, zero_blocks = 0, 0, 0
    for q in (1, 12, 24, 33, 65, 127, 200):
        for intra in (True, False):
            cap = qf_window(q, intra)
            pattern = 0x3F if (q % 2) else 0x15
            rows = []
            for idx in range(BLOCKS_PER_MB):
                if (pattern & (1 << (5 - idx))) == 0:
                    rows.append([0] * BLOCK_COEFS)
                    continue
                if (idx + q) % 3 == 0:  # 在场且整块全零：本法把它写成合法码字
                    rows.append([0] * BLOCK_COEFS)
                    zero_blocks += 1
                    continue
                rows.append(
                    [
                        (
                            cap
                            if k == 0
                            else (
                                -(cap // 2)
                                if k == 1
                                else (0 if k > (q % 40) + 2 else 1)
                            )
                        )
                        for k in range(BLOCK_COEFS)
                    ]
                )
            body = _block_bytes(pattern, rows, q, intra, COEF_EOB_EG)
            n = present_block_count(pattern)
            up = n * block_bytes_max(q, intra, COEF_EOB_EG)
            if not (0 < len(body) <= up if n else len(body) == 0):
                raise SystemExit(
                    f"块数据长度越出封口上界：q'={q} intra={intra} {len(body)} > {up}"
                )
            br = BitReader(body, 0, end_bit=len(body) * 8)
            got = read_present_blocks(br, pattern, q, intra, COEF_EOB_EG)
            if got != rows or br.bits_left() >= 8:
                raise SystemExit(
                    f"法 1 载体往返不等或剩余字节未清：q'={q} intra={intra}"
                )
            rt1 += 1
            # 拒绝一：eob 位上的系数为零（与 eob 的定义自相矛盾）
            bad = BitWriter()
            bad.write(0, 1)
            bad.write(3, EOB_FIELD_BITS)
            write_eg(bad, 5)
            bad.write(0, 1)
            write_eg(bad, 0)
            write_eg(bad, 0)
            write_eg(bad, 0)
            # 块尾补齐到整字节，与写方同一条口径
            bad.align()
            try:
                read_present_blocks(
                    BitReader(bad.to_bytes(), 0, end_bit=len(bad.to_bytes()) * 8),
                    0x20,
                    q,
                    intra,
                    COEF_EOB_EG,
                )
                raise SystemExit(f"eob 位为零未判拒：q'={q}")
            except ModelError as exc:
                if exc.code != E_VALUE_RANGE:
                    raise
                rej1 += 1
            # 拒绝二：level 越出窗口上界
            bad2 = BitWriter()
            bad2.write(0, 1)
            bad2.write(0, EOB_FIELD_BITS)
            write_eg(bad2, cap + 1)
            bad2.write(0, 1)
            bad2.align()
            try:
                read_present_blocks(
                    BitReader(bad2.to_bytes(), 0, end_bit=len(bad2.to_bytes()) * 8),
                    0x20,
                    q,
                    intra,
                    COEF_EOB_EG,
                )
                raise SystemExit(f"level 越窗未判拒：q'={q}")
            except ModelError as exc:
                if exc.code != E_VALUE_RANGE:
                    raise
                rej1 += 1
    narrower = sum(
        1
        for intra in (True, False)
        for q in range(0, 256)
        if block_bits_max(q, intra, COEF_EOB_EG)
        <= BLOCK_COEFS * bits_per_coef(q, intra)
    )
    if narrower:
        raise SystemExit(f"封口上界不比定宽宽的档：{narrower} 档")
    lines.append(
        f"法 1 载体：EG 往返 14 点 + 前导零上限 {prefix_rej} 判拒；往返 {rt1} 组逐系数复原"
        f"（含 all_zero {zero_blocks} 块）；拒绝 {rej1} 组（eob 位为零 / level 越窗）；"
        f"q' = 0..255（含无损档）封口上界恒宽于定宽（反例 {narrower} 档）"
    )
    # 10) FLAC 校验式与公开例子对拍：外部文档附录用那条一采样帧的帧头给的样例
    # 头六字节 = FF F8 69 18 00 00（块大小码 0b0110 → 8 位非常规、采样率码 0b1001 = 44100、
    # 声道码 0b0001、位深码 0b100、帧号 0、非常规块大小存 0 → 块大小 1），其后 CRC-8 = 0xBF
    rfc_hdr = bytes.fromhex("FFF869180000")
    if crc8_flac(rfc_hdr) != 0xBF:
        raise SystemExit(f"帧头 CRC-8 与附录样例不符：{crc8_flac(rfc_hdr):#04x}")
    if crc8_flac(b"123456789") != 0xF4:
        raise SystemExit("CRC-8 与多项式 0x07 的标准 check 值不符")
    rh = parse_flac_head(
        rfc_hdr
        + bytes([0xBF])
        + b"\x00\x00"
        + crc16_flac(rfc_hdr + bytes([0xBF]) + b"\x00\x00").to_bytes(2, "big")
    )
    if (rh.blocksize, rh.sample_rate, rh.channels, rh.bit_depth, rh.header_size) != (
        1,
        44100,
        2,
        16,
        7,
    ):
        raise SystemExit(f"附录样例帧头解析不符：{rh}")
    lines.append(
        "FLAC：CRC-8 命中附录样例(0xBF)与标准 check(0xF4)；该帧头逐字段解析一致"
    )
    lines.append(
        "FLAC：CRC-16 的位反射口径只由本件自检与帧头解析自核（无对照管线与之逐帧对账）"
    )
    # 11) 帧头层合法 / 非法分档：块大小码 0b1111 → 32768 在帧头层必须过，越的是子集层
    ok = _flac_head(bs_code=0x0F, sr_code=10, ch_code=1, bits_code=4)
    h = parse_flac_head(ok)
    if (h.blocksize, h.sample_rate, h.channels, h.bit_depth) != (32768, 48000, 2, 16):
        raise SystemExit(f"块大小码 0b1111 解析错：{h}")
    bad_hits = 0
    good = _flac_head(bs_code=8, sr_code=10, ch_code=1, bits_code=4)
    cases = [
        _flac_head(bs_code=0, sr_code=10, ch_code=1, bits_code=4),  # 块大小表 Reserved
        _flac_head(bs_code=1, sr_code=15, ch_code=1, bits_code=4),  # 采样率表 Forbidden
        _flac_head(
            bs_code=1, sr_code=10, ch_code=11, bits_code=4
        ),  # 声道表 Reserved 起端
        _flac_head(bs_code=1, sr_code=10, ch_code=1, bits_code=3),  # 位深表 Reserved
        good[:3] + bytes([good[3] | 1]) + good[4:],  # byte3 的位深后保留位
        bytes([good[0], good[1] | 2]) + good[2:],  # 同步码尾位
        good[:5] + bytes([good[5] ^ 0x01]) + good[6:],  # 帧头 CRC-8 不符
        good[: len(good) - 1] + bytes([good[-1] ^ 0x01]),  # 帧尾 CRC-16 不符
        _flac_head(
            bs_code=1, sr_code=10, ch_code=1, bits_code=4, number_bytes=b"\xff"
        ),  # 编码号首字节 0xFF（连续 1 达 8 位）
        _flac_head(
            bs_code=1, sr_code=10, ch_code=1, bits_code=4, number_bytes=b"\x80"
        ),  # 编码号首字节落在续接位形态 0x80..0xBF
        _flac_head(
            bs_code=1, sr_code=10, ch_code=1, bits_code=4, number_bytes=b"\xc1\xff"
        ),  # 两字节形态写 0x7F：单字节档装得下 = 非最短形式
    ]
    for blob in cases:
        rejected = False
        try:
            parse_flac_head(blob)
        except ModelError as exc:
            rejected = exc.code == "FRAME_HEADER_BAD"
        if not flac_frame_crc_ok(blob):
            rejected = True
        if not rejected:
            raise SystemExit(f"帧头层非法值未判拒：{blob.hex()}")
        bad_hits += 1
    if not flac_frame_crc_ok(good):
        raise SystemExit("自核把合法帧判成坏帧")
    lines.append(
        f"帧头层非法值：{bad_hits} / {len(cases)} 例逐条判拒（两张码表的 Reserved / Forbidden、"
        "两个保留位、两条 CRC、编码号三种形态）"
    )
    # 12) 编码号的量程与最短形式：逐档核掩码恰好铺满本档、下界判必咬、合法形态不误判，
    #     并且固定块流不再吃那道模型自加的 31 位界（帧号按形态自己的量程走）
    caps_bad = [
        lead
        for lead in range(2, 8)
        if EXT_UTF8_CAPS[lead]
        != (((1 << (7 - lead)) - 1) << (6 * (lead - 1))) | ((1 << (6 * (lead - 1))) - 1)
    ]
    # 单字节档是 0xxxxxxx，共 7 位载荷，不走上面那条式（那条式只对 lead ≥ 2 成立）。
    if EXT_UTF8_CAPS[1] != (1 << 7) - 1:
        caps_bad.append(1)
    if caps_bad:
        raise SystemExit(f"编码号量程表与逐档载荷位宽算式不符：{caps_bad}")
    top_ok = 0
    for lead in range(1, 8):
        v, used = _utf8_coded_number(
            _utf8_number_bytes((1 << 40) - 1, lead) + b"\x00", 0
        )
        if (v, used) != (EXT_UTF8_CAPS[lead], lead):
            raise SystemExit(f"本档掩码没铺满量程：lead={lead} 读到 {v} 用了 {used}")
        top_ok += 1
    rej_lead, ok_lead = 0, 0
    for lead in range(2, 8):
        over = _utf8_number_bytes(EXT_UTF8_CAPS[lead - 1], lead) + b"\x00"
        try:
            _utf8_coded_number(over, 0)
            raise SystemExit(f"{lead} 字节形态写更短一档的值却未判拒")
        except ModelError as exc:
            if exc.code != "FRAME_HEADER_BAD":
                raise
            rej_lead += 1
        shortest = EXT_UTF8_CAPS[lead - 1] + 1
        blob = _utf8_number_bytes(shortest, lead) + b"\x00"
        if _utf8_coded_number(blob, 0) != (shortest, lead):
            raise SystemExit(f"{lead} 字节形态的下边界值读回不符")
        ok_lead += 1
    n7 = _utf8_number_bytes(
        0x1_0000_0000, 7
    )  # 7 字节形态写 2^32：越出 31 位，但落在本档量程
    big_fixed = _flac_head(
        bs_code=8, sr_code=10, ch_code=1, bits_code=4, number_bytes=n7
    )
    bh = parse_flac_head(big_fixed)
    if bh.coded_number != 0x1_0000_0000 or bh.blocking_strategy != 0:
        raise SystemExit(f"固定块流的 7 字节编码号被误判：{bh.coded_number}")
    over_frame = _flac_head(
        bs_code=8, sr_code=10, ch_code=1, bits_code=4, number_bytes=b"\xc1\xff"
    )
    try:
        parse_flac_head(over_frame)
        raise SystemExit("非最短形式的编码号在整帧路径上未判拒")
    except ModelError as exc:
        if exc.code != "FRAME_HEADER_BAD":
            raise
    lines.append(
        f"编码号：量程表 {top_ok} 档逐档与掩码铺满值同值；最短形式判拒 {rej_lead} 档 / "
        f"放行 {ok_lead} 档；固定块流的 7 字节 2^32 帧号放行（模型不随块策略加界），"
        "整帧路径上的两字节 0x7F 判拒"
    )
    print("\n".join(lines))
    return 0


def _carrier_bytes(rows: list, pattern: int, mag_bits: int) -> bytes:
    """按位图顺序把在场块的系数定长直写成字节（自检与语料生成共用那一条写式）。"""
    bw = BitWriter()
    for bit in range(5, -1, -1):
        if (pattern & (1 << bit)) == 0:
            continue
        for v in rows[5 - bit]:
            bw.write(1 if v < 0 else 0, 1)
            bw.write(abs(v), mag_bits)
    return bw.to_bytes()


def _pad(code: int, length: int) -> bytes:
    """把码字放到字节最高位一侧，后面补一整字节零，供码表逐位匹配用（读到 9 位也有的读）。"""
    bw = BitWriter()
    bw.write(code, length)
    bw.write(0, 8)
    bw.align()
    return bw.to_bytes()


def _utf8_number_bytes(value: int, lead: int) -> bytes:
    """按 lead 个字节的形态写出编码号（只为造读方的例，不进期望值文件）。

    lead = 1 是 0xxxxxxx 共 7 位载荷；lead ≥ 2 时首字节 = lead 个前导 1 再接 (7 − lead) 位载荷，
    续字节各带 6 位。高过本档位宽的部分被掩掉，所以这一式写不出「越出本档量程」的字节序列
    —— 量程判据归读方，写方只负责形态。
    """
    if lead == 1:
        return bytes([value & EXT_UTF8_CAPS[1]])
    tail = bytearray()
    v = value
    for _ in range(lead - 1):
        tail.insert(0, (v & 0x3F) | 0x80)
        v >>= 6
    first = ((0xFF << (8 - lead)) & 0xFF) | (v & ((1 << (7 - lead)) - 1))
    return bytes(bytearray([first]) + tail)


def _flac_head(
    bs_code=8,
    sr_code=10,
    ch_code=1,
    bits_code=4,
    number=0,
    blocking=0,
    body=b"\x00\x00",
    number_bytes=None,
) -> bytes:
    """按帧头位域拼一条帧：只服务自检与语料生成，字段取值由调用方给。

    number_bytes 给定时原样写入编码号那一段（造非最短形态与多字节形态用），此时 number 不生效。
    """
    out = bytearray(
        [
            FLAC_SYNC_HIGH,
            0xF8 | blocking,
            (bs_code << 4) | sr_code,
            (ch_code << 4) | (bits_code << 1),
        ]
    )
    if number_bytes is not None:
        out += number_bytes
    elif number < 0x80:
        out.append(number)
    else:
        out.append(0xC0 | (number >> 6))
        out.append(0x80 | (number & 0x3F))
    if bs_code in (6, 7):
        extra = 1 if bs_code == 6 else 2
        out += (0).to_bytes(extra, "big")
    if sr_code in (12, 13, 14):
        extra = 1 if sr_code == 12 else 2
        out += (1).to_bytes(extra, "big")
    out.append(crc8_flac(bytes(out)))
    frame = bytes(out) + body
    return frame + crc16_flac(frame).to_bytes(2, "big")


def main() -> int:
    ap = argparse.ArgumentParser(description="交叉语料档的独立模型层自检")
    ap.add_argument(
        "--selfcheck", action="store_true", help="跑模型自检（不写任何文件）"
    )
    args = ap.parse_args()
    if not args.selfcheck:
        ap.print_help()
        return 0
    configure_streams()  # 输出流的编码由入口自己定，import 本件的宿主进程不受影响
    return selfcheck()


if __name__ == "__main__":
    sys.exit(main())
