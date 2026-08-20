/*
 * Frozen on-disk spellings for the casefold escape
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Each row pairs a guest filename with the exact bytes the codec stores it
 * under. The values are the on-disk format: every sysroot ever written holds
 * names spelled this way, so a row may change only together with a deliberate
 * format migration, never to make a test pass. The codec's own tests all read
 * through the codec and therefore stay green across any self-consistent format
 * change; only a comparison against these frozen literals fails when the format
 * moves. test-casefold-host.c asserts every row in both directions, and the
 * mk/tests.mk corpus recipe stages a subset host-side for the guest corpus test
 * to read back.
 */

#pragma once

/* Doubling blocks compose the repeated-payload rows as compile-time literals,
 * so even the full-length spellings are frozen bytes rather than something a
 * loop derives from the codec under test.
 */
#define CFV_X2 "XX"
#define CFV_X4 CFV_X2 CFV_X2
#define CFV_X8 CFV_X4 CFV_X4
#define CFV_X16 CFV_X8 CFV_X8
#define CFV_X32 CFV_X16 CFV_X16
#define CFV_X64 CFV_X32 CFV_X32
#define CFV_X125 CFV_X64 CFV_X32 CFV_X16 CFV_X8 CFV_X4 "X"
#define CFV_X126 CFV_X64 CFV_X32 CFV_X16 CFV_X8 CFV_X4 CFV_X2
#define CFV_X255 \
    CFV_X64 CFV_X64 CFV_X64 CFV_X32 CFV_X16 CFV_X8 CFV_X4 CFV_X2 "X"

#define CFV_B2 "bb"
#define CFV_B4 CFV_B2 CFV_B2
#define CFV_B8 CFV_B4 CFV_B4
#define CFV_B16 CFV_B8 CFV_B8
#define CFV_B32 CFV_B16 CFV_B16
#define CFV_B64 CFV_B32 CFV_B32
#define CFV_B125 CFV_B64 CFV_B32 CFV_B16 CFV_B8 CFV_B4 "b"

/* "58" is hex for 'X'; 125 repetitions spell the hex-tier maximum. */
#define CFV_H2 "5858"
#define CFV_H4 CFV_H2 CFV_H2
#define CFV_H8 CFV_H4 CFV_H4
#define CFV_H16 CFV_H8 CFV_H8
#define CFV_H32 CFV_H16 CFV_H16
#define CFV_H64 CFV_H32 CFV_H32
#define CFV_H125 CFV_H64 CFV_H32 CFV_H16 CFV_H8 CFV_H4 "58"

/* Long-tier symbols, one UTF-8 literal per code point. The tier packs the name
 * MSB-first into 12-bit groups and adds U+4E00 to each; a leading symbol
 * carries the byte length. 'X' = 0x58 repeated gives the periodic groups
 * 0x585/0x858 -> U+5385/U+5658, so one two-symbol pair covers three payload
 * bytes: 126 bytes = 1008 bits = exactly 42 pairs, 255 bytes = 2040 bits =
 * exactly 85 pairs. Length symbols: 126 -> U+4E7E, 255 -> U+4EFF.
 */
#define CFV_LEN126 "\xe4\xb9\xbe"           /* U+4E7E */
#define CFV_LEN255 "\xe4\xbb\xbf"           /* U+4EFF */
#define CFV_PAIR "\xe5\x8e\x85\xe5\x99\x98" /* U+5385 U+5658 */
#define CFV_PAIR2 CFV_PAIR CFV_PAIR
#define CFV_PAIR4 CFV_PAIR2 CFV_PAIR2
#define CFV_PAIR8 CFV_PAIR4 CFV_PAIR4
#define CFV_PAIR16 CFV_PAIR8 CFV_PAIR8
#define CFV_PAIR32 CFV_PAIR16 CFV_PAIR16
#define CFV_PAIR64 CFV_PAIR32 CFV_PAIR32
#define CFV_PAIR42 CFV_PAIR32 CFV_PAIR8 CFV_PAIR2
#define CFV_PAIR85 CFV_PAIR64 CFV_PAIR16 CFV_PAIR4 CFV_PAIR

