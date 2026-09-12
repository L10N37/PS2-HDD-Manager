# PS2 HDD Manager

A desktop HDD manager, formatter and provisioning tool for PlayStation 2 APA/PFS storage.

**Current release: v0.2.0**

Fedora/Linux is the hardware-validated platform. Windows physical-HDD workflows remain experimental.

## v0.2.0 — 4 TB / Extended APA hardware validation

v0.2.0 was validated on a **Toshiba X300 4 TB HDD** (3.64 TiB visible to the host) populated with
**1,274 PS2 games**.

Validated on real PS2 hardware:

- FHDB boots from Bank 0;
- Bank 0 and Bank 1 HDL game installation and launch;
- **AUTO Bank 0 -> Bank 1 rollover** when Bank 0 reaches capacity;
- rapid detection/skip of already-installed exact games when a large queue is resumed;
- Extended APA-aware OPL on the >2 TiB layout;
- installed-title fixes plus OPL `games.bin` invalidation/rebuild;
- direct HDD IGR return to a second OPL copy on the existing HDD;
- fresh-format provisioning with HDD IGR return enabled by default;
- Bank-0 OPL artwork shared across the multi-bank game set.
- Native PS2-themed application icon on Qt, KDE/Wayland and Windows builds;

## Extended APA

Large HDDs are represented as independent APA banks:

```text
Bank 0 physical base: 0x000000000
Bank 1 physical base: 0x100000000
Bank 2 physical base: 0x200000000
...

physical_lba = (bank_index * 0x100000000) + bank_relative_lba
```

Bank 0 remains the boot/system bank and contains FHDB, normal PFS application/configuration storage,
artwork and games. Bank 1+ are games-only APA banks.

The manager uses the matching Extended APA-aware OPL build for multi-bank disks:

https://github.com/L10N37/Open-PS2-Loader-Extended-APA

Current pinned Extended APA loader family:

`v1.2.0-Beta-2273-Extended-APA-1`

Upstream Open PS2 Loader:

https://github.com/ps2homebrew/Open-PS2-Loader

## Transfer queue

The PC pane supports a persistent marked queue and explicit-bank or **AUTO** placement.

Large queues cache installed-game identities and use targeted post-install verification instead of
performing full all-bank destination scans for every title.

AUTO capacity accounting stays fast through the normal part of a bank, then switches back to an
authoritative live APA check near the bank boundary. This prevents accumulated allocation-estimate
drift from attempting to place another title in a bank that is actually full.

If the queued source set cannot fit on the physical HDD, the manager warns before the transfer starts.

## HDD IGR return

Fresh formatting/provisioning includes this option, enabled by default:

```text
[x] Install HDD IGR Return (recommended)
```

The same matching OPL build is installed as:

```text
hdd0:PP.FHDB.APPS:pfs:/OPL/OPNPS2LD.ELF
hdd0:PP.FHDB.APPS:pfs:/OPL/IGR.ELF
```

and the manager writes:

```ini
exit_path=hdd0:PP.FHDB.APPS:pfs:/OPL/IGR.ELF
```

On >2 TiB disks, both copies are the matching Extended APA-aware OPL build.

Existing formatted HDDs can add or revert the same feature with:

- **Install HDD IGR Return**
- **Disable HDD IGR Return**

No reformat is required.

## PC Game ID / file pane

The PC pane has an optional **Scan Game IDs** checkbox.

- Off: the Game ID column remains blank and no ISO/folder probes run.
- On: the current directory is scanned asynchronously.
- Entering another directory scans it while the option remains enabled.
- Folder probing is intentionally non-recursive.
- Disabling scanning invalidates stale in-flight results.

The Date Modified column uses padded `MM/dd/yy` dates and the metadata columns use explicit spacing.

Game-ID/title work is shared with:

https://github.com/L10N37/PS2-ISO-Batch-Renamer-

## OPL artwork

Artwork source:

https://github.com/Luden02/psx-ps2-opl-art-database

The source repository documents itself as a preserved dump of the OPL Manager GameArt database:

https://oplmanager.com/

Recorded large-library validation pass:

- **5,000 ART files installed**
- **3,274 downloaded**
- **1,726 reused from cache**
- optional screenshots absent from the source database reported separately
- genuine unavailable/download failures reported separately

Artwork/media rights remain with their respective owners.

## Modified host backends

PS2 HDD Manager uses **modified pinned builds**, not stock binaries, of:

- hdl-dump — https://github.com/ps2homebrew/hdl-dump
- pfsshell — https://github.com/ps2homebrew/pfsshell

Exact pinned commits and local patch descriptions are documented in:

- `tools/hdl-dump/README.md`
- `tools/pfsshell/README.md`
- `THIRD_PARTY_NOTICES.md`

## Provisioning

The manager can provision/update OPL, FHDB resources, wLaunchELF ISR, Memory Card Annihilator,
FHDB HDD Boot Configuration, recommended OPL settings, HDD IGR return and OPL artwork.

See `PROVISIONING.md`.

## Building on Fedora

```bash
./prepare_fedora_test.sh --skip-packages
./build_fedora.sh
./build/fedora/PS2-HDD-Manager
```

## Credits and third-party notices

See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Safety

PS2 HDD Manager performs raw writes to physical disks. Verify the selected target device before
formatting or installing and keep backups of important data.

Windows physical-HDD workflows have not received the same real-hardware validation as Fedora/Linux.
