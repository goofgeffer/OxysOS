/*
 * File: kernel/test/verify_privilege.c
 * Purpose: Asserts the apparatus of a privilege transition established by
 *          sub-task 6.1: the user-mode descriptors and the order the
 *          processor\'s arithmetic imposes upon them, the task state segment and
 *          the stacks it names, the interrupt stack table, and the three
 *          registers that configure SYSCALL.
 * Key functions: KernelVerifyPrivilege.
 * References:
   - docs/design/PRIVILEGE.md, Section 7: every assertion below, paired with
 *     the silent failure it catches.
 *   - Intel SDM, Volume 3A, Figure 3-8: the segment descriptor fields the
 *     assertions decode; Section 8.2.3, the task state segment descriptor and
 *     its type of 9 while available and 11 once loaded.
 *   - Intel SDM, Volume 2B, "SYSCALL": what the instruction loads, saves and
 *     clears, all of which is asserted here by executing it.
 *
 * Two of the five parts do more than inspect a structure, and they are the
 * two that matter: inspecting a structure the processor reads establishes only
 * what this kernel wrote into it. The interrupt stack table is exercised by
 * raising a spare vector with the entry and without; and the transition itself
 * is exercised, SYSCALL being executable from privilege level 0, where it raises
 * no privilege but performs every other part of the transition.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/gdt.h>
#include <oxys/tss.h>
#include <oxys/syscall.h>
#include <oxys/msr.h>
#include <oxys/idt.h>
#include <oxys/interrupts.h>
#include <oxys/exceptions.h>
#include <oxys/cpu.h>

/*
 * ---------------------------------------------------------------------------
 * The self-test of the privilege-transition apparatus, sub-task 6.1.
 *
 * Nothing here transfers to privilege level 3: there is no user mapping and no
 * program to put in it until sub-task 6.10. What is asserted is that every
 * structure the processor will consult when it does is present, well formed, and
 * says what this kernel believes it says.
 *
 * That distinction matters because each of these structures is read by the
 * processor and by nothing else. A wrong value in any of them produces no
 * diagnostic at the moment it is written; it produces a general-protection
 * exception, or a triple fault, at the first privilege transition — which will
 * be several sub-tasks away, in code that is itself new, and the fault will be
 * attributed there.
 *
 * The one thing that can be exercised now is the system-call transition itself,
 * because SYSCALL may be executed from privilege level 0 as well as from 3. It
 * does not raise privilege — there is none to raise — but it loads the selectors
 * IA32_STAR names, transfers to the address IA32_LSTAR holds, and clears the
 * bits IA32_FMASK sets, and all three may therefore be asserted against what the
 * processor actually did rather than against what it was told.
 * ------------------------------------------------------------------------- */

/* Whether the sequence has held so far. */
static bool KernelPrivilegeSucceeded;

/* Asserts one property, naming it where it fails. */
static void KernelPrivilegeRequire(bool condition, const char *statement)
{
    if (condition)
    {
        return;
    }

    KernelWriteString("  ");
    KernelWriteString(statement);
    KernelWriteString("\n");
    KernelPrivilegeSucceeded = false;
}

/*
 * The fields of a segment descriptor, per Intel SDM, Volume 3A, Figure 3-8.
 *
 * They are extracted here rather than the descriptors being compared against
 * whole constants, because a constant would agree with a mistaken descriptor as
 * readily as with a correct one: what is asserted below is that the descriptor
 * says "privilege level 3" and "64-bit code", not that it holds a particular
 * quadword somebody transcribed twice.
 */
static uint64_t KernelSegmentPrivilege(uint64_t descriptor)
{
    return (descriptor >> 45) & UINT64_C(0x3);
}

static bool KernelSegmentIsPresent(uint64_t descriptor)
{
    return ((descriptor >> 47) & UINT64_C(1)) != 0U;
}

static bool KernelSegmentIsCode(uint64_t descriptor)
{
    /* Bit 44 distinguishes a code or data descriptor from a system one; bit 43
     * distinguishes code from data within that. */
    return (((descriptor >> 44) & UINT64_C(1)) != 0U) &&
           (((descriptor >> 43) & UINT64_C(1)) != 0U);
}

