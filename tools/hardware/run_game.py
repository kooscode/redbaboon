#!/usr/bin/env python3
"""Install and run an nxdk build on the real Xbox (the green debug kit on the Modxo rig). See docs/hardware.md.

usage: run_game.py BIN_DIR [--seconds N] [--record OUT.mp4] [--keep-on] [--name RedBaboon] [--ip ADDR]

1. Switches the console off if it is on, serves (netboot) the OXOS serial-log kernel with the OXOS FTP app as the
   boot title, and powers the console on: it boots straight into the FTP app.
2. Mirrors BIN_DIR to F:\\Games\\NAME\\ (files the build no longer has are deleted there) and checks every size.
3. Launches F:\\Games\\NAME\\default.xbe through the FTP app (SITE LAUNCH), and optionally records the capture card.
4. After --seconds (default 120) saves the console's serial log for this run and switches the console OFF, unless
   --keep-on. The game stays installed: the dashboard lists it under Games at the next normal power-on.
"""
import argparse
import ftplib
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.normpath(os.path.join(HERE, "..", "..", "captures", "serial"))  # local, not in git
OXOS = os.path.normpath(os.path.join(HERE, "..", "..", "third_party", "oxos"))  # submodule
KERNEL = os.path.join(HERE, "..", "xemu", "bios", "oxos_serial_256k.bin")
FTP_APP = os.path.normpath(os.path.join(HERE, "..", "..", "build", "FtpServer"))  # make ftp_app
NETBOOT = os.path.join(OXOS, "tools", "netboot", "netboot.py")
CONSOLE = [sys.executable, "-I", os.path.join(OXOS, "tools", "modxo", "console.py")]

# The debug kit's DHCP address (2026-11-10); the subnet is scanned when it does not answer.
DEFAULT_IP = "192.168.1.91"
BROADCAST = "192.168.1.255"
FTP_BANNER = "OXOS FTP server"
FTP_USER = "xbox"
FTP_PASSWORD = "xbox"

FTP_BOOT_TIMEOUT = 60   # seconds from power-on until the FTP app must answer (it takes about 18)
POWER_OFF_SETTLE = 3    # seconds after switching off before the next power-on
NETBOOT_START = 1       # seconds for the netboot server to listen before the console asks for the kernel
RECORD_LEAD = 4         # seconds recorded before the launch: the capture card shows colour bars for the first 2-3 s
RECORD_TAIL = 5         # seconds recorded beyond --seconds, so the end is not cut short
MODXO_STATUS = b" pico "  # marks the modchip's own lines in the logger's file


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def console(cmd):
    """Runs console.py state / on / off and returns what it prints."""
    result = subprocess.run(CONSOLE + [cmd], capture_output=True, text=True, timeout=30)
    return result.stdout.strip()


# --- Checks before a run

def logger_file():
    """The running Modxo logger's output file, or None when no logger runs (the console does not boot without it).

    Read from each candidate's own arguments (/proc/PID/cmdline): a shell that started the logger has the same text
    in its command line, but not as separate arguments.
    """
    pids = subprocess.run(["pgrep", "-f", "console.py log"], capture_output=True, text=True).stdout.split()
    for pid in pids:
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                args = f.read().decode().split("\0")
            cwd = os.readlink(f"/proc/{pid}/cwd")
        except OSError:
            continue  # the process has ended
        if "log" in args and "--out" in args and any(arg.endswith("console.py") for arg in args):
            return os.path.join(cwd, args[args.index("--out") + 1])
    return None


def netboot_running():
    return subprocess.run(["pgrep", "-f", "tools/netboot/netboot.py"], capture_output=True).returncode == 0


# --- Booting the FTP app

def start_netboot(stamp):
    """Serves the serial kernel with the FTP app as the boot title, for one boot (--once)."""
    with open(os.path.join(LOGS, f"netboot-{stamp}.log"), "w") as out:
        return subprocess.Popen([sys.executable, "-I", NETBOOT, KERNEL, "--title", FTP_APP, "--title-name", "ftp",
                                 "--to", BROADCAST, "--once"], stdout=out, stderr=subprocess.STDOUT)


def ftp_answers(ip, timeout=1.0):
    """True when the OXOS FTP app greets on ip."""
    try:
        with socket.create_connection((ip, 21), timeout=timeout) as s:
            s.settimeout(timeout)
            return FTP_BANNER in s.recv(200).decode("latin1", "replace")
    except OSError:
        return False


def wait_for_ftp(ip):
    """The FTP app's address once it answers: ip, or else another address on its subnet. None if none answers."""
    for _ in range(FTP_BOOT_TIMEOUT):
        time.sleep(1)
        if ftp_answers(ip):
            return ip
    subnet = ip.rsplit(".", 1)[0]
    for host in range(2, 255):
        candidate = f"{subnet}.{host}"
        if ftp_answers(candidate, 0.2):
            return candidate
    return None


# --- Installing the build

