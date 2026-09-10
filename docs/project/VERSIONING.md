# Release Versioning

**Document status**: Living document. It governs every release this project ever
publishes and is amended in the same change as any decision that alters the
scheme.

**Authority**: `PROJECT_GUIDELINES.md`, Section 7. The scheme itself is the
project owner's; this document records it so that the decision is made once
rather than argued at each release.

**Where this sits**: [`PLAN.md`](PLAN.md) says what is being built and
[`HISTORY.md`](HISTORY.md) says how each change came about. Neither says anything
about releases, and neither should: a commit and a release are different things
published to different audiences. This document is the third question — **what
does a released thing get called, and why that** — and it is the only place the
answer lives.

**Nothing has been released.** The scheme below is written before the first
release rather than after it, which is the point: a naming scheme invented at the
moment it is first needed is invented under pressure, and the first release is
then the one that establishes by accident whatever the scheme fails to state.
Section 11 records what happened to the one tag this repository has ever carried.

## 1. The premise

**One repository, many releases.** `OxysOS` stays; the releases are versions of
it. There is no branch per release, no repository per release and no fork; a
release is a tag upon `main` and an image built from it.

That premise is the constraint everything below answers to. Every rule here that
looks arbitrary is there because the alternative would have required a second
branch to live for longer than an afternoon, and a second long-lived branch is
how "one repository" quietly stops being true.

## 2. The four parts of a release identity

A release is named by up to four things. Only the first is compulsory.

| Part | Example | What it is |
| ---- | ------- | ---------- |
| **Ordinal** | `1`, `2`, `4` | Which major release this is. Compulsory, never repeated, never skipped, and it only goes up. |
| **Point** | the `.1` of `1.1` | A smaller successor within the same ordinal. Optional. |
| **Name** | `Aetos` | A word standing for the ordinal. Optional, and an alias — **not a fifth level**. |
| **Edition** | `Workspace`, `Compatibility` | A variety of a release rather than a successor to it. Optional. |

So `Oxys Aetos` **is** `Oxys 4`. They are not two releases and not two levels of
one; they are two ways of writing the same ordinal, and the ordinal is the one
that is authoritative.

## 3. The three written forms

Every release can be written three ways, and which one to use depends entirely on
who is reading.

| Form | Shape | Where it is used |
| ---- | ----- | ---------------- |
| **Ordinal form** | `4`, `4.1`, `4-workspace` | Anything a machine reads or sorts: the git tag, the image filename, the checksum manifest, `OXYS_VERSION_STRING`. Lower case, no spaces, no name. |
| **Full form** | `Oxys 4 "Aetos" Workspace Edition` | The release title, the tag annotation, and the first line of the release notes. Everything at once, so that the ordinal is recoverable from the title alone. |
| **Short form** | `Oxys Aetos Workspace Edition` | Prose, conversation, the desktop's own "about" panel. What a person actually says. |

Worked through the examples this scheme was written for:

| Ordinal form | Full form | Short form |
| ------------ | --------- | ---------- |
| `1` | `Oxys 1` | Oxys 1 |
| `1.1` | `Oxys 1.1` | Oxys 1.1 |
| `2` | `Oxys 2` | Oxys 2 |
| `4` | `Oxys 4 "Aetos"` | Oxys Aetos |
| `4.1` | `Oxys 4.1 "Aetos"` | Oxys Aetos 4.1 |
| `4-workspace` | `Oxys 4 "Aetos" Workspace Edition` | Oxys Aetos Workspace Edition |
| `4-compatibility` | `Oxys 4 "Aetos" Compatibility Edition` | Oxys Aetos Compatibility Edition |

**A release with no name has no short form distinct from its full form**, which
is why `Oxys 1` appears twice above. That is not a defect in the table; an
unnamed release simply has less to say.

### 3.1 `.0` is never written

`Oxys 1`, never `Oxys 1.0`. A release with no point release does not have one, and
writing a zero there would be a claim that the level exists and is empty.

It matters for a second reason. `1.0` invites `1.0.1`, and a third level is
exactly what this scheme does not have (Section 5.2). Never writing the second
level when it is unused is the cheapest way to stop a third from being invented
by somebody who assumed it was already there.

