# Interrupt and Exception Handling

**Corresponding phase**: Phase 3, sub-tasks 3.1 and 3.2.

**Specifications**: Intel 64 and IA-32 Architectures Software Developer's Manual,
Volume 3A, Chapter 6; Volume 3A, Sections 3.4 and 3.5; Volume 2A, `LGDT/LIDT`,
`SGDT/SIDT` and `IRET/IRETQ`.

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

## 3. The two irregularities the stubs normalise

The stubs exist because the processor presents its state inconsistently.

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

The structure and `kernel/cpu/interrupt_stubs.asm` are one interface expressed in
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

A minimal kernel table is therefore established in `kernel/cpu/gdt.c`, residing
in `.data` in the higher half. It reproduces the three descriptors of the boot
table, and `kernel/cpu/gdt.asm` reloads every segment register including `CS`,
which cannot be assigned by an ordinary instruction and is changed by a far
return.

The table is deliberately not `const`. Intel SDM, Volume 3A, Section 3.4.2,
provides that the processor sets the accessed bit of a descriptor when its
selector is loaded, and these descriptors have that bit clear. In read-only
memory the first segment load would itself fault.

Phase 6, sub-task 6.1, extends this table with the user-mode descriptors and the
task state segment.

### 5.1 The general lesson

A structure that the processor reads directly must remain mapped for as long as
the processor may read it, and the processor's reads are not visible in the
source. The interrupt descriptor table, the task state segment of Phase 6 and the
application processor trampoline of sub-task 6.8 are all of this kind.

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

| Vectors | Handler | Disposition |
| ------- | ------- | ----------- |
| 3 `#BP` | Breakpoint | Reports and resumes. A trap, so returning does not re-enter. |
| 4 `#OF` | Overflow | Reports and resumes. Likewise a trap. |
| 14 `#PF` | Page fault | Reports, decodes the error code and `CR2`, and halts. Sub-task 2.8 adds resolution. |
| All others 0–31 | Fatal | Reports and halts. |

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

## 9. Present limitations

1. No page fault is yet resolved. The handler reports and halts. Sub-tasks 2.7
   and 2.8 add copy-on-write resolution.
2. No interrupt stack table entry is used. A fault taken on a bad stack cannot
   presently be reported. This requires the task state segment of sub-task 6.1.
3. No hardware interrupt can yet arrive: the 8259A is not remapped until
   sub-task 3.5, and the interrupt flag remains clear throughout.
