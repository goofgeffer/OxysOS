# Oxys-OS Bibliography

**Authority**: `PROJECT_GUIDELINES.md`, Section 6, requires that every design decision be
justified by reference to an authoritative specification, and that the
specification be cited in the code comment or design document that relies upon
it. This file is the register of those specifications.

## 1. Consulted and presently relied upon

### Multiboot2 Specification, version 2.0
Free Software Foundation. Published as part of the GNU GRUB manual.
`https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html`

Sections relied upon:

- **3.1.1**, the layout of the Multiboot2 header: the 32-bit fields `magic`,
  `architecture`, `header_length` and `checksum`, in that order, followed by the
  tags.
- **3.1.2**, the magic fields: the header magic is `0xE85250D6`; the architecture
  value `0` denotes the 32-bit protected mode of i386; the checksum is the value
  which, added to the other three magic fields, yields an unsigned 32-bit sum of
  zero.
- **3.1.3**, the general tag structure: tags are aligned upon 8-byte boundaries
  and are terminated by a tag of type `0` and size `8`.
- **3.3**, the i386 machine state at entry: `EAX` contains `0x36D76289`; `EBX`
  contains the physical address of the boot information structure; `CS` is a
  32-bit read/execute segment of base 0 and limit `0xFFFFFFFF`; the data segments
  are 32-bit read/write segments of the same base and limit; the A20 gate is
  enabled; `CR0.PG` is clear and `CR0.PE` is set; `EFLAGS.VM` and `EFLAGS.IF` are
  clear.
- **3.6.1**, the placement of the structure: the boot loader may place it
  anywhere in memory, and the operating system must avoid overwriting it until it
  has finished using it.
- **3.6.2**, the basic tag structure: the fixed part of a 32-bit total size and a
  32-bit reserved field; the common tag header of a 32-bit type and a 32-bit
  size; the rule that a tag's size excludes its trailing padding and that each
  tag begins at an 8-byte aligned address.
- **3.1.10**, the framebuffer request tag placed in the image header: type 5,
  size 20, a flags field whose bit 0 marks the request optional, and width,
  height and depth of which zero means no preference. Its presence is what
  obliges the boot loader to emit the framebuffer information tag below.
- **3.6.7**, the ELF-Symbols tag, type 9. Note the discrepancy recorded in
  `kernel/include/oxys/multiboot2.h`: the prose of this section and the reference
  C header in the same document disagree upon the widths of the `num`, `entsize`
  and `shndx` fields.
- **3.6.8**, the memory map tag, type 6: the `entry_size` and `entry_version`
  fields; the guarantee that `entry_size` is a multiple of eight; the entry
  layout of `base_addr`, `length`, `type` and `reserved`; the region type values,
  of which 1 denotes available memory; and the warning that the map includes the
  regions occupied by the kernel and by the boot information structure, which the
  kernel must take care not to overwrite.
- **3.6.12**, the framebuffer information tag, type 8: the 64-bit address, the
  pitch, the width, the height, the bits per pixel, the framebuffer kind
  (0 indexed, 1 RGB, 2 EGA text), and the **sixteen-bit** reserved field, after
  which the colour description begins at offset 32. The width of the reserved
  field is settled by the reference implementation's
  `struct multiboot_tag_framebuffer_common`, the prose diagram being ambiguous
  upon it: the prose shows a `u8` reserved and the header a `multiboot_uint16_t`,
  and only the latter puts the colour description at the offset the fixed
  32-byte prefix requires.
- **3.6.16 and 3.6.17**, the ACPI old and new RSDP tags, types 14 and 15: each
  carries, after the common type and size fields, a copy of the Root System
  Description Pointer as ACPI 1.0 and as ACPI 2.0 or later respectively define
  it. The copy lies within the boot information structure and not in the
  firmware's own memory, which is what makes the tag the only source a UEFI boot
  could supply.

Used by: `boot/boot.asm`, `kernel/multiboot2.c`,
`kernel/include/oxys/multiboot2.h`, `kernel/kernel.c`, `kernel/acpi/acpi.c`,
`kernel/include/oxys/bootinfo.h`, `linker.ld`, `boot/grub/grub.cfg`.

### Intel 64 and IA-32 Architectures Software Developer's Manual
Intel Corporation. `https://www.intel.com/sdm`

Sections relied upon:

- **Volume 1, Section 18.3**, input/output: the I/O address space comprises
  65536 individually addressable 8-bit ports.
- **Volume 2A, "CPUID"**: the ability to modify `EFLAGS.ID`, bit 21, indicates
  the availability of the instruction; leaf `0x80000001` reports Intel 64
  support in `EDX` bit 29.
- **Volume 2B, "HLT"**: the instruction halts the processor until an interrupt,
  a debug exception, a non-maskable interrupt or a reset occurs.
- **Volume 2B, "STI"**: the instruction's effect upon the interrupt flag is
  delayed by one instruction, so that an interrupt cannot be delivered until
  after the instruction following it. This is why the idiom `STI; HLT` has no
  window in which an interrupt is serviced before the processor halts, and why
  nothing may be placed between the two.
- **Volume 3A, Section 3.3.7.1**, canonical addressing: bits 63 to 47 of a linear
  address must be identical.
- **Volume 3A, Section 3.4.5 and Figure 3-8**, the segment descriptor format,
  including the `L` flag that designates a 64-bit code segment.
- **Volume 3A, Section 4.1.2 and Table 4-14**, the sequence required to enter
  IA-32e mode: enable `CR4.PAE`, set `IA32_EFER.LME`, then enable `CR0.PG`.
- **Volume 3A, Section 4.5 and Figure 4-8**, four-level paging: the structure
  hierarchy, the decomposition of a linear address into indices, and the use of
  the `PS` flag to map a 2 MiB page.
- **Volume 3A, Table 4-15**, the paging-structure entry flags `P`, `R/W`, `U/S`
  and `PS`.
- **Volume 3A, Table 4-19**, the format of a page-table entry that maps a 4-KByte
  page: bits 11:9 are Ignored, and are therefore available to software. Bit 9 is
  used to mark a page copy-on-write.
- **Volume 3A, Section 4.6**, access rights: the permissions of a translation are
  the conjunction of those held at every level of the hierarchy, which is why a
  restriction must be applied at the leaf entry and not at an intermediate one.
- **Volume 3A, Section 6.2**, exception and interrupt vectors: vectors 0 to 31
  are reserved for architecture-defined exceptions; 32 to 255 are available. This
  is the constraint that obliges the 8259A controllers to be remapped, the
  vectors 8 to 15 that the firmware leaves the master presenting lying wholly
  within the reserved range.
- **Volume 3A, Section 6.10**, the interrupt descriptor table and the IDTR
  register; the limit is one less than the size of the table in bytes.
- **Volume 3A, Section 6.14.1 and Figure 6-8**, the 64-bit mode IDT: the index is
  the vector scaled by 16, each descriptor occupying sixteen bytes; only 64-bit
  interrupt and trap gates are valid, a legacy 32-bit type generating a
  general-protection exception.
- **Volume 3A, Section 3.4.2**, the processor sets the accessed bit of a segment
  descriptor when its selector is loaded, so the table must be writable.
- **Volume 3A, Section 3.4.3**, the CS register cannot be loaded by MOV; it is
  changed by a far transfer.
- **Volume 3A, Section 3.5.1**, the GDTR, and the limit being one less than the
  size of the table.
- **Volume 3A, Section 6.5**, the classification of exceptions as faults, traps
  and aborts, and the consequence for restarting the interrupted instruction.
- **Volume 3A, Section 6.12.1 and Figure 6-4**, the stack frame the processor
  pushes: in 64-bit mode SS, RSP, RFLAGS, CS and RIP unconditionally, with RSP
  aligned to sixteen bytes beforehand.
- **Volume 3A, Section 6.13**, the error code, pushed last and padded to eight
  bytes in 64-bit mode.
- **Volume 3A, Section 2.5**, the control registers: CR0 the system control
  flags, CR2 the page-fault linear address, CR3 the paging-structure base, CR4
  the architectural extension flags.
- **Volume 3A, Section 6.13 and Figure 6-6**, the selector-form error code: EXT,
  IDT and TI flags with a 13-bit selector index; the rule that the handler must
  remove the error code before returning, IRET not popping it; and the statement
  that no error code is pushed for an exception generated by the `INT n`
  instruction or externally, even where one is normally produced.
- **Volume 3A, Section 6.15 and Figure 6-9**, the page-fault exception: the error
  code flags P, W/R, U/S, RSVD, I/D, PK and SGX; the loading of CR2 with the
  faulting linear address, and the warning that CR2 must be saved before a
  further fault can occur; and the provision that supervisor-mode writes to a
  read-only page fault only when CR0.WP is set.
- **Volume 3A, Table 6-1**, the architecturally defined exceptions, their
  mnemonics, their classification, and whether each pushes an error code. From
  sub-task 6.4 this table also fixes which faults receive a graphical fault
  screen written for them, and what each of those screens says the processor is
  reporting.
- **Volume 2A, Section 2.3.11**, the fifteen-byte limit upon the length of an
  instruction, which is how many bytes a fault screen reproduces from the
  instruction pointer: fewer could stop short of the instruction that faulted.
- **Volume 2A, "IRET/IRETQ"** and **"RET"** (far form), the return from a handler
  and the far return used to reload CS.
- **Volume 2A, "LGDT/LIDT" and "SGDT/SIDT"**, the instructions that load and
  store the descriptor table registers.
