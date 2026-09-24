/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/stdlib/system.c
 * Purpose: The one place the heap of libc/stdlib/heap.c touches the system: it
 *          obtains a region of memory by moving the program's break.
 * Key functions: OxysHeapExtend.
 * References:
 *   - libc/include/heap.h: the seam this implements, and why the allocator's
 *     memory source is a named function rather than a call inside the allocator.
 *   - libc/include/syscall.h: OxysSbrk, and the failure value it returns.
 *   - kernel/abi/oxys/syscall_abi.h: SYSCALL_BRK, which is what lies beneath it.
 *   - docs/design/LIBC.md: the division of the sub-task into a
 *     policy that runs anywhere and a source that runs only at privilege level 3.
 *
 * This file is six lines of code and a translation unit of its own, which is
 * deliberate.
 *
 * It is the whole of what the allocator knows about the machine beneath it. The
 * policy in libc/stdlib/heap.c calls nothing else that could fail for a reason
 * outside the C language, so the two can be — and are — asserted separately: the
 * policy by the kernel's own boot-time self-test, which gives it a region
 * directly, and the system call beneath this function by a program at privilege
 * level 3 that exercises `brk` and uses what it gets. Section 9.5 of the design
 * document holds both tables.
 *
 * **This function cannot be called by the kernel**, and neither can anything
 * else that reaches a system call: SYSRET returns to privilege level 3
 * unconditionally, so a kernel that executed SYSCALL would leave its own entry
 * path as a user program upon a stack that is not a user program's. That is the
 * same hazard libc/syscall/calls.c has carried since sub-task 7.2 — every one of
 * its wrappers is linked into the kernel image and none is called from it — and
 * it is answered the same way: the boot-time self-test asserts that the heap it
 * exercises never asked for an extension, so a change that made it ask would be
 * reported rather than discovered as a reset.
 */

#include <heap.h>
#include <stddef.h>
#include <stdint.h>
#include <syscall.h>

void *OxysHeapExtend(size_t bytes)
{
    void *region;

    /*
     * A request of nothing is refused rather than passed on. OxysSbrk reads the
     * break for an increment of zero and returns it without moving it, which is
     * a correct answer to a different question and would leave the allocator
     * adopting a region it does not own.
     */
    if (bytes == 0U)
    {
        return NULL;
    }

    /*
     * The cast is checked and not assumed. OxysSbrk takes a signed increment,
     * because its whole purpose is that it may be negative; a size that does not
     * fit in the positive half of that type would arrive as a negative one and
     * ask the kernel to *shrink* the heap by an enormous amount. The kernel would
     * refuse it — a break below where the heap begins is SYSCALL_EINVAL — so the
     * failure would be reported rather than acted upon; it is refused here all
     * the same, because "the allocator asked for two exbibytes and the kernel
     * said the argument was invalid" is a worse account of what happened than
     * "the allocator could not ask for two exbibytes".
     */
    if (bytes > (size_t)INTPTR_MAX)
    {
        return NULL;
    }

    region = OxysSbrk((intptr_t)bytes);

    /*
     * OxysSbrk reports a failure as (void *)-1 and not as a null pointer, zero
     * being a plausible break and the greatest representable address not being
     * one. The allocator's seam reports a failure as null, that being what a C
     * caller tests; the translation between the two conventions is this line, and
     * it is here rather than in the allocator so that heap.c holds no knowledge
     * of how memory is got.
     */
    return (region == (void *)(intptr_t)-1) ? NULL : region;
}
