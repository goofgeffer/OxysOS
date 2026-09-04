/*
 * File: kernel/include/oxys/verify.h
 * Purpose: Declares the boot-time self-tests and the diagnostic probes, which
 *          KernelMain runs in dependency order after the subsystem each asserts
 *          has been initialised.
 * Key definitions: KernelVerifyFrameAllocator, KernelVerifyPaging,
 *          KernelVerifyAllocators, KernelVerifyReferenceCounting,
 *          KernelVerifyCopyOnWrite, KernelVerifyAddressSpaces, KernelVerifyIdt,
 *          KernelVerifyInterruptStubs, KernelVerifyDispatcher,
 *          KernelVerifyExceptions, KernelVerifyPrivilege, KernelVerifyPic,
 *          KernelVerifyPit, KernelVerifyKeyboard, KernelVerifySerial,
 *          KernelVerifyVga, KernelVerifyPci, KernelVerifyAta, KernelVerifyBlock,
 *          KernelVerifyBuffer, KernelVerifyExt2, KernelVerifyVfs,
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

#ifndef OXYS_VERIFY_H
#define OXYS_VERIFY_H

#include <oxys/types.h>
#include <oxys/bootinfo.h>

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

/* Phase 6, sub-task 6.3: the two-dimensional primitives, asserted against a
 * surface composed in memory so that they hold upon a machine with no display. */
void KernelVerifyGraphics(void);

/* Phase 6, sub-task 6.1: the descriptors, the task state segment, the interrupt
 * stack table and the three registers that configure SYSCALL. */
void KernelVerifyPrivilege(void);

/* Phases 3 and 4: the devices. */
void KernelVerifyPic(void);
void KernelVerifyPit(void);
void KernelVerifyKeyboard(void);
void KernelVerifySerial(void);
void KernelVerifyVga(void);
void KernelVerifyPci(void);

/* Phase 4: the disk, the generic block layer above it, and the buffer cache
 * above that. */
void KernelVerifyAta(void);
void KernelVerifyBlock(void);
void KernelVerifyBuffer(void);

/* Phase 5: the EXT2 format, and the virtual filesystem layer above it. */
void KernelVerifyExt2(void);
void KernelVerifyVfs(void);

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
void KernelVfsProbeVolume(void);

#endif /* OXYS_VERIFY_H */
