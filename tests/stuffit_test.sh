#!/usr/bin/env bash
#
# stuffit_test.sh - byte-identical comparison of lib/stuffit/ extraction
# against unar (The Unarchiver CLI) for every sample archive, both forks.
#
# For each archive in $SIT_SAMPLES_DIR (defaults to
# ~/code/FujiNet_macOS_2026/sit_samples, override with an env var or the
# first argument so this keeps working if the samples move - and so any
# new sample dropped in there is picked up automatically), this:
#   1. runs `unsit -a` into a temp directory. Every entry's data fork is
#      extracted as "<path>"; an entry that also has a resource fork
#      additionally gets "<path>.rsrc" - a testing/inspection sidecar
#      convention unsit -a documents, not a real file format (see
#      tests/unsit.c's do_extract_all()).
#   2. runs `unar -D -f -q -o <dir>` (unar's default fork mode, "fork" -
#      real HFS+ resource forks via the com.apple.ResourceFork xattr on
#      the extracted data-fork file) into a second temp directory. unar
#      handles a .hqx (BinHex) input exactly the same as a bare .sit -
#      no special-casing needed here for that, only in unsit itself.
#   3. compares every non-".rsrc", non-zero-length file unsit produced
#      against unar's copy of the same relative path, byte for byte.
#   4. for every ".rsrc" sidecar unsit produced, compares it against the
#      com.apple.ResourceFork xattr unar attached to its counterpart
#      data-fork file, byte for byte (via `xattr -px` + `xxd -r -p`,
#      empirically confirmed on this machine to hold the same raw
#      resource-fork bytes `-xr` produces - see below for the `-k
#      visible` alternative that was tried first and rejected).
#
# Why xattr and not `-k visible`: unar's `-k visible` mode writes
# AppleDouble-format ".rsrc" sidecar files, which start with a fixed
# ~82-byte AppleDouble header before the actual resource-fork bytes
# (confirmed empirically: Risk.sit's 121429-byte resource fork came out
# as a 121511-byte "Risk.rsrc" under -k visible - exactly 82 bytes
# larger). unar's *default* fork mode ("fork") instead attaches the
# resource fork verbatim as the com.apple.ResourceFork extended
# attribute on the data-fork file, with no wrapper - confirmed
# empirically to match `unsit -xr` byte for byte for every case tried
# (data-only entries with no xattr at all, resource-only entries like
# Risk.sit's, and small "bcem" resource forks like Now_Software.sit's
# Install Disk *.img entries).

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SAMPLES_DIR="${1:-${SIT_SAMPLES_DIR:-$HOME/code/FujiNet_macOS_2026/sit_samples}}"
UNSIT="${UNSIT_BIN:-$SCRIPT_DIR/../build_stuffit_test_only/unsit}"
UNAR="${UNAR_BIN:-/opt/homebrew/bin/unar}"

if [ ! -x "$UNSIT" ]; then
    # Fall back to searching common build output locations.
    for cand in "$SCRIPT_DIR"/../build/unsit "$SCRIPT_DIR"/../build*/tests/unsit "$SCRIPT_DIR"/../build*/unsit; do
        if [ -x "$cand" ]; then UNSIT="$cand"; break; fi
    done
fi

if [ ! -x "$UNSIT" ]; then
    echo "stuffit_test.sh: cannot find the unsit binary (looked at $UNSIT and common build dirs)." >&2
    echo "Build it first, e.g.: cc -std=c99 -Wall -Wextra -Ilib -o /tmp/unsit tests/unsit.c lib/stuffit/*.c" >&2
    echo "then: UNSIT_BIN=/tmp/unsit $0" >&2
    exit 1
fi

if [ ! -x "$UNAR" ]; then
    echo "stuffit_test.sh: unar not found at $UNAR (install The Unarchiver CLI, or set UNAR_BIN)." >&2
    exit 1
fi

if [ ! -d "$SAMPLES_DIR" ]; then
    echo "stuffit_test.sh: samples directory not found: $SAMPLES_DIR" >&2
    exit 1
fi

