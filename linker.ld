/*
 * File: linker.ld
 * Purpose: Link script for the Oxys-OS kernel, producing an ELF64 image that is
 *          loaded by GRUB at physical address 0x00100000 but executes from the
 *          higher half of the canonical 64-bit virtual address space.
 * Key symbols: _start, KernelVirtualBase, KernelPhysicalStart, KernelPhysicalEnd,
 *              KernelBssStart, KernelBssEnd.
 * Key segments: boot and text are readable and executable; bootdata, data and
 *              rodata are not executable, and rodata is not writable either. No
 *              segment is both writable and executable.
 * References:
 *   - Multiboot2 Specification 2.0, Section 3.1.1 ("The layout of Multiboot2
 *     header"): the header must appear within the first 32768 bytes of the image
 *     and must be aligned on an 8-byte boundary. It is therefore placed first.
 *   - Multiboot2 Specification 2.0, Section 3.4 ("Machine state"): the boot
 *     loader enters the image in 32-bit protected mode with paging disabled,
 *     hence the boot sections must be linked at their physical addresses.
 *   - System V Application Binary Interface, AMD64 Architecture Processor
 *     Supplement: ELF64 object file format; the program header, whose p_flags
 *     field holds PF_R (4), PF_W (2) and PF_X (1), and whose p_align obliges
 *     p_offset and p_vaddr to be congruent modulo that value.
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.5: the higher-half base 0xFFFFFFFF80000000 is a canonical
 *     address in the 48-bit linear address space and lies within the topmost
 *     2 GiB, which permits the use of the GCC "kernel" code model.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/*
 * The virtual base of the higher-half kernel. The topmost 2 GiB of the address
 * space is selected so that the compiler may address all kernel symbols with
 * 32-bit sign-extended displacements (-mcmodel=kernel).
 */
KernelVirtualBase = 0xFFFFFFFF80000000;

/*
 * The physical address at which GRUB places the image. One mebibyte is the
 * conventional lowest address free of legacy device and firmware reservations.
 */
KernelPhysicalBase = 0x00100000;

/*
 * The program headers.
 *
 * A segment carries one set of permissions, so a section may only share a
 * segment with sections of the same permissions. Left to itself the linker
 * places every section it can into as few segments as possible and gives each
 * the union of the permissions of what it holds, which is how a kernel acquires
 * a segment that is readable, writable and executable at once. Declaring the
 * segments here states the division instead of inferring it.
 *
 * The flags are those of the ELF64 program header: 4 is read, 2 is write and 1
 * is execute. Nothing here is both writable and executable.
 */
PHDRS
{
    boot     PT_LOAD FLAGS(5); /* r-x: the Multiboot2 header and the entry code. */
    bootdata PT_LOAD FLAGS(6); /* rw-: the boot GDT and the boot paging structures. */
    text     PT_LOAD FLAGS(5); /* r-x: the kernel's code. */
    rodata   PT_LOAD FLAGS(4); /* r--: constants and read-only tables. */
    data     PT_LOAD FLAGS(6); /* rw-: initialised data, then .bss. */
}

/*
 * Every output section below carries ALIGN(4K) twice, and the two mean different
 * things. The first, before the colon, sets the address at which the section
 * begins. The second, after it, sets the section's own alignment, which the
 * linker takes as the alignment of the segment holding it; without it the
 * segment would inherit the largest alignment among the input sections — 16 or
 * 32 bytes — and the file offset of the segment would no longer be congruent to
 * its address modulo a page. That congruence is what allows a loader to map the
 * file rather than copy it, and so to apply the permissions declared above.
 */
SECTIONS
{
    . = KernelPhysicalBase;
    KernelPhysicalStart = .;

    /*
     * The boot sections are linked and loaded at their physical addresses
     * because they execute before paging is enabled. The code and the data are
     * separate output sections rather than one, so that each may be given a
     * segment of its own: the entry code is executed and never written, and the
     * boot GDT and the boot-time paging structures are written and never
     * executed.
     */
    .boot ALIGN(4K) : ALIGN(4K)
    {
        KEEP(*(.multiboot_header))
        *(.boot.text)
    } :boot

    .boot.data ALIGN(4K) : ALIGN(4K)
    {
        *(.boot.data)
    } :bootdata

    . = ALIGN(4K);

    /*
     * All subsequent sections execute from the higher half. Their load memory
     * addresses (LMA) continue contiguously from the boot section, while their
     * virtual memory addresses (VMA) are displaced by KernelVirtualBase.
     */
    . += KernelVirtualBase;

    .text ALIGN(4K) : AT(ADDR(.text) - KernelVirtualBase) ALIGN(4K)
    {
        KernelTextStart = .;
        *(.text .text.*)
        KernelTextEnd = .;
    } :text

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KernelVirtualBase) ALIGN(4K)
    {
        KernelRodataStart = .;
        *(.rodata .rodata.*)
        KernelRodataEnd = .;
    } :rodata

    .data ALIGN(4K) : AT(ADDR(.data) - KernelVirtualBase) ALIGN(4K)
    {
        KernelDataStart = .;
        *(.data .data.*)
        KernelDataEnd = .;
    } :data

    /*
     * The .bss section is of type NOBITS and occupies no space in the file. The
     * boot loader is required to zero the difference between the memory size and
     * the file size of the containing program header. It is placed last so that
     * no PROGBITS section follows it in the load order.
     */
    .bss ALIGN(4K) : AT(ADDR(.bss) - KernelVirtualBase) ALIGN(4K)
    {
        KernelBssStart = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4K);
        KernelBssEnd = .;
    } :data

    KernelVirtualEnd = .;
    KernelPhysicalEnd = . - KernelVirtualBase;

    /*
     * Sections emitted by the toolchain that carry no meaning for a freestanding
     * kernel image are discarded. The .eh_frame and .note.GNU-stack sections in
     * particular would otherwise enlarge the image without purpose.
     */
    /DISCARD/ :
    {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
    }
}
