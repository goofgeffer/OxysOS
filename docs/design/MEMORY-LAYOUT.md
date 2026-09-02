# Oxys-OS Memory Layout

**Corresponding phases**: Phase 1, sub-tasks 1.2 and 1.4, which establish the
layout and the boot-time hierarchy; and the whole of Phase 2, which realises it.

**Specifications**: Intel 64 and IA-32 Architectures Software Developer's Manual,
Volume 3A, Sections 3.3.7.1, 4.1.2, 4.5, 4.6, 4.10.4 and 6.15, and Tables 4-15
and 4-19; Volume 1, Section 3.3.7.1.

## 1. The canonical address space

The x86_64 processors implemented to date translate 48 significant bits of a
linear address. Intel SDM, Volume 3A, Section 3.3.7.1, requires that bits 63 to
47 of a linear address be identical; an address satisfying this constraint is
termed canonical. The address space is therefore divided into two usable halves
separated by a non-canonical void.

| Range | Extent | Assignment |
| ----- | ------ | ---------- |
| `0x0000000000000000` – `0x00007FFFFFFFFFFF` | 128 TiB | The lower half. Reserved for user address spaces, from Phase 6. |
| `0x0000800000000000` – `0xFFFF7FFFFFFFFFFF` | — | Non-canonical. Any reference faults. |
| `0xFFFF800000000000` – `0xFFFFFFFFFFFFFFFF` | 128 TiB | The upper half. Reserved for the kernel, and mapped identically into every address space. |

## 2. The planned upper-half assignment

The following assignment is planned. Only the kernel image region is realised at
the completion of Phase 1; the remainder is recorded here so that the regions do
not conflict when they are introduced.

| Base | Extent | Region | Introduced |
| ---- | ------ | ------ | ---------- |
| `0xFFFF800000000000` | 64 TiB | The direct map of all physical memory. | Phase 2, sub-task 2.4 (established) |
| `0xFFFFC00000000000` | 32 TiB | The kernel virtual allocator arena, comprising the kernel heap and device mappings. | Phase 2, sub-task 2.5 (established) |
| `0xFFFFFFFF80000000` | 2 GiB | The kernel image: text, read-only data, data and BSS. | Phase 1 |

The kernel image is placed within the topmost 2 GiB so that every kernel symbol
may be reached by a 32-bit sign-extended displacement, which is the requirement
of the GCC `kernel` code model. That model is selected in the `Makefile` by
`-mcmodel=kernel` and materially reduces both code size and instruction count
relative to the `large` model.

## 3. The boot-time paging hierarchy

The hierarchy constructed by `BootBuildPageTables` in `boot/boot.asm` comprises
four 4096-byte structures, each aligned as Intel SDM, Volume 3A, Section 4.5
requires.

```
BootPml4                          (page-map level 4)
  entry[0]   ---> BootPdptIdentity
  entry[511] ---> BootPdptHigher

BootPdptIdentity                  (page-directory-pointer table)
  entry[0]   ---> BootPageDirectory

BootPdptHigher                    (page-directory-pointer table)
  entry[510] ---> BootPageDirectory

BootPageDirectory                 (page directory)
  entry[0..511] ---> 2 MiB pages covering physical [0, 1 GiB)
```

Both page-directory-pointer tables refer to the same page directory, so a single
set of 512 large-page entries serves both mappings. The consequent mappings are:

| Linear range | Physical range | Purpose |
| ------------ | -------------- | ------- |
| `0x0000000000000000` – `0x000000003FFFFFFF` | `0x0` – `0x3FFFFFFF` | The identity mapping, required at the instant paging is enabled. Removed in Phase 2, sub-task 2.3. |
| `0xFFFFFFFF80000000` – `0xFFFFFFFFBFFFFFFF` | `0x0` – `0x3FFFFFFF` | The higher-half mapping, at which the kernel is linked. |

### 3.1 Derivation of the indices

The higher-half base `0xFFFFFFFF80000000` decomposes as follows, in accordance
with Intel SDM, Volume 3A, Figure 4-8.

| Field | Bits | Value |
| ----- | ---- | ----- |
| Page-map level 4 index | 47:39 | 511 |
| Page-directory-pointer index | 38:30 | 510 |
| Page-directory index | 29:21 | 0 |
| Offset within a 2 MiB page | 20:0 | 0 |

### 3.2 Entry flags

The flags employed are those of Intel SDM, Volume 3A, Table 4-15.

| Flag | Bit | Applied to | Purpose |
| ---- | --- | ---------- | ------- |
| `P` (present) | 0 | All entries | The referenced structure or page is present. |
| `R/W` (writable) | 1 | All entries | Writes are permitted. |
| `PS` (page size) | 7 | Page-directory entries only | The entry maps a 2 MiB page rather than referring to a page table. |

The `U/S` flag remains clear throughout, so every mapping is accessible only at
privilege levels 0, 1 and 2. The `NX` flag is not yet employed; it is introduced
in Phase 13, sub-task 13.3, together with `SMEP` and `SMAP`.

## 4. The physical layout of the kernel image

`linker.ld` places the image at physical `0x00100000`, one mebibyte, which is the
conventional lowest address free of legacy device and firmware reservations. The
sections are laid out as follows.

