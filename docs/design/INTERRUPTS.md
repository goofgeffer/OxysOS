<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Interrupt and Exception Handling

**Corresponding phase**: Phase 3, sub-tasks 3.1 to 3.5; Phase 6, sub-task 6.12,
which added the controller-neutral request layer of Section 10; and Phase 6,
sub-task 6.13, which added the conditional segment-base exchange of Section 3.3.

**Specifications**: Intel 64 and IA-32 Architectures Software Developer's Manual,
Volume 3A, Chapter 6; Volume 3A, Sections 3.4, 3.4.4 and 3.5; Volume 2A and 2B,
`LGDT/LIDT`, `SGDT/SIDT`, `IRET/IRETQ` and `SWAPGS`; Intel 8259A Programmable Interrupt Controller
datasheet, sections "INITIALIZATION COMMAND WORDS (ICWS)" and "OPERATION COMMAND
WORDS (OCWS)"; IBM Personal Computer AT technical reference; ACPI Specification
6.5, Sections 5.2.12.4 and 5.2.12.5.

The two controllers that supersede the 8259A pair are documented apart, in
[`../devices/APIC.md`](../devices/APIC.md), and the tables that describe them in
[`../devices/ACPI.md`](../devices/ACPI.md). What is here is the kernel's side of
the arrangement: the descriptor table, the stubs, the dispatcher, the exception
handlers, and the layer through which a device driver claims a request line
whichever controller is answering.

## 1. The path an interrupt takes

```
Processor presents vector N
      |
      |  Reads gate N from the IDT (index = N x 16).
      |  Reads the descriptor named by the gate's selector from the GDT.
      |  Aligns RSP to 16, pushes SS, RSP, RFLAGS, CS, RIP.
      |  Pushes an error code, for the ten vectors that produce one.
      |  Clears IF, the gate being an interrupt gate.
      v
InterruptStubN
      |  Pushes a zero error code, if the processor pushed none.
      |  Pushes N.
      v
InterruptCommonStub
      |  Pushes the fifteen general-purpose registers.
      |  Clears the direction flag.
      |  RDI <- RSP, the address of the completed frame.
      v
InterruptDispatch(TrapFrame *)
      |
      v  returns
InterruptCommonStub
      |  Pops the registers, discards the vector and the error code.
      |  IRETQ.
      v
The interrupted instruction stream.
```

## 2. The interrupt descriptor table

Each gate is sixteen bytes, and the index is the vector scaled by sixteen, per
Intel SDM, Volume 3A, Section 6.14.1. The table holds 256 gates, so its limit is
`0xFFF`.

Every gate is an **interrupt gate** at descriptor privilege level zero, not a
trap gate. An interrupt gate clears the interrupt flag upon entry; a handler that
could itself be interrupted before saving its state would corrupt that state. The
privilege level of zero means user code cannot raise these vectors with `INT n`;
the vectors user code is to be permitted acquire level three when they are
introduced in Phase 6.

## 3. The irregularities the stubs normalise

The stubs exist because the processor presents its state inconsistently. There
were two until sub-task 6.13, which added a third.

### 3.1 The vector number is not recorded

Nothing in the frame the processor pushes says which vector was presented. Each
stub therefore pushes its own number. This is the only reason 256 distinct stubs
are needed rather than one.

### 3.2 Only some vectors push an error code

Per Intel SDM, Volume 3A, Table 6-1:

| Vector | Mnemonic | Error code |
| ------ | -------- | ---------- |
| 8 | `#DF` Double Fault | Yes (always zero) |
| 10 | `#TS` Invalid TSS | Yes |
| 11 | `#NP` Segment Not Present | Yes |
| 12 | `#SS` Stack-Segment Fault | Yes |
| 13 | `#GP` General Protection | Yes |
| 14 | `#PF` Page Fault | Yes |
| 17 | `#AC` Alignment Check | Yes (always zero) |
| 21 | `#CP` Control Protection | Yes, in later revisions |
| 29 | `#VC` VMM Communication | Yes, AMD64 |
| 30 | `#SX` Security Exception | Yes, AMD64 |

Every other vector pushes none. A stub for such a vector pushes a zero in its
place, so that the frame has the same shape whatever the vector and the
dispatcher need not know, for each of 256 cases, where the frame begins.

**Vectors 21, 29 and 30.** The revision of Table 6-1 consulted lists vectors 21
to 31 as reserved; later revisions define `#CP`, and the AMD64 architecture
defines `#VC` and `#SX`. All three are treated as pushing an error code because
the two possible errors are not symmetric. If such an exception is never raised,
the treatment is immaterial. If one is raised on a processor that does push an
error code, and the stub pushed a further zero, every field beyond that point
would be displaced by eight bytes and the resulting diagnosis would be nonsense.

**A hazard.** `INT n` never pushes an error code, whatever the vector. The stubs
for those ten vectors must therefore never be reached by `INT n`, which would
leave the frame eight bytes short. Nothing in this kernel invokes them that way.

