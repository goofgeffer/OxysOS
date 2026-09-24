/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/libc/heap.c
 * Purpose: Asserts the work of sub-task 7.3 — the user-space heap — in the two
 *          halves it divides into: the allocator's policy, which is ordinary C
 *          and is asserted by calling it against a region this file supplies;
 *          and the `brk` system call beneath it, which the kernel cannot call
 *          and which is therefore asserted by a program at privilege level 3.
 * Key functions: KernelVerifyHeap.
 * References:
 *   - docs/design/LIBC.md: the two tables pairing every property
 *     asserted below with the silent failure that assertion exists to catch.
 *   - ISO/IEC 9899:2011, Section 7.22.3: the behaviour the first half asserts.
 *   - libc/include/heap.h: OxysHeapAdopt, which is how the policy is given
 *     memory without a system call, and the census taken of it afterwards.
 *   - kernel/abi/oxys/syscall_abi.h: SYSCALL_BRK and SYSCALL_BREAK_QUERY, which
 *     the composed program exercises.
 *
 * Why half of this test is a program, again.
 *
 *   The reason is the one kernel/test/verify_wrappers.c gives at length: this
 *   kernel cannot execute SYSCALL, the SYSRET that ends its handling of one
 *   returning to privilege level 3 unconditionally. `brk` is a system call, so
 *   every property of it has to be asserted by something that may make one.
 *
 *   The allocator above it is not a system call and is asserted here directly.
 *   That is what libc/include/heap.h names the seam for: the policy asks
 *   OxysHeapExtend for memory and nothing else, so a caller that gives it memory
 *   by OxysHeapAdopt exercises the whole of the policy — the code the library
 *   actually ships — without a system call being reached.
 *
 * **Nothing here may allocate more than the arena holds.**
 *
 *   A request the free list cannot meet is what makes the allocator ask
 *   OxysHeapExtend for more, and this kernel executing that call is a reset
 *   rather than a failed assertion. The arena below is two orders of magnitude
 *   larger than anything this test asks for, and the last assertion made is that
 *   the heap never asked the system for anything — so a change that made it ask
 *   is reported, and a change that made it ask *and* the kernel survive is
 *   impossible. The same hazard has stood since sub-task 7.2: every wrapper in
 *   libc/syscall/calls.c is linked into this image and none may be called.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/proc/process.h>
#include <oxys/exec/elf.h>
#include <oxys/mm/memory.h>
#include <oxys/arch/syscall/syscall.h>

#include <errno.h>
#include <heap.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#include "../program.h"

static bool VerifyHeapSucceeded;

static void VerifyHeapRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED.\n");
        VerifyHeapSucceeded = false;
    }
}

/* -------------------------------------------------------------------------
 * 1. The policy, which is the half that can be called.
 * ------------------------------------------------------------------------- */

/*
 * The region the allocator is given, and the one thing about it that differs
 * from the memory it will have in a program.
 *
 * In a program the heap's memory comes from `brk`, and is therefore storage with
 * no declared type — which ISO/IEC 9899:2011, Section 6.5, paragraph 6, gives the
 * effective type of whatever an lvalue first stores through, so a block header
 * written into it is a defined thing to read back. This array has a declared
 * type, `uint8_t[]`, and the allocator reads its own headers out of it through a
 * different type. That is a property of the *test* and not of the library, and it
 * is recorded here rather than left for a reader to notice: the arrangement is
 * sound in this image because nothing is compiled with link-time optimisation, so
 * no translation unit can see both the declaration here and the accesses in
 * libc/stdlib/heap.c.
 *
 * The alignment is stated rather than hoped for. An arena that began part way
 * through an alignment would be adopted from its first aligned byte — the
 * allocator handles it — and the test's arithmetic about how many bytes it got
 * back would then be off by the adjustment, which is a failing assertion about
 * something that is not wrong.
 */
#define VERIFY_HEAP_ARENA_BYTES 65536U

static _Alignas(OXYS_HEAP_ALIGNMENT) uint8_t VerifyHeapArena[VERIFY_HEAP_ARENA_BYTES];

/*
 * An object that is not an allocation, for the assertions about a pointer this
 * allocator did not hand out.
 *
 * The pointer offered is the *middle* of the array and not its beginning,
 * because `free` looks at the thirty-two bytes before the pointer it is given.
 * Offering the beginning would have it read the bytes before the array, which is
 * a read outside the object and precisely the kind of thing this project does
 * not do deliberately. The bytes it does read are this array's own and are zero,
 * which is neither of the allocator's two marks.
 */
static uint8_t VerifyHeapForeign[128];

static bool VerifyHeapIsAligned(const void *pointer)
{
    return ((uintptr_t)pointer % (uintptr_t)OXYS_HEAP_ALIGNMENT) == 0U;
}

/* Whether the census is internally consistent: the headers, what is handed out
 * and what is free are together exactly the bytes the heap was given. */
