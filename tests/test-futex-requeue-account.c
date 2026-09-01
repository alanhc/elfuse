/*
 * test-futex-requeue-account.c -- a requeued waiter's bookkeeping survives it.
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * elfuse answers a FUTEX_WAKE with nobody parked from EL1, off a per-bucket
 * count the wait paths publish. FUTEX_REQUEUE moves a parked waiter to another
 * address, so the charge has to move with it. If the drop still names the
 * bucket the wait started on, that bucket goes one below zero and wraps, and
 * the wrap is the dangerous half: it reads as occupied, so nothing looks wrong,
 * until a later waiter's increment brings the count back to exactly zero while
 * that waiter is genuinely parked. The next wake is then answered 0 at EL1 and
 * never reaches it.
 *
 * The sequence below is that bug end to end: requeue a waiter away from A, so a
 * mismatched drop lands on A's bucket; then park a fresh waiter on A, whose
 * increment cancels the wrap; then wake A.
 *
 * The assertion is the time that last wake takes, not whether it completes. A
 * lost wakeup does not hang here: the wait paths re-arm on a bounded quantum so
 * a waiter nobody woke still re-checks its word within 100 ms and leaves. That
 * backstop turns the bug into latency rather than a hang, so the test measures
 * the latency. A delivered wake returns in microseconds; a lost one waits out
 * the quantum.
 *
 * FUTEX_WAIT_BITSET rather than FUTEX_WAIT: only the bitset form is guaranteed
 * to take the bucket queue, which is the only queue requeue moves waiters on.
 * Every assertion here is plain Linux futex ABI.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <linux/futex.h>

#include "raw-syscall.h"
#include "test-harness.h"

int passes = 0, fails = 0;

static uint32_t addr_a, addr_b;

static long wait_bitset(uint32_t *w, uint32_t expect)
{
    return raw_syscall6(__NR_futex, (long) w,
                        FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, expect, 0, 0,
                        (long) (int32_t) FUTEX_BITSET_MATCH_ANY);
}

static long wake(uint32_t *w, int n)
{
    return raw_syscall6(__NR_futex, (long) w, FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                        n, 0, 0, 0);
}

static long cmp_requeue(uint32_t *from,
                        uint32_t *to,
                        int nwake,
                        int nrequeue,
                        uint32_t expect)
{
    return raw_syscall6(__NR_futex, (long) from,
                        FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG, nwake, nrequeue,
                        (long) to, (long) (int32_t) expect);
}

/* Give a thread time to reach its park. A sleep, not a spin: a guest thread is
 * a host thread, and a spin starves rather than converges under load.
 */
static void settle(void)
{
    struct timespec ts = {0, 50 * 1000 * 1000};
    nanosleep(&ts, NULL);
}

static void *park_on_a(void *arg)
{
    (void) arg;
    while (__atomic_load_n(&addr_a, __ATOMIC_SEQ_CST) == 0)
        wait_bitset(&addr_a, 0);
    return NULL;
}

int main(void)
{
    pthread_t t1, t2;

    printf("FUTEX_REQUEUE accounting tests\n");

    /* 1. Park a waiter on A, then move it to B and wake it there. A drop that
     * names the wrong bucket wraps A's count here.
     */
    TEST("a requeued waiter wakes on its new address");
    if (pthread_create(&t1, NULL, park_on_a, NULL) != 0) {
        FAIL("pthread_create");
    } else {
        settle();
        long rc = cmp_requeue(&addr_a, &addr_b, 0, 1, 0);
        if (rc < 0) {
            (printf("FAIL: cmp_requeue rc=%ld\n", rc), fails++);
        } else {
            /* The waiter is on B now, so the store it re-checks is on A and the
             * wake that reaches it is on B.
             */
            __atomic_store_n(&addr_a, 1, __ATOMIC_SEQ_CST);
            wake(&addr_b, 1);
            pthread_join(t1, NULL);
            PASS();
        }
    }

    /* 2. A fresh waiter on A. Its increment is what cancels a wrapped count, so
     * this is the wake that goes missing when the accounting is wrong.
     */
    TEST("a later waiter on the vacated address wakes promptly");
    __atomic_store_n(&addr_a, 0, __ATOMIC_SEQ_CST);
    if (pthread_create(&t2, NULL, park_on_a, NULL) != 0) {
        FAIL("pthread_create");
    } else {
        struct timespec t0, t1s;
        settle();
        __atomic_store_n(&addr_a, 1, __ATOMIC_SEQ_CST);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        wake(&addr_a, 1);
        pthread_join(t2, NULL);
        clock_gettime(CLOCK_MONOTONIC, &t1s);

        long ms = (t1s.tv_sec - t0.tv_sec) * 1000 +
                  (t1s.tv_nsec - t0.tv_nsec) / 1000000;

        /* Well under the 100 ms re-arm quantum and well over a delivered wake,
         * which is microseconds even on a loaded host.
         */
        if (ms < 40)
            PASS();
        else
            (printf("FAIL: wake took %ld ms, so it was not delivered\n", ms),
             fails++);
    }

    SUMMARY("test-futex-requeue-account");
    return fails > 0 ? 1 : 0;
}
