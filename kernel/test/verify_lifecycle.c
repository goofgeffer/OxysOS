/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/verify_lifecycle.c
 * Purpose: Asserts the four calls of sub-task 6.11 — fork, execve, exit and
 *          wait — first upon the structures alone and then by running a program
 *          that makes a child, replaces the child's program with one read from a
 *          volume, and collects what it ended with.
 * Key functions: KernelVerifyFork, KernelVerifyLifecycle.
 * References:
 *   - docs/design/PROCESS.md, Sections 12 and 13: the design of these four
 *     calls, and these assertions paired with what each would catch.
 *   - docs/design/MEMORY-LAYOUT.md, Section 14: the copy-on-write cloning a fork
 *     is built upon.
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2A
 *     and 2B: the encodings the two programs are assembled from. `MOV r32,
 *     imm32` is B8+rd; `TEST r/m64, r64` is REX.W + 85 /r; `JZ rel8` is 74 cb,
 *     whose displacement is measured from the instruction following it; `MOV
 *     r32, r/m32` is 8B /r, and a ModR/M byte with mod 00 and r/m 111 names
 *     [RDI] with no displacement; `XOR r/m32, r32` is 31 /r; `SYSCALL` is 0F 05;
 *     and `UD2` is 0F 0B, which is the undefined instruction the architecture
 *     guarantees will raise vector 6.
 *   - Tool Interface Standard, Executable and Linking Format, and the ELF-64
 *     Object File Format, version 1.5 draft 2: the file-header and
 *     program-header field offsets the images are composed at. They are not
 *     restated here; <oxys/elf.h> names every one of them and this file uses
 *     those names, so a fixture cannot agree with a mistaken decoder by
 *     restating its mistake.
 *   - System V Application Binary Interface, AMD64 supplement: the machine is
 *     62, and the arguments of a system call are in RDI, RSI and RDX, the fourth
 *     moving to R10 because SYSCALL destroys RCX.
 *
 * Two tests, and the division is the one sub-task 6.10 used.
 *
 *   The first forks a process and examines what was made, without running
 *   anything. A failure there is a failure of the fork.
 *
 *   The second runs a program that calls all four, and a failure there could be
 *   a failure of the fork, of the loader, of the filesystem, of the system-call
 *   path, of the switch or of the descent — every one of which this puts
 *   together at once. Asserting the cheap half first is what makes the second
 *   failure interpretable when it comes.
 *
 * The programs are written here, in machine code.
 *
 *   There is no compiler for user code in this build, so both programs are
 *   assembled by hand into ELF images composed for the purpose, exactly as
 *   kernel/test/verify_usermode.c does. The second is written to a volume
 *   through the filesystem of Phase 5 before the first is run, because `execve`
 *   loads from a path and a path must lead to something.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/process.h>
#include <oxys/elf.h>
#include <oxys/memory.h>
#include <oxys/paging.h>
#include <oxys/syscall.h>
#include <oxys/testvolume.h>
#include <oxys/vfs.h>
#include <oxys/block.h>

static bool KernelLifecycleSucceeded;

static void KernelLifecycleRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        KernelLifecycleSucceeded = false;
    }
}

/* ------------------------------------------------------------ the two images */

/*
 * Where each program's text and data stand, and where within the data the three
 * things the parent needs are placed.
 *
 * The addresses are the same for both programs deliberately. The child is loaded
 * over an address space cloned from the parent's, so an image that occupied
 * different addresses would leave the parent's pages mapped beneath the child's
 * — and this way the load must displace them, which is what the release of the
 * old address space in ProcessExecute is there to do.
 */
#define KERNEL_LIFECYCLE_TEXT_ADDRESS UINT64_C(0x0000000000401000)
#define KERNEL_LIFECYCLE_DATA_ADDRESS UINT64_C(0x0000000000402000)
#define KERNEL_LIFECYCLE_TEXT_OFFSET  0x1000U
#define KERNEL_LIFECYCLE_DATA_OFFSET  0x2000U
#define KERNEL_LIFECYCLE_IMAGE_BYTES  0x3000U
#define KERNEL_LIFECYCLE_DATA_BYTES   1024U

/* Within the data segment: the greeting at the start, the path a quarter of the
 * way in, and the two quadwords `wait` writes a child's status into. */
#define KERNEL_LIFECYCLE_PATH_OFFSET    256U
#define KERNEL_LIFECYCLE_STATUS_OFFSET  512U
#define KERNEL_LIFECYCLE_STATUS2_OFFSET 640U

