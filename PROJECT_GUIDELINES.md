# Oxys-OS Project Guidelines

This document sets out the conventions binding upon all work in the Oxys-OS
repository. It governs project identity, research protocol, technical stack,
coding and documentation standards, the phase roadmap in outline, and the
practices that are prohibited.

## 1. Project Identity
- **Name**: Oxys-OS (Oxys).
- **Purpose**: A monolithic, Unix-based x86_64 operating system built entirely from scratch in ISO C11 and assembly.
- **Root Directory**: `~/oxys-os` (within WSL2 on Windows 10).
- **Primary Language**: C11 for kernel and userland; NASM assembly for boot and architecture-specific routines.

## 2. Absolute Rules of Engagement
- **Formality**: All communication, documentation, code comments, and commit messages must adopt a strictly formal, technical, and objective tone. The use of emojis, slang, humour, or any informal expression is strictly prohibited.
- **Specification-Driven Development**: Before implementing any subsystem, the contributor must retrieve and cite the official authoritative specifications (e.g., Intel manuals, Multiboot2, EXT2, System V ABI, UEFI Specification, relevant RFCs). No implementation shall proceed without referenced specification.
- **Synchronous Documentation**: Every single code change must be immediately reflected in:
  - Updated inline comments and file-header blocks.
  - Corresponding design notes within the `docs/` folder, in the group its subject belongs to: `project/`, `design/`, `devices/` or `storage/`, as `docs/README.md` sets out.
  - The living roadmap `docs/project/PLAN.md` (marking completed tasks and adjusting subsequent steps).
- **No External Code Copying**: All source code must be original. Reference implementations may be studied for understanding but must not be transcribed. The only permitted inclusions are standard public domain headers or minimal stub code explicitly required by the toolchain (e.g., linker scripts).
- **Testing Mandate**: Every milestone must be bootable and testable in both QEMU (with `-machine q35 -cpu qemu64 -smp cores=2` for SMP testing) and VirtualBox. UEFI testing requires QEMU with OVMF firmware (`-bios /usr/share/ovmf/OVMF.fd`). Real hardware compatibility must be considered from the first ISO build.


## 3. Technical Stack and Constraints
- **Toolchain**: `x86_64-elf-gcc`, `x86_64-elf-ld`, `nasm`, `grub-mkrescue`, `make`. All must be installed and functional in the WSL2 environment.
- **Boot Protocol**: Multiboot2 (GRUB as the bootloader) for legacy BIOS; UEFI boot path (PE32+ image) added later.
- **Kernel Image**: ELF64, linked according to `linker.ld`. Higher-half kernel layout is recommended.
- **Build System**: GNU Make with explicit targets: `all`, `clean`, `iso`, `run-qemu`, `run-vbox`, `run-uefi` (for OVMF testing).
- **Debugging**: Serial output over COM1 shall be implemented in the earliest device-driver stage to enable remote debugging.

## 4. Code and Documentation Standards
- **File Headers**: Each source file must commence with a block comment containing:
  - The file name and path.
  - A one-sentence summary of its purpose.
  - A list of key functions or data structures.
  - References to the specifications it implements.
- **Identifier Conventions**:
  - Types and global functions: `PascalCase` (e.g., `MemoryAllocator`, `ParseELFSegment`).
  - Macros and constants: `UPPER_SNAKE_CASE` (e.g., `VGA_WIDTH`, `PAGE_SIZE`).
  - Local variables: `snake_case`.
- **Comment Language**: Complete, grammatically correct English sentences. Abbreviations are permitted only if they are universally recognised (e.g., `PIC`, `IDT`, `ATA`).
- **Compiler Flags**: `-Wall -Wextra -Werror` shall be enabled at the earliest stable stage, with exceptions explicitly documented in the Makefile.

## 5. Project Milestones (13 Formal Phases)
The roadmap is enumerated in full in `docs/project/PLAN.md`, which is the single source
of truth for progress. It comprises 13 major phases, ordered by dependency:

1. **Bootstrapping & Early Output** — Cross-compiler, Multiboot2, long-mode, VGA text output, ISO generation.
2. **Memory Management (incl. Copy‑on‑Write)** — Physical frame allocator, paging, higher-half kernel, virtual allocator, COW page fault handler with reference counting.
3. **Interrupts, Exceptions & Keyboard Input** — IDT, PIC/APIC, interrupt dispatcher, PS/2 keyboard driver.
4. **Basic Device Drivers** — Serial (COM1), VGA text mode, ATA PIO, PCI enumeration.
5. **EXT2 Filesystem** — Superblock, group descriptors, inodes, directories, read/write support, mounting.
6. **Graphics, System Calls, Process Management & SMP** — Linear framebuffer, 2D primitives, bitmap font and graphical console, PS/2 mouse, compositing surface; syscall interface, ELF loader, PCBs, scheduler (MP-aware), context switching, APIC initialisation, spinlocks and IPIs, CPU bring-up.
7. **Userland & Minimal C Library** — Libc core functions, `malloc`/`free`, syscall wrappers, basic utilities (`ls`, `cat`, `echo`), initial ramdisk.
8. **Shell** — Command interpreter, built-ins, external program execution, job control.
9. **The Desktop, its System Services & its Configuration** — Stacking window manager, the client protocol serving user processes, `init` and service supervision, the system configuration format and the `/etc` hierarchy, the session and its panel, terminal emulator, the utilities a desktop requires, and the settings application.
10. **Cryptography** — PRNG (RDRAND/timing), SHA-256, AES-128/256, user-space API.
11. **Networking** — Ethernet driver (RTL8139/E1000), ARP, IP, ICMP, UDP, minimal TCP, socket API, utility (`ping`).
12. **UEFI Transition** — UEFI application entry point, System Table parsing, Boot Services, runtime services, GOP integration, dual-boot (BIOS + UEFI) capability.
13. **Polish, Optimisation & Final Hardening** — Performance optimisation, security hardening (SMEP, SMAP, KASLR), comprehensive documentation, real-hardware testing, final image.

Each phase shall be broken into atomic, testable sub-tasks.

The graphical work is divided between Phases 6 and 9, by dependency. What requires no process to exist — the framebuffer the boot loader describes, the 2D primitives, the bitmap font and the console above it, the pointer, the compositing surface — belongs to Phase 6, and supplies the diagnostic console every later phase reports through. What cannot be built without processes — the window manager, the client protocol, the system services and the desktop they maintain — belongs to Phase 9, after the shell.

UEFI is a dedicated phase before final polish.

## 6. Research and Reference Protocol
- **Permitted Sources**:
  - Intel 64 and IA-32 Architectures Software Developer Manuals (Volumes 1–4).
  - Multiboot2 Specification.
  - EXT2 Filesystem Specification (Linux kernel documentation).
  - System V ABI for x86_64 (including ELF and calling convention).
  - ATA-8/ATAPI command set.
  - IEEE 802.3 and relevant IETF RFCs (for networking).
  - UEFI Specification (latest version).
  - VESA BIOS Extensions (VBE) and UEFI GOP documentation.
- **Citing**: Every design document and relevant code comment must include a formal citation (e.g., "Refer to Intel Vol. 3A, Section 4.1 for paging structure details").

## 7. Update Discipline
- `docs/project/PLAN.md` is the single source of truth for task tracking. It shall be updated in every session after any functional change.
- This document is amended only by explicit decision of the project owner. Any such amendment must be acknowledged, and the reason for it recorded, in the commit that makes it.

## 8. Prohibited Practices
- Use of non-standard or GCC-specific extensions without first documenting the rationale.
- Use of floating-point operations in the kernel unless explicitly required for a specific algorithm (and even then, with clear justification).
- Reliance on undefined behaviour. All pointer arithmetic, type punning, and bitwise operations must be explicitly defined in the C11 standard.
- Use of any third-party library or external code inside the kernel proper. The only external dependencies are the bootloader (GRUB) and the cross-compiler toolchain.

## 9. Initialisation Checklist
At the start of each working session, the contributor shall verify:
- The working directory is `~/oxys-os`.
- The cross-compiler is available in `$PATH`.
- `docs/project/PLAN.md` exists and reflects the current state of progress.
- The latest code compiles without fatal errors (if applicable).

## 10. Directory-Level Documentation
- Every high-level folder containing useful material (like `crypto/`, `drivers/`, `boot/`, or `kernel/`) should have ATLEAST a `README.md` for documentation.
- Such a `README.md` shall state the purpose of the directory, enumerate its contents, cite the specifications its material implements, and identify the phase of `docs/project/PLAN.md` to which that material belongs.
- A directory that is presently empty, having been created in anticipation of a later phase, acquires its `README.md` at the moment material is first placed within it.
- This requirement is subordinate to the Synchronous Documentation rule of Section 2: a directory `README.md` must be updated in the same change that alters the contents it describes.

These guidelines are binding upon every contributor operating within the Oxys-OS
repository, whether human or automated.