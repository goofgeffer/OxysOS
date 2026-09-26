<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Oxys-OS Memory Layout

**Phase**: sub-tasks 1.2 and 1.4, Phase 2, 7.3 and the growing tables of
[`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../boot/boot.asm`](../../boot/boot.asm),
[`../../linker.ld`](../../linker.ld), [`../../kernel/mm/`](../../kernel/mm/),
[`../../kernel/arch/x86_64/mm/`](../../kernel/arch/x86_64/mm/).
**Specifications**: Intel SDM, Volume 3A, Sections 3.3.7.1, 4.1.2, 4.5, 4.6,
4.10.4, 4.10.5 and 6.15, Figure 4-8 and Tables 4-15 and 4-19; Volume 1,
Section 3.3.7.1; Multiboot2 Specification, Sections 3.6.6 to 3.6.8.

How memory is divided and managed: the virtual address space and its regions,
the kernel image's placement, the physical frame allocator, the kernel's paging
hierarchy and direct map, the arena and heap above them, reference counting,
copy-on-write and address-space cloning, the user address space, and the
growing tables the kernel's object tables are built from.

## 1. The address space

x86_64 translates 48 bits of a linear address, and bits 63 to 47 must be equal
(Volume 3A, Section 3.3.7.1). The space is therefore two usable halves separated
by a non-canonical gap:

| Range | Extent | Assignment |
| ----- | ------ | ---------- |
| `0x0000000000000000` – `0x00007FFFFFFFFFFF` | 128 TiB | User address spaces (Section 13). |
| `0x0000800000000000` – `0xFFFF7FFFFFFFFFFF` | — | Non-canonical; any reference faults. |
| `0xFFFF800000000000` – `0xFFFFFFFFFFFFFFFF` | 128 TiB | The kernel, mapped identically into every address space. |

The kernel half:

| Base | Extent | Region |
| ---- | ------ | ------ |
| `0xFFFF800000000000` | 64 TiB | The direct map of all physical memory (Section 7). |
| `0xFFFFC00000000000` | 32 TiB | The kernel arena: the heap and device mappings such as the framebuffer (Section 8). |
| `0xFFFFFFFF80000000` | 2 GiB | The kernel image. |

**The image is in the top 2 GiB** so every kernel symbol is reachable by a
32-bit sign-extended displacement, the requirement of GCC's `-mcmodel=kernel`,
which gives smaller and faster code than the `large` model.

## 2. The boot-time paging hierarchy

`BootBuildPageTables` in `boot/boot.asm` builds four 4 KiB structures, aligned
as Section 4.5 requires. Both page-directory-pointer tables refer to one page
directory of 512 2 MiB pages, so one set of entries serves two mappings of
physical `[0, 1 GiB)`: an identity mapping, needed at the instant paging is
enabled, and the higher-half mapping at `0xFFFFFFFF80000000` (root index 511,
pointer index 510, by Figure 4-8). Entries carry `P`, `R/W` and, in the
directory, `PS` (Table 4-15); `U/S` stays clear, so nothing is reachable from
privilege level 3. The structures are initialised data in `.boot.data`, at a
defined physical address, needing nothing from the boot loader.

## 3. The kernel image

`linker.ld` loads the image at physical `0x00100000`, above the legacy
reservations of the first mebibyte.

| Section | Linked at | Holds |
| ------- | --------- | ----- |
| `.boot` | Its physical address | The Multiboot2 header and the 32-bit entry code. |
| `.boot.data` | Its physical address | The boot GDT, the preserved boot-loader values, the boot stack and the boot paging structures. |
| `.text`, `.rodata`, `.data` | `0xFFFFFFFF80000000` + load address | The kernel. |
| `.bss` | The same | Uninitialised data, including the 64 KiB kernel stack; placed last. |

- **`.boot` and `.boot.data` are separate** so each has a program header of its
  own: one executed and never written, the other written and never executed. One
  section would force the linker to make both readable, writable and executable
  ([`BOOT.md`](BOOT.md)).
- **`.bss` is last** because a `PROGBITS` section after a `NOBITS` one forces an
  extra program header; the boot loader zeroes the difference.

`PhysicalToVirtual` and `VirtualToPhysical` (`kernel.h`) translate within the
image window, and so only below 1 GiB; `PhysicalToDirect` (Section 7) covers all
physical memory.

## 4. The physical memory map and its reservations

The Multiboot2 memory map, reduced to `BootInformation`, describes the machine,
not what is free: the map "includes the regions occupied by kernel, mbi,
segments and modules" (Section 3.6.8). The frame allocator therefore reserves,
within regions the map calls usable:

| Extent | Taken from |
| ------ | ---------- |
| The kernel image | The linker symbols `KernelPhysicalStart` and `KernelPhysicalEnd`. |
| The boot information structure | Its address and `total_size`. |
| The frame bitmap | Where `PhysicalMemoryPlaceBitmap` placed it. |
| The boot modules, including the initial ramdisk | Each module tag's `mod_start` and `mod_end` (Section 3.6.6). |
| The low mebibyte | Wholly: legacy reservations the map does not describe, and the application processors' trampoline page at `0x8000`. |

- **A missed module reservation is the one that fails quietly.** A kernel that
  forgot its own image or bitmap stops at once; one that forgot the ramdisk
  mounts it and later reads whatever the allocator has since put there.
- **Every reservation is by extent**, marking each frame a range touches, so an
  unaligned module costs a frame and never shares one
  ([`../storage/INITRD.md`](../storage/INITRD.md)).
- **The kernel's extent comes from the linker symbols, not the ELF sections
  tag**, whose addresses for a higher-half kernel are virtual (Section 3.6.7);
  the tag is parsed only to validate and report it.

## 5. The physical frame allocator

`kernel/mm/pmm.c` is the sole authority on which frames are free: a bitmap of
one bit per 4 KiB frame below the highest usable address. A bitmap rather than a
free stack, because initialisation must reserve frames by address, which a stack
cannot remove from its middle. Frames above the highest usable address are not
represented: a reserved region QEMU reports at `0xFD00000000` would otherwise
cost megabytes of bitmap for memory that does not exist.

**Initialisation runs in an order that errs toward withholding:**

1. Every frame is marked unavailable, so memory the map does not attest is never
   issued.
2. Usable regions are released, rounded inward, so a frame only partly usable
   stays withheld.
3. The reservations of Section 4 are marked, rounded outward, so a partly
   occupied frame is reserved whole. This must follow step 2 or the release
   would undo it.

The bitmap is placed by scanning usable regions past the low mebibyte, the image
and the boot information, in two passes because moving past one obstruction may
land in the other.

## 6. The kernel paging hierarchy

`PagingInitialise` replaces the boot structures with a hierarchy built from
allocator frames: root entry 511 alone, a pointer table reaching the image
window, and a directory whose first entry is a 4 KiB page table for the first
2 MiB and whose others map 2 MiB pages.

- **Two granularities.** The image lies in the first 2 MiB, and section
  permissions need pages no larger than the sections; beyond it 2 MiB pages cost
  511 entries where 4 KiB pages would cost 261,632.
- **Permissions.** `.text` is read and execute, `.rodata` read only, everything
  else read-write. Execute-disable needs `IA32_EFER.NXE` and waits for 13.3.
- **`CR0.WP` is set**, because without it supervisor writes ignore read-only
  pages (Section 6.15) and the permissions above would be advisory.
- **Restrictions are applied at the leaf**, because a translation's rights are
  the conjunction of every level (Section 4.6), and a restrictive intermediate
  entry would restrict everything beneath it.
- **The identity mapping is gone the instant CR3 is written**: root entry 0 is
  never created, the write flushes every non-global translation (Section
  4.10.4.1), and nothing after `KernelEntryHigh` uses a low address. A structure
  the processor reads directly, such as the boot GDT, must stay mapped while it
  may be read, which the kernel's own GDT ensures ([`INTERRUPTS.md`](INTERRUPTS.md)).
- **The report asks of a translation, not a root entry.** `PagingReport` states
  whether one mebibyte still translates. Root entry 0 spans 512 GiB and stays
  present once anything was ever mapped low, since unmapping keeps the
  intermediate tables, which a later mapping of the region reuses.

## 7. The direct physical map

`0xFFFF800000000000` maps all physical memory below the highest usable address,
in 2 MiB pages, so `PhysicalToDirect` gives every frame an address. The image
window covers only its first gibibyte and cannot address a frame above it. The
window stays, because the kernel is linked in it; one frame is reachable by two
addresses, and the helpers name which is meant.

The map cannot build itself: `PagingTableAt` reaches paging structures through
the window until the map is active and through the map afterwards, the switch
made only after CR3 is written, and `PagingAllocateTable` draws below 1 GiB
until then.

## 8. The kernel arena

`kernel/mm/vmm.c` issues virtually contiguous ranges of the 32 TiB arena, each
page backed by an allocator frame. The direct map cannot give contiguity, since
a frame's address there is fixed by its physical address; the arena maps any
frames into adjacent pages. Physically contiguous memory, as a bus master
needs, is not offered.

- **A bump pointer and a free list of 128 released ranges**, sorted and
  coalesced. The list is an array because the heap draws its pages from here,
  and a list node taken from the heap would be circular. A range released with
  the list full is counted and forfeited: address space only, of which there is
  32 TiB.
- **A failed allocation unwinds**: pages mapped so far are unmapped, their
  frames returned and the range restored, so nothing is left half made.
