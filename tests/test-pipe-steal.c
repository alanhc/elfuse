/*
 * Readiness-poll steal: a reader that loses the race must not park a vCPU
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * elfuse waits for readiness before every blocking read or write, and the wait
 * reserves nothing. With several threads on one pipe, one byte per wakeup, all
 * of them come back from the wait ready and one takes the byte; the losers then
 * transfer. If that transfer is a plain blocking host call it parks the vCPU
 * thread where neither hv_vcpus_exit nor the wakeup pipe reaches it, and the
 * execve teardown counts it as a sibling that would not leave and kills the
 * process (exit 128) instead of running the new image.
 *
 * The exec chain is what tests it: READERS threads share one blocking pipe, a
 * writer feeds single bytes, and once the writer stops the losers are the ones
 * left holding a transfer that will never complete. Then the process execs
 * itself and checks the facts Linux guarantees in the new image.
 *
 * Iteration 0 also checks the other half of the contract: making the transfer
 * non-blocking must not leak short writes to the guest. A blocking write(2)
 * moves every byte, so a write and a writev larger than the pipe buffer must
 * still report the full count, in order, with nothing dropped or repeated.
 *
 * Syscalls exercised: execve(221), clone(220), read(63), write(64),
 *                     writev(66), pipe2(59), fcntl(25), ioctl(29),
 *                     gettid(178), getpid(172), socketpair(199),
 *                     sendmsg(211), recvmsg(212), ppoll(73)
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

/* drain_worker runs while main is also reporting, so its failures go through an
 * atomic of its own and are folded in after the join.
 */
static atomic_int worker_fails;

#define READERS 4
#define TOTAL_EXECS 40
#define BIG_WRITE (1u << 20) /* well past any pipe buffer */

static atomic_int ready;
static atomic_int exec_now;
static int pipe_rd = -1, pipe_wr = -1;
static const char *self_path;
static int g_iter;

/* Byte i of the stream the full-write checks send. */
static uint8_t stream_byte(size_t i)
{
    return (uint8_t) (i * 7u + 3u);
}

/* Reader half of the full-write check: drains the pipe in small bites and
 * verifies every byte arrives once, in order. A partial write reassembled with
 * a bad offset shows up here as a mismatch, not as a lost byte count.
 */
static void *drain_worker(void *arg)
{
    size_t want = (size_t) (uintptr_t) arg;
    size_t seen = 0;
    uint8_t buf[4096];

    while (seen < want) {
        /* Wait with a deadline rather than blocking outright. A writer that
         * reports the full count but moves less leaves this thread waiting for
         * bytes nobody will send, and main is joining it: without the deadline
         * the whole test hangs and says nothing, which is how a truncated
         * blocking write hid here once already.
         */
        struct pollfd pfd = {.fd = pipe_rd, .events = POLLIN};
        int pr = poll(&pfd, 1, 5000);
        if (pr == 0) {
            printf("\nstalled after %zu of %zu bytes\n", seen, want);
            atomic_fetch_add(&worker_fails, 1);
            return NULL;
        }
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        ssize_t n = read(pipe_rd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            /* Both writes send the same BIG_WRITE pattern, so the expected byte
             * wraps at that boundary.
             */
            if (buf[i] != stream_byte((seen + (size_t) i) % BIG_WRITE)) {
                printf("\nstream mismatch at byte %zu\n", seen + (size_t) i);
                atomic_fetch_add(&worker_fails, 1);
                return NULL;
            }
        }
        seen += (size_t) n;

        /* Keep the writer against a full pipe rather than a drained one. */
        usleep(200);
    }

    if (seen != want) {
        printf("\ndrained %zu bytes, want %zu\n", seen, want);
        atomic_fetch_add(&worker_fails, 1);
    }
    return NULL;
}

/* A blocking write moves every byte it was given. Send more than the pipe
 * buffer holds, once through write() and once through writev(), and require the
 * full count both times.
 */
