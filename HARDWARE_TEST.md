# PS2 HDD Manager 0.1.0-alpha hardware test

## What this test is for

Earlier hardware tests proved the fast 2 TB format and ordinary HDL game write on real hardware. The initial alpha tests the complete
day-to-day manager workflow: one-time Linux authentication, persistent red transfer marks, live installed-
game view, built-in artwork downloads, OPL PFS file copy, plug-and-play OPL settings, Memory Card
Annihilator and the replacement standalone FHDB EEPROM utility.

The current physical writer is still limited to one ordinary <=2 TiB APA bank.

## 1. Prepare and launch

Put the source ZIP and Fedora launcher together in `~/Downloads`, then run the launcher as your normal
user. Do not use sudo:

```sh
cd ~/Downloads
chmod +x ./prepare_fedora_test.sh
./prepare_fedora_test.sh --run
```

On the first initial-alpha run a host backend may need to be built. Later runs should report cache hits from:

`~/.cache/ps2-hdd-manager`

The dedicated PS2 EEPROM ELF only needs PS2DEV when that optional payload is actually requested. If no
PS2DEV environment is already installed, the first request downloads the official prebuilt Ubuntu
PS2DEV archive once and caches it. It must not be redownloaded on subsequent manager builds.

## 2. One-time Linux unlock

Launch the GUI as your normal user. After the main window appears, expect **one** KDE/Polkit
authentication request. Enter the password once. The top bar should change to:

`HDD access unlocked for session`

For the rest of this application session, **Refresh HDD**, PFS browsing, installing multiple games,
Add Art, Apps updates and the setup/format dialog must not ask for the password again. Close the GUI and
confirm the privileged helper exits with it. If authentication is cancelled, no physical HDD operation
should start.

## 3. Existing prepared Hitachi — no reformat required

Reconnect the already-prepared HDD and select it in **PS2 physical disk**.

Expected right pane:

```text
Internal HDD
├── HDL Games
│   ├── DVD
│   │   └── Batman - Vengeance ...
│   └── CD
└── OPL Storage
    ├── APPS
    ├── ART
    ├── CFG
    └── ...
```

The existing Batman test game should be read back from `hdl_toc`, not shown as a planned placeholder.
The previous unprivileged `/dev/sdX: Permission denied` APA message should no longer be the right-pane
model; live reads go through the guarded privileged helper.

## 4. Update Apps on the existing disk — no reformat

Click **Install / Update OPL Apps...**. On the existing `PP.FHDB.APPS` disk, leave these selected:

- OPL Beta;
- wLaunchELF ISR;
- Memory Card Annihilator;
- FHDB HDD Boot Configuration;
- recommended OPL settings.

The first request for the dedicated FHDB utility may perform the one-time cached PS2DEV toolchain
download/build. The HDD itself must not be formatted. After completion, expand **OPL Storage/APPS**
and confirm all selected App folders are visible.

## 5. Repair the OPL configuration on the existing disk

Click **Apply Recommended OPL Defaults** if it was not already selected in the Apps updater.

This should replace the real OPL `conf_opl.cfg` and report success. On the next OPL boot verify:

1. Internal HDD initializes automatically — no manual **Start Device** action.
2. OPL opens on Internal HDD games by default.
3. Apps initialize automatically.
4. Cover art is enabled.
5. write/delete/rename operations are enabled.
6. HDD game-list cache, auto-refresh and auto-sort are enabled.

IGR `exit_path` is intentionally left at OPL's safe default in this build; do not treat that omission
as a failed config write.

## 6. Game install + live progress + red marks

First right-click two PC-side ISO files. They should remain visibly **red marked** even if the normal
selection moves elsewhere. F5 should use those red marks. Right-click one again to unmark it. Ctrl+A,
Ctrl+click and Shift selection must continue working normally when no red marks are present.

Then install several images or drag them to **HDL Games**. Check that:

- Fedora volumes are selectable from the PC disk dropdown;
- each image is automatically classified as CD or DVD;
- a live progress bar reports percentage plus hdl-dump ETA/MB/s when available;
- after completion the game appears immediately under the real CD/DVD category;
- OPL sees and launches it on the PS2.

## 7. Built-in Add Art + manual ART/PFS copy

With Batman or another installed game visible, select it and click **Add Art...**. Test both selected-games
and all-games scope. Request Cover, Icon and Screenshots. The manager should derive the Game ID from the
live HDL table, download/cache matching files, then copy them into OPL `ART` in one PFS transaction. For
Batman Vengeance (`SLUS_202.26`), at minimum the preserved database contains `SLUS_202.26_COV.png`,
`SLUS_202.26_ICO.png`, and numbered screenshots such as `SLUS_202.26_SCR_00.png`. Repeating Add Art should
reuse the local cache. There must be **no additional password prompt**.

Verify `enable_coverart=1` is enabled without other `conf_opl.cfg` settings being replaced.

Also test the manual path: from the PC pane, drag an existing `ART` directory onto **OPL Storage**. The manager should recognize
the folder name and merge its complete contents into the real OPL ART directory in one privileged
batch.

You can also drop files or arbitrary folders directly on a specific visible PFS directory.

After copying ART, launch OPL and verify covers are displayed without manually enabling artwork.

## 8. Memory Card Annihilator

For a freshly provisioned initial-alpha disk, verify under OPL Apps:

`Memory Card Annihilator`

Launch it and confirm normal operation. The manager selects the packed release ELF when present.

## 9. Dedicated FHDB HDD Boot Configuration app

This replaces the broken KELFBinder-wrapper test. On startup it should show a normal text screen, not
a centre line/white screen, and it must not write anything automatically.

On the current already-enabled console, expected initial status is **ENABLED**.

First test only **TRIANGLE — Re-read / Verify**. The status should remain enabled and the operation line
should report a verified read.

If deliberately testing Disable/Enable, have an independent boot route such as FMCB available first:

1. press SQUARE;
2. release SQUARE on the confirmation screen;
3. press CROSS as the deliberate second confirmation;
4. verify status becomes DISABLED and the write is reported VERIFIED;
5. reboot/test the changed HDD boot behavior if desired;
6. boot the utility through FMCB/another route;
7. Enable again and verify before relying on FHDB autoboot.

## 10. Fresh format/provision test

For another disposable <=2 TiB disk, leave the recommended selections enabled:

- latest OPL Beta;
- plug-and-play OPL config;
- wLaunchELF ISR;
- Memory Card Annihilator;
- FHDB HDD Boot Configuration;
- FreeHDBoot 1.966.

The writer must still require the exact `ERASE /dev/sdX` confirmation and verify the APA/PFS layout
before reporting success.

Do not enable or test the >2 TiB physical writer yet.


<!-- V020_STABLE_4TB_VALIDATION -->
## v0.2.0 stable 4 TB validation

Hardware candidate: **Toshiba X300 4 TB** (3.64 TiB host-visible), populated with **1,274 games**.

Validated: Bank 0 / Bank 1 operation, AUTO rollover, FHDB/OPL boot flow, installed-title/cache fixes,
large-library artwork handling and direct HDD IGR return to
`hdd0:PP.FHDB.APPS:pfs:/OPL/IGR.ELF`.

Recorded artwork pass: 5,000 files installed (3,274 downloaded, 1,726 reused from cache).