### 3.3 The segment base is not exchanged

Added at sub-task 6.13, and it is the irregularity that had been harmless only
because nothing in the kernel read `GS`.

The kernel keeps its per-processor data area in `GS.base` while it executes and
in `IA32_KERNEL_GS_BASE` while a user program does. `SYSCALL` is entered only
from privilege level 3, so the system-call path exchanges the two
unconditionally. **An interrupt is not.** It may arrive at either privilege
level, and the processor performs no exchange either way — so an interrupt taken
while a user program was running enters kernel code with the program's segment
base in place, and the first spinlock the handler takes reaches for the area
through it.

The common stub therefore tests the low two bits of the saved `CS` and exchanges
where they are not zero. The test is made against the frame rather than against a
register because at that moment no register can be trusted to hold anything: the
interrupted code owned all of them. The frame is read **again** on the way out
rather than a decision being remembered, because a handler is permitted to alter
it — Section 7.2 — and the exchange must match the privilege level being returned
to and not the one that was left.

The offset the stub reads `CS` at is `TRAP_FRAME_CS_OFFSET`, asserted against the
structure by a `_Static_assert` in `kernel/arch/x86_64/interrupt/interrupts.c`. A field inserted
above `cs` without that assertion would leave the stub testing the saved `RIP`,
whose low two bits are whatever the interrupted instruction's address happened to
end in. The design is
[`CONCURRENCY.md`](CONCURRENCY.md), Section 3.3.

## 4. The trap frame

Declared in `kernel/include/oxys/interrupts.h`, in ascending order of address:

| Offset | Field | Pushed by |
| ------ | ----- | --------- |
| 0–112 | `r15` … `r8`, `rbp`, `rdi`, `rsi`, `rdx`, `rcx`, `rbx`, `rax` | The common stub, `rax` first |
| 120 | `vector` | The per-vector stub |
| 128 | `error_code` | The processor, or the stub as a zero |
| 136 | `rip` | The processor |
| 144 | `cs` | The processor |
| 152 | `rflags` | The processor |
| 160 | `rsp` | The processor |
| 168 | `ss` | The processor |

Twenty-two quadwords, 176 bytes, asserted at compilation.

`RSP` is absent from the saved register set. The stack pointer of the interrupted
code is recorded by the processor in the field of that name; pushing the register
would record the handler's own stack pointer instead, and the two would be
confused.

The structure and `kernel/arch/x86_64/interrupt/interrupt_stubs.asm` are one interface expressed in
two languages. Neither may be changed without the other.

### 4.1 Stack alignment

The processor aligns `RSP` to sixteen bytes before pushing its frame, per Intel
SDM, Volume 3A, Section 6.12.1. Five quadwords, plus the vector and the error
code, plus fifteen registers, is twenty-two quadwords — 176 bytes, a multiple of
sixteen. `RSP` is therefore aligned at the `CALL`, which then pushes the eight
bytes the System V ABI expects upon entry to a function.

This holds identically for the vectors that push an error code, because the
processor's extra push replaces the stub's.

## 5. The global descriptor table, and why it appears here

The table loaded by `boot/boot.asm` resides in the `.boot` section at physical
`0x101000`, reachable only through the identity mapping.

**Sub-task 2.3 removed that mapping**, and nothing noticed. No segment register
was reloaded thereafter, so the cached descriptors remained in force and the
table itself was never read again — until sub-task 3.2 installed interrupt gates.
Delivering an interrupt obliges the processor to read the descriptor named by the
gate's selector. That read faulted:

```
0: v=03 e=0000 i=1 cpl=0 IP=0008:ffffffff80107766
check_exception old: 0xffffffff new 0xe
1: v=0e e=0000 i=0 cpl=0 CR2=0000000000101008
2: v=08  ->  triple fault
```

`CR2` is `0x101008`: the code descriptor at selector `0x08`, eight bytes into a
table at `0x101000`.

A minimal kernel table is therefore established in `kernel/arch/x86_64/cpu/gdt.c`, residing
in `.data` in the higher half. It reproduces the three descriptors of the boot
table, and `kernel/arch/x86_64/cpu/gdt.asm` reloads every segment register including `CS`,
which cannot be assigned by an ordinary instruction and is changed by a far
return.

The table is deliberately not `const`. Intel SDM, Volume 3A, Section 3.4.2,
provides that the processor sets the accessed bit of a descriptor when its
selector is loaded, and these descriptors have that bit clear. In read-only
memory the first segment load would itself fault.

Phase 6, sub-task 6.1, has since extended this table to seven descriptors in
eight slots, adding the user-mode code and data segments and the sixteen-byte
task state segment descriptor. The order of the three user descriptors is not
free: it is fixed by the arithmetic `SYSCALL` and `SYSRET` derive their selectors
by. See [`PRIVILEGE.md`](PRIVILEGE.md), Section 2.

### 5.1 The general lesson

