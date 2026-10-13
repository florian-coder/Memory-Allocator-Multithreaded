# Memory Allocator

A C library for manual virtual memory management, built as an **adaptive
multithreaded allocator**: threads that run on their own get a private arena
and allocate with no locking at all, while a crowd of threads falls back to a
shared pool that keeps the number of kernel mappings bounded.

```c
void  *os_malloc(size_t size);
void  *os_calloc(size_t nmemb, size_t size);
void  *os_realloc(void *ptr, size_t size);
void   os_free(void *ptr);

void  *os_memalign(size_t alignment, size_t size);     /* cache lines, pages */
int    os_posix_memalign(void **out, size_t a, size_t n);
void  *os_aligned_alloc(size_t alignment, size_t size);
size_t os_usable_size(void *ptr);
size_t os_trim(void);                                  /* give pages back */
```

Memory comes exclusively from `mmap`/`mremap`/`munmap`/`madvise`. `brk`/`sbrk`
are not used at all, which removes the global kernel bottleneck and the risk of
threads colliding on a single program break.

It can also replace a program's allocator entirely:

```bash
make preload
LD_PRELOAD=./libosmem_preload.so python3 -c 'print(sum(i*i for i in range(300000)))'
```

---

## How it works

**Arenas.** Memory is requested from the kernel in large slices ("arenas", 4 MiB
by default), one `mmap` each. An arena is carved up in user space by a local
bump pointer plus a local free list, and its mapping is never returned. A hard
cap `A_max` (`8 x ncpu`, clamped to `[2, 64]`) limits how many arenas the
process may ever create, so the kernel's page tables stay tidy no matter how
many threads show up.

**Allocation** climbs a ladder, stopping at the first step that works:

1. **Private arena — lock-free.** The calling thread owns its arena, so nothing
   has to be locked: the free list is consulted first, then the bump pointer.
2. **Adoption.** A thread that needs an arena first looks for one orphaned by a
   thread that has finished, and takes it over instead of asking the kernel.
3. **A new private arena**, while the budget allows one — private arenas may
   claim all but two of the `A_max` slots, so the shared pool always has room.
4. **Reclaim.** An arena that fell idle in the shared pool can go private again,
   so a thread spike does not cost the process its fast path permanently.
5. **Shared regime (free-for-all).** Once arenas run out, threads share them:
   a `trylock` scan skips any arena that is momentarily busy, and only if the
   whole pool is contended does a thread queue up — on the arena with the most
   room.

**Freeing** works from any thread. A hidden 16-byte header in front of every
payload records the block size, a validity tag, and the arena it was carved
from, so `os_free()` always knows where a block belongs:

- its own arena → insert and coalesce immediately, no lock;
- an arena with no owner → one brief lock, then insert and coalesce;
- another thread's private arena → push the block on a lock-free stack that the
  owner drains on its next allocation, so the owner's fast path stays lock-free;
- a pointer this allocator never handed out → rejected by the tag and ignored.

Freed blocks are merged with their physically adjacent holes right away
(boundary tags make backward merging O(1)), a hole that ends up at the arena's
frontier is handed back to the bump pointer, and an arena that falls completely
idle gives its physical pages back to the kernel without giving up its mapping.

---

## Features

- **Lock-free fast path** for threads with a private arena: no mutex and no
  atomic read-modify-write in `malloc`/`free`.
- **Bounded kernel footprint**: at most `A_max` mappings, whatever the thread
  count.
- **Dynamic arena ownership**: arenas are orphaned on thread exit, adopted by
  new threads, shared under pressure, and reclaimed when the pressure is gone.
- **Safe cross-thread free**, including when the allocating thread has already
  exited.
- **`fork()`-safe**: `pthread_atfork` handlers quiesce the allocator, and an
  arena that a dying thread was writing to is quarantined rather than reused.
- **16-byte aligned payloads**, with a **32-byte minimum block** — 16 bytes of
  header and 16 usable.
- **Segregated free lists**: 62 exact-size bins below 1 KiB give an O(1)
  perfect fit; power-of-two bins above that are searched for the best fit.
- **Immediate coalescing** forwards and backwards, plus frontier retraction.
  Optional bounded delayed coalescing roughly doubles throughput for 7% more
  committed memory (`make test-quarantine`).
