<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Interrupts and Exceptions

**Phase**: sub-tasks 3.1 to 3.5 of [`../project/PLAN.md`](../project/PLAN.md); the
request layer from 6.12; the conditional `SWAPGS` from 6.13.
**Source**: [`../../kernel/arch/x86_64/interrupt/`](../../kernel/arch/x86_64/interrupt/)
(`interrupts.c`, `interrupt_stubs.asm`, `exceptions.c`, `irq.c`),
[`../../kernel/arch/x86_64/cpu/gdt.c`](../../kernel/arch/x86_64/cpu/gdt.c),
[`../../drivers/pic/pic.c`](../../drivers/pic/pic.c).
**Specifications**: Intel SDM, Volume 3A, Chapter 6 (Table 6-1; Sections 6.2, 6.5,
6.12.1, 6.13, 6.14.1, 6.15; Figures 6-6, 6-9), Sections 3.4, 3.4.2, 3.4.4, 3.5;
Volumes 2A/2B, `LGDT`, `LIDT`, `IRETQ`, `SWAPGS`; Intel 8259A data sheet (ICWs,
OCWs); IBM PC/AT Technical Reference; ACPI 6.5, Sections 5.2.12.4, 5.2.12.5, Table
5.20.

The kernel's side of interrupts: the IDT, the 256 stubs and the uniform trap frame,
the dispatcher, the exception handlers and their dispositions, the 8259A pair used
until the APIC takes over, and the request layer through which a driver claims a
line whichever controller answers. The APIC is
[`../devices/APIC.md`](../devices/APIC.md).

## 1. The path

```
Processor presents vector N
   reads gate N of the IDT; reads the GDT descriptor the gate names
   aligns RSP to 16; pushes SS, RSP, RFLAGS, CS, RIP; pushes an error code
   for the vectors that have one; clears IF (interrupt gate)
InterruptStubN          pushes 0 if the processor pushed no error code; pushes N
InterruptCommonStub     SWAPGS if the saved CS is level 3; pushes 15 registers;
                        clears DF; RDI := RSP
InterruptDispatch(TrapFrame *)
InterruptCommonStub     pops the registers, discards vector and code;
                        SWAPGS if returning to level 3; IRETQ
```

## 2. The IDT and the stubs

256 gates of 16 bytes (limit `0xFFF`), all **interrupt gates** (a handler must not
be interrupted before it has saved state) at DPL 0 (user code cannot `INT` them;
system calls use `SYSCALL`, [`PRIVILEGE.md`](PRIVILEGE.md)).

The stubs normalise three irregularities:

- **The vector is not recorded** by the processor, so each stub pushes its own
  number; hence 256 stubs.
- **Only some vectors push an error code** (SDM Table 6-1): 8 `#DF` (always 0),
  10 `#TS`, 11 `#NP`, 12 `#SS`, 13 `#GP`, 14 `#PF`, 17 `#AC` (always 0), and 21
  `#CP`, 29 `#VC`, 30 `#SX` from later and AMD64 revisions. The others' stubs push
  a zero so every frame has one shape. The last three are treated as pushing one
  because the errors are asymmetric: never raised, the choice is immaterial; raised
  with a code and a stub pushing another, every field would be displaced. `INT n`
  never pushes a code, so these ten stubs must never be reached by `INT n`.
- **`GS` is not exchanged** by the processor. The kernel's per-processor area is in
  `GS.base` in the kernel and `IA32_KERNEL_GS_BASE` in a user program
  ([`CONCURRENCY.md`](CONCURRENCY.md)). The common stub tests the saved `CS` in the
  frame (no register can be trusted at entry) and exchanges when it is level 3, and
  tests the frame **again** on exit, since a handler may change it. The offset is
  `TRAP_FRAME_CS_OFFSET`, held to the structure by `_Static_assert`.

## 3. The trap frame

