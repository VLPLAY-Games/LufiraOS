# Changelog

All notable changes to LufiraOS are documented in this file.

## [0.9.5] - 2026-10-23

A userspace-drivers release: networking and sound move out of the kernel binary
entirely, into two ordinary (if privileged) userspace daemons, `netd` and
`soundd`, running on top of a new PCI/MMIO/port-I/O/IRQ/DMA privilege model
that stops any root process from poking hardware directly. Alongside that
move: `LufiraOS-Builder` gained a driver-selection UI, a batch of real
security findings got closed (signal delivery, raw USB block access, WM
registration hijacking, out-of-bounds FAT/ext2/exFAT cluster/block access,
unsalted 32-bit password hashing), and four independent fixes landed that
don't depend on any of the above: unbounded package-download RAM usage,
a virtual-address leak in `mmap`/`munmap`, predictable TCP sequence
numbers/DNS transaction IDs, and the first piece of a partial-repaint API
for the GUI.

### Added

- **Userspace driver infrastructure** (`kernel/system/driver/driver.c`/`.h`,
  syscalls 77–88, 91) — a process must first *claim* a specific PCI device
  (`SYS_DRIVER_CLAIM_DEVICE`/`_BY_CLASS`) before any PCI config space, port
  I/O, MMIO mapping, or IRQ syscall is allowed, and only within the bounds of
  that one device — replacing the previous "any root process can touch any
  hardware" model with one scoped to the one trusted daemon that actually
  claimed a device. Also added: `SYS_MMAP_MMIO` (with a new `PAGE_MMIO_NOFREE`
  PTE bit, so process teardown never hands a device's own physical memory
  back to the PMM), `SYS_DMA_ALLOC` (physically-contiguous pages with the
  physical address handed back to the caller), `SYS_IRQ_REGISTER`/`_WAIT`
  (a real blocking wait on a hardware interrupt line, mirroring
  `mailbox_recv()`), and `SYS_SHM_CREATE`/`_ATTACH` (named shared memory for
  bulk IPC too large for the 96-byte mailbox — Ethernet frames, PCM). See
  [`13_syscalls.md`](documentation/13_syscalls.md#hardware--sound--network-6776)
  and [`07_drivers.md`](documentation/07_drivers.md).
- **Sound moved to userspace** (`lufira-packages/daemons/soundd.c`) — a full
  port of the AC'97 and Intel HDA drivers onto the new driver-claim syscalls,
  with the HDA player rewritten around a continuous ring buffer (the same
  design AC'97 already used), fixing Doom's HDA audio being reduced to
  beeps. `SYS_SOUND_PLAY_PCM` carries PCM data through a kernel-resident ring
  buffer (too large for the mailbox); `SYS_SOUND_INFO`/`SET_VOLUME`/`STOP`
  relay through a fixed `service_call()` (see Fixed, below). The kernel no
  longer contains any sound driver code at all — it spawns `/bin/soundd.elf`
  at boot instead.
- **Networking moved to userspace** (`lufira-packages/daemons/netd/`) — the
  entire Ethernet/ARP/IP/ICMP/TCP/UDP/DHCP/DNS/TLS/HTTP stack, its crypto
  (SHA-256/HMAC/AES-GCM/X25519/bignum/RSA), and all three NIC drivers
  (RTL8139/e1000/e1000e), ported onto the driver-claim syscalls as one
  single-threaded daemon. `SYS_NET_FETCH`/`SYS_PING`/`SYS_NET_CONFIG_*` now
  relay to it; `SYS_NET_FETCH`'s multi-megabyte response body goes through a
  kernel-brokered shared-memory region (`MAX_SHARED_REGION_PAGES` raised to
  4096 / 16 MiB to fit it) rather than the mailbox. The kernel no longer
  contains any network driver or protocol-stack code at all — it spawns
  `/bin/netd.elf` at boot instead.
- **Streaming package downloads** — `SYS_NET_FETCH_OPEN`/`_READ`/`_CLOSE`
  split the previous one-shot `SYS_NET_FETCH` into open-headers/read-a-chunk
  ×N/close, keeping the TCP/TLS connection alive across calls so repeated
  reads don't each pay for a new handshake. `dlpg install`/`update` now
  stream a `.lpg` straight to disk in 128 KiB chunks instead of buffering the
  whole file in RAM, removing the old 2 MiB `PACKAGE_FETCH_CAP` that would
  otherwise need raising again for the next large game package. See
  [`17_package_manager.md`](documentation/17_package_manager.md).
- **`mmap`/`munmap` virtual-address reuse** — a per-process free-list
  (`mmap_free_ranges[]`, first-fit with splitting) now reclaims the address
  range of every `munmap()`'d region for reuse by `mmap`/`mmap_mmio`/
  `dma_alloc`/`shm_create`/`shm_attach`, instead of the previous bump
  allocator that only ever grew forward and could exhaust a long-lived
  process's address space under repeated map/unmap cycles.
- **Randomized TCP initial sequence numbers and DNS transaction IDs/source
  ports** — a shared entropy-mixing module (`netd/netrand.c`, the same
  rdtsc+PIT-tick+counter→SHA-256 technique `tls.c` already used for its own
  randomness) replaces the previous predictable incrementing counter (TCP
  ISN) and linear transaction-ID/port derivation (DNS), closing an
  off-path packet-injection weakness.
- **Partial-dirty hint for the GUI** (`SYS_WIN_MARK_DIRTY`, syscall 96;
  `WM_OP_WIN_MARK_DIRTY` on the wire) plus a `dirty` flag on every widget in
  `gui_widgets.c` (button/textbox/checkbox/listbox), set by the library's own
  mutators and cleared at the end of `_draw()`. Advisory only — `_draw()`
  still always redraws unconditionally, so the existing
  "fill background then redraw everything" pattern used by `calc`/`notepad`
  keeps working — the mechanism itself lands in 0.9.5; migrating client apps
  to actually skip unnecessary redraws is left for 1.0. See
  [`18_gui_wm.md`](documentation/18_gui_wm.md).
- **`LufiraOS-Builder` driver-selection UI** — a "Userspace drivers" section
  (CLI: `--driver NAME`/`--no-drivers`; GUI: a checkbox per daemon) picks
  which of `netd`/`soundd` get built into an image, independent of the
  existing `--only-package`/`--no-default-packages` controls.

### Fixed

- **`SYS_KILL` sent a signal to any process from any process** — a
  non-root caller could `kill()` a pid it didn't own. Now requires root or a
  matching `uid`, the same ownership model files already have.
- **`SYS_USB_READ` let any process read raw blocks off a USB device** —
  including a device mounted elsewhere under someone else's permissions,
  bypassing the filesystem permission model entirely. Made root-only, like
  `SYS_USB_WRITE` already was.
- **`SYS_WM_REGISTER` could be hijacked** — any unprivileged process that won
  the race to call it before the real `wm.elf` started became the system's
  window manager, intercepting every other client's input and drawing.
  Root-only now, matching the model already used by `SYS_NETD_REGISTER`/
  `SYS_SOUNDD_REGISTER`.
- **No bounds checking on FAT/ext2/exFAT cluster and block pointers read
  from disk** — a corrupted or crafted filesystem image could drive cluster-
  chain or block-group lookups arbitrarily far past the end of the mounted
  image/metadata buffers. Added upper-bound checks to FAT's
  `get_fat_entry()`/`set_fat_entry()`, exFAT's `get_fat()`/`set_fat()`/
  `free_cluster()`/raw cluster reads (plus a `fat_length_bytes` sanity check
  at mount time), and ext2's block/group/inode-pointer helpers (plus
  `s_blocks_count`/`s_inode_size` checks at mount time, falling back to a
  zeroed trap buffer instead of reading/writing out of bounds).
- **Passwords hashed with unsalted 32-bit FNV-1a** — trivially reversible
  offline in well under a second. Replaced with salted SHA-256 (the same
  implementation `netd`'s TLS client uses), ported into the kernel at
  `kernel/lib/sha256.c`. `/etc/passwd`'s hash field grew from 8 to 64 hex
  digits; the seed accounts were rehashed to match. See
  [`15_users_permissions.md`](documentation/15_users_permissions.md#password-hashing).
- **`pci_config_read32()`/`pci_config_write32()` ran without interrupts
  disabled** — a suspected contributor to intermittent NVMe/xHCI hangs on
  real hardware (a config-space access interleaved with an interrupt handler
  touching the same `0xCF8`/`0xCFC` ports). Now wrapped in `cli`/`sti`;
  `pci_enable_interrupts()` was also added alongside it.

## [0.9.0] - 2026-10-17

Mostly a storage/hardware-support release: two new disk controllers (AHCI,
NVMe), three new filesystem drivers (ext2/ext4, exFAT, RAMFS), Intel HDA
audio, legacy BIOS boot alongside UEFI, a second Intel NIC family
(e1000/e1000e) with DHCP, and real FPU/XMM support in the libc — the last of
which is what makes the release's other headline possible: a port of
**Doom** (`doomgeneric`), playable from the desktop with sound. The GUI
builder (`LufiraOS-Builder`) grew a hardware-selection UI (which audio/net/
disk device to build for) and pins fresh checkouts to the latest released
tag instead of whatever commit HEAD happens to be. A long tail of
reliability fixes — audio latency, an HDA verb timeout, a log-interleaving
race, AHCI/NVMe boot stalls, a real-hardware input hang — came out of
actually exercising all of the above.

### Added

- **AHCI and NVMe disk drivers** (`kernel/drivers/disk/ahci.c`, `nvme.c`),
  alongside the existing legacy ATA PIO path — `--disk-mode ahci|nvme|ide`
  in `LufiraOS-Builder`.
- **RAMFS** (`kernel/fs/ramfs/`) and **ext2/ext4** (`kernel/fs/ext2/`) and
  **exFAT** (`kernel/fs/exfat/`) filesystem drivers, alongside LufiraFS and
  FAT.
- **Intel HDA audio driver** (`kernel/drivers/sound/hda.c`), alongside the
  existing AC'97 driver — CORB/RIRB command rings, DMA playback. Builder
  gained an `--audio ac97|hda` choice.
- **Legacy BIOS boot**, alongside UEFI (`boot_legacy/`: `stage1.asm`/
  `stage2.asm`) — same kernel image, a second boot path for firmware
  without UEFI support.
- **USB hotplug** — `xhci_poll_hotplug()` detects device attach/detach after
  boot, not just devices present at `xhci_init()` time.
- **FPU/XMM support in the libc** — `libc/src/math.c` is the first libc
  source file built *without* `-mgeneral-regs-only` (real `double` needs
  the SysV ABI's XMM registers); the kernel already saved/restored the
  FXSAVE area on every context switch, so this was safe to turn on. What
  `doomgeneric`'s fixed-point-averse renderer needed to run at all.
- **Serial UART (16550)**, **HPET**, and **CMOS RTC** drivers
  (`kernel/drivers/serial/`, `kernel/system/timer/hpet.c`, `rtc.c`).
- **Intel e1000/e1000e NIC drivers**, alongside RTL8139, plus a **DHCP**
  client (`kernel/net/dhcp.c`) — networking no longer requires a static IP.
- **Doom** — a `doomgeneric` port (`games/doom/`) with sound, launched as a
  fullscreen GUI app from the desktop (`wm.c`'s `launch_fullscreen_app()`
  hands the WM registration to the game process and takes it back on exit);
  installable via `dlpg` from a new "games" package category.
- **Mounted USB/FAT drives no longer read the whole device into RAM up
  front** for exFAT; the same streaming change landed for mounts in
  `lufira-packages`' `mount` command.
- **Settings app** (`lufira-packages`) gained an **About** tab (OS name,
  version, build date, developer) and a scrollable installed-packages list
  (previously clipped past a handful of entries).
- **Per-package kernel-version requirements** — `lufira-packages`'
  `index.json` now declares which kernel version(s) each package needs;
  `dlpg` and `LufiraOS-Builder` both check it before installing/building.
- **`dlpg`'s installed-package table grew from 32 to 64 entries**
  (`MAX_INSTALLED`), ahead of the ~49-package repository size.
- **`ifconfig`/`wget`/`beep`/`music`/`ping`/`mixer`** shell commands,
  previously dropped during the 0.8.0 userspace-split, restored in
  `lufira-packages`.
- **`LufiraOS-Builder` GUI**: per-component hardware pickers (AC'97 vs HDA,
  which NIC, which disk controller); a fresh clone now pins itself to the
  latest released tag of `LufiraOS`/`lufira-packages` instead of building
  whatever commit HEAD happens to be; Arch Linux OVMF firmware paths
  (`edk2-ovmf` package) added to the auto-detection list.

### Fixed

- **AC'97 audio had a noticeable playback delay.** The ring-buffer producer
  could run arbitrarily far ahead of the hardware `CIV` register; capped
  how far ahead it's allowed to get (`AC97_STREAM_MAX_LOOKAHEAD_PAGES`),
  bounding worst-case latency instead of only guarding against a full-wrap
  collision.
- **Debug-mode boot log occasionally printed two lines merged into one.**
  Timer-IRQ-driven log calls (from `usb_poll()`) could interleave with the
  kernel's own foreground `printf()` — neither had interrupt-disable
  protection around the shared cursor state. Fixed with the same
  save-EFLAGS/cli/restore pattern already used by the heap and console
  write locks.
- **AHCI/NVMe boot was much slower than IDE, especially visible as
  text-rendering stalls during Debug-mode init; NVMe sometimes failed to
  initialize at all** (`[NVMe] Namespace LBA size unsupported`, falling
  back to legacy ATA PIO). Three compounding causes: fixed-iteration busy
  waits that are far too slow under QEMU/TCG emulation (replaced with a
  fast-spin-then-PIT-calibrated-wait hybrid in `ahci.c`/`nvme.c`/`disk.c`);
  a false-positive legacy-ATA device-presence check (QEMU's "pc" chipset
  always instantiates an inert PIIX IDE controller, so the old floating-bus
  test reported "present" even with no real drive — replaced with the
  OSDev-standard register-latch probe); and `lufirafs_sync()`'s dirty-block
  set growing unboundedly once writes started failing, re-walked on every
  `klog()` call. NVMe's own Identify-Namespace read could also observe a
  torn DMA payload (an impossible `lbads==0`) despite a successful
  completion — now retried a few times instead of accepted at face value.
- **Debug mode printed a stale syscall count** (`27 system calls
  registered`, later `45`, while 65+ were actually registered) — no single
  source of truth for the total, just a hand-maintained number next to the
  registration code.
- **Real hardware: boot hung completely during mouse initialization and
  reported "keyboard not found."** `mouse_init()`'s byte-wait loops were
  unbounded busy-waits — harmless in QEMU (no PS/2 mouse response ever
  comes, but something always eventually answers), fatal on hardware with
  no physical PS/2 mouse wired up at all. Bounded the same way the PS/2
  keyboard path already was.
- **`LufiraOS-Builder`'s GUI package list was empty on a fresh clone**
  (worked only with a local `lufira-packages` checkout alongside it) — it
  now falls back to fetching `index.json` from GitHub, the same source
  `dlpg sync` uses.
- **`SYS_NET_FETCH` stalled the whole system for the duration of a fetch.**
  The network stack's internal blocking waits (`tcp.c`, `dns.c`) used
  `pit_wait_ms(1)` — a `hlt` in the current process with no call into the
  scheduler — instead of `process_sleep(1)`, so nothing else got a
  timeslice until the fetch finished or timed out. Switched to
  `process_sleep(1)` throughout; `dlpg sync`/`upgrade` and `wget` no longer
  freeze the rest of the system while they run.

### Known Issues

- **WM partial-repaint (dirty-rect compositing) is implemented but not
  fully reliable.** `do_win_fill`/`do_win_draw_rect`/`do_win_draw_text` now
  scope the present step to the changed window region instead of the whole
  screen, but interacting with some programs still behaves like a full
  redraw in practice — not resolved, needs further investigation.
- **Memory usage grows with repeated open/close of GUI programs**,
  especially noticeable when resizing a window repeatedly — looks like a
  leak somewhere in the WM or window-buffer lifecycle, not yet
  root-caused.
- **Doom's audio over HDA is reduced to short beeps, not continuous
  playback.**
- **NVMe is still occasionally flaky**: boot sometimes stalls around
  `[XHCI] Controller ready`, or the system hangs at a random point later —
  intermittent, looks like a race condition specific to NVMe disk mode,
  not yet root-caused.
- **Real hardware: boot hangs right after "AHCI SATA mode enabled."**
  Untested-on-real-hardware status continues to apply broadly — see below.
- **TLS has no certificate chain validation** and no strong entropy source
  (unchanged from 0.8.0).
- **Mounted FAT drives are still root-directory-only** (exFAT mounts no
  longer read the whole device into RAM up front — see Added — but FAT
  mounts still do).
- All prior Known Issues not superseded above still apply.

## [0.8.0] - 2026-10-08

The largest release so far: a package manager, a GUI with a userspace window
manager, and a real network stack (DNS/TLS/HTTPS) — plus two critical bugs
found and fixed only once those features were tested end-to-end over a real
network. The shell and every userspace program now live in their own
repository, [lufira-packages](https://github.com/VLPLAY-Games/lufira-packages),
built/staged by a third repo, [LufiraOS-Builder](https://github.com/VLPLAY-Games/LufiraOS-Builder);
this repository is kernel + bootloader only from this release on.

### Added

- **Package manager.** `.lpg` format (`tools/lpg_format.h`: header, dependency
  table, file table) plus `dlpg` (install/update/list/remove, dependency
  version checks, `/etc/packages/` bookkeeping) — see `lufira-packages`.
- **`dlpg sync`/`dlpg upgrade`** — fetch the package index and newer `.lpg`s
  straight from `lufira-packages`' published `index.json`/`release/` on
  GitHub, over the new network stack below. `LufiraOS-Builder` does the same
  by default now (prebuilt packages, sha256-verified, `index.json`'s `"lpg"`
  field is a real download URL) instead of requiring a local source build;
  `--build-packages-from-source` keeps the old path. `LufiraOS-Builder` also
  auto-clones a missing `LufiraOS` checkout.
- **GUI + userspace window manager.** New generic per-process mailbox IPC
  (`SYS_IPC_SEND`/`RECV`, `kernel/system/ipc/mailbox.c`) and a GUI syscall
  surface (`SYS_WIN_*`, `SYS_WM_REGISTER`, `SYS_FB_*`) — the compositor
  itself (`wm.c`: z-order, drag, window resize/maximize/minimize, a taskbar
  with a Start menu (Exit GUI/Shutdown/Reboot), desktop launcher icons,
  dirty-rectangle compositing for responsive cursor movement) is an ordinary
  ring-3 client of these syscalls, not kernel code. GUI apps (terminal —
  wraps a real shell behind two pipes, notepad, file manager, calculator,
  system info) in `lufira-packages`.
- **Dynamic linking.** Packages now link against one shared `/lib/libc.so`
  (`kernel/system/elf/dynlink.c`, ET_DYN) instead of a static copy each.
- **Real userspace shell**, replacing the old kernel-native one — a ring-3
  ELF (`lufira-packages/shell/shell.c`) reading `/dev/console` via blocking
  `SYS_READ`, `fork`/`exec`/`wait` for every command.
- **Mounted USB/FAT drives are now real VFS citizens.** `SYS_MOUNT` wires a
  mounted prefix (e.g. `/mnt/usb0`) straight into the normal
  `open`/`read`/`write`/`mkdir`/`unlink`/`readdir` path (`kernel/fs/fat/
  fat_mount.c`) — ordinary `cat`/`cp`/`ls`/`rm` just work on it, no more
  separate `mountls`/`mountcat`/`mountwrite`. Writes sync to the USB device
  immediately, not just on `unmount`. Resolves a limitation called out as
  deferred in 0.6.5. Still root-directory-only per mount (no subdirectory
  traversal on the flash drive) and the whole device is still read into RAM
  up front — see Known Issues.
- **UDP + DNS resolver** (`kernel/net/udp.c`, `dns.c`) — networking no longer
  requires literal IP addresses.
- **TLS 1.2 client + HTTPS**, from scratch (`kernel/net/tls.c`,
  `kernel/net/crypto/`: SHA-256, HMAC, AES-128-GCM, X25519, bignum/RSA) —
  ECDHE X25519 key exchange, AES-128-GCM records, SNI, RSA PKCS#1v1.5
  verification of the server's ServerKeyExchange signature against its own
  certificate. **Does not validate the certificate chain against a root CA
  store** — defeats a naive on-path attacker, not a CA-capable one. No
  hardware RNG either (handshake randomness is `rdtsc`+PIT through SHA-256),
  so no real forward secrecy. Good enough for fetching public package files
  over SLIRP, not a general-purpose secure channel yet.
- **`SYS_NET_FETCH` (66)** — one syscall, `http_fetch()` underneath: resolves
  the host, connects (TCP or TLS for `https://`), sends the request, parses
  the response (`Content-Length` or chunked), writes the body straight into
  the caller's buffer. `dlpg sync`/`upgrade` are its only caller today.
- **`SYS_DUP2` (65)** — redirect a child's stdio; used by the terminal app to
  wrap a real `shell.elf` behind two pipes instead of reimplementing a shell.
- QEMU KVM acceleration in `LufiraOS-Builder` (`-machine pc,accel=kvm:tcg`,
  falls back to software emulation automatically).

### Fixed

Three latent bugs in code that predates this release, each sitting
unnoticed until new 0.8.0 functionality finally exercised it hard enough to
expose it — genuine regressions against 0.6.5 behavior, not new-feature
teething problems:

- **USB HID mouse input silently dropped.** Boot-protocol mice send 3-byte
  reports against an 8-byte requested transfer, producing a legitimate Short
  Packet completion that `xhci.c` was treating as failure — the mouse simply
  didn't move. Present since the HID driver shipped in 0.6.0.
- **Pipes could deadlock instead of short-reading.** `pipe_read()` waited to
  fill the entire requested buffer instead of returning as soon as any data
  arrived, violating normal pipe semantics; harmless until a reader asked
  for more than a writer was sending at once (the new GUI terminal, reading
  the shell's small prompt writes, was the first to hit it in practice).
- **TCP silently dropped almost all received data.** The receive path kept
  only a single one-segment buffer, so any burst of back-to-back segments
  (how every response longer than one MSS arrives) kept just the first and
  dropped the rest, recoverable only by the peer's retransmission timeout —
  present since TCP shipped in 0.6.5. Fine for `wget`'s generous 10-second
  no-new-data timeout, fatal for TLS's much tighter per-read budget.
  Replaced with a real receive queue and an honest advertised window, so the
  sender itself never sends more than there's room for.

### Known Issues

- **TLS has no certificate chain validation** (see Added, above) and no
  strong entropy source — treat it as "keeps a passive listener out", not as
  a hardened channel.
- **`SYS_NET_FETCH` stalls the whole system for the duration of the
  fetch** — same synchronous model as `wget` always had; a big download
  blocks every other process until it finishes or times out.
- **No DHCP** — still a static IP plus QEMU SLIRP's built-in DNS forwarder.
- **Mounted FAT drives are still root-directory-only and read whole into
  RAM** — see Added, above, for what did improve this release.
- All prior Known Issues not superseded above still apply.

## [0.6.5] - 2026-09-29

### Added

- **`free`** — physical RAM and kernel-heap usage, backed by a live free-page
  counter in `pmm.c` and a heap free-list walk.
- **`cpuload`** — system-wide load plus per-process breakdown, sampled via
  `process_sleep()` (a real yield, not a `pit_wait_ms()` spin).
- **`-help`/`--help`/`-h`** — uniform convention for every command that takes
  arguments, documented in `commands.h` for future v0.7 packages.
- **`passwd`** — change a password after `useradd` (previously impossible);
  root can reset anyone's.
- **Password salt + double-round FNV-1a.** `/etc/passwd` grows a salt field.
  Explicitly non-cryptographic — defeats a precomputed rainbow table, not a
  determined attacker. No inode format change.
- **Boot warning** if root's password is still the seeded default (`toor`).
- **Real PATH fallback** — unknown commands resolve as `/bin/<name>`
  (root-relative); non-executable files are correctly reported as unknown.
- **`/bin`**, root:root 0755 — non-root writes blocked by the existing
  permission system, no new enforcement code.
- **64 KB USB MSD batched transfers** — up to 128 blocks per SCSI command via
  a new contiguous DMA buffer; `mount` of an 8 MB image drops from ~16384
  transfers to ~128.
- **Multi-mount** — up to two named mounts (`mount 0 a`), each with its own
  `fat_fs_t`, plus `mountls`/`mountcat`/`mountwrite <name>`.

### Fixed

- **TCP retransmission** — stop-and-wait, every segment (SYN/FIN included)
  retained until cumulatively ACKed; fixed 300 ms RTO, 5 retries, then clean
  close instead of a hang. `tcp_send()` is now blocking. No adaptive RTT or
  backoff — enough for QEMU's SLIRP link.
- USB MSD transfer-ring wraparound re-verified at the new batched sizes.

### Changed

- Confirmed the `vfs_*_at()` migration of shell filesystem commands was
  already complete; only PATH resolution was missing.
- **Shell dispatcher table refactor deferred to v0.7** — pure
  behavior-preserving change with risk across every command at once.
- **VFS routing into mounts deferred.** Full integration required reviving
  ~1100 lines of never-compiled `fat_vfs.c`; shipped named mounts instead.
- `tools/seed/passwd` updated for the salt field. Version bumped to 0.6.5.

### Known Issues

- **No VFS routing into mounts** — `cd` into a mount point does not work; use
  `mountls`/`mountcat`/`mountwrite`.
- **Shell dispatcher is still a 52-branch if/else chain** — deferred to v0.7.
- **Shell is still kernel-native ring 0** (`shell_task`, synchronous console
  input from the keyboard IRQ). Moving it to a ring-3 ELF needs a real
  console subsystem (blocking read, Ctrl+C as a signal, cursor/color
  syscalls) — planned as a separate final sub-stage of v0.7.
- **Password hashing is non-cryptographic** (salted double-round FNV-1a,
  best-effort salt).
- **TCP uses a single fixed RTO** (300 ms, 5 retries), no adaptive RTT or
  backoff.
- **`mount` reads the whole device into RAM up front** (8 MB cap);
  `MAX_MOUNTS = 2` — each image lives in the 16 MB kernel heap.
- All prior Known Issues still apply (no DNS/DHCP, no congestion control,
  real hardware untested, no package manager yet).

## [0.6.0] - 2026-09-24

Covers all changes since `v0.3.1` (commit `7da5d8a`, "fixes 1", inclusive).

### Added

- **Preemptive multitasking.** The scheduler now genuinely preempts ring-3 code from the timer (100 Hz, ~50 ms timeslice) instead of relying on a process to voluntarily yield; a new `context_enter_ring3()` cold-start path guarantees every process actually reaches ring 3 (a CPU-bound program that never calls a syscall previously never left ring 0). A misbehaving program can no longer freeze the whole system.
- **Anonymous memory mapping.** `mmap`/`munmap` are fully implemented (eager allocation, per-process region tracking, `PROT_READ`/`PROT_WRITE`/`PROT_EXEC` enforced via the NX bit) — no longer stubs. This is what the new minimal libc's `malloc()` is built on.
- **Syscall hardening.** Every syscall that dereferences a user pointer now validates it (rejects kernel addresses and unmapped memory instead of trusting user input blindly). Real errno-style error codes (`EFAULT`, `EINVAL`, `ENOENT`, `ENOTDIR`, `ERANGE`, `EPERM`, `EACCES`, …) replace bare `-1`. `getcwd`/`chdir` are real syscalls backed by per-process state instead of stubs. New filesystem syscalls `SYS_MKDIR`, `SYS_RMDIR`, `SYS_UNLINK`, `SYS_READDIR` expose functionality the VFS already had internally. Total syscall count: 19 → 27.
- **Real argv/envp.** `run`/`runbg`/`exec` now split and pass real command-line arguments through to user programs (`int main(int argc, char **argv, char **envp)`); previously every program always received zero arguments.
- **A minimal userspace libc** (`libc/`) — `crt0.S` startup, `malloc`/`free` (arena-based, built on `mmap`), a `string.h` subset, and a small `printf`. This is the first C toolchain pipeline in the project; every prior test program was hand-written NASM.
- **Users, groups, and permissions** — a Unix-like `uid`/`gid`/9-bit permission model. Persistent `/etc/passwd` and `/etc/group` (colon-separated, FNV-1a password hashing), new commands `whoami`, `chmod`, `chown`, `useradd`, `groupadd`, and `su`. Every file/directory operation (open, create, mkdir, rm, cp, mv, run, …) now enforces owner/group/other permission bits; root (`uid` 0) bypasses all checks. Boots straight into a root shell — no login prompt.
- **xHCI USB host controller**, replacing the old UHCI (USB 1.1) driver entirely — real command/event/transfer ring model, HID keyboard/mouse support carried over with feature parity.
- **USB Mass Storage** (Bulk-Only Transport over xHCI) — block-level `usbinfo`/`usbread`/`usbwrite` commands, and new `mount`/`unmount` commands that activate the (previously entirely dead, never-compiled) FAT driver to mount a USB flash drive's FAT12/16/32 filesystem read/write. Not integrated into the VFS (LufiraFS stays the only VFS-mounted filesystem) — `mount` reads the whole device into RAM (capped at 8 MB) and works directly against the FAT driver, the same way `usbread`/`usbwrite` talk to the block layer directly.
- **RTL8139 network driver and a basic TCP/IP stack** — Ethernet, ARP, IPv4, ICMP, and a minimal single-connection blocking TCP client, entirely polled (no APIC/MSI support exists in this kernel). New commands `ifconfig`, `ping`, and `wget` (IP-address targets only, no DNS).
- New filesystem syscalls exposed to shell commands via a `vfs_*_at()` layer (`cp`/`mv`/`ls`/`mkdir`/`rm`/`touch`/`run` now go through the same syscall-layer VFS calls that ring-3 programs use, instead of calling LufiraFS internals directly — removing the prior code duplication between the two paths).
- Process teardown now actually frees a process's address space (page tables), ring-0 stack, and open file descriptors on exit — previously every `run`/`runbg`/`exec` leaked all three. Orphaned background (`runbg`) processes with no `wait()` are now reaped automatically from the idle loop. `MAX_PROCESSES` is now enforced (previously defined but never checked).

### Fixed

- `cd`, `mkdir`, `cp`, `mv`, `exec`, `fork()` (a register-clobber bug in the ring-3 entry path), and a small process-management race — the batch of stability fixes that opened this cycle (`fixes 1`, `fix mkdir and cd`, `fix`, `fix fork 2`).
- A page-table-permission bug (`get_or_create_table()` never set `PAGE_USER` on newly-created intermediate paging structures) that silently made every freshly-`mmap`'d or stack page kernel-only accessible from ring 3 — invisible until real ring-3 code first tried to read/write data through `mmap`.
- Two xHCI USB Mass Storage reliability bugs found while stress-testing the new `mount` command (the first code in the project to issue thousands of bulk transfers back-to-back): a bulk-transfer timeout that was five times shorter than the control-transfer timeout (a forgotten debugging artifact), and a real ring-buffer bug — the transfer ring's Link TRB never had its Cycle bit updated on wraparound, so the controller would stall at the boundary of every ~255 transfers. A third race (the timer-interrupt-driven USB poll could steal the completion event a synchronous Mass Storage wait was blocked on) was also found and fixed.
- `command_rm`'s bulk-delete path (`rm *`) and its single-file path could triple-fault — GCC reserves a function's entire worst-case stack frame up front (no `-O`), so a large stack-local array inside a rarely-taken branch still consumed stack on every call, and the newly-added VFS call chain tipped an already-marginal 16 KB shell stack over the edge.
- `lufirafs_create()`'s VFS wrapper collapsed every failure reason to the same generic error code instead of passing the real one through.
- Various boot-path, HID input, and process-lifecycle fixes bundled in `bug fixes 1`.

### Changed

- Kernel version bumped to **0.6.0**.
- Shell filesystem commands (`cp`, `mv`, `ls`, `mkdir`, `rm`, `touch`, `run`) migrated from calling LufiraFS internals directly to the same `vfs_*_at()` layer the syscalls use, while explicitly keeping their existing permission checks (the VFS layer itself does no permission enforcement).
- Trimmed and corrected a number of oversized or stale comment blocks across the kernel (including doc comments that still referred to the removed UHCI driver or a cooperative-only scheduler).

### Known Issues

- **`mount` reads the whole USB device into RAM up front** (capped at 8 MB) — no lazy/streaming FAT access, and no VFS integration (files on a mounted USB drive aren't reachable through `cat`/`cp`/`ls` yet, only through `mount`'s own directory listing).
- **The bootloader's own FAT loader and the legacy `kernel/fs/fat/fat.c` are two independent FAT implementations** that happen to now both be in active use (ESP loading vs. USB mount) — not unified.
- **No DNS or DHCP.** `wget`/`ifconfig` work with literal IP addresses and a static network configuration only.
- **Networking has no retransmission or congestion control.** The TCP client is a minimal, single-connection, best-effort implementation suited to a local/QEMU link, not a real network.
- **Real hardware remains untested.** The system is developed and tested exclusively in QEMU.
- No package manager yet — every driver and shell command still ships built into the kernel binary. This is the explicit scope of the next release (v0.7).


## [0.3.1] - 2026-09-13

### Fixed

- `cd`, `cp`, `du`, and `mv` commands 
- `exec` command path bug

### Known Issues

- Holding down a keyboard key does not repeat the character.
- `fork()` and `exec()` are unreliable — a forked child process can crash before it reaches its first instruction.


## [0.3.0] - 2026-09-12

Covers all changes since `v0.1.0` (commit `1fa52bd`, "fix bug with clear", inclusive).

### Added

- **LufiraFS** — a custom filesystem that replaces FAT as the primary storage backend. FAT is now used only for the small UEFI ESP partition that firmware requires to load the bootloader and kernel; the rest of the disk is a dedicated LufiraFS region with its own superblock, block bitmap, fixed inode table, and real `.`/`..` directory entries (fixing FAT's root-only directory search and hardcoded-parent-cluster bugs). Ships with a host-side `tools/mkfs_lufirafs` formatting/populating tool and full VFS integration.
- `df` and `du` shell commands for filesystem free-space and disk-usage reporting.
- USB support: a UHCI host-controller driver and a USB HID boot-protocol keyboard/mouse driver, unified with the existing PS/2 input through a new `drivers/input` dispatcher.
- Real process primitives: `fork()`, in-place `exec()`, `wait()`, signal-based `kill` (`SIGTERM`/`SIGKILL`/`SIGSTOP`/`SIGCONT`), and anonymous pipes, exposed as new syscalls (`SYS_FORK`, `SYS_WAIT`, `SYS_KILL`, `SYS_PIPE`) and shell commands (`kill`, `wait`, `exec`).
- Per-process address-space cloning for `fork()`, and a private low-memory identity map per process so that mapping a user ELF segment in one process can no longer corrupt the identity map used by others.
- `Ctrl+C` to interrupt the current foreground process from the shell.
- **Developer mode** — a persistent on-disk flag (`/system/devmode.flag`) that gates verbose boot and driver diagnostics. Toggle with the new `devmode [on|off]` shell command; `make debug` now enables it automatically for every debug run.
- **Kernel logging** (`klog`) — short status lines are recorded to `/logs/system.log` for boot and process events, so they remain available even when developer mode is off.
- New root directories `/system` (developer-mode flag) and `/logs` (system log), created when the disk image is built.
- A graphical boot splash (large scaled "LufiraOS" wordmark + caption) shown while the system boots quietly, and a small tilted "LufiraOS" watermark drawn once the shell starts.
- Colourised `ls` output (directories vs. files) and support for `ls [-l] <path>` — previously any path argument to `ls` was silently ignored and it always listed the current directory.

### Changed

- Kernel version bumped to **0.3.0**.
- The boot log is quiet by default: low-level status lines (GDT/TSS/IDT/PIC/PMM/paging/heap/LufiraFS mount/ACPI/process manager/PIT/syscalls/VFS/keyboard/mouse/AC'97/UHCI) now only print when developer mode is enabled. Genuine errors and warnings are always shown regardless of the mode.
- Introduced a dedicated kernel-space physical memory map (`phys_to_virt()`) used throughout paging/process/ELF code, replacing the previous assumption that low physical memory stayed identity-mapped under every process's page tables — that assumption broke as soon as a user ELF segment was mapped over the same physical range.
- `status` now reports whether interrupts have ever been enabled on this boot instead of the live EFLAGS.IF bit, which always read as cleared because shell commands run synchronously inside the keyboard IRQ handler.
- `write`/`edit` (and other filesystem commands) now resolve paths relative to the current working directory instead of always writing to the root directory.
- Cleaned up numerous oversized/disproportionate comment blocks across the kernel while preserving the non-obvious "why" behind each one.

### Fixed

- `clear` no longer corrupts console/history state.
- `cp`/`mv` no longer corrupt data when the source and destination share the same filename.
- Sector writes now take effect without requiring a reboot.
- Assorted UHCI/USB stability fixes (control-transfer timeouts, port-reset sequencing, TD/QH pool exhaustion).
- Assorted process/paging fixes that made running ELF programs (`run`/`runbg`) more reliable, including a register-clobber bug in `context_switch()`.

### Known Issues

- Holding down a keyboard key does not repeat the character.
- `fork()` and `exec()` are unreliable — a forked child process can crash before it reaches its first instruction.
- `cd`, `cp`, and `mv` are not fully stable — will be fixed in the next release (v0.3.1).
