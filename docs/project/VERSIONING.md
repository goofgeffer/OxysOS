<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
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

**Three releases are planned and Section 11.1 is the plan**: `Oxys 1 Alpha` at
sub-task 8.7, `Oxys 1 Beta` at 9.7, and `Oxys 1` at around 11.10. Each is fixed to
a sub-task of [`PLAN.md`](PLAN.md) and not to a date. The alpha and the beta
belong to the first release alone; everything after it is preceded by internal
debug builds, which Section 5.5 distinguishes from releases and which are
numbered in [`BUILDS.md`](BUILDS.md) rather than named here — once that register
resumes at the alpha, recording having been suspended by the owner on 2026-09-13.

## 1. The premise

**One repository, many releases.** `OxysOS` stays; the releases are versions of
it. There is no branch per release, no repository per release and no fork; a
release is a tag upon `main` and an image built from it.

That premise is the constraint everything below answers to. Every rule here that
looks arbitrary is there because the alternative would have required a second
branch to live for longer than an afternoon, and a second long-lived branch is
how "one repository" quietly stops being true.

## 2. The five parts of a release identity

A release is named by up to five things. Only the first is compulsory.

| Part | Example | What it is |
| ---- | ------- | ---------- |
| **Ordinal** | `1`, `2`, `4` | Which major release this is. Compulsory, never repeated, never skipped, and it only goes up. |
| **Point** | the `.1` of `1.1` | A smaller successor within the same ordinal. Optional. |
| **Pre-release** | `alpha`, `beta` | A release published on the way to an ordinal, **before** it. Optional, and there are exactly two of them; Section 5.4. |
| **Name** | `Aetos` | A word standing for the ordinal. Optional, and an alias — **not a further level**. |
| **Edition** | `Workspace`, `Compatibility` | A variety of a release rather than a successor to it. Optional. |

A pre-release and a point are mutually exclusive: a pre-release precedes its
ordinal and a point follows it, so `1.1-alpha` names a moment that cannot exist.

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
| `1-alpha` | `Oxys 1 Alpha` | Oxys Alpha |
| `1-beta` | `Oxys 1 Beta` | Oxys Beta |
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
[`../design/PRIVILEGE.md`](../design/PRIVILEGE.md): a number handed
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

### 5.4 There are two pre-releases, and they belong to Oxys 1

`Oxys 1` is preceded by an alpha and a beta at named sub-tasks of
[`PLAN.md`](PLAN.md); Section 11.1 is the plan. **No release after the first is
expected to have either**, and Section 5.5 is what the rest get instead.

`main` remains the candidate for everything, anybody who wants the unreleased
state builds it in four commands ([`../../README.md`](../../README.md)), and
nothing here creates a second branch. A pre-release exists for one reason: a
system on the way to its first release has two moments worth publishing an image
of, and publishing them unlabelled as `Oxys 1` would misrepresent what they are.

| Rule | Why |
| ---- | --- |
| There are **two** pre-release qualifiers, `alpha` and `beta`, and the list is closed. No `rc`, no `pre`, no `snapshot`, no `nightly`. | An open-ended ladder of pre-releases is a way of never cutting the release. Two moments decided in advance is not that. |
| **Both belong to `Oxys 1`.** There is no `2-alpha`, and the project owner's expectation is that there never will be an alpha or a beta again. | The first release is the one whose shape nobody has seen. Every release after it succeeds something people are already running, and the thing they want before it is not a differently-labelled image but the reasons to install the real one. Reopening this is the owner's to direct, and would be an amendment like this one. |
| Both are reserved words. **An edition may never be called `alpha` or `beta`**, and a pre-release qualifier may never be anything else. | The ordinal form appends both after a hyphen (`1-alpha`, `4-workspace`), so one reserved list is what keeps `1-alpha` from being ambiguous between a pre-release and an edition of that name. |
| A pre-release belongs to the ordinal it precedes, and **takes no point**: there is `1-alpha`, never `1.1-alpha`. A correction to a pre-release is the next pre-release, or the ordinal itself. | Section 5.2's argument against a third level, applied here: a pre-release that could take points would need its own sequence, and two sequences under one ordinal is the ladder this scheme refuses. |
| A pre-release is **superseded by its ordinal** and by nothing else. `Oxys 1` supersedes both `Oxys 1 Alpha` and `Oxys 1 Beta`. | — |
| A pre-release **may carry an edition**, written `1-beta-workspace`. | An edition is a build selection of one source (Section 7.2) and nothing about that depends upon the release being final. |
| A pre-release is subject to **every condition of Section 10**, unchanged. | This is the one that would be tempting to relax and must not be. A pre-release is an image published to other people; an image that has not passed `make verify` is not less of a release, it is a defect distributed deliberately. |
| A pre-release **does not get a name** (Section 6). `Oxys Alpha`, never `Oxys Alpha "Aetos"`. | A name belongs to an ordinal, and the ordinal it belongs to has not been released yet. Naming it twice — once at the alpha and once at the release — would be a name reused, which Section 6 forbids. |