A structure that the processor reads directly must remain mapped for as long as
the processor may read it, and the processor's reads are not visible in the
source. The interrupt descriptor table, the task state segment of Phase 6 and the
application processor trampoline of sub-task 6.14 are all of this kind.

Sub-task 6.1 met the same lesson from its other side, and it is recorded in
[`PRIVILEGE.md`](PRIVILEGE.md), Section 6: such a structure must also be
*established* at a point where the processor's rejection of it can be reported.
`LTR` faults upon a malformed task state segment descriptor, and a fault raised
before any gate existed would have escalated to a reset.

## 6. Verification

`KernelVerifyInterruptStubs` raises `INT3` with a sentinel value in `RAX`, then
checks the recorded frame. Testing that an interrupt was merely *taken* would
prove little; an error in the frame layout does not announce itself, because the
dispatcher would read plausible values from the wrong offsets.

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| The dispatch count rose | The gate was not installed, or the stub did not reach the dispatcher. |
| `vector` is 3 | The stub pushed the wrong number, or the field is at the wrong offset. |
| `rax` holds the sentinel | The common stub saved the registers in a different order from the one the structure declares. |
| `error_code` is zero | The stub did not substitute for the absent error code. |
| `cs` is `0x08` | The frame is displaced, or the gate names the wrong selector. |
| `rip` lies within the kernel image | The frame is displaced. |
| `rsp` lies above the frame | The processor's stack pointer field was confused with the handler's. |
| A second interrupt at vector 42 reports 42 | Only the first stub is correct, or all stubs share one address. |
| Ten vectors are recorded as pushing an error code | The C and the assembly disagree about the set, which would displace the frame for precisely the faults that matter most. |

`INT3` is usable for this because the breakpoint exception is a trap rather than
a fault: it reports the state *after* the instruction, so returning resumes at
the instruction following. A fault would be restarted and would re-enter without
end.

## 7. The dispatcher

Sub-task 3.3 introduces a table of 256 handler pointers and the registration
interface of `kernel/include/oxys/interrupts.h`.

### 7.1 Routing

`InterruptDispatch` records the frame, increments the counters, and calls the
handler registered for the vector. Where none is registered the treatment depends
upon the vector, and the distinction is not arbitrary:

| Vector range | Unregistered treatment | Reason |
| ------------ | ---------------------- | ------ |
| 0–31 | Reported and fatal | Intel SDM, Volume 3A, Section 6.5: most of these are *faults*, which report the state before the offending instruction and restart it upon return. Returning without removing the cause re-enters the same exception without end. Halting with a diagnosis is the only outcome that yields information. |
| 32–255 | Counted and ignored | Not architecture-defined; nothing is restarted. This is the correct treatment of a spurious interrupt, which the 8259A of sub-task 3.5 is known to deliver. |

### 7.2 A handler may alter the frame

The frame is passed by address and is not `const`. Any change a handler makes is
restored into the registers by the common stub and becomes the state of the
interrupted code.

This is not a convenience. A handler that could observe the interrupted state but
not change it would suffice for reporting and for nothing else. Correcting a
page fault, delivering the result of a system call, and switching context all
require exactly this property, and the self-test asserts it directly: a probe
handler writes a known value into `frame->rax`, and the interrupted code observes
that value in `RAX` after the return.

### 7.3 The default breakpoint handler

`InterruptInitialise` registers a handler for vector 3. Without it an `INT3`
would be an exception with no handler, and therefore fatal under the rule above.

The breakpoint is a trap rather than a fault, so recording it and returning is
safe, and it makes `INT3` usable as a diagnostic marker in kernel code that has
no debugger attached.

## 8. The exception handlers

Sub-task 3.4 registers a handler for every architecture-defined vector.

### 8.1 Disposition

Every architecture-defined exception has one of three dispositions, and
`ExceptionDispositionOf` decides which from **the vector and the privilege level
together**. It is a pure function of those two, which is what lets the whole of
it be asserted without raising a single exception — and what let the half
concerning privilege level 3 be asserted at sub-task 6.4, six sub-tasks before
anything ran there.

| Disposition | Meaning |
| ----------- | ------- |
| `RESUME` | The cause was removed, or there was never one. Execution continues. |
| `TERMINATE` | The fault belongs to the program that raised it. That program ends; the machine does not. |
| `FATAL` | The kernel cannot continue. The machine halts, and **only this draws a fault screen**. |

| Vectors | Raised at privilege level 3 | Raised at privilege level 0 |
| ------- | --------------------------- | --------------------------- |
| 3 `#BP`, 4 `#OF` | `RESUME` | `RESUME` |
| 14 `#PF`, resolvable by copy-on-write | `RESUME` | `RESUME` |
| 2 NMI, 8 `#DF`, 18 `#MC` | `FATAL` | `FATAL` |
| 10 `#TS`, 11 `#NP` | `FATAL` | `FATAL` |
| 0 `#DE`, 6 `#UD`, 12 `#SS`, 13 `#GP`, 14 `#PF` unresolved, 17 `#AC`, and every other vector | `TERMINATE` | `FATAL` |

