---
name: elfuse-refactor
description: Judging and cleaning elfuse C code that already works - which Refactoring smells carry weight in a Linux ABI reimplementation, which ones are false alarms in this tree, and what has to stay green before a change counts as behavior-preserving. Use when asked to clean up, simplify, deduplicate, or restructure, when reviewing a diff for maintainability rather than for bugs, or on the symptom alone: an over-long function, the same cleanup block repeated at many failure exits, a helper that returns 0 or -1 and is read with a < 0 test, a comment asserting a number. Not for adding a syscall (elfuse-syscall), for naming and commit style (elfuse-conventions), or for chasing a live failure (elfuse-debug).
---

# Refactoring elfuse code

Refactoring is behavior-preserving change, and in this tree the behavior that
must be preserved is what the guest observes, not what the host code returns.
That makes the two halves of this file: what is worth changing, and what has
to stay green before the change counts.

## What the gates actually measure

Worth knowing before treating a green build as a verdict. `.clang-tidy` enables
`bugprone-*`, `cert-*`, `clang-analyzer-*`, and `performance-*`, which answer
"is this a bug". The one structural check is `readability-function-size`, and
it measures line count only: nesting, statement, branch, and parameter
thresholds are explicitly off, and the functions that predate the ceiling carry
`NOLINTNEXTLINE`. Nothing counts a repeated block or a duplicated field list.
`make check-format` settles layout and the generated dispatch header.

Two things follow, and both have been claimed wrongly before. Read
`.clang-tidy` rather than this paragraph for the current check set: it changes,
and a stale summary of a gate is exactly the kind of assertion that reads as
verified.

First, none of it gates. `WarningsAsErrors` is empty and the tidy CI job says
in its own comment that it logs rather than fails. A ceiling here reports; it
does not stop anything.

Second, `make lint` has never been clean. It emits thousands of advisory
warnings, mostly `bugprone-multi-level-implicit-pointer-conversion`, none of
which fail a build. "Lint is clean" is therefore not a claim this tree
supports. The claims it does support are narrower and worth making precisely:
no new warnings inside the code you touched, and zero `function-size`
warnings.

So structure here is a judgment call with almost no compiler behind it, and
this file is the calibration set for the judgment.

## Measure before judging

Say which numbers were measured and which were eyeballed, and never quote a
count from a document, including this one. Recompute it. The tree is
clang-formatted, which is the only reason these one-liners are honest: a
function definition is a signature at column 0 followed by a lone `{`, and its
body ends at the first lone `}`.

```sh
# function lengths over 60 body lines, longest first
awk 'FNR==1{d=0} /^\{$/{d=1;s=FNR;next}
     d&&/^\}$/{if(FNR-s-1>60)printf "%5d  %s:%d\n",FNR-s-1,FILENAME,s;d=0}' \
    $(git ls-files 'src/*.c') | sort -rn

# statements at nesting depth 4 or deeper (4-space indent, so 20 columns in)
grep -rn '^ \{20\}[^ *]' --include='*.c' src

# file sizes
git ls-files 'src/*.c' | xargs wc -l | sort -rn | head -20
```

The nesting grep also catches wrapped continuation lines, so it is a lead, not
a count. Duplication has no mechanical detector here; `jscpd` and `lizard` are
not part of this build and adding a dependency to score a cleanup is not worth
it. Call duplication findings what they are: judgment, with the two locations
quoted so a reader can check the call.

Report a measured number with the command that produced it, and a judgment as
a judgment. A count with no command behind it reads as a fact and rots into a
wrong one.

## Behavior-preserving is a claim the lanes settle

`docs/testing.md`, section "Validation Strategy By Change Type", maps the area
touched to the minimum command set; a pure cleanup does not earn a smaller set
than the feature would have. `elfuse-verify` explains what a failure in each
lane means.

Three cases where "I only moved code" is wrong:

- Anything under `src/proved/`. The contracts move with the arithmetic, so
  `make verify` and `make verify-mutants` both have to pass again. A proof
  that still discharges after a rewrite but no longer rejects the mutant is a
  proof that got weaker while looking green.
