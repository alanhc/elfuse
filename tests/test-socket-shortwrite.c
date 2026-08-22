/*
 * A nonblocking socket write reports what it moved
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * elfuse owns O_NONBLOCK on the sockets it opens, so a socket's guest-visible
 * flag lives in the fd_entry_t.linux_flags shadow and the transfer loop asks
 * that shadow whether the guest wanted to wait.
 *
 * If it asks wrongly, a nonblocking socket write that fills the send buffer
 * waits for a reader that may never come, which is a hang rather than a wrong
 * answer. This test is the guard: it fills a socket nobody reads and requires
 * the write to come back with what it moved.
 *
 * The blocking half of the same question is
 * tests/test-socket-blockwrite-signal.c.
 *
 * Syscalls exercised: socketpair(199), write(64), read(63), fcntl(25),
 *                     setsockopt(208)
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

#define BIG (1u << 20)

int main(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL("socketpair failed");
        SUMMARY("test-socket-shortwrite");
        return 1;
    }

    char *buf = malloc(BIG);
    if (!buf) {
        FAIL("malloc failed");
        SUMMARY("test-socket-shortwrite");
        return 1;
    }
    memset(buf, 'z', BIG);

    /* Bound the send buffer rather than trusting the platform default to be
     * smaller than BIG: without this the short count and the EAGAIN below are
     * claims about the host's socket sizing, not about elfuse.
     */
    int sndbuf = 8192;
    if (setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) != 0)
        FAIL("SO_SNDBUF failed");
    int rcvbuf = 8192;
    if (setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) != 0)
        FAIL("SO_RCVBUF failed");

    /* Nonblocking, and nobody is reading sv[1]. The first write can only move
     * what the send buffer holds.
     */
    if (fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK) != 0)
        FAIL("F_SETFL O_NONBLOCK failed");

    TEST("a nonblocking socket write returns instead of waiting");
    ssize_t n = write(sv[0], buf, BIG);
    EXPECT_TRUE(n > 0 && n < (ssize_t) BIG,
                "write did not report a short count");

    /* And again once the buffer is full: EAGAIN, still no wait. */
    TEST("a full nonblocking socket reports EAGAIN");
    ssize_t again;
    do {
        again = write(sv[0], buf, BIG);
    } while (again > 0);
    EXPECT_ERRNO(again, EAGAIN, "write did not report EAGAIN");

    /* The bytes are really there: drain what the writes claimed. */
    char sink[4096];
    size_t drained = 0;
    for (;;) {
        ssize_t r = read(sv[1], sink, sizeof(sink));
        if (r <= 0)
            break;
        drained += (size_t) r;
        if (drained >= (size_t) n)
            break;
    }
    TEST("the reported bytes are readable from the peer");
    EXPECT_TRUE(drained >= (size_t) n, "peer saw fewer bytes than reported");

    free(buf);
    close(sv[0]);
    close(sv[1]);
    SUMMARY("test-socket-shortwrite");
    return fails > 0 ? 1 : 0;
}
