#!/bin/sh
# Boot xemu on OXOS straight into an nxdk game: the bin folder becomes an XISO in the DVD drive, and OXOS starts the
# disc's default.xbe at power-on. See docs/xemu.md.
#
#   run_game.sh BIN_DIR [--release | --bios FILE] [--memory 64|128] [--fresh] [--pad] [-- RUN.PY-ARGS...]
#     BIN_DIR        the nxdk build output: default.xbe and the files it loads from D:\
#     --release      the release kernel (no serial log); default is the serial-log kernel (DbgPrint + kernel log)
#     --bios FILE    any other OXOS image (e.g. a TRACE_IMPORTS=2 build)
#     --memory N     console RAM in MB (default 64, a retail console)
#     --fresh        start the disk over from the template (wipes saves and anything copied to it)
#     --pad          create the virtual controller first (pad.py serve) and make xemu see only it
#     -- ARGS        passed to OXOS's tools/run.py (third_party/oxos) (e.g. -- --scale 2 --widescreen)
#
# Everything this rig writes is under tools/xemu/run/: game.iso, state/ (the persistent disk copy + EEPROM),
# qmp.sock, pad.fifo, logs/. The serial log is printed here and saved in run/logs/. Close the xemu window, or kill
# this script, to stop.
set -eu
RIG=$(cd "$(dirname "$0")" && pwd)
OXOS=$(cd "$RIG/../../third_party/oxos" && pwd)
if [ ! -f "$OXOS/tools/run.py" ]; then
    echo "third_party/oxos is missing: git submodule update --init --recursive" >&2
    exit 1
fi
RUN="$RIG/run"

if [ $# -lt 1 ] || [ ! -f "$1/default.xbe" ]; then
    sed -n '4,14p' "$0" >&2
    exit 2
fi
bin=$(cd "$1" && pwd)
shift
bios="$RIG/bios/oxos_serial_256k.bin"
serial=--serial
memory=64
fresh=
pad=0
while [ $# -gt 0 ]; do
    case "$1" in
        --release)
            bios="$RIG/bios/oxos_release_256k.bin"
            serial=
            ;;
        --bios)
            bios=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
            shift
            ;;
        --memory)
            memory="$2"
            shift
            ;;
        --fresh)
            fresh=--fresh
            ;;
        --pad)
            pad=1
            ;;
        --)
            shift
            break
            ;;
        *)
            echo "unknown option $1" >&2
            exit 2
            ;;
    esac
    shift
done

mkdir -p "$RUN/logs"
python3 -I "$RIG/make_iso.py" "$bin" "$RUN/game.iso"
rm -f "$RUN/qmp.sock"

# The virtual pad, when --pad asks for it; removed again when this script ends.
pad_pid=
if [ "$pad" = 1 ]; then
    rm -f "$RUN/pad.fifo"
    python3 -I "$RIG/pad.py" serve "$RUN/pad.fifo" &
    pad_pid=$!
    sleep 1
    # xemu sees only the virtual pad (not the real controllers), also while its window has no focus.
    export SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x045e/0x028e
fi
# A signal, not "quit" on the FIFO: writing to the FIFO would block forever if the pad server has already gone
# (tools/xemu_test.sh stops this script's children itself).
cleanup() {
    if [ -n "$pad_pid" ]; then
        kill "$pad_pid" 2>/dev/null || true
        wait "$pad_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

log="$RUN/logs/serial-$(date +%Y%m%d-%H%M%S).log"
echo "xemu: $(basename "$bios"), ${memory} MB, disc $RUN/game.iso, QMP $RUN/qmp.sock${serial:+, serial log $log}"
python3 -I "$OXOS/tools/run.py" "$bios" --iso "$RUN/game.iso" --hdd "$RIG/disk/oxos_exfat_template.qcow2" \
    --state "$RUN/state" $fresh --memory "$memory" --qmp "$RUN/qmp.sock" $serial "$@" 2>&1 | tee "$log"
