---
name: elfuse-conventions
description: elfuse conventions that are not guessable from the code: the seven-rule commit message style, ASCII-only source comments, kebab-case filenames, the type and return conventions at the ABI boundary, the untracked root working docs, and the rule that a tool checked out for evaluation never becomes a build dependency. Use when drafting a commit message or PR description, adding a new file or a new type, or wiring the build to anything a fresh clone would not have.
---

# elfuse conventions

Almost nothing here is enforced by a gate. `clang-format` settles formatting
and `make check-format` settles the generated files, so those are not in this
file; what is left is the set a reviewer would otherwise have to say out loud.

The one exception is these files themselves. `scripts/check-skill-refs.py`
fails when a skill names a file, make target, docs section, or sibling skill
that no longer exists. It checks every skill, plus any routing document you
name on the command line. Run it after renaming or moving anything the skills
point at, because a pointer that no longer resolves does not read as stale, it
reads as authoritative.

## Working docs

A contributor may keep untracked working docs at the repo root or under
`.claude/`; which ones exist differs per clone. Never `git add`, commit,
stage, gitignore, or delete another person's: untracked but visible in
`git status` is deliberate. None of them survive a fresh clone, so nothing
here defers to one; the load-bearing conventions are written out below,
because on a clean checkout `docs/` and these skills are all a contributor
gets.

## Style

Source comments and commit messages are ASCII only, with no markdown syntax:
no inline backticks, no non-ASCII arrows. Write code, path, and symbol
references as plain text (EPOLL_CTL_MOD, tests/foo.c). Markdown files are
exempt and use normal GitHub Markdown, so do not strip backticks out of a
`.md` file in its name.

