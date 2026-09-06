# hdl-dump backend

`prepare_fedora_test.sh` clones and builds the maintained `ps2homebrew/hdl-dump` command-line tool
at the pinned revision used by PS2 HDD Manager 0.1.0-alpha, then places the executable in `bin/`.
The GUI invokes it only through `PS2-HDD-Writer`, which repeats the target disk safety checks first.
