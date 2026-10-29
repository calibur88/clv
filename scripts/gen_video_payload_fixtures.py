#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 tests/win32/video/video_payload_fixtures_p.h —— 块系数载体的冻结期望件。

期望载体**不得由被测函数现算**。本件按 `src/video/block.h` 文件头那段载体口径（「块数据 =
量化后变换系数的载体」，声明 `BlockBits` / `BlockQfMax` / `BlockBytes` 的同一处）独立重写一遍
整条链路，再冻成字面量：

    取 8×8 块（行主序）→ 减帧内预测电平 128 → T = H X Hᵀ（8×8 Sylvester Hadamard，
    元素 ±1、全程整数、不归一，这里按矩阵乘法直算，与 C++ 侧的蝶形不是同一写法）
    → 最近电平量化（电平集 {0} ∪ {±((2n+1)q' − δ)}，δ = 0（q' 奇）/ 1（q' 偶），
      码流里存的 QF 是电平序号 n，故 n 相同时只差一个符号；q' = 0 是无损档，量化是恒等映射）
    → 定长档直写：每系数 bits(q') 位 = 1 位符号（0 正 1 负）+ bits(q') − 1 位幅值，
      MSB 先出，不做零省略，块与块之间无填充。法 1 档（EOB+EG）的打包见 pack_blocks_eob_eg。

系数位宽档表的权威只有一份，就是 `src/video/block.cpp` 里的 `kIntraRows` / `kInterRows` 与无损
那一档的 `kLosslessRows`：本件把这三张表连同 `src/video/block.h`、`src/video/quant.h`、
`src/video/reconstruct.h` 里的具名常数一起解析进来当取值来源，同时按
`|QF| 上界 = max over |T| ≤ T_max 的 |QuantizeValue(T, q')|`（T_max 帧内 = 系数个数 × 预测电平、
帧间 = 系数个数 × 满幅残差）与 `bits(q') = ⌈log2(QF_max + 1)⌉ + 1` 独立另扫一遍，逐档与库侧核对，
对不上就直接失败退出。冻进头件的位宽取自库侧那一张表，独立扫掠是给这张表上的等式闸门——
产物头注释写的也是这一点，不写成「独立算式的输出」。

只用标准库。仓内只读上面那几个 `src/video/` 的表与常数，另读 `tests/win32/video/`
`video_quant_fixtures_p.h` 取界扫掠的锚（旁件不在场即失败退出，绝不静默少发一格）；仓库外不读
任何文件，重跑逐字节同值。

    python scripts/gen_video_payload_fixtures.py
    生成后跑 scripts/format_all.py --write（产出的数组要按仓库 clang-format 归位）
"""

from __future__ import annotations

import argparse
import math
import pathlib
import re
import sys
from dataclasses import dataclass

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCRIPT_DIR = ROOT / "scripts"
OUT = ROOT / "tests/win32/video/video_payload_fixtures_p.h"
QUANT_FIXTURES = ROOT / "tests/win32/video/video_quant_fixtures_p.h"

# 档表与具名常数的来源件：这里只读它们的文本取数，不调用库、不跑库。
BLOCK_H = ROOT / "src/video/block.h"
BLOCK_CPP = ROOT / "src/video/block.cpp"
QUANT_H = ROOT / "src/video/quant.h"
RECONSTRUCT_H = ROOT / "src/video/reconstruct.h"
CONST_SOURCES = (BLOCK_H, BLOCK_CPP, QUANT_H, RECONSTRUCT_H)

# ---- 载体口径（src/video/block.h 文件头那一段；下列每一格都在 main 里与库侧同名常数核一遍）----
INTRA_PRED = 128  # 帧内块的预测电平，变换输入 = 像素 − 128
COEFS = 64  # 一块的系数个数
T_MAX_INTRA = COEFS * INTRA_PRED  # 帧内变换域上界：64 × 128
T_MAX_INTER = COEFS * 255  # 帧间变换域上界：残差 ∈ [−255, 255]
BLOCKS_PER_MB = 6  # 一宏块的块数：4 个亮度块 + Cb + Cr
LUMA_PATTERN_SHIFT = 2  # cbpy = 块位图 >> 2（4 bit 亮度位图）
CHROMA_PATTERN_MASK = 0x03  # cbpc = 块位图 & 3（2 bit 色度位图）
Q_LOSSLESS = 0  # 无损档：量化步长字段里的 0，不进任何量化式
EOB_FIELD_BITS = int(math.log2(COEFS))  # eob 字段宽：下标 0..63 正好铺满
Q_PRIME = 24  # 两处载体用例都走 q' = 24

# ---- 两个用例的输入几何（宏块数由尺寸推出来，不手抄）----
MB_SIDE = 16
STRIDE_ALIGN = 32  # 可写平面的行距对齐（4:2:0 的色度宽度取亮度的一半）
ZERO_FRAME_W = 176
ZERO_FRAME_H = 144
ZERO_FRAME_MBS = (ZERO_FRAME_W // MB_SIDE) * (ZERO_FRAME_H // MB_SIDE)  # 11 × 9
PLANE16_SIDE = 16
PLANE16_MBS = (PLANE16_SIDE // MB_SIDE) * (PLANE16_SIDE // MB_SIDE)

# 发成常量的格子由用例的断言面决定：块位图本身、以及逐块 DC 与在场块计数里没人读的那几格，
# 都只写进注释作人读对照，不占期望值位（现查 tests/ 与 src/，读得到才发）。
ZERO_FRAME_DC_BLOCKS = (0,)
ZERO_FRAME_PRESENT_COUNTS = True
PLANE16_DC_BLOCKS: tuple[int, ...] = ()
PLANE16_PRESENT_COUNTS = False

CONST_DEF_RE = re.compile(
    r"constexpr\s+[A-Za-z_]\w*(?:<[^>]*>)?\s+(?P<name>\w+)\s*=\s*(?P<expr>[^;]+);"
)
ROW_TABLE_RE = re.compile(
    r"(?P<name>\w+)\s*\[\s*\d+\s*\]\s*=\s*\{(?P<body>.*?)\n\s*\};", re.S
)
CELL_RE = re.compile(r"\{\s*([^,{}]+?)\s*,\s*([^,{}]+?)\s*,\s*([^,{}]+?)\s*\}")
TABLE_NAMES = ("kIntraRows", "kInterRows", "kLosslessRows")


@dataclass(frozen=True)
class WidthRow:
    """一档：q' 起点（含）、每系数位宽（含 1 bit 符号）、|QF| 上界。"""

    start: int
    bits: int
    qf_max: int


class CarrierTables:
    """库侧三张位宽档表的解析结果；定长档查位宽与上界都只走这里。"""

    def __init__(
        self, intra: list[WidthRow], inter: list[WidthRow], lossless: list[WidthRow]
    ):
        self.intra = intra
        self.inter = inter
        # 无损那两行的先后按 src/video/block.cpp 的 Pick 取档下标：0 = 帧内、1 = 帧间。
        self.lossless = {True: lossless[0], False: lossless[1]}

    def rows(self, intra: bool) -> list[WidthRow]:
        return self.intra if intra else self.inter

    def row_of(self, q: int, intra: bool) -> WidthRow:
        """q' = 0 走无损那一档（恒等直载），q' ≥ 1 取起点不超过它的最后一档。"""
        if q == Q_LOSSLESS:
            return self.lossless[intra]
        if q < Q_LOSSLESS:
            raise SystemExit(
                f"bits_of: q'={q} is out of domain (0 = lossless, 1.. = lossy)"
            )
        picked = self.rows(intra)[0]
        for row in self.rows(intra):
            if row.start <= q:
                picked = row
        return picked

    def bits_of(self, q: int, intra: bool) -> int:
        return self.row_of(q, intra).bits


def constant_resolver(sources: tuple[pathlib.Path, ...]):
    """把仓内具名 constexpr 常数解析成 name -> int；解析不出或表达式算不了就当场失败。

    库侧写法是 `constexpr <type> name = <整数表达式>;`，表达式里还可以再引用同名常数
    （`kCoefClampMaxIntra8` 就写成系数个数 × 变换输入幅度上界），故按定义逐层代入后再算。
    """
    defs: dict[str, str] = {}
    for path in sources:
        if not path.is_file():
            raise SystemExit(f"authority source is not readable: {path}")
        text = path.read_text(encoding="utf-8")
        for found in CONST_DEF_RE.finditer(text):
            defs.setdefault(found.group("name"), found.group("expr"))
    resolved: dict[str, int] = {}

    def normalize(expr: str) -> str:
        text = re.sub(r"static_cast<[^>]+>\s*", "", expr)
        return re.sub(r"(0[xX][0-9A-Fa-f]+|\d)[uUlL]+(?![0-9A-Za-z_])", r"\1", text)

    def value_of(name: str, trail: tuple[str, ...] = ()) -> int:
        if name in resolved:
            return resolved[name]
        if name in trail:
            raise SystemExit(
                "constant definition cycle: " + " -> ".join(trail + (name,))
            )
        if name not in defs:
            raise SystemExit(f"constant not found in repo sources: {name}")
        text = normalize(defs[name])

        def sub(found: re.Match[str]) -> str:
            token = found.group(0)
            if token[:2] in ("0x", "0X"):
                return str(int(token, 16))
            return str(value_of(token, trail + (name,)))

        text = re.sub(r"0[xX][0-9A-Fa-f]+|[A-Za-z_]\w*", sub, text)
        if not re.fullmatch(r"[0-9\s+\-*/%()<>]+", text):
            raise SystemExit(
                f"constant expression is not evaluable: {name} = {defs[name]}"
            )
        value = int(eval(text, {"__builtins__": {}}, {}))  # noqa: S307
        resolved[name] = value
        return value

    return value_of


def read_width_tables(value_of) -> CarrierTables:
    """从 src/video/block.cpp 把 kIntraRows / kInterRows / kLosslessRows 读成三张表。

    这三张表是位宽档的唯一权威；读不全就失败退出，不退回件内手抄的一份。
    """
    text = BLOCK_CPP.read_text(encoding="utf-8")
    rows: dict[str, list[WidthRow]] = {}
    for found in ROW_TABLE_RE.finditer(text):
        name = found.group("name")
        if name not in TABLE_NAMES:
            continue
        rows[name] = [
            WidthRow(*(cell_value(cell, value_of) for cell in cells))
            for cells in CELL_RE.findall(found.group("body"))
        ]
    missing = [name for name in TABLE_NAMES if name not in rows]
    if missing:
        raise SystemExit(f"width tables not found in src/video/block.cpp: {missing}")
    for name, table in rows.items():
        if not table:
            raise SystemExit(f"width table is empty: {name}")
    for row in rows["kLosslessRows"]:
        if row.start != Q_LOSSLESS:
            raise SystemExit(
                f"lossless row must start at {Q_LOSSLESS}, table gives {row.start}"
            )
    if len(rows["kLosslessRows"]) != 2:
        raise SystemExit(
            f"lossless table must hold two rows (intra / inter), got {len(rows['kLosslessRows'])}"
        )
    return CarrierTables(rows["kIntraRows"], rows["kInterRows"], rows["kLosslessRows"])


def cell_value(token: str, value_of) -> int:
    token = token.strip()
    if re.fullmatch(r"-?\d+[uU]?", token):
        return int(token.rstrip("uU"))
    return value_of(token)


def bind_library_constants(value_of) -> None:
    """件内的几何常数必须与库侧同名常数同值，否则本件算出的载体与库侧写的不是同一口径。"""
    pairs = (
        ("COEFS", "kBlockCoefs", COEFS),
        ("INTRA_PRED", "kIntraPredLevel8", INTRA_PRED),
        ("BLOCKS_PER_MB", "kBlocksPerMacroblock", BLOCKS_PER_MB),
        ("LUMA_PATTERN_SHIFT", "kLumaPatternShift", LUMA_PATTERN_SHIFT),
        ("CHROMA_PATTERN_MASK", "kChromaPatternMask", CHROMA_PATTERN_MASK),
        ("Q_LOSSLESS", "kQuantLossless", Q_LOSSLESS),
        ("EOB_FIELD_BITS", "kEobFieldBits", EOB_FIELD_BITS),
        ("T_MAX_INTRA", "kCoefClampMaxIntra8", T_MAX_INTRA),
        ("T_MAX_INTER", "kCoefClampMaxInter8", T_MAX_INTER),
    )
    for local_name, repo_name, value in pairs:
        got = value_of(repo_name)
        if got != value:
            raise SystemExit(
                f"{local_name} = {value} but {repo_name} in the library is {got}; "
                "this fixture must follow the library, not a hand copy"
            )


def load_lcg():
    """取量化对拍生成件的那一份 LCG 参数与种子，本件不再抄第二份。

    搜索路径只在这里改，改完就取数：模块导入时不动全局状态，别的件 import 本件不会被牵连。
    """
    if str(SCRIPT_DIR) not in sys.path:
        sys.path.insert(0, str(SCRIPT_DIR))
    try:
        import gen_video_quant_fixtures as sibling  # noqa: PLC0415
    except (
        ImportError
    ) as exc:  # 拿不到就失败退出，回落到件内自己的一份参数等于又是第二处手抄
        raise SystemExit(
            f"cannot import the LCG source from scripts/gen_video_quant_fixtures.py: {exc}"
        )
    return list(sibling.SEEDS), sibling.lcg_block


def delta_of(q: int) -> int:
    """δ = 0（q 奇）/ 1（q 偶）——电平集里唯一的偏移项。"""
    return 0 if q & 1 else 1


def level_of(n: int, q: int) -> int:
    """电平序号 n 的幅度：n ≤ 0 是零电平，否则 (2n + 1) q − δ。"""
    if n <= 0:
        return 0
    return (2 * n + 1) * q - delta_of(q)


def hadamard8(x: list[int]) -> list[int]:
    """T = H X Hᵀ，H 是 8×8 Sylvester Hadamard：H[i][j] = (−1)^{popcount(i & j)}。

    按定义做两次矩阵乘法，刻意不用蝶形 / 原位 fast WHT —— 与 C++ 侧不同写法才谈得上独立。
    """
    h = [[1 - 2 * ((i & j).bit_count() & 1) for j in range(8)] for i in range(8)]
    xm = [x[r * 8 : r * 8 + 8] for r in range(8)]
    # A = H X
    a = [
        [sum(h[i][k] * xm[k][j] for k in range(8)) for j in range(8)] for i in range(8)
    ]
    # T = A Hᵀ = A H（H 对称）
    t = [[sum(a[i][k] * h[j][k] for k in range(8)) for j in range(8)] for i in range(8)]
    return [t[r][c] for r in range(8) for c in range(8)]


def check_quant_domain(q: int, where: str) -> None:
    """量化步长的合法域只有两段：0 = 无损、1..= 有损。域外一律当场失败。

    库侧 src/video/quant.cpp 明文不给「0 折成 1」的退路——折了就把无损请求悄悄编成最细的
    有损档；本件跟着折，两侧就同错，对拍也就白对。
    """
    if q < Q_LOSSLESS:
        raise SystemExit(
            f"{where}: q'={q} is out of domain (0 = lossless, 1.. = lossy)"
        )


def quantize_exhaustive(coef: int, q: int) -> int:
    """取最近电平：把 |coef| 到各电平的距离逐个比过去，平局取序号小的（= 先出现的电平）。

    穷举而不是候选窗，界外也多枚举一档，保证「最近」由定义给出而非由实现推定。
    q' = 0 是无损档：量化是恒等映射，系数原值直载，不进电平集（与库侧同一支）。
    """
    check_quant_domain(q, "quantize_exhaustive")
    if q == Q_LOSSLESS:
        return coef
    a = abs(coef)
    best_n, best_err = 0, a  # 零电平先入最佳
    n = 0
    while True:
        n += 1
        lv = level_of(n, q)
        if lv - a > best_err:
            break  # 电平已越过误差窗，再往后的只会更远
        err = abs(a - lv)
        if err < best_err:
            best_err, best_n = err, n
    if best_n == 0:
        return 0
    return -best_n if coef < 0 else best_n


def quantize_value(coef: int, q: int) -> int:
    """quantize_exhaustive 的等价款：解 n ≈ (|coef| + δ − q) / 2q 后比四个候选电平。

    候选枚举的是 n0 − 1 / n0 / n0 + 1 / n0 + 2 这四档，零只是初值不是候选；电平集对 n 单调增，
    故「最近」必落在这一窗里，多出来的一档留给 n0 的取整偏差。全域扫表要用它，穷举版只在
    样本上与它比对（两版一致才认这条捷径）。
    q' = 0 是无损档：恒等映射，与库侧 src/video/quant.cpp 的同一支逐字对称。
    """
    check_quant_domain(q, "quantize_value")
    if q == Q_LOSSLESS:
        return coef
    a = abs(coef)
    d = delta_of(q)
    if a == 0:
        return 0
    # 电平对 n 单调增、间隔恒 2q ⇒ 最近电平的序号 n0 = ⌊(a + δ − q + q) / 2q⌋ = ⌊(a + δ) / 2q⌋
    n0 = (a + d) // (2 * q)
    best_n, best_err = 0, a
    for n in (n0 - 1, n0, n0 + 1, n0 + 2):
        if n < 1:
            continue
        err = abs(a - level_of(n, q))
        if err < best_err or (err == best_err and n < best_n):
            best_err, best_n = err, n
    if best_n == 0:
        return 0
    return -best_n if coef < 0 else best_n


def dequantize_value(qf: int, q: int) -> int:
    """第二法反分段：|F''| = (2|QF| + 1) q − δ，符号照搬。q' = 0 的无损档是恒等映射。"""
    check_quant_domain(q, "dequantize_value")
    if q == Q_LOSSLESS:
        return qf
    if qf == 0:
        return 0
    mag = level_of(abs(qf), q)
    return -mag if qf < 0 else mag


def scan_table(t_max: int, label: str) -> list[WidthRow]:
    """逐 q' 求出 |QF| 上界与 bits(q')，按 bits 的变化切档，切出的每一档起点做一次全域复核。

    上界取在 |T| = T_max：电平集对幅度单调增 ⇒ 最近电平的序号随 |T| 不减（单调性由
    `verify_monotone` 沿网格逐点核过，不在这里靠口头断言）。这条循环只走有损档（q' ≥ 1）：
    无损那一档没有电平集，|QF| 上界就是变换域上界，位宽由 verify_lossless_row 单独核。
    """
    rows: list[WidthRow] = []
    prev_bits = None
    for q in range(1, 128):
        qf_max = abs(quantize_value(t_max, q))
        bits = int(math.ceil(math.log2(qf_max + 1))) + 1 if qf_max > 0 else 1
        if bits != prev_bits:
            full = max(abs(quantize_value(t, q)) for t in range(-t_max, t_max + 1))
            if full != qf_max:
                raise SystemExit(
                    f"{label} bound is not stable at q'={q}: endpoint {qf_max} / full sweep {full}"
                )
            rows.append(WidthRow(q, bits, qf_max))
            prev_bits = bits
    return rows


def verify_monotone(t_max: int, label: str) -> None:
    """每个 q'（含无损那一档）都沿 |T| 走一遍网格：|QF| 必须不减。"""
    step = max(1, t_max // 256)
    for q in range(Q_LOSSLESS, 128):
        prev = -1
        for a in list(range(0, t_max + 1, step)) + [t_max]:
            v = abs(quantize_value(a, q))
            if v < prev:
                raise SystemExit(
                    f"nearest-level index goes backwards: {label} q'={q} |T|={a} "
                    f"|QF|={v} < previous {prev}"
                )
            prev = v


def verify_two_versions_agree() -> None:
    """捷径版与穷举版的抽样比对：两版不一致就说明候选窗推定不成立，直接失败退出。

    取样面是量化档按步长 7 跳（含无损那一档）、下面这串幅度点、正负两个符号，不是全域；
    全域扫一遍属离线一次性核对，不在每次重生成里跑。
    """
    pts = [
        0,
        1,
        2,
        3,
        7,
        8,
        9,
        23,
        24,
        25,
        47,
        48,
        71,
        72,
        119,
        120,
        240,
        241,
        1023,
        4095,
        8191,
        8192,
        16319,
        16320,
    ]
    for q in range(Q_LOSSLESS, 128, 7):
        for a in pts:
            for sgn in (1, -1):
                x = sgn * a
                fast = quantize_value(x, q)
                if fast != quantize_exhaustive(x, q):
                    raise SystemExit(
                        f"the two quantizer versions disagree: coef={x} q'={q} "
                        f"shortcut {fast} / exhaustive {quantize_exhaustive(x, q)}"
                    )


def verify_lossless_row(
    tables: CarrierTables, t_max: int, intra: bool, label: str
) -> None:
    """无损那一档独立核一遍：恒等映射下 |QF| 上界就是变换域上界，位宽由它推出来。

    库里这一档另带两条编译期自检（src/video/block.cpp 的 static_assert），本件核的是同一条
    不等式：幅值上界必须装得进 bits − 1 位幅值域。
    """
    row = tables.lossless[intra]
    bits = int(math.ceil(math.log2(t_max + 1))) + 1
    if bits != row.bits:
        raise SystemExit(f"{label} lossless width: formula {bits} / library {row.bits}")
    if row.qf_max != t_max:
        raise SystemExit(
            f"{label} lossless bound: transform domain {t_max} / library {row.qf_max}"
        )
    if abs(quantize_value(t_max, Q_LOSSLESS)) != row.qf_max:
        raise SystemExit(f"{label} lossless identity does not reach the table bound")
    if t_max > (1 << (row.bits - 1)) - 1:
        raise SystemExit(
            f"{label} lossless bound {t_max} does not fit {row.bits - 1} magnitude bits"
        )


def check_tables(tables: CarrierTables) -> None:
    """扫出来的两张有损档表与无损那一档，必须与库侧解析进来的表逐档一致，否则本件没有冻结的价值。"""
    verify_two_versions_agree()
    for t_max, lib_rows, label, intra in (
        (T_MAX_INTRA, tables.intra, "intra", True),
        (T_MAX_INTER, tables.inter, "inter", False),
    ):
        verify_monotone(t_max, label)
        got = scan_table(t_max, label)
        if len(got) != len(lib_rows):
            raise SystemExit(
                f"{label} row count: scanned {len(got)} / library {len(lib_rows)}: {got}"
            )
        for scanned, lib in zip(got, lib_rows):
            if scanned != lib:
                raise SystemExit(
                    f"{label} table does not match the library: scanned {scanned} / library {lib}"
                )
        verify_lossless_row(tables, t_max, intra, label)


def pack_blocks_fixed(
    qf_rows: list[list[int]], pattern: int, q: int, intra: bool, tables: CarrierTables
) -> bytes:
    """定长档：按块模式把在场块依次定长直写，块序 = 位序 bit5 → bit0，MSB 先出，不做零省略。"""
    bits = tables.bits_of(q, intra)
    mag_bits = bits - 1
    out = bytearray()
    acc, acc_bits = 0, 0

    def put(value: int, n: int) -> None:
        nonlocal acc, acc_bits
        acc = (acc << n) | (value & ((1 << n) - 1))
        acc_bits += n
        while acc_bits >= 8:
            acc_bits -= 8
            out.append((acc >> acc_bits) & 0xFF)

    for idx in range(BLOCKS_PER_MB):
        if (pattern & (1 << (BLOCKS_PER_MB - 1 - idx))) == 0:
            continue
        for v in qf_rows[idx]:
            put(1 if v < 0 else 0, 1)
            put(abs(v), mag_bits)
    if acc_bits:  # 64 × bits 恒是 8 的倍数，走到这里说明口径破了
        raise SystemExit(
            f"carrier tail leaves {acc_bits} bits, contradicting "
            f"{COEFS} x {bits} bits = whole bytes per block"
        )
    return bytes(out)


def pack_blocks_eob_eg(qf_rows: list[list[int]], pattern: int) -> bytes:
    """法 1 档（零省略那一法）：每块 all_zero(1) + eob(6) + 槽 0..eob 的 EG(|QF|)，非零槽紧跟 1 bit 符号。

    块序 = 位序 bit5 → bit0，MSB 先出；eob 是自然行序里最后一个非零系数的下标，整块无非零时
    只写 all_zero = 1 这一位。宏块末补 0 到整字节 —— 每宏块的块数据都以字节边界结束。
    """
    if (1 << EOB_FIELD_BITS) != COEFS:
        raise SystemExit(
            f"eob field of {EOB_FIELD_BITS} bits cannot cover slot indices 0..{COEFS - 1}"
        )
    out = bytearray()
    acc, acc_bits = 0, 0

    def put(value: int, n: int) -> None:
        nonlocal acc, acc_bits
        if n == 0:
            return
        acc = (acc << n) | (value & ((1 << n) - 1))
        acc_bits += n
        while acc_bits >= 8:
            acc_bits -= 8
            out.append((acc >> acc_bits) & 0xFF)

    for idx in range(BLOCKS_PER_MB):
        if (pattern & (1 << (BLOCKS_PER_MB - 1 - idx))) == 0:
            continue
        qf = qf_rows[idx]
        nz = [i for i, v in enumerate(qf) if v != 0]
        if not nz:
            put(1, 1)  # all_zero
            continue
        put(0, 1)
        put(nz[-1], EOB_FIELD_BITS)  # eob = 最后一个非零系数的下标
        for i in range(nz[-1] + 1):
            v = qf[i]
            mag = abs(v)
            x = mag + 1
            L = x.bit_length() - 1
            put(0, L)
            put(1, 1)
            if L:
                put(x & ((1 << L) - 1), L)
            if mag:
                put(1 if v < 0 else 0, 1)
    if acc_bits:  # 宏块末补齐（变长法下尾位个数随内容走，补齐位不校验取值）
        put(0, 8 - acc_bits)
    return bytes(out)


def carrier_for_blocks(
    blocks: list[list[int]], q: int, tables: CarrierTables
) -> tuple[bytes, bytes, int, list[list[int]]]:
    """六块输入像素 → (定长档载体, 法 1 档载体, 块模式, 逐块 QF)。输入是像素，减预测电平在这里做。

    两档一次算全：头件里两套载体都发出去，选哪一套由 `tests/win32/video/test_config_expect.h`
    按编译期的 CLV_COEF_CODING 定 —— 期望字节由本件独立算出，不该取决于生成时敲了哪个旗标，
    否则换构型编译就得先重生成一次，账面与器件的对应关系会随最后一次跑而漂。
    """
    qf_rows: list[list[int]] = []
    pattern = 0
    for idx, pix in enumerate(blocks):
        x = [p - INTRA_PRED for p in pix]
        t = hadamard8(x)
        qf = [quantize_value(v, q) for v in t]
        qf_rows.append(qf)
        if any(qf):
            pattern |= 1 << (BLOCKS_PER_MB - 1 - idx)
    return (
        pack_blocks_fixed(qf_rows, pattern, q, True, tables),
        pack_blocks_eob_eg(qf_rows, pattern),
        pattern,
        qf_rows,
    )


def read_frozen_coefs(path: pathlib.Path) -> dict[str, list[int]]:
    """读量化对拍件里冻好的 kBlock{0,1}Coefs，用来核对本件的正变换（不参与任何期望值计算）。

    旁件不在场、或那两张数组读不出来，一律当场失败退出：界扫掠的那几格锚是产物的一部分，
    少了它们等于悄悄换了一份期望值，只打一行提示是拦不住这个的。
    """
    if not path.is_file():
        raise SystemExit(f"side fixture is not readable: {path}")
    text = path.read_text(encoding="utf-8")
    found: dict[str, list[int]] = {}
    for name in ("kBlock0Coefs", "kBlock1Coefs"):
        m = re.search(rf"{name}\[64\]\s*=\s*\{{([^\}}]*)\}}", text)
        if not m:
            raise SystemExit(f"frozen coefficients {name} not found in {path}")
        found[name] = [int(v) for v in re.findall(r"-?\d+", m.group(1))]
    return found


def gather_blocks(pixel_of) -> list[list[int]]:
    """一宏块六块的取块位序：Y0 (0,0) / Y1 (8,0) / Y2 (0,8) / Y3 (8,8)、Cb、Cr（色度各 8×8）。"""
    lum = [(0, 0), (8, 0), (0, 8), (8, 8)]
    blocks = []
    for px, py in lum:
        blocks.append([pixel_of(0, px + c, py + r) for r in range(8) for c in range(8)])
    for plane in (1, 2):
        blocks.append([pixel_of(plane, c, r) for r in range(8) for c in range(8)])
    return blocks


def zero_dc_note(qf: int) -> str:
    """全零帧那档 DC 的算式：变换域上界夹在哪两个电平之间、各差多少，逐字算出来写进注释。"""
    n = abs(qf)
    lo, hi = level_of(n, Q_PRIME), level_of(n + 1, Q_PRIME)
    sign = "-" if qf < 0 else ""
    return (
        f"QF = {sign}{n}：|{T_MAX_INTRA} − {lo}| = {abs(T_MAX_INTRA - lo)} ≤ "
        f"|{hi} − {T_MAX_INTRA}| = {abs(hi - T_MAX_INTRA)}"
        f"（电平取 (2n+1) × {Q_PRIME} − {delta_of(Q_PRIME)}）"
    )


def dc_row_note(qf_rows: list[list[int]], emitted: tuple[int, ...]) -> str:
    """逐块 DC 的人读对照：期望值位上只发有读者的那几格，其余块的取值留在注释里。"""
    dcs = [row[0] for row in qf_rows]
    body = (
        f"{len(dcs)} 块同值 {dcs[0]}"
        if len(set(dcs)) == 1
        else "、".join(str(v) for v in dcs)
    )
    tail = (
        "发成常量的只有块 "
        + "/".join(str(i) for i in emitted)
        + "（用例读它），其余只作人读对照"
        if emitted
        else "全库没有用例读它，整族只作人读对照"
    )
    return f"逐块 DC 的 QF = {body}：{tail}。"


def pattern_row_note(pattern: int, present: int, mb_count: int) -> str:
    """块位图与在场块计数的人读对照（同样只在有读者处发常量）。"""
    return (
        f"块位图 0x{pattern:02X}：在场块 {present} 块、整帧 {mb_count} × {present} = "
        f"{mb_count * present} 块（位图与这两格计数全库没有读者，用例读的是由位图切出来的 "
        "cbpy / cbpc，故这里只作人读对照）"
    )


def c_array(name: str, data: bytes, per_row: int = 12) -> list[str]:
    lines = [f"\tinline constexpr unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), per_row):
        lines.append("\t\t" + " ".join(f"0x{b:02X}u," for b in data[i : i + per_row]))
    lines.append("\t};")
    return lines


def scalar(name: str, value: int, note: str, unsigned: bool = True) -> str:
    """冻一个标量。带符号的量（QF 电平序号可负）走 int32_t，其余 uint32_t。"""
    if unsigned:
        suffix = "u" if value >= 0 else ""
        return f"\tinline constexpr uint32_t {name} = {value}{suffix};\t// {note}"
    return f"\tinline constexpr int32_t {name} = {value};\t// {note}"


def emit_case(
    o,
    tag: str,
    carrier_f0: bytes,
    carrier_f1: bytes,
    pattern: int,
    qf_rows: list[list[int]],
    mb_count: int,
    desc: list[str],
    tables: CarrierTables,
    dc_blocks: tuple[int, ...],
    present_counts: bool,
) -> None:
    """发一个用例集：两档载体都发，档无关的字段发一份。

    这里**不发** block_size 与整帧载体合计 —— 它们等于「本档载体实长」，由
    `test_config_expect.h` 按 CLV_COEF_CODING 选出来的 CarrierLen 推出来；在这里再写一遍就成了第二处。
    块位图与逐块 DC 也一样：用例读的是派生出来的那几格（cbpy / cbpc），位图本身、以及六块里
    与块 0 同值的那些 DC 都没有读者，就不发成常量，取值留在上面的注释里给人对照。
    """
    bits = tables.bits_of(Q_PRIME, True)
    single_block_bytes = COEFS * bits // 8
    present = bin(pattern).count("1")
    o("")
    o("\t/*")
    for line in desc:
        o(f"\t *\t{line}")
    o("\t */")
    for suffix, carrier in (("F0", carrier_f0), ("F1", carrier_f1)):
        for line in c_array(f"k{tag}Carrier{suffix}", carrier):
            o(line)
        if suffix == "F0":
            note = (
                f"定长档载体实长 {len(carrier)} B：在场块 {present} × 单块 {single_block_bytes} B"
                f"（{COEFS} × {bits} ÷ 8）= {present * single_block_bytes} B，零照样占位"
            )
        else:
            note = (
                f"法 1 档载体实长 {len(carrier)} B：在场块 {present} 块各 all_zero(1)+eob({EOB_FIELD_BITS})"
                f"+逐槽 EG(幅)，非零槽跟 1 bit 符号，宏块末补齐（变长 ⇒ 不再有「块数 × 单块」的闭式）"
            )
        o(scalar(f"k{tag}CarrierLen{suffix}", len(carrier), note))
    o(
        scalar(
            f"k{tag}Cbpy",
            pattern >> LUMA_PATTERN_SHIFT,
            f"cbpy = 块模式 >> {LUMA_PATTERN_SHIFT}（4 bit 亮度位图）",
        )
    )
    o(
        scalar(
            f"k{tag}Cbpc",
            pattern & CHROMA_PATTERN_MASK,
            f"cbpc = 块模式 & {CHROMA_PATTERN_MASK}（2 bit 色度位图）",
        )
    )
    if present_counts:
        o(scalar(f"k{tag}PresentBlocks", present, "位图里 1 的个数"))
        o(
            scalar(
                f"k{tag}FramePresentBlocks",
                mb_count * present,
                f"整帧在场块 = {mb_count} × {present} = {mb_count * present} 块",
            )
        )
    o(scalar(f"k{tag}MbCount", mb_count, "本用例的宏块数"))
    # 逐块 DC 电平序号：载体里唯一（或最先）可肉眼核对的量，算式在注释里
    for idx in dc_blocks:
        nz = [v for v in qf_rows[idx] if v != 0]
        first = nz[0] if nz else 0
        o(
            scalar(
                f"k{tag}Block{idx}DcQf",
                qf_rows[idx][0],
                f"块 {idx} DC 的 QF；非零系数 {len(nz)} 个"
                + (f"，首非零 QF = {first}" if nz else "（全零块）"),
                unsigned=False,
            )
        )


def row_span_note(tables: CarrierTables, q: int, intra: bool) -> str:
    """q' 落在哪一档、上下界怎么来的，全部由权威档表推出来，不手抄档位边界。"""
    rows = tables.rows(intra)
    idx = max(i for i, row in enumerate(rows) if row.start <= q)
    if idx + 1 >= len(rows):
        return f"q' = {q} 落末档（起点 {rows[idx].start} 起顶格）"
    return f"q' = {q} 落 {rows[idx].start}..{rows[idx + 1].start - 1} 档"


def sweep_anchor(coefs: dict[str, list[int]]) -> dict[str, int]:
    """量化误差界最紧一档：2·max|t − f2| 与 3q' − δ，逐 q' 交叉相乘取最紧的一档。

    输入用对拍件里冻好的 kBlock0Coefs / kBlock1Coefs（本件只读不写那两个数），
    反量化按第二法独立重写，故这里冻的是「式子的结果」而不是被测函数的输出。
    扫掠只走有损档（q' ≥ 1）：界 3q' − δ 说的是电平集量化的误差，无损那一档是恒等映射、
    误差恒零，不进这条比式。
    """
    res: dict[str, int] = {}
    for name, key in (("Block0", "kBlock0Coefs"), ("Block1", "kBlock1Coefs")):
        t = coefs.get(key)
        if not t:
            raise SystemExit(
                f"sweep anchor needs the frozen {key}; the fixture gave nothing"
            )
        worst_num, worst_den, worst_q = 0, 1, 0
        for q in range(1, 128):
            f2 = [dequantize_value(quantize_value(v, q), q) for v in t]
            err = max(abs(a - b) for a, b in zip(t, f2))
            bound = 3 * q - delta_of(q)
            if 2 * err * worst_den > worst_num * bound:
                worst_num, worst_den, worst_q = 2 * err, bound, q
        res[name] = worst_q
        res[f"{name}Num"] = worst_num
        res[f"{name}Den"] = worst_den
    return res


def main() -> int:
    ap = argparse.ArgumentParser(
        description="emit the frozen block-coefficient carrier fixture (both carriers)"
    )
    ap.add_argument(
        "--out",
        default=str(OUT),
        help="output header path (default tests/win32/video/video_payload_fixtures_p.h)",
    )
    args = ap.parse_args()
    out_path = pathlib.Path(args.out).resolve()

    value_of = constant_resolver(CONST_SOURCES)
    bind_library_constants(value_of)
    tables = read_width_tables(value_of)
    intra_rows, inter_rows = tables.intra, tables.inter
    check_tables(tables)

    # 判点全部排在落盘之前：对拍不成立时头件根本不会被写出去，谈不上「先污染再归因」。
    lcg_seeds, lcg_block = load_lcg()
    frozen = read_frozen_coefs(QUANT_FIXTURES)
    agree = []
    for i, key in enumerate(("kBlock0Coefs", "kBlock1Coefs")):
        agree.append((i, hadamard8(lcg_block(lcg_seeds[i])) == frozen[key]))
    mismatched = [i for i, ok in agree if not ok]
    if mismatched:
        # 抽检的目的就是「独立实现与实码不一致时当场红」，只 print 等于把判定推给读日志的人
        raise SystemExit(
            f"independent transform disagrees with the quant fixture on blocks {mismatched}; "
            "nothing was written, attribute first then rerun"
        )
    sw = sweep_anchor(frozen)

    out: list[str] = []
    o = out.append
    row24 = tables.row_of(Q_PRIME, True)
    bits24 = row24.bits
    block_bytes24 = COEFS * bits24 // 8
    mb_bytes24 = BLOCKS_PER_MB * block_bytes24
    o("/* video_payload_fixtures_p.h")
    o(" *")
    o(
        " * 由 scripts/gen_video_payload_fixtures.py 生成，请勿手改，也不要手填里面的数字。"
    )
    o(
        " * 期望载体由本件独立算出后冻结：取块 → 减帧内预测电平 "
        f"{INTRA_PRED} → T = H X Hᵀ（8×8 Sylvester"
    )
    o(
        " * Hadamard，元素 ±1、整数、不归一，按矩阵乘法直算）→ 最近电平量化（电平集 {0} ∪"
    )
    o(
        " * {±((2n+1)q' − δ)}，δ = 0（q' 奇）/ 1（q' 偶），穷举取最近、平局取小序号）→ 每系数"
    )
    o(" * bits(q') 位定长档直写（1 位符号 + bits − 1 位幅值，MSB 先出，不做零省略）；")
    o(
        f" * 法 1 档 = 每块 all_zero(1) + eob({EOB_FIELD_BITS}) + 槽 0..eob 的 Exp-Golomb(order-0) 幅值，"
        "非零槽紧跟 1 bit 符号，宏块末补齐。"
    )
    o(
        " * 两档载体本件都发（后缀 F0 = 定长档、F1 = 法 1 档），编译期按 CLV_COEF_CODING 选哪一套由"
        " tests/win32/video/test_config_expect.h 负责 —— 本件里不留 #if，也不留第二处实长数字。"
    )
    o(
        " * 重跑：python scripts/gen_video_payload_fixtures.py，再跑 scripts/format_all.py --write。"
    )
    o(" *")
    o(
        " * 载体档位（位宽档表的唯一权威是 src/video/block.cpp 的 kIntraRows / kInterRows，"
        f"帧内 {len(intra_rows)} 档起点 {'/'.join(str(r.start) for r in intra_rows)}、"
        f"帧间 {len(inter_rows)} 档起点 {'/'.join(str(r.start) for r in inter_rows)}；"
    )
    o(
        f" *   本件按 T_max = {T_MAX_INTRA}（帧内）/ {T_MAX_INTER}（帧间）全域另扫一遍，"
        "逐档与那张表核对，对不上就不落盘；冻在这里的位宽取自库侧那张表，独立扫掠是给它的等式闸门。"
    )
    o(
        f" *   无损档（q' = {Q_LOSSLESS}）由 src/video/block.cpp 的 kLosslessRows 给出：bits ="
        f" {tables.lossless[True].bits}、帧内 |QF| ≤ {tables.lossless[True].qf_max}、"
        f"帧间 |QF| ≤ {tables.lossless[False].qf_max}，两法的 bits(q') 同值。"
    )
    o(
        " *   "
        + row_span_note(tables, Q_PRIME, True)
        + f" → bits = {bits24}、|QF| ≤ {row24.qf_max}、"
    )
    o(
        f" *   单块 {COEFS} × {bits24} ÷ 8 = {block_bytes24} B、"
        f"宏块 {BLOCKS_PER_MB} 块 = {mb_bytes24} B"
    )
    o(" */")
    o("#ifndef CLV_TEST_VIDEO_PAYLOAD_FIXTURES_P_H")
    o("#define CLV_TEST_VIDEO_PAYLOAD_FIXTURES_P_H")
    o("")
    o("#include <cstdint>")
    o("")
    o("namespace clvtest")
    o("{")
    o("")
    o(
        scalar(
            "kCarrierBlockBytesAt24",
            block_bytes24,
            f"单块字节 = {COEFS} × {bits24} ÷ 8",
        )
    )

    # ---- 用例 1：176 × 144 帧缓冲分配后未写入 ⇒ 像素全 0，逐块同值的宏块共用一份载体 ----
    chroma_w = ZERO_FRAME_W // 2
    zero_blocks = gather_blocks(lambda plane, x, y: 0)
    carrier0, carrier1, pattern, qf_rows = carrier_for_blocks(
        zero_blocks, Q_PRIME, tables
    )
    zero_present = bin(pattern).count("1")
    emit_case(
        o,
        "ZeroFrameMb",
        carrier0,
        carrier1,
        pattern,
        qf_rows,
        ZERO_FRAME_MBS,
        [
            f"输入 = {ZERO_FRAME_W} × {ZERO_FRAME_H}"
            f"（{ZERO_FRAME_W // MB_SIDE} × {ZERO_FRAME_H // MB_SIDE} = {ZERO_FRAME_MBS} 个宏块）"
            "编码帧，句柄分配后一像素都没写 ⇒ 像素全 0。",
            f"平面行距按 {STRIDE_ALIGN} 字节对齐（{ZERO_FRAME_W} → "
            f"{-(-ZERO_FRAME_W // STRIDE_ALIGN) * STRIDE_ALIGN}、{chroma_w} → "
            f"{-(-chroma_w // STRIDE_ALIGN) * STRIDE_ALIGN}），取块只吃有效区，补边列不参与。",
            f"每块 {COEFS} 个像素都是 0 ⇒ 变换输入恒 −{INTRA_PRED} ⇒ T 只有 DC = "
            f"{COEFS} × (−{INTRA_PRED}) = −{T_MAX_INTRA}，其余 {COEFS - 1} 项为 0。",
            f"于是 {ZERO_FRAME_MBS} 个宏块共用同一份载体（定长档 {len(carrier0)} B / 法 1 档 {len(carrier1)} B）。",
            dc_row_note(qf_rows, ZERO_FRAME_DC_BLOCKS),
            zero_dc_note(qf_rows[0][0]),
        ],
        tables,
        dc_blocks=ZERO_FRAME_DC_BLOCKS,
        present_counts=ZERO_FRAME_PRESENT_COUNTS,
    )

    # ---- 用例 2：16 × 16 一个宏块，像素 = (0x10 + 平面 × 0x20 + 行 × 3 + 列) & 0xFF ----
    def plane16(plane: int, x: int, y: int) -> int:
        return (0x10 + plane * 0x20 + y * 3 + x) & 0xFF

    blocks16 = gather_blocks(plane16)
    carrier16_0, carrier16_1, pattern16, qf16 = carrier_for_blocks(
        blocks16, Q_PRIME, tables
    )
    emit_case(
        o,
        "Plane16Mb",
        carrier16_0,
        carrier16_1,
        pattern16,
        qf16,
        PLANE16_MBS,
        [
            f"输入 = {PLANE16_SIDE} × {PLANE16_SIDE} 一个宏块，像素按 (0x10 + 平面 × 0x20 + 行 × 3 + 列) & 0xFF"
            " 写进可写平面。",
            "Y 4 块取左上 / 右上 / 左下 / 右下，Cb、Cr 各一整块（色度平面 8 × 8）。",
            "写入字节与用例里那份完全同式，本件只负责把它推到载体字节。",
            pattern_row_note(pattern16, bin(pattern16).count("1"), PLANE16_MBS),
            dc_row_note(qf16, PLANE16_DC_BLOCKS),
        ],
        tables,
        dc_blocks=PLANE16_DC_BLOCKS,
        present_counts=PLANE16_PRESENT_COUNTS,
    )

    # ---- 量化误差界最紧一档（供 test_video_quant_dpb 的界扫掠挂外部锚）----
    o("")
    o("\t/*")
    o("\t *\t量化误差界 2·max|t − f2| ≤ 3q' − δ 的全域最紧一档（q' = 1..127）。")
    o(
        "\t *\t输入取 video_quant_fixtures_p.h 冻好的 kBlock0Coefs / kBlock1Coefs（旁件不在场即失败退出）；"
    )
    o("\t *\t反量化按第二法在本件里独立重写，冻的是式子的结果、不是被测函数的输出。")
    o(
        "\t *\tNum / Den = 不等式两边的整数形态：用例除核 Num ≤ Den，还钉死 Num / Den / q' 三个数。"
    )
    o("\t */")
    for name in ("Block0", "Block1"):
        o(
            scalar(
                f"kSweepWorst{name}Num",
                sw[f"{name}Num"],
                f"最紧一档 2·max|err|，出现在 q' = {sw[name]}",
            )
        )
        o(
            scalar(
                f"kSweepWorst{name}Den",
                sw[f"{name}Den"],
                f"同档的界 3q' − δ = 3 × {sw[name]} − {delta_of(sw[name])}",
            )
        )
        o(scalar(f"kSweepWorst{name}Q", sw[name], "取到最紧一档的 q'"))

    o("")
    o("}\t // namespace clvtest")
    o("")
    o("#endif\t  // CLV_TEST_VIDEO_PAYLOAD_FIXTURES_P_H")
    o("")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(out), encoding="utf-8", newline="\n")
    # --out 指到仓外时 relative_to 会抛，别让一条打印把已经写好的产物判成失败
    try:
        shown = out_path.relative_to(ROOT)
    except ValueError:
        shown = out_path
    print("wrote %s : %d bytes" % (shown.as_posix(), out_path.stat().st_size))
    print(
        "width tables bound to src/video/block.cpp : intra %d rows %s / inter %d rows %s"
        % (len(intra_rows), intra_rows, len(inter_rows), inter_rows)
    )
    print(
        "lossless row q'=%d : bits %d, intra bound %d / inter bound %d"
        % (
            Q_LOSSLESS,
            tables.lossless[True].bits,
            tables.lossless[True].qf_max,
            tables.lossless[False].qf_max,
        )
    )
    print(
        "case 1 (zero frame) : fixed %d B / EOB+EG %d B, pattern 0x%02X, present %d blocks, DC QF = %d"
        % (len(carrier0), len(carrier1), pattern, zero_present, qf_rows[0][0])
    )
    print(
        "case 2 (16x16 plane): fixed %d B / EOB+EG %d B, pattern 0x%02X"
        % (len(carrier16_0), len(carrier16_1), pattern16)
    )
    for i, ok in agree:
        print(
            f"forward transform vs quant fixture: block {i} {'match' if ok else 'MISMATCH'}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
