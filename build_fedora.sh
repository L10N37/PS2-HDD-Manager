#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$project_root/build/fedora"

missing=()
for command_name in cmake ninja g++; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        missing+=("$command_name")
    fi
done
if ! pkg-config --exists Qt6Widgets Qt6Network 2>/dev/null; then
    missing+=("Qt6Widgets/Qt6Network")
fi

if ((${#missing[@]} != 0)); then
    echo "Missing build dependencies: ${missing[*]}" >&2
    echo "Install them on Fedora with:" >&2
    echo "  sudo dnf install gcc-c++ cmake ninja-build qt6-qtbase-devel pkgconf-pkg-config" >&2
    exit 1
fi

cmake -S "$project_root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DPS2_HDD_BUILD_GUI=ON
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure

echo
echo "PS2 HDD Manager Fedora build and tests completed successfully."
echo "Executable: $build_dir/PS2-HDD-Manager"
echo "Privileged writer: $build_dir/PS2-HDD-Writer"

if [[ "${1:-}" == "--run" ]]; then
    exec "$build_dir/PS2-HDD-Manager"
fi
