# pfsshell backend

`prepare_fedora_test.sh` checks out the maintained `ps2homebrew/pfsshell`
backend at the formatter-tested commit, initializes its submodules, builds it
natively on Fedora, and copies the executable to `bin/pfsshell`.

The third-party source tree and built binary are intentionally not bundled in
the PS2 HDD Manager source archive.
