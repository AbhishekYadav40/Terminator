#!/bin/bash
# bad.sh — deliberately broken: the `fi` is missing. Terminator must
# report the syntax error WITHOUT running any of the script.
echo "SHOULD-NOT-RUN"
if [ -f prog.c ]; then
echo "inside the if"
