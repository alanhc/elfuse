/*
 * A dup taken while a sibling flips O_NONBLOCK agrees with the description
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * O_NONBLOCK belongs to the open file description, so every name for one must
 * report the same value. elfuse keeps that in a per-fd shadow and sweeps the
 * aliases on F_SETFL, which leaves one window: an alias built from a snapshot
 * taken before it is published is not in the table when the sweep runs, so the
 * sweep cannot reach it and the publish restores the stale value. Nothing later
 * notices, because F_SETFL stamps no new generation.
 *
 * Concretely: T1 snapshots a blocking pipe on its way into dup(). T2 sets
 * O_NONBLOCK and sweeps the aliases that exist. T1 publishes. The new name
 * reports blocking and waits in io_xfer where the guest asked for EAGAIN, on a
 * description whose other names are nonblocking.
 *
 * The comparison happens only at quiescence, and that is the whole design of
 * this test. Reading two names while a third thread is still flipping the flag
 * cannot tell a stale alias from a value that changed between the two reads --
 * an earlier version of this test did exactly that and reported disagreements
 * on a tree that had none. So each round dups a batch, parks the flipper, and
 * waits for it to acknowledge; only then must every name agree, because no
 * writer remains and the last sweep covered every slot that existed.
 *
 * Passes on real Linux, where one description simply has one flag.
 *
 * Syscalls exercised: pipe2(59), dup(23), fcntl(25), close(57), clone(220),
 *                     futex(98), sched_yield(124)
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

/* 700 x 24 dups, about two seconds. The window is a few instructions wide, so
 * one round hits it rarely: at 60 rounds a deliberately broken tree was caught
 * in one run out of three, which is not a gate. Sized from that measurement so
 * a regression is caught essentially every run, and the loop exits as soon as
 * it finds one.
 */
#define ROUNDS 700
#define BATCH 24

static int pipe_rd, pipe_wr;

/* pause_ack counts both edges of each request. Main waits for the flipper to
 * observe the release before requesting the next pause, so one wait cannot
 * absorb two rounds.
 */
static atomic_int stop, pause_req;
static atomic_uint pause_ack;
static int stale_round = -1, stale_alias = -1, stale_src = -1;

static void *flipper(void *arg)
{
    (void) arg;
    while (!atomic_load(&stop)) {
        if (atomic_load(&pause_req)) {
            /* Publish the ack only once per request, after the last write. */
            atomic_fetch_add(&pause_ack, 1u);
            while (atomic_load(&pause_req) && !atomic_load(&stop))
                sched_yield();
            atomic_fetch_add(&pause_ack, 1u);
            continue;
        }
        int fl = fcntl(pipe_wr, F_GETFL);
        if (fl >= 0)
            (void) fcntl(pipe_wr, F_SETFL, fl ^ O_NONBLOCK);
    }
    atomic_fetch_add(&pause_ack, 1u);
    return NULL;
}

int main(void)
{
    int fds[2];
    if (pipe(fds) != 0) {
        FAIL("pipe failed");
        SUMMARY("test-dup-setfl-race");
        return 1;
    }
    pipe_rd = fds[0];
    pipe_wr = fds[1];

    pthread_t t;
    if (pthread_create(&t, NULL, flipper, NULL) != 0) {
        FAIL("pthread_create failed");
        SUMMARY("test-dup-setfl-race");
        return 1;
    }

    int dups[BATCH];
    int dups_taken = 0;

    for (int r = 0; r < ROUNDS && stale_round < 0; r++) {
        int n = 0;
        for (int i = 0; i < BATCH; i++) {
            int d = dup(pipe_wr);
            if (d >= 0)
                dups[n++] = d;
        }

        /* Park the flipper and wait for it to say so. From here nothing can
         * write the flag, so any name that still disagrees kept a value the
         * description has moved past.
         */
        unsigned want = atomic_load(&pause_ack) + 1u;
        atomic_store(&pause_req, 1);
        while (atomic_load(&pause_ack) < want && !atomic_load(&stop))
            sched_yield();

        int src = fcntl(pipe_wr, F_GETFL);
        for (int i = 0; i < n; i++) {
            int alias = fcntl(dups[i], F_GETFL);
            if (src >= 0 && alias >= 0 && ((src ^ alias) & O_NONBLOCK) &&
                stale_round < 0) {
                stale_round = r;
                stale_alias = alias & O_NONBLOCK;
                stale_src = src & O_NONBLOCK;
            }
        }

        atomic_store(&pause_req, 0);
        want++;
        while (atomic_load(&pause_ack) < want && !atomic_load(&stop))
            sched_yield();
        for (int i = 0; i < n; i++)
            close(dups[i]);
        dups_taken += n;
    }

    atomic_store(&stop, 1);
    atomic_store(&pause_req, 0);
    pthread_join(t, NULL);

    if (stale_round >= 0)
        printf("    round %d: alias O_NONBLOCK=%d, description=%d\n",
               stale_round, stale_alias ? 1 : 0, stale_src ? 1 : 0);

    TEST("every dup taken during an F_SETFL sweep sees the description's flag");
    EXPECT_TRUE(stale_round < 0,
                "a dup kept an O_NONBLOCK the description had moved past");

    /* Count what the rounds actually produced, not that they ran. Asserting on
     * a counter the loop bumps unconditionally says only that the loop body
     * executed, which the loop condition already guarantees; a dup() failing
     * every time would still have passed it.
     */
    TEST("the race window was actually exercised");
    EXPECT_TRUE(dups_taken >= BATCH,
                "no dup was taken, so nothing was compared");

    close(pipe_rd);
    close(pipe_wr);
    SUMMARY("test-dup-setfl-race");
    return fails ? 1 : 0;
}
