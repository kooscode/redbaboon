#!/bin/sh
# Install and run a build on the real Xbox: run_game.sh BIN_DIR [OPTIONS]. See run_game.py and docs/hardware.md.
exec python3 -I "$(dirname "$0")/run_game.py" "$@"
