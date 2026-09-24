<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Documentation

The documentation standard of Oxys-OS, binding under `PROJECT_GUIDELINES.md`,
Section 11. It says where each kind of fact is written, the form each kind of
document takes, and what a change must update. `make docs-check` enforces the
parts that can be checked mechanically.

## 1. Where to look

| Group | Holds | Index |
| ----- | ----- | ----- |
| [`project/`](project/) | How the work is run: the plan, the status, the tests, the builds, the toolchain, the standards, the references. | [`project/README.md`](project/README.md) |
| [`design/`](design/) | The kernel and userland subsystems: how each works and why. | [`design/README.md`](design/README.md) |
| [`devices/`](devices/) | The hardware drivers, one document per device. | [`devices/README.md`](devices/README.md) |
| [`storage/`](storage/) | From a medium to a file: disk drivers, block layer, cache, EXT2, VFS, ramdisk, persistent `/etc`. | [`storage/README.md`](storage/README.md) |

Each source directory's `README.md` indexes that directory's files. A design
document explains a subsystem; a directory `README.md` says which file holds
what. Neither repeats the other.

## 2. One home per fact

Every fact has exactly one home, below. Anywhere else, link to it.

| Fact | Its home |
| ---- | -------- |
| How a subsystem works, and why it was built that way | Its design, device or storage document |
| What a file contains | The `README.md` of its directory, one line |
| The detail of a function or a structure | The source file's comments |
| Which sub-tasks exist and their state | [`project/PLAN.md`](project/PLAN.md) |
| What works now, where it was observed, what is missing | [`project/STATUS.md`](project/STATUS.md) |
| That a change was made, and in which commit | [`project/HISTORY.md`](project/HISTORY.md), one line |
| Why a change was made, what was found, the negative tests | The commit message |
| That a test was run, where, and with what result | [`project/TESTING-RECORD.md`](project/TESTING-RECORD.md), one line |
| How to run the tests | [`project/TESTING.md`](project/TESTING.md) and its companions |
| Every image built | [`project/builds.tsv`](project/builds.tsv) |
| A specification relied upon | [`project/REFERENCES.md`](project/REFERENCES.md) |

## 3. The form of each kind of document

### 3.1 Design, device and storage documents

```
# <Subject>

**Phase**: <phase and sub-task(s)> of project/PLAN.md.
**Source**: <files that implement it>.
**Specifications**: <sections relied upon, registered in REFERENCES.md>.

<One paragraph: what this is and what it is for.>

## 1. ... ## n.   The design, one section per concern.
## Verification   The self-test's assertions.
## Limitations    What it does not yet do.
```

- **Present tense, current state.** Describe how the subsystem works now. No
  dates, no "since", no "was", no struck-through text, no story of how it came
  to be. When the behaviour changes, rewrite the text; git keeps the old.
- **Every decision with its reason.** State the decision, then the silent
  failure it prevents, in a sentence or two. A paragraph that only restates the
  code is cut.
- **Verification** is a two-column table: *property asserted* and *the silent
  failure it would catch*. Name the self-test file once, above the table.
- **Limitations** is a numbered list of what is true today. A limitation that
  is resolved is deleted, and the design section is rewritten to describe the
  new behaviour.
- **Citations** name the specification and section, and the specification is
  registered in `REFERENCES.md`.
- **Length.** A subsystem document should be readable in one sitting; above
  about 4,000 words, split it by the reader's question and index the pieces.

### 3.2 `project/PLAN.md`

The roadmap: one table per phase, a row per sub-task with its **State**
(`Planned`, `In progress`, `Implemented`) and its design document. Below the
tables, one short paragraph naming the next sub-task. No narrative of past
work.

### 3.3 `project/STATUS.md`

Tables only: the capabilities that work, each with its design document; the
environments each phase was observed in; the known gaps, each with the
sub-task expected to close it; the current number of self-test assertions.

### 3.4 `project/HISTORY.md`

One row per change: date, phase, one line saying what changed (at most about
a hundred and sixty characters), and the commit. The commit message holds the
rest.

### 3.5 `project/TESTING-RECORD.md`

One row per test run: date, environments, what was tested, the result in a
few words. The commit message of the change tested holds the detail.

### 3.6 A directory `README.md`

```
# `<dir>/` — <subject>

**Phase**: <n>. **Documentation**: <design document>.

<Two or three sentences: what the directory is for.>

| File | What it holds |            one line per file
| Specification | Applied to |     where the directory applies one
```

## 4. Style

- Formal, precise, present tense; British spelling, as the corpus uses.
- Lead with the rule, then the reason. Prefer a table to a list and a list to
  a paragraph wherever the content is an enumeration.
- Link rather than repeat. A sentence copied into a second document is a
  sentence that will disagree with the first.
- A document says what *is*. A comment in a commit says what *changed*.

## 5. What a change updates

In the same commit as the code:

1. The design document of each subsystem whose behaviour changed, rewritten to
   the new behaviour.
2. The directory `README.md`, if a file was added, removed or repurposed.
3. `PLAN.md`, if a sub-task changed state.
4. `STATUS.md`, if a capability, an environment result, a known gap or the
   assertion count changed.
5. One line in `HISTORY.md`, filled with the hash by the second commit.
6. One line in `TESTING-RECORD.md` for each test run recorded.

A correction to documentation alone updates only the document corrected.