| Section | Virtual address | Load address | Type | Contents |
| ------- | --------------- | ------------ | ---- | -------- |
| `.boot` | `0x00100000` | `0x00100000` | PROGBITS | The Multiboot2 header, the 32-bit entry code and the 64-bit trampoline. |
| `.boot.data` | `0x00101000` | `0x00101000` | PROGBITS | The boot GDT, the preserved boot loader values, the boot stack and the four boot-time paging structures. |
| `.text` | `KernelVirtualBase + p` | `p` | PROGBITS | The 64-bit kernel code. |
| `.rodata` | `KernelVirtualBase + p` | `p` | PROGBITS | Read-only data and string literals. |
| `.data` | `KernelVirtualBase + p` | `p` | PROGBITS | Initialised writable data. |
| `.bss` | `KernelVirtualBase + p` | `p` | NOBITS | Uninitialised data, including the 64 KiB kernel stack. |

Here `p` denotes the load address that the linker assigns by continuing
contiguously from the preceding section, and `KernelVirtualBase` is
`0xFFFFFFFF80000000`.

The `.boot` and `.boot.data` sections are linked at their physical addresses
because they are used before paging is enabled. They are two sections and not
one so that each may occupy a program header of its own: the entry code is
executed and never written, the boot data is written and never executed, and a
single section holding both obliged the linker to describe the pair as readable,
writable and executable together. See [`BOOT.md`](BOOT.md), Section 8. Every subsequent section is linked at its higher-half virtual
address, with an explicit `AT()` clause fixing its load address, so that GRUB
places the image correctly while the code executes from the upper half.

The `.bss` section is of type `NOBITS` and occupies no space in the file. It is
placed last so that no `PROGBITS` section follows it, which would otherwise
oblige the linker to emit a further program header. The boot loader zeroes the
difference between the memory size and the file size of the containing program
header.

The boot-time paging structures are emitted as initialised zero data within
`.boot.data` rather than being reserved in `.bss`, so that they occupy a defined
physical location within the loaded image and require no action by the boot
loader before `_start` executes.

## 5. Address translation helpers

`kernel/include/oxys/kernel.h` provides `PhysicalToVirtual` and
`VirtualToPhysical`, which add and subtract `KERNEL_VIRTUAL_BASE` respectively.
While the boot-time hierarchy is in effect these are valid only for physical
addresses below one gibibyte. Phase 2, sub-task 2.4, introduces the direct
physical map and extends their domain to the whole of physical memory.


## 6. The physical memory map and the extents that must be reserved

From Phase 2, sub-task 2.1, the kernel parses the Multiboot2 memory map into the
boot-protocol-neutral `BootInformation` structure declared in
`kernel/include/oxys/bootinfo.h`. The map observed under QEMU with 512 MiB of
memory is representative:

| Range | Extent | Classification |
| ----- | ------ | -------------- |
| `0x00000000` – `0x0009FC00` | 639 KiB | usable |
| `0x0009FC00` – `0x000A0000` | 1 KiB | reserved |
| `0x000F0000` – `0x00100000` | 64 KiB | reserved |
| `0x00100000` – `0x1FFDF000` | 523132 KiB | usable |
| `0x1FFDF000` – `0x20000000` | 132 KiB | reserved |
| `0xB0000000` – `0xC0000000` | 262144 KiB | reserved |
| `0xFED1C000` – `0xFED20000` | 16 KiB | reserved |
| `0xFFFC0000` – `0x100000000` | 256 KiB | reserved |
| `0xFD00000000` – `0x10000000000` | 12582912 KiB | reserved |

### 6.1 Why the map alone is insufficient

Multiboot2 Specification, Section 3.6.8, states that the map "includes the
regions occupied by kernel, mbi, segments and modules", and that the kernel must
take care not to overwrite them. The map is a description of the machine, not of
what is free. Three extents therefore fall within a region the map calls usable
and must be reserved separately by the frame allocator of sub-task 2.2:

| Extent | Source | Observed range |
| ------ | ------ | -------------- |
| The kernel image | The linker symbols `KernelPhysicalStart` and `KernelPhysicalEnd`. | `0x00100000` – `0x0011A000` |
| The boot information structure | Its address and its `total_size` field. | `0x00120370` – `0x00120948` |
| The low 1 MiB | Legacy device and firmware reservations, the real-mode interrupt vector table and the VGA frame buffer. | `0x00000000` – `0x00100000` |

The low mebibyte is reserved in its entirety rather than by the map, because it
contains structures that the map does not describe and that later phases will
require: the application processor trampoline of sub-task 6.8 must be placed
below 1 MiB, since a processor released from reset begins execution in real mode.

### 6.2 Why the kernel extent is not derived from the ELF sections tag

Section 3.6.7 states that the address fields of the section headers "refer to
where the sections are in memory". That holds for a kernel linked at the address
at which it is loaded. Oxys-OS is a higher-half kernel: the address field of
every section other than `.boot` and `.boot.data` holds a virtual address in
the topmost two gibibytes. Deriving a physical extent from those values would
yield an absurd range spanning almost the whole address space.

The extent is therefore taken from the linker symbols, which are correct by
construction, and the ELF sections tag is parsed for validation and reporting
alone. The tag is nevertheless useful: the count of section headers it reports
confirms that the tag was interpreted correctly, since the parser rejects it
unless the entry size is exactly 64 bytes, the size of an ELF64 section header.


## 7. The physical frame allocator

Sub-task 2.2 introduces the frame allocator of `kernel/mm/pmm.c`. It is the sole
authority upon which frames are free; every later subsystem that requires
physical memory obtains it here.

### 7.1 Structure

The allocator is a bitmap of one bit per 4 KiB frame, in which a set bit denotes
a frame that is allocated or reserved. A bitmap is chosen in preference to a
free-frame stack because the initialisation sequence must be able to reserve a
frame *by address*: the kernel image, the boot information structure and the
bitmap itself all fall within regions the memory map classifies as usable, and
must be excluded after those regions have been released. A stack would allocate
in constant time but offers no means of removing a particular frame from the
middle.

### 7.2 Extent governed

