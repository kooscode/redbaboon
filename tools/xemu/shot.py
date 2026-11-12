#!/usr/bin/env python3
"""Screenshot of a running xemu (started by run_game.sh): reads the scanout surface over QMP and writes a PNG.

usage: shot.py OUT.png [--qmp SOCK] [--size WxH] [--bpp 32|16]

xemu has no QMP screendump, so the surface is copied from guest RAM (pmemsave) at the CRTC start address
(NV2A register 0xFD600800). The mode is not read from the hardware: pass --size for anything but 640x480x32.
"""
import argparse
import json
import os
import socket
import struct
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
CRTC_START = 0xFD600800       # NV2A PCRTC register: the scanout surface's address
PHYSICAL_MASK = 0x07FFFFFF    # keeps the register's value within RAM (128 MB)
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def qmp(f, command, **arguments):
    """Sends one QMP command and returns its answer, skipping the events xemu sends meanwhile."""
    f.write(json.dumps({"execute": command, "arguments": arguments}) + "\n")
    f.flush()
    while True:
        answer = json.loads(f.readline())
        if "return" in answer or "error" in answer:
            return answer


def read_surface(sock, size):
    """The scanout surface's address and its first size bytes, from a running xemu."""
    with socket.socket(socket.AF_UNIX) as s:
        s.connect(sock)
        f = s.makefile("rw")
        f.readline()  # QMP's greeting
        qmp(f, "qmp_capabilities")
        register = qmp(f, "human-monitor-command", **{"command-line": f"xp/1wx {CRTC_START:#x}"})["return"]
        start = int(register.split(":")[1].split()[0], 16) & PHYSICAL_MASK
        # xemu writes the file itself; it only needs a free name.
        fd, dump = tempfile.mkstemp(suffix=".bin")
        os.close(fd)
        try:
            answer = qmp(f, "pmemsave", val=start, size=size, filename=dump)
            if "error" in answer:
                raise SystemExit(answer["error"])
            with open(dump, "rb") as d:
                return start, d.read()
        finally:
            os.remove(dump)


def to_rgb(data, width, height, bytes_per_pixel):
    """PNG scanlines (filter byte 0, then RGB) from a BGRX8888 or RGB565 surface."""
    pitch = width * bytes_per_pixel
    raw = bytearray()
    for y in range(height):
        row = data[y * pitch:(y + 1) * pitch]
        rgb = bytearray(width * 3)
        if bytes_per_pixel == 4:
            rgb[0::3] = row[2::4]
            rgb[1::3] = row[1::4]
            rgb[2::3] = row[0::4]
        else:
            for x in range(width):
                v = row[2 * x] | row[2 * x + 1] << 8
                rgb[3 * x] = (v >> 11) << 3
                rgb[3 * x + 1] = ((v >> 5) & 63) << 2
                rgb[3 * x + 2] = (v & 31) << 3
        raw += b"\0" + rgb
    return bytes(raw)


def png_chunk(kind, payload):
    crc = zlib.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc)


def write_png(path, width, height, scanlines):
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB, no interlace
    with open(path, "wb") as f:
        f.write(PNG_SIGNATURE + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", zlib.compress(scanlines))
                + png_chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--qmp", default=os.path.join(HERE, "run", "qmp.sock"))
    ap.add_argument("--size", default="640x480")
    ap.add_argument("--bpp", type=int, choices=(16, 32), default=32)
    args = ap.parse_args()
    width, height = (int(n) for n in args.size.split("x"))
    bytes_per_pixel = args.bpp // 8
    start, data = read_surface(args.qmp, width * height * bytes_per_pixel)
    write_png(args.out, width, height, to_rgb(data, width, height, bytes_per_pixel))
    print(f"{args.out}: {width}x{height} from {start:#x}")


if __name__ == "__main__":
    main()
