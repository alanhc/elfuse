/*
 * SIGPIPE reaches the guest, and only the guest
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two properties, both of which were broken.
 *
 * A host write to a pipe with no reader raises SIGPIPE in the elfuse process
 * itself, whose default action is to die. Every such write is made on the
 * guest's behalf, so the whole VM went down where the guest should merely have
 * seen EPIPE: an ordinary pipeline whose reader exits first was enough.
 *
 * And a write that moved some bytes before the reader vanished reports the
 * count, which Linux accompanies with SIGPIPE (pipe_write raises it even when
 * it has something to return). elfuse reported the count silently, because the
 * value the write path sees is a non-negative number with no errno attached.
 *
 * Syscalls exercised: pipe2(59), write(64), read(63), close(57),
 *                     rt_sigaction(134), clone(220)
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

#define BIG (4u << 20)

static volatile sig_atomic_t sigpipes;
static int fds[2];

static void on_pipe(int signo)
{
    (void) signo;
    sigpipes++;
}

/* Drain a little, then close: the write is already under way and has moved
 * bytes when its reader disappears.
 */
static void *closer(void *arg)
{
    (void) arg;
    char buf[4096];
    for (int i = 0; i < 8; i++) {
        if (read(fds[0], buf, sizeof(buf)) <= 0)
            break;
    }
    usleep(20000);
    close(fds[0]);
    return NULL;
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_pipe;
    sigaction(SIGPIPE, &sa, NULL);

    /* No reader at all: the write fails outright and the guest is signalled.
     * Reaching this line at all is the first property -- before the fix the
     * process running this test was killed by the host's own SIGPIPE.
     */
    int solo[2];
    if (pipe(solo) != 0) {
        FAIL("pipe failed");
        SUMMARY("test-sigpipe");
        return 1;
    }
    close(solo[0]);
    sigpipes = 0;
    ssize_t n = write(solo[1], "x", 1);
    TEST("a write with no reader reports EPIPE");
    EXPECT_ERRNO(n, EPIPE, "write did not report EPIPE");
    TEST("and raises SIGPIPE in the guest");
    EXPECT_EQ(sigpipes, 1, "no SIGPIPE delivered");
    close(solo[1]);

    /* Bytes moved, then the reader leaves: Linux returns the partial count and
     * signals anyway.
     */
    if (pipe(fds) != 0) {
        FAIL("pipe failed");
        SUMMARY("test-sigpipe");
        return 1;
    }
    char *buf = malloc(BIG);
    if (!buf) {
        FAIL("malloc failed");
        SUMMARY("test-sigpipe");
        return 1;
    }
    memset(buf, 'z', BIG);

    sigpipes = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, closer, NULL) != 0) {
        FAIL("pthread_create failed");
        SUMMARY("test-sigpipe");
        return 1;
    }
    ssize_t moved = write(fds[1], buf, BIG);
    pthread_join(t, NULL);

    TEST("a partial write reports what it moved");
    EXPECT_TRUE(moved > 0 && moved < (ssize_t) BIG,
                "write did not report a partial count");
    TEST("a partial write still raises SIGPIPE");
    EXPECT_EQ(sigpipes, 1, "no SIGPIPE for the interrupted stream");

    free(buf);
    close(fds[1]);
    SUMMARY("test-sigpipe");
    return fails > 0 ? 1 : 0;
}
