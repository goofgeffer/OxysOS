/*
 * File: kernel/cpu/gdt.c
 * Purpose: Defines the kernel global descriptor table and installs it, replacing
 *          the table established in boot/boot.asm which resided at a low address
 *          no longer mapped.
 * Key functions: GdtInitialise, GdtBase, GdtLimit, GdtTableAddress, GdtReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 3.4.5 and Figure 3-8: the segment descriptor format.
 *   - Intel SDM, Volume 3A, Section 3.5.1: the GDTR, and the limit being one less
 *     than the size of the table.
 *   - Intel SDM, Volume 3A, Section 3.4.2: the processor sets the accessed bit of
 *     a descriptor when its selector is loaded.
 */

#include <oxys/gdt.h>
#include <oxys/kernel.h>

/*
 * The descriptors, reproducing those of boot/boot.asm.
 *
 * In 64-bit mode the base and limit of a code or data descriptor are ignored;
 * the significant fields are the present bit, the descriptor type, the privilege
 * level, the L flag of the code descriptor, and the writable bit of the data
 * descriptor. The values are retained in full so that the table remains valid
 * should compatibility mode ever be entered.
 *
 * The table is deliberately not const. Intel SDM, Volume 3A, Section 3.4.2,
 * provides that the processor sets the accessed bit of a descriptor when its
 * selector is loaded, and the descriptors below have that bit clear. Placing the
 * table in read-only memory would turn the first segment load into a page fault.
 */
static uint64_t GdtTable[GDT_ENTRY_COUNT] __attribute__((aligned(16))) = {
    /* 0x00: the mandatory null descriptor. */
    UINT64_C(0x0000000000000000),
    /* 0x08: 64-bit code, privilege level 0, L set, D clear. */
    UINT64_C(0x00AF9A000000FFFF),
    /* 0x10: data, privilege level 0, writable. */
    UINT64_C(0x00CF92000000FFFF)
};

/* The operand supplied to LGDT, retained because the processor holds only the
 * values it contained, not the storage itself. */
static GdtRegister GdtLoadedRegister;

/* Defined in kernel/cpu/gdt.asm. */
extern void GdtLoadAndReloadSegments(const GdtRegister *descriptor,
                                     uint16_t code_selector,
                                     uint16_t data_selector);

void GdtInitialise(void)
{
    GdtLoadedRegister.limit = (uint16_t)(sizeof(GdtTable) - 1U);
    GdtLoadedRegister.base = (uint64_t)(uintptr_t)&GdtTable[0];

    GdtLoadAndReloadSegments(&GdtLoadedRegister,
                             GDT_KERNEL_CODE_SELECTOR,
                             GDT_KERNEL_DATA_SELECTOR);
}

/* Reads the table register back from the processor with SGDT. */
static void GdtStore(GdtRegister *destination)
{
    __asm__ __volatile__("sgdt %0" : "=m"(*destination) : : "memory");
}

uint64_t GdtBase(void)
{
    GdtRegister stored;

    GdtStore(&stored);

    return stored.base;
}

uint16_t GdtLimit(void)
{
    GdtRegister stored;

    GdtStore(&stored);

    return stored.limit;
}

const uint64_t *GdtTableAddress(void)
{
    return &GdtTable[0];
}

void GdtReport(void)
{
    KernelWriteString("Global descriptor table: base ");
    KernelWriteHexadecimal(GdtBase());
    KernelWriteString(", limit ");
    KernelWriteHexadecimal((uint64_t)GdtLimit());
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)GDT_ENTRY_COUNT);
    KernelWriteString(" descriptors.\n");
}
