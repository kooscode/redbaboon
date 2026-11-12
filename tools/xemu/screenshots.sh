#!/bin/bash
# Boots a build in xemu (OXOS serial kernel), takes screenshots, keeps the serial log, and stops xemu.
# Holds a lock, so runs from several shells take turns (the rig has one run folder).
#
#   tools/xemu/screenshots.sh BIN_DIR OUT_DIR [WAIT_S] [SHOTS] [INTERVAL_S] [PAD_SCRIPT]
#     BIN_DIR     folder with default.xbe
#     OUT_DIR     receives shot_00.png ..., serial.log
#     WAIT_S      seconds from xemu start to the first screenshot (default 25: ~10 s boot + game start-up)
#     SHOTS       number of screenshots (default 3)
#     INTERVAL_S  seconds between screenshots (default 3)
#     PAD_SCRIPT  optional file of pad.py commands, one per line (e.g. "hold RT 255", "wait 2", "axis LX -32768");
#                 it starts when the first screenshot would, and the screenshots run alongside it.
set -u
RIG=$(cd "$(dirname "$0")" && pwd)
bin=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
wait_s=${3:-25}
shots=${4:-3}
interval=${5:-3}
pad_script=${6:-}

exec 9>"$RIG/run.lock"
flock 9

pad_opt=
if [ -n "$pad_script" ]; then
    pad_opt=--pad
fi
start=$(date +%s)
"$RIG/run_game.sh" "$bin" $pad_opt > "$out/serial.log" 2>&1 &
rig_pid=$!
sleep "$wait_s"
if [ -n "$pad_script" ]; then
    (
        while IFS= read -r line; do
            if [ -n "$line" ]; then
                python3 -I "$RIG/pad.py" send "$line"
            fi
        done < "$pad_script"
    ) &
fi
for ((i = 0; i < shots; i++)); do
    python3 -I "$RIG/shot.py" "$(printf '%s/shot_%02d.png' "$out" "$i")" > /dev/null 2>&1 \
        || echo "screenshot $i failed" >&2
    if ((i + 1 < shots)); then
        sleep "$interval"
    fi
done
pkill -P "$rig_pid" 2>/dev/null
kill "$rig_pid" 2>/dev/null
sleep 2
pkill -f "config_path.*$RIG" 2>/dev/null
wait "$rig_pid" 2>/dev/null
echo "done in $(( $(date +%s) - start )) s: $(ls "$out")"
grep -a -m5 "unhandled exception\|bug check" "$out/serial.log"
exit 0
