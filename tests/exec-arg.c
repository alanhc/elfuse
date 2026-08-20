/*
 * execve helper
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Replace this process with argv[1], handing it argv[2..] as its own argv.
 * argv[2] is the target's argv[0], separate from the path because a multicall
 * binary dispatches on it. Sysroot recipes run this as a guest so the target
 * goes through the guest execve path: the initial process is loaded by the core
 * bootstrap, which resolves PT_INTERP by literal concatenation plus the /lib
 * fallback only, so a lane about interpreter resolution through escaped
 * spellings has to exec from inside the guest.
 */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: exec-arg PROG ARGV0 [ARGS...]\n");
        return 1;
    }
    execv(argv[1], argv + 2);
    perror(argv[1]);
    return 1;
}
