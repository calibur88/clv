#!/usr/bin/env python3
"""setup_skills.py —— 逐件把 skills/ 里的件链进某个平台的扫描目录。

分工（本仓约定）：
    skills/<name>/SKILL.md                              通用件与项目共同件，唯一事实来源，随 git 提交
    <平台路径>/<平台入口名>/<name>                      逐件链接，**不入库**，每台机器自己重建。可达形态有两种：
        主形 相对符号链接——相对深度建时按入口目录层数现算（入口目录直挂仓根即常见的 ../../skills/<name> 两级形），
            仓库换目录不失效
        退形 Windows junction——目标是绝对路径，换目录或换机器后要重跑本脚本

为什么逐件、而不是整目录链过去：整目录链会把"只给某一家用"的东西也带进那家的视野；
逐件链接等于一份显式加载清单——那家要用哪几件，就链哪几件。
某家要一份与共同件同名但内容不同的东西时，不要在 skills/ 里改，
在它自己的目录里放真实物理文件；本脚本遇到这种同名实体就判成分歧并停手，不覆盖也不删。

链接为什么不入库：git 存符号链接时存的是"内容为目标路径的特殊文件"，
Windows 上没开开发者模式的话 checkout 会退化成一份写着路径的普通文本——看着在、实际不可用。
所以每台机器跑一次本脚本现场重建。

用法：
    python scripts/setup_skills.py --check                                  # 只报状态，不写
    python scripts/setup_skills.py                                          # 给已复核的平台链上 skills/ 全部件
    python scripts/setup_skills.py --only clv-gate-run clv-receipt-ledger   # 只链其中几件；未命中的件名会警告并按码 6 退出
    python scripts/setup_skills.py --platform ".claude:skills"              # 新平台：查过那家官方文档后显式给
    python scripts/setup_skills.py --prune                                  # 顺手删掉目标已消失的陈旧链接

退出码（有限枚举，按严重度升序；一轮里多个问题并存时取最高一个值，不做按位累加）：
    0 全部件链对，或 --check 未见异常
    1 skills/ 源目录不存在
    2 平台入口里出现同名的真实目录/文件（判成有意分叉，不动），或普通文件占了目录位
    3 某个链接指向 skills/ 之外（件级只警告不动；入口目录级停手不动）
    4 链接建不成（相对符号链接与 junction 两路都没成功）
    5 平台参数不在已复核表内，或解析后落点在仓界之外（立即停止）
    6 --only 给的件名不在 skills/ 里（警告，其余件照常处理）
    7 该摘掉的链接或空占位目录没能摘除（记失败并继续处理其余件，不崩溃）

输出一律 ASCII：这台机器的控制台是 GBK，中文在这里会乱码并误导读日志的人。
子进程（mklink）的错误串按字节取回、只打印可 ASCII 解码的部分，
本地化码页的文本与解码异常都不进这条输出通道。
"""

from __future__ import annotations

import argparse
import ctypes
import os
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
SOURCE = REPO / "skills"
FILE_ATTRIBUTE_REPARSE_POINT = 0x400

# 只收已按该平台官方文档复核过的条目；其余必须显式传 "<平台目录>:<入口名>"
VERIFIED = {".qoder": "skills"}  # Qoder CN CLI，官方文档复核于 2026-10-13

# frontmatter 扫描上限：只为兜住「分隔符不闭合就别扫全文」，正常 frontmatter 远小于这个数
FM_SCAN_LIMIT = 512

# 退出码（见模块 docstring 的码表）：按严重度升序的有限枚举，多问题并存取最高
EXIT_OK = 0
EXIT_NO_SOURCE = 1
EXIT_CONFLICT = 2
EXIT_ELSEWHERE = 3
EXIT_BUILD_FAILED = 4
EXIT_BAD_PLATFORM = 5
EXIT_NO_MATCH = 6
EXIT_UNLINK_FAILED = 7


