/*
 * Sysroot chdir regression tests
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The guest's working directory is tracked as a guest path, not a host one, so
 * getcwd(3) and /proc/self/cwd must report where the guest thinks it is rather
 * than where the file actually sits. Two things can break that: the sysroot
 * prefix leaking out, and (on a folding volume) a component whose on-disk
 * spelling is escaped being reported as stored.
 *
 * Code under test: proc_cwd_refresh in src/syscall/proc-state.c and the
 * host-to-guest conversion in src/syscall/path.c. A regression shows up as a
 * cwd the guest cannot chdir back into, because the path it was handed names
 * nothing in its own namespace.
 *
 * Run under --sysroot.
 */

#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

int main(void)
{
    char cwd[256];
    char proc_cwd[256];

    printf("test-sysroot-chdir: sysroot chdir tests\n");

    TEST("absolute chdir keeps guest cwd");
    {
        ssize_t len;

        if (chdir("/bin") < 0) {
            FAIL("chdir /bin failed");
        } else if (!getcwd(cwd, sizeof(cwd))) {
            FAIL("getcwd after chdir /bin failed");
        } else if (strcmp(cwd, "/bin") != 0) {
            FAIL("getcwd leaked host sysroot path");
        } else if ((len = readlink("/proc/self/cwd", proc_cwd,
                                   sizeof(proc_cwd) - 1)) < 0) {
            FAIL("readlink /proc/self/cwd failed");
        } else {
            proc_cwd[len] = '\0';
            if (strcmp(proc_cwd, "/bin") != 0)
                FAIL("/proc/self/cwd leaked host sysroot path");
            else
                PASS();
        }
    }

    TEST("relative chdir stays guest-visible under sysroot");
    {
        if (chdir("../lib") < 0) {
            FAIL("chdir ../lib failed");
        } else if (!getcwd(cwd, sizeof(cwd))) {
            FAIL("getcwd after chdir ../lib failed");
        } else if (strcmp(cwd, "/lib") != 0) {
            FAIL("relative chdir produced wrong guest cwd");
        } else {
            PASS();
        }
    }

    TEST("missing absolute path does not fall back to sysroot lib basename");
    {
        errno = 0;
        if (chdir("/elfuse-sysroot-shadow") == 0) {
            FAIL("chdir unexpectedly succeeded via sysroot lib fallback");
        } else if (errno != ENOENT) {
            FAIL("chdir failed with wrong errno");
        } else {
            PASS();
        }
    }

    /* A directory whose name the volume cannot hold as itself is stored under
     * an escape. The cwd is reported to the guest, so it has to be reported in
     * the guest's spelling; handing back the escape names a directory the guest
     * never created and cannot chdir into.
     */
    TEST("getcwd reports the guest spelling of an escaped directory");
    {
        ssize_t len;

        if (mkdir("/Cwd.Dir", 0755) < 0 && errno != EEXIST) {
            FAIL("mkdir failed");
        } else if (chdir("/Cwd.Dir") < 0) {
            FAIL("chdir failed");
        } else if (!getcwd(cwd, sizeof(cwd))) {
            FAIL("getcwd failed");
        } else if (strcmp(cwd, "/Cwd.Dir")) {
            FAIL("getcwd leaked the on-disk spelling");
        } else if ((len = readlink("/proc/self/cwd", proc_cwd,
                                   sizeof(proc_cwd) - 1)) < 0) {
            FAIL("readlink /proc/self/cwd failed");
        } else {
            proc_cwd[len] = '\0';
            if (strcmp(proc_cwd, "/Cwd.Dir"))
                FAIL("/proc/self/cwd leaked the on-disk spelling");
            else
                PASS();
        }
    }

    /* The cwd must be usable, not merely printable: a guest that reads it and
     * chdirs back has to arrive where it started.
     */
    TEST("the reported cwd can be returned to");
    {
        if (chdir("/") < 0) {
            FAIL("chdir / failed");
        } else if (chdir(cwd) < 0) {
            FAIL("the reported cwd does not resolve");
        } else if (!getcwd(proc_cwd, sizeof(proc_cwd)) ||
                   strcmp(proc_cwd, "/Cwd.Dir")) {
            FAIL("round trip landed somewhere else");
        } else {
            PASS();
        }
    }

    SUMMARY("test-sysroot-chdir");
    return fails > 0 ? 1 : 0;
}
