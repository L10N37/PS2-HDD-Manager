#!/usr/bin/env bash
set -euo pipefail

# PS2 HDD Manager 0.2.0 Fedora preparer.
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
pfsshell_patch_rev="fast-format-v2+pfs-merge-v1+banked-atad-v1"
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

    atad_c="$src/subprojects/fakeps2sdk/atad.c"
    python3 - "$atad_c" <<'PYPFSBANK'
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text()
s=s.replace('#include <fcntl.h>\n', '#include <fcntl.h>\n#include <ctype.h>\n', 1)
s=s.replace('static u32 hdd_length = 0; /* in sectors */', 'static u32 hdd_length = 0; /* selected virtual bank, sectors */\nstatic u64 hdd_base_sector = 0; /* physical sector base */', 1)
old = """void set_atad_device_path(const char *path)
{
    int fd;
    fd = open(path, O_RDWR | O_BINARY);
    if (fd == -1 || set_atad_device_handle(fd)) {
        perror(path), exit(1);
    }
}"""
new = """void set_atad_device_path(const char *path)
{
    const u64 BANK_SECTORS = ((u64)1 << 32);
    const u64 MAX_VIRTUAL_SECTORS = 0xfffffffeULL;
    const char *real_path = path;
    u64 bank_index = 0;
    int fd;
    if (strncmp(path, \"bank\", 4) == 0) {
        const char *cursor = path + 4;
        if (!isdigit((unsigned char)*cursor)) { fprintf(stderr, \"Invalid banked path: %s\\n\", path); exit(1); }
        while (isdigit((unsigned char)*cursor)) { bank_index = bank_index * 10 + (u64)(*cursor - '0'); if (bank_index > 255) exit(1); ++cursor; }
        if (*cursor != ':' || cursor[1] == '\\0') { fprintf(stderr, \"Invalid banked path: %s\\n\", path); exit(1); }
        real_path = cursor + 1;
    }
    fd = open(real_path, O_RDWR | O_BINARY);
    if (fd == -1 || set_atad_device_handle(fd)) { perror(real_path), exit(1); }
    hdd_base_sector = 0;
    if (real_path != path) {
        off_t size = lseek(fd, 0, SEEK_END);
        if (size == (off_t)-1) { perror(\"lseek\"); exit(1); }
        const u64 total = (u64)size / 512;
        const u64 base = bank_index * BANK_SECTORS;
        if (base >= total) { fprintf(stderr, \"Bank lies beyond disk\\n\"); exit(1); }
        u64 count = total - base; if (count > MAX_VIRTUAL_SECTORS) count = MAX_VIRTUAL_SECTORS;
        hdd_base_sector = base; hdd_length = (u32)count;
    }
}"""
if old not in s: raise SystemExit('Could not patch set_atad_device_path')
s=s.replace(old,new,1)
old2='    off_t pos = lseek(handle, (off_t)lba * 512, SEEK_SET);'
new2='    if ((u64)lba + nsectors > (u64)hdd_length) return (-1);\n    off_t pos = lseek(handle, (off_t)(hdd_base_sector + (u64)lba) * 512, SEEK_SET);'
if old2 not in s: raise SystemExit('Could not patch atad seek')
s=s.replace(old2,new2,1)
p.write_text(s)
PYPFSBANK

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
hdl_patch_rev="banked-hio-v1+scan-progress-v2+rename-self-collision-v1"
hdl_key="${hdl_commit}-${hdl_patch_rev}"
hdl_cache="$backend_cache/hdl-dump/$hdl_key"
hdl_cached_bin="$hdl_cache/bin/hdl_dump"
if [[ -x "$hdl_cached_bin" ]]; then
    echo "==> hdl-dump: cache hit ($hdl_patch_rev)"
else
    echo "==> hdl-dump: one-time build for pinned bank-aware revision"
    rm -rf "$hdl_cache"; mkdir -p "$hdl_cache"
    git clone https://github.com/ps2homebrew/hdl-dump.git "$hdl_cache/source"
    git -C "$hdl_cache/source" checkout --detach "$hdl_commit"
    python3 - "$hdl_cache/source/hio_win32.c" <<'PYHDLBANK'
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text()
s=s.replace("""typedef struct hio_win32_type
{
    hio_t hio;
    osal_handle_t device;
    unsigned long error_code; /* against osal_... */
} hio_win32_t;""","""typedef struct hio_win32_type
{
    hio_t hio;
    osal_handle_t device;
    unsigned long error_code; /* against osal_... */
    u_int64_t bank_base_sector;
    u_int64_t bank_sector_count;
} hio_win32_t;""",1)
s=s.replace("""    u_int64_t size_in_bytes;
    int result = osal_get_estimated_device_size(hw32->device, &size_in_bytes);
    if (result == OSAL_OK) {
        if (size_in_bytes / 1024 < (u_int32_t)0xffffffff)
            *size_in_kb = (u_int32_t)(size_in_bytes / 1024);
        else
            *size_in_kb = (u_int32_t)0xffffffff;""","""    u_int64_t size_in_bytes;
    int result = osal_get_estimated_device_size(hw32->device, &size_in_bytes);
    if (result == OSAL_OK) {
        const u_int64_t bank_size_in_kb = hw32->bank_sector_count / 2;
        (void)size_in_bytes;
        *size_in_kb = bank_size_in_kb < (u_int32_t)0xffffffff ? (u_int32_t)bank_size_in_kb : (u_int32_t)0xffffffff;""",1)
