#!/usr/bin/env bash
#
# scripts/mcp-launch.sh — repeatable launcher for an MCP-driven DOSBox-X session
# (Slice 12). Boots the freshly built src/dosbox-x with the TCP JSON-RPC server
# listening, in one of two launch modes (a runtime choice — the SAME --enable-mcp
# binary, only the SDL driver env differs):
#
#   --mode headless   (default)  SDL_VIDEODRIVER=dummy + SDL_AUDIODRIVER=dummy:
#                                 no window, no display/tty/audio. Used by CI and
#                                 scripts/mcp-check.sh.
#   --mode visible               drivers left unset: a real SDL window opens
#                                 (WSLg under WSL2) so you can watch the guest.
#
# The MCP server, tools and dispatcher are identical in both modes; the screen
# tools and input injection work the same regardless. See docs/MCP_MANUAL.md.
#
# Usage:
#   scripts/mcp-launch.sh --port 5022 [options] [-- extra dosbox-x args]
#
# Options:
#   --port N            TCP port for the MCP server on 127.0.0.1 (required).
#   --mode MODE         headless (default) | visible.
#   --mount DIR         mount DIR as drive C: and make it current.
#   --run "CMD"         run CMD in the guest at boot (repeatable; e.g. a program
#                       name or DOS command). Implies booting to the shell first.
#   --config FILE       load FILE as the dosbox-x.conf (-conf FILE).
#   --break-start       park the CPU at machine reset (debugger engaged) so
#                       parked-class tools work immediately.
#   --captures DIR      directory for screenshots/captures (default: a temp dir).
#   --                  everything after this is passed verbatim to dosbox-x.
#
# Everything is scriptable (guardrail #4): no manual steps. The binary must be
# built first (scripts/mcp-check.sh, or ./build-debug with --enable-mcp).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/src/dosbox-x"

fail() { printf 'mcp-launch: %s\n' "$*" >&2; exit 1; }

PORT=""
MODE="headless"
MOUNT=""
CONFIG=""
BREAK_START=0
CAPTURES=""
RUN_CMDS=()
PASSTHROUGH=()

while [ $# -gt 0 ]; do
    case "$1" in
        --port)        PORT="${2:-}"; shift 2 ;;
        --mode)        MODE="${2:-}"; shift 2 ;;
        --mount)       MOUNT="${2:-}"; shift 2 ;;
        --config)      CONFIG="${2:-}"; shift 2 ;;
        --captures)    CAPTURES="${2:-}"; shift 2 ;;
        --run)         RUN_CMDS+=("${2:-}"); shift 2 ;;
        --break-start) BREAK_START=1; shift ;;
        --)            shift; PASSTHROUGH=("$@"); break ;;
        -h|--help)     sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)             fail "unknown option: $1 (use -- to pass dosbox-x args)" ;;
    esac
done

[ -n "$PORT" ] || fail "--port is required"
[ -x "$BIN" ] || fail "dosbox-x binary not found at $BIN (build it first)"
case "$MODE" in headless|visible) ;; *) fail "--mode must be headless or visible" ;; esac

if [ -z "$CAPTURES" ]; then
    CAPTURES="$(mktemp -d "${TMPDIR:-/tmp}/mcp_launch_cap.XXXXXX")"
fi
mkdir -p "$CAPTURES"

# Launch mode toggles ONLY the SDL driver env (same binary, same MCP server).
if [ "$MODE" = "headless" ]; then
    export SDL_VIDEODRIVER=dummy
    export SDL_AUDIODRIVER=dummy
else
    unset SDL_VIDEODRIVER || true
    unset SDL_AUDIODRIVER || true
fi

# The MCP server is opt-in via MCP_PORT; setting it here is what makes the
# launched process listen (127.0.0.1 only — enforced in the server).
export MCP_PORT="$PORT"

ARGS=("$BIN" -set "dosbox captures=$CAPTURES")

[ -n "$CONFIG" ] && ARGS+=(-conf "$CONFIG")
[ "$BREAK_START" = "1" ] && ARGS+=(-break-start)

# Mount + per-boot commands are expressed as -c lines (the same AUTOEXEC the
# shell runs at boot), so a known target can be launched by name.
if [ -n "$MOUNT" ]; then
    [ -d "$MOUNT" ] || fail "--mount path is not a directory: $MOUNT"
    ARGS+=(-c "MOUNT C \"$MOUNT\"" -c "C:")
fi
for cmd in "${RUN_CMDS[@]:-}"; do
    [ -n "$cmd" ] && ARGS+=(-c "$cmd")
done

[ ${#PASSTHROUGH[@]} -gt 0 ] && ARGS+=("${PASSTHROUGH[@]}")

printf 'mcp-launch: mode=%s port=%s captures=%s\n' "$MODE" "$PORT" "$CAPTURES" >&2

# Dry run (used by the Slice 12 smoke test): print the resolved env + argv that
# WOULD be exec'd, without booting. Lets the integration test assert the launch
# mode toggles only the SDL driver env, with no display required.
if [ "${MCP_LAUNCH_DRYRUN:-0}" = "1" ]; then
    printf 'SDL_VIDEODRIVER=%s\n' "${SDL_VIDEODRIVER-<unset>}"
    printf 'SDL_AUDIODRIVER=%s\n' "${SDL_AUDIODRIVER-<unset>}"
    printf 'MCP_PORT=%s\n' "${MCP_PORT-<unset>}"
    printf 'ARGV='; printf '%q ' "${ARGS[@]}"; printf '\n'
    exit 0
fi

exec "${ARGS[@]}"
