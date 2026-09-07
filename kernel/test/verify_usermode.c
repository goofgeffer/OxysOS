/*
 * File: kernel/test/verify_usermode.c
 * Purpose: Asserts the two transfers of sub-task 6.10: the exchange of one
 *          thread's execution for another's, and the descent to privilege level
 *          3 — by composing a program, loading it, running it, and observing
 *          what it did.
 * Key functions: KernelVerifyContextSwitch, KernelVerifyUserMode.
 * References:
 *   - docs/design/PROCESS.md, Sections 9 and 10: the design of the switch and
 *     the descent, and these assertions paired with what each would catch.
 *
 * The program is written here, in machine code.
 *
 *   There is no compiler for user code in this build and no executable upon any
 *   volume this kernel is required to carry, so the program is twenty-nine bytes
 *   assembled by hand and placed in an ELF image composed for the purpose. It is
 *   loaded by the loader of sub-task 6.8, into an address space made by
 *   sub-task 6.9, and entered by the descent of this one — so a single assertion
 *   at the end of it covers all three.
 *
 *   What the program does is chosen so that its having run is undeniable. It
 *   makes a system call that writes a string only it could name, and then
 *   executes an undefined instruction, which is a fault that belongs to it and
 *   ends it. Either half alone would be weaker: a program that only wrote might
 *   have been simulated by the kernel, and one that only faulted might never
 *   have executed an instruction of its own.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/process.h>
#include <oxys/elf.h>
#include <oxys/memory.h>
#include <oxys/syscall.h>

static bool KernelUserSucceeded;

static void KernelUserRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        KernelUserSucceeded = false;
    }
}

/* ------------------------------------------------- the context switch alone */

static Thread *KernelSwitchBack;
static Thread *KernelSwitchOther;
static volatile uint64_t KernelSwitchVisits;

/*
 * Where the second thread of the switch assertion goes.
 *
 * It runs at privilege level 0 upon its own kernel stack, records that it was
 * reached, and switches back. That is the whole of a context switch demonstrated
 * without a program, an address space or a privilege transition being involved
 * at all — which is why it is asserted first and separately: a failure here is a
 * failure of the switch, and a failure in the next section could be a failure of
 * anything.
 */
static void KernelSwitchSecondThread(void)
{
    ++KernelSwitchVisits;

    /* And back, which is the half that proves the outgoing context was saved
     * rather than merely that the incoming one was restored. */
    ThreadSwitchTo(KernelSwitchOther, KernelSwitchBack);

    /* Not reached: nothing switches to this thread again. */
    KernelPanic("A thread that had ended was resumed.");
}

void KernelVerifyContextSwitch(void)
{
    Thread *boot;
    Thread *other;

    KernelUserSucceeded = true;
    KernelSwitchVisits = 0U;

    KernelWriteString("Switch: asserting the exchange of one thread for another.\n");

    boot = ThreadAdoptCurrent("boot");
    KernelUserRequire(boot != NULL, "the running execution could not be described as a "
                                    "thread");

    if (boot == NULL)
    {
        KernelWriteString("Switch self-test FAILED.\n");
        return;
    }

    KernelUserRequire(ThreadCurrent() == boot,
                      "the adopted thread is not the current one");
    KernelUserRequire(!boot->owns_stack,
                      "the thread describing the boot stack claims to own it");

    other = ThreadCreateKernel(&KernelSwitchSecondThread);
    KernelUserRequire(other != NULL, "a second kernel thread could not be created");

    if (other == NULL)
    {
        ThreadDestroy(boot);
        KernelWriteString("Switch self-test FAILED.\n");
        return;
    }

    KernelSwitchBack = boot;
    KernelSwitchOther = other;

    /*
     * Away, and back. Execution resumes upon the line after the switch, which is
     * the assertion: a switch that restored the incoming thread and lost the
     * outgoing one would never reach it, and the machine would stop here rather
     * than report anything.
     */
    ThreadSwitchTo(boot, other);

    KernelUserRequire(KernelSwitchVisits == 1U,
                      "the second thread did not run, or ran more than once");
    KernelUserRequire(ThreadCurrent() == boot,
                      "the thread that resumed is not the one that was switched away "
                      "from");

    ThreadDestroy(other);

    KernelWriteString(KernelUserSucceeded ? "Switch self-test passed.\n"
                                          : "Switch self-test FAILED.\n");

    ThreadDestroy(boot);
}

/* ------------------------------------------------------ the descent, entire */

