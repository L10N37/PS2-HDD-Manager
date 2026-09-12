# v0.2.0 provisioning

All application selections are optional. The format operation always creates and verifies the standard
APA/PFS system layout first, then performs the selected provisioning transaction.

## OPL first

The recommended/default primary application is current OPL Beta. The fetcher resolves the GitHub
release tag `latest` from `ps2homebrew/Open-PS2-Loader` and selects exact asset `OPNPS2LD.ELF` rather
than GitHub's stable-only `/releases/latest` endpoint.

On a PS2 HDD Manager provisioned disk OPL is stored at:

`hdd0:PP.FHDB.APPS:pfs:/OPL/OPNPS2LD.ELF`

and `__common:/OPL/conf_hdd.cfg` contains:

```ini
hdd_partition=PP.FHDB.APPS
```

## Plug-and-play conf_opl.cfg

When selected, the manager writes a small `conf_opl.cfg` containing only the settings it owns:

```ini
hdd_mode=2
default_device=6
app_mode=2
usb_mode=0
eth_mode=0
enable_coverart=1
enable_delete_rename=1
hdd_game_list_cache=1
autorefresh=1
autosort=1
remember_last=0
autostart_last=0
```

`START_MODE_AUTO` is 2 in current OPL and Internal HDD is `HDD_MODE` 6. If no Apps are selected,
`app_mode` is emitted as disabled instead.

The same preset can be applied later from the main window with **Apply Recommended OPL Defaults**,
which replaces the existing config through PFS without reformatting.

## Install / Update Apps on an existing HDD

The main window also provides **Install / Update OPL Apps...**. This is non-destructive: it does not
format APA and does not touch the FHDB MBR/system files. It resolves the actual OPL PFS partition and
can install/update:

- OPL Beta itself when the managed `PP.FHDB.APPS` layout is detected;
- latest normal wLaunchELF ISR;
- latest Memory Card Annihilator;
- the dedicated FHDB HDD Boot Configuration ELF;
- the recommended `conf_opl.cfg` preset.

On an arbitrary `+OPL` disk the OPL ELF update option is disabled because the manager cannot safely
infer which external OPL executable the console actually boots. Apps and PFS settings can still be
installed normally.

### IGR / exit path

Direct HDD IGR return is hardware-validated on the managed `PP.FHDB.APPS` layout.

Fresh HDD setup includes **Install HDD IGR Return (recommended)** and it is pre-ticked by default
when OPL and the recommended OPL configuration are enabled.

Provisioning installs the matching OPL build as:

```text
/OPL/OPNPS2LD.ELF
/OPL/IGR.ELF
```

and writes:

```ini
exit_path=hdd0:PP.FHDB.APPS:pfs:/OPL/IGR.ELF
```

On >2 TiB Extended APA disks, both copies are the matching Extended APA-aware OPL build.

Existing formatted HDDs can add the same configuration with **Install HDD IGR Return**.
**Disable HDD IGR Return** restores `exit_path=Browser`; no reformat is required.
## Additional preconfigured OPL Apps

### wLaunchELF ISR

The fetcher resolves `israpps/wLaunchELF_ISR` tag `latest` and requires exact normal asset `BOOT.ELF`.
It is installed as:

```text
APPS/wLaunchELF/
├── BOOT.ELF
└── title.cfg
```

When FHDB is installed it is also copied to `__sysconf:/FMCB/BOOT.ELF` as the configured R1 fallback.

### Memory Card Annihilator

The fetcher resolves `ffgriever-pl/Memory-Card-Annihilator` tag `latest`, downloads the current release
ZIP, and prefers `mca-packed.elf` (falling back to `mca.elf` if the release layout changes that way).
It is installed as:

```text
APPS/Memory-Card-Annihilator/
├── BOOT.ELF
└── title.cfg
```

### FHDB HDD Boot Configuration

This is now a PS2 HDD Manager-owned standalone ELF, not a KELFBinder runtime wrapper. It displays the
current EEPROM HDD-MBR-boot state and provides Status/Enable/Disable/Verify controls.

Important safety properties:

- no EEPROM write at startup;
- a button that opens a confirmation screen must be released before a second press can confirm;
- state is reread immediately before a write;
- unrelated OSD configuration bits are preserved;
- Enable normalizes the low bits to the canonical FMCB/FHDB value `0x02`;
- Disable sets only the HDD-MBR-skip bit;
- every write is followed immediately by a reread and state verification;
- read failure disables writes.

## FreeHDBoot 1.966

FHDB resources are downloaded from pinned `israpps/FreeMcBoot-Installer` commit
`ac53a47a5c6eae675cc2611c7bebe62f56c7845c`. The writer installs the standard system/config files,
generates `FREEHDB.CNF`, installs the MBR payload at LBA `0x2000`, updates the APA MBR OSD fields and
rereads/checks the result.

When OPL is selected, FHDB's normal automatic target is the OPL ELF in `PP.FHDB.APPS`.

The PC cannot modify a console's EEPROM. The dedicated utility must still be run once on each console
whose HDD MBR boot state has not been enabled.

## FreeDVDBoot

The optional ISO uses CTurt's FreeDVDBoot filesystem profiles. The all-Slim English 3.10/3.11 profile
boots directly into the dedicated standalone HDD-boot configuration ELF while retaining the upstream
DVD-capable uLaunchELF as `ULE-DVD.ELF`.

The 2.10-2.13 and 3.04M+ phat custom-disc profiles remain explicitly experimental/manual-launch
because upstream custom phat disc setup is not equivalent to the all-Slim route.

## Host-side cache

Downloads and host backends live under `~/.cache/ps2-hdd-manager`. The application version directory
therefore no longer owns expensive dependencies. In particular:

- pinned pfsshell is reused until commit/patch revision changes;
- pinned hdl-dump is reused until its commit changes;
- release downloads are reused if size/digest still match;
- the optional official PS2DEV toolchain used to compile the dedicated EEPROM ELF is a one-time cache.
