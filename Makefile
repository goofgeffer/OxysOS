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
#   verify    - Executes the ISO under QEMU without a display and asserts that
#               the expected banner is emitted upon the serial port.
#   toolcheck - Confirms that every required tool is present.
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
# -std=c11 -pedantic    Conformance to ISO C11 as required by CLAUDE.md.
# -Wall -Wextra -Werror The diagnostic regime mandated by CLAUDE.md, Section 4.
#
# Documented exception: -Wno-unused-parameter is NOT applied. No warning is
# presently suppressed; should a suppression become necessary it must be
# recorded here together with its justification.
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
             kernel/mm/pmm.c \
             kernel/mm/paging.c \
             kernel/mm/vmm.c \
             kernel/mm/heap.c \
             drivers/vga/vga.c \
             drivers/serial/serial.c

ASM_SOURCES := boot/boot.asm

OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.c.o,$(C_SOURCES)) \
           $(patsubst %.asm,$(BUILD_DIR)/%.asm.o,$(ASM_SOURCES))

DEPENDENCIES := $(patsubst %.c,$(BUILD_DIR)/%.c.d,$(C_SOURCES))

# ------------------------------------------------------------------------------
# QEMU invocation.
#
# The q35 machine type and a two-core processor configuration are selected so
# that the symmetric multi-processing support of Phase 6 may be exercised from
# the earliest opportunity, as required by CLAUDE.md, Section 2.
# ------------------------------------------------------------------------------

QEMU_FLAGS := -machine q35 -cpu qemu64 -smp cores=2 -m 512M
OVMF_FIRMWARE := /usr/share/ovmf/OVMF.fd

VBOX_VM_NAME := Oxys-OS

.PHONY: all iso clean run-qemu run-uefi run-vbox verify toolcheck

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
# output is examined for the expected banner. This provides a regression test
# that requires no operator observation.
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
		&& echo "VERIFICATION SUCCEEDED: the kernel booted and reported completion." \
		|| (echo "VERIFICATION FAILED: the expected banner was not observed." && false)

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

-include $(DEPENDENCIES)