- **Volume 3A, Section 4.10.4.1**, invalidation: writing CR3 invalidates every
  translation-lookaside-buffer entry associated with the current process context,
  save those for global pages; `INVLPG` invalidates the entries for one linear
  address upon the executing processor.
- **Volume 3A, Section 4.10.4.4**, an invalidation may be deferred only while no
  processor can use the stale translation. This is what obliges a shootdown to be
  waited for rather than merely sent.
- **Volume 3A, Section 4.10.5**, "Propagation of Paging-Structure Changes to
  Multiple Processors": the invalidation reaches the executing processor alone,
  so software must interrupt every other processor that may have cached the
  translation and have each perform the invalidation for itself. The manual names
  the procedure "TLB shootdown".
- **Volume 3A, Section 3.4.4**, the FS and GS segment bases in 64-bit mode. They
  are held in `IA32_FS_BASE` and `IA32_GS_BASE`, are **not** ignored as the other
  segment bases are, and a load of the segment register from a descriptor
  replaces the hidden base with the descriptor's — which for a flat data
  descriptor is zero.
- **Volume 3A, Section 8.1.2.2**, bus locking: the `LOCK` prefix makes the
  read-modify-write of the destination operand atomic with respect to every other
  processor, and is honoured for `XADD`, `CMPXCHG`, `ADD` and `SUB` among others.
- **Volume 3A, Section 8.2.2**, the memory-ordering model: loads are not
  reordered with other loads, stores are not reordered with other stores, and
  stores are not reordered with older loads. An acquire and a release therefore
  need no fence instruction.
- **Volume 2A, "XADD"** and **"CMPXCHG"**, fetch-and-add and compare-and-exchange,
  which are the whole of the spinlock's atomicity.
- **Volume 2B, "PAUSE"**, which improves the performance of a spin-wait loop and
  reduces the power it draws, de-pipelining the loop so that the processor
  leaving it does not pay the memory-order violation penalty its speculated reads
  would otherwise incur.
- **Volume 2B, "SWAPGS"**, which exchanges `GS.base` with the contents of
  `IA32_KERNEL_GS_BASE` and is valid only at privilege level 0 — which is what
  makes the value it produces one a user program cannot have chosen.
- **Volume 3A, Section 13.1**, the enabling and state management required of the
  SSE and x87 units.
- **Volume 1, Section 3.4.3**, the flags of the `RFLAGS` register, from which the
  mask written into `IA32_FMASK` is composed.
- **Volume 2B, "SYSCALL"**, the fast system-call transition: `RCX` receives the
  address of the following instruction and `R11` the value of `RFLAGS` before the
  mask is applied; `RIP` is loaded from `IA32_LSTAR`; `CS` and `SS` are loaded
  from `IA32_STAR`; every bit set in `IA32_FMASK` is then cleared in `RFLAGS`;
  and the stack pointer is *not* changed.
- **Volume 2B, "SYSRET"**, the inverse transition, which returns to privilege
  level 3 unconditionally, the requested privilege level of the selectors it
  loads being forced to 3.
- **Volume 3A, Section 5.8.8**, fast system calls in 64-bit mode: `SYSCALL` loads
  `CS` from `IA32_STAR[47:32]` and `SS` from that value plus eight; `SYSRET` with
  a 64-bit operand size loads `CS` from `IA32_STAR[63:48]` plus sixteen and `SS`
  from that value plus eight. This fixes the order of the user descriptors in the
  global descriptor table.
- **Volume 3A, Table 2-1**, `IA32_EFER` bit 0, `SCE`, without which `SYSCALL`
  raises an invalid-opcode exception.
- **Volume 3A, Section 8.7 and Figure 8-11**, the 64-bit task state segment: its
  104 bytes, `RSP0` to `RSP2`, the seven interrupt stack table entries, and the
  I/O map base.
- **Volume 3A, Section 8.2.3 and Figure 8-4**, the sixteen-byte task state
  segment descriptor of 64-bit mode, whose type is 9 while the segment is
  available and 11 once a task register has been loaded with a selector for it.
- **Volume 3A, Section 6.14.4**, the interrupt stack table: where a gate names an
  entry, the processor loads that stack unconditionally, whether or not the
  privilege level changes — which is what makes it usable for a fault taken at
  privilege level 0 upon a stack that is itself the fault.
- **Volume 3A, Section 20.5.2**, the I/O permission bitmap: where the map base
  exceeds the segment limit, there is no bitmap and every port is denied to a
  privilege level above `IOPL`.
- **Volume 2A, "LTR"**, the loading of the task register: the operand is a
  selector for an available task state segment descriptor, and the instruction
  marks that descriptor busy.
- **Volume 2A, "CPUID"**, leaf `0x80000000` reporting the highest extended leaf
  implemented, and bit 11 of `EDX` from leaf `0x80000001` reporting the
  availability of `SYSCALL` and `SYSRET`.
- **Volume 3A, Section 11.12.2 and Table 11-11**, the page attribute table:
  `IA32_PAT` holds eight memory-type entries of eight bits, and a page-table
  entry selects one of them by the index `(PAT << 2) | (PCD << 1) | PWT`, where
  PAT is bit 7 of a 4-KByte page-table entry.
- **Volume 3A, Table 11-10**, the memory-type encodings, of which `0x00` is
  uncacheable, `0x01` write-combining, `0x04` write-through, `0x06` write-back
  and `0x07` uncacheable-minus. The processor's defaults for the eight entries
  are `0x06`, `0x04`, `0x07`, `0x00` and those four again.
- **Volume 3A, Table 11-7**, the effective memory type: what the page attribute
  table selects is combined with what the memory type range registers say, and
  the more conservative of the two prevails.
- **Volume 3A, Section 4.5 and Table 4-19**, bit 7 of a page-table entry is PAT,
  whereas bit 7 of a directory entry is PS. The two meanings share one bit and
  are distinguished only by the level at which the entry stands.
- **Volume 2A, "CPUID"**, leaf 1, EDX bit 16: whether the page attribute table is
  present at all.
- **Volume 3A, Chapter 10 (Advanced Programmable Interrupt Controller)**, and
  within it:
  - **Section 10.4.1**, the registers are memory mapped to a 4 KiB region whose
    initial address is `0xFEE00000`, and that region must be mapped strong
    uncacheable.
  - **Table 10-1**, the register address map: the identifier at `0x020`, the
    version at `0x030`, the task priority at `0x080`, the end-of-interrupt at
    `0x0B0`, the spurious-interrupt vector at `0x0F0`, the error status at
    `0x280`, the interrupt command at `0x300`, and the local vector table
    entries from `0x2F0` to `0x370`.
  - **Section 10.4.2**, CPUID leaf 1 reports an on-chip local APIC in EDX bit 9.
  - **Sections 10.4.3 and 10.4.4 and Figure 10-5**, `IA32_APIC_BASE` at MSR
    `0x1B`: bit 8 the bootstrap processor flag, bit 11 the global enable, bits
    35:12 the base address. There are two enables and they are separate
    mechanisms; the software one is bit 8 of the spurious-interrupt vector
    register.
  - **Section 10.4.7.1**, the state after reset: every local vector table entry
    masked, and the spurious-interrupt vector register holding `0x000000FF`,
    whose bit 8 is clear — so the controller arrives software-disabled.
  - **Section 10.4.8**, the version register, and the count of local vector table
    entries it reports as the field's value plus one.
  - **Section 10.5.1 and Figure 10-8**, the local vector table entry: the vector
    in bits 7:0, the delivery mode in bits 10:8, the pin polarity in bit 13, the
    trigger mode in bit 15 and the mask in bit 16.
  - **Section 10.5.2**, vectors 16 to 255 are valid; a vector below 16 is
    recorded as illegal in the error status register.
  - **Section 10.8.5**, every handler save those entered by the non-maskable,
    system-management, initialisation and external delivery modes must write the
    end-of-interrupt register before returning; for a level-triggered interrupt
    the local APIC additionally sends an end-of-interrupt message to the I/O
    APICs.
  - **Section 10.8.6**, the task priority register blocks every interrupt of a
    priority class at or below the value it holds; zero blocks none.
  - **Section 10.6**, issuing interprocessor interrupts: one is sent by writing
    the interrupt command register of the sending processor's local controller,
    and is received by the target exactly as a device's request would be.
  - **Section 10.6.1 and Figure 10-12**, the interrupt command register: the
    vector in bits 7:0, the delivery mode in 10:8 (000 fixed, 001 lowest
    priority, 010 SMI, 100 NMI, 101 INIT, 110 start-up), the destination mode in
    bit 11, the read-only delivery status in bit 12, the level in bit 14, the
    trigger mode in bit 15, and the destination shorthand in bits 19:18 (00 none,
    01 self, 10 all including self, 11 all excluding self). **"The act of writing
    to the low doubleword of the ICR causes the IPI to be sent"**, which is why
    the high half must be written first.
  - **Section 10.6.2.1**, the destination field in xAPIC mode: bits 63:56 of the
    register, being bits 31:24 of the high half.
  - **Section 10.8.3**, the vector number is the interrupt's priority, so an
    interrupt sent between processors at a high vector is served ahead of any
    device request the target is also holding.
  - **Section 10.9 and Figure 10-23**, the spurious interrupt: its handler must
    return without an end-of-interrupt, bit 8 of the register is the software
    enable, and upon the P6 family and the Pentium the low four bits of the
    vector are hardwired to one.

  **The chapter number depends upon the edition.** This corpus cites the edition
  in which Memory Cache Control is Chapter 11 and the page attribute table is
  Section 11.12.2, and in that edition the APIC is Chapter 10. Intel has since
  renumbered: in a current manual the APIC is Chapter 11 and Memory Cache Control
  is Chapter 12, and every `10.x` above becomes `11.x`. The earlier numbering is
  used throughout because the rest of this document already does, and a corpus
  citing two editions at once would send a reader to the wrong chapter roughly
  half the time.
