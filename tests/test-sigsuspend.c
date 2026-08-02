/*
 * rt_sigsuspend blocking semantics
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * sigsuspend() must install the temporary mask, block until a signal that the
 * guest can actually observe becomes deliverable, run the handler, and leave
 * the caller's original mask behind. Returning early -- the behaviour before
 * rt_sigsuspend learned to block -- is invisible to a caller that loops, but
 * pins a core and lets an alarm-driven waiter report a spurious timeout.
 *
 * The multi-threaded case covers the shared-signal race: a process-directed
 * signal wakes one waiter, and signal delivery drains the shared set on
 * whichever vCPU reaches it first. A waiter that loses that race would return
 * with no handler run and its temporary mask still installed, so this checks
 * that exactly one thread reports the handler and that both threads come back
 * with the mask they started with.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

static volatile sig_atomic_t alarm_ran;
static volatile sig_atomic_t usr1_ran;
/* Delivery count while only the process-directed signal is in flight. The
 * per-thread wakeups sent afterwards must not be able to satisfy the
 * shared-signal assertion, so they are counted in a separate phase.
 */
static volatile sig_atomic_t usr1_shared_phase;
static volatile sig_atomic_t usr1_shared_deliveries;

static void on_alarm(int s)
{
    (void) s;
    alarm_ran = 1;
}

static void on_usr1(int s)
{
    (void) s;
    usr1_ran = 1;
    if (usr1_shared_phase)
        usr1_shared_deliveries++;
}

static double elapsed_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) + (t1.tv_nsec - t0->tv_nsec) / 1e9;
}

/* sigsuspend must not return before a signal arrives, must run the handler,
 * and must restore the mask it was called with.
 */
static void test_blocks_until_signal(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    sigset_t block, orig, empty;
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    sigprocmask(SIG_BLOCK, &block, &orig);
    sigemptyset(&empty);

    alarm_ran = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    alarm(1);
    sigsuspend(&empty);
    double waited = elapsed_since(&t0);

    TEST("sigsuspend blocks until the signal arrives");
    EXPECT_TRUE(waited >= 0.5, "returned before the alarm fired");

    TEST("handler ran before sigsuspend returned");
    EXPECT_TRUE(alarm_ran == 1, "handler did not run");

    TEST("sigsuspend reports EINTR");
    EXPECT_TRUE(errno == EINTR, "errno was not EINTR");

    /* The temporary mask must not outlive the call: SIGALRM was blocked on
     * entry and must still be blocked on return.
     */
    sigset_t after;
    sigemptyset(&after);
    sigprocmask(SIG_BLOCK, NULL, &after);
    TEST("original mask restored after delivery");
    EXPECT_TRUE(sigismember(&after, SIGALRM) == 1,
                "temporary sigsuspend mask leaked past the call");

    sigprocmask(SIG_SETMASK, &orig, NULL);
}

/* An ignored signal is discarded without reaching the guest, so it must not
 * end the wait. The alarm is the escape hatch that keeps this bounded.
 */
static void test_ignored_signal_does_not_wake(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGUSR2, &sa, NULL);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    sigset_t block, orig, empty;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    sigaddset(&block, SIGALRM);
    sigprocmask(SIG_BLOCK, &block, &orig);
    sigemptyset(&empty);

    alarm_ran = 0;
    raise(SIGUSR2); /* pending, but ignored: must not wake the sleeper */
    alarm(1);
    sigsuspend(&empty);

    TEST("ignored signal does not end the wait");
    EXPECT_TRUE(alarm_ran == 1, "woke on the ignored signal instead");

    sigprocmask(SIG_SETMASK, &orig, NULL);
}

static pthread_barrier_t ready;
static volatile sig_atomic_t mask_leaked;
/* Incremented immediately before each worker calls sigsuspend(). The barrier
 * only gates the threads up to that point, so waiting on this instead of a
 * wall-clock sleep keeps the test honest on a loaded or cross-checked host.
 */
static volatile sig_atomic_t parked;

static void *suspender(void *arg)
{
    (void) arg;
    sigset_t block, empty, after;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block, NULL);
    sigemptyset(&empty);

    pthread_barrier_wait(&ready);
    parked++;
    sigsuspend(&empty);

    sigemptyset(&after);
    pthread_sigmask(SIG_BLOCK, NULL, &after);
    if (sigismember(&after, SIGUSR1) != 1)
        mask_leaked = 1;
    return NULL;
}

/* Two threads wait on one process-directed signal. Whichever thread the
 * delivery lands on, neither may come back with the temporary mask still
 * installed.
 */
static void test_shared_signal_two_waiters(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    usr1_ran = 0;
    mask_leaked = 0;
    parked = 0;
    usr1_shared_phase = 0;
    usr1_shared_deliveries = 0;

    sigset_t block, orig;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, &orig);

    pthread_t a, b;
    pthread_barrier_init(&ready, NULL, 3);
    pthread_create(&a, NULL, suspender, NULL);
    pthread_create(&b, NULL, suspender, NULL);
    pthread_barrier_wait(&ready);

    /* Wait for both workers to reach sigsuspend rather than guessing with a
     * sleep. parked is bumped just before the call, so one short grace period
     * after it reaches 2 covers the remaining instructions into the syscall.
     */
    for (int i = 0; i < 500 && parked < 2; i++)
        usleep(1000);
    usleep(50000);

    /* Phase 1: one process-directed signal, delivered to exactly one thread. */
    usr1_shared_phase = 1;
    kill(getpid(), SIGUSR1);
    for (int i = 0; i < 500 && usr1_shared_deliveries == 0; i++)
        usleep(1000);
    usleep(50000);
    usr1_shared_phase = 0;

    /* Phase 2: wake whichever thread lost the race so it can check its mask.
     * These are per-thread and no longer counted against the assertion above.
     */
    pthread_kill(a, SIGUSR1);
    pthread_kill(b, SIGUSR1);

    pthread_join(a, NULL);
    pthread_join(b, NULL);
    pthread_barrier_destroy(&ready);

    TEST("process-directed signal delivered exactly once");
    EXPECT_TRUE(usr1_shared_deliveries == 1,
                "shared signal was not delivered exactly once");

    TEST("no waiter leaked its temporary mask");
    EXPECT_TRUE(mask_leaked == 0, "a thread returned with the suspend mask");

    sigprocmask(SIG_SETMASK, &orig, NULL);
}

int main(void)
{
    printf("test-sigsuspend: rt_sigsuspend blocking semantics\n");

    test_blocks_until_signal();
    test_ignored_signal_does_not_wake();
    test_shared_signal_two_waiters();

    SUMMARY("test-sigsuspend");
    return fails > 0 ? 1 : 0;
}
