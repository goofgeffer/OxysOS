<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Executable Loader

**Phase**: sub-task 6.8 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../kernel/exec/elf.c`](../../kernel/exec/elf.c),
[`../../kernel/include/oxys/exec/elf.h`](../../kernel/include/oxys/exec/elf.h).
**Specifications**: System V gABI, ELF chapters (header, program header, segment
types); System V AMD64 psABI (machine 62, `ET_EXEC`).

The loader for statically linked ELF64 executables. Every instruction it follows
comes **from the file**: the addresses, lengths, offsets and entry point. A
malformed filesystem gives a wrong answer; a malformed executable gives a kernel
that writes where the file said. The design is the list of things the loader
refuses to be told.

## 1. Reading the image

Every field is assembled from bytes (`ElfRead32` and its kin), never read through
an overlaid structure ([`../project/CODING-STANDARDS.md`](../project/CODING-STANDARDS.md)):
the image in the heap need not be aligned; the byte order is then visible in the
code; and the compiler's padding cannot disagree with the format.

- **The magic is tested before any other field**: reading fixed offsets of a file
  that is not ELF is reading arbitrary numbers.
- **The encoding is tested before any multi-byte field**: the readers are
  little-endian, and a big-endian image would decode to wrong but plausible sizes.
- **Ranges are tested by subtraction**, never `offset + size`, which overflows and
  admits a range covering everything ([`PRIVILEGE.md`](PRIVILEGE.md) applies the
  same rule to system-call arguments).
- **The whole image is judged before any page is mapped**, so a bad segment found
  half way never leaves a half-loaded address space.

## 2. What is refused

| Refused | Because |
| ------- | ------- |
| No `7Fh 'E' 'L' 'F'`. | Not ELF. |
| Not 64-bit, or not little-endian. | Fields read at wrong widths or backwards. |
| Machine not 62 (x86-64). | Instructions for another architecture. |
| Type not `ET_EXEC`. | A relocatable object has nothing to load; a shared object (including a PIE) needs relocation this loader does not do. |
| A `PT_INTERP` segment. | A dynamically linked program would fault at its first library call. |
| A header or program-header entry size not the format's. | The table walk strides by the entry size; zero reads one header repeatedly. |
| A program header table past the end of the file. | Reading whatever follows the image as segments. |
| More than `ELF_SEGMENT_MAXIMUM` (16) program headers. | Validation cost grows with the count, so the count is bounded first. |
| A segment's contents past the end of the file. | The same, per segment. |
| A segment larger in the file than in memory. | Copying more than was reserved. (The reverse is ordinary: `.bss`.) |
| Any byte at or above `SYSCALL_USER_LIMIT`. | A program in the kernel's half. Tested on the segment's end, not its start. |
| A segment in the first page. | A null pointer would be a valid address, and its dereference would silently succeed. |
| Segments not in ascending address order. | Required by the gABI, and relied on for the shared page (Section 3). |
| An entry point outside every segment. | A first instruction fetch that faults. |

**A segment of zero memory size is judged, then skipped.** The gABI permits it.
Its address is checked like any other, so an empty segment in the kernel's half is
still refused; only then is it skipped, which also avoids computing its last page
as `address + (0 − 1)`.

## 3. Loading

- **Through the direct map.** Each page is a new frame, written through the direct
  physical map and then given to the address space with the segment's permissions.
  Switching into the half-built space to write would require read-only text to be
  writable for a while.
- **Every page is zeroed first.** A frame holds whatever its last owner left; the
  part of a segment beyond its file size is what the program reads as zero. Not
  zeroing it hands kernel memory to a user program with no fault anywhere.
- **The page two segments share is reused, not replaced.** Text usually ends
  mid-page and data starts in the same page. Mapping a fresh frame would discard
  the end of the text. The shared page takes the **more permissive** permissions,
  or the program's first store to its data faults.

The stack, arguments and environment are the process's
([`PROCESS.md`](PROCESS.md)); the loader reports where to begin.

## Verification

`KernelVerifyElf` in [`../../kernel/test/exec/`](../../kernel/test/exec/) composes
images byte by byte, with its own little-endian writers (shared helpers with a
wrong byte order would agree with themselves). Only a composed image can be
malformed on purpose. A real linked program is loaded too: `startup-check` shows
that what a toolchain produces is accepted ([`LIBC.md`](LIBC.md)).

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| Each refusal of Section 2, **by its own reason**. | A loader that refuses everything, or cannot tell "not for this machine" from "not a file". |
| A segment in the first page is refused. | Null pointers that work. |
| A zero-size segment in the kernel's half is refused; one at a legitimate address is accepted and maps nothing. | The address left unjudged; empty segments wrongly rejected. |
| A good image decodes to the entry and header count written. | Headers read at wrong offsets. |
| Both segments arrive at the requested addresses, with the contents the image held. | A fresh frame over the shared page losing the text's end. |
| Memory beyond a segment's file contents reads zero, **with frames dirtied beforehand**. | Kernel leftovers disclosed. On a fresh machine frames are already zero, so the test first fills and frees a few. |
| The shared page is writable; the text is accessible at privilege level 3. | A first store that faults; text the program cannot execute. |

The contents are read at their virtual addresses with the composed space active:
the one assertion that the loader placed what it accepted.

## Limitations

1. Statically linked executables only.
2. The image is read whole into the heap, up to `ELF_FILE_MAXIMUM`; no demand
   paging from files.
3. Only `PT_LOAD` is acted on; TLS, notes and stack descriptions are ignored.
4. The execute flag is not enforced: every mapped page is executable until
   `IA32_EFER.NXE` (sub-task 13.3).
5. A failed load leaves a partial address space for the caller to destroy.
