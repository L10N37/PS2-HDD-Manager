# hdl-dump backend

Upstream:

https://github.com/ps2homebrew/hdl-dump

Pinned commit:

`32c296c69cf9c263fcbe035004aa28c345b3b279`

PS2 HDD Manager's Fedora preparation path applies:

- `banked-hio-v1` — Extended APA bank-relative host I/O;
- `scan-progress-v2` — progress during large populated HDL/APA scans;
- `rename-self-collision-v1` — safe title-only rename/self-collision handling.

This is therefore a **modified pinned hdl-dump backend**, not a stock binary.

Raw physical-disk operations are launched through `PS2-HDD-Writer`, which performs the manager's
target validation before invoking backend operations.
