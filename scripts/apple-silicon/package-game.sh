#!/usr/bin/env bash
set -euo pipefail

# Pack an extracted Original Xbox game directory into a XISO ISO that xemu
# can load. Wraps `xdvdfs pack` with project-aware defaults: external
# game-library lookup by name, Test_Games-friendly output naming,
# overwrite protection, post-pack verification, and an autonomous
# install path for the xdvdfs binary.
#
# Examples:
#   scripts/apple-silicon/package-game.sh "Rainbow Six 3"
#   scripts/apple-silicon/package-game.sh --source /path/to/extracted --output out.iso
#   scripts/apple-silicon/package-game.sh --list
#   scripts/apple-silicon/package-game.sh --list "Rainbow"
#
# Project rules honored:
#   #1 no guessing - we verify the produced ISO with `xdvdfs info`.
#   #5 build tools when blocked - this is one such tool.
#   #9 never modify Test_Games or Xbox-Emulator-Files in place - the
#      external library is read-only (we only read), and the default
#      output refuses to overwrite an existing file without --force.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TEST_GAMES_DIR="${XEMU_TEST_GAMES_DIR:-/Users/jbbrack03/XEMU_MacOS/Test_Games}"
GAME_LIBRARY_DIR="${XEMU_GAME_LIBRARY:-/Volumes/Josh-Backup-Files/Console Games/Original Xbox}"
XDVDFS_BIN_DEFAULT="${XEMU_XDVDFS_BIN:-xdvdfs}"
INSTALL_ROOT="${XEMU_XDVDFS_INSTALL_ROOT:-$HOME/.cargo}"

