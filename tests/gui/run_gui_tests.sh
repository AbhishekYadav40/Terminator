#!/bin/bash
# GUI tests: drive the real Terminator window under Xvfb with synthetic
# X11 key events and assert on side-effect files.
# Usage: run_gui_tests.sh <mode-tag> [extra terminator args...]
set -u
TAG="$1"; shift
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${TERMINATOR_BIN:-$REPO/terminator}"
KEYS="$REPO/tests/gui/keys-bin"
SHOT="$REPO/tests/gui/shot-bin"
# The harness cds into a work dir below, so KEYS/SHOT must be absolute
# paths (a relative path broke every key event: the sender was never found).
if [ ! -x "$KEYS" ]; then gcc -O2 -o "$KEYS" "$REPO/tests/gui/keys.c" -lX11 || exit 1; fi
if [ ! -x "$SHOT" ]; then gcc -O2 -o "$SHOT" "$REPO/tests/gui/shot.c" -lX11 || exit 1; fi
DISP=":$((RANDOM % 100 + 60))"
WORK="/tmp/guiwork_$TAG"
HOME_DIR="/tmp/guihome_$TAG"
rm -rf "$WORK" "$HOME_DIR"; mkdir -p "$WORK" "$HOME_DIR"
printf 'completion-content\n' > "$WORK/def.txt"
printf 'a1\n' > "$WORK/abc.txt"
printf 'a2\n' > "$WORK/abcd.txt"
cd "$WORK" || exit 1

Xvfb "$DISP" -screen 0 1024x768x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1
DISPLAY="$DISP" HOME="$HOME_DIR" "$BIN" "$@" >/tmp/gui_$TAG.log 2>&1 &
GUI_PID=$!
sleep 2

K() { DISPLAY="$DISP" "$KEYS" "$@" >/dev/null 2>&1; }
GPASS=0; GFAIL=0
gchk() { # gchk <name> <file> <expected-substring>
    if [ -f "$2" ] && grep -qF -- "$3" "$2"; then GPASS=$((GPASS+1)); echo "PASS[$TAG]: $1";
    else GFAIL=$((GFAIL+1)); echo "FAIL[$TAG]: $1 (file $2 missing or lacks '$3')"; [ -f "$2" ] && cat "$2"; fi
}
gchkn() { # file must NOT exist
    if [ -e "$2" ]; then GFAIL=$((GFAIL+1)); echo "FAIL[$TAG]: $1 ($2 should not exist)";
    else GPASS=$((GPASS+1)); echo "PASS[$TAG]: $1"; fi
}
alive() { kill -0 $GUI_PID 2>/dev/null && [ "$(cut -d' ' -f3 /proc/$GUI_PID/stat 2>/dev/null)" != "Z" ]; }

# 1. basic command execution in the GUI
K "type:echo gui-$TAG > out1.txt" key:Return sleep:900
gchk "typed command executes" out1.txt "gui-$TAG"

# 2. Ctrl+C interrupts a running sleep and returns the prompt
K "type:sleep 30" key:Return sleep:700 ctrl:c sleep:500 "type:echo after-int-$TAG > out2.txt" key:Return sleep:900
gchk "Ctrl+C returns prompt" out2.txt "after-int-$TAG"

# 3. Ctrl+Z / jobs / bg / fg / Ctrl+C
K "type:sleep 30" key:Return sleep:700 ctrl:z sleep:500 "type:jobs > jobs.txt" key:Return sleep:800
gchk "jobs shows Stopped job" jobs.txt "Stopped"
gchk "jobs shows the command" jobs.txt "sleep 30"
K "type:bg %1" key:Return sleep:500 "type:fg %1" key:Return sleep:600 ctrl:c sleep:500 "type:echo after-z-$TAG > out3.txt" key:Return sleep:900
gchk "bg/fg + Ctrl+C returns prompt" out3.txt "after-z-$TAG"

