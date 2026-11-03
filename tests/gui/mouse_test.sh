#!/bin/bash
# mouse_test.sh — Task 1: tab-bar mouse switching, closing and creation.
# A fresh window starts with exactly one tab, so click positions are known:
# tab 1 at x 0-119, tab 2 at x 120-239, '+' box after the last tab, and the
# close 'x' in the last 28 px of a tab. TAB_HEIGHT is 34, so y=10 is in bar.
# Usage: mouse_test.sh <mode-tag> [extra terminator args...]
set -u
TAG="$1"; shift
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${TERMINATOR_BIN:-$REPO/terminator}"
KEYS="$REPO/tests/gui/keys-bin"
if [ ! -x "$KEYS" ]; then gcc -O2 -o "$KEYS" "$REPO/tests/gui/keys.c" -lX11 || exit 1; fi
DISP=":$((RANDOM % 100 + 60))"
rm -f "/tmp/.X${DISP#:}-lock"
WORK="/tmp/mousework_$TAG"
HOME_DIR="/tmp/mousehome_$TAG"
rm -rf "$WORK" "$HOME_DIR"; mkdir -p "$WORK" "$HOME_DIR"
cd "$WORK" || exit 1

Xvfb "$DISP" -screen 0 1024x768x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1
DISPLAY="$DISP" HOME="$HOME_DIR" "$BIN" "$@" >/tmp/mouse_$TAG.log 2>&1 &
GUI_PID=$!
sleep 2

K() { DISPLAY="$DISP" "$KEYS" "$@" >/dev/null 2>&1; }
MPASS=0; MFAIL=0
gexact() { # gexact <name> <file> <expected-content>
    if [ -f "$2" ] && [ "$(cat "$2")" = "$3" ]; then MPASS=$((MPASS+1)); echo "PASS[$TAG]: $1";
    else MFAIL=$((MFAIL+1)); echo "FAIL[$TAG]: $1 (file $2 has '$(cat "$2" 2>/dev/null)', expected '$3')"; fi
}
alive() { kill -0 $GUI_PID 2>/dev/null && [ "$(cut -d' ' -f3 /proc/$GUI_PID/stat 2>/dev/null)" != "Z" ]; }

# Tab 1 starts in $WORK; move it to /tmp so tabs are distinguishable.
K "type:cd /tmp" key:Return sleep:600
# Click the '+' box (x=180 is inside it while there is 1 tab).
K click:180,10 sleep:600 "type:pwd > /tmp/m2_$TAG.txt" key:Return sleep:800
gexact "click + creates tab 2" "/tmp/m2_$TAG.txt" "$WORK"
# Click tab 1, then tab 2, then tab 2's close 'x'.
K click:60,10 sleep:400 "type:pwd > /tmp/m1_$TAG.txt" key:Return sleep:800
gexact "click tab 1 switches" "/tmp/m1_$TAG.txt" "/tmp"
K click:180,10 sleep:400 "type:pwd > /tmp/m2b_$TAG.txt" key:Return sleep:800
gexact "click tab 2 switches back" "/tmp/m2b_$TAG.txt" "$WORK"
K click:220,10 sleep:600 "type:pwd > /tmp/mclosed_$TAG.txt" key:Return sleep:800
gexact "click x closes tab 2" "/tmp/mclosed_$TAG.txt" "/tmp"
if alive; then MPASS=$((MPASS+1)); echo "PASS[$TAG]: GUI alive after mouse close";
else MFAIL=$((MFAIL+1)); echo "FAIL[$TAG]: GUI died after mouse close"; fi

kill $GUI_PID 2>/dev/null; wait $GUI_PID 2>/dev/null
kill $XVFB_PID 2>/dev/null
echo "MOUSE[$TAG]: $MPASS passed, $MFAIL failed"
[ "$MFAIL" -eq 0 ]