- **Volume 4**, the model-specific registers `IA32_APIC_BASE` (`0x1B`),
  `IA32_PAT` (`0x277`),
  `IA32_EFER` (`0xC0000080`),
  `IA32_STAR` (`0xC0000081`), `IA32_LSTAR` (`0xC0000082`), `IA32_CSTAR`
  (`0xC0000083`), `IA32_FMASK` (`0xC0000084`), `IA32_FS_BASE` (`0xC0000100`),
  `IA32_GS_BASE` (`0xC0000101`) and `IA32_KERNEL_GS_BASE` (`0xC0000102`).

Used by: `boot/boot.asm`, `linker.ld`, `Makefile`, `kernel/include/oxys/io.h`,
`kernel/kernel.c`, `kernel/include/oxys/pic.h`, `kernel/include/oxys/pit.h`,
`kernel/include/oxys/lapic.h`, `drivers/apic/lapic.c`, `kernel/cpu/irq.c`,
`docs/devices/APIC.md`,
`kernel/cpu/gdt.c`, `kernel/cpu/tss.c`, `kernel/cpu/syscall.c`,
`kernel/cpu/syscall_entry.asm`, `kernel/include/oxys/tss.h`,
`kernel/include/oxys/syscall.h`, `kernel/include/oxys/msr.h`,
`graphics/framebuffer.c`, `kernel/include/oxys/framebuffer.h`,
`graphics/faultscreen.c`, `kernel/include/oxys/faultscreen.h`,
`kernel/test/verify_faultscreen.c`, `docs/design/MEMORY-LAYOUT.md`,
`docs/design/INTERRUPTS.md`, `docs/design/PRIVILEGE.md`,
`docs/design/FAULTSCREEN.md`.

### Algorithm for computer control of a digital plotter
J. E. Bresenham, IBM Systems Journal, volume 4, number 1, pages 25 to 30, 1965.

Sections relied upon:

- The line algorithm entire: each step is chosen by comparing an error
  accumulated in integers against the deltas of the two axes, so the line is
  drawn without division and without floating point — the latter being prohibited
  in this kernel by `PROJECT_GUIDELINES.md`, Section 8.

The consequence this project depends upon, which the paper states and which is
easily forgotten: the choice at each step depends upon the error accumulated
*since the start*. A line begun at a different point is therefore a different
line, which is why `graphics/draw.c` clips per pixel rather than by moving the
endpoints. See `docs/design/DRAWING.md`, Section 4.

Used by: `graphics/draw.c`, `kernel/test/verify_graphics.c`,
`docs/design/DRAWING.md`.

### AMD64 Architecture Programmer's Manual, Volume 2: System Programming
Advanced Micro Devices, publication 24593.

Consulted as the second authority upon the mechanism AMD defined, where the two
manuals describe one processor feature and agree.

Sections relied upon:

- **Section 6.1**, `SYSCALL` and `SYSRET`, and the three registers that
  configure them under the names `STAR`, `LSTAR` and `SFMASK`. The mechanism is
  AMD's; Intel's manual documents the same register numbers and the same
  behaviour, and the two are cited together where a reader may know it under
  either set of names.

Used by: `kernel/include/oxys/msr.h`, `docs/design/PRIVILEGE.md`.

### System V Application Binary Interface, AMD64 Architecture Processor Supplement
`https://gitlab.com/x86-psABIs/x86-64-ABI`

Sections relied upon:

- **Section 3.1.2**, data representation: the LP64 model.
- **Section 3.2.2**, the stack frame: the 128-byte red zone below the stack
  pointer, which is inadmissible in kernel code.
- **Section 3.2.3**, parameter passing: the first two integer arguments are
  passed in `RDI` and `RSI`.
- **Section 3.2.1**, the direction flag is required to be clear at a function's
  entry, which is among the reasons it appears in `IA32_FMASK`.
- **Section 3.2.2**, the stack pointer is sixteen-byte aligned at a function's
  entry, which is why the stacks the task state segment names are aligned and
  sized in multiples of sixteen.
- The ELF64 object file format, and the machine identifier 62 by which an
  object states that it is for this architecture.

Used by: `boot/boot.asm`, `linker.ld`, `Makefile`, `kernel/cpu/tss.c`,
`kernel/include/oxys/tss.h`, `kernel/include/oxys/syscall.h`,
`kernel/exec/elf.c`, `docs/design/PRIVILEGE.md`.

### Executable and Linking Format Specification, version 1.2
Tool Interface Standard, together with the **ELF-64 Object File Format**,
version 1.5 draft 2.

Sections relied upon:

- **The identification**: bytes 0 to 3 of the file are `7Fh 'E' 'L' 'F'`; byte 4
  the class, 2 being a 64-bit object; byte 5 the data encoding, 1 being two's
  complement little endian; byte 6 the file version, 1 being current.
- **The file header**, which is 64 bytes: the type at offset 16, the machine at
  18, the version at 20, the entry at 24, the program header offset at 32, the
  section header offset at 40, the flags at 48, the header's own size at 52, the
  program header entry size at 54 and their number at 56.
- **The file types**: 1 relocatable, 2 executable, 3 a shared object — which is
  what a position-independent executable is, its addresses being offsets from
  wherever it is placed.
- **The program header**, which is 56 bytes: the type at offset 0, the flags at
  4, the offset within the file at 8, the virtual address at 16, the physical
  address at 24, the size within the file at 32, the size in memory at 40 and the
  alignment at 48.
- **The segment types**: 1 a loadable segment, 3 the name of an interpreter,
  which marks a program as dynamically linked.
- **The segment permissions**: 1 execute, 2 write, 4 read.
- **The ordering requirement**: loadable segments appear in the program header
  table in ascending order of virtual address.

Used by: `kernel/exec/elf.c`, `kernel/include/oxys/elf.h`.

### ISO/IEC 9899:2011, Programming languages — C
International Organization for Standardization.

Sections relied upon:

- **Section 4, paragraph 6**, the freestanding execution environment and the
  headers that it must provide.
- **Section 6.7.9, paragraph 4**, the requirement that the initialiser of an
  object of static storage duration be a constant expression.
- **Sections 7.18, 7.20**, `<stdbool.h>` and `<stdint.h>`.

Used by: the whole of the C source.

### National Semiconductor PC16550D datasheet
The universal asynchronous receiver/transmitter of the IBM Personal Computer AT
and its successors.

Sections and tables relied upon:

- **Table 1**, "Summary of Registers": the bit assignments of every register.
- **Table 2**, "Register Addresses": the eight registers at consecutive offsets
  from the base address, and the overlay of the divisor latches upon offsets 0
  and 1 while the divisor latch access bit is set.
- **Table 5**, "Interrupt Control Functions": bit 0 of the interrupt
  identification register is clear while an interrupt is pending; bits 3 to 1
  identify the highest-priority pending source, being 011 the receiver line
  status, 010 received data available, 110 the character timeout, 001 the
  transmitter holding register empty and 000 the modem status; and the action
  that resets each, the transmitter interrupt being reset by reading that
  register or by writing the transmitter holding register.
- **Section "Line Control Register"**: bits 1 and 0 the word length, bit 2 the
  number of stop bits, bits 5 to 3 the parity including stick parity, bit 7 the
  divisor latch access bit.
- **Section "Line Status Register"**: bit 0 a received character, bits 1 to 4 the
  overrun, parity, framing and break conditions, bit 5 the transmitter holding
  register empty, bit 6 the transmitter wholly idle.
- **Section "Programmable Baud Generator"**: the divisor is the reference
  oscillator frequency divided by sixteen times the desired signalling rate.
- **Section "FIFO Interrupt Mode Operation"**: the transmitter first-in-first-out
  buffer holds sixteen characters, and the adapter reports it empty when it has
  room for a full complement.
- **Section "MODEM Control Register"**: bit 3 the auxiliary output OUT2, bit 4
  local loopback, in which the modem control outputs are internally connected to
  the corresponding inputs.

The datasheet is distributed as a scanned document, so the figures within it
cannot be quoted by number with confidence; the tables and sections above are
named as they are printed.

Used by: `drivers/serial/serial.c`, `kernel/include/oxys/serial.h`,
`docs/devices/SERIAL.md`.

### Intel 8259A Programmable Interrupt Controller datasheet
Intel Corporation, order number 231468-003, December 1988.
`https://pdos.csail.mit.edu/6.828/2010/readings/hardware/8259A.pdf`

Sections relied upon:

- **"INITIALIZATION COMMAND WORDS (ICWS)"**, the initialisation sequence: a write
  to the command port with bit 4 set is interpreted as ICW1 and begins the
  sequence; ICW1 bit 0 (IC4) declares that ICW4 will follow and bit 1 (SNGL)
  distinguishes a single controller from a cascaded pair; ICW2 supplies bits 7 to
  3 of the vector, the controller filling bits 2 to 0 with the request level,
  whence a vector base must be divisible by eight; ICW3 is a bit mask of the
  lines bearing slaves at the master and the cascade identity at the slave; ICW4
  bit 0 selects the 8086 mode, in which the controller presents an eight-bit
  vector rather than a `CALL` instruction.
