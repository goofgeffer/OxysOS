# SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
# SPDX-License-Identifier: CC0-1.0
# ==============================================================================
# File: Makefile
#
# Purpose:
#   Builds the Oxys-OS kernel, produces a bootable ISO 9660 image by way of
#   grub-mkrescue, and provides targets for execution under QEMU with legacy
#   BIOS firmware, under QEMU with UEFI firmware, and under VirtualBox.
#
# Targets:
#   all       - Builds the kernel ELF image. This is the default target.
#   iso       - Builds the bootable ISO image.
#   clean     - Removes every generated artefact.
#   run-qemu  - Executes the ISO under QEMU with legacy BIOS firmware.
#   run-uefi  - Executes the ISO under QEMU with the OVMF UEFI firmware.
#   run-vbox  - Registers and executes the ISO under VirtualBox.
#   verify    - Executes the ISO under QEMU without a display, asserts that the
#               expected banner is emitted upon the serial port, and asserts
#               that no boot-time self-test reported a failure.
#   toolcheck - Confirms that every required tool is present, and reports upon
#               the one optional one.
#   clang-check - Compiles every translation unit with a second compiler and
#               discards the objects, for the diagnostics alone. Builds nothing.
#   build-record - Appends one numbered row to docs/project/BUILDS.md describing
#               the image presently in build/. Builds nothing and runs nothing.
#
# References:
#   - GNU Make Manual, Section 10.5.3 (automatic variables) and Section 4.12
#     (pattern rules).
#   - GNU GRUB Manual, Section 3.4 (grub-mkrescue).
#   - System V Application Binary Interface, AMD64 Architecture Processor
#     Supplement: the red zone described in Section 3.2.2 must be disabled in
#     kernel code because an interrupt may be delivered at any instruction
#     boundary and would otherwise overwrite it.
#   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
#     Section 13.1: the SSE and x87 units require explicit enabling and state
#     management, which the kernel does not yet perform; their instruction sets
#     are therefore excluded from code generation.
# ==============================================================================

# ------------------------------------------------------------------------------
# Toolchain.
# ------------------------------------------------------------------------------

CROSS_PREFIX := x86_64-elf-
CC           := $(CROSS_PREFIX)gcc
LD           := $(CROSS_PREFIX)ld
# The archiver, added at sub-task 7.5: the C library is collected into an archive
# that user programs link against, which is the first thing this project has
# built that is neither an object nor an image.
AR           := $(CROSS_PREFIX)ar
OBJCOPY      := $(CROSS_PREFIX)objcopy
NASM         := nasm
GRUB_MKRESCUE := grub-mkrescue
QEMU         := qemu-system-x86_64

# ------------------------------------------------------------------------------
# Directories and output artefacts.
# ------------------------------------------------------------------------------

BUILD_DIR    := build
ISO_DIR      := $(BUILD_DIR)/isodir
KERNEL_ELF   := $(BUILD_DIR)/oxys.elf
KERNEL_MAP   := $(BUILD_DIR)/oxys.map
ISO_IMAGE    := $(BUILD_DIR)/oxys.iso
LINKER_SCRIPT := linker.ld

# ------------------------------------------------------------------------------
# Compilation flags.
#
# -ffreestanding        The program does not assume a hosted environment
#                       (ISO/IEC 9899:2011, Section 4, paragraph 6).
# -fno-builtin          Suppresses the substitution of library routines that the
#                       kernel does not provide.
# -fno-stack-protector  The stack canary requires runtime support that does not
#                       yet exist. Stack protection is introduced in Phase 13.
# -fno-pic -fno-pie     The kernel is loaded at a fixed address and requires no
#                       position-independent code.
# -mno-red-zone         Required of kernel code; refer to the reference above.
# -mno-mmx -mno-sse
# -mno-sse2 -mno-80387  The vector and floating-point units are not yet enabled.
# -mcmodel=kernel       All symbols reside within the topmost 2 GiB of the
#                       address space, permitting 32-bit sign-extended
#                       displacements.
# -std=c11 -pedantic    Conformance to ISO C11 as required by PROJECT_GUIDELINES.md.
# -Wall -Wextra -Werror The diagnostic regime mandated by PROJECT_GUIDELINES.md, Section 4.
#
# Documented exception: -Wno-unused-parameter is NOT applied. No warning is
# suppressed in this regime, which is the one the kernel is built with; the one
# suppression that exists anywhere in this file belongs to the clang-check
# target alone and is recorded there with its justification.
# ------------------------------------------------------------------------------

# The kernel is compiled against two include roots, and the second is not a
# convenience. `kernel/abi/` holds the system-call interface a program is
# entitled to, under the permissive licence so that the MIT C library may include
# it; `kernel/include/` holds the kernel's own corpus, under the kernel's
# licence. LICENSING.md, Section 2.1, required the division. The two roots carry
# no file of the same name, so nothing here depends upon the order.
INCLUDE_DIRS := -Ikernel/abi -Ikernel/include

# What the C library is compiled against, in addition to the two roots above. It
# is a separate variable because only the C library's own rule uses it.
LIBC_INCLUDE_DIRS := -Ilibc/include

CFLAGS := -std=c11 -pedantic \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
          -mcmodel=kernel \
          -Wall -Wextra -Werror \
          -Wshadow -Wpointer-arith -Wcast-align -Wstrict-prototypes \
          -Wmissing-prototypes -Wredundant-decls -Wwrite-strings \
          -O2 -g \
          $(INCLUDE_DIRS)

ASFLAGS := -f elf64 -g -F dwarf -Wall -Werror

LDFLAGS := -n -T $(LINKER_SCRIPT) -Map $(KERNEL_MAP) -z max-page-size=0x1000

# ------------------------------------------------------------------------------
# Source enumeration.
# ------------------------------------------------------------------------------

