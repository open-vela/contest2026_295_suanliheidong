#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# ESP32-S3 ai_agent build compatibility helper.
#
# IMPORTANT POLICY:
#   packages/ai_agent is USER-OWNED SOURCE and is NEVER modified here.
#   This script does NOT git apply/reverse/checkout/reset/restore anything
#   under packages/ai_agent.
#
# The only temporary changes made by this helper are:
#   1. apps/crypto/mbedtls/Make.defs:
#        ${INCDIR_PREFIX} -> -isystem for the two mbedTLS include paths
#   2. HAL mbedtls_config.h:
#        disable MBEDTLS_CCM_C for this compatibility build
#
# Interface kept compatible with build_with_hal_backport.sh:
#   apply | restore | status

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT_DIR="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
readonly AGENT_DIR="${ROOT_DIR}/packages/ai_agent"
readonly HAL_DIR="${ROOT_DIR}/nuttx/arch/xtensa/src/esp32s3/esp-hal-3rdparty"
readonly MAKEDEFS="${ROOT_DIR}/apps/crypto/mbedtls/Make.defs"
readonly MBEDTLS_CFG="${HAL_DIR}/components/mbedtls/mbedtls/include/mbedtls/mbedtls_config.h"

readonly STATE_DIR="${TMPDIR:-/tmp}/openvela-contest-ai-agent-compat"
readonly MARKER="${STATE_DIR}/root"
readonly MAKEDEFS_BACKUP="${STATE_DIR}/make.defs"
readonly MBEDTLS_BACKUP="${STATE_DIR}/mbedtls_config.h"
readonly MAKEDEFS_PATCHED_HASH="${STATE_DIR}/make.defs.patched.sha256"
readonly MBEDTLS_PATCHED_HASH="${STATE_DIR}/mbedtls_config.h.patched.sha256"

die()
{
  echo "ai_agent compatibility: $*" >&2
  exit 1
}

file_hash()
{
  sha256sum "$1" | awk '{print $1}'
}

require_targets()
{
  test -d "${AGENT_DIR}" || die "missing ai_agent source directory: ${AGENT_DIR}"
  test -f "${MAKEDEFS}" || die "missing target: ${MAKEDEFS}"
  test -f "${MBEDTLS_CFG}" || die "missing target: ${MBEDTLS_CFG}"

  # Deliberately do not inspect or enforce any ai_agent source marker here.
  # Local ai_agent source is authoritative and must be compiled as-is.
}

state_matches_root()
{
  test -f "${MARKER}" && test "$(cat "${MARKER}")" = "${ROOT_DIR}"
}

clear_state()
{
  rm -f \
    "${MAKEDEFS_BACKUP}" \
    "${MBEDTLS_BACKUP}" \
    "${MAKEDEFS_PATCHED_HASH}" \
    "${MBEDTLS_PATCHED_HASH}" \
    "${MARKER}"
  rmdir "${STATE_DIR}" 2>/dev/null || true
}

makedefs_is_ready()
{
  grep -Fq '${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/include' "${MAKEDEFS}" &&
  grep -Fq '${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include' "${MAKEDEFS}"
}

makedefs_is_applied()
{
  ! grep -Fq '${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/include' "${MAKEDEFS}" &&
  ! grep -Fq '${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include' "${MAKEDEFS}" &&
  grep -Fq 'CFLAGS += -isystem $(APPDIR)/crypto/mbedtls/include' "${MAKEDEFS}" &&
  grep -Fq 'CFLAGS += -isystem $(APPDIR)/crypto/mbedtls/mbedtls/include' "${MAKEDEFS}"
}

mbedtls_is_ready()
{
  grep -Fqx '#define MBEDTLS_CCM_C' "${MBEDTLS_CFG}"
}

mbedtls_is_applied()
{
  grep -Fqx '/* #define MBEDTLS_CCM_C */' "${MBEDTLS_CFG}"
}

all_ready()
{
  makedefs_is_ready && mbedtls_is_ready
}

all_applied()
{
  makedefs_is_applied && mbedtls_is_applied
}

target_matches_backup_or_patched()
{
  local target="$1"
  local backup="$2"
  local patched_hash_file="$3"
  local current_hash

  test -f "${target}" || return 1
  test -f "${backup}" || return 1

  current_hash="$(file_hash "${target}")"

  if test "${current_hash}" = "$(file_hash "${backup}")"; then
    return 0
  fi

  if test -s "${patched_hash_file}" &&
     test "${current_hash}" = "$(cat "${patched_hash_file}")"; then
    return 0
  fi

  return 1
}

