; ==============================================================================
; File: boot/boot.asm
;
; Purpose:
;   Provides the Multiboot2 header, the 32-bit protected-mode entry point of the
;   Oxys-OS kernel, the construction of the boot-time paging hierarchy, the
;   transition from 32-bit protected mode to 64-bit long mode, and the transfer
;   of control to the higher-half C entry point KernelMain.
;
; Key routines and data:
;   _start                  - The ELF entry point, invoked by GRUB in 32-bit mode.
;   BootCheckCpuid          - Determines whether the CPUID instruction is available.
;   BootCheckLongMode       - Determines whether IA-32e mode is supported.
;   BootBuildPageTables     - Constructs the boot-time PML4, PDPT and PD hierarchy.
;   BootEnableLongMode      - Sets CR4.PAE, IA32_EFER.LME and CR0.PG.
;   BootLongModeEntry       - The 64-bit trampoline resident in identity-mapped memory.
;   KernelEntryHigh         - The 64-bit higher-half entry point.
;   BootGdt                 - The minimal 64-bit global descriptor table.
;   BootPml4/BootPdpt*/BootPd - The boot-time paging structures.
;
; References:
;   - Multiboot2 Specification 2.0, Section 3.1.1: header layout, comprising the
;     32-bit fields magic, architecture, header_length and checksum, in that
;     order, aligned on an 8-byte boundary.
;   - Multiboot2 Specification 2.0, Section 3.1.2: the header magic is
;     0xE85250D6; the architecture value 0 denotes 32-bit (protected) mode of
;     i386; the checksum is the value which, added to the other three magic
;     fields, yields a 32-bit unsigned sum of zero.
;   - Multiboot2 Specification 2.0, Section 3.1.3: tags are 8-byte aligned and
;     are terminated by a tag of type 0 and size 8.
;   - Multiboot2 Specification 2.0, Section 3.3 ("I386 machine state"): upon
;     entry EAX contains 0x36D76289, EBX contains the 32-bit physical address of
;     the Multiboot2 information structure, CS is a 32-bit read/execute segment
;     with base 0 and limit 0xFFFFFFFF, the data segments are 32-bit read/write
;     segments with base 0 and limit 0xFFFFFFFF, the A20 gate is enabled,
;     CR0.PG is clear, CR0.PE is set, and EFLAGS.VM and EFLAGS.IF are clear.
;   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2A,
;     "CPUID": the ability to set or clear EFLAGS.ID (bit 21) indicates the
;     availability of CPUID; leaf 0x80000001 returns the Intel 64 support
;     indication in EDX bit 29.
;   - Intel SDM, Volume 3A, Section 4.1.2 and Table 4-14: the sequence required
;     to enter IA-32e mode is to enable CR4.PAE (bit 5), to set IA32_EFER.LME
;     (bit 8 of MSR 0xC0000080), and then to enable CR0.PG (bit 31).
;   - Intel SDM, Volume 3A, Section 4.5: four-level paging structures, and the
;     use of the PS flag (bit 7) in a page-directory entry to map a 2 MiB page.
;   - Intel SDM, Volume 3A, Section 3.4.5 and Figure 3-8: segment descriptor
;     format, including the L flag (bit 21 of the upper doubleword) which
;     designates a 64-bit code segment.
; ==============================================================================

; ------------------------------------------------------------------------------
; Symbolic constants.
; ------------------------------------------------------------------------------

MULTIBOOT2_HEADER_MAGIC     equ 0xE85250D6  ; Multiboot2 Specification, Section 3.1.2.
MULTIBOOT2_ARCHITECTURE_I386 equ 0          ; 32-bit protected mode of i386.
MULTIBOOT2_BOOTLOADER_MAGIC equ 0x36D76289  ; Expected in EAX at entry.

MULTIBOOT2_TAG_TYPE_END     equ 0           ; Terminating tag type.
MULTIBOOT2_TAG_SIZE_END     equ 8           ; Terminating tag size, in bytes.

KERNEL_VIRTUAL_BASE         equ 0xFFFFFFFF80000000

CR4_PAE_BIT                 equ 5           ; Physical Address Extension.
CR0_PAGING_BIT              equ 31          ; Paging enable.
IA32_EFER_MSR               equ 0xC0000080  ; Extended Feature Enable Register.
IA32_EFER_LME_BIT           equ 8           ; Long Mode Enable.

CPUID_LEAF_EXTENDED_MAXIMUM equ 0x80000000
CPUID_LEAF_EXTENDED_FEATURES equ 0x80000001
CPUID_EDX_LONG_MODE_BIT     equ 29          ; Intel 64 architecture available.

EFLAGS_ID_BIT               equ 21          ; Identification flag.