# ------------------------------------------------------------------------------
# The C library of Phase 7.
#
# These are not kernel sources and the kernel does not call them. They are
# compiled into the image for one reason: `make verify` is the only thing in this
# project that can run code at all, and kernel/test/libc/string.c is what
# asserts them. Sub-task 7.5 adds the user-mode link, at which point the same
# sources are compiled a second time — with the flags a program requires, which
# are not these — into a library a program links against. docs/design/LIBC.md,
# Section 7, records both paths and why the first exists.
#
# They are named in a list of their own rather than merged into C_SOURCES so that
# the boundary is legible in one place, and so that the second compilation has a
# list to name. They are compiled by a rule of their own below, which adds
# -Ilibc/include: the kernel is deliberately not compiled against that root, so
# that no kernel translation unit can include <string.h> by accident and acquire
# a dependency upon the userland.
# ------------------------------------------------------------------------------

LIBC_SOURCES := libc/string/copying.c \
                libc/string/comparison.c \
                libc/string/search.c \
                libc/string/miscellaneous.c \
                libc/string/error.c \
                libc/syscall/result.c \
                libc/syscall/calls.c \
                libc/stdlib/heap.c \
                libc/stdlib/system.c \
                libc/stdio/stream.c \
                libc/stdio/format.c \
                libc/stdio/system.c \
                libc/stdlib/exit.c \
                libc/stdlib/environment.c \
                libc/line/line.c \
                libc/line/system.c \
                libc/config/config.c \
                libc/config/system.c \
                libc/signal/signal.c

# The C library's one assembly translation unit, which is the system-call
# instruction itself.
#
# It is named here rather than in ASM_SOURCES below for the same reason
# LIBC_SOURCES is not merged into C_SOURCES: it is not the kernel's, and the
# boundary is worth being able to see in one place. It is assembled by the same
# pattern rule as the kernel's assembly, there being nothing about the flags that
# differs — NASM has no include root and no code model.
#
# docs/design/LIBC.md, Section 8.1, records why the invocation is a translation
# unit of assembly rather than inline assembly inside the C wrappers: it must
# contain no relocation, so that kernel/test/libc/wrappers.c can copy the bytes
# this library ships into a program's address space and execute them at privilege
# level 3.
LIBC_ASM_SOURCES := libc/syscall/invoke.asm

# ------------------------------------------------------------------------------
# The shell's grammar, of sub-task 8.2.
#
# The tokeniser and the parser are two translation units of the shell that
# reach no system call, so they are compiled into the kernel image as the C
# library is and for the same reason: `make verify` is the only thing here that
# can run code, and kernel/test/shell/parser.c asserts the grammar against the
# code the shell actually ships rather than a reconstruction. They are named
# apart from LIBC_SOURCES because they are not the library's — a program's
# sources compiled into the kernel is a stranger arrangement than a library's,
# and it should be visible as one — and they are compiled by a rule of their
# own that gives them the library's include root, which the kernel is denied.
# ------------------------------------------------------------------------------

SHELL_SOURCES := userland/sh/lexer.c \
                 userland/sh/expand.c \
                 userland/sh/variables.c \
                 userland/sh/parser.c

C_SOURCES := kernel/kernel.c \
             kernel/handoff/multiboot2.c \
             kernel/test/volume.c \
             kernel/test/program.c \
             kernel/test/mm/memory.c \
             kernel/test/arch/interrupts.c \
             kernel/test/arch/privilege.c \
             kernel/test/arch/syscall.c \
             kernel/test/arch/usermode.c \
             kernel/test/arch/apic.c \
             kernel/test/arch/smp.c \
             kernel/test/exec/elf.c \
             kernel/test/proc/process.c \
             kernel/test/proc/sched.c \
             kernel/test/proc/lifecycle.c \
             kernel/test/gfx/framebuffer.c \
             kernel/test/gfx/graphics.c \
             kernel/test/gfx/console.c \
             kernel/test/gfx/compositor.c \
             kernel/test/gfx/faultscreen.c \
             kernel/test/gfx/windows.c \
             kernel/test/gfx/client.c \
             kernel/test/proc/init.c \
             kernel/test/dev/devices.c \
             kernel/test/dev/mouse.c \
             kernel/test/storage/stack.c \
             kernel/test/storage/ext2.c \
             kernel/test/storage/ext2/format.c \
             kernel/test/storage/ext2/directory.c \
             kernel/test/storage/ext2/file.c \
             kernel/test/storage/ext2/write.c \
             kernel/test/storage/ext2/probe.c \
             kernel/test/storage/vfs.c \
             kernel/test/storage/initrd.c \
             kernel/test/libc/string.c \
             kernel/test/libc/wrappers.c \
             kernel/test/libc/heap.c \
             kernel/test/libc/stdio.c \
             kernel/test/libc/startup.c \
             kernel/test/libc/utilities.c \
             kernel/test/libc/line.c \
             kernel/test/libc/config.c \
             kernel/test/terminal/terminal.c \
             kernel/test/shell/parser.c \
             kernel/test/proc/directory.c \
             kernel/test/proc/signal.c \
             kernel/terminal/terminal.c \
             kernel/mm/pmm.c \
             kernel/mm/vmm.c \
             kernel/mm/heap.c \
             kernel/arch/x86_64/mm/paging.c \
             kernel/arch/x86_64/mm/shootdown.c \
             kernel/arch/x86_64/mm/addrspace.c \
             kernel/arch/x86_64/cpu/gdt.c \
             kernel/arch/x86_64/cpu/idt.c \
             kernel/arch/x86_64/cpu/tss.c \
             kernel/arch/x86_64/cpu/percpu.c \
             kernel/arch/x86_64/cpu/spinlock.c \
             kernel/arch/x86_64/interrupt/interrupts.c \
             kernel/arch/x86_64/interrupt/irq.c \
             kernel/arch/x86_64/interrupt/exceptions.c \
             kernel/arch/x86_64/syscall/syscall.c \
             kernel/arch/x86_64/syscall/sigframe.c \
             kernel/arch/x86_64/smp/ipi.c \
             kernel/arch/x86_64/smp/smp.c \
             kernel/acpi/acpi.c \
             kernel/exec/elf.c \
             kernel/proc/process.c \
             kernel/proc/sched.c \
             kernel/proc/signal.c \
             kernel/fs/ext2/core.c \
             kernel/fs/ext2/superblock.c \
             kernel/fs/ext2/group.c \
             kernel/fs/ext2/inode.c \
             kernel/fs/ext2/file.c \
             kernel/fs/ext2/alloc.c \
             kernel/fs/ext2/directory.c \
             kernel/fs/ext2/path.c \
             kernel/fs/ext2/name.c \
             kernel/fs/ext2_vfs.c \
             kernel/fs/vfs/vfs.c \
             kernel/fs/vfs/node.c \
             kernel/fs/vfs/path.c \
             kernel/fs/vfs/mount.c \
             kernel/fs/vfs/file.c \
             kernel/fs/vfs/namespace.c \
             kernel/fs/vfs/pipe.c \
             drivers/vga/vga.c \
             drivers/serial/serial.c \
             drivers/pic/pic.c \
             drivers/apic/lapic.c \
             drivers/apic/ioapic.c \
             drivers/pit/pit.c \
             drivers/ps2/ps2.c \
             drivers/keyboard/keyboard.c \
             drivers/mouse/mouse.c \
             drivers/pci/pci.c \
             drivers/ata/ata.c \
             drivers/ata/port.c \
             drivers/ata/identify.c \
             drivers/ata/channel.c \
             drivers/ata/transfer.c \
             drivers/ata/report.c \
             drivers/ahci/ahci.c \
             drivers/sdhci/sdhci.c \
             drivers/ramdisk/ramdisk.c \
             kernel/block/block.c \
             kernel/block/buffer.c \
             graphics/framebuffer.c \
             graphics/draw.c \
             graphics/font.c \
             graphics/console.c \
             graphics/compositor.c \
             graphics/faultscreen.c \
             graphics/cursor.c \
             graphics/window.c \
             graphics/client.c \
             $(LIBC_SOURCES) \
             $(SHELL_SOURCES)

