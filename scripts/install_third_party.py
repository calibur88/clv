#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""第三方库本地安装：把 third-party/ 里的源码构建并安装到 build/_install。

用法（项目根目录）：
    python scripts/install_third_party.py                # 已装好则跳过
    python scripts/install_third_party.py --force       # 强制重装
    python scripts/install_third_party.py --config Release
    python scripts/install_third_party.py --generator "Visual Studio 18 2026"
    python scripts/install_third_party.py --sanitize=address  # 插桩构型，装进 build/_install_asan

Ninja 需要 MSVC 环境（先在“Developer Command Prompt / vcvars64”里跑，或用 --generator 换回 VS 生成器）。
顶层把 build/_install 挂进 CMAKE_PREFIX_PATH，用 find_package(<pkg> QUIET) 找这些包；找不到再走源码内建或直接定位。

本件有两个安装前缀：默认那个（build/_install）由顶层自动挂上；`--sanitize` 那一档装进独立前缀
（build/_install_asan），是第二个落点，顶层不认它 —— 用它配库必须显式传 `-DCMAKE_PREFIX_PATH=<该前缀>`，
否则 find_package 只会看见默认前缀里那份未插桩的预编译件（混链即报 annotate 系列不匹配）。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

# 子进程兜底超时（配置 / 编译 / 安装都在内）：锁文件或被占用的输出会让脚本无限期挂着，
# 且日志不再动，从外面分不出是慢还是死。取 30 分钟是照冷缓存下最慢一次 flac 全量构建留的余量。
BUILD_TIMEOUT_SEC = 1800

ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "third-party"
BUILD_DIR = ROOT / "build"
INSTALL_DIR = BUILD_DIR / "_install"

# ---------- sanitizer 构型（--sanitize=…）----------
# 为什么要有这一路：库与测试带 /fsanitize=address 后，对象里带 annotate 元数据，
# 链接 _install 里那份未插桩的 gtest/spdlog/libFLAC 预编译静态件会报
# LNK2038: annotate_string / annotate_vector / annotate_optional（ASan 构型与主构型混链时实测报这三条）。
# 落法是同一套开关把这几包重编一份、装进独立前缀，默认构型与默认落点一律不动。
#
# 开关只给编译侧（CMAKE_C_FLAGS / CMAKE_CXX_FLAGS），不给链接侧：link.exe 不认
# /fsanitize=address（实测 `LINK : warning LNK4044: 无法识别的选项"/fsanitize=address"；已忽略`），
# ASan 运行库由链接器从插桩 obj 推导（推导时会忽略 /INCREMENTAL，报 LNK4075，属预期）。
# 只列 address 一路：MSVC 14.51 不认 /fsanitize=undefined（实测
# `cl: 命令行 warning D9002 :忽略未知选项"/fsanitize=undefined"`，静默忽略而非报错），
# 把不支持的开关写成能过会让 UB 一栏假绿。
#
# 这一路只有 MSVC 那台机器上有意义：开关形制是 cl.exe 的 /fsanitize=address，别的平台的编译器
# 不认；在非 Windows 上照投这套参数，只会造出一个必然失败的构型而不是当场拒判，故入口直接拒。
SANITIZE_FLAGS = {
    "address": [
        "-DCMAKE_C_FLAGS=/fsanitize=address",
        "-DCMAKE_CXX_FLAGS=/fsanitize=address",
    ],
}
# 独立前缀 / 独立构建目录名后缀：与正常构型的 _install、*-build 完全错开。
SANITIZE_DIR_SUFFIX = {"address": "_asan"}
SANITIZE_BUILD_SUFFIX = {"address": "-asan"}
# SDL3 只被 demo 播放器用、库与测试都不链它，插桩构型不建（要插桩的只有 spdlog/googletest/flac）。
SANITIZE_SKIP = {"address": ("SDL3",)}

# (源目录名, 构建目录名, 安装后的 config 文件, 额外 cmake 选项)
PACKAGES = [
    ("spdlog", "spdlog-build", "lib/cmake/spdlog/spdlogConfig.cmake", []),
    (
        "googletest",
        "gtest-build",
        "lib/cmake/GTest/GTestConfig.cmake",
        ["-Dgtest_force_shared_crt=ON"],
    ),
    # demo 层的显示与发声后端；只取构建所需子集（源树里没有 test/ examples/ 等目录）
    (
        "SDL3",
        "sdl3-build",
        "cmake/SDL3Config.cmake",
        [
            "-DSDL_SHARED=ON",
            "-DSDL_STATIC=OFF",
            "-DSDL_TESTS=OFF",
            "-DSDL_EXAMPLES=OFF",
        ],
    ),
    # 音频层 v1 复用 libFLAC（装头、库与 config 包，供顶层 find_package(FLAC)）；程序/示例/测试/文档/ogg 都不建
    (
        "flac",
        "flac-build",
        # 安装产物里的目录名是 FLAC（CMake 的包名大小写照 config 文件生成），
        # 写成小写 flac/ 在 Windows 上侥幸命中、到类 Unix 就成了「装完判未装」。
        "lib/cmake/FLAC/flac-config.cmake",
        [
            "-DBUILD_PROGRAMS=OFF",
            "-DBUILD_EXAMPLES=OFF",
            "-DBUILD_TESTING=OFF",
            "-DBUILD_CXXLIBS=OFF",
            "-DBUILD_DOCS=OFF",
            "-DINSTALL_MANPAGES=OFF",
            "-DWITH_OGG=OFF",
        ],
    ),
]

# 许可文本清单：各包 cmake 的安装规则不一致（SDL3 自带、flac 不带），故由本脚本统一补投到 _install。
# flac 这一条列五份：不带后缀的 COPYING 与四份带后缀的 COPYING.FDL / COPYING.GPL / COPYING.LGPL /
# COPYING.Xiph。带后缀那四份覆盖不同组件——链进去的 libFLAC 适用 COPYING.Xiph（Xiph.Org 的 BSD 式），
# GPL / LGPL / FDL 三份属未构建的程序、旧组件与文档；不带后缀那份是 flac 自己的总许可，一并投出。
# 打印的份数以这份清单为准，清单加一项就投一项，别在注释里另记一个数。
LICENSE_FILES = {
    "spdlog": ["LICENSE"],
    "googletest": ["LICENSE"],
    "SDL3": ["LICENSE.txt"],
    "flac": ["COPYING", "COPYING.FDL", "COPYING.GPL", "COPYING.LGPL", "COPYING.Xiph"],
}

MULTI_CONFIG_PREFIXES = ("Visual Studio", "Xcode", "Ninja Multi-Config")

# --config 的白名单：CMake 的四个标准配置名。不校验时，拼错的配置名会一路走到
# 多配置生成器的 `--config <拼错的名字>` 那步才由 cmake 报错，或在单配置里被当成
# CMAKE_BUILD_TYPE 的怪值传给编译器 —— 拒在入口最省。
CMAKE_CONFIGS = ("Debug", "Release", "RelWithDebInfo", "MinSizeRel")


def has_msvc_env() -> bool:
    return "VCToolsInstallDir" in os.environ or "VSCMD_ARG_TGT_ARCH" in os.environ


# 没有 MSVC 环境时按这个顺序找本机的 Visual Studio 生成器；三个都不在支持列表里就直接报错，
# 不再悄悄退回 Ninja —— 那恰好是这台机器上用不了的生成器（Ninja 需要 vcvars 环境）。
VS_GENERATOR_VERSIONS = ("18 2026", "17 2022", "16 2019")

# 取帮助文本也带超时：本件自定「所有子进程都带兜底超时」，cmake 起不来时这里不能无限等。
CMAKE_HELP_TIMEOUT_SEC = 60


def cmake_help_text() -> str:
    """跑一次 `cmake --help` 取生成器清单，同一份输出只取一遍（过去写在循环体内，最多跑三遍）。

    解码取 utf-8 + replace：帮助文本只用来认生成器名，坏字节换成占位符不影响判定；
    用严格模式的话，本机码页或上游文案里的坏字节会抛未捕获异常，把「挑生成器」这件小事变成崩溃。
    """
    try:
        proc = subprocess.run(
            ["cmake", "--help"],
            capture_output=True,
            check=False,
            timeout=CMAKE_HELP_TIMEOUT_SEC,
        )
    except subprocess.TimeoutExpired:
        raise SystemExit(
            "cmake --help timed out (%d s); cannot list available generators"
            % CMAKE_HELP_TIMEOUT_SEC
        )
    return proc.stdout.decode("utf-8", errors="replace")


def pick_generator(cli_value: str | None) -> str:
    if cli_value:
        return cli_value
    if sys.platform == "win32" and not has_msvc_env():
        # 没有 vcvars 环境时 Ninja 找不到编译器，退回 VS 生成器
        print(
            "[info] no MSVC environment detected (vcvars64); "
            "falling back to a Visual Studio generator."
        )
        help_text = cmake_help_text()
        for version in VS_GENERATOR_VERSIONS:
            if f"Visual Studio {version}" in help_text:
                return f"Visual Studio {version}"
        raise SystemExit(
            "no usable Visual Studio generator on this machine (tried: %s), "
            "and Ninja needs an MSVC environment this shell does not have. "
            "Run the developer environment script (Developer Command Prompt / vcvars64) "
            "first, or pass --generator with a generator name from `cmake --help`."
            % ", ".join("Visual Studio " + v for v in VS_GENERATOR_VERSIONS)
        )
    return "Ninja"


