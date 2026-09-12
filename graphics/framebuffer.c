/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: graphics/framebuffer.c
 * Purpose: Acquires the linear framebuffer the boot loader supplies, gives its
 *          pages the write-combining memory type, maps them into the kernel
 *          arena, and describes what was obtained.
 * Key functions: FramebufferInitialise, FramebufferIsPresent,
 *          FramebufferIsGraphical, FramebufferAddress, FramebufferEncode,
 *          FramebufferWriteCombining,
 *          FramebufferEstablishWriteCombiningOnThisProcessor, FramebufferReport.
 * References:
 *   - Multiboot2 Specification 2.0, Sections 3.1.10 and 3.6.12: the request tag
 *     and the information tag.
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 11.12.2 and Table 11-11: IA32_PAT holds eight memory-type entries
 *     of eight bits, and a page-table entry selects one of them by the index
 *     (PAT << 2) | (PCD << 1) | PWT.
 *   - Intel SDM, Volume 3A, Table 11-10: the encodings, of which 0x01 is
 *     write-combining and 0x06 write-back.
 *   - Intel SDM, Volume 3A, Table 11-7: the effective memory type combines what
 *     the PAT selects with what the memory type range registers say, the more
 *     conservative prevailing.
 *   - Intel SDM, Volume 2A, "CPUID": leaf 1, EDX bit 16 reports the page
 *     attribute table.
 *   - docs/design/FRAMEBUFFER.md: the design and its limitations.
 *
 * Concurrency. Everything here is established once, before any second processor
 * exists, and is read-only thereafter. The framebuffer's contents are not: two
 * processors drawing upon one surface require the lock sub-task 6.13 built, and
 * this file will not be the place it is taken.
 */

#include <oxys/gfx/framebuffer.h>
#include <oxys/boot/bootinfo.h>
#include <oxys/arch/mm/paging.h>
#include <oxys/mm/vmm.h>
#include <oxys/mm/memory.h>
#include <oxys/arch/cpu/msr.h>
#include <oxys/kernel.h>

/*
 * The entry of IA32_PAT this kernel repurposes, and the memory type written
 * into it.
 *
 * Entry 4 is chosen and not one of the first four, and the choice is the whole
 * of the safety of this arrangement. A page-table entry selects its memory type
 * by the index (PAT << 2) | (PCD << 1) | PWT, so entries 0 to 3 are those every
 * mapping in the kernel already selects — the paging hierarchy, the direct map,
 * the kernel image, every page the heap has issued. Altering one of them would
 * change the memory type of memory already in use, retrospectively and without
 * any of its users being consulted. Entries 4 to 7 are selected only by an entry
 * that sets the PAT bit, and nothing in this kernel set it before this file.
 *
 * The processor's default for entry 4 is write-back, the same as entry 0. It is
 * therefore an entry no existing mapping can be distinguished by, which is what
 * makes it free to take.
 */
#define FRAMEBUFFER_PAT_INDEX      4U
#define FRAMEBUFFER_PAT_ENTRY_WC   UINT64_C(0x01)

/* CPUID leaf 1, EDX bit 16. */
#define CPUID_LEAF_FEATURES        UINT32_C(1)
#define CPUID_EDX_PAGE_ATTRIBUTE_TABLE (UINT32_C(1) << 16)

static BootFramebuffer FramebufferDescription;
static volatile uint8_t *FramebufferBase;
static uint64_t FramebufferMappedBytes;
static bool FramebufferGraphical;
static bool FramebufferCombining;
static bool FramebufferDescribed;

/* Whether the processor reports the page attribute table. */
static bool FramebufferPatIsSupported(void)
{
    uint32_t eax = 0U;
    uint32_t unused_b = 0U;
    uint32_t unused_c = 0U;
    uint32_t features = 0U;

    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(unused_b), "=c"(unused_c), "=d"(features)
                         : "a"(CPUID_LEAF_FEATURES));

    return (features & CPUID_EDX_PAGE_ATTRIBUTE_TABLE) != 0U;
}