The allocator governs every frame below the highest usable address, and no
frames above it. This is deliberate. Under QEMU the memory map reports a reserved
region beginning at `0xFD00000000`; representing it would demand a bitmap of some
two mebibytes to describe memory that does not exist. Nothing usable lies above
the highest usable address, so nothing is lost.

### 7.3 Order of initialisation

The order is significant, and a different one would be incorrect:

1. **Every frame is marked unavailable.** A region the boot loader did not
   describe is thereby treated as reserved. Memory whose existence is unattested
   must not be issued.
2. **Frames of usable regions are released.** The start of each region is rounded
   *upward* and its end *downward*, so that a frame only partially covered by a
   usable region is not released; the remainder of such a frame belongs to an
   adjacent region which may be reserved.
3. **The reserved extents are marked again.** Here the start is rounded
   *downward* and the end *upward*, so that a partially occupied frame is
   reserved in its entirety. The two roundings are deliberately opposite: both
   err towards withholding a frame rather than issuing one that is in use.

Step 3 must follow step 2, or the release would undo it.

### 7.4 Placement of the bitmap

The bitmap must itself occupy memory, and cannot be allocated by the allocator it
constitutes. It is placed by scanning the usable regions for one that can
accommodate it, advancing a candidate address past the low mebibyte, the kernel
image and the boot information structure. Two passes are made over the two
obstructions, because advancing past one may bring the candidate into the other
and their order in memory is not guaranteed.

The bitmap must also be addressable, which until sub-task 2.4 confines it to the
first gibibyte of physical memory, that being the extent of the higher-half
mapping.

### 7.5 Observed state

Under QEMU with 512 MiB:

| Quantity | Value |
| -------- | ----- |
| Frames governed | 131039 |
| Frames free | 130751 (523004 KiB) |
| Frames reserved | 288 |
| Bitmap | `0x0011B000` – `0x0011F000` (16 KiB) |

The 288 reserved frames account exactly: 256 for the low mebibyte, 27 for the
kernel image, 4 for the bitmap and 1 for the boot information structure.

### 7.6 The boot-time self-test

There is no test harness in a kernel, and none can exist before the userland of
Phase 7. `KernelVerifyFrameAllocator` therefore exercises the allocator at every
boot and asserts the properties whose violation would corrupt memory silently:
that issued frames are page aligned, that a frame is not issued twice, that no
frame is issued from the low mebibyte or from within the kernel image, that the
free count moves correctly, and that a freed frame is reissued in preference to
an untouched one.


## 8. The permanent kernel paging hierarchy

Sub-task 2.3 replaces the boot-time structures of `boot/boot.asm` with a
hierarchy built from frames obtained from the allocator of Section 7.

### 8.1 Structure

| Structure | Frames | Contents |
| --------- | ------ | -------- |
| Page-map level 4 | 1 | Entry 511 alone. Entry 0 is deliberately absent. |
| Page-directory-pointer table | 1 | Entry 510, reaching the kernel's 1 GiB window. |
| Page directory | 1 | Entry 0 refers to the page table below; entries 1 to 511 map 2 MiB pages. |
| Page table | 1 | 512 entries of 4 KiB, covering the first 2 MiB of physical memory. |

Four frames, 16 KiB in total, with the root observed at `0x0011F000`.

### 8.2 Why two granularities

The first 2 MiB of physical memory is mapped with 4 KiB pages, and the remainder
of the gibibyte with 2 MiB pages.

The kernel image lies within the first 2 MiB, and per-section permissions cannot
be applied at a granularity coarser than the sections themselves; a 2 MiB page
spanning both `.text` and `.data` would have to be writable, which would defeat
the protection entirely. Beyond the image there is nothing to distinguish, and
2 MiB pages cost 511 entries where 4 KiB pages would cost 261632, besides
consuming fewer translation-lookaside-buffer entries.

### 8.3 Permissions

| Region | Permission | Reason |
| ------ | ---------- | ------ |
| `.text` | Read, execute | Code must not be modifiable. |
| `.rodata` | Read | Constant data must not be modifiable. |
| `.data`, `.bss`, `.boot.data`, all other mapped memory | Read, write | Required for operation. |

The execute-disable bit is not applied. It requires `IA32_EFER.NXE` to be set,
and its introduction together with SMEP and SMAP belongs to Phase 13, sub-task
13.3. Withholding write permission is the part of the protection obtainable
without that machinery, and it is taken now rather than retrofitted.

**`CR0.WP` is required for any of this to bind.** Intel SDM, Volume 3A, Section
6.15, provides that supervisor-mode code faults upon writing to a read-only page
only when that flag is set; it is clear upon reset and GRUB does not set it.
Until Phase 3, sub-task 3.4, added the flag to `PagingInitialise`, the read-only
mappings described here were advisory: the kernel could write through them and no
fault would arise. Refer to `docs/design/INTERRUPTS.md`, Section 8.4.

Restrictions are applied at the leaf entry, never at an intermediate one. Intel
SDM, Volume 3A, Section 4.6, provides that the permissions of a translation are
the conjunction of those at every level, so a restrictive intermediate entry
would restrict every mapping beneath it rather than the one intended.

### 8.4 The removal of the identity mapping

No entry is created at index 0 of the page-map level 4 table, so the identity
mapping ceases to exist the instant CR3 is written. Per Intel SDM, Volume 3A,
Section 4.10.4.1, that write invalidates every translation-lookaside-buffer entry
for the current process context save those marked global; no mapping here is
global, so no stale translation of the low addresses can survive.

