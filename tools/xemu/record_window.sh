#!/bin/bash
# Boots a build in xemu (OXOS serial kernel) and records the xemu window to an mp4 (X11 screen grab), then stops.
# Shares screenshots.sh's lock, so runs take turns. The clip has no sound: the screen grab records video only.
#
#   tools/xemu/record_window.sh BIN_DIR OUT.mp4 [WAIT_S] [SECONDS] [PAD_SCRIPT]
#     WAIT_S      seconds from xemu start to the start of recording (default 25)
#     SECONDS     clip length (default 20)
#     PAD_SCRIPT  optional file of pad.py commands, one per line, started with the recording
set -u
RIG=$(cd "$(dirname "$0")" && pwd)
bin=$(cd "$1" && pwd)
out=$2
wait_s=${3:-25}
seconds=${4:-20}
pad_script=${5:-}
mkdir -p "$(dirname "$out")"

exec 9>"$RIG/run.lock"
flock 9

pad_opt=
if [ -n "$pad_script" ]; then
    pad_opt=--pad
fi
"$RIG/run_game.sh" "$bin" $pad_opt > "${out%.mp4}_serial.log" 2>&1 &
rig_pid=$!
sleep "$wait_s"
# The xemu client window (the game picture, without the window frame): named "xemu | ...", class xemu.
line=$(xwininfo -root -tree | grep '"xemu |' | grep '"app.xemu.xemu"' | head -1)
size=$(echo "$line" | awk '{print $(NF-1)}' | cut -d+ -f1)
w=${size%x*}
h=${size#*x}
offset=$(echo "$line" | awk '{print $NF}')
x=$(echo "$offset" | cut -d+ -f2)
y=$(echo "$offset" | cut -d+ -f3)
if [ -n "$pad_script" ]; then
    (
        while IFS= read -r line; do
            if [ -n "$line" ]; then
                python3 -I "$RIG/pad.py" send "$line"
            fi
        done < "$pad_script"
    ) &
fi
ffmpeg -y -loglevel error -f x11grab -framerate 30 -video_size "$((w / 2 * 2))x$((h / 2 * 2))" \
    -i "$DISPLAY+$x,$y" -t "$seconds" -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p "$out"
pkill -P "$rig_pid" 2>/dev/null
kill "$rig_pid" 2>/dev/null
sleep 2
pkill -f "config_path.*$RIG" 2>/dev/null
wait "$rig_pid" 2>/dev/null
echo "recorded $out (window ${w}x${h} at $x,$y)"
