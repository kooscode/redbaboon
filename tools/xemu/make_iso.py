#!/usr/bin/env python3
"""Pack an nxdk app's bin folder (default.xbe + data) into an XISO for xemu's DVD drive.

usage: make_iso.py BIN_DIR OUT.iso

OXOS boots \\Device\\CdRom0\\default.xbe at power-on when a disc is in the drive, so the game starts directly.
Uses the XISO writer of the OXOS tests ($OXOS_DIR/tests/mkxiso.py).
"""
import os
import sys


def oxos_dir():
    """The OXOS checkout (OXOS_DIR in the environment; the Makefile sets it from local.mk)."""
    path = os.environ.get("OXOS_DIR")
    if not path or not os.path.isdir(path):
        sys.exit("set OXOS_DIR to the OXOS checkout (see docs/xemu.md)")
    return path


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    if not os.path.isfile(os.path.join(src, "default.xbe")):
        sys.exit(f"{src}: no default.xbe")
    sys.path.insert(0, os.path.join(oxos_dir(), "tests"))
    import mkxiso

    mkxiso.write_image(out, mkxiso.tree_from_dir(src))
    print(f"{out}: {os.path.getsize(out) // 1024} KB")


if __name__ == "__main__":
    main()
