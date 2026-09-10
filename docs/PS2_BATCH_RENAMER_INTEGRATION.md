# PS2 Batch Renamer integration

The transfer-renaming option deliberately does **not** pin a database release.

When **Auto-rename source files using latest gameid.txt** is enabled, PS2 HDD
Manager downloads this file at the start of every transfer queue:

`https://raw.githubusercontent.com/L10N37/PS2-ISO-Batch-Renamer-/refs/heads/main/gameid.txt`

The current file is validated before any HDD write begins. If the current
database cannot be downloaded or validated, the transfer does not start while
auto-rename remains enabled.

The existing `hdl_dump cdvd_info2` source probe supplies the startup/Game ID,
so database lookup adds no second disc-image scan.

When a current database title is found:

- the database title becomes the HDL game title;
- after a successful install, the PC source file is renamed to that title;
- an exact existing-game verification also permits the source rename;
- a failed HDD transaction never renames the source;
- an existing destination filename is never overwritten;
- the source extension is preserved.

Unticking the option restores the previous behaviour and does not require the
online database.