PAGE_PRESENT                equ 0x001       ; Intel SDM, Volume 3A, Table 4-15.
PAGE_WRITABLE               equ 0x002
PAGE_LARGE                  equ 0x080       ; PS flag; maps a 2 MiB page.

PAGE_SIZE_LARGE             equ 0x200000    ; Two mebibytes.
PAGE_TABLE_ENTRIES          equ 512

; The higher-half kernel is mapped at 0xFFFFFFFF80000000. Decomposition of that
; linear address into paging-structure indices (Intel SDM, Volume 3A, Figure 4-8)
; yields PML4 index 511 and PDPT index 510, with a PD index of 0.
BOOT_PML4_INDEX_HIGH        equ 511
BOOT_PDPT_INDEX_HIGH        equ 510

GDT_SELECTOR_CODE           equ 0x08        ; Offset of the code descriptor.
GDT_SELECTOR_DATA           equ 0x10        ; Offset of the data descriptor.

BOOT_STACK_SIZE             equ 4096        ; Stack used prior to the higher-half switch.
KERNEL_STACK_SIZE           equ 65536       ; Stack used by KernelMain.

VGA_TEXT_BUFFER_PHYSICAL    equ 0x000B8000  ; Legacy colour text-mode frame buffer.
VGA_ERROR_ATTRIBUTE         equ 0x4F00      ; White foreground upon a red background.

; ------------------------------------------------------------------------------
; The Multiboot2 header.
;
; The header is placed in a dedicated section which the link script positions at
; the very beginning of the image, thereby satisfying the requirement that it
; reside within the first 32768 bytes and be aligned on an 8-byte boundary.
; ------------------------------------------------------------------------------

section .multiboot_header
align 8
MultibootHeaderStart:
    dd  MULTIBOOT2_HEADER_MAGIC
    dd  MULTIBOOT2_ARCHITECTURE_I386
    dd  MultibootHeaderEnd - MultibootHeaderStart
    ; The checksum is chosen such that the unsigned 32-bit sum of the four magic
    ; fields is zero. The subtraction from 2^32 performs the negation.
    dd  0x100000000 - (MULTIBOOT2_HEADER_MAGIC + MULTIBOOT2_ARCHITECTURE_I386 + (MultibootHeaderEnd - MultibootHeaderStart))

    ; The terminating tag, of type 0 and size 8, aligned on an 8-byte boundary.
align 8
    dw  MULTIBOOT2_TAG_TYPE_END
    dw  0                                   ; Flags; bit 0 (optional) is clear.
    dd  MULTIBOOT2_TAG_SIZE_END
MultibootHeaderEnd:

; ------------------------------------------------------------------------------
; The 32-bit protected-mode entry point and its supporting routines.
;
; This section is linked at its physical load address because it executes before
; paging is enabled and therefore cannot be addressed through the higher-half
; mapping.
; ------------------------------------------------------------------------------

section .boot.text progbits alloc exec nowrite align=16
bits 32

global _start

; ------------------------------------------------------------------------------
; _start
;
; The ELF entry point. GRUB transfers control here in 32-bit protected mode with
; paging disabled, in the machine state prescribed by Multiboot2 Specification,
; Section 3.3.
; ------------------------------------------------------------------------------
_start:
    cli                                     ; Interrupts are already masked; this is defensive.
    cld                                     ; Establish the forward string direction required by the ABI.

    ; A stack is not supplied by the boot loader and must be established before
    ; any procedure call is made.
    mov     esp, BootStackTop

    ; Preserve the two values supplied by the boot loader before any instruction
    ; may overwrite them.
    mov     dword [BootMultibootMagic], eax
    mov     dword [BootMultibootInformation], ebx

    ; Confirm that the image was in fact loaded by a Multiboot2-compliant loader.
    cmp     eax, MULTIBOOT2_BOOTLOADER_MAGIC
    jne     BootErrorNotMultiboot

    call    BootCheckCpuid
    call    BootCheckLongMode
    call    BootBuildPageTables
    call    BootEnableLongMode

    ; Load the 64-bit global descriptor table and perform the far jump that
    ; loads a code segment whose L flag is set, thereby entering 64-bit mode.
    lgdt    [BootGdtDescriptor]
    jmp     GDT_SELECTOR_CODE:BootLongModeEntry

; ------------------------------------------------------------------------------
; BootCheckCpuid
;
; Determines whether the CPUID instruction is supported by attempting to toggle
; EFLAGS.ID. If the bit cannot be modified, CPUID is unavailable and the boot is
; aborted.
; ------------------------------------------------------------------------------
BootCheckCpuid:
    pushfd
    pop     eax
    mov     ecx, eax                        ; Retain the original value for comparison.
    xor     eax, 1 << EFLAGS_ID_BIT
    push    eax
    popfd
    pushfd
    pop     eax
    push    ecx                             ; Restore the original EFLAGS value.
    popfd
    cmp     eax, ecx
    je      BootErrorNoCpuid
    ret