### 8.1.1 Why the privilege level decides it

**This was wrong until sub-task 6.4 and the correction is worth recording.**
Every exception was treated as fatal to the machine, so a divide by zero — the
plainest mistake a program can make, and one that must cost that program and
nothing else — would have halted the system and drawn a full-screen page saying
so. That is not a missing feature; it is a false statement about what happened.

The vector alone cannot decide it. The same page fault is a program to be
destroyed when it comes from privilege level 3 and a kernel that cannot continue
when it comes from privilege level 0. What separates them is the low two bits of
the code segment selector the processor pushed, which are the privilege level the
faulting code was actually running at.

Three sets do not depend upon the privilege level, and each for its own reason:

**The aborts**, `#DF` and `#MC`. Intel SDM, Volume 3A, Section 6.5: an abort
permits no reliable resumption and the state it reports may not describe where
the error occurred. A double fault says the processor could not deliver an
earlier exception, which is a statement about the machine; a machine check is the
hardware reporting a fault in itself.

**The non-maskable interrupt.** Memory parity, a watchdog, a bus error. Nothing a
program did caused it, and terminating whatever happened to be running when it
arrived would blame the wrong thing.

**The descriptor-table faults**, `#TS` and `#NP`. The descriptor tables are the
kernel's own data. Whoever tripped over a malformed one, terminating them would
leave the same malformed descriptor in place for the next program to meet.

### 8.1.2 What `TERMINATE` does, and what it did before there was anything to terminate

**The classification was written at sub-task 6.4, when it could not be reached.**
There were no programs then, so nothing outside the kernel could raise such a
fault and there was nothing to destroy; `TERMINATE` was unreachable, and
`ExceptionTerminateProgram` panicked upon arrival because there was nothing to
end and nowhere to return to. It was classified anyway, because the disposition
is what decides whether a fault screen is drawn, and because it could be
asserted without being reached: `ExceptionDispositionOf` is a pure function of a
vector and a privilege level, so the self-test asks it about every vector at
both levels. That is the same idiom sub-task 6.1 used upon `SYSCALL` —
exercising a mechanism where the condition it exists for has not yet arrived.

**Sub-task 6.10 supplied the condition.** A program now runs at privilege level
3, and a fault it raises reaches this path: `ThreadTerminateCurrent` marks the
thread ended, records the exit status as the vector negated, and switches back to
whichever thread started it. The machine carries on and no screen is drawn. The
self-test of that sub-task ends its program by an undefined instruction on
purpose and asserts an exit status of −6, which is what establishes that the
program reached its last instruction rather than merely its first.

What is still absent is the rest of the ordinary path: the process is marked
`PROCESS_EXITED` but its address space is not released, nobody collects the
status, and there is no scheduler to pick another thread. Those arrive with
`exit()` and `wait()` at sub-task 6.11 and the scheduler at 6.15.

### 8.1.3 Where the judgement is made when there is nobody to return to

`ThreadTerminateCurrent` returns false where nothing started the thread that
faulted, which means privilege level 3 was reached by something that did not go
through `ThreadStart`. That cannot be continued from: returning would return
through the dispatcher to `IRETQ`, which restarts the faulting instruction, which
faults again — the machine reporting the same fault for ever without progress,
which is exactly the hazard the fatal handler exists to avoid, arriving by a
different route.

**The judgement is made once, in `ExceptionTerminateProgram`**, which is the
function that knows whether the termination succeeded. It was made at each of the
two call sites until the review that followed sub-task 6.10, and the duplication
had already begun to rot: the two sites panicked with two different messages, and
one of them said the fault had been "raised before any program exists", which
ceased to be true at 6.10. A condition tested in two places is a condition
described in two places, and they do not stay in agreement.

### 8.2 The two error-code formats

An exception presents one of two entirely different error codes, and decoding one
as the other yields nonsense.

**The page fault**, per Intel SDM, Volume 3A, Figure 6-9:

| Bit | Flag | Meaning when set |
| --- | ---- | ---------------- |
| 0 | `P` | A protection violation. When clear, no translation existed. |
| 1 | `W/R` | The access was a write. |
| 2 | `U/S` | The access was made in user mode. |
| 3 | `RSVD` | A reserved bit was set in a paging-structure entry. |
| 4 | `I/D` | The access was an instruction fetch. |
| 5 | `PK` | A protection-key violation. |
| 15 | `SGX` | An SGX access-control violation. |

Bit 0 is reported first because it separates the two fundamentally different
causes: an absent translation and a violated permission have entirely different
remedies.

**The selector form**, presented by `#TS`, `#NP`, `#SS` and `#GP`, per Section
6.13 and Figure 6-6: bit 0 `EXT` (external event), bit 1 `IDT` (the index refers
to an IDT gate), bit 2 `TI` (the LDT rather than the GDT), and bits 3 to 15 the
selector index. A code that is null but for `EXT` denotes no specific segment, or
a null selector.

### 8.3 `CR2` is read first

