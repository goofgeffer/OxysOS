# Oxys-OS Bibliography

**Authority**: `PROJECT_GUIDELINES.md`, Section 6, requires that every design decision be
justified by reference to an authoritative specification, and that the
specification be cited in the code comment or design document that relies upon
it. This file is the register of those specifications.

## 1. Consulted and presently relied upon

### Multiboot2 Specification, version 2.0
Free Software Foundation. Published as part of the GNU GRUB manual.
`https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html`

Sections relied upon:

- **3.1.1**, the layout of the Multiboot2 header: the 32-bit fields `magic`,
  `architecture`, `header_length` and `checksum`, in that order, followed by the
  tags.
- **3.1.2**, the magic fields: the header magic is `0xE85250D6`; the architecture
  value `0` denotes the 32-bit protected mode of i386; the checksum is the value
  which, added to the other three magic fields, yields an unsigned 32-bit sum of
  zero.
- **3.1.3**, the general tag structure: tags are aligned upon 8-byte boundaries
  and are terminated by a tag of type `0` and size `8`.
- **3.3**, the i386 machine state at entry: `EAX` contains `0x36D76289`; `EBX`
  contains the physical address of the boot information structure; `CS` is a
  32-bit read/execute segment of base 0 and limit `0xFFFFFFFF`; the data segments
  are 32-bit read/write segments of the same base and limit; the A20 gate is
  enabled; `CR0.PG` is clear and `CR0.PE` is set; `EFLAGS.VM` and `EFLAGS.IF` are
  clear.
- **3.6.1**, the placement of the structure: the boot loader may place it
  anywhere in memory, and the operating system must avoid overwriting it until it
  has finished using it.
- **3.6.2**, the basic tag structure: the fixed part of a 32-bit total size and a
  32-bit reserved field; the common tag header of a 32-bit type and a 32-bit
  size; the rule that a tag's size excludes its trailing padding and that each
  tag begins at an 8-byte aligned address.
- **3.6.7**, the ELF-Symbols tag, type 9. Note the discrepancy recorded in
  `kernel/include/oxys/multiboot2.h`: the prose of this section and the reference
  C header in the same document disagree upon the widths of the `num`, `entsize`
  and `shndx` fields.
- **3.6.8**, the memory map tag, type 6: the `entry_size` and `entry_version`
  fields; the guarantee that `entry_size` is a multiple of eight; the entry
  layout of `base_addr`, `length`, `type` and `reserved`; the region type values,
  of which 1 denotes available memory; and the warning that the map includes the
  regions occupied by the kernel and by the boot information structure, which the
  kernel must take care not to overwrite.

Used by: `boot/boot.asm`, `kernel/kernel.c`, `linker.ld`, `boot/grub/grub.cfg`.

### Intel 64 and IA-32 Architectures Software Developer's Manual
Intel Corporation. `https://www.intel.com/sdm`

Sections relied upon:

- **Volume 1, Section 18.3**, input/output: the I/O address space comprises
  65536 individually addressable 8-bit ports.
- **Volume 2A, "CPUID"**: the ability to modify `EFLAGS.ID`, bit 21, indicates
  the availability of the instruction; leaf `0x80000001` reports Intel 64
  support in `EDX` bit 29.
- **Volume 2B, "HLT"**: the instruction halts the processor until an interrupt,
  a debug exception, a non-maskable interrupt or a reset occurs.
- **Volume 2B, "STI"**: the instruction's effect upon the interrupt flag is
  delayed by one instruction, so that an interrupt cannot be delivered until
  after the instruction following it. This is why the idiom `STI; HLT` has no
  window in which an interrupt is serviced before the processor halts, and why
  nothing may be placed between the two.
- **Volume 3A, Section 3.3.7.1**, canonical addressing: bits 63 to 47 of a linear
  address must be identical.
- **Volume 3A, Section 3.4.5 and Figure 3-8**, the segment descriptor format,
  including the `L` flag that designates a 64-bit code segment.
- **Volume 3A, Section 4.1.2 and Table 4-14**, the sequence required to enter
  IA-32e mode: enable `CR4.PAE`, set `IA32_EFER.LME`, then enable `CR0.PG`.
