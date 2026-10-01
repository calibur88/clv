#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""第三方库本地安装：把 third-party/ 里的源码构建并安装到 build/_install。

用法（项目根目录）：
    python script/install_third_party.py                # 已装好则跳过
    python script/install_third_party.py --force       # 强制重装
    python script/install_third_party.py --config Release
    python script/install_third_party.py --generator "Visual Studio 18 2026"

Ninja 需要 MSVC 环境（先在“Developer Command Prompt / vcvars64”里跑，或用 --generator 换回 VS 生成器）。
CLV 顶层用 find_package(<pkg> CONFIG REQUIRED) 取它们，CMAKE_PREFIX_PATH 指向 build/_install。
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "third-party"
BUILD_DIR = ROOT / "build"
INSTALL_DIR = BUILD_DIR / "_install"

# (源目录名, 构建目录名, 安装后的 config 文件, 额外 cmake 选项)
PACKAGES = [
    ("spdlog", "spdlog-build", "lib/cmake/spdlog/spdlogConfig.cmake", []),
    (
        "googletest",
        "gtest-build",
        "lib/cmake/GTest/GTestConfig.cmake",
        ["-Dgtest_force_shared_crt=ON"],
    ),
]

MULTI_CONFIG_PREFIXES = ("Visual Studio", "Xcode", "Ninja Multi-Config")


def has_msvc_env() -> bool:
    return "VCToolsInstallDir" in os.environ or "VSCMD_ARG_TGT_ARCH" in os.environ


def pick_generator(cli_value: str | None) -> str:
    if cli_value:
        return cli_value
    if sys.platform == "win32" and not has_msvc_env():
        # 没有 vcvars 环境时 Ninja 找不到编译器，退回 VS 生成器
        print("提示：未检测到 MSVC 环境（vcvars64），退回 Visual Studio 生成器。")
        for version in ("18 2026", "17 2022", "16 2019"):
            help_text = subprocess.run(
                ["cmake", "--help"], text=True, capture_output=True, check=False
            ).stdout
            if f"Visual Studio {version}" in help_text:
                return f"Visual Studio {version}"
    return "Ninja"


def is_multi_config(generator: str) -> bool:
    return generator.startswith(MULTI_CONFIG_PREFIXES)


def run(cmd: list[str], log_path: Path) -> None:
    """执行命令，输出写日志文件；失败即抛错并回显日志尾部。"""
    print("+ " + " ".join(str(c) for c in cmd))
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        tail = log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-20:]
        raise SystemExit(
            f"命令失败（退出码 {result.returncode}）：{' '.join(cmd)}\n"
            + "\n".join(tail)
        )


def install_package(
    name: str,
    build_name: str,
    marker: str,
    extra: list[str],
    generator: str,
    config: str,
    force: bool,
) -> None:
    source = SRC_DIR / name
    if not source.is_dir():
        raise SystemExit(f"缺少第三方源码目录：{source}")

    build = BUILD_DIR / build_name
    installed = INSTALL_DIR / marker

    if installed.is_file() and not force:
        print(f"[跳过] {name}：已安装（{marker}）。要重装加 --force")
        return

    cfg = [
        "cmake",
        "-S",
        str(source),
        "-B",
        str(build),
        "-G",
        generator,
        f"-DCMAKE_INSTALL_PREFIX={INSTALL_DIR.as_posix()}",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        *extra,
    ]
    if not is_multi_config(generator):
        cfg.append(f"-DCMAKE_BUILD_TYPE={config}")

    BUILD_DIR.mkdir(exist_ok=True)
    run(cfg, BUILD_DIR / f"{name}-configure.log")

    build_cmd = ["cmake", "--build", str(build)]
    if is_multi_config(generator):
        build_cmd += ["--config", config]
    run(build_cmd, BUILD_DIR / f"{name}-build.log")

    install_cmd = ["cmake", "--install", str(build)]
    if is_multi_config(generator):
        install_cmd += ["--config", config]
    run(install_cmd, BUILD_DIR / f"{name}-install.log")

    if not installed.is_file():
        raise SystemExit(f"{name} 安装后找不到 {marker}，检查 {name}-*.log")
    print(f"[完成] {name} → {INSTALL_DIR.relative_to(ROOT)}")


def main() -> int:
    parser = argparse.ArgumentParser(description="第三方库本地安装")
    parser.add_argument("--force", action="store_true", help="即使已安装也重新构建安装")
    parser.add_argument("--config", default="Debug", help="构建配置（默认 Debug）")
    parser.add_argument("--generator", default=None, help="强制指定 CMake 生成器")
    args = parser.parse_args()

    generator = pick_generator(args.generator)
    print(f"生成器：{generator}\n构建配置：{args.config}\n安装前缀：{INSTALL_DIR}")

    for name, build_name, marker, extra in PACKAGES:
        install_package(
            name, build_name, marker, extra, generator, args.config, args.force
        )

    print(
        "\n顶层 CMakeLists 会自动把 build/_install 加进 CMAKE_PREFIX_PATH，无需额外参数。"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