static bool KernelSegmentIsWritableData(uint64_t descriptor)
{
    return (((descriptor >> 44) & UINT64_C(1)) != 0U) &&
           (((descriptor >> 43) & UINT64_C(1)) == 0U) &&
           (((descriptor >> 41) & UINT64_C(1)) != 0U);
}

/* The L flag, which designates a 64-bit code segment, and the D flag, which the
 * architecture requires to be clear wherever L is set. */
static bool KernelSegmentIsLongMode(uint64_t descriptor)
{
    return ((descriptor >> 53) & UINT64_C(1)) != 0U;
}

static bool KernelSegmentIsDefault32(uint64_t descriptor)
{
    return ((descriptor >> 54) & UINT64_C(1)) != 0U;
}

static uint64_t KernelSegmentType(uint64_t descriptor)
{
    return (descriptor >> 40) & UINT64_C(0xF);
}

/* The base and the limit of a system descriptor, reassembled from the four
 * pieces the format scatters them into. */
static uint64_t KernelSystemSegmentBase(uint64_t low, uint64_t high)
{
    return ((low >> 16) & UINT64_C(0x00FFFFFF)) | (((low >> 56) & UINT64_C(0xFF)) << 24) |
           ((high & UINT64_C(0xFFFFFFFF)) << 32);
}

static uint64_t KernelSystemSegmentLimit(uint64_t low)
{
    return (low & UINT64_C(0xFFFF)) | (((low >> 48) & UINT64_C(0xF)) << 16);
}

/*
 * Executes SYSCALL.
 *
 * RCX and R11 are destroyed by the instruction itself — it places the return
 * address in the one and the flags in the other — so both are declared clobbered
 * rather than being saved, and the condition codes with them, the entry point
 * restoring the flags it was given.
 */
static void KernelExecuteSystemCall(void)
{
    __asm__ __volatile__("syscall" : : : "rcx", "r11", "cc", "memory");
}

/* Asserts the descriptors the transition will load. */
static void KernelVerifyPrivilegeDescriptors(void)
{
    const uint64_t kernel_code = GdtDescriptorAt(GDT_KERNEL_CODE_SELECTOR / 8U);
    const uint64_t user_code32 = GdtDescriptorAt(GDT_USER_CODE32_SELECTOR / 8U);
    const uint64_t user_data = GdtDescriptorAt(GDT_USER_DATA_SELECTOR / 8U);
    const uint64_t user_code = GdtDescriptorAt(GDT_USER_CODE_SELECTOR / 8U);

    KernelPrivilegeRequire(GdtLimit() == (uint16_t)((GDT_ENTRY_COUNT * 8U) - 1U),
                           "the table's limit does not cover the descriptors it holds");
    KernelPrivilegeRequire(GdtBase() == (uint64_t)(uintptr_t)GdtTableAddress(),
                           "the processor holds a table base that is not this table's");

    /* The kernel descriptors, which were established in Phase 3 and must not
     * have moved: IA32_STAR names them by position. */
    KernelPrivilegeRequire(KernelSegmentIsPresent(kernel_code) &&
                               KernelSegmentIsCode(kernel_code) &&
                               KernelSegmentIsLongMode(kernel_code) &&
                               (KernelSegmentPrivilege(kernel_code) == 0U),
                           "the kernel code descriptor is not a present 64-bit code segment "
                           "at privilege level 0");

    /*
     * The user descriptors. Each is asserted for what it says rather than for
     * what it is, and the privilege level is the field that matters: a user
     * descriptor left at privilege level 0 would be loaded without complaint and
     * would leave a program running with the kernel's authority.
     */
    KernelPrivilegeRequire(KernelSegmentIsPresent(user_code) &&
                               KernelSegmentIsCode(user_code) &&
                               KernelSegmentIsLongMode(user_code) &&
                               (!KernelSegmentIsDefault32(user_code)) &&
                               (KernelSegmentPrivilege(user_code) == 3U),
                           "the 64-bit user code descriptor is not a present 64-bit code "
                           "segment at privilege level 3");

    KernelPrivilegeRequire(KernelSegmentIsPresent(user_data) &&
                               KernelSegmentIsWritableData(user_data) &&
                               (KernelSegmentPrivilege(user_data) == 3U),
                           "the user data descriptor is not a present writable data segment "
                           "at privilege level 3");

    /*
     * The compatibility-mode code descriptor, which this kernel never loads and
     * cannot omit. SYSRET finds the 64-bit descriptor at this one's selector
     * plus sixteen, so a slot left empty here would be a descriptor SYSRET
     * loads and the processor rejects.
     */
    KernelPrivilegeRequire(KernelSegmentIsPresent(user_code32) &&
                               KernelSegmentIsCode(user_code32) &&
                               (!KernelSegmentIsLongMode(user_code32)) &&
                               KernelSegmentIsDefault32(user_code32) &&
                               (KernelSegmentPrivilege(user_code32) == 3U),
                           "the compatibility-mode user code descriptor, which SYSRET names, "
                           "is absent or malformed");

    /*
     * The order, asserted as an ordering rather than as three separate
     * descriptors. This is the property that has no local symptom: every
     * descriptor above may be perfectly formed and the transition still fail,
     * because what SYSRET loads is decided by arithmetic upon a selector and not
     * by which descriptor was intended.
     */
    KernelPrivilegeRequire((GDT_KERNEL_DATA_SELECTOR == (GDT_KERNEL_CODE_SELECTOR + 8U)) &&
                               (GDT_USER_DATA_SELECTOR ==
                                (GDT_USER_CODE32_SELECTOR + 8U)) &&
                               (GDT_USER_CODE_SELECTOR ==
                                (GDT_USER_CODE32_SELECTOR + 16U)),
                           "the descriptors do not stand at the displacements SYSCALL and "
                           "SYSRET derive their selectors by");
}