#define KERNEL_LIFECYCLE_PATH_ADDRESS \
    (KERNEL_LIFECYCLE_DATA_ADDRESS + KERNEL_LIFECYCLE_PATH_OFFSET)
#define KERNEL_LIFECYCLE_STATUS_ADDRESS \
    (KERNEL_LIFECYCLE_DATA_ADDRESS + KERNEL_LIFECYCLE_STATUS_OFFSET)
#define KERNEL_LIFECYCLE_STATUS2_ADDRESS \
    (KERNEL_LIFECYCLE_DATA_ADDRESS + KERNEL_LIFECYCLE_STATUS2_OFFSET)

/*
 * The status the exec'd program ends with, and the one the parent ends with when
 * `execve` failed.
 *
 * They are different numbers because the parent ends with whatever `wait` gave
 * it, so the number the kernel reads at the end says *which* path the child
 * took. A single number would have said only that something ended.
 */
#define KERNEL_LIFECYCLE_CHILD_STATUS 7U
#define KERNEL_LIFECYCLE_EXEC_FAILED  99U

/* The path the second program is written to and the first program names. */
static const char KernelLifecyclePath[] = "/prog";

static const char KernelLifecycleParentGreeting[] =
    "  The first program is running at privilege level 3 and is about to fork.\n";

static const char KernelLifecycleChildGreeting[] =
    "  The second program was loaded from a file by execve and is the child.\n";

static uint8_t KernelLifecycleParentImage[KERNEL_LIFECYCLE_IMAGE_BYTES];
static uint8_t KernelLifecycleChildImage[KERNEL_LIFECYCLE_IMAGE_BYTES];


static size_t KernelLifecycleLength(const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0')
    {
        ++length;
    }

    return length;
}

static void KernelLifecyclePut16(uint8_t *image, uint64_t at, uint16_t value)
{
    image[at] = (uint8_t)(value & 0xFFU);
    image[at + 1U] = (uint8_t)((value >> 8) & 0xFFU);
}

static void KernelLifecyclePut32(uint8_t *image, uint64_t at, uint32_t value)
{
    for (uint64_t index = 0U; index < 4U; ++index)
    {
        image[at + index] = (uint8_t)((value >> (index * 8U)) & 0xFFU);
    }
}

static void KernelLifecyclePut64(uint8_t *image, uint64_t at, uint64_t value)
{
    for (uint64_t index = 0U; index < 8U; ++index)
    {
        image[at + index] = (uint8_t)((value >> (index * 8U)) & 0xFFU);
    }
}

static void KernelLifecycleProgramHeader(uint8_t *image, uint64_t at, uint32_t flags,
                                         uint64_t file_offset, uint64_t address,
                                         uint64_t size)
{
    KernelLifecyclePut32(image, at + 0U, ELF_SEGMENT_LOAD);
    KernelLifecyclePut32(image, at + 4U, flags);
    KernelLifecyclePut64(image, at + 8U, file_offset);
    KernelLifecyclePut64(image, at + 16U, address);
    KernelLifecyclePut64(image, at + 24U, address);
    KernelLifecyclePut64(image, at + 32U, size);
    KernelLifecyclePut64(image, at + 40U, size);
    KernelLifecyclePut64(image, at + 48U, PAGE_SIZE);
}

/* The file header and the two program headers, which are the same for both
 * programs but for how much text each has. */
static void KernelLifecycleWrapper(uint8_t *image, uint64_t text_bytes)
{
    image[0] = 0x7FU;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = (uint8_t)ELF_CLASS_64;
    image[5] = (uint8_t)ELF_DATA_LITTLE_ENDIAN;
    image[6] = (uint8_t)ELF_VERSION_CURRENT;

    KernelLifecyclePut16(image, 16U, (uint16_t)ELF_TYPE_EXECUTABLE);
    KernelLifecyclePut16(image, 18U, (uint16_t)ELF_MACHINE_X86_64);
    KernelLifecyclePut32(image, 20U, ELF_VERSION_CURRENT);
    KernelLifecyclePut64(image, 24U, KERNEL_LIFECYCLE_TEXT_ADDRESS);
    KernelLifecyclePut64(image, 32U, ELF_HEADER_BYTES);
    KernelLifecyclePut16(image, 52U, (uint16_t)ELF_HEADER_BYTES);
    KernelLifecyclePut16(image, 54U, (uint16_t)ELF_PROGRAM_HEADER_BYTES);
    KernelLifecyclePut16(image, 56U, 2U);

    KernelLifecycleProgramHeader(image, ELF_HEADER_BYTES,
                                 ELF_SEGMENT_READ | ELF_SEGMENT_EXECUTE,
                                 KERNEL_LIFECYCLE_TEXT_OFFSET,
                                 KERNEL_LIFECYCLE_TEXT_ADDRESS, text_bytes);
    KernelLifecycleProgramHeader(image, ELF_HEADER_BYTES + ELF_PROGRAM_HEADER_BYTES,
                                 ELF_SEGMENT_READ | ELF_SEGMENT_WRITE,
                                 KERNEL_LIFECYCLE_DATA_OFFSET,
                                 KERNEL_LIFECYCLE_DATA_ADDRESS,
                                 KERNEL_LIFECYCLE_DATA_BYTES);
}

