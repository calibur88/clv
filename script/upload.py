#!/usr/bin/env python
"""上传入口：把分支推到远端，并回读校验远端 ref 与本地是否一致。

只做推送与校验，不做 add / commit / remote add——那几步要人自己看过再敲。
凭证一律非交互：GitHub 的弹窗式认证在无界面终端里会把命令挂住，这里显式关掉。

用法：
    python script/upload.py                      # 推当前分支到 origin
    python script/upload.py --dry-run            # 校验 + 带 --dry-run 推送，不改远端
    python script/upload.py --branch main        # 指定分支
    python script/upload.py --remote origin --tag v0.0.1
    python script/upload.py --repo D:/somewhere  # 对别的仓库执行（默认本仓库根）

退出码：0 成功；1 推送或校验失败；2 参数与仓库状态不对。
"""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_REPO = Path(__file__).resolve().parent.parent

# 关掉一切交互式取凭据的入口：拿不到凭据就直接失败，不许挂在那里等输入
NONINTERACTIVE_ENV = {
    "GIT_TERMINAL_PROMPT": "0",
    "GCM_INTERACTIVE": "never",
}


def run(cmd, cwd):
    """回显并执行一条命令，返回 CompletedProcess（不抛异常，由调用方看返回码）。"""
    print("+ " + " ".join(cmd))
    return subprocess.run(
        cmd, cwd=cwd, text=True, capture_output=True, encoding="utf-8", errors="replace"
    )


def git_out(args, cwd):
    """取一条 git 命令的 stdout（去尾换行）；失败返回 None。"""
    proc = subprocess.run(
        ["git", *args],
        cwd=cwd,
        text=True,
        capture_output=True,
        encoding="utf-8",
        errors="replace",
    )
    return None if proc.returncode != 0 else proc.stdout.strip()


def current_branch(cwd):
    # symbolic-ref 在「一个提交都还没有」的新仓库里也能给出分支名，rev-parse --abbrev-ref 则不行
    return git_out(["symbolic-ref", "--quiet", "--short", "HEAD"], cwd)


def preflight(branch, remote, cwd):
    """推送前的硬条件：仓库可读、有提交、分支存在、远端已配、身份已配。"""
    if not (Path(cwd) / ".git").exists():
        print(f"[错误] {cwd} 不是 git 仓库", file=sys.stderr)
        return False

    if git_out(["rev-parse", "--verify", "HEAD"], cwd) is None:
        print(
            "[错误] 仓库还没有任何提交，无内容可推。先 add / commit。", file=sys.stderr
        )
        return False

    if (
        branch
        not in (
            git_out(["branch", "--format=%(refname:short)"], cwd) or ""
        ).splitlines()
    ):
        print(f"[错误] 本地没有分支 {branch}", file=sys.stderr)
        return False

    if git_out(["remote", "get-url", remote], cwd) is None:
        print(
            f"[错误] 未配置远端 {remote}。先执行：\n"
            "  git remote add "
            f"{remote} https://github.com/<owner>/<repo>.git",
            file=sys.stderr,
        )
        return False

    missing = [
        k
        for k in ("user.name", "user.email")
        if git_out(["config", "get", k], cwd) is None
    ]
    if missing:
        print(
            "[错误] git 身份未配置，提交无法署名。缺失项：" + "、".join(missing) + "\n"
            '  git config --global user.name "<名字>"\n'
            '  git config --global user.email "<邮箱>"',
            file=sys.stderr,
        )
        return False

    return True


def push_with_retry(cmd, cwd, attempts, wait):
    """执行推送并按需重试：走代理时链路会间歇性 reset，一次失败不代表推不上去。"""
    for i in range(attempts):
        if i:
            print(f"第 {i + 1} 次尝试…")
            time.sleep(wait)
        proc = run(cmd, cwd)
        print(proc.stdout.strip())
        print(proc.stderr.strip())
        if proc.returncode == 0:
            return True
    return False


def remote_sha(branch, remote, cwd, attempts=1, wait=3):
    """读远端分支指向的提交：分支不存在返回 None，读取失败返回 False。

    走代理时链路会间歇性 reset，所以读取也要重试，否则一次抖动就被当成「远端没有这个分支」。
    """
    for i in range(attempts):
        if i:
            print(f"读远端第 {i + 1} 次重试…")
            time.sleep(wait)
        proc = subprocess.run(
            ["git", "ls-remote", remote, f"refs/heads/{branch}"],
            cwd=cwd,
            text=True,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
        )
        if proc.returncode == 0:
            for line in proc.stdout.splitlines():
                fields = line.split()
                if len(fields) == 2:
                    return fields[0]
            return None
        print("[警告] 读远端失败：" + proc.stderr.strip(), file=sys.stderr)
    return False


def main():
    parser = argparse.ArgumentParser(description="推送分支并回读校验远端 ref")
    parser.add_argument(
        "--repo", default=str(DEFAULT_REPO), help="仓库路径，默认本脚本所在项目根"
    )
    parser.add_argument("--branch", default=None, help="要推的分支，默认当前分支")
    parser.add_argument("--remote", default="origin", help="远端名，默认 origin")
    parser.add_argument("--tag", default=None, help="附带推送的标签名，可选")
    parser.add_argument(
        "--retries", type=int, default=3, help="推送与读远端的最大尝试次数，默认 3"
    )
    parser.add_argument(
        "--wait", type=int, default=5, help="两次尝试之间的间隔秒数，默认 5"
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="只跑校验并带 --dry-run 推送，不真的改远端",
    )
    args = parser.parse_args()

    os.environ.update(NONINTERACTIVE_ENV)
    cwd = str(Path(args.repo).resolve())

    branch = args.branch or current_branch(cwd)
    if not branch:
        print(
            "[错误] 取不到当前分支名（游离 HEAD？），请用 --branch 指定",
            file=sys.stderr,
        )
        return 2

    if not preflight(branch, args.remote, cwd):
        return 2

    before = git_out(["rev-parse", f"refs/heads/{branch}"], cwd)
    print(f"仓库：{cwd}")
    print(f"分支：{branch} @ {before}")
    print(f"远端：{args.remote} -> {git_out(['remote', 'get-url', args.remote], cwd)}")
    remote_now = remote_sha(branch, args.remote, cwd, args.retries, args.wait)
    print("远端该分支当前：" + (remote_now or "（不存在或读取失败）"))

    push_cmd = ["git", "push", "--set-upstream", args.remote, branch]
    if args.dry_run:
        push_cmd.append("--dry-run")

    if not push_with_retry(push_cmd, cwd, args.retries, args.wait):
        print("PUSH FAILED", file=sys.stderr)
        return 1

    if args.tag and not push_with_retry(
        ["git", "push", args.remote, args.tag], cwd, args.retries, args.wait
    ):
        print(f"TAG PUSH FAILED: {args.tag}", file=sys.stderr)
        return 1

    print(run(["git", "branch", "-vv"], cwd).stdout.strip())

    if args.dry_run:
        print("[dry-run] 未改动远端，跳过一致性校验")
        return 0

    after = remote_sha(branch, args.remote, cwd, args.retries, args.wait)
    if after is False:
        print("[校验失败] 远端读不通，无法确认", file=sys.stderr)
        return 1
    if after != before:
        print(
            f"[校验失败] 远端 {branch} = {after}，本地 = {before}，两者不一致",
            file=sys.stderr,
        )
        return 1

    print(f"[校验通过] 远端 {branch} 已指向 {after}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
