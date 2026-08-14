/*
 * macOS libc constants Frama-C's modeled libc does not carry
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Frama-C models a portable libc, so anything Darwin-specific is absent even
 * when the header it lives in is present. A source using one stops with "Cannot
 * resolve variable", which is a missing declaration rather than a modeling gap:
 * the value is an integer the host header would have supplied.
 *
 * Only what the tree actually references, and only what Frama-C lacks. A new
 * one fails loudly with the same "Cannot resolve variable", which is the
 * intended way to discover it belongs here.
 *
 * Values match Darwin's headers. They matter here in the same narrow way the
 * Hypervisor stub's do: nothing proved reads them, but a value that collided
 * with another arm of the same switch would make a walked branch look
 * unreachable, so they are the real ones rather than placeholders.
 */

#pragma once

/* fcntl.h: return the path of an open fd. procemu.c and io.c use it to answer
 * /proc/self/fd/N and to re-resolve a host fd.
 */
#ifndef F_GETPATH
#define F_GETPATH 50
#endif

/* sys/socket.h: suppress SIGPIPE per socket rather than per process. Darwin's
 * answer to Linux's MSG_NOSIGNAL, which is why the socket layer reaches for it.
 */
#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0x1022
#endif

/* errno.h: too many references, cannot splice. Darwin defines it; the modeled
 * libc stops at the POSIX set. 59, as sys/errno.h and the "mac 59 -> linux 109"
 * arm of linux_errno() both say. 62 is Darwin's ELOOP, which is another arm of
 * that same switch, so the two must not share a value.
 */
#ifndef ETOOMANYREFS
#define ETOOMANYREFS 59
#endif