/* ------------------------------------------------------------ the instructions
 *
 * Each emitter writes one instruction at the cursor and advances it. They are
 * named for the instruction rather than for the byte, because a sequence written
 * as bytes is a sequence nobody can check against what it was meant to say.
 */

static void KernelLifecycleMoveImmediate(uint8_t *image, uint64_t *at, uint8_t opcode,
                                         uint32_t value)
{
    image[*at] = opcode;
    KernelLifecyclePut32(image, *at + 1U, value);
    *at += 5U;
}

/* mov eax, imm32 — the call number. */
#define KERNEL_LIFECYCLE_MOVE_EAX 0xB8U
/* mov edi, imm32 — the first argument. */
#define KERNEL_LIFECYCLE_MOVE_EDI 0xBFU
/* mov esi, imm32 — the second. */
#define KERNEL_LIFECYCLE_MOVE_ESI 0xBEU
/* mov edx, imm32 — the third. */
#define KERNEL_LIFECYCLE_MOVE_EDX 0xBAU

static void KernelLifecycleBytes(uint8_t *image, uint64_t *at, uint8_t first,
                                 uint8_t second)
{
    image[*at] = first;
    image[*at + 1U] = second;
    *at += 2U;
}

/* syscall: 0F 05. */
static void KernelLifecycleSyscall(uint8_t *image, uint64_t *at)
{
    KernelLifecycleBytes(image, at, 0x0FU, 0x05U);
}

/* ud2: 0F 0B. Written after every call that does not return, so that a call
 * which did return would raise a fault the kernel names rather than run into
 * whatever bytes follow. */
static void KernelLifecycleUndefined(uint8_t *image, uint64_t *at)
{
    KernelLifecycleBytes(image, at, 0x0FU, 0x0BU);
}

/* The write of a string held in the program's own data segment. */
static void KernelLifecycleEmitWrite(uint8_t *image, uint64_t *at, uint64_t address,
                                     uint32_t length)
{
    KernelLifecycleMoveImmediate(image, at, KERNEL_LIFECYCLE_MOVE_EAX, SYSCALL_WRITE);
    KernelLifecycleMoveImmediate(image, at, KERNEL_LIFECYCLE_MOVE_EDI, 1U);
    KernelLifecycleMoveImmediate(image, at, KERNEL_LIFECYCLE_MOVE_ESI, (uint32_t)address);
    KernelLifecycleMoveImmediate(image, at, KERNEL_LIFECYCLE_MOVE_EDX, length);
    KernelLifecycleSyscall(image, at);
}

/* exit(status), which does not return. */
static void KernelLifecycleEmitExit(uint8_t *image, uint64_t *at, uint32_t status)
{
    KernelLifecycleMoveImmediate(image, at, KERNEL_LIFECYCLE_MOVE_EAX, SYSCALL_EXIT);
    KernelLifecycleMoveImmediate(image, at, KERNEL_LIFECYCLE_MOVE_EDI, status);
    KernelLifecycleSyscall(image, at);
    KernelLifecycleUndefined(image, at);
}

/* Copies a string into an image's data segment at a given offset within it. */
static void KernelLifecycleText(uint8_t *image, uint64_t offset, const char *text)
{
    const size_t length = KernelLifecycleLength(text);

    for (size_t index = 0U; index <= length; ++index)
    {
        image[KERNEL_LIFECYCLE_DATA_OFFSET + offset + index] = (uint8_t)text[index];
    }
}