Intel SDM, Volume 3A, Section 6.15, warns that a further page fault may occur
while the handler runs, and that `CR2` must be saved before that can happen.
Every handler here reads it as its first action.

### 8.4 `CR0.WP`

`PagingInitialise` now sets the write-protect flag in `CR0`.

Section 6.15 provides that user-mode code always faults upon writing to a
read-only page, but that supervisor-mode code does so **only when `CR0.WP` is
set**. The flag is clear upon reset and GRUB does not set it.

Without it the read-only mappings of sub-task 2.3 were advisory: the kernel could
write straight through them and no fault would arise, so the protection recorded
in the paging structures did not exist in fact. It is equally a prerequisite of
copy-on-write, whose entire mechanism is a write to a page deliberately marked
read-only.

### 8.5 The stack reproduction

A fatal report reproduces up to eight quadwords from `RSP`, but only after
confirming through `PagingTranslate` that each is mapped. A fault taken with a
corrupt stack pointer is exactly the case in which a report is most wanted, and
reading through the bad pointer would raise a second fault and lose the report
entirely.

### 8.6 The deferred negative test

Sub-task 2.3 could confirm the read-only kernel mappings only by inspecting the
paging structures in software, which establishes what the entries *say* rather
than what the processor *does*. With a page-fault handler that test becomes
possible and has been performed.

A page is taken from the kernel arena and remapped read-only; a probe handler is
substituted for the fatal default using the registration interface of sub-task
3.3; the page is written to; and the fault is examined. The probe then restores
write permission, so that the restarted instruction succeeds.

That resolution is the point. A page fault is a fault, not a trap: the offending
instruction is restarted upon return, so a handler that merely recorded the fault
would be re-entered without end. The shape — fault, alter the mapping, restart —
is exactly that of the copy-on-write handler of sub-task 2.8, which will differ
only in substituting a private copy of the frame for a shared one.

The assertions are that a fault occurred at all; that `CR2` holds the address
written; that the error code records a protection violation rather than an absent
translation, a write rather than a read, and a supervisor-mode access; and that
the faulting instruction completed.

## 9. The 8259A programmable interrupt controller

Sub-task 3.5 remaps the pair of cascaded controllers and establishes the
end-of-interrupt protocol. The implementation is `drivers/pic/pic.c`; the
interface is `kernel/include/oxys/pic.h`.

### 9.1 Why remapping is not optional

The 8259A holds no vector base of its own; it presents whatever ICW2 last
supplied it. The firmware of the IBM Personal Computer AT and its successors
programmes the master to present vectors 8 to 15 and the slave 0x70 to 0x77, and
that is the state in which the kernel receives the machine. The distinction
matters: the collision described here is a property of the firmware, recorded in
the IBM Personal Computer AT technical reference, and not a property of the
device.

The first of those ranges is precisely that which Intel SDM, Volume 3A, Section
6.2, reserves for architecture-defined exceptions:

| Line | Vector as the firmware leaves it | The exception it collides with |
| ---- | -------------------------------- | ------------------------------ |
| IR0 (timer) | 8 | `#DF` Double Fault |
| IR1 (keyboard) | 9 | Coprocessor Segment Overrun |
| IR2 (cascade) | 10 | `#TS` Invalid TSS |
| IR3 | 11 | `#NP` Segment Not Present |
| IR4 | 12 | `#SS` Stack-Segment Fault |
| IR5 | 13 | `#GP` General Protection |
| IR6 | 14 | `#PF` Page Fault |
| IR7 | 15 | (Intel reserved) |

A timer tick would be indistinguishable from a double fault, and a keystroke
from a page fault. The collision is not detectable after the event: the
processor presents a vector and nothing else, and the error code the exception
handler would decode is whatever happened to lie upon the stack.

The controllers are therefore remapped to vectors 32 to 47, 32 being the first
vector Section 6.2 leaves available. The base must be divisible by eight, since
ICW2 supplies only bits 7 to 3 of the vector and the controller fills bits 2 to 0
with the request level.

### 9.2 The initialisation sequence

The 8259A datasheet, section "INITIALIZATION COMMAND WORDS (ICWS)", defines a
sequence of four words that must be issued in order, each to the port the
controller expects next. A write to the command port with bit 4 set is
interpreted as ICW1 and begins the sequence.

| Word | Port | Master | Slave | Meaning |
| ---- | ---- | ------ | ----- | ------- |
| ICW1 | command | `0x11` | `0x11` | Begin initialisation; ICW4 will follow; cascaded, edge-triggered. |
| ICW2 | data | `0x20` | `0x28` | The vector base of each controller. |
| ICW3 | data | `0x04` | `0x02` | The cascade wiring. |
| ICW4 | data | `0x01` | `0x01` | The 8086 mode. |

ICW3 is expressed differently at the two controllers, which is easily mistaken
for an inconsistency. At the master it is a *bit mask* of the request lines
bearing a slave, so a slave upon IR2 is `1 << 2`, that is `0x04`. At the slave it
is a *number*, the cascade identity, which must equal the master line the slave
is attached to, that is `2`. Writing `0x04` to the slave would give it identity
4, and it would ignore every cascade acknowledgement the master issued.