/*
 * Writes the write-combining type into entry 4 of IA32_PAT, leaving the other
 * seven as the processor left them.
 *
 * The read-modify-write is what confines the change. Writing a whole constant
 * would be shorter and would also decide the memory type of every mapping in the
 * machine from a value transcribed here, which is a great deal to assert about
 * memory this file has never seen.
 *
 * Returns false where the processor does not report the table, in which case
 * nothing is written and the caller must map the framebuffer cache-disabled
 * instead.
 */
static bool FramebufferEstablishWriteCombining(void)
{
    uint64_t pat;
    const unsigned int shift = FRAMEBUFFER_PAT_INDEX * 8U;

    if (!FramebufferPatIsSupported())
    {
        return false;
    }

    pat = ReadMsr(IA32_PAT);
    pat &= ~(UINT64_C(0xFF) << shift);
    pat |= FRAMEBUFFER_PAT_ENTRY_WC << shift;
    WriteMsr(IA32_PAT, pat);

    /*
     * Every processor holds its own IA32_PAT, and a mapping made here would be
     * write-back upon any processor that had not performed this write. Since
     * sub-task 6.14 there are other processors, and one of them writes the
     * framebuffer the first time it announces its arrival; each therefore
     * repeats this write for itself through
     * FramebufferEstablishWriteCombiningOnThisProcessor, before it writes
     * anything. Two processors holding different memory types for one physical
     * page is precisely the aliasing Intel SDM, Volume 3A, Section 11.12.4,
     * declines to define the behaviour of.
     */
    return true;
}

bool FramebufferInitialise(const BootInformation *information)
{
    uint64_t flags;
    void *mapping;

    FramebufferDescription = information->framebuffer;
    FramebufferDescribed = (FramebufferDescription.format != BOOT_FRAMEBUFFER_NONE);
    FramebufferBase = NULL;
    FramebufferMappedBytes = 0U;
    FramebufferGraphical = false;
    FramebufferCombining = false;

    if (!FramebufferDescribed)
    {
        return false;
    }

    /*
     * A text-mode display is described and not mapped.
     *
     * The address the boot loader reports for it is the VGA text buffer, which
     * the display driver of sub-task 4.2 already owns and already reaches. A
     * second mapping of it would be a second name for one device, with a
     * different memory type from the first, and nothing to decide which name a
     * later writer should use. What is gained by recording it at all is that the
     * report can state what mode the machine is in, and the self-test can assert
     * that the request tag of boot/boot.asm was honoured.
     */
    if (FramebufferDescription.format != BOOT_FRAMEBUFFER_RGB)
    {
        return false;
    }

    FramebufferMappedBytes =
        (uint64_t)FramebufferDescription.pitch * (uint64_t)FramebufferDescription.height;

    /*
     * Write-combining if the processor offers it, cache-disabled if not.
     *
     * Write-back is not among the choices, and this is the decision that matters
     * here. The adapter scans the framebuffer out of memory continuously and
     * without participating in cache coherency for that purpose, so a cached
     * write may sit in a cache line while the display shows what memory held
     * before it. The write is not lost — it lands eventually, when something
     * unrelated evicts the line — which is precisely what makes the fault so
     * unpleasant to diagnose: the image is wrong, and then some time later, for
     * no reason connected to anything, it is right.
     *
     * Cache-disabled is correct and slow: every write is a separate transaction
     * to the adapter. Write-combining is correct and fast: writes are gathered
     * in a buffer and issued together, which is what a framebuffer wants, since
     * it is written in runs and essentially never read.
     */
    if (FramebufferEstablishWriteCombining())
    {
        flags = PAGE_ENTRY_WRITABLE | PAGE_ENTRY_PAT;
        FramebufferCombining = true;
    }
    else
    {
        flags = PAGE_ENTRY_WRITABLE | PAGE_ENTRY_CACHE_DISABLE;
        FramebufferCombining = false;
    }

    mapping = KernelDeviceMap(FramebufferDescription.address, FramebufferMappedBytes, flags);

    if (mapping == NULL)
    {
        KernelWriteString("The framebuffer could not be mapped; the arena refused the "
                          "range.\n");
        FramebufferMappedBytes = 0U;
        return false;
    }

    FramebufferBase = (volatile uint8_t *)mapping;
    FramebufferGraphical = true;

    return true;
}