oldio="""    hio_win32_t *hw32 = (hio_win32_t *)hio;
    int result = osal_seek(hw32->device, (u_int64_t)start_sector * 512);"""
newio="""    hio_win32_t *hw32 = (hio_win32_t *)hio;
    if ((u_int64_t)start_sector + num_sectors > hw32->bank_sector_count) return RET_ERR;
    int result = osal_seek(hw32->device, (hw32->bank_base_sector + start_sector) * 512);"""
if s.count(oldio) < 2: raise SystemExit('Could not patch hdl I/O')
s=s.replace(oldio,newio,2)
s=s.replace('win32_alloc(osal_handle_t device) /*@allocates result@*/ /*@defines result@*/','win32_alloc(osal_handle_t device, u_int64_t bank_base_sector, u_int64_t bank_sector_count) /*@allocates result@*/ /*@defines result@*/',1)
s=s.replace("""        hw32->device = device;
    }""","""        hw32->device = device;
        hw32->bank_base_sector = bank_base_sector;
        hw32->bank_sector_count = bank_sector_count;
    }""",1)
needle="""int hio_win32_probe(const dict_t *config,
                    const char *path,
                    hio_t **hio)
{
    int result;"""
repl="""int hio_win32_probe(const dict_t *config,
                    const char *path,
                    hio_t **hio)
{
    const u_int64_t BANK_SECTORS = ((u_int64_t)1 << 32);
    const u_int64_t MAX_BANK_SECTORS = 0xfffffffeULL;
    const char *real_path = path;
    u_int64_t bank_index = 0;
    int result;
    if (strncmp(path, \"bank\", 4) == 0) {
        const char *cursor = path + 4;
        if (!isdigit((unsigned char)*cursor)) return RET_NOT_COMPAT;
        while (isdigit((unsigned char)*cursor)) { bank_index=bank_index*10+(u_int64_t)(*cursor-'0'); if(bank_index>255) return RET_BAD_DEVICE; ++cursor; }
        if (*cursor != ':' || cursor[1] == '\\0') return RET_BAD_DEVICE;
        real_path = cursor + 1;
    }"""
if needle not in s: raise SystemExit('hdl probe missing')
s=s.replace(needle,repl,1)
s=s.replace('result = osal_map_device_name(path, device_name);','result = osal_map_device_name(real_path, device_name);',1)
oldalloc="""            result = osal_open_device_for_writing(device_name, &device);
            if (result == OSAL_OK) {
                *hio = win32_alloc(device);
                if (*hio != NULL)
                    ; /* success */
                else
                    result = RET_NO_MEM;"""
newalloc="""            result = osal_open_device_for_writing(device_name, &device);
            if (result == OSAL_OK) {
                u_int64_t size_in_bytes = 0;
                result = osal_get_estimated_device_size(device, &size_in_bytes);
                if (result == OSAL_OK) {
                    const u_int64_t total = size_in_bytes / 512;
                    const u_int64_t base = bank_index * BANK_SECTORS;
                    if (base >= total) result = RET_BAD_DEVICE;
                    else {
                        u_int64_t count = total - base; if (count > MAX_BANK_SECTORS) count = MAX_BANK_SECTORS;
                        *hio = win32_alloc(device, base, count);
                        if (*hio == NULL) result = RET_NO_MEM;
                    }
                }"""
if oldalloc not in s: raise SystemExit('hdl allocation missing')
s=s.replace(oldalloc,newalloc,1)
p.write_text(s)
PYHDLBANK

    # PS2 HDD Manager v14b: opt-in real scan progress for hdl_toc.
    python3 - "$hdl_cache/source/hdl.c" <<'PYHDLSCANV14B'
from pathlib import Path
import sys

p = Path(sys.argv[1])
s = p.read_text()

if '#include <stdlib.h>' not in s:
    s = s.replace('#include <stdio.h>\n', '#include <stdio.h>\n#include <stdlib.h>\n', 1)

old_sig = ('static int\n'
           'hdl_glist_read_slice(hio_t *hio,\n'
           '                     hdl_games_list_t *glist,\n'
           '                     const apa_toc_t *toc,\n'
           '                     int slice_index)\n'
           '{')
