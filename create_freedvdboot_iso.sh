#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
payload_root="$project_root/payload/runtime"
profile=""
output=""

usage() {
    cat <<'USAGE'
Usage: ./create_freedvdboot_iso.sh --profile PROFILE --output FILE [--payload-dir DIR]

PROFILE:
  slim       All PS2 slims, DVD Player 3.10/3.11, English (direct enabler boot)
  2.10-2.13  FreeDVDBoot 2.10-2.13 profile (experimental/manual launch)
  3.04M      FreeDVDBoot 3.04M+ English profile (experimental/manual launch)

The slim image replaces FreeDVDBoot's initial uLaunchELF with the HDD-boot enabler
and preserves that uLaunchELF as ULE-DVD.ELF. Phat custom-disc setup is not fully
documented upstream, so those profiles retain the upstream initial loader and carry
FHDB-HDD-BOOT-CONFIG.ELF for manual launch. Compatibility still depends on DVD Player.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --profile) profile="${2:-}"; shift 2 ;;
        --output) output="${2:-}"; shift 2 ;;
        --payload-dir) payload_root="${2:-}"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done
[[ -n "$profile" && -n "$output" ]] || { usage >&2; exit 2; }
command -v genisoimage >/dev/null 2>&1 || { echo "genisoimage is required." >&2; exit 1; }
command -v git >/dev/null 2>&1 || { echo "git is required." >&2; exit 1; }

case "$profile" in
    slim) source_rel="Filesystems/All PS2 slims (3.10 + 3.11) - English language" ;;
    2.10-2.13) source_rel="Filesystems/2.10-2.13" ;;
    3.04M) source_rel="Filesystems/3.04M+ - English language" ;;
    *) echo "Unsupported FreeDVDBoot profile: $profile" >&2; exit 2 ;;
esac

fdroot="$project_root/tools/freedvdboot/source"
if [[ ! -d "$fdroot/.git" ]]; then
    echo "==> FreeDVDBoot source is not cached; fetching it now"
    rm -rf "$fdroot"
    git clone --depth 1 https://github.com/CTurt/FreeDVDBoot.git "$fdroot"
fi
source_dir="$fdroot/$source_rel"
[[ -d "$source_dir" ]] || { echo "FreeDVDBoot profile not found: $source_dir" >&2; exit 1; }

enabler="$payload_root/fhdb-enabler/FHDB-Boot-Config.ELF"
[[ -s "$enabler" ]] || { echo "Missing dedicated HDD boot configuration ELF: $enabler" >&2; exit 1; }

if [[ -e "$output" ]]; then
    echo "Refusing to overwrite existing ISO: $output" >&2
    exit 1
fi
mkdir -p "$(dirname "$output")"
work="$(mktemp -d /tmp/ps2-freedvdboot.XXXXXX)"
trap 'rm -rf "$work"' EXIT
cp -a "$source_dir/." "$work/disc/"
cp "$enabler" "$work/disc/FHDB-HDD-BOOT-CONFIG.ELF"

# CTurt documents direct initial-program replacement for the all-Slim filesystem:
# VIDEO_TS/VTS_02_0.IFO is the ELF loaded by the exploit. Our dedicated utility is
# a self-contained ELF, so no sidecar runtime files are required. Preserve the
# upstream DVD-capable uLaunchELF at disc root.
if [[ "$profile" == "slim" ]]; then
    [[ -s "$work/disc/VIDEO_TS/VTS_02_0.IFO" ]] || {
        echo "Slim FreeDVDBoot initial ELF was not found; upstream layout changed." >&2
        exit 1
    }
    cp "$work/disc/VIDEO_TS/VTS_02_0.IFO" "$work/disc/ULE-DVD.ELF"
    cp "$enabler" "$work/disc/VIDEO_TS/VTS_02_0.IFO"
fi

cat > "$work/disc/FHDB-HDD-BOOT-CONFIG-README.TXT" <<'TXT'
PS2 HDD MANAGER - FHDB HDD BOOT CONFIGURATION

On the all-Slim image the exploit boots the dedicated utility directly.
On experimental phat custom images, use the upstream ELF launcher to run:
  FHDB-HDD-BOOT-CONFIG.ELF

The utility starts read-only, displays current status, and provides explicit
Enable, Disable and Re-read/Verify controls with post-write verification.
TXT

tmpiso="$output.part"
rm -f "$tmpiso"
echo "==> Creating FreeDVDBoot ISO ($profile)"
genisoimage -quiet -udf -o "$tmpiso" "$work/disc"
mv "$tmpiso" "$output"
echo "FreeDVDBoot ISO created: $output"
if [[ "$profile" == "slim" ]]; then
    echo "Slim profile: configured to boot the HDD-boot enabler directly."
else
    echo "Phat profile: experimental custom disc; launch FHDB-HDD-BOOT-CONFIG.ELF manually from the upstream loader."
fi
echo "NOTE: FreeDVDBoot support depends on the PS2 DVD Player version/profile."
