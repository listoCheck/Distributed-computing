#!/bin/sh
set -eu
compiler=${1:-clang-3.5}
archive=${2:-/lab3/pa3.tar.gz}
workspace=$(mktemp -d /tmp/pa3-archive-XXXXXX)
tar -xzf "$archive" -C "$workspace"
cd "$workspace/pa3"
"$compiler" -std=c99 -Wall -pedantic *.c -L. -lruntime 2>build.stderr
test ! -s build.stderr
LD_LIBRARY_PATH=. LD_PRELOAD=./libruntime.so timeout 20 ./a.out -p 3 10 20 30 >run.stdout 2>run.stderr
test ! -s run.stderr
python3 -c 'import re; text=open("run.stdout").read(); row=next(line for line in text.splitlines() if "Total |" in line); values=[int(n) for n in re.findall(r"\d+",row)]; assert values and all(n==60 for n in values); print("PASS: extracted archive, PDF compiler flags, exit 0, empty stderr, conserved total")'
