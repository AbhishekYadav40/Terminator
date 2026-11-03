#!/bin/bash
# demo.sh — a tour of the Terminator script interpreter (Task 11).
# Run it as:  ./demo.sh somefile.c
# It exercises variables, $1/$#/$?, nested if/elif/else/fi and every
# [ ] test operator. It ends with `exit 7` on purpose.
NAME=world
echo "hello $NAME"
echo "argc=$# arg1=$1"
X=7
if [ $X -eq 7 ]; then
echo eq-ok
fi
if [ $X -ne 7 ]; then
echo ne-bad
else
echo ne-ok
fi
if [ $X -gt 5 ]; then
echo gt-ok
fi
if [ $X -ge 7 ]; then
echo ge-ok
fi
if [ $X -lt 9 ]; then
echo lt-ok
fi
if [ $X -le 7 ]; then
echo le-ok
fi
if [ "$NAME" = "world" ]; then
echo seq-ok
fi
if [ "$NAME" != "mars" ]; then
echo sne-ok
fi
if [ -e "$1" ]; then
echo e-ok
fi
if [ -d /tmp ]; then
echo d-ok
fi
EMPTY=
if [ -z "$EMPTY" ]; then
echo z-ok
fi
if [ -n "$NAME" ]; then
echo n-ok
fi
if [ $X -gt 10 ]; then
echo outer-bad
elif [ $X -eq 7 ]; then
echo elif-ok
if [ -f "$1" ]; then
echo nested-ok
fi
else
echo outer-else-bad
fi
echo "status-before-exit=$?"
exit 7
