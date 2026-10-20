#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 tests/win32/video/video_quant_fixtures_p.h —— 量化器与整数 Hadamard 的 C++ 侧对拍素材。

正变换 / 最近电平量化 / 第二法反量化由一份**独立实现的参考件**现算
（该件提供正变换、量化、反量化三个入口，与 C++ 侧不同源），C++ 侧要逐样本对上。
输入块用固定 LCG 生成，可复现、不依赖外部媒体件。

参考件目录由 --ref 给出：
    python scripts/gen_video_quant_fixtures.py --ref <含参考实现件的目录>
生成后跑 scripts/format_all.py --write（产出的数组要按仓库 clang-format 归位）。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "tests/win32/video/video_quant_fixtures_p.h"

BLOCKS = 2
QS = (1, 2, 127)
SEEDS = [20261004, 917]


def lcg_block(seed: int, n: int = 64) -> list[int]:
    state = seed
    vals = []
    for _ in range(n):
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        vals.append((state >> 8) % 256 - 128)
    return vals


def array(name: str, vals) -> str:
    vals = [int(v) for v in vals]
    lines = [f"\tinline constexpr int32_t {name}[64] = {{"]
    for i in range(0, len(vals), 8):
        lines.append("\t\t" + ", ".join(str(v) for v in vals[i : i + 8]) + ",")
    lines.append("\t};")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="生成量化器对拍素材头")
    ap.add_argument(
        "--ref",
        required=True,
        help="独立实现的参考件所在目录（提供正变换 / 量化 / 反量化入口）",
    )
    args = ap.parse_args()

    sys.path.insert(0, str(Path(args.ref).resolve()))
    import numpy as np  # noqa: E402
    import q1_q7_quantizer as Q  # noqa: E402

    out: list[str] = []
    o = out.append
    o("/* video_quant_fixtures_p.h")
    o(" *")
    o(
        " * 由 scripts/gen_video_quant_fixtures.py 生成，请勿手改，也不要手填里面的数字。"
    )
    o(" * 期望值由一份独立实现的参考件现算，与 C++ 侧不同源；")
    o(" * 输入块由固定 LCG 生成，种子见下，可逐字节复现。")
    o(
        " * 重跑：python scripts/gen_video_quant_fixtures.py --ref <参考件目录>，然后跑 scripts/format_all.py --write"
    )
    o(" *")
    for i, s in enumerate(SEEDS):
        o(f" *   块 {i}：LCG 种子 {s}")
    for q in QS:
        o(f" *   量化步长 q = {q}（{'奇数，δ=0' if q % 2 else '偶数，δ=1'}）")
    o(" */")
    o("#ifndef CLV_TEST_VIDEO_QUANT_FIXTURES_P_H")
    o("#define CLV_TEST_VIDEO_QUANT_FIXTURES_P_H")
    o("")
    o("#include <cstdint>")
    o("")
    o("namespace clvtest")
    o("{")
    o("")
    o(f"\tinline constexpr unsigned kQuantQCount = {len(QS)}u;")
    o(
        "\tinline constexpr unsigned char kQuantQ[%d] = { %s };"
        % (len(QS), ", ".join(f"{q}u" for q in QS))
    )
    o(
        "\tinline constexpr unsigned char kQuantDelta[%d] = { %s };\t// δ = 0（q 奇）/ 1（q 偶），由 q 的奇偶推得"
        % (len(QS), ", ".join(f"{0 if q % 2 else 1}u" for q in QS))
    )
    o("")

    for b in range(BLOCKS):
        x = np.array(lcg_block(SEEDS[b]), dtype=np.int64).reshape(8, 8)
        o(array(f"kBlock{b}Pixels", x.reshape(-1)))
        t = Q.H8 @ x @ Q.H8.T
        o(array(f"kBlock{b}Coefs", t.reshape(-1)))
        for qi, q in enumerate(QS):
            qf = Q.clv_quant(t, q)
            f2 = Q.clv_dequant(qf, q)
            o(array(f"kBlock{b}Qf{qi}", qf.reshape(-1)))
            o(array(f"kBlock{b}F2{qi}", f2.reshape(-1)))
            err = int(np.max(np.abs(t - f2)))
            o(
                f"\tinline constexpr int32_t kBlock{b}MaxErr{qi} = {err};\t// 界 = 1.5q - δ/2"
            )
            o("")

    o("}\t // namespace clvtest")
    o("")
    o("#endif\t  // CLV_TEST_VIDEO_QUANT_FIXTURES_P_H")
    o("")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(out), encoding="utf-8", newline="\n")
    print(f"写出 {OUT.relative_to(ROOT)}：{OUT.stat().st_size} 字节")
    for q in QS:
        delta = 0 if q % 2 else 1
        first = 3 * q - delta
        print(
            f"  q={q:>3} δ={delta} 首电平={first} 步长={2 * q} 死区半宽={first / 2:.2f} 界={1.5 * q - delta / 2:.2f}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
