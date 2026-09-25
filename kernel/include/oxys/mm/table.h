/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/mm/table.h
 * Purpose: Declares the growing table: an array of fixed-size entries held in
 *          chunks that never move, the first chunk static and the rest taken
 *          from the kernel heap as the table fills.
 * Key definitions: GrowingTable, GROWING_TABLE_INITIALISER,
 *          GrowingTableCapacity, GrowingTableAt, GrowingTableGrow,
 *          GROWING_TABLE_DIRECTORY_CAPACITY.
 * References:
 *   - docs/design/MEMORY-LAYOUT.md: the design, and why entries never move.
 *
 * Why chunks, and not one array that is reallocated. The tables this serves —
 * processes, threads, filesystem nodes — are referred to by pointer from all
 * over the kernel: run queues, wait channels, per-processor areas, open files.
 * An array moved to a larger allocation would leave every one of those naming
 * freed memory. A chunk, once installed, stays where it is for the life of the
 * machine, so a pointer to an entry is good for as long as the entry is.
 *
 * Why the first chunk is static. The tables are needed before the heap exists,
 * and the boot path must never depend upon an allocation succeeding. Growth is
 * for load, not for starting.
 *
 * Concurrency. A chunk is published by writing its pointer into the directory
 * and only then the count, with a compiler barrier between, so a reader on
 * another processor sees either the old capacity or the new one with its
 * chunk in place, x86_64 not reordering stores with stores. Growth itself
 * calls the heap, which is unsynchronised, so GrowingTableGrow refuses on any
 * processor but the bootstrap one: docs/design/CONCURRENCY.md.
 */

#ifndef OXYS_MM_TABLE_H
#define OXYS_MM_TABLE_H

#include <oxys/types.h>

/*
 * How many chunks a table may hold. With the chunk sizes the kernel uses, that
 * is thousands of processes, threads or nodes — far more than the memory their
 * stacks, address spaces and inodes would need — so what limits a table in
 * practice is memory, and exhaustion is reported as a lack of it.
 */
#define GROWING_TABLE_DIRECTORY_CAPACITY 256U

typedef struct GrowingTable
{
    /* What the table holds, for the report. */
    const char *name;

    /* The size of one entry, and how many a chunk holds. */
    size_t entry_size;
    size_t chunk_entries;

    /* The chunks installed; chunks[0] is the static first chunk. */
    volatile size_t chunk_count;
    uint8_t *chunks[GROWING_TABLE_DIRECTORY_CAPACITY];

    /* Chunks added, and growth refused (heap exhausted, directory full, or a
     * processor other than the bootstrap one). */
    uint64_t growths;
    uint64_t refusals;
} GrowingTable;

/* A table whose first chunk is `first`, an array of `count` entries of `type`. */
#define GROWING_TABLE_INITIALISER(label, type, first, count)                          \
    {                                                                                   \
        .name = (label), .entry_size = sizeof(type), .chunk_entries = (count),         \
        .chunk_count = 1U, .chunks = { (uint8_t *)(first) }, .growths = 0U,            \
        .refusals = 0U                                                                  \
    }

/* How many entries the table holds now, used and unused alike. */
static inline size_t GrowingTableCapacity(const GrowingTable *table)
{
    return table->chunk_count * table->chunk_entries;
}

/* The entry at an index, or NULL beyond the table. */
static inline void *GrowingTableAt(const GrowingTable *table, size_t index)
{
    if (index >= GrowingTableCapacity(table))
    {
        return NULL;
    }

    return table->chunks[index / table->chunk_entries] +
           ((index % table->chunk_entries) * table->entry_size);
}

/*
 * Adds one chunk of zeroed entries to the table. Returns false, and changes
 * nothing, where the heap cannot supply the chunk, the directory is full, or
 * the caller is not upon the bootstrap processor. A zeroed entry must mean an
 * unused one in every table that uses this.
 */
bool GrowingTableGrow(GrowingTable *table);

/* Emits one line: the table's capacity, its chunks, and its growth. */
void GrowingTableReport(const GrowingTable *table);

#endif /* OXYS_MM_TABLE_H */