## 4. When a release takes a new ordinal, and when it takes a point

**A new ordinal is for a release that changes what the system is.** A phase of
[`PLAN.md`](PLAN.md) completed, a subsystem that did not exist before, a boot path
that did not exist before, a thing the machine can now do that it could not do at
all.

**A point is for a release that changes what the system is like.** Appearance,
the utilities, defects corrected, one more device driven, a default reconsidered.
Smaller than a new ordinal, mostly at the front, and still large enough that
somebody would want to install it.

The distinction is the project owner's judgement and **is recorded in the release
notes of the release it applies to**. That is not a hedge. Any rule sharp enough
to decide every case would decide some of them wrongly, and a decision written
down at the time it was made is worth more than a rule that has to be reasoned
about afresh whenever the boundary is unclear.

### 4.1 A point release promises nothing about the next one

`Oxys 1.1` does not imply that `Oxys 1.2` will exist. The release after `Oxys 1.1`
may be `Oxys 1.2`, or it may be `Oxys 2`; that is decided when it is cut and not
before.

This is stated because the opposite is what people assume. A version scheme that
counts is read as a scheme that *promises to keep counting*, and somebody waiting
for `1.2` before upgrading would be waiting for a release nobody ever intended to
make.

### 4.2 Neither ordinals nor points skip

There is no `Oxys 3` if there was no `Oxys 2`, and no `Oxys 4.2` if there was no
`Oxys 4.1`. A gap in a sequence makes a reader look for the release that is
missing, and there is never one to find.

The sequence of points simply **ends** rather than skipping: `4`, `4.1`, then
`5`. Nothing is missing between `4.1` and `5`; the fourth release finished having
points.

## 5. What this scheme is not

### 5.1 It is not semantic versioning

`Oxys 1.1` and `Oxys 2` say how large a release is. They say **nothing whatever**
about interface compatibility, and no rule here permits them to be read that way.

This is the misreading worth guarding against, because it is silent. Semantic
versioning gives `2.0` a specific meaning — a breaking change — and somebody who
brings that expectation here would conclude from `Oxys 1.1` that the system-call
numbers had not moved, which this project has never promised. What it has
promised is in [`../../LICENSING.md`](../../LICENSING.md), Section 2, and in the
system-call numbering rule of
[`../design/PRIVILEGE.md`](../design/PRIVILEGE.md), Section 9.6: a number handed
to a program is one that must not change. That promise is made by the interface
and not by the version number.

### 5.2 There is no third level

No `1.1.2`. Two levels, and the second is optional.

A correction to a released image is therefore **a point release**, not a
sub-point: if `Oxys 1` needs a fix, the fix is `Oxys 1.1`; if `Oxys 1.1` needs one,
it is `Oxys 1.2`. That is a slightly heavier label than the change deserves, and
it is chosen anyway, because the alternative is a third level that would then have
to be given a meaning of its own and defended against a fourth.

### 5.3 The ordinal is not a phase number

`Oxys 6` does not mean Phase 6. The two counts are unrelated and will not stay in
step, and this project has thirteen numbered phases sitting in a document a reader
of a release note is likely to have open.

A release may be cut in the middle of a phase, several may be cut within one
phase, and a phase may pass without one.

### 5.4 There are no pre-releases

No alphas, no betas, no release candidates, no `-rc1`. There is one branch, and
`main` is the candidate for everything; anybody who wants the unreleased state
builds it, which [`../../README.md`](../../README.md) describes in four commands.
[`../../SECURITY.md`](../../SECURITY.md), Section 5, says the same thing from the
other direction.

## 6. Names

A release may be given a name. `Oxys Aetos` is `Oxys 4` with a name.

| Rule | Why |
| ---- | --- |
| A name belongs to the **ordinal**, so every point release under it carries the same name. `Oxys 4.1` is still "Aetos". | Otherwise a point release would need a name of its own, and the name would become a third level pretending to be a label. |
| A name **never** appears in the ordinal form — not in a tag, not in a filename. | Names do not sort, and two releases whose names sort the wrong way against their ordinals is a directory listing that lies. |
| A name is chosen when the release is cut, never reserved in advance. | A name reserved for a release that is never made is a name that has to be explained. |
| A name is never reused. | — |
| Names are **not** alphabetical, sequential, or otherwise ordered. | An ordered naming scheme invites a reader to infer both the next name and the ordering, and the ordering is the ordinal's job. |
| Not every release gets one. An unnamed release is not a lesser release. | — |

