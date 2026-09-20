#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Temporary build-selection helper for the contest-local robot voice channel.
#
# The contest robot_voice implementation intentionally provides the public
# voice_channel_*, voice_tts_{get,set}_backend and voice_asr_{get,set}_backend
# symbols used by ai_agent.  Upstream ai_agent also compiles its own voice
# implementation unconditionally, so both cannot be linked at the same time.
#
# This helper changes ONLY packages/ai_agent build metadata (Makefile and
# CMakeLists.txt) during the contest build.  It does not modify any .c/.h
# implementation file.  apply() saves byte-for-byte backups and restore()
# restores them on the build wrapper EXIT trap.
#
# Interface:
#   apply | restore | status

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT_DIR="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
readonly AGENT_DIR="${ROOT_DIR}/packages/ai_agent"
readonly MAKEFILE="${AGENT_DIR}/Makefile"
readonly CMAKEFILE="${AGENT_DIR}/CMakeLists.txt"

readonly STATE_DIR="${TMPDIR:-/tmp}/openvela-contest-voice-build-override"
readonly MARKER="${STATE_DIR}/root"
readonly MAKEFILE_BACKUP="${STATE_DIR}/Makefile"
readonly CMAKEFILE_BACKUP="${STATE_DIR}/CMakeLists.txt"
readonly MAKEFILE_PATCHED_HASH="${STATE_DIR}/Makefile.patched.sha256"
readonly CMAKEFILE_PATCHED_HASH="${STATE_DIR}/CMakeLists.txt.patched.sha256"

# Remove the complete upstream voice implementation, not just the three files
# that currently collide.  The remaining voice backend/audio objects belong to
# the same subsystem and may reference symbols from voice_tts/voice_asr.
readonly VOICE_SRCS=(
  "src/voice/voice_channel.c"
  "src/voice/voice_tts.c"
  "src/voice/voice_asr.c"
  "src/voice/volc_tts.c"
  "src/voice/volc_tts_ws.c"
  "src/voice/volc_asr.c"
  "src/voice/audio_capture.c"
  "src/voice/audio_playback.c"
)

die()
{
  echo "contest voice build override: $*" >&2
  exit 1
}

file_hash()
{
  sha256sum "$1" | awk '{print $1}'
}

require_targets()
{
  test -f "${MAKEFILE}" || die "missing ${MAKEFILE}"
  test -f "${CMAKEFILE}" || die "missing ${CMAKEFILE}"
}

state_matches_root()
{
  test -f "${MARKER}" && test "$(cat "${MARKER}")" = "${ROOT_DIR}"
}

clear_state()
{
  rm -f \
    "${MAKEFILE_BACKUP}" \
    "${CMAKEFILE_BACKUP}" \
    "${MAKEFILE_PATCHED_HASH}" \
    "${CMAKEFILE_PATCHED_HASH}" \
    "${MARKER}"
  rmdir "${STATE_DIR}" 2>/dev/null || true
}

metadata_is_original()
{
  local src

  for src in "${VOICE_SRCS[@]}"; do
    grep -Fq "${src}" "${MAKEFILE}" || return 1
    grep -Fq "${src}" "${CMAKEFILE}" || return 1
  done

  # Keep the command/channel layer and ai_agent core.  Those call the contest
  # implementation through the same public API names.
  grep -Fq 'src/channels/cmd_voice.c' "${MAKEFILE}" || return 1
  grep -Fq 'src/channels/cmd_voice.c' "${CMAKEFILE}" || return 1
  return 0
}

metadata_is_overridden()
{
  local src

  for src in "${VOICE_SRCS[@]}"; do
    ! grep -Fq "${src}" "${MAKEFILE}" || return 1
    ! grep -Fq "${src}" "${CMAKEFILE}" || return 1
  done

  grep -Fq 'src/channels/cmd_voice.c' "${MAKEFILE}" || return 1
  grep -Fq 'src/channels/cmd_voice.c' "${CMAKEFILE}" || return 1
  return 0
}

matches_backup_or_patched()
{
  local target="$1"
  local backup="$2"
  local patched_hash_file="$3"
  local current

  test -f "${target}" || return 1
  test -f "${backup}" || return 1
  current="$(file_hash "${target}")"

  if test "${current}" = "$(file_hash "${backup}")"; then
    return 0
  fi

  if test -s "${patched_hash_file}" &&
     test "${current}" = "$(cat "${patched_hash_file}")"; then
    return 0
  fi

  return 1
}

