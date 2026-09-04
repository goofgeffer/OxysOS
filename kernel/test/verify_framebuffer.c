/*
 * File: kernel/test/verify_framebuffer.c
 * Purpose: Asserts the framebuffer acquired by sub-task 6.2: that the boot
 *          loader honoured the request tag, that what it described is
 *          self-consistent, that the mapping reaches the physical memory the
 *          adapter scans out of, and that the pages carry the memory type this
 *          kernel intended them to.
 * Key functions: KernelVerifyFramebuffer.
 * References:
 *   - docs/design/GRAPHICS.md, Section 6: every assertion below, paired with the
 *     silent failure it catches.
 *   - Multiboot2 Specification 2.0, Section 3.6.12: the tag the description is
 *     reduced from.
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 11.12: the page attribute table, whose entry 4 is read back here.
 *
 * The one assertion that cannot be made.
 *
 * Nothing here can establish that anything appeared upon the screen. A kernel
 * has no way to read its own display back through the eye of whoever is looking
 * at it, and a framebuffer that is mapped, written and read back correctly may
 * still be scanned out by nothing at all. What the pattern written below is for
 * is the operator: it is the half of this sub-task's verification that a person
 * performs, and docs/project/TESTING.md, Section 15, records how.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/framebuffer.h>
#include <oxys/bootinfo.h>
#include <oxys/paging.h>
#include <oxys/vmm.h>
#include <oxys/memory.h>
#include <oxys/msr.h>

static bool KernelFramebufferSucceeded;

static void KernelFramebufferRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        KernelFramebufferSucceeded = false;
    }
}

/* The memory type IA32_PAT holds in the entry a page-table entry selects when
 * its PAT bit is set and PCD and PWT are clear. */
#define KERNEL_PAT_INDEX_FOR_PAT_BIT 4U
#define KERNEL_PAT_WRITE_COMBINING   UINT64_C(0x01)

static uint64_t KernelPatEntry(unsigned int index)
{
    return (ReadMsr(IA32_PAT) >> (index * 8U)) & UINT64_C(0xFF);
}

/*
 * The paging flags in force for an address, read out of the hierarchy rather
 * than remembered from the request.
 *
 * PagingTranslate gives the frame; it does not give the attributes, and the
 * attributes are the whole of what is asserted about the memory type. The
 * hierarchy is therefore walked here for the entry itself.
 */
static bool KernelPageEntryFlags(VirtualAddress address, uint64_t *flags)
{
    const PhysicalAddress root = PagingKernelRoot();
    uint64_t *table;
    uint64_t entry;
    unsigned int level;
    const unsigned int shifts[4] = { 39U, 30U, 21U, 12U };
    PhysicalAddress next = root;

    for (level = 0U; level < 4U; ++level)
    {
        const size_t index = (size_t)((address >> shifts[level]) & UINT64_C(0x1FF));

        table = PagingTableEntries(next);
        entry = table[index];

        if ((entry & PAGE_ENTRY_PRESENT) == 0U)
        {
            return false;
        }

        /*
         * A large page terminates the walk early. The framebuffer is mapped in
         * 4 KiB pages, so meeting one here would itself be a fault.
         *
         * The test is confined to levels 1 and 2 — the directory-pointer and
         * directory entries — and must be. Bit 7 is PS in those, and PAT in the
         * table entry of level 3, which is the very bit this walk exists to
         * read. Testing it at level 3 as well would report every write-combining
         * page as a large one, and did, before this comment was written.
         */
        if ((level >= 1U) && (level <= 2U) && ((entry & PAGE_ENTRY_LARGE) != 0U))
        {
            return false;
        }

        next = (PhysicalAddress)(entry & PAGE_ENTRY_ADDRESS_MASK);
    }

    *flags = entry;
    return true;
}

