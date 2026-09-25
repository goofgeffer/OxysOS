<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Concurrency

**Phase**: sub-task 6.13 of [`../project/PLAN.md`](../project/PLAN.md); the
interrupt state carried per thread from 8.6.
**Source**: [`../../kernel/arch/x86_64/cpu/spinlock.c`](../../kernel/arch/x86_64/cpu/spinlock.c),
[`percpu.c`](../../kernel/arch/x86_64/cpu/percpu.c),
[`../../kernel/arch/x86_64/smp/ipi.c`](../../kernel/arch/x86_64/smp/ipi.c),
[`../../kernel/arch/x86_64/mm/shootdown.c`](../../kernel/arch/x86_64/mm/shootdown.c),
and their headers; the command register in
[`../../drivers/apic/lapic.c`](../../drivers/apic/lapic.c).
**Specifications**: Intel SDM, Volume 3A, Sections 3.4.4 (FS and GS bases in 64-bit
mode), 4.10.4 and 4.10.5 (invalidation across processors), 8.1.2.2 (bus locking),
8.2.2 (memory ordering), 10.6 and 10.6.1 (IPIs, the command register), 10.8.3;
Volumes 2A and 2B, `XADD`, `CMPXCHG`, `PAUSE`, `SWAPGS`, `INVLPG`.

The four mechanisms a second processor cannot safely exist without: a spinlock, a
per-processor area, the inter-processor interrupt, and the TLB shootdown built on
it; and **the list of what is still unsynchronised**, which is why user threads run
only on the bootstrap processor. Starting the processors is [`SMP.md`](SMP.md);
scheduling them is [`SCHEDULER.md`](SCHEDULER.md).

## 1. The spinlock

A **ticket lock**: a 16-bit ticket counter and a 16-bit serving counter; free when
equal.

```
acquire:  mask interrupts
          ticket := LOCK XADD(lock.ticket, 1)
          while lock.serving != ticket: PAUSE
release:  lock.serving := lock.serving + 1
          unmask interrupts, if this was the outermost section
```

- **Ticket, not test-and-set**: waiters are admitted in arrival order, so none
  starves; and `ticket − serving` is the number waiting, which a report can print
  and a single-processor test can assert.
- **Only `LOCK XADD` is atomic.** Two arrivals must get different tickets. The
  release is an ordinary store: stores are not reordered with stores (SDM 8.2.2),
  so the owner field is cleared everywhere before the next waiter is admitted. No
  fence is needed; the inline assembly's memory clobber stops the compiler moving
  accesses across the lock.
- **`PAUSE`** in the wait loop avoids a memory-order violation on exit and saves
  power; it is a no-op where unimplemented.
- **Every acquire masks interrupts, before taking the ticket.** Masking only where a
  handler is known to use the lock fails the first time someone is wrong: a handler
  spinning on a lock the interrupted code holds, on the only processor that can
  release it. Sections are a few dozen instructions.
- **Misuse panics** at the moment it happens, naming the lock, counters and owner:
  acquiring a lock this processor holds (which would wait for ever), and releasing
  one it does not (which would admit a second processor).
- **The wait is bounded** at hundreds of millions of iterations, far beyond real
  contention. It fires only for a lock never released, and panics with the lock,
  holder, waiter and queue length. It recovers the explanation, not the machine.

## 2. The per-processor area

One structure per processor holding what it owns outright: its kernel stack, index,
APIC identifier, interrupt-disable depth, lock count and counters. Every field is
written only by its own processor, so none is guarded.

**Reached through `GS`** in one load, `movq %gs:16, reg`: the area holds a pointer
to itself at offset 16, since no instruction loads a segment base. Identifying the
processor another way (reading the APIC ID) is an uncached device access, and every
lock acquire and release needs it.

> **The invariant**: while kernel code runs, `GS.base` holds this processor's area
> and `IA32_KERNEL_GS_BASE` holds zero; while a user program runs, the two are
> exchanged.

| Where | What it does |
| ----- | ------------ |
| `PerCpuInitialise` | Establishes the area and writes `GS.base`, before anything can take a lock. |
| `GdtInitialise` | **Repairs** `GS.base`: reloading `GS` from a flat descriptor zeroes the base in 64-bit mode (SDM 3.4.4). `PerCpuEstablishSegmentBase` finds the area by the APIC ID `CPUID` reports. The repair is inside the function so no caller can forget it. |
| `syscall_entry.asm` | `SWAPGS` on entry and return, unconditionally (`SYSCALL` comes only from level 3). |
| `interrupt_stubs.asm` | `SWAPGS` on entry and return **only if the saved `CS` is level 3**, read from the frame each time, since a handler may change the frame. Without it an interrupt during a user program would run kernel code with the user's base. |
| `ThreadSwitchTo`, `ThreadTrampolineEntry` | **Write** both registers rather than exchanging ([`PROCESS.md`](PROCESS.md)). |