- **The same section**, the actions ICW1 performs automatically: the edge sense
  circuit is reset, **the interrupt mask register is cleared**, IR7 is assigned
  the lowest priority, the slave mode address is set to seven, the special mask
  mode is cleared and the status read is set to the interrupt request register.
  The clearing of the mask register is why `drivers/pic/pic.c` masks every line
  after the sequence and not before it.
- **"OPERATION COMMAND WORDS (OCWS)"**, OCW1: the interrupt mask register,
  reached at the data port, a set bit withholding the corresponding line.
- **The same section**, OCW2: the R, SL and EOI bits, of which the encoding R=0,
  SL=0, EOI=1 is the non-specific end-of-interrupt, resetting the highest
  priority bit set in the in-service register. This is correct in the fully
  nested mode the controller is initialised into, in which that bit is
  necessarily the one being completed.
- **The same section**, OCW3: the RR bit selects the register subsequently read
  at the command port and the RIS bit chooses the in-service register when set
  and the interrupt request register when clear. This is the mechanism by which a
  spurious request is distinguished from a real one.

Used by: `drivers/pic/pic.c`, `kernel/include/oxys/pic.h`, `kernel/kernel.c`.

### Intel 8254 Programmable Interval Timer datasheet
Intel Corporation, order number 231164-005, September 1993.
`https://www.scs.stanford.edu/10wi-cs140/pintos/specs/8254.pdf`

Sections relied upon:

- **"Programming the 8254"**, the control word format: bits 7 and 6 (SC1, SC0)
  select the counter; bits 5 and 4 (RW1, RW0) select the read/write format, of
  which the value 11 transfers the count as two bytes with the least significant
  first; bits 3 to 1 (M2, M1, M0) select the operating mode; bit 0 selects binary
  counting when clear. The control word must precede the count, the counter using
  it to determine how many bytes to expect.
- **"Mode 2: Rate Generator"**: the counter reloads automatically upon reaching
  one, so the output is periodic without further intervention by software; and a
  count of one is illegal in this mode, the output remaining high and no
  interrupt being raised.
- **"Mode 3: Square Wave Mode"**: the count is decremented by two upon each
  clock so that the output's high and low phases are of equal duration, whence
  the mode behaves as intended only for an even count. This is why the rate
  generator is preferred for the system tick.
- **"Counter Latch Command"**: a control word whose read/write field is 00
  latches the present count into a holding register, which may then be read
  without disturbing the counting in progress. Without it the two halves of a
  sixteen-bit count would be sampled at different instants.

Used by: `drivers/pit/pit.c`, `kernel/include/oxys/pit.h`, `kernel/kernel.c`.

### Intel 82093AA I/O Advanced Programmable Interrupt Controller datasheet
Intel Corporation, order number 290566-001, May 1996.

Sections relied upon:

- **Section 3.1**, the two memory-mapped registers through which every other is
  reached: `IOREGSEL` at offset `0x00`, whose bits 7:0 "specify the IOAPIC
  register to be read/written via the IOWIN Register", and `IOWIN` at offset
  `0x10`, whose "memory references ... are mapped to the APIC register specified
  by the contents of the IOREGSEL Register".
- **Section 3.2.1**, `IOAPICID` at index `0x00`, the identification occupying
  bits 27:24.
- **Section 3.2.2**, `IOAPICVER` at index `0x01`: bits 7:0 the implementation
  version, and bits 23:16 the maximum redirection entry, being "the entry number
  (0 being the lowest entry) of the highest entry in the I/O Redirection Table.
  The value is equal to the number of interrupt input pins for the IOAPIC minus
  one. The range of values is 0 through 239."
- **Section 3.2.4**, the redirection table registers at indices `0x10` upward,
  two 32-bit registers to each 64-bit entry, and the fields of an entry:
  destination in bits 63:56, mask in bit 16, trigger mode in bit 15 where one is
  level sensitive, remote in-service in bit 14, input polarity in bit 13 where
  one is active low, delivery status in bit 12, destination mode in bit 11,
  delivery mode in bits 10:8, and vector in bits 7:0.

The datasheet is marked "PRELIMINARY" and describes one implementation of an
interface every chipset since has reproduced. Where it and the ACPI
specification both speak — the count of interrupt inputs, for instance — the
register is read and the table is not, ACPI 6.5, Section 5.2.12.3, expressly
referring the reader to this register.

Used by: `drivers/apic/ioapic.c`, `kernel/include/oxys/ioapic.h`,
`docs/devices/APIC.md`.

### Advanced Configuration and Power Interface Specification, version 6.5
UEFI Forum, August 2022. `https://uefi.org/specs/ACPI/6.5/`

Sections relied upon:

- **Section 5.2.5.1**, finding the Root System Description Pointer upon an IA-PC
  system: the first kibibyte of the Extended BIOS Data Area, whose segment
  address is the two bytes at `0x40E`, and the read-only memory between
  `0x0E0000` and `0x0FFFFF`, both searched upon 16-byte boundaries.
- **Section 5.2.5.2**, upon a UEFI system the pointer is instead a field of the
  EFI System Table. This is why the boot loader's copy is preferred to a search.
- **Section 5.2.5.3 and Table 5.3**, the RSDP: the signature `"RSD PTR "` with
  its trailing space, the checksum over bytes 0 to 19, the revision at offset 15,
  the 32-bit RSDT address at 16, and — from revision 2 — the length at 20, the
  64-bit XSDT address at 24 and the extended checksum at 32.
- **Section 5.2.6 and Table 5.4**, the 36-byte description header every table
  begins with, and that "the entire table, including the checksum field, must add
  to zero to be considered valid".
- **Sections 5.2.7 and 5.2.8**, the RSDT with 32-bit entries and the XSDT with
  64-bit ones; "an ACPI-compatible OS must use the XSDT if present".
- **Section 5.2.12 and Table 5.19**, the Multiple APIC Description Table: the
  32-bit local interrupt controller address at offset 36, the flags at 40 and the
  list of interrupt controller structures from 44.
- **Table 5.20**, the `PCAT_COMPAT` flag: "A one indicates that the system also
  has a PC-AT-compatible dual-8259 setup. The 8259 vectors must be disabled (that
  is, masked) when enabling the ACPI APIC operation."
- **Table 5.21**, the interrupt controller structure types, of which this kernel
  acts upon 0, 1, 2, 4, 5 and 9.
- **Section 5.2.12.2 and Tables 5.22 and 5.23**, the Processor Local APIC
  structure and its Enabled and Online Capable flags.
- **Section 5.2.12.3 and Table 5.24**, the I/O APIC structure: the identifier,
  the 32-bit address, and the global system interrupt its first input carries.
- **Section 5.2.12.4**, that global system interrupts 0 to 15 carry the 8259A
  request lines 0 to 15 except where an override says otherwise. This is what
  allows one line number to mean the same device under either controller.
- **Section 5.2.12.5 and Table 5.25**, the Interrupt Source Override structure,
  and that "this specification only supports overriding ISA interrupt sources".
- **Table 5.26**, the MPS INTI flags: polarity in bits 1:0, trigger mode in bits
  3:2, and `00` in either meaning the source conforms to the convention of its
  bus.
- **Section 5.2.12.7 and Table 5.28**, the Local APIC NMI structure, and that a
  processor identifier of `0xFF` applies the entry to every processor.
- **Section 5.2.12.8 and Table 5.29**, the Local APIC Address Override structure,
  which supersedes the MADT header's 32-bit field for every local controller.
- **Section 5.2.12.12 and Table 5.34**, the Processor Local x2APIC structure.

Used by: `kernel/acpi/acpi.c`, `kernel/include/oxys/acpi.h`, `kernel/cpu/irq.c`,
`drivers/apic/lapic.c`, `drivers/apic/ioapic.c`, `docs/devices/ACPI.md`,
`docs/devices/APIC.md`, `docs/design/INTERRUPTS.md`.

### IBM Personal Computer AT technical reference
International Business Machines Corporation. The system technical reference for
the IBM Personal Computer AT, which defines the peripheral complement that every
subsequent x86 machine reproduces, and whose arrangement of ports and interrupt
lines the firmware of a modern machine still presents.

Sections relied upon:

- **The interrupt controllers**: the master is decoded at I/O ports `0x20` and
  `0x21` and the slave at `0xA0` and `0xA1`; the slave's output is attached to
  the master's IR2 input, whence IR2 is unavailable as an ordinary request line.
  The firmware programmes the master to present vectors 8 to 15 and the slave
  0x70 to 0x77, which is the state in which the kernel receives the machine; the
  8259A itself holds no vector base until ICW2 is written, so these values are a
  property of the firmware and not of the device.
- **The interrupt request assignment**: IR0 is the interval timer and IR1 the
  keyboard controller.
- **The interval timer**: counter 0 is decoded at port `0x40`, counter 1 at
  `0x41`, counter 2 at `0x42` and the control register at `0x43`; counter 0's
  output is attached to IR0, counter 1's to the dynamic memory refresh request
  and counter 2's to the loudspeaker gate. The counters are driven at
  1.193182 MHz, being the 14.31818 MHz reference oscillator divided by twelve,
  that oscillator running at four times the 3.579545 MHz NTSC colour subcarrier.
  The firmware leaves counter 0 running.
- **The keyboard controller**: the 8042 is decoded at port `0x60` for data and
  `0x64` for the status register when read and the command register when written;
  status bit 0 is set while the output buffer holds a byte for the processor and
  bit 1 while the input buffer still holds one for the controller. Status bit 5
  is set when the byte standing in the output buffer arrived from the second
  device port rather than the first.
