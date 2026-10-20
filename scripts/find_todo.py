"""列出仓库里的待办标记。

标记集合就是代码里 MARKERS 那六个，逐个列全：TODO、FIXME、XXX、HACK、待办、待补。
不参与 CMake 构建；手动或 CI 显式调用。默认只扫源码、脚本与构建脚本目录，跳过第三方与构建产物。

匹配在剥掉字符串与字符字面量内容之后进行，剥法复用注释提取件 dump_comments.skeleton（同仓已有实现，
不另写一套）：不剥的话，别的脚本里用作提示的 TODO / 待办 字样会被当残留抓出来，属假命中。

输出的每一行都是 ASCII：命中的那一行原文转义成 \\u 加四位十六进制的形式后打印。依据：本机控制台码页是 GBK，
非 GBK 字符直接打印会乱码甚至抛编码异常打断长任务；转义只影响观感，路径与行号仍可定位。

用法：
    python scripts/find_todo.py
    python scripts/find_todo.py --pattern FIXME          # 只找 FIXME 一类
    python scripts/find_todo.py --include-archive        # 在默认目录之外再加 archive 这一个目录
"""

import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from dump_comments import skeleton  # noqa: E402

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
DEFAULT_DIRS = ("include", "src", "tests", "scripts", "cmake", "demo")
# 本文件自身不参与扫描；说明里列的标记与本集合逐字对应，加一个删一个都要同时改说明
MARKERS = ("TODO", "FIXME", "XXX", "HACK", "待办", "待补")


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
            parts = set(path.relative_to(ROOT).parts)
            if parts & set(DEFAULT_SKIP):
                continue
            if path.name == pathlib.Path(__file__).name:
                continue  # 本工具的标记表自身不算残留
            text = path.read_text(encoding="utf-8", errors="replace")
            for number, line in enumerate(text.splitlines(), start=1):
                # 匹配走剥了字面量的骨架串，回报走原文：字面量里的提示语不是残留，
                # 而读方要看的是原行；骨架串只保证「不因为提示语而假命中」。
                if pattern.search(skeleton(line)):
                    hits.append((path, number, line.strip()))
    return hits


def main():
    parser = argparse.ArgumentParser(description="扫描待办标记")
    parser.add_argument("--pattern", default=None, help="只匹配这一个词")
    parser.add_argument(
        "--include-archive",
        action="store_true",
        help="在默认目录之外只额外扫 archive 这一个目录（不是按忽略清单全仓扫）",
    )
    args = parser.parse_args()

    dirs = list(DEFAULT_DIRS)
    if args.include_archive:
        dirs.append("archive")
    markers = (args.pattern,) if args.pattern else MARKERS

    # 标记名里有中文，直接拼进输出就不是 ASCII 了；ascii() 把非 ASCII 字符转成转义形式。
    marker_labels = ", ".join(ascii(marker) for marker in markers)

    hits = scan(dirs, markers)
    if not hits:
        print(f"[find_todo] clean, markers scanned: {marker_labels}")
        return

    print(f"[find_todo] {len(hits)} hit(s), markers scanned: {marker_labels}")
    for path, number, line in hits:
        # 路径也走转义：archive 下的文件名是中文，码页不认的字符会让长任务在这一行抛编码异常。
        print(f"  {ascii(path.relative_to(ROOT))}:{number}: {ascii(line[:120])}")


if __name__ == "__main__":
    main()