/*
 * The first program.
 *
 *   write(1, greeting, len)
 *   if (fork() == 0) { execve("/prog", 0, 0); exit(99); }
 *   wait(&status);
 *   if (fork() == 0) { ud2; }
 *   wait(&second);
 *   exit(status);
 *
 * The second child exists for one reason and it is stated where it is emitted:
 * the two ways out of privilege level 3 leave the kernel holding different
 * segment bases, and a program whose children all ended by asking would exercise
 * only one of them.
 *
 * The parent ends with the status its child ended with, and that single number
 * is what the kernel reads at the end. It is not an economy: a status that
 * travelled from a program loaded from a file, through its exit, through its
 * parent's wait, and out of its parent's exit has passed through all four calls,
 * and no one of them could have produced it alone.
 *
 * The branch is the only thing here that must be patched. The displacement of a
 * short conditional jump is relative to the instruction after it, and the
 * instruction after it is not written until the parent's half is done — so the
 * byte is left blank, its position remembered, and filled in when the child's
 * half begins.
 */
static void KernelLifecycleEmitForkBranch(uint8_t *image, uint64_t *at,
                                          uint64_t *displacement_at)
{
    KernelLifecycleMoveImmediate(image, at, KERNEL_LIFECYCLE_MOVE_EAX, SYSCALL_FORK);
    KernelLifecycleSyscall(image, at);

    /* test rax, rax — 48 85 C0 — and jz, whose displacement is patched once the
     * instruction it jumps over has been written. */
    image[*at] = 0x48U;
    image[*at + 1U] = 0x85U;
    image[*at + 2U] = 0xC0U;
    *at += 3U;
    image[*at] = 0x74U;
    *displacement_at = *at + 1U;
    *at += 2U;
}

/* The displacement of a short jump is measured from the instruction after it,
 * which is the byte one past the displacement itself. */
static void KernelLifecyclePatch(uint8_t *image, uint64_t displacement_at,
                                 uint64_t target)
{
    image[displacement_at] = (uint8_t)(target - (displacement_at + 1U));
}

/* wait(&status). */
static void KernelLifecycleEmitWait(uint8_t *image, uint64_t *at, uint64_t address)
{
    KernelLifecycleMoveImmediate(image, at, KERNEL_LIFECYCLE_MOVE_EAX, SYSCALL_WAIT);
    KernelLifecycleMoveImmediate(image, at, KERNEL_LIFECYCLE_MOVE_EDI,
                                 (uint32_t)address);
    KernelLifecycleSyscall(image, at);
}