**The source of names is Greek**, transliterated into the Latin alphabet, one
word, no accents. `Oxys` is itself Greek — ὀξύς, *sharp*, *keen* — and `Aetos` is
ἀετός, *eagle*. This is the one line in this document that is a preference rather
than a rule, and the project owner may replace it with any other source without
anything else here changing.

## 7. Editions

An edition is a **variety** of a release, not a successor to it. `Oxys Aetos
Workspace Edition` and `Oxys Aetos Compatibility Edition` are both `Oxys Aetos`;
neither supersedes the other and neither is newer in any sense that matters.

In *scale* an edition is like a point release — it is the same order of work — but
in *position* it is beside its release rather than after it. That is the whole
distinction, and it is why an edition is a qualifier and not a number.

### 7.1 The rules

| Rule | Why |
| ---- | --- |
| An edition qualifier is appended to the ordinal form after a hyphen, in lower case: `4-workspace`. | It sorts adjacent to its release, and it cannot be mistaken for a point. |
| **A release with no edition qualifier is the standard edition**, and "Standard Edition" is never written. | Two names for one thing is two things to keep in step. |
| The point belongs to the release, and every edition shares it. A correction to the Workspace Edition of `Oxys 4` produces `Oxys 4.1 Workspace Edition`, ordinal form `4.1-workspace`. | The number says *which moment of the fourth release*; the qualifier says *which variety*. Giving each edition its own point sequence would make `4.1-workspace` and `4.1-compatibility` unrelated releases that happen to share a number. |
| It is legitimate to publish `4.1-workspace` and no `4.1`. | Only the edition that needed rebuilding is rebuilt. The absent standard `4.1` is not a missing release; `Oxys 4` remains the current standard edition. |
| A point release supersedes everything before it **within its own edition**. `Oxys 4.1 Workspace Edition` supersedes `Oxys 4 Workspace Edition` and says nothing about the standard edition. | — |
| An edition may be introduced at a point release rather than at the ordinal. | An edition is a decision that can be taken late. |

### 7.2 An edition is never a branch

This is the rule that keeps the premise of Section 1 true, and it is the one an
edition would break first.

**An edition is the same source, built differently.** Two editions published at
the same moment are built from **one commit** on `main`, differing only in what
the build includes or how it is configured — a `make` target, a selection of
drivers, a different default in `/etc`.

If an edition would need code that is not on `main`, it is **not an edition**.
There are two honest ways out and no third: put the code on `main` behind a build
selection, or make the thing a new ordinal.

The consequence is worth stating plainly, because it is the benefit rather than
the cost: **`make verify` covers every edition**, there being one source for all
of them. A scheme that let an edition diverge would have produced editions nothing
had ever asserted anything about.

## 8. Ordering, and what supersedes what

- Within an ordinal, points ascend: `4` precedes `4.1` precedes `4.2`.
- Across ordinals, ordinals ascend: everything under `4` precedes everything
  under `5`.
- **Editions do not order against one another.** `4-workspace` and
  `4-compatibility` are siblings. Nothing in this scheme, and nothing in a
  directory listing, means that one is later than the other.
- A release supersedes only what precedes it in **its own edition**.

## 9. What a release carries

### 9.1 The tag

Ordinal form, prefixed by `v`, on `main`, annotated:

```
v1        v1.1        v2        v4        v4-workspace
```

