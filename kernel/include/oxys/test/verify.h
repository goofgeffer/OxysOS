/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/test/verify.h
 * Purpose: Declares the boot-time self-tests and the diagnostic probes, which
 *          KernelMain runs in dependency order after the subsystem each asserts
 *          has been initialised.
 * Key definitions: KernelVerifyFrameAllocator, KernelVerifyPaging,
 *          KernelVerifyAllocators, KernelVerifyReferenceCounting,
 *          KernelVerifyCopyOnWrite, KernelVerifyAddressSpaces, KernelVerifyIdt,
 *          KernelVerifyInterruptStubs, KernelVerifyDispatcher,
 *          KernelVerifyExceptions, KernelVerifyPrivilege, KernelVerifySyscall,
 *          KernelVerifyElf, KernelVerifyProcess, KernelVerifyContextSwitch,
 *          KernelVerifyUserMode, KernelVerifyFork, KernelVerifyLifecycle,
 *          KernelVerifyPic, KernelVerifyIrq, KernelVerifyAcpi,
 *          KernelVerifyLocalApic, KernelVerifyIoApic, KernelVerifyApicRouting,
 *          KernelVerifyPerCpu, KernelVerifySpinlock, KernelVerifyIpi,
 *          KernelVerifyShootdown, KernelVerifyApplicationProcessors,
 *          KernelVerifyScheduler, KernelVerifyString, KernelVerifyWrappers,
 *          KernelVerifyHeap, KernelVerifyStdio, KernelVerifyStartup,
 *          KernelVerifyUtilities, KernelVerifyLine, KernelVerifyShell,
 *          KernelVerifyTerminal, KernelVerifyWindows, KernelVerifyCircle,
 *          KernelWindowDemonstrationStart, KernelWindowDemonstrationService,
 *          KernelVerifyPit, KernelVerifyKeyboard, KernelVerifySerial,
 *          KernelVerifyVga, KernelVerifyPci, KernelVerifyAta, KernelVerifyBlock,
 *          KernelVerifyBuffer, KernelVerifyExt2, KernelVerifyVfs,
 *          KernelVerifyInitrd, KernelVerifyDirectory, KernelVerifySignals,
 *          KernelReportVolumes, KernelVfsProbeVolume, KernelBootInformation,
 *          KernelCommandLineHasOption.
 * References:
 *   - PROJECT_GUIDELINES.md, Section 2, the testing mandate: every milestone
 *     must be bootable and testable.
 *   - docs/project/TESTING.md, Section 1: what `make verify` asserts, and why a
 *     self-test that fails must say so rather than halt.
 *
 * Why these are declared rather than being local to the entry point.
 *
 * There is no test harness and there will be none before Phase 7, there being
 * no userland to run one in. The tests are therefore executed by the kernel
 * upon itself, at boot, in the order the subsystems are initialised — a test
 * cannot run before the thing it asserts exists, and the sequence in KernelMain
 * is that dependency order made explicit.
 *
 * Until this header existed they were static functions within kernel.c, which
 * had grown to some nine thousand lines of which the entry point was the last
 * two hundred. Each is now implemented in kernel/test/, one file per subsystem,
 * and this header is the only thing the entry point needs to know about them.
 * The arrangement is described in kernel/test/README.md.
 *
 * Every routine here reports its own verdict and returns. None halts the
 * machine, and none may: a kernel that stopped at the first failed assertion
 * would report one failure where it might have reported nine, and the run that
 * matters most is the one where several things are broken at once.
 */

#ifndef OXYS_TEST_VERIFY_H
#define OXYS_TEST_VERIFY_H

#include <oxys/types.h>
#include <oxys/boot/bootinfo.h>

/*
 * The description of the machine, parsed from the boot loader's handover.
 *
 * It is defined by kernel.c and read here because two of the self-tests must
 * know where the kernel image lies in physical memory in order to assert that
 * the allocator does not hand out the frames it occupies.
 */
extern BootInformation KernelBootInformation;

/*
 * Whether the boot loader's command line names the given option.
 *
 * The tests that write to a real medium are selected this way rather than being
 * run unconditionally, a kernel that wrote to a stranger's disk merely by having
 * been booted imposing a real cost for nothing. The GRUB entries that set these
 * options are in boot/grub/grub.cfg.
 */
