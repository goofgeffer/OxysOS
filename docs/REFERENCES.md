# Oxys-OS Bibliography

**Authority**: `CLAUDE.md`, Section 6, requires that every design decision be
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

Used by: `boot/boot.asm`, `kernel/kernel.c`, `linker.ld`, `boot/grub/grub.cfg`.

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
- **Volume 3A, Section 4.6**, access rights: the permissions of a translation are
  the conjunction of those held at every level of the hierarchy, which is why a
  restriction must be applied at the leaf entry and not at an intermediate one.
- **Volume 3A, Section 4.10.4.1**, invalidation: writing CR3 invalidates every
  translation-lookaside-buffer entry associated with the current process context,
  save those for global pages.
- **Volume 3A, Section 13.1**, the enabling and state management required of the
  SSE and x87 units.

Used by: `boot/boot.asm`, `linker.ld`, `Makefile`, `kernel/include/oxys/io.h`,
`docs/MEMORY-LAYOUT.md`.

### System V Application Binary Interface, AMD64 Architecture Processor Supplement
`https://gitlab.com/x86-psABIs/x86-64-ABI`

Sections relied upon:

- **Section 3.1.2**, data representation: the LP64 model.
- **Section 3.2.2**, the stack frame: the 128-byte red zone below the stack
  pointer, which is inadmissible in kernel code.
- **Section 3.2.3**, parameter passing: the first two integer arguments are
  passed in `RDI` and `RSI`.
- The ELF64 object file format.

Used by: `boot/boot.asm`, `linker.ld`, `Makefile`.

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

Sections relied upon: the register map at offsets 0 to 7 from the base address;
the divisor latch access bit, being bit 7 of the line control register; the
transmitter holding register empty flag, being bit 5 of the line status
register; the loopback bit, being bit 4 of the modem control register.

Used by: `drivers/serial/serial.c`.

### IBM Video Graphics Array technical reference
The colour text mode 3, presenting 80 columns by 25 rows, whose frame buffer
begins at physical address `0x000B8000` and whose cells comprise a code-point
byte followed by an attribute byte; and the CRT controller cursor location
registers `0x0E` and `0x0F`, addressed through the index port `0x03D4` and the
data port `0x03D5`.

Used by: `drivers/vga/vga.c`, `boot/boot.asm`.

### GNU GRUB Manual
Free Software Foundation.
`https://www.gnu.org/software/grub/manual/grub/grub.html`

Sections relied upon: Section 3.4, `grub-mkrescue`; Section 6, the configuration
file; Section 16.3.16, the `multiboot2` command.

Used by: `boot/grub/grub.cfg`, `Makefile`.

## 2. To be consulted in later phases

| Specification | Phase | Subject |
| ------------- | ----- | ------- |
| Intel SDM, Volume 3A, Chapter 6 | 3 | Interrupt and exception handling; the 64-bit interrupt gate. |
| Intel 8259A datasheet | 3 | The programmable interrupt controller and its remapping. |
| IBM PS/2 technical reference | 3 | The keyboard controller and scancode set 1. |
| PCI Local Bus Specification 3.0 | 4 | Configuration space and device enumeration. |
| ATA/ATAPI Command Set (ACS-3) | 4 | `IDENTIFY DEVICE`; 28-bit and 48-bit logical block addressing. |
| The Second Extended File System (Poirier) | 5 | Superblock, group descriptors, inodes and directories. |
| Linux kernel documentation, `filesystems/ext2.rst` | 5 | The on-disk format as presently implemented. |
| Intel SDM, Volume 2B, `SYSCALL` and `SYSRET` | 6 | The fast system-call mechanism. |
| Intel MultiProcessor Specification 1.4 | 6 | Application processor bring-up. |
| ACPI Specification 6.5 | 6, 12 | The Multiple APIC Description Table; the Root System Description Pointer. |
| VESA BIOS Extensions 3.0 | 9 | Linear framebuffer modes under legacy BIOS. |
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
