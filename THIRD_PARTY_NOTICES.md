# Third-party projects, backends and data sources

PS2 HDD Manager is GPL-3.0-or-later. It builds on, interoperates with, downloads from, or reuses work
from the projects below. Refer to each upstream project for its own authorship, license and
redistribution terms.

## hdl-dump — ps2homebrew

https://github.com/ps2homebrew/hdl-dump

Pinned upstream commit:

`32c296c69cf9c263fcbe035004aa28c345b3b279`

PS2 HDD Manager applies manager-specific host patches for Extended APA bank-relative I/O, large-scan
progress and safe title/self-collision handling. The resulting backend is modified, not stock
hdl-dump.

## pfsshell — ps2homebrew

https://github.com/ps2homebrew/pfsshell

Pinned upstream commit:

`8c92467b3d715c3698f1f8ce63a8a07e214d6c73`

PS2 HDD Manager applies manager-specific fast-format, PFS merge/replace and bank-aware ATAD host
patches. The resulting backend is modified, not stock pfsshell.

## PS2DEV / PS2SDK

https://github.com/ps2dev
https://github.com/ps2dev/ps2sdk

## Open PS2 Loader

Upstream project:

https://github.com/ps2homebrew/Open-PS2-Loader

Extended APA fork used for multi-bank HDDs:

https://github.com/L10N37/Open-PS2-Loader-Extended-APA

The Extended APA project derives from upstream OPL; upstream OPL authors and contributors retain
credit for the base project.

## FreeHDBoot / FreeMcBoot

https://github.com/israpps/FreeMcBoot-Installer

The provisioning workflow uses pinned FreeHDBoot / FreeMcBoot resources.

## wLaunchELF ISR

https://github.com/israpps/wLaunchELF_ISR

## Memory Card Annihilator

https://github.com/ffgriever-pl/Memory-Card-Annihilator

## FreeDVDBoot

https://github.com/CTurt/FreeDVDBoot

Used by the optional FreeDVDBoot ISO workflow.

## FCEUmm PS2 SMB

Manager payload source:

https://github.com/L10N37/Fceumm-PS2-SMB

Retain the upstream project attribution/license from that repository when redistributing the payload.

## OPL artwork

Preserved OPL Manager GameArt database:

https://github.com/Luden02/psx-ps2-opl-art-database

OPL Manager:

https://oplmanager.com/

The manager retrieves Game-ID-indexed cover/icon/screenshot files from the preserved mirror, caches
them locally and copies them into OPL `ART` storage. Artwork/media rights remain with their respective
owners; PS2 HDD Manager does not claim ownership of those assets.

## PS2 Game ID / title database work

PS2 ISO Batch Renamer:

https://github.com/L10N37/PS2-ISO-Batch-Renamer-

PS2 HDD Manager reuses audited Game-ID/title data and related identification/title-normalization work
developed for that project.

## Qt 6

https://www.qt.io/

Qt is used for the desktop user interface.

This notice collects the project's major external backends, runtime payloads and data sources. It does
not replace the upstream projects' own license and copyright files.
