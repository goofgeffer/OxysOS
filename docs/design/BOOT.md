# The Oxys-OS Boot Sequence

**Corresponding phase**: Phase 1, sub-tasks 1.3 to 1.6.

**Specifications**: Multiboot2 Specification 2.0, Sections 3.1 and 3.3; Intel 64
and IA-32 Architectures Software Developer's Manual, Volume 3A, Sections 3.4.5,
4.1.2 and 4.5; Volume 2A, "CPUID".

## 1. Overview

Control passes through six stages between the firmware and the C entry point of
the kernel.

```
Firmware (BIOS)
      |
      v
GRUB 2, in Multiboot2 mode
      |  Loads the ELF64 image at physical 0x00100000.
      |  Establishes the machine state of Multiboot2 Section 3.3.
      v
_start                       32-bit protected mode, paging disabled
      |  Establishes a stack; preserves EAX and EBX.
      |  Validates the Multiboot2 magic value.
      |  Confirms CPUID and Intel 64 availability.
      |  Constructs the boot-time paging hierarchy.
      |  Sets CR4.PAE, IA32_EFER.LME, CR0.PG.
      |  Loads the 64-bit GDT and performs a far jump.
      v
BootLongModeEntry            64-bit mode, identity-mapped low memory
      |  Loads the data segment selectors.
      |  Performs an indirect jump through a 64-bit immediate.
      v
KernelEntryHigh              64-bit mode, higher-half virtual addresses
      |  Establishes the kernel stack; clears RBP.
      |  Marshals the preserved values into RDI and RSI.
      v
KernelMain                   C, System V AMD64 calling convention
```

## 2. The Multiboot2 header

The header is emitted into the section `.multiboot_header`, which `linker.ld`
places at the very beginning of the image. This satisfies the requirement of the
Multiboot2 Specification, Section 3.1, that the header appear within the first
32768 bytes of the file and be aligned on an 8-byte boundary.

Its fields, in the order prescribed by Section 3.1.1, are as follows.

| Offset | Field | Value | Authority |
| ------ | ----- | ----- | --------- |
| 0 | `magic` | `0xE85250D6` | Section 3.1.2 |
| 4 | `architecture` | `0` (32-bit protected mode of i386) | Section 3.1.2 |
| 8 | `header_length` | The length of the header in bytes, magic fields included | Section 3.1.2 |
| 12 | `checksum` | The value which, added to the preceding three fields, yields an unsigned 32-bit sum of zero | Section 3.1.2 |
| 16 | tags | The terminating tag alone: type `0`, flags `0`, size `8` | Section 3.1.3 |

No optional request tags are presently emitted. The framebuffer tag will be
added in Phase 9, sub-task 9.1.

## 3. The machine state at entry

The Multiboot2 Specification, Section 3.3, guarantees the following state upon
entry to `_start`, and Oxys-OS depends upon each guarantee.

| Element | Guaranteed state | Use made of it |
| ------- | ---------------- | -------------- |
| `EAX` | `0x36D76289` | Validated by `_start`, and again by `KernelMain`. |
| `EBX` | The 32-bit physical address of the Multiboot2 information structure | Preserved; consumed by `KernelReportBootState` and, from Phase 2, by the memory-map parser. |
| `CS` | A 32-bit read/execute segment, base 0, limit `0xFFFFFFFF` | Permits execution before the kernel's own GDT is loaded. |
| `DS`, `ES`, `FS`, `GS`, `SS` | 32-bit read/write segments, base 0, limit `0xFFFFFFFF` | Permits data access with flat addressing. |
| A20 gate | Enabled | No A20 enabling code is required. |
| `CR0` | `PE` set, `PG` clear | The long-mode transition may set `PG` directly. |
| `EFLAGS` | `VM` clear, `IF` clear | Interrupts are already masked; no IDT is yet installed. |

The specification declares every other register and flag undefined. In
particular no stack is supplied, and `_start` therefore establishes one before
issuing any `call` instruction.

## 4. Feature detection

Two checks precede the long-mode transition. A failure of either writes a
distinguishing character to the VGA text buffer at physical `0x000B8000` and
halts, that buffer being directly addressable while paging remains disabled.

| Check | Method | Failure code |
| ----- | ------ | ------------ |
| Multiboot2 handover | `EAX` compared against `0x36D76289` | `M` |
| `CPUID` availability | `EFLAGS.ID` (bit 21) is toggled and the change observed | `C` |
| Intel 64 availability | `CPUID` leaf `0x80000000` must report at least `0x80000001`; leaf `0x80000001` must return `EDX` bit 29 set | `L` |

## 5. The boot-time paging hierarchy

Long mode requires paging to be enabled; the processor cannot enter IA-32e mode
with paging disabled. A hierarchy is therefore constructed before the
transition. Its layout is described in `MEMORY-LAYOUT.md`, Section 3.

