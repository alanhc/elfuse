/*
 * copy helper
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copy argv[1] to argv[2], mode 0755. Sysroot recipes run this as a guest so
 * the destination is created through the guest: on a folding volume a
 * case-protected destination is then stored under its escape, which is the
 * on-disk shape a later resolution in the same recipe must cross.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "test-util.h"

int main(int argc, char **argv)
{
    char buf[65536];
    ssize_t n;

    if (argc != 3) {
        fprintf(stderr, "usage: copy-arg SRC DST\n");
        return 1;
    }
    int in = open(argv[1], O_RDONLY);
    if (in < 0)
        return perror(argv[1]), 1;
    int out = open(argv[2], O_CREAT | O_WRONLY | O_TRUNC, 0755);
    if (out < 0)
        return perror(argv[2]), 1;
    /* write_fd_all, not a bare write: the destination goes through the FUSE
     * write path, where a short count is a success that must be resumed, and
     * write(2) leaves errno untouched on one. The read retries EINTR for the
     * same reason: an interrupted copy is not a failed one.
     */
    for (;;) {
        n = read(in, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (write_fd_all(out, buf, (size_t) n) != 0)
            return perror("write"), 1;
    }
    close(in);
    close(out);
    return n < 0 ? 1 : 0;
}