- **Volume 3A, Section 4.5 and Figure 4-8**, four-level paging: the structure
  hierarchy, the decomposition of a linear address into indices, and the use of
  the `PS` flag to map a 2 MiB page.
- **Volume 3A, Table 4-15**, the paging-structure entry flags `P`, `R/W`, `U/S`
  and `PS`.
- **Volume 3A, Table 4-19**, the format of a page-table entry that maps a 4-KByte
  page: bits 11:9 are Ignored, and are therefore available to software. Bit 9 is
  used to mark a page copy-on-write.
- **Volume 3A, Section 4.6**, access rights: the permissions of a translation are
  the conjunction of those held at every level of the hierarchy, which is why a
  restriction must be applied at the leaf entry and not at an intermediate one.
- **Volume 3A, Section 6.2**, exception and interrupt vectors: vectors 0 to 31
  are reserved for architecture-defined exceptions; 32 to 255 are available. This
  is the constraint that obliges the 8259A controllers to be remapped, the
  vectors 8 to 15 that the firmware leaves the master presenting lying wholly
  within the reserved range.
- **Volume 3A, Section 6.10**, the interrupt descriptor table and the IDTR
  register; the limit is one less than the size of the table in bytes.
- **Volume 3A, Section 6.14.1 and Figure 6-8**, the 64-bit mode IDT: the index is
  the vector scaled by 16, each descriptor occupying sixteen bytes; only 64-bit
  interrupt and trap gates are valid, a legacy 32-bit type generating a
  general-protection exception.
- **Volume 3A, Section 3.4.2**, the processor sets the accessed bit of a segment
  descriptor when its selector is loaded, so the table must be writable.
- **Volume 3A, Section 3.4.3**, the CS register cannot be loaded by MOV; it is
  changed by a far transfer.
- **Volume 3A, Section 3.5.1**, the GDTR, and the limit being one less than the
  size of the table.
- **Volume 3A, Section 6.5**, the classification of exceptions as faults, traps
  and aborts, and the consequence for restarting the interrupted instruction.
- **Volume 3A, Section 6.12.1 and Figure 6-4**, the stack frame the processor
  pushes: in 64-bit mode SS, RSP, RFLAGS, CS and RIP unconditionally, with RSP
  aligned to sixteen bytes beforehand.
- **Volume 3A, Section 6.13**, the error code, pushed last and padded to eight
  bytes in 64-bit mode.
- **Volume 3A, Section 2.5**, the control registers: CR0 the system control
  flags, CR2 the page-fault linear address, CR3 the paging-structure base, CR4
  the architectural extension flags.
- **Volume 3A, Section 6.13 and Figure 6-6**, the selector-form error code: EXT,
  IDT and TI flags with a 13-bit selector index; the rule that the handler must
  remove the error code before returning, IRET not popping it; and the statement
  that no error code is pushed for an exception generated by the `INT n`
  instruction or externally, even where one is normally produced.
- **Volume 3A, Section 6.15 and Figure 6-9**, the page-fault exception: the error
  code flags P, W/R, U/S, RSVD, I/D, PK and SGX; the loading of CR2 with the
  faulting linear address, and the warning that CR2 must be saved before a
  further fault can occur; and the provision that supervisor-mode writes to a
  read-only page fault only when CR0.WP is set.
- **Volume 3A, Table 6-1**, the architecturally defined exceptions, their
  mnemonics, their classification, and whether each pushes an error code.
- **Volume 2A, "IRET/IRETQ"** and **"RET"** (far form), the return from a handler
  and the far return used to reload CS.
- **Volume 2A, "LGDT/LIDT" and "SGDT/SIDT"**, the instructions that load and
  store the descriptor table registers.
- **Volume 3A, Section 4.10.4.1**, invalidation: writing CR3 invalidates every
  translation-lookaside-buffer entry associated with the current process context,
  save those for global pages.
- **Volume 3A, Section 13.1**, the enabling and state management required of the
  SSE and x87 units.

Used by: `boot/boot.asm`, `linker.ld`, `Makefile`, `kernel/include/oxys/io.h`,
`kernel/kernel.c`, `kernel/include/oxys/pic.h`, `kernel/include/oxys/pit.h`,
`docs/design/MEMORY-LAYOUT.md`, `docs/design/INTERRUPTS.md`.

