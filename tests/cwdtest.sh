#!/bin/sh
# cwdtest.sh — [ ] tests and arithmetic must resolve against the tab's
# cwd after cd, not the process's launch directory.
cd subdir
if [ -f inner.txt ]; then
  echo inner-ok
fi
if [ -d innerdir ]; then
  echo dir-ok
fi
X=$((2+3))
echo arith=$X
