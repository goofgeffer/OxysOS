/*
 * File: linker.ld
 * Purpose: Link script for the Oxys-OS kernel, producing an ELF64 image that is
 *          loaded by GRUB at physical address 0x00100000 but executes from the
 *          higher half of the canonical 64-bit virtual address space.
 * Key symbols: _start, KernelVirtualBase, KernelPhysicalStart, KernelPhysicalEnd,
 *              KernelBssStart, KernelBssEnd.
 * References:
 *   - Multiboot2 Specification 2.0, Section 3.1.1 ("The layout of Multiboot2
 *     header"): the header must appear within the first 32768 bytes of the image
 *     and must be aligned on an 8-byte boundary. It is therefore placed first.
 *   - Multiboot2 Specification 2.0, Section 3.4 ("Machine state"): the boot
 *     loader enters the image in 32-bit protected mode with paging disabled,
 *     hence the boot sections must be linked at their physical addresses.
 *   - System V Application Binary Interface, AMD64 Architecture Processor
 *     Supplement: ELF64 object file format.
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

SECTIONS
{
    . = KernelPhysicalBase;
    KernelPhysicalStart = .;

    /*
     * The boot section is linked and loaded at its physical address because it
     * executes before paging is enabled. It contains the Multiboot2 header, the
     * 32-bit entry code, the long-mode trampoline, the boot GDT and the
     * boot-time paging structures.
     */
    .boot ALIGN(4K) :
    {
        KEEP(*(.multiboot_header))
        *(.boot.text)
        . = ALIGN(4K);
        *(.boot.data)
    }

    . = ALIGN(4K);

    /*
     * All subsequent sections execute from the higher half. Their load memory
     * addresses (LMA) continue contiguously from the boot section, while their
     * virtual memory addresses (VMA) are displaced by KernelVirtualBase.
     */
    . += KernelVirtualBase;

    .text ALIGN(4K) : AT(ADDR(.text) - KernelVirtualBase)
    {
        KernelTextStart = .;
        *(.text .text.*)
        KernelTextEnd = .;
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KernelVirtualBase)
    {
        KernelRodataStart = .;
        *(.rodata .rodata.*)
        KernelRodataEnd = .;
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KernelVirtualBase)
    {
        KernelDataStart = .;
        *(.data .data.*)
        KernelDataEnd = .;
    }

    /*
     * The .bss section is of type NOBITS and occupies no space in the file. The
     * boot loader is required to zero the difference between the memory size and
     * the file size of the containing program header. It is placed last so that
     * no PROGBITS section follows it in the load order.
     */
    .bss ALIGN(4K) : AT(ADDR(.bss) - KernelVirtualBase)
    {
        KernelBssStart = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4K);
        KernelBssEnd = .;
    }

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