ASM_SOURCES := boot/boot.asm \
               kernel/arch/x86_64/interrupt/interrupt_stubs.asm \
               kernel/arch/x86_64/cpu/gdt.asm \
               kernel/arch/x86_64/smp/smp_trampoline.asm \
               kernel/arch/x86_64/syscall/syscall_entry.asm \
               kernel/arch/x86_64/proc/switch.asm \
               kernel/test/libc/startup_image.asm \
               kernel/test/libc/utilities_image.asm \
               kernel/test/libc/line_image.asm \
               kernel/test/libc/config_image.asm \
               kernel/test/proc/directory_image.asm \
               kernel/test/proc/signal_image.asm \
               kernel/test/gfx/client_image.asm \
               kernel/test/proc/init_image.asm \
               kernel/test/shell/programs_image.asm \
               $(LIBC_ASM_SOURCES)

OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.c.o,$(C_SOURCES)) \
           $(patsubst %.asm,$(BUILD_DIR)/%.asm.o,$(ASM_SOURCES))

DEPENDENCIES := $(patsubst %.c,$(BUILD_DIR)/%.c.d,$(C_SOURCES))

# ------------------------------------------------------------------------------
# QEMU invocation.
#
# The q35 machine type and a two-core processor configuration are selected so
# that the symmetric multi-processing support of Phase 6 may be exercised from
# the earliest opportunity, as required by PROJECT_GUIDELINES.md, Section 2.
# ------------------------------------------------------------------------------

QEMU_FLAGS := -machine q35 -cpu qemu64 -smp cores=2 -m 512M
OVMF_FIRMWARE := /usr/share/ovmf/OVMF.fd

VBOX_VM_NAME := Oxys-OS

.PHONY: all iso clean run-qemu run-uefi run-vbox verify toolcheck clang-check \
        spdx-check spdx-apply docs-check lint build-record

# ------------------------------------------------------------------------------
# Principal targets.
# ------------------------------------------------------------------------------

all: $(KERNEL_ELF)

$(KERNEL_ELF): $(OBJECTS) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)
	@echo "Verifying that the image is Multiboot2 compliant."
	@grub-file --is-x86-multiboot2 $@ \
		&& echo "The image is Multiboot2 compliant." \
		|| (echo "ERROR: the image is not Multiboot2 compliant." && false)

$(BUILD_DIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -MF $(patsubst %.o,%.d,$@) -c $< -o $@

# The C library's own rule. It is chosen over the one above because make prefers
# the pattern rule with the shorter stem, and `string/copying.c` is shorter than
# `libc/string/copying.c`. The only difference is the include root: see the note
# where LIBC_SOURCES is defined for why the kernel does not get it.
$(BUILD_DIR)/libc/%.c.o: libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBC_INCLUDE_DIRS) -MMD -MP -MF $(patsubst %.o,%.d,$@) -c $< -o $@

# The self-tests that are compiled against the C library's include root, being
# the ones that assert it: the string and memory functions of sub-task 7.1, the
# system-call wrappers of 7.2 and the heap of 7.3.
#
# This was three explicit rules naming three files, and the note upon them said
# a pattern was refused because "a pattern over kernel/test/ would put every
# future self-test in reach of the userland's headers whether or not it asserted
# the userland". That objection was right about `kernel/test/` and does not
# apply here. The reorganisation gave those three files a directory **whose
# membership is the exception** — a file is in `kernel/test/libc/` precisely
# when it asserts the C library — so the pattern's scope and the exception's
# scope are now the same set, and a fourth such test is added by putting it in
# the directory rather than by remembering to add a fourth rule.
#
# Every other file under kernel/ is still compiled without <string.h> in reach,
# which is the property being protected.
$(BUILD_DIR)/kernel/test/libc/%.c.o: kernel/test/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBC_INCLUDE_DIRS) -MMD -MP -MF $(patsubst %.o,%.d,$@) -c $< -o $@

# The shell's grammar units, compiled for the kernel image with the library's
# include root, for the reason SHELL_SOURCES records; and the self-test that
# asserts them, which is in a directory of its own for the reason the one above
# is: a file is in kernel/test/shell/ exactly when it asserts the shell.
$(BUILD_DIR)/userland/%.c.o: userland/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBC_INCLUDE_DIRS) -MMD -MP -MF $(patsubst %.o,%.d,$@) -c $< -o $@

$(BUILD_DIR)/kernel/test/shell/%.c.o: kernel/test/shell/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBC_INCLUDE_DIRS) -MMD -MP -MF $(patsubst %.o,%.d,$@) -c $< -o $@

