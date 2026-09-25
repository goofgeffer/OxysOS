/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/init/internal.h
 * Purpose: Declares the phases of the boot, one per file of this directory,
 *          and what they share with kernel/kernel.c and with each other.
 * Key definitions: KernelInitialiseEarly, KernelInitialiseMemory,
 *          KernelInitialiseDisplay, KernelInitialiseFrameReferences,
 *          KernelInitialiseInterrupts, KernelInitialiseDevices,
 *          KernelInitialiseProcesses, KernelInitialiseProcessors,
 *          KernelInitialiseStorage, KernelVerifyUserland, KernelEnterSession,
 *          KernelDisplayMode.
 * References:
 *   - docs/design/ARCHITECTURE.md, Section 4: the dependency order KernelMain
 *     calls these in.
 *
 * Why the boot is divided by phase. Until 2026-09-25 KernelMain was one
 * function of eleven hundred lines, and kernel.c some two thousand six hundred:
 * the order of initialisation, which is the substance of the boot, could be
 * read only by scrolling through all of it, and a change to one phase was a
 * change to the file every other phase lived in. Each phase is now a function
 * in a file of its own, the way kernel/test/ was divided at sub-task 6.1, and
 * KernelMain is the list of them. The statements were moved and not
 * reordered: the order within each phase, and the comments that give its
 * reasons, are what they were.
 *
 * This header is internal to the boot. Nothing outside kernel/kernel.c and
 * kernel/init/ includes it.
 */

#ifndef OXYS_KERNEL_INIT_INTERNAL_H
#define OXYS_KERNEL_INIT_INTERNAL_H

#include <oxys/types.h>

/*
 * What the display is doing while the machine runs, decided once by the entry
 * point and read by the bootstrap processor's tick.
 *
 *   KERNEL_DISPLAY_IDLE      Nothing: before the shell, when the mouse self-test
 *                            reads the driver's events itself and a service that
 *                            drained them would take the packets the test
 *                            injected, and after it, when the echo loop drains
 *                            them on its own.
 *   KERNEL_DISPLAY_POINTER   The shell has the console and the pointer follows
 *                            the mouse, since 2026-09-16, because a person with
 *                            a mouse in hand saw nothing move and asked why.
 *   KERNEL_DISPLAY_WINDOWS   Sub-task 9.1: the window manager has the screen,
 *                            the keyboard and the mouse; the shell has the
 *                            serial line.
 */
typedef enum KernelDisplayMode
{
    KERNEL_DISPLAY_IDLE = 0,
    KERNEL_DISPLAY_POINTER,
    KERNEL_DISPLAY_WINDOWS
} KernelDisplayMode;

/* The display mode, set by the session and read by the tick; kernel/kernel.c. */
void KernelDisplaySetMode(KernelDisplayMode mode);
KernelDisplayMode KernelDisplayCurrentMode(void);

/* Stops the processor for good, the diagnostic channel flushed; kernel/kernel.c. */
_Noreturn void KernelHalt(void);

/* The boot screen of sub-task 9.3, drawn to the back buffer; kernel/kernel.c. */
void KernelBootScreen(void);

/* Whether the menu entry chosen is the desktop's; session.c. */
bool KernelDesktopEntry(void);

/* Mounts the initial ramdisk at the root, and the machine's volume beneath
 * it; storage.c. */
void KernelMountRootVolume(void);

/* The phases, in the order KernelMain calls them. */
void KernelInitialiseEarly(uint32_t multiboot_information_address, uint32_t multiboot_magic);
void KernelInitialiseMemory(void);
void KernelInitialiseDisplay(void);
void KernelInitialiseFrameReferences(void);
void KernelInitialiseInterrupts(void);
void KernelInitialiseDevices(void);
void KernelInitialiseProcesses(void);
void KernelInitialiseProcessors(void);
void KernelInitialiseStorage(void);
void KernelVerifyUserland(void);
_Noreturn void KernelEnterSession(void);

#endif /* OXYS_KERNEL_INIT_INTERNAL_H */