- Anything that writes guest registers on the return path, sets page
  permissions, or touches the TLBI epilogue. That is guest-observable, so it
  is not a refactor until `elfuse-guest-abi` says the observable side is
  unchanged.
- Extracting a helper that acquires a lock. The lock order comment at the top
  of `src/syscall/internal.h` is the contract, and moving an acquire inside a
  callee changes the order at every call site at once.

## Smells that carry weight here

- A `sys_` function that has grown to hold several unrelated Linux behaviors.
  Length alone is not the finding: one syscall's flag matrix is allowed to be
  long, because each branch is a documented kernel behavior and splitting it
  scatters one contract. The finding is when the flag matrix, the path
  resolution, the host call, and the result translation are all inline in one
  body, because then no reader can tell which of the four a bug lives in.
- Shotgun surgery across the translation boundary. If a new syscall needs an
  edit in `src/syscall/translate.c`, a domain file, and two other places that
  each re-derive the same thing, the re-derivations are the bug.
- Dead flexibility: a parameter every caller passes the same value for, a hook
  with one implementation, scaffolding for a feature the root working docs
  still list as future work. Delete it; the git history keeps it.
- A raw ABI literal in a domain file. The constant belongs in
  `src/syscall/abi.h` with a name, and the domain file uses the name.
- A helper that returns 0 or -1 and is only ever read with `< 0`. That is a
  `bool` wearing an `int`, and `elfuse-conventions` says which is which.
- A comment that asserts a number or a guarantee. Both rot silently and both
  read as checked. A comment saying a buffer is "~100KiB" when the type it
  sizes has since grown, or saying a check "has to be declared in the diff"
  when `WarningsAsErrors` is empty and the CI job logs rather than gates, is
  worse than no comment: the next reader trusts it instead of measuring. When
  a refactor moves a comment, its claims move with it unverified, so recompute
  the number and re-read the config the sentence describes. This is the same
  failure `scripts/check-skill-refs.py` exists to catch in the skills, and
  nothing catches it in `src/`.

## Smells that are false alarms in this tree

Each of these is a textbook finding that a generic audit reports and that is
correct code here.

- Two structures that look alike across the ABI boundary. A Linux `stat` and a
  macOS `stat` are not duplicated knowledge; they are two knowledges that
  happen to rhyme, and merging them puts one definition where a translation
  belongs. `elfuse-conventions` requires packed Linux structures to live next
  to the translation code that reads them, precisely so they can diverge.

  A translation and its inverse are the opposite case, and the resemblance is
  easy to wave off with the rule above. Which Linux bit means which macOS bit
  is one fact, and a hand-written forward function plus a hand-written reverse
  function state it twice, so a bit added to one silently misses the other and
  nothing fails. State it once as a table and walk it in both directions. The
  test is whether the two bodies encode the same correspondence or two
  different ones.
- Repeated switches at the translation boundary. errno, `AT_*` flags, clock
  ids, and socket flags each get their own switch in
  `src/syscall/translate.c`. That concentration is the design; the smell is a
  conversion written inline somewhere else.
- The wall of `sc_` wrappers in `src/syscall/syscall.c`. They look like
  boilerplate begging for a loop; they are the typed unpack the generated
  dispatch table calls. Moving one into a domain file yields a symbol nothing reaches.
- `uint64_t gva` as primitive obsession. The fixed-width type is the signal
  that says which side of the boundary the value came from. A wrapper type
  removes information rather than adding it.
- A `_Thread_local` slot that "should" be shared state. `cpu_tlbi_req` is
  per-vCPU on purpose, and promoting it to guest-global reintroduces the
  cross-vCPU drain race it was split to fix.
- A test that makes no assertion. Guest fixtures like `tests/test-argc.c` only
  print; the oracle is the shell lane that greps their output, so an
  assertion-free `main` is the design rather than a gap. Among the tests that
  do run in-process, `EXPECT_EQ` and friends are a convenience layer over
  `TEST` / `PASS` / `FAIL` in `tests/test-harness.h`, so a file using only the
  latter is fully asserted. Judge a test by whether something fails when the
  behavior breaks, not by which macro it spells.
