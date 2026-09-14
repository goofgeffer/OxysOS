<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Oxys-OS Toolchain and Build System

**Corresponding phase**: Phase 1, sub-tasks 1.1 and 1.9.

## 1. Required tools

| Tool | Purpose | Status in the present environment |
| ---- | ------- | --------------------------------- |
| `x86_64-elf-gcc` | Cross-compilation of C11 to ELF64 objects. | Present at `~/opt/cross/bin`. |
| `x86_64-elf-ld` | Linking of the kernel image. | Present at `~/opt/cross/bin`. |
| `nasm` | Assembly of the boot code. | Present. |
| `grub-mkrescue` | ISO 9660 image generation with an embedded GRUB. | Present. |
| `xorriso` | The ISO writer employed by `grub-mkrescue`. | Present. |
| `make` | Build orchestration. | Present. |
| `qemu-system-x86_64` | Virtual machine execution and automated verification. | Present. |
| OVMF firmware | UEFI firmware for QEMU, at `/usr/share/ovmf/OVMF.fd`. | Present. |
| `mke2fs` | The initial ramdisk of sub-task 7.7. Builds the EXT2 image the kernel mounts as its root, populating it from a directory with `-d` — so no loop device, no mount and no privilege is needed. | Present, e2fsprogs 1.47.0. |
| `VBoxManage` | VirtualBox execution. | **Absent from the Linux environment.** `make run-vbox` is provided and cannot be executed there; the VirtualBox runs this project records are made through the Windows host's `VBoxManage.exe`, the WSL2 environment being a guest of it. |

**`mke2fs` is the first required tool here that is not a compiler, an assembler,
a linker or an image builder**, and it is required for a reason rather than for
convenience. `docs/storage/INITRD.md`, Section 3.2, argues it at length; the short
form is that a filesystem image composed by this project and read by this project
proves the reader consistent with the composer and nothing more, while one
composed by e2fsprogs is corroboration from an implementation that has never seen
this one — delivered at every boot rather than upon the day somebody remembers to
run a comparison.

`PROJECT_GUIDELINES.md`, Section 3, names five tools that must be installed and
functional. That statement stands: all five still must be. It is not amended,
Section 7 of that document reserving amendments to the project owner, and this
table is where the build's full set of dependencies is recorded.

One tool is **optional** and is listed apart, nothing in the build requiring it:

| Tool | Purpose | Status in the present environment |
| ---- | ------- | --------------------------------- |
| `clang` | The second compiler of Section 9. Compiles the sources for their diagnostics; builds nothing. | Present, version 18.1.3. |

The command `make toolcheck` reports the presence or absence of each tool.

## 2. Why a cross-compiler is required

The system compiler of the host targets a hosted Linux environment. It presumes
the presence of the system C library, emits references to the dynamic loader,
and defines preprocessor macros that describe an environment the kernel does not
possess. A compiler configured for the `x86_64-elf` target presumes nothing
beyond the freestanding environment of ISO/IEC 9899:2011, Section 4, paragraph 6,
and is therefore the only correct instrument for the task.

## 3. Compilation flags and their justification

The flags below are applied to every C translation unit. Each is recorded here
together with its reason, as `PROJECT_GUIDELINES.md`, Section 8, requires of any deviation
from plain ISO C.