# 4. hotkey line editing: Ctrl+A + cursor insert, Ctrl+W, Ctrl+U
K "type:cho hk-a-$TAG > a.txt" key:Return sleep:200
# The line above was submitted (creates error output); now a fresh line:
K "type:cho hk-a-$TAG > a.txt" ctrl:a "type:e" key:Return sleep:800
gchk "Ctrl+A + insert at start (echo fixed)" a.txt "hk-a-$TAG"
K "type:echo BAD two > w.txt" ctrl:w "type:w2.txt" key:Return sleep:800
gchkn "Ctrl+W deleted last word (w.txt absent)" w.txt
gchk "Ctrl+W: remaining line ran" w2.txt "BAD two"
K "type:echo garbage-line" ctrl:u "type:echo hk-u-$TAG > u.txt" key:Return sleep:800
gchk "Ctrl+U cleared the line" u.txt "hk-u-$TAG"
# Ctrl+E / Ctrl+K: type partial, go start, go end, kill tail, retype
K "type:echo hk-e > e.txt" ctrl:a ctrl:e sleep:100 key:Return sleep:800
gchk "Ctrl+A/Ctrl+E harmless, line runs" e.txt "hk-e"

# 5. Tab completion (single match)
K "type:cat de" key:Tab sleep:300 "type: > comp.txt" key:Return sleep:800
gchk "Tab completion completed def.txt" comp.txt "completion-content"

# 6. second tab: cwd isolation + shared history
K "type:cd /tmp" key:Return sleep:600 "type:echo tab1-$TAG > /tmp/tab1_$TAG.txt" key:Return sleep:700
K ctrl:t sleep:600 "type:pwd > tab2pwd.txt" key:Return sleep:800 "type:history > tab2hist.txt" key:Return sleep:800
gchk "tab2 cwd is its own (not /tmp)" tab2pwd.txt "$WORK"
gchk "tab2 history shows tab1 commands (shared)" tab2hist.txt "cd /tmp"
gchk "tab1 command ran in /tmp" "/tmp/tab1_$TAG.txt" "tab1-$TAG"

# 7. Ctrl+R flow: run a unique command, search for it, GUI stays alive
K "type:echo r-target-$TAG > r1.txt" key:Return sleep:800
K ctrl:r sleep:400 "type:r-target-$TAG" key:Return sleep:600
DISPLAY="$DISP" "$SHOT" "/tmp/gui_$TAG.ppm" >/dev/null 2>&1
K "type:echo alive-after-r-$TAG > r2.txt" key:Return sleep:800
gchk "command after Ctrl+R search executes" r2.txt "alive-after-r-$TAG"

# 8. process-mode only: exit in tab2 closes just that tab
if [ "$TAG" = "process" ]; then
    K "type:exit" key:Return sleep:800
    if alive; then GPASS=$((GPASS+1)); echo "PASS[$TAG]: GUI alive after tab exit";
    else GFAIL=$((GFAIL+1)); echo "FAIL[$TAG]: GUI died after tab exit"; fi
    K ctrl:Tab sleep:300 "type:echo tab1-still-$TAG > /tmp/still_$TAG.txt" key:Return sleep:800
    gchk "other tab unaffected by exit" "/tmp/still_$TAG.txt" "tab1-still-$TAG"
fi

if alive; then GPASS=$((GPASS+1)); echo "PASS[$TAG]: GUI still alive at end";
else GFAIL=$((GFAIL+1)); echo "FAIL[$TAG]: GUI died"; fi

# 9. Up/Down history browsing
K "type:echo HIST-ONE > /tmp/h1_$TAG.txt" key:Return sleep:800 "type:echo HIST-TWO > /tmp/h2_$TAG.txt" key:Return sleep:800
K "type:rm /tmp/h1_$TAG.txt" key:Return sleep:600
K key:Up key:Up key:Up key:Return sleep:800
gchk "Up/Up/Up re-runs older command" "/tmp/h1_$TAG.txt" "HIST-ONE"
K "type:echo D1 > /tmp/d1_$TAG.txt" key:Return sleep:600 "type:echo D2 > /tmp/d2_$TAG.txt" key:Return sleep:600
K key:Up key:Down "type:echo down-ok > /tmp/dn_$TAG.txt" key:Return sleep:800
gchk "Down returns to a fresh line" "/tmp/dn_$TAG.txt" "down-ok"

