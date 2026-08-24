/*
 * Socket option cache helpers
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

bool net_socket_fd_is_valid(int guest_fd);

/* Fold a failed host recv/read into Linux semantics: returns 0 (EOF) only for
 * the SEQPACKET-over-DGRAM socketpair substitute's peer-close ECONNRESET (guest
 * cached SO_TYPE SEQPACKET and host fd AF_UNIX SOCK_DGRAM), which Linux reports
 * as a clean end-of-stream; otherwise the translated negative Linux errno.
 * Genuine AF_UNIX datagram sockets are left untouched. Call only when the host
 * call returned < 0, on the pinned host fd; errno is preserved across the
 * internal socket probes.
 */
int64_t recv_eof_or_errno(int host_fd, int guest_fd);
bool net_socket_cached_int_get(int guest_fd,
                               int level,
                               int optname,
                               int *value);
bool net_socket_cached_int_get_if_generation(int guest_fd,
                                             uint64_t generation,
                                             int level,
                                             int optname,
                                             int *value);
void net_socket_cached_int_set(int guest_fd, int level, int optname, int value);
void net_socket_cache_set_index(int guest_fd, int idx, int value);
void net_socket_cache_init_defaults(int guest_fd, int domain, int real_type);
void net_socket_cache_init_accept(int guest_fd, int inherit_passcred);