static bool VerifyHeapCensusBalances(const OxysHeapCensus *census)
{
    return (census->overhead + census->allocated + census->available) == census->bytes;
}

static void VerifyHeapAdoption(void)
{
    OxysHeapCensus census;

    /*
     * The two refusals first, and before the region that is going to work.
     *
     * A region of nothing and a region too small for one block are both things a
     * caller may offer by accident — the second is what a mistaken sizeof
     * produces — and an allocator that accepted either would build a block whose
     * size is smaller than its own header, after which every walk of the free
     * list reads past the end of the region.
     */
    VerifyHeapRequire(!OxysHeapAdopt(NULL, VERIFY_HEAP_ARENA_BYTES),
                      "a null region was adopted");
    VerifyHeapRequire(!OxysHeapAdopt(VerifyHeapArena, 8U),
                      "a region smaller than one block was adopted");

    OxysHeapInspect(&census);
    VerifyHeapRequire(census.regions == 0U,
                      "a refused region was counted");

    VerifyHeapRequire(OxysHeapAdopt(VerifyHeapArena, VERIFY_HEAP_ARENA_BYTES),
                      "the arena was not adopted");

    OxysHeapInspect(&census);

    VerifyHeapRequire(census.regions == 1U, "the adopted region was not counted");
    VerifyHeapRequire(census.bytes == VERIFY_HEAP_ARENA_BYTES,
                      "the adopted region is not the size it was given as");
    VerifyHeapRequire(census.blocks == 1U,
                      "an adopted region did not become exactly one block");
    VerifyHeapRequire(census.free_blocks == 1U,
                      "the block an adopted region became is not free");
    VerifyHeapRequire(census.allocated == 0U,
                      "a heap that has allocated nothing reports memory in use");
    VerifyHeapRequire(VerifyHeapCensusBalances(&census),
                      "the census of a fresh heap does not balance");
}

static void VerifyHeapAllocation(void)
{
    OxysHeapCensus before;
    OxysHeapCensus after;
    unsigned char *first;
    unsigned char *second;
    unsigned char *third;
    void *empty_a;
    void *empty_b;

    OxysHeapInspect(&before);

    /*
     * A request of zero bytes produces a pointer, and two such requests produce
     * different ones.
     *
     * Section 7.22.3, paragraph 1, makes the first implementation-defined and
     * this library chose the pointer; the second is not optional at all — that
     * paragraph requires every allocation to yield a pointer disjoint from any
     * other object, and an allocator that answered every zero-sized request with
     * one shared address would satisfy every other assertion here.
     */
    empty_a = malloc(0U);
    empty_b = malloc(0U);

    VerifyHeapRequire(empty_a != NULL, "a request of zero bytes returned nothing");
    VerifyHeapRequire(empty_b != NULL,
                      "a second request of zero bytes returned nothing");
    VerifyHeapRequire(empty_a != empty_b,
                      "two requests of zero bytes returned the same pointer");

    free(empty_a);
    free(empty_b);

    first = malloc(100U);
    second = malloc(200U);
    third = malloc(300U);

    VerifyHeapRequire((first != NULL) && (second != NULL) && (third != NULL),
                      "an allocation the arena can hold was refused");

    if ((first == NULL) || (second == NULL) || (third == NULL))
    {
        return;
    }

    VerifyHeapRequire(VerifyHeapIsAligned(first) && VerifyHeapIsAligned(second) &&
                          VerifyHeapIsAligned(third),
                      "an allocation is not aligned for every fundamental type");

    /*
     * The three are disjoint, and that is established by writing to them rather
     * than by comparing their addresses.
     *
     * Comparing addresses catches an allocator that returned the same pointer
     * twice and nothing else. What it does not catch is the commoner defect: a
     * split that left the second block overlapping the tail of the first, so that
     * the addresses differ and the storage does not. Writing a distinct pattern
     * to each and reading all three back afterwards is what distinguishes them.
     */
    (void)memset(first, 0x11, 100U);
    (void)memset(second, 0x22, 200U);
    (void)memset(third, 0x33, 300U);

    VerifyHeapRequire((first[0] == 0x11) && (first[99] == 0x11),
                      "the first allocation did not keep its contents");
    VerifyHeapRequire((second[0] == 0x22) && (second[199] == 0x22),
                      "the second allocation did not keep its contents");
    VerifyHeapRequire((third[0] == 0x33) && (third[299] == 0x33),
                      "the third allocation did not keep its contents");

    OxysHeapInspect(&after);

    VerifyHeapRequire(after.allocated >= (before.allocated + 600U),
                      "the census does not account for what was allocated");
    VerifyHeapRequire(after.blocks > before.blocks,
                      "three allocations from one block did not divide it");
    VerifyHeapRequire(after.splits > before.splits,
                      "no block was divided to meet a request");
    VerifyHeapRequire(VerifyHeapCensusBalances(&after),
                      "the census of a heap in use does not balance");

    free(first);
    free(second);
    free(third);
}