/* Asserts the task state segment and the descriptor that names it. */
static void KernelVerifyPrivilegeTaskSegment(void)
{
    const uint64_t low = GdtDescriptorAt(GDT_TSS_SELECTOR / 8U);
    const uint64_t high = GdtDescriptorAt((GDT_TSS_SELECTOR / 8U) + 1U);
    const uint64_t base = KernelSystemSegmentBase(low, high);

    KernelPrivilegeRequire(base == (uint64_t)(uintptr_t)TssAddress(),
                           "the descriptor's base does not name the segment this kernel "
                           "built");
    KernelPrivilegeRequire(KernelSystemSegmentLimit(low) == TssLimit(),
                           "the descriptor's limit does not cover the segment exactly");
    KernelPrivilegeRequire(KernelSegmentIsPresent(low) &&
                               (KernelSegmentPrivilege(low) == 0U),
                           "the task state segment descriptor is absent or not at privilege "
                           "level 0");

    /*
     * The type is 9 while the segment is available and 11 once a task register
     * has been loaded with a selector for it. The processor makes that change
     * itself, so asserting the changed value is the only evidence available that
     * the processor read the descriptor at all rather than that this kernel
     * wrote a selector into a register.
     */
    KernelPrivilegeRequire(KernelSegmentType(low) == UINT64_C(0xB),
                           "the task state segment descriptor is not marked busy, so the "
                           "processor never read it");
    KernelPrivilegeRequire(TssTaskRegister() == GDT_TSS_SELECTOR,
                           "the task register does not hold the selector of the segment");

    /* The stacks. Neither may be absent, they must be distinct, and both must be
     * aligned as the ABI requires of a stack pointer at a function's entry. */
    KernelPrivilegeRequire(TssKernelStack() != 0U,
                           "no stack is named for a transfer to privilege level 0");
    KernelPrivilegeRequire((TssKernelStack() % 16U) == 0U,
                           "the privilege level 0 stack is not sixteen-byte aligned");
    KernelPrivilegeRequire(TssInterruptStack(TSS_IST_DOUBLE_FAULT) != 0U,
                           "no stack is named for the double fault");
    KernelPrivilegeRequire(TssInterruptStack(TSS_IST_DOUBLE_FAULT) != TssKernelStack(),
                           "the double fault would be delivered upon the stack that is "
                           "already in use, which is the case it exists to survive");

    /*
     * The map base beyond the limit is what denies every port to a user program.
     * Were it within the limit, the bytes it addressed would be read as a
     * permission bitmap, and whatever happened to lie there would decide which
     * ports a user program could drive.
     */
    KernelPrivilegeRequire((uint32_t)TssIoMapBase() > TssLimit(),
                           "the I/O map base lies within the segment, so stray bytes decide "
                           "which ports a user program may drive");

    /* The refusals of the accessor, the table being numbered from one. */
    KernelPrivilegeRequire((TssInterruptStack(0U) == 0U) && (TssInterruptStack(8U) == 0U),
                           "an interrupt stack table entry outside the seven was reported");
}