- **The auxiliary device**: the mouse is attached to the controller's second
  device port and raises IR12, a line of the slave controller.
- **Scan code set 1**: a make code is the key's own code and the corresponding
  break code is that code with bit 7 set; a code prefixed by `0xE0` denotes one
  of the keys added after the original 84-key layout.
- **The serial adapter**: the first adapter is decoded at I/O base address
  `0x03F8` and the second at `0x02F8`; the first and third raise IRQ4 and the
  second and fourth IRQ3. The reference oscillator is 1.8432 MHz, which yields
  115200 baud at a divisor of one. The adapter's interrupt output reaches its
  request line through a buffer enabled by the auxiliary output OUT2, which the
  UART's own datasheet describes only as user-designated; an adapter whose OUT2
  is clear is therefore never heard by the interrupt controller.
- **The fixed disk adapter**: the first channel's command block is decoded at
  `0x01F0` with its control register at `0x03F6` and raises IRQ14; the second
  channel answers at `0x0170` and `0x0376` upon IRQ15.

Used by: `drivers/pic/pic.c`, `drivers/pit/pit.c`, `drivers/ps2/ps2.c`,
`drivers/keyboard/keyboard.c`, `drivers/mouse/mouse.c`,
`drivers/serial/serial.c`, `kernel/include/oxys/pic.h`,
`kernel/include/oxys/pit.h`, `kernel/include/oxys/ps2.h`,
`kernel/include/oxys/keyboard.h`, `kernel/include/oxys/mouse.h`,
`drivers/ata/`, `kernel/include/oxys/ata.h`, `kernel/kernel.c`.

### The 8042 controller and PS/2 device command sets
The command sets of the IBM Personal Computer AT keyboard controller and of the
devices attached to its two ports. Recorded in the IBM Personal Computer AT
technical reference and in the PS/2 hardware interface technical reference, and
reproduced consistently by every subsequent implementation.

Controller commands, written to port `0x64`:

- **0x20**, read the controller configuration byte; **0x60**, write it. The byte
  carries the first port's interrupt enable in bit 0, the second port's in bit 1,
  the first port's clock disable in bit 4, the second port's in bit 5, and the
  translation of scan code set 2 into set 1 in bit 6.
- **0xAD** and **0xAE**, disable and enable the first device port; **0xA7** and
  **0xA8**, disable and enable the second.
- **0xAA**, the controller self-test, answered by `0x55` upon success. It resets
  the controller upon some implementations, discarding the configuration byte,
  which must therefore be written again afterwards.
- **0xAB**, test the first device port, answered by `0x00` upon success;
  **0xA9**, test the second, answered the same way.
- **0xD4**, direct the byte written next to the data port to the second device
  port rather than the first.

There is no command reporting how many device ports the controller has. The
second port's existence is established by enabling it and reading the
configuration byte back: a controller that has one has started its clock, so
bit 5 is found clear, while a controller with one port ignores the command and
the bit stands as it was.

Device commands, written to port `0x60` and forwarded by the controller:

- **0xFF**, reset, answered by `0xFA` and then by `0xAA` where the device's own
  self-test passed, and then, upon an auxiliary device, by its identifier.
- **0xF4** and **0xF5**, enable and disable scanning or data reporting.
- **0xF6**, restore the default parameters.
- **0xF3**, set the sample rate, taking the rate as a second byte; **0xF2**, read
  the device identifier; **0xE8**, set the resolution, taking it as a second
  byte; **0xE6**, set scaling to one to one.
- The answers **0xFA**, acknowledged, and **0xFE**, send the command again.

Note that the two command sets use overlapping numbers for unrelated purposes:
`0xAA` is the controller's self-test command and also a device's report that its
own self-test passed. The port to which a byte is written is what distinguishes
them.

Used by: `drivers/ps2/ps2.c`, `kernel/include/oxys/ps2.h`,
`drivers/keyboard/keyboard.c`, `kernel/include/oxys/keyboard.h`,
`drivers/mouse/mouse.c`, `kernel/include/oxys/mouse.h`.

### The PS/2 auxiliary device movement packet
The format in which a mouse upon the controller's second port reports. Recorded
in the IBM Personal System/2 hardware interface technical reference and
reproduced consistently since.

A packet is three bytes:

- **Byte 0**: bits 0, 1 and 2 the left, right and middle buttons, set while held;
  bit 3 **always set**; bits 4 and 5 the signs of the horizontal and vertical
  movements; bits 6 and 7 their overflow indications.
- **Byte 1**: the magnitude of the horizontal movement.
- **Byte 2**: the magnitude of the vertical movement.

Each movement is therefore a **nine-bit** two's complement quantity, its low
eight bits in its own byte and its sign in byte 0: a magnitude of `0xFF` with the
sign bit set is −1, and a magnitude of `0x00` with the sign bit set is −256.
Vertical movement is positive **upward**, which is the opposite of a display's
sense. An overflow indication means the magnitude sent is the low bits of a
larger movement and is not the movement.

**The wheel extension.** A device given the sample rates 200, 100 and 80 in
succession and then asked for its identifier answers `0x03` if it has a wheel,
and sends four-byte packets thereafter; the fourth byte carries the wheel
movement in its low four bits as a two's complement quantity. A device that does
not recognise the sequence accepts three sample rates, continues to report itself
as identifier `0x00`, and continues to send three-byte packets. There is no
command that asks the question directly.

Used by: `drivers/mouse/mouse.c`, `kernel/include/oxys/mouse.h`.

### IBM Video Graphics Array technical reference
The colour text mode 3, presenting 80 columns by 25 rows, whose frame buffer
begins at physical address `0x000B8000` and whose cells comprise a code-point
byte followed by an attribute byte.

Registers relied upon, each named as the reference prints it:

- **Miscellaneous Output Register**, written at `0x03C2` and read at `0x03CC`.
  Bit 0: "If set Color Emulation. Base Address=3Dxh else Mono Emulation. Base
  Address=3Bxh". The CRT controller pair, the input status register and the frame
  buffer all move with it, to `0x03B4`, `0x03BA` and `0x000B0000` respectively.
- **CRT Controller Registers**, addressed through an index port and a data port.
  **Cursor Start Register** (index `0x0A`): bits 0 to 4, the first scan line of
  the cursor within the character cell; bit 5, which "Turns Cursor off if set".
  **Cursor End Register** (index `0x0B`): bits 0 to 4, the last scan line; bits 5
  and 6, the cursor skew, "Delay of cursor data in character clocks".
  **Cursor Location High** and **Low Registers** (indices `0x0E` and `0x0F`):
  the upper and lower eight bits of the cursor address.
- **Attribute Controller Registers**. "The address register is read and written
  via port 3C0h. The data register is written to port 3C0h and read from port
  3C1h"; an internal flip-flop selects between the two and is returned to the
  address by a read of the **Input Status #1 Register** (`0x03DA`, or `0x03BA` in
  the monochrome configuration), "the data received is not important". Bit 7 of
  the address register is the **Palette Address Source**, which "is set to 0 to
  load color values to the registers in the internal palette. It is set to 1 for
  normal operation". In the **Attribute Mode Control Register** (index `0x10`),
  bit 3 set makes "Attribute bit 7 ... blinking", clear makes it "high
  intensity", the latter yielding sixteen background colours rather than eight.

The reference itself is not distributed in a form that can be quoted by page, and
the register descriptions above were therefore taken from the FreeVGA reference
and from the VGA register summary of the same lineage, and cross-verified against
one another before being relied upon, as Section 6 of `PROJECT_GUIDELINES.md`
requires.

Used by: `drivers/vga/vga.c`, `kernel/include/oxys/vga.h`, `boot/boot.asm`.

### ANSI X3.4-1986, Coded Character Set — 7-Bit American National Standard Code for Information Interchange
American National Standards Institute. Republished, with the same repertoire of
control characters, as ISO/IEC 646.

The definitions of the control characters relied upon by the display driver and,
from sub-task 6.4, by the graphical console that must agree with it: **BS**
(`0x08`) moves the active position one character position backward; **HT**
(`0x09`) advances it to the next horizontal tabulation stop; **LF** (`0x0A`)
moves it one line down; **CR** (`0x0D`) moves it to the first character position
of the line. None of the four erases the character it moves over, which is why
erasure upon the display and upon a terminal alike is expressed as the sequence
`BS`, `SP`, `BS`.

Also the repertoire the bitmap font of sub-task 6.4 covers: the printable
characters `0x20` to `0x7E`, and the names given to them, which are the comments
beside the glyphs in `graphics/font.c`.

Used by: `drivers/vga/vga.c`, `kernel/include/oxys/vga.h`, `kernel/kernel.c`,
`graphics/font.c`, `graphics/console.c`, `kernel/include/oxys/font.h`,
`kernel/include/oxys/console.h`.

### ECMA-48, Control Functions for Coded Character Sets
European Computer Manufacturers Association, fifth edition.

The two control sequences by which a serial terminal is told of a cursor movement
that the backspace does not itself express: **CUU** — Cursor Up, `CSI Pn A`,
which moves the active position up by Pn lines without erasing; and **CHA** —
Cursor Character Absolute, `CSI Pn G`, which moves it to column Pn of the active
line, the columns being numbered from one. They are used only by the echo loop,
and only where the display driver has carried its own cursor into the row above.

Used by: `kernel/kernel.c`.

### PCI Local Bus Specification, revision 3.0
PCI Special Interest Group.