; ------------------------------------------------------------------------------
; BootCheckLongMode
;
; Determines whether the processor implements the Intel 64 architecture, by
; inspecting bit 29 of EDX as returned by CPUID leaf 0x80000001. The availability
; of that leaf is itself established first, by way of leaf 0x80000000.
; ------------------------------------------------------------------------------
BootCheckLongMode:
    mov     eax, CPUID_LEAF_EXTENDED_MAXIMUM
    cpuid
    cmp     eax, CPUID_LEAF_EXTENDED_FEATURES
    jb      BootErrorNoLongMode

    mov     eax, CPUID_LEAF_EXTENDED_FEATURES
    cpuid
    test    edx, 1 << CPUID_EDX_LONG_MODE_BIT
    jz      BootErrorNoLongMode
    ret

; ------------------------------------------------------------------------------
; BootBuildPageTables
;
; Constructs a four-level paging hierarchy that establishes two mappings of the
; first gibibyte of physical memory:
;
;   (a) An identity mapping at linear address 0x0000000000000000, which is
;       required because the instruction pointer continues to refer to low
;       addresses at the instant paging is enabled.
;   (b) A higher-half mapping at linear address 0xFFFFFFFF80000000, at which the
;       kernel proper is linked.
;
; Both mappings refer to a single page directory of 512 entries, each of which
; maps a 2 MiB page by means of the PS flag. The identity mapping is retained
; only until the permanent kernel tables are constructed in Phase 2.
; ------------------------------------------------------------------------------
BootBuildPageTables:
    ; Zero the four paging structures, which are contiguous in the boot data
    ; section. Each structure occupies 4096 bytes.
    mov     edi, BootPml4
    mov     ecx, (4 * 4096) / 4
    xor     eax, eax
    rep     stosd

    ; PML4[0] refers to the page-directory-pointer table of the identity mapping.
    mov     eax, BootPdptIdentity
    or      eax, PAGE_PRESENT | PAGE_WRITABLE
    mov     dword [BootPml4], eax

    ; PML4[511] refers to the page-directory-pointer table of the higher half.
    mov     eax, BootPdptHigher
    or      eax, PAGE_PRESENT | PAGE_WRITABLE
    mov     dword [BootPml4 + BOOT_PML4_INDEX_HIGH * 8], eax

    ; Both page-directory-pointer tables refer to the same page directory.
    mov     eax, BootPageDirectory
    or      eax, PAGE_PRESENT | PAGE_WRITABLE
    mov     dword [BootPdptIdentity], eax
    mov     dword [BootPdptHigher + BOOT_PDPT_INDEX_HIGH * 8], eax

    ; Populate the page directory with 512 large-page entries, mapping the
    ; physical range [0, 1 GiB). The upper doubleword of each entry remains zero,
    ; which is correct for physical addresses below 4 GiB.
    mov     edi, BootPageDirectory
    mov     eax, PAGE_PRESENT | PAGE_WRITABLE | PAGE_LARGE
    mov     ecx, PAGE_TABLE_ENTRIES
.FillPageDirectory:
    mov     dword [edi], eax
    add     eax, PAGE_SIZE_LARGE
    add     edi, 8
    dec     ecx
    jnz     .FillPageDirectory
    ret

; ------------------------------------------------------------------------------
; BootEnableLongMode
;
; Performs the control-register sequence prescribed by Intel SDM, Volume 3A,
; Section 4.1.2: the paging structures are installed in CR3, physical address
; extension is enabled, long mode is enabled in IA32_EFER, and paging is enabled.
; Upon return the processor is in IA-32e compatibility mode; 64-bit mode is
; entered by the subsequent far jump.
; ------------------------------------------------------------------------------
BootEnableLongMode:
    mov     eax, BootPml4
    mov     cr3, eax

    mov     eax, cr4
    or      eax, 1 << CR4_PAE_BIT
    mov     cr4, eax

    mov     ecx, IA32_EFER_MSR
    rdmsr
    or      eax, 1 << IA32_EFER_LME_BIT
    wrmsr

    mov     eax, cr0
    or      eax, 1 << CR0_PAGING_BIT
    mov     cr0, eax
    ret

; ------------------------------------------------------------------------------
; Fatal error paths.
;
; Each path writes a distinguishing code directly to the VGA text buffer and
; halts the processor. The buffer is addressed physically, which is valid because
; these paths are reachable only while paging remains disabled.
; ------------------------------------------------------------------------------
BootErrorNotMultiboot:
    mov     al, 'M'                         ; The Multiboot2 magic value was absent.
    jmp     BootFatalError
BootErrorNoCpuid:
    mov     al, 'C'                         ; The CPUID instruction is unavailable.
    jmp     BootFatalError
