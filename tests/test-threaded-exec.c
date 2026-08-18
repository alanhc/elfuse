/*
 * Threaded execve stress: exec while sibling threads are still live
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux de_thread()s on execve: every sibling thread is destroyed and the
 * calling thread takes over the group leader's tid, so the new image always
 * starts single-threaded with gettid() == getpid(). elfuse does the same in
 * thread_exec_de_thread(), and this is what holds it to that: without the
 * teardown, guest_reset zeroes memory that sibling vCPU threads are still
 * parked in host syscalls on, or still executing.
 *
 * The process execs itself TOTAL_EXECS times, spawning WORKERS siblings before
 * each exec (half parked in a blocking read(), half in a compute loop), and
 * checks the three facts Linux guarantees in each new image, failing fast if
 * any is wrong. Chained exec rather than fork-per-iteration: it keeps one guest
 * for the whole run, so a sibling teardown bug has nowhere to hide behind a
 * fresh VM.
 *
 * Two modes, both in make check. "main" (the default) execs from the main
 * thread. "worker" execs from a sibling, which elfuse cannot satisfy directly
 * (it cannot destroy the main host thread, whose run loop returning is what
 * tears the process down) so it hands the syscall to the leader, which runs it
 * on its own vCPU. Both modes assert the same three facts, which is the point:
 * the guest cannot tell which thread called execve.
 *
 * Syscalls exercised: execve(221), clone(220), read(63), pipe2(59),
 *                     gettid(178), getpid(172)
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

#define WORKERS 8
#define TOTAL_EXECS 200

static atomic_int ready;
static atomic_int exec_now;
static int pipe_rd = -1;
static const char *self_path;
static const char *g_mode = "main";
static int g_iter;

static void exec_next(void)
{
    /* execve replaces the image, stdio buffer included, and the harness
     * captures stdout through a pipe, so it is block-buffered. Anything this
     * image printed is discarded unless it is flushed first.
     */
    fflush(stdout);
    fflush(stderr);

    char iterbuf[16], pidbuf[16];
    snprintf(iterbuf, sizeof(iterbuf), "%d", g_iter + 1);
    snprintf(pidbuf, sizeof(pidbuf), "%d", (int) getpid());

    extern char **environ;
    char *argv[] = {(char *) self_path, (char *) g_mode, iterbuf, pidbuf, NULL};
    execve(self_path, argv, environ);

    fprintf(stderr, "\ntest-threaded-exec: execve(%s) failed at iter %d (%s)\n",
            self_path, g_iter, strerror(errno));
    _exit(1);
}

/* Nothing is ever written to the pipe, so this parks in a host syscall that
 * hv_vcpus_exit cannot interrupt, the case an exec-time teardown has to reach
 * through the wakeup pipe instead. EINTR is a retry: only the read end closing
 * (at exec) ends the loop.
 */
static void park_on_pipe(void)
{
    char c;
    for (;;) {
        ssize_t n = read(pipe_rd, &c, 1);
        if (n == 0 || (n < 0 && errno != EINTR))
            return;
    }
}

static void *blocking_worker(void *arg)
{
    (void) arg;
    atomic_fetch_add(&ready, 1);
    park_on_pipe();
    return NULL;
}

/* The other half stay in guest code. One of them is the designated exec'ing
 * thread in worker mode.
 */
static void *compute_worker(void *arg)
{
    long exec_here = (long) arg;
    volatile unsigned long acc = 0;

    atomic_fetch_add(&ready, 1);
    for (;;) {
        for (int i = 0; i < 20000; i++)
            acc += (unsigned long) i;

        /* Touch the memory syscalls too. A sibling parked on the host mutex
         * that serializes them is reachable by none of the teardown wakes, so a
         * teardown that runs while holding it can never finish. Workers that
         * only compute and read cannot show that.
         */
        void *p = mmap(NULL, 64 * 1024, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED)
            munmap(p, 64 * 1024);

        if (exec_here && atomic_load(&exec_now))
            exec_next();
    }
}

static int read_thread_count(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;

    char line[256];
    int n = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "Threads: %d", &n) == 1)
            break;
    }
    fclose(f);
    return n;
}

/* The three facts Linux guarantees in the post-execve image. Fail fast: a
 * per-iteration PASS line would print 200 times.
 */
static void check_post_exec(int iter, int want_pid)
{
    int nthreads = read_thread_count();
    if (nthreads != 1) {
        printf("\niter %d: /proc/self/status Threads: %d, want 1\n", iter,
               nthreads);
        fails++;
    }

    int pid = (int) getpid();
    if (pid != want_pid) {
        printf("\niter %d: pid %d after exec, want %d\n", iter, pid, want_pid);
        fails++;
    }

    int tid = (int) syscall(SYS_gettid);
    if (tid != pid) {
        printf("\niter %d: gettid %d != getpid %d after exec\n", iter, tid,
               pid);
        fails++;
    }
}

static void spawn_workers(void)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        FAIL("pipe2 failed");
        exit(1);
    }

    /* fds[1] is deliberately left open and never written: the readers block
     * rather than seeing EOF.
     */
    pipe_rd = fds[0];

    for (int i = 0; i < WORKERS; i++) {
        pthread_t t;

        /* Worker 1 is a compute thread and is the one that execs on odd
         * iterations.
         */
        int rc = (i % 2 == 0) ? pthread_create(&t, NULL, blocking_worker, NULL)
                              : pthread_create(&t, NULL, compute_worker,
                                               (void *) (long) (i == 1));
        if (rc != 0) {
            FAIL("pthread_create failed");
            exit(1);
        }
        pthread_detach(t);
    }

    /* Yield rather than spin: the workers being waited on are competing for the
     * same cores, WORKERS fresh vCPUs at a time.
     */
    while (atomic_load(&ready) < WORKERS)
        sched_yield();
}

int main(int argc, char **argv)
{
    self_path = argv[0];
    if (argc > 1)
        g_mode = argv[1];
    int iter = argc > 2 ? atoi(argv[2]) : 0;
    bool from_worker = strcmp(g_mode, "worker") == 0;

    if (iter == 0) {
        printf(
            "test-threaded-exec: %d execs from the %s thread, %d live "
            "siblings each\n",
            TOTAL_EXECS, from_worker ? "worker" : "main", WORKERS);
        TEST("threaded execve chain");
    } else {
        check_post_exec(iter, argc > 3 ? atoi(argv[3]) : 0);
        if (fails > 0)
            return 1;
    }

    if (iter >= TOTAL_EXECS) {
        PASS();
        SUMMARY("test-threaded-exec");
        return fails > 0 ? 1 : 0;
    }

    g_iter = iter;
    spawn_workers();

    if (!from_worker)
        exec_next();

    /* Worker mode: park the main thread in a host syscall while the exec runs
     * under it.
     */
    atomic_store(&exec_now, 1);
    park_on_pipe();

    /* Only reachable if the designated worker never replaced this image: a
     * successful exec never returns here, and the pipe's write end is held open
     * so the park ends at EOF alone. Reporting success would hide exactly the
     * failure this mode exists to catch.
     */
    return 1;
}
