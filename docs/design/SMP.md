<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Symmetric Multiprocessing

**Phase**: 6, sub-task 6.14, of [`../project/PLAN.md`](../project/PLAN.md).
Section 2 is the protocol by which a processor is started; Section 3 is the
trampoline it starts into; Sections 4 and 5 are what the bootstrap processor
prepares and what the started processor does with it; Section 6 is the one lock
this sub-task applies.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../../kernel/arch/x86_64/smp/smp.c`](../../kernel/arch/x86_64/smp/smp.c),
[`../../boot/trampoline.asm`](../../boot/trampoline.asm) and
[`../../kernel/arch/x86_64/smp/smp_trampoline.asm`](../../kernel/arch/x86_64/smp/smp_trampoline.asm),
with [`../../kernel/include/oxys/arch/smp/smp.h`](../../kernel/include/oxys/arch/smp/smp.h). The
per-processor task state segments are in
[`../../kernel/arch/x86_64/cpu/tss.c`](../../kernel/arch/x86_64/cpu/tss.c) and their descriptors in
[`../../kernel/arch/x86_64/cpu/gdt.c`](../../kernel/arch/x86_64/cpu/gdt.c); the command register the
startup interrupts are sent through is in
[`../../drivers/apic/lapic.c`](../../drivers/apic/lapic.c); the microsecond wait
the protocol's delays are measured by is in
[`../../drivers/pit/pit.c`](../../drivers/pit/pit.c).

**Specifications**: Intel SDM, Volume 3A, Sections 8.4.3 (the MP initialisation
protocol, and the startup vector's meaning), 8.4.4.1 (the bootstrap processor's
initialisation sequence, and the inhibition of interrupts across it), 8.2.2 (the
memory-ordering model, which is what makes the parameter block visible), 9.1.4
and Table 9-1 (the processor state after a reset), 9.9.1 (switching to protected
mode), 4.1.2 and Table 4-14 (the order in which long mode is entered), 3.4.5 and
Figure 3-8 (the segment descriptor and its `L` flag), 2.5 (`CR0.WP`), 10.6.1 and
Figure 10-12 (the interrupt command register), 4.10.4.4 and 4.10.5
(invalidation, and its propagation between processors); Intel SDM, Volume 2A,
`LTR`; ACPI Specification 6.5, Section 5.2.12.2 and Table 5.23 (which declared
processors are usable); Multiboot2 Specification 2.0, Section 3.6.8 (the memory
map).

## 1. What this sub-task is, and what it is not

**It starts every processor the firmware declares usable, and gives none of them
anything to run.** A started processor loads the kernel's descriptor tables,
claims a per-processor area, loads a task state segment of its own, configures
its own model-specific registers, enables its own local controller, and then
parks in a loop that halts until it is interrupted.

That last clause is the whole of its contribution, and it is not nothing. Until
this sub-task the translation-lookaside-buffer shootdown of
[`CONCURRENCY.md`](CONCURRENCY.md), Section 6, was a mechanism exercised upon its
own sender: `ShootdownBroadcast` returned at once, having nobody to tell. From
here it is a real broadcast to a real target, and the target's acknowledgement is
written by the target, in its own area, from inside its own handler — which is
the one quantity a kernel that started nobody cannot fabricate. Section 8 is
built entirely on that observation.

**It applies exactly one lock**, and only one: the diagnostic channel, which is
the whole of what a started processor touches. Section 6 says why that is the
right boundary and why it is not more. The remaining structures listed in
[`CONCURRENCY.md`](CONCURRENCY.md), Section 10, limitation 1, are still
unsynchronised, and remain so honestly: a parked processor does not reach them.
Sub-task 6.15 is what gives it work, and the locks those structures need are what
6.15 must bring with it.

**Why this follows 6.13 rather than preceding it** is in
[`ARCHITECTURE.md`](ARCHITECTURE.md), Section 4.1. In short: starting processors
against a kernel that has no lock, no per-processor area and no way for one
processor to interrupt another produces a milestone that cannot be demonstrated,
because there is nothing to demonstrate it with.

## 2. The startup protocol

Intel SDM, Volume 3A, Section 8.4.4.1, prescribes the sequence, and
`SmpStartProcessor` performs it verbatim:

```
    INIT inter-processor interrupt, addressed to one processor
    wait 10 milliseconds
    Start-Up inter-processor interrupt, vector = trampoline page number
    wait 200 microseconds
    if not answered: Start-Up inter-processor interrupt again
    wait for the acknowledgement, up to 100 milliseconds