/* Asserts the description, whatever kind of display was reported. */
static void KernelVerifyFramebufferDescription(void)
{
    const uint64_t bytes_per_row =
        ((uint64_t)FramebufferWidth() * FramebufferBitsPerPixel()) / 8U;

    /*
     * That a display was described at all.
     *
     * This is the assertion upon boot/boot.asm rather than upon this file: the
     * framebuffer information tag is emitted only where the image's Multiboot2
     * header carries the request tag, so a description of no display means the
     * request tag is absent, malformed, or placed where the boot loader did not
     * look for it.
     */
    KernelFramebufferRequire(FramebufferIsPresent(),
                             "the boot loader described no display, so the framebuffer "
                             "request tag was not honoured");

    if (!FramebufferIsPresent())
    {
        return;
    }

    KernelFramebufferRequire(FramebufferPhysicalAddress() != 0U,
                             "the display was reported at physical address zero");
    KernelFramebufferRequire((FramebufferWidth() != 0U) && (FramebufferHeight() != 0U),
                             "the display was reported with no extent");
    KernelFramebufferRequire(FramebufferBitsPerPixel() != 0U,
                             "the display was reported with pixels of no depth");

    /*
     * The pitch covers a row, and is asserted as a relation rather than as a
     * product. A boot loader may pad a row to an alignment its hardware prefers,
     * so equality is not required; but a pitch below the occupied width would
     * make the second row begin inside the first, and every row after it drift
     * further down and to the left. That is the failure this catches, and it is
     * one that looks like a skewed image rather than like an error.
     */
    KernelFramebufferRequire((uint64_t)FramebufferPitch() >= bytes_per_row,
                             "the pitch is narrower than one row, so the rows overlap");

    KernelFramebufferRequire(
        FramebufferBytesPerPixel() == ((FramebufferBitsPerPixel() + 7U) / 8U),
        "the pixel size in bytes does not round its size in bits upward");
}