/*
 * The invariant that covers the whole of splitting, coalescing and the size
 * arithmetic at once: a heap with nothing allocated is the heap it started as.
 *
 * Every other assertion here is about one operation. This one is about all of
 * them together, and it is the only assertion in this file that a defect in the
 * size arithmetic cannot hide from: a block whose size is one alignment too small
 * or too large produces a heap that never returns to one block, however
 * plausible each individual allocation looked.
 *
 * The release order is deliberately not the allocation order. Releasing in order
 * exercises one of the two joins — each block meets the one before it — and a
 * coalescence that only ever worked backward would pass. The order below makes
 * the middle block the last released, so it has a free neighbour upon each side
 * and both joins must happen in one insertion.
 */
static void VerifyHeapCoalescing(void)
{
    OxysHeapCensus before;
    OxysHeapCensus after;
    void *blocks[5];

    OxysHeapInspect(&before);

    for (size_t index = 0U; index < 5U; ++index)
    {
        blocks[index] = malloc(64U * (index + 1U));
    }

    for (size_t index = 0U; index < 5U; ++index)
    {
        VerifyHeapRequire(blocks[index] != NULL,
                          "an allocation for the coalescing assertion was refused");
    }

    free(blocks[0]);
    free(blocks[4]);
    free(blocks[1]);
    free(blocks[3]);
    free(blocks[2]);

    OxysHeapInspect(&after);

    VerifyHeapRequire(after.blocks == before.blocks,
                      "a heap with nothing allocated is not the heap it started as");
    VerifyHeapRequire(after.free_blocks == before.free_blocks,
                      "releasing everything left the free list divided");
    VerifyHeapRequire(after.available == before.available,
                      "releasing everything did not give back every byte");
    VerifyHeapRequire(after.largest == before.largest,
                      "the largest request the heap can meet has shrunk");
    VerifyHeapRequire(after.coalescences > before.coalescences,
                      "no blocks were joined");
}

static void VerifyHeapRelease(void)
{
    OxysHeapCensus before;
    OxysHeapCensus after;
    void *block;

    OxysHeapInspect(&before);

    /* A null pointer is no action: 7.22.3.3, paragraph 2. Neither a release nor
     * a refusal, because an allocator that counted it as either would be
     * reporting an event that did not happen. */
    free(NULL);

    OxysHeapInspect(&after);
    VerifyHeapRequire((after.releases == before.releases) &&
                          (after.refusals == before.refusals),
                      "freeing a null pointer was recorded as something");

    /* A pointer this allocator never handed out. The bytes before it are not one
     * of the two marks, so it is refused rather than linked into the free list —
     * which would put storage the allocator does not own upon the list and hand
     * it to the next caller. */
    OxysHeapInspect(&before);
    free(&VerifyHeapForeign[64]);
    OxysHeapInspect(&after);

    VerifyHeapRequire(after.refusals == (before.refusals + 1U),
                      "a pointer that is not an allocation was not refused");
    VerifyHeapRequire(after.releases == before.releases,
                      "a pointer that is not an allocation was released");

    /*
     * A second release of the same pointer.
     *
     * This is the defect the marks exist for. A block released twice is upon the
     * free list twice, after which two later requests are met with the same
     * memory — and the failure appears in whichever of the two callers writes
     * second, with nothing to connect it to the double release that caused it.
     */
    block = malloc(48U);
    VerifyHeapRequire(block != NULL, "an allocation for the release assertion was refused");

    if (block == NULL)
    {
        return;
    }

    OxysHeapInspect(&before);
    free(block);
    free(block);
    OxysHeapInspect(&after);

    VerifyHeapRequire(after.releases == (before.releases + 1U),
                      "a block released twice was released twice");
    VerifyHeapRequire(after.refusals == (before.refusals + 1U),
                      "a block released twice was not refused the second time");
}

