/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/fs/vfs/pipe.c
 * Purpose: Implements the pipe of sub-task 8.6: a bounded byte queue between
 *          two open files, the read that sleeps while it is empty, the write
 *          that sleeps while it is full, the end of file a reader sees when
 *          every writer has gone, and the refusal a writer meets when every
 *          reader has.
 * Key functions: VfsPipeCreate, VfsPipeRead, VfsPipeWrite, VfsPipeReleaseEnd,
 *          VfsPipeCount, VfsPipeBytesCarried, VfsPipeReport.
 * References:
 *   - IEEE Std 1003.1-2017, `pipe()`, `read()` and `write()`: the semantics
 *     the header sets out — the read end is the first descriptor, an empty
 *     pipe with no writer reads as end of file, a write to a pipe with no
 *     reader is EPIPE, and a write of {PIPE_BUF} bytes or fewer is not
 *     interleaved with another's.
 *   - kernel/include/oxys/proc/sched.h: the wait channel, and the discipline
 *     of testing a condition and sleeping within one masked section.
 *   - docs/design/SHELL.md, Section 22; docs/storage/VFS.md, Section 11.
 *
 * Why the reader and the writer sleep upon the pipe and not upon two channels.
 *
 *   One channel — the pipe's own address — is woken by every event upon it: a
 *   write wakes a reader that found it empty, a read wakes a writer that found
 *   it full, and a close wakes whichever of the two was waiting for the other.
 *   Two channels would let each wake name its sleeper, at the cost of a close
 *   having to wake both and a defect in which it woke one. Every sleeper here
 *   re-tests its condition upon waking and sleeps again if it was not the one
 *   meant, so the cost of the single channel is a spurious wake of a thread
 *   that is asleep anyway, and the benefit is that no event can forget who it
 *   was for.
 *
 * Why a writer waits for room for the whole of what remains.
 *
 *   `write()` promises that a write of {PIPE_BUF} bytes or fewer is not
 *   interleaved with another process's, and a writer that wrote what fitted
 *   and slept for the rest would let a second writer put its bytes into the
 *   gap. Every write a program can make fits this buffer whole, the buffer and
 *   SYSCALL_TRANSFER_MAXIMUM being the same page, so the writer sleeps until
 *   the buffer has room for everything it has left and then writes it in one
 *   piece. A write larger than the buffer — which no system call can deliver,
 *   and which a kernel caller could — waits for room for a byte at a time,
 *   which is what the standard permits above {PIPE_BUF}.
 *
 * Concurrency. Every reader and writer of a pipe is a user thread, and every
 *   user thread runs upon the bootstrap processor and is never pre-empted
 *   inside the kernel, so no two of them are inside this file at once. The
 *   masked sections below are the discipline <oxys/proc/sched.h> sets out for
 *   a sleep, kept for the day a wake arrives from an interrupt handler, and not
 *   a lock. docs/design/CONCURRENCY.md, Section 10, limitation 1, records the
 *   structures a second processor would race for, and this is one of them.
 */

#include "internal.h"

#include <oxys/fs/pipe.h>
#include <oxys/fs/vfs.h>
#include <oxys/kernel.h>
#include <oxys/proc/sched.h>
#include <oxys/proc/signal.h>
#include <oxys/arch/cpu/percpu.h>

_Static_assert((VFS_PIPE_BUFFER_SIZE & (VFS_PIPE_BUFFER_SIZE - 1U)) == 0U,
               "The buffer is a power of two so that the indices reduce by a mask.");

/*
 * One pipe.
 *
 * The two indices are never reduced: their difference is the number of bytes
 * held, and each is reduced to a subscript by the mask. `readers` and
 * `writers` count open files and not descriptors — an end shared by a parent
 * and its child is one open file with two holders, and it is the release of the
 * open file, not of a holder, that closes an end.
 */
typedef struct VfsPipe
{
    uint8_t buffer[VFS_PIPE_BUFFER_SIZE];
    uint64_t read_index;
    uint64_t write_index;
    uint32_t readers;
    uint32_t writers;
    bool in_use;
} VfsPipe;

static VfsPipe VfsPipes[VFS_PIPE_CAPACITY];

/* Accounting. */
static uint64_t VfsPipesCreated;
static uint64_t VfsPipeBytes;
static uint64_t VfsPipeReaderSleeps;
static uint64_t VfsPipeWriterSleeps;

/* ------------------------------------------------------------------ helpers */

static uint64_t VfsPipeHeld(const VfsPipe *pipe)
{
    return pipe->write_index - pipe->read_index;
}

static uint64_t VfsPipeRoom(const VfsPipe *pipe)
{
    return VFS_PIPE_BUFFER_SIZE - VfsPipeHeld(pipe);
}

/* The open file slot the layer will give to a pipe end, or null. It is the
 * search VfsOpen makes, repeated here rather than shared because VfsOpen's
 * search is followed by a node the pipe end does not have. */