shopt -s nullglob
samples=("$SAMPLES_DIR"/*)
shopt -u nullglob

if [ ${#samples[@]} -eq 0 ]; then
    echo "stuffit_test.sh: no sample archives found in $SAMPLES_DIR" >&2
    exit 1
fi

# A classic Mac HFS filename may legally contain a literal '/' - unar
# (matching Finder convention) writes such a name to POSIX disk with
# that character mapped to ':' instead. lib/stuffit's own e->path is
# documented as "raw bytes as stored" (stuffit.h), so an embedded '/'
# in a single component's real name is indistinguishable, on our side,
# from an actual folder boundary; unsit ends up building what looks
# like one extra path level where unar instead emits one flat
# component with a ':' in it (seen for real in Now_Software.sit's
# ".../Sample/Tutorial Calendar", whose true on-disk name is
# "Sample/Tutorial Calendar" as a single component). This helper tries
# the literal relative path first, then progressively folds trailing
# "/"-separated components together with ':' (matching unar's
# substitution) until it finds unar's actual file - a representation
# quirk, not a decompression difference (content still compared byte
# for byte once the counterpart is found).
find_unar_counterpart() {
    local dir="$1" rel="$2"
    if [ -e "$dir/$rel" ]; then
        printf '%s\n' "$dir/$rel"
        return 0
    fi

    local IFS='/'
    read -r -a parts <<< "$rel"
    local n=${#parts[@]}
    local k
    for ((k = n - 2; k >= 0; k--)); do
        local candidate=""
        local i
        for ((i = 0; i <= k; i++)); do
            [ -z "$candidate" ] && candidate="${parts[i]}" || candidate="$candidate/${parts[i]}"
        done
        for ((i = k + 1; i < n; i++)); do
            candidate="$candidate:${parts[i]}"
        done
        if [ -e "$dir/$candidate" ]; then
            printf '%s\n' "$dir/$candidate"
            return 0
        fi
    done

    return 1
}

total_files_compared=0
total_rsrc_compared=0
total_archives=0
failures=0

for archive in "${samples[@]}"; do
    [ -f "$archive" ] || continue
    base="$(basename "$archive")"
    total_archives=$((total_archives + 1))

    unsit_dir="$(mktemp -d "${TMPDIR:-/tmp}/stuffit_test_unsit.XXXXXX")"
    unar_dir="$(mktemp -d "${TMPDIR:-/tmp}/stuffit_test_unar.XXXXXX")"

    echo "== $base =="

    if ! "$UNSIT" -a "$archive" "$unsit_dir" >"$unsit_dir.log" 2>&1; then
        echo "  unsit -a exited nonzero (see $unsit_dir.log) - continuing, some entries may still have extracted"
    fi

    if ! "$UNAR" -D -f -q -o "$unar_dir" "$archive" >"$unar_dir.log" 2>&1; then
        echo "  FAIL: unar could not extract $archive (see $unar_dir.log)" >&2
        failures=$((failures + 1))
        rm -rf "$unsit_dir" "$unar_dir" "$unsit_dir.log" "$unar_dir.log"
        continue
    fi

    archive_files=0
    archive_rsrc=0
    archive_failures=0

    # --- data forks: every plain (non-".rsrc") file unsit produced ---
    while IFS= read -r -d '' f; do
        rel="${f#"$unsit_dir"/}"
        [ -s "$f" ] || continue   # skip zero-length files, nothing meaningful to compare

        if ! counterpart="$(find_unar_counterpart "$unar_dir" "$rel")"; then
            echo "  FAIL: $rel (data) - unsit extracted it but unar has no matching file" >&2
            archive_failures=$((archive_failures + 1))
            continue
        fi

        if ! cmp -s "$f" "$counterpart"; then
            echo "  FAIL: $rel (data) differs from unar's extraction" >&2
            archive_failures=$((archive_failures + 1))
            continue
        fi

        archive_files=$((archive_files + 1))
    done < <(find "$unsit_dir" -type f -name '*.rsrc' -prune -o -type f -print0)

    # --- resource forks: every ".rsrc" sidecar unsit produced, compared
    #     against unar's counterpart data file's resource-fork xattr ---
    while IFS= read -r -d '' f; do
        rel="${f#"$unsit_dir"/}"
        base_rel="${rel%.rsrc}"
        [ -s "$f" ] || continue   # skip zero-length resource forks

        if ! counterpart="$(find_unar_counterpart "$unar_dir" "$base_rel")"; then
            echo "  FAIL: $base_rel (rsrc) - unar has no counterpart file at all" >&2
            archive_failures=$((archive_failures + 1))
            continue
        fi

        rsrc_tmp="$(mktemp "${TMPDIR:-/tmp}/stuffit_test_rsrc.XXXXXX")"
        if ! xattr -px com.apple.ResourceFork "$counterpart" 2>/dev/null | tr -d ' \n' | xxd -r -p > "$rsrc_tmp" 2>/dev/null || [ ! -s "$rsrc_tmp" ]; then
            echo "  FAIL: $base_rel (rsrc) - unsit found a resource fork but unar's file has no com.apple.ResourceFork xattr" >&2
            archive_failures=$((archive_failures + 1))
            rm -f "$rsrc_tmp"
            continue
        fi

        if ! cmp -s "$f" "$rsrc_tmp"; then
            echo "  FAIL: $base_rel (rsrc) differs from unar's resource-fork xattr" >&2
            archive_failures=$((archive_failures + 1))
            rm -f "$rsrc_tmp"
            continue
        fi

        rm -f "$rsrc_tmp"
        archive_rsrc=$((archive_rsrc + 1))
    done < <(find "$unsit_dir" -type f -name '*.rsrc' -print0)

    echo "  compared $archive_files data fork(s), $archive_rsrc resource fork(s), $archive_failures mismatch(es)"
    total_files_compared=$((total_files_compared + archive_files))
    total_rsrc_compared=$((total_rsrc_compared + archive_rsrc))
    failures=$((failures + archive_failures))

    rm -rf "$unsit_dir" "$unar_dir" "$unsit_dir.log" "$unar_dir.log"
done

echo
echo "stuffit_test.sh summary: $total_archives archive(s), $total_files_compared data fork(s) and $total_rsrc_compared resource fork(s) compared, $failures failure(s)"

if [ "$failures" -ne 0 ]; then
    echo "stuffit_test.sh: FAILED" >&2
    exit 1
fi

echo "stuffit_test.sh: all extracted forks byte-identical to unar"
exit 0
