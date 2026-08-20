#!/usr/bin/env python3
"""Simulates the iPad-side OpenDisplay receiver for local testing without hardware.

Listens on :9000 (like the real iPad), accepts one connection, sends `hello`,
then logs every framed message it receives and structurally validates any
video-looking payload against the Annex-B rules from the wire spec:

  - only 4-byte start codes (00 00 00 01)
  - every IDR (first slice NAL after a run of only SPS/PPS at the front) is
    preceded by SPS (type 7) and PPS (type 8)
  - first byte is 0x00, never '{'

It also plays back Apple Pencil input on demand (--pencil-stroke), which is the
only way to exercise the sender's pen path without an actual iPad and Pencil:
the receiver announces protocol 3 in `hello`, waits for the sender's `welcome`,
and then emits a proximity/hover/down/move/up sequence with a rising pressure
ramp and changing tilt.

Usage:
  python3 tools/mock_receiver.py [--host 0.0.0.0] [--port 9000]
                                  [--width 2388] [--height 1668]
                                  [--send-kf-after N] [--dump-h264 out.h264]
                                  [--pencil-stroke] [--pencil-repeat SECONDS]
                                  [--stall-after N] [--stall-secs SECONDS]

--stall-after exercises the send-backpressure path: after N video frames the
mock stops reading (while still requesting keyframes, the only payload big
enough to back up the socket on a static desktop), so the sender's WaitWritable
budget expires and it drops a frame. The mock can only confirm the connection
survives and video resumes; that the drop actually happened is visible on the
sender side, which logs "send backpressure, dropped a frame" once per episode.
"""
import argparse
import json
import socket
import struct
import sys
import threading
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


# The pencil playback runs on its own thread while the main loop may send a `kf`
# request, and sendall is not atomic — two writers would interleave and break
# the receiver's length-prefixed framing.
_send_lock = threading.Lock()


def send_frame(sock: socket.socket, payload: bytes) -> None:
    with _send_lock:
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


def is_keyframe(payload: bytes) -> bool:
    return any(payload[o + c] & 0x1F == 5
               for o, c in find_start_codes(payload) if o + c < len(payload))


def send_control(sock: socket.socket, **fields) -> None:
    send_frame(sock, json.dumps(fields).encode("utf-8"))


def play_pencil_tap(sock: socket.socket, x: float, y: float, pressure: float) -> None:
    """A tap: down, a few samples in place, up.

    Worth its own playback because a zero-force tap is how the pen clicks UI
    elements, and because a down/up pair with nothing in between behaves
    differently from a real tap (a Pencil samples at ~240 Hz, so even the
    shortest tap carries move samples).
    """
    altitude = 1.4
    send_control(sock, type="proximity", entering=True, x=x, y=y)
    send_control(sock, type="pencil", phase="hover", x=x, y=y, pressure=0.0,
                 azimuth=0.0, altitude=altitude, rotation=0)
    time.sleep(0.05)
    send_control(sock, type="pencil", phase="down", x=x, y=y, pressure=pressure,
                 azimuth=0.0, altitude=altitude, rotation=0)
    for _ in range(4):
        time.sleep(0.02)
        send_control(sock, type="pencil", phase="move", x=x, y=y, pressure=pressure,
                     azimuth=0.0, altitude=altitude, rotation=0)
    send_control(sock, type="pencil", phase="up", x=x, y=y, pressure=0.0,
                 azimuth=0.0, altitude=altitude, rotation=0)
    send_control(sock, type="proximity", entering=False, x=x, y=y)


