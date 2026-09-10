# Installed title rename + PC Game ID

## PC pane

The PC file browser has a **Game ID** column for `.iso` files.

It uses the same small direct-read identification algorithm as PS2 ISO Batch
Renamer: root-directory location -> ISO9660 root -> SYSTEM.CNF -> startup ID.
Only the required sectors are read and scans are dispatched off the UI thread.

**CHD is deliberately not implemented.** OPL's internal-HDD HDL path does not
use CHD images.

## Installed HDD titles

Installed HDL game titles can be changed without retransferring game data:

- select one game -> **Rename Game...**
- right-click -> **Rename installed game...**
- right-click selected games -> **Rename selected from latest gameid.txt**
- toolbar -> **Fix Installed Titles...** to audit all installed games

The database operation always downloads the current
`L10N37/PS2-ISO-Batch-Renamer-/main/gameid.txt`.

Every installed-title write goes through the privileged writer, checks the
target/bank and current Game ID/title, runs `hdl_dump modify`, then re-reads
`hdl_toc` and verifies that the same Game ID now has the requested title.
A failed metadata transaction stops a bulk pass before later writes.