```

The INIT carries the level-assert and level-trigger bits, which is the manual's
own `000C4500H` less the destination shorthand — this is addressed to one
processor rather than broadcast, for the reason Section 7 gives.

**Every send is checked.** `LocalApicSendCommand` waits out a command still
pending in the register and returns false if that wait expired, and a sequence
with one step missing is worse than one never attempted: an INIT that arrived
without the startup interrupt that should have followed it leaves the processor
held in reset. That is a processor the firmware declared and this kernel then
removed from the machine, and nothing later in the boot would mention it.

### 2.1 The startup vector is the page number

Section 8.4.3 gives the address a processor begins at as `000VV000H` for a
startup interrupt carrying vector `VV`. The vector and the address are therefore
one fact written twice, and `smp.h` keeps them one with a static assertion rather
than with a comment:

```c
_Static_assert(((uint64_t)SMP_TRAMPOLINE_VECTOR << 12) == SMP_TRAMPOLINE_ADDRESS, …);
```

A second assertion requires the address to lie below the first mebibyte, which is
all a processor in real mode can address at all.

### 2.2 Why the second startup interrupt is conditional

The manual prescribes it unconditionally, and prescribing it is right for a
processor that may be slow to answer the first. Sending it to a processor that
has *already* started is a different thing: it is a startup interrupt delivered
to a processor executing kernel code, which Section 8.4.4.1 does not define.

The acknowledgement in the parameter block is what distinguishes the two cases,
and it is written by the trampoline *before* it jumps into the kernel precisely
so that it can be. See Section 3.5.

### 2.3 The delays are measured by the interval timer, with interrupts masked

Section 8.4.4.1 requires every device capable of delivering an interrupt to be
inhibited between the INIT and the last startup interrupt of a sequence. This
kernel's interrupt flag is the blunt instrument that achieves it, and the whole
bring-up runs inside one critical section entered through the per-processor area
— so the flag is restored to what it was rather than to a constant, and the depth
is counted. The kernel arrives at `SmpInitialise` with interrupts enabled and
leaves with them enabled.

That masking is what forced a new wait. `PitWaitTicks` reads a variable the
timer's interrupt handler increments, so it cannot advance with interrupts
masked, and a tick is a millisecond, so it cannot express two hundred
microseconds at all. `PitBusyWaitMicroseconds` watches the counter directly
instead, at a resolution of one count of the 1.193182 MHz input — about 838
nanoseconds — rounded upward so that a wait is never short. It returns false
where the counter is not running, so a caller is told that nothing was waited for
rather than left to assume it was.

It occupies the processor entirely, which is exactly what is wanted: there is
nothing else for the bootstrap processor to do between an INIT and the startup
interrupt that must follow it.

### 2.4 The bound upon the wait

`SMP_ANSWER_WAIT_MICROSECONDS` is a hundred milliseconds, and it is this
kernel's own figure rather than the manual's. A processor that answered the
startup interrupt reaches 64-bit mode within a few thousand instructions, so this
is four orders of magnitude of margin — and it is short enough that a machine
declaring a processor it does not physically have still finishes booting, with
the absence reported rather than the boot stopped.

**The wait is for the trampoline's acknowledgement and then, separately, for the
per-processor area.** The two answer different questions:

| What is waited for | What it establishes |
| ------------------ | ------------------- |
| `acknowledged` in the parameter block | The processor answered the interrupt and reached 64-bit mode upon this kernel's paging hierarchy. It is the last thing `trampoline.asm` can speak for. |
| The online count rising | The processor got through the kernel's own initialisation afterwards. |

A processor that satisfies the first and not the second failed somewhere this
kernel wrote, and that is worth being able to tell apart from one that never
woke. A single flag written by the C entry point would have collapsed the two
causes into one symptom.

## 3. The trampoline

A processor answering a startup interrupt begins in 16-bit real mode, with
`CR0 = 60000010H`, the interrupt flag clear, and its segment registers undefined
save `CS`, which is set from the vector (Section 9.1.4 and Table 9-1). It must be
carried from there to 64-bit mode upon the kernel's own hierarchy. That is
[`../../boot/trampoline.asm`](../../boot/trampoline.asm), 254 bytes of it.

### 3.1 Why it is a flat binary and not a section of the kernel

The code must execute from a low physical page, and the addresses it names must
be that page's — not the higher-half addresses the kernel is linked at. NASM's
`org` directive states that origin, and `org` is available only in the flat
binary format. So the image cannot be an ordinary member of `ASM_SOURCES`: the
Makefile assembles it with `-f bin` to `build/trampoline.bin`, and
[`../../kernel/arch/x86_64/smp/smp_trampoline.asm`](../../kernel/arch/x86_64/smp/smp_trampoline.asm)
embeds the result with `incbin` between two global symbols the C takes the size
from.

Embedding rather than reading it from a file at run time is not a matter of
convenience. There is no filesystem mounted at the moment processors are started,
and the build order is what makes the image and its source impossible to get out
of step: the link fails if the binary was not assembled.

The origin is a constant rather than an argument because a real-mode near
reference is an offset from a segment base, and a relocatable trampoline would
have to compute every one of them at run time from a base it was handed. Fixing
the page and *proving* the firmware calls it usable is the cheaper of the two,
and the proof is the part that matters.

### 3.2 Why `0x8000`

The whole page must lie below the first mebibyte, and that mebibyte is not this
kernel's to allocate from: `kernel/mm/pmm.c` reserves it in its entirety, so no
frame within it is ever issued and a fixed page cannot collide with one that was.
Within that mebibyte, `0x8000` stands above the interrupt vector table at 0, the
BIOS data area at `0x400`, and the sector a legacy boot loader is read to at
`0x7C00`.

### 3.3 The page is proved usable, not assumed

What a fixed page *can* collide with is something the firmware left there, and
`SmpTrampolinePageIsUsable` refuses the entire bring-up rather than assume it
away. It requires the Multiboot2 memory map to declare the page available, and
then — because "available" is necessary and not sufficient, the kernel image and
the boot information structure both lying within regions the map calls available
— it requires the page to stand clear of both. These are the same two exclusions
`kernel/mm/pmm.c` applies, applied again here because this page does not come
from the allocator.

The failure this prevents is a machine that fails inside the firmware's code with
this kernel's name on the screen: a reserved page overwritten with a trampoline,
and processors started into whatever remained.

### 3.4 The identity mapping, and why its removal is a shootdown

The instruction after the one that enables paging executes at the address it
already had — which is this page. So the page must be mapped in the hierarchy
just loaded, at its own address, or the first processor to enable paging triple
faults with nothing reported.

This is the only identity mapping the kernel has. The one `boot/boot.asm`
established was removed the instant sub-task 2.3 wrote `CR3`, and `PagingReport`
announces its absence on every boot. It comes back for the duration of the
bring-up and no longer, and the hazard is stated rather than accepted quietly: a
stray low pointer dereferenced by the kernel while it stands reads and writes
real memory instead of faulting.

**Its removal is the first shootdown this kernel has performed that had anybody
to send it to.** `PagingUnmapKernelPage` invalidates through
`ShootdownBroadcast`, so every processor just started is interrupted, discards
its translation of the page, and acknowledges before the call returns.

The order is deliberate: the mapping goes after every processor has come online,
and therefore after every processor has left this page for the higher half. A
processor still executing here would lose the ground beneath it. A processor that
never answered has not been started at all and cannot be executing anywhere.

### 3.5 The parameter block, and the magic

`boot/trampoline.asm` lays the block out in assembly; `smp.h` lays a C structure
over it. Neither translation unit can see the other, and neither compiler checks
the other's arithmetic.

The magic is what closes that. The assembler writes `"XSMP"` as the block's first
field; the bootstrap processor reads it back *through the C structure* after the
image has been copied, and refuses to start anybody if it does not match. A field
inserted or resized on one side would otherwise send a processor to whatever
address the entry-point field had come to overlap — a jump into arbitrary memory,
upon a processor with no diagnostic channel of its own, which is as close to
unreportable as this kernel gets.

The structure is `__attribute__((packed))`, and `PROJECT_GUIDELINES.md`,
Section 8, requires the reason: the layout is not this compiler's to choose. Its
fields are nevertheless ordered and sized so that each is naturally aligned, so
the attribute changes no offset and exists solely to prevent one being
introduced. A static assertion fixes the total at 48 bytes.

| Offset | Field | Written by | Read by |
| ------ | ----- | ---------- | ------- |
| `+0x00` | `magic` | The assembler | The bootstrap processor, once, as proof |
| `+0x08` | `page_table` | The bootstrap processor | The trampoline, into `CR3` |
| `+0x10` | `entry_point` | The bootstrap processor | The trampoline, as the jump target |
| `+0x18` | `stack_top` | The bootstrap processor | The trampoline, into `RSP` |
| `+0x20` | `argument` | The bootstrap processor | The trampoline, into `RDI` |
| `+0x28` | `acknowledged` | The **starting** processor | The bootstrap processor, in a loop |

The block is `volatile` on the C side because `acknowledged` is written by
another processor and read here in a loop; a compiler entitled to assume no other
writer would hoist the read out and spin upon a register.

Its visibility needs no barrier. Intel SDM, Volume 3A, Section 8.2.2, provides
that stores are not reordered with other stores, so everything written before the
command register is written is visible to a processor that woke because of it.

### 3.6 The three modes

| Step | What it does | Why it is where it is |
| ---- | ------------ | --------------------- |
| Far jump to `0000:SmpTrampolineReal` | Normalises `CS` to zero | The processor arrives with `CS = VV00H`, so a label assembled at origin `8000H` would be reached at offset `8000H` from a base already `8000H`. After the jump every offset means what `org` says it means. |
| `LGDT`, then `CR0.PE` | Enters 32-bit protected mode | Section 9.9.1: the table must be in force before protection is enabled. |
| Far jump to the 32-bit code selector | Loads `CS` from the new table | And discards what was prefetched under the previous mode. |
| `CR4.PAE`, `CR3`, `IA32_EFER.LME`, `CR0.PG` | Enters long mode | The order of Section 4.1.2 and Table 4-14, exactly. |
| Far jump to the 64-bit code selector | 64-bit mode | The `L` flag of Figure 3-8 is what makes that descriptor 64-bit; `D` must be clear where `L` is set. |

Two details in that sequence are worth naming.

`CR0.WP` is set in the same store as `CR0.PG`. Write protection is **per
processor**, and without it a write by privilege level 0 to a page marked
read-only succeeds. A processor that skipped it could write the kernel's own text
while its fellows could not — a difference between processors that nothing in the
kernel would ever report, and which the self-test of Section 8 therefore checks
directly.

Only the low doubleword of `page_table` is loaded into `CR3`, because `CR3` is a
32-bit register until paging is enabled and the trampoline is in 32-bit mode when
it writes it. A hierarchy above four gibibytes could not be named there, and half
of its address would be. `SmpInitialise` refuses the whole bring-up in that case
rather than truncating — a refusal that has never fired, written so that it fires
rather than truncates if it ever can.

**No stack is established and none is used.** There is no call and no push in the
file before `RSP` is loaded from the parameter block in 64-bit mode. A stack in
low memory would be a second page to find, to map and to prove usable, for the
sake of a handful of instructions that need none.

The trampoline's own descriptor table is the kernel's in miniature — a 32-bit
code and data pair, and a 64-bit code descriptor — and is discarded the moment
the C entry point loads the real one. A build-time `%error` requires the whole
image to fit the single page: a failure to build, rather than a failure to boot.

## 4. What the bootstrap processor prepares, and the rule behind it

> **A starting processor allocates nothing, maps nothing and claims nothing.**

Every resource it will need — its stack, its double-fault stack, its task state
segment descriptor — is prepared by the bootstrap processor before the startup
interrupt is sent, and handed over through the parameter block or found by index.

The reason is the state of this kernel at this sub-task. The frame allocator, the
kernel arena and the heap are all unsynchronised, and
[`CONCURRENCY.md`](CONCURRENCY.md), Section 10, limitation 1, says so. A
processor that allocated its own stack would be the second processor in an
allocator that admits one, at the one moment when a corruption would be
indistinguishable from a processor that simply failed to start.

The stacks are held in a table keyed by index rather than passed through the
parameter block, because only one of them can be passed: the block hands over the
stack the entry point begins upon, and the entry point needs two further facts —
the same stack again, to give the task state segment, and the double-fault stack
— at a point where the block has already been overwritten for the next processor.
The index is the key, and the index is what the block carries.

### 4.1 One task state segment per processor

The segment holds `RSP0`, the stack a transition to privilege level 0 is made
upon, and the interrupt stack table. Both are properties of a processor rather
than of the machine, so the array in `kernel/arch/x86_64/cpu/tss.c` is now one segment per
processor and the global descriptor table holds one sixteen-byte descriptor for
each.

Sharing is not merely unwise; the architecture forbids it. Intel SDM, Volume 2A,
`LTR`, provides that the instruction marks the descriptor busy and refuses one
already so marked, so the second processor to execute it against a shared
descriptor takes a general-protection exception.

The array is statically sized to `PER_CPU_MAXIMUM` rather than grown to fit the
firmware's declaration, because a descriptor must be installed before its
processor runs and the address it names must not move afterwards — the processor
holds it in a structure it reads without asking. An array that grew would move
every segment already in use.

`GdtTaskStateSegmentSelector(index)` derives each selector, and
`GDT_TSS_SELECTOR` is that expression evaluated at zero, so the constant and the
function agree by construction rather than by inspection.

## 5. The C entry point

`SmpApplicationProcessorEntry` is entered in 64-bit mode with a stack and with
nothing else established. Its order is the order the dependencies impose, and
each step is something the trampoline could not have done.

| Step | Why it is at this point and not later |
| ---- | ------------------------------------- |
| `GdtLoadOnThisProcessor`, `IdtLoadOnThisProcessor` | The trampoline's table lives in the page about to be unmapped. A processor holding a selector into it would fault on the next segment load with the table gone. |
| `PerCpuInitialise` | Every lock in this kernel reaches through the area. **Nothing above this line may take one, and nothing above it does.** |
| The index check | The claimed index must equal the prepared one. It is a panic and not a report: there is no correct way to continue with two processors upon one stack. |
| `TssInitialiseProcessor` | The double-fault gate names an interrupt stack table entry, and a processor whose task register is null cannot supply one. A double fault there is a triple fault — a reset with nothing written anywhere. |
| `SyscallInitialise`, `SyscallSetKernelStack` | Four model-specific registers, all per processor. `IA32_KERNEL_GS_BASE` is among them, and the interrupt entry path exchanges `GS` against it on every interrupt from privilege level 3. |
| `LocalApicInitialiseThisProcessor` | Without it the processor accepts no interrupt at all — including the shootdown its fellows are about to send, whose absence would present as a machine that hangs inside `PagingUnmapKernelPage`. |
| Record, announce, park | Section 5.1, then `sti; hlt`. |

The table is not decoration. Each row is a step that a processor can skip and go
on running perfectly well until the single moment that step exists for, and the
`TssInitialiseProcessor` row is there because a negative test found exactly that:
a processor given no task state segment came online, answered a shootdown, and
passed every assertion that existed at the time.

`GdtLoadOnThisProcessor` loads the table the kernel already built rather than
composing it again. The table is one table, and composing it twice would be a
second chance to compose it differently; the register operand is per processor
and the table it names is not, which is the whole of the distinction. Like
`GdtInitialise` it re-establishes `GS.base` afterwards, the segment reload having
destroyed it — Intel SDM, Section 3.4.4, and the defect
[`CONCURRENCY.md`](CONCURRENCY.md), Section 3.4, records.

### 5.1 What the processor writes down about itself

Before it parks, each started processor records its task register, its descriptor
table bases and limits, and `CR0`, `CR3` and `CR4`, into `SmpRecords[index]`.

Every value is taken **with the instruction that reads that register** — `STR`,
`SGDT`, `SIDT`, and the control registers — and not from what this kernel
believes it loaded. The distinction is the whole point: what is being asserted
afterwards is that the load reached this processor.

The bootstrap processor makes no record. Everything in it can be read from it
directly, and a copy would be a second thing that could disagree.

### 5.2 The park

```
    for (;;) { __asm__ __volatile__("sti; hlt"); }
