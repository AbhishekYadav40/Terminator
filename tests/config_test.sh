#!/bin/bash
# config_test.sh — Task 7: ~/.terminatorrc warnings, custom bindings and
# fallback to defaults. Drives the real window under Xvfb.
# Usage: bash tests/config_test.sh [path-to-terminator-binary]
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$REPO/terminator}"
if [ ! -x "$BIN" ]; then echo "binary not found: $BIN (run make first)"; exit 1; fi
KEYS="$REPO/tests/gui/keys-bin"
if [ ! -x "$KEYS" ]; then gcc -O2 -o "$KEYS" "$REPO/tests/gui/keys.c" -lX11 || exit 1; fi

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "PASS: $1"; }
bad()  { FAIL=$((FAIL+1)); echo "FAIL: $1"; }
DISP=":$((RANDOM % 100 + 60))"
rm -f "/tmp/.X${DISP#:}-lock"

start_gui() { # start_gui <home> <work> [extra args...]
    local home="$1" work="$2"; shift 2
    rm -rf "$work"; mkdir -p "$work" "$home"
    Xvfb "$DISP" -screen 0 1024x768x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 1
    (cd "$work" && DISPLAY="$DISP" HOME="$home" "$BIN" "$@" >/tmp/cfg_gui.log 2>/tmp/cfg_gui.err & echo $! > /tmp/cfg_gui.pid)
    sleep 2
    GUI_PID=$(cat /tmp/cfg_gui.pid)
}
stop_gui() {
    kill "$GUI_PID" 2>/dev/null; wait "$GUI_PID" 2>/dev/null
    kill "$XVFB_PID" 2>/dev/null
}
K() { DISPLAY="$DISP" "$KEYS" "$@" >/dev/null 2>&1; }

# --- Phase A: malformed / unknown / invalid / conflicting lines are reported ---
HA="$(mktemp -d)/home"; mkdir -p "$HA"
cat > "$HA/.terminatorrc" <<'EOF'
this line is malformed
bind Ctrl+Q frobnicate
clear=Ctrl+ZZZ
bind Ctrl+L clear-screen
bind Ctrl+L history-search
EOF
start_gui "$HA" "$(mktemp -d)"
if kill -0 "$GUI_PID" 2>/dev/null; then ok "GUI starts with a bad config file"; else bad "GUI starts with a bad config file"; fi
stop_gui
for msg in "ignoring malformed line" "unknown action 'frobnicate'" "invalid hotkey 'Ctrl+ZZZ' for clear" "conflicts with clear"; do
    if grep -qF -- "$msg" /tmp/cfg_gui.err; then ok "warning reported: $msg"; else bad "warning reported: $msg"; cat /tmp/cfg_gui.err; fi
done

# --- Phase B: a custom binding works, and still works after a restart ---
HB="$(mktemp -d)/home"; mkdir -p "$HB"
printf 'bind Ctrl+G new-tab\n' > "$HB/.terminatorrc"
for round in 1 2; do
    W="$(mktemp -d)"
    start_gui "$HB" "$W"
    K "type:cd /tmp" key:Return sleep:600 ctrl:g sleep:600 "type:pwd > cfg2.txt" key:Return sleep:800
    stop_gui
    if [ "$(cat "$W/cfg2.txt" 2>/dev/null)" = "$W" ]; then ok "custom Ctrl+G opens a new tab (run $round)";
    else bad "custom Ctrl+G opens a new tab (run $round): $(cat "$W/cfg2.txt" 2>/dev/null)"; fi
    rm -rf "$W"
done

# --- Phase C: an invalid binding falls back to the default ---
HC="$(mktemp -d)/home"; mkdir -p "$HC"
printf 'new-tab=Ctrl+ZZZ\n' > "$HC/.terminatorrc"
W="$(mktemp -d)"
start_gui "$HC" "$W"
K "type:cd /tmp" key:Return sleep:600 ctrl:t sleep:600 "type:pwd > cfg3.txt" key:Return sleep:800
stop_gui
if [ "$(cat "$W/cfg3.txt" 2>/dev/null)" = "$W" ]; then ok "invalid binding falls back to default Ctrl+T";
else bad "invalid binding falls back to default Ctrl+T: $(cat "$W/cfg3.txt" 2>/dev/null)"; fi
if grep -qF "invalid hotkey 'Ctrl+ZZZ' for new_tab" /tmp/cfg_gui.err; then ok "invalid new-tab binding reported";
else bad "invalid new-tab binding reported"; fi
rm -rf "$W"

pkill -x Xvfb 2>/dev/null
echo
echo "config_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
