/*
 * MAP_SHARED mappings across execve
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux drops the whole mm on execve, so a MAP_SHARED file mapping the old
 * image held is simply unmapped and the file keeps its bytes. elfuse recycles
 * one address space instead: guest_reset zeroes each tracked region by writing
 * through its host VA, and for an overlaid region that host VA is the backing
 * file's own page cache. Without mmap_exec_drop_overlays running first, those
 * zeroes reach the file, and a guest that maps a file MAP_SHARED and execs
 * truncates its contents to zero on the host.
 *
 * Checked in the image after the exec: the file still holds what the pre-exec
 * image wrote, writes into a fresh anonymous mapping at the address the shared
 * mapping used do not reach it (an overlay left installed would hand the new
 * image the host file at that VA), and a forked peer holding a
 * MAP_SHARED|MAP_ANONYMOUS region still sees its bytes. That last one is the
 * other backing an overlay can have: the region is promoted to a temp-file
 * overlay at fork, and the promotion outlives it.
 *
 * Syscalls exercised: execve(221), mmap(222), munmap(215), msync(227),
 *                     openat(56), ftruncate(46), clone(220), wait4(260),
 *                     pipe2(59), read(63), write(64)
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

#define FILE_TEMPLATE "/tmp/elfuse-exec-shared-mmap-XXXXXX"
#define MARK "ALIVE!!!"
#define MARK_LEN 8
#define MAP_LEN 65536
#define ANON_MARK "PEER!!!!"

extern char **environ;

static void die(const char *what)
{
    fprintf(stderr, "test-exec-shared-mmap: %s: %s\n", what, strerror(errno));
    exit(1);
}

/* The forked peer: hold the anon-shared mapping open while the parent execs,
 * then report through @wr whether the bytes are still there. '1' is intact, '0'
 * is zeroed, and the pipe closing without a byte means the parent's new image
 * never got far enough to ask.
 */
static void peer_main(char *map, int rd, int wr)
{
    char c;
    ssize_t n;

    do {
        n = read(rd, &c, 1);
    } while (n < 0 && errno == EINTR);
    if (n <= 0)
        _exit(2);

    char verdict = memcmp(map, ANON_MARK, MARK_LEN) == 0 ? '1' : '0';
    do {
        n = write(wr, &verdict, 1);
    } while (n < 0 && errno == EINTR);
    _exit(0);
}

/* Everything after the exec. The arguments carry what only the pre-exec image
 * knew: the backing file it created, where its shared mapping lived, and the
 * pipe ends to the peer.
 */
static int stage2(const char *path,
                  uint64_t shared_addr,
                  int peer_rd,
                  int peer_wr,
                  pid_t peer)
{
    printf("test-exec-shared-mmap: post-exec checks\n");

    TEST("file survives execve");
    int fd = open(path, O_RDONLY);
    char buf[MARK_LEN + 1] = {0};
    if (fd < 0) {
        FAIL("open");
    } else {
        ssize_t n = read(fd, buf, MARK_LEN);
        close(fd);
        if (n == MARK_LEN && memcmp(buf, MARK, MARK_LEN) == 0)
            PASS();
        else
            FAIL("file contents were zeroed by the exec");
    }

    TEST("remap of the file sees the bytes");
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        FAIL("open");
    } else {
        char *p = mmap(NULL, MAP_LEN, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED)
            FAIL("mmap");
        else if (memcmp(p, MARK, MARK_LEN) == 0)
            PASS();
        else
            FAIL("mapping reads zero");
        if (p != MAP_FAILED)
            munmap(p, MAP_LEN);
    }

    /* The hint is where the old image's shared mapping was. The region table is
     * empty again after the reset, so elfuse honors it, and an overlay left
     * installed would still be bound to that host VA underneath. Reading zero
     * there proves nothing on its own (the reset zeroes the file too, through
     * the very overlay this is looking for), so write a pattern and go back to
     * the file: the new image's private memory must not reach it.
     */
    TEST("vacated range does not write through");
    char *p = mmap((void *) (uintptr_t) shared_addr, MAP_LEN,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        FAIL("mmap");
    } else if ((uint64_t) (uintptr_t) p != shared_addr) {
        /* A failure, not a skip. The region table is empty after the reset, so
         * elfuse honors this hint in both the fixed and the broken build
         * (measured: the broken one reaches the check and fails it). If it ever
         * stops being honored, this check silently stops testing anything, and
         * a run that reports success without having looked is worse than a red
         * one. Whoever changes the allocator gets to see this and decide.
         */
        printf("hint 0x%llx not honored (got %p): ",
               (unsigned long long) shared_addr, (void *) p);
        FAIL("could not place the mapping at the vacated address");
        munmap(p, MAP_LEN);
    } else {
        memset(p, 'S', MAP_LEN);
        munmap(p, MAP_LEN);
        fd = open(path, O_RDONLY);
        char after[MARK_LEN + 1] = {0};
        ssize_t n = fd < 0 ? -1 : read(fd, after, MARK_LEN);
        if (fd >= 0)
            close(fd);
        if (n == MARK_LEN && memcmp(after, MARK, MARK_LEN) == 0)
            PASS();
        else
            FAIL("the new image's private memory reached the file");
    }

    TEST("peer keeps its anon-shared bytes");
    if (peer <= 0) {
        FAIL("no peer: the pre-exec image failed to start one");
    } else {
        char go = 'g', verdict = 0;
        ssize_t n;
        do {
            n = write(peer_wr, &go, 1);
        } while (n < 0 && errno == EINTR);
        do {
            n = read(peer_rd, &verdict, 1);
        } while (n < 0 && errno == EINTR);
        int status = 0;
        waitpid(peer, &status, 0);
        if (n != 1)
            FAIL("peer reported nothing");
        else if (verdict == '1')
            PASS();
        else
            FAIL("peer's shared region was zeroed by the exec");
    }

    unlink(path);
    SUMMARY("test-exec-shared-mmap");
    return fails == 0 ? 0 : 1;
}

