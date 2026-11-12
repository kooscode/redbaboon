#!/usr/bin/env python3
"""Virtual controller for xemu: a uinput gamepad driven by commands, one per line, on a FIFO.

  pad.py serve [FIFO]       create the pad and read commands (run_game.sh --pad starts this BEFORE xemu, so xemu
                            binds the pad to controller port 1)
  pad.py send CMD... [FIFO] write commands to a running pad, e.g.:
      pad.py send "press A"            tap A (0.15 s)
      pad.py send "press START 0.5"    hold START 0.5 s
      pad.py send "hold RT 255" "wait 2" "release RT"
      pad.py send "axis LX -32768" "wait 1" "axis LX 0"
      pad.py send quit                 remove the pad

Buttons: A B X Y LB RB BACK START GUIDE LS RS. Axes: LX LY RX RY (-32768..32767), LT RT (0..255), HATX HATY (-1..1).
Uses the OXOS tests' VirtualPad ($OXOS_DIR/tests/virtual_gamepad.py); needs write access to /dev/uinput.
"""
import os
import signal
import sys
import time

if not os.path.isdir(os.environ.get("OXOS_DIR", "")):
    sys.exit("set OXOS_DIR to the OXOS checkout (see docs/xemu.md)")
sys.path.insert(0, os.path.join(os.environ["OXOS_DIR"], "tests"))
from virtual_gamepad import AXES, BUTTONS, VirtualPad  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FIFO = os.path.join(HERE, "run", "pad.fifo")
TAP_SECONDS = 0.15  # how long "press" holds a button when the command gives no time

# Each command's name, and whether its first argument is a control (button or axis).
COMMANDS = {"press": True, "hold": True, "release": True, "axis": True, "wait": False, "quit": False}
# How many words each command needs at least (its name included).
MIN_WORDS = {"press": 2, "hold": 2, "release": 2, "axis": 3, "wait": 2, "quit": 1}


def check(line):
    """None if line is a valid command, else what is wrong with it."""
    words = line.split()
    if not words or words[0] not in COMMANDS:
        return f"unknown command {line!r}"
    if len(words) < MIN_WORDS[words[0]]:
        return f"too few arguments in {line!r}"
    takes_control = COMMANDS[words[0]]
    if takes_control and words[1] not in BUTTONS and words[1] not in AXES:
        return f"unknown control in {line!r}"
    for word in words[2:] if takes_control else words[1:]:
        try:
            float(word)
        except ValueError:
            return f"{word!r} is not a number in {line!r}"
    return None


def execute(pad, line):
    """Carries out one checked command; returns False for quit."""
    words = line.split()
    command = words[0]
    if command == "quit":
        return False
    if command == "wait":
        time.sleep(float(words[1]))
    elif command == "press":
        pad.set(words[1], 1)
        time.sleep(float(words[2]) if len(words) > 2 else TAP_SECONDS)
        pad.set(words[1], 0)
    elif command == "hold":
        pad.set(words[1], int(float(words[2])) if len(words) > 2 else 1)
    elif command == "release":
        pad.set(words[1], 0)
    elif command == "axis":
        pad.set(words[1], int(float(words[2])))
    return True


def stop(signum, frame):
    """SIGTERM handler: ends serve() through its finally, which removes the pad."""
    sys.exit(0)


def serve(fifo):
    """Creates the pad and carries out the commands written to fifo until "quit" or SIGTERM."""
    signal.signal(signal.SIGTERM, stop)
    if not os.path.exists(fifo):
        os.mkfifo(fifo)
    pad = VirtualPad("RedBaboon test pad")
    print(f"pad: ready, commands on {fifo}", flush=True)
    try:
        while True:
            # Opening blocks until a writer opens the FIFO; the loop ends when that writer closes it.
            with open(fifo) as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    problem = check(line)
                    if problem:
                        print(f"pad: {problem}", flush=True)
                    elif not execute(pad, line):
                        return
    finally:
        pad.close()
        print("pad: removed", flush=True)


def send(commands):
    """Writes commands to a running pad; a last argument ending in .fifo names its FIFO."""
    fifo = FIFO
    if commands[-1].endswith(".fifo"):
        fifo = commands.pop()
    if not commands:
        sys.exit("no commands to send")
    for command in commands:
        problem = check(command)
        if problem:
            sys.exit(problem)
    with open(fifo, "w") as f:
        f.write("\n".join(commands) + "\n")


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "serve":
        serve(sys.argv[2] if len(sys.argv) > 2 else FIFO)
    elif len(sys.argv) >= 3 and sys.argv[1] == "send":
        send(sys.argv[2:])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
