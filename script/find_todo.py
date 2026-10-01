"""列出仓库里的 TODO / FIXME / XXX / 待办 标记，供收轮自查。

不参与 CMake 构建；手动或 CI 显式调用。默认排除第三方、构建产物与过程材料目录。

用法：
    python script/find_todo.py
    python script/find_todo.py --pattern FIX          # 只找 FIXME 一类
    python script/find_todo.py --include-archive      # 连 archive/ 一起看
"""

import argparse
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_SKIP = {
    "build",
    "third-party",
    ".venv",
    ".git",
    "__pycache__",
    ".cache",
    "参考资料",
}
DEFAULT_DIRS = ("include", "src", "tests", "script", "cmake", "demo")
MARKERS = ("TODO", "FIXME", "XXX", "HACK", "待办", "待补")  # 本文件自身不参与扫描


def scan(dirs, markers):
    pattern = re.compile("|".join(re.escape(marker) for marker in markers))
    hits = []
    for name in dirs:
        base = ROOT / name
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix not in (
                ".h",
                ".hpp",
                ".cpp",
                ".c",
                ".py",
                ".cmake",
                ".txt",
                ".md",
            ):
                continue
            if path == pathlib.Path(__file__).resolve():
                continue  # 本工具的标记表自身不算残留
            parts = set(path.relative_to(ROOT).parts)
            if parts & set(DEFAULT_SKIP):
                continue
            if path.name == pathlib.Path(__file__).name:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for number, line in enumerate(text.splitlines(), start=1):
                if pattern.search(line):
                    hits.append((path, number, line.strip()))
    return hits


def main():
    parser = argparse.ArgumentParser(description="扫描待办标记")
    parser.add_argument("--pattern", default=None, help="只匹配这一个词")
    parser.add_argument(
        "--include-archive", action="store_true", help="连 archive/ 一起扫"
    )
    args = parser.parse_args()

    dirs = list(DEFAULT_DIRS)
    if args.include_archive:
        dirs.append("archive")
    markers = (args.pattern,) if args.pattern else MARKERS

    hits = scan(dirs, markers)
    if not hits:
        print(f"[find_todo] 没有 {', '.join(markers)} 残留")
        return

    print(f"[find_todo] {len(hits)} 处：")
    for path, number, line in hits:
        print(f"  {path.relative_to(ROOT)}:{number}: {line[:120]}")


if __name__ == "__main__":
    main()
