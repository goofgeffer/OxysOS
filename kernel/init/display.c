/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/init/display.c
 * Purpose: The display of Phase 6 and sub-task 9.1: the framebuffer, the
 *          drawing primitives, the compositor and the console upon it, the
 *          boot screen, and the window manager asserted upon a screen in
 *          memory.
 * Key functions: KernelInitialiseDisplay.
 * References:
 *   - docs/design/ARCHITECTURE.md, Section 4: the dependency order that fixes
 *     where this phase stands in KernelMain, and the order within it.
 *
 * Moved out of kernel/kernel.c on 2026-09-25, unchanged in order, when
 * KernelMain was reduced to the driver that calls one function per phase;
 * kernel/init/internal.h says why.
 */

#include "internal.h"
#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/gfx/framebuffer.h>
#include <oxys/gfx/graphics.h>
#include <oxys/gfx/compositor.h>
#include <oxys/gfx/console.h>
void KernelInitialiseDisplay(void)
{
    /*
     * Phase 6, sub-task 6.2. The framebuffer the boot loader left the machine
     * with.
     *
     * It is acquired here, after the arena exists and before anything else
     * competes for it, because it is mapped out of the arena and its extent is
     * fixed by the hardware rather than chosen: a display of 1024 by 768 at four
     * bytes a pixel is three mebibytes of contiguous virtual address space, and
     * taking it first means taking it from a region nothing has fragmented.
     *
     * A false return is not a failure. It means the boot loader left the adapter
     * in a text mode, or described no display at all, and in either case the
     * VGA driver of sub-task 4.2 continues to own the screen. The report states
     * which it was.
     */
    (void)FramebufferInitialise(&KernelBootInformation);
    FramebufferReport();
    KernelVerifyFramebuffer();

    /*
     * Phase 6, sub-task 6.3. The primitives that draw upon it.
     *
     * They need nothing but the framebuffer above, and the greater part of what
     * they are asserted against is a surface composed in memory, so this runs
     * here rather than later: a fault in the arithmetic that computes a byte
     * offset into a surface is better found before anything else has drawn.
     */
    GraphicsReport();
    KernelVerifyGraphics();
    KernelVerifyCircle();

    /*
     * Phase 6, sub-task 6.4. The console, which takes the screen.
     *
     * It is started after the drawing self-tests and not before, because it
     * clears the framebuffer and replays the boot log over it: started first, it
     * would be drawn upon by the figures those tests paint, and the log would be
     * unreadable. Started here, it erases them.
     *
     * That the two cannot coexist is why the figures are drawn only when the
     * boot loader's command line asks for them, and why this is then not started
     * at all. Whoever wants to look at the figures of sub-tasks 6.2 and 6.3 asks
     * for them and gives up the console for that boot; everybody else gets the
     * console, which is what a screen is for.
     */
    if (!KernelCommandLineHasOption("graphics-figure"))
    {
        /*
         * Sub-task 6.6. The compositor takes the display first, and the console
         * then draws into its back buffer rather than upon the framebuffer.
         *
         * The order is fixed by that: a console started first would hold a
         * surface describing the framebuffer, and every presentation would copy
         * the back buffer over the top of what it had drawn. Where the
         * compositor cannot be prepared — no framebuffer, or an arena that
         * cannot supply the pages — the console falls back to the framebuffer
         * and behaves as it did before this sub-task.
         */
        (void)CompositorInitialise();
        (void)ConsoleInitialise();

        /*
         * The boot screen, of sub-task 9.3, drawn the moment there is a back
         * buffer to draw it into and upon the desktop entry alone. It stands
         * for the whole of the boot: the display is quiet upon that entry, so
         * nothing writes over it, and the self-tests that present do so with
         * this in the buffer they present. The desktop composes over it at the
         * end. Every other entry shows its log or its prompt, which is what
         * belongs upon a screen the shell is about to take.
         */
        if (KernelDesktopEntry())
        {
            KernelBootScreen();
        }
    }

    ConsoleReport();

    /*
     * The compositing primitives are asserted first, upon surfaces composed in
     * memory, and the compositor itself after: the second uses the first, and a
     * failure in the clip or the blend would otherwise be reported as a failure
     * of the compositor that merely called them.
     */
    KernelVerifyCompositing();
    KernelVerifyCompositor();
    CompositorReport();

    /*
     * Sub-task 9.1: the window manager, upon a screen composed in memory, after
     * the compositor it will be given the back buffer of and before the pointer
     * whose events it will route. It leaves the manager holding the test's
     * surface; the entry point gives it the real one when the demonstration is
     * started, below the banner.
     */
    KernelVerifyWindows();
    KernelVerifyConsole();
    KernelVerifyFaultScreen();
}
