#!/bin/sh
# CLI errors and actual data-root selection, without game data or a GPU.
set -eu
exe=${1:-./nfsu2}
for value in missing empty option; do
    case $value in
        missing) set -- --path ;;
        empty) set -- --path '' ;;
        option) set -- --path --hud ;;
    esac
    code=0
    output=$("$exe" "$@" 2>&1) || code=$?
    if [ "$code" -ne 2 ]; then
        echo "data_path_cli_test: FAIL $value operand (exit $code)" >&2; exit 1
    fi
    case $output in *'--path requires'*) ;; *) echo 'missing --path diagnostic' >&2; exit 1 ;; esac
done
temp=$(mktemp -d "${TMPDIR:-/tmp}/openug2-path-XXXXXX")
trap 'rmdir "$temp"' EXIT HUP INT TERM
root="$temp/data with spaces"
for form in positional named after; do
    case $form in
        positional) set -- "$root" --carinfo __PATH_TEST__ ;;
        named) set -- --path "$root" --carinfo __PATH_TEST__ ;;
        after) set -- --carinfo __PATH_TEST__ --path "$root" ;;
    esac
    code=0
    output=$("$exe" "$@" 2>&1) || code=$?
    [ "$code" -eq 1 ] || { echo "unexpected carinfo exit $code" >&2; exit 1; }
    case $output in *"$root/CARS/__PATH_TEST__/GEOMETRY.BIN"*) ;; *) echo "wrong data root: $form" >&2; exit 1 ;; esac
done
code=0
output=$("$exe" --path "$root" --hud --devui 2>&1) || code=$?
[ "$code" -eq 1 ] || { echo "unexpected missing-world exit $code" >&2; exit 1; }
for file in L4RA.BUN STREAML4RA.BUN; do
    case $output in
        *"$root/TRACKS/$file (READ FAILED, 0 bytes)"*) ;;
        *) echo "missing startup file diagnostic: $file" >&2; exit 1 ;;
    esac
done
for flag in --help -h; do
    code=0
    output=$("$exe" "$flag" 2>&1) || code=$?
    [ "$code" -eq 0 ] || { echo "unexpected $flag exit $code" >&2; exit 1; }
    case $output in *'Usage: nfsu2'*) ;; *) echo "missing usage for $flag" >&2; exit 1 ;; esac
done
# A misspelt option or a value option given last must fail before any data
# loading, never be taken as the data directory.
for args in "--trak STREAML4RB" "--shot" "--path /nonexistent-openug2-root --car"; do
    code=0
    # shellcheck disable=SC2086
    output=$("$exe" $args 2>&1) || code=$?
    [ "$code" -eq 2 ] || { echo "unexpected exit $code for: $args" >&2; exit 1; }
    case $output in *'unknown option or missing value'*) ;; *) echo "missing unknown-option diagnostic: $args" >&2; exit 1 ;; esac
    case $output in *'READ FAILED'*) echo "option taken as data root: $args" >&2; exit 1 ;; esac
done
# Negative option values are still values, not unknown options.
code=0
output=$("$exe" --path "$root" --heading -101 --spawn -12,5 2>&1) || code=$?
[ "$code" -eq 1 ] || { echo "unexpected negative-value exit $code" >&2; exit 1; }
case $output in *'unknown option'*) echo 'negative value rejected' >&2; exit 1 ;; esac
echo 'data_path_cli_test: PASS'