# 10. completion with several matches: numbered list + pick by number
# (cd first: in process mode the current tab may be tab 1, whose cwd is
# /tmp after section 6; completion globs the tab's own directory.)
K "type:cd $WORK" key:Return sleep:600
K "type:cat ab" key:Tab sleep:700 "type:2" sleep:400 "type: > /tmp/pick2_$TAG.txt" key:Return sleep:1000
gchk "completion list pick 2 runs abcd.txt" "/tmp/pick2_$TAG.txt" "a2"

# 11. Ctrl+Shift+C / Ctrl+Shift+V copy/paste of the input line
K "type:echo copy-paste-ok > /tmp/cp_$TAG.txt" ctrl+shift:c sleep:300 ctrl:u sleep:200 ctrl+shift:v sleep:500 key:Return sleep:800
gchk "copy/paste round-trip runs the line" "/tmp/cp_$TAG.txt" "copy-paste-ok"

# 12. Ctrl+D (eof) closes the current tab when several exist
K ctrl:t sleep:600 "type:echo tab2-d-$TAG > /tmp/t2d_$TAG.txt" key:Return sleep:700
K ctrl:d sleep:600 "type:echo back1-d-$TAG > /tmp/b1d_$TAG.txt" key:Return sleep:800
gchk "Ctrl+D closed the new tab, other tab works" "/tmp/b1d_$TAG.txt" "back1-d-$TAG"

# 12b. Ctrl+D with text typed but not executed still closes the tab
K ctrl:t sleep:600 "type:cd" sleep:300 ctrl:d sleep:600 "type:echo eof-typed-$TAG > /tmp/eoft_$TAG.txt" key:Return sleep:800
gchk "Ctrl+D with typed text closes the tab" "/tmp/eoft_$TAG.txt" "eof-typed-$TAG"

# 13. multiWatch stopped by Ctrl+C in the window
K "type:multiWatch [\"echo mw-gui\"]" key:Return sleep:3500 ctrl:c sleep:500 "type:echo mw-after-$TAG > /tmp/mw_$TAG.txt" key:Return sleep:800
gchk "multiWatch Ctrl+C returns prompt" "/tmp/mw_$TAG.txt" "mw-after-$TAG"

# 15. heredoc typed in the window: multi-line entry with the > prompt,
#    then the body reaches the command; one-line paste errors cleanly.
K "type:cat > /tmp/hd_gui_$TAG.txt <<EOF" key:Return sleep:500 "type:gui heredoc line" key:Return sleep:300 "type:EOF" key:Return sleep:800
K "type:cat /tmp/hd_gui_$TAG.txt > /tmp/hdout_$TAG.txt" key:Return sleep:800
gchk "GUI heredoc body reaches the command" "/tmp/hdout_$TAG.txt" "gui heredoc line"
K "type:cat > /tmp/badhd.txt <<EOF body EOF" key:Return sleep:600 "type:echo still-alive-$TAG > /tmp/al_$TAG.txt" key:Return sleep:800
gchk "one-line heredoc paste does not hang the prompt" "/tmp/al_$TAG.txt" "still-alive-$TAG"

# 16. scrolling: output, scroll up by key and by wheel, typing snaps
#    back to the tail and the command still runs.
K "type:seq 1 60" key:Return sleep:1000
K shift:Prior sleep:400 "wheel:up" sleep:400 "wheel:down" sleep:400
K "type:echo scroll-snap-$TAG > /tmp/sc_$TAG.txt" key:Return sleep:800
gchk "typing after scrolling snaps back and runs" "/tmp/sc_$TAG.txt" "scroll-snap-$TAG"