bool KernelCommandLineHasOption(const char *option);

/* Phase 2: the physical frame allocator, the paging hierarchy, the virtual
 * address allocator and the heap above it, and per-frame reference counting. */
void KernelVerifyFrameAllocator(void);
void KernelVerifyPaging(void);
void KernelVerifyAllocators(void);
void KernelVerifyReferenceCounting(void);

/* Phase 2, deferred until Phase 3 supplied the fault handler they depend upon:
 * the copy-on-write resolution and the cloning of an address space. */
void KernelVerifyCopyOnWrite(void);
void KernelVerifyAddressSpaces(void);

/* Phase 3: the interrupt descriptor table, the 256 stubs and the uniform trap
 * frame they construct, the dispatcher, and the exception handlers. */
void KernelVerifyIdt(void);
void KernelVerifyInterruptStubs(void);
void KernelVerifyDispatcher(void);
void KernelVerifyExceptions(void);

/* Phase 6, sub-task 6.2: the framebuffer the boot loader supplied, its mapping
 * and the memory type of that mapping. */
void KernelVerifyFramebuffer(void);

/* Phase 6, sub-task 6.4: the bitmap font and the console drawn with it. */
void KernelVerifyConsole(void);
void KernelVerifyCompositing(void);
void KernelVerifyCompositor(void);

/* Phase 6, sub-task 6.4: the table of fault screens — that every severe fault
 * has one of its own, and that no two of them are the same. */
void KernelVerifyFaultScreen(void);

/* Phase 6, sub-task 6.5: the PS/2 mouse, its packet decoder and framing, and
 * the pointer drawn from it, both asserted without a mouse and without a
 * display. */
void KernelVerifyMouse(void);
void KernelVerifyCursor(void);

/* Phase 6, sub-task 6.3: the two-dimensional primitives, asserted against a
 * surface composed in memory so that they hold upon a machine with no display. */
void KernelVerifyGraphics(void);

/*
 * Sub-task 9.1: the window manager, and the disc it draws its close control
 * with. Both are asserted upon surfaces composed in memory, so that the
 * stacking order is read from the pixel where two windows overlap rather
 * than taken from the manager's word for it, and both hold upon a machine
 * with no display.
 */
void KernelVerifyWindows(void);
void KernelVerifyCircle(void);

/*
 * The demonstration the default boot entry presents: the window manager upon
 * the real screen, with three windows a person can operate. Started by the
 * entry point where there is a compositor and a mouse; serviced from the
 * bootstrap processor's tick, where the pointer already was.
 */
bool KernelWindowDemonstrationStart(void);
void KernelWindowDemonstrationService(void);
bool KernelWindowDemonstrationIsRunning(void);

/* Phase 6, sub-task 6.1: the descriptors, the task state segment, the interrupt
 * stack table and the three registers that configure SYSCALL. */
void KernelVerifyPrivilege(void);
void KernelVerifySyscall(void);
void KernelVerifyElf(void);
void KernelVerifyProcess(void);
void KernelVerifyContextSwitch(void);
void KernelVerifyUserMode(void);
void KernelVerifyFork(void);
void KernelVerifyLifecycle(void);

/* Phases 3 and 4: the devices. */
void KernelVerifyPic(void);

/* Phase 3, and sub-task 6.12: the layer through which a driver claims a request
 * line, whichever controller is presently delivering it. */
void KernelVerifyIrq(void);

/*
 * Phase 6, sub-task 6.12: the firmware's description tables, the Local APIC, the
 * I/O APIC, and the routing of the request lines through them once the 8259A
 * pair has been retired.
 *
 * The first three assert what was programmed. The fourth asserts that a device
 * pin still reaches its driver afterwards, which is the only thing the change
 * was for and the only property whose failure a machine cannot report: a
 * controller programmed wrongly produces a device that is silent, and a silent
 * device looks exactly like an absent one.
 */
void KernelVerifyAcpi(void);
void KernelVerifyLocalApic(void);
void KernelVerifyIoApic(void);
void KernelVerifyApicRouting(void);

