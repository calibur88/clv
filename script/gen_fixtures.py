#!/usr/bin/env python3
"""容器层测试素材生成器。

写出三份**确定性**的 .clv 到 assets/fixtures/，供端到端测试与人工核对使用。

确定性是硬要求：字节流只能是输入的纯函数，不得含时间戳、路径、随机数或版本号以外的环境量。
CLV 版本号一旦变更（文件头 version_major / version_minor），产出字节随之变化——
那种情况下必须重跑本脚本并更新 assets/README.md 里的冻结值。

本脚本是容器写方的**独立实现**（不复用 C++ 侧代码），与库的读方互为交叉验证：
两边对不上，就说明有一方偏离了格式。
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parent.parent

BASE_TICKS = 1_081_080_000
VERSION_MAJOR = 1
VERSION_MINOR = 0

FLAG_KEYFRAME = 0x01
FLAG_FRAGMENT = 0x02
FLAG_LAST_FRAGMENT = 0x04
FLAG_HAS_PTS = 0x08

HEAD_INDEX_PRESENT = 0x01

STREAM_VIDEO = 0
STREAM_SUBTITLE = 2


# ---------------------------------------------------------------- CRC / varint


def crc32_mpeg2(data: bytes, seed: int = 0xFFFFFFFF) -> int:
    """CRC-32/MPEG-2：poly 0x04C11DB7，init 0xFFFFFFFF，不反射，xorout 0。"""
    crc = seed
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def uvarint(value: int) -> bytes:
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value & 0x7F)
    return bytes(out)


def zigzag(value: int) -> bytes:
    encoded = ((value << 1) ^ (value >> 63)) & 0xFFFFFFFFFFFFFFFF
    return uvarint(encoded)


# ---------------------------------------------------------------- 结构


def file_head(
    stream_count: int, flags: int, duration: int, packets: int, index_offset: int
) -> bytes:
    body = b"CLVFILE\x00" + struct.pack(
        "<HHB B HH", VERSION_MAJOR, VERSION_MINOR, stream_count, flags, 0, 0
    )
    body = b"CLVFILE\x00" + struct.pack("<HH", VERSION_MAJOR, VERSION_MINOR)
    body += struct.pack("<BB", stream_count, flags)
    body += struct.pack("<H", 0)  # 14 reserved
    body += struct.pack("<QQQ", duration, packets, index_offset)  # 16 / 24 / 32
    body += struct.pack("<I", 0)  # 40 header_crc32 占位
    body += b"\x00" * 20  # 44..63 reserved
    assert len(body) == 64, len(body)
    return body[:40] + struct.pack("<I", crc32_mpeg2(body[:40])) + body[44:]


def stream_desc(
    stream_id: int,
    stream_type: int,
    codec_id: int,
    max_packet: int,
    timebase_num: int,
    timebase_den: int,
    bit_depth: int,
    sample_rate: int = 0,
    channels: int = 0,
    ext_offset: int = 0,
) -> bytes:
    body = struct.pack(
        "<BBBB", stream_id, stream_type, codec_id, 1
    )  # codec_version = 1
    body += struct.pack("<QQ", timebase_num, timebase_den)
    body += struct.pack("<I", max_packet)
    body += struct.pack("<Q", 0)  # 24 first_dts：v1 强制 0
    body += struct.pack("<Q", 0)  # 32 language
    body += struct.pack("<I", sample_rate)
    body += struct.pack("<H", channels)
    body += struct.pack("<H", bit_depth)
    body += struct.pack("<BB", 0, 0)  # 48 codec_flags_0 / 49 vlc_table_id
    body += struct.pack("<I", ext_offset)  # 50
    body += struct.pack("<BB", 0, 0)  # 54 codec_flags_1 / 55 layer_id
    body += struct.pack("<I", 0)  # 56 crc32 占位
    body += struct.pack("<I", 0)  # 60 reserved
    assert len(body) == 64, len(body)
    return body[:56] + struct.pack("<I", crc32_mpeg2(body[:56])) + body[60:]


def ext_block(
    ext_type: int, ext_version: int, data: bytes, next_offset: int = 0
) -> bytes:
    assert len(data) <= 0xFFFF
    body = (
        b"CLVX"
        + struct.pack("<BB", ext_type, ext_version)
        + struct.pack("<H", len(data))
    )
    body += struct.pack("<Q", next_offset)
    body += data
    body += struct.pack("<I", 0)  # crc32 占位
    covered = len(body) - 4
    return (
        body[:covered]
        + struct.pack("<I", crc32_mpeg2(body[:covered]))
        + body[covered + 4 :]
    )


def packet(
    stream_id: int,
    dts_delta: int,
    pts_delta: int,
    payload: bytes,
    *,
    keyframe: bool = False,
    fragment_index: int | None = None,
    last_fragment: bool = False,
) -> bytes:
    flags = FLAG_HAS_PTS  # 冗余位：各片恒 1
    if keyframe:
        flags |= FLAG_KEYFRAME
    if fragment_index is not None:
        flags |= FLAG_FRAGMENT
    if last_fragment:
        flags |= FLAG_LAST_FRAGMENT

    body = struct.pack("<BB", stream_id, flags)
    if fragment_index is not None:
        body += uvarint(fragment_index)
    body += uvarint(dts_delta)
    if fragment_index is None or fragment_index == 0:  # 帧首包才带 pts_delta
        body += zigzag(pts_delta)
    body += uvarint(len(payload))
    body += uvarint(0)  # ext_len_varint：v1 = 0
    body += payload
    body += struct.pack("<I", 0)  # crc32 占位
    total = len(body)
    crc = crc32_mpeg2(body[: total - 4])
    return struct.pack("<I", total) + body[: total - 4] + struct.pack("<I", crc)


def index_head(count: int) -> bytes:
    body = struct.pack("<II", count, 0) + struct.pack("<I", 0) + struct.pack("<I", 0)
    return body[:8] + struct.pack("<I", crc32_mpeg2(body[:8])) + body[12:]


def index_entry(stream_id: int, keyframe: bool, file_offset: int, dts: int) -> bytes:
    return struct.pack(
        "<BBHI", stream_id, 1 if keyframe else 0, 0, file_offset
    ) + struct.pack("<Q", dts)


def file_tail(packets: int) -> bytes:
    return b"CLVEND\x00\x00" + struct.pack("<Q", packets)


# ---------------------------------------------------------------- 组装


def assemble(
    streams: list[dict], exts: dict[int, list[tuple[int, bytes]]], frames: list[dict]
) -> bytes:
    """按布局组装：头 | 描述符表 | 扩展块区 | 包区 | 索引区 | 尾。

    streams: [{id,type,codec,max_packet,tb_num,tb_den,bit_depth,sample_rate,channels}]
    exts:    {stream_id: [(ext_type, data), ...]}
    frames:  [{stream,dts,pts,key,payload,chunk}]，dts 是**源时间轴**值，写方在此完成平移到 0
    """
    origin: dict[int, int] = {}
    last: dict[int, int] = {}
    for frame in frames:  # 解码序首见即该流原点
        origin.setdefault(frame["stream"], frame["dts"])
        last.setdefault(frame["stream"], frame["dts"])

    ext_area_start = 64 + 64 * len(streams)
    ext_blobs: list[bytes] = []
    ext_offsets: dict[int, int] = {}
    cursor = ext_area_start
    for stream in streams:
        items = exts.get(stream["id"], [])
        if not items:
            continue
        ext_offsets[stream["id"]] = cursor
        for index, (ext_type, data) in enumerate(items):
            blob = ext_block(ext_type, 1, data)
            blob = (
                blob[:-8]
                + struct.pack("<Q", cursor + len(blob) if index + 1 < len(items) else 0)
                + blob[-4:]
            )
            # next 改写后 crc 覆盖段含 next，需重算
            covered = 16 + len(data)
            blob = (
                blob[:covered]
                + struct.pack("<I", crc32_mpeg2(blob[:covered]))
                + blob[covered + 4 :]
            )
            ext_blobs.append(blob)
            cursor += len(blob)

    out = bytearray()
    out += file_head(len(streams), HEAD_INDEX_PRESENT, 0, 0, 0)
    for stream in streams:
        out += stream_desc(
            stream["id"],
            stream["type"],
            stream["codec"],
            stream["max_packet"],
            stream["tb_num"],
            stream["tb_den"],
            stream["bit_depth"],
            stream.get("sample_rate", 0),
            stream.get("channels", 0),
            ext_offsets.get(stream["id"], 0),
        )
    out += b"".join(ext_blobs)

    packets = 0
    duration = 0
    entries: list[tuple[int, bool, int, int]] = []
    for frame in frames:
        stream = next(s for s in streams if s["id"] == frame["stream"])
        shifted = frame["dts"] - origin[frame["stream"]]
        dts_delta = shifted - (last[frame["stream"]] - origin[frame["stream"]])
        last[frame["stream"]] = frame["dts"]
        pts_delta = frame["pts"] - frame["dts"]  # 平移量在相减时抵消

        payload = frame["payload"]
        chunk = frame.get("chunk", 0) or len(payload) or 1
        total_parts = max(1, (len(payload) + chunk - 1) // chunk)
        first_offset = len(out)
        for part in range(total_parts):
            piece = payload[part * chunk : (part + 1) * chunk]
            out += packet(
                frame["stream"],
                dts_delta,
                pts_delta,
                piece,
                keyframe=frame["key"],
                fragment_index=part if total_parts > 1 else None,
                last_fragment=total_parts > 1 and part + 1 == total_parts,
            )
            packets += 1
        entries.append((frame["stream"], frame["key"], first_offset, shifted))

        shifted_pts = frame["pts"] - origin[frame["stream"]]
        if shifted_pts > 0:
            ticks = shifted_pts * stream["tb_num"] * BASE_TICKS // stream["tb_den"]
            duration = max(duration, ticks)

    # 索引按统一时间轴升序；相等按 stream_id，再按文件内出现顺序
    def sort_key(entry):
        stream = next(s for s in streams if s["id"] == entry[0])
        ticks = entry[3] * stream["tb_num"] * BASE_TICKS // stream["tb_den"]
        return (ticks, entry[0], entry[2])

    entries.sort(key=sort_key)
    index_offset = len(out)
    out += index_head(len(entries))
    for stream_id, keyframe, offset, dts in entries:
        out += index_entry(stream_id, keyframe, offset, dts)
    out += file_tail(packets)

    head = bytearray(
        file_head(len(streams), HEAD_INDEX_PRESENT, duration, packets, index_offset)
    )
    out[:64] = head
    return bytes(out)


# ---------------------------------------------------------------- 三份素材


def video_stream(stream_id: int = 0, max_packet: int = 4096) -> dict:
    return {
        "id": stream_id,
        "type": STREAM_VIDEO,
        "codec": 0,
        "max_packet": max_packet,
        "tb_num": 1,
        "tb_den": BASE_TICKS,
        "bit_depth": 8,
    }


def subtitle_stream(stream_id: int = 1, max_packet: int = 1024) -> dict:
    return {
        "id": stream_id,
        "type": STREAM_SUBTITLE,
        "codec": 0,
        "max_packet": max_packet,
        "tb_num": 1,
        "tb_den": BASE_TICKS,
        "bit_depth": 0,
    }


def pattern(size: int, seed: int) -> bytes:
    return bytes(((i * 37 + seed) & 0xFF for i in range(size)))


STYLE = b"[Script Info]\nTitle: clv fixture\nScriptType: v4.00+\n"


def fixture_minimal() -> bytes:
    """最小合法文件：一条视频流、两个单包帧、带索引。"""
    return assemble(
        [video_stream()],
        {},
        [
            {"stream": 0, "dts": 0, "pts": 0, "key": True, "payload": pattern(64, 1)},
            {
                "stream": 0,
                "dts": 43243200,
                "pts": 43243200,
                "key": False,
                "payload": pattern(48, 2),
            },
        ],
    )


def fixture_neg_dts() -> bytes:
    """负初始 DTS + 分片：源首包 dts = -40，写方平移后首包 dts = 0；第二帧 700 B 切三片。"""
    return assemble(
        [video_stream()],
        {},
        [
            {
                "stream": 0,
                "dts": -40,
                "pts": 40,
                "key": True,
                "payload": pattern(120, 3),
            },
            {
                "stream": 0,
                "dts": 43243200,
                "pts": 43243240,
                "key": False,
                "payload": pattern(700, 4),
                "chunk": 300,
            },
        ],
    )


def fixture_subtitle_ext4() -> bytes:
    """双流 + ext_type=4 样式块 + 跨流索引次序（含 dts 相等时的 stream_id tie-break）。"""
    return assemble(
        [video_stream(), subtitle_stream()],
        {1: [(4, STYLE)]},
        [
            {
                "stream": 0,
                "dts": -40,
                "pts": 40,
                "key": True,
                "payload": pattern(1000, 5),
                "chunk": 300,
            },
            {"stream": 1, "dts": 0, "pts": 0, "key": False, "payload": STYLE[:30]},
            {
                "stream": 1,
                "dts": 21621600,
                "pts": 21621600,
                "key": False,
                "payload": pattern(20, 6),
            },
            {
                "stream": 0,
                "dts": 43243200,
                "pts": 43243200,
                "key": False,
                "payload": pattern(200, 7),
            },
        ],
    )


FIXTURES = {
    "minimal.clv": fixture_minimal,
    "neg_dts.clv": fixture_neg_dts,
    "subtitle_ext4.clv": fixture_subtitle_ext4,
}


def main() -> int:
    default_out = str(ROOT / "assets" / "fixtures")

    parser = argparse.ArgumentParser(description="生成确定性 .clv 测试素材")
    parser.add_argument(
        "--out", default=default_out, help="输出目录，默认 assets/fixtures"
    )
    args = parser.parse_args()

    out_dir = pathlib.Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, builder in sorted(FIXTURES.items()):
        data = builder()
        path = out_dir / name
        with open(path, "wb") as handle:
            handle.write(data)
        print(
            "%-20s %7d B  crc32=%08X  sha256=%s"
            % (
                name,
                len(data),
                crc32_mpeg2(data),
                hashlib.sha256(data).hexdigest()[:16],
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