The annotation's first line is the full form — `Oxys 4 "Aetos" Workspace
Edition` — so that the name is recoverable from the repository alone and does not
depend on a hosting service's release page continuing to exist.

### 9.2 The image

Ordinal form, lower case, hyphens, no spaces:

```
oxys-1.iso        oxys-4.1.iso        oxys-4-workspace.iso
```

A filename with a space in it is a filename that is quoted wrongly by somebody
eventually, and a filename carrying a name rather than an ordinal is one that
cannot be sorted or matched by a pattern.

### 9.3 What the kernel itself says

`OXYS_VERSION_STRING` in
[`../../kernel/include/oxys/kernel.h`](../../kernel/include/oxys/kernel.h) holds
the **ordinal form** of the release the image belongs to, and `"unreleased"` where
it belongs to none. It is what the boot banner prints and what the `version`
system call copies into a caller's buffer, and both were reporting `0.1.0` — a
release that had been withdrawn — until this document was written.

**The name is deliberately not carried in the image.** Nothing inside the kernel
has any use for it: the banner is read by whoever is already looking at the
machine, and a program asking what it runs upon wants something it can compare.
The name lives in the release notes, the tag annotation and Section 11. This is
the same reasoning [`../devices/APIC.md`](../devices/APIC.md), Section 7, gives for
not having programmed a register nothing yet used — a field with no consumer is a
field nothing has ever shown to be right.

### 9.4 The release notes

One document per release, published with it, opening with the full form and
carrying:

- which of Section 4's two kinds of release this is, **and the judgement that
  decided it**;
- what changed, and which sub-tasks of [`PLAN.md`](PLAN.md) closed;
- which editions were published at this moment and which were not;
- where it was tested and on what, cross-referring to
  [`TESTING-RECORD.md`](TESTING-RECORD.md);
- the checksum of every image.

## 10. What must be true before a release is cut

| Condition | Authority |
| --------- | --------- |
| `make verify` passes at the commit being tagged | `PROJECT_GUIDELINES.md`, Section 2 — this is the gate for any change, and a release is not an exception to it |
| `make clang-check` passes at that commit | [`TOOLCHAIN.md`](TOOLCHAIN.md), Section 9 |
| Every document affected by the work in it is up to date | `PROJECT_GUIDELINES.md`, Section 2 |
| The image has been booted under QEMU and VirtualBox, with the runs recorded | [`TESTING.md`](TESTING.md), Sections 2 and 4 |
| The image has been booted on physical hardware | [`TESTING.md`](TESTING.md), Section 5. **The project owner's call per release**: it is required of a release that claims to be usable, and a release that has not had it says so in its notes rather than being silent |

A release is cut from `main` and from no other branch, because there is no other
branch.

## 11. The release record

One row per release, newest first. **It is empty**, and the emptiness is the
current state of the project rather than an omission.

| Ordinal | Full form | Editions | Date | Tag | Notes |
| ------- | --------- | -------- | ---- | --- | ----- |
| — | Nothing has been released. | — | — | — | — |

**The one tag this repository has ever carried was `v0.1.0`**, published and then
withdrawn on 2026-09-09 at the project owner's direction, with the version badge
that went with it; [`HISTORY.md`](HISTORY.md) records why. It is **not part of this
scheme**, its number is not reused, and it is not the predecessor of anything. The
first release under this scheme will be **Oxys 1**, and there will be no Oxys 0.

## 12. Limitations

1. **Nothing here is enforced by a tool.** No script checks that a tag matches the
   ordinal form, that `OXYS_VERSION_STRING` agrees with the tag, or that an image
   filename is well formed. With no releases there is nothing to check; a check
   becomes worth writing at the second release, when there is a pattern for it to
   compare against.
2. **The edition mechanism has no build support.** Section 7.2 requires an
   edition to be a build selection of one source, and the `Makefile` presently
   offers no such selection — there is one target and one image. The first
   edition is what must add it, and it must add it as a selection and not as a
   branch.
3. **`OXYS_VERSION_STRING` is set by hand.** It is a macro in a header and nothing
   derives it from the tag, so a release could be tagged `v1` and built from a
   source still saying `unreleased`. Deriving it from `git describe` at build time
   would fix that and would also make every ordinary build depend upon the
   repository being a git checkout, which the release tarball of a later phase
   would not be. Recorded rather than solved.
4. **There is no statement of how long a release is supported.**
   [`../../SECURITY.md`](../../SECURITY.md), Section 5, presently supports `main`
   and nothing else, which is correct while nothing has been released and will
   need revisiting at the first one.
5. **Nothing here says anything about the userland's or the C library's own
   versioning.** They are licensed apart from the kernel
   ([`../../LICENSING.md`](../../LICENSING.md)) and do not yet exist; whether they
   are released with the system or apart from it is a Phase 7 question.
