/*
 * A blocked synthetic reader can still take a signal
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * eventfd, signalfd and timerfd all waited by parking in a host call: two
 * flipped their own pipe to blocking and read() it, and one sat in kevent()
 * with no timeout. A vCPU thread parked there is reachable by neither
 * hv_vcpus_exit nor the wakeup pipe, so a guest signal could not interrupt it
 * and an execve teardown counted it as a sibling that would not leave -- the
 * failure this tree removed from every transfer path, left in place on the
 * synthetic ones.
 *
 * Each case blocks on an fd nobody will ever make ready, arranges a signal, and
 * requires the read to come back. Before the fix the read never returned and
 * the alarm never arrived; the test simply hung.
 *
 * Syscalls exercised: eventfd2(19), signalfd4(74), timerfd_create(85),
 *                     read(63), rt_sigaction(134), setitimer(103)
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

static volatile sig_atomic_t fired;

static void on_alarm(int signo)
{
    (void) signo;
    fired++;
}

/* Block reading fd, with a 300 ms alarm pending. The read must return: EINTR if
 * the wait was interrupted, or a short count if the handler ran first and the
 * dispatcher restarted it into a ready fd. Hanging is the failure.
 */
static void check_interruptible(const char *what, int fd)
{
    if (fd < 0) {
        TEST(what);
        FAIL("could not create the fd");
        return;
    }

    fired = 0;
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_usec = 300000;
    setitimer(ITIMER_REAL, &it, NULL);

    uint64_t buf[16];
    ssize_t n = read(fd, buf, sizeof(buf));
    int saved = errno;

    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_REAL, &it, NULL);

    TEST(what);
    EXPECT_TRUE(n >= 0 || saved == EINTR || saved == EAGAIN,
                "read failed for a reason other than the signal");
    close(fd);
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm; /* no SA_RESTART: this one interrupts */
    sigaction(SIGALRM, &sa, NULL);

    /* Nothing will ever write these, so the read blocks until the signal. */
    check_interruptible("a blocked eventfd read takes a signal", eventfd(0, 0));

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2); /* never raised here */
    check_interruptible("a blocked signalfd read takes a signal",
                        signalfd(-1, &mask, 0));

    /* Armed far enough out that only the alarm can end the wait. */
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd >= 0) {
        struct itimerspec far;
        memset(&far, 0, sizeof(far));
        far.it_value.tv_sec = 3600;
        timerfd_settime(tfd, 0, &far, NULL);
    }
    check_interruptible("a blocked timerfd read takes a signal", tfd);

    TEST("the alarm was delivered at least once");
    EXPECT_TRUE(fired > 0, "no SIGALRM reached the guest");

    SUMMARY("test-synthetic-wait-signal");
    return fails > 0 ? 1 : 0;
}