The switch is safe because the instruction following the write to CR3 is fetched
through the new hierarchy, and the kernel already executes from the higher half,
which the new hierarchy maps. The stack likewise resides in the kernel's BSS.
Nothing depends upon the identity map at this point: the values that the boot
code preserved at low physical addresses were consumed by `KernelEntryHigh`
before `KernelMain` was entered, and every pointer the kernel holds is a
higher-half address.

**One thing did depend upon it, and was missed.** The global descriptor table
loaded by `boot/boot.asm` resides in the `.boot.data` section at physical
`0x101000`.
No segment register was reloaded after the switch, so the cached descriptors
remained in force and the table was never read again — until Phase 3 installed
interrupt gates, delivery of which obliges the processor to read the descriptor
named by the gate's selector. The consequence and the remedy are recorded in
`docs/design/INTERRUPTS.md`, Section 5. The general rule it illustrates is that a
structure the processor reads directly must remain mapped for as long as the
processor may read it, and such reads are not visible in the source.

### 8.5 Verification

The hierarchy is verified by walking it in software rather than by dereferencing
addresses. There is no interrupt descriptor table until Phase 3, so a page fault
would escalate to a triple fault and reset the machine, destroying the evidence.
`KernelVerifyPaging` therefore confirms that the VGA frame buffer and the kernel
text translate to the physical addresses they were derived from, that a low
virtual address translates to nothing, that the text is not writable and the data
is, and that a write through a writable mapping is observable.

A read-only mapping cannot be confirmed by attempting a write until a page-fault
handler exists. That negative test belongs to Phase 3, sub-task 3.4.


## 9. The direct physical map

Sub-task 2.4 adds a second mapping of physical memory, at `0xFFFF800000000000`,
covering everything below the highest usable address.

### 9.1 Why a second mapping is needed

The kernel image window of Section 8 covers only the first gibibyte, because it
exists to map the kernel where it is linked. That was sufficient while the only
frames the kernel had to address were its own paging structures, which
`FrameAllocateBelow` confined to that gibibyte. It is not sufficient in general:
a machine with more memory would be unable to use any frame above the boundary
for a page table, a heap page or a process image.

The direct map removes the restriction. Every physical address has a
corresponding virtual address, `PhysicalToDirect` of it, and the kernel may
address any frame the allocator issues.

### 9.2 Why the window is retained

The kernel image window is not superseded. The kernel is linked within it: every
code address, every string literal and the kernel stack are addresses in that
window. Abandoning it would invalidate all of them at the instant CR3 was
written. Both mappings therefore coexist, and the same physical frame is
reachable by two virtual addresses.

The distinction is one of purpose, and the two translation helpers name it:

| Helper | Domain | Use |
| ------ | ------ | --- |
| `PhysicalToVirtual` | Below 1 GiB | The kernel image window. Used during the construction of the hierarchy, before the direct map is active. |
| `PhysicalToDirect` | All physical memory | The direct map. Used by everything running after `PagingInitialise`. |

### 9.3 Granularity and extent

2 MiB pages are used throughout. A gibibyte costs 512 entries in one page
directory; 4 KiB pages would cost 262144 entries across 512 page tables, which is
2 MiB of paging structures for every gibibyte mapped.

The extent runs to the highest usable address, rounded up to a large-page
boundary. Nothing usable lies beyond it, and the reserved regions at the top of
the address space, one of which QEMU reports at `0xFD00000000`, would demand an
enormous number of entries to describe memory that does not exist.

Under QEMU with 512 MiB the map covers 524288 KiB, and the whole hierarchy,
window and direct map together, occupies six frames.

### 9.4 The bootstrap ordering

The direct map cannot be used to build itself. Paging structures are reached
through `PagingTableAt`, which consults the window until the map is active and
the direct map thereafter. The flag governing that choice is set only after CR3
has been written, because until then the map exists in the structures but not in
the translations the processor performs.

`PagingAllocateTable` observes the same distinction, drawing from
`FrameAllocateBelow` before the map exists and from `FrameAllocate` afterwards.


## 10. The kernel virtual address allocator

Sub-task 2.5 introduces the arena of `kernel/mm/vmm.c`, occupying the 32 TiB at
`0xFFFFC00000000000`. It issues virtually contiguous ranges and backs every page
with a frame from the physical allocator.

### 10.1 Why it is distinct from the direct map

The direct map of Section 9 makes every physical frame addressable, but the
address of a frame there is fixed by its physical address. Two frames that are
not physically adjacent are not adjacent in the direct map either. A caller
requiring a contiguous buffer larger than a page cannot use it.

The arena provides the missing property. It allocates *address space*, and maps
arbitrary frames into it, so a range is contiguous in virtual memory whatever the
physical arrangement of the frames beneath it. The frames are explicitly not
contiguous, and a caller needing physical contiguity, such as a driver
programming a bus master, requires a facility this allocator does not offer.

### 10.2 Structure

Address space is issued by a bump pointer, with a free list of released ranges
searched first. The free list is a fixed-capacity array of 128 entries rather
than a linked structure, because this allocator sits *beneath* the heap: the heap
obtains its pages here, so allocating a list node from the heap would be
circular. The array introduces no such dependency.

Ranges are held in ascending order and coalesced with an adjacent neighbour upon
release. Without coalescing, a sequence of allocations and releases of differing
sizes would fragment the list into entries too small to satisfy any request while
the address space they describe remained contiguous.

A range released when the list is full is not reused, and the event is counted
and reported. Only address space is forfeit; the frames are always returned to
the physical allocator, and the arena has 32 TiB to lose.

### 10.3 Failure partway through

An allocation that cannot obtain a frame for every page unwinds: the pages
already mapped are unmapped, their frames returned, and the address range
restored to the free list. Returning NULL with part of the range mapped would
leak both frames and address space and leave the arena inconsistent.