ICW4 selects the 8086 mode, in which the controller presents an eight-bit vector.
Without it the controller presents a `CALL` instruction in the manner of the
8080, and the processor would execute nonsense.

**ICW1 clears the interrupt mask register.** The datasheet lists this among the
actions the word performs automatically, together with resetting the edge sense
circuit, assigning IR7 the lowest priority, setting the slave mode address to
seven and clearing the special mask mode. Every line is therefore *permitted* at
the instant the sequence completes. `PicInitialise` masks all sixteen
immediately afterwards, before anything can be delivered; masking them before the
sequence would accomplish nothing, the sequence itself undoing it.

### 9.3 Why every line begins masked

A device whose driver does not yet exist would otherwise present a request that
nothing could service. That is worse than it sounds. The controller withholds
every request of equal or lower priority until the bit standing in its in-service
register is reset, so a single unserviced request upon a high-priority line
silences every line beneath it permanently. The interval timer is IR0, the
highest priority of all.

A driver unmasks its own line when it is ready to receive from it. Sub-task 3.6
unmasks IR0 and sub-task 3.7 unmasks IR1.

### 9.4 Why the controller owns the end-of-interrupt

`PicSendEndOfInterrupt` is written once, in `pic.c`, and is called by the routing
layer of Section 11 upon the return of the device handler. A device driver
neither may nor need call it.

The alternative — each driver signalling for itself — distributes a piece of
protocol that belongs to the controller across every driver that will ever exist,
and makes the consequence of forgetting it the permanent silencing of the
machine rather than a local defect. There is nothing device-specific in the
decision, so there is no reason for a device to make it.

The cascade makes the same point. A request of the slave stands in the in-service
register of **both** controllers, the master having accepted it upon IR2, so both
must be signalled. Signalling only the slave would leave the master withholding
every line of priority below IR2 — which is IR3 to IR7 and the whole of the
slave. `PicSendEndOfInterrupt` therefore signals the slave first and the master
always.

The command used is the non-specific end-of-interrupt, OCW2 with R=0, SL=0 and
EOI=1, which resets the highest priority bit set in the in-service register. It
is correct here because the controller is operating in the fully nested mode it
is initialised into, in which the highest priority bit in service is necessarily
the one being completed.

### 9.5 The spurious request

The 8259A presents its lowest priority line — IR7 at the master, IR15 at the
slave — when a request it had begun to accept is withdrawn before the vector is
read. The commonest causes are noise upon the line and an end-of-interrupt sent
at the wrong moment by an earlier handler.

No line is genuinely in service, so **the bit is absent from the in-service
register**, and that absence is the only means of distinguishing the case: the
vector presented is identical to that of a real IR7.

A spurious request must not be acknowledged. An end-of-interrupt issued in that
state would reset the bit of whatever line *was* genuinely in service, discarding
a real interrupt. The defect this produces is among the least tractable a kernel
can have: it occurs at a rate governed by electrical noise, loses an interrupt
belonging to an unrelated device, and reproduces nowhere.

The slave's IR15 is the one exception, and it is not symmetric. The master did
accept the cascade request and does hold a bit in its own in-service register, so
the master alone is signalled and the slave is not.

### 9.6 Verification

`KernelVerifyPic` asserts the properties of the device whose violation would be
silent. The routing above it is asserted by `KernelVerifyIrq`, in Section 10.6;
both halves were asserted here until sub-task 6.12 separated them.

| Assertion | The failure it detects |
| --------- | ---------------------- |
| Every line is masked after initialisation | The mask that ICW1 cleared was never re-established. |
| Nothing is in service before a line is unmasked | The in-service register is not being read as intended. |
| Unmasking IR1 clears exactly its bit | A mask read-modify-write that disturbs its neighbours. |
| Unmasking IR12 also unmasks the cascade | A slave line permitted at the slave whose requests can never reach the processor — a device that simply never interrupts. |
| The interrupt flag may be set with every line masked, and nothing is delivered | **The remapping itself.** |

The last deserves comment, because it is the only assertion that establishes the
remapping and it cannot be made by inspection. Reading the vector base back from
the controller is not possible; the 8259A does not present ICW2 for reading.
Instead the interrupt flag is set with every line masked and time is allowed to
pass. The firmware leaves the interval timer running, so IR0 is being asserted
throughout. Were the controllers still presenting the firmware's vectors, that
request would arrive as vector 8 — the double fault — the instant the flag was
set, and the machine would not reach the following line. Reaching it, with the
request count unmoved, establishes both that the remapping took effect and that
the mask is honoured.

### 9.7 Observed state

| Quantity | Value |
| -------- | ----- |
| Vectors | 32 to 47 |
| Mask after initialisation | `0xFFFF` |
| Mask after sub-task 6.12 retires the pair | `0xFFFF` |

