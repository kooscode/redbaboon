# Running RedBaboon in xemu

The rig in `tools/xemu/` boots [xemu](https://xemu.app) on the [OXOS](https://github.com/kooscode/oxos) kernel
straight into a build of the game: the build folder becomes an XISO in the DVD drive, and OXOS starts its
`default.xbe` at power-on, with no dashboard, FTP or network step. The rig uses three things from the OXOS submodule
(`third_party/oxos`): `tools/run.py`, which starts xemu, and two test helpers, the XISO writer and the virtual gamepad.

To just play the game in your own xemu, `make iso` and load `build/RedBaboon.iso` (Machine > Load Disc) instead.

## Quick start

```sh
make xemu                                 # build, then boot it in xemu
make xemu ARGS=--pad                      # ... with a virtual controller you can drive (see Input)
tools/xemu/run_game.sh build/RedBaboon    # the same without make
```

`run_game.sh BIN_DIR` packs the folder into `tools/xemu/run/game.iso`, puts it in xemu's DVD drive and boots. The
game comes up about 10 s later. Its files are on `D:\` as on a real disc. Close the xemu window (or kill the
script) to stop.

Options: `--release` (kernel without serial log), `--memory 128`, `--fresh` (reset the disk), `--pad`, and `-- ARGS`
for OXOS's `tools/run.py` (e.g. `-- --scale 2`, `-- --widescreen`).

## 1. Setting up the rig

The rig's kernels and disk are not in git; put them in place once:

| File | What |
|---|---|
| `tools/xemu/bios/oxos_serial_256k.bin` | default: the kernel log and the game's DbgPrint on the serial port |
| `tools/xemu/bios/oxos_release_256k.bin` | the release kernel, as shipped (`--release`) |
| `tools/xemu/disk/oxos_exfat_template.qcow2` | an OXOS exFAT test disk (see "The disk") |

The kernels are OXOS's own open images: **no MCPX boot ROM is needed** (xemu's `bootrom_path` stays empty and xemu
starts in OXOS's reset code). Build them from the OXOS submodule:

```sh
make -C third_party/oxos/kernel BUILD=$PWD/build/oxos/rel SELFTEST=0 $PWD/build/oxos/rel/bios_256k.bin
make -C third_party/oxos/kernel BUILD=$PWD/build/oxos/ser SELFTEST=0 SERIAL=1 $PWD/build/oxos/ser/bios_256k.bin
cp build/oxos/rel/bios_256k.bin tools/xemu/bios/oxos_release_256k.bin
cp build/oxos/ser/bios_256k.bin tools/xemu/bios/oxos_serial_256k.bin
```

**The disk:** `oxos_exfat_template.qcow2` is one exFAT volume whose top-level folders are the drives
(`/C /E /F /X /Y /Z`). The first run copies it to `tools/xemu/run/state/hdd.qcow2`, which keeps saves (`E:\UDATA`,
`E:\TDATA`) between runs; `--fresh` starts over from the template. To look inside with xemu stopped:
`python3 -I third_party/oxos/tests/exfat.py --ls tools/xemu/run/state/hdd.qcow2 /E/UDATA`.

## 2. The xemu command line

`run_game.sh` runs (serial kernel, 64 MB):

```sh
python3 -I third_party/oxos/tools/run.py tools/xemu/bios/oxos_serial_256k.bin --iso tools/xemu/run/game.iso \
    --hdd tools/xemu/disk/oxos_exfat_template.qcow2 --state tools/xemu/run/state --memory 64 \
    --qmp tools/xemu/run/qmp.sock --serial
```

- `run.py` writes its own xemu config to a temporary folder (`-config_path`), so it never reads or changes your
  `~/.local/share/xemu` settings and runs next to any other xemu. `run.py --help` lists every option.
- xemu always opens a window; there is no headless mode.
- QMP: `tools/xemu/run/qmp.sock` (used by `shot.py`; HMP commands go through `human-monitor-command`, e.g.
  `xp/16wx 0x80010000` to read physical memory).
- Networking is off. Add `-- --net-forward 2121:21` (xemu NAT, console at 10.0.2.15) if a build needs it.

## 3. Debugging

**Text output**
- Serial kernel (default): the kernel's log and everything the game sends with `DbgPrint` / `OutputDebugStringA`
  print in the terminal and go to `tools/xemu/run/logs/serial-*.log`.
- nxdk's `debugPrint()` draws on the screen only, so the game logs with `DbgPrint`.
- Release kernel: DbgPrint goes only to an in-memory ring (kernel symbols `XkDebugLog` / `XkDebugLogPosition`).

**Crashes**
- The serial log shows `*** unhandled exception 0xc0000005 at <eip> ...` with registers, code bytes and stack, then
  `*** bug check ...`. The screen shows OXOS's fatal-error screen.
- An `eip` below 0x80000000 is in the game (XBE base 0x10000); look it up in the linker map.

**Kernel-call trace** (what the game called last before a crash): build a tracing kernel and run it with its map:

```sh
make -C third_party/oxos/kernel BUILD=$PWD/build/oxos/trace SELFTEST=0 TRACE_IMPORTS=2 SERIAL=1 \
    $PWD/build/oxos/trace/bios.bin
tools/xemu/run_game.sh build/RedBaboon --bios build/oxos/trace/bios.bin --memory 128 \
    -- --trace-map build/oxos/trace/xboxkrnl.map --dump-dir tools/xemu/run/logs
```

On a crash it prints the game's last 30 kernel calls and saves all of them. TRACE_IMPORTS=2 needs `--memory 128`.

**Unattended runs:** `tools/xemu/screenshots.sh BIN_DIR OUT_DIR [WAIT_S] [SHOTS] [INTERVAL_S] [PAD_SCRIPT]` boots a
build, takes screenshots, keeps the serial log and stops xemu; `tools/xemu/record_window.sh BIN_DIR OUT.mp4 ...`
records the xemu window instead. Both take turns with other runs through a lock (`tools/xemu/run.lock`). An
`autoplay.txt` next to `default.xbe` makes the game play itself (docs/design.md §9).

**Screenshots:** `tools/xemu/shot.py out.png [--size 640x480] [--bpp 32|16]` copies the visible framebuffer over
QMP (xemu has no screendump). The video mode isn't read from the hardware, so pass `--size` (and `--bpp 16`) for
other modes.

**Input** (`--pad`): `run_game.sh` creates a virtual Xbox 360-style pad before xemu starts and makes xemu ignore
the real controllers. Drive it from another shell:

```sh
tools/xemu/pad.py send "press START"                         # tap
tools/xemu/pad.py send "hold A" "wait 2" "release A"
tools/xemu/pad.py send "axis LX -32768" "wait 1" "axis LX 0" # stick left, then centre
tools/xemu/pad.py send "hold RT 255"                         # trigger fully down
```

- Buttons: A B X Y LB RB BACK START GUIDE LS RS. Axes: LX LY RX RY (-32768..32767), LT RT (0..255).
- It needs write access to `/dev/uinput`.
- Only one rig at a time should use `--pad`: every xemu on the desktop sees the pad.

## 4. Platform notes

**Sound:**
- xemu plays only what the MCPX APU's voice processor makes, never the AC'97 output (xemu issue #1529). nxdk's
  XAudio and SDL's audio callback are therefore silent in xemu, whichever kernel runs.
- RedBaboon handles this itself: it drives the AC'97 on a console and plays the same mix through one APU voice in
  xemu (`src/audio/audio_xbox.cpp`, `audio_apu.cpp`; docs/design.md §6). Keep xemu's "Use DSP" setting off, its
  default.

**Memory:** 64 MB by default, as a retail console. With `--memory 128`, OXOS gives a title the upper 64 MB only when
its XBE header's init flags (+0x124) don't have bit 0x4 ("limit to 64 MB") set. nxdk's cxbe always sets that bit, so
the game gets 64 MB even on a 128 MB console, as retail games do.
