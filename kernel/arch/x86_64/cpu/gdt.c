/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/arch/x86_64/cpu/gdt.c
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
#include <oxys/percpu.h>
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
     * From 0x30 upward: one task state segment descriptor for each processor,
     * each occupying sixteen bytes because its base address is 64 bits wide.
     * They are left empty here — the remainder of the array is zero-initialised
     * by the rule of ISO/IEC 9899:2011, Section 6.7.9, paragraph 21 — and each
     * is written by GdtInstallTaskStateSegment once the segment it describes
     * exists. A descriptor whose present bit is clear is one LTR refuses, which
     * is the right behaviour for a table that has been loaded and a segment
     * belonging to a processor that has not yet been started.
     *
     * Sub-task 6.14 is what fills any of them but the first. The reservation is
     * unconditional rather than sized to the machine because the table is a
     * static array reached before a heap exists upon the processor that reads
     * it, and because a table that grew would have to be re-loaded upon every
     * processor already running it.
     */
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

/* Defined in kernel/arch/x86_64/cpu/gdt.asm. */
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

    /*
     * The reload above destroyed GS.base, and with it the kernel's route to the
     * per-processor area of sub-task 6.13.
     *
     * Intel SDM, Volume 3A, Section 3.4.4: in 64-bit mode the FS and GS bases
     * are not ignored as the other segment bases are, and loading the segment
     * register from a descriptor replaces the hidden base with the descriptor's
     * — which for the flat data descriptor this table holds is zero. The next
     * per-processor access after this function would therefore read address
     * sixteen, and address sixteen is where the self pointer would be if the
     * base were correct.
     *
     * It is repaired here, in the function that broke it, rather than by the
     * caller: a caller that forgot would produce a fault some hundreds of lines
     * away with nothing connecting the two, which is how this was found in the
     * first place.
     *
     * A false result means no area has been established yet — the ordering an
     * application processor will have at sub-task 6.14 — and nothing needs to be
     * done, PerCpuInitialise writing the base itself when it runs.
     */
    (void)PerCpuEstablishSegmentBase();
}

void GdtLoadOnThisProcessor(void)
{
    /*
     * The register operand is the one already composed by GdtInitialise, and it
     * is read here rather than composed again. It names a table that does not
     * move and a limit that does not change, so a second composition could only
     * ever differ from the first — and a processor loading a table one quadword
     * shorter than the one its fellows loaded would fault upon exactly the
     * descriptors this sub-task added.
     *
     * GdtInitialise must therefore have run. It runs upon the bootstrap
     * processor long before any other is started, which SmpInitialise's position
     * in KernelMain guarantees.
     */
    GdtLoadAndReloadSegments(&GdtLoadedRegister,
                             GDT_KERNEL_CODE_SELECTOR,
                             GDT_KERNEL_DATA_SELECTOR);

    /* The reload destroyed GS.base; see GdtInitialise for the whole of why. A
     * false result is the ordinary case here, this running before the calling
     * processor's area has been established. */
    (void)PerCpuEstablishSegmentBase();
}

void GdtInstallTaskStateSegment(uint32_t processor_index, uint64_t base,
                                uint32_t limit)
{
    const size_t index =
        GdtTaskStateSegmentSelector(processor_index) / sizeof(uint64_t);

    /*
     * A processor number beyond the reservation is refused rather than written.
     * PerCpuInitialise refuses the same machine for the same reason, and both
     * refusals are reached only upon a machine declaring more processors than
     * PER_CPU_MAXIMUM; writing here would corrupt whatever follows the table.
     */
    if (processor_index >= PER_CPU_MAXIMUM)
    {
        return;
    }

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