# ------------------------------------------------------------------------------
# The user-mode build of sub-task 7.5.
#
# The same C library sources are compiled a second time, with the flags a program
# requires rather than the kernel's, and collected into an archive a program links
# against. docs/design/LIBC.md, Section 7, foresaw this: it is why LIBC_SOURCES is
# a list of its own rather than merged into C_SOURCES.
#
# **The flag that had to change is -mcmodel=kernel**, and the reason is not the
# one first written here. That note said an object compiled with it "carries
# relocations that cannot be satisfied, and the linker says so"; the negative
# test that put the flag back found otherwise — the program links, boots and
# passes every assertion. The measurement is this: the model emits R_X86_64_32S
# relocations, which hold a signed 32-bit address, and a program linked at four
# mebibytes has every address well inside that range. The relocations are
# satisfiable by arithmetic accident.
#
# It is still wrong, and what it asserts is what matters. -mcmodel=kernel tells
# the compiler that every symbol lies in the topmost two gibibytes of the address
# space; that statement is false of a program at four mebibytes, and a compiler
# entitled to rely upon a false statement is a compiler entitled to any code
# generation it likes. Nothing here has yet depended upon it, and the day
# something does — a program linked above two gibibytes, a large static object,
# an optimiser that folds an address comparison — the failure arrives with
# nothing to connect it to a build flag.
#
# So the user model is the default `small`, which is a true statement about an
# image at a fixed low address. -mno-red-zone is kept, and for a different reason
# than the kernel's: the kernel forbids the red zone because an interrupt may
# arrive upon any stack at any instruction, and a program could safely use it
# here, this system having no signals. It costs a hundred and twenty-eight bytes
# of stack per frame and removes a difference between the two compilations that
# nothing needs.
#
# There is no target of its own for any of this, and that is deliberate rather
# than an omission. The programs are embedded in the kernel image — the self-test
# of sub-task 7.5 reads one from a volume it composes — so they are a dependency
# of the image exactly as build/trampoline.bin is, and are built by `make all`
# without anybody having to remember a second command. A phony target would also
# have had to be added to PROJECT_GUIDELINES.md, Section 3, which Section 7 of
# that document permits only by explicit decision of the project owner.
# ------------------------------------------------------------------------------

USER_DIR := $(BUILD_DIR)/user

# The kernel's regime, less the code model, plus the C library's include root.
# Every diagnostic flag is kept: a program built here is held to the standard the
# kernel is held to, and the first program this project compiled would otherwise
# be the first one nobody checked.
USER_CFLAGS := -std=c11 -pedantic \
               -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
               -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
               -Wall -Wextra -Werror \
               -Wshadow -Wpointer-arith -Wcast-align -Wstrict-prototypes \
               -Wmissing-prototypes -Wredundant-decls -Wwrite-strings \
               -O2 -g \
               $(INCLUDE_DIRS) $(LIBC_INCLUDE_DIRS)

# `-n` is what keeps the image small, and the measurement is recorded here
# because the first version of this line got the attribution wrong.
#
# It tells the linker not to page-align the sections within the *file*. Without
# it this program's ELF is 26,056 bytes stripped; with it, 17,832 — the
# difference being padding between segments that exists so that a demand-paged
# loader can map a file offset straight to a page, which this kernel does not do:
# kernel/exec/elf.c copies bytes into frames it allocates itself.
#
# **`-z max-page-size=0x1000` was here too and has been removed.** It was written
# on the belief that it prevents the linker aligning segments to two mebibytes,
# and the four combinations were measured: with `-T libc/user.ld` the flag
# changes the output by not one byte, because the explicit `ALIGN(4K)` in that
# script has already fixed every segment's virtual address. It would matter to a
# link that did not use this script, and this build has no such link. A flag that
# does nothing is a flag somebody will one day reason from.
USER_LDFLAGS := -n -T libc/user.ld

USER_LIBC_OBJECTS := $(patsubst %.c,$(USER_DIR)/%.c.o,$(LIBC_SOURCES)) \
                     $(patsubst %.asm,$(USER_DIR)/%.asm.o,$(LIBC_ASM_SOURCES))

USER_LIBC_ARCHIVE := $(USER_DIR)/liboxys.a
USER_CRT0         := $(USER_DIR)/crt0.o

