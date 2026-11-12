# Running RedBaboon on a real Xbox

The scripts in `tools/hardware/` install a build on a real console, run it, record it and keep its serial log, with
no one at the console. They need:

- a console with a [Modxo](https://github.com/shalxmva/modxo) modchip (RP2040 on the LPC bus, USB to the PC): it
  serves the BIOS, forwards the debug serial port (COM1) and presses the power button;
- an [OXOS](https://github.com/kooscode/oxos) checkout (`OXOS_DIR`) for its netboot server, its FTP app and the Modxo
  logger and power control (`tools/modxo/console.py`);
- the OXOS serial kernel in `tools/xemu/bios/oxos_serial_256k.bin` (docs/xemu.md §1);
- optionally, a USB capture card (MS2109) for recordings.

Nothing on the console is flashed.

## Quick start

```sh
make hardware                                                # install to F:\Games\RedBaboon, run 120 s, power off
make hardware ARGS="--seconds 300 --record captures/run.mp4" # ... and record the capture card meanwhile
tools/hardware/run_game.sh build/RedBaboon [OPTIONS]         # the same without make (needs OXOS_DIR)
```

The game stays installed: at the next normal power-on, the console's own BIOS boots the dashboard, which lists it
under Games.

## 1. How a run works

`tools/hardware/run_game.py BIN_DIR` (`run_game.sh` calls it):

1. Switches the console off if it is on.
2. Runs OXOS's `tools/netboot/netboot.py --once`, serving the OXOS serial-log kernel with the OXOS FTP app as the
   boot title.
3. Powers the console on. The modchip's netboot loader takes the kernel from the network, and the console boots
   straight into the FTP app in about 18 s.
4. Mirrors `BIN_DIR` to `F:\Games\RedBaboon\` (`/F/Games/RedBaboon`): changed files are replaced, files the build no
   longer has are deleted, and every size is checked.
5. Launches the game through the FTP app (`SITE LAUNCH /F/Games/RedBaboon/default.xbe`); the console quick-reboots
   into it.
6. After `--seconds` (default 120), saves the run's serial log as `captures/serial/serial-*.log` and switches the
   console off.

Options: `--record OUT.mp4`, `--keep-on`, `--name` (another folder under `F:\Games`), `--ip` (the console's address;
the subnet is scanned when it doesn't answer).

## 2. Recording

`tools/hardware/record_capture.sh OUT.mp4 SECONDS [VIDEO_DEVICE]` records the capture card: video from
`/dev/video0` (MJPEG 1920x1080 at 30 fps), audio from the card's ALSA device (`hw:CARD=MS2109,DEV=0`), as H.264 +
AAC.

- The card shows colour bars for the first 2-3 s after it is opened, and whenever the console sends no picture (a
  boot, a quick reboot, a video mode change); it needs about 1 s to lock onto a new signal.
- `run_game.py --record` therefore records from 4 s before the launch until 5 s after `--seconds`; trim the start.
- Only one program can hold the card.

## 3. Unattended play

The modchip has no path to the controller ports, so a run cannot press buttons. Put `autoplay.txt` in `BIN_DIR` and
the game plays itself (docs/design.md §9; `seed=fixed` gives the same mission every run). Remove it again before an
install meant for playing: `run_game.py` mirrors the folder, so a build without it removes it from the console.

## 4. Serial log

- The serial kernel sends its log, and the game's `DbgPrint` text, to COM1, which the Modxo forwards over USB.
- The OXOS Modxo logger (`$OXOS_DIR/tools/modxo/console.py log --out FILE`) must be running: the console does not
  boot while nobody holds the Modxo's serial port. `run_game.py` finds the logger's file and copies each run's part
  of it. Kernel and game lines are prefixed `uart`; the modchip's own status lines are dropped.
- A crash shows up as `*** unhandled exception ...` with registers and stack, then `*** bug check`.
- `hang:   title return addresses ...` lines are periodic samples of where the title is, normal while it runs.

## 5. Power

- `python3 -I $OXOS_DIR/tools/modxo/console.py state | on | off` (with the logger running). `run_game.py` uses these.
- Leave the console off after a run; `run_game.py` does this unless `--keep-on`.
- Only one netboot server at a time: `run_game.py` refuses to start when one is running, and its own server exits
  after one boot.

## Uninstall

Boot the FTP app without launching anything, then delete the folder (one `-Q` per file, folders last):

```sh
cd $OXOS_DIR
python3 -I tools/netboot/netboot.py REDBABOON/tools/xemu/bios/oxos_serial_256k.bin \
    --title tools/ftp_server/bin --title-name ftp --to BROADCAST_ADDRESS --once &
python3 -I tools/modxo/console.py on
# about 20 s later:
curl ftp://CONSOLE_ADDRESS/ -u x:x -Q "DELE /F/Games/RedBaboon/default.xbe" -Q "RMD /F/Games/RedBaboon"
python3 -I tools/modxo/console.py off
```
