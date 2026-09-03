# The Apparatus of a Privilege Transition

**Corresponding phase**: 6, sub-task 6.1, which opens the phase.
**Authority**: `PROJECT_GUIDELINES.md`, Sections 2 and 4.
**Implemented by**: [`../../kernel/cpu/gdt.c`](../../kernel/cpu/gdt.c),
[`../../kernel/cpu/tss.c`](../../kernel/cpu/tss.c),
[`../../kernel/cpu/syscall.c`](../../kernel/cpu/syscall.c),
[`../../kernel/cpu/syscall_entry.asm`](../../kernel/cpu/syscall_entry.asm),
[`../../kernel/cpu/idt.c`](../../kernel/cpu/idt.c),
[`../../kernel/cpu/exceptions.c`](../../kernel/cpu/exceptions.c),
[`../../kernel/include/oxys/gdt.h`](../../kernel/include/oxys/gdt.h),
[`../../kernel/include/oxys/tss.h`](../../kernel/include/oxys/tss.h),
[`../../kernel/include/oxys/syscall.h`](../../kernel/include/oxys/syscall.h),
[`../../kernel/include/oxys/msr.h`](../../kernel/include/oxys/msr.h).
**Asserted by**: `KernelVerifyPrivilege` in
[`../../kernel/kernel.c`](../../kernel/kernel.c).

**Specifications**: Intel 64 and IA-32 Architectures Software Developer's Manual,
Volume 3A, Sections 3.4.5, 5.8.8, 6.14.4, 8.2.3 and 8.7, and Table 2-1; Volume
2A, `LTR` and `CPUID`; Volume 2B, `SYSCALL` and `SYSRET`; Volume 1, Section
3.4.3; AMD64 Architecture Programmer's Manual, Volume 2, Section 6.1; System V
Application Binary Interface, AMD64 supplement, Sections 3.2.1 and 3.2.2.

---

## 1. What this sub-task is

Everything this kernel has done so far it has done at privilege level 0, in one
address space, upon one stack. A user program is none of those things, and the
processor will not carry it across the boundary between them upon request: the
transition is performed by hardware, out of structures the hardware reads
directly, and every one of those structures must already be correct before the
first transition is attempted, because the failure of any of them is a
general-protection exception or a reset and not a return code.

This sub-task builds those structures and proves them. It does not run a user
program — that is sub-task 6.5 — and it does not implement a single system call —
that is sub-task 6.2. What it establishes is that when those arrive, the machine
underneath them is already known to work:

1. **The descriptors** a transition loads: a 64-bit code segment and a writable
   data segment at privilege level 3, and a compatibility-mode code segment this
   kernel will never execute in but cannot omit.
2. **The task state segment**, which is where the processor finds a stack it can
   trust — one for an entry from privilege level 3, and one for a fault taken
   when the stack in use is the thing that is wrong.
3. **The three model-specific registers** that configure `SYSCALL`: `IA32_STAR`,
   which fixes the selectors; `IA32_LSTAR`, which fixes the entry point; and
   `IA32_FMASK`, which fixes what the kernel refuses to inherit from its caller.

The theme running through all three is the same. Each is a structure the
processor reads without asking, at a moment when nothing can be reported, so each
must be asserted from its consequence rather than from its contents. Section 7
is the record of how.

## 2. The descriptors, and why their order is not free

The global descriptor table of Phase 3 held a null descriptor and the kernel's
code and data. Sub-task 6.1 extends it to seven descriptors in eight slots:

| Selector | Slot | Descriptor | Value |
| -------- | ---- | ---------- | ----- |
| `0x00` | 0 | Null | `0x0000000000000000` |
| `0x08` | 1 | Kernel code, 64-bit, DPL 0 | `0x00AF9A000000FFFF` |
| `0x10` | 2 | Kernel data, writable, DPL 0 | `0x00CF92000000FFFF` |
| `0x18` | 3 | User code, 32-bit compatibility, DPL 3 | `0x00CFFA000000FFFF` |
| `0x20` | 4 | User data, writable, DPL 3 | `0x00CFF2000000FFFF` |
| `0x28` | 5 | User code, 64-bit, DPL 3 | `0x00AFFA000000FFFF` |
| `0x30` | 6–7 | Task state segment (sixteen bytes) | built at run time |