/* The one non-periodic long-tier row, 'A' then 125 x 'b': groups 0x416, then
 * 0x262/0x626 alternating, 84 symbols in all: 0x416 once, then 41 pairs of
 * (0x262, 0x626), then a final 0x262. U+5216 / U+5062 / U+5426.
 */
#define CFV_AB_HEAD "\xe5\x88\x96"             /* U+5216 */
#define CFV_AB_PAIR "\xe5\x81\xa2\xe5\x90\xa6" /* U+5062 U+5426 */
#define CFV_AB_PAIR2 CFV_AB_PAIR CFV_AB_PAIR
#define CFV_AB_PAIR4 CFV_AB_PAIR2 CFV_AB_PAIR2
#define CFV_AB_PAIR8 CFV_AB_PAIR4 CFV_AB_PAIR4
#define CFV_AB_PAIR16 CFV_AB_PAIR8 CFV_AB_PAIR8
#define CFV_AB_PAIR32 CFV_AB_PAIR16 CFV_AB_PAIR16
#define CFV_AB_PAIR41 CFV_AB_PAIR32 CFV_AB_PAIR8 CFV_AB_PAIR
#define CFV_AB_TAIL "\xe5\x81\xa2" /* U+5062 */

/* A row whose @host equals its @guest is stored literally; any other row is
 * escaped, and the test asserts the escape byte-exact in both directions.
 */
struct casefold_vector {
    const char *label;
    const char *guest;
    const char *host;
};

static const struct casefold_vector casefold_vectors[] = {
    {"lowercase ascii", "config.json", "config.json"},
    {"old index name", ".elfuse_case_index", ".elfuse_case_index"},
    {"uppercase first", "Foo", ".ef=466f6f"},
    {"all uppercase", "README", ".ef=524541444d45"},
    {"cjk with suffix", "\xe6\x96\x87\xe6\xa1\xa3.txt",
     ".ef=e69687e6a1a32e747874"},
    {"escape-shaped", ".ef=464f4f", ".ef=2e65663d343634663466"},
    {"nfc accent", "caf\xc3\xa9", ".ef=636166c3a9"},
    {"nfd accent", "cafe\xcc\x81", ".ef=63616665cc81"},
    {"eszett",
     "stra\xc3\x9f"
     "e",
     ".ef=73747261c39f65"},
    {"eszett fold target", "strasse", "strasse"},
    {"final sigma", "\xcf\x83o\xcf\x82", ".ef=cf836fcf82"},
    {"deseret", "\xf0\x90\x90\x80y", ".ef=f090908079"},
    {"invalid utf-8", "bad\xff", ".ef=626164ff"},
    {"hex-tier max", CFV_X125, ".ef=" CFV_H125},
    {"long-tier min", CFV_X126, ".ef=" CFV_LEN126 CFV_PAIR42},
    {"linux name max", CFV_X255, ".ef=" CFV_LEN255 CFV_PAIR85},
    {"long-tier mixed", "A" CFV_B125,
     ".ef=" CFV_LEN126 CFV_AB_HEAD CFV_AB_PAIR41 CFV_AB_TAIL},
};

/* The frozen lengths follow from the packing arithmetic above; a literal that
 * stops matching them was mis-composed, not a format change.
 */
_Static_assert(sizeof(CFV_X125) - 1 == 125, "hex-tier guest length");
_Static_assert(sizeof(CFV_X126) - 1 == 126, "long-tier guest length");
_Static_assert(sizeof(CFV_X255) - 1 == 255, "max guest length");
_Static_assert(sizeof(".ef=" CFV_H125) - 1 == 254, "hex-tier host length");
_Static_assert(sizeof(".ef=" CFV_LEN126 CFV_PAIR42) - 1 == 259,
               "long-tier host length");
_Static_assert(sizeof(".ef=" CFV_LEN255 CFV_PAIR85) - 1 == 517,
               "max host length");