# 17. mouse drag selection + Ctrl+Shift+C/V: select output with the
#    mouse, copy it, paste it after `echo `, run it. Rows after Ctrl+L:
#    0 = prompt + printf line, 1 = SELECTME-123, 2 = SECOND-LINE.
K ctrl:l sleep:400 "type:printf 'SELECTME-123\nSECOND-LINE\n'" key:Return sleep:900
K "drag:5,64,400,64" sleep:500 ctrl+shift:c sleep:400 "type:echo " ctrl+shift:v sleep:400 "type: > /tmp/sel_$TAG.txt" key:Return sleep:800
gchk "mouse selection copies one output line" "/tmp/sel_$TAG.txt" "SELECTME-123"
# A two-line selection pastes as two lines: each runs when submitted.
# (Absolute paths: the tab's cwd here is /tmp from the earlier cd test,
# so relative targets would land outside the work dir.)
K ctrl:l sleep:400 "type:printf 'echo one > /tmp/s2a_$TAG.txt\necho two > /tmp/s2b_$TAG.txt\n'" key:Return sleep:900
K "drag:5,64,400,82" sleep:500 ctrl+shift:c sleep:400 ctrl+shift:v sleep:400 key:Return sleep:900
gchk "two-line paste runs the first line" "/tmp/s2a_$TAG.txt" "one"
gchk "two-line paste runs the second line" "/tmp/s2b_$TAG.txt" "two"

# 18. Ctrl+R: the search term is typed after the "Enter search term: "
#    label (the label is the live prompt, not a stale line in the
#    buffer), Return prints the match, Escape restores the typed line.
K "type:echo search-seed-$TAG" key:Return sleep:600
K "type:echo restore-search-$TAG > /tmp/rs_$TAG.txt" sleep:300 ctrl:r sleep:300 "type:search-seed" key:Return sleep:600
K "type:echo search-done-$TAG > /tmp/sd_$TAG.txt" key:Return sleep:800
gchk "Ctrl+R search returns to a working prompt" "/tmp/sd_$TAG.txt" "search-done-$TAG"
K "type:echo restore2-$TAG > /tmp/rs2_$TAG.txt" sleep:300 ctrl:r sleep:300 "type:zzz" key:Escape sleep:300 key:Return sleep:800
gchk "Ctrl+R Escape restores the typed line" "/tmp/rs2_$TAG.txt" "restore2-$TAG"

# 19. Pasting several lines: newlines stay newlines, so one Enter runs
#    each line (background jobs + jobs), and a paste ending in a
#    newline submits at once.
K ctrl:l sleep:400 "type:printf 'sleep 30 &\nsleep 40 &\nsleep 50 &\njobs\n'" key:Return sleep:900
K "drag:5,64,400,118" sleep:500 ctrl+shift:c sleep:400 ctrl+shift:v sleep:400 key:Return sleep:1200
K "type:jobs > /tmp/jobs19_$TAG.txt" key:Return sleep:800
if [ "$(grep -c Running /tmp/jobs19_$TAG.txt 2>/dev/null)" = 3 ]; then GPASS=$((GPASS+1)); echo "PASS[$TAG]: pasted lines all run as background jobs";
else GFAIL=$((GFAIL+1)); echo "FAIL[$TAG]: pasted jobs file: $(cat /tmp/jobs19_$TAG.txt 2>/dev/null)"; fi
# Trailing-newline paste: dragging to the start of the next line
# includes the newline, so the paste submits without pressing Return.
K ctrl:l sleep:400 "type:printf 'echo paste-auto-$TAG > /tmp/pa_$TAG.txt\n'" key:Return sleep:900
K "drag:5,64,5,82" sleep:500 ctrl+shift:c sleep:400 ctrl+shift:v sleep:900
gchk "paste ending in newline submits at once" "/tmp/pa_$TAG.txt" "paste-auto-$TAG"
K "type:pkill -x sleep" key:Return sleep:600

# 14. Ctrl+D on the last tab quits Terminator
for _ in 1 2 3; do K ctrl:d sleep:600; alive || break; done
if alive; then GFAIL=$((GFAIL+1)); echo "FAIL[$TAG]: Ctrl+D did not quit";
else GPASS=$((GPASS+1)); echo "PASS[$TAG]: Ctrl+D quits on last tab"; fi

kill $GUI_PID 2>/dev/null; wait $GUI_PID 2>/dev/null
kill $XVFB_PID 2>/dev/null
echo "GUI[$TAG]: $GPASS passed, $GFAIL failed"
[ "$GFAIL" -eq 0 ]