def is_multi_config(generator: str) -> bool:
    return generator.startswith(MULTI_CONFIG_PREFIXES)


def run(cmd: list[str], log_path: Path, timeout: int = BUILD_TIMEOUT_SEC) -> None:
    """执行命令，输出写日志文件；失败或超时都抛错并回显日志尾部。

    超时要判：cmake --build / --install 挂住（锁文件、杀软的交互式弹窗）本来会无限等，
    挂住之后连日志都不再动，从外面看不出是慢还是死。

    日志句柄按二进制开：`encoding=` 那个声明只约束「Python 自己往句柄写 str」，子进程拿到的是
    操作系统层面的文件描述符，落盘字节由它自己的码页决定 —— 先以 utf-8 打开再交出去，声明根本不生效。
    回读时自己按字节解码：utf-8 + replace，中文 Windows 上那些非 utf-8 字节换成占位符后逐行转义打印，
    既不谎称日志是 utf-8，也不让回显打断脚本；成败一律看退出码，不看日志文本。
    """
    print("+ " + " ".join(str(c) for c in cmd))
    with log_path.open("wb") as log:
        try:
            result = subprocess.run(
                cmd, stdout=log, stderr=subprocess.STDOUT, timeout=timeout
            )
        except subprocess.TimeoutExpired:
            log.flush()
            raise SystemExit(
                "command timed out (%d s): %s" % (timeout, " ".join(map(str, cmd)))
            )
    # 已知局限：超时只杀直接子进程，cmake 起的 MSBuild / ninja 孙进程不在内。
    # 按进程树杀需要 psutil 之类的第三方件，而本仓的 Python 依赖清单里只有 black 与 numpy，
    # 所以这里保留 stdlib 的行为，局限登记在此。
    if result.returncode != 0:
        tail = (
            log_path.read_bytes().decode("utf-8", errors="replace").splitlines()[-20:]
        )
        raise SystemExit(
            "command failed (exit code %d): %s\n%s"
            % (
                result.returncode,
                " ".join(cmd),
                "\n".join(ascii(line) for line in tail),
            )
        )


def copy_licenses(name: str, install_dir: Path) -> tuple[list[str], list[str]]:
    """把许可文本投到 `<安装前缀>/licenses/<name>/`，返回（投出的件名清单，源树里缺的件名清单）。

    各包的 cmake 安装规则不保证带许可（SDL3 自带、flac 不带），故由本脚本统一补投，
    使安装前缀里许可文本与库文件同树可读。已存在且内容相同则不写。
    源树里没有的那份不当「已补齐」也不静默跳过：对外声称许可文本由本脚本补齐，缺一份就是
    同树读不到那一份，故缺项逐条报出并交回调用方计入退出码。
    """
    copied: list[str] = []
    missing: list[str] = []
    target_dir = install_dir / "licenses" / name
    for fname in LICENSE_FILES.get(name, []):
        src = SRC_DIR / name / fname
        if not src.is_file():
            missing.append("%s/%s" % (name, fname))
            print(
                "[warn] license file absent in source tree, nothing staged for it: %s"
                % ascii("%s/%s" % (name, fname))
            )
            continue
        target_dir.mkdir(parents=True, exist_ok=True)
        dst = target_dir / fname
        if dst.is_file() and dst.read_bytes() == src.read_bytes():
            copied.append("%s(already-there)" % fname)
            continue
        shutil.copyfile(src, dst)
        copied.append(fname)
    return copied, missing


