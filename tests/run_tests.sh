#!/bin/bash
# run_tests.sh — automated checks for Terminator, driven through the
# headless --batch mode (same shell engine as the GUI).
# Usage:  bash tests/run_tests.sh [path-to-terminator-binary]
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$REPO/terminator}"
if [ ! -x "$BIN" ]; then echo "binary not found: $BIN (run make first)"; exit 1; fi

WORK="$(mktemp -d)"
export HOME="$WORK/home"; mkdir -p "$HOME"
cd "$WORK" || exit 1
cp "$REPO/tests/check.sh" "$REPO/tests/demo.sh" "$REPO/tests/bad.sh" "$REPO/tests/script2.sh" "$REPO/tests/cwdtest.sh" "$REPO/tests/hdscript.sh" .
chmod +x check.sh demo.sh bad.sh script2.sh cwdtest.sh hdscript.sh
printf 'int main(void){return 0;}\n' > prog.c
printf 'banana\napple\ncherry\n' > fruits.txt
seq 1 5000 > big.txt
mkdir -p subdir/innerdir; touch subdir/inner.txt

PASS=0; FAIL=0
chk() { # chk <name> <expected-substring> <output>
    if printf '%s' "$3" | grep -qF -- "$2"; then PASS=$((PASS+1)); echo "PASS: $1";
    else FAIL=$((FAIL+1)); echo "FAIL: $1 (expected to see: $2)"; echo "--- output was:"; printf '%s\n' "$3" | head -20; fi
}
chkn() { # chk-not <name> <forbidden-substring> <output>
    if printf '%s' "$3" | grep -qF -- "$2"; then FAIL=$((FAIL+1)); echo "FAIL: $1 (should NOT see: $2)";
    else PASS=$((PASS+1)); echo "PASS: $1"; fi
}
run() { printf '%s\n' "$@" | timeout 15 "$BIN" --batch 2>&1; }

# --- quoting & expansion (Task 3) ---
OUT=$(run 'echo "double $HOME" '"'"'single $HOME'"'"'')
chk "double quotes expand, single literal" "double $HOME single \$HOME" "$OUT"
OUT=$(run 'echo "nested: $(basename $(pwd))"')
chk "nested command substitution" "nested: $(basename "$WORK")" "$OUT"
OUT=$(run 'echo "bt: `echo inner` end"')
chk "backtick substitution" "bt: inner end" "$OUT"
OUT=$(run 'echo "$(echo a $(echo b))"')
chk "deeply nested substitution" "a b" "$OUT"
OUT=$(run 'echo "one \
two"')
chk "backslash continuation" "one two" "$OUT"
OUT=$(printf 'echo "alpha\ngamma"\n' | timeout 10 "$BIN" --batch 2>&1)
chk "multi-line double quote" "gamma" "$OUT"

# --- pipes & redirection (Tasks 4-5) ---
OUT=$(run 'printf "x\ny\n" | wc -l')
chk "two-stage pipeline" "2" "$OUT"
OUT=$(run 'cat fruits.txt | sort | head -1')
chk "three-stage pipeline" "apple" "$OUT"
OUT=$(run 'sort < fruits.txt > sorted.txt' 'cat sorted.txt')
chk "redirection in+out" "cherry" "$OUT"
OUT=$(run 'cat < no_such_file.txt' 'echo st=$?')
chk "missing input file reported" "No such file" "$OUT"
chkn "missing input: command not run" "st=0" "$OUT"

# --- exit status & errors (Task 2) ---
OUT=$(run 'notacommand' 'echo st=$?')
chk "command not found message" "terminator: notacommand: command not found" "$OUT"
chk "exit status 127" "st=127" "$OUT"
OUT=$(run 'false' 'echo st=$?' 'true' 'echo st2=$?')
chk "status 1 after false" "st=1" "$OUT"
chk "status 0 after true" "st2=0" "$OUT"

# --- variables ---
OUT=$(run 'MYVAR=hello' 'echo "v=$MYVAR ${MYVAR}x"')
chk "variable assignment + expansion" "v=hello hellox" "$OUT"