/* Asserts the mapping, which exists only for a framebuffer of pixels. */
static void KernelVerifyFramebufferMapping(void)
{
    const volatile uint8_t *base = FramebufferAddress();
    const VirtualAddress first = (VirtualAddress)(uintptr_t)base;
    const uint64_t extent = FramebufferByteCount();
    VirtualAddress last;
    uint64_t entry = 0U;

    /*
     * A text-mode display is described and not mapped, and that is asserted as
     * firmly as a mapping would be. The address the boot loader reports for a
     * text mode is the VGA text buffer, which the display driver of sub-task 4.2
     * already owns; a second mapping of it with a different memory type would be
     * two names for one device with nothing to decide which a writer should use.
     */
    if (!FramebufferIsGraphical())
    {
        KernelFramebufferRequire(base == NULL,
                                 "a display this kernel cannot draw upon was "
                                 "nevertheless mapped");
        KernelFramebufferRequire(FramebufferByteCount() == 0U,
                                 "an unmapped display reports a mapped extent");
        KernelFramebufferRequire(FramebufferEncode(255U, 255U, 255U) == 0U,
                                 "a display of no known pixel layout encoded a colour");
        return;
    }

    KernelFramebufferRequire(base != NULL, "a graphical framebuffer has no address");

    if (base == NULL)
    {
        return;
    }

    KernelFramebufferRequire(
        extent == ((uint64_t)FramebufferPitch() * FramebufferHeight()),
        "the mapped extent is not the pitch multiplied by the height");

    last = first + extent - 1U;

    /* The whole of it lies within the arena, and not in the direct map or the
     * kernel image window, either of which would mean it had been mapped over
     * memory that belongs to something else. */
    KernelFramebufferRequire(
        (first >= KERNEL_ARENA_BASE) && (last < (KERNEL_ARENA_BASE + KERNEL_ARENA_SIZE)),
        "the framebuffer was not mapped within the kernel arena");

    /*
     * The mapping reaches the memory the boot loader named.
     *
     * Both ends are translated, not merely the first: a mapping loop that
     * advanced the virtual address and forgot to advance the physical one would
     * give a correct first page and point every page after it at the same frame,
     * and the visible result would be the first few rows of the image repeated
     * down the screen rather than anything that reports itself.
     */
    KernelFramebufferRequire(PagingTranslate(first) == FramebufferPhysicalAddress(),
                             "the first page of the mapping does not reach the "
                             "framebuffer");
    KernelFramebufferRequire(
        PagingTranslate(last & ~(VirtualAddress)(PAGE_SIZE - 1U)) ==
            ((FramebufferPhysicalAddress() + extent - 1U) & ~(PhysicalAddress)(PAGE_SIZE - 1U)),
        "the last page of the mapping does not reach the framebuffer, so the range "
        "is not contiguous");

    KernelFramebufferRequire(PagingAddressIsWritable(first),
                             "the framebuffer was mapped without write permission");

    /* The memory type, read out of the page-table entry and out of IA32_PAT.
     * Neither alone establishes it: the entry selects an index, and the register
     * decides what that index means. */
    if (KernelPageEntryFlags(first, &entry))
    {
        if (FramebufferWriteCombining())
        {
            KernelFramebufferRequire(
                (entry & PAGE_ENTRY_PAT) != 0U,
                "the framebuffer's page-table entry does not select the upper half of "
                "the page attribute table");
            KernelFramebufferRequire(
                (entry & (PAGE_ENTRY_CACHE_DISABLE | PAGE_ENTRY_WRITE_THROUGH)) == 0U,
                "PCD or PWT is set, so the entry selects an index other than 4");
            KernelFramebufferRequire(
                KernelPatEntry(KERNEL_PAT_INDEX_FOR_PAT_BIT) == KERNEL_PAT_WRITE_COMBINING,
                "entry 4 of IA32_PAT does not hold write-combining, so the framebuffer "
                "is write-back and the display may lag the memory indefinitely");
        }
        else
        {
            KernelFramebufferRequire((entry & PAGE_ENTRY_CACHE_DISABLE) != 0U,
                                     "without the page attribute table the framebuffer "
                                     "must be cache-disabled, and is not");
        }
    }
    else
    {
        KernelFramebufferRequire(false,
                                 "the framebuffer's mapping could not be walked, so it "
                                 "is absent or made of large pages");
    }

    /*
     * The first four entries of IA32_PAT are as the processor left them.
     *
     * This is the assertion that the arrangement is safe rather than merely
     * effective. Every mapping in this kernel that existed before this sub-task
     * selects one of those four, so altering one would have changed the memory
     * type of the paging hierarchy, the direct map and the whole of the heap,
     * retrospectively and with nothing to report it.
     */
    KernelFramebufferRequire(KernelPatEntry(0U) == UINT64_C(0x06),
                             "entry 0 of IA32_PAT is no longer write-back, so existing "
                             "mappings have had their memory type changed beneath them");
    KernelFramebufferRequire(KernelPatEntry(1U) == UINT64_C(0x04),
                             "entry 1 of IA32_PAT is no longer write-through");
    KernelFramebufferRequire(KernelPatEntry(2U) == UINT64_C(0x07),
                             "entry 2 of IA32_PAT is no longer uncacheable-minus");
    KernelFramebufferRequire(KernelPatEntry(3U) == UINT64_C(0x00),
                             "entry 3 of IA32_PAT is no longer uncacheable");
}

/*
 * Asserts that the mapping is memory that can be written and read back, and
 * leaves a pattern upon the screen for a person to look at.
 *
 * The readback is what distinguishes a mapping that reaches the adapter from one
 * that reaches nothing: an address that translated correctly but named a region
 * no device decodes would accept every write and return ones, or zeroes, or the
 * last value on the bus.
 *
 * The corners are written rather than a run at the beginning, because a mapping
 * short by a page would pass every test made upon its start. The last pixel of
 * the last row is the one that fails if the extent is wrong by any amount.
 */