def install_package(
    name: str,
    build_name: str,
    marker: str,
    extra: list[str],
    generator: str,
    config: str,
    force: bool,
    install_dir: Path,
    log_suffix: str = "",
) -> list[str]:
    """构建并把一个包装进 install_dir；返回源树里缺、因而没投出去的许可文本项。

    安装前缀由调用方传进来（默认 build/_install，sanitizer 那一档是 build/_install_asan），
    本函数不读模块级变量 —— 过去靠改全局变量改写出落点，装哪一份与投哪一份许可隐式绑在一起，
    读代码的人得分两步确认才知道当前落点是谁。
    """
    source = SRC_DIR / name
    if not source.is_dir():
        raise SystemExit("missing third-party source directory: %s" % source)

    build = BUILD_DIR / build_name
    installed = install_dir / marker

    if installed.is_file() and not force:
        got, missing = copy_licenses(name, install_dir)
        print(
            "[skip] %s: already installed (marker %s); --force rebuilds it; "
            "license entries verified %d %s" % (name, marker, len(got), got)
        )
        return missing

    cfg = [
        "cmake",
        "-S",
        str(source),
        "-B",
        str(build),
        "-G",
        generator,
        f"-DCMAKE_INSTALL_PREFIX={install_dir.as_posix()}",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        *extra,
    ]
    if not is_multi_config(generator):
        cfg.append(f"-DCMAKE_BUILD_TYPE={config}")

    BUILD_DIR.mkdir(exist_ok=True)
    # 日志名带 sanitizer 构型的后缀：sanitizer 那一轮的日志不覆盖正常构型留在 build/ 的那些。
    run(cfg, BUILD_DIR / f"{name}-configure{log_suffix}.log")

    build_cmd = ["cmake", "--build", str(build)]
    if is_multi_config(generator):
        build_cmd += ["--config", config]
    run(build_cmd, BUILD_DIR / f"{name}-build{log_suffix}.log")

    install_cmd = ["cmake", "--install", str(build)]
    if is_multi_config(generator):
        install_cmd += ["--config", config]
    run(install_cmd, BUILD_DIR / f"{name}-install{log_suffix}.log")

    if not installed.is_file():
        raise SystemExit(
            "%s: marker %s not found after install, check the %s-*.log files"
            % (name, marker, name)
        )
    got, missing = copy_licenses(name, install_dir)
    print(
        "[done] %s -> %s; license entries staged %d %s"
        % (name, install_dir.relative_to(ROOT), len(got), got)
    )
    return missing


def main() -> int:
    parser = argparse.ArgumentParser(description="第三方库本地安装")
    parser.add_argument("--force", action="store_true", help="即使已安装也重新构建安装")
    parser.add_argument(
        "--config",
        default="Debug",
        choices=CMAKE_CONFIGS,
        help="构建配置（默认 Debug），只认 CMake 的四个标准配置名",
    )
    parser.add_argument("--generator", default=None, help="强制指定 CMake 生成器")
    parser.add_argument(
        "--sanitize",
        default=None,
        choices=tuple(SANITIZE_FLAGS),
        help="用 sanitizer 插桩开关重编依赖并装进独立前缀（address → build/_install_asan，"
        "第二个落点，配库要显式传 -DCMAKE_PREFIX_PATH）；不给则是完全不变的默认构型与默认落点",
    )
    args = parser.parse_args()

    san = args.sanitize
    if san and sys.platform != "win32":
        raise SystemExit(
            "--sanitize is only supported on Windows: the switches are MSVC cl.exe form "
            "(/fsanitize=address). On this platform they would only produce a "
            "configuration that cannot succeed, so this is refused instead."
        )

    generator = pick_generator(args.generator)
    install_dir = (
        BUILD_DIR / ("_install" + SANITIZE_DIR_SUFFIX[san]) if san else INSTALL_DIR
    )
    print(
        "generator: %s\nbuild config: %s\ninstall prefix: %s"
        % (generator, args.config, install_dir)
    )
    if san:
        print(
            "sanitize config: %s (extra cmake options %s)" % (san, SANITIZE_FLAGS[san])
        )

    missing_licenses: list[str] = []
    for name, build_name, marker, extra in PACKAGES:
        if san and name in SANITIZE_SKIP[san]:
            print(
                "[skip] %s: not built in the sanitizer configuration "
                "(neither the library nor the tests link it)" % name
            )
            continue
        missing_licenses += install_package(
            name,
            build_name + (SANITIZE_BUILD_SUFFIX[san] if san else ""),
            marker,
            list(extra) + (SANITIZE_FLAGS[san] if san else []),
            generator,
            args.config,
            args.force,
            install_dir,
            SANITIZE_BUILD_SUFFIX[san] if san else "",
        )

    if san:
        print(
            "\nsanitizer configuration installed into %s; the default prefix build/_install "
            "is untouched. This is the second install prefix, the top-level CMakeLists does "
            "not add it: pass it explicitly when configuring the library, "
            "-DCMAKE_PREFIX_PATH=%s" % (install_dir, install_dir.as_posix())
        )
    else:
        print(
            "\nThe top-level CMakeLists adds build/_install to CMAKE_PREFIX_PATH by itself; "
            "no extra parameter is needed."
        )

    if missing_licenses:
        print(
            "[license] %d entry(ies) listed in LICENSE_FILES are absent from the source "
            "tree, so they are not readable next to the installed libraries: %s"
            % (
                len(missing_licenses),
                ", ".join(ascii(item) for item in missing_licenses),
            )
        )
        print(
            "[install_third_party] NOT COMPLETE: libraries installed, licenses missing"
        )
        return 1
    print("[install_third_party] OK: all listed license entries staged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
