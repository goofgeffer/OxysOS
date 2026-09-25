/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/mm/table.c
 * Purpose: Implements the growing table's one operation that allocates, and
 *          its report.
 * Key functions: GrowingTableGrow, GrowingTableReport.
 * References:
 *   - docs/design/MEMORY-LAYOUT.md: the design.
 *
 * Concurrency. Growth calls the heap, which is unsynchronised, and so is
 * refused upon any processor but the bootstrap one; the publication order that
 * lets other processors read a table while it grows is in the header.
 */

#include <oxys/mm/table.h>
#include <oxys/mm/heap.h>
#include <oxys/arch/cpu/percpu.h>
#include <oxys/kernel.h>

bool GrowingTableGrow(GrowingTable *table)
{
    const size_t count = table->chunk_count;
    uint8_t *chunk;

    if ((count >= GROWING_TABLE_DIRECTORY_CAPACITY) ||
        (PerCpuIsEstablished() && (PerCpuIndex() != 0U)))
    {
        ++table->refusals;

        return false;
    }

    chunk = (uint8_t *)KernelAllocateZeroed(table->entry_size * table->chunk_entries);

    if (chunk == NULL)
    {
        ++table->refusals;

        return false;
    }

    /* The pointer first, the count after: a reader that sees the new count
     * sees the chunk it names. */
    table->chunks[count] = chunk;
    __asm__ __volatile__("" ::: "memory");
    table->chunk_count = count + 1U;
    ++table->growths;

    return true;
}

void GrowingTableReport(const GrowingTable *table)
{
    KernelWriteString("  ");
    KernelWriteString(table->name);
    KernelWriteString(": ");
    KernelWriteDecimal((uint64_t)GrowingTableCapacity(table));
    KernelWriteString(" slots in ");
    KernelWriteDecimal((uint64_t)table->chunk_count);
    KernelWriteString(" chunk(s) of ");
    KernelWriteDecimal((uint64_t)table->chunk_entries);
    KernelWriteString("; grown ");
    KernelWriteDecimal(table->growths);
    KernelWriteString(", refused ");
    KernelWriteDecimal(table->refusals);
    KernelWriteString(".\n");
}
