#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$project_root/build/arch"

missing=()
for command_name in cmake ninja g++; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        missing+=("$command_name")
    fi
done
if ! pkg-config --exists Qt6Widgets Qt6Network 2>/dev/null; then
    missing+=("Qt6Widgets/Qt6Network (qt6-base)")
fi

if ((${#missing[@]} != 0)); then
    echo "Missing build dependencies: ${missing[*]}" >&2
    echo "Install them on Arch Linux with:" >&2
    echo "  sudo pacman -S --needed base-devel gcc cmake ninja qt6-base pkgconf" >&2
    exit 1
fi

cmake -S "$project_root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DPS2_HDD_BUILD_GUI=ON
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure

echo
echo "PS2 HDD Manager Arch Linux build and tests completed successfully."
echo "Executable: $build_dir/PS2-HDD-Manager"
echo "Privileged writer: $build_dir/PS2-HDD-Writer"

if [[ "${1:-}" == "--run" ]]; then
    exec "$build_dir/PS2-HDD-Manager"
fi