/* The pre-exec image: build the two shared mappings, hand one to a forked peer,
 * and exec self.
 */
static int stage1(const char *self)
{
    /* A unique name rather than a fixed one: two lanes of the matrix can share
     * a host /tmp, and a fixed name would let one truncate the file the other
     * is checking (and would follow a symlink planted at that path).
     */
    char path[sizeof(FILE_TEMPLATE)];
    memcpy(path, FILE_TEMPLATE, sizeof(FILE_TEMPLATE));
    int fd = mkstemp(path);
    if (fd < 0)
        die("mkstemp");
    if (ftruncate(fd, MAP_LEN) < 0)
        die("ftruncate");

    char *shared =
        mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED)
        die("mmap file");
    close(fd);
    memcpy(shared, MARK, MARK_LEN);
    if (msync(shared, MAP_LEN, MS_SYNC) < 0)
        die("msync");

    /* Anon-shared plus a peer holding it. The fork is what promotes the region
     * to an overlay, so the mapping has to exist before it.
     */
    char *anon = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (anon == MAP_FAILED)
        die("mmap anon-shared");
    memcpy(anon, ANON_MARK, MARK_LEN);

    int to_peer[2], from_peer[2];
    if (pipe(to_peer) < 0 || pipe(from_peer) < 0)
        die("pipe");
    pid_t peer = fork();
    if (peer < 0)
        die("fork");
    if (peer == 0) {
        close(to_peer[1]);
        close(from_peer[0]);
        peer_main(anon, to_peer[0], from_peer[1]);
    }
    close(to_peer[0]);
    close(from_peer[1]);

    /* Block-buffered under the harness pipe, and execve discards whatever is
     * still sitting in the buffer.
     */
    fflush(stdout);
    fflush(stderr);

    char addrbuf[32], rdbuf[16], wrbuf[16], peerbuf[16];
    snprintf(addrbuf, sizeof(addrbuf), "0x%llx",
             (unsigned long long) (uintptr_t) shared);
    snprintf(rdbuf, sizeof(rdbuf), "%d", from_peer[0]);
    snprintf(wrbuf, sizeof(wrbuf), "%d", to_peer[1]);
    snprintf(peerbuf, sizeof(peerbuf), "%d", (int) peer);

    char *argv[] = {(char *) self, "stage2", addrbuf, rdbuf,
                    wrbuf,         peerbuf,  path,    NULL};
    execve(self, argv, environ);
    die("execve");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc >= 7 && !strcmp(argv[1], "stage2"))
        return stage2(argv[6], strtoull(argv[2], NULL, 0), atoi(argv[3]),
                      atoi(argv[4]), (pid_t) atoi(argv[5]));

    return stage1(argv[0]);
}
