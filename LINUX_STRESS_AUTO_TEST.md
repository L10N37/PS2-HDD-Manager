# Linux large-library / AUTO test candidate

Base: c530f8dbeeb7a44f186df01b4cdbb649a6baf5c2

This candidate intentionally targets Fedora/Linux first.

Changes:
- Removes the false 256-header APA-chain ceiling.
- Uses hash-set loop detection instead of O(n^2) vector searching.
- Adds native per-bank 128 MiB APA chunk accounting.
- Adds native HDL allocation-fit estimation for AUTO routing.
- AUTO is the default game destination.
- AUTO rolls sequentially Bank 0 -> Bank 1 -> Bank 2... when a game no longer fits.
- A failed APA/game-table verification scan stops the queue before any further writes.
- PP.FHDB.APPS is selectable with a 4 GiB default.
- Creates /OPL/ROMS/NES and /OPL/SAVES/FCEUMM.
- Adds pinned FCEUmm-PS2 SMB v0.3.4-smb1 as a selectable/preconfigured app.
- hdl_dump failures retain exit/invocation/output diagnostics.

Important architecture note:
The existing mature HDL and PFS transfer backends are retained in this stress
candidate while native Ps2Hdl/Ps2Pfs replacements are developed and verified.
No new runtime backend dependency was added. Native APA walking, bank accounting
and AUTO bank selection are now handled by PS2 HDD Manager itself.