A reader coming to this table for the first time will find the order of slots 3
to 5 perverse: the segment this kernel actually intends to run user programs in
is the 64-bit one, and it stands *last*, behind a compatibility-mode segment that
will never be loaded. That order is not a choice. It is arithmetic performed by
the processor, and the table is laid out to satisfy it.

`IA32_STAR` holds two selectors, at bits [47:32] and [63:48], and from those two
the processor derives four (Intel SDM, Volume 3A, Section 5.8.8):

```
SYSCALL:  CS = IA32_STAR[47:32]
          SS = IA32_STAR[47:32] + 8

SYSRET    CS = IA32_STAR[63:48] + 16      (64-bit operand size)
(to 64):  SS = IA32_STAR[63:48] + 8

          both with RPL forced to 3
```

So the kernel's data descriptor must stand eight bytes after its code
descriptor; the user's data descriptor must stand eight bytes after the value in
`IA32_STAR[63:48]`; and the user's 64-bit code descriptor must stand sixteen
bytes after it. The compatibility-mode descriptor occupies the slot
`IA32_STAR[63:48]` itself names — the one `SYSRET` would load with a 32-bit
operand size — and this kernel puts a valid descriptor there rather than nothing,
because a `SYSRET` that reached it would otherwise load a null or absent
descriptor and fault.

The consequence is that **the descriptors are placed by the processor's
arithmetic and not by any preference of this kernel's**. That is the fact worth
carrying away, because it is the one with no local symptom: every descriptor in
the table may be individually perfect and the transition still fail, since what
`SYSRET` loads is decided by adding sixteen to a number and not by which
descriptor anybody meant. Section 7.1 asserts the ordering as an ordering, for
exactly that reason.

The table is deliberately not `const`. Intel SDM, Volume 3A, Section 3.4.2,
provides that the processor sets the accessed bit of a descriptor when its
selector is loaded, and the descriptors above have that bit clear; in read-only
memory the first segment load would fault. This was already true of the Phase 3
table and is documented in `INTERRUPTS.md`, Section 5.

### 2.1 The task state segment descriptor

Slot 6 does not hold a quadword written into the table by hand. A system
descriptor in 64-bit mode is *sixteen* bytes — the base address is 64 bits wide
and does not fit in the eight the older format allowed — and it names an address
that is not known until link time. `GdtInstallTaskStateSegment` therefore builds
it at run time from the base and limit it is given, scattering them into the five
fields the format divides them across, with an access byte of `0x89`: present,
DPL 0, system descriptor, type 9.

Type 9 means *available*. The processor changes it to 11, *busy*, when `LTR`
loads a selector for it. That change is made by the processor and by nothing
else, which is why Section 7.2 asserts it: it is the only evidence available that
the processor read the descriptor at all, rather than that this kernel wrote a
number into a register and drew a conclusion.

## 3. The task state segment

In 64-bit mode the structure named "task state segment" holds no task state.
Hardware task switching does not exist there. What remains is 104 bytes holding
a table of stack pointers, and the processor reads them at moments when it needs
a stack it can trust:

| Field | Read when | Held here |
| ----- | --------- | --------- |
| `rsp0` | A transfer to privilege level 0 occurs through an interrupt or trap gate from level 3. | 16 KiB in `.bss`. |
| `rsp1`, `rsp2` | Levels 1 and 2, which this kernel does not use. | Zero. |
| `ist[0]`–`ist[6]` | A gate names interrupt stack table entry 1 to 7. | Entry 1 holds a separate 16 KiB stack; the rest are zero. |
| `io_map_base` | An I/O instruction is attempted at a privilege level above `IOPL`. | 104. |