recover_stale_state()
{
  state_matches_root || return 1

  # Recovery is intentionally restricted to the two compatibility targets.
  # packages/ai_agent is NEVER restored or reverse-patched.
  target_matches_backup_or_patched \
    "${MAKEDEFS}" "${MAKEDEFS_BACKUP}" "${MAKEDEFS_PATCHED_HASH}" || return 1
  target_matches_backup_or_patched \
    "${MBEDTLS_CFG}" "${MBEDTLS_BACKUP}" "${MBEDTLS_PATCHED_HASH}" || return 1

  echo "ai_agent compatibility: clearing stale compatibility state; ai_agent source untouched" >&2

  # Put only compatibility targets back to their saved originals.
  cp -p "${MAKEDEFS_BACKUP}" "${MAKEDEFS}"
  cp -p "${MBEDTLS_BACKUP}" "${MBEDTLS_CFG}"
  clear_state
  return 0
}

apply_compat()
{
  require_targets

  if state_matches_root; then
    if all_applied; then
      echo "ALREADY_APPLIED"
      return 0
    fi

    if ! recover_stale_state; then
      die "saved compatibility state is inconsistent; refusing to overwrite unknown changes"
    fi
  fi

  if all_applied; then
    # Compatibility files are already in the desired build state.
    # No source operation is needed.
    echo "ALREADY_APPLIED"
    return 0
  fi

  all_ready || die "compatibility targets are partially modified; refusing unsafe apply"

  mkdir -p "${STATE_DIR}"
  printf '%s\n' "${ROOT_DIR}" > "${MARKER}"
  cp -p "${MAKEDEFS}" "${MAKEDEFS_BACKUP}"
  cp -p "${MBEDTLS_CFG}" "${MBEDTLS_BACKUP}"

  sed -i \
    's|CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/include|CFLAGS += -isystem $(APPDIR)/crypto/mbedtls/include|g; s|CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include|CFLAGS += -isystem $(APPDIR)/crypto/mbedtls/mbedtls/include|g; s|CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/include|CXXFLAGS += -isystem $(APPDIR)/crypto/mbedtls/include|g; s|CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include|CXXFLAGS += -isystem $(APPDIR)/crypto/mbedtls/mbedtls/include|g' \
    "${MAKEDEFS}"

  sed -i \
    's/^#define MBEDTLS_CCM_C$/\/\* #define MBEDTLS_CCM_C \*\//' \
    "${MBEDTLS_CFG}"

  if ! all_applied; then
    cp -p "${MAKEDEFS_BACKUP}" "${MAKEDEFS}"
    cp -p "${MBEDTLS_BACKUP}" "${MBEDTLS_CFG}"
    clear_state
    die "post-apply compatibility check failed"
  fi

  file_hash "${MAKEDEFS}" > "${MAKEDEFS_PATCHED_HASH}"
  file_hash "${MBEDTLS_CFG}" > "${MBEDTLS_PATCHED_HASH}"

  echo "ai_agent compatibility: local packages/ai_agent source preserved exactly as-is" >&2
  echo "APPLIED"
}

restore_compat()
{
  require_targets

  if ! state_matches_root; then
    # No state means there is nothing this invocation owns.
    # Never guess, and never touch ai_agent source.
    echo "REVERTED"
    return 0
  fi

  target_matches_backup_or_patched \
    "${MAKEDEFS}" "${MAKEDEFS_BACKUP}" "${MAKEDEFS_PATCHED_HASH}" ||
    die "${MAKEDEFS} changed after apply; refusing to overwrite it"

  target_matches_backup_or_patched \
    "${MBEDTLS_CFG}" "${MBEDTLS_BACKUP}" "${MBEDTLS_PATCHED_HASH}" ||
    die "${MBEDTLS_CFG} changed after apply; refusing to overwrite it"

  cp -p "${MAKEDEFS_BACKUP}" "${MAKEDEFS}"
  cp -p "${MBEDTLS_BACKUP}" "${MBEDTLS_CFG}"
  clear_state

  echo "ai_agent compatibility: restored compatibility files only; packages/ai_agent untouched" >&2
  echo "REVERTED"
}

status_compat()
{
  require_targets

  if state_matches_root; then
    if all_applied; then
      echo "APPLIED"
    else
      echo "DIRTY"
      return 1
    fi
  elif all_applied; then
    echo "ALREADY_APPLIED"
  elif all_ready; then
    echo "READY"
  else
    echo "DIRTY"
    return 1
  fi
}

case "${1:-}" in
  apply) apply_compat ;;
  restore) restore_compat ;;
  status) status_compat ;;
  *) echo "Usage: $0 {apply|restore|status}" >&2; exit 2 ;;
esac
