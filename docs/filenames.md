# Filenames

A Linux guest names files by exact bytes. The macOS volume underneath usually
does not. This document describes how a guest filename becomes a name on disk
and back again, and why the mechanism is shaped the way it is.

It is about *representation* only: which bytes end up in a directory entry, and
how they are read back. Which tree a path is resolved against (the sysroot or
the host filesystem) is a separate question, decided on existence by
`proc_resolve_sysroot_path_flags` (`src/syscall/proc-state.c`).

## The problem

A filesystem has to answer one question about two names: are they the same name?

```
Linux ext4          compares bytes         "Foo" != "foo" != "FOO"   three files
macOS APFS          case- and              "Foo" == "foo" == "FOO"   one file
(default volume)    normalization-blind
```

APFS is case-*preserving*: it stores exactly the bytes it is given, so `ls`
shows `Foo`. It only *matches* loosely. A guest running against a sysroot on
such a volume therefore needs two things that do not come for free:

1. `open("foo")` must fail when the directory holds `Foo`. The volume would
   happily open it.
2. `Foo` and `foo` must be able to coexist. The volume cannot hold both.

The first is answered by asking the volume for a name's stored spelling and
comparing bytes. The second needs one of the two names to be stored on disk
under a different spelling, and that is what the rest of this document is about.

A volume that compares byte-exactly (one made by `--create-sysroot`, or any
volume that probes case-sensitive) needs neither, and nothing here applies to
it.

## What the folding table actually does

Choosing which names are safe to store as themselves requires knowing what the
volume considers equal. It is considerably more than upper versus lower case.
Measured on macOS 15, APFS:

```
ascii Foo / foo                COLLIDE
french cafe NFC / NFD          COLLIDE   composed vs combining accent
german strasse / straße        COLLIDE   the sharp s folds onto "ss", so a
                                         fold can change a name's length
greek σoς / σoσ                COLLIDE   final sigma folds onto medial, so a
                                         fold can depend on position
ligature ﬁb / fib              COLLIDE   compatibility mapping, not just NFD
ohm Ωa / Ωa (U+2126 / U+03A9)  COLLIDE   singleton normalization
korean 한 NFC / jamo NFD        COLLIDE
deseret 𐐀y / 𐐨y                COLLIDE   case folding reaches beyond the BMP
chinese 文档 / 文件             distinct  CJK has no case
turkish ıd / id                distinct
cherokee Ꭰx / ᏸx               distinct  cased in Unicode 8, but not folded here
```

No rule short of full Unicode tables predicts that list, and elfuse carries no
Unicode tables and should not acquire any.

## Which names are stored as themselves

A name is stored under its own spelling when it is **fold-stable**: no uppercase
ASCII letter, no byte above `0x7F`, and not already shaped like an escape.
Everything else is escaped.

This is deliberately conservative: a Chinese filename has no case and would
usually be safe as itself. The rule still has to be correct for reasons
stronger than the table above. What it leaves stored literally is
lowercase ASCII, and lowercase ASCII is a **fixed point of every transformation
the volume applies**: case folding does nothing to it, canonical decomposition
does nothing to it, and compatibility decomposition maps *into* ASCII but never
between two distinct ASCII strings. Two literally stored names therefore cannot
collide, whatever is in the folding table and however it changes in a future
macOS release. That is a proof rather than an enumeration, which is what makes
it safe to rely on.

Escaping keys on the name alone and never on what the directory already holds.
That is what makes the on-disk name a pure function of the guest name, which in
turn is what lets two processes create colliding names in the same directory
without coordinating: they are writing different names.

Looking a name up still tries its literal spelling first, because a sysroot
staged from a tarball is full of names like `Makefile`, `README` and
`Documentation/` that were written by something other than elfuse and keep their
real spelling.

## The escape

```
.ef=524541444d45          is the guest name  README
.ef=e69687e6a1a32e747874                     文档.txt
```

An escaped name is the four-byte prefix `.ef=` followed by a payload encoding
the guest name's bytes. Decoding is a pure function of the name: no file, no
side table, nothing to keep in step.