static void KernelVerifyFramebufferAccess(void)
{
    volatile uint8_t *base = FramebufferAddress();
    const uint32_t bytes = FramebufferBytesPerPixel();
    const uint32_t pitch = FramebufferPitch();
    const uint32_t width = FramebufferWidth();
    const uint32_t height = FramebufferHeight();
    const uint32_t white = FramebufferEncode(255U, 255U, 255U);
    const uint32_t black = FramebufferEncode(0U, 0U, 0U);
    uint64_t corner;
    uint32_t read_back = 0U;

    if (!FramebufferIsGraphical() || base == NULL)
    {
        return;
    }

    /* The colour encoding, asserted before it is used to write anything. Black
     * must be zero in every layout, and white must set every bit of every
     * channel and no bit outside them. */
    KernelFramebufferRequire(black == 0U, "black does not encode as zero");
    KernelFramebufferRequire(white != 0U, "white encodes as zero, so no channel was set");

    corner = ((uint64_t)(height - 1U) * pitch) + ((uint64_t)(width - 1U) * bytes);

    KernelFramebufferRequire((corner + bytes) <= FramebufferByteCount(),
                             "the last pixel of the last row lies outside the mapping");

    if ((corner + bytes) > FramebufferByteCount())
    {
        return;
    }

    /*
     * Write white into the final pixel and read it back.
     *
     * Only the bytes the pixel occupies are compared. A 24-bit pixel occupies
     * three bytes and the fourth belongs to the pixel after it, so reading four
     * would compare a byte this test never wrote.
     */
    for (uint32_t index = 0U; index < bytes; ++index)
    {
        base[corner + index] = (uint8_t)((white >> (index * 8U)) & 0xFFU);
    }

    for (uint32_t index = 0U; index < bytes; ++index)
    {
        read_back |= (uint32_t)base[corner + index] << (index * 8U);
    }

    KernelFramebufferRequire(read_back == (white & ((bytes >= 4U)
                                                        ? UINT32_C(0xFFFFFFFF)
                                                        : ((UINT32_C(1) << (bytes * 8U)) - 1U))),
                             "a pixel written to the framebuffer did not read back, so "
                             "the mapping reaches no memory the adapter decodes");

    /*
     * The pattern the operator judges: a band of red, green and blue across the
     * top of the screen, in that order, each a sixteenth of the height.
     *
     * Its purpose is to be looked at, and it is composed so that looking at it
     * establishes something. If the channel positions were misread, the bands
     * appear in the wrong order or in the wrong colours. If the pitch were
     * wrong, the bands skew rather than lying flat. If the extent were wrong,
     * they stop short of the right-hand edge.
     */
    for (uint32_t row = 0U; row < (height / 16U); ++row)
    {
        for (uint32_t column = 0U; column < width; ++column)
        {
            const uint32_t third = width / 3U;
            const uint32_t colour = (column < third)
                                        ? FramebufferEncode(200U, 30U, 30U)
                                        : ((column < (third * 2U))
                                               ? FramebufferEncode(30U, 200U, 30U)
                                               : FramebufferEncode(30U, 30U, 200U));
            const uint64_t offset = ((uint64_t)row * pitch) + ((uint64_t)column * bytes);

            for (uint32_t index = 0U; index < bytes; ++index)
            {
                base[offset + index] = (uint8_t)((colour >> (index * 8U)) & 0xFFU);
            }
        }
    }
}

void KernelVerifyFramebuffer(void)
{
    KernelFramebufferSucceeded = true;

    KernelVerifyFramebufferDescription();
    KernelVerifyFramebufferMapping();
    KernelVerifyFramebufferAccess();

    KernelWriteString(KernelFramebufferSucceeded ? "Framebuffer self-test passed.\n"
                                                 : "Framebuffer self-test FAILED.\n");
}
