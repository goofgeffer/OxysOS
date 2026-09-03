/*
 * File: kernel/cpu/gdt.c
 * Purpose: Defines the kernel global descriptor table and installs it, replacing
 *          the table established in boot/boot.asm which resided at a low address
 *          no longer mapped.
 * Key functions: GdtInitialise, GdtInstallTaskStateSegment, GdtDescriptorAt,
 *          GdtBase, GdtLimit, GdtTableAddress, GdtReport.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 3.4.5 and Figure 3-8: the segment descriptor format.
 *   - Intel SDM, Volume 3A, Section 3.5.1: the GDTR, and the limit being one less
 *     than the size of the table.
 *   - Intel SDM, Volume 3A, Section 3.4.2: the processor sets the accessed bit of
 *     a descriptor when its selector is loaded.
 *   - Intel SDM, Volume 3A, Section 5.8.8: the selectors SYSCALL and SYSRET
 *     derive from IA32_STAR by fixed displacements, which fixes the order of the
 *     user descriptors below.
 *   - Intel SDM, Volume 3A, Section 8.2.3 and Figure 8-4: the sixteen-byte task
 *     state segment descriptor, whose type is 9 while available.
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
    UINT64_C(0x00CF92000000FFFF),
    /*
     * 0x18: 32-bit code, privilege level 3, L clear, D set.
     *
     * This kernel does not support compatibility mode and never loads this
     * descriptor. It exists because SYSRET names it — IA32_STAR[63:48] is this
     * selector, and the 64-bit code descriptor is found at that selector plus
     * sixteen — so the slot cannot be omitted without moving the descriptor
     * SYSRET must reach to an offset SYSRET cannot express. It is nonetheless a
     * correct descriptor rather than a filler, since a slot the processor may be
     * made to load must be one it can load.
     */
    UINT64_C(0x00CFFA000000FFFF),
    /* 0x20: data, privilege level 3, writable. Loaded into SS by SYSRET, and
     * into DS, ES, FS and GS by whatever enters user mode. */
    UINT64_C(0x00CFF2000000FFFF),
    /* 0x28: 64-bit code, privilege level 3, L set, D clear. The segment every
     * user program executes in. */
    UINT64_C(0x00AFFA000000FFFF),
    /*
     * 0x30 and 0x38: the task state segment descriptor, which occupies sixteen
     * bytes because its base address is 64 bits wide. It is left empty here and
     * written by GdtInstallTaskStateSegment once the segment it describes
     * exists; a descriptor whose present bit is clear is one LTR refuses, which
     * is the right behaviour for a table that has been loaded and a segment that
     * has not yet been built.
     */
    UINT64_C(0x0000000000000000),
    UINT64_C(0x0000000000000000)
};

/*
 * The type of an available 64-bit task state segment, and the privilege and
 * present bits that accompany it, occupying the access byte of the descriptor.
 *
 * The type becomes 11 — busy — the moment LTR loads a selector for it, which is
 * the processor's doing and not this kernel's. The self-test asserts the changed
 * value, that being the only evidence available that the processor read the
 * descriptor at all.
 */
#define GDT_TSS_TYPE_AVAILABLE UINT64_C(0x9)
#define GDT_TSS_ACCESS_BYTE    (UINT64_C(0x80) | GDT_TSS_TYPE_AVAILABLE)

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

void GdtInstallTaskStateSegment(uint64_t base, uint32_t limit)
{
    const size_t index = GDT_TSS_SELECTOR / sizeof(uint64_t);

    /*
     * The low quadword carries the fields an ordinary descriptor carries, with
     * the base and limit split across it exactly as Figure 8-4 splits them. The
     * granularity flag is left clear, so the limit is a count of bytes; the
     * segment is 104 bytes and a granularity of 4 KiB could not express it.
     */
    GdtTable[index] = ((uint64_t)(limit & UINT32_C(0x0000FFFF))) |
                      ((base & UINT64_C(0x0000000000FFFFFF)) << 16) |
                      (GDT_TSS_ACCESS_BYTE << 40) |
                      (((uint64_t)(limit & UINT32_C(0x000F0000))) << (48 - 16)) |
                      (((base >> 24) & UINT64_C(0xFF)) << 56);

    /*
     * The high quadword carries the upper half of the base address and nothing
     * else. Its remaining bits are reserved and must be zero: the processor
     * checks them, and a descriptor with any of them set makes LTR raise a
     * general-protection exception.
     */
    GdtTable[index + 1U] = (base >> 32) & UINT64_C(0x00000000FFFFFFFF);
}

uint64_t GdtDescriptorAt(size_t index)
{
    if (index >= GDT_ENTRY_COUNT)
    {
        return 0U;
    }

    return GdtTable[index];
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
    KernelWriteDecimal((uint64_t)GDT_DESCRIPTOR_COUNT);
    KernelWriteString(" descriptors in ");
    KernelWriteDecimal((uint64_t)GDT_ENTRY_COUNT);
    KernelWriteString(" slots.\n");

    KernelWriteString("  Kernel code ");
    KernelWriteHexadecimal((uint64_t)GDT_KERNEL_CODE_SELECTOR);
    KernelWriteString(", kernel data ");
    KernelWriteHexadecimal((uint64_t)GDT_KERNEL_DATA_SELECTOR);
    KernelWriteString("; user code ");
    KernelWriteHexadecimal((uint64_t)(GDT_USER_CODE_SELECTOR |
                                      GDT_REQUESTED_PRIVILEGE_USER));
    KernelWriteString(", user data ");
    KernelWriteHexadecimal((uint64_t)(GDT_USER_DATA_SELECTOR |
                                      GDT_REQUESTED_PRIVILEGE_USER));
    KernelWriteString("; task state segment ");
    KernelWriteHexadecimal((uint64_t)GDT_TSS_SELECTOR);
    KernelWriteString(".\n");
}
