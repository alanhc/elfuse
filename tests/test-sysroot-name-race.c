/*
 * Concurrent creation of colliding names
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Nothing serializes name creation in a sysroot, and the reason is that the
 * on-disk spelling of a guest name is a function of that name alone: two
 * processes creating names that the volume would fold together are writing
 * different entries, so they never contend. This asserts the consequence rather
 * than the mechanism.
 *
 * fork(2) under elfuse spawns a separate host process, so the children below
 * really are separate processes sharing one sysroot with no shared state
 * between them.
 *
 * Two rounds. In the first every child creates a different member of one
 * case-colliding set, and all of them must survive with their own content: a
 * lost update or a create landing on a sibling's entry shows up as wrong
 * content or a missing name. In the second every child races for the *same*
 * name under O_EXCL, where Linux guarantees exactly one winner.
 *
 * Code under test: casefold_needs_escape and casefold_escape in
 * src/syscall/casefold.c, which decide the target entry without consulting the
 * directory, reached through src/syscall/casefold-walk.c. A regression shows up
 * as two children writing the same file, a create reporting EEXIST for a name
 * nobody else took, or more than one winner of the O_EXCL round.
 *
 * A pass does not prove the absence of a race: it is a scheduling test, and the
 * make recipe repeats it because a single round can miss a narrow window. What
 * a failure proves is that one exists. Run under --sysroot.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test-harness.h"
#include "test-util.h"

int passes = 0, fails = 0;

#define DIR_R "/name-race"
#define KIDS 4

/* One case-colliding set: the volume matches all four against each other, so
 * each has to end up in a different on-disk entry.
 */
static const char *const spellings[KIDS] = {"Race", "race", "RACE", "rAcE"};

/* Everything in this lane lives under one directory and is addressed by name,
 * so the shared helpers are reached through a path built here. Only the
 * composition is local; the I/O is not duplicated.
 */
static int write_file(const char *name, const char *text)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/%s", DIR_R, name);
    return file_write(path, text);
}

static int content_is(const char *name, const char *want)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/%s", DIR_R, name);
    return file_content_is(path, want);
}

/* Fork KIDS children and run @body in each, recording each child's exit code in
 * @code (-1 for a child that could not be forked or reaped, or that died
 * abnormally). One fork/wait implementation for both rounds, so the reaping
 * bookkeeping cannot drift between them.
 */
static void run_children(int (*body)(int), int code[KIDS])
{
    pid_t pid[KIDS];

    for (int i = 0; i < KIDS; i++) {
        pid[i] = fork();
        if (pid[i] == 0)
            _exit(body(i));
    }
    for (int i = 0; i < KIDS; i++) {
        int status = 0;

        code[i] = -1;
        if (pid[i] < 0)
            continue;
        if (waitpid(pid[i], &status, 0) < 0)
            continue;
        if (WIFEXITED(status))
            code[i] = WEXITSTATUS(status);
    }
}

/* The count of children whose exit code was @want. */
static int exited_with(const int code[KIDS], int want)
{
    int n = 0;

    for (int i = 0; i < KIDS; i++)
        if (code[i] == want)
            n++;
    return n;
}

static int create_own_spelling(int i)
{
    char text[8];

    snprintf(text, sizeof(text), "%d", i);
    return write_file(spellings[i], text) == 0 ? 0 : 1;
}

/* Every child races for one name under O_EXCL. Linux gives it to exactly one,
 * so a child reports success only if it created the file, and EEXIST is the
 * expected answer for the rest.
 */
static int claim_shared_name(int i)
{
    char path[PATH_MAX];
    int fd;

    /* Every child races for the same name, so which child this is does not
     * enter into it; run_children's signature supplies the index regardless.
     */
    (void) i;
    snprintf(path, sizeof(path), "%s/Contended", DIR_R);
    fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd >= 0) {
        close(fd);
        return 0;
    }
    return errno == EEXIST ? 1 : 2;
}

int main(void)
{
    TEST("fixture mkdir");
    EXPECT_TRUE(mkdir(DIR_R, 0755) == 0 || errno == EEXIST, "mkdir");

    TEST("every child creates its own spelling");
    {
        int code[KIDS];

        run_children(create_own_spelling, code);
        EXPECT_EQ(exited_with(code, 0), KIDS, "all children succeed");
    }

    /* Nobody may have landed on a sibling's entry: each spelling holds the
     * index of the child that wrote it.
     */
    for (int i = 0; i < KIDS; i++) {
        char want[8];
        char label[64];

        snprintf(want, sizeof(want), "%d", i);
        snprintf(label, sizeof(label), "spelling %d kept its own content", i);
        TEST(label);
        EXPECT_TRUE(content_is(spellings[i], want) == 0,
                    "a concurrent create landed on the wrong entry");
    }

    TEST("the directory holds exactly one entry per spelling");
    EXPECT_EQ(dir_entry_count(DIR_R), KIDS, "entry count");

    /* Racing for one name is ordinary O_EXCL: the kernel picks a winner and
     * everyone else gets EEXIST, with no elfuse-level arbitration involved.
     */
    TEST("exactly one child wins a contended O_EXCL create");
    {
        int code[KIDS];

        run_children(claim_shared_name, code);
        EXPECT_TRUE(
            exited_with(code, 0) == 1 && exited_with(code, 1) == KIDS - 1,
            "exactly one winner, the rest EEXIST");
    }

    SUMMARY("test-sysroot-name-race");
    return fails > 0 ? 1 : 0;
}
