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
| `VBoxManage` | VirtualBox execution. | **Absent.** The `run-vbox` target is provided but cannot presently be executed. |

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

No warning is presently suppressed. Should a suppression become necessary, it
must be recorded in the `Makefile` together with its justification, as required
by `PROJECT_GUIDELINES.md`, Section 4.

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

## 6. Make targets

| Target | Effect |
| ------ | ------ |
| `all` | Builds `build/oxys.elf` and confirms, by `grub-file --is-x86-multiboot2`, that the image is Multiboot2 compliant. This is the default target. |
| `iso` | Builds `build/oxys.iso` by staging the kernel and the GRUB configuration and invoking `grub-mkrescue`. |
| `clean` | Removes the whole of the `build` directory. |
| `run-qemu` | Executes the ISO under QEMU with legacy BIOS firmware, the serial port directed to the standard output stream. |
| `run-uefi` | Executes the ISO under QEMU with the OVMF UEFI firmware. |
| `run-vbox` | Registers and starts a VirtualBox machine attached to the ISO, with the serial port directed to a file. |
| `verify` | Executes the ISO under QEMU without a display, captures the serial output, and asserts that the expected banner appears. |
| `toolcheck` | Reports the presence or absence of each required tool. |

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
