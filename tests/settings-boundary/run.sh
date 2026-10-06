#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
root="$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)"
export ZEPHYR_BASE="${ZEPHYR_BASE:-$(west list zephyr -f '{abspath}')}"
zmk_root="${ZMK_ROOT:-$(west list zmk -f '{abspath}')}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT HUP INT TERM
out="${CUSTOM_SETTINGS_NATIVE_ARTIFACTS:-$work/results}"
failed=0
for variant in core features; do
    folder="$out/$variant"
    if [ -e "$folder/status.tsv" ]; then
        echo "Use a fresh CUSTOM_SETTINGS_NATIVE_ARTIFACTS directory to preserve evidence" >&2
        exit 2
    fi
    features=OFF
    cases=(control scope save discard reset discard-error persist-error missing)
    if [ "$variant" = features ]; then
        features=ON
        cases+=(array-error blob-error keyspace-error)
    fi
    args=()
    if [ -n "${CUSTOM_SETTINGS_SOURCE_DIR:-}" ]; then
        args+=(-DCUSTOM_SETTINGS_SOURCE_DIR="$CUSTOM_SETTINGS_SOURCE_DIR")
    fi
    if ! west build -p=always -b native_sim/native/64 -s "$root/tests/settings-boundary" \
        -d "$work/$variant" -- -DZMK_ROOT="$zmk_root" -DZEPHYR_MODULES= \
        -DSETTINGS_BOUNDARY_FEATURES="$features" "${args[@]}" \
        -DZephyr_DIR="$ZEPHYR_BASE/share/zephyr-package/cmake" > "$work/build.log" 2>&1; then
        cat "$work/build.log" >&2; exit 2
    fi
    mkdir -p "$folder"
    cp "$work/build.log" "$work/$variant/CMakeCache.txt" "$folder/"
    cp "$work/$variant/zephyr/.config" "$folder/config"
    cp "$work/$variant/zephyr/zephyr.exe" "$work/$variant/zephyr/zephyr.elf" "$folder/"
    for scenario in "${cases[@]}"; do
        for run in 1 2 3; do
            status=0
            CUSTOM_SETTINGS_CASE="$scenario" timeout 15 "$work/$variant/zephyr/zephyr.exe" \
                > "$folder/$scenario-$run.log" 2>&1 || status=$?
            cat "$folder/$scenario-$run.log"
            printf '%s %s %s\n' "$scenario" "$run" "$status" >> "$folder/status.tsv"
            [ "$status" = 0 ] || failed=1
        done
    done
    echo "Completed $variant boundary scenarios=${#cases[@]} repeats=3"
done
exit "$failed"
