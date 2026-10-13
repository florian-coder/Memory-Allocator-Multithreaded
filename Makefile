# SPDX-License-Identifier: BSD-3-Clause

UTILS_PATH ?= utils
TESTS_PATH ?= tests
SRC_PATH ?= src

CC = gcc
CPPFLAGS = -I$(UTILS_PATH) -I$(SRC_PATH)
CFLAGS = -fPIC -Wall -Wextra -g -O2 -std=gnu11 -pthread
LDFLAGS = -shared
LDLIBS = -pthread

# Sanitizer flags, injected by the test-asan / test-tsan targets.
SAN ?=
# Extra defines, injected by the test-chaos / test-paranoid targets.
EXTRA ?=
# How many times each suite is run by `make test`.
REPEAT ?= 1
# Prefix used to launch the test binaries.  ThreadSanitizer needs ASLR off on
# kernels that randomise mmap over more bits than its shadow mapping allows.
RUNNER ?=

ALLOC_SRCS = $(SRC_PATH)/osmem.c $(SRC_PATH)/arena.c $(SRC_PATH)/block.c \
	     $(SRC_PATH)/ffa.c $(SRC_PATH)/debug.c
SRCS = $(ALLOC_SRCS) $(UTILS_PATH)/printf.c
OBJS = $(SRCS:.c=.o)
TARGET = libosmem.so
PRELOAD = libosmem_preload.so

TEST_SRCS = $(wildcard $(TESTS_PATH)/test_*.c)
TEST_BINS = $(patsubst %.c,%,$(filter-out $(TESTS_PATH)/test_common.c,$(TEST_SRCS)))

.PHONY: all clean clean-tests test test-asan test-tsan test-chaos test-paranoid \
	test-nostats test-quarantine test-preload bench profile strict check-no-brk \
	preload

all: $(TARGET)

# Separate library, so that linking libosmem.so does not replace a program's
# allocator behind its back.
preload: $(PRELOAD)

$(PRELOAD): $(ALLOC_SRCS) $(SRC_PATH)/interpose.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared -o $@ $^ $(LDLIBS)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EXTRA) $(SAN) -c -o $@ $<

# Each test binary links the allocator directly so that it can use the
# introspection API in utils/osmem_debug.h.
$(TESTS_PATH)/%: $(TESTS_PATH)/%.c $(TESTS_PATH)/test_common.c $(ALLOC_SRCS)
	$(CC) $(CPPFLAGS) -I$(TESTS_PATH) $(CFLAGS) $(EXTRA) $(SAN) -o $@ $^ \
		$(LDLIBS) $(SAN)

test: $(TEST_BINS) check-no-brk
	@fail=0; \
	i=1; while [ $$i -le $(REPEAT) ]; do \
		if [ $(REPEAT) -gt 1 ]; then echo "--- round $$i/$(REPEAT) ---"; fi; \
		for t in $(TEST_BINS); do $(RUNNER) ./$$t || fail=1; echo; done; \
		i=$$((i+1)); \
	done; \
	if [ $$fail -eq 0 ]; then echo "all test suites passed"; else \
		echo "there were failures"; exit 1; fi

# Compile everything under flags stricter than the build normally uses.  Most
# of these are warnings clang turns on in situations gcc does not, so this is
# the cheapest available substitute for building with both compilers.
STRICT_FLAGS = -std=gnu11 -Wall -Wextra -pedantic -Wshadow -Wpointer-arith \
	       -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes \
	       -Wredundant-decls -Wundef -Wwrite-strings -Wvla -Werror

strict:
	@for f in $(ALLOC_SRCS) $(SRC_PATH)/interpose.c; do \
		echo "  strict $$f"; \
		$(CC) $(CPPFLAGS) $(STRICT_FLAGS) $(EXTRA) -O2 -pthread -c $$f -o /dev/null || exit 1; \
	done
	@echo "strict: no diagnostics"

# Phase 1.2 of the plan: the allocator must not contain a single brk/sbrk call.
check-no-brk: $(TARGET)
	@if nm -D --undefined-only $(TARGET) | grep -qE ' (brk|sbrk)$$'; then \
		echo "check-no-brk: $(TARGET) still references brk/sbrk"; exit 1; \
	else echo "check-no-brk: no brk/sbrk dependency in $(TARGET)"; fi

test-asan:
	$(MAKE) clean-tests
	$(MAKE) SAN="-fsanitize=address -fno-omit-frame-pointer" test
	$(MAKE) clean-tests

