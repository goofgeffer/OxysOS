# The Executable

**Phase**: 6, sub-task 6.8, of [`../project/PLAN.md`](../project/PLAN.md).

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6. Every assertion of
format behaviour below carries a citation, and every specification named is
registered in [`REFERENCES.md`](../project/REFERENCES.md).

**Implementation**: [`../../kernel/exec/elf.c`](../../kernel/exec/elf.c),
[`../../kernel/include/oxys/elf.h`](../../kernel/include/oxys/elf.h).

## 1. What a loader is for, and what makes it dangerous

A loader turns a file into a program: it reads a description of where bytes
should go, puts them there, and says where to begin.

Every one of those steps takes its instructions **from the file**. The addresses
are the file's, the lengths are the file's, the offsets within itself are the
file's, and the entry point is the file's. A loader is therefore a piece of the
kernel that does what an untrusted document tells it to, and the whole of its
design is the list of things it refuses to be told.

That is the difference between this and the parsers of Phase 5. A malformed EXT2
volume produces a wrong answer; a malformed executable produces a *kernel that
writes where the file said*.

## 2. Nothing is overlaid

An ELF image is a format defined outside this project and read from a medium, so
every field is read byte by byte and assembled — `CODING-STANDARDS.md`, Section
7.1, and the same rule the EXT2 structures follow.

Three things follow from it and all three matter here:

- **The image need not be aligned.** A file read into the heap lands wherever the
  allocator put it, and a structure laid over it would be an unaligned access at
  best and a differently padded structure at worst.
- **The byte order is visible.** `ElfRead32` shifts bytes into place, so a reader
  can see that the format is little endian. A cast makes that invisible and makes
  it agree with the host by accident.
- **The compiler cannot pad the format.** A `uint16_t` at offset 16 followed by a
  `uint64_t` at 24 is what the format says; what a C structure would do with those
  fields is the compiler's business and need not match.

## 3. The order of the checks

The magic is tested **before any other field is read**, and that is not
politeness. Everything after it interprets bytes at fixed offsets, and
interpreting the offsets of a file that is not an ELF file is reading arbitrary
numbers out of arbitrary data — which is how a loader ends up mapping a segment
described by somebody's photograph.

The encoding is tested **before any multi-byte field is assembled**, for the same
reason one step further in. The readers are little endian by construction, so a
big-endian image decoded by them yields numbers that are wrong in a way nothing
else would notice: a byte count of sixteen million where the file said sixteen.

## 4. What is refused

| Refused | Because |
| ------- | ------- |
| No `7Fh 'E' 'L' 'F'` | Section 3 |
| A class that is not 64-bit, or an encoding that is not little endian | The fields would be read at the wrong widths, or assembled backwards |
| A machine that is not 62 | The instructions are for another architecture; the addresses would be fine and the program would fault at its first instruction |
| A type that is not `ET_EXEC` | A relocatable object has no segments to load. A **shared object** — which is what a position-independent executable is — has addresses that are offsets from wherever it is placed, and placing it means applying its relocations. This sub-task is the loader for *statically linked* executables, and refusing the others by name is better than loading one at addresses it did not mean |
| A segment of type `PT_INTERP` | The program is dynamically linked and expects something to load a library before it runs. Loading it regardless produces a program that reaches its first call into that library and faults |
| A header size or program header entry size that is not the format's | The walk **strides** by the entry size. A size of zero reads every header from the same offset; a larger one reads them from the wrong offsets entirely, and both produce numbers that look like a program |
| A program header table that lies beyond the end of the file | This is what stands between the loader and reading whatever follows the image in memory and calling it a segment |
| More program headers than `ELF_SEGMENT_MAXIMUM` | The validation is proportional to the count, so the count is what must be bounded first: an image claiming sixty thousand headers must be refused before they are walked |
| A segment whose contents lie beyond the end of the file | The same, per segment |
| A segment larger in the file than in memory | The reverse is ordinary — it is what a zero-filled section is — but this direction has the loader copy more bytes than it reserved pages for |
| Any byte of a segment at or above `SYSCALL_USER_LIMIT` | A program is loaded into the half a user may occupy. The test is against the limit and not against the last address, so a segment *beginning* below it and *ending* above is refused too — which is the case a check of the starting address alone admits |
| Segments not in ascending order of address | The specification requires it, and this loader depends upon it for the shared page of Section 5. A table out of order would have it reuse a page belonging to a segment it had not reached |
| An entry point outside every segment | A program whose first instruction fetch faults, at an address nothing in the image accounts for |

Every range test is written as a **subtraction**, never as a sum: `offset + size`
overflows for a size near the greatest value and the sum is then smaller than the
offset, so a range covering everything appears to lie within the file. This is
the same arithmetic, and the same reasoning, as the system-call validation of
[`PRIVILEGE.md`](PRIVILEGE.md), Section 9.3.

**The image is judged whole before a page of it is mapped.** Not interleaved: a
malformed segment discovered half way through would otherwise be a half-loaded
address space the caller must know to unpick. The cost is a second walk of at
most sixteen headers.

## 5. Loading

### 5.1 Through the direct map, not by switching

Each page is a frame the loader allocates, writes through the direct physical
map, and then gives to the address space with the permissions the segment asked
for.

