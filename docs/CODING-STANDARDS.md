# Oxys-OS Coding Standards

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 4 and 8. This document elaborates upon
those requirements; it does not amend them. Where the two appear to differ,
`PROJECT_GUIDELINES.md` governs.

## 1. Language

The kernel and the userland are written in ISO C11, as defined by
ISO/IEC 9899:2011. Assembly is written for NASM in Intel syntax.

Compiler extensions are prohibited except where no conforming alternative
exists. Every use of an extension must be accompanied by a comment recording the
reason. The extensions presently in use are enumerated in Section 7 of this
document.

## 2. File headers

Every source file, header and script commences with a block comment containing:

1. The file name, given as a path relative to the project root.
2. A statement of the file's purpose.
3. An enumeration of its principal functions or data structures.
4. Citations to the specifications that it implements, each naming the
   specification, the section and, where applicable, the table or figure.

## 3. Naming conventions

| Category | Convention | Example |
| -------- | ---------- | ------- |
| Types | `PascalCase` | `VgaColour`, `PhysicalAddress` |
| Functions of external linkage | `PascalCase` | `KernelMain`, `SerialInitialise` |
| Functions of internal linkage | `PascalCase` | `VgaComposeAttribute` |
| Objects of static storage duration | `PascalCase` | `VgaCursorRow`, `SerialActivePort` |
| Local variables and parameters | `snake_case` | `page_directory_index` |
| Macros and enumeration constants | `UPPER_SNAKE_CASE` | `VGA_WIDTH`, `PAGE_SIZE` |
| Assembly labels | `PascalCase`, prefixed by the subsystem | `BootBuildPageTables` |
| Local assembly labels | `.PascalCase` | `.FillPageDirectory` |

Names are unabbreviated except where the abbreviation is universally recognised
within the discipline: `PIC`, `IDT`, `ATA`, `PCI`, `APIC`, `TLB`, `ABI`, `ELF`,
`MSR`, `GDT`, `TSS`, `DMA`, `UART`.

## 4. Layout

- Indentation is four spaces. Tabulation characters do not appear in C source,
  and appear in the `Makefile` only where GNU Make requires them.
- Braces are placed upon their own lines, in the Allman style.
- Lines do not exceed one hundred characters.
- A single blank line separates functions; two do not appear.

## 5. Comments

Comments are written in complete, grammatically correct English sentences,
terminated by a full stop. British spelling is used throughout, save in
identifiers derived from a specification, which retain the spelling of that
specification.

A comment states why the code is as it is. A comment restating what the code
plainly does is noise and is to be removed. Assertions of hardware behaviour
carry a citation.

## 6. Types

Fixed-width types from `<stdint.h>` are used wherever a value is of a defined
width, which in a kernel is almost everywhere. The types `int`, `long` and
`unsigned` appear only where the width is genuinely immaterial.

`PhysicalAddress` and `VirtualAddress` are distinct type names, so that the
confusion of the two is visible upon inspection. Conversion between them is
performed only by `PhysicalToVirtual` and `VirtualToPhysical`.

Memory that is examined or modified by hardware independently of the processor
is declared `volatile`, so that the compiler neither elides nor reorders
accesses to it.

## 7. Register of permitted compiler extensions

`PROJECT_GUIDELINES.md`, Section 8, prohibits compiler extensions unless the rationale is
documented. The following are in use.

| Extension | Where | Rationale |
| --------- | ----- | --------- |
| GNU C extended inline assembly (`__asm__ __volatile__`) | `kernel/include/oxys/io.h`, `kernel/kernel.c` | ISO C provides no means of expressing the `IN`, `OUT`, `CLI` and `HLT` instructions. No conforming alternative exists. |
| `_Noreturn` | `kernel/kernel.c` | This is an ISO C11 keyword, not an extension. It is listed here for completeness because it affects code generation. |

## 8. Prohibitions

- Floating-point and vector arithmetic in the kernel. The corresponding
  instruction sets are excluded at compilation, so a violation is a build
  failure rather than a latent defect.
- Reliance upon undefined behaviour. Pointer arithmetic, type punning and
  bitwise operations must be defined by ISO/IEC 9899:2011. Type punning is
  performed by a union or by `memcpy`, never by casting a pointer to an
  incompatible type.
- Third-party code within the kernel proper. The sole external dependencies are
  GRUB and the cross-compiler.
- The transcription of code from other operating systems. Reference
  implementations may be studied; the code that results must be original.

## 9. Documentation discipline

`PROJECT_GUIDELINES.md`, Section 2, requires that no change be regarded as complete until:

1. The inline comments and the file header of every file touched are current.
2. The affected documents within `docs/` are updated.
3. `docs/PLAN.md` reflects the new state of progress.

A change that satisfies the compiler but not this discipline is incomplete.

## 10. Commit messages

Commit messages are formal and objective. The subject line is written in the
imperative mood, does not exceed seventy-two characters, and carries no
terminating full stop. The body explains the reason for the change and names the
phase and sub-task of `PLAN.md` to which it corresponds. Emojis, colloquialisms
and humour do not appear.
