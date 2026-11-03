#!/bin/bash
# The example script from the project specification (Task 11).
if [ -f "$1" ]; then
    echo "File exists, compiling..."
    gcc "$1" -o myprog
else
    echo "Source file missing!"
fi
