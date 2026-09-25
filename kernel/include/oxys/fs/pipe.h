/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/fs/pipe.h
 * Purpose: Declares the pipe of sub-task 8.6: a bounded byte queue between two
 *          open files of the filesystem layer, one that reads it and one that
 *          writes it, upon which a reader sleeps while it is empty and a writer
 *          sleeps while it is full.
 * Key definitions: VFS_PIPE_CHUNK, VFS_PIPE_BUFFER_SIZE, VfsPipeCreate,
 *          VfsPipeCount, VfsPipeBytesCarried, VfsPipeReport.
 * References:
 *   - IEEE Std 1003.1-2017, `pipe()`: "Data can be written to the file
 *     descriptor fildes[1] and read from the file descriptor fildes[0]"; both
 *     descriptors have O_NONBLOCK and FD_CLOEXEC clear.
 *   - IEEE Std 1003.1-2017, `read()`: a read of an empty pipe whose write end
 *     is open by no process returns zero, and one whose write end is open
 *     blocks until data is written or every write end is closed.
 *   - IEEE Std 1003.1-2017, `write()`: "Write requests of {PIPE_BUF} bytes or
 *     less shall not be interleaved with data from other processes doing
 *     writes on the same pipe"; a write to a pipe "that is not open for
 *     reading by any process" fails with EPIPE, and a signal is also sent —
 *     which this kernel sends since sub-task 8.7.
 *   - docs/design/SHELL.md: what the pipe is for and why it is an
 *     open file of the filesystem layer rather than a thing of its own.
 *
 * Why a pipe is an open file.
 *
 *   A pipe's two ends are reached by descriptors, and everything a descriptor
 *   already does — the count of holders that lets a child inherit one, the
 *   `dup2` that places one at 0 or 1, the close at exit that releases it — was
 *   built at sub-task 8.5 upon the open file of the filesystem layer. A pipe
 *   end that was not an open file would have needed every one of those written
 *   a second time, and the two copies would have differed the first time one
 *   was corrected. So an end is an open file with no node beneath it and a pipe
 *   in the node's place, and `VfsRead`, `VfsWrite` and `VfsClose` ask which it
 *   is before they touch the node.
 *
 * Why the buffer is a page and the pipes are eight.
 *
 *   A page is what one `write` may carry at most — SYSCALL_TRANSFER_MAXIMUM is
 *   the same number — so every write a program can make fits the buffer whole,
 *   and the atomicity `write()` promises for {PIPE_BUF} bytes is kept for every
 *   write rather than for the first five hundred and twelve: a writer waits for
 *   room for the whole of what remains, not for a byte of it. Eight pipes is
 *   thirty-two kibibytes of the kernel image, drawn from a fixed array for the
 *   reason every table of this layer is: a pipe that could not be had because
 *   the heap was exhausted would fail at the prompt, in a pipeline somebody
 *   typed, with a message about memory.
 */

#ifndef OXYS_FS_PIPE_H
#define OXYS_FS_PIPE_H

#include <oxys/types.h>

/* How many pipes a chunk of the growing pipe table holds (not a limit), and
 * how many bytes each pipe holds. The size is a power of two so that the two
 * indices are reduced to subscripts by a mask, and is exactly one page for the
 * reason the header gives. */
#define VFS_PIPE_CHUNK       8U
#define VFS_PIPE_BUFFER_SIZE 4096U

/*
 * Makes a pipe and two open files upon it, and reports their descriptors of
 * the filesystem layer: the read end first, as `pipe()` orders them.
 *
 * Returns false, having refused, where every pipe or every open file is in
 * use — VFS_ERROR_NO_RESOURCE, which a program sees as EMFILE — so that a
 * pipeline that cannot be made is refused at the prompt and not half-built.
 */
bool VfsPipeCreate(int *read_end, int *write_end);

/* How many pipes exist at this moment, and how many bytes every pipe has
 * carried since the boot. Both exist for the report and the self-test. */
size_t VfsPipeCount(void);
uint64_t VfsPipeBytesCarried(void);

/* Emits a summary upon the console and the serial port. */
void VfsPipeReport(void);

#endif /* OXYS_FS_PIPE_H */