static void VerifyHeapResize(void)
{
    OxysHeapCensus before;
    OxysHeapCensus after;
    unsigned char *block;
    unsigned char *grown;
    unsigned char *moved;
    unsigned char *obstacle;
    void *refused;

    /* A null pointer makes realloc into malloc: 7.22.3.5, paragraph 3. */
    block = realloc(NULL, 64U);
    VerifyHeapRequire(block != NULL, "realloc of a null pointer did not allocate");

    if (block == NULL)
    {
        return;
    }

    (void)memset(block, 0x5A, 64U);

    /*
     * Growing where the block after it is free: the allocation must not move.
     *
     * This is what makes a program that grows one buffer repeatedly — which is
     * every program that reads something of unknown length — cost one region
     * rather than a copy of everything it has read at every step. An allocator
     * without it is correct and quadratic, and nothing but this assertion
     * distinguishes the two.
     */
    grown = realloc(block, 256U);
    VerifyHeapRequire(grown == block,
                      "growing into a free neighbour moved the allocation");
    VerifyHeapRequire((grown != NULL) && (grown[0] == 0x5A) && (grown[63] == 0x5A),
                      "growing an allocation did not preserve its contents");

    if (grown == NULL)
    {
        return;
    }

    /*
     * Growing where the block after it is not free: the allocation must move,
     * and must arrive with its contents.
     *
     * The obstacle is allocated immediately after it, which the address ordering
     * of the free list guarantees: the block after `grown` is the beginning of
     * the one free block there is, and a request met by first fit takes it from
     * the front.
     */
    obstacle = malloc(64U);
    VerifyHeapRequire(obstacle != NULL, "the obstacle allocation was refused");

    moved = realloc(grown, 1024U);
    VerifyHeapRequire(moved != NULL, "growing past an obstacle was refused");

    if (moved == NULL)
    {
        free(obstacle);

        return;
    }

    VerifyHeapRequire(moved != grown, "growing past an obstacle did not move the block");
    VerifyHeapRequire((moved[0] == 0x5A) && (moved[63] == 0x5A),
                      "moving an allocation did not carry its contents");

    /* Shrinking: the block stays where it is and the heap gets the difference
     * back. An allocator that returned a fresh block for every shrink would pass
     * every assertion above and copy the whole buffer each time. */
    OxysHeapInspect(&before);
    block = realloc(moved, 64U);
    OxysHeapInspect(&after);

    VerifyHeapRequire(block == moved, "shrinking an allocation moved it");
    VerifyHeapRequire(after.available > before.available,
                      "shrinking an allocation gave nothing back");
    VerifyHeapRequire((block != NULL) && (block[0] == 0x5A),
                      "shrinking an allocation lost the contents it kept");

    /* A pointer that is not an allocation is refused, and with EINVAL: the
     * machine has not run out of anything. An implementation that reported
     * ENOMEM would send a caller looking for memory pressure that is not there. */
    OxysHeapInspect(&before);
    errno = EDOM;
    refused = realloc(&VerifyHeapForeign[64], 64U);
    OxysHeapInspect(&after);

    VerifyHeapRequire(refused == NULL,
                      "reallocating a pointer that is not an allocation succeeded");
    VerifyHeapRequire(errno == EINVAL,
                      "reallocating a pointer that is not an allocation did not report "
                      "EINVAL");
    VerifyHeapRequire(after.refusals == (before.refusals + 1U),
                      "reallocating a pointer that is not an allocation was not refused");

    free(block);
    free(obstacle);
}

static void VerifyHeapClearing(void)
{
    OxysHeapCensus before;
    OxysHeapCensus after;
    unsigned char *dirty;
    unsigned char *cleared;
    bool zeroed = true;
    void *refused;

    /*
     * The block is soiled before it is asked for again.
     *
     * Every page this kernel maps arrives zeroed, so a calloc that never cleared
     * anything would pass upon a fresh heap and fail upon a used one — which is
     * every heap a real program has. Writing a pattern, releasing the block and
     * asking for the same size again is what puts the used case in front of the
     * assertion.
     */
    dirty = malloc(512U);
    VerifyHeapRequire(dirty != NULL, "an allocation for the clearing assertion was refused");

    if (dirty == NULL)
    {
        return;
    }

    (void)memset(dirty, 0xAA, 512U);
    free(dirty);

    cleared = calloc(512U, 1U);
    VerifyHeapRequire(cleared != NULL, "a cleared allocation was refused");

    if (cleared == NULL)
    {
        return;
    }

    for (size_t index = 0U; index < 512U; ++index)
    {
        if (cleared[index] != 0U)
        {
            zeroed = false;
        }
    }

    VerifyHeapRequire(zeroed, "a cleared allocation was not all bits zero");
    free(cleared);

    /*
     * The product that cannot be formed.
     *
     * This is the one assertion in this file about a security property rather
     * than about correctness. A product that wraps produces a small block for a
     * large request; the caller then writes the elements it asked for and the
     * write runs off the end of a block the allocator believes is smaller than it
     * is. Nothing in the allocator can detect it afterwards, which is why it has
     * to be refused before the multiplication happens.
     */
    OxysHeapInspect(&before);
    errno = EDOM;
    refused = calloc((SIZE_MAX / 2U) + 1U, 4U);
    OxysHeapInspect(&after);

    VerifyHeapRequire(refused == NULL, "a count and size whose product wraps was met");
    VerifyHeapRequire(errno == ENOMEM,
                      "a count and size whose product wraps did not report ENOMEM");
    VerifyHeapRequire(after.allocated == before.allocated,
                      "a refused request took memory from the heap");

    /* A size no block could ever be. It must be refused by the arithmetic and
     * not by the free list, so the heap must not have asked the system for
     * anything to discover it. */
    OxysHeapInspect(&before);
    errno = EDOM;
    refused = malloc(SIZE_MAX);
    OxysHeapInspect(&after);

    VerifyHeapRequire(refused == NULL, "a request of the greatest representable size was met");
    VerifyHeapRequire(errno == ENOMEM,
                      "a request of the greatest representable size did not report ENOMEM");
    VerifyHeapRequire(after.extensions == before.extensions,
                      "an impossible request made the heap ask the system for memory");
    VerifyHeapRequire(after.failures > before.failures,
                      "a refused request was not counted as a failure");
}

