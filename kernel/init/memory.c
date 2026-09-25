/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/init/memory.c
 * Purpose: Phase 2: the physical frame allocator, the permanent paging
 *          hierarchy, the kernel arena and the heap, each asserted as it is
 *          established; and, later in the boot, per-frame reference counting.
 * Key functions: KernelInitialiseMemory, KernelInitialiseFrameReferences.
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
#include <oxys/mm/pmm.h>
#include <oxys/arch/mm/paging.h>
#include <oxys/mm/vmm.h>
#include <oxys/mm/heap.h>
void KernelInitialiseMemory(void)
{
    PhysicalMemoryInitialise(&KernelBootInformation);
    PhysicalMemoryReport();
    KernelVerifyFrameAllocator();

    PagingInitialise(&KernelBootInformation);
    PagingReport();
    KernelVerifyPaging();

    KernelVirtualInitialise();
    KernelHeapInitialise();
    KernelVerifyAllocators();
    KernelVerifyGrowingTable();
    KernelVirtualReport();
    KernelHeapReport();
}

/*
 * Per-frame reference counting, which the heap must exist for
 * (docs/design/MEMORY-LAYOUT.md, Section 12.2). KernelMain calls it after the
 * display phase, the position it has always held in the boot, and the seeding
 * counts every frame issued until then, the display's included.
 */
void KernelInitialiseFrameReferences(void)
{
    FrameReferenceInitialise();
    KernelVerifyReferenceCounting();
    PhysicalMemoryReport();
}