### System V Application Binary Interface, AMD64 Architecture Processor Supplement
`https://gitlab.com/x86-psABIs/x86-64-ABI`

Sections relied upon:

- **Section 3.1.2**, data representation: the LP64 model.
- **Section 3.2.2**, the stack frame: the 128-byte red zone below the stack
  pointer, which is inadmissible in kernel code.
- **Section 3.2.3**, parameter passing: the first two integer arguments are
  passed in `RDI` and `RSI`.
- The ELF64 object file format.

Used by: `boot/boot.asm`, `linker.ld`, `Makefile`.

### ISO/IEC 9899:2011, Programming languages — C
International Organization for Standardization.

Sections relied upon:

- **Section 4, paragraph 6**, the freestanding execution environment and the
  headers that it must provide.
- **Section 6.7.9, paragraph 4**, the requirement that the initialiser of an
  object of static storage duration be a constant expression.
- **Sections 7.18, 7.20**, `<stdbool.h>` and `<stdint.h>`.

Used by: the whole of the C source.

### National Semiconductor PC16550D datasheet
The universal asynchronous receiver/transmitter of the IBM Personal Computer AT
and its successors.

Sections and tables relied upon:

- **Table 1**, "Summary of Registers": the bit assignments of every register.
- **Table 2**, "Register Addresses": the eight registers at consecutive offsets
  from the base address, and the overlay of the divisor latches upon offsets 0
  and 1 while the divisor latch access bit is set.
- **Table 5**, "Interrupt Control Functions": bit 0 of the interrupt
  identification register is clear while an interrupt is pending; bits 3 to 1
  identify the highest-priority pending source, being 011 the receiver line
  status, 010 received data available, 110 the character timeout, 001 the
  transmitter holding register empty and 000 the modem status; and the action
  that resets each, the transmitter interrupt being reset by reading that
  register or by writing the transmitter holding register.
- **Section "Line Control Register"**: bits 1 and 0 the word length, bit 2 the
  number of stop bits, bits 5 to 3 the parity including stick parity, bit 7 the
  divisor latch access bit.
- **Section "Line Status Register"**: bit 0 a received character, bits 1 to 4 the
  overrun, parity, framing and break conditions, bit 5 the transmitter holding
  register empty, bit 6 the transmitter wholly idle.
- **Section "Programmable Baud Generator"**: the divisor is the reference
  oscillator frequency divided by sixteen times the desired signalling rate.
- **Section "FIFO Interrupt Mode Operation"**: the transmitter first-in-first-out
  buffer holds sixteen characters, and the adapter reports it empty when it has
  room for a full complement.
- **Section "MODEM Control Register"**: bit 3 the auxiliary output OUT2, bit 4
  local loopback, in which the modem control outputs are internally connected to
  the corresponding inputs.

The datasheet is distributed as a scanned document, so the figures within it
cannot be quoted by number with confidence; the tables and sections above are
named as they are printed.

Used by: `drivers/serial/serial.c`, `kernel/include/oxys/serial.h`,
`docs/devices/SERIAL.md`.

### Intel 8259A Programmable Interrupt Controller datasheet
Intel Corporation, order number 231468-003, December 1988.
`https://pdos.csail.mit.edu/6.828/2010/readings/hardware/8259A.pdf`

Sections relied upon:

- **"INITIALIZATION COMMAND WORDS (ICWS)"**, the initialisation sequence: a write
  to the command port with bit 4 set is interpreted as ICW1 and begins the
  sequence; ICW1 bit 0 (IC4) declares that ICW4 will follow and bit 1 (SNGL)
  distinguishes a single controller from a cascaded pair; ICW2 supplies bits 7 to
  3 of the vector, the controller filling bits 2 to 0 with the request level,
  whence a vector base must be divisible by eight; ICW3 is a bit mask of the
  lines bearing slaves at the master and the cascade identity at the slave; ICW4
  bit 0 selects the 8086 mode, in which the controller presents an eight-bit
  vector rather than a `CALL` instruction.
- **The same section**, the actions ICW1 performs automatically: the edge sense
  circuit is reset, **the interrupt mask register is cleared**, IR7 is assigned
  the lowest priority, the slave mode address is set to seven, the special mask
  mode is cleared and the status read is set to the interrupt request register.
  The clearing of the mask register is why `drivers/pic/pic.c` masks every line
  after the sequence and not before it.