static void check_full_write(void)
{
    uint8_t *buf = malloc(BIG_WRITE);
    if (!buf) {
        FAIL("malloc");
        return;
    }
    for (size_t i = 0; i < BIG_WRITE; i++)
        buf[i] = stream_byte(i);

    int fds[2];
    if (pipe(fds) != 0) {
        FAIL("pipe");
        free(buf);
        return;
    }
    pipe_rd = fds[0];
    pipe_wr = fds[1];

    size_t split = BIG_WRITE / 3;
    pthread_t drain;
    if (pthread_create(&drain, NULL, drain_worker,
                       (void *) (uintptr_t) (BIG_WRITE + BIG_WRITE)) != 0) {
        FAIL("pthread_create");
        goto out;
    }

    TEST("blocking write moves every byte");
    ssize_t w = write(pipe_wr, buf, BIG_WRITE);
    EXPECT_EQ(w, (ssize_t) BIG_WRITE, "short write on a blocking pipe");

    /* The writev sends the same pattern a second time, split across three
     * segments, so a partial write reassembled at the wrong offset shows up as
     * a mismatch in the reader rather than only as a short count.
     */
    struct iovec iov[3] = {
        {.iov_base = buf, .iov_len = split},
        {.iov_base = buf + split, .iov_len = split},
        {.iov_base = buf + 2 * split, .iov_len = BIG_WRITE - 2 * split},
    };
    TEST("blocking writev moves every byte");
    ssize_t wv = writev(pipe_wr, iov, 3);
    EXPECT_EQ(wv, (ssize_t) BIG_WRITE, "short writev on a blocking pipe");

    pthread_join(drain, NULL);
    fails += atomic_load(&worker_fails);

out:
    close(pipe_wr);
    close(pipe_rd);
    pipe_rd = pipe_wr = -1;
    free(buf);
}

/* Reading an empty pipe is how the guest observes its own O_NONBLOCK. */
static void expect_eagain_read(int fd, const char *what)
{
    char c;
    TEST(what);
    EXPECT_ERRNO(read(fd, &c, 1), EAGAIN, "read did not report EAGAIN");
}

/* elfuse keeps O_NONBLOCK set on the host pipe so a transfer can report EAGAIN
 * instead of parking, and answers the guest from its own shadow of the flag.
 * The guest must see exactly what it asked for, through either spelling.
 */
static void check_nonblock_view(void)
{
    int fds[2];
    if (pipe2(fds, O_NONBLOCK) != 0) {
        FAIL("pipe2(O_NONBLOCK)");
        return;
    }

    TEST("pipe2(O_NONBLOCK) is visible");
    EXPECT_TRUE(fcntl(fds[0], F_GETFL) & O_NONBLOCK, "F_GETFL lost O_NONBLOCK");

    expect_eagain_read(fds[0], "nonblocking read of an empty pipe");
    close(fds[0]);
    close(fds[1]);

    if (pipe(fds) != 0) {
        FAIL("pipe");
        return;
    }

    TEST("a plain pipe reads as blocking");
    EXPECT_TRUE((fcntl(fds[0], F_GETFL) & O_NONBLOCK) == 0,
                "F_GETFL invented O_NONBLOCK");

    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    expect_eagain_read(fds[0], "F_SETFL O_NONBLOCK takes effect");

    /* ioctl is the other spelling libuv uses on pipes. */
    int off = 0, on = 1;
    ioctl(fds[0], FIONBIO, &off);
    TEST("FIONBIO clears the guest's O_NONBLOCK");
    EXPECT_TRUE((fcntl(fds[0], F_GETFL) & O_NONBLOCK) == 0,
                "F_GETFL still reports O_NONBLOCK");

    ioctl(fds[0], FIONBIO, &on);
    expect_eagain_read(fds[0], "FIONBIO sets it back");

    close(fds[0]);
    close(fds[1]);
}

/* O_NONBLOCK lives on the open file description, so every dup alias observes a
 * change made through any of them. elfuse owns the host flag on a pipe and
 * answers from its own shadow, which is per-fd, so this is the case that shadow
 * has to keep in step.
 */
static void check_dup_alias_flags(void)
{
    int fds[2];
    if (pipe(fds) != 0) {
        FAIL("pipe");
        return;
    }
    int alias = dup(fds[0]);
    if (alias < 0) {
        FAIL("dup");
        close(fds[0]);
        close(fds[1]);
        return;
    }

    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    TEST("F_SETFL reaches a dup alias");
    EXPECT_TRUE(fcntl(alias, F_GETFL) & O_NONBLOCK,
                "the alias still reads as blocking");

    expect_eagain_read(alias, "the alias transfers nonblocking too");

    /* And back the other way, through the other spelling. */
    int off = 0;
    ioctl(alias, FIONBIO, &off);
    TEST("FIONBIO on the alias reaches the original");
    EXPECT_TRUE((fcntl(fds[0], F_GETFL) & O_NONBLOCK) == 0,
                "the original still reads as nonblocking");

    close(alias);
    close(fds[0]);
    close(fds[1]);
}

