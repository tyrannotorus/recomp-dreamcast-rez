#!/bin/sh
# Rez for NextUI (tg5040: the TrimUI Brick and Smart Pro), started from Tools > Rez. rez looks for
# the player's CHD in disc/ beside it, says what to do when there is none (or asks which one to
# play when there are two), then plays.
PAK_DIR="$(dirname "$0")"
cd "$PAK_DIR" || exit 1
LOG="${LOGS_PATH:-/mnt/SDCARD/.userdata/tg5040/logs}/Rez.txt"
mkdir -p "$(dirname "$LOG")"
# Settings and the memory card: .userdata/shared/rez_recompiled/.
export XDG_CONFIG_HOME="${SHARED_USERDATA_PATH:-/mnt/SDCARD/.userdata/shared}"
./rez > "$LOG" 2>&1
