"""格式化：C/C++ 走仓库根 `.clang-format`，Python 走 black 默认参数，Markdown 走仓库根 `.pymarkdown`。

格式规则的唯一依据是项目配置（仓库根 `.clang-format`、black 默认参数、仓库根 `.pymarkdown`）。
默认只检查不改写，--write 才落盘。

三半同则（对「工具不在」的处置）：clang-format、black、pymarkdown 谁找不到都不单独决定成败，
都记一笔「这一半没检查」，跑完其余半再统一结算——「未检查」绝不折算成「通过」。退出码：
    0 = 三半都跑了且都合格
    1 = 跑了的半边报不合格
    2 = 有一半根本没跑成（工具不在，或该半扫不到文件，或工具自己没跑成）
末行把覆盖面件数与未检查的条数一起打出来：件数为 0 或骤降时按未跑成读，别看退出码就放心。

Markdown 半的退出码不是 0/1 两档：pymarkdown 的默认方案把「有违例」与「调用失败」都压成 1，分不清
「文档不合格」还是「根本没跑成」，所以本半统一带 `--return-code-scheme explicit` 起进程，实测三档是
    0 = 干净 / 4 = 有违例 / 1 = 调用失败（例如传入路径不存在）
只认这三档：既不是 0 也不是 4 的，一律按「这一半没跑成」结算，不折算成通过。

Markdown 半的外部前置（另两半不需要，故单独声明）：`pymarkdown` 是 pip 包，装法走本仓常规那一条
`python -m pip install -r requirements.txt`（版本钉在 `requirements.txt`，随 `.venv/` 走，不入库）。
入口两路，与 black 同形：PATH → `.venv/Scripts/pymarkdown.exe`，首行打印命中的是哪一路。
配置件 `.pymarkdown` **只在当前工作目录查找、不沿祖先链上溯**（实测把长行探针放到子目录里跑，
仓库根那份配置照不到，MD013 照样报），所以必须在仓库根执行本件（本件正是以仓库根为 cwd 起子进程）。
`--write` 有一条实测坑要补偿：pymarkdown 的 `fix` 只要改一个文件，就把**整件**写成 CRLF
（实测一件只有 1 处空行违例的文件，改后 LF 10 → CRLF 9），与本仓 `.gitattributes` 钉的 `eol=lf` 冲突，
所以本半在 `fix` 之后自己把换行归一回 LF 并复扫一遍；归一后仍有 CR 就按不合格报，绝不留 CRLF 出货。
另两条实测限制：`fix` 的改动面会超出触发它的那条规则（曾把示例源码的制表符重排成空格），故 `.pymarkdown`
把 MD010 整条关掉；规则号与 option 键写错都**不会**报错（`md999_bogus` 段与 `bogus_opt` 键都静默通过），
覆盖面按跟踪清单取，规则名改动要对着 `pymarkdown plugins list` 人工核。

输出全部 ASCII：回显的每行（含子进程诊断）都转义成 \\u 加四位十六进制的形式。依据：本机控制台码页是 GBK，
非 GBK 字符直接打印会乱码甚至抛编码异常打断整道闸。

用法：
    python scripts/format_all.py             # 检查，列出不合格文件
    python scripts/format_all.py --write     # 就地格式化（Markdown 半含换行归一与复扫）
"""

import argparse
import glob
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCAN_DIRS = ("include", "src", "platform", "backends", "tests", "demo", "scripts")
CODE_SUFFIXES = (".c", ".h", ".hpp", ".cpp")
PY_DIRS = ("scripts",)

# clang-format 的落点按这条顺序找，找到就用，并把命中的是哪一路打出来：
#   一 · 环境变量 CLV_CLANG_FORMAT
#   二 · 仓库根的本机私有件 .clv-tools.local 里的 `clang_format=` 一行
#        （该件不入库：发行文本里不得出现本机绝对路径，而每台机器的落点不一样）
#   三 · PATH
#   四 · 常见安装位置（cpptools 扩展自带的 LLVM/bin、LLVM 标准安装根）
# 找不到时只记「这一半没检查」，不硬失败——依据见模块头「三半同则」。
TOOLS_LOCAL_CONFIG = ROOT / ".clv-tools.local"
CLANG_FORMAT_INSTALL_GLOBS = (
    str(
        pathlib.Path(os.path.expandvars("%USERPROFILE%"))
        / ".vscode"
        / "extensions"
        / "ms-vscode.cpptools-*"
        / "LLVM"
        / "bin"
        / "clang-format.exe"
    ),
    str(
        pathlib.Path(os.path.expandvars("%ProgramFiles%"))
        / "LLVM"
        / "bin"
        / "clang-format.exe"
    ),
)