def is_reparse(path: pathlib.Path) -> bool:
    """是否为重分析点（符号链接 / junction）。Windows 上 is_symlink() 盖不住 junction。"""
    if os.name != "nt":
        return path.is_symlink()
    attr = ctypes.windll.kernel32.GetFileAttributesW(str(path))
    if attr == -1:
        return False
    return bool(attr & FILE_ATTRIBUTE_REPARSE_POINT)


def ascii_only(raw: bytes) -> str:
    """子进程错误串按字节取回，只保留可 ASCII 解码的部分并截短，非 ASCII 一律丢弃。"""
    return raw.decode("ascii", errors="ignore").strip()[:140]


def shared_names() -> list[str]:
    """skills/ 下可作为件加载的目录名（必须含 SKILL.md）。"""
    if not SOURCE.is_dir():
        return []
    return sorted(
        p.name for p in SOURCE.iterdir() if p.is_dir() and (p / "SKILL.md").is_file()
    )


def classify(link: pathlib.Path, target: pathlib.Path) -> str:
    """某位置的现状：ABSENT / LINK_OK / LINK_ELSEWHERE / DANGLING / REAL_DIR / EMPTY_DIR / REAL_FILE。"""
    if is_reparse(link):
        try:
            resolved = link.resolve()
        except OSError:
            return "DANGLING"
        if not resolved.exists():
            return "DANGLING"
        return "LINK_OK" if resolved == target.resolve() else "LINK_ELSEWHERE"
    if not link.exists():
        return "ABSENT"
    if link.is_dir():
        return "REAL_DIR" if any(link.iterdir()) else "EMPTY_DIR"
    return "REAL_FILE"


def make(link: pathlib.Path, target: pathlib.Path) -> tuple[str, str]:
    """先试相对符号链接（仓库换目录不失效），失败退 junction（绝对路径，换机器要重跑）。"""
    try:
        link.symlink_to(os.path.relpath(target, link.parent), target_is_directory=True)
        return "symlink", "relative target"
    except FileExistsError:
        return (
            "failed",
            "entry already exists at the link path (rerun --check to classify)",
        )
    except (OSError, NotImplementedError):
        pass
    if os.name == "nt":
        r = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(target)],
            capture_output=True,
        )
        if r.returncode == 0:
            return "junction", "ABSOLUTE target - rerun after moving or cloning"
        return (
            "failed",
            ascii_only(r.stderr or r.stdout) or "(no ascii-decodable child output)",
        )
    return (
        "failed",
        "symlink unsupported on this platform (junction fallback needs Windows)",
    )


def unlink(path: pathlib.Path) -> None:
    """只摘链接本身；绝不递归删它指向的内容。

    链接（POSIX 符号链接、Windows 符号链接）一律走删链接的调用；
    只有删链接调用不成功的 Windows junction 才落到删目录调用——对重分析点
    它同样只移除链接结点自身，不会跟进目标。调用方自行捕获 OSError。
    """
    if not is_reparse(path):
        return
    if path.is_symlink():
        try:
            path.unlink()
            return
        except OSError:
            pass
    os.rmdir(path)


def audit_source() -> list[str]:
    """skills/ 自身体检：frontmatter 是否完整、name 是否等于目录名、缺不缺 SKILL.md。"""
    names = shared_names()
    print(
        "source: %s -> %d skill(s): %s"
        % (
            SOURCE.relative_to(REPO).as_posix(),
            len(names),
            ", ".join(names) if names else "(none)",
        )
    )
    for p in sorted(SOURCE.iterdir()) if SOURCE.is_dir() else []:
        if not p.is_dir():
            continue
        f = p / "SKILL.md"
        if not f.is_file():
            print("WARN  skills/%s has no SKILL.md" % p.name)
            continue
        lines = f.read_text(encoding="utf-8", errors="replace").split("\n")
        fences = []
        for i, line in enumerate(lines[:FM_SCAN_LIMIT]):
            if line.strip() == "---":
                fences.append(i)
                if len(fences) == 2:
                    break
        if len(fences) < 2:
            print(
                "WARN  skills/%s frontmatter fences incomplete (need two bare --- lines within %d rows)"
                % (p.name, FM_SCAN_LIMIT)
            )
            continue
        body = lines[fences[0] + 1 : fences[1]]
        nm = [l for l in body if l.startswith("name:")]
        if not nm:
            print("WARN  skills/%s frontmatter has no name:" % p.name)
        elif nm[0].split(":", 1)[1].strip() != p.name:
            print("WARN  skills/%s name != dir name" % p.name)
        if not [l for l in body if l.startswith("description:")]:
            print("WARN  skills/%s frontmatter has no description:" % p.name)
    return names