def mirror(ftp, src, dst):
    """Makes the console's folder dst a copy of src: creates it, uploads every file, deletes what src does not have."""
    try:
        ftp.mkd(dst)
    except ftplib.error_perm:
        pass  # already there
    want = sorted(os.listdir(src))
    for name, facts in ftp.mlsd(dst):
        if name in (".", "..") or name in want:
            continue
        if facts.get("type") == "dir":
            remove_tree(ftp, f"{dst}/{name}")
        else:
            ftp.delete(f"{dst}/{name}")
        log(f"removed stale {dst}/{name}")
    for name in want:
        full = os.path.join(src, name)
        if os.path.isdir(full):
            mirror(ftp, full, f"{dst}/{name}")
        else:
            upload(ftp, full, f"{dst}/{name}")


def upload(ftp, src, dst):
    """Stores one file and checks its size on the console."""
    size = os.path.getsize(src)
    with open(src, "rb") as f:
        ftp.storbinary(f"STOR {dst}", f, blocksize=65536)
    got = ftp.size(dst)
    if got != size:
        raise SystemExit(f"{dst}: {got} bytes on the console, {size} here")
    log(f"uploaded {dst} ({size} bytes)")


def remove_tree(ftp, path):
    for name, facts in ftp.mlsd(path):
        if name in (".", ".."):
            continue
        if facts.get("type") == "dir":
            remove_tree(ftp, f"{path}/{name}")
        else:
            ftp.delete(f"{path}/{name}")
    ftp.rmd(path)


# --- After the run

def save_serial_log(serial, start, stamp):
    """Copies what the logger wrote since start, without the modchip's own lines, to LOGS/serial-STAMP.log."""
    with open(serial, "rb") as f:
        f.seek(start)
        lines = f.read().splitlines(True)
    out = os.path.join(LOGS, f"serial-{stamp}.log")
    with open(out, "wb") as f:
        f.writelines(line for line in lines if MODXO_STATUS not in line)
    return out


# --- The run

def run(args, serial, stamp):
    """Boots the FTP app, installs the build, launches it, and lets it run for args.seconds."""
    if console("state") != "off":
        log("console is on: switching it off")
        console("off")
        time.sleep(POWER_OFF_SETTLE)
    netboot = start_netboot(stamp)
    recorder = None
    try:
        time.sleep(NETBOOT_START)
        log("power on (netboot: OXOS serial kernel + FTP app)")
        console("on")
        ip = wait_for_ftp(args.ip)
        if not ip:
            raise SystemExit("the FTP app did not answer (see captures/serial/netboot-*.log and the serial log)")
        log(f"FTP app up at {ip}")
        ftp = ftplib.FTP(ip, timeout=60)
        ftp.login(FTP_USER, FTP_PASSWORD)
        try:
            ftp.mkd("/F/Games")
        except ftplib.error_perm:
            pass  # already there
        mirror(ftp, os.path.abspath(args.bin_dir), f"/F/Games/{args.name}")
        if args.record:
            recorder = subprocess.Popen([os.path.join(HERE, "record_capture.sh"), os.path.abspath(args.record),
                                         str(int(args.seconds) + RECORD_LEAD + RECORD_TAIL)])
            time.sleep(RECORD_LEAD)
        log(ftp.sendcmd(f"SITE LAUNCH /F/Games/{args.name}/default.xbe"))
        # No QUIT: the console is already rebooting into the game and would not answer it.
        ftp.close()
        log(f"running for {args.seconds:.0f} s")
        time.sleep(args.seconds)
        if recorder:
            recorder.wait()
    finally:
        if recorder and recorder.poll() is None:
            recorder.terminate()
        if netboot.poll() is None:
            netboot.terminate()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin_dir")
    ap.add_argument("--seconds", type=float, default=120, help="how long the game runs before the console goes off")
    ap.add_argument("--record", metavar="OUT.mp4", help="record the capture card (video + audio) while it runs")
    ap.add_argument("--keep-on", action="store_true", help="leave the console on at the end")
    ap.add_argument("--name", default="RedBaboon", help="folder under F:\\Games")
    ap.add_argument("--ip", default=DEFAULT_IP)
    args = ap.parse_args()
    if not os.path.isfile(NETBOOT):
        raise SystemExit("third_party/oxos is missing: git submodule update --init --recursive")
    if not os.path.isfile(os.path.join(FTP_APP, "default.xbe")):
        raise SystemExit("the FTP app is not built: make ftp_app")
    if not os.path.isfile(os.path.join(args.bin_dir, "default.xbe")):
        raise SystemExit(f"{args.bin_dir}: no default.xbe")
    serial = logger_file()
    if not serial:
        raise SystemExit("the Modxo logger is not running: python3 -I third_party/oxos/tools/modxo/console.py log "
                         "--out FILE; the console does not boot without it")
    if netboot_running():
        raise SystemExit("another netboot server is running; stop it first")

    os.makedirs(LOGS, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    serial_start = os.path.getsize(serial)
    try:
        run(args, serial, stamp)
    finally:
        log(f"serial log of this run: {save_serial_log(serial, serial_start, stamp)}")
        if not args.keep_on:
            console("off")
            log("console off")


if __name__ == "__main__":
    main()