Sections relied upon:

- **Configuration Space Access Mechanism #1**: two 32-bit I/O locations,
  CONFIG_ADDRESS at `0x0CF8` and CONFIG_DATA at `0x0CFC`. Bit 31 of the former is
  the enable flag, bits 30 to 24 are reserved, bits 23 to 16 the bus number, bits
  15 to 11 the device number, bits 10 to 8 the function number and bits 7 to 2 the
  register number; the two least significant bits are always zero, every
  configuration access being aligned to a double word.
- **Configuration Space Header**: the first sixteen bytes common to every header
  type — the vendor and device identifiers at offsets `0x00` and `0x02`, the
  command and status registers at `0x04` and `0x06`, the revision, programming
  interface, subclass and class code at `0x08` to `0x0B`, and the header type at
  `0x0E`, whose bit 7 marks a multifunction device.
- **Absence**: "When a configuration access attempts to select a device that does
  not exist, the host bridge will complete the access without error, dropping all
  data on writes and returning all ones on reads."
- **PCI-to-PCI bridge header (type 1)**: the primary, secondary and subordinate
  bus numbers at offsets `0x18`, `0x19` and `0x1A`.
- **Base Address Registers**: bit 0 clear denotes memory space and set denotes
  I/O space; for memory, bits 2 and 1 give the width, the value 2 meaning a
  64-bit address whose upper half is the following register, and bit 3 marks the
  region prefetchable. The base address is the register with its low four bits
  cleared for memory and its low two bits cleared for I/O.

The specification is not distributed publicly by the PCI SIG. The field layouts
above were taken from two independent secondary renderings of it and
cross-verified against one another before being relied upon, as Section 6 of
`PROJECT_GUIDELINES.md` requires; they agree in every particular used here.

Used by: `drivers/pci/pci.c`, `kernel/include/oxys/pci.h`.

### PCI Code and ID Assignment Specification
PCI Special Interest Group.

The base class, subclass and programming interface codes: class `0x01` mass
storage, whose subclass `0x01` is an IDE controller, `0x05` an ATA controller,
`0x06` a serial ATA controller and `0x08` a non-volatile memory controller; class
`0x02` network; class `0x03` display; class `0x06` bridge, whose subclass `0x00`
is a host bridge, `0x01` an ISA bridge and `0x04` a PCI-to-PCI bridge; class
`0x08` base system peripheral, whose subclass `0x05` is an SD host controller;
class `0x0C` serial bus, whose subclass `0x03` is a USB controller and `0x05`
SMBus.

For an IDE controller, bit 0 of the programming interface denotes the primary
channel in native mode and bit 2 the secondary, each clear meaning the
compatibility mode that answers upon the legacy ports; bits 1 and 3 say whether
the respective mode may be changed. For a serial ATA controller, the programming
interface `0x01` denotes the AHCI register interface; for a non-volatile memory
controller, `0x02` denotes NVM Express.

That a subclass is meaningful only against its class is what the table shows
plainest: subclass `0x05` is an ATA controller, an SD host controller or an SMBus
controller according to the class above it.

Used by: `drivers/pci/pci.c`, `drivers/ata/channel.c`.

### AT Attachment with Packet Interface (ATA/ATAPI)
ANSI INCITS, Technical Committee T13. The revisions relied upon are ATA/ATAPI-6
and later, 48-bit addressing having been introduced in the sixth.

Sections relied upon:

- **Command block registers**: at offsets 0 to 7 from the base address, the data
  register, the error register (features when written), the sector count, the
  logical block address low, mid and high registers, the device register and the
  status register (the command register when written). The alternate status
  register (the device control register when written) lies in the separate
  control block.
- **Status register**: bit 7 BSY, bit 6 DRDY, bit 5 DF — a device fault, which
  does not set ERR — bit 3 DRQ, bit 0 ERR.
- **Device control register**: bit 1 nIEN, which stops the device asserting its
  interrupt; bit 2 SRST, which when set and then cleared resets both devices upon
  the channel.
- **Timing**: the status presented by a device is not valid for 400 nanoseconds
  after a command is written or a device selected. The delay is obtained by
  reading the alternate status register, which has no side effect; an input from
  an I/O port may be assumed to take at least 30 nanoseconds.
- **Command set**, cross-verified against an independent table of opcodes in
  opcode order: `20h` READ SECTOR(S), `24h` READ SECTOR(S) EXT, `30h` WRITE
  SECTOR(S), `34h` WRITE SECTOR(S) EXT, `E7h` FLUSH CACHE, `EAh` FLUSH CACHE EXT,
  `ECh` IDENTIFY DEVICE, `A1h` IDENTIFY PACKET DEVICE.
- **Identification data**: words 10 to 19 the serial number and 27 to 46 the
  model number, each word holding two characters with the first in its high half;
  words 60 and 61 the number of sectors addressable by 28 bits; bit 10 of word 83
  the support of 48-bit addressing; words 100 to 103 the number of sectors
  addressable by 48 bits.
- **Addressing**: a sector count register of zero means the greatest count the
  mode allows — 256 for the 28-bit commands and 65536 for the 48-bit ones. The
  four most significant bits of a 28-bit address lie in the device register. The
  48-bit commands write each address and count register twice, the high-order
  byte first, the device retaining the previous content in a hidden half.
- **Signatures**: a device that declines IDENTIFY DEVICE leaves `14h` and `EBh`
  in the address mid and high registers if it is a packet device, `3Ch` and `C3h`
  if it is a serial ATA device, and zeroes if it is an ATA device that aborted
  the command.

The standard is not distributed publicly by the committee. The register and
command details above were taken from two independent secondary renderings and
cross-verified against one another before being relied upon, as Section 6 of
`PROJECT_GUIDELINES.md` requires.

Used by: `drivers/ata/`, `kernel/include/oxys/ata.h`.

### Serial ATA Advanced Host Controller Interface Specification, revision 1.3.1
Intel Corporation, on behalf of the Serial ATA International Organization.

Sections relied upon:

- **Section 3.1, the generic host control registers**: `CAP` at `00h`, `GHC` at
  `04h`, `IS` at `08h`, `PI` at `0Ch`, `VS` at `10h`, `CAP2` at `24h` and `BOHC`
  at `28h`. `GHC.AE` is bit 31 and `GHC.HR` bit 0. `PI` is a **bitmap** of the
  ports that exist and not a count of them; the implemented ports need not be
  consecutive.
- **Section 3.3, the port registers**: the block for port *x* begins at
  `100h + x * 80h`, and within it `PxCLB` at `00h`, `PxCLBU` at `04h`, `PxFB` at
  `08h`, `PxFBU` at `0Ch`, `PxIS` at `10h`, `PxIE` at `14h`, `PxCMD` at `18h`,
  `PxTFD` at `20h`, `PxSIG` at `24h`, `PxSSTS` at `28h`, `PxSCTL` at `2Ch`,
  `PxSERR` at `30h`, `PxSACT` at `34h` and `PxCI` at `38h`. `PxCMD.ST` is bit 0,
  `PxCMD.FRE` bit 4, `PxCMD.FR` bit 14 and `PxCMD.CR` bit 15; `PxIS.TFES` is bit
  30. `PxTFD` carries the device's status byte in its low eight bits, with BSY at
  bit 7 and DRQ at bit 3.
- **Section 3.3.8, `PxSSTS`**: `DET` in bits 3:0 is 3 when a device is present
  and communication is established; `IPM` in bits 11:8 is 1 when the interface is
  active. The negotiated speed lies between them, in bits 7:4.
- **Section 4.2.1, the command header**: thirty-two bytes. The first double word
  holds the command FIS length in **double words** in bits 4:0, the ATAPI bit at
  5, the write bit at 6 and the region descriptor table length in bits 31:16; the
  second holds the byte count transferred; the third and fourth the physical
  address of the command table.
- **Section 4.2.2, the alignment of the structures**: the command list is 1024
  bytes upon a 1024-byte boundary, the received FIS structure 256 bytes upon a
  256-byte boundary, and a command table lies upon a 128-byte boundary.
- **Section 4.2.3, the command table**: the command FIS at offset 0, the ATAPI
  command at `40h`, and the region descriptors from `80h`. Each descriptor is
  sixteen bytes: a 64-bit data base address whose low bit is reserved, a reserved
  double word, and a byte count **less one** in bits 21:0 with an interrupt bit
  at 31.
- **The port signatures**: `00000101h` a serial ATA disk, `EB140101h` a packet
  device, `C33C0101h` an enclosure services device and `96690101h` a port
  multiplier.
- **The BIOS/OS handoff**: `CAP2.BOH`, bit 0, states that it is implemented;
  `BOHC.BOS` bit 0 that the firmware owns the adaptor, `BOHC.OOS` bit 1 the
  request for it, and `BOHC.BB` bit 4 that the firmware is still busy with it.

The Register Host to Device frame information structure this driver composes is
defined by **Serial ATA revision 3.0** rather than by AHCI: type `27h`, the C bit
at bit 7 of byte 1, the command at byte 2, the low three address bytes at 4 to 6,
the device register at 7, the high three at 8 to 10, and the sector count at 12
and 13.

Used by: `drivers/ahci/ahci.c`, `kernel/include/oxys/ahci.h`.

### SD Host Controller Simplified Specification, version 4.20
SD Association.

Sections relied upon:

