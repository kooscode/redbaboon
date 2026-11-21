#!/usr/bin/env python3
"""Write a version into an XBE's certificate (cxbe always leaves it 0).

usage: xbe_version.py XBE VERSION

VERSION is MAJOR.MINOR (e.g. 0.03); the certificate's version field (offset 0xAC) becomes MAJOR << 16 | MINOR, so
0.03 is 00000003 and 1.10 is 0001000A. Dashboards and OXOS's ISO launcher show the field. Run it after each link:
the XBE is changed in place, and running it twice gives the same file.
"""
import struct
import sys

BASE_ADDRESS = 0x104         # header: the address the XBE loads at
CERTIFICATE_ADDRESS = 0x118  # header: the certificate's address
CERTIFICATE_VERSION = 0xAC   # certificate: the title's version


def parse_version(text):
    """MAJOR.MINOR as the certificate's 32-bit value."""
    major, _, minor = text.partition(".")
    if not major.isdigit() or not minor.isdigit() or int(major) > 0xFFFF or int(minor) > 0xFFFF:
        sys.exit(f"bad version {text!r}: expected MAJOR.MINOR, e.g. 0.03")
    return int(major) << 16 | int(minor)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    path, version = sys.argv[1], parse_version(sys.argv[2])
    with open(path, "r+b") as xbe:
        data = xbe.read()
        if data[:4] != b"XBEH":
            sys.exit(f"{path}: not an XBE")
        base = struct.unpack_from("<I", data, BASE_ADDRESS)[0]
        certificate = struct.unpack_from("<I", data, CERTIFICATE_ADDRESS)[0] - base
        xbe.seek(certificate + CERTIFICATE_VERSION)
        xbe.write(struct.pack("<I", version))


if __name__ == "__main__":
    main()
