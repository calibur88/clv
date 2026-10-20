#!/usr/bin/env python
"""上传：把分支推到远端，并回读校验远端 ref 与本地是否一致。

只做推送与校验，不做 add / commit / remote add。
副作用：推送固定带 --set-upstream，会把本地分支的上游改写成 <remote>/<branch>——
原本没有上游会被设上，已有上游会被改指；只想推一次而不改上游时不要用本脚本，直接 git push。
--dry-run 是例外：预演那一趟不带 --set-upstream，免得「只想看一眼」却把本地分支的上游配置改了。
远端地址回显前会把 scheme:// 与 host 之间的 userinfo 段抹掉——那里可能内嵌口令。

凭证一律非交互，两路各自设闸：超文本传输那一路靠 GIT_TERMINAL_PROMPT 与 GCM_INTERACTIVE；
SSH 那一路的口令与主机确认提示由 ssh 客户端自己发、上面两个变量管不到它，靠给 GIT_SSH_COMMAND
补 -o BatchMode=yes（遇任何交互直接失败），再靠子进程 stdin 挂空兜住「标准输入仍继承」。

输出的每一行都是 ASCII：外部命令的回显逐行转义后打印。依据：本机控制台码页是 GBK，
git 的中文/多语言输出里有码页外字符时，直接打印会抛编码异常，把已经成功的推送读成失败。

用法：
    python scripts/upload.py                      # 推当前分支到 origin
    python scripts/upload.py --dry-run            # 校验 + 带 --dry-run 推送，不改远端也不改本地上游
    python scripts/upload.py --branch main        # 指定分支
    python scripts/upload.py --remote origin --tag v0.0.1
    python scripts/upload.py --repo <另一个仓库路径>  # 对别的仓库执行（默认本仓库根）
    python scripts/upload.py --retries 5 --wait 3 # 推送与读远端的尝试次数 / 两次尝试之间的间隔秒数

退出码：0 成功；1 推送或校验失败；2 参数与仓库状态不对。
"""

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

# 远端类命令的兜底超时：网络挂住不等于还在跑，宁可判一次超时失败走重试或记红。
TIMEOUT_SEC = 300

DEFAULT_REPO = Path(__file__).resolve().parent.parent

# 关掉一切交互式取凭据的入口：拿不到凭据就直接失败，不许挂在那里等输入。
# 这两条覆盖超文本传输那一路（git 自己的提示与凭据管理器）。
NONINTERACTIVE_ENV = {
    "GIT_TERMINAL_PROMPT": "0",
    "GCM_INTERACTIVE": "never",
}
# SSH 那一路要单独设：口令提示与主机确认提示由 ssh 客户端发，不吃上面两个环境变量。
# 只补不改——调用方自己设的 GIT_SSH_COMMAND（私钥、代理等）要保住，批处理开关往后面并。
SSH_BATCH_OPTION = "-o BatchMode=yes"


def apply_noninteractive_env() -> None:
    for key, value in NONINTERACTIVE_ENV.items():
        os.environ[key] = value
    ssh = os.environ.get("GIT_SSH_COMMAND") or "ssh"
    if "BatchMode" not in ssh:
        os.environ["GIT_SSH_COMMAND"] = ssh + " " + SSH_BATCH_OPTION


def to_ascii(text: str) -> str:
    """整行是 ASCII 就原样返回，否则交 ascii() 转义。

    本机控制台码页是 GBK，码页外的字符直接 print 会抛 UnicodeEncodeError —— 那时推送可能已经成功，
    脚本却在这一行炸掉，读数就成了假失败。转义只影响观感，信息一个字节不丢。
    """
    return text if text.isascii() else ascii(text)


def echo(text: str) -> None:
    """把外部命令的回显按 ASCII 逐行打出来（坏字符由 ascii() 转义，不打断长任务）。"""
    for line in text.splitlines():
        print("    " + ascii(line))


# userinfo 段（scheme://user:token@host 里 user:token 那一截）可能内嵌凭据，回显前抹掉。
URL_USERINFO_RE = re.compile(r"(?<=://)[^/@]*@")


def redact_url(url: str) -> str:
    """抹掉 URL 里可能内嵌的凭据段；不含 userinfo 时原样返回。"""
    return URL_USERINFO_RE.sub("", url)


