/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/heap.h
 * Purpose: Declares the two things the allocator of <stdlib.h> needs that the C
 *          standard does not describe — where its memory comes from, and how to
 *          be given a region directly — together with the census a caller may
 *          take of it.
 * Key definitions: OxysHeapExtend, OxysHeapAdopt, OxysHeapCensus,
 *          OxysHeapInspect, OXYS_HEAP_ALIGNMENT, OXYS_HEAP_OVERHEAD.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.22.3: the functions this serves.
 *   - libc/include/syscall.h: OxysSbrk, which is what the shipped
 *     implementation of OxysHeapExtend is written in terms of.
 *   - docs/design/LIBC.md: the design of the allocator, the division
 *     this header is the seam of, and why that division is what makes the policy
 *     assertable at all.
 *
 * Why the allocator's memory source is a named function rather than a call to
 * OxysSbrk inside it.
 *
 * The two are different kinds of thing and fail in different ways. **The policy**
 * — first fit, splitting, coalescing, the arithmetic of realloc — is ordinary C
 * that runs anywhere and is wrong in ways a test can see. **The source** is a
 * system call, and this kernel cannot execute one: SYSRET returns to privilege
 * level 3 unconditionally, so a kernel that called OxysSbrk would leave its own
 * entry path as a user program. docs/design/LIBC.md records the
 * same obstacle for the wrappers of sub-task 7.2.
 *
 * Naming the seam is what lets each be asserted where it can be. The kernel's
 * boot-time self-test gives the allocator a region by OxysHeapAdopt and then
 * exercises the whole of the policy against it — the code this library actually
 * ships, not a reconstruction of it — while the source is asserted from the
 * other side, by a program at privilege level 3 that calls `brk` and uses the
 * memory it gets. What joins them is one function of six lines, and sub-task 7.5
 * is where a program links against it and runs both halves together.
 *
 * OxysHeapAdopt is not a test hook. A program with a statically reserved arena,
 * or one running before a break exists, has the same need and no other way to
 * meet it; the self-test is merely its first caller.
 */

#ifndef OXYS_LIBC_HEAP_H
#define OXYS_LIBC_HEAP_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * The alignment every pointer this allocator returns satisfies, and the bytes a
 * block spends upon its own bookkeeping.
 *
 * The alignment is _Alignof(max_align_t) upon this architecture, which ISO/IEC
 * 9899:2011, Section 6.2.8, paragraph 2, makes the greatest fundamental
 * alignment — so a pointer satisfying it satisfies every alignment requirement
 * Section 7.22.3, paragraph 1, obliges an allocation to satisfy. It is stated
 * here as a number rather than obtained from <stddef.h> because a freestanding
 * <stddef.h> need not define max_align_t at all; libc/stdlib/heap.c asserts the
 * two agree wherever the type is available.
 *
 * The overhead is per allocation and is not small. It is exposed because a
 * caller measuring a heap needs it to tell the allocator's cost from its own,
 * and because a number a program can read is harder to let drift than a number
 * in a comment.
 */
#define OXYS_HEAP_ALIGNMENT 16U
#define OXYS_HEAP_OVERHEAD  32U

/*
 * Obtains a region of at least `bytes` bytes from the system, or returns a null
 * pointer.
 *
 * The region is the caller's until the program ends; there is no way to give one
 * back, the break being a single boundary and a heap being free to hold regions
 * that are not the topmost. The bytes are zero upon arrival — this kernel zeroes
 * every page it maps — and the allocator does not depend upon that, a region
 * obtained any other way being entitled to hold anything.
 *
 * The shipped implementation is libc/stdlib/system.c and is written in terms of
 * OxysSbrk. It is declared here so that the policy may be compiled and asserted
 * without it, which is the division this header exists for.
 */
void *OxysHeapExtend(size_t bytes);

/*
 * Gives the heap a region of memory the caller obtained itself.
 *
 * The region becomes one free block, coalesced with a neighbour where it happens
 * to abut one, and everything allocated from it thereafter is the allocator's to
 * manage. It must not overlap a region the heap already holds, and it must
 * remain valid for as long as the program runs: nothing here ever hands a region
 * back, so a caller cannot know when it has stopped being used.
 *
 * Returns false, having done nothing, for a null region, for one smaller than a
 * block can be, or for one whose alignment the allocator cannot work with — the
 * last being a report and not a refusal to try, the region's own first bytes
 * being where the first block's header has to stand.
 */
bool OxysHeapAdopt(void *region, size_t bytes);

/*
 * What the heap presently holds, and what it has done.
 *
 * `allocated` and `available` are payload bytes and exclude every header;
 * `overhead` is what the headers occupy. The three together with nothing missing
 * are the bytes of every region adopted, which is the property a caller can
 * check this census against rather than trusting it.
 */
typedef struct OxysHeapCensus
{
    size_t regions;    /* Regions the heap has been given. */
    size_t bytes;      /* Bytes in them, headers included. */
    size_t allocated;  /* Payload bytes presently handed out. */
    size_t available;  /* Payload bytes in the free blocks. */
    size_t overhead;   /* Bytes occupied by block headers. */
    size_t blocks;     /* Blocks in the heap, free and allocated. */
    size_t free_blocks;/* How many of those are free. */
    size_t largest;    /* The largest payload a request could be met with now. */

    uint64_t allocations;   /* Requests that produced a pointer. */
    uint64_t releases;      /* Pointers given back. */
    uint64_t reallocations; /* Requests to change a size. */
    uint64_t extensions;    /* Times the heap asked the system for more. */
    uint64_t failures;      /* Requests that could not be met. */
    uint64_t refusals;      /* Pointers refused by free or realloc as not ours. */
    uint64_t splits;        /* Blocks divided to meet a request. */
    uint64_t coalescences;  /* Blocks joined to a neighbour upon release. */
} OxysHeapCensus;

/* Takes the census. A null argument does nothing. */
void OxysHeapInspect(OxysHeapCensus *census);

#endif /* OXYS_LIBC_HEAP_H */