### 9.8 Limitations

1. Nothing here is safe against concurrent access. The read-modify-write of a
   mask register is not atomic, and requires the spinlock governing this device.
   The lock has existed since sub-task 6.13 and has not been applied here, there
   being one flow of control that reaches it — a processor started by sub-task
   6.14 is parked and registers no handler, and 6.15 gave it only kernel threads.
   The change that widens a user thread's affinity mask is what makes the
   register contended.
2. **The pair is retired, not removed.** `PicDisable` masks both mask registers
   and clears the flag that governs the report; the controllers are still
   remapped and still hold their initialisation. Nothing re-enables them, and
   there is no path back to them once the APIC has been adopted — a machine whose
   I/O APIC failed to initialise never leaves the 8259A in the first place.
3. The pair is initialised upon every machine, including one whose MADT declares
   `PCAT_COMPAT` clear and therefore has no such pair to initialise. The writes
   go to ports nothing decodes and are harmless; what is not harmless is
   *unmasking* a line there, which nothing does before the APIC is adopted.

## 10. The interrupt request layer

Sub-task 6.12 introduced `kernel/arch/x86_64/interrupt/irq.c` and the interface of
`kernel/include/oxys/irq.h`. It is the one place a device driver claims a request
line through, whichever controller is presently delivering it.

### 10.1 Why it exists

Until that sub-task every driver called `PicInstallHandler` and `PicUnmaskLine`.
That named the 8259A in the source of a keyboard driver, a mouse driver, a timer
driver and a serial driver, none of which has anything to do with the 8259A. It
was nevertheless accurate, there being one controller.

The moment a second controller exists it stops being accurate, and two things go
wrong at once. Every one of those calls becomes a statement that is no longer
true; and each driver acquires a decision — *which controller am I upon?* — that
is not its business and that four drivers would answer four times, in four places
that would not stay in agreement.

**The line number is the durable fact.** IR1 is the keyboard whether the request
arrives from a 8259A as vector 33 or from an I/O APIC input carrying global
system interrupt 1 as vector 33. ACPI 6.5, Section 5.2.12.4, is what guarantees
it: upon a machine supporting both models the first sixteen global system
interrupts carry the 8259A request lines, save where an override says otherwise —
and the override is resolved here, not by the driver.

### 10.2 What it owns and what it does not

| Owned here | Owned by the controller driver |
| ---------- | ------------------------------ |
| The table of handlers, by line number | The mask registers, and the writes to them |
| The vector each line is presented upon | The initialisation sequence of the device |
| The routing of a vector to a handler | The recognition of a spurious request |
| Which controller is answering | The end-of-interrupt command itself |
| The mask each driver asked for | — |

The division follows from what each fact belongs to. That a request of the
8259A's slave stands in the in-service register of both controllers is a property
of that device, and `pic.c` keeps it. That IR1 belongs to the keyboard is a
property of the machine, and does not change when the controller does.

### 10.3 Why the layer signals the completion, and not the driver

The argument of Section 9.4 survives the change of controller and gains a second
part.

The first part is unchanged: the signalling is a property of the controller
rather than of any device, so there is no reason for a device to decide it, and
the consequence of forgetting it is not a local defect but the silencing of every
line of lower priority.

The second part is new. **The command differs by controller.** Under the 8259A it
is an operation command word written to one or both of two I/O ports; under the
APIC it is a write to the Local APIC's end-of-interrupt register, and there is no
cascade to consider. A driver that signalled for itself would have to know which,
which is precisely the knowledge this layer exists to hold.

### 10.4 The adoption

`IrqAdoptApic` is the whole of the retirement, and it happens in one place.

```
   Requires: the Local APIC enabled, at least one I/O APIC mapped,
             and the interrupt flag clear.

1. PicDisable                      Both mask registers set to 0xFF.
2. For each of the sixteen lines:
     resolve the line to its global system interrupt
     programme the redirection entry: vector 32 + line,
       destination this processor, polarity and trigger from ACPI
     set the mask to what the line's claimant asked for
3. Record the APIC as the controller answering.
```

A driver observes nothing. It keeps the same line number, the same vector and the
same handler, and its device keeps interrupting.

**The interrupt flag must be clear.** Between step 1 and step 2 there is no
controller that will deliver a device's request, and one raised in that interval
would simply be lost. `KernelMain` performs the adoption while the flag is still
clear, which it is for the whole of the initialisation.

**The 8259A is masked first, and not last.** ACPI 6.5, Table 5.20, requires it of
any machine declaring `PCAT_COMPAT`. Two controllers presenting one device upon
one vector would deliver every request twice, and the second delivery would be
acknowledged at a controller that had not sent it.

**A line that cannot be carried is reported and skipped**, and the adoption
continues. It is not a reason to abandon the change: the lines that do have
inputs are better served by them than by a controller that has just been masked.

### 10.5 The recorded mask state, and why it exists

A controller's mask register is the truth while that controller is answering, and
`IrqLineIsMasked` reads it rather than any copy. The layer nevertheless records
what each driver asked for.

