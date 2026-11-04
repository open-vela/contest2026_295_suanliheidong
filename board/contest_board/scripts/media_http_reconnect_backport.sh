#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Apply or reverse the contest-local HTTP Range reconnect backport.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly OPENVELA_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
readonly MEDIA_DIR="${OPENVELA_ROOT}/frameworks/multimedia/media"
readonly PATCH_FILE="${SCRIPT_DIR}/../patches/0028-media-http-reconnect.patch"
readonly PATCHED_FILE="server/media_player.c"

die()
{
  echo "Media HTTP reconnect backport: $*" >&2
  exit 1
}

require_repository()
{
  test -f "${PATCH_FILE}" || die "patch not found: ${PATCH_FILE}"
  git -C "${MEDIA_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    die "Media checkout not found: ${MEDIA_DIR}"
}

patch_is_applied()
{
  git -C "${MEDIA_DIR}" apply --reverse --check "${PATCH_FILE}" >/dev/null 2>&1
}

apply_patch()
{
  require_repository
  if patch_is_applied; then
    echo "ALREADY_APPLIED"
    return 0
  fi

  git -C "${MEDIA_DIR}" apply --check "${PATCH_FILE}" ||
    die "backport conflicts with existing Media change: ${PATCHED_FILE}"
  git -C "${MEDIA_DIR}" apply --whitespace=nowarn "${PATCH_FILE}"
  echo "APPLIED"
}

reverse_patch()
{
  require_repository
  if patch_is_applied; then
    git -C "${MEDIA_DIR}" apply --reverse --whitespace=nowarn "${PATCH_FILE}"
    echo "REVERTED"
  else
    echo "NOT_APPLIED"
  fi
}

case "${1:-}" in
  apply) apply_patch ;;
  reverse) reverse_patch ;;
  status)
    require_repository
    if patch_is_applied; then echo "ALREADY_APPLIED"; else echo "READY"; fi
    ;;
  *) echo "Usage: $0 {apply|reverse|status}" >&2; exit 2 ;;
esac
