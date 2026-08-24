/*
 * CommonCrypto/CommonDigest.h, for the analyzer only
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * core/rosetta.c keys its AOT cache by the SHA-256 of the guest binary, and
 * takes it through Darwin's CommonCrypto. Only the SHA-256 arm is declared,
 * since that is all the file names.
 *
 * The context layout is copied from the SDK header rather than invented. It is
 * a local whose address is passed to all three calls, so the analyzer has to
 * agree with the compiler about its size for anything it concludes about the
 * surrounding frame to be about the real program. The functions are
 * declarations only: what SHA-256 computes is not a property any proof here
 * rests on, and a hand-written body would only give the analyzer a second
 * implementation to be wrong about.
 *
 * Same placement rule as the Hypervisor stub: outside src/, reachable only
 * through FRAMAC_STUB_DIR in mk/verify.mk, never on a compile's include path.
 */

#pragma once

#include <stdint.h>

#define CC_SHA256_DIGEST_LENGTH 32

typedef uint32_t CC_LONG;

typedef struct CC_SHA256state_st {
    CC_LONG count[2];
    CC_LONG hash[8];
    CC_LONG wbuf[16];
} CC_SHA256_CTX;

int CC_SHA256_Init(CC_SHA256_CTX *c);
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len);
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c);