/* -------------------------------------------------------------------------
 * 2. The system call, which is the half that cannot be called.
 * ------------------------------------------------------------------------- */

/*
 * The composed program's address space, and where each thing stands in it.
 *
 * The text page holds the driver at its beginning and the C library's invocation
 * block at a fixed displacement, exactly as kernel/test/verify_wrappers.c
 * arranges them and for the same reason: the displacement has to be known before
 * either is written, so that the driver's call displacements can be computed.
 */
#define VERIFY_HEAP_TEXT_ADDRESS UINT64_C(0x0000000000401000)
#define VERIFY_HEAP_DATA_ADDRESS UINT64_C(0x0000000000402000)
#define VERIFY_HEAP_TEXT_OFFSET  0x1000U
#define VERIFY_HEAP_DATA_OFFSET  0x2000U
#define VERIFY_HEAP_IMAGE_BYTES  0x3000U
#define VERIFY_HEAP_TEXT_BYTES   0x0200U
#define VERIFY_HEAP_DATA_BYTES   0x0100U
#define VERIFY_HEAP_INVOKE_AT    0x0180U

/* Within the data page: the single newline the program writes after the string
 * it fetched into its heap, so that the log's next line begins where a reader
 * expects. */
#define VERIFY_HEAP_NEWLINE_AT 0x00U

/* What the program asks for, and what it reads into it. The growth is one page
 * because one page is what the smallest growth this kernel can perform costs,
 * and the capacity is well within it. */
#define VERIFY_HEAP_GROWTH   4096U
#define VERIFY_HEAP_CAPACITY 64U

extern const uint8_t OxysSyscallInvokeBegin[];
extern const uint8_t OxysSyscallInvokeEnd[];
extern const uint8_t OxysSyscallInvokeBytes1[];
extern const uint8_t OxysSyscallInvokeBytes2[];
extern const uint8_t OxysSyscallInvokeBytes3[];

static uint8_t VerifyHeapImage[VERIFY_HEAP_IMAGE_BYTES];
static uint64_t VerifyHeapDriverEnd;
static bool VerifyHeapOverran;

/*
 * Composes the program: the ELF header, the two segments, the library's
 * invocation block copied in verbatim, and the driver that calls it.
 *
 * The driver, in the order it executes. RBX holds the break the program began
 * with, and RBP the running sum; both are callee-saved, so carrying them across
 * nine system calls asserts in passing that the kernel's entry path restores what
 * it is not returning in.
 *
 *   1.  brk(0)                  reports the break. Kept in RBX. Not summed: it is
 *                               an address, and the assertions below are about
 *                               what changed rather than about where the heap is.
 *   2.  version(break, 64)      **must fail with EFAULT**, nothing being mapped
 *                               at the break yet. Summed.
 *   3.  brk(break + 4096)       grows the heap by a page. The *difference* from
 *                               RBX is summed, so the assertion is that the
 *                               kernel returned the address asked for and not
 *                               some other plausible one.
 *   4.  version(break, 64)      now succeeds and returns the length copied,
 *                               which proves the page is mapped and writable at
 *                               privilege level 3. Summed.
 *   5.  write(1, break, length) reads it back, which proves the page is readable,
 *                               and puts a line in the log that came out of
 *                               memory the program asked the kernel for. Summed.
 *   6.  write(1, newline, 1)    ends that line. Summed.
 *   7.  brk(break)              shrinks the heap back. The difference from RBX is
 *                               summed and must be zero.
 *   8.  version(break, 64)      **must fail with EFAULT again**, the page having
 *                               been withdrawn. Summed. This is the assertion
 *                               that a shrink unmaps rather than merely moving a
 *                               number.
 *   9.  brk(0)                  reports the break, which must be where it began.
 *                               The difference is summed and must be zero.
 *   10. exit(sum)
 *
 * The sum is exact and the kernel computes what it must be from the same version
 * string its own call copies: two EFAULTs, one page, the length twice over, and
 * one newline. Nothing is within a tolerance, because there is none.
 */
