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

INCLUDE_DIRS := -Ikernel/include

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

C_SOURCES := kernel/kernel.c \
             kernel/multiboot2.c \
             kernel/test/volume.c \
             kernel/test/verify_memory.c \
             kernel/test/verify_interrupts.c \
             kernel/test/verify_privilege.c \
             kernel/test/verify_syscall.c \
             kernel/test/verify_elf.c \
             kernel/test/verify_process.c \
             kernel/test/verify_usermode.c \
             kernel/test/verify_lifecycle.c \
             kernel/test/verify_framebuffer.c \
             kernel/test/verify_graphics.c \
             kernel/test/verify_console.c \
             kernel/test/verify_compositor.c \
             kernel/test/verify_faultscreen.c \
             kernel/test/verify_mouse.c \
             kernel/test/verify_devices.c \
             kernel/test/verify_storage.c \
             kernel/test/verify_ext2.c \
             kernel/test/ext2/format.c \
             kernel/test/ext2/directory.c \
             kernel/test/ext2/file.c \
             kernel/test/ext2/write.c \
             kernel/test/ext2/probe.c \
             kernel/test/verify_vfs.c \
             kernel/test/verify_apic.c \
             kernel/test/verify_smp.c \
             kernel/test/verify_sched.c \
             kernel/mm/pmm.c \
             kernel/mm/paging.c \
             kernel/mm/shootdown.c \
             kernel/mm/addrspace.c \
             kernel/mm/vmm.c \
             kernel/mm/heap.c \
             kernel/cpu/gdt.c \
             kernel/cpu/idt.c \
             kernel/cpu/tss.c \
             kernel/cpu/syscall.c \
             kernel/cpu/percpu.c \
             kernel/cpu/spinlock.c \
             kernel/cpu/ipi.c \
             kernel/cpu/smp.c \
             kernel/cpu/interrupts.c \
             kernel/cpu/irq.c \
             kernel/cpu/exceptions.c \
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
             drivers/block/block.c \
             drivers/block/buffer.c \
             graphics/framebuffer.c \
             graphics/draw.c \
             graphics/font.c \
             graphics/console.c \
             graphics/compositor.c \
             graphics/faultscreen.c \
             graphics/cursor.c

ASM_SOURCES := boot/boot.asm \
               kernel/cpu/interrupt_stubs.asm \
               kernel/cpu/gdt.asm \
               kernel/cpu/smp_trampoline.asm \
               kernel/cpu/syscall_entry.asm \
               kernel/proc/switch.asm

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

.PHONY: all iso clean run-qemu run-uefi run-vbox verify toolcheck clang-check

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
# kernel/cpu/smp_trampoline.asm embeds the result with `incbin`, and the
# dependency below is what guarantees the binary exists before that file is
# assembled. The path in the `incbin` is relative to the directory make runs in,
# which is the repository root.
# ------------------------------------------------------------------------------

TRAMPOLINE_SOURCE := boot/trampoline.asm
TRAMPOLINE_BINARY := $(BUILD_DIR)/trampoline.bin

$(TRAMPOLINE_BINARY): $(TRAMPOLINE_SOURCE)
	@mkdir -p $(dir $@)
	$(NASM) -f bin -Wall -Werror $< -o $@

$(BUILD_DIR)/kernel/cpu/smp_trampoline.asm.o: $(TRAMPOLINE_BINARY)

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
# quietly tolerating. Its first run found exactly one such thing: `kernel/cpu/tss.c`
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
#   -Wno-cast-align.  kernel/multiboot2.c casts the byte cursor it walks the
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
CLANG_FLAGS  := --target=$(CLANG_TARGET) $(CFLAGS) -Wno-cast-align

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