/*
 * A vector well clear of everything the machine uses, upon which the interrupt
 * stack table may be exercised without disturbing a handler that matters.
 */
#define KERNEL_PRIVILEGE_PROBE_VECTOR 200U

/* Where the trap frame of the probe was built, which is the stack the processor
 * switched to — the frame being the first thing pushed upon it. */
static uint64_t KernelPrivilegeProbeFrame;

static void KernelPrivilegeProbeHandler(TrapFrame *frame)
{
    KernelPrivilegeProbeFrame = (uint64_t)(uintptr_t)frame;
}

/* Whether an address lies within the stack the double fault is delivered upon. */
static bool KernelWithinDoubleFaultStack(uint64_t address)
{
    const uint64_t top = TssInterruptStack(TSS_IST_DOUBLE_FAULT);

    if (top == 0U)
    {
        return false;
    }

    return (address < top) && (address >= (top - TSS_INTERRUPT_STACK_SIZE));
}

/*
 * Raises the probe vector and reports where the trap frame was built.
 *
 * The address of the frame is the evidence, because the frame is the first thing
 * placed upon whatever stack the processor selected. Nothing else in the machine
 * observes that selection: the stack pointer the frame records is the one that
 * was in use before the exception, and is the same under either arrangement.
 */
static uint64_t KernelRaiseProbeVector(void)
{
    KernelPrivilegeProbeFrame = 0U;

    __asm__ __volatile__("int $200" : : : "memory");

    return KernelPrivilegeProbeFrame;
}

/* Asserts that the double fault is delivered upon its own stack. */
static void KernelVerifyPrivilegeInterruptStacks(void)
{
    uint64_t without_stack;
    uint64_t with_stack;


    KernelPrivilegeRequire(IdtGateStack(EXCEPTION_DOUBLE_FAULT) ==
                               (uint8_t)TSS_IST_DOUBLE_FAULT,
                           "the double fault's gate selects no interrupt stack, so a fault "
                           "upon a bad stack would triple fault silently");
    KernelPrivilegeRequire(IdtGateStack(14U) == 0U,
                           "the page fault was given an interrupt stack, which it must not "
                           "have: it is taken often and its handler may itself fault");

    /* An entry above the seven the architecture provides is refused rather than
     * truncated into one that belongs to something else. */
    KernelPrivilegeRequire(!IdtSetGateStack(EXCEPTION_DOUBLE_FAULT, 8U),
                           "an interrupt stack table entry above seven was accepted");
    KernelPrivilegeRequire(IdtGateStack(EXCEPTION_DOUBLE_FAULT) ==
                               (uint8_t)TSS_IST_DOUBLE_FAULT,
                           "a refused assignment nevertheless altered the gate");

    /*
     * The mechanism itself, exercised rather than inspected.
     *
     * Everything above establishes that the gate names an entry and that the
     * entry names a stack. It does not establish that the processor uses it, and
     * that is the property the whole arrangement exists for: a task state
     * segment whose descriptor the processor rejected, or a table register that
     * was never loaded, would satisfy every assertion above and switch no stack.
     *
     * The double fault cannot be raised to find out — its handler is fatal by
     * design, and a self-test that halted the machine to prove a point would be
     * of no use. A vector clear of everything else is given the same entry
     * instead, raised, and the address at which its trap frame was built
     * compared against the extent of that stack.
     *
     * It is raised twice, with the entry and without it, and the two results
     * must differ. One measurement alone proves nothing: a frame that happened
     * to be built within the range would satisfy it, and so would a machine
     * where every exception used the same stack for some other reason. The pair
     * establishes that the interrupt stack table field is what caused the
     * difference.
     */
    InterruptRegisterHandler((uint8_t)KERNEL_PRIVILEGE_PROBE_VECTOR,
                             KernelPrivilegeProbeHandler, "privilege probe");

    without_stack = KernelRaiseProbeVector();

    KernelPrivilegeRequire(!IdtSetGateStack((uint8_t)KERNEL_PRIVILEGE_PROBE_VECTOR, 8U),
                           "an out-of-range entry was accepted upon the probe vector");
    KernelPrivilegeRequire(IdtSetGateStack((uint8_t)KERNEL_PRIVILEGE_PROBE_VECTOR,
                                           (uint8_t)TSS_IST_DOUBLE_FAULT),
                           "an interrupt stack could not be attached to the probe vector");

    with_stack = KernelRaiseProbeVector();

    (void)IdtSetGateStack((uint8_t)KERNEL_PRIVILEGE_PROBE_VECTOR, 0U);
    InterruptUnregisterHandler((uint8_t)KERNEL_PRIVILEGE_PROBE_VECTOR);

    KernelPrivilegeRequire((without_stack != 0U) && (with_stack != 0U),
                           "the probe vector was not delivered");
    KernelPrivilegeRequire(!KernelWithinDoubleFaultStack(without_stack),
                           "a vector selecting no interrupt stack was nevertheless delivered "
                           "upon one");
    KernelPrivilegeRequire(KernelWithinDoubleFaultStack(with_stack),
                           "a vector selecting an interrupt stack was not delivered upon it, "
                           "so the processor is not reading the task state segment");
    KernelPrivilegeRequire(with_stack != without_stack,
                           "the interrupt stack table entry made no difference to where the "
                           "frame was built");

    /* The gate is left as it was found, so that a later boot-time test meets the
     * table this one did. */
    KernelPrivilegeRequire(IdtGateStack((uint8_t)KERNEL_PRIVILEGE_PROBE_VECTOR) == 0U,
                           "the probe vector was left holding an interrupt stack");
}