bool FramebufferIsPresent(void)
{
    return FramebufferDescribed;
}

bool FramebufferIsGraphical(void)
{
    return FramebufferGraphical;
}

volatile uint8_t *FramebufferAddress(void)
{
    return FramebufferBase;
}

PhysicalAddress FramebufferPhysicalAddress(void)
{
    return FramebufferDescription.address;
}

uint32_t FramebufferWidth(void)
{
    return FramebufferDescription.width;
}

uint32_t FramebufferHeight(void)
{
    return FramebufferDescription.height;
}

uint32_t FramebufferPitch(void)
{
    return FramebufferDescription.pitch;
}

uint8_t FramebufferBitsPerPixel(void)
{
    return FramebufferDescription.bits_per_pixel;
}

uint8_t FramebufferBytesPerPixel(void)
{
    /* Rounded upward, so that a depth of 15 bits occupies two bytes as the
     * hardware stores it, rather than one as division would give. */
    return (uint8_t)((FramebufferDescription.bits_per_pixel + 7U) / 8U);
}

uint64_t FramebufferByteCount(void)
{
    return FramebufferMappedBytes;
}

BootFramebufferFormat FramebufferFormat(void)
{
    return FramebufferDescription.format;
}

bool FramebufferWriteCombining(void)
{
    return FramebufferCombining;
}

/*
 * Repeats that write upon the executing processor.
 *
 * It exists for sub-task 6.14. IA32_PAT is per processor, and the mapping the
 * bootstrap processor made carries the page-attribute-table flag: a processor
 * that had not written entry 4 would resolve that flag to whatever its own
 * entry 4 holds — write-back, as a reset leaves it — and would then be writing
 * one physical page under a memory type different from every other processor's.
 * Intel SDM, Volume 3A, Section 11.12.4, declines to define what that produces.
 *
 * It does nothing where the framebuffer was not mapped write-combining, because
 * then nothing maps through entry 4 and the write would change a type no
 * mapping selects. It does nothing where there is no framebuffer at all.
 *
 * It must be called before the processor writes to the framebuffer, which in
 * practice means before it announces its arrival; SmpApplicationProcessorEntry
 * calls it among the establishments that precede any diagnostic.
 */
void FramebufferEstablishWriteCombiningOnThisProcessor(void)
{
    if (!FramebufferCombining)
    {
        return;
    }

    (void)FramebufferEstablishWriteCombining();
}

uint32_t FramebufferEncode(uint8_t red, uint8_t green, uint8_t blue)
{
    const BootFramebuffer *description = &FramebufferDescription;
    uint32_t pixel = 0U;

    if (description->format != BOOT_FRAMEBUFFER_RGB)
    {
        return 0U;
    }

    /*
     * Each channel is given at the width the hardware provides, by discarding
     * the low-order bits of the eight supplied. Discarding the low bits rather
     * than the high ones is what keeps white white: a five-bit channel given
     * 0xFF must yield 0x1F, and taking the top five bits does, whereas taking
     * the bottom five would yield the same 0x1F for 0x1F and for 0xFF alike and
     * would make the whole range dark and banded.
     */
    pixel |= (uint32_t)(red >> (8U - description->red_size)) << description->red_position;
    pixel |= (uint32_t)(green >> (8U - description->green_size))
             << description->green_position;
    pixel |= (uint32_t)(blue >> (8U - description->blue_size)) << description->blue_position;

    return pixel;
}

/*
 * Splits a pixel back into the three eight-bit channels it was packed from.
 *
 * This is the inverse of FramebufferEncode and exists because blending needs the
 * channels apart: a colour laid over another at some coverage is computed per
 * channel, and a packed pixel cannot be interpolated as a whole — the carries
 * would run from one channel into the next.
 *
 * A channel narrower than eight bits is widened by **replicating its high bits
 * into the low ones** rather than by shifting and leaving zeroes. A five-bit
 * channel holding 0x1F must come back as 0xFF: replication gives that, and a
 * plain shift gives 0xF8, so white would decode to something slightly grey and
 * every round trip would darken the image a little further.
 */
