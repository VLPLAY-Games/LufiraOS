# LufiraOS Kernel Initialisation

This document provides comprehensive documentation for the kernel entry point, initialisation sequence, and the main execution loop of the LufiraOS operating system.

---

## Table of Contents

1. [Overview](#overview)
2. [Entry Point](#entry-point)
3. [Initialisation Sequence](#initialisation-sequence)
4. [Detailed Initialisation Steps](#detailed-initialisation-steps)
   - [1. Console Initialisation](#1-console-initialisation)
   - [2. GDT Initialisation](#2-gdt-initialisation)
   - [3. TSS Initialisation](#3-tss-initialisation)
   - [4. IDT Initialisation](#4-idt-initialisation)
   - [5. PIC Remapping](#5-pic-remapping)
   - [6. Physical Memory Manager (PMM)](#6-physical-memory-manager-pmm)
   - [7. Paging](#7-paging)
   - [8. Heap Initialisation](#8-heap-initialisation)
   - [9. FAT Filesystem](#9-fat-filesystem)
   - [10. ACPI](#10-acpi)
   - [11. Process Manager](#11-process-manager)
   - [12. PIT Timer](#12-pit-timer)
   - [13. System Calls](#13-system-calls)
   - [14. VFS](#14-vfs)
   - [15. Keyboard Driver](#15-keyboard-driver)
   - [16. Mouse Driver](#16-mouse-driver)
   - [17. PCI Bus](#17-pci-bus)
   - [18. AC'97 Audio](#18-ac97-audio)
   - [19. Enable Interrupts and IRQs](#19-enable-interrupts-and-irqs)
   - [20. Create Shell Process](#20-create-shell-process)
5. [Main Loop (Idle Loop)](#main-loop-idle-loop)
6. [Test System Call Process](#test-system-call-process)
7. [Boot Info Structure](#boot-info-structure)
8. [Dependencies](#dependencies)
9. [Error Handling](#error-handling)
10. [Future Extensions](#future-extensions)

---

## Overview

The kernel initialisation process is the heart of LufiraOS. It sets up all subsystems in a carefully ordered sequence, from the most fundamental (CPU and memory management) to the highest-level (drivers, filesystem, and user interface). The initialisation is designed to be modular, with clear separation between components, and includes comprehensive logging for debugging.

**Key Characteristics:**
- **Monolithic Design** – all subsystems are initialised in a single boot process.
- **Early Console** – the graphical console is available from the very beginning.
- **Interrupt Management** – interrupts are disabled during initialisation and enabled only when the system is ready.
- **Comprehensive Logging** – all steps use coloured log macros (LOG_PENDING, LOG_DONE_OK, etc.).
- **Cooperative Scheduling** – the scheduler is started after all subsystems are ready.

---

## Entry Point

The kernel entry point is `_start`, defined with a special section attribute that ensures the function is placed at the very beginning of the text section, making it the first code executed after the bootloader transfers control.

**Parameters:**
- A pointer to the `BootInfo` structure filled by the bootloader (see BootInfo Documentation).

**First Actions:**
1. Disable interrupts to ensure no interruptions occur during initialisation.
2. Initialise the console to set up the framebuffer and colour palette.
3. Begin logging using the coloured logging macros for status updates.

---

## Initialisation Sequence

The kernel performs the following steps in strict order:

1. Console Initialisation
2. GDT Initialisation
3. TSS Initialisation
4. IDT Initialisation
5. PIC Remapping
6. Physical Memory Manager (PMM)
7. Paging
8. Heap Initialisation
9. FAT Filesystem
10. ACPI
11. Process Manager
12. PIT Timer
13. System Calls
14. VFS
15. Keyboard Driver
16. Mouse Driver
17. PCI Bus
18. AC'97 Audio
19. Enable Interrupts and IRQs
20. Create Shell Process
21. Enter Main Loop

---

## Detailed Initialisation Steps

### 1. Console Initialisation

**Function:** initialize_console()

**Location:** drivers/console/console.c

**Purpose:** Sets up the graphical console using the framebuffer information from the bootloader.

**What it does:**
- Stores framebuffer parameters (base address, size, resolution, pixel format).
- Initialises the 256-colour palette (16 standard VGA colours + 6×6×6 RGB cube + 24 grays).
- Calculates the console grid size based on the 8×8 font.
- Clears the screen to black.
- Initialises the scrollback buffer with 256 lines.

**Why early?** The console is needed for all subsequent logging and user interaction.

---

### 2. GDT Initialisation

**Function:** gdt_init()

**Location:** system/cpu/gdt.c

**Purpose:** Sets up the Global Descriptor Table, which is required for segmentation in long mode.

**GDT Entries (8 total):**
- Entry 0: Null descriptor (required by x86_64)
- Entry 1: Kernel code (ring 0, executable, 64-bit) – selector 0x08
- Entry 2: Kernel data (ring 0, writable) – selector 0x10
- Entries 3-4: TSS descriptor (64-bit TSS, two slots) – selector 0x18
- Entry 5: User data (ring 3, writable) – selector 0x2B
- Entry 6: User code (ring 3, executable, 64-bit) – selector 0x33
- Entry 7: User data2 (for stack segment after sysretq) – selector 0x3B

**What it does:**
- Sets up each entry with appropriate base, limit, and flags.
- Loads the GDT using the lgdt instruction.
- Reloads all segment registers (ds, es, ss, fs, gs).
- Performs a far jump to reload cs.

---

### 3. TSS Initialisation

**Function:** tss_init()

**Location:** system/cpu/tss.c

**Purpose:** Initialises the Task State Segment, which provides the stack pointer for ring 0 transitions.

**TSS Structure:**
- The TSS contains the stack pointers for privilege levels 0, 1, and 2.
- It also contains interrupt stack table entries and the I/O map base address.

**What it does:**
- Zeroes the entire TSS structure.
- Sets the I/O map base to point past the TSS to disable I/O port access.
- Calls gdt_set_tss() to install the TSS descriptor in the GDT.
- Loads the TR register with the TSS selector using the ltr instruction.

---

### 4. IDT Initialisation

**Function:** idt_init()

**Location:** system/cpu/idt.c

**Purpose:** Sets up the Interrupt Descriptor Table for exception handling and hardware interrupts.

**IDT Structure (256 entries):**
- Vectors 0–31: CPU exceptions (divide by zero, page fault, general protection fault, etc.)
- Vectors 32–47: Hardware interrupts (IRQs from the PIC)
- Vectors 48–255: Reserved / unused (point to a default stub)

**What it does:**
- Sets up each entry with the appropriate handler address.
- Configures interrupt gates with the kernel code segment selector and appropriate attributes.
- Loads the IDT using the lidt instruction.

**Exception Handlers:**
- All exceptions have dedicated stubs in assembly.
- A common handler receives an interrupt frame with all registers.
- Page faults print CR2 and detailed error code flags.

---

### 5. PIC Remapping

**Function:** pic_remap()

**Location:** kernel.c

**Purpose:** Remaps the Programmable Interrupt Controller (PIC) from the default vectors to safe vectors to avoid conflicts with CPU exceptions.

**Initialisation Commands:**
1. Send initialisation command to both master and slave PICs.
2. Send vector offsets: master → 0x20, slave → 0x28.
3. Send cascade configuration: master has slave at IRQ2, slave is in cascade mode.
4. Send 8086 mode configuration.
5. Mask all interrupts by writing 0xFF to both data ports.

**Result:**
- IRQ0 (timer) → IDT vector 0x20
- IRQ1 (keyboard) → IDT vector 0x21
- IRQ12 (mouse) → IDT vector 0x2C

---

### 6. Physical Memory Manager (PMM)

**Function:** pmm_init()

**Location:** system/mm/pmm.c

**Purpose:** Tracks free physical memory pages using a bitmap.

**Process:**
1. Find the maximum physical address by scanning the memory map for conventional memory entries.
2. Allocate space for the bitmap by choosing the largest free block.
3. Initialise the bitmap by marking all pages as used, then clearing bits for free pages.
4. Reserve critical pages:
   - Low memory (0–1 MB)
   - Identity mapping area (first 2 MB)
   - The bitmap itself
   - The kernel image
5. Count used pages for statistics.

**Result:** The PMM is ready to allocate and free physical pages.

---

### 7. Paging

**Function:** paging_init()

**Location:** system/mm/paging.c

**Purpose:** Sets up 4-level paging with identity mapping for all physical memory.

**Memory Layout:**
- Kernel space: Upper half of the address space
- Identity mapping: Physical memory is mapped 1:1 in the lower half
- Heap: A dedicated region at a fixed virtual address (16 MiB)
- Kernel stacks: A dedicated region for each process (16 KiB per process)

**Process:**
1. Create PML4 and PDPT tables.
2. For each 1 GB region up to the maximum physical memory:
   - Allocate a Page Directory (PD) table.
   - Fill it with 2 MiB huge page entries.
3. Load the PML4 address into CR3.
4. Update CR3 to activate the new page tables.

**Result:** All physical memory is identity-mapped, and paging is active.

---

### 8. Heap Initialisation

**Function:** heap_init()

**Location:** system/mm/heap.c

**Purpose:** Initialises the kernel heap allocator.

**Heap Region:**
- Start: A fixed virtual address in the kernel's upper half
- Size: 16 MiB
- End: Start address plus size

**Process:**
1. Pre-map heap pages by allocating physical pages and mapping them to the heap region.
2. Create an initial free block covering the entire heap.
3. Mark the heap as initialised.

**Allocator Algorithm:**
- First-fit: Searches for the first block large enough.
- Splitting: If a block is larger than needed, it is split.
- Merging: Adjacent free blocks are merged when memory is freed.
- Protection: Interrupts are disabled during heap operations for thread safety.

---

### 9. FAT Filesystem

**Function:** fat_init()

**Location:** fs/fat/fat.c

**Purpose:** Mounts the FAT filesystem image loaded by the bootloader.

**Process:**
1. Parse the boot sector (BPB) to determine:
   - FAT type (12, 16, or 32)
   - Sector size and cluster size
   - FAT location and size
   - Root directory location
2. Initialise the dirty sector bitmap for tracking modified sectors.
3. Store the image pointer and filesystem state for later use.

**Result:** The filesystem is ready for file operations.

---

### 10. ACPI

**Function:** acpi_init()

**Location:** system/acpi/acpi.c

**Purpose:** Initialises ACPI for power management.

**Process:**
1. Validate the RSDP signature and checksum.
2. Select RSDT or XSDT based on the revision.
3. Scan for the FADT table (signatures FACP or FADT).
4. Enable ACPI mode if SMI_CMD and ACPI_ENABLE are present:
   - Write ACPI_ENABLE to the SMI command port.
   - Poll the PM1a control register for the SCI_EN bit.
5. Store the FADT pointer for later use (shutdown).

**Result:** ACPI is ready for system shutdown.

---

### 11. Process Manager

**Function:** process_init()

**Location:** system/process/process.c

**Purpose:** Initialises the process management subsystem.

**Process:**
1. Save the kernel CR3 value.
2. Create the idle process (PID 0, name "idle"):
   - State: READY
   - Uses the current address space (kernel CR3)
   - Entry point: idle_thread (enables interrupts and halts)
3. Set up the circular process list.
4. Set the current process to the idle process.

**Result:** The scheduler is ready to manage processes.

---

### 12. PIT Timer

**Function:** pit_init()

**Location:** system/timer/pit.c

**Purpose:** Initialises the Programmable Interval Timer for system ticks.

**Configuration:**
- Frequency: 100 Hz (10 ms per tick)
- Mode: Square wave (mode 3)
- Channel: Channel 0
- Divisor: 1193180 / 100 = 11931

**Process:**
1. Set the command byte to select channel 0, access mode, square wave mode, and binary mode.
2. Write the divisor (low byte, then high byte) to channel 0.

**Result:** IRQ0 fires 100 times per second.

---

### 13. System Calls

**Function:** syscall_init()

**Location:** system/syscall/syscall.c

**Purpose:** Enables the syscall instruction for fast system calls.

**MSR Configuration:**
- IA32_STAR: Sets kernel and user code segment selectors.
- IA32_LSTAR: Sets the address of the syscall entry stub.
- IA32_FMASK: Clears the interrupt flag on entry.
- IA32_EFER: Enables the System Call Enable bit.

**Result:** User-mode programs can use the syscall instruction.

---

### 14. VFS

**Function:** vfs_init()

**Location:** fs/vfs/vfs.c

**Purpose:** Initialises the Virtual Filesystem layer.

**Process:**
1. Initialise the global file table.
2. Set up per-process file descriptor tables.
3. Open standard file descriptors:
   - stdin (fd 0) → console device
   - stdout (fd 1) → console device
   - stderr (fd 2) → console device

**Result:** The VFS is ready for file operations.

---

### 15. Keyboard Driver

**Function:** keyboard_init()

**Location:** drivers/keyboard/keyboard.c

**Purpose:** Initialises the PS/2 keyboard.

**Process:**
1. Reset the keyboard controller.
2. Enable keyboard interrupts.
3. Perform a self-test.
4. Send a reset command and wait for acknowledgement.

**Result:** The keyboard is ready, and IRQ1 is enabled later.

---

### 16. Mouse Driver

**Function:** mouse_init()

**Location:** drivers/mouse/mouse.c

**Purpose:** Initialises the PS/2 mouse.

**Process:**
1. Disable mouse and keyboard to avoid interference.
2. Read and update the controller configuration byte.
3. Re-enable mouse and keyboard.
4. Send reset command and wait for acknowledgement.
5. Send default settings, set sample rate, and enable the mouse.

**Result:** The mouse is ready, and IRQ12 is enabled later.

---

### 17. PCI Bus

**Function:** pci_init()

**Location:** drivers/pci/pci.c

**Purpose:** Enumerates all devices on the PCI bus.

**Process:**
1. Scan all 256 buses, up to 32 devices per bus.
2. For each function, read vendor/device ID and class/subclass.
3. Store device information in the PCI device list.
4. Print a list of found devices for debugging.

**Result:** The PCI device list is populated.

---

### 18. AC'97 Audio

**Function:** ac97_init()

**Location:** drivers/sound/ac97.c

**Purpose:** Initialises the AC'97 audio controller.

**Process:**
1. Find the controller via PCI (class 0x04, subclass 0x01).
2. Read BAR0 (NAM) and BAR1 (NABM) I/O addresses.
3. Perform a controller reset (cold reset).
4. Initialise the codec:
   - Read vendor ID
   - Enable Variable Rate Audio
   - Set sample rate and volume
5. Allocate DMA buffers (BDL + 32 DMA pages).
6. Set default volume (80%) and sample rate (48 kHz).

**Result:** Audio is ready for playback.

---

### 19. Enable Interrupts and IRQs

After all subsystems are initialised, the kernel enables interrupts and specific IRQ lines:

1. Enable the CPU interrupt flag.
2. Enable IRQ0 (timer) on the PIC.
3. Enable IRQ1 (keyboard) on the PIC.
4. Enable IRQ2 (PIC cascade line) for slave interrupts.
5. Enable IRQ12 (mouse) on the PIC.

**Why IRQ2?** IRQ2 is the cascade line from the slave PIC; it must be enabled for slave IRQs (8–15) to work.

---

### 20. Create Shell Process

**Function:** process_create()

**Location:** system/process/process.c

**Purpose:** Creates the user shell as a separate process.

**Shell Task:**
- Shows the prompt with current working directory.
- Draws the cursor.
- Enters an infinite loop that enables interrupts, halts the CPU, disables interrupts, and calls the scheduler.

**Result:** The shell runs as a user process and is scheduled by the kernel.

---

## Main Loop (Idle Loop)

After all initialisation, the kernel enters the idle loop:

1. Enable interrupts (so timer ticks can wake the CPU).
2. Halt the CPU until the next interrupt.
3. Disable interrupts before calling the scheduler.
4. Run the scheduler, which selects the next process to run.

If no other process is ready, the idle process runs, which also enables interrupts and halts the CPU.

---

## Test System Call Process

A test process is defined to demonstrate system calls. It performs:
- A write system call to output a message.
- A getpid system call to retrieve the process ID.
- A gettick system call to retrieve timer ticks.
- An exit system call to terminate the process.

This process is not created by default but can be used for testing.

---

## Boot Info Structure

The BootInfo structure is passed from the bootloader and contains:
- Framebuffer information (base address, size, resolution, pixel format)
- Memory information (total memory, memory map, descriptor size)
- Kernel information (base address, size)
- System table addresses (ACPI RSDP, SMBIOS)
- FAT image information (base address, size)

For more details, see the BootInfo Documentation.

---

## Dependencies

| Subsystem | Depends On |
|-----------|------------|
| Console | BootInfo (framebuffer) |
| PMM | BootInfo (memory map) |
| Paging | PMM |
| Heap | Paging, PMM |
| FAT | BootInfo (FAT image) |
| ACPI | BootInfo (RSDP address) |
| VFS | FAT |
| Process Manager | GDT, TSS, PMM, Paging, Heap |
| System Calls | Process Manager, VFS, PIT |
| Drivers | PCI, PMM, Console |

---

## Error Handling

- **PMM**: If bitmap allocation fails, the kernel halts with an infinite loop.
- **Paging**: If a page table cannot be allocated, the kernel halts.
- **Heap**: If the heap cannot be mapped, the kernel halts.
- **FAT**: If mounting fails, a warning is printed, but the kernel continues.
- **ACPI**: If initialisation fails, a warning is printed, but the kernel continues.
- **Drivers**: Most drivers continue even if they fail (mouse, audio, etc.).

---

## Conclusion

The LufiraOS kernel initialisation is a carefully orchestrated sequence that sets up all subsystems in a logical order. From the console to the shell, each component builds on the previous one, creating a solid foundation for the operating system.

For more details, refer to the source code in kernel.c and the individual subsystem documentation.

---

**Document Version:** 1.0  
**Last Updated:** September 2026  
**Project:** LufiraOS