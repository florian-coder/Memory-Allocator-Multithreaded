#!/bin/sh
# Run real programs with the allocator interposed over libc's malloc, and
# compare what they print and what they return with a normal run.
set -u

LIB=./libosmem_preload.so
PROBE=./tests/preload_probe
fail=0
ran=0

run_both() {
	desc="$1"
	shift
	out_plain=$("$@" 2>/dev/null); rc_plain=$?
	out_ours=$(LD_PRELOAD=$LIB "$@" 2>/dev/null); rc_ours=$?
	ran=$((ran + 1))
	if [ "$rc_plain" != "$rc_ours" ]; then
		echo "  FAIL $desc: exit code $rc_plain without, $rc_ours with"
		fail=$((fail + 1))
	elif [ "$out_plain" != "$out_ours" ]; then
		echo "  FAIL $desc: output differs"
		fail=$((fail + 1))
	else
		echo "  ok   $desc (exit $rc_ours, $(printf '%s' "$out_ours" | wc -c) bytes of output)"
	fi
}

echo "== test-preload =="
[ -f "$LIB" ] || { echo "  $LIB missing (run: make preload)"; exit 1; }

run_both "the multithreaded probe"      $PROBE
run_both "ls -la of the project"        ls -la .
run_both "cat of a source file"         cat src/osmem.c
run_both "grep over the sources"        grep -rn "arena_acquire" src
run_both "sed over a source file"       sed -n 1,80p src/arena.c
run_both "sort of a word list"          sh -c 'ls -1 src utils tests | sort'
run_both "wc over the project"          sh -c 'wc -l src/*.c utils/*.h'
command -v git >/dev/null    && run_both "git status"        git status --porcelain
command -v python3 >/dev/null && run_both "python3 workload" python3 -c \
	'd={str(i):i*i for i in range(200000)}; s=sum(d[k] for k in d); print(s); print(len("x"*5000000))'
command -v gcc >/dev/null    && run_both "gcc compiling a file" \
	gcc -I utils -I src -O2 -Wall -Wextra -c src/block.c -o /tmp/osmem_preload_check.o
command -v make >/dev/null   && run_both "make --version"    make --version

rm -f /tmp/osmem_preload_check.o
echo
if [ "$fail" -eq 0 ]; then
	echo "all $ran programs behaved identically on the interposed allocator"
else
	echo "$fail of $ran programs differed"
	exit 1
fi
