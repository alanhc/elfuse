/*
 * exec identity under a case-fold sysroot
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * An executable under a case-protected directory exists on disk only under an
 * escaped spelling, and exec crosses that boundary twice: the path being
 * executed must be resolved like every other guest path, and the identity the
 * kernel then reports (/proc/self/exe, /proc/self/fd/N) must carry the guest's
 * bytes, never the stored spelling or the sysroot prefix.
 *
 * Linux contract pinned: execveat(2) resolves pathname relative to dirfd with
 * the caller's namespace rules, and proc(5) says /proc/self/exe is a symlink to
 * the executed binary as pathnames name it, a path the process can hand
 * straight back to execve.
 *
 * Code under test: sc_execveat in src/syscall/syscall.c, sys_execve in
 * src/syscall/exec.c, and proc_readlink_self_exe / proc_intercept_readlink in
 * src/runtime/procemu.c. A regression shows up as execveat reporting ENOENT for
 * a binary execve runs fine, or as /proc/self/exe naming an .ef= spelling,
 * which is how a self-re-exec (busybox applets, watchdogs) stops working.
 *
 * Run under --sysroot on a case-folding volume.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test-harness.h"
#include "test-util.h"

#ifndef SYS_execveat
#define SYS_execveat 281
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

int passes = 0, fails = 0;

#define DIR_A "/Apps"
#define RUNME DIR_A "/RunMe"

/* Exit codes for the child lanes, chosen away from errno-shaped values. */
#define EXIT_MATCH 0
#define EXIT_MISMATCH 42
#define EXIT_SETUP 43

/* Child lane: compare /proc/self/exe against argv[2]; an .ef= substring or a
 * mismatch is a failure whoever spawned it.
 */
static int child_exe_is(const char *want, bool reexec)
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);

    if (n < 0)
        return EXIT_SETUP;
    buf[n] = '\0';
    if (strcmp(buf, want) || strstr(buf, ".ef=")) {
        fprintf(stderr, "child: /proc/self/exe = %s, want %s\n", buf, want);
        return EXIT_MISMATCH;
    }
    if (reexec) {
        /* The reported identity must be live: hand it straight back to execve,
         * as a self-re-exec does.
         */
        char *argv2[] = {buf, "--exe-is", (char *) want, NULL};
        execve("/proc/self/exe", argv2, NULL);
        return EXIT_SETUP;
    }
    return EXIT_MATCH;
}

/* Copy this binary to @dst, mode 0755, through /proc/self/exe. */
static int self_copy(const char *dst)
{
    char buf[65536];
    int in = open("/proc/self/exe", O_RDONLY);
    int out;
    ssize_t n;

    if (in < 0)
        return -1;
    out = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    if (out < 0) {
        close(in);
        return -1;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t) n) != n) {
            n = -1;
            break;
        }
    }
    close(in);
    close(out);
    return n < 0 ? -1 : 0;
}

/* Fork, run @fn in the child, and report its exit code. */
static int wait_code(pid_t pid)
{
    int st;

    if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st))
        return -1;
    return WEXITSTATUS(st);
}

int main(int argc, char **argv)
{
    struct stat stbuf;
    pid_t pid;

    if (argc == 3 && !strcmp(argv[1], "--exe-is"))
        return child_exe_is(argv[2], false);
    if (argc == 3 && !strcmp(argv[1], "--exe-is-reexec"))
        return child_exe_is(argv[2], true);

    printf("test-sysroot-exec-names: exec identity in the guest namespace\n");

    TEST("stage a copy of this binary under an escaped directory");
    EXPECT_TRUE((mkdir(DIR_A, 0755) == 0 || errno == EEXIST) &&
                    self_copy(RUNME) == 0 && stat(RUNME, &stbuf) == 0 &&
                    S_ISREG(stbuf.st_mode),
                "stage");

    /* The guard: plain execve of the staged copy, child checks its own
     * identity. Held so a fix for the lanes below cannot trade this away.
     */
    TEST("execve runs it and the child sees the guest path");
    pid = fork();
    if (pid == 0) {
        char *argv2[] = {(char *) RUNME, "--exe-is", (char *) RUNME, NULL};
        execve(RUNME, argv2, NULL);
        _exit(EXIT_SETUP);
    }
    EXPECT_EQ(wait_code(pid), EXIT_MATCH, "child exit");

    /* execveat with a dirfd and a relative name is how fexecve-style runners
     * reach a binary; the name is a guest name and must resolve like one.
     */
    TEST("execveat(dirfd, name) runs an escaped binary");
    pid = fork();
    if (pid == 0) {
        int dfd = open(DIR_A, O_RDONLY | O_DIRECTORY);
        char *argv2[] = {(char *) RUNME, "--exe-is", (char *) RUNME, NULL};
        if (dfd < 0)
            _exit(EXIT_SETUP);
        syscall(SYS_execveat, dfd, "RunMe", argv2, NULL, 0);
        _exit(errno == ENOENT ? EXIT_MISMATCH : EXIT_SETUP);
    }
    EXPECT_EQ(wait_code(pid), EXIT_MATCH, "child exit");

    /* AT_EMPTY_PATH executes the fd itself; the identity the child then reads
     * must still be the guest spelling, not the F_GETPATH host bytes.
     */
    TEST("execveat(fd, \"\", AT_EMPTY_PATH) keeps the guest identity");
    pid = fork();
    if (pid == 0) {
        int fd = open(RUNME, O_RDONLY);
        char *argv2[] = {(char *) RUNME, "--exe-is-reexec", (char *) RUNME,
                         NULL};
        if (fd < 0)
            _exit(EXIT_SETUP);
        syscall(SYS_execveat, fd, "", argv2, NULL, AT_EMPTY_PATH);
        _exit(EXIT_SETUP);
    }
    EXPECT_EQ(wait_code(pid), EXIT_MATCH, "child exit");

    /* /proc/self/fd/N is the same reverse mapping on a different readlink. */
    TEST("readlink of /proc/self/fd/N reports the guest path");
    {
        char proc_path[64];
        char buf[PATH_MAX];
        ssize_t n;
        int fd = open(RUNME, O_RDONLY);

        if (fd < 0) {
            FAIL("open");
        } else {
            snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
            n = readlink(proc_path, buf, sizeof(buf) - 1);
            if (n < 0) {
                FAIL("readlink");
            } else {
                buf[n] = '\0';
                EXPECT_TRUE(!strcmp(buf, RUNME),
                            "fd path leaked a host spelling");
                if (strcmp(buf, RUNME))
                    fprintf(stderr, "  got %s\n", buf);
            }
            close(fd);
        }
    }

    SUMMARY("test-sysroot-exec-names");
    return fails > 0 ? 1 : 0;
}