/*
 * Phase 6, sub-task 6.13: the per-processor area, the spinlock above it, the
 * inter-processor interrupt, and the translation-lookaside-buffer shootdown
 * built upon that.
 *
 * The first two assert internal state and not behaviour, because upon a machine
 * with one processor a lock that does not lock behaves exactly like one that
 * does. The last two are behavioural: an interrupt a processor sends to itself
 * is delivered like any other, so the whole path — send, gate, handler,
 * invalidation, acknowledgement — is exercised before there is a second
 * processor to exercise it against. docs/design/ARCHITECTURE.md, Section 4.1,
 * records that as the condition upon which 6.13 was placed before 6.14.
 */
void KernelVerifyPerCpu(void);
void KernelVerifySpinlock(void);
void KernelVerifyIpi(void);
void KernelVerifyShootdown(void);

/*
 * Sub-task 6.14: the processors that were started.
 *
 * Where the four above assert mechanisms upon one processor, this one asserts
 * that there are several — and it asserts it behaviourally rather than by
 * counting. A count is what a kernel that wrote a number into a variable would
 * also produce; what only a running processor can produce is an acknowledgement
 * to an interrupt it was sent, and the assertion is therefore a shootdown
 * broadcast whose effect is read out of each target's own area afterwards.
 *
 * Upon a machine with one processor it asserts the same thing the other way: no
 * processor was started, the report says which condition declined it, and the
 * state is consistent with that.
 */
void KernelVerifyApplicationProcessors(void);

/*
 * Sub-task 6.15: the scheduler.
 *
 * Where the five before it asserted mechanisms, this asserts a rotation. It
 * admits kernel threads that count and yield, and watches counters that nothing
 * in the test itself writes: a scheduler that enqueued threads and never ran
 * them produces the same admissions, the same queue lengths and the same report,
 * and the counters are the only thing that distinguishes the two.
 *
 * Upon a machine with one processor it asserts everything but the pre-emption,
 * which yielding cannot be told apart from there.
 */
void KernelVerifyScheduler(void);

/*
 * Sub-task 7.1: the C library's string and memory functions.
 *
 * The first assertion in this corpus whose subject is not the kernel. The
 * functions of `libc/string/` are freestanding — they depend upon nothing but
 * the C language — so anything that can execute C can run them, and `make
 * verify` is the only thing in this project that can execute anything at all.
 * They are therefore compiled into the image and asserted here, until the
 * userland of this phase can host a harness of its own.
 *
 * The kernel does not call them. Only this file is compiled against the C
 * library's include root, by a rule named explicitly in the Makefile, so no
 * kernel translation unit can acquire a dependency upon the userland by
 * including <string.h> without that rule being edited.
 */
void KernelVerifyString(void);

/*
 * Sub-task 7.2: the C library's system-call wrappers.
 *
 * It runs after the string test because it is the same subject — the userland —
 * and because it depends upon far more of the kernel: a process, an address
 * space, an executable loader and a descent to privilege level 3. Half of it is
 * an ordinary call and half of it is a program, and the division is not a
 * convenience. SYSCALL cannot be executed by this kernel at all: the SYSRET that
 * ends the kernel's handling of it returns to privilege level 3
 * unconditionally, so a kernel that called a wrapper would leave its own entry
 * path as a user program. The translation of a result into an errno is
 * therefore asserted by calling it; the invocation is asserted by copying the
 * bytes the library ships into a program and running them where they belong.
 */
void KernelVerifyWrappers(void);

/*
 * Sub-task 7.3: the C library's heap, and the `brk` system call beneath it.
 *
 * Two halves again, and the division is the one the sub-task itself has. The
 * allocator's policy is ordinary C — it calls nothing that can fail for a reason
 * outside the C language — so it is asserted by giving it a region directly
 * through OxysHeapAdopt and exercising malloc, calloc, realloc and free against
 * it, which asserts the code the library ships rather than a description of it.
 * The break beneath the policy is a system call and cannot be executed by this
 * kernel, so it is asserted by a program at privilege level 3 which grows its
 * heap, has the kernel write into the page it gained, reads it back, gives the
 * page up, and confirms that the address is no longer one it may name.
 *
 * It runs after the wrapper test because it depends upon everything that test
 * depends upon and upon the wrappers besides.
 */
void KernelVerifyHeap(void);

