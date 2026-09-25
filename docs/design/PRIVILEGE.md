<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Privilege Transitions and System Calls

**Phase**: sub-task 6.1 (the apparatus) and 6.7 (the entry path and validation) of
[`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../kernel/arch/x86_64/cpu/gdt.c`](../../kernel/arch/x86_64/cpu/gdt.c),
[`tss.c`](../../kernel/arch/x86_64/cpu/tss.c),
[`idt.c`](../../kernel/arch/x86_64/cpu/idt.c),
[`../../kernel/arch/x86_64/syscall/syscall.c`](../../kernel/arch/x86_64/syscall/syscall.c),
[`syscall_entry.asm`](../../kernel/arch/x86_64/syscall/syscall_entry.asm); the call
numbers and error values in
[`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h).
**Specifications**: Intel SDM, Volume 3A, Sections 3.4.2, 3.4.5, 4.6, 5.8.8,
6.14.4, 20.5.2, Table 2-1; Volume 2A, `LTR`, `CPUID`; Volume 2B, `SYSCALL`,
`SYSRET`, `SWAPGS`; AMD64 APM Volume 2, Section 6.1; System V AMD64 ABI, Sections
3.2.1, 3.2.2.

The structures the processor reads when crossing between privilege levels 3 and 0
(the descriptors, the task state segment, the `SYSCALL` registers), the system-call
entry path, and the validation of every address a program passes. Each structure is
read by the processor without asking, at a moment when nothing can be reported, so
each is asserted from its **consequence**, not its contents. What the calls do is
the subject of their own documents ([`PROCESS.md`](PROCESS.md),
[`LIBC.md`](LIBC.md), [`../storage/VFS.md`](../storage/VFS.md) and others).

## 1. The descriptors

| Selector | Slot | Descriptor | Value |
| -------- | ---- | ---------- | ----- |
| `0x00` | 0 | Null | `0` |
| `0x08` | 1 | Kernel code, 64-bit, DPL 0 | `0x00AF9A000000FFFF` |
| `0x10` | 2 | Kernel data, DPL 0 | `0x00CF92000000FFFF` |
| `0x18` | 3 | User code, 32-bit compatibility, DPL 3 | `0x00CFFA000000FFFF` |
| `0x20` | 4 | User data, DPL 3 | `0x00CFF2000000FFFF` |
| `0x28` | 5 | User code, 64-bit, DPL 3 | `0x00AFFA000000FFFF` |
| `0x30`+ | 6+ | One 16-byte TSS descriptor per processor | Built at run time |

**The order of slots 3–5 is the processor's arithmetic** (SDM 5.8.8), from the two
selectors in `IA32_STAR`:

```
SYSCALL:  CS = STAR[47:32]         SS = STAR[47:32] + 8
SYSRET:   CS = STAR[63:48] + 16    SS = STAR[63:48] + 8     (64-bit; RPL forced to 3)
```

So kernel data follows kernel code by 8; user data follows `STAR[63:48]` by 8; user
64-bit code follows it by 16. Slot 3 is what a 32-bit `SYSRET` would load, so it
holds a valid descriptor rather than nothing. Every descriptor can be perfect and
the transition still fail, since `SYSRET` adds 16 to a number; the ordering is
therefore asserted as an ordering.

**The TSS descriptor** is 16 bytes (a 64-bit base), assembled by
`GdtInstallTaskStateSegment` with access byte `0x89` (present, DPL 0, type 9
available). `LTR` changes 9 to 11 (busy), and only the processor does, which makes
it the one evidence that the processor read the descriptor. `LTR` takes a 16-bit
operand only (`0F 00 /3`); the selector is held in a `uint16_t` so the operand is
`%ax`, which Clang's assembler requires. `GdtTaskStateSegmentSelector(index)` gives
each processor's selector.

**Established after the IDT**: `LTR` raises `#GP` on a malformed descriptor, and
before any gate exists that becomes a triple fault. A structure the processor reads
must be established where its rejection can be reported.

## 2. The task state segment

In 64-bit mode the TSS holds only stack pointers. One per processor (`TssSegments`,
[`SMP.md`](SMP.md)): `rsp0` names that processor's current thread's stack, and
`LTR` refuses a busy descriptor, so sharing is impossible.

| Field | Read when | Holds |
| ----- | --------- | ----- |
| `rsp0` | Entering level 0 from level 3 through a gate. | The current thread's kernel stack, written by `ThreadSetCurrent` and on every switch ([`PROCESS.md`](PROCESS.md)). |
| `rsp1`, `rsp2` | Levels 1 and 2 (unused). | Zero. |
| `ist[0]` | A gate names IST entry 1. | A separate 16 KiB stack for the double fault. |
| `ist[1]`–`ist[6]` | — | Zero. |
| `io_map_base` | An I/O instruction above `IOPL`. | 104. |

- The structure is `packed` and asserted to be 104 bytes: the layout is the
  processor's, and padding would displace every field after it.
- **`io_map_base` is 104, past the limit of 103**, so there is no I/O bitmap and
  every port is denied to level 3 (SDM 20.5.2). A base inside the limit would make
  whatever bytes lie there the permission map, and a clear bit *grants* a port. The
  test asserts the relation (`base > limit`), not the number.
- **The double fault has an IST stack** (SDM 6.14.4: IST stacks are loaded
  unconditionally, `rsp0` only on a change from level 3). A fault at level 0 on a
  bad stack would otherwise push onto that stack, fail, and reset the machine.
- **The page fault has none**: an IST stack is a fixed address and does not nest,
  and the page-fault handler may fault again.

## 3. The `SYSCALL` registers

`SyscallInitialise`, after checking `CPUID` (leaf `0x80000000` first for the
highest extended leaf, then bit 11 of leaf `0x80000001` EDX; querying an absent leaf
returns another leaf's data), writes, in this order, so `SYSCALL` never becomes
legal while pointing at nothing:

```
IA32_STAR  = (0x08 << 32) | (0x18 << 48)   = 0x0018_0008_0000_0000
IA32_LSTAR = &SyscallEntry
IA32_FMASK = TF | IF | DF | IOPL | NT | AC = 0x0004_7700
IA32_EFER |= SCE
```

`SYSRET` then yields `CS 0x2B`, `SS 0x23` (RPL 3). `IA32_CSTAR` (compatibility-mode
entry) is not written: 32-bit programs are not supported.

| `FMASK` bit | Why it is cleared on entry |
| ----------- | -------------------------- |
| `IF` | **`SYSCALL` does not switch stacks**: an interrupt before the switch would run at level 0 on memory the program controls. |
| `TF` | The caller would single-step the kernel. |
| `DF` | The ABI requires it clear at a function's entry. |
| `NT` | It changes what `IRET` does. |
| `AC` | With `CR4.SMAP`, it controls supervisor access to user pages. |
| `IOPL` | The kernel runs at I/O privilege 0 whatever the caller's. |

The bits are named and combined in `syscall.h`, not written as `0x47700`, which
would keep the answer and lose the reasons.

## 4. The entry path

**The first three instructions** are the security of the path:

```
swapgs
mov     [gs:BLOCK_USER_STACK], rsp
mov     rsp, [gs:BLOCK_KERNEL_STACK]
```

1. **`SWAPGS`**: until then no kernel state is addressable; every register and the
   `GS` base are the caller's. `IA32_KERNEL_GS_BASE` cannot be written at level 3.
2. **Save the caller's `RSP`** in the per-processor area, the only place available.
3. **Load the kernel stack**; only now may anything be pushed. Pushing earlier
   writes where level 3 points.

The kernel stack is the one `rsp0` names, the same as for interrupts from level 3.
Both copies (`rsp0` and the area's) follow the current thread
(`SyscallSetKernelStack` in `ThreadSetCurrent`); with only `rsp0` updated, a child's
call and its suspended parent's built frames at the same addresses.

**The caller's `RSP` is pushed into the frame**, and restored on exit by
`POP RSP` from the frame, not from the per-processor area: while a parent sleeps in
`wait`, its child's own system call overwrites the area's copy.

| Register | Carries |
| -------- | ------- |
| `RAX` | Call number in; result out |
| `RDI`, `RSI`, `RDX` | Arguments 1–3 |
| `R10` | Argument 4 (`SYSCALL` puts the return address in `RCX`) |
| `R8`, `R9` | Arguments 5–6 |

Every register is saved and restored, since `SYSRET` restores none and a register
changed for no visible reason is the least debuggable fault. The frame's fields and
the assembly's pushes are one order stated twice, tied by `_Static_assert` on the
offsets. The return is `SYSRET`.

**No test hook.** `SYSRET` always returns to level 3, so the self-test cannot
execute `SYSCALL` from the kernel. A branch that returned differently for a trusted
caller would be indistinguishable from a privilege-escalation bug, and none exists.
The transition is exercised by the user programs of the self-test instead.

## 5. Validating arguments

Every range a caller names:

| Refused | Because |
| ------- | ------- |
| A zero length or a null address. | Meaningless. |
| A range that **wraps** the address space. | `address + length` overflows and looks small; the test is a subtraction from the limit. |
| Any byte at or above `SYSCALL_USER_LIMIT`. | The kernel's half. One comparison, made **before** the page walk, so a mapped kernel page marked user is still refused. |
| Any page unmapped, not user-accessible, or (for a write) not writable. | The kernel would read or write on the caller's behalf what the caller could never reach. |

- **Every page is walked**, not the first: a copy begun must be finishable.
- **Bytes are copied once** into a kernel buffer, in both directions. Reading
  caller memory twice lets another processor change it between check and use; and
  nothing below the system call validates an address, so a filesystem handed one
  would fault holding its locks.
- **User access is the conjunction of every level** (SDM 4.6): intermediate paging
  entries carry the user bit (`PagingObtainTable`), or a user leaf beneath them is
  unreachable. It grants nothing alone; a kernel leaf has no user bit.
- **A copy-on-write page is resolved, not refused**, when the kernel must write to
  it: after `fork`, a program's buffers are read-only in the page tables and still
  its own to write. The kernel cannot provoke the fault (`CR0.WP` makes it a
  level-0 fault), and refusing would fail the call for a reason the caller cannot
  see.
- **Paths** are copied by `SyscallCopyUserPath`, which reports "too long"
  (`ENAMETOOLONG`) apart from "bad address" (`EFAULT`), and joins a relative path to
  the process's working directory in that one place, so every call resolves paths
  the same way; the bound applies to the joined path.
- **Descriptors are the process's own**: every call translates the number through
  the process's table first, so no program reaches another's file by guessing.

## 6. The calls

Numbers never change once given to a program; new calls are appended. The numbers
and the error values are this kernel's own, defined once in
[`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h)
(`SYSCALL_COUNT` is 44). A number beyond the table returns `ENOSYS`; the number is
unsigned, so one comparison covers negatives.

| Numbers | Calls | Documented in |
| ------- | ----- | ------------- |
| 0–2 | `write`, `ticks`, `version` | This document; [`LIBC.md`](LIBC.md) |
| 3–6 | `fork`, `execve`, `exit`, `wait` | [`PROCESS.md`](PROCESS.md) |
| 7 | `brk` | [`LIBC.md`](LIBC.md) |
| 8–13 | `open`, `close`, `read`, `readdir`, `mkdir`, `unlink` | [`LIBC.md`](LIBC.md), [`../storage/VFS.md`](../storage/VFS.md) |
| 14–17 | `chdir`, `getcwd`, `dup2`, `rmdir` | [`SHELL.md`](SHELL.md) |
| 18–28 | `pipe`, `waitpid`, `kill`, `sigaction`, `sigreturn`, `getpid`, `getpgid`, `setpgid`, `tcgroup`, `link`, `procinfo` | [`SHELL.md`](SHELL.md), [`PROCESS.md`](PROCESS.md) |
| 29–34, 37, 38, 40, 41 | `window_create`, `window_destroy`, `window_move`, `window_blit`, `window_event`, `window_screen`, `window_session`, `window_text`, `window_state`, `window_list` | [`WINDOWS.md`](WINDOWS.md), [`SESSION.md`](SESSION.md) |
| 35, 36 | `power`, `pause` | [`INIT.md`](INIT.md) |
| 39 | `poll` | [`TERMINAL.md`](TERMINAL.md) |
| 42, 43 | `time`, `alarm` | [`../devices/TIME.md`](../devices/TIME.md) |

## Verification

`KernelVerifyPrivilege` in [`../../kernel/test/arch/privilege.c`](../../kernel/test/arch/privilege.c)
and `KernelVerifySyscall` in [`../../kernel/test/arch/syscall.c`](../../kernel/test/arch/syscall.c).
Nothing is asserted from what was written; a read-back of a written value proves
only that memory works.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| The GDT limit covers every slot; `GDTR` names this table. | Selectors past the old end rejected; the boot table still loaded. |
| Each descriptor, decoded field by field, has the right presence, type, `L`, `D` and DPL. | A user segment at DPL 0 (a program with kernel authority); `D` with `L`. |
| Kernel data = code + 8; user data = code32 + 8; user code64 = code32 + 16. | The ordering failure with no local symptom. |
| The TSS descriptor's base and limit match the segment; it is present, DPL 0, **type 11**. | Another memory's stack pointers; `LTR` never read it. |
| The task register holds this processor's selector. | `LTR` not executed. |
| `rsp0` is non-zero and 16-aligned; `ist[0]` is non-zero and differs from `rsp0`. | No entry stack; a double fault on the broken stack. |
| `io_map_base > limit`. | Stray bytes deciding port access. |
| `TssInterruptStack(0)` and `(8)` return zero; `IdtSetGateStack(8, 8)` is refused and changes nothing. | Neighbouring fields read; out-of-range entries truncated into others. |
| Vector 8's gate selects IST 1; vector 14's selects none. | Triple faults on bad stacks; a non-nesting page-fault stack. |
| **A probe vector raised without and then with IST 1 builds its frame outside and then inside the double-fault stack.** | The processor not reading the TSS: every configuration check passes and no stack switches. |
| `EFER.SCE` is set; `LSTAR` is the entry point. | `SYSCALL` undefined or jumping to nothing. |
| The selectors derived from `STAR` are kernel `0x08`/`0x10` and user `0x2B`/`0x23`. | Wrong segments on entry or return, found otherwise as a `#GP` at the first return. |
| `FMASK` clears `IF`, `DF`, `TF`, `NT`, `AC`. | An interruptible entry on the user's stack. |
| A number in the table is valid; one beyond it, and the largest, are not, returning `ENOSYS`. | A call through whatever follows the table. |
| Wrapping, straddling-the-limit and kernel-half ranges are refused. | Kernel memory reached through a bound check. |
| A mapped user page is **accepted**; a range crossing into an unmapped page is refused; a page's last byte is accepted and the next is not. | A validation that refuses everything; a copy that cannot finish; boundary errors. |
| A write from kernel memory, and `version` into kernel memory, are refused; an unknown descriptor is refused before its buffer is examined. | The kernel disclosing or corrupting itself. |
| The test's user page is gone afterwards. | A stray level-3 mapping for the rest of the boot. |

That the transition itself works is asserted by every user program the self-test
runs ([`PROCESS.md`](PROCESS.md)).

## Limitations

1. `IA32_CSTAR` is not written; no 32-bit programs.
2. The boot stack has no guard page (thread stacks do).
3. Only the double fault has an IST stack; NMI and machine check are the next
   candidates.
4. `CR4.SMEP`, `CR4.SMAP` and `IA32_EFER.NXE` are not set (sub-task 13.3, all three
   together): a supervisor access to a user page does not fault, and every page is
   executable.
5. `GS.base` is written, not exchanged, at context switches and at the descent to
   level 3 (two MSR writes each), because a switch cannot know how the kernel was
   entered ([`PROCESS.md`](PROCESS.md)).
6. The `SWAPGS` window against NMI ([`CONCURRENCY.md`](CONCURRENCY.md)).

## Appendix A. Reference: the calls

Every call by number, with the C library's wrapper from
[`../../libc/include/syscall.h`](../../libc/include/syscall.h). The numbers are
`SYSCALL_*` of [`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h),
which is authoritative where the two differ. Every wrapper returns the call's
result, or −1 with `errno` set (Appendix B). What each call does is in the
document Section 6 names for it.

| No. | Call | Wrapper |
| --: | ---- | ------- |
| 0 | `write` | `int64_t OxysWrite(int descriptor, const void *buffer, size_t length)` |
| 1 | `ticks` | `int64_t OxysTicks(void)` |
| 2 | `version` | `int64_t OxysVersion(char *buffer, size_t capacity)` |
| 3 | `fork` | `int64_t OxysFork(void)` |
| 4 | `execve` | `int64_t OxysExecve(const char *path, char *const argument_vector[], char *const environment_vector[])` |
| 5 | `exit` | `_Noreturn void OxysExit(int64_t status)` |
| 6 | `wait` | `int64_t OxysWait(int64_t *status)` |
| 7 | `brk` | `int64_t OxysBrk(void *address)` |
| 8 | `open` | `int64_t OxysOpen(const char *path, uint64_t flags, uint16_t permissions)` |
| 9 | `close` | `int64_t OxysClose(int descriptor)` |
| 10 | `read` | `int64_t OxysRead(int descriptor, void *buffer, size_t length)` |
| 11 | `readdir` | `int64_t OxysReadDirectory(int descriptor, SyscallDirectoryEntry *entry)` |
| 12 | `mkdir` | `int64_t OxysMakeDirectory(const char *path, uint16_t permissions)` |
| 13 | `unlink` | `int64_t OxysUnlink(const char *path)` |
| 14 | `chdir` | `int64_t OxysChangeDirectory(const char *path)` |
| 15 | `getcwd` | `int64_t OxysGetWorkingDirectory(char *buffer, size_t capacity)` |
| 16 | `dup2` | `int64_t OxysDuplicate(int from, int to)` |
| 17 | `rmdir` | `int64_t OxysRemoveDirectory(const char *path)` |
| 18 | `pipe` | `int64_t OxysPipe(int descriptors[2])` |
| 19 | `waitpid` | `int64_t OxysWaitFor(int64_t pid, int64_t *status, uint64_t options)` |
| 20 | `kill` | `int64_t OxysKill(int64_t pid, int signal)` |
| 21 | `sigaction` | `int64_t OxysSignalAction(int signal, uint64_t disposition, uint64_t restorer)` |
| 22 | `sigreturn` | None: entered by `OxysSignalRestorer`, which a handler returns into |
| 23 | `getpid` | `int64_t OxysGetProcessId(void)` |
| 24 | `getpgid` | `int64_t OxysGetProcessGroup(int64_t pid)` |
| 25 | `setpgid` | `int64_t OxysSetProcessGroup(int64_t pid, int64_t group)` |
| 26 | `tcgroup` | `int64_t OxysTerminalGroup(int64_t group)` |
| 27 | `link` | `int64_t OxysLink(const char *existing, const char *name)` |
| 28 | `procinfo` | `int64_t OxysProcessInformation(uint64_t index, SyscallProcessInformation *information)` |
| 29 | `window_create` | `int64_t OxysWindowCreate(const SyscallWindowRectangle *geometry, const char *title, uint64_t layer)` |
| 30 | `window_destroy` | `int64_t OxysWindowDestroy(int64_t window)` |
| 31 | `window_move` | `int64_t OxysWindowMove(int64_t window, int32_t x, int32_t y)` |
| 32 | `window_blit` | `int64_t OxysWindowBlit(int64_t window, const SyscallWindowRectangle *area, const uint32_t *pixels)` |
| 33 | `window_event` | `int64_t OxysWindowEvent(int64_t window, SyscallWindowEvent *event, uint64_t flags)` |
| 34 | `window_screen` | `int64_t OxysWindowScreen(SyscallWindowRectangle *geometry)` |
| 35 | `power` | `int64_t OxysPower(uint64_t action)` |
| 36 | `pause` | `int64_t OxysPause(void)` |
| 37 | `window_session` | `int64_t OxysWindowSession(void)` |
| 38 | `window_text` | `int64_t OxysWindowText(int64_t window, const SyscallWindowText *placement, const char *text)` |
| 39 | `poll` | `int64_t OxysPoll(SyscallPollEntry *entries, uint64_t count, uint64_t options)` |
| 40 | `window_state` | `int64_t OxysWindowState(int64_t window, uint64_t action)` |
| 41 | `window_list` | `int64_t OxysWindowList(SyscallWindowEntry *entries, uint64_t capacity)` |
| 42 | `time` | `int64_t OxysTime(void)` |
| 43 | `alarm` | `int64_t OxysAlarm(uint64_t milliseconds)` |

## Appendix B. Reference: the error codes

A call fails by returning the negative of a code; the C library's wrappers
return −1 and set `errno` to the positive value
([`../../libc/include/errno.h`](../../libc/include/errno.h)). The codes are this
kernel's own and are not Linux's numbers.

| Kernel result | `errno` | Name | Meaning |
| ------------: | ------: | ---- | ------- |
| −1 | 1 | `ENOSYS` | No such call. |
| −2 | 2 | `EFAULT` | An address the caller may not use. |
| −3 | 3 | `EINVAL` | An argument that cannot be right. |
| −4 | 4 | `EBADF` | No such descriptor. |
| −5 | 5 | `ECHILD` | The caller has no children to wait for. |
| −6 | 6 | `ENOENT` | No such file, or one that will not load. |
| −7 | 7 | `ENOMEM` | A frame, a table or a slot could not be had. |
| −8 | 8 | `EEXIST` | A file of that name already. |
| −9 | 9 | `ENOTDIR` | A component of the path is not a directory. |
| −10 | 10 | `EISDIR` | A directory where a file was required. |
| −11 | 11 | `ENOTEMPTY` | A directory holding more than `.` and `..`. |
| −12 | 12 | `EROFS` | The mount, or the volume, may not be written. |
| −13 | 13 | `ENAMETOOLONG` | A path or a component beyond the bounds. |
| −14 | 14 | `ELOOP` | Symbolic links followed beyond the depth bound. |
| −15 | 15 | `ENOSPC` | The volume has no room. |
| −16 | 16 | `EMFILE` | Every descriptor is in use. |
| −17 | 17 | `EBUSY` | Something held that the operation would destroy. |
| −18 | 18 | `EXDEV` | An operation confined to one volume was not. |
| −19 | 19 | `ENOTSUP` | The filesystem does not offer the operation. |
| −20 | 20 | `EIO` | The volume or the device beneath it failed. |
| −21 | 21 | `EPIPE` | The pipe is open for reading by nobody. |
| −22 | 22 | `EINTR` | A signal arrived while the call slept. |
| −23 | 23 | `ESRCH` | No such process or process group. |
| −24 | 24 | `EPERM` | The caller is not permitted this. |
| −25 | 25 | `ENOTTY` | The caller's standard input is not the terminal. |
| — | 32 | `EDOM` | Required by the C standard; nothing sets it yet. |
| — | 33 | `EILSEQ` | Required by the C standard; nothing sets it yet. |
| — | 34 | `ERANGE` | Required by the C standard; nothing sets it yet. |