The system-call entry's kernel stack and scratch are the area's first two fields,
at offsets fixed by `_Static_assert`.

## 3. The interrupt-disable is counted

`PerCpuPushInterruptState` clears the flag and, **if the depth was zero**, records
the flag it found; `PerCpuPopInterruptState` restores it **when the depth returns
to zero**. Saving and restoring per section is wrong: two sections released out of
order re-enable interrupts inside a critical section, and the deadlock appears
minutes later elsewhere. The flag is read before `CLI`, the area reached after.

It panics on a pop without a push, and on a pop with interrupts already enabled
(something inside executed `STI`).

**The count travels with the thread.** `ThreadSwitchTo` saves the depth and recorded
flag into the outgoing thread and loads the incoming thread's
(`PerCpuSaveInterruptState`, `PerCpuLoadInterruptState`). A thread asleep in a
system call is resumed by whoever pushed next, perhaps the idle thread with
interrupts on; without this its pop would execute `STI` inside the system call. A
thread that has never run starts with `PerCpuResetInterruptState`
([`SCHEDULER.md`](SCHEDULER.md)).

## 4. Inter-processor interrupts

A processor writes its Local APIC's interrupt command register; the target receives
the vector through the ordinary gate, dispatcher and end-of-interrupt
([`../devices/APIC.md`](../devices/APIC.md) describes the register).

- **The register is waited for before and after a send**: before, because a write
  while delivery status (bit 12) is set discards a message; after, so a caller
  waiting for an acknowledgement knows the message left. Both waits are bounded, and
  an abandoned send is counted.
- **Audiences**: `IpiSendToOthers` (shorthand "all but self"), `IpiSendToSelf`
  ("self"), `IpiSendToProcessor` (a destination). A shorthand avoids holding the
  register across a list.

| Vector | Purpose |
| ------ | ------- |
| `0xFD` | TLB shootdown. |
| `0xFC` | Halt, sent by `KernelPanic` before it writes anything, and not waited for. |

Both are just below the APIC's own `0xFF` and `0xFE` and far above device vectors
(which end at 47); a vector's number is its priority (SDM 10.8.3), so a shootdown is
served before any device request. A panic halts the others because a report written
while they run describes a machine that has moved on.

## 5. TLB shootdown

`INVLPG` reaches only the executing processor (SDM 4.10.5); every processor that may
cache a changed translation must invalidate it itself.

```
sender:   invalidate here
          if fewer than two processors online: done
          take the shootdown lock
          publish the address; pending := online - 1
          send 0xFD to all but self
          wait until pending = 0
          release the lock

receiver: read the published address
          invalidate it
          pending := pending - 1        (LOCK SUB)
          end of interrupt
```

- **The receiver reads first and acknowledges last.** An acknowledgement lets the
  sender publish another address, so it must follow the read; and it is the
  sender's evidence that the translation is gone, so it must follow the
  invalidation.
- **The sender waits.** Returning earlier would let it reuse a frame another
  processor can still reach (SDM 4.10.4.4). An unacknowledged shootdown is
  **fatal**: `PagingInvalidate` panics.
- **The check for one processor comes before the lock**, since `PagingInvalidate` is
  on every map, unmap and copy-on-write path.
- **One request at a time**, a global address under a lock; a per-sender request
  block is the refinement if shootdowns are ever measured to be a bottleneck.
- **`ShootdownToSelf`** (for the self-test) cannot hold the lock while waiting,
  since its own handler must run; it refuses unless exactly one processor is online
  and interrupts are enabled. Its caller runs before `SmpInitialise`.
- **`PagingInvalidateLocalPage`** is the bare instruction, for the shootdown handler
  alone; anyone else changing a mapping must announce it.

## Unsynchronised structures

Locked: the run queues (a lock each), the process and thread tables
(`ProcessTableLock`), and the diagnostic path (`KernelDiagnosticLock` in
`KernelWriteString`, covering the VGA cursor, the console, the serial transmit
buffer and the compositor; [`SMP.md`](SMP.md)).

Every file below is unsynchronised and says so in a `Concurrency.` paragraph of its
header; `make docs-check` holds the two to each other. None is unsafe today: what
runs on an application processor is a kernel thread, a **user thread's affinity
names only the bootstrap processor** ([`SCHEDULER.md`](SCHEDULER.md)), and a user
thread is never pre-empted inside the kernel. When these are locked the mask widens,
and this table shrinks in the same change.