The record exists for one instant: the adoption, at which the old controller's
registers are about to be abandoned and the new controller's have never been
written. Something must carry the answer across.

It must be a record of what each driver *asked for* rather than a copy of the old
registers. A line the 8259A was withholding for a reason of its own — a slave line
whose cascade was masked, say — must not become a line the I/O APIC withholds for
ever.

### 10.6 Verification

`KernelVerifyIrq` asserts the routing while the 8259A is still answering. It is
the half of the old `KernelVerifyPic` that was about the layer rather than about
the device.

| Assertion | The failure it detects |
| --------- | ---------------------- |
| The 8259A is recorded as the controller answering | An adoption that ran before the drivers claimed their lines. |
| A request upon a claimed line enters its handler | The layer confusing the vector with the request line, delivering every interrupt to the wrong driver. |
| A request upon an unclaimed line is counted and acknowledged | An unclaimed device silencing every line beneath it. |
| A request upon IR7 with an empty in-service register is counted spurious, not routed, and above all not acknowledged | The lost-interrupt defect of Section 9.5. |

`KernelVerifyApicRouting` asserts the same path after the adoption, and is
described in [`../devices/APIC.md`](../devices/APIC.md), Section 8. The assertion
that matters there is the last: the interval timer is let run and its ticks
counted, which is the only thing that establishes the whole path from a device
pin to a handler.

### 10.7 The defect the self-test found on its first run

It is recorded because the reasoning that produced the fault was reasonable and
the fault was invisible from every direction but one.

The lines were programmed in ascending order, each resolved to its global system
interrupt. QEMU's tables declare that ISA request 0 — the interval timer — is
carried by global interrupt 2. Request line 2 has no override, so it resolves by
the identity mapping to global interrupt 2 as well.

Line 2 was therefore programmed after line 0 and over it. The timer's input was
left presenting line 2's vector, masked; the machine kept running, because the
serial adapter and the keyboard were unaffected, and lost its tick.

**The resolution from line to global interrupt is not injective**, and an
override displaces a line as well as moving one. The rule is that an explicit
declaration wins over the implicit identity mapping: a line owns its global
interrupt unless some other line was expressly declared to be carried by it. Line
2 accordingly has no input, which is the truth — it is the cascade of the 8259A
and is not a device line under any controller.

The rule is applied at three places and not one: when the entries are programmed,
when a line is masked, and when a line is unmasked. Masking line 2 without it
would mask the timer.

### 10.8 Limitations

1. **Sixteen lines.** An I/O APIC input above the fifteenth cannot be claimed;
   [`../devices/APIC.md`](../devices/APIC.md), limitation 1, records why and what
   would be needed to lift it.
2. **The handler table and the recorded mask state are unsynchronised.** Both
   require the spinlock governing this layer, an interrupt handler and an
   application processor each being able to enter either. The lock has existed
   since sub-task 6.13; neither has been put under it, there being one flow of
   control that reaches either — a processor started by sub-task 6.14 is parked
   and claims no line. Sub-task 6.15 is what changes that. See
   [`CONCURRENCY.md`](CONCURRENCY.md), Section 10, limitation 1.
3. **A line may be claimed by one driver only.** `IrqInstallHandler` replaces
   whatever was registered rather than refusing, and shared interrupt lines —
   which PCI requires — have no representation here at all. Phase 11 is where
   that becomes a real want.
4. **There is no path back to the 8259A.** A machine whose APIC could not be
   initialised never leaves it; a machine that has left it cannot return.

## 11. Present limitations

1. Only a copy-on-write fault is resolved. Every other page fault is reported
   and fatal; demand paging and stack growth do not exist. The handler tests the
   error code for a write to a present page before consulting the paging
   structures at all, so the commoner faults cost nothing extra.
2. One interrupt stack table entry is used, since sub-task 6.1: the double
   fault is delivered upon a stack of its own, so a fault taken upon a bad stack
   is now reported rather than escalating to a reset. The page fault is
   deliberately given none, an interrupt stack table entry being a fixed address
   that does not nest and the page-fault handler being one that may itself
   fault. See [`PRIVILEGE.md`](PRIVILEGE.md), Section 3.2.
3. Three devices claim a request line: the interval timer of sub-task 3.6 upon
   IR0, described in `docs/devices/TIME.md`; the PS/2 keyboard of sub-task 3.7 upon IR1,
   described in `docs/devices/KEYBOARD.md`; and the 16550 serial adapter of sub-task 4.1
   upon IR4, described in `docs/devices/SERIAL.md`. Every other line remains masked until
   a driver for its device exists.
4. The interrupt flag is left clear except where a self-test sets it deliberately,
   each such test clearing it again before it returns. From sub-task 3.7 the
   echo loop of `docs/devices/KEYBOARD.md`, Section 7.2, sets it permanently at
   the completion of initialisation, and the kernel thereafter runs with
   interrupts enabled and halts between them.
