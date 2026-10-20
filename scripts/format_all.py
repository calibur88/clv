"""格式化：C/C++ 走仓库根 .clang-format，Python 走 black 默认参数。

格式规则的唯一依据是项目配置（仓库根 .clang-format 与 black 默认参数）。
默认只检查不改写，--write 才落盘。

两半同则（对「工具不在」的处置）：clang-format 与 black 谁找不到都不单独决定成败，都记一笔
「这一半没检查」，跑完另一半再统一结算——「未检查」绝不折算成「通过」。退出码：
    0 = 两半都跑了且都合格
    1 = 跑了的半边报不合格
    2 = 有一半根本没跑成（工具不在，或该半扫不到文件）
末行把覆盖面件数与未检查的条数一起打出来：件数为 0 或骤降时按未跑成读，别看退出码就放心。

输出全部 ASCII：回显的每行（含子进程诊断）都转义成 \\u 加四位十六进制的形式。依据：本机控制台码页是 GBK，
非 GBK 字符直接打印会乱码甚至抛编码异常打断整道闸。

用法：
    python scripts/format_all.py             # 检查，列出不合格文件
    python scripts/format_all.py --write     # 就地格式化
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
#   四 · 常见安装位置：VS Code cpptools 扩展自带的 LLVM/bin，以及 LLVM 的标准安装根
CLANG_FORMAT_LOCAL_CONFIG = ROOT / ".clv-tools.local"
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


def clang_format_from_local_config(config):
    """读 `clang_format=<路径>`；其余行与 # 注释忽略。文件不在就返回 None。"""
    if not config.is_file():
        return None
    for line in config.read_text(encoding="utf-8-sig").splitlines():
        key, sep, value = line.partition("=")
        if sep and key.strip() == "clang_format" and value.strip():
            return value.strip()
    return None


def clang_format_installed():
    """在安装位置里取最近修改的那一份——按版本号字典序挑会挑错（`1.9` 会赢过 `1.34`）。"""
    hits = [
        pathlib.Path(p)
        for pattern in CLANG_FORMAT_INSTALL_GLOBS
        for p in glob.glob(pattern)
    ]
    hits = [p for p in hits if p.is_file()]
    if not hits:
        return None
    return str(max(hits, key=lambda p: p.stat().st_mtime))


def clang_format_candidates():
    return (
        ("env", os.environ.get("CLV_CLANG_FORMAT")),
        ("local-config", clang_format_from_local_config(CLANG_FORMAT_LOCAL_CONFIG)),
        ("path", shutil.which("clang-format")),
        ("install-scan", clang_format_installed()),
    )


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


def run(cmd):
    """跑一条外部命令，退出码交回调用方判。

    子进程的输出一律按字节收（capture_output），本件自己解码：格式化工具的诊断里可能带本机码页
    编不出的字符，直接透传会打断这道闸。解码取 utf-8 + replace——诊断文本只给人看，坏字节换成
    占位符即可，不参与成败判定；换掉的字符再由 ascii() 转义，落到的输出必然是 ASCII。
    """
    print("[run] " + " ".join(str(part) for part in cmd))
    proc = subprocess.run(
        [str(part) for part in cmd],
        cwd=ROOT,
        check=False,
        capture_output=True,
    )
    for stream in (proc.stdout, proc.stderr):
        text = stream.decode("utf-8", errors="replace")
        for line in text.splitlines():
            print("    " + ascii(line))
    return proc.returncode


def main():
    parser = argparse.ArgumentParser(description="clang-format + black 格式化检查")
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

    for reason in unchecked:
        print("[unchecked] " + reason)

    if failed:
        print(
            "[format_all] FAILED: code=%d python=%d unchecked=%d"
            % (len(code_files), len(py_files), len(unchecked))
        )
        sys.exit(1)
    if unchecked:
        print(
            "[format_all] NOT PASSED: %d half(s) were not checked at all "
            "(code=%d python=%d), see the [unchecked] lines above"
            % (len(unchecked), len(code_files), len(py_files))
        )
        sys.exit(2)
    print(
        "[format_all] PASS: %d C/C++ files, %d Python files checked"
        % (len(code_files), len(py_files))
    )


if __name__ == "__main__":
    main()
