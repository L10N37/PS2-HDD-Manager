#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
payload_root="$project_root/payload/runtime"
cache_root="${XDG_CACHE_HOME:-$HOME/.cache}/ps2-hdd-manager"
download_cache="$cache_root/downloads"
want_opl=0 want_wle=0 want_fhdb=0 want_enabler=0 want_mca=0 want_freedvdboot=0

usage() {
    cat <<'USAGE'
Usage: ./fetch_payloads.sh [--opl] [--wle] [--mca] [--fhdb] [--hdd-enabler] [--freedvdboot] [--all]

Downloads only selected optional payloads. Downloads/toolchains are persisted under
~/.cache/ps2-hdd-manager so subsequent PS2 HDD Manager builds reuse them.

  --opl            Latest Open PS2 Loader development ELF (tag: latest)
  --wle            Latest normal wLaunchELF_ISR BOOT.ELF (tag: latest)
  --mca            Latest Memory Card Annihilator packed ELF
  --fhdb           Pinned FreeHDBoot 1.966 HDD payload
  --hdd-enabler    Build the dedicated FHDB HDD Boot Configuration ELF
  --freedvdboot    CTurt FreeDVDBoot source profiles used by ISO creator
  --all            Prepare every item above
USAGE
}

[[ $# -gt 0 ]] || { usage >&2; exit 2; }
for arg in "$@"; do
    case "$arg" in
        --opl) want_opl=1 ;;
        --wle) want_wle=1 ;;
        --mca) want_mca=1 ;;
        --fhdb) want_fhdb=1 ;;
        --hdd-enabler) want_enabler=1 ;;
        --freedvdboot) want_freedvdboot=1; want_enabler=1 ;;
        --all) want_opl=1; want_wle=1; want_mca=1; want_fhdb=1; want_enabler=1; want_freedvdboot=1 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown argument: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

for cmd in curl python3 sha256sum; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "Missing required command: $cmd" >&2; exit 1; }
done
mkdir -p "$payload_root" "$download_cache"
manifest="$payload_root/MANIFEST.txt"
touch "$manifest"

resolve_release_asset() {
    local repo="$1" tag="$2" selector="$3" mode="${4:-exact}"
    local meta
    meta="$(mktemp)"
    curl -fsSL --retry 3 --connect-timeout 15 -H 'Accept: application/vnd.github+json' \
        "https://api.github.com/repos/${repo}/releases/tags/${tag}" -o "$meta"
    python3 - "$meta" "$selector" "$mode" <<'PY'
import json, sys
p, wanted, mode = sys.argv[1:]
with open(p, encoding='utf-8') as f: j=json.load(f)
assets=j.get('assets',[])
if mode == 'exact':
    found=next((a for a in assets if a.get('name') == wanted), None)
elif mode == 'prefix-zip':
    found=next((a for a in assets if a.get('name','').startswith(wanted) and a.get('name','').lower().endswith('.zip')), None)
else:
    found=None
if not found: raise SystemExit(f'No matching release asset: {wanted}')
for v in (found.get('browser_download_url',''), found.get('name',''), str(found.get('size',0)),
          found.get('digest') or '', j.get('name') or j.get('tag_name') or '', j.get('published_at') or ''):
    print(v)
PY
    rm -f "$meta"
}