- **"OPERATION COMMAND WORDS (OCWS)"**, OCW1: the interrupt mask register,
  reached at the data port, a set bit withholding the corresponding line.
- **The same section**, OCW2: the R, SL and EOI bits, of which the encoding R=0,
  SL=0, EOI=1 is the non-specific end-of-interrupt, resetting the highest
  priority bit set in the in-service register. This is correct in the fully
  nested mode the controller is initialised into, in which that bit is
  necessarily the one being completed.
- **The same section**, OCW3: the RR bit selects the register subsequently read
  at the command port and the RIS bit chooses the in-service register when set
  and the interrupt request register when clear. This is the mechanism by which a
  spurious request is distinguished from a real one.

Used by: `drivers/pic/pic.c`, `kernel/include/oxys/pic.h`, `kernel/kernel.c`.

### Intel 8254 Programmable Interval Timer datasheet
Intel Corporation, order number 231164-005, September 1993.
`https://www.scs.stanford.edu/10wi-cs140/pintos/specs/8254.pdf`

Sections relied upon:

- **"Programming the 8254"**, the control word format: bits 7 and 6 (SC1, SC0)
  select the counter; bits 5 and 4 (RW1, RW0) select the read/write format, of
  which the value 11 transfers the count as two bytes with the least significant
  first; bits 3 to 1 (M2, M1, M0) select the operating mode; bit 0 selects binary
  counting when clear. The control word must precede the count, the counter using
  it to determine how many bytes to expect.
- **"Mode 2: Rate Generator"**: the counter reloads automatically upon reaching
  one, so the output is periodic without further intervention by software; and a
  count of one is illegal in this mode, the output remaining high and no
  interrupt being raised.
- **"Mode 3: Square Wave Mode"**: the count is decremented by two upon each
  clock so that the output's high and low phases are of equal duration, whence
  the mode behaves as intended only for an even count. This is why the rate
  generator is preferred for the system tick.
- **"Counter Latch Command"**: a control word whose read/write field is 00
  latches the present count into a holding register, which may then be read
  without disturbing the counting in progress. Without it the two halves of a
  sixteen-bit count would be sampled at different instants.

Used by: `drivers/pit/pit.c`, `kernel/include/oxys/pit.h`, `kernel/kernel.c`.

### IBM Personal Computer AT technical reference
International Business Machines Corporation. The system technical reference for
the IBM Personal Computer AT, which defines the peripheral complement that every
subsequent x86 machine reproduces, and whose arrangement of ports and interrupt
lines the firmware of a modern machine still presents.

Sections relied upon:

- **The interrupt controllers**: the master is decoded at I/O ports `0x20` and
  `0x21` and the slave at `0xA0` and `0xA1`; the slave's output is attached to
  the master's IR2 input, whence IR2 is unavailable as an ordinary request line.
  The firmware programmes the master to present vectors 8 to 15 and the slave
  0x70 to 0x77, which is the state in which the kernel receives the machine; the
  8259A itself holds no vector base until ICW2 is written, so these values are a
  property of the firmware and not of the device.
- **The interrupt request assignment**: IR0 is the interval timer and IR1 the
  keyboard controller.
- **The interval timer**: counter 0 is decoded at port `0x40`, counter 1 at
  `0x41`, counter 2 at `0x42` and the control register at `0x43`; counter 0's
  output is attached to IR0, counter 1's to the dynamic memory refresh request
  and counter 2's to the loudspeaker gate. The counters are driven at
  1.193182 MHz, being the 14.31818 MHz reference oscillator divided by twelve,
  that oscillator running at four times the 3.579545 MHz NTSC colour subcarrier.
  The firmware leaves counter 0 running.
- **The keyboard controller**: the 8042 is decoded at port `0x60` for data and
  `0x64` for the status register when read and the command register when written;
  status bit 0 is set while the output buffer holds a byte for the processor and
  bit 1 while the input buffer still holds one for the controller.
- **Scan code set 1**: a make code is the key's own code and the corresponding
  break code is that code with bit 7 set; a code prefixed by `0xE0` denotes one
  of the keys added after the original 84-key layout.
