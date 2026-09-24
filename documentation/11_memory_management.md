
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

## Process Memory Mapping (mmap / munmap)

`sys_mmap()` / `sys_munmap()` (`system/syscall/syscall.c`, syscalls 9 and 10 — see `13_syscalls.md`) let a process reserve and release its own virtual address ranges on top of the paging primitives described above. As of v0.6.0 both are fully implemented, subject to the constraints below.

### Anonymous, Eager Allocation

- **Anonymous only.** `flags` must include `MAP_ANONYMOUS`; file-backed mmap is not supported. `MAP_FIXED` is rejected outright — a caller-supplied address is never honoured, rather than being silently ignored.
- **Eager, not demand-paged.** Every requested page is physically allocated (`pmm_alloc_page()`) and mapped (`map_page_in_pml4()`) synchronously, inside the `sys_mmap()` call itself — not lazily on first access via a page-fault handler. The kernel's page-fault handler (vector 14, see [10_cpu_interrupts.md](10_cpu_interrupts.md#exception-handlers)) has no recovery path for a real fault: it prints CR2/register state and halts the system unconditionally. Demand paging therefore isn't feasible without building that recovery path first, so eager allocation sidesteps the need for it.
- If physical memory runs out partway through a request, every page mapped so far is rolled back via `unmap_page()` (which also frees the physical frame) and the call fails with `(uint64_t)-1` — no partially-mapped region is ever left behind.
- Because `sys_mmap()` runs entirely in ring 0 as a syscall handler, it can never be interrupted by the scheduler's preemption (which only fires for ring-3 code — see [10_cpu_interrupts.md](10_cpu_interrupts.md#preemptive-scheduling)), so it needs no extra locking while it walks and mutates the process's page tables.

### Address Space Layout

| Constant | Value | Purpose |
|----------|-------|---------|
| `MMAP_AREA_START` | `0x0000600000000000` | Fixed virtual base of the mmap area (`process.h`) |
| `MAX_MMAP_REGIONS` | 32 | Max simultaneously-live mmap regions per process |

`MMAP_AREA_START` is the same virtual address for every process, which is safe because each process has its own private page table — identical virtual addresses in different processes never physically collide. Its PML4 index (`0x600000000000 / 2^39 = 192`) falls inside the 0–255 range that `process_fork()`/`clone_address_space_deep()` already copies wholesale, and sits well away from both ELF code (`0x400000` upward) and `USER_STACK_AREA_START` (PML4 index 224).

Each `process_t` has its own bump-pointer cursor, `next_mmap_addr`, initialised to `MMAP_AREA_START` and advanced by the aligned length of every successful `mmap()`. **This cursor is never rewound by `munmap()`** — freeing a region only unmaps its pages and clears its tracking slot; the address range itself is not returned to a free list for reuse. A process that repeatedly `mmap()`s and `munmap()`s in a loop will eventually exhaust its mmap address range, even though the underlying physical memory is correctly freed each time. This is a known, documented limitation rather than an oversight.

### Region Tracking

Each `process_t` carries a fixed array of regions:

| Field | Type | Description |
|-------|------|--------------|
| `mmap_regions[MAX_MMAP_REGIONS]` | `mmap_region_t[]` | One entry per live mapping; `length == 0` marks a free slot |
| `mmap_region_t.addr` | `uint64_t` | Base virtual address returned by `mmap()` |
| `mmap_region_t.length` | `uint64_t` | Page-aligned length of the region |

`sys_munmap(addr, length)` requires an **exact match** against a previously-returned `(addr, length)` pair (the requested length is page-aligned the same way `mmap()` aligns it, before comparing). There is no partial or sub-range unmapping — unlike POSIX `munmap()`, a caller cannot free just a prefix, suffix, or middle slice of a larger mapping. If no tracked region matches exactly, the call fails with `(uint64_t)-1`.