Two mappings of the same first gibibyte of physical memory are established:

1. **The identity mapping**, at linear address `0x0000000000000000`. It is
   indispensable, because at the instant `CR0.PG` is set the instruction pointer
   still holds a low address, and the very next instruction fetch must succeed.
2. **The higher-half mapping**, at linear address `0xFFFFFFFF80000000`, at which
   the kernel proper is linked.

The identity mapping is retained until the permanent kernel page tables are
constructed in Phase 2, sub-task 2.3, at which point it is removed. From that
point the structures described in this section are dead: nothing refers to them,
and the frames they occupy lie within the kernel image and are reserved by the
frame allocator. Reclaiming them is deferred, being an optimisation of some
twenty kibibytes with a corresponding risk of releasing memory still in use.

## 6. The long-mode transition

The sequence follows Intel SDM, Volume 3A, Section 4.1.2 exactly.

1. `MOV CR3, BootPml4` — install the paging-structure root.
2. Set `CR4.PAE` (bit 5) — four-level paging requires physical address extension.
3. Set `IA32_EFER.LME` (bit 8 of MSR `0xC0000080`) by `RDMSR`/`WRMSR`.
4. Set `CR0.PG` (bit 31) — enabling paging activates IA-32e mode.

At the completion of step 4 the processor is in IA-32e **compatibility** mode,
because `CS.L` is still clear. Sixty-four-bit mode is entered by loading a code
segment whose `L` flag is set, which is accomplished by the far jump in step 6.

5. `LGDT [BootGdtDescriptor]` — load a table containing a null descriptor, a
   64-bit code descriptor at selector `0x08`, and a data descriptor at selector
   `0x10`.
6. `JMP 0x08:BootLongModeEntry` — a far jump that reloads `CS` and enters
   64-bit mode.

## 7. The transfer to the higher half

The far jump of step 6 encodes a 32-bit offset and therefore cannot name an
address in the upper half of the address space. A trampoline, `BootLongModeEntry`,
is consequently placed in the identity-mapped boot section. It loads the data
segment selectors and then performs an indirect jump through a register loaded
with a 64-bit immediate:

```
mov rax, KernelEntryHigh
jmp rax
```

`KernelEntryHigh` resides in the `.text` section and therefore executes at its
higher-half virtual address. It establishes the 64 KiB kernel stack, clears
`RBP` to terminate the frame-pointer chain, loads `RDI` with the Multiboot2
information address and `RSI` with the Multiboot2 magic value in accordance with
the System V AMD64 calling convention, and calls `KernelMain`. Should
`KernelMain` return, which it is not designed to do, the processor is halted
permanently with interrupts masked.

## 8. The program headers

A segment carries one set of permissions, so a section may share a segment only
with sections of the same permissions. Left to itself the linker packs sections
into as few segments as it can and gives each the union of the permissions of
what it holds, which is how an image acquires a segment that is readable,
writable and executable at once — the condition the linker warns of. `linker.ld`
declares the segments in a `PHDRS` block instead, so the division is stated
rather than inferred:

| Segment | Flags | Holds |
| ------- | ----- | ----- |
| `boot` | `r-x` | The Multiboot2 header and the 32-bit entry code. |
| `bootdata` | `rw-` | The boot GDT, the preserved boot loader values, the boot stack and the four boot-time paging structures. |
| `text` | `r-x` | The 64-bit kernel code. |
| `rodata` | `r--` | Constants, string literals and read-only tables. |
| `data` | `rw-` | Initialised writable data, followed by `.bss`. |

The division that made this possible is between the boot code and the boot data.
They were one output section, `.boot`, because both are linked at their physical
addresses and both are finished with before the permanent tables of Phase 2 are
built; but the entry code is executed and never written, and the paging
structures are written and never executed, so nothing but their common lifetime
placed them together. They are now `.boot` and `.boot.data`, adjacent and each
in a segment of its own.

Every output section carries `ALIGN(4K)` twice. The first, before the colon,
sets the address at which the section begins; the second, after it, sets the
section's own alignment, which the linker takes as the alignment of the segment
holding it. Without the second, a segment would inherit the largest alignment
among its input sections — 16 or 32 bytes — and its file offset would no longer
be congruent to its address modulo a page, as `p_align` obliges. That congruence
is what allows a loader to map the file rather than copy it, and so to apply the
permissions these headers declare. The image accordingly presents five `LOAD`
segments, each page-aligned in both address and file offset, and none both
writable and executable.

This is the image's own statement of its permissions and not an enforcement of
them. GRUB copies the segments to their physical addresses and does not apply
their flags, and this kernel is running with paging of its own making within a
few instructions; the mappings that actually enforce anything are the ones
`PagingInitialise` builds, described in
[`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md), Section 5. What the headers give is a
correct declaration for the loader of Phase 12, which maps rather than copies,
and for every tool that reads the image in the meantime.