- **The serial adapter**: the first adapter is decoded at I/O base address
  `0x03F8` and the second at `0x02F8`; the first and third raise IRQ4 and the
  second and fourth IRQ3. The reference oscillator is 1.8432 MHz, which yields
  115200 baud at a divisor of one. The adapter's interrupt output reaches its
  request line through a buffer enabled by the auxiliary output OUT2, which the
  UART's own datasheet describes only as user-designated; an adapter whose OUT2
  is clear is therefore never heard by the interrupt controller.
- **The fixed disk adapter**: the first channel's command block is decoded at
  `0x01F0` with its control register at `0x03F6` and raises IRQ14; the second
  channel answers at `0x0170` and `0x0376` upon IRQ15.

Used by: `drivers/pic/pic.c`, `drivers/pit/pit.c`, `drivers/keyboard/keyboard.c`,
`drivers/serial/serial.c`, `kernel/include/oxys/pic.h`,
`kernel/include/oxys/pit.h`, `kernel/include/oxys/keyboard.h`,
`drivers/ata/ata.c`, `kernel/include/oxys/ata.h`, `kernel/kernel.c`.

### The 8042 controller and PS/2 device command sets
The command sets of the IBM Personal Computer AT keyboard controller and of the
devices attached to its two ports. Recorded in the IBM Personal Computer AT
technical reference and in the PS/2 hardware interface technical reference, and
reproduced consistently by every subsequent implementation.

Controller commands, written to port `0x64`:

- **0x20**, read the controller configuration byte; **0x60**, write it. The byte
  carries the first port's interrupt enable in bit 0, the second port's in bit 1,
  the first port's clock disable in bit 4, and the translation of scan code set 2
  into set 1 in bit 6.
- **0xAD** and **0xAE**, disable and enable the first device port; **0xA7**,
  disable the second.
- **0xAA**, the controller self-test, answered by `0x55` upon success. It resets
  the controller upon some implementations, discarding the configuration byte,
  which must therefore be written again afterwards.
- **0xAB**, test the first device port, answered by `0x00` upon success.

Device commands, written to port `0x60` and forwarded by the controller:

- **0xFF**, reset, answered by `0xFA` and then by `0xAA` where the device's own
  self-test passed.
- **0xF4**, enable scanning.
- The answers **0xFA**, acknowledged, and **0xFE**, send the command again.

Note that the two command sets use overlapping numbers for unrelated purposes:
`0xAA` is the controller's self-test command and also a device's report that its
own self-test passed. The port to which a byte is written is what distinguishes
them.

Used by: `drivers/keyboard/keyboard.c`, `kernel/include/oxys/keyboard.h`.

### IBM Video Graphics Array technical reference
The colour text mode 3, presenting 80 columns by 25 rows, whose frame buffer
begins at physical address `0x000B8000` and whose cells comprise a code-point
byte followed by an attribute byte.

Registers relied upon, each named as the reference prints it:

- **Miscellaneous Output Register**, written at `0x03C2` and read at `0x03CC`.
  Bit 0: "If set Color Emulation. Base Address=3Dxh else Mono Emulation. Base
  Address=3Bxh". The CRT controller pair, the input status register and the frame
  buffer all move with it, to `0x03B4`, `0x03BA` and `0x000B0000` respectively.
- **CRT Controller Registers**, addressed through an index port and a data port.
  **Cursor Start Register** (index `0x0A`): bits 0 to 4, the first scan line of
  the cursor within the character cell; bit 5, which "Turns Cursor off if set".
  **Cursor End Register** (index `0x0B`): bits 0 to 4, the last scan line; bits 5
  and 6, the cursor skew, "Delay of cursor data in character clocks".
  **Cursor Location High** and **Low Registers** (indices `0x0E` and `0x0F`):
  the upper and lower eight bits of the cursor address.
- **Attribute Controller Registers**. "The address register is read and written
  via port 3C0h. The data register is written to port 3C0h and read from port
  3C1h"; an internal flip-flop selects between the two and is returned to the
  address by a read of the **Input Status #1 Register** (`0x03DA`, or `0x03BA` in
  the monochrome configuration), "the data received is not important". Bit 7 of
  the address register is the **Palette Address Source**, which "is set to 0 to
  load color values to the registers in the internal palette. It is set to 1 for
  normal operation". In the **Attribute Mode Control Register** (index `0x10`),
  bit 3 set makes "Attribute bit 7 ... blinking", clear makes it "high
  intensity", the latter yielding sixteen background colours rather than eight.

