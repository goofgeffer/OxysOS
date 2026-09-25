/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/init/userland.c
 * Purpose: Phases 7 to 9 asserted from the root filesystem: fork and the
 *          process lifecycle, the C library, the utilities and the shell, the
 *          signals, the client protocol, init, the configuration, the terminal
 *          emulator, the icons, the mark, the background and the calendar; the
 *          root is mounted part-way, where the first test needs it.
 * Key functions: KernelVerifyUserland.
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
#include <oxys/arch/mm/paging.h>
#include <oxys/arch/mm/addrspace.h>
#include <oxys/arch/interrupt/interrupts.h>
#include <oxys/proc/sched.h>
#include <oxys/arch/interrupt/irq.h>
#include <oxys/dev/lapic.h>
#include <oxys/gfx/client.h>
#include <oxys/proc/process.h>
#include <oxys/fs/pipe.h>
#include <oxys/proc/signal.h>
void KernelVerifyUserland(void)
{
    /*
     * Sub-task 6.11, and it stands here rather than beside the rest of Phase 6
     * for one reason: `execve` loads a program from a path, and a path leads
     * nowhere until a volume is mounted.
     *
     * The dependency is upon the line above and not upon the one below.
     * `KernelVerifyVfs` is what initialises the filesystem layer and registers
     * the EXT2 type; the lifecycle test then composes a volume of memory,
     * presents it as a device of its own, mounts it, writes the program it means
     * to execute, and withdraws all three before it returns — so it needs
     * nothing of the machine's own root volume, and `KernelMountRootVolume`
     * below would in any case find nothing upon a machine with no disk.
     *
     * The fork alone is asserted first and needs none of that, being an
     * operation upon two address spaces and a table; it is run here beside the
     * test it explains rather than three hundred lines above it, so that a
     * reader of the log meets the cheap assertion immediately before the
     * expensive one it makes interpretable.
     */
    KernelVerifyFork();
    KernelVerifyLifecycle();
    ProcessReport();

    /*
     * Sub-task 7.1, and the first assertion here whose subject is not the
     * kernel.
     *
     * The C library's string and memory functions are freestanding: they call
     * nothing, allocate nothing and depend upon nothing but the C language. They
     * are therefore placed at the end of the sequence rather than within it —
     * they have no dependency upon any subsystem above, and nothing above has
     * any dependency upon them, the kernel not being compiled against them at
     * all. Everything else in this function is ordered by what must exist
     * before it; this is ordered by what a reader of the log should meet last.
     */
    KernelVerifyString();

    /*
     * Sub-task 7.2, which must follow everything above and not merely the line
     * before it.
     *
     * The string test above depends upon nothing; this one depends upon almost
     * everything. It composes a program, loads it into an address space of its
     * own, and runs it at privilege level 3 — so it needs the loader of
     * sub-task 6.8, the address spaces of 6.9, the descent of 6.10 and the
     * dispatch of 6.7, all of which stand by this point in the sequence. It is
     * placed after the string test rather than before it because the two are
     * the same subject and a reader of the log should meet the library's floor
     * before the thing built upon it.
     */
    KernelVerifyWrappers();

    /*
     * Sub-task 7.3, which is the same subject one storey higher.
     *
     * It depends upon everything the wrapper test depends upon — the loader, the
     * address spaces, the descent to privilege level 3 — and upon the frame
     * allocator besides, `brk` mapping a frame for every page a heap gains. It
     * is placed immediately after the wrappers because a reader of the log
     * should meet the library's floor, then the calls it is built upon, then the
     * first thing built upon those.
     */
    KernelVerifyHeap();

    /*
     * Sub-task 7.4, which is the same subject one storey higher again.
     *
     * It depends upon nothing the heap test depends upon — the streams it
     * exercises are given regions of memory and reach no device — so it could
     * stand anywhere after the string test. It is placed here because the order
     * of these five is the order the library was built in, and a reader of the
     * log should meet the buffered output after the heap that a program's
     * buffers will one day come from rather than before it.
     */
    KernelVerifyStdio();

    /*
     * Sub-task 7.5, which runs last because it depends upon every one of the
     * four above and upon the loader, the address spaces and the descent
     * besides.
     *
     * It is the first assertion in this project made by a program that was
     * **built** rather than composed byte by byte, and it is what closes the
     * half of sub-task 7.4 this kernel could not assert: the line the program
     * prints reaches this log through the C library's own printf, its own
     * buffering and its own write.
     */
    KernelVerifyStartup();

    /*
     * Sub-task 7.6, which runs after it because every program it runs is built
     * by the procedure that one asserts.
     *
     * It is the first test here that asserts a program by **what it did** rather
     * than by what it reported. Seven programs run at privilege level 3 upon a
     * volume this kernel composed, and the assertions are made afterwards
     * against the volume: that `mkdir` left a directory where there was none,
     * that `rm` removed a name and left its neighbours alone, and that no
     * program ended holding a descriptor of the machine's.
     */
    KernelVerifyUtilities();

    /*
     * Sub-task 8.1, which runs after it for the same reason. It asserts the
     * line editor twice over: the editing and the history in this kernel, with
     * what the editor writes captured and compared byte for byte, and then a
     * session placed upon the terminal and read by a program through
     * descriptor 0 at privilege level 3 — the first `read` of the standard
     * input this system has ever made.
     */
    KernelVerifyLine();

    KernelMountRootVolume();

    /*
     * Sub-task 7.7, and it is the only self-test in this sequence that runs
     * after a mount rather than before one.
     *
     * Its subject is the root the machine actually booted with, which does not
     * exist until the line above has been executed. Every other test here
     * composes the thing it asserts; this one cannot, because a test that
     * mounted a ramdisk for itself would establish that a ramdisk can be
     * mounted and would say nothing at all about whether this kernel mounted
     * one.
     */
    KernelVerifyInitrd();

    /* Sub-task 8.3, after the root for the same reason: the working directory
     * is asserted by a program that changes into /bin. */
    KernelVerifyDirectory();

    /* Sub-task 8.7: the signals, asserted upon a fixture process and then by
     * signal-check and its children, the first programs to be ended, stopped
     * and continued from outside themselves. */
    KernelVerifySignals();

    /* Sub-task 9.2: the client protocol, asserted by window-check upon a
     * manager holding a screen composed in memory, with a kernel thread for
     * the world outside the program. After the signals, because the wait it
     * asserts is ended by a wake the signals' machinery also uses. */
    KernelVerifyClients();
    WindowClientReport();

    /*
     * Sub-tasks 8.2 and 8.3: the shell's grammar, asserted in this kernel, and
     * then the shell itself run upon its sessions — after the root for the
     * reason the test above is, the second session changing into /bin.
     */
    KernelVerifyShell();

    /*
     * Sub-task 9.3: `init` — the adoption of orphans, and the power and pause
     * calls it rests upon, asserted last of all and before the real `init` is
     * started below.
     *
     * It is last because it is the order the machine itself has — `init` is
     * the thing started after everything — and because of what happened when
     * it was not. Placed before the shell's sessions, it ran a program that
     * forks a child which spins, and then sleeps in `pause` to be woken: the
     * job-control session that follows delivers two control bytes through the
     * bootstrap processor's tick, and is sensitive to where that tick falls
     * against the shell's forking, as docs/design/SHELL.md
     * records. Under Bochs, which is some hundred times slower than the
     * machine this is developed upon, the shift was enough to make the
     * session's third `cat` end by the wrong signal, and the shell self-test
     * failed there and nowhere else. Nothing in this test or in that session
     * was wrong; the two simply must not be interleaved, and the natural order
     * is also the safe one.
     */
    KernelVerifyInit();

    /* Sub-task 9.4: the configuration format `init` reads its services from,
     * asserted after it because the parsing is the library's and the files it
     * reads are the ones `init` has just been started without. */
    KernelVerifyConfig();

    /* Sub-task 9.6: the terminal emulator's grid and keys, and the `poll` it
     * waits in. It runs after the configuration's test for the reason that one
     * runs after `init`'s — poll-check forks, waits and sleeps, and a test that
     * does those is placed where it cannot move the tick under the shell's
     * job-control session; docs/design/INIT.md. */
    KernelVerifyTerm();

    /* Sub-task 9.6: the icons the launcher draws, which are files upon the
     * ramdisk and are therefore asserted after it is mounted. */
    KernelVerifyIcon();

    /* Sub-task 9.6: the mark the boot screen was drawn with. It needs nothing
     * but the header, and stands beside the icons because both are the
     * pictures of art/. */
    KernelVerifyMark();

    /* The background, a file upon the ramdisk as the icons are, and asserted
     * beside them for the same reason. */
    KernelVerifyImage();

    /* Sub-task 9.7: the C library's calendar arithmetic, which the clock and
     * `/bin/date` draw with. */
    KernelVerifyTime();

    /* Sub-task 8.6: the pipes the sessions above made, and the scheduler the
     * pipelines ran upon, which the shell is the first thing to sleep in. */
    VfsPipeReport();
    SignalReport();
    SchedulerReport();

    IrqReport();
    LocalApicReport();
    InterruptReport();
    PagingReport();
    AddressSpaceReport();
}
