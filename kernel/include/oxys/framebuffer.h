/*
 * File: kernel/include/oxys/framebuffer.h
 * Purpose: Declares the acquisition of the linear framebuffer the boot loader
 *          supplies: its validation, its mapping into kernel space, and the
 *          description through which every later phase draws upon it.
 * Key definitions: FramebufferInitialise, FramebufferIsPresent,
 *          FramebufferIsGraphical, FramebufferAddress, FramebufferWidth,
 *          FramebufferHeight, FramebufferPitch, FramebufferBitsPerPixel,
 *          FramebufferBytesPerPixel, FramebufferByteCount, FramebufferFormat,
 *          FramebufferEncode, FramebufferWriteCombining, FramebufferReport.
 * References:
 *   - Multiboot2 Specification 2.0, Section 3.1.10: the framebuffer request tag
 *     placed in the image header, without which no framebuffer is supplied.
 *   - Multiboot2 Specification 2.0, Section 3.6.12: the framebuffer information
 *     tag, from which the description below is reduced.
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 11.12: the page attribute table, by which the mapping is given the
 *     write-combining memory type.
 *   - Intel SDM, Volume 3A, Table 11-7: the effective memory type is the
 *     combination of what the PAT selects and what the memory type range
 *     registers say, and the more conservative of the two prevails.
 *   - docs/design/GRAPHICS.md: the design, and every assertion made upon it.
 *
 * What this is, and what it is not.
 *
 * This is sub-task 6.2 and it draws nothing. It obtains a framebuffer, decides
 * whether it can be used, maps it so that it may be written efficiently, and
 * describes it. The primitives that draw are sub-task 6.3; the font is 6.4.
 *
 * A framebuffer is not memory the kernel owns. It is memory an adapter scans out
 * of, continuously and without asking, and that fact governs two decisions here.
 * The frame allocator is never told of it, so that it can never be handed to
 * somebody as ordinary memory. And it is mapped write-combining rather than
 * write-back, because a cached write may sit in the cache indefinitely while the
 * adapter displays what memory held before it — the write is not lost, but the
 * image is wrong until something unrelated evicts the line.
 */

#ifndef OXYS_FRAMEBUFFER_H
#define OXYS_FRAMEBUFFER_H

#include <oxys/types.h>
#include <oxys/bootinfo.h>

/*
 * Acquires the framebuffer described by the boot information.
 *
 * Returns true where a framebuffer of pixels was obtained and mapped, and false
 * in every other case: where the boot loader supplied none, where it left the
 * adapter in a text mode, where it offered a kind this kernel cannot draw upon,
 * or where the mapping could not be made. A false return is not a failure of the
 * machine and does not prevent the kernel from proceeding; it means only that
 * there is nothing here to draw upon, and the report states which of the cases
 * it was.
 */
bool FramebufferInitialise(const BootInformation *information);

/* Whether the boot loader described a display at all, of any kind. */
bool FramebufferIsPresent(void);

/*
 * Whether that display is a framebuffer of pixels that this kernel has mapped
 * and may write to. False where the adapter was left in a text mode, which the
 * VGA driver of sub-task 4.2 continues to own.
 */
bool FramebufferIsGraphical(void);

/*
 * The first byte of the framebuffer, in kernel virtual address space. NULL
 * unless FramebufferIsGraphical.
 */
volatile uint8_t *FramebufferAddress(void);

/* The physical address the boot loader reported, retained for the report and
 * for the self-test, which asserts that the mapping reaches it. */
PhysicalAddress FramebufferPhysicalAddress(void);

uint32_t FramebufferWidth(void);
uint32_t FramebufferHeight(void);

/*
 * The number of bytes from the start of one row to the start of the next.
 *
 * This is not the width multiplied by the pixel size and must never be computed
 * as though it were: a boot loader may pad a row to an alignment the hardware
 * prefers, and a traversal that stepped by the occupied width would shear the
 * image progressively down the screen.
 */
uint32_t FramebufferPitch(void);

uint8_t FramebufferBitsPerPixel(void);
uint8_t FramebufferBytesPerPixel(void);

/* The extent of the mapping, being the pitch multiplied by the height. */
uint64_t FramebufferByteCount(void);

/* The format the boot loader reported, whether or not it was usable. */
BootFramebufferFormat FramebufferFormat(void);

/*
 * Packs three eight-bit channels into a pixel of this framebuffer's layout.
 *
 * The channel positions and widths are the hardware's and are read from the boot
 * information rather than assumed. 0x00RRGGBB is a convention of the common case
 * and not a rule; a kernel that assumed it would write blue where it meant red
 * upon an adapter that orders the channels otherwise, and would do so without
 * any error to report.
 *
 * A channel narrower than eight bits is reduced by discarding low-order bits,
 * which is the correct direction: it preserves the bright end of the range.
 */
uint32_t FramebufferEncode(uint8_t red, uint8_t green, uint8_t blue);

/*
 * Whether the mapping was given the write-combining memory type.
 *
 * False where the processor does not report the page attribute table, in which
 * case the mapping is cache-disabled instead: correct, and slow. The distinction
 * is exposed because it is a property of the machine that a later phase's
 * measurements will need to account for.
 */
bool FramebufferWriteCombining(void);

/* Emits the description upon the console and the serial port. */
void FramebufferReport(void);

#endif /* OXYS_FRAMEBUFFER_H */
