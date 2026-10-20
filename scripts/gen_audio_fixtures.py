#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 tests/win32/audio/audio_fixtures_p.h —— 音频帧头 / 承载块 / 掩码剥离用例的真素材与期望值。

帧字节从真实 FLAC 件里按帧边界切出来，期望值由一份**独立实现的位域解析件**现算
（与 C++ 侧不同源），再与该流自身的流信息交叉核对——块大小 / 采样率 / 声道 / 位深对不上就直接失败退出。

两个输入目录由命令行给出：
    python scripts/gen_audio_fixtures.py --ref <含位域解析件的目录> --src <含素材 .flac 的目录>
生成后跑 scripts/format_all.py --write。源件的文件名、字节数与整文件 SHA-256 写进产物头注释：
每条用例对回 --src 目录里的一个源件。
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "tests/win32/audio/audio_fixtures_p.h"

# (符号名前缀, --src 目录内的素材文件名, 用途说明)——目录由 --src 给，文件名是本件的固定预期，缺件即失败退出
SOURCES = [
    ("kFrame16Fixed", "at1_默认.flac", "固定块流 16bit / 2ch / 44100，整首帧"),
    ("kFrame24Fixed", "at1_24bit_源.flac", "位深码 0b110 = 24 bit"),
    ("kFrameVarBlock", "variable.flac", "变长块流，块策略位为 1"),
    ("kFrameBig32768", "at1_lax_b_32768.flac", "块大小码 0b1111 = 32768，越流式子集"),
    ("kFrame6Ch", "at7_6ch_ext.flac", "6 声道，注释块里带扩展声道掩码"),
]
STREAMINFO_SRC = "at1_默认.flac"
VORBIS_SRC = "at7_6ch_ext.flac"
MASK_KEY = b"WAVEFORMATEXTENSIBLE_CHANNEL_MASK"

# 采样率码表的定值采样率档（1..11），0 与 12..14 走特殊路径
RATE_CODES = {
    1: 88200,
    2: 176400,
    3: 192000,
    4: 8000,
    5: 16000,
    6: 22050,
    7: 24000,
    8: 32000,
    9: 44100,
    10: 48000,
    11: 96000,
}
BITS_CODES = {1: 8, 2: 12, 4: 16, 5: 20, 6: 24, 7: 32}


def blocksize_of(code: int, extra) -> int:
    if code == 1:
        return 192
    if 2 <= code <= 5:
        return 144 << code
    if code in (6, 7):
        return int(extra or 0) + 1
    return 1 << code


def meta_block(buf: bytes, want_type: int):
    """按 FLAC 文件级块链取某类块的载荷（跳过 fLaC 标记）。"""
    p = 4
    while p + 4 <= len(buf):
        head = buf[p]
        last = (head >> 7) & 1
        typ = head & 0x7F
        ln = int.from_bytes(buf[p + 1 : p + 4], "big")
        if typ == want_type:
            return buf[p + 4 : p + 4 + ln]
        p += 4 + ln
        if last:
            return None
    return None