- **Page trimming** via `MADV_DONTNEED`, automatic when an arena falls idle and
  on demand through `os_trim()`, so RSS does not stay at its peak.
- **Zero-page aware `os_calloc()`**: a block carved from memory the arena has
  never used is already zeroed by the kernel, so the `memset` is skipped.
- **Large blocks** (above half an arena) get a dedicated mapping, resized with
  `mremap()` and released with `munmap()`.
- **Introspection API** (`utils/osmem_debug.h`) with a full consistency
  checker, mapping-failure injection and build-feature reporting, used by the
  test suite to assert structural properties rather than just non-NULL returns.

---

## Project Structure

```text
.
├── Makefile
├── src/                      # the allocator, one module per concern
│   ├── osmem_internal.h      # tunables, types, global state, hot inlines
│   ├── osmem.c               # public API, aligned and standalone blocks
│   ├── arena.c               # global state, start-up, fork, arena life cycle
│   ├── block.c               # bins, splitting, coalescing, trimming, remote frees
│   ├── ffa.c                 # the shared (free-for-all) regime
│   ├── debug.c               # introspection and the consistency checker
│   └── interpose.c           # the libc malloc family, for LD_PRELOAD
├── README.md
├── utils/
│   ├── osmem.h               # public API
│   ├── block_meta.h          # per-block hidden header
│   ├── osmem_debug.h         # introspection / consistency checking
│   └── printf.[ch]           # heap-free printf (framework)
├── tests/
│   ├── test_common.[ch]      # harness: checks, patterns, /proc introspection
│   ├── test_basic.c          # API semantics and the Phase 1 promises
│   ├── test_reuse.c          # free list: reuse, split, coalesce, fragmentation
│   ├── test_threads.c        # private arenas, adoption, cross-thread free
│   ├── test_ffa.c            # the shared regime and the A_max cap
│   ├── test_reclaim.c        # deterministic walk of the arena state machine
│   ├── test_fork.c           # fork() with threads inside the allocator
│   ├── test_faults.c         # out-of-memory paths, via injected failures
│   ├── test_trim.c           # returning pages, measured against RSS
│   ├── test_align.c          # over-aligned allocation and usable size
│   ├── test_fuzz.c           # randomised programs (also a libFuzzer target)
│   ├── test_paranoid.c       # checking invariants while threads run
│   ├── preload_probe.c       # an ordinary program, for the interposed library
│   ├── preload.sh            # runs real programs both ways and compares
│   └── bench.c               # throughput against glibc malloc
└── .github/workflows/ci.yml
```

---

## License

BSD 3-Clause, with the heap-free `printf` in `utils/` remaining under its own
MIT notice. See [`LICENSE`](LICENSE).

---

## Requirements

* Linux (x86_64 recommended)
* GCC / Clang, `make`
* POSIX threads

---

## Build

```bash
make                 # libosmem.so
make preload         # libosmem_preload.so, for LD_PRELOAD
make clean
```

## Test

```bash
make test            # all suites, plus a check that nothing links brk/sbrk
make test REPEAT=5   # repeat everything five times (races are probabilistic)
make test-asan       # under AddressSanitizer
make test-tsan       # under ThreadSanitizer (no data races)
make test-chaos      # with a delay injected at every race window
make test-paranoid   # + invariants checked while eight threads allocate
make test-quarantine # with bounded delayed coalescing
make test-nostats    # without the informational counters
make test-preload    # real programs (ls, git, python3, gcc) on the allocator
make bench           # throughput, and the memory it costs, next to glibc malloc
make profile         # gprof over a single-threaded churn run
make strict          # every module under stricter warnings, as errors
```

Environment variables reconfigure the allocator at start-up, which is how the
tests reach states a normal run never gets into:

| variable | meaning | default |
|----------|---------|---------|
| `OSMEM_ARENA_MAX`  | `A_max`, the maximum number of arenas | `8 x ncpu`, clamped to `[2, 64]` |
| `OSMEM_ARENA_SIZE` | bytes per arena | 4 MiB |
| `OSMEM_TRIM_THRESHOLD` | footprint above which an idle arena is trimmed | 256 KiB |
| `OSMEM_FAIL_MMAP_AFTER` | make every mapping after the Nth fail | unlimited |
| `OSMEM_CHAOS_RATE` | 1-in-N chance of a delay at a race window (chaos builds) | 4 |