```

The two instructions are adjacent and must be. Intel SDM, Volume 2B, `STI`,
provides that the interrupt flag takes effect only after the instruction
*following* `STI`, so an interrupt arriving in between is held until the `HLT`
has been entered — rather than being delivered before it and leaving the
processor asleep with the reason it was woken already past. The same pairing, for
the same reason, is in the echo loop of [`../devices/KEYBOARD.md`](../devices/KEYBOARD.md),
Section 7.3.

### 5.3 The announcement is composed before it is written

`SmpAnnounceArrival` builds the whole line into a local buffer and emits it in
one `KernelWriteString` call, because the lock of Section 6 governs a call and
the thing that must not interleave is a line. Two processors arriving at once
would otherwise produce

```
    Processor Processor 1 is online…2 is online…
```

which is not merely untidy. The count of processors is read out of this log by a
person, and a log that has to be reassembled before it can be read is one whose
figures cannot be trusted.

## 6. The one lock this sub-task applies

A started processor touches exactly one thing outside its own area: the
diagnostic channel. So the diagnostic channel is what acquires a lock, and it
acquires it in `KernelWriteString` — one function, above four unsynchronised
structures:

- the text-mode display's cursor (`drivers/vga/vga.c`),
- the console's rows (`graphics/console.c`),
- the serial adapter's transmit buffer (`drivers/serial/serial.c`),
- the compositor's back buffer and damage rectangle (`graphics/compositor.c`).

One lock covers all four because one function reaches all four. Each of those
files' headers names this as where its lock is taken, so the claim can be checked
by `grep` rather than by memory — the discipline
[`CONCURRENCY.md`](CONCURRENCY.md), Section 10, limitation 1, adopted after the
corpus audit of sub-task 6.13.

### 6.1 The critical section is a call, not a line

Composing a line before writing it is therefore the caller's business. The one
caller that runs upon several processors at once — `SmpAnnounceArrival` — does
exactly that, for the reason Section 5.3 gives.

### 6.2 The lock is taken only once there is an area to take it through

```c
    const bool locked = PerCpuIsEstablished();
    if (locked) { SpinlockAcquire(&KernelDiagnosticLock); }
