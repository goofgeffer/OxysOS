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
                libc/stdio/system.c

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
             kernel/test/libc/string.c \
             kernel/test/libc/wrappers.c \
             kernel/test/libc/heap.c \
             kernel/test/libc/stdio.c \
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
             kernel/arch/x86_64/smp/ipi.c \
             kernel/arch/x86_64/smp/smp.c \
             kernel/acpi/acpi.c \
             kernel/exec/elf.c \
             kernel/proc/process.c \
             kernel/proc/sched.c \
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
             kernel/block/block.c \
             kernel/block/buffer.c \
             graphics/framebuffer.c \
             graphics/draw.c \
             graphics/font.c \
             graphics/console.c \
             graphics/compositor.c \
             graphics/faultscreen.c \
             graphics/cursor.c \
             $(LIBC_SOURCES)

ASM_SOURCES := boot/boot.asm \
               kernel/arch/x86_64/interrupt/interrupt_stubs.asm \
               kernel/arch/x86_64/cpu/gdt.asm \
               kernel/arch/x86_64/smp/smp_trampoline.asm \
               kernel/arch/x86_64/syscall/syscall_entry.asm \
               kernel/arch/x86_64/proc/switch.asm \
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

iso: $(ISO_IMAGE)

$(ISO_IMAGE): $(KERNEL_ELF) boot/grub/grub.cfg
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_ELF) $(ISO_DIR)/boot/oxys.elf
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
	@for tool in $(CC) $(LD) $(NASM) $(GRUB_MKRESCUE) $(QEMU) xorriso; do \
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

clang-check:
	@command -v $(CLANG) >/dev/null \
		|| (echo "ERROR: $(CLANG) was not found upon the PATH, and this target requires it." \
		    && echo "It is optional: nothing else in this Makefile uses it." && false)
	@echo "Second compiler: $$($(CLANG) --version | head -1)"
	@echo "Compiling $(words $(C_SOURCES)) translation units for their diagnostics."
	@for source in $(C_SOURCES); do \
		$(CLANG) $(CLANG_FLAGS) -c $$source -o /dev/null || exit 1; \
	done
	@echo "CLANG CHECK SUCCEEDED: every translation unit compiles without diagnostics."

-include $(DEPENDENCIES)