/*
 * Sub-task 7.4: the C library's buffered streams and its formatted conversion.
 *
 * Two halves once more, and this time the second half is not here. The
 * buffering policy — when a buffer is emptied, what a partial transfer means,
 * how a pushback interacts with an end-of-file indicator — and the whole of the
 * conversion of a format string are ordinary C, and are asserted by giving the
 * library streams whose device is memory rather than a descriptor. The two
 * transfers beneath them execute SYSCALL and this kernel cannot, so they are
 * asserted by the program sub-task 7.5 builds, whose output arrives upon the
 * serial channel by way of printf.
 *
 * It runs after the heap because the two are independent and a reader of the log
 * should meet the library in the order it was built.
 */
void KernelVerifyStdio(void);

/*
 * Sub-task 7.5: the C runtime startup object, and the link that produces a
 * program.
 *
 * It is the first self-test here whose subject was **built** rather than
 * composed byte by byte, and that is the whole of its value: it asserts the
 * toolchain, the linker script, the startup object, the archive, and every
 * translation unit of the C library compiled with a program's flags rather than
 * the kernel's — which are not the same flags, `-mcmodel=kernel` being wrong for
 * an image at four mebibytes.
 *
 * The program makes its own assertions, prints them through this library's own
 * printf, and ends with the number that failed; this kernel asserts that it
 * loaded, was entered, ended, and ended with zero. Both halves are needed: the
 * program can reach what only a program can reach, and the kernel can see a
 * program that printed nothing at all.
 *
 * It runs last, after every other test of the library, because it depends upon
 * all of them.
 */
void KernelVerifyStartup(void);

/*
 * Sub-task 7.6: the six filesystem system calls, the argument vector `execve`
 * now carries, and the five utilities built above them.
 *
 * It composes an EXT2 volume in memory, mounts it as the root, writes one
 * program onto it and builds a small tree of files and directories, and then
 * runs each of seven programs at privilege level 3 — asserting the status each
 * ended with, and, where a utility changes the volume, what the volume holds
 * afterwards.
 *
 * **It asserts what a program did and not what it printed.** Nothing in this
 * kernel captures the diagnostic path, so a `cat` that copied the wrong file
 * would satisfy every assertion here. The evidence for what was printed is the
 * serial log and a person reading it; the evidence for what was *done* is here.
 * Two of the seven programs — `arg-check` and `exec-check` — exist precisely to
 * turn the one thing that would otherwise be printed-only, the argument vector,
 * into a status a machine can read.
 *
 * It runs after KernelVerifyStartup, because every program it runs depends upon
 * what that one asserts.
 */
void KernelVerifyUtilities(void);

/*
 * Sub-task 8.1: the line editor, in two halves.
 *
 *   The editing and the history, driven in this kernel with the editor's
 *   output captured and compared byte for byte — the character echoed, the
 *   tail redrawn, the backspaces that return the cursor, the recalled line
 *   drawn over the old one. The one place in this project where what a
 *   program would have printed is asserted rather than read by a person.
 *
 *   Then a session of keystrokes, as a terminal sends them, placed upon the
 *   terminal's queue, and `line-check` run at privilege level 3 to read it
 *   through descriptor 0 and compare the lines it edits into.
 *
 * It runs after KernelVerifyUtilities, because the program it runs is built by
 * the procedure KernelVerifyStartup asserts and reads a terminal
 * KernelVerifyTerminal asserts.
 */
void KernelVerifyLine(void);

/*
 * Sub-task 8.2: the shell's tokeniser and parser, asserted in this kernel
 * against the translation units the shell ships — every operator, the quoting
 * of Section 2.2, the io_number, the subset of the grammar parsed and the
 * remainder refused by name, and every bound — and then the shell itself run
 * at privilege level 3 upon a session that continues a command across lines
 * and ends with control-D.
 *
 * It runs after KernelVerifyLine, because the shell reads the terminal through
 * the editor that one asserts.
 */
void KernelVerifyShell(void);

/* Phases 3 and 4: the remaining devices. */
void KernelVerifyPit(void);
void KernelVerifyKeyboard(void);