**The shape resembles semantic versioning's `1.0.0-alpha` and the resemblance is
coincidental.** Section 5.1 stands: a pre-release qualifier here says *when in the
sequence*, and says nothing whatever about interface compatibility. It is a hyphen
because Section 7.1 had already established a hyphen for a qualifier, not because
another scheme uses one.

### 5.5 An internal debug build is not a release

Every release after `Oxys 1` is expected to be preceded by internal debug builds
and by nothing else that is labelled. **An internal debug build is an image and
not a release**, and the distinction is the whole of this section: a release is
published to other people, and an image that is not published is answerable to
nobody but whoever built it.

| It has | It has not |
| ------ | ---------- |
| A number, in [`BUILDS.md`](BUILDS.md), once recording resumes at `Oxys 1 Alpha` — it is suspended at the owner's direction and the register presently holds no rows | An ordinal, a point, a pre-release qualifier or an edition |
| A commit, a compiler, a size and a verification result, recorded by `make build-record` | A tag. Nothing under Section 9.1 is cut for it |
| A note saying why it was built | Release notes under Section 9.4 |
| `OXYS_VERSION_STRING` reading `unreleased`, which is what that value is for | A row in the release record of Section 11 |

**This is why the build register exists and why it is not part of this scheme.**
The register answers *which image*; this document answers *what a released thing
is called*. An internal build needs the first and has no use for the second, and
before the register there was nowhere for such an image to be recorded at all.

**Recording is suspended until the alpha**, by the project owner's direction on
2026-09-13, and the two rows above describing a number and a `build-record` entry
describe what an internal build will have rather than what one has today. The
register was emptied and every archived image deleted in the same decision.
Nothing in this section changes because of it: an internal build is still not a
release, and the reason the register exists is still that it answers *which
image*. What changed is when it starts answering.
[`BUILDS.md`](BUILDS.md) holds the decision, and records that numbering restarts
at 1 — so a build number identifies an image within a register and not across
this project's life.

**The moment an image is handed to somebody else it stops being internal**, and
at that moment it is a release and every condition of Section 10 applies to it.
There is no third category — no "preview", no "nightly", no image that is
published but exempt. That is the rule the word *internal* is carrying, and it is
stated because it is the one that erodes: an image shared with one person outside
the project is published, whatever it was called when it was built.

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
| An edition may **not** be called `alpha` or `beta`. Both are reserved to Section 5.4. | They occupy the same position in the ordinal form, so one of the two lists has to be closed against the other, and the pre-release list is the one that is closed already. |
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