Switching to the space in order to write into it is the obvious alternative and
is worse in two ways. The pages would have to be writable while being written, so
a read-only segment could not be given its final permissions until afterwards —
and a window in which the text of a program is writable is a window. And it would
put the kernel into an address space that is only half built.

### 5.2 The page two segments share

The text of an ordinary program ends part way through a page and its data begins
in the same one. **The loader must write into the frame already there rather than
mapping a fresh one over it**, or the end of the first segment is discarded — and
what is discarded is the last few hundred bytes of the program's code, which is
about as hard to diagnose as anything in this document.

This is a correctness requirement and not an optimisation, and it is why the
ascending order of Section 4 is enforced rather than assumed.

The shared page takes the **more permissive** of the two segments' permissions.
It holds bytes belonging to both, and a page the writable segment cannot write to
is a program whose first store to its own data faults.

### 5.3 Every page is zeroed first

Every page of a segment, not merely the part beyond the file's contents.

A frame arrives holding whatever its last owner left in it. The bytes of a
segment beyond its file size are exactly what a program is entitled to read as
zero — it is where its uninitialised data lives — so a loader that left the frame
as it found it hands a user program the kernel's leavings. That is a disclosure
of kernel memory with **no fault to report**: every byte involved is a byte
something meant to write, and nothing anywhere would notice.

## 6. Verification

The image is composed in memory, byte by byte. There is no compiler in this
kernel and no executable upon any volume it is required to carry — but that is
not why, and composing is not the weaker option. **A real executable can only
ever be well formed.** Every refusal in Section 4 is a case a real executable
would never produce, and composing is the only way to produce them.

The composing writes little-endian bytes with its own routines rather than
sharing the loader's. If the two shared a helper with the byte order wrong they
would compose and decode consistently and assert nothing at all.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| Each of the eleven refusals of Section 4, **by name** | A loader that refused everything would satisfy a test that only checked *that* it refused. Naming the result asserts that the kernel can tell "not for this machine" from "not a file", which is the difference between two quite different things to tell whoever is reading |
| A well-formed image decodes to the entry and the header count that were written | The header read at the wrong offsets, which every other assertion here would survive |
| Both segments arrive, at the addresses the image asked for | — |
| The first segment's contents are what the image held | Section 5.2: a fresh frame mapped over the shared page discards the end of it |
| The second segment's contents are what the image held | The same fault in the other direction |
| The memory beyond a segment's file contents reads as **zero** | Section 5.3: the kernel's leavings handed to a user program |
| The page the two segments share is writable | Section 5.2: a program whose first store to its own data faults |
| The loaded text is accessible to privilege level 3 | A program the processor will not let its own privilege level execute |

The contents are read at the addresses the image asked for, which needs the
composed space to be the active one. That assertion is what the whole test is
built toward: everything above it says the loader refused what it should, and
only this says it placed what it accepted.

### 6.1 The frames are dirtied first

The zeroing assertion could not fail as first written. Upon a freshly booted
machine the frame allocator hands out frames that happen to be zero already, so a
loader that never zeroed anything passed.

The test now allocates a handful of frames, fills them with a pattern, and gives
them back before the load. The allocator's next answers are then dirty — which is
the state they are in upon any machine that has been running for more than a
moment, and the state in which the disclosure of Section 5.3 is real.

### 6.2 The negative tests

Each was applied to `kernel/exec/elf.c`, confirmed, and reverted.

| The damage | What the run reported |
| ---------- | --------------------- |
| A fresh frame mapped over the page the two segments share. | `the first segment's contents are not what the image held` |
| The shared page left with the first segment's permissions. | `the page the two segments share is not writable` |
| The frame not zeroed before the contents are copied into it. | `the memory beyond a segment's file contents was not zeroed` — **and only after Section 6.1**. Before the frames were dirtied it passed. |

## 7. Limitations

1. **Statically linked executables only.** A dynamically linked program, a
   position-independent executable and a relocatable object are each refused by
   name. Relocation and an interpreter are a loader of their own.
2. **The image is read whole into the heap**, bounded at `ELF_FILE_MAXIMUM`. A
   loader reading it piecewise would be validating offsets against a length it
   had already used, and demand paging from a file is Phase 8's.
3. **`PT_LOAD` and nothing else.** A thread-local segment, a note, a stack
   description: read, reported, and not acted upon. Nothing yet has threads to
   give local storage to.
4. **No execute permission is enforced.** The segment's execute flag is recorded
   and not applied: it needs `IA32_EFER.NXE`, which arrives with SMEP and SMAP at
   sub-task 13.3. Until then every mapped page is executable, so a data segment
   is executable too.
5. **No arguments, no environment.** The loader places the image and reports
   where to begin. The stack is now the process's, `ProcessCreateUserStack`
   having arrived at sub-task 6.9; what a program is *told* when it starts —
   its arguments and its environment — is written onto that stack by whatever
   starts it, which is sub-task 6.11's `execve`.
6. **A partly loaded address space is not cleaned up.** Failure returns and the
   space is the caller's to destroy, because destroying it here would mean a
   loader that frees an address space it did not create.
7. **Nothing has been executed.** The loader places a program and sub-task 6.10
   is what transfers to one. Until then the entry point is a number this kernel
   has checked and never jumped to.
