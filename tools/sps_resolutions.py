#!/usr/bin/env python3
"""Liest die Auflösungen aus allen SPS eines Annex-B-H.264-Dumps.

    python tools/sps_resolutions.py dump.h264

Belegt, mit welcher Geometrie der Encoder tatsächlich gesendet hat — genau das,
was bei der Rotations-Regression stehenblieb, während die Capture schon gedreht war.
Ohne Argument: Selbsttest (SPS bauen, zurücklesen).
"""
import sys
from collections import Counter


class Bits:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def u(self, n):
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3]
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def ue(self):
        zeros = 0
        while self.u(1) == 0:
            zeros += 1
            if zeros > 32:
                raise ValueError("kaputter Exp-Golomb-Code")
        return (1 << zeros) - 1 + (self.u(zeros) if zeros else 0)

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k % 2 else -(k // 2)


def unescape(nal):
    """Emulation-Prevention-Bytes (00 00 03) entfernen."""
    out, i = bytearray(), 0
    while i < len(nal):
        if i + 2 < len(nal) and nal[i] == 0 and nal[i + 1] == 0 and nal[i + 2] == 3:
            out += b"\x00\x00"
            i += 3
        else:
            out.append(nal[i])
            i += 1
    return bytes(out)


def sps_size(nal):
    b = Bits(unescape(nal[1:]))  # NAL-Header-Byte weg
    profile = b.u(8)
    b.u(8)  # constraint flags + reserved
    b.u(8)  # level_idc
    b.ue()  # seq_parameter_set_id
    chroma = 1
    if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        chroma = b.ue()
        if chroma == 3:
            b.u(1)  # separate_colour_plane_flag
        b.ue()  # bit_depth_luma_minus8
        b.ue()  # bit_depth_chroma_minus8
        b.u(1)  # qpprime_y_zero_transform_bypass_flag
        if b.u(1):  # seq_scaling_matrix_present_flag
            for i in range(8 if chroma != 3 else 12):
                if b.u(1):
                    last, next_ = 8, 8
                    for _ in range(16 if i < 6 else 64):
                        if next_:
                            next_ = (last + b.se() + 256) % 256
                        last = next_ or last
    b.ue()  # log2_max_frame_num_minus4
    poc_type = b.ue()
    if poc_type == 0:
        b.ue()
    elif poc_type == 1:
        b.u(1)
        b.se()
        b.se()
        for _ in range(b.ue()):
            b.se()
    b.ue()  # max_num_ref_frames
    b.u(1)  # gaps_in_frame_num_value_allowed_flag
    width_mbs = b.ue() + 1
    height_map = b.ue() + 1
    frame_mbs_only = b.u(1)
    if not frame_mbs_only:
        b.u(1)  # mb_adaptive_frame_field_flag
    b.u(1)  # direct_8x8_inference_flag
    crop = [0, 0, 0, 0]
    if b.u(1):  # frame_cropping_flag
        crop = [b.ue() for _ in range(4)]
    width = width_mbs * 16
    height = height_map * 16 * (2 - frame_mbs_only)
    sub_w, sub_h = (2, 2) if chroma == 1 else (2, 1) if chroma == 2 else (1, 1)
    if chroma == 0:
        sub_w = sub_h = 1
    width -= (crop[0] + crop[1]) * sub_w
    height -= (crop[2] + crop[3]) * sub_h * (2 - frame_mbs_only)
    return width, height


def nals(data):
    i, start = 0, None
    while i < len(data) - 3:
        if data[i] == 0 and data[i + 1] == 0 and (data[i + 2] == 1 or (data[i + 2] == 0 and data[i + 3] == 1)):
            skip = 3 if data[i + 2] == 1 else 4
            if start is not None:
                yield data[start:i]
            start = i + skip
            i += skip
        else:
            i += 1
    if start is not None:
        yield data[start:]


def main(pfad):
    data = open(pfad, "rb").read()
    folge, zaehler = [], Counter()
    for nal in nals(data):
        if nal and (nal[0] & 0x1F) == 7:  # SPS
            groesse = sps_size(nal)
            zaehler[groesse] += 1
            if not folge or folge[-1] != groesse:
                folge.append(groesse)
    print(f"{pfad}: {len(data)} Bytes, {sum(zaehler.values())} SPS")
    for groesse, n in zaehler.items():
        print(f"  {groesse[0]}x{groesse[1]}: {n}x")
    print("Reihenfolge:", " -> ".join(f"{w}x{h}" for w, h in folge))
    return folge


class BitWriter:
    def __init__(self):
        self.bits = []

    def u(self, value, n):
        self.bits += [(value >> (n - 1 - i)) & 1 for i in range(n)]

    def ue(self, value):
        n = (value + 1).bit_length()
        self.u(0, n - 1)
        self.u(value + 1, n)

    def bytes(self):
        while len(self.bits) % 8:
            self.bits.append(0)
        return bytes(
            int("".join(str(b) for b in self.bits[i : i + 8]), 2) for i in range(0, len(self.bits), 8)
        )


def baue_sps(width, height):
    """Minimales Baseline-SPS (profile_idc 66) für den Selbsttest. Nicht durch 16
    teilbare Maße werden wie beim echten Encoder aufgerundet und weggecroppt."""
    mbs_w, mbs_h = -(-width // 16), -(-height // 16)
    crop_r, crop_b = (mbs_w * 16 - width) // 2, (mbs_h * 16 - height) // 2  # 4:2:0: Einheit = 2 Pixel
    w = BitWriter()
    w.u(0x67, 8)  # NAL-Header: SPS
    w.u(66, 8)  # profile_idc baseline -> kein chroma/scaling-Block
    w.u(0, 8)  # constraint flags
    w.u(31, 8)  # level_idc
    w.ue(0)  # seq_parameter_set_id
    w.ue(0)  # log2_max_frame_num_minus4
    w.ue(2)  # pic_order_cnt_type 2 -> keine weiteren POC-Felder
    w.ue(1)  # max_num_ref_frames
    w.u(0, 1)  # gaps_in_frame_num_value_allowed_flag
    w.ue(mbs_w - 1)  # pic_width_in_mbs_minus1
    w.ue(mbs_h - 1)  # pic_height_in_map_units_minus1
    w.u(1, 1)  # frame_mbs_only_flag
    w.u(1, 1)  # direct_8x8_inference_flag
    if crop_r or crop_b:
        w.u(1, 1)  # frame_cropping_flag
        w.ue(0)  # left
        w.ue(crop_r)
        w.ue(0)  # top
        w.ue(crop_b)
    else:
        w.u(0, 1)
    w.u(0, 1)  # vui_parameters_present_flag
    return w.bytes()


def demo():
    """Selbsttest ohne Dump: SPS bauen, zurücklesen, muss dieselbe Größe ergeben."""
    for groesse in [(2732, 2048), (2048, 2732), (1920, 1080), (640, 480)]:
        gelesen = sps_size(baue_sps(*groesse))
        assert gelesen == groesse, f"{groesse} gelesen als {gelesen}"
    # Und der Rahmen drumherum: zwei SPS in einem Annex-B-Strom finden.
    strom = b"\x00\x00\x00\x01" + baue_sps(2732, 2048) + b"\x00\x00\x01" + baue_sps(2048, 2732)
    gefunden = [sps_size(n) for n in nals(strom) if n[0] & 0x1F == 7]
    assert gefunden == [(2732, 2048), (2048, 2732)], gefunden
    print("demo ok")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        demo()
    else:
        main(sys.argv[1])
