#!/bin/bash
# script2.sh — Task 11: pipes, redirection and command substitution
# inside a script. Run as: ./script2.sh (from a directory with fruits.txt)
# A comment line; this one too.
COUNT=$(cat fruits.txt | wc -l)
echo "count=$COUNT" > subout.txt
cat fruits.txt | sort > ssorted.txt
X=done
echo "status=$X $?"
