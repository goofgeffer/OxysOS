# `.github/` — the automation that runs upon GitHub

This directory holds no part of the system. It holds the configuration by which
GitHub runs, upon a machine nobody in this project has configured, the same
assertions a contributor is required to run before committing.

| Path | Subject |
| ---- | ------- |
| [`workflows/ci.yml`](workflows/ci.yml) | The verification workflow: the second compiler of [`../docs/project/TOOLCHAIN.md`](../docs/project/TOOLCHAIN.md), Section 9, and the `make verify` procedure of [`../docs/project/TESTING.md`](../docs/project/TESTING.md). |

**Phase**: none of its own. The workflow asserts the work of whichever phase is
current, and needs no revision when a phase advances: `make verify` greps the
serial output for the word a failing self-test emits, so a self-test written in
a later phase is covered by this workflow on the day it is written.

## Why the assertions are run twice

`PROJECT_GUIDELINES.md`, Section 2, requires a change to be verified before it
is final, and [`../CONTRIBUTING.md`](../CONTRIBUTING.md) states the procedure. A
contributor running it on their own machine is running it against a toolchain
they installed, a QEMU they configured, and a `PATH` they exported — and a build
that passes only because of something present on one machine passes silently.
That is the failure this directory exists to catch: it is invisible locally by
construction, because the thing that makes it pass is the thing that is not
being tested.

The workflow therefore installs every tool from nothing on each run, and builds
the cross-compiler from the versions named in `workflows/ci.yml` rather than
from whatever a package index offers today. A compiler that differs from the one
a contributor runs would report a difference between two compilers as though it
were a defect in the kernel.

## What it does not do

It does not run the interactive tests, the VirtualBox target, or the physical
hardware procedure of `../docs/project/TESTING.md`, Section 10. None of those
can be performed by a hosted runner: they require a display, a hypervisor the
runner does not provide, or a machine to boot from a USB medium. The record of
those runs is kept by hand in `TESTING.md`, and this workflow does not replace
it.