def run(cmd, cwd, timeout=TIMEOUT_SEC):
    """回显并执行一条命令，返回 CompletedProcess（不抛异常，由调用方看返回码）。

    超时不走异常通道：折算成 returncode = 124 的一条结果，stderr 里写明等了多久——
    网络类命令（clone / ls-remote / push）挂住时，调用方拿到的是「一次失败」而不是一条 traceback。
    解码口径 utf-8 + replace：git 的输出混着本机码页时坏字节换成占位符，回显只给人看，
    成败一律看返回码，不因解码把一次成功的命令读成失败；换成占位符的字符再由 echo 转义。
    stdin 挂空（DEVNULL）：环境变量只挡住「主动弹凭据提示」那几路，客户端自己发的交互提示
    还是会读标准输入并等下去；给它一个立即 EOF 的输入，问不出结果就当场失败。
    """
    print(to_ascii("+ " + " ".join(str(part) for part in cmd)))
    try:
        return subprocess.run(
            cmd,
            cwd=cwd,
            text=True,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            stdin=subprocess.DEVNULL,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(
            cmd,
            124,
            stdout="",
            stderr="timed out after %d s: %s" % (timeout, " ".join(map(str, cmd))),
        )


def git_out(args, cwd):
    """取一条 git 命令的 stdout（去尾换行）；失败或超时返回 None。走 run()，超时口径同它。"""
    proc = run(["git", *args], cwd)
    return None if proc.returncode != 0 else proc.stdout.strip()


def current_branch(cwd):
    # symbolic-ref 在「一个提交都还没有」的新仓库里也能给出分支名，rev-parse --abbrev-ref 则不行
    return git_out(["symbolic-ref", "--quiet", "--short", "HEAD"], cwd)


def preflight(branch, remote, cwd):
    """推送前的硬条件：仓库可读、有提交、分支存在、远端已配、身份已配。"""
    # 问版本控制本身，不看目录下有没有那个标记目录：独立工作区里它是文件不是目录，
    # 非常规布局（--repo 指过来的、子目录里跑的）也会被「按目录名判」误成「不是仓库」而退 2。
    if git_out(["rev-parse", "--git-dir"], cwd) is None:
        print(to_ascii("[error] not a git repository: %s" % cwd), file=sys.stderr)
        return False

    if git_out(["rev-parse", "--verify", "HEAD"], cwd) is None:
        print(
            "[error] repository has no commit yet, nothing to push; add / commit first",
            file=sys.stderr,
        )
        return False

    if (
        branch
        not in (
            git_out(["branch", "--format=%(refname:short)"], cwd) or ""
        ).splitlines()
    ):
        print(to_ascii("[error] no such local branch: %s" % branch), file=sys.stderr)
        return False

    if git_out(["remote", "get-url", remote], cwd) is None:
        print(
            "[error] remote %s is not configured. Run:\n"
            "  git remote add %s https://github.com/<owner>/<repo>.git"
            % (remote, remote),
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
            "[error] git identity is not configured, commits cannot be signed. Missing: "
            + ", ".join(missing)
            + '\n  git config --global user.name "<name>"\n'
            '  git config --global user.email "<email>"',
            file=sys.stderr,
        )
        return False

    return True


def push_with_retry(cmd, cwd, attempts, wait):
    """执行推送并按需重试：网络链路会间歇性 reset，单次失败不代表最终推不上去。

    attempts 的取值范围由入口把关（>= 1）：这里不兜 0 次的情形——循环一次不跑却回报「推送失败」，
    等于把「根本没推」说成「推了且推失败」，是假信号。
    """
    for i in range(attempts):
        if i:
            print("attempt %d/%d (waited %d s)" % (i + 1, attempts, wait))
            time.sleep(wait)
        proc = run(cmd, cwd)
        echo(proc.stdout)
        echo(proc.stderr)
        if proc.returncode == 0:
            return True
    return False


def remote_sha(branch, remote, cwd, attempts, wait):
    """读远端分支指向的提交：分支不存在返回 None，读取失败返回 False。

    网络链路会间歇性 reset，读取同样带重试，把链路抖动和「分支不存在」区分开。
    attempts / wait 由调用方给（本函数的两个读数都来自命令行参数，不留默认值——
    留了也没人用，还会让人以为默认值与 `--retries` / `--wait` 的默认数是一回事）。
    """
    for i in range(attempts):
        if i:
            print("remote read retry %d/%d" % (i + 1, attempts))
            time.sleep(wait)
        proc = run(["git", "ls-remote", remote, f"refs/heads/{branch}"], cwd)
        if proc.returncode == 0:
            for line in proc.stdout.splitlines():
                fields = line.split()
                if len(fields) == 2:
                    return fields[0]
            return None
        print("[warn] remote read failed:", file=sys.stderr)
        echo(proc.stderr)
    return False


def build_push_cmd(remote, branch, dry_run):
    """拼推送命令。

    预演不带 --set-upstream：git 对「--dry-run 时是否仍写本地分支配置」没有承诺，
    脚本层不赌它——预演就是只读，远端与本地上游配置都不动。
    """
    cmd = ["git", "push"]
    if not dry_run:
        cmd.append("--set-upstream")
    cmd += [remote, branch]
    if dry_run:
        cmd.append("--dry-run")
    return cmd


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
        help="只跑校验并带 --dry-run 推送，不真的改远端；预演那趟不带 --set-upstream",
    )
    args = parser.parse_args()

    # 参数范围在入口就判死：--retries 0 会让循环一次不跑却报「推送失败」，
    # --wait 负数会在 time.sleep 上抛未捕获异常（一条 traceback 而不是一条读数）。
    if args.retries < 1:
        print(
            "[error] --retries must be >= 1 (0 would push nothing and still report failure)",
            file=sys.stderr,
        )
        return 2
    if args.wait < 0:
        print("[error] --wait must be >= 0", file=sys.stderr)
        return 2

    apply_noninteractive_env()
    cwd = str(Path(args.repo).resolve())

    branch = args.branch or current_branch(cwd)
    if not branch:
        print(
            "[error] cannot resolve the current branch (detached HEAD?), use --branch",
            file=sys.stderr,
        )
        return 2

    if not preflight(branch, args.remote, cwd):
        return 2

    before = git_out(["rev-parse", f"refs/heads/{branch}"], cwd)
    remote_url = git_out(["remote", "get-url", args.remote], cwd)
    print(to_ascii("repo: %s" % cwd))
    print(to_ascii("branch: %s @ %s" % (branch, before)))
    # 远端地址回显前先抹凭据段：scheme://user:token@host 里那一截可能是口令，
    # 原样打出来就随控制台滚动或流水线日志一起外泄了。
    print(
        to_ascii(
            "remote: %s -> %s"
            % (args.remote, redact_url(remote_url) if remote_url else "(unknown)")
        )
    )

    remote_now = remote_sha(branch, args.remote, cwd, args.retries, args.wait)
    if remote_now is False:
        print("[remote] branch state unknown: remote read failed")
    elif remote_now is None:
        print(
            to_ascii("[remote] branch %s does not exist on %s" % (branch, args.remote))
        )
    else:
        print(to_ascii("[remote] branch %s currently at %s" % (branch, remote_now)))

    push_cmd = build_push_cmd(args.remote, branch, args.dry_run)

    if not push_with_retry(push_cmd, cwd, args.retries, args.wait):
        print("PUSH FAILED", file=sys.stderr)
        return 1

    if args.tag:
        tag_cmd = ["git", "push", args.remote, args.tag]
        if args.dry_run:
            tag_cmd.append("--dry-run")
        if not push_with_retry(tag_cmd, cwd, args.retries, args.wait):
            print(to_ascii("TAG PUSH FAILED: %s" % args.tag), file=sys.stderr)
            return 1

    echo(run(["git", "branch", "-vv"], cwd).stdout)

    if args.dry_run:
        print("[dry-run] remote untouched, consistency check skipped")
        return 0

    after = remote_sha(branch, args.remote, cwd, args.retries, args.wait)
    if after is False:
        print("[check failed] remote is unreadable, cannot confirm", file=sys.stderr)
        return 1
    if after != before:
        print(
            to_ascii(
                "[check failed] remote %s = %s, local = %s, they differ"
                % (branch, after, before)
            ),
            file=sys.stderr,
        )
        return 1

    print(to_ascii("[check passed] remote %s points at %s" % (branch, after)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