# Markdown 半的覆盖面判据是「这份文本归不归本仓格式化」，取法是**跟踪清单里的 .md**：
# 未跟踪的在制品（别人正在写的过程件、还没 `git add` 的新文档）自然不进闸，被 `.gitignore` 排除的
# 未随仓库发行的过程账也不用逐条点名——判据取「在不在版本库里」，一条就把那一整片说完了。
# third-party/ 下只留直接放在它根部的那份 README（本仓自己写的 vendoring 说明），
# 其子目录全是上游文档，不归本仓格式化。
MD_VENDORED_ROOT = "third-party"

# pymarkdown 的退出码方案（`--return-code-scheme explicit`，实测三档）：
# 0 干净 / 4 有违例 / 1 调用失败。既不 0 也不 4 的一律按「没跑成」结算。
PYMARKDOWN_RC_CLEAN = 0
PYMARKDOWN_RC_ISSUES = 4
# pymarkdown 的入口两路，与 black 同形：PATH → 本项目 .venv 里的那份（装法见 requirements.txt）。
PYMARKDOWN_VENV_EXE = ROOT / ".venv" / "Scripts" / "pymarkdown.exe"


def local_config_value(key, config):
    """读 `<key>=<路径>`；其余行与 # 注释忽略。文件不在就返回 None。"""
    if not config.is_file():
        return None
    for line in config.read_text(encoding="utf-8-sig").splitlines():
        name, sep, value = line.partition("=")
        if sep and name.strip() == key and value.strip():
            return value.strip()
    return None


def first_existing(paths):
    """在候选里取最近修改的那一份——按版本号字典序挑会挑错（`1.9` 会赢过 `1.34`）。"""
    hits = [pathlib.Path(p) for pattern in paths for p in glob.glob(pattern)]
    hits = [p for p in hits if p.is_file()]
    if not hits:
        return None
    return str(max(hits, key=lambda p: p.stat().st_mtime))


def clang_format_candidates():
    return (
        ("env", os.environ.get("CLV_CLANG_FORMAT")),
        ("local-config", local_config_value("clang_format", TOOLS_LOCAL_CONFIG)),
        ("path", shutil.which("clang-format")),
        ("install-scan", first_existing(CLANG_FORMAT_INSTALL_GLOBS)),
    )


def find_pymarkdown():
    """Markdown 半的入口：PATH → `.venv/Scripts/pymarkdown.exe`，返回 (路径, 命中来源)。"""
    on_path = shutil.which("pymarkdown")
    if on_path:
        return on_path, "path"
    if PYMARKDOWN_VENV_EXE.is_file():
        return str(PYMARKDOWN_VENV_EXE), "venv"
    return None, None


def find_tool_with_source(candidates):
    for source, candidate in candidates:
        if candidate and pathlib.Path(candidate).is_file():
            return candidate, source
    return None, None


def collect_code_files():
    files = []
    for name in SCAN_DIRS:
        base = ROOT / name
        if not base.is_dir():
            continue
        files.extend(p for p in base.rglob("*") if p.suffix in CODE_SUFFIXES)
    return sorted(files)


def collect_py_files():
    files = []
    for name in PY_DIRS:
        base = ROOT / name
        if not base.is_dir():
            continue
        files.extend(base.rglob("*.py"))
    files.extend(ROOT.glob("*.py"))
    return sorted(files)