# --- background jobs (Task 8) ---
OUT=$(run 'sleep 30 &' 'jobs')
chk "background job listed" "Running" "$OUT"

# --- history (Task 9) ---
OUT=$(run 'echo hist-one' 'echo hist-two' 'history')
chk "history lists commands" "hist-two" "$OUT"
if [ -f "$HOME/.terminator_history" ] && grep -q "hist-one" "$HOME/.terminator_history"; then
    PASS=$((PASS+1)); echo "PASS: history persisted to file"
else FAIL=$((FAIL+1)); echo "FAIL: history persisted to file"; fi

# --- scripts (Task 11) ---
OUT=$(run './check.sh prog.c')
chk "check.sh existing file branch" "File exists, compiling..." "$OUT"
OUT=$(run './check.sh nothere.c')
chk "check.sh missing file branch" "Source file missing!" "$OUT"
OUT=$(run 'bash check.sh prog.c')
chk "bash script.sh form" "File exists, compiling..." "$OUT"
OUT=$(run 'source check.sh nothere.c')
chk "source script.sh form" "Source file missing!" "$OUT"
OUT=$(run './demo.sh prog.c' 'echo demo-status=$?')
for tok in hello argc=1 eq-ok ne-ok gt-ok ge-ok lt-ok le-ok seq-ok sne-ok e-ok d-ok z-ok n-ok elif-ok nested-ok; do
    chk "demo.sh: $tok" "$tok" "$OUT"
done
chk "demo.sh exit 7 propagates" "demo-status=7" "$OUT"
OUT=$(run './bad.sh')
chk "missing fi reported" "Syntax error" "$OUT"
chkn "missing fi: script did not run" "SHOULD-NOT-RUN" "$OUT"

# --- built-in output redirection ---
OUT=$(run 'echo cap-base' 'history > caph.txt' 'cat caph.txt')
chk "history > file redirects built-in output" "cap-base" "$OUT"
OUT=$(run 'pwd > capp.txt' 'cat capp.txt')
chk "pwd > file" "$WORK" "$OUT"

# --- multiWatch (Task 6): format, re-runs, SIGINT survival ---
MWOUT=$( { printf 'multiWatch ["date +%%T", "echo tick"]\n'; sleep 6.5; printf 'echo after-int\n'; sleep 0.5; } | timeout -s INT 5 "$BIN" --batch 2>&1 )
TICKS=$(printf '%s' "$MWOUT" | grep -c '"echo tick", [0-9]*:')
if [ "$TICKS" -ge 3 ]; then PASS=$((PASS+1)); echo "PASS: multiWatch re-runs every 2s ($TICKS rounds)"
else FAIL=$((FAIL+1)); echo "FAIL: multiWatch re-runs (only $TICKS rounds)"; fi
HYPH=$(printf '%s' "$MWOUT" | grep -c '^-\{52\}$')
if [ "$HYPH" -ge 6 ]; then PASS=$((PASS+1)); echo "PASS: multiWatch 52-hyphen separators ($HYPH lines)"
else FAIL=$((FAIL+1)); echo "FAIL: multiWatch hyphen lines ($HYPH)"; fi
chk "multiWatch: SIGINT stops watch, shell survives" "after-int" "$MWOUT"