Freeing needs no separate physical-memory bookkeeping step: `unmap_page()` (`system/mm/paging.c`) already frees the underlying physical frame (`pmm_free_page()`) as part of clearing each PTE, so `sys_munmap()` simply calls it once per page in the region.

### Protection Flags

| Flag | Value | Effect on the mapped page table entry |
|------|-------|------------------------------|
| `PROT_READ` | 1 | Implicit — every mapped page is `PAGE_PRESENT \| PAGE_USER` regardless of flags |
| `PROT_WRITE` | 2 | Sets `PAGE_WRITE` |
| `PROT_EXEC` | 4 | *Absence* sets `PAGE_NX` — execute access is opt-in; omitting `PROT_EXEC` makes the region non-executable |

`PAGE_NX` enforcement ultimately depends on `EFER.NXE` being enabled, but no boot-time code in this kernel explicitly sets that bit. `syscall_init()` (`system/syscall/syscall.c`) reads `EFER` and ORs in only bit 0 (`SCE`, to enable the `syscall`/`sysret` instructions) before writing it back — every other bit, including `NXE` (bit 11), is left exactly as the UEFI firmware set it at boot, with no assertion or fallback if firmware left it clear. `PAGE_NX`'s enforcement is therefore inherited from firmware state, not independently guaranteed by the kernel.

### Zero-Fill Guarantee

Every freshly mapped page is explicitly zeroed (`memset((void*)virt, 0, PAGE_SIZE)`) immediately after mapping and before `mmap()` returns. The source comment ties this directly to POSIX anonymous-mapping semantics and to a specific consumer: the userspace `malloc()` in `libc/src/malloc.c` relies on this guarantee and deliberately does not re-zero memory it receives from `sys_mmap()`.

`06_libraries.md` currently documents only the kernel-side `kernel/lib/` utilities and does not yet cover the userspace `libc/` tree, so no cross-reference to it is given here.

### A Fixed Bug: Missing `PAGE_USER` on Intermediate Page Tables

`get_or_create_table()` (`system/mm/paging.c`), used by both `map_page()` and `map_page_in_pml4()` — and therefore by every mapping path that goes through them, including `sys_mmap()` and `allocate_user_stack()` — previously created new intermediate PDPT/PD/PT tables with only `PAGE_PRESENT | PAGE_WRITE`, never `PAGE_USER`.

This was a real, silent bug: x86-64 ANDs the user/supervisor bit across *every* translation level (PML4E, PDPTE, PDE, and PTE). A supervisor-only entry at any intermediate level makes the entire address supervisor-only no matter what the leaf PTE itself requests. As a result, any freshly-mapped page reached through a newly-created intermediate table was silently kernel-only-accessible, regardless of the leaf's own `PAGE_USER` flag — a page fault waiting to happen on the first ring-3 access to that region.

The fix sets `PAGE_USER` on intermediate tables too, both when a table is newly created and (idempotently) whenever an existing intermediate table is reused for a new mapping. This does not weaken kernel-only mappings: allowing `PAGE_USER` at the intermediate levels only relaxes an upper bound on access — actual access control is still enforced by the *leaf* PTE's own flags, and kernel-only mappings simply never request `PAGE_USER` at the leaf level, so they remain inaccessible from ring 3 regardless of what the intermediate tables permit.

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| PMM | BootInfo (Memory Map) | Physical memory tracking |
| Paging | PMM | Page table allocation |
| Heap | Paging, PMM | Pre-mapping heap pages |
| mmap/munmap | Paging, PMM, Process Manager | Anonymous user-space memory mapping |

---

## Conclusion

The memory management subsystem provides a solid foundation for kernel and process memory management. The PMM tracks physical memory, the paging system provides virtual memory, and the heap allocator provides dynamic memory for the kernel. Together, they form the core of LufiraOS memory management.

For more details, refer to the source code in `system/mm/`.

---

**Document Version:** 2.0  
**Last Updated:** September 2026  
**Project:** LufiraOS