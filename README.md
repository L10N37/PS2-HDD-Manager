# PS2 HDD Manager

A Qt 6/C++20 PC application for preparing and managing PlayStation 2 APA/PFS hard drives.

## 0.5.0-alpha — live HDD manager + plug-and-play OPL setup

0.5.0-alpha turns the successful 0.3 hardware baseline into the day-to-day manager intended for a
PS2 build. Physical writes remain deliberately limited to one ordinary 32-bit APA bank (<=2 TiB);
the banked 4 TB design remains disabled until the modified OPL path is proven.

### Live dual-pane manager

The left PC pane now uses `QStorageInfo::mountedVolumes()` on Linux, so Fedora mount points appear as
real selectable disks instead of the single `/` entry returned by `QDir::drives()`.

The right pane is read from the selected physical PS2 HDD through the guarded privileged helper:

```text
Internal HDD
├── HDL Games
│   ├── DVD
│   │   └── installed games...
│   └── CD
│       └── installed games...
└── OPL Storage
    ├── ART
    ├── CFG
    ├── APPS
    ├── CHT
    ├── LNG
    ├── THM
    ├── VMC
    └── conf_opl.cfg
```

`CD` and `DVD` are GUI categories only. Games remain normal HDL APA partitions. The game list is
obtained with pinned `ps2homebrew/hdl-dump` `hdl_toc --csv`. OPL Storage is the real PFS filesystem.
The manager maps both supported conventions correctly:

- custom OPL partition such as `PP.FHDB.APPS` -> `/OPL`;
- canonical `+OPL` partition -> PFS root `/`.

### Drag/drop games with progress

Select multiple supported PS2 images in the PC pane and press F5, or drag them onto **HDL Games**,
**DVD**, or **CD**. The manager probes each image with `hdl_dump cdvd_info2 --csv`, automatically picks
`inject_cd`/`inject_dvd`, and installs a normal HDL game partition.

The GUI parses hdl-dump's live percentage/ETA/MB/s output and shows transfer progress. After each
batch the physical HDD is reread and the installed games immediately appear in the right pane.

### ART / CFG / APPS file management

Files and whole directories can be dropped onto the real OPL PFS tree. Dropping one of these folders
onto **OPL Storage** automatically merges it into the matching destination:

`ART`, `CFG`, `APPS`, `THM`, `VMC`, `LNG`, `CHT`.

A whole folder is sent through one privileged batch transaction rather than one privilege prompt per
file. The Fedora pfsshell build is patched so existing directories are valid merge targets and `put`
uses `FIO_O_TRUNC`, ensuring a shorter replacement file cannot leave stale bytes at the end.


### One-time Linux unlock + red transfer marks

Fedora no longer launches a new `pkexec` process for every physical-disk read or every installed game.
After the main window appears it asks for Polkit authentication once and starts a restricted privileged
writer session. The session can only execute PS2 HDD Manager writer operations, remains tied to the
application lifetime, and exits automatically when the GUI closes. If authentication is cancelled the
manager stays locked and no physical HDD operation starts.

Normal Ctrl/Shift selection still works in the PC pane. Right-click also toggles the older persistent
**red transfer mark** workflow. When any red marks exist, F5 installs the marked files; otherwise F5 uses
the normal current selection. Right-clicking a red item again removes its mark.

### Built-in Add Art

The manager can now scan the live HDL table for Game IDs and install OPL artwork without OPL Manager.
**Add Art...** supports all installed games or the selected games, downloads/caches matching cover (`COV`),
icon (`ICO`) and screenshot (`SCR`, `SCR_00`, `SCR_01`) assets, then sends one PFS batch into the active
OPL `ART` folder. The first provider is the preserved OPL Manager GameArt database mirror maintained by
Luden02. Artwork is cached under `~/.cache/ps2-hdd-manager/art/PS2/<GAMEID>/`, so later HDD builds reuse it.

After a successful artwork copy, the writer patches only `enable_coverart=1` in the existing
`conf_opl.cfg`; unrelated OPL settings are preserved. The artwork provider is abstracted so a live provider
can be added later without changing the HDD/PFS implementation.

### Existing-HDD app updater + plug-and-play OPL configuration

A fresh setup can generate `conf_opl.cfg` before first boot. Existing disks can be repaired/configured
without formatting via **Apply Recommended OPL Defaults** in the main window.

**Install / Update OPL Apps...** performs the same non-destructive workflow for an already-prepared HDD.
It can fetch/install current wLaunchELF, Memory Card Annihilator and the dedicated FHDB HDD Boot
Configuration utility, and can update OPL itself when the manager recognizes its managed
`PP.FHDB.APPS` FHDB layout. It can apply the recommended `conf_opl.cfg` in the same transaction.

