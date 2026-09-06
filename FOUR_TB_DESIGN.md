# 4 TB single-HDD design — OPL banked APA extension

This is the Stage-2 design, not yet a write-enabled format.

## Why upstream stops at 2 TB for APA

The ATA hardware path is not the main limitation. Current OPL detects 48-bit LBA-capable disks, and
its bundled ATAD implementation already contains a 64-bit internal sector-I/O routine. The legacy
Sony-compatible APIs above it still expose 32-bit LBAs.

The APA/HDL structures used by OPL are also 32-bit:

- APA `start`, `length`, `next`, `prev` and subpartition sector fields are `u32`.
- OPL `apa_sub_t.start/length` are `u32`.
- `hdl_game_info_t.start_sector` is `u32`.
- EE-side `hddReadSectors()` accepts a `u32 lba`.
- `hddAtaTransfer_t.lba` is `u32`.
- HDD cdvdman ultimately computes a `u32 lba` before calling the ATA transfer routine.

So simply making one APA chain 64-bit would stop being compatible with Sony/APA tools and FHDB.

## Proposed disk contract

Keep APA itself unchanged and use multiple independent 32-bit APA banks on one physical 48-bit LBA
HDD:

- **Bank 0** starts at physical LBA `0x000000000`.
- **Bank 1** starts at physical LBA `0x100000000` (2 TiB in 512-byte sectors).
- APA headers inside every bank keep ordinary 32-bit **relative** LBAs.
- Bank 0 remains a completely normal PS2 disk and is the only bank containing FHDB/system PFS.
- Later banks are game storage only.
- OPL scans all valid banks, merges all HDL games into one menu, and never exposes "Disk 1 / Disk 2"
  to the user.

A 4,000,000,000,000-byte HDD therefore appears to OPL as one source containing games from Bank 0
and Bank 1.

## OPL changes required

### EE-side catalogue / management

Add a bank base to every HDL game record, for example:

```c
u64 bank_base_sector;
u32 start_sector;       // unchanged APA-relative sector
```

All physical reads become:

```c
physical_lba = bank_base_sector + relative_lba;
```

The game list scanner must enumerate Bank 0 through the normal APA driver and additional banks
through a bank-aware APA reader, then merge the results into the existing `hddGames` list.

### 64-bit raw ATA path

Do not widen on-disk APA fields. Instead add a new OPL-private raw-read API whose request contains a
`u64 lba`. The bundled ATAD code can then call its existing 64-bit internal sector routine directly.

The legacy Sony-compatible `sceAtaDmaTransfer(... u32 lba ...)` export remains unchanged for normal
Bank-0 APA/PFS/FHDB compatibility.

### Game launch / cdvdman

`cdvdman_settings_hdd` needs the physical bank base (u64). `device-hdd.c` continues reading the
ordinary HDL part table from the selected game's relative APA header, but adds the bank base when it
turns each HDL `data_start` into a physical ATA LBA.

This is the critical launch path: catalogue support alone is not enough.

### Shared resources

PFS resources stay on Bank 0 only (`+OPL`, CFG, ART, VMC, themes, etc.). Additional banks are not
mounted through stock PFS and contain HDL games only.

## FHDB

FHDB remains conventional and boots from Bank 0. Once it launches the modified OPL ELF, OPL itself
becomes responsible for discovering and using Bank 1+. No FHDB format change is required.

## Validation order

1. Prove current 2 TB standard APA path on real hardware.
2. Build a two-bank sparse image and test bank-relative APA scanning on PC.
3. Add OPL 64-bit raw ATA read primitive and test reads just below/above physical LBA `0x100000000`.
4. Merge Bank-1 game catalogue into OPL UI.
5. Launch one game stored entirely in Bank 1.
6. Test games whose HDL subpartitions cross locations within Bank 1.
7. Only then enable the 4 TB physical writer in PS2 HDD Manager.
