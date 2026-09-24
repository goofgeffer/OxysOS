<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `docs/project/` — How the Work Is Conducted

These documents govern the work; none explains how the system functions, which
is [`../design/`](../design/README.md), [`../devices/`](../devices/README.md)
and [`../storage/`](../storage/README.md). Where each kind of fact belongs is
[`../README.md`](../README.md), Section 2.

| Document | Answers |
| -------- | ------- |
| [`PLAN.md`](PLAN.md) | Which sub-tasks exist, their state, and what is next. |
| [`STATUS.md`](STATUS.md) | What works now, where it was observed, what is missing. |
| [`HISTORY.md`](HISTORY.md) | Which commit made each change: one line per change. |
| [`TESTING.md`](TESTING.md) | How to run the tests, in each environment. |
| [`TESTING-SYSTEM.md`](TESTING-SYSTEM.md) | The checks of the devices, storage and kernel that a person performs. |
| [`TESTING-GRAPHICS.md`](TESTING-GRAPHICS.md) | The checks of the graphics and the desktop that a person performs. |
| [`TESTING-RECORD.md`](TESTING-RECORD.md) | Which tests were run, where, with what result: one line per run. |
| [`BUILDS.md`](BUILDS.md) | Which image was that: the build register, generated from [`builds.tsv`](builds.tsv). |
| [`VERSIONING.md`](VERSIONING.md) | What a release is called, and which releases are planned. |
| [`RELEASE-1-ALPHA.md`](RELEASE-1-ALPHA.md) | The release notes of `Oxys 1 Alpha`. Frozen when cut. |
| [`TOOLCHAIN.md`](TOOLCHAIN.md) | How the cross-toolchain is built, and how the build and the CI use it. |
| [`CODING-STANDARDS.md`](CODING-STANDARDS.md) | How source is written: style, names, headers, diagnostics, extensions. |
| [`REFERENCES.md`](REFERENCES.md) | Every specification relied upon, and the sections relied upon. |
| [`INSPIRATIONS.md`](INSPIRATIONS.md) | The systems Oxys-OS takes its character from, and what it takes. |

A release is named in `VERSIONING.md`; an image that is not released is only
numbered in `BUILDS.md`.