/* Send one fd over a socketpair to ourselves and return what came back, or -1.
 * The caller owns the result.
 */
static int scm_roundtrip(int fd)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return -1;

    char cbuf[CMSG_SPACE(sizeof(int))] = {0}, data = 'x';
    struct iovec iov = {.iov_base = &data, .iov_len = 1};
    struct msghdr m = {.msg_iov = &iov,
                       .msg_iovlen = 1,
                       .msg_control = cbuf,
                       .msg_controllen = sizeof(cbuf)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd));
    if (sendmsg(sv[0], &m, 0) < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    char rbuf[8], rc[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec riov = {.iov_base = rbuf, .iov_len = sizeof(rbuf)};
    struct msghdr rm = {.msg_iov = &riov,
                        .msg_iovlen = 1,
                        .msg_control = rc,
                        .msg_controllen = sizeof(rc)};
    int got = -1;
    if (recvmsg(sv[1], &rm, 0) > 0) {
        struct cmsghdr *rcm = CMSG_FIRSTHDR(&rm);
        if (rcm && rcm->cmsg_type == SCM_RIGHTS)
            memcpy(&got, CMSG_DATA(rcm), sizeof(got));
    }
    close(sv[0]);
    close(sv[1]);
    return got;
}

static void *late_writer(void *arg)
{
    usleep(50000);
    ssize_t n = write(*(int *) arg, "y", 1);
    (void) n;
    return NULL;
}

/* The received end of the same pipe must still read as blocking.
 *
 * elfuse keeps O_NONBLOCK on the host end of every pipe it opens and emulates
 * the wait on top, so a descriptor arriving over SCM_RIGHTS from another elfuse
 * process is found already nonblocking. Reporting that flag as the guest's view
 * would give a plain read EAGAIN on a pipe the guest never set nonblocking,
 * which is fatal to a program that has no handling for it. The receive path
 * adopts the emulation instead, which sets nothing on a description it did not
 * create.
 */
static void check_scm_recv_blocking(void)
{
    int fds[2];
    if (pipe(fds) != 0) {
        FAIL("pipe failed");
        return;
    }

    int rfd = scm_roundtrip(fds[0]);
    if (rfd < 0) {
        FAIL("SCM_RIGHTS roundtrip failed");
        close(fds[0]);
        close(fds[1]);
        return;
    }

    TEST("a received pipe end reads as blocking");
    EXPECT_TRUE((fcntl(rfd, F_GETFL) & O_NONBLOCK) == 0,
                "the received fd reports O_NONBLOCK the guest never set");

    /* And behaves that way: with the data 50ms out, a blocking read waits for
     * it and a leaked O_NONBLOCK returns EAGAIN at once.
     */
    pthread_t t;
    if (pthread_create(&t, NULL, late_writer, &fds[1]) != 0) {
        FAIL("pthread_create failed");
    } else {
        char c = 0;
        ssize_t n = read(rfd, &c, 1);
        TEST("a read on a received pipe end waits for the data");
        EXPECT_TRUE(n == 1 && c == 'y', "read did not return the late byte");
        pthread_join(t, NULL);
    }

    close(rfd);
    close(fds[0]);
    close(fds[1]);
}

/* Ask elfuse to duplicate the stdin it was handed, so the host side can check
 * that nothing about elfuse's own use of O_NONBLOCK reached the description its
 * launcher owns. See tests/test-stdio-nonblock-host.c. Pass stdin to ourselves
 * over a socketpair. The description that comes back is the launcher's, reached
 * through the one fd-creating path that has no source slot to inherit from, so
 * it is the path most likely to take ownership of a description elfuse did not
 * create.
 */
static int scm_pass_stdin_and_exit(void)
{
    int got = scm_roundtrip(0);
    if (got < 0)
        return 1;
    close(got);
    return 0;
}

static int dup_stdin_and_exit(bool via_fork)
{
    /* A magic link is served by dup()ing the descriptor the process already
     * holds, so it aliases the launcher's description exactly as dup(0) does,
     * through a different path.
     */
    int ml = open("/dev/stdin", O_RDONLY);
    if (ml >= 0)
        close(ml);

    int a = dup(0);
    int b = fcntl(0, F_DUPFD, 20);
    if (a < 0 || b < 0)
        return 1;

    /* An alias of an alias still names the launcher's description. */
    int c = dup(a);
    if (c < 0)
        return 1;
    if (dup2(0, 9) < 0)
        return 1;

    /* The fork path rebuilds the child's fd table from scratch, so the alias
     * has to survive that too: the child re-allocates a slot for the same
     * description the launcher owns.
     */
    if (via_fork) {
        pid_t pid = fork();
        if (pid < 0)
            return 1;
        if (pid == 0)
            _exit(0);
        int status = 0;
        if (waitpid(pid, &status, 0) < 0)
            return 1;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
    }
    return 0;
}

static void exec_next(void)
{
    fflush(stdout);
    fflush(stderr);

    char iterbuf[16], pidbuf[16];
    snprintf(iterbuf, sizeof(iterbuf), "%d", g_iter + 1);
    snprintf(pidbuf, sizeof(pidbuf), "%d", (int) getpid());

    extern char **environ;
    char *argv[] = {(char *) self_path, iterbuf, pidbuf, NULL};
    execve(self_path, argv, environ);

    fprintf(stderr, "\ntest-pipe-steal: execve(%s) failed at iter %d (%s)\n",
            self_path, g_iter, strerror(errno));
    _exit(1);
}

/* Every reader wants the same single byte. Whoever loses is left holding a
 * transfer that only the next write can satisfy, and the writer stops before
 * the exec.
 */
static void *steal_reader(void *arg)
{
    (void) arg;
    atomic_fetch_add(&ready, 1);
    for (;;) {
        char c;
        ssize_t n = read(pipe_rd, &c, 1);
        if (n == 0 || (n < 0 && errno != EINTR))
            return NULL;
    }
}

static void *feeder(void *arg)
{
    (void) arg;
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&exec_now)) {
        char c = 'x';
        if (write(pipe_wr, &c, 1) != 1)
            return NULL;
        usleep(100);
    }
    return NULL;
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