- Apparently dead code. Before deleting a symbol, grep `src/syscall/dispatch.tbl`,
  `src/core/shim.S`, and `tests/` for it. Reachability here runs through a
  generated table, hand-written assembly, and test lanes, so the C call graph
  alone does not decide it.

## Where a textbook refactor is a bug

- Vector entry stubs in `src/core/shim.S` must not clobber any GPR before
  `svc_handler`. A shared prologue macro that uses one scratch register
  corrupts the EL0 caller after ERET, and the tests that notice are not the
  ones a cleanup runs.
- The paths that hand the shim its drop-frame marker in X8 look like one
  duplication and are not safely unified. Which paths those are has already
  changed once, so grep for the writes rather than trusting any list,
  including this sentence: `grep -rn 'HV_REG_X8' src/` shows who sets it and
  to what, and `src/core/shim.S` documents what each value means. They differ
  in whether they return through the dispatch epilogue at all, so read
  `elfuse-guest-abi` first; a wrong unification ties the handler PC to a stale
  syscall frame and only shows up under signals.
- The generated dispatch header is generated. Edit `src/syscall/dispatch.tbl`
  and rerun `make check-format`.

## Leave it cleaner, in proportion

Every edit is a chance to leave one thing better, and the tree reached
four-figure functions because nobody took it. But "in proportion" is the
load-bearing half, and in this tree it has a mechanical edge that a good
intention does not survive.

Do not run `clang-format -i` on a whole file to tidy a small change. On the
current tree that pass is a no-op, since every file `make indent` selects
formats to itself with no diff, so it buys nothing. What it can cost is
everything: a clang-format that is not version 22 reformats the whole tree at
once and buries the change, which is why `CONTRIBUTING.md` pins the version
rather than asking for 22 or newer.

`make indent` also runs `commentflow`, and since the one-time reflow landed it
is a no-op too. Treat a tree-wide diff from either half as a signal rather than
as cleanup: it means the formatter you have is not the one the gate runs.

Format the file only when you added code to it, and check `git diff --numstat`
before calling the work done: a file you meant to touch in one line reporting
twenty is churn, not cleanup. The same goes for a scripted edit that rewrites
whole files.

The proportionality test is the diff, not the intent. A cleanup that lands in
a file the task never needed to open is a separate change, and it costs the
next reader a bisect.

## Fix it now, or write it down

Most findings are not for the change you are making. Fix one now only when it
blocks the task, when it is a live risk, or when your own edit introduced it.
Everything else gets reported, in the summary or in the root working docs, and
that report is the deliverable rather than a consolation prize: a named,
located finding is worth more than a drive-by fix nobody asked to review.

Two cases decide themselves. A correctness bug found while cleaning is never
folded into the cleanup, because a diff that both moves code and changes
behavior cannot be reviewed as either. A finding whose fix would touch a
subsystem the task never opened is a separate change, however obvious it looks
from here.

## Before a multi-step cleanup

Run the lanes before touching anything, and keep the output. This tree is not
green everywhere: `make check-format` fails on shell scripts, `make lint`
emits thousands of advisory warnings, and neither is your doing. Without that
baseline you will spend the session unable to tell your breakage from the
breakage you inherited, or worse, you will report someone else's red as your
own.

Then work in batches that each end green, one smell family or one file at a
time, and say after every batch which lanes ran. When a lane cannot run, name
it and say what risk that leaves rather than rounding up to success. When the
baseline is already red in the area you are about to change, stop and report
instead of refactoring on top of it.

## Sequencing

One behavior-preserving step per commit, each with its own green lanes, so a
bisect lands on a step rather than on a rewrite. A commit that both moves code
and changes behavior cannot be reviewed as either. Subject lines follow the
seven rules in `elfuse-conventions`, and a cleanup commit says what stops
happening, not that things were cleaned up.

## Authoritative sources

This skill is a working summary. These are tracked and survive a fresh clone,
so prefer them when the two disagree:

- `.clang-tidy` - the enabled check set, which is what `make lint` decides.
- `docs/testing.md`, section "Validation Strategy By Change Type" - the change
  area to command mapping.
- `src/syscall/internal.h` - the lock order comment at the top.