| File | Structure |
| ---- | --------- |
| `kernel/mm/pmm.c` | The frame bitmap and search hint. |
| `kernel/mm/vmm.c` | The kernel arena. |
| `kernel/mm/heap.c` | The kernel heap. |
| `kernel/mm/table.c` | A growing table's chunk directory; growth is refused off the bootstrap processor. |
| `kernel/block/block.c` | The block device table. |
| `kernel/block/buffer.c` | The buffer cache; the first structure that will want a lock it can sleep on. |
| `kernel/fs/vfs/vfs.c` | The mount, node and open-file tables. |
| `kernel/fs/vfs/pipe.c` | Pipes: reader and writer take turns on one processor. |
| `kernel/proc/process.c` | Each process's descriptor table (beyond the slot claim). |
| `kernel/proc/signal.c` | Pending sets and dispositions, written by senders and the tick. |
| `kernel/terminal/terminal.c` | The terminal's queue, filled and drained by its one reader. |
| `kernel/arch/x86_64/interrupt/interrupts.c` | The interrupt dispatch table. |
| `kernel/arch/x86_64/interrupt/irq.c` | The request layer's mask state. |
| `drivers/pic/pic.c` | The 8259A mask registers. |
| `drivers/apic/ioapic.c` | The I/O APIC's select-then-window sequence. |
| `drivers/ps2/ps2.c` | The 8042 configuration byte. |
| `drivers/keyboard/keyboard.c` | The keyboard event buffer's consumer side. |
| `drivers/mouse/mouse.c` | The mouse event buffer's consumer side. |
| `drivers/ramdisk/ramdisk.c` | The ramdisk's registration (its copies are disjoint and safe). |
| `graphics/draw.c` | Drawing surfaces; the owner of a surface must lock it. |
| `graphics/window.c` | The window table, stack and queues, filled by the tick and drained by clients on the bootstrap processor. |
| `graphics/faultscreen.c` | The fault screen, **by decision**: a handler waiting on a lock held by the processor that faulted would hang instead of reporting. Two interleaved screens is the lesser failure. |

## Verification

`KernelVerifyPerCpu`, `KernelVerifySpinlock`, `KernelVerifyIpi` and
`KernelVerifyShootdown` in [`../../kernel/test/arch/smp.c`](../../kernel/test/arch/smp.c).
On one processor a lock that does not lock behaves like one that does, so the
assertions are about each mechanism's internal state.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| The area's self pointer names the area; `GS.base` (read from the MSR) holds it; `IA32_KERNEL_GS_BASE` holds zero. | Another processor's state; the invariant broken, working until the first `SWAPGS`. |
| The area's APIC identifier is the controller's; its kernel stack is the TSS's. | IPIs to the wrong processor; a kernel on stack address zero at the first system call. |
| Depth and lock count are zero on entry. | A section entered and never left. |
| An acquire masks interrupts; a held lock is one ticket ahead of serving. | The easily omitted half; broken ordering. |
| Nested sections deepen and restore the count with the flag clear throughout; a release restores the flag the acquire found. | Save-and-restore semantics; a release that always enables. |
| A conditional acquire of a held lock fails without waiting; an uncontended acquire is not counted as contention. | The report path waiting; a useless contention counter. |
| A self-IPI sent with interrupts off is delivered only once they are on; it is delivered; **a second of the same vector is delivered**. | Send path broken; an end-of-interrupt missing (blocking everything below `0xFD`). |
| No APIC error; no abandoned send. | Illegal vectors; a register that never went idle. |
| **After a shootdown, a deliberately staled mapping reads through the new entry**, and the handler ran once with the requested address. | Any broken shootdown. The test maps a page, reads it, rewrites the page-table entry by hand without invalidating, then shoots down to itself. |

[`SMP.md`](SMP.md) asserts that application processors acknowledge real shootdowns.

## Limitations

1. The structures in the table above are unsynchronised.
2. A shootdown carries one address; a range is one page at a time.
3. An address space is never shot down as a whole; revisit when a process can run on
   one processor and be collected on another.
4. No reader-writer lock, and no lock that sleeps (the wait channel of
   [`SCHEDULER.md`](SCHEDULER.md) could support one).
5. **`SWAPGS` has an NMI window**: between `SWAPGS` and `SYSRET`/`IRETQ` the code
   segment is the kernel's and the base the user's. An NMI there runs with the wrong
   base. Closing it needs an entry that reads `IA32_GS_BASE` and a dedicated IST
   stack; the NMI handler does not currently touch the area.
6. A lock's `contentions` counter is written without holding the lock; a torn
   increment costs a diagnostic figure.
7. At most 64 processors (`PER_CPU_MAXIMUM`); more panic at `PerCpuInitialise`.
