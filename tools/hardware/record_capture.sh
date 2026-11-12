#!/bin/sh
# Record the real console from the USB capture card: video (/dev/video0, MJPEG 1920x1080 at 30 fps) and the card's
# audio (ALSA hw:CARD=MS2109,DEV=0), as H.264 + AAC.
#
#   record_capture.sh OUT.mp4 SECONDS [VIDEO_DEVICE]
#
# The card shows colour bars for the first 2-3 s after it is opened, and whenever the console sends no picture (a
# boot, a video mode change): start recording a few seconds before what you want, and trim.
set -eu
if [ $# -lt 2 ]; then
    sed -n '2,8p' "$0" >&2
    exit 2
fi
out=$1
seconds=$2
video=${3:-/dev/video0}
exec ffmpeg -loglevel error -y -f v4l2 -input_format mjpeg -video_size 1920x1080 -framerate 30 \
    -thread_queue_size 512 -i "$video" -f alsa -thread_queue_size 512 -ac 2 -ar 48000 -i hw:CARD=MS2109,DEV=0 \
    -t "$seconds" -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p -c:a aac -b:a 160k "$out"
