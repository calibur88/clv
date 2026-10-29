#!/usr/bin/env python3
"""dump_comments.py —— 把要纳管的 C/C++ 注释提取成一份可逐条核对的清单件。

用途：收口时做注释纪律复扫（见 skills/clv-discipline-sweep）。清单件让「每条注释」变成可数、
可 grep、可分批处理的对象；直接读源文件容易漏掉块注释中间的行。

范围：`--dir` 指定的目录，默认七个顶层目录：src、include、tests、demo、platform、backends、scripts，
只吃 .c/.cpp/.h/.hpp。

third-party/ 与未随仓库发行的过程账目录不在默认集里，所以默认扫不到 —— 本工具没有排除清单，
要扫别的目录就把它写进 `--dir`。不放进默认集的理由：上游件不受本仓纪律约束，
后者不随 git 发行。

输出默认落 build/audit_comments.txt（build/ 已被 .gitignore 排除），每行：
    <相对路径>:<起始行>:<B|L>:<注释正文，块注释的内部换行压成单空格>

字符串与字符字面量里的 // 与 /* 由状态机跳过，免得把代码里的斜杠当注释。
"""

from __future__ import annotations

import argparse
import io
import pathlib
import sys

DEFAULT_DIRS = ["src", "include", "tests", "demo", "platform", "backends", "scripts"]
SUFFIXES = (".c", ".cpp", ".h", ".hpp")


def skeleton(line: str) -> str:
    """把字符串 / 字符字面量的内容抹成 x，返回可安全扫注释的骨架串。"""
    out = []
    quote = None
    i = 0
    while i < len(line):
        c = line[i]
        if quote is None:
            if c in ('"', "'"):
                quote = c
            out.append(c)
        else:
            if c == "\\":
                i += 2
                continue
            if c == quote:
                quote = None
                out.append(c)
            else:
                out.append("x")
        i += 1
    return "".join(out)


def comments_in(text: str):
    """返回 [(起始行, 'B'|'L', 正文)]，行号 1 基。块注释整块算一条。"""
    lines = text.split("\n")
    items = []
    in_block = False
    block_start = 0
    block_buf = []
    for idx, raw in enumerate(lines, start=1):
        skel = skeleton(raw)
        if in_block:
            end = skel.find("*/")
            if end == -1:
                block_buf.append(raw.strip())
                continue
            block_buf.append(raw[:end].strip())
            body = " ".join(x for x in block_buf if x)
            if body:
                items.append((block_start, "B", body))
            in_block = False
            block_buf = []
            off = end + 2
            skel, raw = skel[off:], raw[min(len(raw), off) :]
        pos = skel.find("//")
        if pos != -1:
            body = raw[min(len(raw), pos + 2) :].strip()
            if body:
                items.append((idx, "L", body))
            continue
        start = skel.find("/*")
        if start == -1:
            continue
        rest = skel[start + 2 :]
        end = rest.find("*/")
        if end != -1:
            body = raw[start + 2 : start + 2 + end].strip()
            if body:
                items.append((idx, "B", body))
            tail = raw[start + 2 + end + 2 :]
            tpos = skeleton(tail).find("//")
            if tpos != -1 and tail[tpos:].strip():
                items.append((idx, "L", tail[tpos:].strip()))
        else:
            in_block = True
            block_start = idx
            block_buf = [raw[start + 2 :].strip()]
    return items


def main() -> int:
    ap = argparse.ArgumentParser(
        description="dump C/C++ comments of tracked code into one audit file"
    )
    ap.add_argument("--root", default=".", help="repo root (default: cwd)")
    ap.add_argument(
        "--dir", nargs="*", default=DEFAULT_DIRS, help="top-level dirs to scan"
    )
    ap.add_argument(
        "--out",
        default="build/audit_comments.txt",
        help="output path (default under build/)",
    )
    args = ap.parse_args()

    root = pathlib.Path(args.root).resolve()
    out = pathlib.Path(args.out)
    if not out.is_absolute():
        out = root / out

    rows = []
    per_dir = {}
    files = 0
    for d in args.dir:
        base = root / d
        if not base.is_dir():
            print("skip (missing): %s" % d)
            continue
        n = 0
        for p in sorted(base.rglob("*")):
            if not p.is_file() or p.suffix not in SUFFIXES:
                continue
            rel = p.relative_to(root).as_posix()
            items = comments_in(io.open(p, encoding="utf-8", newline="").read())
            files += 1
            n += len(items)
            for ln, kind, body in items:
                rows.append("%s:%d:%s:%s" % (rel, ln, kind, body))
        per_dir[d] = n

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")
    blocks = sum(1 for r in rows if r.split(":")[2] == "B")
    print(
        "out=%s files=%d comments=%d (block=%d line=%d)"
        % (out.relative_to(root), files, len(rows), blocks, len(rows) - blocks)
    )
    print("by dir: " + ", ".join("%s=%d" % (k, v) for k, v in sorted(per_dir.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
