#!/bin/sh
# hdscript.sh — a heredoc body containing lines that read "if" and "fi"
# must not disturb the if/fi nesting check, and must reach the command.
cat > hdout.txt <<EOF
if
fi
EOF
cat hdout.txt
if [ -f hdout.txt ]; then
  echo hd-file-ok
fi
