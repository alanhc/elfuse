/*
 * fd magic link resolution regression test
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Covers the paths Linux serves through a procfs magic symlink
 * ("/proc/self/fd/<n>", the own-pid spelling, "/dev/fd/<n>"):
 *   1. A follow-style metadata call acts on the file the descriptor holds.
 *   2. It follows the descriptor, not a pathname resolved from it: the call
 *      still lands after the file has been unlinked, which no path can reach.
 *   3. A no-follow or create-style call does NOT act on that file, so
 *      unlink("/proc/self/fd/<n>") cannot delete it.
 *   4. Only names Linux's name_to_int() accepts resolve: no sign, no leading
 *      whitespace, no leading zero beyond "0" itself.
 *   5. open/stat/readlink keep answering from the existing /proc intercepts.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

#define TEST_FILE "/fd-magiclink.tmp"

/* Mode of the file behind fd, or (mode_t) -1. */
static mode_t fd_mode(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return (mode_t) -1;
    return st.st_mode & 07777;
}

static int make_test_file(mode_t mode)
{
    unlink(TEST_FILE);
    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -1;
    /* open() honours umask, so set the mode the test actually asked for. */
    if (fchmod(fd, mode) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* "/proc/self/fd/<fd>" and friends, built for the fd the caller holds. */
static const char *magic(const char *prefix, int fd)
{
    static char buf[128];
    snprintf(buf, sizeof(buf), "%s%d", prefix, fd);
    return buf;
}

static void check_follow_style_ops(void)
{
    int fd = make_test_file(0600);
    if (fd < 0) {
        TEST("fd magiclink: fixture");
        FAIL("could not create test file");
        return;
    }

    TEST("chmod through magic link");
    EXPECT_TRUE(
        chmod(magic("/proc/self/fd/", fd), 0644) == 0 && fd_mode(fd) == 0644,
        "chmod did not reach the descriptor's file");

    TEST("chmod through /dev/fd");
    EXPECT_TRUE(chmod(magic("/dev/fd/", fd), 0600) == 0 && fd_mode(fd) == 0600,
                "/dev/fd spelling did not resolve");

    TEST("chmod through own-pid spelling");
    char pid_prefix[64];
    snprintf(pid_prefix, sizeof(pid_prefix), "/proc/%d/fd/", (int) getpid());
    EXPECT_TRUE(chmod(magic(pid_prefix, fd), 0640) == 0 && fd_mode(fd) == 0640,
                "own-pid spelling did not resolve");

    /* A no-op ownership change exercises the chown path without needing any
     * privilege the test may not hold.
     */
    TEST("chown through magic link");
    EXPECT_TRUE(chown(magic("/proc/self/fd/", fd), (uid_t) -1, (gid_t) -1) == 0,
                "chown did not reach the descriptor's file");

    TEST("utimensat through magic link");
    struct timespec ts[2] = {{.tv_sec = 1000000000, .tv_nsec = 0},
                             {.tv_sec = 1000000000, .tv_nsec = 0}};
    struct stat st;
    EXPECT_TRUE(utimensat(AT_FDCWD, magic("/proc/self/fd/", fd), ts, 0) == 0 &&
                    fstat(fd, &st) == 0 && st.st_mtime == 1000000000,
                "utimensat did not reach the descriptor's file");

    close(fd);
    unlink(TEST_FILE);
}

/* The property a resolved pathname cannot have: once the file is unlinked no
 * path names it, so a call that still lands proves it followed the descriptor.
 */
static void check_follows_descriptor_not_path(void)
{
    int fd = make_test_file(0600);
    if (fd < 0) {
        TEST("fd magiclink: unlinked fixture");
        FAIL("could not create test file");
        return;
    }
    if (unlink(TEST_FILE) < 0) {
        TEST("fd magiclink: unlinked fixture");
        FAIL("could not unlink test file");
        close(fd);
        return;
    }

    TEST("chmod on unlinked held file");
    EXPECT_TRUE(
        chmod(magic("/proc/self/fd/", fd), 0644) == 0 && fd_mode(fd) == 0644,
        "chmod did not follow the descriptor past unlink");

    TEST("unlinked file has no links");
    struct stat st;
    EXPECT_TRUE(fstat(fd, &st) == 0 && st.st_nlink == 0,
                "fixture still had a name, so the check proved nothing");

    close(fd);
}

/* unlink and rename do not follow the final component, so on Linux they act on
 * the procfs entry and never on the file the descriptor holds.
 */
static void check_no_follow_ops_spare_the_target(void)
{
    int fd = make_test_file(0600);
    if (fd < 0) {
        TEST("fd magiclink: nofollow fixture");
        FAIL("could not create test file");
        return;
    }

    TEST("unlink through magic link refused");
    EXPECT_TRUE(unlink(magic("/proc/self/fd/", fd)) < 0,
                "unlink through a magic link was accepted");

    TEST("unlink left the target in place");
    struct stat st;
    EXPECT_TRUE(stat(TEST_FILE, &st) == 0,
                "unlink through a magic link deleted the target");

    TEST("rename through magic link refused");
    EXPECT_TRUE(rename(magic("/proc/self/fd/", fd), "/fd-magiclink.moved") < 0,
                "rename through a magic link was accepted");

    TEST("rename left the target in place");
    EXPECT_TRUE(stat(TEST_FILE, &st) == 0,
                "rename through a magic link moved the target");

    TEST("chmod AT_SYMLINK_NOFOLLOW spares the target");
    mode_t before = fd_mode(fd);
    fchmodat(AT_FDCWD, magic("/proc/self/fd/", fd), 0755, AT_SYMLINK_NOFOLLOW);
    EXPECT_TRUE(fd_mode(fd) == before,
                "a nofollow chmod reached the descriptor's file");

    close(fd);
    unlink(TEST_FILE);
    unlink("/fd-magiclink.moved");
}

/* Linux resolves both the pid and the fd component through name_to_int(), which
 * rejects a sign, leading whitespace, and a leading zero past "0" itself.
 */
static void check_strict_name_parsing(void)
{
    int fd = make_test_file(0600);
    if (fd < 0) {
        TEST("fd magiclink: name fixture");
        FAIL("could not create test file");
        return;
    }

    /* Spelled under /proc, where a name this parser rejects stops here. The
     * /dev/fd equivalents are deliberately left out: a rejected name there
     * falls through to generic path resolution, whose answer depends on what
     * the sysroot's own /dev holds rather than on the parser under test.
     */
    static const char *const bad_fd_prefixes[] = {
        "/proc/self/fd/+",
        "/proc/self/fd/0",
        "/proc/self/fd/ ",
    };
    bool all_rejected = true;
    for (size_t i = 0; i < sizeof(bad_fd_prefixes) / sizeof(*bad_fd_prefixes);
         i++) {
        if (chmod(magic(bad_fd_prefixes[i], fd), 0755) == 0)
            all_rejected = false;
    }
    TEST("malformed fd names rejected");
    EXPECT_TRUE(all_rejected && fd_mode(fd) == 0600,
                "a sign, space or leading zero resolved a descriptor");

    TEST("trailing garbage rejected");
    char buf[128];
    snprintf(buf, sizeof(buf), "/proc/self/fd/%dx", fd);
    EXPECT_TRUE(chmod(buf, 0755) < 0 && fd_mode(fd) == 0600,
                "a non-numeric suffix resolved a descriptor");

    TEST("walking through a magic link rejected");
    snprintf(buf, sizeof(buf), "/proc/self/fd/%d/child", fd);
    EXPECT_TRUE(chmod(buf, 0755) < 0 && fd_mode(fd) == 0600,
                "a path below a magic link resolved");

    TEST("malformed pid names rejected");
    bool pid_rejected = true;
    snprintf(buf, sizeof(buf), "/proc/+%d/fd/%d", (int) getpid(), fd);
    if (chmod(buf, 0755) == 0)
        pid_rejected = false;
    snprintf(buf, sizeof(buf), "/proc/0%d/fd/%d", (int) getpid(), fd);
    if (chmod(buf, 0755) == 0)
        pid_rejected = false;
    EXPECT_TRUE(pid_rejected && fd_mode(fd) == 0600,
                "a signed or zero-padded pid resolved");

    close(fd);
    unlink(TEST_FILE);
}

/* open, stat and readlink are served by the /proc intercepts, and the magic
 * link rewrite must not have taken them over.
 */
static void check_intercepts_unchanged(void)
{
    int fd = make_test_file(0600);
    if (fd < 0) {
        TEST("fd magiclink: intercept fixture");
        FAIL("could not create test file");
        return;
    }
    if (write(fd, "payload", 7) != 7) {
        TEST("fd magiclink: intercept fixture");
        FAIL("could not write test payload");
        close(fd);
        return;
    }

    TEST("open through magic link reopens");
    int reopened = open(magic("/proc/self/fd/", fd), O_RDONLY);
    char buf[16] = {0};

    /* Seek explicitly: elfuse serves this open by duplicating the descriptor,
     * so the result shares the original file offset rather than starting at 0
     * the way a fresh open on Linux would.
     */
    bool read_ok = reopened >= 0 && lseek(reopened, 0, SEEK_SET) == 0 &&
                   read(reopened, buf, sizeof(buf) - 1) == 7 &&
                   !strcmp(buf, "payload");
    if (reopened >= 0)
        close(reopened);
    EXPECT_TRUE(read_ok, "open through a magic link did not reopen the file");

    TEST("stat through magic link");
    struct stat st;
    EXPECT_TRUE(stat(magic("/proc/self/fd/", fd), &st) == 0 && st.st_size == 7,
                "stat through a magic link did not describe the file");

    TEST("readlink reports the target");
    char link[512];
    ssize_t n = readlink(magic("/proc/self/fd/", fd), link, sizeof(link) - 1);
    if (n > 0)
        link[n] = '\0';
    EXPECT_TRUE(n > 0 && strstr(link, "fd-magiclink.tmp") != NULL,
                "readlink did not report the descriptor's file");

    close(fd);
    unlink(TEST_FILE);
}

int main(void)
{
    check_follow_style_ops();
    check_follows_descriptor_not_path();
    check_no_follow_ops_spare_the_target();
    check_strict_name_parsing();
    check_intercepts_unchanged();

    SUMMARY("test-fd-magiclink");
    return fails > 0 ? 1 : 0;
}