/* Fail fast: a per-iteration PASS line would print TOTAL_EXECS times. */
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

static void spawn_contenders(void)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        FAIL("pipe2 failed");
        exit(1);
    }
    pipe_rd = fds[0];
    pipe_wr = fds[1];

    atomic_store(&ready, 0);
    atomic_store(&exec_now, 0);

    for (int i = 0; i < READERS; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, steal_reader, NULL) != 0) {
            FAIL("pthread_create failed");
            exit(1);
        }
        pthread_detach(t);
    }

    pthread_t w;
    if (pthread_create(&w, NULL, feeder, NULL) != 0) {
        FAIL("pthread_create failed");
        exit(1);
    }
    pthread_detach(w);

    while (atomic_load(&ready) < READERS + 1)
        sched_yield();
}

int main(int argc, char **argv)
{
    self_path = argv[0];
    if (argc > 1 && strcmp(argv[1], "dupstdin") == 0)
        return dup_stdin_and_exit(false);
    if (argc > 1 && strcmp(argv[1], "dupstdin-fork") == 0)
        return dup_stdin_and_exit(true);
    if (argc > 1 && strcmp(argv[1], "scmpass") == 0)
        return scm_pass_stdin_and_exit();

    int iter = argc > 1 ? atoi(argv[1]) : 0;

    if (iter == 0) {
        printf(
            "test-pipe-steal: %d readers contending for one byte, %d execs\n",
            READERS, TOTAL_EXECS);
        check_nonblock_view();
        check_dup_alias_flags();
        check_scm_recv_blocking();
        check_full_write();
        if (fails > 0)
            return 1;
        TEST("exec chain under a contended pipe");
    } else {
        check_post_exec(iter, argc > 2 ? atoi(argv[2]) : 0);
        if (fails > 0)
            return 1;
    }

    if (iter >= TOTAL_EXECS) {
        PASS();
        SUMMARY("test-pipe-steal");
        return fails > 0 ? 1 : 0;
    }

    g_iter = iter;
    spawn_contenders();

    /* Let the readers cycle through the contended wakeup, then cut the feed so
     * the losers stay where they are, and exec on top of them.
     */
    usleep(20000);
    atomic_store(&exec_now, 1);
    usleep(2000);
    exec_next();
    return 1;
}