static void KernelLifecycleComposeParent(void)
{
    uint64_t at = KERNEL_LIFECYCLE_TEXT_OFFSET;
    uint64_t first_branch;
    uint64_t second_branch;
    const uint32_t greeting =
        (uint32_t)KernelLifecycleLength(KernelLifecycleParentGreeting);

    for (uint64_t index = 0U; index < KERNEL_LIFECYCLE_IMAGE_BYTES; ++index)
    {
        KernelLifecycleParentImage[index] = 0U;
    }

    KernelLifecycleEmitWrite(KernelLifecycleParentImage, &at,
                             KERNEL_LIFECYCLE_DATA_ADDRESS, greeting);

    /* The first fork, whose child becomes the program upon the volume. */
    KernelLifecycleEmitForkBranch(KernelLifecycleParentImage, &at, &first_branch);
    KernelLifecycleEmitWait(KernelLifecycleParentImage, &at,
                            KERNEL_LIFECYCLE_STATUS_ADDRESS);

    /* The second, whose child ends by faulting instead of by asking. */
    KernelLifecycleEmitForkBranch(KernelLifecycleParentImage, &at, &second_branch);
    KernelLifecycleEmitWait(KernelLifecycleParentImage, &at,
                            KERNEL_LIFECYCLE_STATUS2_ADDRESS);

    /* mov edi, STATUS; mov edi, [rdi] — 8B 3F — the status the kernel wrote for
     * the first child, which is what this program ends with. */
    KernelLifecycleMoveImmediate(KernelLifecycleParentImage, &at,
                                 KERNEL_LIFECYCLE_MOVE_EDI,
                                 (uint32_t)KERNEL_LIFECYCLE_STATUS_ADDRESS);
    KernelLifecycleBytes(KernelLifecycleParentImage, &at, 0x8BU, 0x3FU);

    KernelLifecycleMoveImmediate(KernelLifecycleParentImage, &at,
                                 KERNEL_LIFECYCLE_MOVE_EAX, SYSCALL_EXIT);
    KernelLifecycleSyscall(KernelLifecycleParentImage, &at);
    KernelLifecycleUndefined(KernelLifecycleParentImage, &at);

    /* The first child: become the other program, or say that it could not. */
    KernelLifecyclePatch(KernelLifecycleParentImage, first_branch, at);

    KernelLifecycleMoveImmediate(KernelLifecycleParentImage, &at,
                                 KERNEL_LIFECYCLE_MOVE_EAX, SYSCALL_EXECVE);
    KernelLifecycleMoveImmediate(KernelLifecycleParentImage, &at,
                                 KERNEL_LIFECYCLE_MOVE_EDI,
                                 (uint32_t)KERNEL_LIFECYCLE_PATH_ADDRESS);
    /* xor esi, esi and xor edx, edx: no arguments and no environment, which is
     * what this kernel's execve accepts and nothing else. */
    KernelLifecycleBytes(KernelLifecycleParentImage, &at, 0x31U, 0xF6U);
    KernelLifecycleBytes(KernelLifecycleParentImage, &at, 0x31U, 0xD2U);
    KernelLifecycleSyscall(KernelLifecycleParentImage, &at);

    KernelLifecycleEmitExit(KernelLifecycleParentImage, &at,
                            KERNEL_LIFECYCLE_EXEC_FAILED);

    /*
     * The second child, which does nothing but raise the fault that ends it.
     *
     * It is here because the two ways out of privilege level 3 leave the kernel
     * in different states and only one of them was otherwise exercised. A child
     * that ends by `exit` is in the kernel by SYSCALL, which exchanged GS.base
     * for the per-processor block; one that ends by faulting is in the kernel by
     * an interrupt, which exchanged nothing. Both return to a parent suspended
     * inside its own system call, and the parent's return by SYSRET performs the
     * exchange either way — so a kernel that did not settle GS.base upon a
     * switch would hand the block to the parent at privilege level 3 and lose it
     * at the parent's next SYSCALL. That is the parent's `exit`, three
     * instructions later.
     */
    KernelLifecyclePatch(KernelLifecycleParentImage, second_branch, at);
    KernelLifecycleUndefined(KernelLifecycleParentImage, &at);

    KernelLifecycleWrapper(KernelLifecycleParentImage,
                           at - KERNEL_LIFECYCLE_TEXT_OFFSET);
    KernelLifecycleText(KernelLifecycleParentImage, 0U, KernelLifecycleParentGreeting);
    KernelLifecycleText(KernelLifecycleParentImage, KERNEL_LIFECYCLE_PATH_OFFSET,
                        KernelLifecyclePath);
}

/*
 * The second program, which is what `execve` loads.
 *
 *   write(1, greeting, len)
 *   exit(7)
 *
 * It is deliberately not the first program. A program that exec'd itself would
 * end with the same status by either path, and the number the kernel reads could
 * then say nothing about whether the file had been read at all.
 */
static void KernelLifecycleComposeChild(void)
{
    uint64_t at = KERNEL_LIFECYCLE_TEXT_OFFSET;
    const uint32_t greeting =
        (uint32_t)KernelLifecycleLength(KernelLifecycleChildGreeting);

    for (uint64_t index = 0U; index < KERNEL_LIFECYCLE_IMAGE_BYTES; ++index)
    {
        KernelLifecycleChildImage[index] = 0U;
    }

    KernelLifecycleEmitWrite(KernelLifecycleChildImage, &at,
                             KERNEL_LIFECYCLE_DATA_ADDRESS, greeting);
    KernelLifecycleEmitExit(KernelLifecycleChildImage, &at,
                            KERNEL_LIFECYCLE_CHILD_STATUS);

    KernelLifecycleWrapper(KernelLifecycleChildImage,
                           at - KERNEL_LIFECYCLE_TEXT_OFFSET);
    KernelLifecycleText(KernelLifecycleChildImage, 0U, KernelLifecycleChildGreeting);
}

static uint64_t KernelLifecycleTextBytes(const uint8_t *image)
{
    uint64_t bytes = 0U;

    for (uint64_t index = 0U; index < 8U; ++index)
    {
        bytes |= ((uint64_t)image[ELF_HEADER_BYTES + 32U + index]) << (index * 8U);
    }

    return bytes;
}

/* ------------------------------------------------- the fork, without running */

