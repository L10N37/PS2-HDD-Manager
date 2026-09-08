# PS2 HDD Manager

A desktop HDD manager and formatter for PlayStation 2 APA/PFS storage.

The current alpha focuses on **Linux/Fedora** and adds experimental **Extended APA** support for internal HDDs larger than 2 TiB while keeping Bank 0 compatible with the normal PS2 APA/PFS/FHDB layout.

## Hardware-tested status

**Linux/Fedora: tested on real PlayStation 2 hardware.**

Validated with a 4 TB HDD:

- FHDB boot from Bank 0
- normal Bank 0 APA/PFS storage
- HDL game installation and launch from Bank 0
- HDL game installation and launch from Bank 1
- multiple games installed consecutively without stalling between titles
- mixed-bank transfer queue: queued titles can target Bank 0 or Bank 1 in one unattended run
- persistent red transfer marks / queue
- separate overall and current-game progress bars
- OPL artwork stored in Bank 0 and used by upper-bank games
- automatic installation of the matching Extended APA OPL build during >2 TiB setup

**Windows:** build support exists, but physical-HDD formatting/install workflows are **not yet hardware-tested**. Treat Windows support in this alpha as experimental.

## Extended APA

Large disks are split into independent APA banks.

```text
Bank 0 physical base: 0x000000000
Bank 1 physical base: 0x100000000
Bank 2 physical base: 0x200000000
...

physical_lba = (bank_index * 0x100000000) + bank_relative_lba
```

Bank 0 remains the boot/system bank and can contain FHDB, PFS data, OPL data, artwork and games. Bank 1+ are intended for HDL games.

For >2 TiB layouts the manager installs the matching public loader automatically:

**Open PS2 Loader Extended APA**
`L10N37/Open-PS2-Loader-Extended-APA`
`v1.2.0-Beta-2273-Extended-APA-1`

The downloaded ELF is pinned and SHA-256 verified by the manager.

## Transfer queue

Choose a destination bank before marking games.

- Right-click toggles a persistent red queue mark.
- **Add Selected to Queue** adds the current selection.
- Change the bank selector and add more titles to queue them for another bank.
- **Unmark All** clears the queue.
- F5 starts the queue.
- New titles can also be appended while a transfer is running.
- Each queued title keeps its own destination bank.
- The manager performs a fresh bank scan between titles to avoid stale APA/hdl_dump state.

## Building on Fedora

```bash
./build_fedora.sh
```

Run:

```bash
./build/fedora/PS2-HDD-Manager
```

Build and run:

```bash
./build_fedora.sh --run
```

## Warning

This is an **alpha** release which performs raw writes to physical disks.

Double-check the selected device before formatting or installing anything. Keep backups of data you care about. Extended APA requires the matching Extended APA-aware OPL build for games stored above Bank 0.

<!-- V011_ALPHA_STATUS_START -->
## v0.1.1-alpha status

`v0.1.1-alpha` is the current **pre-release reliability baseline**.

The previous `v0.1.0-alpha` release was a proof-of-concept / early hardware-validation build. The new alpha has been stress-tested on a heavily populated 2 TB HDD with a 602-title queue: **602/602 processed, 601 newly installed and 1 exact existing title safely skipped**.

The stress run took roughly **16 hours at 40-60 MiB/s** on the test setup. That is a known performance problem, not a target: conservative manager-side checks currently repeat too much work between games as the APA chain grows.

### Next branch roadmap

The next development branch focuses on three major areas:

- **Transfer preflight / performance:** inventory source games and destination banks once, optionally check duplicates once, calculate the complete AUTO bank plan once, then execute the queue without repeated manager-side full scans between titles.
- **PS2 Batch Renamer integration:** reuse the audited PS2 title/game-ID database and ISO/CHD identification logic from `L10N37/PS2-ISO-Batch-Renamer-` (`v4.0.1`) so users can optionally auto-rename before transfer, detect duplicate IDs and use the database without being forced to rename files.
- **Automatic PS1 POPS / POPStarter setup:** reserve/configure Bank 0 POPS storage during HDD setup, provide selectable reserve sizes, validate user-supplied proprietary runtime files, prepare VCD installs, detect disc IDs and skip existing exact matches.

Artwork handling will also become destination-aware so already-present ART can be skipped before unnecessary downloads/writes.

See `docs/releases/v0.1.1-alpha.md` for the detailed release status and roadmap.
<!-- V011_ALPHA_STATUS_END -->