- **A pre-release precedes its ordinal**: `1-alpha`, then `1-beta`, then `1`.
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
v1-alpha  v1-beta   v1        v1.1        v2        v4        v4-workspace
```

A pre-release is tagged exactly as a release is, because it is one. The
annotation's first line is its full form — `Oxys 1 Alpha`.

The annotation's first line is the full form — `Oxys 4 "Aetos" Workspace
Edition` — so that the name is recoverable from the repository alone and does not
depend on a hosting service's release page continuing to exist.

### 9.2 The image

Ordinal form, lower case, hyphens, no spaces:

```
oxys-1-alpha.iso  oxys-1.iso        oxys-4.1.iso        oxys-4-workspace.iso
```

**A pre-release sorts before its release in a directory listing**, and by
accident rather than by design: `-` precedes `.` in the character set, so
`oxys-1-alpha.iso` stands before `oxys-1.iso` without anything having to arrange
it. It is written down because it is the kind of property that is relied upon
once somebody notices it and is then broken by a change nobody connected to it.

A filename with a space in it is a filename that is quoted wrongly by somebody
eventually, and a filename carrying a name rather than an ordinal is one that
cannot be sorted or matched by a pattern.

### 9.3 What the kernel itself says

`OXYS_VERSION_STRING` in
[`../../kernel/include/oxys/kernel.h`](../../kernel/include/oxys/kernel.h) holds
the **ordinal form** of the release the image belongs to — `"1-beta"` since the
beta was cut on 2026-09-24, and so upon every image built after it until the
next release — and `"unreleased"` where
it belongs to none. It is what the boot banner prints and what the `version`
system call copies into a caller's buffer, and both were reporting `0.1.0` — a
release that had been withdrawn — until this document was written.

**The name is deliberately not carried in the image.** Nothing inside the kernel
has any use for it: the banner is read by whoever is already looking at the
machine, and a program asking what it runs upon wants something it can compare.
The name lives in the release notes, the tag annotation and Section 11. This is
the same reasoning [`../devices/APIC.md`](../devices/APIC.md) gives for
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
| `make clang-check` passes at that commit | [`TOOLCHAIN.md`](TOOLCHAIN.md) |
| Every document affected by the work in it is up to date | `PROJECT_GUIDELINES.md`, Section 2 |
| The image has been booted under QEMU and VirtualBox, with the runs recorded | [`TESTING.md`](TESTING.md) |
| The image has been booted on physical hardware | [`TESTING.md`](TESTING.md). **The project owner's call per release**: it is required of a release that claims to be usable, and a release that has not had it says so in its notes rather than being silent |

A release is cut from `main` and from no other branch, because there is no other
branch.

## 11. The release record

One row per release, newest first.

| Ordinal | Full form | Editions | Date | Tag | Notes |
| ------- | --------- | -------- | ---- | --- | ----- |
| `1-beta` | Oxys 1 Beta | — | 2026-09-24 | `v1-beta` | [`RELEASE-1-BETA.md`](RELEASE-1-BETA.md) |
| `1-alpha` | Oxys 1 Alpha | — | 2026-09-16 | `v1-alpha` | [`RELEASE-1-ALPHA.md`](RELEASE-1-ALPHA.md) |

**It held no row until 2026-09-16**, and the emptiness was the state of the
project rather than an omission: the scheme was written before the first release
rather than under the pressure of it.

**The one tag this repository carried before `v1-alpha` was `v0.1.0`**, published and then
withdrawn on 2026-09-09 at the project owner's direction, with the version badge
that went with it; [`HISTORY.md`](HISTORY.md) records why. It is **not part of this
scheme**, its number is not reused, and it is not the predecessor of anything. The
first release under this scheme is **Oxys 1 Alpha**, and there is no
Oxys 0 — a pre-release belongs to the ordinal it precedes and takes no ordinal of
its own, so the sequence begins at 1 whatever is published first.

### 11.1 The planned releases

**Three releases are planned, and each is fixed to a sub-task of
[`PLAN.md`](PLAN.md) rather than to a date.** The project owner decided this on
2026-09-11, and it is recorded here rather than left to be judged at the time
because Section 4 requires the *kind* of a release to be a judgement while
leaving *when to cut one* unstated — and a system with no release plan reaches its
first release by somebody deciding one afternoon that enough has been done.

A sub-task rather than a date, because a date is a guess about how long work
takes and a sub-task is a statement about what the system can do. These three are
not promises about when.

| Release | Ordinal form | Cut at | What the system can do that it could not before |
| ------- | ------------ | ------ | ----------------------------------------------- |
| **Oxys 1 Alpha** | `1-alpha` | Sub-task **8.7**, which closes Phase 8 | **A person can drive it.** Phases 1 to 8 complete: it boots, manages memory, services interrupts, drives its devices, mounts an EXT2 volume, runs programs at privilege level 3, and presents a shell with line editing, pipelines, redirection and job control. It is the first image that is worth handing to somebody, because it is the first one that does anything when they type. |
| **Oxys 1 Beta** | `1-beta` | Sub-task **9.7** | **It has a desktop.** A stacking window manager, the client protocol beneath it, `init` and the services it supervises, the session and its panel, a terminal emulator hosting the Phase 8 shell, and the utilities the desktop is not usable without. Phase 10's cryptography is not in it and neither is Phase 11's networking. |
| **Oxys 1** | `1` | Around sub-task **11.10**, which closes Phase 11 | **It is on a network.** Cryptography (Phase 10) and the whole of the network stack (Phase 11) — Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets, DHCP and `ping`. |

**Oxys 1 Beta is cut one sub-task before the end of its phase, and that is
deliberate.** Sub-task 9.8 is the settings application, by which the
configuration of 9.4 is edited rather than hand-written. A desktop whose settings
are edited in a text file is a desktop somebody can use and complain about, which
is what a beta is for; waiting for 9.8 would mean the beta and the release
differed by two whole phases.

#### What Oxys 1 will not have, and why that is being decided now

**Oxys 1 is cut before Phases 12 and 13**, and the consequences are not small
enough to leave implied:

- **It boots by BIOS alone.** The UEFI path is Phase 12, so a machine with no
  compatibility support module will not boot `Oxys 1`.
  [`TESTING.md`](TESTING.md) already records that `make run-uefi` is
  expected to fail, and it will still be expected to fail at the release.
- **It has none of Phase 13's hardening.** No NX, SMEP or SMAP enforcement, no
  write-exclusive-or-execute in kernel mappings, no stack canaries, no address
  space layout randomisation. [`../../SECURITY.md`](../../SECURITY.md) states
  which of these are in force; at `Oxys 1` the answer will still be none of them,
  and its release notes must say so under Section 9.4 rather than leaving a
  reader to infer it.
- **It has not been tested upon three machines.** Sub-task 13.7 is what
  systematises physical testing; Section 10 leaves physical testing to the project
  owner's call per release, and `Oxys 1` will be exercising that judgement rather
  than satisfying a completed sub-task.

Those three are what **Oxys 2** is for, and naming them here is the point of
writing this down: a release plan that only says what is included is a plan whose
omissions are discovered by whoever installs it.

#### Why `Oxys 1` and not `Oxys 1.0`

The decision was expressed as "Oxys 1.0". Section 3.1 forbids writing the zero: a
release with no point release does not have one, and `1.0` invites `1.0.1`, which
is the third level Section 5.2 refuses. So the release is written **`Oxys 1`**,
ordinal form `1`, and it is the same release the decision named. Changing that
would be an amendment to Section 3.1 and is the project owner's to direct.

#### These three do not exhaust the scheme

Nothing here says `Oxys 1` is the last release, that there will be no points
beneath it, or that Phases 12 and 13 arrive as `Oxys 2` rather than as `1.1` and
`1.2`. Section 4's distinction decides that when each is cut, and Section 4.1
already says that a point release promises nothing about the next one.

**What is not planned is a fourth pre-release.** The alpha and the beta are the
first release's, and every release after `Oxys 1` is expected to be preceded by
internal debug builds and nothing else — Section 5.5. So this plan has three
entries and the next one will have one: an ordinal, cut when the work it names is
done.

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
   need revisiting at the first one. **Section 11.1 now names which sub-task that
   is** — 8.7 — so the obligation has a trigger rather than an intention, and the
   question a pre-release raises is the awkward one: whether `Oxys 1 Alpha` is
   supported at all, or whether `main` remains the only supported thing while a
   published image exists that is not it. It is left open here because it is
   `SECURITY.md`'s to answer and answering it in two documents is how they come to
   disagree.
5. **Nothing here says anything about the userland's or the C library's own
   versioning.** They are licensed apart from the kernel
   ([`../../LICENSING.md`](../../LICENSING.md)) and do not yet exist; whether they
   are released with the system or apart from it is a Phase 7 question.
