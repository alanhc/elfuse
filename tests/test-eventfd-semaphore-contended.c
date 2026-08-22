/*
 * Two threads blocking-reading one EFD_SEMAPHORE eventfd
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * elfuse signals an eventfd's readability through an internal pipe, and posts
 * to it on the counter's 0-to-nonzero edge. That edge is the whole story for a
 * plain eventfd, whose read takes the counter to zero every time. It is half
 * the story for EFD_SEMAPHORE, whose read decrements by one: a counter of 2
 * read once stays readable while the pipe goes empty, because the reader
 * consumed the single byte and the edge will not come again until the counter
 * returns to zero. A second reader blocked on that pipe then sleeps through a
 * count it was entitled to, and its vCPU thread is parked where neither
 * hv_vcpus_exit nor the wakeup pipe reaches it.
 *
 * Two readers and one writer make the window easy to hit: measured three hangs
 * in five runs before the read path learned to re-arm the pipe, and five clean
 * runs out of five against the qemu reference kernel, which is what says the
 * expectation below is Linux's and not elfuse's.
 *
 * The second assertion is the older bug the same path had: a sibling taking the
 * byte first must not turn a blocking read into EAGAIN.
 *
 * Syscalls exercised: eventfd2(19), read(63), write(64), clone(220),
 *                     futex(98), nanosleep(101)
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

/* Linux spelling; the guest headers may not expose eventfd(). SYS_eventfd2
 * comes from <sys/syscall.h>, which already knows it.
 */
#define L_EFD_SEMAPHORE 1

#define POSTS 400

static int efd;
static atomic_int eagains, got, stop;

static void *reader(void *arg)
{
    (void) arg;
    while (!atomic_load(&stop)) {
        uint64_t v;
        ssize_t n = read(efd, &v, sizeof(v));
        if (n < 0) {
            if (errno == EAGAIN)
                atomic_fetch_add(&eagains, 1);
            return NULL;
        }
        atomic_fetch_add(&got, (int) v);
    }
    return NULL;
}

int main(void)
{
    efd = (int) syscall(SYS_eventfd2, 0, L_EFD_SEMAPHORE);
    if (efd < 0) {
        FAIL("eventfd2 failed");
        SUMMARY("test-eventfd-semaphore-contended");
        return 1;
    }

    pthread_t a, b;
    if (pthread_create(&a, NULL, reader, NULL) != 0 ||
        pthread_create(&b, NULL, reader, NULL) != 0) {
        FAIL("pthread_create failed");
        SUMMARY("test-eventfd-semaphore-contended");
        return 1;
    }

    /* Aim at the window rather than hoping to stumble into it. Let both readers
     * reach the wait, then post two units in one write: the first reader takes
     * one and consumes the single pipe byte, and the counter it leaves behind
     * is exactly the state the 0-to-nonzero edge cannot signal again. Without
     * the re-arm the second reader sleeps here.
     *
     * Detection of the lost wakeup is not certain, and the reason is a window
     * no guest can reach: a reader only waits when it has just seen a zero
     * counter, so a second reader has to be between that check and its poll at
     * the moment the first consumes the byte. One reader cannot reproduce it at
     * all, because after a take that leaves a remainder it simply loops and
     * takes again rather than parking.
     *
     * Measured against a tree with the re-arm removed: this shape catches it in
     * roughly two runs in five (4/5 and 3/8 across two sittings), and the fixed
     * tree passed 5/5. Repeating the burst and draining between rounds was
     * tried and measured worse, 2/6, because a full drain destroys the leftover
     * count the bug needs. The EAGAIN assertion below is the reliable half:
     * origin/main fails it in four runs out of five.
     */
    usleep(50 * 1000);
    uint64_t burst = 2;
    int expect = POSTS;
    if (write(efd, &burst, sizeof(burst)) == (ssize_t) sizeof(burst)) {
        expect += 2;
    } else {
        /* Do not go on to wait for units this write never posted: the loop
         * below would hang for a count that cannot arrive, and the harness
         * would report a timeout rather than the write that failed.
         */
        FAIL("burst write failed");
    }

    for (int i = 0; i < POSTS; i++) {
        uint64_t one = 1;
        if (write(efd, &one, sizeof(one)) != (ssize_t) sizeof(one)) {
            FAIL("write failed");
            break;
        }
    }

    /* Every posted unit has to come back out. A missed wakeup shows up here as
     * a test that never finishes, which the harness timeout reports.
     */
    while (atomic_load(&got) < expect)
        usleep(1000);

    atomic_store(&stop, 1);
    uint64_t two = 2;
    write(efd, &two, sizeof(two)); /* release both readers */
    pthread_join(a, NULL);
    pthread_join(b, NULL);

    TEST("every posted unit reaches a blocked reader");
    EXPECT_TRUE(atomic_load(&got) >= expect,
                "a semaphore eventfd lost a wakeup");

    TEST("a blocking read never reports EAGAIN to the guest");
    EXPECT_TRUE(atomic_load(&eagains) == 0,
                "a sibling taking the byte turned a blocking read into EAGAIN");

    close(efd);
    SUMMARY("test-eventfd-semaphore-contended");
    return fails ? 1 : 0;
}
