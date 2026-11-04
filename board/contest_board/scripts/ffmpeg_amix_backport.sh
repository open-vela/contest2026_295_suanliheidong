#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Apply or reverse the contest-local FFmpeg amix helper build backport.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly OPENVELA_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
readonly FFMPEG_DIR="${OPENVELA_ROOT}/external/ffmpeg/ffmpeg"
readonly PATCH_FILE="${SCRIPT_DIR}/../patches/0025-ffmpeg-asubgraph-amix-helper.patch"
readonly PATCHED_FILE="libavfilter/Makefile"

die()
{
  echo "FFmpeg amix backport: $*" >&2
  exit 1
}

require_repository()
{
  test -f "${PATCH_FILE}" || die "patch not found: ${PATCH_FILE}"
  git -C "${FFMPEG_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    die "FFmpeg checkout not found: ${FFMPEG_DIR}"
}

require_clean_target()
{
  git -C "${FFMPEG_DIR}" diff --quiet -- "${PATCHED_FILE}" ||
    die "refusing to overwrite existing FFmpeg change: ${PATCHED_FILE}"
}

patch_is_applied()
{
  git -C "${FFMPEG_DIR}" apply --reverse --check "${PATCH_FILE}" >/dev/null 2>&1
}

apply_patch()
{
  require_repository

  if patch_is_applied; then
    echo "ALREADY_APPLIED"
    return 0
  fi

  require_clean_target
  git -C "${FFMPEG_DIR}" apply --check "${PATCH_FILE}" ||
    die "backport does not apply cleanly"
  git -C "${FFMPEG_DIR}" apply --whitespace=nowarn "${PATCH_FILE}"
  echo "APPLIED"
}

reverse_patch()
{
  require_repository

  if patch_is_applied; then
    git -C "${FFMPEG_DIR}" apply --reverse --whitespace=nowarn "${PATCH_FILE}"
    echo "REVERTED"
  else
    require_clean_target
    echo "NOT_APPLIED"
  fi
}

case "${1:-}" in
  apply)
    apply_patch
    ;;
  reverse)
    reverse_patch
    ;;
  status)
    require_repository
    if patch_is_applied; then
      echo "ALREADY_APPLIED"
    else
      require_clean_target
      git -C "${FFMPEG_DIR}" apply --check "${PATCH_FILE}" >/dev/null 2>&1 ||
        die "backport does not apply cleanly"
      echo "READY"
    fi
    ;;
  *)
    echo "Usage: $0 {apply|reverse|status}" >&2
    exit 2
    ;;
esac