The reference itself is not distributed in a form that can be quoted by page, and
the register descriptions above were therefore taken from the FreeVGA reference
and from the VGA register summary of the same lineage, and cross-verified against
one another before being relied upon, as Section 6 of `PROJECT_GUIDELINES.md`
requires.

Used by: `drivers/vga/vga.c`, `kernel/include/oxys/vga.h`, `boot/boot.asm`.

### ANSI X3.4-1986, Coded Character Set — 7-Bit American National Standard Code for Information Interchange
American National Standards Institute. Republished, with the same repertoire of
control characters, as ISO/IEC 646.

The definitions of the control characters relied upon by the display driver:
**BS** (`0x08`) moves the active position one character position backward;
**HT** (`0x09`) advances it to the next horizontal tabulation stop; **LF**
(`0x0A`) moves it one line down; **CR** (`0x0D`) moves it to the first character
position of the line. None of the four erases the character it moves over, which
is why erasure upon the display and upon a terminal alike is expressed as the
sequence `BS`, `SP`, `BS`.

Used by: `drivers/vga/vga.c`, `kernel/include/oxys/vga.h`, `kernel/kernel.c`.

### ECMA-48, Control Functions for Coded Character Sets
European Computer Manufacturers Association, fifth edition.

The two control sequences by which a serial terminal is told of a cursor movement
that the backspace does not itself express: **CUU** — Cursor Up, `CSI Pn A`,
which moves the active position up by Pn lines without erasing; and **CHA** —
Cursor Character Absolute, `CSI Pn G`, which moves it to column Pn of the active
line, the columns being numbered from one. They are used only by the echo loop,
and only where the display driver has carried its own cursor into the row above.

Used by: `kernel/kernel.c`.

### PCI Local Bus Specification, revision 3.0
PCI Special Interest Group.

Sections relied upon:

- **Configuration Space Access Mechanism #1**: two 32-bit I/O locations,
  CONFIG_ADDRESS at `0x0CF8` and CONFIG_DATA at `0x0CFC`. Bit 31 of the former is
  the enable flag, bits 30 to 24 are reserved, bits 23 to 16 the bus number, bits
  15 to 11 the device number, bits 10 to 8 the function number and bits 7 to 2 the
  register number; the two least significant bits are always zero, every
  configuration access being aligned to a double word.
- **Configuration Space Header**: the first sixteen bytes common to every header
  type — the vendor and device identifiers at offsets `0x00` and `0x02`, the
  command and status registers at `0x04` and `0x06`, the revision, programming
  interface, subclass and class code at `0x08` to `0x0B`, and the header type at
  `0x0E`, whose bit 7 marks a multifunction device.
- **Absence**: "When a configuration access attempts to select a device that does
  not exist, the host bridge will complete the access without error, dropping all
  data on writes and returning all ones on reads."
- **PCI-to-PCI bridge header (type 1)**: the primary, secondary and subordinate
  bus numbers at offsets `0x18`, `0x19` and `0x1A`.
- **Base Address Registers**: bit 0 clear denotes memory space and set denotes
  I/O space; for memory, bits 2 and 1 give the width, the value 2 meaning a
  64-bit address whose upper half is the following register, and bit 3 marks the
  region prefetchable. The base address is the register with its low four bits
  cleared for memory and its low two bits cleared for I/O.

The specification is not distributed publicly by the PCI SIG. The field layouts
above were taken from two independent secondary renderings of it and
cross-verified against one another before being relied upon, as Section 6 of
`PROJECT_GUIDELINES.md` requires; they agree in every particular used here.

Used by: `drivers/pci/pci.c`, `kernel/include/oxys/pci.h`.

### PCI Code and ID Assignment Specification
PCI Special Interest Group.