| Flag | Justification |
| ---- | ------------- |
| `-std=c11 -pedantic` | Conformance to ISO/IEC 9899:2011, as mandated by `PROJECT_GUIDELINES.md`, Section 1. |
| `-ffreestanding` | The program does not execute in a hosted environment; `main` is not the entry point and the standard library is unavailable. |
| `-fno-builtin` | Prevents the compiler from substituting calls to library routines that the kernel does not provide. |
| `-fno-stack-protector` | The stack canary requires a runtime `__stack_chk_guard` object and a failure handler, neither of which exists before Phase 13, sub-task 13.4. |
| `-fno-pic -fno-pie` | The kernel is loaded at a fixed address; position-independent code would add indirection without benefit. |
| `-mno-red-zone` | The System V AMD64 ABI, Section 3.2.2, reserves 128 bytes below the stack pointer for leaf functions. An interrupt may be delivered at any instruction boundary and would overwrite that region. The red zone is therefore inadmissible in kernel code. |
| `-mno-mmx -mno-sse -mno-sse2 -mno-80387` | Intel SDM, Volume 3A, Section 13.1, requires the vector and floating-point units to be explicitly enabled and their state to be saved and restored across context switches. The kernel performs neither, so the corresponding instruction sets must not be emitted. This also enforces the prohibition upon kernel floating-point arithmetic recorded in `PROJECT_GUIDELINES.md`, Section 8. |
| `-mcmodel=kernel` | All kernel symbols reside within the topmost 2 GiB of the address space, permitting 32-bit sign-extended displacements rather than 64-bit absolute addressing. |
| `-Wall -Wextra -Werror` | The diagnostic regime mandated by `PROJECT_GUIDELINES.md`, Section 4. |
| `-Wshadow -Wpointer-arith -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wredundant-decls -Wwrite-strings` | Additional diagnostics selected because each detects a class of defect that is difficult to observe in a kernel, where there is no debugger of last resort. |
| `-O2` | Optimisation at level two. Level three is not selected because its aggressive inlining complicates the correlation of a fault address with a source line. |
| `-g` | DWARF debugging information, consumed by the QEMU GDB stub. |

**No warning is suppressed in this regime**, which is the one the kernel is
built with. Should a suppression become necessary here, it must be recorded in
the `Makefile` together with its justification, as required by
`PROJECT_GUIDELINES.md`, Section 4.

One suppression exists elsewhere in the `Makefile` and belongs to the
`clang-check` target alone: `-Wno-cast-align`, for the reason set out in Section
9.3. It is not applied to any build, and `-Wcast-align` remains in force above.

### 3.1 The three include roots, and why the kernel is given only two

| Root | Holds | Given to |
| ---- | ----- | -------- |
| `kernel/include` | The kernel's own header corpus, `LGPL-3.0-or-later`. | Every translation unit. |
| `kernel/abi` | The system-call interface a program is entitled to, `MIT`. | Every translation unit, and the C library. |
| `libc/include` | The C library's headers, `MIT`. | The C library's own translation units, and the two self-tests that assert them — `kernel/test/libc/string.c` and `kernel/test/libc/wrappers.c` — and nothing else. |

**The kernel is deliberately compiled without `libc/include` in reach**, so that
no kernel translation unit can include `<string.h>` and quietly acquire a
dependency upon the userland. The two exceptions are named explicitly by rules of
their own in the `Makefile` rather than by a flag applied to everything, so the
exception is a line somebody can find. `make clang-check` adds the root to every
unit, because that target compiles and discards and produces no image: the
isolation is enforced where it has an effect, and maintaining a second list of
which files are which would be a worse arrangement than not.

`kernel/abi` is a root and not a subdirectory of `kernel/include` for a licensing
reason rather than a structural one, which
[`../design/LIBC.md`](../design/LIBC.md), Section 2, sets out: a library that
added the kernel's corpus to its include path in order to reach one permissive
header would have every header of the kernel within reach.

## 4. Assembler flags

| Flag | Justification |
| ---- | ------------- |
| `-f elf64` | The output object format required by the linker. |
| `-g -F dwarf` | DWARF debugging information, consistent with the C translation units. |
| `-Wall -Werror` | The same diagnostic regime as is applied to C. |

## 5. Linker flags

| Flag | Justification |
| ---- | ------------- |
| `-n` | Suppresses page alignment of sections by the linker, so that the explicit alignment of `linker.ld` governs and the image is not needlessly enlarged. |
| `-T linker.ld` | Selects the project link script in place of the default. |
| `-Map build/oxys.map` | Emits a link map, which is the primary instrument for correlating a fault address with a symbol before the kernel possesses a symbol table of its own. |
| `-z max-page-size=0x1000` | Instructs the linker that the page size is 4096 bytes, preventing the alignment of program headers to a larger boundary. |