Every character of the prefix earns its place. The leading `.` keeps escaped
names out of a casual host-side listing. `ef` identifies what produced them.
Four characters is as short as that can be while staying recognizable, and
length matters because the prefix is charged against the same per-name budget as
the payload. `=` is the separator rather than `^` because host tooling matches
this prefix constantly and `^` is a regular-expression anchor, so
`grep -E '.ef\^'` silently matches nothing.

A guest name that is itself escape-shaped is escaped too, so it can never be
confused with the encoding of another name:

```
guest name  .ef=464f4f   is stored as  .ef=2e65663d343634663466
                         and reads back as  .ef=464f4f,  not  FOO
```

A name that is not a well-formed escape means itself. `.ef=464F4F` (uppercase
hex), `.ef=464f4` (odd length), `.ef=2f` (decodes to `/`) and `.ef=` (empty) are
all ordinary files.

## Name length

Both platforms limit a name, but they do not count the same thing, and that
mismatch is what makes the encoding possible.

**Linux** limits a path component to 255 **bytes**, and bakes the limit into
`struct dirent`'s `d_name[256]`. No guest can hand over or receive a longer
name, so 255 bytes is a hard ceiling on the input.

**APFS** limits a component to 255 **UTF-16 code units**. `pathconf` reports 255
for both, which looks like a match and is not:

```
ascii U+0061           max 255 chars =  255 bytes = 255 utf16 units
latin-1 U+00E9         max 255 chars =  510 bytes = 255 utf16 units
BMP/CJK U+6587         max 255 chars =  765 bytes = 255 utf16 units
non-BMP U+1F680        max 127 chars =  508 bytes = 254 utf16 units
```

Every alphabet fails at 256 units regardless of byte count. So the host budget
in bytes swings by a factor of three with the alphabet, and an encoding that
spends units frugally can carry far more than an ASCII one.

That gives the payload two tiers:

| Guest name | Payload | Cost |
|---|---|---|
| up to 125 bytes | lowercase hex, 2 characters per byte | `4 + 2n` units |
| longer | 4096 CJK Unified Ideographs from U+4E00, 12 bits per character | `4 + 1 + ceil(2n/3)` units |

Hex caps out at 125 bytes because `4 + 2 * 126` is 256, one unit over. The short
tier exists anyway because an escaped name is then readable by eye (`xxd -r -p`
decodes it), and almost every escaped name a person ever sees is a short one.

The long tier carries the rest. Its first symbol holds the guest name's length,
so decoding knows exactly how many bytes the payload stands for; the remainder
packs three input bytes into two symbols. The largest name Linux can express
costs 175 of the 255 available units:

```
255-byte guest name -> .ef= + 171 symbols = 175 units, 517 bytes
```

There are 80 units to spare, and that margin is not an estimate: 255 bytes is
the largest input that can exist, so no name gets closer. Both tiers are held
to the limit by a `_Static_assert` in `src/syscall/casefold.h`, so widening the
prefix or raising the guest-name ceiling fails the build rather than producing
names the volume quietly refuses.

The payload block matters. CJK Unified Ideographs have no case mappings and no
decompositions, so no two payloads can fold together. Neighboring blocks are
not interchangeable: CJK **Compatibility** Ideographs normalize (U+F900
collides with U+8C48), Hangul syllables and dakuten kana decompose under NFD,
and Cherokee gained case in Unicode 8.

### Whole paths

Component length is solved; total path length is a separate budget, and here
macOS is the stricter of the two:

```
macOS PATH_MAX     1024
Linux PATH_MAX     4096
```

A guest may legitimately build a path more than three times longer than the host
can accept, and an escaped component is roughly twice the bytes of the name it
stands for, so a deep tree of escaped names reaches the host limit sooner. An
over-long host path reports `ENAMETOOLONG`; it is never truncated, because a
truncated path names a different file.

## Reproducing the measurements

Every table above is a measurement, not a specification, and can be re-run:

```sh
make probe-volume-naming                        # against a temp directory
build/probe-volume-naming /Volumes/cs-image     # against any other volume
```

The probe reports what a volume does, including behavior elfuse is immune to.
The facts the design actually depends on are asserted separately by
`make test-casefold-host`, which fails the build if a future macOS release
changes them: that the payload alphabet cannot fold, that everything the encoder
emits can be created, and that the per-name budget is counted in UTF-16 units.
That test also takes a directory, so it can be pointed at another volume.