usage() {
    cat <<EOF
usage: $0 [options] <game-name>
       $0 --source <dir> [--output <iso>] [options]
       $0 --list [filter]
       $0 --help

Packs an extracted Original Xbox game directory into a XISO ISO using
xdvdfs. Resolves <game-name> against the external game library (by
default \`/Volumes/Josh-Backup-Files/Console Games/Original Xbox\`).

Positional:
  <game-name>          Case-insensitive substring match against folder
                       names under the game library. Must match exactly
                       one folder unless --source is given.

Options:
  --source DIR         Use DIR as the extracted game tree (skips
                       library lookup). Must contain default.xbe.
  --output FILE        Output ISO path. Default:
                       \$XEMU_TEST_GAMES_DIR/<sanitized-name>.xiso.iso
                       (\$XEMU_TEST_GAMES_DIR defaults to
                       /Users/jbbrack03/XEMU_MacOS/Test_Games).
  --library DIR        Override the game library root (env:
                       XEMU_GAME_LIBRARY).
  --force              Overwrite the output ISO if it already exists.
  --no-verify          Skip post-pack \`xdvdfs info\` verification.
  --no-install         Do not auto-install xdvdfs if missing; fail
                       instead.
  --xdvdfs PATH        Use PATH as the xdvdfs binary (env:
                       XEMU_XDVDFS_BIN).
  --list [filter]      List games found under the library; if filter is
                       given, restrict to case-insensitive substring
                       matches. Does not pack anything.
  -h, --help           Show this help.

Environment variables:
  XEMU_GAME_LIBRARY        external library root
  XEMU_TEST_GAMES_DIR      default output directory
  XEMU_XDVDFS_BIN          xdvdfs binary path
  XEMU_XDVDFS_INSTALL_ROOT cargo --root for autoinstall (default: \$HOME/.cargo)

Exit codes:
  0  success (or already-packed no-op)
  1  runtime/IO error (missing source, pack failure, verify failure)
  2  usage error (bad args, ambiguous name, missing tooling without
     --no-install permission)
EOF
}

log() { printf '[package-game] %s\n' "$*"; }
err() { printf '[package-game] error: %s\n' "$*" >&2; }

GAME_NAME=""
SOURCE_DIR=""
OUTPUT=""
FORCE=0
VERIFY=1
ALLOW_INSTALL=1
LIST_MODE=0
LIST_FILTER=""
XDVDFS_BIN="$XDVDFS_BIN_DEFAULT"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source)
            [[ $# -ge 2 ]] || { err "--source requires an argument"; exit 2; }
            SOURCE_DIR="$2"; shift 2 ;;
        --output)
            [[ $# -ge 2 ]] || { err "--output requires an argument"; exit 2; }
            OUTPUT="$2"; shift 2 ;;
        --library)
            [[ $# -ge 2 ]] || { err "--library requires an argument"; exit 2; }
            GAME_LIBRARY_DIR="$2"; shift 2 ;;
        --xdvdfs)
            [[ $# -ge 2 ]] || { err "--xdvdfs requires an argument"; exit 2; }
            XDVDFS_BIN="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        --no-verify) VERIFY=0; shift ;;
        --no-install) ALLOW_INSTALL=0; shift ;;
        --list)
            LIST_MODE=1
            if [[ $# -ge 2 && "$2" != --* ]]; then
                LIST_FILTER="$2"; shift 2
            else
                shift
            fi
            ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        --*)
            err "unknown option: $1"; usage >&2; exit 2 ;;
        *)
            if [[ -z "$GAME_NAME" ]]; then
                GAME_NAME="$1"; shift
            else
                err "unexpected positional argument: $1"; exit 2
            fi
            ;;
    esac
done

ensure_xdvdfs() {
    if command -v "$XDVDFS_BIN" >/dev/null 2>&1; then
        return 0
    fi
    if [[ -x "$INSTALL_ROOT/bin/xdvdfs" ]]; then
        XDVDFS_BIN="$INSTALL_ROOT/bin/xdvdfs"
        return 0
    fi
    if [[ "$ALLOW_INSTALL" -ne 1 ]]; then
        err "xdvdfs not found and --no-install set; run \`cargo install xdvdfs-cli\`"
        return 2
    fi
    if ! command -v cargo >/dev/null 2>&1; then
        err "xdvdfs not found and cargo is not on PATH; install Rust toolchain first"
        return 2
    fi
    log "xdvdfs not found; installing via 'cargo install xdvdfs-cli --root $INSTALL_ROOT'"
    if ! cargo install xdvdfs-cli --root "$INSTALL_ROOT" >&2; then
        err "cargo install xdvdfs-cli failed"
        return 1
    fi
    if [[ -x "$INSTALL_ROOT/bin/xdvdfs" ]]; then
        XDVDFS_BIN="$INSTALL_ROOT/bin/xdvdfs"
        return 0
    fi
    if command -v "$XDVDFS_BIN_DEFAULT" >/dev/null 2>&1; then
        XDVDFS_BIN="$XDVDFS_BIN_DEFAULT"
        return 0
    fi
    err "xdvdfs install completed but binary still not found"
    return 1
}

list_library_dirs() {
    local lib="$1"
    if [[ ! -d "$lib" ]]; then
        return 1
    fi
    local entry
    for entry in "$lib"/*/; do
        [[ -d "$entry" ]] || continue
        local base
        base="$(basename "$entry")"
        case "$base" in
            .*|"DLC") continue ;;
        esac
        if [[ "$base" == "XBOX HDD ready"* ]] || [[ "$base" == XBOX*HDD* ]]; then
            local game
            for game in "$entry"*/; do
                [[ -d "$game" ]] || continue
                printf '%s\n' "$(basename "$game")|$game"
            done
        else
            printf '%s\n' "$base|$entry"
        fi
    done
}

if [[ "$LIST_MODE" -eq 1 ]]; then
    if [[ ! -d "$GAME_LIBRARY_DIR" ]]; then
        err "library not mounted: $GAME_LIBRARY_DIR"
        exit 1
    fi
    filter_lower=""
    if [[ -n "$LIST_FILTER" ]]; then
        filter_lower="$(printf '%s' "$LIST_FILTER" | tr '[:upper:]' '[:lower:]')"
    fi
    matches=0
    while IFS='|' read -r name path; do
        [[ -n "$name" ]] || continue
        if [[ -n "$filter_lower" ]]; then
            name_lower="$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')"
            [[ "$name_lower" == *"$filter_lower"* ]] || continue
        fi
        printf '%s\t%s\n' "$name" "${path%/}"
        matches=$((matches + 1))
    done < <(list_library_dirs "$GAME_LIBRARY_DIR" | sort)
    if [[ "$matches" -eq 0 ]]; then
        err "no matches in $GAME_LIBRARY_DIR${LIST_FILTER:+ for \"$LIST_FILTER\"}"
        exit 1
    fi
    exit 0
fi

if [[ -z "$SOURCE_DIR" && -z "$GAME_NAME" ]]; then
    usage >&2
    exit 2
fi

resolve_source_by_name() {
    local query="$1"
    local query_lower
    query_lower="$(printf '%s' "$query" | tr '[:upper:]' '[:lower:]')"
    if [[ ! -d "$GAME_LIBRARY_DIR" ]]; then
        err "library not mounted: $GAME_LIBRARY_DIR"
        return 1
    fi
    local exact_path=""
    local exact_name=""
    local matches=()
    while IFS='|' read -r name path; do
        [[ -n "$name" ]] || continue
        local name_lower
        name_lower="$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')"
        if [[ "$name_lower" == "$query_lower" ]]; then
            exact_name="$name"
            exact_path="${path%/}"
            break
        fi
        if [[ "$name_lower" == *"$query_lower"* ]]; then
            matches+=("$name|${path%/}")
        fi
    done < <(list_library_dirs "$GAME_LIBRARY_DIR")

    if [[ -n "$exact_path" ]]; then
        printf '%s\t%s\n' "$exact_name" "$exact_path"
        return 0
    fi
    if [[ "${#matches[@]}" -eq 0 ]]; then
        err "no game found matching \"$query\" under $GAME_LIBRARY_DIR"
        err "use --list [filter] to enumerate available games"
        return 1
    fi
    if [[ "${#matches[@]}" -gt 1 ]]; then
        err "ambiguous name \"$query\": ${#matches[@]} matches"
        local m
        for m in "${matches[@]}"; do
            printf '  %s\n' "${m%%|*}" >&2
        done
        err "narrow the query, pass the full folder name, or use --source"
        return 1
    fi
    printf '%s\t%s\n' "${matches[0]%%|*}" "${matches[0]##*|}"
    return 0
}

if [[ -n "$SOURCE_DIR" ]]; then
    if [[ ! -d "$SOURCE_DIR" ]]; then
        err "--source path is not a directory: $SOURCE_DIR"
        exit 1
    fi
    SOURCE_DIR="$(cd "$SOURCE_DIR" && pwd)"
    if [[ -n "$GAME_NAME" ]]; then
        log "ignoring positional name '$GAME_NAME' because --source was given"
    fi
    GAME_NAME="$(basename "$SOURCE_DIR")"
else
    if ! resolved="$(resolve_source_by_name "$GAME_NAME")"; then
        exit 1
    fi
    GAME_NAME="${resolved%%	*}"
    SOURCE_DIR="${resolved##*	}"
fi

if [[ ! -e "$SOURCE_DIR/default.xbe" ]]; then
    err "source missing default.xbe: $SOURCE_DIR"
    err "this does not look like an extracted Xbox game tree"
    exit 1
fi

if [[ -z "$OUTPUT" ]]; then
    if [[ ! -d "$TEST_GAMES_DIR" ]]; then
        err "default output dir does not exist: $TEST_GAMES_DIR"
        err "create it or pass --output explicitly"
        exit 1
    fi
    OUTPUT="$TEST_GAMES_DIR/${GAME_NAME}.xiso.iso"
fi

OUTPUT_DIR="$(dirname "$OUTPUT")"
if [[ ! -d "$OUTPUT_DIR" ]]; then
    err "output directory does not exist: $OUTPUT_DIR"
    exit 1
fi

if [[ -e "$OUTPUT" && "$FORCE" -ne 1 ]]; then
    log "already packed: $OUTPUT (pass --force to rebuild)"
    exit 0
fi

if ! ensure_xdvdfs; then
    rc=$?
    exit "$rc"
fi

XDVDFS_VERSION="$("$XDVDFS_BIN" --version 2>/dev/null | head -1 || true)"
log "game:    $GAME_NAME"
log "source:  $SOURCE_DIR"
log "output:  $OUTPUT"
log "xdvdfs:  $XDVDFS_BIN${XDVDFS_VERSION:+ ($XDVDFS_VERSION)}"

TMP_OUTPUT="${OUTPUT}.partial"
rm -f "$TMP_OUTPUT"

START_TS=$(date +%s)
if ! "$XDVDFS_BIN" pack "$SOURCE_DIR" "$TMP_OUTPUT"; then
    err "xdvdfs pack failed"
    rm -f "$TMP_OUTPUT"
    exit 1
fi
END_TS=$(date +%s)

if [[ "$VERIFY" -eq 1 ]]; then
    if ! "$XDVDFS_BIN" info "$TMP_OUTPUT" >/dev/null; then
        err "xdvdfs info verification failed for $TMP_OUTPUT"
        rm -f "$TMP_OUTPUT"
        exit 1
    fi
fi

mv "$TMP_OUTPUT" "$OUTPUT"

ISO_BYTES="$(stat -f%z "$OUTPUT" 2>/dev/null || stat -c%s "$OUTPUT" 2>/dev/null || echo 0)"
META_FILE="${OUTPUT}.meta.txt"
{
    echo "game: $GAME_NAME"
    echo "source: $SOURCE_DIR"
    echo "output: $OUTPUT"
    echo "iso_bytes: $ISO_BYTES"
    echo "packed_at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "pack_seconds: $((END_TS - START_TS))"
    echo "xdvdfs_version: ${XDVDFS_VERSION:-unknown}"
    echo "xdvdfs_bin: $XDVDFS_BIN"
    echo "verify: $VERIFY"
} > "$META_FILE"

log "packed:  $OUTPUT ($ISO_BYTES bytes, $((END_TS - START_TS))s)"
log "metadata: $META_FILE"
