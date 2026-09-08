#!/usr/bin/env bash
set -uo pipefail

TREE="${HOME}/Downloads/PS2-HDD-Manager-linux-stress-auto"
APP="$TREE/build/fedora/PS2-HDD-Manager"

if [[ ! -x "$APP" ]]; then
    echo "PS2-HDD-Manager executable not found: $APP" >&2
    exit 1
fi

stamp="$(date +%Y%m%d-%H%M%S)"
base="${HOME}/Downloads/ps2-hdd-manager-debug-${stamp}"
trace="${base}-trace.log"
console="${base}-console.log"
coredump="${base}-coredump.txt"
bundle="${base}.tar.gz"

export PS2_HDD_DEBUG_LOG="$trace"
ulimit -c unlimited 2>/dev/null || true

echo "============================================================"
echo " PS2 HDD Manager DEBUG RUN"
echo "============================================================"
echo "Trace:   $trace"
echo "Console: $console"
echo
echo "Reproduce the crash normally. Logging is automatic."
echo

start="$(date --iso-8601=seconds)"

set +e
stdbuf -oL -eL "$APP" 2>&1 | tee "$console"
rc=${PIPESTATUS[0]}
set -e

{
    echo "PS2 HDD Manager exit code: $rc"
    echo "Debug run started: $start"
    echo "Debug run ended:   $(date --iso-8601=seconds)"
    echo
    echo "----- coredumpctl info -----"
    if command -v coredumpctl >/dev/null 2>&1; then
        coredumpctl info PS2-HDD-Manager \
            --since "$start" \
            --no-pager 2>&1 || true
    else
        echo "coredumpctl is not installed."
    fi
} > "$coredump"

files=()
[[ -f "$trace" ]] && files+=("$trace")
[[ -f "$console" ]] && files+=("$console")
[[ -f "$coredump" ]] && files+=("$coredump")

if ((${#files[@]})); then
    tar -czf "$bundle" "${files[@]}"
fi

echo
echo "============================================================"
echo " DEBUG RUN ENDED — exit code $rc"
echo "============================================================"
echo "Please send me this single bundle:"
echo "  $bundle"
echo
echo "It contains:"
echo "  - timestamped application trace"
echo "  - stdout/stderr"
echo "  - coredumpctl crash information / stack when available"
echo
echo "Do NOT reformat the HDD just because the GUI crashed."
exit "$rc"
