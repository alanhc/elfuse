/*
 * Sustained churn of case-colliding names
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * test-sysroot-name-race aims ten processes at one narrow window; this is the
 * volume counterpart. Eight threads and two forked children hammer eight
 * directories with creates, renames, unlinks, stats, and listing scans over
 * one colliding set (four case spellings plus an NFC/NFD pair) for a
 * deadline given in seconds as argv[1]. Nothing serializes name creation (the
 * spelling is a function of the name alone), so this is the shape that would
 * surface a lost update or a decode landing on a sibling's entry.
 *
 * Two invariants hold at every step, however the operations interleave:
 * every syscall returns success or the ENOENT/EEXIST that a concurrent
 * unlink or create legitimately produces, and every listing is a
 * duplicate-free subset of the colliding set: a duplicate means two disk
 * entries decoded to one guest name, an escape spelling means a decode was
 * skipped. After the deadline, workers join and a quiescent sweep checks
 * that whatever survived reads back a member's content and unlinks cleanly.
 *
 * A pass does not prove the absence of a race; it is evidence that sustained
 * churn lacks a reproducer today, and only a failure proves anything. Kept
 * out of `make check` for its runtime; run via test-sysroot-name-soak (or
 * check-soak) with --timeout 0.
 *
 * Code under test: the create/rename/unlink translation paths in
 * src/syscall/fs.c, the case-exact walk in src/syscall/casefold-walk.c, and
 * the dirent decode in src/syscall/path.c under contention.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

#define N_DIRS 8
#define N_THREADS 8
#define N_FORKS 2
#define DEFAULT_SECS 60

static const char *const names[] = {
    "Race", "race", "RACE", "rAcE", "caf\xc3\xa9", "cafe\xcc\x81",
};
#define N_NAMES (sizeof(names) / sizeof(names[0]))

/* Monotonic seconds, so a wall-clock step during the run cannot stretch or
 * cut the soak (time(2) tracks CLOCK_REALTIME).
 */
static time_t mono_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static time_t deadline;
static atomic_int worker_failures;

/* One line per anomaly, not per operation: a soak that floods the log buries
 * the first failure, which is the one that names the bug.
 */
static void soak_fail(const char *what, const char *dir, const char *name)
{
    fprintf(stderr, "soak: %s (dir %s, name %s, errno %d)\n", what, dir, name,
            errno);
    atomic_fetch_add(&worker_failures, 1);
}

static uint64_t prng_next(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static void dir_path(char *out, size_t outsz, int d)
{
    snprintf(out, outsz, "/soak-%d", d);
}

static void entry_path(char *out, size_t outsz, int d, const char *name)
{
    snprintf(out, outsz, "/soak-%d/%s", d, name);
}

static bool name_in_set(const char *n)
{
    for (size_t i = 0; i < N_NAMES; i++) {
        if (!strcmp(n, names[i]))
            return true;
    }
    return false;
}

/* A listing must be a duplicate-free subset of the set at every instant:
 * entries come and go under churn, but two entries for one guest name mean
 * two disk spellings decoded onto it, and an escape prefix means one was
 * not decoded at all.
 */
static void scan_listing(const char *dir)
{
    char seen[N_NAMES] = {0};
    DIR *d = opendir(dir);
    struct dirent *de;

    if (!d) {
        soak_fail("listing did not open", dir, "-");
        return;
    }
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (!strncmp(de->d_name, ".ef=", 4)) {
            soak_fail("escape spelling leaked into a listing", dir, de->d_name);
            break;
        }
        if (!name_in_set(de->d_name)) {
            soak_fail("listing holds a name outside the set", dir, de->d_name);
            break;
        }
        for (size_t i = 0; i < N_NAMES; i++) {
            if (strcmp(de->d_name, names[i]))
                continue;
            if (seen[i]) {
                soak_fail("guest name listed twice", dir, de->d_name);
                break;
            }
            seen[i] = 1;
        }
    }
    closedir(d);
}