The base class, subclass and programming interface codes: class `0x01` mass
storage, whose subclass `0x01` is an IDE controller and `0x06` a serial ATA
controller; class `0x02` network; class `0x03` display; class `0x06` bridge,
whose subclass `0x00` is a host bridge, `0x01` an ISA bridge and `0x04` a
PCI-to-PCI bridge; class `0x0C` serial bus, whose subclass `0x05` is SMBus. For
an IDE controller, bit 0 of the programming interface denotes the primary channel
in native mode and bit 2 the secondary, each clear meaning the compatibility mode
that answers upon the legacy ports.

Used by: `drivers/pci/pci.c`.

### AT Attachment with Packet Interface (ATA/ATAPI)
ANSI INCITS, Technical Committee T13. The revisions relied upon are ATA/ATAPI-6
and later, 48-bit addressing having been introduced in the sixth.

Sections relied upon:

- **Command block registers**: at offsets 0 to 7 from the base address, the data
  register, the error register (features when written), the sector count, the
  logical block address low, mid and high registers, the device register and the
  status register (the command register when written). The alternate status
  register (the device control register when written) lies in the separate
  control block.
- **Status register**: bit 7 BSY, bit 6 DRDY, bit 5 DF — a device fault, which
  does not set ERR — bit 3 DRQ, bit 0 ERR.
- **Device control register**: bit 1 nIEN, which stops the device asserting its
  interrupt; bit 2 SRST, which when set and then cleared resets both devices upon
  the channel.
- **Timing**: the status presented by a device is not valid for 400 nanoseconds
  after a command is written or a device selected. The delay is obtained by
  reading the alternate status register, which has no side effect; an input from
  an I/O port may be assumed to take at least 30 nanoseconds.
- **Command set**, cross-verified against an independent table of opcodes in
  opcode order: `20h` READ SECTOR(S), `24h` READ SECTOR(S) EXT, `30h` WRITE
  SECTOR(S), `34h` WRITE SECTOR(S) EXT, `E7h` FLUSH CACHE, `EAh` FLUSH CACHE EXT,
  `ECh` IDENTIFY DEVICE, `A1h` IDENTIFY PACKET DEVICE.
- **Identification data**: words 10 to 19 the serial number and 27 to 46 the
  model number, each word holding two characters with the first in its high half;
  words 60 and 61 the number of sectors addressable by 28 bits; bit 10 of word 83
  the support of 48-bit addressing; words 100 to 103 the number of sectors
  addressable by 48 bits.
- **Addressing**: a sector count register of zero means the greatest count the
  mode allows — 256 for the 28-bit commands and 65536 for the 48-bit ones. The
  four most significant bits of a 28-bit address lie in the device register. The
  48-bit commands write each address and count register twice, the high-order
  byte first, the device retaining the previous content in a hidden half.
- **Signatures**: a device that declines IDENTIFY DEVICE leaves `14h` and `EBh`
  in the address mid and high registers if it is a packet device, `3Ch` and `C3h`
  if it is a serial ATA device, and zeroes if it is an ATA device that aborted
  the command.

The standard is not distributed publicly by the committee. The register and
command details above were taken from two independent secondary renderings and
cross-verified against one another before being relied upon, as Section 6 of
`PROJECT_GUIDELINES.md` requires.

Used by: `drivers/ata/ata.c`, `kernel/include/oxys/ata.h`.

### The Second Extended File System: Internal Layout
Poirier, D. `https://www.nongnu.org/ext2-doc/ext2.html`

Sections relied upon:

- **The Superblock**: it lies 1024 bytes from the start of the volume and
  occupies 1024 bytes, the first kibibyte being reserved for a boot sector; the
  field table giving the offset and width of every field, of which this kernel
  reads all those up to `s_last_mounted` at offset 136; `s_magic` at offset 56
  holding `0xEF53`; the block size being `1024 << s_log_block_size`;
  `s_first_data_block` at offset 20 being 1 upon a volume of 1024-byte blocks and
  0 upon any other.
- **Revision Levels**: revision 0 (`EXT2_GOOD_OLD_REV`) has no field for the
  inode size or the first usable inode, both being fixed; revision 1
  (`EXT2_DYNAMIC_REV`) states them at offsets 88 and 84 and adds the three
  feature fields at 92, 96 and 100.
- **Reserved Inodes**: inodes 1 to 10 are reserved, so the first inode available
  to a file upon a revision 0 volume is 11.