```

This condition is not a nicety, and the negative case is not hypothetical. A
spinlock acquire masks interrupts by way of the per-processor area, which it
reaches through `GS.base`. `KernelWriteString` prints the banner, and the banner
is printed before `PerCpuInitialise` has run. Taking the lock unconditionally
read address sixteen through a segment base of zero, before the interrupt
descriptor table existed — and the machine reset with an empty log, which is the
only symptom a fault *inside* the diagnostic channel can ever have.

There is nothing to protect on that side of the line in any case. Before the area
exists, no processor has been started and none can be, so there is one writer by
construction. The lock begins to mean something at exactly the moment the
mechanism it is built upon begins to work.

### 6.3 A panic resets the lock rather than waiting for it

`KernelPanic` calls `IpiHaltOtherProcessors`, and then reinitialises the
diagnostic lock before it writes a word.

Both of the cases that motivates have the same answer. A panic reaches that line
from anywhere, including from inside `KernelWriteString`'s own critical section —
a fault raised by the console or by the compositor does exactly that — and a
ticket lock reacquired by the processor already holding it does not deadlock
loudly; it spins until the bound of sub-task 6.13 fires and then panics about the
lock instead of about the fault. It could equally be held by one of the
processors just halted, which will never release it.

After the halt request, no processor but this one is going to write anything, so
there is nothing left for the lock to protect. The report is the last thing this
machine will do, and it must not be the thing that stops it being written.

## 7. Processors are started one at a time

`SmpInitialise` starts a processor, waits for its acknowledgement, waits for its
area to come online, and only then considers the next.

This is slower than a broadcast and it is what makes the rest of the design
sound. There is one parameter block at one fixed address; starting two processors
at once would mean two processors reading a block while it was being rewritten
for the second of them. It is also what lets the claimed index be checked against
the prepared index at all — the check in Section 5 holds *because* the starts are
serialised, and it is written to fail loudly if that ever ceases to be true.

**That check has never fired, and cannot fire here.** Removing `SmpWaitForOnline`
altogether was tried and changed nothing: QEMU declares two processors, so there
is exactly one application processor, so there is no second start for the first
to collide with. The panic is retained on the argument that a check which fires
loudly when the serialisation stops holding is worth more than one proven to fire
in an environment that cannot exercise it — but it is recorded as unproven, in
[`../project/TESTING-SYSTEM.md`](../project/TESTING-SYSTEM.md), Section 10.4,
rather than counted among the things this sub-task has demonstrated.

The INIT is addressed to a single destination for the same reason, and for one
more: the bootstrap processor must never receive one. Section 8.4.4.1's sequence
is addressed to a processor that has not started, and an INIT delivered to one
that has is a reset of the machine this kernel is running upon. `SmpInitialise`
skips the entry whose APIC identifier matches `LocalApicIdentifier()`.

### 7.1 What is declined, and why declining is a report

| Condition | Outcome |
| --------- | ------- |
| No MADT, or one usable processor declared | Nothing is attempted; `SmpDeclinedReason` says which. |
| The local controller is not enabled | Nothing can be sent, so nothing is. |
| The kernel hierarchy lies above four gibibytes | The trampoline cannot name it in 32-bit mode (Section 3.6). |
| The interval timer is not running | The protocol's delays cannot be measured (Section 2.3). |
| The firmware does not offer the low page | Section 3.3. |
| An APIC identifier above eight bits | Counted as refused. It cannot be named in the destination field in xAPIC mode, and comes from an x2APIC structure this kernel does not enable. It is declined rather than truncated, a truncated identifier naming a *different* processor rather than none. |
| More processors than `PER_CPU_MAXIMUM` | Counted as refused. |
| Started, but never answered | Counted as a failure; the machine carries on. |

Each of these is a sentence in the report rather than a silent zero, because
"this kernel started no processors" and "this machine has no processors to start"
look identical from outside and have entirely different causes.

## 8. Verification

`KernelVerifyApplicationProcessors`, in
[`../../kernel/test/arch/smp.c`](../../kernel/test/arch/smp.c).

**A count is not the assertion.** A kernel that incremented a variable and
started nobody would produce the same count, the same report and the same banner.
What only a running processor can produce is an acknowledgement to an interrupt
it was sent — so the substance of this test is a shootdown broadcast, with each
target's own `shootdowns_serviced` read out afterwards.

`ShootdownBroadcast` waits for every target to acknowledge before it returns, so
no wait is needed in the test: a target that did not answer would have made the
broadcast itself fail.

| Assertion | The failure it detects |
| --------- | ---------------------- |
| The trampoline's identity mapping is gone | The one lasting hazard this sub-task introduces. A bring-up that left it standing would leave every stray low pointer in the kernel silently working. |
| Every area within the online count exists, is marked online, holds its own index, and its `self` names itself | An area counted but never claimed, or one whose pointer names another's state. |
| No two areas carry the same APIC identifier | Two processors sharing a stack and a task state segment — and every count in the report still right. |
| Index 0 is the bootstrap processor, and no other claims to be | A started processor that claimed index 0 would take the bootstrap processor's stack. |
| Online = started + 1 | The accounting against the areas, each of which the other could contradict. |
| Each started processor's task register names **its own** segment | The negative test of Section 5. A processor without one runs until its first double fault, which is then a triple fault. |
| Its `GDT` base and limit, and its `IDT` base, equal the bootstrap processor's | A processor still on the trampoline's table, which is about to be unmapped. |
| Its `CR3` is the kernel root | A processor on a hierarchy of its own. |
| `CR0.PG` set | Long mode not actually entered. |
| `CR0.WP` matches the bootstrap processor's | **The one a machine would never report.** Without it that processor could write the kernel's text while its fellows could not, and nothing would fault, ever. |
| `CR4.PAE` set | Long mode cannot have been entered at all. |
| **Every started processor's `shootdowns_serviced` rose across a broadcast** | The assertion this test exists for. A processor halted with interrupts masked, or that never reached the kernel's gate, or whose local controller was never enabled, cannot produce it. |

**Upon a machine with one processor it asserts the other side of the same coin**:
that nobody was started, that `SmpDeclinedReason` says which condition produced
it, that no processor is reported started that is not online, and that the online
count is one. A test that reported nothing there would be a test that passed upon
a machine where the bring-up silently did nothing.

`KernelVerifyPerCpu` still asserts that one processor is online, and now does so
for a different reason: it runs *before* `SmpInitialise`, not because nothing can
start a processor. That ordering is load-bearing and is stated in the test, so
that moving the assertion after the bring-up is recognised as the change it is
rather than rediscovered as a failure on every multiprocessor machine.

## 9. Observed state

Under QEMU with `-machine q35 -cpu qemu64 -smp cores=2`, on 2026-09-10:

| Quantity | Value |
| -------- | ----- |
| Processors declared usable by the firmware | 2 |
| Application processors started | 1; none failed to answer, none declined |
| Processors online | 2 — index 0 (APIC 0, bootstrap), index 1 (APIC 1, application) |
| Trampoline | `0x8000`, startup vector `0x8`, 254 bytes; identity mapping **removed** |
| Interrupts sent through the command register | 4, none refused, none abandoned |
| Shootdown requests / serviced | 2 / 4, none abandoned; last address `0x8000` |
| Shootdowns serviced by processor 1 | 1, from its own handler |
| Locks acquired, processor 0 / processor 1 | 1127 / 1, none contended |
| Local controller errors | 0 |

The reports are emitted before `KernelVerifyApplicationProcessors` runs, so the
figures above do not include that test's own broadcast. The serviced count
exceeds the request count for the reason
[`CONCURRENCY.md`](CONCURRENCY.md), Section 9, gives: `KernelVerifyIpi` sends the
shootdown vector twice on its own account without publishing a request.

That processor 1 has acquired exactly one lock is the design of Section 6 visible
in the accounting: the one acquisition is its arrival announcement.

## 10. Limitations

1. **A started processor has nothing to run.** It answers inter-processor
   interrupts and halts. Sub-task 6.15 is the scheduler, and until it exists the
   second processor contributes exactly what Section 1 claims and no more.
2. **The locks of sub-task 6.13 remain unapplied, save the diagnostic channel.**
   [`CONCURRENCY.md`](CONCURRENCY.md), Section 10, limitation 1, enumerates what
   is outstanding, file by file. This is not unsafe today — a parked processor
   reaches none of those structures — and it becomes unsafe the instant 6.15
   gives it a reason to. Every one of those files names 6.15 in its header now
   that 6.14 has passed without reaching it.
3. **x2APIC is not entered, so an APIC identifier above eight bits cannot be
   started.** Such a processor is counted as refused and named in the report.
   Reaching it requires the x2APIC mode this kernel does not enable, in which the
   destination is a full 32-bit field rather than bits 31:24 of the command
   register's high half.
4. **The bring-up is serial**, for the reason Section 7 gives. A machine with
   many processors pays ten milliseconds and change for each. Starting them in
   parallel requires a parameter block per processor, or an allocation the
   starting processor makes for itself — which limitation 2 forbids for now.
5. **`PER_CPU_MAXIMUM` is 64**, matching `ACPI_PROCESSOR_MAXIMUM` and now also the
   task state segment reservation and the descriptor table's length. A machine
   declaring more has the excess counted as refused rather than silently ignored.
6. **The trampoline page is fixed at `0x8000`.** A firmware that reserved it stops
   the bring-up entirely rather than relocating around it. The refusal is
   reported, and Section 3.1 records why a relocatable trampoline was judged the
   more expensive of the two.
7. **Nothing takes a processor offline.** There is no shutdown path, no idle
   accounting and no response to a hot-removal event. `IpiHaltOtherProcessors` on
   the panic path is the only thing that ever stops one, and it stops it for good.
8. **An unresponsive processor presents as a hang, not a report.** A processor
   that answered the startup interrupt and then failed to take interrupts —
   parked with the flag clear, or with its local controller never enabled —
   leaves `SmpUnmapTrampolinePage`'s shootdown unacknowledged, and
   `ShootdownBroadcast` spins out `SHOOTDOWN_WAIT_LIMIT` before it panics. That
   is 100,000,000 iterations, which under QEMU outlasts the 25-second bound
   `make verify` allows: the panic is correct and arrives far too late to be the
   diagnostic. It was observed deliberately, and is recorded in
   [`../project/TESTING-SYSTEM.md`](../project/TESTING-SYSTEM.md), Section 10.4.
   A bound expressed in time rather than in spins would fix it, and wants the
   interval timer, which is what the bring-up is already using.
9. **The index check cannot fire in any environment tested.** Section 7 gives the
   argument for keeping it; what is recorded here is that removing the wait it
   depends upon changed nothing, there being one application processor upon a
   two-processor machine. It is an unproven guard, kept knowingly.