The program headers themselves are declared by `linker.ld` rather than inferred
from the sections, so that no `LOAD` segment is both writable and executable; the
division and the reason for it are recorded in
[`../design/BOOT.md`](../design/BOOT.md), Section 8. `readelf -lW build/oxys.elf`
shows the result, and the linker no longer warns of a segment with `RWX`
permissions.

## 6. Make targets

| Target | Effect |
| ------ | ------ |
| `all` | Builds `build/oxys.elf` and confirms, by `grub-file --is-x86-multiboot2`, that the image is Multiboot2 compliant. This is the default target. |
| `iso` | Builds `build/oxys.iso` by staging the kernel, the initial ramdisk and the GRUB configuration, and invoking `grub-mkrescue`. |
| `clean` | Removes the whole of the `build` directory. |
| `run-qemu` | Executes the ISO under QEMU with legacy BIOS firmware, the serial port directed to the standard output stream. |
| `run-uefi` | Executes the ISO under QEMU with the OVMF UEFI firmware. |
| `run-vbox` | Registers and starts a VirtualBox machine attached to the ISO, with the serial port directed to a file. |
| `verify` | Executes the ISO under QEMU without a display, captures the serial output, and asserts that the expected banner appears. |
| `toolcheck` | Reports the presence or absence of each required tool, and of the one optional one. |
| `clang-check` | Compiles every translation unit with a second compiler and discards the objects. Builds nothing; see Section 9. |
| `spdx-check`, `spdx-apply`, `docs-check`, `lint` | The corpus checks of [`../../tools/README.md`](../../tools/README.md). Neither builds anything nor needs this toolchain; `lint` is the two checks together and is what CI runs. |
| `build-record` | Appends one numbered row to [`builds.tsv`](builds.tsv), the build register, and re-renders the view in [`BUILDS.md`](BUILDS.md). Builds nothing and runs nothing. `NOTE`, `ENVIRONMENT`, `RESULT` and `ASSERTIONS` are its variables, and `BUILD_DIR` selects which image it reads — which is how an image built by the second compiler is recorded as such. |

**No target was added for the initial ramdisk**, and that is deliberate rather
than an omission. `build/initrd.img` is a prerequisite of the ISO exactly as
`build/trampoline.bin` and the user programs are prerequisites of the kernel
image, so it is built by `make iso` without anybody having to remember a second
command. A phony target would also have obliged an amendment to
`PROJECT_GUIDELINES.md`, Section 3, which Section 7 of that document permits only
by explicit decision of the project owner — the same reasoning sub-task 7.5
recorded when it declined to add a target for the user-mode build.

## 7. Header dependency tracking

Object files are compiled with `-MMD -MP`, which emit a dependency file beside
each object. Those files are included by the `Makefile`, so that a modification
to a header causes every translation unit that includes it to be rebuilt. This
is essential in a project whose headers define the layout of hardware
structures, where a stale object file would produce a fault that is exceedingly
difficult to diagnose.

## 8. Environment note

The cross-compiler resides in `~/opt/cross/bin`, which is not upon the default
`PATH` of a non-interactive shell. Every build must therefore be invoked with
that directory upon the path, for example:

```sh
export PATH="$HOME/opt/cross/bin:$PATH"
make iso
```

## 9. The second compiler

```sh
make clang-check
```

This target compiles every translation unit with `clang` and **discards the
objects**. It does not build the kernel, is not part of `all` or of `verify`,
and its output is nothing but diagnostics. `clang` is optional: the target is
the only thing in the project that uses it, and it says so and stops if it is
absent.

### 9.1 Why a second compiler earns a target

