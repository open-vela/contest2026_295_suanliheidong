#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Apply or reverse the board-local ESP HAL lock initializer backport.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly OPENVELA_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
readonly HAL_DIR="${ESP_HAL_3RDPARTY_DIR:-${OPENVELA_ROOT}/nuttx/arch/xtensa/src/esp32s3/esp-hal-3rdparty}"
readonly PATCH_FILE="${SCRIPT_DIR}/../patches/0001-esp-hal-nuttx-lock-initializer.patch"
readonly BASE_REVISION="9fc713a95b1ff150dd0b0647e465d3c624056bb1"
readonly PATCHED_FILES=(
  components/esp_hw_support/clk_ctrl_os.c
  components/esp_hw_support/modem_clock.c
)

die()
{
  echo "esp-hal lock backport: $*" >&2
  exit 1
}

require_clean_target_files()
{
  git -C "${HAL_DIR}" diff --quiet -- "${PATCHED_FILES[@]}" ||
    die "refusing to overwrite existing HAL changes"
}

patch_is_applied()
{
  git -C "${HAL_DIR}" apply --reverse --check "${PATCH_FILE}" >/dev/null 2>&1
}

require_repository()
{
  test -f "${PATCH_FILE}" || die "patch not found: ${PATCH_FILE}"
  git -C "${HAL_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    die "HAL checkout not found: ${HAL_DIR}"
}

apply_patch()
{
  require_repository

  if patch_is_applied; then
    echo "ALREADY_APPLIED"
    return
  fi

  require_clean_target_files
  test "$(git -C "${HAL_DIR}" rev-parse HEAD)" = "${BASE_REVISION}" ||
    die "HAL HEAD must be ${BASE_REVISION} before applying this backport"

  git -C "${HAL_DIR}" apply --check "${PATCH_FILE}" ||
    die "backport does not apply cleanly"
  git -C "${HAL_DIR}" apply --whitespace=nowarn "${PATCH_FILE}"
  echo "APPLIED"
}

reverse_patch()
{
  require_repository

  if patch_is_applied; then
    git -C "${HAL_DIR}" apply --reverse --whitespace=nowarn "${PATCH_FILE}"
    echo "REVERTED"
  else
    require_clean_target_files
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
      require_clean_target_files
      if test "$(git -C "${HAL_DIR}" rev-parse HEAD)" = "${BASE_REVISION}" &&
         git -C "${HAL_DIR}" apply --check "${PATCH_FILE}" >/dev/null 2>&1; then
        echo "READY"
      else
        echo "UNSUPPORTED_STATE"
        exit 1
      fi
    fi
    ;;
  *)
    echo "Usage: $0 {apply|reverse|status}" >&2
    exit 2
    ;;
esac
