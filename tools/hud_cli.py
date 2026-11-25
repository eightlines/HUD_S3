#!/usr/bin/env python3
"""
HUD S3 Host CLI — Convert, Upload, and Live-Stream animations to LILYGO T-Display S3 AMOLED.

Examples:
  python hud_cli.py convert anim.gif -o anim.hud
  python hud_cli.py upload anim.hud --host 192.168.4.1
  python hud_cli.py upload anim.hud --port COM5
  python hud_cli.py stream anim.gif --host 192.168.4.1
  python hud_cli.py stream anim.gif --port COM5
  python hud_cli.py demo-stream --host 192.168.4.1
"""

import argparse
import glob
import math
import os
import socket
import struct
import sys
import time
from typing import List, Tuple

import numpy as np
from PIL import Image, ImageDraw, ImageSequence

HUD_MAGIC = b"HUD1"
HUD_FLAG_RLE16 = 0x01
UDP_PORT = 5555
UDP_CHUNK_SIZE = 1400


def image_to_rgb565(img: Image.Image, width: int, height: int, black_thresh: int = 12) -> np.ndarray:
    """Resize/letterbox image onto a pure black canvas and convert to uint16 RGB565."""
    img = img.convert("RGB")
    src_w, src_h = img.size
    scale = min(width / src_w, height / src_h)
    nw, nh = max(1, int(src_w * scale)), max(1, int(src_h * scale))
    resized = img.resize((nw, nh), Image.Resampling.LANCZOS)

    canvas = Image.new("RGB", (width, height), (0, 0, 0))
    canvas.paste(resized, ((width - nw) // 2, (height - nh) // 2))

    arr = np.asarray(canvas, dtype=np.uint16)
    if black_thresh > 0:
        mask = (arr[:, :, 0] < black_thresh) & (arr[:, :, 1] < black_thresh) & (arr[:, :, 2] < black_thresh)
        arr[mask] = 0

    r = (arr[:, :, 0] & 0xF8) << 8
    g = (arr[:, :, 1] & 0xFC) << 3
    b = arr[:, :, 2] >> 3
    return (r | g | b).astype("<u2").ravel()


def encode_rle16(pixels: np.ndarray) -> bytes:
    """Encode a 1D uint16 RGB565 array into RLE16 bytes."""
    out = bytearray()
    n = len(pixels)
    i = 0
    while i < n:
        val = int(pixels[i])
        run = 1
        while i + run < n and run < 32767 and int(pixels[i + run]) == val:
            run += 1
        if run >= 3:
            out += struct.pack("<HH", 0x8000 | run, val)
            i += run
        else:
            lit_start = i
            lit_len = 0
            while i < n and lit_len < 32767:
                if i + 2 < n and int(pixels[i]) == int(pixels[i + 1]) == int(pixels[i + 2]):
                    break
                i += 1
                lit_len += 1
            out += struct.pack("<H", lit_len)
            out += pixels[lit_start:lit_start + lit_len].tobytes()
    return bytes(out)


def load_frames(source: str, width: int, height: int, black_thresh: int) -> Tuple[List[bytes], int]:
    """Load GIF or directory of PNGs into RLE16-encoded frames."""
    frames: List[bytes] = []
    fps = 30

    if os.path.isdir(source):
        files = sorted(glob.glob(os.path.join(source, "*.png")) + glob.glob(os.path.join(source, "*.jpg")))
        if not files:
            raise FileNotFoundError(f"No PNG/JPG files found in {source}")
        for f in files:
            with Image.open(f) as im:
                px = image_to_rgb565(im, width, height, black_thresh)
                frames.append(encode_rle16(px))
    else:
        with Image.open(source) as im:
            dur = im.info.get("duration", 33)
            if dur and dur > 0:
                fps = max(1, min(60, round(1000 / dur)))
            for frame in ImageSequence.Iterator(im):
                px = image_to_rgb565(frame, width, height, black_thresh)
                frames.append(encode_rle16(px))

    return frames, fps


def cmd_convert(args: argparse.Namespace) -> None:
    w, h = (536, 240) if args.landscape else (240, 536)
    frames, detected_fps = load_frames(args.input, w, h, args.black_thresh)
    fps = args.fps if args.fps else detected_fps

    header = struct.pack("<4sHHHHHH", HUD_MAGIC, w, h, fps, len(frames), HUD_FLAG_RLE16, 0)
    with open(args.output, "wb") as f:
        f.write(header)
        for payload in frames:
            f.write(struct.pack("<I", len(payload)))
            f.write(payload)

    size_kb = os.path.getsize(args.output) / 1024
    print(f"Wrote {args.output}: {len(frames)} frames ({w}x{h} @ {fps} fps), {size_kb:.1f} KB")


def usb_send_packet(ser, cmd: int, flags: int, payload: bytes) -> None:
    hdr = struct.pack("<4sBBI", b"HUDU", cmd, flags, len(payload))
    ser.write(hdr)
    if payload:
        ser.write(payload)
    ser.flush()


def cmd_upload(args: argparse.Namespace) -> None:
    with open(args.file, "rb") as f:
        data = f.read()

    if args.host:
        import requests
        url = f"http://{args.host}/upload"
        print(f"Uploading {len(data)} bytes to {url}...")
        resp = requests.post(url, data=data, timeout=30)
        resp.raise_for_status()
        print("Server:", resp.text)
    elif args.port:
        import serial
        print(f"Uploading {len(data)} bytes via {args.port}...")
        with serial.Serial(args.port, 115200, timeout=2) as ser:
            usb_send_packet(ser, 2, 0, b"")  # save_start
            chunk = 8192
            for i in range(0, len(data), chunk):
                usb_send_packet(ser, 3, 0, data[i:i + chunk])
                time.sleep(0.01)
            usb_send_packet(ser, 4, 0, b"")  # save_end
        print("Upload complete!")
    else:
        sys.exit("Specify --host <IP> or --port <COMx|/dev/ttyACMx>")


def send_udp_frame(sock: socket.socket, host: str, frame_id: int, rle_bytes: bytes) -> None:
    total_chunks = max(1, (len(rle_bytes) + UDP_CHUNK_SIZE - 1) // UDP_CHUNK_SIZE)
    for idx in range(total_chunks):
        chunk = rle_bytes[idx * UDP_CHUNK_SIZE:(idx + 1) * UDP_CHUNK_SIZE]
        hdr = struct.pack("<4sHHHH", b"HUDF", frame_id & 0xFFFF, idx, total_chunks, 0x01)
        sock.sendto(hdr + chunk, (host, UDP_PORT))


def cmd_stream(args: argparse.Namespace) -> None:
    w, h = (536, 240) if args.landscape else (240, 536)
    frames, detected_fps = load_frames(args.input, w, h, args.black_thresh)
    fps = args.fps if args.fps else detected_fps
    delay = 1.0 / fps
    print(f"Streaming {len(frames)} frames @ {fps} fps (Ctrl+C to stop)...")

    if args.host:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        fid = 0
        try:
            while True:
                for payload in frames:
                    t0 = time.perf_counter()
                    send_udp_frame(sock, args.host, fid, payload)
                    fid += 1
                    dt = time.perf_counter() - t0
                    if dt < delay:
                        time.sleep(delay - dt)
        except KeyboardInterrupt:
            pass
    elif args.port:
        import serial
        with serial.Serial(args.port, 115200, timeout=1) as ser:
            try:
                while True:
                    for payload in frames:
                        t0 = time.perf_counter()
                        usb_send_packet(ser, 1, 0x01, payload)
                        dt = time.perf_counter() - t0
                        if dt < delay:
                            time.sleep(delay - dt)
            except KeyboardInterrupt:
                pass
    else:
        sys.exit("Specify --host <IP> or --port <COMx|/dev/ttyACMx>")


def cmd_demo_stream(args: argparse.Namespace) -> None:
    w, h = (536, 240) if args.landscape else (240, 536)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM) if args.host else None
    ser = None
    if args.port:
        import serial
        ser = serial.Serial(args.port, 115200, timeout=1)
    if not sock and not ser:
        sys.exit("Specify --host <IP> or --port <COMx|/dev/ttyACMx>")

    print("Streaming live procedural HUD demo (Ctrl+C to stop)...")
    t0 = time.perf_counter()
    fid = 0
    try:
        while True:
            t = time.perf_counter() - t0
            im = Image.new("RGB", (w, h), (0, 0, 0))
            draw = ImageDraw.Draw(im)
            cx, cy = w // 2, h // 2
            r = 40 + int(18 * math.sin(t * 3.0))
            draw.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(0, 235, 255), width=2)
            ax = cx + int(70 * math.cos(t * 2.2))
            ay = cy + int(70 * math.sin(t * 2.2))
            draw.line((cx, cy, ax, ay), fill=(255, 170, 0), width=2)
            draw.text((16, 16), f"LIVE STREAM  T={t:5.1f}S", fill=(60, 255, 140))

            rle = encode_rle16(image_to_rgb565(im, w, h, 0))
            if sock:
                send_udp_frame(sock, args.host, fid, rle)
            else:
                usb_send_packet(ser, 1, 0x01, rle)
            fid += 1
            time.sleep(1.0 / 30.0)
    except KeyboardInterrupt:
        if ser:
            ser.close()


def main() -> None:
    p = argparse.ArgumentParser(description="HUD S3 Animation CLI")
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("convert", help="Convert GIF or PNG folder to .hud framepack")
    c.add_argument("input", help="Input .gif or directory of .png/.jpg frames")
    c.add_argument("-o", "--output", default="anim.hud", help="Output .hud file")
    c.add_argument("--fps", type=int, default=0, help="Override FPS")
    c.add_argument("--landscape", action="store_true", help="536x240 landscape instead of 240x536")
    c.add_argument("--black-thresh", type=int, default=12, help="Clamp dark pixels < thresh to pure black")
    c.set_defaults(func=cmd_convert)

    u = sub.add_parser("upload", help="Upload .hud file to device flash")
    u.add_argument("file", help="Path to .hud file")
    u.add_argument("--host", help="Device IP (e.g. 192.168.4.1)")
    u.add_argument("--port", help="Serial port (e.g. COM5 or /dev/ttyACM0)")
    u.set_defaults(func=cmd_upload)

    s = sub.add_parser("stream", help="Live-stream GIF/PNG frames to device")
    s.add_argument("input", help="Input .gif or directory of .png/.jpg frames")
    s.add_argument("--host", help="Device IP (e.g. 192.168.4.1)")
    s.add_argument("--port", help="Serial port (e.g. COM5 or /dev/ttyACM0)")
    s.add_argument("--fps", type=int, default=0, help="Override FPS")
    s.add_argument("--landscape", action="store_true")
    s.add_argument("--black-thresh", type=int, default=12)
    s.set_defaults(func=cmd_stream)

    d = sub.add_parser("demo-stream", help="Stream a live procedural test animation")
    d.add_argument("--host", help="Device IP (e.g. 192.168.4.1)")
    d.add_argument("--port", help="Serial port")
    d.add_argument("--landscape", action="store_true")
    d.set_defaults(func=cmd_demo_stream)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