/* Asserts the three registers that configure the system-call transition. */
static void KernelVerifyPrivilegeSystemCallConfiguration(void)
{
    KernelPrivilegeRequire(SyscallIsEnabled(),
                           "IA32_EFER.SCE is clear, so SYSCALL is an invalid opcode");
    KernelPrivilegeRequire((SyscallLstar() != 0U) &&
                               (SyscallLstar() == SyscallEntryAddress()),
                           "IA32_LSTAR does not hold the address of the entry point");

    /*
     * The four selectors the processor will derive. They are compared against
     * the selectors this kernel means it to derive, which is a different
     * assertion from comparing IA32_STAR against what was written into it: the
     * arithmetic is the processor's, and it is the arithmetic that a misordered
     * table defeats.
     */
    KernelPrivilegeRequire(SyscallDerivedKernelCode() == GDT_KERNEL_CODE_SELECTOR,
                           "SYSCALL would load a code selector that is not the kernel's");
    KernelPrivilegeRequire(SyscallDerivedKernelStack() == GDT_KERNEL_DATA_SELECTOR,
                           "SYSCALL would load a stack selector that is not the kernel's");
    KernelPrivilegeRequire(SyscallDerivedUserCode() ==
                               (GDT_USER_CODE_SELECTOR | GDT_REQUESTED_PRIVILEGE_USER),
                           "SYSRET would return to a code selector that is not the user's "
                           "64-bit segment at privilege level 3");
    KernelPrivilegeRequire(SyscallDerivedUserStack() ==
                               (GDT_USER_DATA_SELECTOR | GDT_REQUESTED_PRIVILEGE_USER),
                           "SYSRET would return to a stack selector that is not the user's "
                           "data segment at privilege level 3");

    /*
     * The mask. The interrupt flag is the bit whose omission is a hole rather
     * than an inconvenience: SYSCALL performs no stack switch, so the first
     * instruction of the handler runs at privilege level 0 upon the caller's
     * stack, and an interrupt delivered there would push a frame onto memory the
     * caller controls.
     */
    KernelPrivilegeRequire((SyscallFmask() & RFLAGS_INTERRUPT_ENABLE) != 0U,
                           "IA32_FMASK does not clear the interrupt flag, so the kernel "
                           "would be entered interruptible upon a user stack");
    KernelPrivilegeRequire((SyscallFmask() & RFLAGS_DIRECTION) != 0U,
                           "IA32_FMASK does not clear the direction flag, which the ABI "
                           "requires clear at a function's entry");
    KernelPrivilegeRequire((SyscallFmask() & RFLAGS_TRAP) != 0U,
                           "IA32_FMASK does not clear the trap flag, so a caller could "
                           "single-step the kernel");
    KernelPrivilegeRequire((SyscallFmask() & RFLAGS_NESTED_TASK) != 0U,
                           "IA32_FMASK does not clear the nested-task flag, which alters "
                           "what IRET does");
    KernelPrivilegeRequire((SyscallFmask() & RFLAGS_ALIGNMENT_CHECK) != 0U,
                           "IA32_FMASK does not clear the alignment-check flag, which is "
                           "half of what makes a supervisor access to a user page fault");
}