void FramebufferDecode(uint32_t pixel, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    const BootFramebuffer *const description = &FramebufferDescription;
    const uint8_t positions[3] = { description->red_position, description->green_position,
                                   description->blue_position };
    const uint8_t sizes[3] = { description->red_size, description->green_size,
                               description->blue_size };
    uint8_t *const outputs[3] = { red, green, blue };

    for (size_t index = 0U; index < 3U; ++index)
    {
        uint32_t value;
        uint8_t widened;

        if (outputs[index] == NULL)
        {
            continue;
        }

        if ((description->format != BOOT_FRAMEBUFFER_RGB) || (sizes[index] == 0U) ||
            (sizes[index] > 8U))
        {
            *outputs[index] = 0U;
            continue;
        }

        value = (pixel >> positions[index]) & ((UINT32_C(1) << sizes[index]) - 1U);
        widened = (uint8_t)(value << (8U - sizes[index]));

        /* Replicate the high bits downward, so that a full channel decodes to
         * 0xFF rather than to 0xFF with its low bits cleared. */
        for (uint32_t shift = sizes[index]; shift < 8U; shift += sizes[index])
        {
            widened = (uint8_t)(widened | (uint8_t)(value << (8U - sizes[index] - shift)));
        }

        *outputs[index] = widened;
    }
}

void FramebufferReport(void)
{
    if (!FramebufferDescribed)
    {
        KernelWriteString("Framebuffer: the boot loader described no display.\n");
        return;
    }

    KernelWriteString("Framebuffer: ");
    KernelWriteString(BootFramebufferFormatName(FramebufferDescription.format));
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)FramebufferDescription.width);
    KernelWriteString(" by ");
    KernelWriteDecimal((uint64_t)FramebufferDescription.height);
    KernelWriteString(FramebufferDescription.format == BOOT_FRAMEBUFFER_EGA_TEXT
                          ? " characters, "
                          : " pixels, ");
    KernelWriteDecimal((uint64_t)FramebufferDescription.bits_per_pixel);
    KernelWriteString(" bits each, pitch ");
    KernelWriteDecimal((uint64_t)FramebufferDescription.pitch);
    KernelWriteString(" bytes.\n");

    KernelWriteString("  Physical ");
    KernelWriteHexadecimal((uint64_t)FramebufferDescription.address);

    if (!FramebufferGraphical)
    {
        KernelWriteString(", not mapped: ");
        KernelWriteString(FramebufferDescription.format == BOOT_FRAMEBUFFER_EGA_TEXT
                              ? "the adapter is in a text mode, which the display "
                                "driver owns.\n"
                              : "this kernel cannot draw upon a display of this "
                                "kind.\n");
        return;
    }

    KernelWriteString(", mapped at ");
    KernelWriteHexadecimal((uint64_t)(uintptr_t)FramebufferBase);
    KernelWriteString(", ");
    KernelWriteDecimal(FramebufferMappedBytes / 1024U);
    KernelWriteString(" KiB, ");
    KernelWriteString(FramebufferCombining ? "write-combining.\n"
                                           : "cache-disabled: no page attribute "
                                             "table.\n");

    KernelWriteString("  Red at bit ");
    KernelWriteDecimal((uint64_t)FramebufferDescription.red_position);
    KernelWriteString(" of ");
    KernelWriteDecimal((uint64_t)FramebufferDescription.red_size);
    KernelWriteString(", green at ");
    KernelWriteDecimal((uint64_t)FramebufferDescription.green_position);
    KernelWriteString(" of ");
    KernelWriteDecimal((uint64_t)FramebufferDescription.green_size);
    KernelWriteString(", blue at ");
    KernelWriteDecimal((uint64_t)FramebufferDescription.blue_position);
    KernelWriteString(" of ");
    KernelWriteDecimal((uint64_t)FramebufferDescription.blue_size);
    KernelWriteString(".\n");
}