The em dash (U+2014) is banned on every surface, markdown included: comments,
commit messages, `docs/`, these skill files, PR bodies, and review replies.
In an otherwise-ASCII tree it is the clearest mark of machine-written prose,
rejected on sight (PR#209). The stand-in is ` -- ` (spaced double hyphen),
used sparingly: reword first (commas, a period, or a colon usually serve),
and more than one in a paragraph means restructure. Write the character as
its codepoint, never the glyph, so `grep -rnP '\x{2014}'` stays a clean
check; `-P` with the codepoint is required, since `grep $'\u2014'`
matches nothing and reports a false all-clear.

The rest of the machine register is banned on the same surfaces. State the
fact and stop:

- Describe the thing, not the change: no "previously", "now we", "we
  refined", or "fixed", and no history of prior attempts or review rounds.
  A decision that still matters is present-tense rationale.
- Inflation words ("delve", "seamless", "robust", "leverage") and empty
  pivots ("it's worth noting"): a sentence that survives deleting the phrase
  never needed it.
- Coined vocabulary, figurative accounting, and anthropomorphism: every
  noun for a mechanism is an identifier in the tree or the standard term
  from a man page, ELF or FUSE clause, or kernel source, and a value is
  computed, cached, discarded, or re-derived. Name the flag or function
  carrying the fact; a coined word cannot be grepped later.
- Trailing "-ing" glosses (", ensuring ..."): the tail names a checkable
  mechanism or goes.
- Negative parallelism ("not X, but Y"): say Y. Copula avoidance ("serves
  as", "acts as"): write "is".
- Rule-of-three padding, stacked transitions ("Moreover"), wrap-ups ("In
  conclusion"), hedge stacking ("could potentially"): cut; one hedge at
  most, for real uncertainty.
- Signposting, prompt echo, and the closing verdict: no announcement of what
  the text is about to do ("This commit will", "Below we describe"), no
  first line restating the subject or the PR title, and no closing sentence
  grading the change ("this makes the code more maintainable"). The last
  sentence carries a fact.
- Effort and flattery: "carefully reviewed", "comprehensive", "thoroughly
  tested", "Great catch", "You're absolutely right". Effort is not a
  finding; name what ran and what it reported.
- Formatting as emphasis in docs and PR text: bolded bullet-header runs
  where a paragraph belongs, decorative rules, emoji.
- Machine artifacts, defects on sight: zero-width and bidi characters,
  homoglyphs, non-standard spaces, unfilled placeholders, leaked citation
  markup. Legitimate Unicode lives in `docs/` (the casefold tables), so scan
  the invisible class only, with a planted positive:

  ```
  grep -rnP '[\x{00A0}\x{200B}-\x{200F}\x{202A}-\x{202F}\x{2060}\x{FEFF}]' src/ tests/ docs/
  ```

Every class above binds every surface, so the sections below name only
their own instance of one.

Comments and commit messages are third-person: name the subject (the caller,
this function) or use imperative phrasing.

Filenames use kebab-case, never underscore. Symbol names use snake_case.

## Code

The conventions below are the ones that come from this project being a Linux
ABI reimplementation rather than an ordinary C program. They settle what a
declaration looks like, not whether the function should exist; for judging and
cleaning code that already works, `elfuse-refactor` is the calibration set.

Fixed-width types for anything the guest defines: guest addresses, Linux ABI
structures, binary layouts, protocol fields, page-table entries. Host-side
types (`size_t`, `ssize_t`, `int`, POSIX types) for host API calls, where they
match what the host declares. The distinction is not cosmetic; it is how a
reader tells which side of the boundary a value came from.

```c
uint64_t gva;       /* guest virtual address */
uint64_t ipa;       /* intermediate physical address */
size_t len;         /* host buffer length */
int host_fd;        /* macOS file descriptor */
```

Packed Linux ABI structures are explicit and live next to the translation code
that uses them, not in a shared header where they can drift away from the
conversion that gives them meaning.

Never pass a guest pointer to a host syscall. Copy through the guest memory
helpers so bounds checking and fault behavior stay in one place.

`bool` is for functions whose only meaningful outcome is success or failure.
`int` is reserved for returns that carry a number: errno-style codes, counts,
indices, or forwarding the status of an `int`-returning primitive. A helper
that only ever returns 0 or -1 and is read with a `< 0` test should have been
a `bool`; a helper that forwards a page-table or syscall primitive's status
should stay `int`.

New code goes in the existing domain file, not a new one. New shared
declarations go in the domain's `internal.h` or an existing header. A new lock
is documented in the lock-order comment before it is used from a second
module.

New C tests are `tests/test-<feature>.c` and use the shared harness macros
rather than rolling their own reporting.

## Vendored tools checked out for evaluation

A third-party tool checked out into an untracked directory to be tried out is
for evaluation and discovery only. Nothing in the build may come to depend on
it: no make target, no script, no CI job, and no documented workflow step. It
is absent from a fresh clone, so anything that reaches for it breaks for the
next person and cannot run in CI.

Run such a tool by hand from a scratch directory and leave any fixes to it
inside its own checkout. Record the outcome in the root working docs,
including when the answer was "evaluated, not integrated" - a rejected
evaluation is worth writing down so the next person does not repeat it. If the
output is worth keeping, land the finding in tree, not the tool.

Being untracked is not itself the test, and reading it that way gets the rule
backwards. The test fixtures are untracked too, and the build depends on them
on purpose: a make variable points at them, `make distclean` removes them, CI
restores them from a cache before the lanes that need them, and
`docs/testing.md` documents the paths the suites resolve. What makes that legitimate is that a tracked script fetches them, so
a fresh clone can reproduce the tree it needs. An evaluation checkout has no
such script, and that is the difference.

## Commit messages

The house style is Chris Beams' seven rules (https://cbea.ms/git-commit/), and
the log follows them closely enough that a deviation reads as an outsider
patch. `git log --no-merges --format=%s` is the calibration set if you want to
check the claim rather than take it.

1. Separate subject from body with a blank line.
2. Keep the subject within 50 characters. 72 is the hard ceiling, not the
   target; if you are past 50, the commit usually wants splitting.
3. Capitalize the subject.
4. No period at the end of the subject.
5. Imperative mood: "Fix the race", not "Fixed" or "Fixes" or "Fixing". The
   test is that the subject completes "If applied, this commit will ___".
6. Wrap the body at 72 columns. Do not let the editor reflow it into one line.
7. The body explains what and why, not how. The diff already shows how.

Real subjects from this tree, for calibration:

```
Stop nl_put_attr truncating its own extent
Prove the netlink walk loops
Count the shared pty declarations correctly
Give a pty's slave accounting one home
Say what the proof matrix cache actually costs
```

Note what is absent: no Conventional Commits prefix (`fix:`, `feat:`, `chore:`),
no area tag, no ticket number in the subject. The subject is a sentence about
behavior. A commit that removes something says what stops happening; a commit
that adds a proof says what is now proved.

The body is where the reasoning goes, and it is expected to be substantial for
anything non-mechanical: what the old code claimed, why that was wrong, what
breaks if you do it the obvious other way. Some commits label body paragraphs
(`Verified:`, `Coverage:`, `Concurrency:`) when a specific claim needs to be
findable later. Issue references go at the end as `Fixes #187`, `Closes #156`,
or the full URL form.

The ASCII and third-person rules above apply here too: no backticks around
symbol names, no em dashes, no non-ASCII arrows.

Merge commits keep git's generated subject and are exempt.

## Layout

All source under `src/`, artifacts under `build/`. The build passes `-Isrc`,
so headers are included as `core/guest.h`, `syscall/internal.h`,
`proved/gva.h`.

Tests in `tests/`, scripts in `scripts/`. Reports and analyses stay out of
the tree: keep them in a scratch directory or in a repo-root directory the
clone excludes through `.git/info/exclude`, never through `.gitignore`, which
would push one person's habit onto everybody.

Build and toolchain requirements are in `docs/testing.md`, section "Build
Requirements". They belong to a machine, not to this convention set.