static void VerifyHeapCompose(void)
{
    const uint64_t block = (uint64_t)(OxysSyscallInvokeEnd - OxysSyscallInvokeBegin);
    const uint64_t invoke1 =
        VERIFY_HEAP_TEXT_OFFSET + VERIFY_HEAP_INVOKE_AT +
        (uint64_t)(OxysSyscallInvokeBytes1 - OxysSyscallInvokeBegin);
    const uint64_t invoke2 =
        VERIFY_HEAP_TEXT_OFFSET + VERIFY_HEAP_INVOKE_AT +
        (uint64_t)(OxysSyscallInvokeBytes2 - OxysSyscallInvokeBegin);
    const uint64_t invoke3 =
        VERIFY_HEAP_TEXT_OFFSET + VERIFY_HEAP_INVOKE_AT +
        (uint64_t)(OxysSyscallInvokeBytes3 - OxysSyscallInvokeBegin);
    const uint64_t newline = VERIFY_HEAP_DATA_ADDRESS + VERIFY_HEAP_NEWLINE_AT;
    TestProgram program;

    TestProgramInitialise(&program, VerifyHeapImage, VERIFY_HEAP_IMAGE_BYTES);

    TestProgramElfHeader(&program, VERIFY_HEAP_TEXT_ADDRESS, 2U);
    TestProgramSegment(&program, 0U, ELF_SEGMENT_READ | ELF_SEGMENT_EXECUTE,
                       VERIFY_HEAP_TEXT_OFFSET, VERIFY_HEAP_TEXT_ADDRESS,
                       VERIFY_HEAP_TEXT_BYTES);
    TestProgramSegment(&program, 1U, ELF_SEGMENT_READ | ELF_SEGMENT_WRITE,
                       VERIFY_HEAP_DATA_OFFSET, VERIFY_HEAP_DATA_ADDRESS,
                       VERIFY_HEAP_DATA_BYTES);

    TestProgramCopy(&program, VERIFY_HEAP_TEXT_OFFSET + VERIFY_HEAP_INVOKE_AT,
                    OxysSyscallInvokeBegin, (size_t)block);

    TestProgramSeek(&program, VERIFY_HEAP_TEXT_OFFSET);

    /* 1. brk(0) — the break, into RBX; and the sum, cleared. */
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RDI, (uint32_t)SYSCALL_BRK);
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RSI,
                               (uint32_t)SYSCALL_BREAK_QUERY);
    TestProgramCall(&program, invoke1);
    TestProgramMoveRegister(&program, TEST_PROGRAM_RBX, TEST_PROGRAM_RAX);
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RBP, 0U);

    /* 2. version(break, 64) — nothing is mapped there, so EFAULT. */
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RDI, (uint32_t)SYSCALL_VERSION);
    TestProgramMoveRegister(&program, TEST_PROGRAM_RSI, TEST_PROGRAM_RBX);
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RDX, VERIFY_HEAP_CAPACITY);
    TestProgramCall(&program, invoke2);
    TestProgramAddRegister(&program, TEST_PROGRAM_RBP, TEST_PROGRAM_RAX);

    /* 3. brk(break + 4096). */
    TestProgramMoveRegister(&program, TEST_PROGRAM_RSI, TEST_PROGRAM_RBX);
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RCX, VERIFY_HEAP_GROWTH);
    TestProgramAddRegister(&program, TEST_PROGRAM_RSI, TEST_PROGRAM_RCX);
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RDI, (uint32_t)SYSCALL_BRK);
    TestProgramCall(&program, invoke1);
    TestProgramSubtractRegister(&program, TEST_PROGRAM_RAX, TEST_PROGRAM_RBX);
    TestProgramAddRegister(&program, TEST_PROGRAM_RBP, TEST_PROGRAM_RAX);

    /* 4. version(break, 64) — into the page just obtained. */
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RDI, (uint32_t)SYSCALL_VERSION);
    TestProgramMoveRegister(&program, TEST_PROGRAM_RSI, TEST_PROGRAM_RBX);
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RDX, VERIFY_HEAP_CAPACITY);
    TestProgramCall(&program, invoke2);
    TestProgramAddRegister(&program, TEST_PROGRAM_RBP, TEST_PROGRAM_RAX);

    /* 5. write(1, break, length) — the length being what the call above
     * returned, taken from RAX before anything else is loaded. */
    TestProgramMoveRegister(&program, TEST_PROGRAM_RCX, TEST_PROGRAM_RAX);
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RDI, (uint32_t)SYSCALL_WRITE);
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RSI, 1U);
    TestProgramMoveRegister(&program, TEST_PROGRAM_RDX, TEST_PROGRAM_RBX);
    TestProgramCall(&program, invoke3);
    TestProgramAddRegister(&program, TEST_PROGRAM_RBP, TEST_PROGRAM_RAX);

    /* 6. write(1, newline, 1). */
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RDI, (uint32_t)SYSCALL_WRITE);
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RSI, 1U);
    TestProgramMoveImmediate64(&program, TEST_PROGRAM_RDX, newline);
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RCX, 1U);
    TestProgramCall(&program, invoke3);
    TestProgramAddRegister(&program, TEST_PROGRAM_RBP, TEST_PROGRAM_RAX);

    /* 7. brk(break) — the page is given back. */
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RDI, (uint32_t)SYSCALL_BRK);
    TestProgramMoveRegister(&program, TEST_PROGRAM_RSI, TEST_PROGRAM_RBX);
    TestProgramCall(&program, invoke1);
    TestProgramSubtractRegister(&program, TEST_PROGRAM_RAX, TEST_PROGRAM_RBX);
    TestProgramAddRegister(&program, TEST_PROGRAM_RBP, TEST_PROGRAM_RAX);

    /* 8. version(break, 64) — EFAULT again, the page having been withdrawn. */
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RDI, (uint32_t)SYSCALL_VERSION);
    TestProgramMoveRegister(&program, TEST_PROGRAM_RSI, TEST_PROGRAM_RBX);
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RDX, VERIFY_HEAP_CAPACITY);
    TestProgramCall(&program, invoke2);
    TestProgramAddRegister(&program, TEST_PROGRAM_RBP, TEST_PROGRAM_RAX);

    /* 9. brk(0) — the break must be where it began. */
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RDI, (uint32_t)SYSCALL_BRK);
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RSI,
                               (uint32_t)SYSCALL_BREAK_QUERY);
    TestProgramCall(&program, invoke1);
    TestProgramSubtractRegister(&program, TEST_PROGRAM_RAX, TEST_PROGRAM_RBX);
    TestProgramAddRegister(&program, TEST_PROGRAM_RBP, TEST_PROGRAM_RAX);

    /* 10. exit(sum). */
    TestProgramMoveRegister(&program, TEST_PROGRAM_RSI, TEST_PROGRAM_RBP);
    TestProgramMoveImmediate32(&program, TEST_PROGRAM_RDI, (uint32_t)SYSCALL_EXIT);
    TestProgramCall(&program, invoke1);

    /* Not reached: exit does not return. */
    TestProgramUndefined(&program);

    VerifyHeapDriverEnd = program.at - VERIFY_HEAP_TEXT_OFFSET;
    VerifyHeapOverran = program.overran;

    VerifyHeapImage[VERIFY_HEAP_DATA_OFFSET + VERIFY_HEAP_NEWLINE_AT] = (uint8_t)'\n';
}

