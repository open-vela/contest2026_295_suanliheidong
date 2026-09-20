#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Apply or reverse the board-local Xtensa BREAK/BREAK.N handler backport.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly OPENVELA_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
readonly NUTTX_DIR="${OPENVELA_ROOT}/nuttx"
readonly PATCH_FILE="${SCRIPT_DIR}/../patches/0002-nuttx-xtensa-break-handler-backport.patch"
readonly PATCHED_FILE="arch/xtensa/src/common/xtensa_debug.c"

die()
{
  echo "NuttX Xtensa BREAK backport: $*" >&2
  exit 1
}

require_repository()
{
  test -f "${PATCH_FILE}" || die "patch not found: ${PATCH_FILE}"
  git -C "${NUTTX_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    die "NuttX checkout not found: ${NUTTX_DIR}"
}

patch_is_applied()
{
  git -C "${NUTTX_DIR}" apply --reverse --check "${PATCH_FILE}" >/dev/null 2>&1
}

patch_is_ready()
{
  git -C "${NUTTX_DIR}" apply --check "${PATCH_FILE}" >/dev/null 2>&1
}

apply_patch()
{
  require_repository

  if patch_is_applied; then
    echo "ALREADY_APPLIED"
    return
  fi

  patch_is_ready || die "backport does not apply cleanly to ${PATCHED_FILE}"
  git -C "${NUTTX_DIR}" apply --whitespace=nowarn "${PATCH_FILE}"
  echo "APPLIED"
}

reverse_patch()
{
  require_repository

  if patch_is_applied; then
    git -C "${NUTTX_DIR}" apply --reverse --check "${PATCH_FILE}" ||
      die "backport cannot be reversed cleanly from ${PATCHED_FILE}"
    git -C "${NUTTX_DIR}" apply --reverse --whitespace=nowarn "${PATCH_FILE}"
    echo "REVERTED"
  elif patch_is_ready; then
    echo "REVERTED"
  else
    die "${PATCHED_FILE} is neither patch-ready nor patch-applied"
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
    elif patch_is_ready; then
      echo "READY"
    else
      echo "UNSUPPORTED_STATE"
      exit 1
    fi
    ;;
  *)
    echo "Usage: $0 {apply|reverse|status}" >&2
    exit 2
    ;;
esac