def c_array(name: str, data: bytes) -> str:
    lines = [f"\tinline constexpr unsigned char {name}[] = {{"]
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02X}u" for b in data[i : i + 12])
        lines.append("\t\t" + chunk + ",")
    lines.append("\t};")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="生成音频帧头与元数据用例的素材头")
    ap.add_argument("--ref", required=True, help="独立实现的位域解析件所在目录")
    ap.add_argument("--src", required=True, help="素材 .flac 所在目录")
    args = ap.parse_args()

    sys.path.insert(0, str(Path(args.ref).resolve()))
    import flac_frames as F  # noqa: E402

    src = Path(args.src)
    header: list[str] = []
    o = header.append
    o("/* audio_fixtures_p.h")
    o(" *")
    o(" * 由 scripts/gen_audio_fixtures.py 生成，请勿手改，也不要手填里面的数字。")
    o(" * 帧字节按帧边界从真实 FLAC 件里切出；期望值由另一份独立实现的位域解析件解出，")
    o(" * 并与该流的流信息交叉核对。")
    o(
        " * 重跑：python scripts/gen_audio_fixtures.py --ref <位域解析件目录> --src <素材目录>，"
    )
    o(" * 然后跑 scripts/format_all.py --write。")
    o(" *")

    cases = []
    for sym, fname, note in SOURCES:
        path = src / fname
        if not path.is_file():
            raise SystemExit(f"素材不在场：{fname}")
        buf = path.read_bytes()
        start = F.first_frame_offset(buf)
        frames = F.parse_frames(buf, start, sample_verify=0)
        if not frames:
            raise SystemExit(f"{fname}: 找不到帧")
        f0 = frames[0]
        frame = buf[f0["offset"] : f0["end"]]
        if not F.crc16(frame[:-2]) == ((frame[-2] << 8) | frame[-1]):
            raise SystemExit(f"{fname}: 首帧 CRC-16 不过，帧边界不可信")
        si = F.streaminfo(buf)
        code = f0["blocksize_code"]
        blocksize = blocksize_of(code, f0["extra_blocksize"])
        chan = f0["channel_code"] + 1 if f0["channel_code"] <= 7 else 2
        if si["channels"] != chan:
            raise SystemExit(f"{fname}: 帧头声道 {chan} 与流信息 {si['channels']} 不符")
        if (
            f0["samplerate_code"] in RATE_CODES
            and RATE_CODES[f0["samplerate_code"]] != si["sample_rate"]
        ):
            raise SystemExit(f"{fname}: 帧头采样率与流信息不符")
        if (
            f0["bits_code"] in BITS_CODES
            and BITS_CODES[f0["bits_code"]] != si["bits_per_sample"]
        ):
            raise SystemExit(f"{fname}: 帧头位深与流信息不符")
        if f0["blocking_strategy"] == 0 and blocksize not in (
            si["min_blocksize"],
            si["max_blocksize"],
        ):
            raise SystemExit(
                f"{fname}: 固定块流的块大小 {blocksize} 不在流信息给出的区间"
            )
        sha = hashlib.sha256(buf).hexdigest()
        o(
            f" *   {sym}: {fname}（{len(buf)} B，sha256 {sha[:16]}）—— {note}；"
            f"首帧 @{f0['offset']}，帧长 {len(frame)} B，块大小码 {code} → {blocksize}"
        )
        cases.append(
            dict(
                sym=sym,
                frame=frame,
                hdr=f0["hdr_len"],
                blocksize=blocksize,
                rate=si["sample_rate"],
                channels=chan,
                depth=si["bits_per_sample"],
                number=f0["coded_number"],
                blocking=f0["blocking_strategy"],
            )
        )

    si_buf = (src / STREAMINFO_SRC).read_bytes()
    si = F.streaminfo(si_buf)
    si_block = meta_block(si_buf, 0)
    if si_block is None or len(si_block) != 34:
        raise SystemExit("取不到 34 字节的流信息块")
    o(
        f" *   kStreamInfo：{STREAMINFO_SRC} 的流信息块载荷（34 B），min_blk={si['min_blocksize']} "
        f"max_blk={si['max_blocksize']} rate={si['sample_rate']} ch={si['channels']} "
        f"bits={si['bits_per_sample']} total={si['total_samples']}"
    )

    ext_buf = (src / VORBIS_SRC).read_bytes()
    vc = meta_block(ext_buf, 4)
    if vc is None:
        raise SystemExit("取不到 Vorbis 注释块")
    mask_n = vc.upper().count(MASK_KEY)
    if mask_n == 0:
        raise SystemExit("注释块里没有扩展声道掩码，用例前提不成立")
    vendor = int.from_bytes(vc[0:4], "little")
    total_entries = int.from_bytes(vc[4 + vendor : 8 + vendor], "little")
    o(
        f" *   kVorbisMask：{VORBIS_SRC} 的注释块载荷（{len(vc)} B），含掩码键 {mask_n} 条、"
        f"共 {total_entries} 条；整文件 sha256 {hashlib.sha256(ext_buf).hexdigest()[:16]}"
    )
    o(" */")
    o("#ifndef CLV_TEST_AUDIO_FIXTURES_P_H")
    o("#define CLV_TEST_AUDIO_FIXTURES_P_H")
    o("")
    o("namespace clvtest")
    o("{")
    o("")

    body: list[str] = []
    b = body.append
    for c in cases:
        b(f"\t/* {c['sym']}：整帧（含帧头与帧尾 2 字节 CRC-16） */")
        b(c_array(f"{c['sym']}Bytes", c["frame"]))
        b(f"\tinline constexpr unsigned {c['sym']}Len = {len(c['frame'])}u;")
        b(f"\tinline constexpr unsigned {c['sym']}Hdr = {c['hdr']}u;")
        b(f"\tinline constexpr unsigned {c['sym']}Blocksize = {c['blocksize']}u;")
        b(f"\tinline constexpr unsigned {c['sym']}Rate = {c['rate']}u;")
        b(f"\tinline constexpr unsigned {c['sym']}Channels = {c['channels']}u;")
        b(f"\tinline constexpr unsigned {c['sym']}Depth = {c['depth']}u;")
        b(f"\tinline constexpr unsigned long long {c['sym']}Number = {c['number']}ull;")
        b(f"\tinline constexpr unsigned char {c['sym']}Blocking = {c['blocking']}u;")
        b("")

    b("\t/* 流信息块载荷 34 字节 */")
    b(c_array("kStreamInfoBytes", si_block))
    b("\tinline constexpr unsigned kStreamInfoLen = 34u;")
    b(f"\tinline constexpr unsigned kStreamInfoMinBlock = {si['min_blocksize']}u;")
    b(f"\tinline constexpr unsigned kStreamInfoMaxBlock = {si['max_blocksize']}u;")
    b(f"\tinline constexpr unsigned kStreamInfoMinFrame = {si['min_framesize']}u;")
    b(f"\tinline constexpr unsigned kStreamInfoMaxFrame = {si['max_framesize']}u;")
    b(f"\tinline constexpr unsigned kStreamInfoRate = {si['sample_rate']}u;")
    b(f"\tinline constexpr unsigned kStreamInfoChannels = {si['channels']}u;")
    b(f"\tinline constexpr unsigned kStreamInfoDepth = {si['bits_per_sample']}u;")
    b(
        f"\tinline constexpr unsigned long long kStreamInfoTotalSamples = {si['total_samples']}ull;"
    )
    b("")
    b("\t/* 带扩展声道掩码的 Vorbis 注释块载荷 */")
    b(c_array("kVorbisMaskBytes", vc))
    b(f"\tinline constexpr unsigned kVorbisMaskLen = {len(vc)}u;")
    b(f"\tinline constexpr unsigned kVorbisMaskEntries = {mask_n}u;")
    b(f"\tinline constexpr unsigned kVorbisTotalEntries = {total_entries}u;")
    b("")
    b("}\t // namespace clvtest")
    b("")
    b("#endif\t  // CLV_TEST_AUDIO_FIXTURES_P_H")
    b("")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(header + body), encoding="utf-8", newline="\n")
    print("\n".join(header[3:]))
    print(f"写出 {OUT.relative_to(ROOT)}：{OUT.stat().st_size} 字节")
    return 0


if __name__ == "__main__":
    sys.exit(main())
