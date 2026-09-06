# >2 TiB single-HDD design — banked APA extension

This is the experimental Stage-2 design. The proven <=2 TiB path on `main` remains unchanged until the modified OPL launch path is proven on real hardware.

## Why upstream stops at 2 TiB for APA

The ATA hardware path is not the main limitation. Current OPL detects 48-bit LBA-capable disks, and its bundled ATAD implementation contains a 64-bit internal sector-I/O routine. The legacy Sony-compatible APIs above it still expose 32-bit LBAs.

The APA/HDL structures used by OPL are also 32-bit:

- APA `start`, `length`, `next`, `prev` and subpartition sector fields are `u32`.
- OPL `apa_sub_t.start/length` are `u32`.
- `hdl_game_info_t.start_sector` is `u32`.
- EE-side `hddReadSectors()` accepts a `u32 lba`.
- `hddAtaTransfer_t.lba` is `u32`.
- HDD cdvdman ultimately computes a 32-bit LBA before calling the ATA transfer routine.

Widening the on-disk APA structures would break compatibility with Sony/APA tooling and FHDB. The design therefore keeps each APA chain ordinary and 32-bit.

## Disk contract: N independent 2-TiB address banks

A physical 48-bit-LBA disk is divided at 2-TiB (`2^32` 512-byte sectors) boundaries:

```text
Bank 0 base = 0x000000000 sectors   (0–2 TiB)
Bank 1 base = 0x100000000 sectors   (2–4 TiB)
Bank 2 base = 0x200000000 sectors   (4–6 TiB)
Bank 3 base = 0x300000000 sectors   (6–8 TiB)
...
Bank N base = N * 0x100000000 sectors
```

The final bank may be partial. A normal decimal 4-TB drive uses Bank 0 plus most of Bank 1; an 8-TB drive uses Bank 0 through Bank 3.

Inside every bank, APA headers use ordinary **bank-relative 32-bit LBAs**. There is no new incompatible 64-bit APA format.

- **Bank 0** remains a conventional PS2 HDD and contains FHDB, system PFS, OPL, ART/CFG/APPS/VMC and games.
- **Bank 1+** are game-storage banks only during the first implementation.
- OPL scans all valid banks and merges every HDL title into its existing single **Internal HDD** menu.
- The console user never has to choose a bank to launch a game.

## PS2 HDD Manager behavior

The PC manager is allowed to expose the physical placement because it is useful for provisioning and testing.

For normal users:

```text
Game destination:  Automatic (recommended)
```

The manager chooses a bank with sufficient free space and records where the game was installed.

For development/testing:

```text
Game destination:
  Automatic
  Bank 0   0–2 TiB
  Bank 1   2–4 TiB
  Bank 2   4–6 TiB
  Bank 3   6–8 TiB
```

This lets us deliberately install known games into Bank 0 and Bank 1+ and verify that OPL presents them together. The installed-game table may show a **Bank** column in the PC manager for diagnostics. That bank is not exposed in OPL's normal game menu.

The formatter/writer must never create a bank whose physical start is beyond the real device capacity. Every bank operation retains the existing exact-device, capacity, mount, system-disk and 512-byte-logical-sector safety checks.

## PC-side game installation

Stock hdl-dump's `slice_index` is **not** our >2-TiB bank index; its existing slice offset is unrelated and must not be reused as if it represented a 2-TiB physical bank.

The host backend therefore needs an explicit bank-base abstraction. The preferred implementation is to patch our pinned hdl-dump backend so all raw physical I/O becomes:

```c
physical_lba = bank_base_sector + apa_relative_lba;
```

while all APA headers written inside the bank remain unchanged 32-bit values. The PS2 HDD Manager passes an explicit destination bank to the patched backend. Automatic mode chooses a suitable bank before invoking it.

## OPL changes required

### Game catalogue

Each discovered game needs a runtime-only bank base, for example:

```c
u64 bank_base_sector;
u32 start_sector;       /* ordinary APA-relative sector */
```

OPL scans Bank 0 using the existing path first. Additional banks are scanned by a bank-aware APA reader. All discovered titles are appended to the same existing HDD game list.

Duplicate title/partition handling must include the bank base so an identical relative start in two different banks is not treated as the same physical game.

### OPL-private 64-bit raw ATA path

Do not widen Sony-compatible exports. Add an OPL-private request containing a `u64` physical LBA and let the bundled ATAD layer call its existing 64-bit internal sector routine.

The legacy `sceAtaDmaTransfer(... u32 lba ...)` path remains unchanged for Bank-0 APA/PFS/FHDB compatibility.

### Game launch / cdvdman

The selected game's runtime settings carry `bank_base_sector` into HDD cdvdman. HDL part tables remain ordinary relative 32-bit values. Every physical game-data read becomes:

```c
u64 physical_lba = bank_base_sector + relative_data_lba;
```

This is the critical path. Merely listing a Bank-1 game is not success; it must boot and continue reading data above `0x100000000` sectors.

### Shared resources

PFS resources remain on Bank 0 for the first implementation. ART, CFG, APPS, VMC and themes therefore behave exactly as they do on a <=2-TiB disk regardless of which bank contains the selected game.

## FHDB

FHDB remains completely conventional in Bank 0. It boots the modified OPL ELF exactly as today. No EEPROM or FHDB format change is required for larger disks.

## Validation order

1. Keep the current real-hardware <=2-TiB baseline untouched and reproducible.
2. Build a sparse image large enough for at least Bank 0 + Bank 1 and prove independent bank-relative APA creation/scanning on PC.
3. Add the PS2 HDD Manager destination selector and deliberately place test games in both banks of the sparse image.
4. Add OPL's private 64-bit raw ATA read primitive and prove reads immediately below and above physical LBA `0x100000000`.
5. Scan Bank 1 and merge its games into the same Internal HDD catalogue as Bank 0.
6. Launch a known game stored wholly in Bank 1.
7. Exercise all HDL subpartitions for that game and long-running gameplay/data reads.
8. Repeat with Bank 2 on a >4-TiB sparse image to prove the design is truly N-bank rather than hard-coded for 4 TB.
9. Only after those tests enable write support on a real >2-TiB physical HDD.
10. Keep an explicit manual bank selector in the PC manager for diagnostics, while defaulting normal users to Automatic.
