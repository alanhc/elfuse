/*
 * The launcher's stdin description survives a guest that duplicates it
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * elfuse owns O_NONBLOCK on the fds whose transfers could park a vCPU thread,
 * and emulates the guest's blocking semantics on top. The three descriptors it
 * inherits are excluded, because their open file description belongs to whoever
 * launched elfuse: a shell that hands over its terminal, or a pipe it still
 * reads, must get it back exactly as it was.
 *
 * Excluding them by fd type is not enough on its own. A dup of an inherited
 * descriptor is allocated as FD_REGULAR, so it escapes a type test while still
 * naming the launcher's description, and taking ownership there sets O_NONBLOCK
 * on a description elfuse does not own. This runs a guest that duplicates its
 * stdin three ways and checks the flag from the other side once it exits.
 *
 * Host-side because that is the only side that can see it: the guest's own view
 * of the flag comes from elfuse's shadow, which reads correctly either way.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

#define ELFUSE_BIN "build/elfuse"
#define GUEST_BIN "build/test-pipe-steal"

/* Run the guest with stdin bound to read_fd. Returns its exit status, or -1. */
static int run_guest_with_stdin(int read_fd, const char *mode)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        if (dup2(read_fd, STDIN_FILENO) < 0)
            _exit(127);
        execl(ELFUSE_BIN, ELFUSE_BIN, GUEST_BIN, mode, (char *) NULL);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(void)
{
    printf("test-stdio-nonblock-host: launcher stdin flags across a guest\n");

    if (access(ELFUSE_BIN, X_OK) != 0 || access(GUEST_BIN, R_OK) != 0) {
        printf("  SKIP: %s or %s not built\n", ELFUSE_BIN, GUEST_BIN);
        return 0;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        FAIL("pipe");
        SUMMARY("test-stdio-nonblock-host");
        return 1;
    }

    int before = fcntl(fds[0], F_GETFL);

    /* Two ways to reach a slot that aliases the launcher's description: a dup
     * inside one guest, and a fork, whose child rebuilds its whole fd table
     * from descriptors the parent hands it.
     */
    static const char *const modes[] = {"dupstdin", "dupstdin-fork", "scmpass"};
    static const char *const names[] = {
        "a guest that dups stdin", "a guest that dups stdin and forks",
        "a guest that passes stdin over SCM_RIGHTS"};

    for (int i = 0; i < 3; i++) {
        int rc = run_guest_with_stdin(fds[0], modes[i]);
        int after = fcntl(fds[0], F_GETFL);

        /* The flag is checked whatever the guest's exit status. A leak is the
         * failure this test exists to catch and a plausible reason for the
         * guest to have failed, so skipping the check on a bad exit would hide
         * exactly what is being looked for. Both are reported.
         */
        if (rc != 0) {
            TEST(names[i]);
            FAIL("guest did not exit 0");
        }

        TEST(names[i]);

        /* This fd and the guest's stdin are one description, so a flag elfuse
         * set for its own use is visible right here, and outlives it.
         */
        EXPECT_EQ(after & O_NONBLOCK, before & O_NONBLOCK,
                  "O_NONBLOCK leaked onto the launcher's description");
    }

    close(fds[0]);
    close(fds[1]);
    SUMMARY("test-stdio-nonblock-host");
    return fails > 0 ? 1 : 0;
}