Every assertion this project makes about its own correctness is made by
machinery this project wrote, against fixtures this project composed. That is a
closed loop, and its characteristic failure is agreement: a misreading of a
specification is composed into the fixture and then asserted against itself, and
everything passes. `docs/project/TESTING.md`, Section 7, sets that argument out
in full and names the several independent judges available to this project.

A compiler written by other people, from the same standard, is one of them. It
shares no assumption with `x86_64-elf-gcc` beyond the language, so what it
refuses is what one toolchain has been quietly tolerating. Its first run found
exactly that: `kernel/arch/x86_64/cpu/tss.c` named a 32-bit register to an instruction the
architecture defines upon r/m16, which GNU `as` had accepted, and assembled
correctly, for as long as the file had existed.

### 9.2 The configuration

`clang` needs no cross-toolchain of its own — it is multi-target by
construction, so `--target=x86_64-elf` is the whole of the configuration and
nothing is built or installed.

**Every flag of `CFLAGS` is accepted verbatim**, `-mno-80387` included, so the
two compilers are given precisely the same regime. That was not assumed; each
flag was offered to `clang` individually and the result recorded. It matters
because a difference in what the two say must be a difference between the
compilers and not between their flags, or the exercise establishes nothing.

Linking is not attempted. The value here is in what the compiler says about the
sources, and the linker script and the boot object are `x86_64-elf-ld`'s
business.

### 9.3 The one suppression, and why

`-Wno-cast-align`, confined to this target.

`kernel/handoff/multiboot2.c` casts the byte cursor it walks the Multiboot2 tag series
with to each tag's structure type, which raises the required alignment from 1 to
4 or to 8. `clang` warns upon that whatever the target; GCC does not warn upon
x86, where the access would work regardless.

**Both are right, and the pointer is in fact correctly aligned.** Multiboot2
Specification, Section 3.6.2, requires every tag to begin upon an 8-byte
boundary — but that is a guarantee made by the boot loader, and no compiler can
see it. This is one of the two documented exceptions to the no-overlay rule of
[`CODING-STANDARDS.md`](CODING-STANDARDS.md), Section 7.1, admitted there for
exactly this reason: the structure is not read from a medium, its fields are
naturally aligned by the specification, and no byte-order decision arises.

The suppression is not applied to the build. `-Wcast-align` remains in force
under GCC for every file, this one included.

### 9.4 What this target does not do

It is a compiler's opinion of the source and nothing more. It executes no code,
asserts no behaviour, and passing it means only that two independent front ends
agree the sources are well formed. `make verify` remains the gate.

## 10. Continuous integration

[`../../.github/workflows/ci.yml`](../../.github/workflows/ci.yml) runs upon
every push to `main`, every pull request, and upon request. It performs two of
the assertions this document and [`TESTING.md`](TESTING.md) describe, and
nothing else.

| Job | What it runs | What it needs |
| --- | ------------ | ------------- |
| Second compiler | `make clang-check`, the target of Section 9. | `clang` alone. It builds nothing, so it reports in about a minute and is the first thing to look at when a run fails. |
| Build and verify | `make toolcheck`, `make iso`, then `make verify`. | The cross-toolchain of Section 1, `nasm`, `grub-mkrescue`, `xorriso` and QEMU. |

The workflow needs no revision when a phase advances. `make verify` greps the
serial output for the word a failing self-test emits rather than naming each
test, so a self-test written in a later phase is covered by the workflow on the
day it is written.

Its steps install nothing and build nothing themselves: each delegates to the
script of Section 11 that owns that stage, so that the workflow and a
contributor's machine cannot be given two different recipes. `.github/` carries
no `README.md` of its own — it holds no material of the system, and this section
is its documentation.

### 10.1 Why the toolchain is built rather than installed

No package index carries `x86_64-elf-gcc`, so the workflow builds binutils and
GCC from source at the versions named in its `env` block — `2.42` and `13.2.0`,
which are the versions Section 1 records as present in the working environment.
It then caches the result under those versions as the key.

