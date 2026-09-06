#!/usr/bin/env bash
set -euo pipefail

# PS2 HDD Manager 0.5.0-alpha Fedora preparer.
# Host backends are cached persistently; this script never opens a physical HDD.

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cache_root="${XDG_CACHE_HOME:-$HOME/.cache}/ps2-hdd-manager"
backend_cache="$cache_root/backend"
run_after=0
skip_packages=0

for arg in "$@"; do
    case "$arg" in
        --run) run_after=1 ;;
        --skip-packages) skip_packages=1 ;;
        --help|-h)
            cat <<USAGE
Usage: ./prepare_fedora_test.sh [--run] [--skip-packages]

Backends and downloaded PS2 assets are persisted under:
  $cache_root

pfsshell and hdl-dump are rebuilt only when their pinned commit or our backend
patch revision changes. This script never writes a physical HDD.
USAGE
            exit 0 ;;
        *) echo "Unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
    echo "Run this preparation script as your normal desktop user, not sudo/root." >&2
    exit 1
fi

if (( skip_packages == 0 )); then
    if ! command -v dnf >/dev/null 2>&1; then
        echo "dnf was not found. This preparer is intended for Fedora." >&2
        exit 1
    fi
    echo "==> Checking Fedora dependencies"
    packages=(gcc gcc-c++ make cmake ninja-build qt6-qtbase-devel git meson pkgconf-pkg-config
        polkit smartmontools usbutils curl python3 7zip genisoimage tar)
    missing=()
    if command -v rpm >/dev/null 2>&1; then
        for package in "${packages[@]}"; do
            rpm -q "$package" >/dev/null 2>&1 || missing+=("$package")
        done
    else
        missing=("${packages[@]}")
    fi
    if (( ${#missing[@]} != 0 )); then
        echo "    installing missing packages: ${missing[*]}"
        sudo dnf install -y "${missing[@]}"
    else
        echo "    all Fedora packages already installed - skipping dnf"
    fi
fi

for c in git make meson ninja cmake gcc g++ pkg-config curl python3 7z genisoimage tar; do
    command -v "$c" >/dev/null 2>&1 || { echo "Missing required command: $c" >&2; exit 1; }
done
mkdir -p "$backend_cache"

# ----- pfsshell ---------------------------------------------------------------
pfsshell_commit="8c92467b3d715c3698f1f8ce63a8a07e214d6c73"
pfsshell_patch_rev="fast-format-v2+pfs-merge-v1"
pfsshell_key="${pfsshell_commit}-${pfsshell_patch_rev}"
pfsshell_cache="$backend_cache/pfsshell/$pfsshell_key"
pfsshell_cached_bin="$pfsshell_cache/bin/pfsshell"

if [[ -x "$pfsshell_cached_bin" ]]; then
    echo "==> pfsshell: cache hit ($pfsshell_patch_rev)"
else
    echo "==> pfsshell: one-time build for pinned backend revision"
    src="$pfsshell_cache/source"
    rm -rf "$pfsshell_cache"
    mkdir -p "$pfsshell_cache"
    git clone --recursive https://github.com/ps2homebrew/pfsshell.git "$src"
    git -C "$src" checkout --detach "$pfsshell_commit"
    git -C "$src" submodule update --init --recursive

    apa_fio="$src/external/ps2sdk/iop/hdd/apa/src/hdd_fio.c"
    python3 - "$apa_fio" <<'PYFAST'
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text()
start=s.find("    // clear apa headers\n")
loop=s.find("    for (i =", start)
end=s.find("    apaCacheFree(clink);", loop)
if min(start,loop,end) < 0: raise SystemExit("Could not locate APA scrub loop")
chunk=s[start:end]
if "totalLBA" not in chunk or "blkIoDmaTransfer" not in chunk: raise SystemExit("Unexpected APA scrub block")
replacement='''    // PS2_HDD_MANAGER_FAST_FORMAT\n    // Skip the historical full-media stale-header scrub. The newly linked APA\n    // chain and every standard PFS system partition are verified immediately.\n'''
p.write_text(s[:start]+replacement+s[end:])
PYFAST

    shell_c="$src/src/shell.c"
    python3 - "$shell_c" <<'PYPFS'
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text()
old='int result = iomanX_mkdir(tmp, 0777);\n    if (result < 0)\n        fprintf(stderr, "(!) %s: %s.\\n", tmp, strerror(-result));\n    return (result);'
new='int result = iomanX_mkdir(tmp, 0777);\n    // PS2_HDD_MANAGER_PFS_MERGE: merging a host folder into an existing OPL folder is success.\n    if (result == -EEXIST)\n        return 0;\n    if (result < 0)\n        fprintf(stderr, "(!) %s: %s.\\n", tmp, strerror(-result));\n    return (result);'
if old not in s: raise SystemExit('Could not patch idempotent mkdir')
s=s.replace(old,new,1)
old2='int out = iomanX_open(tmp, FIO_O_WRONLY | FIO_O_CREAT, 0666);'
new2='int out = iomanX_open(tmp, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666); // PS2_HDD_MANAGER_PFS_REPLACE'
if old2 not in s: raise SystemExit('Could not patch PFS put truncation')
s=s.replace(old2,new2,1)
p.write_text(s)
PYPFS

    build="$pfsshell_cache/build"
    meson setup "$build" "$src" --buildtype=release -Denable_pfsfuse=false \
        -Denable_pfs2tar=false -Denable_pfsd=false -Denable_ps2kinst=false
    meson compile -C "$build"
    built="$(find "$build" -type f -name pfsshell -perm -u+x -print -quit)"
    [[ -n "$built" ]] || { echo "pfsshell executable not found after build" >&2; exit 1; }
    mkdir -p "$pfsshell_cache/bin"
    install -m 0755 "$built" "$pfsshell_cached_bin"
fi
mkdir -p "$project_root/tools/pfsshell/bin"
install -m 0755 "$pfsshell_cached_bin" "$project_root/tools/pfsshell/bin/pfsshell"
printf '%s\n' "ps2homebrew/pfsshell @ $pfsshell_commit" "patch=$pfsshell_patch_rev" > "$project_root/tools/pfsshell/bin/BUILD-SOURCE.txt"

# ----- hdl-dump ---------------------------------------------------------------
hdl_commit="32c296c69cf9c263fcbe035004aa28c345b3b279"
hdl_cache="$backend_cache/hdl-dump/$hdl_commit"
hdl_cached_bin="$hdl_cache/bin/hdl_dump"
if [[ -x "$hdl_cached_bin" ]]; then
    echo "==> hdl-dump: cache hit"
else
    echo "==> hdl-dump: one-time build for pinned revision"
    rm -rf "$hdl_cache"; mkdir -p "$hdl_cache"
    git clone https://github.com/ps2homebrew/hdl-dump.git "$hdl_cache/source"
    git -C "$hdl_cache/source" checkout --detach "$hdl_commit"
    make -C "$hdl_cache/source" RELEASE=yes
    mkdir -p "$hdl_cache/bin"
    install -m 0755 "$hdl_cache/source/hdl_dump" "$hdl_cached_bin"
fi
mkdir -p "$project_root/tools/hdl-dump/bin"
install -m 0755 "$hdl_cached_bin" "$project_root/tools/hdl-dump/bin/hdl_dump"
printf '%s\n' "ps2homebrew/hdl-dump @ $hdl_commit" > "$project_root/tools/hdl-dump/bin/BUILD-SOURCE.txt"

# ----- application ------------------------------------------------------------
echo "==> Building PS2 HDD Manager 0.5.0-alpha"
chmod +x "$project_root/build_fedora.sh" "$project_root/fetch_payloads.sh" "$project_root/create_freedvdboot_iso.sh"
"$project_root/build_fedora.sh"

echo "==> Fast-format regression test on exact 2,000,398,934,016-byte sparse image"
smoke_dir="$(mktemp -d /tmp/ps2-hdd-manager-2tb-smoke.XXXXXX)"
trap 'rm -rf "$smoke_dir"' EXIT
"$project_root/build/fedora/PS2-HDD-Writer" --smoke-image "$smoke_dir/test.img" \
    --pfsshell "$project_root/tools/pfsshell/bin/pfsshell"
rm -rf "$smoke_dir"; trap - EXIT

cat <<DONE

PS2 HDD Manager 0.5.0-alpha preparation complete.
Nothing was written to a physical HDD.

Persistent cache: $cache_root
  * pfsshell is reused until its pinned commit/patch changes
  * hdl-dump is reused until its pinned commit changes
  * downloaded OPL/wLaunchELF/MCA/FHDB assets are reused
  * the PS2DEV toolchain for the tiny FHDB config ELF is downloaded only once, if requested

Launch:
  $project_root/build/fedora/PS2-HDD-Manager
DONE

if (( run_after != 0 )); then
    exec "$project_root/build/fedora/PS2-HDD-Manager"
fi