static VfsFile *VfsPipeFreeFile(void)
{
    for (size_t index = 0U; index < VFS_FILE_CAPACITY; ++index)
    {
        if (!VfsFiles[index].open)
        {
            return &VfsFiles[index];
        }
    }

    return NULL;
}

/* ---------------------------------------------------------------- creation */

bool VfsPipeCreate(int *read_end, int *write_end)
{
    VfsPipe *pipe = NULL;
    VfsFile *reader;
    VfsFile *writer;

    if ((read_end == NULL) || (write_end == NULL))
    {
        return VfsRefuse(VFS_ERROR_INVALID, "nowhere to report the pipe's descriptors");
    }

    for (size_t index = 0U; index < VFS_PIPE_CAPACITY; ++index)
    {
        if (!VfsPipes[index].in_use)
        {
            pipe = &VfsPipes[index];
            break;
        }
    }

    if (pipe == NULL)
    {
        return VfsRefuse(VFS_ERROR_NO_RESOURCE, "every pipe is in use");
    }

    /*
     * Both open files are found before either is claimed, so that a table
     * with one slot left refuses the pipe rather than making half of one: a
     * read end with no write end would be a pipe at its end before anything
     * was written, and the reader would end at once with nothing said.
     */
    reader = VfsPipeFreeFile();

    if (reader == NULL)
    {
        return VfsRefuse(VFS_ERROR_NO_RESOURCE, "every open file is in use");
    }

    reader->open = true;
    writer = VfsPipeFreeFile();
    reader->open = false;

    if (writer == NULL)
    {
        return VfsRefuse(VFS_ERROR_NO_RESOURCE, "one open file remains and a pipe needs two");
    }

    *pipe = (VfsPipe){ 0 };
    pipe->readers = 1U;
    pipe->writers = 1U;
    pipe->in_use = true;

    *reader = (VfsFile){ 0 };
    reader->flags = VFS_OPEN_READ;
    reader->holders = 1U;
    reader->pipe = pipe;
    reader->open = true;

    *writer = (VfsFile){ 0 };
    writer->flags = VFS_OPEN_WRITE;
    writer->holders = 1U;
    writer->pipe = pipe;
    writer->open = true;

    *read_end = (int)(reader - VfsFiles);
    *write_end = (int)(writer - VfsFiles);

    ++VfsPipesCreated;
    ++VfsFilesOpenedCount;
    ++VfsFilesOpenedCount;

    VfsSucceed();
    return true;
}

/* ------------------------------------------------------------ the transfer */

bool VfsPipeRead(VfsFile *file, void *buffer, uint64_t length, uint64_t *read)
{
    VfsPipe *const pipe = file->pipe;
    uint8_t *const destination = buffer;

    if ((file->flags & VFS_OPEN_READ) == 0U)
    {
        return VfsRefuse(VFS_ERROR_INVALID, "the end of the pipe that writes was read");
    }

    for (;;)
    {
        uint64_t held;

        /* The test and the sleep are one masked section: the discipline
         * <oxys/proc/sched.h> sets out, so that no wake can fall between. */
        PerCpuPushInterruptState();

        held = VfsPipeHeld(pipe);

        if (held > 0U)
        {
            const uint64_t count = (held < length) ? held : length;

            for (uint64_t index = 0U; index < count; ++index)
            {
                destination[index] =
                    pipe->buffer[(pipe->read_index + index) & (VFS_PIPE_BUFFER_SIZE - 1U)];
            }

            pipe->read_index += count;
            VfsPipeBytes += count;
            *read = count;

            PerCpuPopInterruptState();

            /* A writer asleep for room is told there is some. */
            (void)SchedulerWake(pipe);

            VfsSucceed();
            return true;
        }

        if (pipe->writers == 0U)
        {
            /* Empty, and nothing will ever fill it: the end of the file, which
             * `read()` reports as zero bytes and which is what tells `cat` and
             * the rest of a pipeline to stop. */
            PerCpuPopInterruptState();

            *read = 0U;
            VfsSucceed();
            return true;
        }

        if (!SchedulerCanSleep())
        {
            /* A caller with no thread to sleep upon — the kernel's own flow
             * of control inside a self-test — is refused rather than spun,
             * because nothing can write the pipe while it spins. */
            PerCpuPopInterruptState();

            return VfsRefuse(VFS_ERROR_BUSY,
                             "the pipe is empty and the caller cannot wait for its writer");
        }

        ++VfsPipeReaderSleeps;
        SchedulerSleep(pipe);
        PerCpuPopInterruptState();

        /* Woken by a signal rather than by the pipe, since sub-task 8.7: the
         * call reports EINTR — as VFS_ERROR_INTERRUPTED — and the signal is
         * delivered on the way out. What was transferred before is reported
         * where any was. */
        if (SignalIsPending(ProcessCurrent()))
        {
            return VfsRefuse(VFS_ERROR_INTERRUPTED, "a signal arrived while the pipe was waited upon");
        }
    }
}