- **The feature fields**: a volume declaring an incompatible feature the
  implementation lacks may not be read; one declaring a read-only compatible
  feature it lacks may be read and not written; a compatible feature may be
  ignored entirely. The bits: incompatible — compression `0x0001`, file type in
  directory entries `0x0002`, journal recovery `0x0004`, journal device `0x0008`,
  meta block groups `0x0010`; read-only compatible — sparse superblocks `0x0001`,
  large files `0x0002`, binary tree directories `0x0004`.
- **`s_state`**: 1 denotes a cleanly unmounted volume and 2 one upon which errors
  were detected.
- **Byte order**: every quantity upon the volume is stored least significant byte
  first, irrespective of the machine.

Used by: `kernel/fs/ext2.c`, `kernel/include/oxys/ext2.h`.

### Linux kernel documentation, the ext4 superblock
Linux kernel source, `Documentation/filesystems/ext4/super.rst`.

Consulted as an independent statement of the superblock field offsets, the two
formats sharing the layout of every field this kernel reads. It confirms
`s_magic` at `0x38`, `s_first_ino` at `0x54`, `s_inode_size` at `0x58`,
`s_block_group_nr` at `0x5A`, the three feature fields at `0x5C`, `0x60` and
`0x64`, `s_uuid` at `0x68`, `s_volume_name` at `0x78` and `s_last_mounted` at
`0x88`, and the magic value `0xEF53`.

Used by: `kernel/fs/ext2.c`, `kernel/include/oxys/ext2.h`.

### GNU GRUB Manual
Free Software Foundation.
`https://www.gnu.org/software/grub/manual/grub/grub.html`

Sections relied upon: Section 3.4, `grub-mkrescue`; Section 6, the configuration
file; Section 16.3.16, the `multiboot2` command.

Used by: `boot/grub/grub.cfg`, `Makefile`.

## 2. To be consulted in later phases

| Specification | Phase | Subject |
| ------------- | ----- | ------- |
| PCI Local Bus Specification 3.0 | 4 | Configuration space and device enumeration. |
| ATA/ATAPI Command Set (ACS-3) | 4 | `IDENTIFY DEVICE`; 28-bit and 48-bit logical block addressing. |
| The Second Extended File System (Poirier) | 5 | Superblock, group descriptors, inodes and directories. |
| Linux kernel documentation, `filesystems/ext2.rst` | 5 | The on-disk format as presently implemented. |
| Intel SDM, Volume 2B, `SYSCALL` and `SYSRET` | 6 | The fast system-call mechanism. |
| Intel MultiProcessor Specification 1.4 | 6 | Application processor bring-up. |
| ACPI Specification 6.5 | 6, 12 | The Multiple APIC Description Table; the Root System Description Pointer. |
| VESA BIOS Extensions 3.0 | 9 | Linear framebuffer modes under legacy BIOS. |
| UEFI Specification 2.10, Section 12.9 | 9, 12 | The Graphics Output Protocol. |
| FIPS 180-4 | 10 | SHA-256. |
| FIPS 197 | 10 | The Advanced Encryption Standard. |
| NIST SP 800-38A | 10 | Modes of operation. |
| NIST SP 800-90A | 10 | Deterministic random bit generators. |
| IEEE 802.3 | 11 | The Ethernet frame format. |
| RFC 826 | 11 | The Address Resolution Protocol. |
| RFC 791 | 11 | The Internet Protocol, version 4. |
| RFC 792 | 11 | The Internet Control Message Protocol. |
| RFC 768 | 11 | The User Datagram Protocol. |
| RFC 9293 | 11 | The Transmission Control Protocol. |
| RFC 2131 | 11 | The Dynamic Host Configuration Protocol. |
| Realtek RTL8139 or Intel 8254x datasheet | 11 | The Ethernet controller. |
| UEFI Specification 2.10 | 12 | The System Table, Boot Services and Runtime Services. |
| Microsoft PE/COFF Specification | 12 | The PE32+ image format. |

## 3. Citation form

A citation in a code comment or a design document names the specification, the
section, and, where applicable, the table or figure. For example:

> Refer to Intel SDM, Volume 3A, Section 4.1.2, for the control-register sequence
> required to enter IA-32e mode.

A statement of hardware or protocol behaviour that carries no citation is to be
treated as unverified.
