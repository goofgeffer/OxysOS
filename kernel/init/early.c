/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/init/early.c
 * Purpose: The first phase of the boot: the serial line and the text display, the
 *          check of the boot loader's handover, the banner, the per-processor
 *          area, the parse of the boot information, and the test of the display
 *          every later test reports through.
 * Key functions: KernelInitialiseEarly.
 * References:
 *   - docs/design/ARCHITECTURE.md, Section 4: the dependency order that fixes
 *     where this phase stands in KernelMain, and the order within it.
 *   - Multiboot2 Specification 2.0, Section 3.3: the magic value in EAX that
 *     is checked here a second time.
 *
 * Moved out of kernel/kernel.c on 2026-09-25, unchanged in order, when
 * KernelMain was reduced to the driver that calls one function per phase;
 * kernel/init/internal.h says why.
 */

#include "internal.h"
#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/boot/bootinfo.h>
#include <oxys/arch/cpu/percpu.h>
#include <oxys/dev/vga.h>
#include <oxys/dev/serial.h>
void KernelInitialiseEarly(uint32_t multiboot_information_address, uint32_t multiboot_magic)
{
    /*
     * The serial port is initialised first, so that any subsequent failure is
     * recorded even should the display be unavailable or illegible. A negative
     * result is not an error; it indicates only that no adapter is present.
     */
    (void)SerialInitialise(SERIAL_COM1_PORT);

    VgaInitialise();

    VgaSetColour(VGA_COLOUR_LIGHT_CYAN, VGA_COLOUR_BLACK);
    /*
     * The magic value is validated a second time here, the first validation
     * having been performed in 32-bit mode by boot/boot.asm. The repetition
     * guards against a transfer of control that bypasses the assembly entry
     * point, and costs nothing measurable. It is validated before the banner
     * is printed, since 2026-09-15, because the banner now says it was.
     */
    if (multiboot_magic != MULTIBOOT2_BOOTLOADER_MAGIC)
    {
        KernelPanic("The boot loader is not Multiboot2 compliant.");
    }

    /*
     * The banner: one line, at the project owner's request of 2026-09-15,
     * where it had been three. The release is OXYS_VERSION_BANNER — the
     * ordinal form of docs/project/VERSIONING.md as the banner
     * shows it — "1 ALPHA" since 2026-09-16 — and "UNRELEASED" where an image
     * belongs to no release, which every image was until then. It said "Version 0.1.0" until the
     * versioning scheme was written, naming a release that had been withdrawn
     * three days after it was published: a boot banner is the one line of the
     * log a person reads without being asked to, and a version number in it
     * that names nothing is worse than no number, because somebody would
     * eventually cite it.
     */
    /*
     * The screen falls silent before the banner upon the desktop entry, since
     * sub-task 9.3, where every other entry hears it: that entry's screen is
     * the boot screen's and then the desktop's, and the one line of text that
     * used to stand upon it was the whole of what a person saw for the length
     * of the boot — a banner, and then a black screen, and then windows. The
     * serial line carries the banner in every case, this being a courtesy to a
     * person and not a change to what the machine says of itself; the entries
     * that give the shell the screen keep the banner, which is what a prompt
     * belongs beneath.
     */
    if (KernelDesktopEntry())
    {
        KernelDisplaySetQuiet(true);
    }

    KernelWriteString(OXYS_SYSTEM_NAME " x86-64 " OXYS_VERSION_BANNER
                      ", Multiboot2 magic value verified\n");
    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);

    /*
     * The per-processor area of sub-task 6.13, established before anything that
     * could take a lock.
     *
     * It stands here, above the frame allocator and above everything else, for
     * one reason: a spinlock acquire reaches the area, and the allocators below
     * are the first structures a lock will ever be taken over. An area
     * established afterwards would leave every acquire before that point
     * reaching through a segment base of zero.
     *
     * It needs nothing to exist. The area is a static structure, the identifier
     * comes from CPUID and the segment base from a model-specific register; there
     * is no allocation to fail and nothing to parse. Sub-task 6.14 calls the same
     * function upon each application processor as it starts.
     */
    if (!PerCpuInitialise())
    {
        KernelPanic("This machine has more processors than the kernel reserves "
                    "per-processor areas for.");
    }

    /*
     * Reduce the Multiboot2 structure to the neutral description upon which the
     * remainder of the kernel depends. A failure here is unrecoverable: without a
     * memory map the physical frame allocator cannot be constructed, and without
     * that the kernel can do nothing further.
     */
    if (!BootInformationParseMultiboot2(multiboot_information_address,
                                        &KernelBootInformation))
    {
        KernelPanic("The Multiboot2 boot information structure could not be parsed.");
    }

    /*
     * Sub-task 8.2: the display falls silent here unless the `diagnostics`
     * entry of the menu was chosen, and stays silent until the shell is about
     * to be started. The line above it is the banner and is left upon
     * the screen; everything from here to the shell is the boot log, which the
     * serial line carries whether or not the screen does. KernelDisplaySetQuiet
     * records why the serial line is exempt.
     */
    KernelDisplaySetQuiet(!KernelCommandLineHasOption("diagnostics"));

    /*
     * The display is tested next, because it is the instrument through which
     * every later test reports.
     *
     * It was tested before the parse until sub-task 6.2, needing nothing the
     * handover had not already supplied. It cannot be any longer: from that
     * sub-task the boot loader may leave the adapter in a graphics mode, and
     * then the memory this test reads character cells back out of is not the
     * text buffer and nothing it asserts means anything. Which mode the machine
     * is in is stated by the boot information and nowhere else, so the test must
     * follow the parse in order to know whether to run at all.
     */
    KernelVerifyVga();

    BootInformationReport(&KernelBootInformation);
}