$(USER_DIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -MF $(patsubst %.o,%.d,$@) -c $< -o $@

$(USER_DIR)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(ASFLAGS) $< -o $@

# `rcs` rather than `rc`: the index is what lets the linker pull a member in to
# satisfy a reference, and an archive without one links only if every member
# happens to be named before the reference to it.
$(USER_LIBC_ARCHIVE): $(USER_LIBC_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(USER_LIBC_OBJECTS)

$(USER_CRT0): libc/crt/crt0.asm
	@mkdir -p $(dir $@)
	$(NASM) $(ASFLAGS) $< -o $@

# The programs, and the one rule that links every one of them.
#
# There was one program at sub-task 7.5 and one rule written out for it, with a
# note saying that sub-task 7.6 would repeat the shape five times. It is not
# repeated: seven copies of a link command is seven places for a flag to be
# forgotten, and the way that fails is that one program is linked without the
# archive and the defect appears as an undefined symbol in whichever program was
# edited last. The rule is generated instead, once per name in USER_PROGRAMS.
#
# A program is a *directory* under userland/, and every `.c` file in it is the
# program's. It was `main.c` alone until sub-task 8.2, whose shell is three
# translation units — the tokeniser and the parser being compiled into the
# kernel image as well, so that the self-test may assert the grammar without
# running the shell; see SHELL_SOURCES. Adding a program is adding a name to
# the list below and a directory beside the others, and nothing else in this
# file.
#
# Within the generated rule, the archive is named *after* the program's objects,
# which is not a style choice: a linker resolves an archive's members against the
# references it has already seen, so an archive named first contributes nothing.
USER_PROGRAMS := startup-check arg-check exec-check file-check line-check dir-check env-check signal-check window-check init-check config-check echo cat ls mkdir rm touch cp rmdir wc micro head tail grep sort mv ps sh windows init shutdown session

USER_PROGRAM_SOURCES := $(foreach program,$(USER_PROGRAMS),$(wildcard userland/$(program)/*.c))
USER_PROGRAM_IMAGES  := $(foreach program,$(USER_PROGRAMS),$(USER_DIR)/$(program).elf)
USER_PROGRAM_EMBEDS  := $(foreach program,$(USER_PROGRAMS),$(USER_DIR)/$(program).embed.elf)

# $(1) is the program's name. The double dollar signs survive the first expansion
# `eval` performs and reach `make` as ordinary automatic variables; the wildcard
# is expanded once, here, so that the objects named are the sources that exist.
define USER_PROGRAM_RULE
$$(USER_DIR)/$(1).elf: $$(USER_CRT0) $(patsubst %.c,$$(USER_DIR)/%.c.o,$(wildcard userland/$(1)/*.c)) \
                       $$(USER_LIBC_ARCHIVE) libc/user.ld
	@mkdir -p $$(dir $$@)
	$$(LD) $$(USER_LDFLAGS) -o $$@ $$(USER_CRT0) $(patsubst %.c,$$(USER_DIR)/%.c.o,$(wildcard userland/$(1)/*.c)) \
		$$(USER_LIBC_ARCHIVE)
	@echo "Linked the user program $$@."
endef

$(foreach program,$(USER_PROGRAMS),$(eval $(call USER_PROGRAM_RULE,$(program))))


# The copy that is embedded in the kernel image, with every symbol and every line
# of debugging information removed.
#
# `build/user/startup-check.elf` keeps its DWARF, because that is the file a
# debugger is pointed at. The kernel image carries this one instead, and the
# difference is most of the file: the loader reads the program header table and
# the loadable segments, and a section table describing where a local variable
# lived is eighty kibibytes it will never look at. There are eight programs since
# sub-task 7.6, so the difference is the whole of that, eight times over.
#
# --strip-all rather than --strip-debug: a symbol table is for a linker and for a
# debugger, and this copy is read by neither.
$(USER_DIR)/%.embed.elf: $(USER_DIR)/%.elf
	@mkdir -p $(dir $@)
	$(OBJCOPY) --strip-all $< $@
USER_DEPENDENCIES := $(patsubst %.c,$(USER_DIR)/%.c.d,$(LIBC_SOURCES)) \
                     $(patsubst %.c,$(USER_DIR)/%.c.d,$(USER_PROGRAM_SOURCES))

# The self-test of sub-task 7.5 embeds the linked program with `incbin`, exactly
# as kernel/arch/x86_64/smp/smp_trampoline.asm embeds the real-mode trampoline, so
# the image must exist before that translation unit is assembled.
#
# The self-test of sub-task 7.6 embeds seven more by the same means, and its
# translation unit is therefore made to depend upon every one of them — written
# as a list derived from USER_PROGRAMS rather than spelled out, so that a program
# added to that list cannot be embedded stale. The self-test of sub-task 8.1
# embeds the two of its own — `line-check`, which it runs, and `sh`, which the
# ramdisk self-test compares against /bin/sh — and names them, because the
# derived list would embed every program twice.
$(BUILD_DIR)/kernel/test/libc/startup_image.asm.o: $(USER_DIR)/startup-check.embed.elf
$(BUILD_DIR)/kernel/test/libc/utilities_image.asm.o: $(USER_PROGRAM_EMBEDS)
$(BUILD_DIR)/kernel/test/libc/line_image.asm.o: $(USER_DIR)/line-check.embed.elf $(USER_DIR)/sh.embed.elf
$(BUILD_DIR)/kernel/test/libc/config_image.asm.o: $(USER_DIR)/config-check.embed.elf
$(BUILD_DIR)/kernel/test/proc/directory_image.asm.o: $(USER_DIR)/dir-check.embed.elf
$(BUILD_DIR)/kernel/test/proc/signal_image.asm.o: $(USER_DIR)/signal-check.embed.elf
$(BUILD_DIR)/kernel/test/gfx/client_image.asm.o: $(USER_DIR)/window-check.embed.elf
$(BUILD_DIR)/kernel/test/proc/init_image.asm.o: $(USER_DIR)/init-check.embed.elf
$(BUILD_DIR)/kernel/test/shell/programs_image.asm.o: $(USER_DIR)/env-check.embed.elf
$(BUILD_DIR)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(ASFLAGS) $< -o $@

# ------------------------------------------------------------------------------
# The real-mode trampoline of sub-task 6.14.
#
# It is assembled to a flat binary rather than to an object file, because the
# processor that executes it begins in real mode at a fixed low physical page and
# the addresses within it must be that page's. NASM's `org` directive states that
# origin and is available only in the flat binary format, so this cannot be an
# ordinary member of ASM_SOURCES.
#
# kernel/arch/x86_64/smp/smp_trampoline.asm embeds the result with `incbin`, and the
# dependency below is what guarantees the binary exists before that file is
# assembled. The path in the `incbin` is relative to the directory make runs in,
# which is the repository root.
# ------------------------------------------------------------------------------

TRAMPOLINE_SOURCE := boot/trampoline.asm
TRAMPOLINE_BINARY := $(BUILD_DIR)/trampoline.bin

$(TRAMPOLINE_BINARY): $(TRAMPOLINE_SOURCE)
	@mkdir -p $(dir $@)
	$(NASM) -f bin -Wall -Werror $< -o $@

$(BUILD_DIR)/kernel/arch/x86_64/smp/smp_trampoline.asm.o: $(TRAMPOLINE_BINARY)

# ------------------------------------------------------------------------------
# The initial ramdisk of sub-task 7.7.
#
# An EXT2 volume holding the five utilities under /bin, delivered to the kernel
# as a Multiboot2 module and mounted as the root. docs/storage/INITRD.md holds
# the reasoning; what follows is the part of it that concerns this file.
#
# **It is made by `mke2fs` and not by anything in this repository**, and that is
# the decision worth recording. This project already possesses a complete
# understanding of the EXT2 format — it is the whole of kernel/fs/ext2/ — so a
# composer here would have been a few hundred lines it already knows how to
# write. It would also have shared every assumption the reader makes. A volume
# this kernel composed and then read back proves the reader consistent with the
# composer and nothing else; a volume e2fsprogs composed proves the reader
# consistent with an implementation that has never seen this one. That is the
# same argument the `clang-check` target rests upon and the same one
# docs/storage/EXT2-VERIFICATION.md, Section 6, made by hand three sub-tasks
# ago — except that this runs at every boot rather than upon the day somebody
# remembers to run it.
#
# `mke2fs` is therefore a build dependency, and the first one this project has
# that is not a compiler, an assembler, a linker or an image builder. It is
# recorded in docs/project/TOOLCHAIN.md and reported by the `toolcheck` target.
# PROJECT_GUIDELINES.md, Section 3, names the five tools that must be present and
# remains true as it stands; it is not amended, Section 7 of that document
# reserving amendments to the project owner.
#
# The options are not defaults and each one is load-bearing:
#
#   -b 1024   The block size the EXT2 implementation is written against and the
#             only one the buffer cache of sub-task 4.6 holds. A 4096-byte volume
#             is readable — EXT2-VERIFICATION.md, Section 6, records one being
#             read — but 1024 is the size every self-test asserts against.
#   -r 1      Revision 1, which is what supplies the three feature words the
#             superblock reader of sub-task 5.1 refuses an unknown bit in. The
#             inode size is left to `mke2fs`, which chooses 256: the reader takes
#             it from the superblock, and a volume somebody else made is far more
#             likely to be 256 than 128, so that is the layout worth exercising.
#   -d        Populate from a directory, which is what makes this possible at all
#             without root: no loop device, no mount, no privilege.
#   -F        Do not ask. The target is a regular file and the answer is always
#             yes; a build that stops for a question is a build that hangs in CI.
#
# **The image is not bit-reproducible here, and the three lines that try are
# still worth having.** `mke2fs` draws three things from outside the source: the
# volume UUID, the seed of the directory hash, and the timestamps. `-U` and
# `-E hash_seed` fix the first two upon every version. `SOURCE_DATE_EPOCH` fixes
# the third only from e2fsprogs 1.47.1, which is where it was implemented; the
# version upon this machine is 1.47.0 and ignores it, so two builds of an
# unchanged tree made in different seconds differ in the superblock's times and
# in each inode's three times, and in nothing else. That is measured and not
# assumed: `cmp -l` between two such images reports forty-two bytes, every one of
# them within a time field, and two images built within the same second are
# identical to the byte. The variable is set
# regardless, because it costs nothing and the build becomes reproducible upon a
# host whose e2fsprogs is new enough without anybody editing this file. Its
# value is 2026-09-13, the date the build register was cleared and numbering
# restarted, which is arbitrary and had to be something.
#
# The image is truncated to its full length before `mke2fs` is run. That is not
# required — `mke2fs` creates the file — and it stops the tool printing
# "Creating regular file" into an otherwise silent build, which is a line a
# reader has to learn to ignore.
#
# The size is stated in 1024-byte blocks. It is comfortably more than the
# utilities need, because the cost of the slack is a few tens of kibibytes of an
# ISO and the cost of being short is a build that fails on the day a utility
# grows.
# ------------------------------------------------------------------------------

INITRD_IMAGE   := $(BUILD_DIR)/initrd.img
INITRD_STAGING := $(BUILD_DIR)/initrd
INITRD_BLOCKS  := 2048
INITRD_UUID    := 0c5f7a10-7b41-4d2e-9a3c-6f0c5f7a1000

# The programs the ramdisk carries, which are the five utilities of sub-task 7.6
# and the shell of sub-task 8.1, and not the five check programs beside them. A
# check program is a test's apparatus: it is embedded in the kernel image, where
# the self-test that runs it is, and a system that shipped it in /bin would be
# shipping its own test harness to somebody who asked for a shell.
INITRD_UTILITIES := echo cat ls mkdir rm touch cp rmdir wc micro head tail grep sort mv ps sh windows init shutdown session
INITRD_SOURCES   := $(foreach utility,$(INITRD_UTILITIES),$(USER_DIR)/$(utility).embed.elf)

# The `/etc` hierarchy of sub-task 9.4: the configuration `init` and the desktop
# read at start. They are files in the repository rather than text written by a
# recipe, so that the thing a person edits upon the running machine and the
# thing they edit in the source are the same file, and so that a change to one
# is a change git can show.
INITRD_CONFIGURATION := etc/system.conf etc/desktop.conf etc/session.conf

# `/mnt` is the second and last thing upon the image, and it is empty.
#
# Before sub-task 7.7 the root was whatever volume the machine carried, and the
# EXT2 write probe of sub-task 5.8 reached that volume by resolving a path from
# the root. The ramdisk takes the root, so without somewhere to put it a machine
# with a disk would lose that probe entirely — which is exactly the kind of
# capability that disappears without anybody noticing, because what it leaves
# behind is a diagnostic that no longer prints rather than a test that fails.
# The machine's own volume is mounted here instead. See kernel/kernel.c,
# KernelMountMachineVolume.

$(INITRD_IMAGE): $(INITRD_SOURCES) $(INITRD_CONFIGURATION)
	@command -v mke2fs >/dev/null \
		|| (echo "ERROR: mke2fs was not found upon the PATH, and the initial ramdisk requires it." \
		    && echo "It is supplied by e2fsprogs; see docs/project/TOOLCHAIN.md." && false)
	@rm -rf $(INITRD_STAGING)
	@mkdir -p $(INITRD_STAGING)/bin
	@mkdir -p $(INITRD_STAGING)/mnt
	@mkdir -p $(INITRD_STAGING)/etc
	@for file in $(INITRD_CONFIGURATION); do \
		cp $$file $(INITRD_STAGING)/etc/; \
		chmod 644 $(INITRD_STAGING)/etc/$$(basename $$file); \
	done
	@for utility in $(INITRD_UTILITIES); do \
		cp $(USER_DIR)/$$utility.embed.elf $(INITRD_STAGING)/bin/$$utility; \
		chmod 755 $(INITRD_STAGING)/bin/$$utility; \
	done
	@rm -f $@
	@truncate -s $$(( $(INITRD_BLOCKS) * 1024 )) $@
	@SOURCE_DATE_EPOCH=1789257600 mke2fs -q -F -t ext2 -b 1024 -r 1 \
		-U $(INITRD_UUID) -E hash_seed=$(INITRD_UUID) \
		-L oxys-initrd -d $(INITRD_STAGING) $@ $(INITRD_BLOCKS)
	@echo "The initial ramdisk has been written to $@ ($(words $(INITRD_UTILITIES)) utilities in /bin, $(words $(INITRD_CONFIGURATION)) files in /etc)."

iso: $(ISO_IMAGE)

$(ISO_IMAGE): $(KERNEL_ELF) $(INITRD_IMAGE) boot/grub/grub.cfg
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_ELF) $(ISO_DIR)/boot/oxys.elf
	cp $(INITRD_IMAGE) $(ISO_DIR)/boot/initrd.img
	cp boot/grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_DIR) 2>/dev/null
	@echo "The ISO image has been written to $@."

clean:
	rm -rf $(BUILD_DIR)

# ------------------------------------------------------------------------------
# Execution targets.
# ------------------------------------------------------------------------------

run-qemu: $(ISO_IMAGE)
	$(QEMU) $(QEMU_FLAGS) -cdrom $(ISO_IMAGE) -serial stdio

run-uefi: $(ISO_IMAGE)
	@test -f $(OVMF_FIRMWARE) \
		|| (echo "ERROR: the OVMF firmware was not found at $(OVMF_FIRMWARE)." && false)
	$(QEMU) $(QEMU_FLAGS) -bios $(OVMF_FIRMWARE) -cdrom $(ISO_IMAGE) -serial stdio

run-vbox: $(ISO_IMAGE)
	@command -v VBoxManage >/dev/null \
		|| (echo "ERROR: VBoxManage was not found upon the PATH." && false)
	-VBoxManage unregistervm "$(VBOX_VM_NAME)" --delete
	VBoxManage createvm --name "$(VBOX_VM_NAME)" --ostype Other_64 --register
	VBoxManage modifyvm "$(VBOX_VM_NAME)" --memory 512 --cpus 2 --firmware bios \
		--uart1 0x3F8 4 --uartmode1 file "$(CURDIR)/$(BUILD_DIR)/vbox-serial.log"
	VBoxManage storagectl "$(VBOX_VM_NAME)" --name "IDE" --add ide
	VBoxManage storageattach "$(VBOX_VM_NAME)" --storagectl "IDE" \
		--port 0 --device 0 --type dvddrive --medium "$(CURDIR)/$(ISO_IMAGE)"
	VBoxManage startvm "$(VBOX_VM_NAME)"

# ------------------------------------------------------------------------------
# Automated verification.
#
# The kernel is executed without a display for a bounded interval, and its serial
# output is examined. This provides a regression test that requires no operator
# observation.
#
# Two assertions are made, and both are necessary.
#
# The first is that the kernel reached the end of its initialisation, which
# catches a machine that faulted, hung or reset on the way there.
#
# The second is that no boot-time self-test reported a failure. A self-test that
# fails states so and allows the kernel to continue, there being no way to
# abandon a boot usefully and no harness to report to; so a kernel whose every
# assertion failed would still reach the banner, and an assertion upon the banner
# alone would call that a success. The self-tests are the substance of this
# project's testing, and a regression net that cannot see them fail is not one.
#
# The word is grepped for rather than each test being named, so that a self-test
# added in a later phase is covered by this target on the day it is written. The
# kernel emits FAILED in no other context; every occurrence is a verdict.
# ------------------------------------------------------------------------------

verify: $(ISO_IMAGE)
	@rm -f $(BUILD_DIR)/serial.log
	@timeout 25 $(QEMU) $(QEMU_FLAGS) -cdrom $(ISO_IMAGE) \
		-display none -serial file:$(BUILD_DIR)/serial.log \
		-no-reboot >/dev/null 2>&1 || true
	@echo "--- Captured serial output ---"
	@cat $(BUILD_DIR)/serial.log || true
	@echo "--- End of captured serial output ---"
	@grep -q "initialisation complete." $(BUILD_DIR)/serial.log \
		|| (echo "VERIFICATION FAILED: the expected banner was not observed." && false)
	@if grep -q "FAILED" $(BUILD_DIR)/serial.log; then \
		echo "VERIFICATION FAILED: a boot-time self-test reported a failure."; \
		grep -n "FAILED" $(BUILD_DIR)/serial.log; \
		false; \
	fi
	@echo "VERIFICATION SUCCEEDED: the kernel booted, reported completion, and every self-test passed."

# ------------------------------------------------------------------------------

# ------------------------------------------------------------------------------
# The corpus checks.
#
# Neither target builds anything, and neither needs the toolchain: they read the
# repository and say whether it contradicts itself. They are separate from
# `verify` because `verify` answers "does the kernel work" and these answer "does
# what is written about it still hold", and a run that conflates the two cannot
# say which of them failed.
#
#   spdx-check   Every tracked file carries the licence tag LICENSING.md,
#                Section 1, assigns to its path.
#   spdx-apply   Add the tag to the files that lack one. A mismatched tag is
#                never rewritten; see tools/spdx.sh.
#   docs-check   The claims the corpus makes about itself and about the source.
#   lint         Both of the checks, and what CI runs.
# ------------------------------------------------------------------------------

spdx-check:
	@tools/spdx.sh --check

spdx-apply:
	@tools/spdx.sh --apply

docs-check:
	@tools/check-docs.sh

lint: spdx-check docs-check
	@echo "LINT SUCCEEDED: the licence tags and the corpus agree with the source."

# ------------------------------------------------------------------------------
# The build register.
#
# One numbered row per image produced, appended to docs/project/builds.tsv —
# which is the record — after which docs/project/BUILDS.md's generated view is
# re-rendered from it. It builds nothing and runs nothing: it reads the artefacts
# in $(BUILD_DIR) and the serial log the `verify` target leaves, so it may be
# called after any target above and after a boot in an environment that leaves no
# log at all.
#
# NOTE is the one field a person supplies, and is the only one that says why the
# build was made. ENVIRONMENT, RESULT and ASSERTIONS are for a run this project
# cannot observe from the repository — under VirtualBox, or upon real hardware.
#
# ARCHIVE=1 keeps the image as well as the row. It is not the default, and the
# reason is that `clean` exists: most builds here are made, verified and thrown
# away within the hour, and archiving every one of them would fill a disk with
# images nobody will ever ask for. The images worth keeping are the ones that
# left the machine or that somebody made an observation about — which is a
# judgement, so it is a flag. docs/project/BUILDS.md, Section *What is kept*,
# records what happened when nothing was kept at all.
#
#   make build-record NOTE="sub-task 7.2, first image with the wrappers"
#   make build-record ENVIRONMENT=Bochs NOTE="the same image under Bochs"
#   BUILD_DIR=build-clang make build-record ENVIRONMENT=QEMU NOTE="by the second compiler"
#
# The register is queried with tools/builds.sh directly; there is no target for
# it, a target per query being a worse arrangement than a script with options.
# docs/project/BUILDS.md sets out the schema and why the record is a delimited
# file rather than a Markdown table or a SQLite database.
# ------------------------------------------------------------------------------

NOTE        :=
ENVIRONMENT :=
RESULT      :=
ASSERTIONS  :=
ARCHIVE     :=

build-record:
	@BUILD_DIR=$(BUILD_DIR) tools/builds.sh record \
		$(if $(ENVIRONMENT),--environment "$(ENVIRONMENT)") \
		$(if $(RESULT),--result "$(RESULT)") \
		$(if $(ASSERTIONS),--assertions "$(ASSERTIONS)") \
		$(if $(ARCHIVE),--archive) \
		$(if $(NOTE),"$(NOTE)")

# ------------------------------------------------------------------------------
# Toolchain verification.
# ------------------------------------------------------------------------------

toolcheck:
	@for tool in $(CC) $(LD) $(AR) $(NASM) $(GRUB_MKRESCUE) $(QEMU) xorriso mke2fs; do \
		if command -v $$tool >/dev/null; then \
			echo "PRESENT: $$tool"; \
		else \
			echo "ABSENT:  $$tool"; \
		fi; \
	done
	@if command -v $(CLANG) >/dev/null; then \
		echo "PRESENT: $(CLANG) (optional; the second compiler of the clang-check target)"; \
	else \
		echo "ABSENT:  $(CLANG) (optional; only the clang-check target requires it)"; \
	fi

# ------------------------------------------------------------------------------
# The second compiler.
#
# This target compiles every translation unit with clang and discards the
# objects. It does not build the kernel and is not part of `all` or of `verify`:
# it exists for the diagnostics alone.
#
# Why a second compiler is worth a target of its own. Every assertion this
# project makes about its own correctness is made by machinery this project
# wrote, against fixtures this project composed. A compiler written by other
# people, from the same standard, shares none of those assumptions — so it
# refuses different things, and what it refuses is what one toolchain has been
# quietly tolerating. Its first run found exactly one such thing: `kernel/arch/x86_64/cpu/tss.c`
# named a 32-bit register to an instruction defined upon r/m16, which GNU as had
# accepted and assembled correctly for as long as the file has existed.
# `docs/project/TESTING.md`, Section 22, records the reasoning at length.
#
# clang needs no cross-toolchain of its own: it is multi-target by construction,
# so `--target=x86_64-elf` is the whole of the configuration. Every flag of
# CFLAGS above is accepted verbatim, `-mno-80387` included, so the two compilers
# are given the same regime and any difference in what they say is a difference
# between them and not between their flags.
#
# One warning is suppressed, and this is the record the diagnostic regime above
# requires of a suppression:
#
#   -Wno-cast-align.  kernel/handoff/multiboot2.c casts the byte cursor it walks the
#   Multiboot2 tag series with to each tag's structure type, which raises the
#   required alignment from 1 to 4 or 8. clang warns upon that irrespective of
#   target; GCC does not warn upon x86. Both are right. The pointer is in fact
#   correctly aligned — Multiboot2 Specification, Section 3.6.2, requires every
#   tag to begin upon an 8-byte boundary — but that guarantee is made by the boot
#   loader and is invisible to a compiler. This is one of the two documented
#   exceptions to the no-overlay rule of docs/project/CODING-STANDARDS.md,
#   Section 7.1, and is admitted there for precisely this reason: the structure
#   is not read from a medium, its fields are naturally aligned by the
#   specification, and no byte-order decision arises. The suppression is confined
#   to this target; -Wcast-align remains in force under GCC for every other file.
# ------------------------------------------------------------------------------

CLANG        := clang
CLANG_TARGET := x86_64-elf
#   The C library's include root is added here for every unit rather than for the
#   five that need it. This target compiles and discards; it produces no image,
#   so the isolation the kernel's own rule enforces — that no kernel translation
#   unit can reach <string.h> — is enforced where it has an effect, and repeating
#   it here would mean maintaining two lists of which files are which.
CLANG_FLAGS  := --target=$(CLANG_TARGET) $(CFLAGS) $(LIBC_INCLUDE_DIRS) \
                -Wno-cast-align
#   The user programs of sub-task 7.5 are compiled too, and with a second flag
#   set rather than the one above: they are not built with -mcmodel=kernel, and
#   compiling them as though they were would be checking a translation unit this
#   project never produces. It is the same argument the target itself rests upon
#   — a compiler that shares none of the first one's assumptions refuses
#   different things — applied to the one part of the source that has two
#   compilations.
CLANG_USER_FLAGS := --target=$(CLANG_TARGET) $(USER_CFLAGS)

USER_SOURCES := $(USER_PROGRAM_SOURCES)

clang-check:
	@command -v $(CLANG) >/dev/null \
		|| (echo "ERROR: $(CLANG) was not found upon the PATH, and this target requires it." \
		    && echo "It is optional: nothing else in this Makefile uses it." && false)
	@echo "Second compiler: $$($(CLANG) --version | head -1)"
	@echo "Compiling $(words $(C_SOURCES)) kernel and library translation units, and $(words $(USER_SOURCES)) user one(s), for their diagnostics."
	@for source in $(C_SOURCES); do \
		$(CLANG) $(CLANG_FLAGS) -c $$source -o /dev/null || exit 1; \
	done
	@for source in $(USER_SOURCES); do \
		$(CLANG) $(CLANG_USER_FLAGS) -c $$source -o /dev/null || exit 1; \
	done
	@echo "CLANG CHECK SUCCEEDED: every translation unit compiles without diagnostics."

-include $(DEPENDENCIES)
-include $(USER_DEPENDENCIES)