- **A page count is bounded before it is multiplied.** `page_count * PAGE_SIZE`
  wraps for a large count: 2³⁸ pages carries the bound past the top of the
  address space, and 2⁵² pages makes it zero, and either is then admitted. The
  wrapped arithmetic moves the bump pointer into the user half and corrupts the
  free list. Every count is therefore refused above the arena's capacity, 2³³
  pages, so every later product and sum is safe by construction.
- **A released range must lie wholly within the arena**, tested by subtraction
  from the arena's end, which cannot wrap. A range that ran above the arena would
  unmap whatever came to be mapped there.

## 9. The kernel heap

`kernel/mm/heap.c` serves eight size classes, 16 to 2048 bytes, each refilled
one arena page at a time; larger requests take whole pages.

- **No per-object header.** Every slab is one page-aligned page, so rounding a
  pointer down finds its slab's header. A header per object would cost
  200 per cent on the 16-byte class the kernel uses most.
- **Validation by magic value**: a pointer not from the heap, or a release from
  a slab with none in use, is reported rather than acted on.
- **A size that cannot be represented is refused** before the header is added,
  since `size + header + PAGE_SIZE - 1` wraps near `SIZE_MAX` and would succeed
  with two pages for a request of nearly the whole address space.
- **An emptied slab is kept** for its class, the class list being singly linked;
  consumption follows each class's high-water mark.

## 10. Per-frame reference counting

Every frame has a 16-bit reference count. `FrameAllocate` issues a count of one,
`FrameReferenceIncrement` adds a holder, and `FrameFree` releases one reference,
returning the frame only at zero, which is what copy-on-write needs and what
every earlier caller already did.

- **The table is created after the heap**, from which its 255 KiB (for 512 MiB)
  is allocated, and it is seeded with one reference for every frame already
  issued before it is published, so `FrameFree` never sees a live frame at zero.
  `KernelInitialiseFrameReferences` runs it after the display phase.
- **An overflow is reported**, not wrapped: a wrapped count frees a frame still
  in use.

## 11. Copy-on-write

A copy-on-write page has `PAGE_ENTRY_WRITABLE` clear and bit 9 of its entry set,
a bit the processor ignores (Table 4-19). The clear bit makes the processor
fault; the set bit says why the page is read-only, distinguishing it from
constant data.

`PagingResolveCopyOnWriteFault` accepts a fault only on a present, flagged,
non-writable page: an absent page is a different fault, an unflagged one is
genuinely read-only, and a writable one would fault again forever. Then:

- **More than one referrer**: a frame is allocated, the contents copied through
  the direct map, the copy installed writable, and one reference to the original
  released.
- **One referrer**: write permission is restored and nothing is copied, which is
  the whole economy of the scheme.

`PagingInvalidate` removes the stale translation on every processor, by
interrupt and acknowledgement, because `INVLPG` acts on one (Section 4.10.5); an
unacknowledged shootdown is fatal, since the frame may otherwise be given away
while another processor still uses it ([`CONCURRENCY.md`](CONCURRENCY.md)).

## 12. Address-space cloning

`kernel/arch/x86_64/mm/addrspace.c` creates, clones, activates and destroys
address spaces; `fork` is one clone and one thread ([`PROCESS.md`](PROCESS.md)).

- **Root entries 256 to 511 are shared**, copied from the kernel's hierarchy, so
  every space has the same kernel page tables: an interrupt finds the kernel
  wherever it lands, CR3 can change under running C code, and a later kernel
  mapping appears everywhere. All the kernel's root entries exist before any
  address space does.
- **Lower-half tables are duplicated and their frames shared**, one reference
  added per frame: a clone costs a frame per table, not per page.
- **A writable page is protected in both spaces.** Protecting only the child
  would let the parent's writes appear in it, silently. A read-only page is
  shared unmarked, since a mark would provoke a fault with nothing to restore.
- **CR3 is rewritten after a clone** of the active space, flushing every
  translation that still grants write; one write, where invalidating each page
  would cost without bound.
- **Destruction** releases one reference per mapped frame and every lower-half
  table; destroying the active space panics.

## 13. The user address space

| Range | Holds |
| ----- | ----- |
| The first page | Never mapped: a null dereference faults. |
| The image | Where its program headers say, at or above `0x400000`. |
| One page above the image | A guard, never mapped. |
| Above it, to the break | The heap, moved by `brk`, bounded at `PROCESS_BREAK_MAXIMUM`. |
| Below the stack | A guard page. |
| `0x00006FFFFFFF0000` – `0x0000700000000000` | The stack, sixteen pages. |

- **The guard is a page**, the unit of mapping; anything smaller would share the
  heap's first page and be writable.
- **Nothing enforces the gap between heap and stack**, because the break's bound
  keeps the heap near twenty mebibytes against a stack at 112 TiB. A check for a
  case that cannot arise cannot be tested; `ProcessSetBreak` is where it goes if
  the bounds change.
