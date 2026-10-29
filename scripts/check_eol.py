# -*- coding: utf-8 -*-
"""行尾扫描（0.9.1）：覆盖 **跟踪件 + 未跟踪件**（`.gitignore` 排除），按 `.gitattributes` 的 `eol=lf` 判。

为什么要有这一件：`git ls-files --eol` 只看索引里的跟踪件，未跟踪的新件从没人扫过——0.9.0 上传时就有三件
新源码带着 CRLF 一路走到 `git add` 才被 git 的告警暴露。判据两条：
  · 工作树侧 EOL 为 crlf / mixed 且该件不是二进制 ⇒ 报；
  · 索引侧（跟踪件）EOL 为 crlf / mixed 且不是二进制 ⇒ 报（发行内容会被 `eol=lf` 改写）。
二进制按 `.gitattributes` 里声明为 binary / -text 的件识别，不计行尾。
输出全 ASCII（本机控制台 GBK）；退出码 0 = 干净、1 = 有违例、2 = 拿不到清单（未跑成，不折算成通过）。
"""

import subprocess
import sys


def listing():
    proc = subprocess.run(
        [
            "git",
            "ls-files",
            "--eol",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
        ],
        check=False,
        capture_output=True,
    )
    if proc.returncode != 0:
        return None
    rows = []
    for raw in proc.stdout.decode("utf-8", "replace").split("\0"):
        if not raw:
            continue
        meta, _, path = raw.partition("\t")
        parts = meta.split()
        if len(parts) < 3:
            continue
        i_eol, w_eol, attr = parts[0], parts[1], " ".join(parts[2:])
        rows.append({"i": i_eol, "w": w_eol, "attr": attr, "path": path})
    return rows


def main() -> int:
    rows = listing()
    if rows is None:
        print("EOL: no listing (git unavailable) -> NOT RUN")
        return 2
    bad = []
    binary = 0
    for r in rows:
        if "binary" in r["attr"] or "-text" in r["attr"]:
            binary += 1
            continue
        w_ok = not r["w"].endswith(("crlf", "mixed"))
        i_ok = not r["i"].endswith(("crlf", "mixed"))
        if not (w_ok and i_ok):
            bad.append(r)
    print("EOL: files=%d binary=%d violations=%d" % (len(rows), binary, len(bad)))
    for r in bad:
        print(
            "EOL: %-8s i/%-6s w/%-6s %s"
            % ("", r["i"], r["w"], r["path"].encode("ascii", "replace").decode("ascii"))
        )
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
