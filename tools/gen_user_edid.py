#!/usr/bin/env python3
"""Generate / decode OpenDisplay MTT user_edid.bin (EDID 1.3, 128 bytes).

The app (src/display/MttVddSettings.cpp: MttModesForHello + BuildMttEdid)
generates this file at connect time from the iPad's hello size; this script is
the reference implementation and check tool. Both must stay byte-identical:
same mode rule, same timing search, same descriptor packing.

Fixed manufacturer/product/serial so Windows keeps the display arrangement
across reconnects and iPads. CustomEdid=true loads the file from
C:\\VirtualDisplayDriver\\.

Usage:
  python tools/gen_user_edid.py                    # 2360x1640 -> assets/mtt/user_edid.bin (shipped fallback)
  python tools/gen_user_edid.py --size 2752x2064   # modes for that hello size
  python tools/gen_user_edid.py --size WxH -o x.bin
  python tools/gen_user_edid.py --size WxH --modes # print the mode list only
  python tools/gen_user_edid.py --decode path      # print modes/identity

Identity (fixed, never randomize):
  Manufacturer: MTT
  Product:      0x1337
  Serial:       0x4F445731  ("ODW1")

Mode rule (landscape, 16-px floor aligned, all @ 60 Hz, max 4 DTDs):
  1. native hello size (long side first)          - preferred
  2. closest-aspect sibling iPad panel: aspect within 1% and width within 8%
  3. 3/4 of native
  4. 1/2 of native
  Unused DTD slots carry a monitor-name ("OpenDisplay") / dummy descriptor.
  2360x1640 -> 2352x1632, 2384x1664, 1760x1216, 1168x816 (the shipped file).
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# --- Fixed identity (do not randomize) ---
MANUFACTURER = "MTT"
PRODUCT_CODE = 0x1337
SERIAL = 0x4F445731  # "ODW1" as ASCII in LE serial field
WEEK = 1
YEAR = 2026  # stored as year - 1990

DEFAULT_SIZE = (2360, 1640)  # shipped fallback: 11" class
REFRESH = 60
MAX_DIM = 4095  # EDID DTD 12-bit active fields

# Native iPad panels (landscape). Sibling candidates for rule 2 after 16-floor.
IPAD_PANELS = [
    (2360, 1640),  # 10.9" / 11" Air, iPad 10th
    (2388, 1668),  # 11" Pro (pre-M4), 11" Air M2
    (2420, 1668),  # 11" Pro M4
    (2732, 2048),  # 12.9" Pro, 13" Air
    (2752, 2064),  # 13" Pro M4
    (2266, 1488),  # mini 6/7
    (2160, 1620),  # 10.2"
    (2224, 1668),  # 10.5" Pro
    (2048, 1536),  # 9.7" / older
]

# Physical size scale: 2352 px -> 225 mm (~265 ppi, iPad-class). Keeps the
# shipped 11" file's 225x156 mm.
MM_NUM = 225
MM_DEN = 2352


def align16_floor(v: int) -> int:
    return v & ~15


def modes_for_size(w: int, h: int) -> list[tuple[int, int, int]]:
    """Mirror of MttModesForHello (C++). Landscape, 16-floor, max 4."""
    if h > w:
        w, h = h, w
    w, h = align16_floor(w), align16_floor(h)
    if w < 16 or h < 16 or w > MAX_DIM or h > MAX_DIM:
        return modes_for_size(*DEFAULT_SIZE)
    out: list[tuple[int, int, int]] = []

    def push(mw: int, mh: int, native: bool = False) -> None:
        if mh > mw:
            mw, mh = mh, mw
        if not native and (mw < 640 or mh < 480):
            return
        if any(m[0] == mw and m[1] == mh for m in out):
            return
        out.append((mw, mh, REFRESH))

    push(w, h, native=True)
    # Closest-aspect sibling panel (integer cross-multiplied comparisons).
    best = None  # (diff, ph, pw)
    for pw0, ph0 in IPAD_PANELS:
        pw, ph = align16_floor(pw0), align16_floor(ph0)
        if pw == w and ph == h:
            continue
        diff = abs(pw * h - w * ph)  # relative aspect error = diff / (w * ph)
        if 100 * diff > w * ph:
            continue  # aspect off by > 1%
        if 100 * abs(pw - w) > 8 * w:
            continue  # different size class
        if best is None or diff * best[1] < best[0] * ph:
            best = (diff, ph, pw)
    if best is not None:
        push(best[2], best[1])
    push(align16_floor(w * 3 // 4), align16_floor(h * 3 // 4))
    push(align16_floor(w // 2), align16_floor(h // 2))
    return out[:4]


def physical_mm(w: int, h: int) -> tuple[int, int]:
    return ((w * MM_NUM + MM_DEN // 2) // MM_DEN, (h * MM_NUM + MM_DEN // 2) // MM_DEN)


def manufacturer_id(letters: str) -> bytes:
    letters = letters.upper()
    assert len(letters) == 3 and letters.isalpha()
    a, b, c = (ord(ch) - ord("A") + 1 for ch in letters)
    val = (a << 10) | (b << 5) | c
    return struct.pack(">H", val)


def cvt_rb_params(h_active: int, v_active: int, refresh: int = 60):
    """CVT reduced-blanking-ish params; pick v_back so clock ≈ refresh."""
    h_front, h_sync, h_blank = 48, 32, 160
    v_front, v_sync = 3, 10
    h_total = h_active + h_blank
    best = None
    for v_back in range(6, 120):
        v_blank = v_front + v_sync + v_back
        v_total = v_active + v_blank
        # Python round() = half-to-even; C++ port reproduces it with integers.
        clock_10khz = round(h_total * v_total * refresh / 10000.0)
        if not (1 <= clock_10khz <= 65535):
            continue
        actual = (clock_10khz * 10000.0) / (h_total * v_total)
        err = abs(actual - refresh)
        if best is None or err < best[0]:
            best = (err, clock_10khz, h_front, h_sync, h_blank, v_front, v_sync, v_back, v_blank)
            if err < 0.01:
                break
    if best is None:
        raise RuntimeError(f"no timing for {h_active}x{v_active}@{refresh}")
    _, clock_10khz, h_front, h_sync, h_blank, v_front, v_sync, v_back, v_blank = best
    return {
        "clock_10khz": clock_10khz,
        "h_active": h_active,
        "h_blank": h_blank,
        "h_front": h_front,
        "h_sync": h_sync,
        "v_active": v_active,
        "v_blank": v_blank,
        "v_front": v_front,
        "v_sync": v_sync,
        "v_back": v_back,
    }


def detailed_timing(h: int, v: int, hz: int, h_mm: int, v_mm: int) -> bytes:
    p = cvt_rb_params(h, v, hz)
    b = bytearray(18)
    b[0] = p["clock_10khz"] & 0xFF
    b[1] = (p["clock_10khz"] >> 8) & 0xFF
    b[2] = p["h_active"] & 0xFF
    b[3] = p["h_blank"] & 0xFF
    b[4] = ((p["h_active"] >> 8) << 4) | ((p["h_blank"] >> 8) & 0x0F)
    b[5] = p["v_active"] & 0xFF
    b[6] = p["v_blank"] & 0xFF
    b[7] = ((p["v_active"] >> 8) << 4) | ((p["v_blank"] >> 8) & 0x0F)
    b[8] = p["h_front"] & 0xFF
    b[9] = p["h_sync"] & 0xFF
    b[10] = ((p["v_front"] & 0x0F) << 4) | (p["v_sync"] & 0x0F)
    b[11] = (
        ((p["h_front"] >> 8) << 6)
        | ((p["h_sync"] >> 8) << 4)
        | ((p["v_front"] >> 4) << 2)
        | ((p["v_sync"] >> 4) & 0x03)
    )
    b[12] = h_mm & 0xFF
    b[13] = v_mm & 0xFF
    b[14] = ((h_mm >> 8) << 4) | ((v_mm >> 8) & 0x0F)
    b[15] = 0  # h border
    b[16] = 0  # v border
    # digital separate sync, h/v positive
    b[17] = 0x1E
    return bytes(b)


def checksum(block: bytes | bytearray) -> int:
    assert len(block) >= 127
    return (256 - (sum(block[:127]) % 256)) % 256


def name_descriptor() -> bytes:
    text = b"OpenDisplay\n"
    return bytes([0, 0, 0, 0xFC, 0]) + text + b" " * (13 - len(text))


def dummy_descriptor() -> bytes:
    return bytes([0, 0, 0, 0x10, 0]) + bytes(13)


def build_edid(modes: list[tuple[int, int, int]]) -> bytes:
    assert 1 <= len(modes) <= 4, "base EDID only has 4 DTD slots"
    h_mm, v_mm = physical_mm(modes[0][0], modes[0][1])
    e = bytearray(128)
    e[0:8] = bytes([0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00])
    e[8:10] = manufacturer_id(MANUFACTURER)
    e[10:12] = struct.pack("<H", PRODUCT_CODE)
    e[12:16] = struct.pack("<I", SERIAL)
    e[16] = WEEK
    e[17] = YEAR - 1990
    e[18] = 0x01  # EDID 1.3
    e[19] = 0x03
    # Digital input, DisplayPort-ish bit depth unspecified, DFPs compliant
    e[20] = 0x80
    e[21] = max(1, h_mm // 10)  # cm
    e[22] = max(1, v_mm // 10)
    e[23] = 0x78  # gamma 2.2
    # features: preferred timing mode, continuous freq not claimed, RGB
    e[24] = 0x0A
    # sRGB-ish chromaticity (standard template)
    e[25:35] = bytes([0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54])
    e[35:38] = bytes([0x00, 0x00, 0x00])  # no established timings
    # Standard timings unused
    for i in range(38, 54):
        e[i] = 0x01

    for slot in range(4):
        off = 54 + slot * 18
        if slot < len(modes):
            w, h, hz = modes[slot]
            e[off : off + 18] = detailed_timing(w, h, hz, h_mm, v_mm)
        elif slot == len(modes):
            e[off : off + 18] = name_descriptor()
        else:
            e[off : off + 18] = dummy_descriptor()

    e[126] = 0  # no extension blocks
    e[127] = checksum(e)
    assert sum(e) % 256 == 0
    return bytes(e)


def parse_size(text: str) -> tuple[int, int]:
    parts = text.lower().replace("*", "x").split("x")
    if len(parts) != 2:
        raise argparse.ArgumentTypeError("expected WxH, e.g. 2360x1640")
    return int(parts[0]), int(parts[1])


def decode_dtd(d: bytes) -> dict | None:
    clock = d[0] | (d[1] << 8)
    if clock == 0:
        return None
    ha = d[2] | ((d[4] >> 4) << 8)
    hb = d[3] | ((d[4] & 0x0F) << 8)
    va = d[5] | ((d[7] >> 4) << 8)
    vb = d[6] | ((d[7] & 0x0F) << 8)
    ht = ha + hb
    vt = va + vb
    hz = (clock * 10000) / (ht * vt) if ht and vt else 0
    return {"w": ha, "h": va, "hz": round(hz, 2), "clock_mhz": clock / 100.0}


def decode(path: Path) -> None:
    data = path.read_bytes()
    print(f"file: {path} ({len(data)} bytes)")
    assert data[0:8] == bytes([0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00])
    mid = (data[8] << 8) | data[9]
    def unpack(v):
        return chr(((v >> 10) & 0x1F) + 64) + chr(((v >> 5) & 0x1F) + 64) + chr((v & 0x1F) + 64)
    prod = data[10] | (data[11] << 8)
    serial = data[12] | (data[13] << 8) | (data[14] << 16) | (data[15] << 24)
    print(f"manufacturer: {unpack(mid)}  product: 0x{prod:04X}  serial: 0x{serial:08X}")
    print(f"edid: {data[18]}.{data[19]}  week={data[16]} year={1990+data[17]}  size={data[21]}x{data[22]} cm")
    print(f"checksum ok: {sum(data[:128]) % 256 == 0}")
    for i in range(4):
        block = data[54 + i * 18 : 72 + i * 18]
        if block[0:3] == b"\x00\x00\x00" and block[3] == 0xFC:
            print(f"  slot{i}: name {block[5:].split(b'\\n')[0].decode('ascii', 'replace')!r}")
            continue
        d = decode_dtd(block)
        if d:
            pref = " (preferred)" if i == 0 else ""
            print(f"  DTD{i}: {d['w']}x{d['h']} @{d['hz']}Hz  clock={d['clock_mhz']:.2f}MHz{pref}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", type=Path, default=None)
    ap.add_argument("--size", type=parse_size, default=None, help="hello size WxH (default 2360x1640)")
    ap.add_argument("--modes", action="store_true", help="print the mode list and exit")
    ap.add_argument("--decode", type=Path, default=None)
    args = ap.parse_args()
    if args.decode:
        decode(args.decode)
        return 0
    size = args.size or DEFAULT_SIZE
    modes = modes_for_size(*size)
    if args.modes:
        print(f"{size[0]}x{size[1]}: " + ", ".join(f"{w}x{h}@{hz}" for w, h, hz in modes))
        return 0
    out = args.output
    if out is None:
        root = Path(__file__).resolve().parents[1]
        out = root / "assets" / "mtt" / "user_edid.bin"
    out.parent.mkdir(parents=True, exist_ok=True)
    data = build_edid(modes)
    out.write_bytes(data)
    print(f"wrote {out} ({len(data)} bytes)")
    decode(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