def collect_md_files():
    """仓库自有 Markdown：取 git 跟踪清单里的 `.md`，vendored 子目录除外。

    判据是「这份文本在不在版本库里」，而不是「它在哪棵目录下」——
    未跟踪的在制品不该被本闸判红，被 `.gitignore` 排除的过程账也不用逐条点名。
    取不到跟踪清单（不在 git 工作树里跑）时返回空表，由调用方按「这一半没跑成」结算。
    """
    try:
        proc = subprocess.run(
            ["git", "ls-files", "-z", "--", "*.md"],
            cwd=ROOT,
            check=False,
            capture_output=True,
        )
    except OSError as exc:
        # git 不在 PATH（或被 PATH 精简过）时是 OSError，不是「扫到 0 件」——打出来再交回空表，
        # 由调用方按「这一半没跑成」结算，绝不让它冒崩成「跑了且不合格」。
        print("[md] git unavailable: " + ascii(str(exc)))
        return []
    if proc.returncode != 0:
        return []
    files = []
    for raw in proc.stdout.decode("utf-8", "replace").split("\0"):
        if not raw:
            continue
        parts = raw.split("/")
        if parts[0] == MD_VENDORED_ROOT and len(parts) > 1:
            continue
        path = ROOT / raw
        if path.is_file():
            files.append(path)
    return sorted(set(files))


def run_capture(cmd, show=None):
    """跑一条外部命令，回 (退出码, 合并后的输出文本)。

    子进程的输出一律按字节收（capture_output），本件自己解码：格式化工具的诊断里可能带本机码页
    编不出的字符，直接透传会打断这道闸。解码取 utf-8 + replace——诊断文本只给人看，坏字节换成
    占位符即可，不参与成败判定；换掉的字符再由 ascii() 转义，落到的输出必然是 ASCII。
    返回的文本是转义前的原样，供需要自证覆盖面的调用方解析（只有 ASCII 输出的工具才这么用）。
    """
    print(
        "[run] " + (show if show is not None else " ".join(str(part) for part in cmd))
    )
    proc = subprocess.run(
        [str(part) for part in cmd],
        cwd=ROOT,
        check=False,
        capture_output=True,
    )
    chunks = []
    for stream in (proc.stdout, proc.stderr):
        text = stream.decode("utf-8", errors="replace")
        chunks.append(text)
        for line in text.splitlines():
            print("    " + ascii(line))
    return proc.returncode, "\n".join(chunks)


def run(cmd):
    """跑一条外部命令，退出码交回调用方判（不需要读输出时用这个）。"""
    rc, _ = run_capture(cmd)
    return rc


def normalize_md_line_endings(files):
    """把 `fix` 写出的 CRLF 归一回 LF，返回 (真改了字节的件数, 归一后仍带 CR 的件数)。

    必须补这一步：实测 pymarkdown 的 `fix` 只要改一个文件就把整件写成 CRLF，
    而 `.gitattributes` 把全仓文本钉成 `eol=lf`——不归一就等于把整文件换行尾改坏。
    改写一律走 `write_bytes`（文本模式会再按本机码页与换行策略二次加工）。
    """
    rewritten = 0
    cr_left = 0
    for path in files:
        raw = path.read_bytes()
        if b"\r" not in raw:
            continue
        fixed = raw.replace(b"\r\n", b"\n")
        if b"\r" in fixed:
            cr_left += 1
        if fixed != raw:
            path.write_bytes(fixed)
            rewritten += 1
    return rewritten, cr_left