### 10.4 Bounding a page count before it is multiplied

Every bound in the arena is computed as `page_count * PAGE_SIZE`, and that
product is a 64-bit unsigned quantity. The count arrives from a caller, and a
sufficiently large one wraps it:

| Count | Product | Effect upon the bound |
| ----- | ------- | --------------------- |
| 2³⁸ pages | 2⁵⁰ bytes | `0xFFFFC00000000000 + 2⁵⁰` carries past the top of the address space and truncates to `0x0003C00000000000`, which compares below the end of the arena. |
| 2⁵² pages | 0 | The bound becomes the bump pointer itself, so every request is admitted. |

In each case the comparison guarding the arena compares a wrapped number against
the arena's end, finds it smaller, and admits the request — a guard computing a
value the guard itself cannot trust. This is defined behaviour and not undefined:
unsigned arithmetic wraps by the standard. It is a defect of logic and not of
conformance, which is why it survived a compiler configured to refuse a great
deal.

The remedy is not an overflow test at each site. It is to bound the count once,
at each entry point, by what the arena could hold were it wholly empty:

```
ARENA_PAGE_CAPACITY = KERNEL_ARENA_SIZE / PAGE_SIZE = 32 TiB / 4 KiB = 2³³
```

A count so bounded gives a product of at most `KERNEL_ARENA_SIZE`, and the arena
ends at `0xFFFFE00000000000`, far enough below the top of the address space that
no sum of the two can wrap. Every later multiplication and addition is then safe
**by construction** rather than by a check repeated wherever one occurs.

`KernelPagesAllocate` applies exactly this bound, having no base to measure from
at the point it must decide. `KernelPagesFree` has one, and is therefore held to
a stronger test; see Section 10.5.

The damage the check prevents is not the refusal itself — the mapping loop is
bounded by physical memory and unwinds when a frame cannot be obtained, so an
oversized request returned NULL before this check existed too. The damage is what
the wrapped arithmetic left behind:

1. **The bump pointer is carried out of the arena.** A request of 2³⁸ pages
   advanced it by 2⁵⁰ bytes, leaving it at `0x0003C00000000000` — in the lower
   half, which is user address space. The next allocation would have been served
   from there and reported as a success.
2. **The free list is corrupted.** The unwinding of Section 10.3 inserts the
   range it failed to map, and `ArenaFreeListInsert` performs the same
   multiplication when testing for adjacency. A range that outlives the call is
   left where a later allocation will take it.

Both persist after the failed call and surface far from it, which is what makes
the defect worth refusing at the door rather than diagnosing later.


### 10.5 The whole range released must lie within the arena

`KernelPagesFree` validates the base address, the alignment and the mapping of
every page it releases. None of that establishes that the **range** lies within
the arena: a base at the arena's last page with a count of 2³³ satisfies both the
base test and the capacity bound of Section 10.4 while describing a range that
sweeps the 32 TiB above the arena.

Because the base is known to lie within the arena, the test is a subtraction:

```c
page_count > ((KERNEL_ARENA_BASE + KERNEL_ARENA_SIZE - base) / PAGE_SIZE)
```

The difference lies in `(0, KERNEL_ARENA_SIZE]` and so cannot wrap, and no
multiplication is performed at all. This subsumes the bound of Section 10.4,
which is this same test for a base at the arena's first page, so the release path
applies this one alone.

**Something already stopped such a range**, and it is worth being exact about
what. The release loop calls `PagingTranslate` upon each page and panics upon an
unmapped one; the space above the arena is unassigned, so the walk met an
unmapped page almost at once and halted, and neither the accounting nor the free
list was reached. There was no silent corruption. The check is nonetheless worth
stating, for three reasons:

1. **It named the wrong error.** A caller passing an over-long range was told
   that an unmapped page had been passed to it — a symptom observed partway
   through the range, pointing away from the argument that was actually wrong.
2. **It refused after acting.** Pages were unmapped and frames returned before
   the diagnosis. That the panic makes this moot is luck rather than design, and
   it is the opposite of the discipline the base and alignment tests follow.
3. **It held only while nothing was mapped above the arena.** Section 2 reserves
   that space for later use. On the day something is placed there the loop stops
   panicking: it unmaps and frees pages belonging to whatever now lives there,
   completes, and inserts the range into the free list. The protection would
   become a corruption path with nothing in the file to warn whoever introduced
   it.

The third is the reason the check is worth its two lines. An incidental
protection that depends upon a region being empty is not a protection; it is a
coincidence with an expiry date.

## 11. The kernel heap

Sub-task 2.5 also introduces the slab heap of `kernel/mm/heap.c`, providing
allocations of arbitrary size above the arena.

### 11.1 Structure

Eight size classes are served: 16, 32, 64, 128, 256, 512, 1024 and 2048 bytes. A
class is refilled by taking one page from the arena and carving it into objects,
which are threaded onto the class free list. A request larger than the greatest
class is served by whole pages.

Free objects hold the free-list link in their own first eight bytes. An object
that is free is by definition not in use by any caller, so this costs no storage.

### 11.2 How the size class is recovered

An allocation carries no header of its own. Every slab is one page, is page
aligned, and no object crosses a page boundary, so rounding a pointer down to a
page boundary yields the header of the slab containing it.

This matters more than it may appear. A per-object header of 32 bytes would be 6
per cent overhead on the 2048-byte class but 200 per cent on the 16-byte class,
which is the class small kernel structures will use most.

### 11.3 Validation

The slab header carries a magic value. A pointer passed to `KernelFree` that was
not obtained from the heap will almost always land on a page whose header does
not bear it, and is reported rather than acted upon. Releasing an object from a
slab that records none in use is likewise reported. Neither check is complete —
a pointer into the middle of a live slab would pass both — but each converts a
class of silent corruption into an immediate diagnosis.