void KernelVerifyFork(void)
{
    Process *parent;
    Process *child;
    ElfImage image;
    SyscallFrame frame;
    uint64_t stack;
    uint64_t child_id;
    int64_t status = 0;
    const size_t processes_before = ProcessCount();
    const size_t threads_before = ThreadCount();
    const uint64_t clones_before = AddressSpaceCloneCount();

    KernelLifecycleSucceeded = true;

    KernelWriteString("Fork: cloning a process and examining what was made.\n");

    KernelLifecycleComposeParent();
    KernelLifecycleComposeChild();

    KernelLifecycleRequire(KernelLifecycleTextBytes(KernelLifecycleParentImage) > 0U,
                           "the first program assembled to nothing");

    /*
     * No thread is current at this point, every self-test above having given
     * back what it adopted. It is asserted rather than assumed because the
     * collection below depends upon it: a child that cannot be started is what
     * this test collects, and a thread being current is what would let it start.
     */
    KernelLifecycleRequire(ThreadCurrent() == NULL,
                           "a thread was still current when the fork self-test began");

    parent = ProcessCreate("forked", NULL);
    KernelLifecycleRequire(parent != NULL, "a process to fork could not be created");

    if (parent == NULL)
    {
        KernelWriteString("Fork self-test FAILED.\n");
        return;
    }

    if (ElfLoad(&parent->space, KernelLifecycleParentImage,
                KERNEL_LIFECYCLE_IMAGE_BYTES, &image) != ELF_OK)
    {
        ProcessDestroy(parent);
        KernelWriteString("  The composed program did not load.\n");
        KernelWriteString("Fork self-test FAILED.\n");
        return;
    }

    ProcessRecordImage(parent, &image);
    stack = ProcessCreateUserStack(parent);
    KernelLifecycleRequire(stack != 0U, "the process to fork was given no stack");

    /*
     * A frame as the entry path would have saved one. The registers are given
     * values a zeroed frame would not have, so that an inheritance that did not
     * happen is distinguishable from a field that was never written.
     */
    for (uint64_t index = 0U; index < (uint64_t)sizeof frame; ++index)
    {
        ((uint8_t *)&frame)[index] = 0U;
    }

    frame.rax = SYSCALL_FORK;
    frame.rbx = UINT64_C(0x00000000DEADBEEF);
    frame.r12 = UINT64_C(0x000000000BADC0DE);
    frame.rcx = image.entry;
    frame.r11 = UINT64_C(0x202);
    frame.user_stack = stack;

    child = ProcessFork(parent, &frame);
    KernelLifecycleRequire(child != NULL, "the process could not be forked");

    if (child == NULL)
    {
        ProcessDestroy(parent);
        KernelWriteString("Fork self-test FAILED.\n");
        return;
    }

    KernelLifecycleRequire(child != parent, "a fork returned the process it forked");
    KernelLifecycleRequire(child->space.root != parent->space.root,
                           "parent and child share one paging hierarchy");
    KernelLifecycleRequire(child->parent_id == parent->id,
                           "the child does not record its parent");
    KernelLifecycleRequire(child->state == PROCESS_READY,
                           "the child was not left runnable");
    KernelLifecycleRequire(child->thread_count == 1U,
                           "the child was not given exactly one thread");
    KernelLifecycleRequire(child->user_stack_top == parent->user_stack_top,
                           "the child's stack does not stand where its parent's does");
    KernelLifecycleRequire(AddressSpaceCloneCount() == (clones_before + 1U),
                           "the fork did not clone an address space");

    if (child->thread_count == 1U)
    {
        const Thread *const thread = child->threads[0];

        KernelLifecycleRequire(thread->resumes_from_fork,
                               "the child's thread would begin rather than resume");
        KernelLifecycleRequire(thread->resume.rax == 0U,
                               "the child would not see zero returned from fork");
        KernelLifecycleRequire(thread->resume.rbx == frame.rbx,
                               "the child did not inherit its parent's registers");
        KernelLifecycleRequire(thread->resume.r12 == frame.r12,
                               "the child did not inherit its parent's registers");
        KernelLifecycleRequire(thread->entry == image.entry,
                               "the child would not resume where its parent will");
        KernelLifecycleRequire(thread->user_stack == stack,
                               "the child would not resume upon its parent's stack");
    }

    child_id = child->id;

    /* A child of the child: there is none, and saying so is the whole of what a
     * wait can do for a process that has never forked. */
    KernelLifecycleRequire(ProcessWait(child, &status) == 0U,
                           "a process with no children was given one");

    /*
     * And the parent collects it. The child has never run and cannot be started,
     * no thread being current, so this asserts the branch that ends such a child
     * rather than leaving it in the table for ever — which is what a parent that
     * was told it had no children would do, again and again.
     */
    KernelLifecycleRequire(ProcessWait(parent, &status) == child_id,
                           "the parent could not collect the child it forked");
    KernelLifecycleRequire(status == SYSCALL_EINVAL,
                           "a child that could not be started was collected as though "
                           "it had run");
    KernelLifecycleRequire(ProcessById(child_id) == NULL,
                           "a collected child still occupies the table");
    KernelLifecycleRequire(ProcessWait(parent, &status) == 0U,
                           "a child was collected twice");

    ProcessDestroy(parent);

    KernelLifecycleRequire(ProcessCount() == processes_before,
                           "the process table did not return to what it held");
    KernelLifecycleRequire(ThreadCount() == threads_before,
                           "the thread table did not return to what it held");

    KernelWriteString(KernelLifecycleSucceeded ? "Fork self-test passed.\n"
                                               : "Fork self-test FAILED.\n");
}

