#!/bin/bash
# edge_test.sh — line-editor boundary cases (user's edge-case table):
# Backspace/Delete on empty input, Left at beginning, Right at end,
# Ctrl+U at beginning, Ctrl+K at end, Ctrl+W at beginning, Up with
# empty history, Down at the latest history entry. Every case must
# leave the GUI alive with no corruption; cursor cases are proven by
# the exact file content of the command that finally runs.
# Usage: edge_test.sh <mode-tag> [extra terminator args...]
set -u
TAG="$1"; shift
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${TERMINATOR_BIN:-$REPO/terminator}"
KEYS="$REPO/tests/gui/keys-bin"
if [ ! -x "$KEYS" ]; then gcc -O2 -o "$KEYS" "$REPO/tests/gui/keys.c" -lX11 || exit 1; fi
DISP=":$((RANDOM % 100 + 60))"
rm -f "/tmp/.X${DISP#:}-lock"
WORK="/tmp/edgework_$TAG"
HOME_DIR="/tmp/edgehome_$TAG"
rm -rf "$WORK" "$HOME_DIR"; mkdir -p "$WORK" "$HOME_DIR"
cd "$WORK" || exit 1

Xvfb "$DISP" -screen 0 1024x768x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1
DISPLAY="$DISP" HOME="$HOME_DIR" "$BIN" "$@" >/tmp/edge_$TAG.log 2>&1 &
GUI_PID=$!
sleep 2

K() { DISPLAY="$DISP" "$KEYS" "$@" >/dev/null 2>&1; }
EPASS=0; EFAIL=0
gexact() { # gexact <name> <file> <expected-content>
    if [ -f "$2" ] && [ "$(cat "$2")" = "$3" ]; then EPASS=$((EPASS+1)); echo "PASS[$TAG]: $1";
    else EFAIL=$((EFAIL+1)); echo "FAIL[$TAG]: $1 (file $2 has '$(cat "$2" 2>/dev/null)', expected '$3')"; fi
}
alive() { kill -0 $GUI_PID 2>/dev/null && [ "$(cut -d' ' -f3 /proc/$GUI_PID/stat 2>/dev/null)" != "Z" ]; }

# --- Empty history + empty input, first keys in a fresh window ---
K key:Up key:Down key:BackSpace key:Delete sleep:400 "type:echo edge-empty > $WORK/edge_empty.txt" key:Return sleep:800
gexact "Up/Down/Backspace/Delete on empty history+input: no crash" "$WORK/edge_empty.txt" "edge-empty"

# --- Left at beginning (clamped) + Ctrl+U at beginning (no-op) ---
K "type:echo ab" key:Left key:Left key:Left key:Left key:Left key:Left key:Left key:Left key:Left key:Left sleep:200 ctrl:u key:End sleep:200 "type: > $WORK/edge_left.txt" key:Return sleep:800
gexact "Left at beginning + Ctrl+U at beginning: no corruption" "$WORK/edge_left.txt" "ab"

# --- Right at end (clamped) ---
K "type:echo ab" key:Right key:Right key:Right key:Right key:Right key:Right key:Right key:Right key:Right key:Right sleep:200 "type:X" sleep:200 "type: > $WORK/edge_right.txt" key:Return sleep:800
gexact "Right at end: cursor stays at end" "$WORK/edge_right.txt" "abX"

# --- Ctrl+K at end (no-op) ---
K "type:echo ab" ctrl:k sleep:200 "type: > $WORK/edge_killend.txt" key:Return sleep:800
gexact "Ctrl+K at end: no corruption" "$WORK/edge_killend.txt" "ab"

# --- Ctrl+W at beginning (no-op) ---
K "type:echo ab" key:Left key:Left key:Left key:Left key:Left key:Left key:Left key:Left key:Left key:Left sleep:200 ctrl:w key:End sleep:200 "type: > $WORK/edge_word.txt" key:Return sleep:800
gexact "Ctrl+W at beginning: no crash, no corruption" "$WORK/edge_word.txt" "ab"

# --- Ctrl+W deletes the previous word (positive check) ---
K "type:echo ab cd" ctrl:w sleep:200 "type:ef > $WORK/edge_word2.txt" key:Return sleep:800
gexact "Ctrl+W deletes previous word" "$WORK/edge_word2.txt" "ab ef"

# --- Left moves the cursor: insert in the middle ---
K "type:echo ab" key:Left sleep:200 "type:X" key:End sleep:200 "type: > $WORK/edge_mid.txt" key:Return sleep:800
gexact "Left then type inserts in the middle" "$WORK/edge_mid.txt" "aXb"

# --- Ctrl+U deletes to the start (positive check) ---
K "type:echo ab" ctrl:u sleep:200 "type:echo ku-ok > $WORK/edge_killstart.txt" key:Return sleep:800
gexact "Ctrl+U deletes to start" "$WORK/edge_killstart.txt" "ku-ok"

# --- Up/Down browsing, Down past the latest entry returns to a fresh line ---
K "type:echo edge-h1" key:Return sleep:600 "type:echo edge-h2" key:Return sleep:600
K key:Up sleep:300 key:Up sleep:300 key:Down sleep:300 key:Down sleep:300 "type:echo down-ok > $WORK/edge_down.txt" key:Return sleep:800
gexact "Down at latest history entry: fresh line, no crash" "$WORK/edge_down.txt" "down-ok"

if alive; then EPASS=$((EPASS+1)); echo "PASS[$TAG]: GUI alive after edge cases";
else EFAIL=$((EFAIL+1)); echo "FAIL[$TAG]: GUI died during edge cases"; fi

kill $GUI_PID 2>/dev/null; wait $GUI_PID 2>/dev/null
kill $XVFB_PID 2>/dev/null
echo "EDGE[$TAG]: $EPASS passed, $EFAIL failed"
[ "$EFAIL" -eq 0 ]
