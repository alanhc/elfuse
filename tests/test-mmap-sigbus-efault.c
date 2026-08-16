/*
 * MAP_SHARED host SIGBUS recovery regression test
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * elfuse backs a MAP_SHARED file mapping with a live host mmap overlay onto the
 * guest slab. Truncating the file leaves the overlay in place with no backing
 * page, so every host-side access from user mode raises SIGBUS and, without a
 * recovery pad, kills elfuse itself. Each case below drives one host path that
 * dereferences guest memory directly and expects EFAULT instead.
 *
 * Host syscall buffers are deliberately absent: the kernel reports EFAULT from
 * copyin/copyout rather than signaling, so read/write/sendmsg never needed a
 * pad.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

/* Map one page MAP_SHARED, then truncate the file out from under it.
 *
 * This only exercises the recovery pad while elfuse serves the mapping through
 * a live host file overlay, which it installs for MAP_SHARED at a
 * host-page-aligned slab offset and file offset. Offset 0 keeps the file side
 * aligned and the mmap gap finder keeps the slab side aligned. Should either
 * stop holding, the mapping falls back to snapshot pread, no host page can
 * vanish, and every case here passes without proving anything.
 *
 * Returns the mapping, or NULL after reporting the failure. On success *fd_out
 * holds the still-open file descriptor.
 */
static char *map_then_truncate(size_t page, int *fd_out)
{
    char path[] = "/tmp/elfuse-mmap-sigbus-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        FAIL("mkstemp");
        return NULL;
    }
    unlink(path);

    if (ftruncate(fd, (off_t) page) < 0) {
        close(fd);
        FAIL("initial ftruncate");
        return NULL;
    }

    char *p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        FAIL("mmap");
        return NULL;
    }
    memset(p, 0, page);
    strcpy(p, "/tmp/elfuse-never-opened");

    /* Truncate from a child so the mapping in this process stays live. */
    pid_t pid = fork();
    if (pid == 0)
        _exit(ftruncate(fd, 0) == 0 ? 0 : 1);
    if (pid < 0) {
        munmap(p, page);
        close(fd);
        FAIL("fork");
        return NULL;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        munmap(p, page);
        close(fd);
        FAIL("child truncate");
        return NULL;
    }

    *fd_out = fd;
    return p;
}

/* guest_read_str, through the path argument of open(2). */
static void test_path_copy(void)
{
    TEST("path copy from truncated MAP_SHARED returns EFAULT");

    const size_t page = (size_t) sysconf(_SC_PAGESIZE);
    int fd = -1;
    char *p = map_then_truncate(page, &fd);
    if (!p)
        return;

    errno = 0;
    int got = open(p, O_RDONLY);
    if (got >= 0)
        close(got);

    EXPECT_TRUE(got < 0 && errno == EFAULT,
                "open path copy should fail with EFAULT after truncate");

    munmap(p, page);
    close(fd);
}

/* The futex word atomics, which run from host user mode. */
static void test_futex_word(void)
{
    TEST("futex on truncated MAP_SHARED returns EFAULT");

    const size_t page = (size_t) sysconf(_SC_PAGESIZE);
    int fd = -1;
    char *p = map_then_truncate(page, &fd);
    if (!p)
        return;

    /* FUTEX_WAIT compares before it decides anything, so the read that faults
     * happens whatever value is passed.
     *
     * 0 is deliberately a value the word cannot hold: map_then_truncate leaves
     * the poison path string at offset 0, so the word reads as "/tmp". A page
     * that has not actually lost its backing therefore answers EAGAIN and fails
     * the assertion below, where an expected value that matched would enqueue a
     * waiter and hang the suite instead.
     */
    errno = 0;
    long rc =
        syscall(SYS_futex, (int *) p, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);

    EXPECT_TRUE(rc < 0 && errno == EFAULT,
                "FUTEX_WAIT should fail with EFAULT after truncate");

    munmap(p, page);
    close(fd);
}

int main(void)
{
    test_path_copy();
    test_futex_word();
    SUMMARY("test-mmap-sigbus-efault");
    return fails ? 1 : 0;
}
