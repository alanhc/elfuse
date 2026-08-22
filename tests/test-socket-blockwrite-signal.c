/*
 * A guest blocked writing to a full socket still takes a signal
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * elfuse owns O_NONBLOCK on the sockets it opens and emulates the guest's
 * blocking semantics on top, the same way it does for pipes. Before that, the
 * socket paths relied on passing MSG_DONTWAIT per call, which macOS does not
 * honour on AF_UNIX: a send on a full stream socket writes what fits and then
 * blocks in the kernel for the rest with the flag set. Measured on the host
 * outside elfuse, one send moved 8192 bytes and sat for three seconds.
 *
 * A vCPU thread parked in that send is reachable by neither hv_vcpus_exit nor
 * the wakeup pipe, so no guest signal is delivered and an execve teardown
 * counts it as a sibling that will not leave. This test is the guard: fill a
 * socket nobody reads, write to it from a thread that has a one-second alarm
 * pending, and require both that the handler runs and that the write returns.
 *
 * Real Linux passes this too, which is the point: it encodes Linux behaviour
 * rather than elfuse's. Linux may answer either way and both are accepted --
 * the partial count when bytes moved before the signal, or EINTR when none did.
 *
 * Syscalls exercised: socketpair(199), write(64), setsockopt(208),
 *                     rt_sigaction(134), setitimer(103), nanosleep(101)
 */

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

#define BIG (1u << 20)

static volatile sig_atomic_t handler_ran;

static void on_alarm(int sig)
{
    (void) sig;
    handler_ran = 1;
}

int main(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL("socketpair failed");
        SUMMARY("test-socket-blockwrite-signal");
        return 1;
    }

    char *buf = malloc(BIG);
    if (!buf) {
        FAIL("malloc failed");
        SUMMARY("test-socket-blockwrite-signal");
        return 1;
    }
    memset(buf, 'z', BIG);

    /* Bound both buffers so the write is certain to fill them. Without this the
     * test asserts something about the platform's socket sizing rather than
     * about elfuse.
     */
    int bufsz = 8192;
    if (setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz)) != 0)
        FAIL("SO_SNDBUF failed");
    if (setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz)) != 0)
        FAIL("SO_RCVBUF failed");

    /* No SA_RESTART: the whole question is whether the write comes back at all,
     * and a restarting handler would hide a failure by re-entering the same
     * wait.
     */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    if (sigaction(SIGALRM, &sa, NULL) != 0)
        FAIL("sigaction failed");

    /* Nobody reads sv[1], so this write cannot complete. The socket is left
     * blocking, which is the case that used to park the vCPU.
     */
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = 1;
    if (setitimer(ITIMER_REAL, &it, NULL) != 0)
        FAIL("setitimer failed");

    TEST("a blocking socket write is interruptible");
    ssize_t n = write(sv[0], buf, BIG);
    int werr = errno;

    /* Either answer is Linux's: the count already moved, or EINTR when the
     * signal arrived before any byte did.
     */
    EXPECT_TRUE((n > 0 && n < (ssize_t) BIG) || (n < 0 && werr == EINTR),
                "write neither reported a partial count nor EINTR");

    TEST("the signal handler ran while the write was waiting");
    EXPECT_TRUE(handler_ran, "SIGALRM was never delivered");

    free(buf);
    close(sv[0]);
    close(sv[1]);
    SUMMARY("test-socket-blockwrite-signal");
    return fails ? 1 : 0;
}