restore_saved_state()
{
  state_matches_root || return 1

  matches_backup_or_patched \
    "${MAKEFILE}" "${MAKEFILE_BACKUP}" "${MAKEFILE_PATCHED_HASH}" ||
    die "${MAKEFILE} changed after apply; refusing to overwrite unknown edits"

  matches_backup_or_patched \
    "${CMAKEFILE}" "${CMAKEFILE_BACKUP}" "${CMAKEFILE_PATCHED_HASH}" ||
    die "${CMAKEFILE} changed after apply; refusing to overwrite unknown edits"

  cp -p "${MAKEFILE_BACKUP}" "${MAKEFILE}"
  cp -p "${CMAKEFILE_BACKUP}" "${CMAKEFILE}"
  clear_state
}

apply_override()
{
  require_targets

  if state_matches_root; then
    if metadata_is_overridden; then
      echo "ALREADY_APPLIED"
      return 0
    fi

    # A previous interrupted build may have restored one target but not the
    # other. Recover only if the current bytes are known backup/patched bytes.
    restore_saved_state
  fi

  if metadata_is_overridden; then
    echo "ALREADY_APPLIED"
    return 0
  fi

  metadata_is_original ||
    die "ai_agent voice build metadata is not in the expected upstream form"

  mkdir -p "${STATE_DIR}"
  printf '%s\n' "${ROOT_DIR}" > "${MARKER}"
  cp -p "${MAKEFILE}" "${MAKEFILE_BACKUP}"
  cp -p "${CMAKEFILE}" "${CMAKEFILE_BACKUP}"

  python3 - "${MAKEFILE}" "${CMAKEFILE}" <<'PY'
from pathlib import Path
import sys

targets = [Path(sys.argv[1]), Path(sys.argv[2])]
voice = {
    "src/voice/voice_channel.c",
    "src/voice/voice_tts.c",
    "src/voice/voice_asr.c",
    "src/voice/volc_tts.c",
    "src/voice/volc_tts_ws.c",
    "src/voice/volc_asr.c",
    "src/voice/audio_capture.c",
    "src/voice/audio_playback.c",
}

for path in targets:
    lines = path.read_text(encoding="utf-8").splitlines(True)
    out = []
    removed = []
    for line in lines:
        stripped = line.strip()
        if stripped in voice:
            removed.append(stripped)
            continue
        # Makefile form: CSRCS += src/voice/foo.c
        if stripped.startswith("CSRCS +="):
            rhs = stripped.split("+=", 1)[1].strip()
            if rhs in voice:
                removed.append(rhs)
                continue
        out.append(line)

    missing = voice.difference(removed)
    if missing:
        raise SystemExit(
            f"{path}: expected voice source entries not all found: "
            + ", ".join(sorted(missing))
        )
    path.write_text("".join(out), encoding="utf-8")
PY

  if ! metadata_is_overridden; then
    cp -p "${MAKEFILE_BACKUP}" "${MAKEFILE}"
    cp -p "${CMAKEFILE_BACKUP}" "${CMAKEFILE}"
    clear_state
    die "post-apply validation failed"
  fi

  file_hash "${MAKEFILE}" > "${MAKEFILE_PATCHED_HASH}"
  file_hash "${CMAKEFILE}" > "${CMAKEFILE_PATCHED_HASH}"

  echo "APPLIED"
}

restore_override()
{
  require_targets

  if ! state_matches_root; then
    echo "REVERTED"
    return 0
  fi

  restore_saved_state
  echo "REVERTED"
}

status_override()
{
  require_targets

  if state_matches_root; then
    if metadata_is_overridden; then
      echo "APPLIED"
    else
      echo "DIRTY"
      return 1
    fi
  elif metadata_is_overridden; then
    echo "ALREADY_APPLIED"
  elif metadata_is_original; then
    echo "READY"
  else
    echo "DIRTY"
    return 1
  fi
}

case "${1:-}" in
  apply) apply_override ;;
  restore) restore_override ;;
  status) status_override ;;
  *) echo "Usage: $0 {apply|restore|status}" >&2; exit 2 ;;
esac