### 11.4 A size that cannot be represented

A request larger than the greatest class is served by whole pages, and the pages
required are computed by adding the slab header to the size and rounding the sum
up to a page:

```c
AlignUp((uint64_t)size + sizeof(HeapPageHeader), PAGE_SIZE) / PAGE_SIZE
```

`AlignUp` is `(value + (alignment - 1)) & ~(alignment - 1)`, so the expression
adds `sizeof(HeapPageHeader) + PAGE_SIZE - 1` to the size before it divides. For
a size within that distance of `SIZE_MAX` the sum wraps to a small number, the
division yields a page count of one or two, and **the allocation succeeds**.

This is a worse failure than the arena's, and of a different kind. The arena's
wrapped bound admitted a request that then failed; this one returns a valid
pointer to two pages for a request of very nearly the whole address space.
Nothing reports an error. The caller learns the truth by writing past the end of
what it was given, at which point the fault has no visible connection to the
allocation that caused it — and the bound of Section 10.4 does not catch it,
the page count reaching the arena having already been made small by the wrap.

The size is therefore refused before the addition is performed. A request that
cannot be represented fails exactly as a request that cannot be satisfied does,
NULL being the only honest answer to either.

### 11.5 Known limitation

A slab whose objects have all been released is not returned to the arena. Doing
so would require removing its remaining objects from the class free list, which
is singly linked and offers no means of locating them. The page is retained and
reused by the next allocation of its class.

The consequence is that the heap's page consumption follows the high-water mark
of each class rather than the current demand. This is acceptable at present and
becomes worth addressing when the heap comes under sustained and varied load,
which is not before Phase 6. The remedy is a doubly linked free list per slab
rather than per class, at the cost of eight further bytes per free object.

### 11.6 Observed state

After the boot-time self-test under QEMU:

| Quantity | Value |
| -------- | ----- |
| Arena pages in use | 3 |
| Arena high-water mark | 4 |
| Live heap allocations | 0 |
| Slab pages retained | 3 |

The three retained pages are those of the 16, 256 and 1024-byte classes, held by
the limitation of Section 11.5. Zero live allocations confirms the self-test
released everything it took.



### 11.7 The boot-time self-test of the refusals

The refusals of Sections 10.4 and 11.4 are asserted at each boot, with counts and
sizes chosen for what each does to the arithmetic rather than for being large:
one page beyond `ARENA_PAGE_CAPACITY`, 2³⁸ pages to wrap the addition, 2⁵² pages
to wrap the multiplication, and `SIZE_MAX`, `SIZE_MAX - sizeof(void *)` and
`SIZE_MAX - PAGE_SIZE` to wrap the heap's rounding.

Asserting that each returns NULL is necessary and **not sufficient**, and the
distinction matters. A request of 2⁵² pages returned NULL before these checks
existed as well, the mapping loop having exhausted physical memory and unwound;
a self-test asserting NULL alone would have passed against the very defect it was
written for. What the wrapped arithmetic did was leave the arena broken behind
it.

Two further assertions therefore follow the refusals. The count of pages in use
must be unchanged, and — the one that does the work — an ordinary single-page
allocation made afterwards must return an address **within the arena**. Before
the bound existed, a request of 2³⁸ pages left the bump pointer at
`0x0003C00000000000`, and that subsequent allocation would have been served from
the lower half and reported as a success.

The heap's refusals need no such corroboration: before the check existed
`KernelAllocate(SIZE_MAX)` returned a non-null pointer, so asserting NULL
distinguishes the two states directly.

**What is not asserted, and cannot be.** The impossible arguments to
`KernelPagesFree` — an address outside the arena, a misaligned address, an
unmapped page, and the range test of Section 10.5 — each panic, which halts the
machine. Asserting one would require a means of surviving a panic, and there is
none before the test harness of Phase 7. The self-test therefore exercises the
other direction: a legitimate multi-page range is allocated, written, read back,
released, reissued from the free list and released again, and the arena's count
of pages in use is required to return to exactly what it was. A bound that was
inverted or off by one would panic upon that legitimate range rather than pass
silently, so the admit direction is covered even though the refusal is not.
## 12. Per-frame reference counting

Sub-task 2.6 gives every frame a reference count, which is the substrate upon
which copy-on-write is built in sub-task 2.8.

### 12.1 Semantics

`FrameAllocate` issues a frame with a count of one. `FrameReferenceIncrement`
records a further holder, as when an address space is cloned and a page becomes
shared. `FrameFree` releases one reference, and returns the frame to the
allocator only when the count reaches zero.

This redefinition of `FrameFree` is deliberate and required no change to its
existing callers. The kernel arena allocates a frame, holds the single reference
that allocation confers, and releases it when the page is unmapped; that is
correct under both the old semantics and the new. Copy-on-write will take
additional references, and the frame will then survive the release of all but the
last.

### 12.2 Why the table is allocated later than the allocator

The table is 255 KiB for the 131039 frames of a 512 MiB machine, and is allocated
from the kernel heap. The heap does not exist until sub-task 2.5, so reference
counting is established in a separate step after it, rather than within
`PhysicalMemoryInitialise`.

Frames allocated before that point — the paging structures, the arena's page
tables, the heap's own slabs, and the pages of the table itself — are seeded with
a single reference when the table is created. This is correct: each was issued
once and released no times.

The table is seeded *before* it is published. Publishing first and seeding
afterwards would leave a window in which `FrameFree` observed a zero count for a
live frame and reported a double release.

### 12.3 Width and overflow