/*
 * The program, in machine code.
 *
 *   B8 00 00 00 00        mov  eax, 0          ; the write call
 *   BF 01 00 00 00        mov  edi, 1          ; the descriptor
 *   48 BE <addr>          mov  rsi, <string>   ; the bytes
 *   BA <len> 00 00 00     mov  edx, <length>
 *   0F 05                 syscall
 *   0F 0B                 ud2                  ; a fault that belongs to it
 *
 * The undefined instruction is deliberate and is not a way of stopping. It is
 * vector 6, which the dispositions of sub-task 6.4 classify as a fault belonging
 * to the program at privilege level 3 — so the program ends and the machine
 * carries on, which is exactly the path that could not be taken until this
 * sub-task gave it somewhere to return to.
 */
#define KERNEL_USER_TEXT_ADDRESS UINT64_C(0x0000000000401000)
#define KERNEL_USER_DATA_ADDRESS UINT64_C(0x0000000000402000)
#define KERNEL_USER_TEXT_OFFSET  0x1000U
#define KERNEL_USER_DATA_OFFSET  0x2000U
#define KERNEL_USER_IMAGE_BYTES  0x3000U

static uint8_t KernelUserImage[KERNEL_USER_IMAGE_BYTES];

static const char KernelUserGreeting[] =
    "  A program at privilege level 3 wrote this line through a system call.\n";

static uint64_t KernelUserWrite;

static void KernelUserPut16(uint64_t at, uint16_t value)
{
    KernelUserImage[at] = (uint8_t)(value & 0xFFU);
    KernelUserImage[at + 1U] = (uint8_t)((value >> 8) & 0xFFU);
}

static void KernelUserPut32(uint64_t at, uint32_t value)
{
    for (uint64_t index = 0U; index < 4U; ++index)
    {
        KernelUserImage[at + index] = (uint8_t)((value >> (index * 8U)) & 0xFFU);
    }
}

static void KernelUserPut64(uint64_t at, uint64_t value)
{
    for (uint64_t index = 0U; index < 8U; ++index)
    {
        KernelUserImage[at + index] = (uint8_t)((value >> (index * 8U)) & 0xFFU);
    }
}

static void KernelUserProgramHeader(uint64_t at, uint32_t flags, uint64_t file_offset,
                                    uint64_t address, uint64_t size)
{
    KernelUserPut32(at + 0U, ELF_SEGMENT_LOAD);
    KernelUserPut32(at + 4U, flags);
    KernelUserPut64(at + 8U, file_offset);
    KernelUserPut64(at + 16U, address);
    KernelUserPut64(at + 24U, address);
    KernelUserPut64(at + 32U, size);
    KernelUserPut64(at + 40U, size);
    KernelUserPut64(at + 48U, PAGE_SIZE);
}

/* Assembles the program and wraps it in an ELF image. */
static void KernelUserCompose(void)
{
    uint64_t at = KERNEL_USER_TEXT_OFFSET;
    uint64_t length = 0U;

    for (uint64_t index = 0U; index < KERNEL_USER_IMAGE_BYTES; ++index)
    {
        KernelUserImage[index] = 0U;
    }

    while (KernelUserGreeting[length] != '\0')
    {
        ++length;
    }

    KernelUserImage[0] = 0x7FU;
    KernelUserImage[1] = 'E';
    KernelUserImage[2] = 'L';
    KernelUserImage[3] = 'F';
    KernelUserImage[4] = (uint8_t)ELF_CLASS_64;
    KernelUserImage[5] = (uint8_t)ELF_DATA_LITTLE_ENDIAN;
    KernelUserImage[6] = (uint8_t)ELF_VERSION_CURRENT;

    KernelUserPut16(16U, (uint16_t)ELF_TYPE_EXECUTABLE);
    KernelUserPut16(18U, (uint16_t)ELF_MACHINE_X86_64);
    KernelUserPut32(20U, ELF_VERSION_CURRENT);
    KernelUserPut64(24U, KERNEL_USER_TEXT_ADDRESS);
    KernelUserPut64(32U, ELF_HEADER_BYTES);
    KernelUserPut16(52U, (uint16_t)ELF_HEADER_BYTES);
    KernelUserPut16(54U, (uint16_t)ELF_PROGRAM_HEADER_BYTES);
    KernelUserPut16(56U, 2U);

    KernelUserProgramHeader(ELF_HEADER_BYTES, ELF_SEGMENT_READ | ELF_SEGMENT_EXECUTE,
                            KERNEL_USER_TEXT_OFFSET, KERNEL_USER_TEXT_ADDRESS, 64U);
    KernelUserProgramHeader(ELF_HEADER_BYTES + ELF_PROGRAM_HEADER_BYTES,
                            ELF_SEGMENT_READ | ELF_SEGMENT_WRITE, KERNEL_USER_DATA_OFFSET,
                            KERNEL_USER_DATA_ADDRESS, (uint64_t)sizeof KernelUserGreeting);

    /* mov eax, SYSCALL_WRITE */
    KernelUserImage[at] = 0xB8U;
    KernelUserPut32(at + 1U, (uint32_t)SYSCALL_WRITE);
    at += 5U;

    /* mov edi, 1 */
    KernelUserImage[at] = 0xBFU;
    KernelUserPut32(at + 1U, 1U);
    at += 5U;

    /* mov rsi, the string's address in the program's own space */
    KernelUserImage[at] = 0x48U;
    KernelUserImage[at + 1U] = 0xBEU;
    KernelUserPut64(at + 2U, KERNEL_USER_DATA_ADDRESS);
    at += 10U;

    /* mov edx, length */
    KernelUserImage[at] = 0xBAU;
    KernelUserPut32(at + 1U, (uint32_t)length);
    at += 5U;

    /* syscall */
    KernelUserImage[at] = 0x0FU;
    KernelUserImage[at + 1U] = 0x05U;
    at += 2U;

    /* ud2 */
    KernelUserImage[at] = 0x0FU;
    KernelUserImage[at + 1U] = 0x0BU;

    for (uint64_t index = 0U; index < (uint64_t)sizeof KernelUserGreeting; ++index)
    {
        KernelUserImage[KERNEL_USER_DATA_OFFSET + index] =
            (uint8_t)KernelUserGreeting[index];
    }
}