cached_release_asset() {
    local repo="$1" tag="$2" selector="$3" mode="$4" destination="$5"
    local -a fields
    mapfile -t fields < <(resolve_release_asset "$repo" "$tag" "$selector" "$mode")
    if (( ${#fields[@]} < 6 )); then
        echo "Could not resolve GitHub release asset for ${repo} tag ${tag}: ${selector}" >&2
        return 1
    fi
    local url="${fields[0]}" asset="${fields[1]}" expected_size="${fields[2]}" digest="${fields[3]}"
    local release="${fields[4]}" published="${fields[5]}"
    local key="${repo//\//_}/${tag}"
    local cached="$download_cache/$key/$asset"
    mkdir -p "$(dirname "$cached")" "$(dirname "$destination")"

    local valid=0
    if [[ -f "$cached" && "$(stat -c %s "$cached")" == "$expected_size" ]]; then
        if [[ "$digest" == sha256:* ]]; then
            [[ "sha256:$(sha256sum "$cached" | awk '{print $1}')" == "$digest" ]] && valid=1
        else
            valid=1
        fi
    fi
    if (( valid )); then
        echo "    cache hit: $asset"
    else
        echo "    downloading: $asset ($release)"
        local tmp="${cached}.part"
        rm -f "$tmp"
        curl -fL --retry 3 --connect-timeout 15 "$url" -o "$tmp"
        [[ "$(stat -c %s "$tmp")" == "$expected_size" ]] || { echo "Size mismatch for $asset" >&2; rm -f "$tmp"; exit 1; }
        if [[ "$digest" == sha256:* ]]; then
            [[ "sha256:$(sha256sum "$tmp" | awk '{print $1}')" == "$digest" ]] || { echo "Digest mismatch for $asset" >&2; rm -f "$tmp"; exit 1; }
        fi
        mv -f "$tmp" "$cached"
    fi
    cp -f "$cached" "$destination"
    printf '%s | tag=%s | release=%s | published=%s | asset=%s | sha256=%s\n' \
        "$repo" "$tag" "$release" "$published" "$asset" "$(sha256sum "$cached" | awk '{print $1}')" >> "$manifest"
}

require_elf() {
    python3 - "$1" <<'PY'
import sys
with open(sys.argv[1],'rb') as f: magic=f.read(4)
if magic != b'\x7fELF': raise SystemExit(f'Not an ELF file: {sys.argv[1]}')
PY
}

if (( want_opl )); then
    echo "==> OPL Beta"
    mkdir -p "$payload_root/opl"
    cached_release_asset "ps2homebrew/Open-PS2-Loader" latest OPNPS2LD.ELF exact "$payload_root/opl/OPNPS2LD.ELF"
    require_elf "$payload_root/opl/OPNPS2LD.ELF"
fi

if (( want_wle )); then
    echo "==> wLaunchELF ISR"
    mkdir -p "$payload_root/wle"
    cached_release_asset "israpps/wLaunchELF_ISR" latest BOOT.ELF exact "$payload_root/wle/BOOT.ELF"
    require_elf "$payload_root/wle/BOOT.ELF"
fi

if (( want_mca )); then
    echo "==> Memory Card Annihilator"
    tmpdir="$(mktemp -d)"
    trap 'rm -rf "$tmpdir"' EXIT
    cached_release_asset "ffgriever-pl/Memory-Card-Annihilator" latest MemoryCardAnihilator_ prefix-zip "$tmpdir/mca.zip"
    python3 - "$tmpdir/mca.zip" "$tmpdir/unpacked" <<'PYZIP'
import os, sys, zipfile
src, dst = sys.argv[1:]
os.makedirs(dst, exist_ok=True)
with zipfile.ZipFile(src) as z:
    z.extractall(dst)
PYZIP
    mca="$(find "$tmpdir/unpacked" -type f -iname 'mca-packed.elf' -print -quit)"
    [[ -n "$mca" ]] || mca="$(find "$tmpdir/unpacked" -type f -iname 'mca.elf' -print -quit)"
    [[ -n "$mca" ]] || { echo "Memory Card Annihilator release layout changed; no mca-packed.elf/mca.elf found." >&2; exit 1; }
    mkdir -p "$payload_root/mca"
    install -m 0644 "$mca" "$payload_root/mca/BOOT.ELF"
    require_elf "$payload_root/mca/BOOT.ELF"
    printf '%s\n' "Memory Card Annihilator selected ELF | sha256=$(sha256sum "$payload_root/mca/BOOT.ELF" | awk '{print $1}')" >> "$manifest"
    rm -rf "$tmpdir"; trap - EXIT
fi

if (( want_fhdb )); then
    echo "==> FreeHDBoot 1.966"
    fhdb_commit="ac53a47a5c6eae675cc2611c7bebe62f56c7845c"
    fhdb_base="https://raw.githubusercontent.com/israpps/FreeMcBoot-Installer/${fhdb_commit}/installer_res/1966/INSTALL"
    mkdir -p "$payload_root/fhdb" "$download_cache/fhdb-$fhdb_commit"
    declare -A urls sizes
    urls[MBR.XLF]="$fhdb_base/SYSTEM/MBR.XLF"; sizes[MBR.XLF]=77360
    urls[FHDB.XLF]="$fhdb_base/SYSTEM/FHDB.XLF"; sizes[FHDB.XLF]=127536
    urls[ENDVDPL.XRX]="$fhdb_base/SYSTEM/ENDVDPL.XRX"; sizes[ENDVDPL.XRX]=128
    urls[FMCB_CFG.ELF]="$fhdb_base/SYS-CONF/FMCB_CFG.ELF"; sizes[FMCB_CFG.ELF]=136584
    urls[USBD.IRX]="$fhdb_base/SYS-CONF/USBD.IRX"; sizes[USBD.IRX]=26933
    urls[USBHDFSD.IRX]="$fhdb_base/SYS-CONF/USBHDFSD.IRX"; sizes[USBHDFSD.IRX]=48845
    for f in MBR.XLF FHDB.XLF ENDVDPL.XRX FMCB_CFG.ELF USBD.IRX USBHDFSD.IRX; do
        cached="$download_cache/fhdb-$fhdb_commit/$f"
        if [[ -f "$cached" && "$(stat -c %s "$cached")" == "${sizes[$f]}" ]]; then
            echo "    cache hit: $f"
        else
            echo "    downloading: $f"
            curl -fL --retry 3 --connect-timeout 15 "${urls[$f]}" -o "$cached.part"
            [[ "$(stat -c %s "$cached.part")" == "${sizes[$f]}" ]] || { echo "Unexpected size for $f" >&2; exit 1; }
            mv -f "$cached.part" "$cached"
        fi
        cp -f "$cached" "$payload_root/fhdb/$f"
    done
    printf '%s\n' "israpps/FreeMcBoot-Installer @ $fhdb_commit | FreeHDBoot 1.966 resources" >> "$manifest"
fi

ensure_ps2dev() {
    # Current PS2DEV uses the canonical triplet compiler name. Older aliases
    # such as ee-gcc are not guaranteed to exist.
    if command -v mips64r5900el-ps2-elf-gcc >/dev/null 2>&1 && [[ -n "${PS2SDK:-}" && -d "$PS2SDK" ]]; then
        return
    fi
    local tcroot="$cache_root/ps2dev-toolchain"
    local marker="$tcroot/.ready"
    local compiler=""

    # Reuse an already extracted archive even when 0.4.0 created a .ready
    # marker before validating the old ee-gcc alias.
    if [[ -d "$tcroot/unpacked" ]]; then
        compiler="$(find "$tcroot/unpacked" -type f -path '*/ee/bin/mips64r5900el-ps2-elf-gcc' -print -quit 2>/dev/null || true)"
    fi

    if [[ -z "$compiler" ]]; then
        echo "    one-time PS2DEV toolchain setup (cached for all later builds)"
        command -v tar >/dev/null 2>&1 || { echo "tar is required" >&2; exit 1; }
        mkdir -p "$tcroot"
        cached_release_asset "ps2dev/ps2dev" latest ps2dev-ubuntu-latest.tar.gz exact "$tcroot/toolchain.tar.gz"
        rm -rf "$tcroot/unpacked"; mkdir -p "$tcroot/unpacked"
        tar -xzf "$tcroot/toolchain.tar.gz" -C "$tcroot/unpacked"
        compiler="$(find "$tcroot/unpacked" -type f -path '*/ee/bin/mips64r5900el-ps2-elf-gcc' -print -quit)"
    fi

    [[ -n "$compiler" ]] || { echo "Cached PS2DEV archive did not contain ee/bin/mips64r5900el-ps2-elf-gcc" >&2; exit 1; }
    export PS2DEV="${compiler%/ee/bin/mips64r5900el-ps2-elf-gcc}"
    export PS2SDK="$PS2DEV/ps2sdk"
    export GSKIT="$PS2DEV/gsKit"
    export PATH="$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2DEV/dvp/bin:$PS2SDK/bin:$PATH"
    touch "$marker"
    echo "    PS2DEV compiler: $compiler"
}

if (( want_enabler )); then
    echo "==> Dedicated FHDB HDD Boot Configuration app"
    ensure_ps2dev
    command -v make >/dev/null 2>&1 || { echo "make is required" >&2; exit 1; }
    src="$project_root/ps2-tools/fhdb-boot-config"
    buildcache="$cache_root/fhdb-boot-config"
    source_hash="$(cat "$src/main.c" "$src/Makefile" | sha256sum | awk '{print $1}')"
    mkdir -p "$buildcache" "$payload_root/fhdb-enabler"
    if [[ -f "$buildcache/FHDB-Boot-Config.ELF" && -f "$buildcache/source.sha256" && "$(cat "$buildcache/source.sha256")" == "$source_hash" ]]; then
        echo "    cache hit: FHDB-Boot-Config.ELF"
    else
        echo "    compiling tiny PS2 ELF"
        make -C "$src" clean >/dev/null 2>&1 || true
        make -C "$src" -j"$(nproc)"
        require_elf "$src/FHDB-Boot-Config.ELF"
        cp -f "$src/FHDB-Boot-Config.ELF" "$buildcache/FHDB-Boot-Config.ELF"
        printf '%s\n' "$source_hash" > "$buildcache/source.sha256"
    fi
    cp -f "$buildcache/FHDB-Boot-Config.ELF" "$payload_root/fhdb-enabler/FHDB-Boot-Config.ELF"
    require_elf "$payload_root/fhdb-enabler/FHDB-Boot-Config.ELF"
    printf '%s\n' "PS2 HDD Manager dedicated FHDB HDD Boot Configuration | sha256=$(sha256sum "$payload_root/fhdb-enabler/FHDB-Boot-Config.ELF" | awk '{print $1}')" >> "$manifest"
fi

if (( want_freedvdboot )); then
    echo "==> FreeDVDBoot filesystem profiles"
    command -v git >/dev/null 2>&1 || { echo "git is required" >&2; exit 1; }
    fdroot="$cache_root/freedvdboot/source"
    if [[ ! -d "$fdroot/.git" ]]; then
        mkdir -p "$(dirname "$fdroot")"
        git clone --depth 1 https://github.com/CTurt/FreeDVDBoot.git "$fdroot"
    else
        git -C "$fdroot" fetch --depth 1 origin master
        git -C "$fdroot" reset --hard origin/master >/dev/null
    fi
    mkdir -p "$project_root/tools/freedvdboot"
    rm -rf "$project_root/tools/freedvdboot/source"
    ln -s "$fdroot" "$project_root/tools/freedvdboot/source"
    printf '%s\n' "CTurt/FreeDVDBoot @ $(git -C "$fdroot" rev-parse HEAD)" >> "$manifest"
fi

echo "==> Selected payloads ready: $payload_root"
echo "    persistent cache: $cache_root"