def settle(
    leaf: pathlib.Path,
    want: list[str],
    check_only: bool,
    prune: bool,
    shared: set[str],
) -> int:
    """在平台入口目录里，为 want 中每个件建 / 核一条逐件链接。"""
    label = leaf.relative_to(REPO).as_posix()
    if leaf.exists() and not leaf.is_dir() and not is_reparse(leaf):
        print("STOP %-42s a regular file sits where a directory is expected" % label)
        return EXIT_CONFLICT
    if is_reparse(leaf):
        # 入口目录自身是链接：只有目标指向本仓 skills/ 的（即上一版整目录链那种）才摘；
        # 指不到本仓 skills/ 的（指向别处、或悬空核验不了）都是别人建的，不动。
        try:
            dest = leaf.resolve()
        except OSError:
            dest = None
        if dest != SOURCE.resolve():
            print(
                "STOP %-42s is a link that does not point at this repo's skills/ - "
                "not ours to remove; fix that entry by hand" % label
            )
            return EXIT_ELSEWHERE
        if check_only:
            print(
                "STOP %-42s is a whole-dir link; rerun without --check to convert it to per-item links"
                % label
            )
            return EXIT_CONFLICT
        try:
            unlink(leaf)
        except OSError as exc:
            print("FAIL %-43s whole-dir link not removable: %s" % (label, exc))
            return EXIT_UNLINK_FAILED
        print("NOTE whole-dir link removed: %s" % label)
    if not leaf.is_dir():
        if check_only:
            print("TODO %s does not exist (would create it)" % label)
        else:
            leaf.mkdir(parents=True, exist_ok=True)
            print("DONE %s created" % label)

    rc = EXIT_OK
    for name in want:
        link, target = leaf / name, SOURCE / name
        state = classify(link, target)
        item = link.relative_to(REPO).as_posix()
        if state == "LINK_OK":
            print("OK   %-48s linked" % item)
            continue
        if state == "DANGLING":
            if check_only:
                print("STALE %-47s link is dangling" % item)
                continue
            try:
                unlink(link)
            except OSError as exc:
                print("FAIL %-47s dangling link not removable: %s" % (item, exc))
                rc = max(rc, EXIT_UNLINK_FAILED)
                continue
            kind, note = make(link, target)
            print("REPAIR %-46s %s (%s)" % (item, kind, note))
            if kind == "failed":
                rc = max(rc, EXIT_BUILD_FAILED)
            continue
        if state == "LINK_ELSEWHERE":
            print("WARN %-48s link points elsewhere; left untouched" % item)
            rc = max(rc, EXIT_ELSEWHERE)
            continue
        if state == "EMPTY_DIR":
            # 残留的空占位目录：classify 已保证为空，删目录调用不递归、碰不到任何内容；
            # 不先清掉它，建链接会撞「已存在」，诊断方向就偏了。
            if check_only:
                print(
                    "TODO %-48s empty dir placeholder (would be replaced by a link)"
                    % item
                )
                continue
            try:
                link.rmdir()
            except OSError as exc:
                print("FAIL %-47s empty dir not removable: %s" % (item, exc))
                rc = max(rc, EXIT_UNLINK_FAILED)
                continue
            print("NOTE %-48s empty placeholder dir removed" % item)
        elif state in ("REAL_DIR", "REAL_FILE"):
            print(
                "STOP %-48s this platform keeps its own %s here - an intentional fork; "
                "keep it, or rename so the two are distinguishable"
                % (item, state.replace("_", " ").lower())
            )
            rc = max(rc, EXIT_CONFLICT)
            continue
        if check_only:
            print("TODO %-48s not linked yet" % item)
            continue
        kind, note = make(link, target)
        if kind == "failed":
            print("FAIL %-48s %s" % (item, note))
            if os.name == "nt":
                print(
                    "     fallback: enable Developer Mode, or copy that skill dir in here"
                )
            else:
                print("     fallback: copy that skill dir in here")
            rc = max(rc, EXIT_BUILD_FAILED)
        else:
            print("DONE %-48s %s (%s)" % (item, kind, note))

    if leaf.is_dir():
        for p in sorted(leaf.iterdir()):
            if p.name in shared:
                continue
            if classify(p, SOURCE / p.name) == "DANGLING":
                rel = p.relative_to(REPO).as_posix()
                if prune and not check_only:
                    try:
                        unlink(p)
                    except OSError as exc:
                        print("FAIL %-47s stale link not removable: %s" % (rel, exc))
                        rc = max(rc, EXIT_UNLINK_FAILED)
                        continue
                    print("PRUNE %s removed" % rel)
                else:
                    print("STALE %s (target gone; pass --prune to remove)" % rel)
    return rc