The preset writes the current OPL keys for:

- Internal HDD startup = AUTO;
- default device = Internal HDD games;
- Apps startup = AUTO when Apps are provisioned;
- cover art enabled;
- OPL delete/rename/write operations enabled;
- HDD game-list cache enabled;
- auto-refresh and auto-sort enabled;
- USB and Ethernet startup disabled for an internal-HDD-first build.

A direct HDD/PFS `exit_path` is intentionally **not** forced. Current OPL's in-game-reset EE loader
calls `LoadElf()` for the configured exit path without first initializing HDD/PFS. Generating an HDD
path would therefore look convenient but is not a safe stock-OPL default. The manager leaves IGR at
OPL's safe default until an HDD-return mechanism is proven.

### Optional applications

OPL is the primary/recommended selection. Additional Apps are independently selectable and are
created with modern OPL `title.cfg` metadata:

- latest normal `wLaunchELF_ISR` `BOOT.ELF`;
- latest Memory Card Annihilator automated build (packed ELF preferred);
- PS2 HDD Manager's dedicated **FHDB HDD Boot Configuration** utility.

### Dedicated FHDB HDD Boot Configuration utility

The previous KELFBinder-wrapper experiment has been removed. 0.5 contains source for a small standalone
PS2 ELF with a plain debug-screen UI:

```text
Current status : ENABLED / DISABLED / READ ERROR
OSD config byte0: 0x..

CROSS    Enable HDD/FHDB boot
SQUARE   Disable HDD/FHDB boot
TRIANGLE Re-read / Verify status
CIRCLE   Exit to PS2 Browser
```

Startup is read-only. Enable/Disable require a deliberate second confirmation press, reread the EEPROM
before writing, preserve unrelated configuration bits, and reread immediately afterward to verify the
requested state. Enable normalizes the low boot flags to FreeMcBoot's canonical `0x02`; Disable sets
only the HDD-MBR-skip bit.

The ELF is built from the included source. If no PS2DEV toolchain is already installed, the first
request for this optional app downloads the official prebuilt PS2DEV Ubuntu toolchain once into the
persistent cache. It is reused for later PS2 HDD Manager versions.

### Persistent cache

Large/repeated setup work is no longer tied to each extracted application version. Persistent data is
stored under:

```text
~/.cache/ps2-hdd-manager/
├── backend/
│   ├── pfsshell/
│   └── hdl-dump/
├── downloads/
├── art/
└── ps2dev-toolchain/
```

Pinned host backends are rebuilt only if their commit or our patch revision changes. OPL, wLaunchELF,
MCA and other downloads are reused when the release asset is unchanged. This is specifically intended
to avoid the long repeated setup observed during the 0.3 hardware tests.

### Fast standard formatter

The PC-specific pfsshell build skips PS2SDK's historical pass that writes an empty APA header every
128 MiB over the full disk. A 2,000,398,934,016-byte disk would otherwise receive about 14,905 tiny,
widely separated writes before the actual format.

The fast path still creates the normal Sony-compatible APA/PFS layout and then independently verifies:

- the linked APA chain and header checksums;
- `__mbr`, `__net`, `__system`, `__sysconf`, `__common`;
- mount/list/unmount of all four standard PFS system partitions.

### Optional provisioning

The setup dialog can additionally install:

- latest OPL Beta from release tag `latest`, exact asset `OPNPS2LD.ELF`;
- latest normal wLaunchELF ISR `BOOT.ELF`;
- latest Memory Card Annihilator automated build;
- FreeHDBoot 1.966 files and MBR bootstrap;
- the dedicated HDD-boot configuration ELF;
- an optional FreeDVDBoot ISO carrying that utility.

Network payloads are obtained and validated before the destructive physical writer starts.

## Fedora

Run as your normal desktop user:

```sh
chmod +x prepare_fedora_test.sh
./prepare_fedora_test.sh --run
```

The preparer installs/checks dependencies, reuses or builds the cached host backends, builds the app,
runs unit tests, and performs the exact standard-format transaction against a sparse image exactly
`2,000,398,934,016` bytes long. It never writes a physical HDD.

On Linux the GUI performs one Polkit authentication after launch and keeps a restricted `PS2-HDD-Writer`
session alive until the application closes. Refreshing the HDD, browsing PFS, installing many games,
copying ART, updating Apps and formatting all reuse that one authorization. Every request still reruns the
full exact-device/capacity/mount/system-disk safety preflight. The setup dialog also keeps the exact
`ERASE /dev/sdX` destructive confirmation.

See [`HARDWARE_TEST.md`](HARDWARE_TEST.md), [`PROVISIONING.md`](PROVISIONING.md), and
[`FOUR_TB_DESIGN.md`](FOUR_TB_DESIGN.md).
