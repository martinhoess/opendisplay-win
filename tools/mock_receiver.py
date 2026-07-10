#!/usr/bin/env python3
"""Simulates the iPad-side OpenDisplay receiver for local testing without hardware.

Listens on :9000 (like the real iPad), accepts one connection, sends `hello`,
then logs every framed message it receives and structurally validates any
video-looking payload against the Annex-B rules from the wire spec:

  - only 4-byte start codes (00 00 00 01)
  - every IDR (first slice NAL after a run of only SPS/PPS at the front) is
    preceded by SPS (type 7) and PPS (type 8)
  - first byte is 0x00, never '{'

Usage:
  python3 tools/mock_receiver.py [--host 0.0.0.0] [--port 9000]
                                  [--width 2388] [--height 1668]
                                  [--send-kf-after N] [--dump-h264 out.h264]
"""
import argparse
import socket
import struct
import sys
import time


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed")
        buf.extend(chunk)
    return bytes(buf)


def read_frame(sock: socket.socket) -> bytes:
    (length,) = struct.unpack(">I", recv_exact(sock, 4))
    return recv_exact(sock, length)


def send_frame(sock: socket.socket, payload: bytes) -> None:
    sock.sendall(struct.pack(">I", len(payload)) + payload)


def is_control(payload: bytes) -> bool:
    return len(payload) < 32768 and payload[:1] == b"{" and b"\x00" not in payload


def find_start_codes(payload: bytes):
    """Yields (offset, code_len) for every occurrence of 00 00 01 or 00 00 00 01."""
    i = 0
    n = len(payload)
    while i < n - 2:
        if payload[i] == 0 and payload[i + 1] == 0:
            if i < n - 3 and payload[i + 2] == 0 and payload[i + 3] == 1:
                yield i, 4
                i += 4
                continue
            if payload[i + 2] == 1:
                yield i, 3
                i += 3
                continue
        i += 1


def validate_annexb_frame(payload: bytes) -> list:
    """Returns a list of problem strings (empty = looks correct)."""
    problems = []
    codes = list(find_start_codes(payload))
    if not codes:
        problems.append("no start codes found")
        return problems

    if any(code_len == 3 for _, code_len in codes):
        problems.append("found 3-byte start code(s) - must be normalized to 4-byte")

    nal_types = []
    for idx, (offset, code_len) in enumerate(codes):
        nal_start = offset + code_len
        if nal_start < len(payload):
            nal_types.append(payload[nal_start] & 0x1F)
        else:
            nal_types.append(None)

    # A frame is a keyframe if it contains a slice type indicating IDR (type 5).
    if 5 in nal_types:
        has_sps = 7 in nal_types
        has_pps = 8 in nal_types
        if not has_sps or not has_pps:
            problems.append(
                f"IDR present but SPS/PPS missing (types seen: {nal_types})"
            )
        else:
            idr_pos = nal_types.index(5)
            sps_pos = nal_types.index(7)
            pps_pos = nal_types.index(8)
            if not (sps_pos < pps_pos < idr_pos):
                problems.append(
                    f"SPS/PPS not immediately before IDR (order: {nal_types})"
                )

    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--width", type=int, default=2388)
    ap.add_argument("--height", type=int, default=1668)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument(
        "--send-kf-after",
        type=int,
        default=0,
        help="send a {\"type\":\"kf\"} request after N video frames (0 = never)",
    )
    ap.add_argument("--dump-h264", default=None, help="write received video payloads (Annex-B) to this file")
    ap.add_argument("--max-frames", type=int, default=0, help="exit after N video frames (0 = run forever)")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    print(f"[mock] listening on {args.host}:{args.port}")

    conn, addr = srv.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"[mock] accepted connection from {addr}")

    hello = (
        '{"type":"hello","pixelsWide":%d,"pixelsHigh":%d,"scale":%d,'
        '"device":"iPad","id":"mock-uuid-0000"}' % (args.width, args.height, args.scale)
    )
    send_frame(conn, hello.encode("utf-8"))
    print(f"[mock] sent hello: {hello}")

    dump = open(args.dump_h264, "wb") if args.dump_h264 else None
    video_frames = 0
    control_frames = 0

    try:
        while True:
            payload = read_frame(conn)

            if is_control(payload):
                control_frames += 1
                print(f"[mock] control #{control_frames}: {payload.decode('utf-8', 'replace')}")
                continue

            video_frames += 1
            problems = validate_annexb_frame(payload)
            status = "OK" if not problems else "PROBLEMS: " + "; ".join(problems)
            print(f"[mock] video frame #{video_frames}: {len(payload)} bytes - {status}")

            if dump:
                dump.write(payload)

            if args.send_kf_after and video_frames == args.send_kf_after:
                send_frame(conn, b'{"type":"kf"}')
                print("[mock] sent kf request")

            if args.max_frames and video_frames >= args.max_frames:
                print("[mock] reached --max-frames, closing")
                break
    except ConnectionError:
        print("[mock] sender disconnected")
    finally:
        if dump:
            dump.close()
        conn.close()
        srv.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