def main():
    parser = argparse.ArgumentParser(
        description="clang-format + black + pymarkdown 格式化检查",
    )
    parser.add_argument("--write", action="store_true", help="就地格式化")
    args = parser.parse_args()

    code_files = collect_code_files()
    py_files = collect_py_files()
    failed = False
    # 每一句 = 有一半根本没跑成。凑齐了再一起结算成退出码 2，绝不当「通过」收尾。
    unchecked = []

    clang_format, clang_format_source = find_tool_with_source(clang_format_candidates())
    if not clang_format:
        unchecked.append(
            "C/C++ half NOT checked: clang-format not found, all four lookup routes "
            "empty (env CLV_CLANG_FORMAT, local config .clv-tools.local clang_format= "
            "line, PATH, install scan)"
        )
    elif not code_files:
        unchecked.append(
            "C/C++ half NOT checked: 0 code files collected under %s" % (SCAN_DIRS,)
        )
    else:
        print(
            "[tool] clang-format source = %s :: %s"
            % (clang_format_source, clang_format)
        )
        flag = "-i" if args.write else "--dry-run"
        rc = run([clang_format, flag, "--Werror", *code_files])
        failed = failed or rc != 0

    black = shutil.which("black")
    if not black:
        venv_black = ROOT / ".venv" / "Scripts" / "black.exe"
        black = str(venv_black) if venv_black.is_file() else None
    if not black:
        unchecked.append(
            "Python half NOT checked: black not found "
            "(neither PATH nor .venv/Scripts/black.exe)"
        )
    elif not py_files:
        unchecked.append("Python half NOT checked: 0 python files collected")
    else:
        rc = run([black, *([] if args.write else ["--check"]), *py_files])
        failed = failed or rc != 0

    md_files = collect_md_files()
    pymarkdown, pymarkdown_source = find_pymarkdown()
    if not pymarkdown:
        unchecked.append(
            "Markdown half NOT checked: pymarkdown not found (neither PATH nor "
            ".venv/Scripts/pymarkdown.exe). Prerequisite: install the pinned package "
            "with 'python -m pip install -r requirements.txt'"
        )
    elif not md_files:
        unchecked.append(
            "Markdown half NOT checked: 0 tracked .md files (git ls-files returned "
            "nothing usable) -- this is not a pass"
        )
    else:
        print("[tool] pymarkdown source = %s :: %s" % (pymarkdown_source, pymarkdown))
        rel = [p.relative_to(ROOT).as_posix() for p in md_files]
        base_cmd = [pymarkdown, "--return-code-scheme", "explicit"]
        show = "%s [scan + %d tracked .md files]" % (pymarkdown, len(rel))
        rc, _ = run_capture([*base_cmd, "scan", *rel], show=show)
        if rc not in (PYMARKDOWN_RC_CLEAN, PYMARKDOWN_RC_ISSUES):
            unchecked.append(
                "Markdown half NOT checked: pymarkdown scan returned rc=%d, which is "
                "neither 'clean' (%d) nor 'issues' (%d) -- the tool itself failed to "
                "run, so nothing was checked"
                % (rc, PYMARKDOWN_RC_CLEAN, PYMARKDOWN_RC_ISSUES)
            )
        elif rc == PYMARKDOWN_RC_ISSUES and args.write:
            run_capture(
                [*base_cmd, "fix", *rel],
                show="%s [fix + %d tracked .md files]" % (pymarkdown, len(rel)),
            )
            rewritten, cr_left = normalize_md_line_endings(md_files)
            print(
                "[md] fix 之后换行归一：%d 件由 CRLF 改回 LF，归一后仍带 CR 的件数 %d"
                % (rewritten, cr_left)
            )
            rc, _ = run_capture(
                [*base_cmd, "scan", *rel],
                show="%s [复扫 + %d tracked .md files]" % (pymarkdown, len(rel)),
            )
            if rc not in (PYMARKDOWN_RC_CLEAN, PYMARKDOWN_RC_ISSUES):
                unchecked.append(
                    "Markdown half NOT checked: re-scan after fix returned rc=%d" % rc
                )
            else:
                failed = failed or rc == PYMARKDOWN_RC_ISSUES or cr_left > 0
        else:
            failed = failed or rc == PYMARKDOWN_RC_ISSUES

    for reason in unchecked:
        print("[unchecked] " + reason)

    if failed:
        print(
            "[format_all] FAILED: code=%d python=%d markdown=%d unchecked=%d"
            % (
                len(code_files),
                len(py_files),
                len(md_files),
                len(unchecked),
            )
        )
        sys.exit(1)
    if unchecked:
        print(
            "[format_all] NOT PASSED: %d half(s) were not checked at all "
            "(code=%d python=%d markdown=%d), see the [unchecked] lines above"
            % (
                len(unchecked),
                len(code_files),
                len(py_files),
                len(md_files),
            )
        )
        sys.exit(2)
    print(
        "[format_all] PASS: %d C/C++ files, %d Python files, %d Markdown files checked"
        % (len(code_files), len(py_files), len(md_files))
    )


if __name__ == "__main__":
    main()