bool VfsPipeWrite(VfsFile *file, const void *buffer, uint64_t length, uint64_t *written)
{
    VfsPipe *const pipe = file->pipe;
    const uint8_t *const source = buffer;

    if ((file->flags & VFS_OPEN_WRITE) == 0U)
    {
        return VfsRefuse(VFS_ERROR_INVALID, "the end of the pipe that reads was written");
    }

    while (*written < length)
    {
        const uint64_t remaining = length - *written;
        /* Room for the whole of what remains where that fits the buffer, so
         * that the write is one piece; a byte's worth otherwise. */
        const uint64_t needed = (remaining <= VFS_PIPE_BUFFER_SIZE) ? remaining : 1U;
        uint64_t room;

        PerCpuPushInterruptState();

        if (pipe->readers == 0U)
        {
            /*
             * Nothing will ever read it. IEEE Std 1003.1-2017 names this
             * EPIPE and sends a signal as well; the signal is 8.7's, so until
             * then a writer whose reader has gone is told so and nothing
             * more. What was written before the reader went stays written
             * and is reported.
             */
            PerCpuPopInterruptState();

            /* And SIGPIPE beside it, since sub-task 8.7: a writer whose reader
             * has gone is ended unless it asked to be told instead, which is
             * how `cat` at the head of a pipeline stops when `head` — when
             * there is one — has had enough. */
            (void)SignalSend(ProcessCurrent(), SYSCALL_SIGPIPE);

            return VfsRefuse(VFS_ERROR_BROKEN_PIPE, "the pipe is open for reading by nobody");
        }

        room = VfsPipeRoom(pipe);

        if (room >= needed)
        {
            const uint64_t count = (room < remaining) ? room : remaining;

            for (uint64_t index = 0U; index < count; ++index)
            {
                pipe->buffer[(pipe->write_index + index) & (VFS_PIPE_BUFFER_SIZE - 1U)] =
                    source[*written + index];
            }

            pipe->write_index += count;
            *written += count;

            PerCpuPopInterruptState();

            /* A reader asleep for bytes is told there are some. */
            (void)SchedulerWake(pipe);

            continue;
        }

        if (!SchedulerCanSleep())
        {
            PerCpuPopInterruptState();

            /* What fitted is written; a caller that cannot wait for the rest
             * is refused only where nothing at all could be written, so that
             * a count is never lost behind a refusal. */
            if (*written > 0U)
            {
                break;
            }

            return VfsRefuse(VFS_ERROR_BUSY,
                             "the pipe is full and the caller cannot wait for its reader");
        }

        ++VfsPipeWriterSleeps;
        SchedulerSleep(pipe);
        PerCpuPopInterruptState();

        /* Woken by a signal rather than by the pipe, since sub-task 8.7: the
         * call reports EINTR — as VFS_ERROR_INTERRUPTED — and the signal is
         * delivered on the way out. What was transferred before is reported
         * where any was. */
        if (SignalIsPending(ProcessCurrent()))
        {
            return VfsRefuse(VFS_ERROR_INTERRUPTED, "a signal arrived while the pipe was waited upon");
        }
    }

    VfsSucceed();
    return true;
}

/* ----------------------------------------------------------------- release */

void VfsPipeReleaseEnd(VfsFile *file)
{
    VfsPipe *const pipe = file->pipe;

    if (pipe == NULL)
    {
        return;
    }

    if ((file->flags & VFS_OPEN_READ) != 0U)
    {
        if (pipe->readers > 0U)
        {
            --pipe->readers;
        }
    }
    else if (pipe->writers > 0U)
    {
        --pipe->writers;
    }

    /*
     * Whoever is asleep upon the pipe is woken, because a close is what a
     * sleeper may have been waiting for: the reader for an end of file it can
     * only see once the last writer has gone, the writer for a refusal it can
     * only meet once the last reader has.
     */
    (void)SchedulerWake(pipe);

    if ((pipe->readers == 0U) && (pipe->writers == 0U))
    {
        pipe->in_use = false;
    }
}

/* -------------------------------------------------------------- accounting */

size_t VfsPipeCount(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < VFS_PIPE_CAPACITY; ++index)
    {
        if (VfsPipes[index].in_use)
        {
            ++count;
        }
    }

    return count;
}

uint64_t VfsPipeBytesCarried(void)
{
    return VfsPipeBytes;
}

void VfsPipeReport(void)
{
    KernelWriteString("Pipes: ");
    KernelWriteDecimal((uint64_t)VfsPipeCount());
    KernelWriteString(" of ");
    KernelWriteDecimal((uint64_t)VFS_PIPE_CAPACITY);
    KernelWriteString(" in use; ");
    KernelWriteDecimal(VfsPipesCreated);
    KernelWriteString(" made, ");
    KernelWriteDecimal(VfsPipeBytes);
    KernelWriteString(" byte(s) carried, ");
    KernelWriteDecimal(VfsPipeReaderSleeps);
    KernelWriteString(" reader sleep(s), ");
    KernelWriteDecimal(VfsPipeWriterSleeps);
    KernelWriteString(" writer sleep(s).\n");
}