void KernelVerifyUserMode(void)
{
    Process *process;
    Thread *boot;
    Thread *thread;
    ElfImage image;
    uint64_t stack;
    const uint64_t terminations_before = ProcessTerminationCount();

    KernelUserSucceeded = true;

    KernelWriteString("User mode: composing a program and running it.\n");

    KernelUserCompose();

    process = ProcessCreate("hello", NULL);
    KernelUserRequire(process != NULL, "a process could not be created");

    if (process == NULL)
    {
        KernelWriteString("User mode self-test FAILED.\n");
        return;
    }

    if (ElfLoad(&process->space, KernelUserImage, KERNEL_USER_IMAGE_BYTES, &image) !=
        ELF_OK)
    {
        ProcessDestroy(process);
        KernelWriteString("  The composed program did not load.\n");
        KernelWriteString("User mode self-test FAILED.\n");
        return;
    }

    ProcessRecordImage(process, &image);
    stack = ProcessCreateUserStack(process);
    KernelUserRequire(stack != 0U, "the program was given no stack");

    boot = ThreadAdoptCurrent("boot");
    thread = ThreadCreate(process, image.entry, stack);
    KernelUserRequire(thread != NULL, "a thread could not be created for the program");

    if ((boot == NULL) || (thread == NULL) || (stack == 0U))
    {
        ProcessDestroy(process);

        if (boot != NULL)
        {
            ThreadDestroy(boot);
        }

        KernelWriteString("User mode self-test FAILED.\n");
        return;
    }

    KernelUserWrite = SyscallDispatched();

    /*
     * And it runs. What happens between this line and the next is the whole of
     * the sub-task: the kernel switches to a thread that has never run, that
     * thread descends to privilege level 3, executes instructions of its own,
     * enters the kernel again through the system call path, is returned to
     * privilege level 3 by SYSRET, faults, and is ended — and this line is
     * reached again because there was a thread to return to.
     */
    KernelUserRequire(ThreadStart(thread), "the program could not be started");

    KernelUserRequire(ThreadCurrent() == boot,
                      "the kernel did not resume the thread that started the program");

    /*
     * The program made a system call. This is what says it executed
     * instructions of its own rather than merely having been loaded: the count
     * is kept by the dispatcher, and nothing in this file could have raised it.
     */
    KernelUserRequire(SyscallDispatched() > KernelUserWrite,
                      "no system call arrived, so the program never ran");

    /* And it ended, by the fault it raised for itself. */
    KernelUserRequire(ProcessTerminationCount() == (terminations_before + 1U),
                      "the program was not ended");
    KernelUserRequire(process->state == PROCESS_EXITED,
                      "the program's process was not marked as ended");

    /*
     * The status is the vector it faulted upon, negated. Six is the undefined
     * instruction, which is what the program executed on purpose — so this
     * asserts that the program reached its *last* instruction and not merely its
     * first.
     */
    KernelUserRequire(process->exit_status == -6,
                      "the program did not end upon the instruction it meant to");

    ProcessDestroy(process);
    ThreadDestroy(boot);

    KernelWriteString(KernelUserSucceeded ? "User mode self-test passed.\n"
                                          : "User mode self-test FAILED.\n");
}