/* ------------------------------------------------------ all four, by a program */

/*
 * The device the composed EXT2 volume of kernel/test/volume.c is presented
 * through for the duration of this test.
 *
 * It is registered here under a name of its own rather than borrowed from the
 * filesystem self-test above, which withdraws both of its devices before it
 * returns. A name of its own is worth having for a second reason as well: the
 * buffer cache is keyed by device, so a volume recomposed beneath a device the
 * cache still holds blocks for would be read as it was before the recomposition.
 * A device nothing has read from yet has nothing cached.
 */
#define KERNEL_LIFECYCLE_DEVICE "mem3"

/* Writes the second program to the volume, so that a path leads to it. */
static bool KernelLifecyclePublish(void)
{
    uint64_t written = 0U;
    int descriptor;

    descriptor = VfsOpen(KernelLifecyclePath,
                         VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, 0755U);

    if (descriptor < 0)
    {
        KernelWriteString("  The program could not be created upon the volume: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");

        return false;
    }

    if (!VfsWrite(descriptor, KernelLifecycleChildImage, KERNEL_LIFECYCLE_IMAGE_BYTES,
                  &written) ||
        (written != KERNEL_LIFECYCLE_IMAGE_BYTES))
    {
        KernelWriteString("  The program could not be written to the volume.\n");
        (void)VfsClose(descriptor);

        return false;
    }

    return VfsClose(descriptor);
}