The versions are pinned rather than left to float for the reason Section 9 gives
for running two compilers at all, read the other way about. A difference between
two compilers is informative when it is the only difference; a workflow that
built whatever GCC was newest would report the newest GCC's opinion of this
kernel as though it were a defect introduced by whoever pushed. When the
toolchain is to move, the `env` block is edited, and the difference that appears
is attributable to that edit.

### 10.2 What it does not assert

It does not run under KVM — a hosted runner provides none, so QEMU emulates,
which is how the target is run locally also. It does not perform the interactive
tests, the VirtualBox target, or the physical hardware procedure of
[`TESTING.md`](TESTING.md), Section 5: each of those requires a display, a
hypervisor or a machine that a hosted runner does not have. Those runs are
recorded by hand and this workflow does not replace them.

**A passing run is not a substitute for `make verify` before a commit.**
`PROJECT_GUIDELINES.md`, Section 2, requires the verification before the change
is final, and a workflow reports after the fact. What the workflow adds is a
machine that has none of a contributor's configuration upon it, which is the one
thing a contributor cannot test for locally: a build that passes because of
something installed long ago passes silently, and does so in exactly the same
way as a build that is correct.

## 11. The build scripts

Three scripts stand at the repository root. They exist because the steps that
precede `make` were, until they were written, recorded in three places that had
no way of agreeing with one another: the prose of Section 1, the steps of
`.github/workflows/ci.yml`, and the memory of whoever configured the machine
this project is developed upon. The workflow now calls the same scripts a
contributor does, so a build that works here and fails there is a difference
between two machines rather than between two recipes.

| Script | What it does | What it does not do |
| ------ | ------------ | ------------------- |
| [`../../build_deps.sh`](../../build_deps.sh) | Installs the host packages of Section 1 with `apt-get`, and the prerequisites of a compiler build. Reports the optional `clang` separately, its absence being no fault. | It does not install the cross-compiler, no index carrying one. Upon a machine without `apt-get` it names the packages and stops rather than guessing at an equivalent. |
| [`../../build_toolchain.sh`](../../build_toolchain.sh) | Builds binutils and GCC for `x86_64-elf` into `PREFIX`, which defaults to `~/opt/cross`. Twenty to forty minutes. | It exits at once, reporting the versions it found, when the toolchain is already present. `FORCE=1` overrides that. It does not edit any profile: the `PATH` of Section 8 remains the contributor's to export. |
| [`../../build_all.sh`](../../build_all.sh) | The two above in order, then `make iso` and `make verify`. `SKIP_DEPS`, `SKIP_TOOLCHAIN` and `SKIP_VERIFY` each omit a stage. | It builds nothing itself. Every stage is delegated to the script or the target that owns it, so that this file cannot drift from them; what it adds is the order and the reporting. |

### 11.1 Why the versions are defaults rather than constants

`build_toolchain.sh` reads `BINUTILS_VERSION` and `GCC_VERSION` from the
environment and falls back upon the versions Section 1 records as present here.
The workflow restates the same two in its `env` block, because there they are
the cache key and a cache keyed upon less would serve a compiler other than the
one the key names.

That is one duplication left standing deliberately, and it is the only one. It
is visible in two files that are read together, and the alternative — a workflow
that asked the script which versions it intended before it knew what to restore
— would be a cache key computed from the thing it is meant to identify.

### 11.2 What a script may assume

`build_all.sh` resolves the repository root from its own location rather than
from the working directory, so it behaves the same when it is invoked by an
absolute path from elsewhere. The other two touch no file in the repository at
all, and may be run from anywhere.

Only `build_deps.sh` raises `sudo`, and only for the one command that installs
packages; `build_toolchain.sh` installs beneath the invoking user's home
directory and needs no privilege whatever. A script that asked for privilege it
did not need would be a script a contributor reads once and then runs without
reading, which is the habit this project would rather not establish.