new_sig = ('static int\n'
           'hdl_glist_read_slice(hio_t *hio,\n'
           '                     hdl_games_list_t *glist,\n'
           '                     const apa_toc_t *toc,\n'
           '                     int slice_index,\n'
           '                     size_t progress_total)\n'
           '{')
if old_sig in s:
    s = s.replace(old_sig, new_sig, 1)
elif new_sig not in s:
    raise SystemExit('Could not patch hdl_glist_read_slice signature')

old_loop = ('        if (get_u16(&part->flags) == 0x00 &&\n'
            '            get_u16(&part->type) == PS2_HDL_PARTITION)\n'
            '            result = hdl_ginfo_read(hio, slice_index, part,\n'
            '                                    glist->games + glist->count++);\n')
new_loop = ('        if (get_u16(&part->flags) == 0x00 &&\n'
            '            get_u16(&part->type) == PS2_HDL_PARTITION) {\n'
            '            result = hdl_ginfo_read(hio, slice_index, part,\n'
            '                                    glist->games + glist->count++);\n'
            '            if (getenv("PS2_HDD_SCAN_PROGRESS") != NULL &&\n'
            '                strcmp(getenv("PS2_HDD_SCAN_PROGRESS"), "1") == 0) {\n'
            '                fprintf(stderr, "HDL_SCAN_PROGRESS\\t%lu\\t%lu\\n",\n'
            '                        (unsigned long)glist->count,\n'
            '                        (unsigned long)progress_total);\n'
            '                fflush(stderr);\n'
            '            }\n'
            '        }\n')
if old_loop in s:
    s = s.replace(old_loop, new_loop, 1)
elif 'HDL_SCAN_PROGRESS' not in s:
    raise SystemExit('Could not patch HDL game-header loop')

old_count = ('        count = (hdl_games_count(toc->slice + 0) +\n'
             '                 hdl_games_count(toc->slice + 1));\n\n'
             '        tmp = osal_alloc(sizeof(hdl_game_info_t) * count);\n')
new_count = ('        count = (hdl_games_count(toc->slice + 0) +\n'
             '                 hdl_games_count(toc->slice + 1));\n\n'
             '        if (getenv("PS2_HDD_SCAN_PROGRESS") != NULL &&\n'
             '            strcmp(getenv("PS2_HDD_SCAN_PROGRESS"), "1") == 0) {\n'
             '            fprintf(stderr, "HDL_SCAN_TOTAL\\t%lu\\n",\n'
             '                    (unsigned long)count);\n'
             '            fflush(stderr);\n'
             '        }\n\n'
             '        tmp = osal_alloc(sizeof(hdl_game_info_t) * count);\n')
if old_count in s:
    s = s.replace(old_count, new_count, 1)
elif 'HDL_SCAN_TOTAL' not in s:
    raise SystemExit('Could not patch HDL game total')

s = s.replace('result = hdl_glist_read_slice(hio, *glist, toc, 0);',
              'result = hdl_glist_read_slice(hio, *glist, toc, 0, count);', 1)
s = s.replace('result = hdl_glist_read_slice(hio, *glist, toc, 1);',
              'result = hdl_glist_read_slice(hio, *glist, toc, 1, count);', 1)

for marker in ('HDL_SCAN_TOTAL', 'HDL_SCAN_PROGRESS', 'progress_total'):
    if marker not in s:
        raise SystemExit('hdl.c verification failed: ' + marker)

p.write_text(s)
PYHDLSCANV14B

    # PS2_HDD_MANAGER_RENAME_SELF_COLLISION_V1
    # Upstream hdl-dump treats the current APA partition as a duplicate when a
    # rename changes only title characters beyond the 16-character APA-ID
    # title prefix. Permit that exact same-partition match while retaining the
    # collision refusal for every other partition.
    python3 - "$hdl_cache/source/hdl.c" <<'PYHDLRENAMESELF'
from pathlib import Path
import sys

p = Path(sys.argv[1])
s = p.read_text(encoding="utf-8")

old = """            result = apa_find_partition(toc, part_id, &tmp_slice_index,
                                        &tmp_partition_index);
            if (result == RET_NOT_FOUND) {
                strcpy(part->header.id, part_id);
                set_u32(&part->header.checksum,
                        apa_partition_checksum(&part->header));
                part->modified = 1;
                result = RET_OK;
            } else if (result == RET_OK)
                /* partition with such name already exists */
                result = RET_PART_EXISTS;
"""