static void VerifyHeapProgram(void)
{
    Process *process;
    Thread *boot;
    Thread *thread;
    ElfImage image;
    uint64_t stack;
    uint64_t dispatched_before;
    uint64_t growths_before;
    uint64_t shrinks_before;
    uint64_t pages_before;
    int64_t expected;
    size_t version_length;
    const uint64_t terminations_before = ProcessTerminationCount();
    const uint64_t block = (uint64_t)(OxysSyscallInvokeEnd - OxysSyscallInvokeBegin);

    /*
     * What the program must end with, computed here from the same string the
     * kernel's own version call copies. It is derived and not written out,
     * because a constant written out stops being right the day the version
     * string changes — and the failure would be a self-test reporting that a
     * correct program returned the wrong thing.
     */
    version_length = strlen(OXYS_SYSTEM_NAME " " OXYS_VERSION_STRING);

    if (version_length > (VERIFY_HEAP_CAPACITY - 1U))
    {
        version_length = VERIFY_HEAP_CAPACITY - 1U;
    }

    expected = (2 * SYSCALL_EFAULT) + (int64_t)VERIFY_HEAP_GROWTH +
               (2 * (int64_t)version_length) + 1;

    VerifyHeapRequire(block > 0U, "the library's invocation block is empty");
    VerifyHeapRequire((VERIFY_HEAP_INVOKE_AT + block) <= VERIFY_HEAP_TEXT_BYTES,
                      "the library's invocation block does not fit in the program's text");

    VerifyHeapCompose();

    VerifyHeapRequire(VerifyHeapDriverEnd <= VERIFY_HEAP_INVOKE_AT,
                      "the composed driver runs into the invocation block");
    VerifyHeapRequire(!VerifyHeapOverran,
                      "the composer refused an emission for want of room");

    process = ProcessCreate("heap", NULL);
    VerifyHeapRequire(process != NULL, "a process could not be created");

    if (process == NULL)
    {
        return;
    }

    if (ElfLoad(&process->space, VerifyHeapImage, VERIFY_HEAP_IMAGE_BYTES, &image) !=
        ELF_OK)
    {
        ProcessDestroy(process);
        KernelWriteString("  The composed program did not load. FAILED.\n");
        VerifyHeapSucceeded = false;

        return;
    }

    ProcessRecordImage(process, &image);

    /*
     * The break is where the loader put it, and the assertions about it are made
     * before the program runs rather than inferred from what it reports.
     *
     * A guard page stands between the image and the heap, so the break is at
     * least one page above the end of the image — and a break that had been
     * placed *at* the image's end would be a heap whose first byte is the next
     * byte after the program's last static object, where an overrun of that
     * object lands in the allocator's own bookkeeping.
     */
    VerifyHeapRequire(process->break_start != 0U,
                      "a loaded program was given no heap");
    VerifyHeapRequire(process->break_start >= (AlignUp(image.highest, PAGE_SIZE) + PAGE_SIZE),
                      "the heap begins without a guard page below it");
    VerifyHeapRequire((process->break_start % PAGE_SIZE) == 0U,
                      "the heap does not begin upon a page boundary");
    VerifyHeapRequire(process->break_current == process->break_start,
                      "a program that has asked for nothing has a heap");

    stack = ProcessCreateUserStack(process, NULL);
    VerifyHeapRequire(stack != 0U, "the program was given no stack");

    boot = ThreadAdoptCurrent("boot");
    thread = ThreadCreate(process, image.entry, stack);
    VerifyHeapRequire(thread != NULL, "a thread could not be created");

    if ((boot == NULL) || (thread == NULL) || (stack == 0U))
    {
        ProcessDestroy(process);

        if (boot != NULL)
        {
            ThreadDestroy(boot);
        }

        return;
    }

    dispatched_before = SyscallDispatched();
    growths_before = ProcessBreakGrowthCount();
    shrinks_before = ProcessBreakShrinkCount();
    pages_before = ProcessBreakPageCount();

    KernelWriteString("  A program at privilege level 3 reports, from a page it asked "
                      "the kernel for: ");

    VerifyHeapRequire(ThreadStart(thread), "the program could not be started");

    VerifyHeapRequire(ThreadCurrent() == boot,
                      "the kernel did not resume the thread that started the program");

    /*
     * Ten calls and not nine or eleven. A count rather than a floor, for the
     * reason kernel/test/verify_wrappers.c gives: an invocation whose
     * displacement was wrong would land in the middle of another routine, which
     * within this block is still a valid instruction sequence and still returns.
     */
    VerifyHeapRequire((SyscallDispatched() - dispatched_before) == 10U,
                      "the program did not make exactly the ten calls it was composed "
                      "to make");

    VerifyHeapRequire(ProcessTerminationCount() == (terminations_before + 1U),
                      "the program was not ended");
    VerifyHeapRequire(process->state == PROCESS_EXITED,
                      "the program's process was not marked as ended");

    VerifyHeapRequire(process->exit_status == expected,
                      "the program's calls did not return what they had to return");

    /* The kernel's own view of what happened, which is independent of what the
     * program reported: one growth, one shrink, and not one page left mapped by
     * either. The last is the assertion that a shrink releases frames rather
     * than merely forgetting about them. */
    VerifyHeapRequire(ProcessBreakGrowthCount() == (growths_before + 1U),
                      "the kernel did not record exactly one growth");
    VerifyHeapRequire(ProcessBreakShrinkCount() == (shrinks_before + 1U),
                      "the kernel did not record exactly one shrink");
    VerifyHeapRequire(ProcessBreakPageCount() == pages_before,
                      "a page mapped by a growth survived the shrink that gave it back");
    VerifyHeapRequire(process->break_current == process->break_start,
                      "the break did not return to where the program found it");

    ProcessDestroy(process);
    ThreadDestroy(boot);
}

