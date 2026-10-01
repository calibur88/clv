"""格式化入口：C/C++ 走仓库根 .clang-format，Python 走 black 默认参数。

格式规则的唯一依据是项目配置，不是任何人的习惯，所以这里只调工具、不自己判风格。
默认只检查不改写（--write 才落盘），避免一次误操作把全仓重排掉。

用法：
    python script/format_all.py             # 检查，列出不合格文件
    python script/format_all.py --write     # 就地格式化
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCAN_DIRS = ("include", "src", "platform", "backends", "tests", "demo")
CODE_SUFFIXES = (".c", ".h", ".hpp", ".cpp")
PY_DIRS = ("script",)

# 本机 clang-format 不在 PATH，VS Code cpptools 自带一份；可用环境变量覆盖
CLANG_FORMAT_CANDIDATES = (
    os.environ.get("CLV_CLANG_FORMAT"),
    str(
        pathlib.Path.home()
        / ".vscode/extensions/ms-vscode.cpptools-1.34.4-win32-x64/LLVM/bin/clang-format.exe"
    ),
    shutil.which("clang-format"),
)


def find_tool(candidates):
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).is_file():
            return candidate
    return shutil.which(os.path.basename(candidates[-1] or ""))


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
    print("[运行]", " ".join(str(part) for part in cmd))
    return subprocess.run([str(part) for part in cmd], cwd=ROOT, check=False).returncode


def main():
    parser = argparse.ArgumentParser(description="clang-format + black 入口")
    parser.add_argument("--write", action="store_true", help="就地格式化")
    args = parser.parse_args()

    code_files = collect_code_files()
    py_files = collect_py_files()
    failed = False

    clang_format = find_tool(CLANG_FORMAT_CANDIDATES)
    if not clang_format:
        sys.exit("[错误] 找不到 clang-format，设 CLV_CLANG_FORMAT 指到可执行文件")
    if code_files:
        flag = "-i" if args.write else "--dry-run"
        rc = run([clang_format, f"{flag}", "--Werror", *code_files])
        failed = failed or rc != 0

    black = shutil.which("black")
    if not black:
        venv_black = ROOT / ".venv" / "Scripts" / "black.exe"
        black = str(venv_black) if venv_black.is_file() else None
    if black and py_files:
        rc = run([black, *([] if args.write else ["--check"]), *py_files])
        failed = failed or rc != 0
    elif not black:
        print("[跳过] 没找到 black，Python 文件未检查")

    if failed:
        sys.exit(1)
    print(
        f"[format_all] 通过：{len(code_files)} 个 C/C++ 文件，{len(py_files)} 个 Python 文件"
    )


if __name__ == "__main__":
    main()
