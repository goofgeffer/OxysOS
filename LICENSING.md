# Licensing

**Authority**: This document, together with the texts in
[`LICENSES/`](LICENSES/). Where it and any file header differ, the file header
governs that file.

**Copyright**: © 2026 The Oxys-OS Authors.

Three licences apply to this repository, one to each kind of thing it holds. The
division is deliberate: the kernel, the programs that run upon it, and the
documents that describe both are addressed to different audiences and are
intended to be used differently.

## 1. What applies where

| Path | Licence | SPDX identifier |
| ---- | ------- | --------------- |
| `boot/`, `kernel/`, `drivers/`, `graphics/`, `linker.ld` | GNU Lesser General Public License, version 3 or later | `LGPL-3.0-or-later` |
| `libc/`, `userland/` | MIT License | `MIT` |
| `docs/`, every `README.md`, `PROJECT_GUIDELINES.md`, this file | Creative Commons CC0 1.0 Universal | `CC0-1.0` |
| `Makefile`, `build_*.sh`, `boot/grub/grub.cfg`, `.gitignore`, `.gitattributes`, `.github/` | CC0 1.0 Universal, as documentation of how the work is built rather than part of it | `CC0-1.0` |

`libc/` and `userland/` are empty at the time of writing; they acquire material
in Phase 7. The licence is declared in advance so that the first file placed
there is placed under a licence already decided, rather than one settled
afterwards when there is code to argue about.

**Both the LGPL and the GPL texts are present**, and the second is not a
mistake. The Lesser General Public License version 3 is written as a set of
additional permissions upon the GNU General Public License version 3, which it
incorporates by reference; it cannot be read without it. Distributing one
without the other would distribute an incomplete licence.

## 2. The boundary between the kernel and a program that runs upon it

This is the part of a split licence that goes wrong quietly, so it is stated
plainly rather than left to be inferred.

**Using the kernel's services through the system-call interface does not make a
program a derivative work of the kernel.** A program that executes `SYSCALL`,
whatever licence it carries, is doing to this kernel what every program does to
every operating system. It is not linked against the kernel, shares no address
space with it beyond the transition itself, and incorporates none of its code.
Such a program is unaffected by the LGPL and may carry any licence its author
chooses, including a proprietary one.

That is the same position the Linux kernel takes, and it is the position this
project intends. It is recorded here because the intent of a copyright holder is
worth having in writing before there is a dispute rather than after.

### 2.1 The interface definitions, and a consequence still to be discharged

The paragraph above disposes of the *calls*. It does not by itself dispose of
the **headers**, and this is a real loose end rather than a formality.

`kernel/include/oxys/syscall.h` presently holds two different things: the
user-visible interface — the call numbers, the error values, the limit an
argument is validated against — and the kernel's own configuration of the
mechanism, which is `IA32_STAR`, `IA32_LSTAR`, the flag mask, the dispatcher and
the validation. The first of those is what a C library of Phase 7 must know; the
second is no business of any program.

An MIT-licensed C library cannot cleanly include a header that mixes them.
**The header must therefore be divided before sub-task 7.2**, into the interface
a program is entitled to and the implementation it is not — the former licensed
permissively so that it may be included by anything, the latter remaining with
the kernel. This is the same division Linux draws between its user-visible
headers and its internal ones, and it is easier to make now, while the interface
is three calls, than after a library depends upon it.

This is recorded as a limitation in Section 5 rather than acted upon here,
because it is a change to the source tree and this document is not the place to
make one.

## 3. Why these three

The choices are the project owner's, and are recorded rather than argued:

**The kernel is copyleft.** A modified kernel distributed to others must come
with its source. The LGPL rather than the GPL was chosen deliberately; the
practical difference for a kernel is small, since the provisions of the LGPL
that concern combining a library with other work have little to act upon in a
monolithic kernel that nothing links against, but the choice is the owner's and
is not this document's to second-guess.

**The userland is permissive.** The utilities and the C library are the parts
most likely to be useful to somebody else outside this project, and the parts
where a copyleft obligation would be most likely to prevent that use for no gain
to anybody.

**The documentation is public domain**, so far as CC0 can place it there. The
`docs/` corpus is the most reusable thing here: it is a long, cited account of
how a great many hardware interfaces actually behave, and of the ways each of
them fails silently. Somebody writing an unrelated kernel should be able to take
any of it, without attribution and without asking.

## 4. Originality

Every line of source in this repository was written for it.
`PROJECT_GUIDELINES.md`, Section 2, prohibits transcribing reference
implementations, and nothing here is vendored: there is no third-party code, no
imported header, and no obtained asset. The bitmap face in `graphics/font.c` was
drawn for this project for exactly that reason, a font being the kind of asset
that is easy to lift without noticing.

The consequence is that the licences above may be granted at all. A repository
carrying vendored code cannot license itself freely, and this one carries none.

## 5. Limitations

1. **`kernel/include/oxys/syscall.h` mixes the user-visible interface with the
   kernel's implementation of it.** Section 2.1. It must be divided before
   sub-task 7.2, and this is the first thing to be done about licensing after
   this document.
2. **No `SPDX-License-Identifier` headers are present in the source files.** The
   mapping of Section 1 is by path, which is sufficient and is what this document
   is for, but a per-file tag is machine-readable where a table is not. Adding
   one to each of the hundred-odd files is a mechanical change and is not made
   here.
3. **The copyright holder is recorded as "The Oxys-OS Authors".** That is a
   placeholder of the ordinary kind and is legally serviceable, but a single
   copyright holder may prefer to be named. Changing it means changing it in
   `LICENSES/MIT.txt` and in this document.
4. **No contributor licensing mechanism is in force**, there being one
   contributor. [`CONTRIBUTING.md`](CONTRIBUTING.md), Section 7, records what
   would be needed if that changes.