- **Table 2-1, the register map**: the block size at `004h` and the block count
  at `006h`, the argument at `008h`, the transfer mode at `00Ch` and the command
  at `00Eh`, the four response registers from `010h`, the buffer data port at
  `020h`, the present state at `024h`, host control 1 at `028h`, power control at
  `029h`, clock control at `02Ch`, timeout control at `02Eh`, software reset at
  `02Fh`, the normal interrupt status at `030h` and the error interrupt status at
  `032h`, their status enables at `034h` and `036h` and their signal enables at
  `038h` and `03Ah`, the capabilities at `040h`, and the host controller version
  at `0FEh`.
- **Table 2-10, the command register**: the command index in bits 13:8, data
  present select at bit 5, command index check enable at bit 4, command CRC check
  enable at bit 3, and the response type select in bits 1:0 — 00 no response, 01
  a response of 136 bits, 10 one of 48 bits, and 11 one of 48 bits after which
  busy is checked.
- **Tables 2-15 and 2-16, the present state register**: command inhibit (CMD) at
  bit 0 and command inhibit (DAT) at bit 1, buffer write enable at bit 10 and
  buffer read enable at bit 11, card inserted at bit 16 and card state stable at
  bit 17.
- **The clock control register**: internal clock enable at bit 0, internal clock
  stable at bit 1, SD clock enable at bit 2, and the frequency divider in bits
  15:8, which holds *half* of the divisor.
- **The power control register**: SD bus power at bit 0 and the bus voltage in
  bits 3:1, the value 7 selecting 3.3 volts. **The software reset register**:
  reset for all at bit 0, for the command line at bit 1 and for the data line at
  bit 2.
- **The normal interrupt status register**: command complete at bit 0, transfer
  complete at bit 1, buffer write ready at bit 4, buffer read ready at bit 5, and
  the error interrupt at bit 15.
- **Section 2.2.7**: a response of 136 bits is presented in the four response
  registers with its low eight bits — the CRC and the end bit — removed, so that
  bit N of the card specific data appears at bit N - 8 of the response.

Used by: `drivers/sdhci/sdhci.c`, `kernel/include/oxys/sdhci.h`.

### SD Physical Layer Simplified Specification, version 8.00
SD Association.

Sections relied upon:

- **The commands**: CMD0 GO_IDLE_STATE, CMD2 ALL_SEND_CID, CMD3
  SEND_RELATIVE_ADDR, CMD7 SELECT/DESELECT_CARD, CMD8 SEND_IF_COND, CMD9
  SEND_CSD, CMD16 SET_BLOCKLEN, CMD17 READ_SINGLE_BLOCK, CMD24 WRITE_BLOCK,
  CMD55 APP_CMD and ACMD41 SD_SEND_OP_COND.
- **The operating conditions register**: bit 31 is set when the card has finished
  its power-up sequence, and bit 30 — the card capacity status — is set when the
  card is block-addressed and clear when it is byte-addressed.
- **The card specific data**: CSD_STRUCTURE in bits 127:126. For version 1,
  READ_BL_LEN in bits 83:80, C_SIZE in 73:62 and C_SIZE_MULT in 49:47, the
  capacity in bytes being (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) * 2^READ_BL_LEN. For
  version 2, C_SIZE in bits 69:48 alone, the capacity being (C_SIZE + 1) * 512
  kibibytes.
- **CMD8**: a card of the second version will not complete its power-up sequence
  unless it has been sent, and answers with the supply voltage and check pattern
  of the argument unchanged. A card of the first version does not answer it.

Used by: `drivers/sdhci/sdhci.c`.

### JEDEC Standard JESD84-B51, the Embedded Multimedia Card
JEDEC Solid State Technology Association.

Relied upon for the two respects in which an embedded card differs from an SD
card in the sequence above: CMD1 SEND_OP_COND takes the place of ACMD41, which an
embedded card does not implement at all; and CMD3 assigns a relative address the
host chooses rather than reporting one the card chose.

Used by: `drivers/sdhci/sdhci.c`.
### The Second Extended File System: Internal Layout
Poirier, D. `https://www.nongnu.org/ext2-doc/ext2.html`

Sections relied upon:

- **The Superblock**: it lies 1024 bytes from the start of the volume and
  occupies 1024 bytes, the first kibibyte being reserved for a boot sector; the
  field table giving the offset and width of every field, of which this kernel
  reads all those up to `s_last_mounted` at offset 136; `s_magic` at offset 56
  holding `0xEF53`; the block size being `1024 << s_log_block_size`;
  `s_first_data_block` at offset 20 being 1 upon a volume of 1024-byte blocks and
  0 upon any other.
- **Revision Levels**: revision 0 (`EXT2_GOOD_OLD_REV`) has no field for the
  inode size or the first usable inode, both being fixed; revision 1
  (`EXT2_DYNAMIC_REV`) states them at offsets 88 and 84 and adds the three
  feature fields at 92, 96 and 100.
- **Reserved Inodes**: inodes 1 to 10 are reserved, so the first inode available
  to a file upon a revision 0 volume is 11.
- **The feature fields**: a volume declaring an incompatible feature the
  implementation lacks may not be read; one declaring a read-only compatible
  feature it lacks may be read and not written; a compatible feature may be
  ignored entirely. The bits: incompatible — compression `0x0001`, file type in
  directory entries `0x0002`, journal recovery `0x0004`, journal device `0x0008`,
  meta block groups `0x0010`; read-only compatible — sparse superblocks `0x0001`,
  large files `0x0002`, binary tree directories `0x0004`.
- **`s_state`**: 1 denotes a cleanly unmounted volume and 2 one upon which errors
  were detected.
- **The Block Group Descriptor Table**: the table begins upon the first block
  following the superblock — the third block of a 1 KiB volume, the second of any
  larger — and holds one descriptor per group; a shadow copy accompanies every
  backup superblock. Table 3.12 gives the descriptor: `bg_block_bitmap` at 0,
  `bg_inode_bitmap` at 4 and `bg_inode_table` at 8, each a word;
  `bg_free_blocks_count` at 12, `bg_free_inodes_count` at 14 and
  `bg_used_dirs_count` at 16, each a half; `bg_pad` at 18 and `bg_reserved` at
  20, 32 bytes in all. Every block identifier within a descriptor is absolute.
- **Inode Table**: there is one inode table per group, located by
  `bg_inode_table`, holding `s_inodes_per_group` inodes, so its length follows
  from the inode size.
- **The Inode Structure**, Table 3.13: `i_mode` at 0, `i_uid` at 2, `i_size` at
  4, the four times at 8, 12, 16 and 20, `i_gid` at 24, `i_links_count` at 26,
  `i_blocks` at 28, `i_flags` at 32, `i_osd1` at 36, `i_block` at 40 as fifteen
  words, `i_generation` at 100, `i_file_acl` at 104, `i_dir_acl` at 108,
  `i_faddr` at 112 and `i_osd2` at 116 — 128 bytes in all. `i_blocks` counts
  512-byte sectors and not filesystem blocks.
- **`i_block`**: the first twelve entries are direct, the thirteenth indirect,
  the fourteenth doubly indirect and the fifteenth triply indirect; a zero
  denotes a block that is not allocated, a sparse file being the reason the
  original terminating interpretation was abandoned.
- **`i_size`**: upon revision 1, and for regular files only, the high 32 bits of
  the size are held in `i_dir_acl`.
- **Locating an Inode**: `block group = (inode - 1) / s_inodes_per_group` and
  `local inode index = (inode - 1) % s_inodes_per_group`. The worked values of
  Table 3.20 were used to check the arithmetic.
- **Defined Reserved Inodes**, Table 3.14: inode 1 is the bad-blocks inode and
  inode 2 the root directory; inodes 1 to 10 are reserved.
- **Defined `i_mode` Values**, Table 3.15: the file format occupies the high four
  bits — socket `0xC000`, symbolic link `0xA000`, regular file `0x8000`, block
  device `0x6000`, directory `0x4000`, character device `0x2000`, FIFO `0x1000` —
  and the low twelve hold the set-user, set-group and sticky bits and the nine
  permission bits.
- **Chapter 4, Directory Structure**: a directory is a file, identified by
  `EXT2_S_IFDIR` in `i_mode`, whose data is a linked list of entries; inode 2
  holds the root. Table 4.1 gives the record: `inode` at offset 0 as a word,
  `rec_len` at 4 and `name_len` at 6 as halves, `file_type` at 7 as a byte, and
  the name from 8, unterminated and no longer than 255 bytes. An `inode` of zero
  marks a record that is not in use. `rec_len` is at least the length of its own
  record, entries are aligned upon four bytes, and no entry may span two data
  blocks; where a name is removed, the record before it has its `rec_len`
  lengthened to cover it, and where the first record of a block is removed a
  blank record is left in its place. `name_len` may never exceed `rec_len - 8`.
  Revision 0 held a sixteen-bit `name_len`, of which the upper byte was
  reclaimed as `file_type`; the value of `file_type` must match the format of
  the inode the entry names.
- **Table 4.2, Defined Inode File Type Values**: `EXT2_FT_UNKNOWN` 0,
  `EXT2_FT_REG_FILE` 1, `EXT2_FT_DIR` 2, `EXT2_FT_CHRDEV` 3, `EXT2_FT_BLKDEV` 4,
  `EXT2_FT_FIFO` 5, `EXT2_FT_SOCK` 6, `EXT2_FT_SYMLINK` 7. The numbering is
  unrelated to that of the `i_mode` formats in Table 3.15.
- **Table 4.3, Sample Linked Directory Data Layout**: the worked layout of one
  directory block, against which the entries the boot-time self-test composes
  were checked, including its final record — inode 0, with a record length
  running to the end of the block.