A count is 16 bits, bounding the sharing of a single frame at 65535 address
spaces. That is far beyond any plausible degree of sharing. An attempt to exceed
it is reported rather than allowed to wrap, because a wrapped count would free a
frame that is still in use — a corruption that would surface arbitrarily later
and nowhere near its cause.

### 12.4 Observed state

After the boot-time self-test under QEMU with 512 MiB:

| Quantity | Value |
| -------- | ----- |
| Frames governed | 131039 |
| Frames free | 130671 (522684 KiB) |
| Frames used | 368 |
| Reference table | 255 KiB |
| Greatest count observed | 3 |

The used count has risen from the 288 of Section 7.5 by the paging structures,
the arena's page tables, the heap's slabs and the pages of the reference table
itself. The greatest count of three is that reached by the self-test, which takes
a frame to three references and confirms it survives the release of two of them.


## 13. Copy-on-write

Sub-task 2.7 implements the resolution of a copy-on-write fault. Sub-task 2.8,
described in Section 14, creates the shared pages that make it useful, by cloning
an address space.

### 13.1 How a page is marked

A copy-on-write page is mapped with two properties: `PAGE_ENTRY_WRITABLE` is
**clear**, and bit 9 of the page-table entry is **set**.

Bit 9 is available because Intel SDM, Volume 3A, Table 4-19 ("Format of a
Page-Table Entry that Maps a 4-KByte Page"), records bits 11:9 as *Ignored* — the
processor neither interprets nor modifies them.

Both properties are required and they do different work. The absence of write
permission is what causes the processor to raise the fault; the software flag
alone would be inert, since the processor ignores it. The flag records *why* the
page is read-only, distinguishing a shared page from one that is genuinely
constant, such as the kernel's `.rodata`.

### 13.2 Resolution

`PagingResolveCopyOnWriteFault` accepts a fault only when three conditions hold,
and each rejection is meaningful:

| Condition | Why |
| --------- | --- |
| The page is present | A fault upon an absent page is a different matter, to be resolved by supplying a page, not by copying one. |
| The software flag is set | The page was never shared; its read-only state is deliberate and permanent. |
| Write permission is absent | If the page is already writable the fault was raised for some other reason, and granting write permission again would resolve nothing — the instruction would restart and fault without end. |

It then takes one of two paths:

**More than one referrer.** A frame is allocated, the contents copied, and the
private copy installed with write permission and the flag cleared. One reference
to the original is then released. The frame returns to the allocator only when
its last holder releases it, which is precisely the property sub-task 2.6 exists
to provide.

**A single referrer.** No copy is made; write permission is restored and the flag
cleared. There is nobody to protect from the write, and copying would be pure
waste. This is the common case once the other holders of a shared page have
released it, and avoiding the copy is the whole economy of the scheme.

### 13.3 The direct map earns its keep

The copy is performed between `PhysicalToDirect` of the two frames. Neither need
have any other virtual address, and the two need not be related in the address
space of the faulting code. Without the direct map of sub-task 2.4 the kernel
would have to construct a temporary mapping for each frame and tear it down
afterwards, on every fault.

### 13.4 Verification

The self-test exercises the real handler rather than a substituted probe. Only
the *sharing* is simulated, a reference being taken to the frame directly rather
than by cloning; the test of Section 14.6 exercises the same handler upon pages
that a genuine clone has shared.

Both paths are tested. The shared case asserts that the frame changed, that the
duplicate retains all 4096 bytes of a pattern save the one written, that the flag
and the read-only state are cleared afterwards, and that the original frame still
carries the simulated holder's reference. The sole-owner case asserts that the
frame did *not* change and that no duplication was counted.

A leak assertion closes the test: the count of free frames must return to its
starting value. Copy-on-write allocates on one path and releases a reference on
another, and an imbalance between the two would leak physical memory in
proportion to the number of faults — the least visible and most damaging way for
the mechanism to be wrong.

### 13.5 Observed state

The figures are those reported at the end of a boot, and count the faults of both
this section's test and that of Section 14.6.

| Quantity | Value |
| -------- | ----- |
| Faults resolved | 4 |
| Frames duplicated | 2 |
| Resolved without duplication | 2 |

### 13.6 Limitations

1. Only 4 KiB pages are supported. A copy-on-write fault upon a large page would
   require the mapping to be split first, which nothing yet needs.
2. Resolution operates upon whichever hierarchy CR3 names, which from sub-task
   2.8 need not be the kernel's. It has no means of resolving a fault in an
   address space that is not the active one, and needs none: a fault is raised
   only by the processor that is translating through that space.
3. No shootdown is performed. `INVLPG` invalidates the translation upon the
   executing processor only; from sub-task 6.9 the other processors holding a
   stale entry must be signalled by inter-processor interrupt.

## 14. Address-space cloning

Sub-task 2.8 completes the memory-management substrate. An address space is a
paging hierarchy that may be created, cloned by the copy-on-write discipline,
activated and destroyed. `fork()`, in sub-task 6.6, is little more than a clone
of the calling process's address space together with a copy of its thread state.

The implementation is `kernel/mm/addrspace.c`; the interface is
`kernel/include/oxys/addrspace.h`.

### 14.1 The two halves

The page-map level 4 index occupies bits 47:39 of a linear address, so an index
below 256 has bit 47 clear. Intel SDM, Volume 1, Section 3.3.7.1, requires bits
63:48 to replicate bit 47 for an address to be canonical. The 512 entries of the
root table therefore divide exactly into the two canonical halves:

| Entries | Linear addresses | Treatment |
| ------- | ---------------- | --------- |
| 0 to 255 | `0x0000000000000000` to `0x00007FFFFFFFFFFF` | The address space proper. Cloned. |
| 256 to 511 | `0xFFFF800000000000` to `0xFFFFFFFFFFFFFFFF` | The kernel. Shared. |

`AddressSpaceCreate` copies the higher-half entries from the kernel hierarchy, so
every address space refers to the *same* kernel page tables rather than to copies
of them. Three consequences follow, and all three are wanted:

1. The kernel is mapped identically wherever execution is. An interrupt may be
   delivered whichever space is active, and its handler finds its code, its stack
   and its data where it left them.
2. A change of CR3 does not disturb the executing kernel. This is what permits
   `AddressSpaceSwitch` to be called from ordinary C code.
3. A mapping the kernel establishes afterwards is visible in every existing
   address space, the structures beneath those entries being the very ones the
   kernel modifies.

The third holds only for a mapping that requires no *new* page-map level 4 entry.
The kernel establishes all of its higher-half entries during `PagingInitialise`,
before any address space can exist, so the case does not presently arise. Should
a later phase extend the kernel's half into a fresh root entry, every existing
address space would have to be amended.

### 14.2 What is copied and what is shared

| Object | Treatment | Why |
| ------ | --------- | --- |
| Paging structures of the lower half | Duplicated | The two spaces must be able to diverge, and they diverge by acquiring different entries. A shared table would propagate every such change from one space to the other. |
| Frames mapped by those structures | Shared, with a reference recorded | This is the economy the whole mechanism exists for. A clone costs one frame per paging structure, not one per page of the address space. |
| Higher half | Shared, no reference taken | The kernel is not owned by any address space and outlives all of them. |

### 14.3 Protecting the parent

For each present leaf entry of the lower half:

- **The page is writable.** `PAGE_ENTRY_WRITABLE` is cleared and
  `PAGE_ENTRY_COPY_ON_WRITE` set, in **both** hierarchies, and the reference
  count of the frame is incremented.
- **The page is already read-only.** It is shared unchanged. Neither holder can
  write to it, so neither can observe a change made by the other, and there is
  nothing for the protection to prevent. Marking it would be worse than
  redundant: the mark would provoke a fault that could resolve to nothing, there
  being no write permission to restore.

Marking both hierarchies is essential rather than symmetric. Were only the child
protected, a write by the parent would proceed into the shared frame and the
child would observe it — the exact failure the mechanism exists to prevent, and
one that would produce no diagnostic of any kind.

### 14.4 Invalidation

The clone modifies the source hierarchy: pages that were writable are so no
longer. Where the source is the active hierarchy, the processor may hold cached
translations that still grant write permission, and a write through such a
translation would proceed without raising the fault upon which everything
depends.

CR3 is therefore rewritten at the end of a successful clone, rather than each
protected page being invalidated in turn. Intel SDM, Volume 3A, Section 4.10.4.1,
provides that writing CR3 discards every translation-lookaside-buffer entry for
the current process context save those marked global, and no mapping the kernel
establishes is global. The choice is one of bounded cost: a clone may protect an
arbitrary number of pages, so a sequence of `INVLPG` instructions is unbounded
where the single write is not.

The destination hierarchy needs no invalidation at all. It has never been loaded
into CR3, so the processor holds no translation derived from it.

### 14.5 Destruction

`AddressSpaceDestroy` walks the lower half, releasing one reference to each
mapped frame and releasing each paging structure outright. A frame still shared
with another address space survives, `FrameFree` returning it to the allocator
only upon the last reference. The higher half is not walked; it is the kernel's
and is merely referred to.

Destroying the active address space is refused with a panic. The hierarchy the
processor is translating through cannot be dismantled beneath it.

### 14.6 Verification

The self-test builds a parent address space containing two lower-half pages, one
writable and one read-only, clones it, and asserts the properties whose violation
would be silent:

| Assertion | The failure it detects |
| --------- | ---------------------- |
| The clone has a distinct root table | A clone that shared the hierarchy entirely. |
| The parent's writable page is read-only and marked | The failure of Section 14.3 — two spaces sharing memory each believes to be private. |
| The read-only page is *not* marked | A mark that would provoke an unresolvable fault. |
| Both frames carry two references | A clone that shared frames without recording the fact, so that the first release would free a frame still in use. |
| A write by the parent duplicates the frame | Sharing that was never protected. |
| The child still maps the original frame, holding the original contents | The parent's write leaking into the child. |
| The original frame falls to one reference | A resolution that released the frame outright rather than dropping one reference. |
| The child's own write duplicates nothing | The sole-owner path of Section 13.2, upon a genuinely shared page rather than a simulated one. |
| Destroying the child leaves the read-only frame allocated | A destruction that released a shared frame. |
| The count of free frames returns to its starting value | A leak of frames or of paging structures, in proportion to the number of clones. |

The test is performed with the parent and then the child actually loaded into
CR3, so the faults it provokes are resolved by the real page-fault handler within
the real hierarchy, not by a probe.

### 14.7 Observed state

| Quantity | Value |
| -------- | ----- |
| Clones performed | 1 |
| Pages shared | 2 |
| Of which protected | 1 |

### 14.8 Limitations

1. A large page in the lower half is rejected rather than provided for. Sharing
   one at 4 KiB granularity would require the mapping to be split first, and
   nothing yet establishes such a mapping.
2. There is no accounting of an address space's extent, and therefore no means of
   answering what a space maps without walking it. The process control block of
   sub-task 6.4 is where that record belongs.
3. Cloning is not safe against a concurrent fault upon the same address space.
   From sub-task 6.9 it must be performed under the lock governing the space, and
   the invalidation of Section 14.4 accompanied by a shootdown to the other
   processors upon which the source may be active.