# --- multiWatch: ; && || lists inside one watched command ---
MWOUT2=$( { printf 'multiWatch ["echo mw-seq-1; echo mw-seq-2", "false && echo mw-nope; echo mw-done"]\n'; sleep 4.5; } | timeout -s INT 4 "$BIN" --batch 2>&1 )
chk "multiWatch: ; runs first part" "mw-seq-1" "$MWOUT2"
chk "multiWatch: ; runs second part" "mw-seq-2" "$MWOUT2"
# The header line itself prints the whole watched string, so the
# short-circuit check must match an output line exactly, not a substring.
if printf '%s' "$MWOUT2" | grep -q "^mw-nope$"; then FAIL=$((FAIL+1)); echo "FAIL: multiWatch: && short-circuits (mw-nope was printed)";
else PASS=$((PASS+1)); echo "PASS: multiWatch: && short-circuits"; fi
chk "multiWatch: ; after && runs" "mw-done" "$MWOUT2"
HDRS2=$(printf '%s' "$MWOUT2" | grep -c '"echo mw-seq-1; echo mw-seq-2", [0-9]*:')
if [ "$HDRS2" -ge 2 ]; then PASS=$((PASS+1)); echo "PASS: multiWatch list command repeats per round ($HDRS2 rounds)"
else FAIL=$((FAIL+1)); echo "FAIL: multiWatch list rounds (only $HDRS2 headers)"; fi
L1=$(printf '%s' "$MWOUT2" | grep -n "^mw-seq-1$" | head -1 | cut -d: -f1)
L2=$(printf '%s' "$MWOUT2" | grep -n "^mw-seq-2$" | head -1 | cut -d: -f1)
if [ -n "$L1" ] && [ -n "$L2" ] && [ "$L1" -lt "$L2" ]; then PASS=$((PASS+1)); echo "PASS: multiWatch list parts run in order";
else FAIL=$((FAIL+1)); echo "FAIL: multiWatch list order ($L1 vs $L2)"; fi

# --- scripts with pipes/redirection/substitution (Task 11) ---
OUT=$(run './script2.sh' 'tr -d " " < subout.txt' 'head -1 ssorted.txt')
chk "script: substitution with pipe" "count=3" "$OUT"
chk "script: pipe + redirection" "apple" "$OUT"
chk "script: variable + \$? after pipeline" "status=done 0" "$OUT"
OUT=$(run 'bash script2.sh' 'tr -d " " < subout.txt')
chk "bash script2.sh form" "count=3" "$OUT"

# --- pipes combined with redirection (Tasks 4-5) ---
OUT=$(run 'cat fruits.txt | sort > psorted.txt' 'head -1 psorted.txt')
chk "pipe into redirected file" "apple" "$OUT"
OUT=$(run 'sort < fruits.txt | head -1')
chk "redirected input through pipe" "apple" "$OUT"
OUT=$(run 'cat fruits.txt | sort | head -2 > phead.txt' 'cat phead.txt')
chk "pipeline with output redirection" "banana" "$OUT"

# --- cd / exit (Task 2) ---
run 'cd /tmp' 'pwd > cd1.txt' "cd $WORK" 'pwd > cd2.txt' >/dev/null
if [ "$(cat /tmp/cd1.txt)" = "/tmp" ]; then PASS=$((PASS+1)); echo "PASS: cd changes directory";
else FAIL=$((FAIL+1)); echo "FAIL: cd1.txt: $(cat /tmp/cd1.txt 2>/dev/null)"; fi
rm -f /tmp/cd1.txt
if [ "$(cat cd2.txt)" = "$WORK" ]; then PASS=$((PASS+1)); echo "PASS: cd back";
else FAIL=$((FAIL+1)); echo "FAIL: cd2.txt: $(cat cd2.txt)"; fi
OUT=$(run 'echo before-exit' 'exit' 'echo after-exit')
chk "exit runs preceding command" "before-exit" "$OUT"
chkn "exit stops the shell" "after-exit" "$OUT"

# --- concurrent background readers (Task 4) ---
OUT=$(run 'sort big.txt > s1.txt &' 'sort big.txt > s2.txt &' 'sleep 2' 'cmp s1.txt s2.txt' 'wc -l < s1.txt')
chkn "concurrent readers agree" "differ" "$OUT"
chk "concurrent readers complete" "5000" "$OUT"

