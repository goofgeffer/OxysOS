# Oxys-OS Test Procedure

**Corresponding phase**: All phases. `PROJECT_GUIDELINES.md`, Section 2, requires every
milestone to be bootable and testable under QEMU and VirtualBox.

## 1. Automated verification

The `verify` target executes the ISO under QEMU without a display, directs the
serial port to a file, and asserts that the string `Phase 1 initialisation
complete.` appears in the captured output. It requires no operator observation
and is therefore suitable for use as a regression test after every change.

```sh
export PATH="$HOME/opt/cross/bin:$PATH"
make verify
```

The expected output at the completion of Phase 1 is:

```
Oxys-OS
Version 0.1.0, x86_64, long mode active, higher-half kernel.
Multiboot2 magic value verified.
Multiboot2 information structure at physical address 0x11E4D8.
Multiboot2 information structure total size: 0x5D8 bytes.
Phase 1 initialisation complete.
No further subsystems are implemented. Halting.
VERIFICATION SUCCEEDED: the kernel booted and reported completion.
```

The physical address and the total size of the Multiboot2 information structure
are determined by GRUB and will vary between invocations and between versions of
GRUB. Their exact values are not part of the assertion.

## 2. Interactive execution under QEMU

```sh
make run-qemu
```

The machine type `q35`, the processor model `qemu64` and the two-core
configuration are fixed in the `Makefile` so that the symmetric multi-processing
work of Phase 6 is exercised under a representative configuration from the
earliest opportunity, as `PROJECT_GUIDELINES.md`, Section 2, requires.

The VGA console is expected to present the identification banner in light cyan
upon black, followed by the status lines. The serial output is directed to the
standard output stream of the invoking terminal.

## 3. Verification of the text-mode display

The serial path and the display path are independent, and a defect in the VGA
driver would not be detected by the serial assertion alone. The display is
therefore verified by capturing a screen image through the QEMU monitor:

```sh
( sleep 10; echo "screendump /tmp/oxys.ppm"; sleep 3; echo "quit" ) \
  | qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
      -cdrom build/oxys.iso -display none -monitor stdio -serial null
```

The resulting image is 720 by 400 pixels, which is the pixel resolution of VGA
mode 3, and contains the rendered banner. This was performed at the completion of
Phase 1 and the banner was confirmed to read `Oxys-OS`.

## 4. Execution under UEFI firmware

```sh
make run-uefi
```

This target invokes QEMU with the OVMF firmware. It is expected to fail at
present, because the ISO carries only the legacy BIOS boot path of GRUB. The
target is provided in advance so that the UEFI work of Phase 12 has an
established point of entry; sub-task 12.7 will render it functional.

## 5. Execution under VirtualBox

```sh
make run-vbox
```

The target destroys any existing machine named `Oxys-OS`, creates a machine with
512 MiB of memory, two processors and legacy BIOS firmware, directs the first
serial port to `build/vbox-serial.log`, attaches the ISO to an IDE controller,
and starts the machine.

**Present status**: `VBoxManage` is not installed in this WSL2 environment, so
the target cannot be executed here. VirtualBox is a Windows host application;
execution requires either that the Windows `VBoxManage.exe` be reachable from
WSL2 or that the test be performed from a Windows command prompt against the ISO
in the WSL2 filesystem. The project owner performs this test upon the Windows
host directly, and sub-task 1.11 of `PLAN.md` is recorded as passed upon that
authority.

## 6. Testing upon physical hardware

The ISO produced by `grub-mkrescue` is a hybrid image and may be written
directly to a USB medium:

```sh
sudo dd if=build/oxys.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

The device name must be confirmed before the command is issued, since an
incorrect name will destroy the contents of the named device.

Physical testing requires a machine offering a legacy BIOS or a compatibility
support module, since the UEFI boot path is not implemented until Phase 12.
Diagnostic output should be captured through a serial adapter where the machine
provides one. Sub-task 1.12 remains open.

## 7. Debugging with GDB

QEMU provides a GDB stub. The kernel is compiled with `-g`, so the DWARF
information in `build/oxys.elf` may be used directly:

```sh
qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso -s -S &
gdb build/oxys.elf -ex 'target remote localhost:1234'
```

Note that breakpoints upon higher-half symbols cannot be serviced until paging
has been enabled. A breakpoint at `_start`, whose address is physical, is the
correct point at which to begin an examination of the boot sequence.

## 8. Test record

| Date | Test | Result |
| ---- | ---- | ------ |
| 2026-08-30 | `make all` — build with `-Wall -Wextra -Werror` | Passed; no diagnostics. |
| 2026-08-30 | `grub-file --is-x86-multiboot2` | Passed; the image is Multiboot2 compliant. |
| 2026-08-30 | `make iso` — ISO generation | Passed. |
| 2026-08-30 | `make verify` — QEMU boot and serial assertion | Passed. |
| 2026-08-30 | QEMU screendump — VGA text-mode rendering | Passed; the banner reads `Oxys-OS`. |
| — | VirtualBox boot | Passed; performed by the project owner upon the Windows host, `VBoxManage` being unavailable in this environment. |
| — | Physical hardware boot | Not performed. |