BootErrorNoLongMode:
    mov     al, 'L'                         ; The Intel 64 architecture is unavailable.
    jmp     BootFatalError

BootFatalError:
    movzx   eax, al
    or      eax, VGA_ERROR_ATTRIBUTE
    mov     word [VGA_TEXT_BUFFER_PHYSICAL], ax
.Halt:
    cli
    hlt
    jmp     .Halt

; ------------------------------------------------------------------------------
; BootLongModeEntry
;
; The 64-bit trampoline. It resides in identity-mapped memory because the far
; jump that reaches it is encoded with a 32-bit offset and therefore cannot name
; a higher-half address. Its sole responsibilities are to load the data segment
; selectors and to perform an indirect jump, through a 64-bit immediate, to the
; higher-half entry point.
; ------------------------------------------------------------------------------
bits 64
BootLongModeEntry:
    ; In 64-bit mode the data segment registers are largely ignored, but they are
    ; loaded with a valid descriptor so that the architectural state is defined.
    mov     ax, GDT_SELECTOR_DATA
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    mov     rax, KernelEntryHigh
    jmp     rax

; ------------------------------------------------------------------------------
; The boot data section: the global descriptor table and the paging structures.
;
; The paging structures are emitted as initialised zero data rather than being
; reserved in .bss, so that they occupy a defined physical location within the
; loaded image and require no action by the boot loader.
; ------------------------------------------------------------------------------

section .boot.data progbits alloc write align=4096
bits 32

; The minimal global descriptor table required by long mode. The base and limit
; fields of the code and data descriptors are ignored in 64-bit mode; the
; significant flags are P (present), S (descriptor type), DPL, the L flag of the
; code descriptor, and, for the data descriptor, the writable bit.
align 8
BootGdt:
    dq  0x0000000000000000                  ; The mandatory null descriptor.
    dq  0x00AF9A000000FFFF                  ; 0x08: 64-bit code, DPL 0, L set, D clear.
    dq  0x00CF92000000FFFF                  ; 0x10: data, DPL 0, writable.
BootGdtEnd:

BootGdtDescriptor:
    dw  BootGdtEnd - BootGdt - 1            ; The limit is one less than the size.
    dq  BootGdt                             ; The linear base address of the table.

; The values supplied by the boot loader in EAX and EBX, preserved for later
; consumption by KernelMain.
align 8
global BootMultibootMagic
global BootMultibootInformation
BootMultibootMagic:
    dd  0
BootMultibootInformation:
    dd  0

; The stack employed by the 32-bit boot code. It is small because the boot code
; performs no recursion and allocates nothing upon the stack.
align 16
BootStackBottom:
    times BOOT_STACK_SIZE db 0
BootStackTop:

; The boot-time paging structures. Each must be aligned on a 4096-byte boundary,
; as required by Intel SDM, Volume 3A, Section 4.5.
align 4096
BootPml4:
    times 4096 db 0
BootPdptIdentity:
    times 4096 db 0
BootPdptHigher:
    times 4096 db 0
BootPageDirectory:
    times 4096 db 0

; ------------------------------------------------------------------------------
; The higher-half 64-bit entry point.
;
; This section is linked at its higher-half virtual address and is reached only
; after paging has been enabled and the indirect jump performed.
; ------------------------------------------------------------------------------

section .text
bits 64

extern KernelMain

KernelEntryHigh:
    ; Establish the kernel stack. The System V AMD64 ABI requires that the stack
    ; pointer be aligned on a 16-byte boundary at the point of a call; the
    ; subsequent call instruction pushes eight bytes, so the pointer is aligned
    ; here to 16 bytes.
    mov     rsp, KernelStackTop

    ; Terminate the frame-pointer chain so that any future stack unwinder
    ; recognises this frame as the outermost.
    xor     rbp, rbp

    ; Marshal the arguments for KernelMain in accordance with the System V AMD64
    ; calling convention: the first integer argument in RDI, the second in RSI.
    ; The saved values reside in the identity-mapped boot data section, which
    ; remains accessible because the identity mapping has not yet been removed.
    ; Writing to the 32-bit registers zero-extends into the full 64-bit registers.
    mov     edi, dword [BootMultibootInformation]
    mov     esi, dword [BootMultibootMagic]

    call    KernelMain

    ; KernelMain is not expected to return. Should it nevertheless do so, the
    ; processor is halted permanently with interrupts masked.
.Halt:
    cli
    hlt
    jmp     .Halt

; ------------------------------------------------------------------------------
; The kernel stack, reserved in .bss. The boot loader is required to zero the
; memory that a NOBITS section occupies.
; ------------------------------------------------------------------------------

section .bss nobits alloc write align=16
KernelStackBottom:
    resb KERNEL_STACK_SIZE
KernelStackTop:
