#!/usr/bin/env python3
"""Fail when a function carrying an ACSL contract is missing from the proof set.

Frama-C ASSUMES the contract of any function it is not asked to prove. A
contracted helper left out of -wp-fct therefore becomes an unchecked axiom that
every goal above it rests on, and the gate still reports success. That is not
hypothetical: hex_nibble sat outside the RSP proof set, and replacing its body
with "return 15;" -- which contradicts its own ensures -- still produced
"PROVED 74 of 74", because gdb_parse_hex's termination and gdb_hex_decode's
stop-at-NUL both follow from that contract and from nothing else.

Usage:
    check-acsl-coverage.py --fcts "a b c" FILE [FILE ...]

Exits non-zero, naming the offenders, when a scanned file carries an ACSL
contract that is not in the proof set -- or one this script cannot attribute to
a function at all, since a contract it cannot read is one it cannot confirm.
"""

import argparse
import re
import sys

# ACSL annotations. Both forms count: Frama-C assumes a contract written with
# one-line //@ exactly as readily as a /*@ ... */ block, so scanning only the
# block form would let two characters re-open the very hole this script exists
# to close.
ACSL_BLOCK = re.compile(
    r"/\*@.*?\*/"  # /*@ ... */
    r"|^[ \t]*//@[^\n]*(?:\n[ \t]*//[^\n]*)*",  # //@ ..., plus its continuations
    re.DOTALL | re.MULTILINE,
)

# Annotations that are not function contracts, so have no function to attribute
# to: logic declarations (no C definition at all) and statement annotations
# (attached to a loop or a statement inside a body already covered by its own
# function's contract).
NOT_A_CONTRACT = re.compile(
    r"^\s*(predicate|logic|axiomatic|lemma|type|ghost"
    r"|loop|assert|check|admit|assume|breaks|continues|returns)\b"
)

# A C function definition header, up to the opening brace. Deliberately loose:
# the codebase writes one parameter per line, so match across newlines and take
# the identifier that precedes the parameter list.
DEFINITION = re.compile(
    r"""^[ \t]*                       # start of a line
        (?:static\s+|inline\s+)*      # storage/inline qualifiers
        [A-Za-z_][A-Za-z_0-9\s*]*?    # return type, possibly with pointers
        \b(?P<name>[A-Za-z_][A-Za-z_0-9]*)\s*   # the function name
        \([^;{]*?\)\s*                # parameter list
        \{                            # a body, not a declaration
    """,
    re.VERBOSE | re.DOTALL,
)


def contracted_definitions(path):
    """Contracted function names in @path, plus contracts not attributable to one.

    Returns (names, unattributed). Anything this cannot identify goes into
    `unattributed` and fails the run rather than being skipped: silently
    ignoring a contract is precisely the failure this script exists to prevent,
    so an unrecognized construct must be loud, not absent. A preprocessor
    directive or an ordinary comment between the annotation and the function is
    enough to defeat a regex, so those are stepped over explicitly.
    """
    with open(path, encoding="utf-8") as handle:
        text = handle.read()

    names, unattributed = [], []
    for block in ACSL_BLOCK.finditer(text):
        raw = block.group(0)
        if raw.startswith("/*"):
            body = raw[3:-2]
        else:
            # //@ form: drop the marker from the first line and the leading //
            # from any continuation lines.
            body = "\n".join(
                re.sub(r"^[ \t]*//@?", "", line) for line in raw.splitlines()
            )
        if NOT_A_CONTRACT.match(body.lstrip("\n")):
            continue

        # Step over what can legitimately sit between an annotation and the
        # function it applies to: further annotation blocks (a predicate
        # declared just above its user), ordinary comments, and preprocessor
        # directives.
        rest = text[block.end() :]
        while True:
            stripped = rest.lstrip()
            if stripped.startswith("/*"):
                rest = stripped[stripped.index("*/") + 2 :]
            elif stripped.startswith("//") or stripped.startswith("#"):
                newline = stripped.find("\n")
                if newline < 0:
                    rest = ""
                    break
                rest = stripped[newline + 1 :]
            else:
                rest = stripped
                break

        match = DEFINITION.match(rest)
        if match:
            names.append(match.group("name"))
        else:
            unattributed.append(first_line(rest))
    return names, unattributed


def first_line(text):
    """First non-empty line of @text, for an error message."""
    for line in text.splitlines():
        if line.strip():
            return line.strip()[:72]
    return "<end of file>"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fcts", required=True, help="space-separated names passed to -wp-fct"
    )
    parser.add_argument(
        "--target", default="the proof", help="proof name, for the error message"
    )
    parser.add_argument("files", nargs="+")
    args = parser.parse_args()

    proved = set(args.fcts.split())
    missing, unattributed = [], []
    for path in args.files:
        names, strays = contracted_definitions(path)
        missing += [(path, n) for n in names if n not in proved]
        unattributed += [(path, s) for s in strays]

    if unattributed:
        sys.stderr.write(
            "  ACSL coverage: %d contract(s) not attributable to a function\n"
            % len(unattributed)
        )
        for path, excerpt in unattributed:
            sys.stderr.write("           %s: before %r\n" % (path, excerpt))
        sys.stderr.write(
            "           Refusing to guess: a contract this script cannot read\n"
            "           is one it cannot confirm is proved, which is the blind\n"
            "           spot it exists to close.\n"
        )
        return 1

    if missing:
        sys.stderr.write(
            "  ACSL coverage: %d contracted function(s) are assumed, not proved\n"
            % len(missing)
        )
        for path, name in missing:
            sys.stderr.write("           %s: %s\n" % (path, name))
        sys.stderr.write(
            "           Frama-C assumes the contract of any function outside\n"
            "           -wp-fct, so each of these is an unchecked axiom the\n"
            "           rest of %s rests on. Add them to the proof set, or\n"
            "           drop their contracts if they are not load-bearing.\n"
            % args.target
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