def play_pencil_stroke(sock: socket.socket, welcome: threading.Event, repeat: float) -> None:
    """A diagonal stroke with rising pressure and rotating tilt.

    Mirrors what the real receiver sends (PhoneReceiver.sendPencil): normalized
    coordinates, pressure 0..1, azimuth/altitude in radians. Waits for the
    sender's `welcome` first, because that is exactly the gate the iPad applies
    before it switches from `touch` to `pencil`.
    """
    if not welcome.wait(timeout=15.0):
        print("[mock] no welcome within 15s - sender does not speak protocol 3, skipping pencil")
        return

    steps = 40
    altitude = 0.9  # radians, clearly tilted so tiltX/tiltY are non-zero
    try:
        while True:
            time.sleep(1.0)
            x0, y0, x1, y1 = 0.25, 0.25, 0.75, 0.75
            print("[mock] pencil: proximity + hover")
            send_control(sock, type="proximity", entering=True, x=x0, y=y0)
            for i in range(5):
                t = i / 4
                send_control(sock, type="pencil", phase="hover", x=x0 - 0.05 + t * 0.05,
                             y=y0 - 0.05 + t * 0.05, pressure=0.0, azimuth=0.0,
                             altitude=altitude, rotation=0)
                time.sleep(0.03)

            print("[mock] pencil: stroke with pressure ramp 0.05 -> 1.0")
            send_control(sock, type="pencil", phase="down", x=x0, y=y0, pressure=0.05,
                         azimuth=0.0, altitude=altitude, rotation=0)
            for i in range(1, steps + 1):
                t = i / steps
                send_control(sock, type="pencil", phase="move", x=x0 + (x1 - x0) * t,
                             y=y0 + (y1 - y0) * t, pressure=0.05 + 0.95 * t,
                             azimuth=6.283 * t, altitude=altitude, rotation=0)
                time.sleep(0.012)
            send_control(sock, type="pencil", phase="up", x=x1, y=y1, pressure=0.0,
                         azimuth=0.0, altitude=altitude, rotation=0)
            send_control(sock, type="proximity", entering=False, x=x1, y=y1)
            print("[mock] pencil: stroke done")

            # Zero-force tap right after: this is what has to click UI elements.
            time.sleep(0.4)
            print("[mock] pencil: zero-pressure tap")
            play_pencil_tap(sock, 0.5, 0.5, 0.0)

            if repeat <= 0:
                return
            time.sleep(repeat)
    except OSError as exc:
        print(f"[mock] pencil playback stopped: {exc}")


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
    ap.add_argument("--pencil-stroke", action="store_true",
                    help="play a scripted Apple Pencil stroke once the sender sent its welcome")
    ap.add_argument("--pencil-repeat", type=float, default=0.0,
                    help="replay that stroke every N seconds (0 = play it once)")
    ap.add_argument("--rotate-after", type=int, default=0,
                    help="after N video frames, send a second hello with width/height swapped")
    ap.add_argument("--stall-after", type=int, default=0,
                    help="after N video frames, stop reading for --stall-secs so the sender "
                         "runs into send backpressure and drops a frame (0 = never)")
    ap.add_argument("--stall-secs", type=float, default=3.0,
                    help="how long --stall-after stops reading")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if args.stall_after:
        # Inherited by the accepted socket. A small receive window is what makes
        # the sender's socket fill within the stall instead of swallowing
        # several seconds of frames in kernel buffers.
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
    srv.bind((args.host, args.port))
    srv.listen(1)
    print(f"[mock] listening on {args.host}:{args.port}")

    conn, addr = srv.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"[mock] accepted connection from {addr}")

    # pv mirrors the real receiver: 3 is what unlocks pencil on the wire.
    def hello_json(w: int, h: int) -> str:
        return ('{"type":"hello","pixelsWide":%d,"pixelsHigh":%d,"scale":%d,'
                '"device":"iPad","id":"mock-uuid-0000","pv":3}' % (w, h, args.scale))

    width, height = args.width, args.height
    hello = hello_json(width, height)
    send_frame(conn, hello.encode("utf-8"))
    print(f"[mock] sent hello: {hello}")

    welcome_seen = threading.Event()
    if args.pencil_stroke:
        threading.Thread(target=play_pencil_stroke,
                         args=(conn, welcome_seen, args.pencil_repeat),
                         daemon=True).start()

    dump = open(args.dump_h264, "wb") if args.dump_h264 else None
    video_frames = 0
    control_frames = 0
    stalled_at = None
    stall_frame = 0

    try:
        while True:
            payload = read_frame(conn)

            if is_control(payload):
                control_frames += 1
                text = payload.decode("utf-8", "replace")
                print(f"[mock] control #{control_frames}: {text}")
                if '"welcome"' in text:
                    welcome_seen.set()
                continue

            video_frames += 1
            problems = validate_annexb_frame(payload)
            status = "OK" if not problems else "PROBLEMS: " + "; ".join(problems)
            print(f"[mock] video frame #{video_frames}: {len(payload)} bytes - {status}")

            if stalled_at is not None and is_keyframe(payload):
                # Deliberately not a latency measurement: reads resume into a
                # backlog, so this keyframe may well be one the sender queued
                # *during* the stall rather than its post-drop resync. All this
                # asserts is that the connection came through the stall intact
                # and video is flowing again. Whether the sender actually hit
                # the drop path is only visible on its side - it logs
                # "send backpressure, dropped a frame" once per episode.
                gap = time.monotonic() - stalled_at
                print(f"[mock] recovered after the stall: keyframe {gap * 1000:.0f}ms in, "
                      f"{video_frames - stall_frame} frame(s) read - OK")
                stalled_at = None

            if dump:
                dump.write(payload)

            if args.send_kf_after and video_frames == args.send_kf_after:
                send_frame(conn, b'{"type":"kf"}')
                print("[mock] sent kf request")

            if args.stall_after and video_frames == args.stall_after:
                print(f"[mock] stalling {args.stall_secs}s - not reading, sender should "
                      f"hit WaitWritable timeout and drop a frame")
                deadline = time.monotonic() + args.stall_secs
                while time.monotonic() < deadline:
                    # Keep *writing* while refusing to read. On a static desktop
                    # the sender only emits tiny keepalive P-frames, which never
                    # fill a socket; requested keyframes are the only payload
                    # big enough to back the connection up within seconds.
                    send_frame(conn, b'{"type":"kf"}')
                    time.sleep(0.1)
                stalled_at = time.monotonic()
                stall_frame = video_frames
                print("[mock] resuming reads")

            if args.rotate_after and video_frames == args.rotate_after:
                # What the iPad does when it is turned: a second hello with the
                # panel dimensions swapped. The sender has to remove and re-add
                # its virtual display for that - the path where it must claim
                # its *own* new monitor again and leave other senders' alone.
                width, height = height, width
                rotated = hello_json(width, height)
                send_frame(conn, rotated.encode("utf-8"))
                print(f"[mock] sent rotation hello: {rotated}")

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