new = """            result = apa_find_partition(toc, part_id, &tmp_slice_index,
                                        &tmp_partition_index);
            if (result == RET_NOT_FOUND) {
                strcpy(part->header.id, part_id);
                set_u32(&part->header.checksum,
                        apa_partition_checksum(&part->header));
                part->modified = 1;
                result = RET_OK;
            } else if (result == RET_OK) {
                /*
                 * PS2_HDD_MANAGER_RENAME_SELF_COLLISION_V1
                 *
                 * hdl_pname() stores only the first 16 normalized title
                 * characters in the 32-byte APA ID. If a rename changes only
                 * later characters, apa_find_partition() finds this very same
                 * partition and upstream incorrectly reports RET_PART_EXISTS.
                 *
                 * Accept only an exact self-match. A match to any other APA
                 * partition remains a real collision and is still refused.
                 */
                const apa_partition_t *matched =
                        toc->slice[tmp_slice_index].parts +
                        tmp_partition_index;
                if (tmp_slice_index == slice_index && matched == part)
                    result = RET_OK;
                else
                    result = RET_PART_EXISTS;
            }
"""

if new in s:
    pass
elif old in s:
    s = s.replace(old, new, 1)
else:
    raise SystemExit(
        "Pinned hdl-dump rename block did not match the expected source. "
        "No backend was built."
    )

if "PS2_HDD_MANAGER_RENAME_SELF_COLLISION_V1" not in s:
    raise SystemExit("Rename self-collision patch verification failed.")

p.write_text(s, encoding="utf-8")
PYHDLRENAMESELF

    python3 - "$hdl_cache/source/apa.c" <<'PYAPASCANV14B'
from pathlib import Path
import sys

p = Path(sys.argv[1])
s = p.read_text()
start = s.find('static int\napa_slice_read(')
end = s.find('\n\n/**************************************************************/\nstatic int\napa_toc_read_internal', start)
if start < 0 or end < 0:
    raise SystemExit('Could not isolate apa_slice_read')
body = s[start:end]

needle = ('                    ++count;\n'
          '                    result = apa_part_add(slice, &part, 1, 1);\n')
insert = ('                    ++count;\n'
          '                    if (getenv("PS2_HDD_SCAN_PROGRESS") != NULL &&\n'
          '                        strcmp(getenv("PS2_HDD_SCAN_PROGRESS"), "1") == 0) {\n'
          '                        fprintf(stderr, "APA_SCAN_PROGRESS\\t%d\\t%u\\t%u\\n",\n'
          '                                slice_index, sector, total_sectors);\n'
          '                        fflush(stderr);\n'
          '                    }\n'
          '                    result = apa_part_add(slice, &part, 1, 1);\n')
if needle in body:
    body = body.replace(needle, insert, 1)
elif 'APA_SCAN_PROGRESS' not in body:
    raise SystemExit('Could not patch APA chain progress')

ret = '    return (result);\n}'
done = ('    if (getenv("PS2_HDD_SCAN_PROGRESS") != NULL &&\n'
        '        strcmp(getenv("PS2_HDD_SCAN_PROGRESS"), "1") == 0 &&\n'
        '        result == RET_OK) {\n'
        '        fprintf(stderr, "APA_SCAN_DONE\\t%d\\t%u\\n",\n'
        '                slice_index, total_sectors);\n'
        '        fflush(stderr);\n'
        '    }\n\n'
        '    return (result);\n'
        '}')
if ret in body:
    body = body.replace(ret, done, 1)
elif 'APA_SCAN_DONE' not in body:
    raise SystemExit('Could not patch APA completion')

s = s[:start] + body + s[end:]
p.write_text(s)
PYAPASCANV14B

    make -C "$hdl_cache/source" RELEASE=yes
    mkdir -p "$hdl_cache/bin"
    install -m 0755 "$hdl_cache/source/hdl_dump" "$hdl_cached_bin"
fi
mkdir -p "$project_root/tools/hdl-dump/bin"
install -m 0755 "$hdl_cached_bin" "$project_root/tools/hdl-dump/bin/hdl_dump"
printf '%s\n' "ps2homebrew/hdl-dump @ $hdl_commit" "patch=$hdl_patch_rev" > "$project_root/tools/hdl-dump/bin/BUILD-SOURCE.txt"

# ----- application ------------------------------------------------------------
echo "==> Building PS2 HDD Manager 0.1.0-alpha"
chmod +x "$project_root/build_fedora.sh" "$project_root/fetch_payloads.sh" "$project_root/create_freedvdboot_iso.sh"
"$project_root/build_fedora.sh"

echo "==> Fast-format regression test on exact 2,000,398,934,016-byte sparse image"
smoke_dir="$(mktemp -d /tmp/ps2-hdd-manager-2tb-smoke.XXXXXX)"
trap 'rm -rf "$smoke_dir"' EXIT
"$project_root/build/fedora/PS2-HDD-Writer" --smoke-image "$smoke_dir/test.img" \
    --pfsshell "$project_root/tools/pfsshell/bin/pfsshell"
rm -rf "$smoke_dir"; trap - EXIT

cat <<DONE

PS2 HDD Manager 0.1.0-alpha preparation complete.
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