void KernelVerifyLifecycle(void)
{
    Process *process;
    Thread *boot;
    Thread *thread;
    ElfImage image;
    uint64_t stack;
    const uint64_t terminations_before = ProcessTerminationCount();
    const uint64_t forks_before = ProcessForkCount();
    const uint64_t executions_before = ProcessExecuteCount();
    const uint64_t copies_before = PagingCopyOnWriteFaultCount();
    const size_t processes_before = ProcessCount();
    const size_t threads_before = ThreadCount();
    BlockDevice *device;
    bool mounted;

    KernelLifecycleSucceeded = true;

    KernelWriteString("Lifecycle: a program forks, its child becomes another program, "
                      "and it is collected.\n");

    /*
     * The volume of memory is composed afresh, presented as a device, mounted
     * for the duration, and every one of those undone before this returns — so
     * that nothing this test leaves behind is present when the machine's own
     * root volume is mounted below.
     */
    KernelComposeVolume();

    device = BlockRegister(KERNEL_LIFECYCLE_DEVICE, &KernelMemoryDeviceOperations, NULL,
                           BLOCK_SIZE_DEFAULT, KERNEL_MEMORY_DEVICE_BLOCKS, false);
    KernelLifecycleRequire(device != NULL,
                           "the device the program is read from could not be "
                           "registered");

    if (device == NULL)
    {
        KernelWriteString("Lifecycle self-test FAILED.\n");
        return;
    }

    mounted = VfsMountVolume(KERNEL_LIFECYCLE_DEVICE, "/", "ext2", false);
    KernelLifecycleRequire(mounted, "the volume the program is read from could not be "
                                    "mounted");

    if (!mounted)
    {
        (void)BlockUnregister(device);
        KernelWriteString("Lifecycle self-test FAILED.\n");
        return;
    }

    if (!KernelLifecyclePublish())
    {
        (void)VfsUnmount("/");
        (void)BlockUnregister(device);
        KernelWriteString("Lifecycle self-test FAILED.\n");
        return;
    }

    process = ProcessCreate("lifecycle", NULL);
    KernelLifecycleRequire(process != NULL, "a process could not be created");

    if (process == NULL)
    {
        (void)VfsUnmount("/");
        (void)BlockUnregister(device);
        KernelWriteString("Lifecycle self-test FAILED.\n");
        return;
    }

    if (ElfLoad(&process->space, KernelLifecycleParentImage,
                KERNEL_LIFECYCLE_IMAGE_BYTES, &image) != ELF_OK)
    {
        ProcessDestroy(process);
        (void)VfsUnmount("/");
        (void)BlockUnregister(device);
        KernelWriteString("  The composed program did not load.\n");
        KernelWriteString("Lifecycle self-test FAILED.\n");
        return;
    }

    ProcessRecordImage(process, &image);
    stack = ProcessCreateUserStack(process);
    boot = ThreadAdoptCurrent("boot");
    thread = ThreadCreate(process, image.entry, stack);

    if ((boot == NULL) || (thread == NULL) || (stack == 0U))
    {
        ProcessDestroy(process);

        if (boot != NULL)
        {
            ThreadDestroy(boot);
        }

        (void)VfsUnmount("/");
        (void)BlockUnregister(device);
        KernelWriteString("  The program could not be prepared.\n");
        KernelWriteString("Lifecycle self-test FAILED.\n");
        return;
    }

    /*
     * And it runs. Between this line and the next the kernel descends to
     * privilege level 3, a program forks, the child is left standing, the parent
     * asks for it, the child resumes at the instruction after its own fork,
     * replaces itself with a program read from a volume, writes, ends, and its
     * parent is given the status and ends with it.
     */
    KernelLifecycleRequire(ThreadStart(thread), "the program could not be started");

    KernelLifecycleRequire(ThreadCurrent() == boot,
                           "the kernel did not resume the thread that started the "
                           "program");

    /*
     * Three programs ended, not one. The first child ended by `exit`, the second
     * by the fault it raised for itself, and the parent by `exit` — and a count
     * of one would mean neither child had run, which is precisely what a `wait`
     * that collected without starting would produce.
     */
    KernelLifecycleRequire(ProcessTerminationCount() == (terminations_before + 3U),
                           "three programs did not end");
    KernelLifecycleRequire(ProcessForkCount() == (forks_before + 2U),
                           "the program did not fork twice");
    KernelLifecycleRequire(ProcessExecuteCount() == (executions_before + 1U),
                           "the child did not replace itself with the program upon the "
                           "volume");

    /*
     * The status the parent ended with is the one its child ended with, and the
     * child's was decided by the program read from the volume. A parent ending
     * with 99 is an execve that was refused; ending with 0 is a wait that
     * collected nothing and wrote nothing.
     */
    KernelLifecycleRequire(process->state == PROCESS_EXITED,
                           "the program's process was not marked as ended");
    KernelLifecycleRequire(process->exit_status == (int64_t)KERNEL_LIFECYCLE_CHILD_STATUS,
                           "the status did not travel from the child's exit through "
                           "its parent's wait");

    /*
     * A copy-on-write fault was taken, and by the kernel on the caller's behalf.
     * `wait` writes its status into a page the fork had made read-only in both
     * hierarchies, so a kernel that had not resolved that fault would have
     * refused the address rather than written it — and the status above would
     * have been the one the program started with.
     */
    KernelLifecycleRequire(PagingCopyOnWriteFaultCount() > copies_before,
                           "no copy-on-write fault was resolved, so the pages were "
                           "never shared");

    ProcessDestroy(process);
    ThreadDestroy(boot);

    KernelLifecycleRequire(ProcessCount() == processes_before,
                           "the process table did not return to what it held");
    KernelLifecycleRequire(ThreadCount() == threads_before,
                           "the thread table did not return to what it held");

    if (!VfsUnmount("/"))
    {
        KernelWriteString("  The volume could not be withdrawn: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        KernelLifecycleSucceeded = false;
    }

    KernelLifecycleRequire(BlockUnregister(device),
                           "the device could not be withdrawn");

    KernelWriteString(KernelLifecycleSucceeded ? "Lifecycle self-test passed.\n"
                                               : "Lifecycle self-test FAILED.\n");
}