| Offset | Field | Pushed by |
| ------ | ----- | --------- |
| 0–112 | `r15` … `r8`, `rbp`, `rdi`, `rsi`, `rdx`, `rcx`, `rbx`, `rax` | The common stub, `rax` first |
| 120 | `vector` | The stub |
| 128 | `error_code` | The processor, or the stub as zero |
| 136 | `rip` | The processor |
| 144 | `cs` | The processor |
| 152 | `rflags` | The processor |
| 160 | `rsp` | The processor |
| 168 | `ss` | The processor |

176 bytes, asserted. `RSP` is not pushed by the stub: the interrupted stack pointer
is the processor's field, and pushing the register would record the handler's.
The structure and `interrupt_stubs.asm` are one interface in two languages; change
both or neither. The processor aligns `RSP` to 16 before its frame (SDM 6.12.1), and
176 is a multiple of 16, so the `CALL` meets the System V ABI either way.

## 4. The GDT

The boot GDT lies in `.boot` at a low physical address, reachable only through the
identity mapping that sub-task 2.3 removes; delivering an interrupt reads the
descriptor its gate names, so the first interrupt would fault on the missing table.
`gdt.c` therefore holds the kernel's GDT in `.data` in the higher half, and
`gdt.asm` reloads every segment register (`CS` by a far return). The table is not
`const`: the processor sets a descriptor's accessed bit when its selector is loaded
(SDM 3.4.2). It holds the kernel and user segments and a TSS descriptor per
processor, in the order `SYSCALL`/`SYSRET` require ([`PRIVILEGE.md`](PRIVILEGE.md)).

**Anything the processor reads directly must stay mapped while it may be read**, and
those reads are invisible in the source: the GDT, the IDT, each TSS and the SMP
trampoline are all of this kind.

## 5. The dispatcher

`InterruptDispatch` records the frame, counts, and calls the handler registered for
the vector.

| Unregistered vector | Treatment | Why |
| ------------------- | --------- | --- |
| 0–31 | Reported and fatal | Most are faults, restarted on return; returning without removing the cause re-enters for ever (SDM 6.5). |
| 32–255 | Counted and ignored | Nothing is restarted; this is right for a spurious request. |

**A handler may alter the frame**: it is passed by address, not `const`, and the
stub restores it into the registers. Correcting a page fault, returning a system
call result and switching context all need this. The breakpoint (vector 3) has a
default handler that records and returns, so `INT3` works as a marker; it is a
trap, reporting the state after the instruction.

## 6. Exceptions

**Disposition.** `ExceptionDispositionOf` decides from the vector **and** the
privilege level of the saved `CS`, a pure function asserted without raising
anything:

| Vectors | From level 3 | From level 0 |
| ------- | ------------ | ------------ |
| 3 `#BP`, 4 `#OF` | `RESUME` | `RESUME` |
| 14 `#PF` resolved by copy-on-write | `RESUME` | `RESUME` |
| 2 NMI, 8 `#DF`, 18 `#MC` | `FATAL` | `FATAL` |
| 10 `#TS`, 11 `#NP` | `FATAL` | `FATAL` |
| 0 `#DE`, 6 `#UD`, 12 `#SS`, 13 `#GP`, 14 `#PF` otherwise, 17 `#AC`, all others | `TERMINATE` | `FATAL` |