/*
 * Sub-task 8.1: the terminal input path. The keyboard decoder is driven with
 * scancodes and the bytes the terminal delivers are compared against what a
 * terminal would send — a character, a control character, a cursor key's
 * control sequence — and the queue's order and bound are asserted by
 * injection. Nothing here needs a keyboard present.
 */
void KernelVerifyTerminal(void);
void KernelVerifySerial(void);
void KernelVerifyVga(void);
void KernelVerifyPci(void);

/* Phase 4: the disk, the generic block layer above it, and the buffer cache
 * above that. */
void KernelVerifyAta(void);
void KernelVerifyAhci(void);
void KernelVerifySdhci(void);
void KernelVerifyBlock(void);
void KernelVerifyBuffer(void);

/* Phase 5: the EXT2 format, and the virtual filesystem layer above it. */
void KernelVerifyExt2(void);
void KernelVerifyVfs(void);

/*
 * Sub-task 7.7: the initial ramdisk, and the root filesystem it is mounted as.
 *
 * It runs **after** KernelMountRootVolume rather than among the self-tests
 * above, and that is the whole shape of it. Every other assertion here composes
 * its own subject; this one's subject is the root the machine actually booted
 * with, which does not exist until the mount has been performed. A test that
 * mounted the ramdisk for itself would assert that a ramdisk *can* be mounted
 * and say nothing whatever about whether this kernel in fact mounted one.
 *
 * Four things are asserted, and the second is the one that matters.
 *
 *   That the module arrived: a device named `ram0` is registered, its geometry
 *   agrees with the module's extent, and the root is mounted upon it.
 *
 *   That the bytes are the right bytes. Each utility is read out of /bin through
 *   the filesystem and compared, byte for byte, against the copy of the same
 *   program embedded in this image. The two came from one file at build time, so
 *   any difference is something the path between them did — a block read from
 *   the wrong offset, an indirect block followed wrongly, a length rounded. A
 *   test that merely opened the files would pass upon every one of those.
 *
 *   That a program from the root runs. One utility is loaded from the ramdisk
 *   and entered at privilege level 3, so that the chain from a module through a
 *   block device, a filesystem and the loader is closed by something executing.
 *
 *   That the root can be written. The ramdisk belongs to this kernel rather than
 *   to whoever owns the machine, so it is mounted for writing; a file is created
 *   upon it, read back and removed, which is what says so.
 */
void KernelVerifyInitrd(void);

/*
 * Sub-task 8.3: the working directory, asserted by `dir-check` at privilege
 * level 3 upon the root the machine booted with — that a process begins at
 * the root, that `chdir` moves it and `getcwd` reports it canonically, that a
 * relative path is resolved against it by a call that is neither, that a
 * child of `fork` inherits it, and that each refusal is the named one. It
 * runs after the root is mounted, for the reason KernelVerifyInitrd does.
 */
void KernelVerifyDirectory(void);

/*
 * Sub-task 8.7: the signals, the process groups and `waitpid`. The pending
 * set, the dispositions and the default actions are asserted from the kernel
 * upon a process that never runs; then signal-check is run, which catches,
 * ignores, sends, stops, continues and kills from privilege level 3 and ends
 * with the number of assertions that failed. It runs after the root is
 * mounted, as the tests that run a program do.
 */
void KernelVerifySignals(void);

/*
 * The diagnostic probes, which are not self-tests.
 *
 * These examine whatever volume the machine actually carries, rather than the
 * one composed in memory that the self-tests assert upon. They report what they
 * find and assert nothing, there being nothing to assert about a disk this
 * kernel did not write; their value is that a claim made about a composed volume
 * may be checked against a real one by a tool outside this kernel.
 */
void KernelReportVolumes(void);

/*
 * The write probe, which acts only upon the volume mounted at `point` and only
 * upon the file `path` within it, and only when the operator asked for it at the
 * GRUB menu.
 *
 * Both are arguments since sub-task 7.7, and were written into the probe until
 * then. The initial ramdisk takes the root, so the machine's own volume is
 * mounted at `/mnt` where there is a ramdisk and at `/` where there is not, and
 * a probe that named a path from the root would have quietly stopped examining
 * anything at all upon the machines it exists for.
 */
void KernelVfsProbeVolume(const char *point, const char *path);

#endif /* OXYS_TEST_VERIFY_H */
