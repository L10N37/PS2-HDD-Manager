# pfsshell backend

Upstream:

https://github.com/ps2homebrew/pfsshell

Pinned commit:

`8c92467b3d715c3698f1f8ce63a8a07e214d6c73`

PS2 HDD Manager's Fedora preparation path applies:

- `fast-format-v2` — avoids the historical whole-media stale-header scrub while retaining manager-side verification;
- `pfs-merge-v1` — idempotent directory merge and safe file replacement;
- `banked-atad-v1` — Extended APA bank-relative host I/O.

This is therefore a **modified pinned pfsshell backend**, not a stock binary.