# --- history limits (Task 9): last 10,000 in file, history shows 1,000 ---
# Pre-seed the history file past the cap (10,040 lines) instead of typing
# 10,050 commands: loading caps memory at 10,000, then new commands shift
# old ones out and the file must be rewritten to exactly 10,000 lines.
HW2="$(mktemp -d)"; HW2HOME="$HW2/home"; mkdir -p "$HW2HOME"
for i in $(seq 1 10040); do printf 'echo cap-%s\n' "$i"; done > "$HW2HOME/.terminator_history"
{ for i in $(seq 1 20); do printf 'echo post-%s\n' "$i"; done; } | HOME="$HW2HOME" timeout 30 "$BIN" --batch >/dev/null 2>&1
if [ "$(wc -l < "$HW2HOME/.terminator_history")" -eq 10000 ]; then PASS=$((PASS+1)); echo "PASS: history file holds last 10000";
else FAIL=$((FAIL+1)); echo "FAIL: history file line count: $(wc -l < "$HW2HOME/.terminator_history")"; fi
if [ "$(head -1 "$HW2HOME/.terminator_history")" = "echo cap-61" ]; then PASS=$((PASS+1)); echo "PASS: history file drops oldest entries";
else FAIL=$((FAIL+1)); echo "FAIL: history file first line: $(head -1 "$HW2HOME/.terminator_history")"; fi
if [ "$(tail -1 "$HW2HOME/.terminator_history")" = "echo post-20" ]; then PASS=$((PASS+1)); echo "PASS: history file keeps newest entry";
else FAIL=$((FAIL+1)); echo "FAIL: history file last line: $(tail -1 "$HW2HOME/.terminator_history")"; fi
printf 'history > hist1000.txt\n' | HOME="$HW2HOME" timeout 30 "$BIN" --batch >/dev/null 2>&1
if [ "$(wc -l < hist1000.txt)" -eq 1000 ]; then PASS=$((PASS+1)); echo "PASS: history shows most recent 1000";
else FAIL=$((FAIL+1)); echo "FAIL: history line count: $(wc -l < hist1000.txt)"; fi
if grep -q "echo post-20" hist1000.txt && ! grep -q "cap-9000" hist1000.txt; then PASS=$((PASS+1)); echo "PASS: history window is the most recent 1000";
else FAIL=$((FAIL+1)); echo "FAIL: history window contents"; fi
rm -rf "$HW2"