/* -------------------------------------------------------------------------
 * The test itself.
 * ------------------------------------------------------------------------- */

void KernelVerifyHeap(void)
{
    OxysHeapCensus census;

    VerifyHeapSucceeded = true;

    KernelWriteString("Heap: asserting the C library's allocator and the break "
                      "beneath it.\n");

    VerifyHeapAdoption();
    VerifyHeapAllocation();
    VerifyHeapCoalescing();
    VerifyHeapRelease();
    VerifyHeapResize();
    VerifyHeapClearing();

    /*
     * Everything the policy was given back, and nothing asked of the system.
     *
     * The first is the same invariant VerifyHeapCoalescing makes, applied to the
     * whole test rather than to one phase of it: every allocation made above has
     * been released, so a heap that is not one block again has leaked something,
     * and which phase leaked it is answerable by the assertions that phase makes.
     *
     * The second is what keeps this test from resetting the machine. A request
     * the arena could not meet would send the allocator to OxysHeapExtend, which
     * executes SYSCALL, which this kernel cannot survive — so the arena is sized
     * far above anything asked for here, and this is the assertion that says so
     * rather than the comment at the head of this file.
     */
    OxysHeapInspect(&census);

    VerifyHeapRequire(census.blocks == 1U,
                      "the heap did not return to one block when everything was "
                      "released");
    VerifyHeapRequire(census.allocated == 0U,
                      "the heap still holds memory nobody has");
    VerifyHeapRequire(census.extensions == 0U,
                      "the heap asked the system for memory, which this kernel "
                      "cannot provide");
    VerifyHeapRequire(VerifyHeapCensusBalances(&census),
                      "the census of the heap at rest does not balance");

    KernelWriteString("  The allocator handled ");
    KernelWriteDecimal(census.allocations);
    KernelWriteString(" allocation(s), ");
    KernelWriteDecimal(census.releases);
    KernelWriteString(" release(s), ");
    KernelWriteDecimal(census.reallocations);
    KernelWriteString(" resize(s), ");
    KernelWriteDecimal(census.refusals);
    KernelWriteString(" refusal(s); ");
    KernelWriteDecimal(census.splits);
    KernelWriteString(" split(s) and ");
    KernelWriteDecimal(census.coalescences);
    KernelWriteString(" join(s).\n");

    VerifyHeapProgram();

    KernelWriteString(VerifyHeapSucceeded ? "Heap self-test passed.\n"
                                          : "Heap self-test FAILED.\n");
}