test-tsan:
	$(MAKE) clean-tests
	$(MAKE) SAN="-fsanitize=thread" \
		RUNNER="$(shell command -v setarch >/dev/null && echo setarch -R)" test
	$(MAKE) clean-tests

# Perturb the interleaving at every window where two threads can race, and run
# the suites several times: a race that takes dozens of runs to show up
# otherwise shows up immediately.
test-chaos:
	$(MAKE) clean-tests
	$(MAKE) EXTRA="-DOSMEM_CHAOS" REPEAT=3 test
	$(MAKE) clean-tests

# Bounded delayed coalescing: parks small freed blocks instead of merging them
# straight away.  A deviation from Phase 3.4 of the plan, so it is off by
# default; the fragmentation assertions in test_reuse are the guard rail.
test-quarantine:
	$(MAKE) clean-tests
	$(MAKE) EXTRA="-DOSMEM_QUARANTINE" test
	$(MAKE) clean-tests

# Without the informational counters.  Worth about 2% on a single thread, so it
# exists mostly to keep the distinction between load-bearing and decorative
# counters honest; the suite skips the assertions about them.
test-nostats:
	$(MAKE) clean-tests
	$(MAKE) EXTRA="-DOSMEM_NO_STATS" test
	$(MAKE) clean-tests

# The same, plus a world lock so that osmem_check() can verify every invariant
# while the threads are running (see tests/test_paranoid.c).
test-paranoid:
	$(MAKE) clean-tests
	$(MAKE) EXTRA="-DOSMEM_CHAOS -DOSMEM_WORLD_LOCK" test
	$(MAKE) clean-tests

# Run real programs on this allocator and compare their output with a normal
# run.  The probe is built against libc's malloc on purpose: LD_PRELOAD is what
# swaps the allocator underneath it.
test-preload: $(PRELOAD) $(TESTS_PATH)/preload_probe
	@sh $(TESTS_PATH)/preload.sh

$(TESTS_PATH)/preload_probe: $(TESTS_PATH)/preload_probe.c
	$(CC) -Wall -Wextra -O2 -g -pthread -o $@ $<

bench: $(TESTS_PATH)/bench
	./$(TESTS_PATH)/bench

# Where the time actually goes on the same-thread churn path.  Single threaded
# on purpose: gprof's instrumentation is not thread safe.
profile: $(TESTS_PATH)/profile
	cd $(TESTS_PATH) && ./profile && gprof -b -p ./profile gmon.out | head -25 && \
		echo && gprof -b -q ./profile gmon.out | sed -n 1,25p
	-rm -f $(TESTS_PATH)/gmon.out

$(TESTS_PATH)/profile: $(TESTS_PATH)/profile.c $(TESTS_PATH)/test_common.c $(ALLOC_SRCS)
	$(CC) $(CPPFLAGS) -I$(TESTS_PATH) $(CFLAGS) $(EXTRA) -pg -o $@ $^ $(LDLIBS)

$(TESTS_PATH)/bench: $(TESTS_PATH)/bench.c $(TESTS_PATH)/test_common.c $(ALLOC_SRCS)
	$(CC) $(CPPFLAGS) -I$(TESTS_PATH) $(CFLAGS) $(EXTRA) -o $@ $^ $(LDLIBS)

# Reproducible source archive: git archive takes exactly the tracked files, so
# no build artefacts, editor leftovers or local experiments can slip in.
ARCHIVE_NAME ?= memory-allocator
ARCHIVE_REF ?= HEAD

pack:
	git archive --format=tar.gz --prefix=$(ARCHIVE_NAME)/ \
		-o ../$(ARCHIVE_NAME).tar.gz $(ARCHIVE_REF)
	git archive --format=zip --prefix=$(ARCHIVE_NAME)/ \
		-o ../$(ARCHIVE_NAME).zip $(ARCHIVE_REF)
	@echo "wrote ../$(ARCHIVE_NAME).tar.gz and ../$(ARCHIVE_NAME).zip"
	@echo "contents:"
	@git archive --format=tar $(ARCHIVE_REF) | tar -t | sed 's/^/  /'

clean-tests:
	-rm -f $(TEST_BINS) $(TESTS_PATH)/bench $(TESTS_PATH)/preload_probe
	-rm -f $(TESTS_PATH)/profile $(TESTS_PATH)/gmon.out

clean: clean-tests
	-rm -f $(TARGET) $(PRELOAD)
	-rm -f $(OBJS)