- **The extents are kept in the process control block**, since an address space
  is only a hierarchy and cannot say what it maps or why ([`LIBC.md`](LIBC.md)
  holds the call).
- **`AddressSpaceUnmapPage` returns the frame it withdrew** rather than freeing
  it, because only the caller knows whether another space still shares it. The
  empty page tables it leaves are kept until the space is destroyed, a heap that
  shrank being one that will grow.

## 14. The growing table

The process, thread, filesystem node, open-file and pipe tables are growing
tables (`kernel/include/oxys/mm/table.h`, `kernel/mm/table.c`): a directory of up
to 256 chunks, the first a static array and each further one a zeroed heap
allocation taken when a claim finds every slot in use.

| Table | Chunk constant | Entries per chunk |
| ----- | -------------- | ----------------- |
| Processes | `PROCESS_CHUNK` | 64 |
| Threads | `THREAD_CHUNK` | 128 |
| Filesystem nodes | `VFS_NODE_CHUNK` | 64 |
| Open files | `VFS_FILE_CHUNK` | 32 |
| Pipes | `VFS_PIPE_CHUNK` | 8 |

- **Chunks never move and are never freed**, because run queues, wait channels,
  per-processor areas and open files hold pointers into these tables; a
  reallocated array would leave them naming freed memory. Memory taken at a peak
  is kept, as a fixed table of that size would have been.
- **The first chunk is static**, so the tables exist before the heap and a file
  can still be opened when the heap is exhausted; the heap's refusal is a full
  table, as before.
- **A zeroed entry is an unused one** in every table, so a new chunk needs no
  pass over it.
- **Memory, not the directory, is the limit** in practice: 256 chunks is 16,384
  processes. Exhaustion is `ENOMEM` from `fork` and `EMFILE` from `open` and
  `pipe`.
- **Growth happens on the bootstrap processor only**, because the heap is
  unsynchronised; elsewhere a full table is refused and the refusal counted.
- **A chunk is published pointer first, then a compiler barrier, then the
  count.** x86_64 does not reorder stores, so another processor sees either the
  old capacity or the new chunk in place.
- **A walk runs to the capacity at the time**, and `procinfo` answers `EINVAL`
  beyond it, which is where `ps` and `shutdown` stop.

## Verification

The self-tests are in [`../../kernel/test/mm/memory.c`](../../kernel/test/mm/memory.c)
and, for the tables' users, [`../../kernel/test/storage/vfs.c`](../../kernel/test/storage/vfs.c).

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| Issued frames are page aligned, never twice, never from the low mebibyte or the image; the free count moves correctly; a freed frame is reissued first. | Two owners of one frame; an allocation over the kernel. |
| The image and the VGA buffer translate to their physical addresses, a low address to nothing; text is not writable and data is. | A hierarchy that maps the wrong frames or grants the wrong rights. |
| Arena ranges map and unmap; oversized counts (2³³+1, 2³⁸, 2⁵² pages) are refused, the pages in use unchanged, and the next allocation lands inside the arena. | A wrapped bound that moves the bump pointer into the user half. |
| `KernelAllocate(SIZE_MAX)` and its near neighbours return NULL; a zeroed allocation is zero. | Two pages returned for a request of nearly the address space. |
| A shared frame survives the release of all but its last reference. | A frame freed while in use, or never freed. |
| A copy-on-write fault on a shared page copies it, keeps every byte but the one written, clears the flag, and drops one reference; on a sole page it copies nothing; the free count returns to its start. | Sharing never broken, or a leak per fault. |
| A clone has its own root; the parent's writable page becomes read-only and flagged, its read-only page unflagged; both frames hold two references; writes by either diverge; destroying the child keeps shared frames; the free count returns. | Two spaces sharing memory each believes private; a leak per clone. |
| A growing table keeps its first entry's address and value when it grows, zeroes the new chunk, refuses the index at its capacity and counts the growth; forty open files and ten pipes are made and used. | Pointers left dangling by growth; a new chunk read as live entries. |

The refusals of `KernelPagesFree` panic and cannot be asserted; the admitting
direction is, with a multi-page range allocated, written, released, reissued and
released again, the pages in use returning exactly to their start.

## Limitations

1. No execute-disable, SMEP or SMAP until 13.3.
2. Copy-on-write and cloning support 4 KiB pages only; a large page in the
   lower half is refused.
3. Cloning is not yet done under the address space's lock, there being one
   user thread of control ([`CONCURRENCY.md`](CONCURRENCY.md)).
4. An emptied heap slab is not returned to the arena.
5. A range released to a full free list forfeits its address space.
6. Intermediate page tables are never reclaimed while their address space lives.
7. A table grows only on the bootstrap processor.