def parse_platform(spec: str) -> pathlib.Path:
    """`.qoder`（表内已复核）或 `.claude:skills`（自己查过那家的官方文档）。"""
    if ":" in spec:
        d, leaf = spec.split(":", 1)
        if not d or not leaf:
            print("STOP platform spec %r needs both a dir and an entry name" % spec)
            raise SystemExit(EXIT_BAD_PLATFORM)
        path = REPO / d / leaf
    elif spec in VERIFIED:
        path = REPO / spec / VERIFIED[spec]
    else:
        print(
            "STOP platform %r is not in the verified table (%s); check that platform's official docs, "
            "then pass <dir>:<leaf>" % (spec, ", ".join(sorted(VERIFIED)))
        )
        raise SystemExit(EXIT_BAD_PLATFORM)
    base = os.path.normpath(str(REPO))
    joined = os.path.normpath(str(path))
    if joined != base and not joined.startswith(base + os.sep):
        print("STOP platform spec %r resolves outside the repo - refusing" % spec)
        raise SystemExit(EXIT_BAD_PLATFORM)
    return path


def main() -> int:
    ap = argparse.ArgumentParser(
        description="link individual skills from skills/ into a platform's skill dir"
    )
    ap.add_argument("--check", action="store_true", help="report only, write nothing")
    ap.add_argument(
        "--prune",
        action="store_true",
        help="also remove links whose target disappeared",
    )
    ap.add_argument(
        "--platform",
        nargs="*",
        default=None,
        help="platform entries: .qoder or .claude:skills",
    )
    ap.add_argument(
        "--only",
        nargs="*",
        default=None,
        help="subset of skills/ dirs to link (default: all); a name missing from skills/ exits 6",
    )
    args = ap.parse_args()

    if not SOURCE.is_dir():
        print("FAIL %s not found - skills/ is the single source of truth" % SOURCE)
        return EXIT_NO_SOURCE
    names = audit_source()
    shared = set(names)
    want = args.only or names
    unknown = [w for w in want if w not in shared]
    for u in unknown:
        print("WARN  %r is not in skills/ - nothing to link" % u)
    want = [w for w in want if w in shared]
    rc = EXIT_NO_MATCH if unknown else EXIT_OK

    for spec in args.platform or sorted(VERIFIED):
        rc = max(rc, settle(parse_platform(spec), want, args.check, args.prune, shared))
    return rc


if __name__ == "__main__":
    sys.exit(main())