- **Indexed Directory Format**: the hash index is made backward compatible by
  disguising its interior nodes as records that are not in use, so a linear
  traversal reads an indexed directory correctly.
- **Block Bitmap** and **Inode Bitmap**: each bit represents the state of one
  block or inode of the group, 1 meaning used and 0 free; "the first block of
  this block group is represented by bit 0 of byte 0, the second by bit 1 of
  byte 0. The 8th block is represented by bit 7 (most significant bit) of byte 0
  while the 9th block is represented by bit 0 (least significant bit) of byte 1".
  The inode bitmap works the same way, and since inode numbers begin at one, the
  first bit of the first group's inode bitmap represents inode 1. When the inode
  table is created every reserved inode is marked as used.
- **Symbolic Links**: a symbolic link is a file holding a text string
  interpreted as a path to another file, absolute or relative, which affects
  pathname resolution; it is a file in its own right and exists independently of
  its target, which it does not oblige to exist. Symbolic links do not affect an
  inode's link count. "For all symlink shorter than 60 bytes long, the data is
  stored within the inode itself; it uses the fields which would normally be
  used to store the pointers to data blocks" — sixty being the fifteen pointers
  of four bytes, and the optimisation being worth having because most links are
  shorter than that.
- **Byte order**: every quantity upon the volume is stored least significant byte
  first, irrespective of the machine.

Used by: `kernel/fs/ext2/`, `kernel/include/oxys/ext2.h`.

### Linux kernel documentation, the ext2 filesystem
Linux kernel source, `Documentation/filesystems/ext2.rst`.
`https://www.kernel.org/doc/html/latest/filesystems/ext2.html`

Sections relied upon: **Directories** — "a directory is a filesystem object and
has an inode just like a file. It is a specially formatted file containing
records which associate each name with an inode number"; the current
implementation "uses a singly-linked list to store the filenames in the
directory"; later revisions "encode the type of the object (file, directory,
symlink, device, fifo, socket) to avoid the need to check the inode itself"; and,
decisively, "FILETYPE is an INCOMPAT flag because older kernels would think a
filename was longer than 256 characters". That last sentence is the reason this
kernel decides the width of `name_len` from the feature flag alone and not from
the revision; see [`../storage/EXT2-FILES.md`](../storage/EXT2-FILES.md), Section 1.2.

Used by: `kernel/fs/ext2/`, `kernel/include/oxys/ext2.h`.

### Linux kernel documentation, the ext4 superblock, group descriptor and inode
Linux kernel source, `Documentation/filesystems/ext4/super.rst`,
`group_descr.rst` and `inodes.rst`.

Consulted as an independent statement of the superblock, block group descriptor
and inode field offsets, the two formats sharing the layout of every field this
kernel reads. It confirms
`s_magic` at `0x38`, `s_first_ino` at `0x54`, `s_inode_size` at `0x58`,
`s_block_group_nr` at `0x5A`, the three feature fields at `0x5C`, `0x60` and
`0x64`, `s_uuid` at `0x68`, `s_volume_name` at `0x78` and `s_last_mounted` at
`0x88`, and the magic value `0xEF53`. For the descriptor it confirms
`bg_block_bitmap_lo` at `0x0`, `bg_inode_bitmap_lo` at `0x4`,
`bg_inode_table_lo` at `0x8`, `bg_free_blocks_count_lo` at `0xC`,
`bg_free_inodes_count_lo` at `0xE` and `bg_used_dirs_count_lo` at `0x10`. For the
inode it confirms `i_mode` at `0x0`, `i_uid` at `0x2`, `i_size_lo` at `0x4`,
`i_dtime` at `0x14`, `i_links_count` at `0x1A`, `i_blocks_lo` at `0x1C`,
`i_flags` at `0x20`, `i_block[15]` at `0x28` occupying 60 bytes, and
`i_generation` at `0x64`.

The two formats diverge at descriptor offset 18, which EXT2 reserves as `bg_pad`
and ext4 reuses as `bg_flags`. This kernel reads it in neither sense, so the
divergence does not bear upon it; it is recorded because a reader comparing the
two tables will meet it.

Used by: `kernel/fs/ext2/`, `kernel/include/oxys/ext2.h`.

### IEEE Std 1003.1-2017, the Portable Operating System Interface
The Open Group and IEEE. Technical Standard Base Specifications, Issue 7,
2018 edition.
`https://pubs.opengroup.org/onlinepubs/9699919799/`

Sections relied upon: **Section 4.13, Pathname Resolution** — a pathname
beginning with a slash is resolved from the root directory; "multiple successive
slashes are considered to be the same as one slash"; each component that is not
the last "shall be resolved to a directory"; a symbolic link met in the pathname
"shall be replaced by the contents of the symbolic link"; and an implementation
shall support at least `SYMLOOP_MAX` links in one resolution, which the standard
sets at a minimum of 8. **The System Interfaces** volume, `open()`, `read()`,
`write()`, `lseek()`, `link()`, `unlink()`, `rmdir()`, `mkdir()`, `readlink()`
and `stat()`: the shape and the outcomes of the operations, so that the system
calls of Phase 6 map upon this layer without an intervening translation. From
`lseek()` in particular: the file offset "may be set beyond the end of the
existing data in the file. If data is later written at this point, subsequent
reads of data in the gap shall return bytes with the value 0 until data is
actually written into the gap", which is the hole this kernel already writes.
And from `open()`: `O_APPEND` places each write at the end of the file "prior to
each write", which is why an appending write here takes the size and not the
position.

Used by: `kernel/fs/vfs/`, `kernel/include/oxys/vfs.h`.

### The UNIX Time-Sharing System
Ritchie, D. M., and Thompson, K. Communications of the ACM, volume 17, number 7,
July 1974, pages 365 to 375.
`https://dsf.berkeley.edu/cs262/unix.pdf`

Section relied upon: **3.4, Removable file systems** — the mount, which "causes
references to the [mount point] to refer instead to the root directory" of the
mounted volume, so that "there is virtually no distinction between references to
files on a removable volume and files on the permanent file system". Consulted
for the concept and for the statement that the substitution is of a directory by
a root, which is the design of Section 5 of
[`../storage/VFS.md`](../storage/VFS.md); the implementation is original.

Used by: `kernel/fs/vfs/`.

### GNU GRUB Manual
Free Software Foundation.
`https://www.gnu.org/software/grub/manual/grub/grub.html`

Sections relied upon: Section 3.4, `grub-mkrescue`; Section 6, the configuration
file; Section 16.3.16, the `multiboot2` command.

Used by: `boot/grub/grub.cfg`, `Makefile`.

## 2. To be consulted in later phases

| Specification | Phase | Subject |
| ------------- | ----- | ------- |
| PCI Local Bus Specification 3.0 | 4 | Configuration space and device enumeration. |
| ATA/ATAPI Command Set (ACS-3) | 4 | `IDENTIFY DEVICE`; 28-bit and 48-bit logical block addressing. |
| Serial ATA AHCI 1.3.1 | 4 | The host bus adaptor: the generic host control and port registers, the command list, and the region descriptors by which a caller's pages are named to the device. |
| Executable and Linking Format 1.2, with ELF-64 1.5 draft 2 | 6 | The file header, the program header table, and what a statically linked executable is as against a position-independent one. |
| SD Host Controller Simplified Specification 4.20 | 4 | The host controller registers, the command register, and the presentation of a response. |
| SD Physical Layer Simplified Specification 8.00 | 4 | The card's own command set, the operating conditions register, and the two encodings of a capacity. |
| JEDEC JESD84-B51 | 4 | The two respects in which an embedded MultiMediaCard differs from an SD card in the identification sequence. |
| IEEE Std 1003.1-2017, System Interfaces | 6, 7 | `fork()`, `exec()`, `wait()` and the file-descriptor semantics a fork imposes upon the open file table. |
| Intel MultiProcessor Specification 1.4 | 6 | Application processor bring-up. |
| VESA BIOS Extensions 3.0 | 6 | Linear framebuffer modes under legacy BIOS, should this kernel ever need to set one for itself rather than accept what the boot loader chose. |
| UEFI Specification 2.10, Section 12.9 | 9, 12 | The Graphics Output Protocol. |
| FIPS 180-4 | 10 | SHA-256. |
| FIPS 197 | 10 | The Advanced Encryption Standard. |
| NIST SP 800-38A | 10 | Modes of operation. |
| NIST SP 800-90A | 10 | Deterministic random bit generators. |
| IEEE 802.3 | 11 | The Ethernet frame format. |
| RFC 826 | 11 | The Address Resolution Protocol. |
| RFC 791 | 11 | The Internet Protocol, version 4. |
| RFC 792 | 11 | The Internet Control Message Protocol. |
| RFC 768 | 11 | The User Datagram Protocol. |
| RFC 9293 | 11 | The Transmission Control Protocol. |
| RFC 2131 | 11 | The Dynamic Host Configuration Protocol. |
| Realtek RTL8139 or Intel 8254x datasheet | 11 | The Ethernet controller. |
| UEFI Specification 2.10 | 12 | The System Table, Boot Services and Runtime Services. |
| Microsoft PE/COFF Specification | 12 | The PE32+ image format. |

## 3. Citation form

A citation in a code comment or a design document names the specification, the
section, and, where applicable, the table or figure. For example:

> Refer to Intel SDM, Volume 3A, Section 4.1.2, for the control-register sequence
> required to enter IA-32e mode.

A statement of hardware or protocol behaviour that carries no citation is to be
treated as unverified.
