# Development roadmap

## Stage 1 — standard <=2 TiB automation baseline (current, 0.1.0-alpha)

Completed/implemented for hardware testing:

- guarded Linux physical formatter and exact-device safety checks;
- fast Sony-compatible APA/PFS initialization with the historical full-media stale-header scrub removed;
- linked APA/checksum verification and required system-PFS mount tests;
- persistent host/download cache under `~/.cache/ps2-hdd-manager`;
- real Fedora mount selector via `QStorageInfo`;
- live privileged HDL game enumeration in the right pane;
- virtual CD/DVD game categories backed by the actual HDL TOC;
- multi-select + drag/drop game install with `cdvd_info2` auto-detection and hdl-dump progress;
- real OPL PFS browsing, including both custom-partition `/OPL` and canonical `+OPL` root conventions;
- batched drag/drop PFS copy for ART/CFG/APPS/THM/VMC/LNG/CHT;
- replacement-safe pfsshell `put` and idempotent folder merge behavior;
- latest OPL Beta + latest normal wLaunchELF ISR provisioning;
- latest Memory Card Annihilator provisioning with generated OPL App metadata;
- non-destructive **Install / Update OPL Apps** workflow for already-prepared HDDs;
- plug-and-play `conf_opl.cfg` at format time and one-click repair/apply on existing disks;
- FreeHDBoot 1.966 system/MBR install and generated `FREEHDB.CNF`;
- standalone FHDB HDD Boot Configuration ELF with Status/Enable/Disable/Verify and write verification;
- optional FreeDVDBoot ISO carrying the standalone EEPROM utility.

Still worth refining within the <=2 TiB manager before final release:

- richer PFS operations (extract/delete/rename/new folder from the GUI);
- explicit OPL settings editor beyond the recommended preset;
- validated IGR return choices. Direct HDD/PFS `exit_path` is intentionally not automated until a
  loader path that initializes HDD/PFS after IGR is proven;
- more detailed progress for large PFS/ART batches;
- optional larger dedicated OPL resources partition strategy if very large artwork libraries require it.

## Stage 2 — >2 TiB OPL bank extension

- keep on-disk APA fields 32-bit and bank-relative;
- independent bank boundaries at `0x100000000` 512-byte sectors;
- add a 64-bit physical bank base to OPL's internal HDL-game representation;
- add an OPL-private 64-bit raw ATA path while preserving Sony-compatible Bank-0 APIs;
- scan and merge games from all valid banks into one user-visible Internal HDD list;
- keep FHDB/PFS/config/art on Bank 0 only;
- pass the bank base into HDD cdvdman so a game wholly in Bank 1 actually launches.

## Stage 3 — 4 TB physical writer

Only after Stage 2 launches Bank-1 games on hardware:

- create/verify independent Bank 1 on a real >2 TiB disk;
- add bank-aware HDL install/extract/delete;
- add sparse-image regressions around physical LBA `0x100000000`;
- never expose Bank 0 / Bank 1 as separate user-facing drives.

## Stage 4 — 4 TB FHDB integration

- FHDB remains conventional on Bank 0;
- autoboot the modified OPL from Bank 0;
- validate cold boot -> FHDB -> modified OPL -> merged game list -> Bank-1 game launch;
- retain the same automated Apps/config/EEPROM-first-run workflow from the <=2 TiB manager.
