# Oxys-OS Memory Layout

**Corresponding phase**: Phase 1, sub-tasks 1.2 and 1.4. This document will be
extended substantially in Phase 2.

**Specifications**: Intel 64 and IA-32 Architectures Software Developer's Manual,
Volume 3A, Sections 3.3.7.1, 4.1.2, 4.5 and Table 4-15.

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
| `0xFFFF800000000000` | 64 TiB | The direct map of all physical memory. | Phase 2, sub-task 2.4 |
| `0xFFFFC00000000000` | 32 TiB | The kernel virtual allocator arena, comprising the kernel heap and device mappings. | Phase 2, sub-task 2.5 |
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
| `.boot` | `0x00100000` | `0x00100000` | PROGBITS | The Multiboot2 header, the 32-bit entry code, the 64-bit trampoline, the boot GDT, the preserved boot loader values, the boot stack and the four paging structures. |
| `.text` | `KernelVirtualBase + p` | `p` | PROGBITS | The 64-bit kernel code. |
| `.rodata` | `KernelVirtualBase + p` | `p` | PROGBITS | Read-only data and string literals. |
| `.data` | `KernelVirtualBase + p` | `p` | PROGBITS | Initialised writable data. |
| `.bss` | `KernelVirtualBase + p` | `p` | NOBITS | Uninitialised data, including the 64 KiB kernel stack. |

Here `p` denotes the load address that the linker assigns by continuing
contiguously from the preceding section, and `KernelVirtualBase` is
`0xFFFFFFFF80000000`.

The `.boot` section is linked at its physical address because it executes before
paging is enabled. Every subsequent section is linked at its higher-half virtual
address, with an explicit `AT()` clause fixing its load address, so that GRUB
places the image correctly while the code executes from the upper half.

The `.bss` section is of type `NOBITS` and occupies no space in the file. It is
placed last so that no `PROGBITS` section follows it, which would otherwise
oblige the linker to emit a further program header. The boot loader zeroes the
difference between the memory size and the file size of the containing program
header.

The boot-time paging structures are emitted as initialised zero data within
`.boot` rather than being reserved in `.bss`, so that they occupy a defined
physical location within the loaded image and require no action by the boot
loader before `_start` executes.

## 5. Address translation helpers

`kernel/include/oxys/kernel.h` provides `PhysicalToVirtual` and
`VirtualToPhysical`, which add and subtract `KERNEL_VIRTUAL_BASE` respectively.
While the boot-time hierarchy is in effect these are valid only for physical
addresses below one gibibyte. Phase 2, sub-task 2.4, introduces the direct
physical map and extends their domain to the whole of physical memory.