/*
 * Exercises the transition, which is the only part of this sub-task that can be
 * made to happen rather than merely inspected.
 *
 * SYSCALL is executed from privilege level 0. It raises no privilege, there
 * being none to raise, but it performs every other part of the transition: it
 * loads CS and SS from IA32_STAR, transfers to IA32_LSTAR, saves the return
 * address in RCX and the flags in R11, and clears the bits IA32_FMASK names. The
 * entry point records what it was given, and this compares that record against
 * what the configuration said would happen.
 *
 * It is performed twice, with the interrupt flag clear and then set, because the
 * assertion that the flag was cleared upon entry says nothing whatever if the
 * flag was already clear.
 */
static void KernelVerifyPrivilegeTransition(void)
{
    const uint64_t before = SyscallEntries();
    const bool interrupts_were_enabled = InterruptsAreEnabled();

    if (!SyscallIsEnabled())
    {
        KernelPrivilegeRequire(false,
                               "the transition was not exercised: SYSCALL is not enabled");
        return;
    }

    KernelExecuteSystemCall();

    KernelPrivilegeRequire(SyscallEntries() == (before + 1U),
                           "SYSCALL did not reach the entry point IA32_LSTAR names");
    KernelPrivilegeRequire(SyscallObservedCode() == GDT_KERNEL_CODE_SELECTOR,
                           "the processor loaded a code selector that is not the kernel's");
    KernelPrivilegeRequire(SyscallObservedStack() == GDT_KERNEL_DATA_SELECTOR,
                           "the processor loaded a stack selector that is not the kernel's");
    KernelPrivilegeRequire(!InterruptsAreEnabled(),
                           "the interrupt flag was set upon return although it was clear "
                           "before, so the flags were not restored");

    /*
     * The same again with the flag set, which is the state a user program will
     * be in. The entry point must observe it clear; that is IA32_FMASK working,
     * and it is not observable at all in the first pass.
     */
    __asm__ __volatile__("sti" : : : "memory");

    KernelExecuteSystemCall();

    KernelPrivilegeRequire(SyscallEntries() == (before + 2U),
                           "the second SYSCALL did not reach the entry point");
    KernelPrivilegeRequire((SyscallObservedFlags() & RFLAGS_INTERRUPT_ENABLE) == 0U,
                           "the interrupt flag was still set within the handler, so the "
                           "kernel was entered interruptible");
    KernelPrivilegeRequire(InterruptsAreEnabled(),
                           "the interrupt flag was not restored upon return, so the flags "
                           "saved in R11 were lost");

    if (!interrupts_were_enabled)
    {
        __asm__ __volatile__("cli" : : : "memory");
    }
}

void KernelVerifyPrivilege(void)
{
    KernelPrivilegeSucceeded = true;

    KernelVerifyPrivilegeDescriptors();
    KernelVerifyPrivilegeTaskSegment();
    KernelVerifyPrivilegeInterruptStacks();
    KernelVerifyPrivilegeSystemCallConfiguration();
    KernelVerifyPrivilegeTransition();

    KernelWriteString(KernelPrivilegeSucceeded ? "Privilege self-test passed.\n"
                                               : "Privilege self-test FAILED.\n");
}
