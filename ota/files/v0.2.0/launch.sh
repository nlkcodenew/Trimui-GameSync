#!/bin/sh
# launch.sh — Trimui-GameSync (TrimUI Brick Pro / Smart Pro S, stock OS).
case "$0" in
  */*) cd "$(dirname "$0")" || exit 1 ;;
esac

APP="$(pwd)"

# --- Dual-device library handling (lesson from RomCloud on Smart Pro S) ---
# Smart Pro S (new stock OS, /proc/device-tree/model contains "sun55i") needs
# the SYSTEM libSDL2 + libfreetype: the bundled Brick Pro (Mali/fbdev) SDL2
# cannot create a window there (and old bundled freetype 2.12.1 clashes with
# system harfbuzz: "undefined symbol: FT_Done_MM_Var").
# Brick Pro keeps the previous behaviour.
TRIMUI_MODEL="$(cat /proc/device-tree/model 2>/dev/null | tr -d '\0')"
case "$TRIMUI_MODEL" in
  *sun55i*)
    for d in /usr/trimui/lib /usr/lib /lib /mnt/SDCARD/System/lib; do
      for lib in libSDL2-2.0.so.0 libfreetype.so.6; do
        if [ -f "$d/$lib" ]; then
          LD_PRELOAD="${LD_PRELOAD:+$LD_PRELOAD }$d/$lib"
        fi
      done
    done
    export LD_PRELOAD
    ;;
esac

export LD_LIBRARY_PATH="$APP/lib:/usr/trimui/lib:/usr/lib64:/usr/lib:/lib:$LD_LIBRARY_PATH"
# No system CA certificates on TrimUI stock OS: use the bundled Mozilla bundle
# for every HTTPS call (Drive API, OTA).
export SSL_CERT_FILE="$APP/certs/cacert.pem"
export CURL_CA_BUNDLE="$APP/certs/cacert.pem"

mkdir -p "$APP/data"
touch /tmp/stay_alive 2>/dev/null || true

# Silent auto OTA (same model as Trimui-LocalSend):
#  - one check 30s after the app opens, then every 10 minutes while open
#  - downloads + installs quietly; errors only go to ota.log
#  - a new version takes effect on the NEXT app launch (never restarts
#    mid-session)
#  - disable entirely: TRIMUI_GS_NO_OTA=1
case "$(cat "$APP/.ota-status" 2>/dev/null)" in
  done*) rm -f "$APP/.ota-status" "$APP/.ota-status.tmp" 2>/dev/null;;
esac
if [ -z "$TRIMUI_GS_NO_OTA" ] && [ ! -f "$APP/data/.no-ota" ] && [ -f "$APP/ota-update.sh" ]; then
  (
    sleep 30
    while :; do
      sh "$APP/ota-update.sh" --apply >> "$APP/ota.log" 2>&1
      sleep 600
    done
  ) &
  OTA_PID=$!
fi

"$APP/bin/trimui-gamesync" >> "$APP/gamesync.log" 2>&1
CODE=$?

[ -n "${OTA_PID:-}" ] && kill "$OTA_PID" 2>/dev/null
rm -f /tmp/stay_alive 2>/dev/null || true
sync
exit "$CODE"
