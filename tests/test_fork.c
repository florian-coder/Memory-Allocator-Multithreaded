// SPDX-License-Identifier: BSD-3-Clause
/*
 * fork() in a process whose threads are allocating.
 *
 * Without the pthread_atfork() handlers in arena.c the child inherits arena
 * mutexes that were held by threads which do not exist any more, and the first
 * allocation that needs one of those locks hangs forever.  The arena budget is
 * deliberately tiny here so that the child *has* to take a lock instead of
 * quietly creating an arena of its own -- that is what makes the test
 * reproduce in one round rather than one in fifty.
 */

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define ARENA_MAX 2
#define ARENA_SIZE (128 * 1024)
#define CHURNERS 4
#define ROUNDS 60
#define CHILD_TIMEOUT_MS 5000

static atomic_int stop_churn;
static atomic_int churn_errors;

/*
 * Whether an arena owned by a thread that does not survive the fork has to be
 * quarantined depends on how the allocator was built: with -DOSMEM_WORLD_LOCK
 * the prepare handler can quiesce the lock-free owners too, so the child
 * inherits everything consistent and reuses it all.  Without it, such an arena
 * may have been caught mid-update and is abandoned.
 */
static int quarantine_expected(void)
{
	struct osmem_stats st;

	osmem_stats(&st);
	return !st.has_world_lock;
}

static void *churner(void *arg)
{
	int id = (int)(long)arg;
	void *p[8];
	int i;

	while (!stop_churn) {
		for (i = 0; i < 8; i++) {
			p[i] = os_malloc(64 + i * 32);
			if (p[i])
				pattern_fill(p[i], 64 + i * 32, (uint64_t)id + 1);
		}
		for (i = 0; i < 8; i++) {
			if (p[i] && !pattern_ok(p[i], 64 + i * 32, (uint64_t)id + 1))
				churn_errors++;
			os_free(p[i]);
		}
	}
	return NULL;
}

/* Returns 0 if the child finished in time, 1 if it had to be killed. */
static int run_child(int expect_quarantine)
{
	pid_t pid = fork();
	int status, ms;

	if (pid == 0) {
		struct osmem_stats cst;
		/* Everything here needs a lock an exited thread may have held. */
		void *p = os_malloc(128);
		void *q = os_calloc(4, 64);
		void *r;

		if (!p || !q)
			_exit(3);
		pattern_fill(p, 128, 7);
		r = os_realloc(p, 2048);
		if (!r || !pattern_ok(r, 128, 7))
			_exit(4);
		os_free(r);
		os_free(q);
		if (osmem_check() != 0)
			_exit(5);
		/* An arena owned by a thread that did not survive must have been
		 * quarantined, otherwise this test would be passing by luck.
		 */
		osmem_stats(&cst);
		if (expect_quarantine && cst.arena_poisoned == 0)
			_exit(6);	/* a torn arena should have been quarantined */
		if (!expect_quarantine && cst.arena_poisoned != 0)
			_exit(7);	/* nothing should have needed quarantining */
		_exit(0);
	}
	CHECKF(pid > 0, "fork failed");

	for (ms = 0; ms < CHILD_TIMEOUT_MS; ms++) {
		usleep(1000);
		if (waitpid(pid, &status, WNOHANG) == pid) {
			CHECKF(WIFEXITED(status) && WEXITSTATUS(status) == 0,
			       "child exited with status %d", WEXITSTATUS(status));
			return 0;
		}
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	return 1;
}

static void test_fork_while_threads_allocate(void)
{
	pthread_t th[CHURNERS];
	struct osmem_stats st;
	int hangs = 0;
	int i;

	t_begin("fork() while other threads are inside the allocator");
	for (i = 0; i < CHURNERS; i++)
		CHECK(pthread_create(&th[i], NULL, churner, (void *)(long)i) == 0);
	usleep(50000);			/* let them get going */

	for (i = 0; i < ROUNDS; i++)
		hangs += run_child(quarantine_expected());

	stop_churn = 1;
	for (i = 0; i < CHURNERS; i++)
		pthread_join(th[i], NULL);

	CHECKF(hangs == 0, "%d of %d children hung on an inherited lock", hangs, ROUNDS);
	CHECKF(churn_errors == 0, "%d corruptions in the parent", churn_errors);
	osmem_stats(&st);
	CHECKF(st.arena_count <= ARENA_MAX, "%zu arenas, A_max is %d", st.arena_count,
	       ARENA_MAX);
	CHECK_SOUND();
}

static void test_fork_single_threaded(void)
{
	int hangs = 0;
	int i;

	t_begin("fork() with no other thread running");
	for (i = 0; i < 5; i++)
		hangs += run_child(0);
	CHECKF(hangs == 0, "%d children hung", hangs);
	CHECK_SOUND();
}

/* A child that forks again, to make sure the handlers stay usable. */
static void test_nested_fork(void)
{
	pid_t pid;
	int status, ms;

	t_begin("fork() inside a forked child");
	pid = fork();
	if (pid == 0) {
		void *p = os_malloc(256);
		/* The churners have already been joined, so there is no live
		 * owner left for this child to quarantine.
		 */
		int rc = run_child(0);		/* fork again from the child */

		os_free(p);
		/* Propagate the checks this process ran: a failure inside a
		 * child is invisible to the parent's exit code otherwise.
		 */
		_exit(rc == 0 && t_failures == 0 ? 0 : 8);
	}
	CHECK(pid > 0);
	for (ms = 0; ms < CHILD_TIMEOUT_MS; ms++) {
		usleep(1000);
		if (waitpid(pid, &status, WNOHANG) == pid) {
			CHECKF(WIFEXITED(status) && WEXITSTATUS(status) == 0,
			       "nested child exited with %d", WEXITSTATUS(status));
			CHECK_SOUND();
			return;
		}
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	CHECKF(0, "the nested child hung");
}

int main(void)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%d", ARENA_MAX);
	setenv("OSMEM_ARENA_MAX", buf, 1);
	snprintf(buf, sizeof(buf), "%d", ARENA_SIZE);
	setenv("OSMEM_ARENA_SIZE", buf, 1);

	printf("== test_fork (A_max=%d, arena=%d KiB, quarantine %s) ==\n",
	       ARENA_MAX, ARENA_SIZE / 1024,
	       quarantine_expected() ? "expected" : "not needed");
	test_fork_single_threaded();
	test_fork_while_threads_allocate();
	test_nested_fork();
	return t_report("test_fork");
}