# --- heredocs (shell fidelity) ---
OUT=$(run 'cat > hd1.txt <<EOF' 'hello' 'world' 'EOF' 'cat hd1.txt')
chk "heredoc writes body lines" "world" "$OUT"
OUT=$(run 'MYNAME=World' "cat <<'EOF'" 'hello $MYNAME' 'EOF')
chk "quoted delimiter: body stays literal" 'hello $MYNAME' "$OUT"
OUT=$(run 'MYNAME=World' 'cat <<EOF' 'hello $MYNAME' 'EOF')
chk "unquoted delimiter: body expands" "hello World" "$OUT"
OUT=$(run 'cat <<EOF | wc -l' 'one' 'two' 'three' 'EOF')
chk "heredoc through pipe" "3" "$OUT"
OUT=$(run 'cat <<EOF' 'a < b > c' 'line one \' 'line two' 'EOF')
chk "heredoc body keeps < > and backslash" 'a < b > c' "$OUT"
chk "heredoc body keeps trailing backslash" 'line one \' "$OUT"
OUT=$(run "cat > hello.c <<'EOF' #include <stdio.h> int main() { return 0; } EOF" 'echo st=$?')
chk "one-line heredoc paste is a clear error" "heredoc has no body" "$OUT"
OUT=$(printf 'cat <<EOF\nnever closed\n' | timeout 10 "$BIN" --batch 2>&1)
chk "unterminated heredoc ends cleanly" "incomplete input" "$OUT"
OUT=$(run 'cat > hdb.txt <<EOF' 'secret-body' 'EOF' 'history' 'cat hdb.txt')
chk "heredoc body reaches the command" "secret-body" "$OUT"
OUT=$(run 'cat > hdb2.txt <<EOF' 'secret-body-2' 'EOF' 'history')
chkn "heredoc body not recorded in history" "secret-body-2" "$OUT"
OUT=$(run 'echo "\$"' 'echo "\\"' 'echo "\""' 'echo "Hello \$HOME World"')
chk "escape: quoted backslash-dollar" 'Hello $HOME World' "$OUT"
if [ "$(run 'echo "\$"')" = '$' ] && [ "$(run 'echo "\\"')" = '\' ] && [ "$(run 'echo "\""')" = '"' ]; then
    PASS=$((PASS+1)); echo "PASS: escape: single chars \$ \\ \""
else FAIL=$((FAIL+1)); echo "FAIL: escape: single chars"; fi
OUT=$(run './cwdtest.sh')
chk "script [ ] tests resolve in tab cwd" "inner-ok" "$OUT"
chk "script [ -d ] resolves in tab cwd" "dir-ok" "$OUT"
chk "script arithmetic expansion" "arith=5" "$OUT"
OUT=$(run './hdscript.sh')
chk "script heredoc body ignores if/fi lines" "hd-file-ok" "$OUT"

# --- command lists: && || ; & (shell fidelity) ---
OUT=$(run 'true && echo and-yes' 'false && echo and-no' 'false || echo or-yes' 'true || echo or-no')
chk "&& runs on success" "and-yes" "$OUT"
chkn "&& skips on failure" "and-no" "$OUT"
chk "|| runs on failure" "or-yes" "$OUT"
chkn "|| skips on success" "or-no" "$OUT"
OUT=$(run 'echo one; echo two' 'false; echo after-semi')
chk "; runs both commands" "two" "$OUT"
chk "; runs after failure" "after-semi" "$OUT"
OUT=$(run 'cd /nonexistent && echo MARKER-NOPE' 'echo st=$?' 'cd /tmp && pwd')
chkn "&& short-circuits cd failure" "MARKER-NOPE" "$OUT"
chk "cd && pwd" "/tmp" "$OUT"
OUT=$(run 'X=1 && echo "x=$X"')
chk "expansion per segment sees new value" "x=1" "$OUT"
OUT=$(run 'false || false || echo third' 'true && false || echo chain')
chk "|| chain reaches third" "third" "$OUT"
chk "&& || chain" "chain" "$OUT"
OUT=$(run 'echo bg1 & echo bg2')
chk "& separator runs first" "bg1" "$OUT"
chk "& separator runs second" "bg2" "$OUT"
OUT=$(run 'echo $(false || echo inner-ok)')
chk "|| inside substitution" "inner-ok" "$OUT"
OUT=$(run 'echo $(echo a; echo b)')
chk "; inside substitution" "a" "$OUT"
chk "; inside substitution second" "b" "$OUT"

# --- redirections: fd forms, order, redirect-only ---
OUT=$(run 'ls missing 2>e1.txt' 'cat e1.txt')
chk "2> captures stderr" "No such file" "$OUT"
OUT=$(run 'echo both &>b1.txt' 'cat b1.txt')
chk "&> captures stdout" "both" "$OUT"
OUT=$(run 'ls missing > o1.txt 2>&1' 'cat o1.txt')
chk "> then 2>&1 sends error to file" "No such file" "$OUT"
OUT=$(run 'ls missing 2>&1 > o2.txt' 'wc -c < o2.txt')
chk "2>&1 then > leaves file empty" "0" "$OUT"
OUT=$(run 'echo three > f2.txt > f3.txt' 'wc -c < f2.txt' 'cat f3.txt')
chk "two > creates both files, last wins" "three" "$OUT"
chk "two > leaves first file empty" "0" "$OUT"
OUT=$(run '> ro.txt' 'wc -c < ro.txt' 'echo st=$?')
chk "redirect-only creates empty file" "st=0" "$OUT"
OUT=$(run 'cat < missing2.txt' 'echo st=$?')
chk "failed input redirect is status 1" "st=1" "$OUT"
OUT=$(run 'ls missing |& cat')
chk "|& pipes stderr" "No such file" "$OUT"
OUT=$(run 'echo hi |& cat')
chk "|& pipes stdout too" "hi" "$OUT"

# --- tilde, arithmetic, comments, prefix assignments, cd - ---
OUT=$(run 'echo ~' 'echo ~/sub')
chk "tilde expands to HOME" "$HOME" "$OUT"
chk "~/ expands" "$HOME/sub" "$OUT"
OUT=$(run 'echo "s1=$((1+2))"' 'echo "s2=$((2*3+4))"' 'echo "s3=$((10/3))"' 'echo "s4=$((10%3))"' 'echo "s5=$((2*(3+4)))"' 'N=5' 'echo "s6=$((N*2))"' 'echo "s7=$((0x10))"')
chk "arithmetic add" "s1=3" "$OUT"
chk "arithmetic precedence" "s2=10" "$OUT"
chk "arithmetic division" "s3=3" "$OUT"
chk "arithmetic modulo" "s4=1" "$OUT"
chk "arithmetic parens" "s5=14" "$OUT"
chk "arithmetic variable" "s6=10" "$OUT"
chk "arithmetic hex" "s7=16" "$OUT"
OUT=$(run 'echo hi # comment' 'echo a#b' 'echo "a#b"')
chk "comment stripped" "hi" "$OUT"
chkn "comment text gone" "comment" "$OUT"
chk "hash inside word is not a comment" "a#b" "$OUT"
OUT=$(run 'FOO=bar printenv FOO')
chk "prefix assignment for command" "bar" "$OUT"
OUT=$(run 'FOO=bar true' 'echo "FOO=[$FOO]"')
chk "prefix assignment does not persist" "FOO=[]" "$OUT"
# --- ${VAR-word} parameter expansion ---
OUT=$(run 'echo "d1=${NOPE-default}"' 'echo "d2=${NOPE:-default}"' 'EMPTY=' 'echo "d3=${EMPTY-default}"' 'echo "d4=${EMPTY:-default}"' 'SET=val' 'echo "d5=${SET-default}"' 'echo "d6=${SET:+alt}"' 'echo "d7=${EMPTY:+alt}"' 'echo "d8=${NEW:=assigned}"' 'echo "d9=$NEW"' 'INNER=deep' 'echo "dA=${OUTER-${INNER}}"')
chk "default when unset" "d1=default" "$OUT"
chk "colon-default when unset" "d2=default" "$OUT"
chk "empty counts as set for -" "d3=" "$OUT"
chkn "empty does not take - default" "d3=default" "$OUT"
chk "colon-default when empty" "d4=default" "$OUT"
chk "set var ignores default" "d5=val" "$OUT"
chk "alternate when set" "d6=alt" "$OUT"
chkn "alternate empty when var empty" "d7=alt" "$OUT"
chk "assign-default expands" "d8=assigned" "$OUT"
chk "assign-default persists" "d9=assigned" "$OUT"
chk "nested default expansion" "dA=deep" "$OUT"
OUT=$(run 'export MYEXP1=abc' 'export')
chk "export with no args lists declare -x" 'declare -x MYEXP1="abc"' "$OUT"
run 'mkdir -p cdsub' 'cd cdsub' 'cd -' 'pwd > cddash.txt' >/dev/null
if [ "$(cat cddash.txt)" = "$WORK" ]; then PASS=$((PASS+1)); echo "PASS: cd - returns to previous directory";
else FAIL=$((FAIL+1)); echo "FAIL: cd - result: $(cat cddash.txt)"; fi
OUT=$(run 'echo "f*.txt"' "echo 'f*.txt'" 'echo \*.txt')
chkn "quoted glob is not expanded" "fruits.txt" "$OUT"
run 'mkdir -p globdir' 'cd globdir' 'touch onlyhere.txt' >/dev/null
OUT=$(run 'cd globdir' 'echo *.txt')
chk "glob uses tab cwd" "onlyhere.txt" "$OUT"
chkn "glob does not leak parent dir" "fruits.txt" "$OUT"

echo
echo "run_tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