The same page fault is one program's mistake at level 3 and an unrecoverable kernel
state at level 0. Three groups ignore the level: **aborts** (SDM 6.5 allows no
reliable resumption); **NMI** (hardware, not the running program); **descriptor
table faults** (the tables are the kernel's, and would trip the next program too).
Only `FATAL` draws a fault screen ([`FAULTSCREEN.md`](FAULTSCREEN.md)).

**`TERMINATE`** is `ExceptionTerminateProgram`: `ThreadTerminateCurrent` ends the
process with status −vector (a program ended by `#UD` reports −6), releases what it
holds, and the machine carries on ([`PROCESS.md`](PROCESS.md)). If the thread was
not started by the kernel, there is nothing to return to, and returning would
restart the faulting instruction for ever; the judgement is made once, in
`ExceptionTerminateProgram`, which panics.

**Error codes** come in two forms, and decoding one as the other is nonsense:

| Page fault (Figure 6-9), bit | Meaning when set |
| ---------------------------- | ---------------- |
| 0 `P` | Protection violation; clear means no translation. |
| 1 `W/R` | A write. |
| 2 `U/S` | From user mode. |
| 3 `RSVD` | A reserved bit set in a paging entry. |
| 4 `I/D` | An instruction fetch. |
| 5 `PK` | Protection key. |
| 15 `SGX` | SGX access control. |

The selector form (`#TS`, `#NP`, `#SS`, `#GP`; Section 6.13, Figure 6-6): bit 0
`EXT`, bit 1 `IDT`, bit 2 `TI`, bits 3–15 the index; zero but for `EXT` means no
particular segment.

- **`CR2` is read first** in every handler, before anything can fault again
  (SDM 6.15).
- **`CR0.WP` is set** by `PagingInitialise`. With it clear (as at reset and after
  GRUB) the kernel writes through read-only pages without a fault, so read-only
  kernel mappings and copy-on-write would both be fictions.
- **A fatal report shows up to eight stack quadwords**, each first checked with
  `PagingTranslate`; a bad stack pointer is when the report matters most.

## 7. The 8259A pair

Used until the APIC is adopted (Section 8), and on machines without one.

**Remapped to vectors 32–47.** The PC firmware leaves the master at 8–15, which
collide with exceptions: a timer tick would be a double fault, a keystroke a page
fault, indistinguishably. The base must be a multiple of 8 (ICW2 supplies bits 7:3).

| Word | Port | Master | Slave | Meaning |
| ---- | ---- | ------ | ----- | ------- |
| ICW1 | command | `0x11` | `0x11` | Initialise; ICW4 follows; cascaded; edge. |
| ICW2 | data | `0x20` | `0x28` | Vector base. |
| ICW3 | data | `0x04` | `0x02` | At the master a **mask** of lines with a slave (IR2); at the slave its **number**. `0x04` at the slave would ignore every acknowledgement. |
| ICW4 | data | `0x01` | `0x01` | 8086 mode (else the controller supplies an 8080 `CALL`). |

- **ICW1 clears the mask register**, so every line is masked straight after the
  sequence (masking before would be undone). An unserviced request blocks every
  lower priority, and IR0 (the timer) is the highest.
- **The controller layer sends end-of-interrupt, not drivers.** For a slave request
  both controllers hold an in-service bit: the slave is signalled, then the master.
  The non-specific EOI (OCW2) is correct in the fully nested mode used.
- **A spurious request** (IR7 or IR15 with no in-service bit) is not acknowledged:
  an EOI would clear some real interrupt's bit and lose it, at a rate set by
  electrical noise. For IR15 the master did accept the cascade, so the master alone
  is signalled.

## 8. The request layer

`irq.c` is where a driver claims a request line (`IrqInstallHandler`,
`IrqUnmaskLine`), whichever controller delivers it. **The line number is the
durable fact**: IR1 is the keyboard on vector 33 under either controller, since
ACPI 6.5 Section 5.2.12.4 has global interrupts 0–15 carry the ISA lines except where
an override says otherwise, and the override is resolved here.

| Owned here | Owned by the controller driver |
| ---------- | ------------------------------ |
| Handlers by line; each line's vector; routing | Mask registers and their writes |
| Which controller answers | Initialisation |
| The mask each driver asked for | Spurious recognition; the EOI command |

**`IrqAdoptApic`** retires the 8259A in one place, with interrupts off (between the
steps no controller would deliver):

1. `PicDisable`: both mask registers to `0xFF` (ACPI Table 5.20; two live
   controllers deliver every request twice).
2. For each of the sixteen lines: resolve to its global interrupt, program the
   redirection entry (vector 32 + line, this processor, polarity and trigger from
   ACPI), and apply the mask the line's claimant asked for.
3. Record the APIC as answering.

Drivers notice nothing. A line that cannot be carried is reported and skipped.

**The recorded masks** exist for the adoption: the new controller has never been
written, and the answer must carry across. They record what each driver **asked
for**, not the old registers, so a line the 8259A withheld for its own reasons (a
masked cascade) is not withheld for ever.

**Resolution is not one-to-one.** Under QEMU's override, line 0 resolves to global
interrupt 2, and line 2 to 2 by identity. **An explicit declaration wins**: line 2
has no input, which is true (it is the cascade). This is applied when entries are
programmed, masked and unmasked; otherwise masking line 2 masks the timer.

## Verification

In [`../../kernel/test/arch/interrupts.c`](../../kernel/test/arch/interrupts.c).

`KernelVerifyInterruptStubs` raises `INT3` with a sentinel in `RAX` and inspects the
frame, since a wrong layout yields plausible values rather than errors:

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| The dispatch count rose. | No gate, or the stub not reaching the dispatcher. |
| `vector` is 3; `error_code` is 0. | Wrong number or offset; no substitute code. |
| `rax` holds the sentinel. | Registers saved in another order than the structure. |
| `cs` is `0x08`; `rip` is inside the kernel; `rsp` is above the frame. | A displaced frame; the handler's stack pointer recorded. |
| A second interrupt at vector 42 reports 42. | Only one stub correct, or all at one address. |
| Ten vectors are recorded as pushing an error code. | C and assembly disagreeing on the set. |

`KernelVerifyDispatcher` and `KernelVerifyExceptions`: a probe handler writes
`frame->rax` and the interrupted code sees it; a page of the arena is remapped
read-only and written, with a probe that restores write permission. The fault
occurs, `CR2` is the address, the code says protection, write, supervisor, and the
restarted write completes: the fault-fix-restart shape of copy-on-write.
Dispositions are asserted in [`FAULTSCREEN.md`](FAULTSCREEN.md).

`KernelVerifyPic`, in [`../../kernel/test/dev/devices.c`](../../kernel/test/dev/devices.c):

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| Every line is masked after initialisation; nothing is in service. | The mask ICW1 cleared never restored. |
| Unmasking IR1 clears only its bit; unmasking IR12 also unmasks the cascade. | A read-modify-write disturbing neighbours; a slave line that can never interrupt. |
| **With interrupts on and every line masked, nothing is delivered.** | Not remapped: the running timer would arrive as vector 8, a double fault, at once. (ICW2 cannot be read back; this is the only proof.) |

`KernelVerifyIrq`, in the same file, before the adoption:

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| The 8259A is recorded as answering. | Adoption before the drivers claimed lines. |
| A claimed line's request reaches its handler. | Vector and line confused. |
| An unclaimed line's request is counted and acknowledged. | An unclaimed device silencing lower lines. |
| IR7 with an empty in-service register is spurious: not routed, **not acknowledged**. | The lost-interrupt defect. |

After the adoption, `KernelVerifyApicRouting` ([`../devices/APIC.md`](../devices/APIC.md))
counts timer ticks through the new path.

## Limitations

1. Only copy-on-write page faults are resolved; there is no demand paging or stack
   growth.
2. One IST entry, for the double fault, so a fault on a bad stack is reported; the
   page fault has none (an IST stack does not nest, and the handler may fault).
3. Sixteen request lines only; one driver per line (`IrqInstallHandler` replaces,
   and shared PCI lines have no representation).
4. No path back to the 8259A once the APIC is adopted; a machine whose APIC fails
   never leaves it.
5. The 8259A is initialised even where `PCAT_COMPAT` says there is none; the writes
   are harmless, and nothing unmasks a line there before adoption.
6. The handler table, the recorded masks and the 8259A masks are unsynchronised
   ([`CONCURRENCY.md`](CONCURRENCY.md)).