static void worker_loop(uint64_t seed)
{
    uint64_t s = seed | 1;

    while (mono_now() < deadline && !atomic_load(&worker_failures)) {
        uint64_t r = prng_next(&s);
        int d = (int) (r % N_DIRS);
        const char *name = names[(r >> 8) % N_NAMES];
        char path[PATH_MAX], dst[PATH_MAX];

        entry_path(path, sizeof(path), d, name);
        switch ((r >> 16) % 5) {
        case 0: {
            int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd < 0) {
                soak_fail("create refused", path, name);
                break;
            }
            /* The content is the creating spelling, so any read landing on
             * a sibling's file is visible to the quiescent sweep.
             */
            if (write(fd, name, strlen(name)) < 0)
                soak_fail("write refused", path, name);
            close(fd);
            break;
        }
        case 1: {
            const char *to = names[(r >> 24) % N_NAMES];

            entry_path(dst, sizeof(dst), d, to);
            if (rename(path, dst) < 0 && errno != ENOENT)
                soak_fail("rename refused", path, to);
            break;
        }
        case 2:
            if (unlink(path) < 0 && errno != ENOENT)
                soak_fail("unlink refused", path, name);
            break;
        case 3: {
            struct stat st;

            if (stat(path, &st) < 0 && errno != ENOENT)
                soak_fail("stat refused", path, name);
            break;
        }
        default: {
            char dir[64];

            dir_path(dir, sizeof(dir), d);
            scan_listing(dir);
            break;
        }
        }
    }
}

static void *thread_main(void *arg)
{
    worker_loop((uint64_t) (uintptr_t) arg);
    return NULL;
}

int main(int argc, char **argv)
{
    long secs = argc > 1 ? strtol(argv[1], NULL, 10) : DEFAULT_SECS;
    pthread_t threads[N_THREADS];
    pid_t kids[N_FORKS];

    if (secs <= 0)
        secs = DEFAULT_SECS;
    deadline = mono_now() + secs;

    TEST("fixture directories");
    {
        bool ok = true;
        for (int d = 0; d < N_DIRS; d++) {
            char dir[64];

            dir_path(dir, sizeof(dir), d);
            if (mkdir(dir, 0755) < 0 && errno != EEXIST)
                ok = false;
        }
        EXPECT_TRUE(ok, "mkdir");
    }

    /* Forked children first so they inherit no thread state; each is a full
     * elfuse host process sharing the sysroot, which is the cross-process
     * half of the churn.
     */
    /* Seeds are fixed so a failing interleaving can at least be retried
     * with the same operation streams; distinct per worker so no two
     * workers replay each other. The multiplier is the usual 64-bit
     * golden-ratio scatter constant.
     */
    for (int i = 0; i < N_FORKS; i++) {
        kids[i] = fork();
        if (kids[i] == 0) {
            worker_loop(0x9E3779B97F4A7C15ULL * (uint64_t) (i + 1));
            _exit(atomic_load(&worker_failures) ? 1 : 0);
        }
    }
    for (int i = 0; i < N_THREADS; i++)
        pthread_create(&threads[i], NULL, thread_main,
                       (void *) (uintptr_t) (0xA5A5A5A5ULL + (uint64_t) i));

    for (int i = 0; i < N_THREADS; i++)
        pthread_join(threads[i], NULL);

    TEST("forked workers exit clean");
    {
        bool ok = true;
        for (int i = 0; i < N_FORKS; i++) {
            int st = 0;

            if (kids[i] < 0 || waitpid(kids[i], &st, 0) != kids[i] ||
                !WIFEXITED(st) || WEXITSTATUS(st) != 0)
                ok = false;
        }
        EXPECT_TRUE(ok, "a forked worker failed or died");
    }

    TEST("no worker reported an anomaly");
    EXPECT_TRUE(atomic_load(&worker_failures) == 0, "see soak: lines above");

    TEST("quiescent sweep");
    {
        bool ok = true;

        for (int d = 0; d < N_DIRS && ok; d++) {
            char dir[64];

            dir_path(dir, sizeof(dir), d);
            scan_listing(dir);
            for (size_t i = 0; i < N_NAMES; i++) {
                char path[PATH_MAX], got[NAME_MAX + 1];
                int fd;
                ssize_t n;

                entry_path(path, sizeof(path), d, names[i]);
                fd = open(path, O_RDONLY);
                if (fd < 0) {
                    if (errno != ENOENT)
                        ok = false;
                    continue;
                }
                n = read(fd, got, sizeof(got) - 1);
                close(fd);
                if (n < 0) {
                    ok = false;
                    continue;
                }
                got[n] = '\0';
                /* Content is whichever member spelling last wrote the file;
                 * anything else means a write landed through the wrong
                 * entry.
                 */
                if (!name_in_set(got))
                    ok = false;
                if (unlink(path) < 0 && errno != ENOENT)
                    ok = false;
            }
        }
        EXPECT_TRUE(ok && atomic_load(&worker_failures) == 0,
                    "post-churn state is inconsistent");
    }

    SUMMARY("test-sysroot-name-soak");
    return fails > 0 ? 1 : 0;
}