The structure is declared `__attribute__((packed))` and the declaration is
followed by a `_Static_assert` that its size is 104. The packed attribute is one
of the two extensions `CODING-STANDARDS.md`, Section 7, permits, and it is
required here for the ordinary reason: the layout is the processor's and not the
compiler's, and a compiler that inserted padding to align `rsp0` would displace
every field after it. The assertion is what makes the requirement enforced rather
than assumed.

### 3.1 `io_map_base`, and the number 104

The value 104 is `sizeof(TaskStateSegment)`, and the segment's limit is 103. The
map base therefore points one byte *past* the end of the segment, and that is
deliberate and is the whole of this kernel's I/O permission policy.

Intel SDM, Volume 3A, Section 20.5.2, provides that where the map base exceeds
the segment limit, there is no I/O permission bitmap and every port is denied to
any privilege level above `IOPL`. Were the base instead within the limit, the
processor would read the bytes it addressed as a bitmap of permissions — and
whatever happened to lie in memory at that offset would decide which ports a user
program could drive. A zero left there by accident is not a safe default: a clear
bit *grants* the port.

This is why Section 7.2 asserts `io_map_base > limit` as a relation rather than
asserting that the field holds 104. The number is a consequence; the relation is
the property.

### 3.2 The interrupt stack table

`rsp0` covers the entry from user mode. It does not cover the case the interrupt
stack table exists for.

Intel SDM, Volume 3A, Section 6.14.4, provides that where a gate names an
interrupt stack table entry, the processor loads that stack **unconditionally** —
whatever the privilege level it was at, whether or not the privilege level
changes. `rsp0` is loaded only on a change from level 3 to level 0. That
difference is the point: a fault taken *at privilege level 0*, upon a stack that
is the thing that has gone wrong, gets no new stack from `rsp0` and is pushed
onto the broken stack it was already using. The processor cannot push an
exception frame, and a processor that cannot push an exception frame does not
raise a third exception. It shuts down, and the machine resets.

The double fault is given entry 1 for that reason, `ExceptionInstallInterruptStacks`
attaching it to vector 8 after the table exists. It is the fault whose commonest
cause is that the stack was bad, and it is fatal by design, so the cost of a
dedicated 16 KiB stack buys the difference between a diagnosis and a reboot loop.

**The page fault is deliberately not given one**, and Section 7.3 asserts that it
has none. An interrupt stack table entry is a *fixed* address, not a stack that
nests: a handler that faults again while running upon one has its second frame
written over its first. The page fault is taken often — every copy-on-write
resolution is one — and its handler may itself fault, so it must run on a stack
that nests, which is the ordinary one.

### 3.3 What is not here yet

There is one segment, because there is one processor. From sub-task 6.8 each
processor requires a segment of its own, with its own stacks and its own
descriptor, because `rsp0` names the stack of whatever is running upon *that*
processor and a shared segment would deliver two system calls onto one stack. The
task register is per-processor already, so what must be duplicated is the storage
and not the mechanism.

`TssSetKernelStack` exists and is unused. From sub-task 6.4, when each thread has
a kernel stack of its own, the context switch must write `rsp0` at every switch;
the function is the seam that will be called there.

## 4. The three registers

`SyscallInitialise` writes three model-specific registers and then sets one bit
in a fourth. The order matters: the configuration is complete before the
instruction is made legal, so there is no window in which `SYSCALL` is valid and
points at address zero.

```
IA32_STAR  = (kernel code selector << 32) | (user 32-bit code selector << 48)
           = 0x0018_0008_0000_0000
IA32_LSTAR = &SyscallEntry
IA32_FMASK = TF | IF | DF | IOPL | NT | AC
           = 0x0004_7700
IA32_EFER |= SCE                        (bit 0)
```

`IA32_STAR[31:0]` is the 32-bit `SYSCALL` target and is meaningless in 64-bit
mode; it is left zero. The value `0x18` in bits [63:48] is the compatibility-mode
user code selector of Section 2 — the base from which `SYSRET` derives `0x28` for
code and `0x20` for stack, each with RPL 3, giving the `0x2B` and `0x23` the
report prints.

