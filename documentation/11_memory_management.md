
### Initialisation

The PMM is initialised by `pmm_init()`:

1. **Find Maximum Physical Address** – scans the memory map for `EfiConventionalMemory` entries to determine the highest usable address.
2. **Allocate Bitmap Space** – finds the largest free block to store the bitmap.
3. **Initialise the Bitmap** – marks all pages as used, then clears bits for free pages.
4. **Reserve Critical Pages**:
   - Low memory (0–1 MiB) – essential for boot and BIOS.
   - Identity mapping area (first 2 MiB).
   - The bitmap itself.
   - The kernel image.
5. **Count Used Pages** – for statistics and debugging.

**Reserved Memory Regions:**

| Region | Address Range | Purpose |
|--------|---------------|---------|
| Low Memory | 0 – 1 MiB | Boot and BIOS |
| Identity Area | 0 – 2 MiB | Identity mapping |
| Bitmap | Variable | Memory tracking |
| Kernel | Variable | Kernel image |

### Allocation and Deallocation

**Allocation:** `pmm_alloc_page()`

1. Scan from `next_free_page` to `total_pages`.
2. Find the first page with a cleared bit.
3. Set the bit and increment `used_pages`.
4. Update `next_free_page` for faster future allocations.
5. Return the physical address (`page * PAGE_SIZE`).

**Deallocation:** `pmm_free_page(phys)`

1. Calculate the page number (`phys / PAGE_SIZE`).
2. Clear the bit in the bitmap.
3. Decrement `used_pages`.

**Error Handling:** `pmm_alloc_page()` returns `0` if no free pages are available.

---

## Paging System

The paging system uses 4-level paging (PML4, PDPT, PD, PT) with 4 KiB pages. Identity mapping is used for all physical memory.

### Page Table Hierarchy

| Level | Structure | Description |
|-------|-----------|-------------|
| 1 | PML4 (Page Map Level 4) | Top-level table, 512 entries, covers 512 GiB each. |
| 2 | PDPT (Page Directory Pointer Table) | 512 entries, covers 1 GiB each. |
| 3 | PD (Page Directory) | 512 entries, covers 2 MiB each (huge page) or points to PT. |
| 4 | PT (Page Table) | 512 entries, covers 4 KiB each. |

**Page Entry Flags:**

| Flag | Value | Description |
|------|-------|-------------|
| `PAGE_PRESENT` | 0x001 | Page is present in memory. |
| `PAGE_WRITE` | 0x002 | Page is writable. |
| `PAGE_USER` | 0x004 | Page is accessible from user mode. |
| `PAGE_HUGE` | 0x080 | 2 MiB huge page (PD level). |
| `PAGE_NX` | 0x8000000000000000 | No Execute bit. |

### Identity Mapping

During initialisation, `paging_init()` sets up identity mapping for all physical memory:

1. Create a PML4 and a PDPT table.
2. For each 1 GiB region up to the maximum physical memory:
   - Allocate a PD table.
   - Fill it with 2 MiB huge page entries covering the region.
3. Load the PML4 address into CR3.

**Benefits of Identity Mapping:**
- Simple to implement.
- No need to handle page faults during early initialisation.
- Physical and virtual addresses are the same for kernel code and data.

**Kernel Space Layout:**

| Region | Start Address | End Address | Size |
|--------|---------------|-------------|------|
| Heap | `0xFFFF900000000000` | `+ 16 MiB` | 16 MiB |
| Kernel Stacks | `0xFFFF880000000000` | `+ (MAX_PROCESSES * 16 KiB)` | ~512 KiB |

### Mapping Functions

**Map a Page:** `map_page(virt, phys, flags)`

1. Get the current PML4 from CR3.
2. Traverse the page tables, creating missing tables as needed.
3. If a huge page is encountered, split it into 4 KiB pages.
4. Set the PTE with the physical address and flags.
5. Invalidate the TLB entry with `invlpg`.

**Map in a Specific PML4:** `map_page_in_pml4(pml4_phys, virt, phys, flags)`

Same as `map_page()` but operates on a specified PML4 (used for process address spaces).

**Synchronise Kernel Mappings:** `sync_kernel_mappings(dest_pml4, src_pml4)`

Copies all kernel-space mappings (PML4 entries 256-511) from the source PML4 to the destination PML4. This ensures all processes have the same kernel mappings.

---

## Kernel Heap

The kernel heap provides dynamic memory allocation for the kernel and is located in a dedicated virtual memory region.

### Allocator Design

The heap uses a **first-fit linked list** allocator:

- **First-fit:** Searches for the first free block that is large enough.
- **Splitting:** Large free blocks are split into smaller blocks.
- **Merging:** Adjacent free blocks are merged when memory is freed.

### Block Structure

Each block has a header:

| Field | Type | Description |
|-------|------|-------------|
| `magic` | `uint32_t` | Magic number (`0xDEADBEE1` for free, `0xDEADBEE2` for used). |
| `size` | `uint32_t` | Size of the block (including the header). |
| `next` | `void*` | Pointer to the next block in the list. |
| `prev` | `void*` | Pointer to the previous block in the list. |



### Initialisation

The heap is initialised by `heap_init()`:

1. **Pre-map Heap Pages** – allocate physical pages and map them to the heap region.
2. **Create the Initial Free Block** – a single block covering the entire heap.
3. **Mark as Initialised** – `heap_initialized = 1`.

**Heap Region:**
- **Start:** `KERNEL_HEAP_START` (`0xFFFF900000000000`)
- **End:** `KERNEL_HEAP_START + KERNEL_HEAP_SIZE`
- **Size:** 16 MiB

### Allocation and Deallocation

**Allocation:** `kmalloc(size)`

1. Align the size to 8 bytes.
2. Add the header size.
3. Disable interrupts (for thread safety).
4. Find a free block that is large enough.
5. Split the block if it is significantly larger than needed.
6. Mark the block as used.
7. Re-enable interrupts.
8. Return a pointer to the user data area.

**Deallocation:** `kfree(ptr)`

1. Calculate the block header from the pointer.
2. Disable interrupts.
3. Verify the magic number (check for double-free or corruption).
4. Mark the block as free.
5. Merge with adjacent free blocks.
6. Re-enable interrupts.

**Error Handling:**
- `kmalloc()` returns `NULL` if no suitable block is found.
- `kfree()` checks the magic number and logs an error if it is invalid.

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| PMM | BootInfo (Memory Map) | Physical memory tracking |
| Paging | PMM | Page table allocation |
| Heap | Paging, PMM | Pre-mapping heap pages |

---

## Conclusion

The memory management subsystem provides a solid foundation for kernel and process memory management. The PMM tracks physical memory, the paging system provides virtual memory, and the heap allocator provides dynamic memory for the kernel. Together, they form the core of LufiraOS memory management.

For more details, refer to the source code in `system/mm/`.

---

**Document Version:** 1.0  
**Last Updated:** September 2026  
**Project:** LufiraOS