Support is established by `CPUID` before anything is written: leaf `0x80000000`
is queried first for the highest extended leaf the processor implements, because
querying leaf `0x80000001` on a processor that does not implement it returns the
contents of some other leaf entirely rather than an error, and only then is bit 11
of `EDX` read. Every processor capable of long mode reports the bit; a false
return means the machine is not one this kernel can run a user program upon at
all, so `KernelMain` proceeds anyway — so that the machine may still be examined —
and both the report and the self-test state the absence.

### 4.1 The mask, bit by bit

`IA32_FMASK` names the bits `SYSCALL` clears in `RFLAGS` upon entry. Each is in
the mask for a reason of its own, and they are not of equal weight:

| Bit | Why it is cleared |
| --- | ----------------- |
| `IF` | **The one whose omission is a hole rather than a nuisance.** `SYSCALL` performs no stack switch: `RSP` is still the caller's when the first instruction of the handler runs. An interrupt delivered there is delivered at privilege level 0 onto memory a user program controls. |
| `TF` | A caller that set the trap flag would single-step the kernel, taking a debug exception at every instruction of the handler. |
| `DF` | The System V AMD64 ABI, Section 3.2.1, requires the direction flag clear at a function's entry. A kernel that inherited it set would run its string operations backwards. |
| `NT` | The nested-task flag alters what `IRET` does. A kernel entered with it set and returning by `IRET` would attempt a task switch. |
| `AC` | Alignment checking is half of what makes a supervisor access to a user page fault when `CR4.SMAP` is set. A user that could disarm it in the kernel could defeat that protection. |
| `IOPL` | Both bits, so the handler runs at an I/O privilege level of zero whatever the caller's was. |

The bits are named individually in `syscall.h` and combined by `|` rather than a
constant `0x47700` being written down, because the constant would record the
answer and lose every one of the reasons above.

## 5. The entry point, and why it is a placeholder

`SyscallEntry` in `kernel/cpu/syscall_entry.asm` records the selectors and flags
the processor loaded, increments a counter, restores `RFLAGS` from `R11` and
jumps to `RCX`. Sub-task 6.2 replaces it entirely. Two of its properties are
deliberate and would be defects in the real path:

**It does not switch stacks.** `SYSCALL` leaves `RSP` exactly as the caller had
it — this is the difference between `SYSCALL` and an interrupt gate, and the
reason `IF` must be in the mask. A genuine entry from privilege level 3 would
arrive here executing kernel code upon a user stack, and the real path's first
act must be to leave it, by `SWAPGS` to reach the per-processor data and a load
of the kernel stack from it. Nothing enters here from privilege level 3, there
being no user program until sub-task 6.5, and the only caller is the self-test,
which executes `SYSCALL` from privilege level 0 where `RSP` is already a kernel
stack. The two pushes are safe for that caller and for no other.

**It does not return by `SYSRET`.** `SYSRET` returns to privilege level 3
unconditionally, forcing the RPL of the selectors it loads to 3 whatever
privilege it was reached from. Returning by it would drop the self-test into user
mode, with no user mapping to execute in and no user stack. Control is returned
instead by `push r11; popfq; jmp rcx`, which arrives back in the caller at
privilege level 0.

## 6. Where this stands in the boot sequence

The apparatus is established in `KernelMain` after `ExceptionInitialise` and not
beside the global descriptor table it extends. The reason is `LTR`: it reads the
descriptor `GdtInstallTaskStateSegment` built and raises a general-protection
exception where it is malformed. Performed before the interrupt descriptor table
existed, that exception would have found no gate and escalated to a triple fault,
and the diagnosis available to whoever met it would have been a machine that
reboots. Performed after, it is reported.

This is the same lesson `INTERRUPTS.md`, Section 5.1, draws from the boot table
that was unmapped: **a structure the processor reads directly must be
established at a point where its rejection can be reported**, because the
processor's reads are not visible in the source and its objections are not
return codes.

The self-test runs later still, with the other self-tests, because it raises an
interrupt and executes `SYSCALL` and both require the dispatcher.

## 7. Verification

`KernelVerifyPrivilege` comprises five parts and forty-eight assertions. The
governing principle is that **nothing here is asserted from what was written
into it**; every assertion is made against a consequence the processor produced,
because a test that reads back the value it wrote proves that memory works.

### 7.1 The descriptors

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| The table's limit is `(8 × 8) − 1` | The limit was not widened with the table; the processor would reject every selector past the old end. |
| `GDTR`'s base is this table's address | The register names some other table — the boot table, or nothing. |
| Kernel code is present, code, long-mode, DPL 0 | The Phase 3 descriptors moved when the table was extended; `IA32_STAR` names them by position. |
| User 64-bit code is present, code, long-mode, `D` clear, DPL 3 | A user descriptor left at DPL 0 would be loaded without complaint and would leave a program running with the kernel's authority. `D` set with `L` set is rejected by the architecture. |
| User data is present, writable, data, DPL 3 | A read-only or DPL 0 stack segment; the first push in user mode faults. |
| User 32-bit code is present, code, *not* long-mode, `D` set, DPL 3 | The slot `SYSRET` names with a 32-bit operand size is empty or malformed. |
| `kernel data == kernel code + 8`, `user data == user code32 + 8`, `user code64 == user code32 + 16` | **The assertion with no local symptom.** Every descriptor above may be perfect and the transition still fail, because what `SYSRET` loads is decided by arithmetic on a selector, not by which descriptor was intended. |

Each descriptor is decoded field by field rather than compared against a whole
quadword constant. A constant would agree with a mistaken descriptor as readily
as with a correct one: what is asserted is that the descriptor *says* "privilege
level 3" and "64-bit code", not that it holds a particular number somebody
transcribed twice.

### 7.2 The task state segment

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| The descriptor's base is the segment's address | The descriptor names some other memory; the processor would load a stack pointer from whatever lies there. |
| The descriptor's limit equals the segment's | Too small and the processor rejects `LTR`; too large and it would read past the segment for an I/O bitmap. |
| The descriptor is present, DPL 0 | An absent descriptor makes `LTR` fault. |
| **The descriptor's type is 11, not 9** | The processor never read the descriptor. Type 9 is written by this kernel; only the processor changes it to 11, and only on a successful `LTR`. This is the single assertion that distinguishes "the processor accepted this" from "this kernel wrote a number down". |
| The task register holds `0x30` | `LTR` did not execute, or executed with another selector. |
| `rsp0` is non-zero and 16-byte aligned | No stack for an entry from user mode, or one violating the ABI at a function's entry. |
| `ist[0]` is non-zero | No stack for the double fault. |
| `ist[0] != rsp0` | The double fault would be delivered upon the stack already in use — which is the case it exists to survive. |
| `io_map_base > limit` | Stray bytes past the segment would be read as a permission bitmap and would decide which ports a user program may drive. See Section 3.1. |
| `TssInterruptStack(0)` and `(8)` return zero | The table is numbered from one; an accessor that accepted 0 or 8 would silently read a neighbouring field. |

### 7.3 The interrupt stack table

The first four assertions inspect the configuration:

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| Vector 8's gate selects entry 1 | A fault upon a bad stack would triple fault silently. |
| Vector 14's gate selects none | The page fault was given a fixed stack it cannot nest upon. See Section 3.2. |
| `IdtSetGateStack(8, 8)` is refused | An entry above the seven the architecture provides was accepted and truncated into one belonging to something else. |
| The refused call altered nothing | A refusal that had already written is worse than one that had not. |

The remainder **exercises the mechanism rather than inspecting it**, which is the
part that matters. Everything above establishes that the gate names an entry and
the entry names a stack; none of it establishes that the processor *uses* them. A
task state segment whose descriptor the processor rejected, or a task register
never loaded, would satisfy every assertion above and switch no stack at all.

The double fault cannot be raised to find out: its handler is fatal by design,
and a self-test that halted the machine to prove a point would be of no use. So
vector 200 — clear of everything the machine uses — is registered, raised by
`int $200`, given entry 1, and raised again. The evidence is the address at which
the trap frame was built, because the frame is the first thing placed upon
whatever stack the processor selected; nothing else in the machine observes that
selection, the stack pointer the frame *records* being the one in use before the
exception and identical under either arrangement.

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| Both raises were delivered | The probe proves nothing about anything. |
| Without the entry, the frame is *outside* the double-fault stack | The measurement has no baseline: a machine where every exception happened to use that stack would pass the next assertion for the wrong reason. |
| With the entry, the frame is *inside* the double-fault stack | The processor is not reading the task state segment. |
| The two addresses differ | The interrupt stack table field made no difference, so whatever caused the frame to land there, it was not this. |
| The probe vector is left with no entry | A later test would meet a table this one had altered. |

Raising it twice is the design. One measurement alone would be satisfied by a
frame that happened to fall in range; the *pair* establishes that the interrupt
stack table entry is what caused the difference.

### 7.4 The system-call configuration

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| `IA32_EFER.SCE` is set, read back from the register | `SYSCALL` is an invalid opcode; the first system call is an exception. |
| `IA32_LSTAR` is non-zero and equals the entry point | The register holds address zero or some other address; the first system call transfers into nothing. |
| The derived kernel `CS` is `0x08` and `SS` is `0x10` | `SYSCALL` would enter the kernel with selectors that are not the kernel's. |
| The derived user `CS` is `0x2B` and `SS` is `0x23` | `SYSRET` would return to selectors that are not the user's DPL 3 segments. |
| `IA32_FMASK` clears `IF`, `DF`, `TF`, `NT` and `AC` | Each as Section 4.1 sets out; `IF` is the hole and the rest are the nuisances. |

The four selectors are **derived** by the same arithmetic the processor performs
and compared against what this kernel intends, rather than `IA32_STAR` being
compared against the value written into it. The two are different assertions: a
table whose user descriptors stood in the wrong order would satisfy an assertion
upon `IA32_STAR` and fail this one, and that failure would otherwise have first
appeared as a general-protection exception at the first return to user mode in
sub-task 6.5, a long way from its cause.

### 7.5 The transition itself

This is the only part of the sub-task that can be made to *happen* rather than
merely inspected, and it turns on an observation: `SYSCALL` may be executed from
privilege level 0. It raises no privilege, there being none to raise, but it
performs every other part of the transition — it loads `CS` and `SS` from
`IA32_STAR`, transfers to `IA32_LSTAR`, saves the return address in `RCX` and the
flags in `R11`, and clears the bits `IA32_FMASK` names. So the whole mechanism
can be exercised now, with no user program, no user mapping and no user stack.

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| The entry counter rose | `SYSCALL` did not reach the entry point `IA32_LSTAR` names. |
| The entry point observed `CS == 0x08` | The processor loaded a code selector that is not the kernel's. |
| The entry point observed `SS == 0x10` | The processor loaded a stack selector that is not the kernel's. |
| Interrupts are still disabled on return | The flags were not restored from `R11`. |
| *With `IF` set beforehand*: the entry point observed `IF` **clear** | `IA32_FMASK` is not applied. The kernel would be entered interruptible upon the caller's stack. |
| *With `IF` set beforehand*: `IF` is set again on return | The flags saved in `R11` were lost. |

It is performed **twice**, once with the interrupt flag clear and once with it
set. The second pass is the one that means anything: an assertion that the flag
was cleared upon entry says nothing whatever if the flag was already clear. The
test restores the flag to the state it found it in.

### 7.6 The negative test

The self-test was confirmed capable of failing before it was trusted.
`RFLAGS_INTERRUPT_ENABLE` was removed from `SYSCALL_FLAG_MASK` and the kernel
rebuilt. Two assertions reported — the configuration one in Section 7.4 and the
exercised one in Section 7.5 — and the run ended `Privilege self-test FAILED.`
The edit was then reverted. `docs/project/TESTING.md`, Section 13.1, records the
procedure.

### 7.7 Corroboration upon a second hypervisor

Everything above is asserted by the kernel about itself, and a kernel that
misread the architecture consistently would assert it consistently too. The
sub-task is therefore run under VirtualBox as well as QEMU — a different
firmware, a different descriptor table handed over, a different memory map — and
the report reads value for value as it does below, with `Privilege self-test
passed.`

That run established one thing besides: this kernel does not detect VirtualBox's
16550A, so there is no serial channel there and the automated assertion of
`docs/project/TESTING.md`, Section 1, cannot be made under it. A verdict must be
read from the console instead, by pausing the machine while the line is still
upon the screen. The condition predates this sub-task and is unrelated to it;
the procedure is `docs/project/TESTING.md`, Sections 9.1 and 9.2.

## 8. Observed state

```
Global descriptor table: base 0xFFFFFFFF8013A000, limit 0x3F, 7 descriptors in 8 slots.
  Kernel code 0x8, kernel data 0x10; user code 0x2B, user data 0x23; task state segment 0x30.
Task state segment: at 0xFFFFFFFF80186960, limit 103, task register 0x30.
  RSP0 0xFFFFFFFF80186960 (16 KiB), IST1 0xFFFFFFFF80182960 (double fault, 16 KiB).
  I/O permission map base 104, beyond the limit: no port is permitted to user mode.
System call: SYSCALL enabled, entry at 0xFFFFFFFF80129500.
  IA32_STAR 0x18000800000000 derives CS 0x8 and SS 0x10 upon entry, CS 0x2B and SS 0x23 upon return.
  IA32_FMASK 0x47700; the interrupt flag is cleared upon entry.
  Entries so far 0.
Privilege self-test passed.
```

`RSP0` and the address of the segment coincide because the segment is placed
immediately above the kernel stack in `.bss` and a stack pointer names one past
its last byte. The report prints selectors with RPL 3 applied — `0x2B` and `0x23`
— because those are the values a program will hold, not the `0x28` and `0x20` in
the table.

## 9. Present limitations

1. **The entry point is a placeholder.** It records and returns; it dispatches
   nothing, validates nothing and switches no stack. Sub-task 6.2 replaces it.
   See Section 5.
2. **`IA32_KERNEL_GS_BASE` is not written.** The real entry path needs `SWAPGS`
   to reach the per-processor data it will load the kernel stack from. The
   register is defined in `msr.h` and used by nothing; sub-task 6.2 writes it.
3. **`IA32_CSTAR` is not written.** It is the entry point for `SYSCALL` from
   compatibility mode, which this kernel does not support. A 32-bit program is
   not something this kernel can run at all, so an unwritten `CSTAR` is not a
   gap that a supported case falls into.
4. **The kernel stack has no guard page.** An overflow runs into the `.bss`
   below it, which happens to be the double-fault stack, so the overflow is
   caught by the double fault and reported. That is an accident of placement and
   not a design. Sub-task 6.4, where each thread has a stack of its own and the
   stacks become numerous enough that an overflow becomes likely, must take them
   from the kernel arena of sub-task 2.5 with a guard page beneath each.
5. **One task state segment, one processor.** See Section 3.3.
6. **`rsp0` is written once and never updated.** It is correct while there is one
   kernel stack. From sub-task 6.4 the context switch must write it at every
   switch; `TssSetKernelStack` exists for that and is presently uncalled.
7. **Six interrupt stack table entries are unused.** Only the double fault has
   one. The non-maskable interrupt and the machine-check exception are the
   conventional next candidates, both being deliverable at moments when the
   current stack cannot be trusted; neither is handled meaningfully yet.
8. **`CR4.SMAP` and `CR4.SMEP` are not set.** The `AC` bit is cleared in
   `IA32_FMASK` in anticipation of `SMAP`, but the feature itself is not enabled,
   so a supervisor access to a user page does not presently fault. Enabling them
   belongs with sub-task 6.5, where the first user mapping exists to be protected
   from.
