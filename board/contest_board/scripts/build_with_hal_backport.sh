#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build the contest ESP32-S3 board with a temporary, reversible
# esp-hal-3rdparty lock-initializer backport.
#
# Fresh workspaces may not contain esp-hal-3rdparty yet. NuttX prepares it
# during the normal build context phase, so this wrapper bootstraps that
# phase first when needed, then applies the local backport and performs the
# real build.
# ROBOT_VOICE_TRANSPORT_HOOKS_V2: contest-local Agent source tree is authoritative.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly OPENVELA_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
readonly CONFIG_PATH="vendor/openvela/boards/contest2026_295_board/configs/nsh"
readonly HAL_PATCH_TOOL="${SCRIPT_DIR}/esp_hal_lock_backport.sh"
readonly NUTTX_BREAK_PATCH_TOOL="${SCRIPT_DIR}/nuttx_xtensa_break_backport.sh"
readonly NUTTX_WLAN_PATCH_TOOL="${SCRIPT_DIR}/esp32s3_wlan_backport.sh"
readonly NUTTX_RAW_EXC_PATCH_TOOL="${SCRIPT_DIR}/nuttx_xtensa_raw_exception_backport.sh"
readonly NUTTX_RAW_EXC="${NUTTX_RAW_EXC:-0}"
readonly AI_AGENT_PATCH_TOOL="${SCRIPT_DIR}/ai_agent_esp32s3_backport.sh"  # compatibility-only; never edits Agent source
readonly FFMPEG_AMIX_PATCH_TOOL="${SCRIPT_DIR}/ffmpeg_amix_backport.sh"
readonly MEDIA_COMPLETION_PATCH_TOOL="${SCRIPT_DIR}/media_player_completed_backport.sh"
readonly MEDIA_HTTP_RECONNECT_PATCH_TOOL="${SCRIPT_DIR}/media_http_reconnect_backport.sh"
readonly VOICE_HOOK_HEADER="${OPENVELA_ROOT}/contest2026_295_suanliheidong/app/robot_voice/robot_voice_agent_hooks.h"
readonly MYENV_DIR="${OPENVELA_ROOT}/myenv"
readonly BUILD_SH="${OPENVELA_ROOT}/build.sh"
readonly NUTTX_DIR="${OPENVELA_ROOT}/nuttx"
readonly HAL_DIR="${NUTTX_DIR}/arch/xtensa/src/esp32s3/esp-hal-3rdparty"
readonly HAL_CACHE_DIR="${OPENVELA_ROOT}/contest2026_295_suanliheidong/third_party/esp-hal-3rdparty"
readonly HAL_EXPECTED_VERSION="$(
  sed -n \
    's/^[[:space:]]*ESP_HAL_3RDPARTY_VERSION[[:space:]]*=[[:space:]]*//p' \
    "${NUTTX_DIR}/arch/xtensa/src/esp32s3/Make.defs" | head -n 1
)"
readonly HAL_URL="$(
  sed -n \
    's/^[[:space:]]*ESP_HAL_3RDPARTY_URL[[:space:]]*=[[:space:]]*//p' \
    "${NUTTX_DIR}/arch/xtensa/src/esp32s3/Make.defs" | head -n 1
)"
readonly -a HAL_REQUIRED_SUBMODULES=(
  components/mbedtls/mbedtls
  components/esp_phy/lib
  components/esp_wifi/lib
  components/bt/controller/lib_esp32c3_family
  components/esp_coex/lib
)
readonly AI_AGENT_OVERLAY_DIR="${OPENVELA_ROOT}/contest2026_295_suanliheidong/packages/ai_agent"

readonly VOICE_FIX_REQUIRED="${VOICE_FIX_REQUIRED:-0}"
readonly VOICE_TLS_SRC="${OPENVELA_ROOT}/packages/ai_agent/src/infra/vela_tls.c"
readonly VOICE_MIMO_SRC="${OPENVELA_ROOT}/packages/ai_agent/src/voice/mimo_voice.c"
# These are user-owned source trees.  build.sh context is allowed to prepare
# its generated context and HAL checkout, but it must not replace local source
# files before the actual make.  Override this list when the project layout
# uses different source roots.
readonly LOCAL_SOURCE_PATHS="${LOCAL_SOURCE_PATHS:-packages/ai_agent contest2026_295_suanliheidong/packages/ai_agent contest2026_295_suanliheidong/app contest2026_295_suanliheidong/board/contest_board/src}"
readonly LOCAL_SOURCE_SNAPSHOT="${TMPDIR:-/tmp}/openvela-local-source-${BASHPID}"

cd "${OPENVELA_ROOT}"

snapshot_local_sources()
{
  local rel
  local archive
  local found=0

  mkdir -p "${LOCAL_SOURCE_SNAPSHOT}"
  : > "${LOCAL_SOURCE_SNAPSHOT}/paths"

  for rel in ${LOCAL_SOURCE_PATHS}; do
    if test ! -d "${OPENVELA_ROOT}/${rel}"; then
      echo "Local source snapshot: skip missing ${rel}" >&2
      continue
    fi

    archive="${LOCAL_SOURCE_SNAPSHOT}/${rel//\//__}.tar"
    # Snapshot user-owned source/config only.  Never preserve generated
    # objects/archives, otherwise restore_local_sources() can resurrect stale
    # code after a context/clean operation.
    tar -C "${OPENVELA_ROOT}" \
      --exclude=.git \
      --exclude='*.o' \
      --exclude='*.a' \
      --exclude='*.d' \
      --exclude='*.gcno' \
      --exclude='*.gcda' \
      -cpf "${archive}" "${rel}"
    printf '%s\n' "${rel}" >> "${LOCAL_SOURCE_SNAPSHOT}/paths"
    found=1
  done

  if test "${found}" != 1; then
    echo "No local source roots were found; refusing to build without source protection." >&2
    exit 1
  fi

  echo "Local source snapshot created; context may not replace local source files."
}

restore_local_sources()
{
  local rel
  local archive
  local restore_root="${LOCAL_SOURCE_SNAPSHOT}/restore"

  rm -rf "${restore_root}"
  mkdir -p "${restore_root}"

  while IFS= read -r rel; do
    archive="${LOCAL_SOURCE_SNAPSHOT}/${rel//\//__}.tar"
    tar -C "${restore_root}" -xpf "${archive}"
    rsync -a --delete \
      --exclude='.git' \
      --exclude='*.o' \
      --exclude='*.a' \
      --exclude='*.d' \
      --exclude='*.gcno' \
      --exclude='*.gcda' \
      --exclude='*.pyc' \
      --exclude='__pycache__/' \
      "${restore_root}/${rel}/" "${OPENVELA_ROOT}/${rel}/"
  done < "${LOCAL_SOURCE_SNAPSHOT}/paths"

  rm -rf "${restore_root}"

  echo "Local source snapshot restored; compiling the files present before context."
}

apply_contest_ai_agent_overlay()
{
  if test ! -d "${AI_AGENT_OVERLAY_DIR}"; then
    echo "Missing contest-local ai_agent overlay: ${AI_AGENT_OVERLAY_DIR}" >&2
    return 1
  fi

  # The generated build context still resolves the package from
  # packages/ai_agent.  Synchronize the complete contest-local package there,
  # including deletions, while leaving the nested Git metadata and generated
  # build artifacts alone. restore_local_sources() restores the original
  # checkout at EXIT.
  ai_agent_overlay_applied=1
  rsync -a --delete \
    --exclude='.git' \
    --exclude='*.o' \
    --exclude='*.a' \
    --exclude='*.d' \
    --exclude='*.gcno' \
    --exclude='*.gcda' \
    --exclude='*.pyc' \
    --exclude='__pycache__/' \
    "${AI_AGENT_OVERLAY_DIR}/" "${OPENVELA_ROOT}/packages/ai_agent/"

  echo "Complete contest-local ai_agent source tree applied; build uses the contest copy."
}

cleanup_local_source_snapshot()
{
  if test -d "${LOCAL_SOURCE_SNAPSHOT}"; then
    rm -rf "${LOCAL_SOURCE_SNAPSHOT}"
  fi
}

if test ! -x "${MYENV_DIR}/bin/python"; then
  echo "Creating Python virtual environment: ${MYENV_DIR}"
  python3 -m venv "${MYENV_DIR}"
fi

# shellcheck disable=SC1091
source "${MYENV_DIR}/bin/activate"

# Keep the board-local esptool wrapper ahead of the system PATH.
export PATH="${SCRIPT_DIR}:${PATH}"

hal_tree_ready()
{
  local tree="$1"
  local submodule
  local status_line

  test -n "${HAL_EXPECTED_VERSION}" || return 1
  test -d "${tree}" || return 1
  git -C "${tree}" rev-parse --is-inside-work-tree >/dev/null 2>&1 || return 1
  test "$(git -C "${tree}" rev-parse HEAD 2>/dev/null || true)" = "${HAL_EXPECTED_VERSION}" || return 1

  # A matching top-level commit is insufficient: wireless builds require these
  # submodules to be initialized in the cached checkout as well.
  for submodule in "${HAL_REQUIRED_SUBMODULES[@]}"; do
    status_line="$(git -C "${tree}" submodule status --recursive -- "${submodule}" 2>/dev/null || true)"
    if ! grep -Eq '^[[:space:]][0-9a-f]{40}[[:space:]]' <<<"${status_line}"; then
      return 1
    fi
  done
}

hal_checkout_ready()
{
  hal_tree_ready "${HAL_DIR}"
}

hal_cache_ready()
{
  hal_tree_ready "${HAL_CACHE_DIR}"
}

restore_hal_from_cache()
{
  if ! hal_cache_ready; then
    return 1
  fi

  mkdir -p "$(dirname "${HAL_DIR}")"
  if test -e "${HAL_DIR}" || test -L "${HAL_DIR}"; then
    rm -rf "${HAL_DIR}"
  fi
  rsync -a --delete "${HAL_CACHE_DIR}/" "${HAL_DIR}/"

  if ! hal_checkout_ready; then
    rm -rf "${HAL_DIR}"
    echo "Local HAL cache restore failed commit or submodule validation." >&2
    return 1
  fi

  echo "HAL restored from local cache: ${HAL_CACHE_DIR}"
  echo "HAL HEAD: $(git -C "${HAL_DIR}" rev-parse HEAD)"
}

save_hal_to_cache()
{
  local cache_parent cache_tmp

  if ! hal_checkout_ready; then
    echo "Cannot cache HAL: checkout, commit, or required submodules are invalid." >&2
    return 1
  fi

  cache_parent="$(dirname "${HAL_CACHE_DIR}")"
  cache_tmp="${HAL_CACHE_DIR}.tmp-${BASHPID}"
  mkdir -p "${cache_parent}"
  rm -rf "${cache_tmp}"
  rsync -a --delete "${HAL_DIR}/" "${cache_tmp}/"

  if ! hal_tree_ready "${cache_tmp}"; then
    rm -rf "${cache_tmp}"
    echo "Cannot cache HAL: copied checkout failed commit or submodule validation." >&2
    return 1
  fi

  if test -e "${HAL_CACHE_DIR}" || test -L "${HAL_CACHE_DIR}"; then
    rm -rf "${HAL_CACHE_DIR}"
  fi
  mv "${cache_tmp}" "${HAL_CACHE_DIR}"
  echo "HAL backup saved locally: ${HAL_CACHE_DIR}"
}

clone_hal_checkout_direct()
{
  local attempt

  test -n "${HAL_URL}" || return 1
  mkdir -p "$(dirname "${HAL_DIR}")"

  for attempt in 1 2 3; do
    if test -e "${HAL_DIR}" || test -L "${HAL_DIR}"; then
      rm -rf "${HAL_DIR}"
    fi
    echo "Cloning HAL directly with Git HTTP/1.1 (attempt ${attempt}/3)..."
    if GIT_TERMINAL_PROMPT=0 git -c http.version=HTTP/1.1 clone --quiet "${HAL_URL}" "${HAL_DIR}" &&
       git -C "${HAL_DIR}" checkout --quiet "${HAL_EXPECTED_VERSION}"; then
      return 0
    fi
    echo "Direct HAL clone attempt ${attempt} failed; retrying..." >&2
    if test -e "${HAL_DIR}" || test -L "${HAL_DIR}"; then
      rm -rf "${HAL_DIR}"
    fi
    sleep 2
  done

  return 1
}

reset_generated_build_state()
{
  rm -f "${NUTTX_DIR}/.config" \
        "${NUTTX_DIR}/Make.defs" \
        "${NUTTX_DIR}/staging/libarch.a" \
        "${NUTTX_DIR}/staging/libboard.a"
}

bootstrap_hal_checkout()
{
  if hal_checkout_ready; then
    if ! hal_cache_ready; then
      save_hal_to_cache
    else
      echo "HAL checkout is available and local cache is current."
    fi
    return 0
  fi

  if restore_hal_from_cache; then
    return 0
  fi

  echo "HAL checkout is missing:"
  echo "  ${HAL_DIR}"
  echo
  echo "Bootstrapping the normal NuttX build context first..."

  reset_generated_build_state

  # The stock clone helper uses the system Git HTTP defaults.  A direct
  # HTTP/1.1 clone avoids transient HTTP/2 cancellations while preserving the
  # exact revision and leaving submodule/mbedTLS preparation to NuttX below.
  if ! clone_hal_checkout_direct; then
    echo "Direct HAL clone failed; falling back to build.sh context." >&2
  fi

  # Follow openvela's normal build path so NuttX creates/clones the ESP HAL
  # checkout using the revision expected by this NuttX tree.
  if ! "${BUILD_SH}" "${CONFIG_PATH}" context; then
    echo "HAL bootstrap via build.sh context failed." >&2
    echo "Expected HAL checkout:" >&2
    echo "  ${HAL_DIR}" >&2
    exit 1
  fi

  if ! hal_checkout_ready; then
    echo "build.sh context completed, but the HAL checkout was not created." >&2
    echo "Expected:" >&2
    echo "  ${HAL_DIR}" >&2
    exit 1
  fi

  save_hal_to_cache

  echo "HAL checkout bootstrapped successfully."
  echo "HAL HEAD: $(git -C "${HAL_DIR}" rev-parse HEAD)"
}

refresh_demo_kconfig()
{
  local app link target
  mkdir -p "${OPENVELA_ROOT}/packages/demos"
  for app in robot_agentctl robot_voice robotctl; do
    link="${OPENVELA_ROOT}/packages/demos/contest2026_295_${app}"
    target="../../contest2026_295_suanliheidong/app/${app}"
    if test ! -e "${link}" && test ! -L "${link}"; then
      ln -s "${target}" "${link}"
    elif test ! -d "${link}" ||
         test "$(cd "${link}" && pwd -P)" != "$(cd "${OPENVELA_ROOT}/contest2026_295_suanliheidong/app/${app}" && pwd -P)"; then
      echo "Unexpected demo path: ${link}; refusing to replace it." >&2
      return 1
    fi
  done
  (
    cd "${OPENVELA_ROOT}/packages/demos"
    "${OPENVELA_ROOT}/apps/tools/mkkconfig.sh" -m Demos -o Kconfig
  )
  (
    cd "${OPENVELA_ROOT}/packages"
    "${OPENVELA_ROOT}/apps/tools/mkkconfig.sh" -o Kconfig
  )
}

prepare_build_context()
{
  reset_generated_build_state

  # The ESP32-S3 context target resets and repatches the HAL mbedTLS
  # submodule.  Run it before applying the temporary ai_agent compatibility
  # patch so the patch survives the actual compilation.
  "${BUILD_SH}" "${CONFIG_PATH}" context
}

refresh_restored_build_context()
{
  # build.sh context may prepare/copy board/application context before the
  # contest-local source snapshot is restored.  Existing source files still
  # compile after restore, but a newly-added source file can be missing from
  # the generated application source list.  Re-run the NuttX context target
  # after restore so the active contest CMakeLists.txt/Kconfig are evaluated
  # again without invoking build.sh's copy/configure phase a second time.
  echo "Refreshing build context from restored contest-local sources..."
  run_configured_make context
  echo "Restored-source build context refreshed."
}

run_configured_make()
{
  local extra_flags="-Wno-cpp -Wno-deprecated-declarations"
  local voice_hooks=0
  local cc_command
  local voice_launcher="${OPENVELA_ROOT}/contest2026_295_suanliheidong/app/robot_voice/scripts/voice_gcc.sh"
  local -a make_args=()
  if ! is_maintenance_target "$@" && grep -Eq '^CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_VOICE=y$' "${NUTTX_DIR}/.config" 2>/dev/null; then
    if test ! -f "${VOICE_HOOK_HEADER}"; then
      echo "Missing contest HTTP hook header: ${VOICE_HOOK_HEADER}" >&2
      return 1
    fi
    extra_flags+=" -DROBOT_VOICE_AGENT_INTERPOSE=1"
    voice_hooks=1
  fi

  # Match build.sh's configuration environment without invoking context a
  # second time after the ai_agent patch has been applied.
  # shellcheck disable=SC1091
  set +e
  set +u
  source "${OPENVELA_ROOT}/build/envsetup.sh"
  set -e
  set -u
  export PATH="${OPENVELA_ROOT}/prebuilts/kconfig-frontends/bin:${PATH}"

  # Important: build only.  Do NOT run savedefconfig here and do NOT copy the
  # generated defconfig back into the board source tree.  A diagnostic build
  # must not silently mutate the persistent NSH board configuration.
  if test "${voice_hooks}" = 1; then
    # CC must contain one executable, with no wrapper-specific -- separators:
    # mkdeps parses CC -- CFLAGS -- files before invoking the compiler.
    cc_command="$(make --no-print-directory -s -C "${NUTTX_DIR}" \
      --eval='__rv_print_cc:;@printf "%s\n" "$(CC)"' __rv_print_cc)"
    if test -z "${cc_command}"; then
      echo "Unable to resolve configured C compiler" >&2
      return 1
    fi
    if ! test -x "${voice_launcher}" || ! test -f "${voice_launcher%/*}/voice_cc.py"; then
      echo "Missing executable voice compiler launcher: ${voice_launcher}" >&2
      return 1
    fi
    export ROBOT_VOICE_REAL_CC="${cc_command}"
    export ROBOT_VOICE_HOOK_HEADER="${VOICE_HOOK_HEADER}"
    make_args+=("CC=${voice_launcher}")
  fi
  make -C "${NUTTX_DIR}" EXTRAFLAGS="${extra_flags}" "${make_args[@]}" "$@"
}


is_maintenance_target()
{
  local arg

  for arg in "$@"; do
    case "${arg}" in
      clean|distclean)
        return 0
        ;;
    esac
  done

  return 1
}


run_maintenance_target()
{
  echo "Maintenance target detected: $*"
  echo "Skipping context bootstrap and all temporary HAL/NuttX/ai_agent backports."
  echo "This prevents distclean from deleting patched targets before the EXIT restore trap."

  # Source/config trees are not touched by NuttX clean/distclean.  Invoke the
  # requested make target directly and do not install the backport trap.
  run_configured_make "$@"
}


verify_voice_fix_sources()
{
  if test "${VOICE_FIX_REQUIRED}" != 1; then
    echo "Voice source policy verification: SKIPPED; contest-local ai_agent overlay is authoritative (VOICE_FIX_REQUIRED=${VOICE_FIX_REQUIRED})"
    return 0
  fi

  test -f "${VOICE_TLS_SRC}" || {
    echo "Missing voice TLS source: ${VOICE_TLS_SRC}" >&2
    exit 1
  }

  test -f "${VOICE_MIMO_SRC}" || {
    echo "Missing MiMo voice source: ${VOICE_MIMO_SRC}" >&2
    exit 1
  }

  local missing=0

  for marker in \
    'DNS start:' \
    'TCP connect start:' \
    'upload progress:'; do
    if ! grep -Fq "${marker}" "${VOICE_TLS_SRC}"; then
      echo "Voice fix marker missing from vela_tls.c: ${marker}" >&2
      missing=1
    fi
  done

  # The build wrapper must verify capability, not dictate runtime key policy.
  # Accept either lookup order as long as both the generic fallback and the
  # MiMo-specific key path still exist in get_api_key().
  local key_block
  key_block="$(
    sed -n '/static int get_api_key/,/^}/p' "${VOICE_MIMO_SRC}"
  )"

  if ! grep -Fq 'AGENT_CFG_KEY_MIMO_API_KEY' <<<"${key_block}"; then
    echo "MiMo voice key support is missing in mimo_voice.c" >&2
    missing=1
  fi

  if ! grep -Fq 'AGENT_CFG_KEY_API_KEY' <<<"${key_block}"; then
    echo "Generic API key fallback is missing in mimo_voice.c" >&2
    missing=1
  fi

  local first_key
  first_key="$(
    grep -Eo 'AGENT_CFG_KEY_(MIMO_API_KEY|API_KEY)' <<<"${key_block}" |
      head -n 1 || true
  )"

  if test "${first_key}" != "AGENT_CFG_KEY_MIMO_API_KEY"; then
    echo "Voice fix source verification: NOTE - get_api_key() checks ${first_key:-no-key} first; build allowed."
  fi

  # The current MiMo TTS path must use incremental HTTP/Base64/WAV
  # processing and feed PCM through the voice streaming callback.  Do not
  # require the old whole-response TTS markers here: those belong to the
  # retired fixed-buffer implementation.
  for marker in \
    'TTS streaming request' \
    'mimo_tts_stream_synthesize' \
    '.stream_synthesize = mimo_tts_stream_synthesize'; do
    if ! grep -Fq "${marker}" "${VOICE_MIMO_SRC}"; then
      echo "Streaming TTS marker missing from mimo_voice.c: ${marker}" >&2
      missing=1
    fi
  done

  # Streaming TTS depends on the incremental HTTPS response path in vela_tls.
  for marker in \
    'vela_https_post_json_stream' \
    'transport stream enter:'; do
    if ! grep -Fq "${marker}" "${VOICE_TLS_SRC}"; then
      echo "Streaming HTTPS marker missing from vela_tls.c: ${marker}" >&2
      missing=1
    fi
  done

  if test "${missing}" != 0; then
    echo "Refusing to build: expected voice-chain fixes are not present." >&2
    exit 1
  fi

  echo "Voice fix source verification: OK"
}

force_clean_rebuild()
{
  # ai_agent and contest sources live outside nuttx/.  Clear generated
  # external objects across the full roots.  Limiting cleanup to voice/
  # robot_voice/ leaves stale network_manager/robot_network_adapter objects
  # able to shadow the restored source.
  echo "Removing stale external ai_agent/contest build artifacts; source files are untouched..."

  find "${OPENVELA_ROOT}/packages/ai_agent/src" \
    -type f \( \
      -name '*.o' -o \
      -name '*.c.*.o' -o \
      -name '*.cc.*.o' -o \
      -name '*.cpp.*.o' -o \
      -name '*.d' \
    \) -print -delete 2>/dev/null || true

  find "${OPENVELA_ROOT}/contest2026_295_suanliheidong/app" \
    -type f \( \
      -name '*.o' -o \
      -name '*.c.*.o' -o \
      -name '*.cc.*.o' -o \
      -name '*.cpp.*.o' -o \
      -name '*.d' \
    \) -print -delete 2>/dev/null || true

  rm -f \
    "${OPENVELA_ROOT}/apps/libapps.a" \
    "${NUTTX_DIR}/staging/libapps.a"
  echo "Forcing clean rebuild so the current local ai_agent sources are compiled exactly as present..."
  make -C "${NUTTX_DIR}" clean
}


verify_built_voice_image()
{
  local elf="${NUTTX_DIR}/nuttx"

  # Skip artifact verification for maintenance targets.
  if printf '%s\n' "$@" | grep -Eq '(^|[[:space:]])(clean|distclean)([[:space:]]|$)'; then
    return 0
  fi

  if test ! -f "${elf}"; then
    echo "Post-build verification failed: missing ${elf}" >&2
    exit 1
  fi

  if grep -Eq '^CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_AGENTCTL=y$' \
      "${NUTTX_DIR}/.config" && \
      ! grep -aF 'robot_agentctl_main' "${elf}" >/dev/null; then
    echo "Post-build verification failed: ELF missing robot_agentctl marker" >&2
    exit 1
  fi

  # The contest-local robot_voice application is independent of the
  # historical ai_agent streaming implementation.  When the legacy
  # voice_echo app is disabled, validate the new app marker and do not require
  # private symbols/strings from packages/ai_agent.
  if grep -Eq '^CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_VOICE=y$' \
      "${NUTTX_DIR}/.config" && \
      ! grep -Eq '^CONFIG_LVX_USE_DEMO_CONTEST2026_295_VOICE_ECHO=y$' \
      "${NUTTX_DIR}/.config"; then
    if ! grep -aF 'robot_voice_main' "${elf}" >/dev/null; then
      echo "Post-build verification failed: ELF missing robot_voice marker" >&2
      exit 1
    fi
    python3 "${OPENVELA_ROOT}/contest2026_295_suanliheidong/app/robot_voice/scripts/verify_voice_elf.py" "${elf}"
    echo "Post-build verification: contest-local robot_voice linked"
    return 0
  fi

  # The official ai_agent source does not promise the contest-local streaming
  # MiMo markers.  Only enforce them when the caller explicitly requests the
  # voice-fix source contract.
  if test "${VOICE_FIX_REQUIRED}" != 1; then
    echo "Post-build streaming voice verification: SKIPPED (VOICE_FIX_REQUIRED=${VOICE_FIX_REQUIRED})"
    return 0
  fi

  local missing=0

  # Verify that the streaming TTS implementation and streaming HTTPS
  # transport actually reached the final firmware image.
  for marker in \
    'TTS streaming request' \
    'transport stream enter:'; do
    if ! strings "${elf}" | grep -Fq "${marker}"; then
      echo "Post-build verification failed: ELF missing streaming marker: ${marker}" >&2
      missing=1
    fi
  done

  # The old fixed-buffer diagnostics should not survive in the final image.
  # Their presence usually means an old mimo_voice object/archive was linked.
  for marker in \
    'TTS response bytes=' \
    'TTS audio base64 bytes=' \
    'TTS WAV bytes=' \
    'TTS PCM output too small'; do
    if strings "${elf}" | grep -Fq "${marker}"; then
      echo "Post-build verification failed: ELF still contains legacy TTS marker: ${marker}" >&2
      missing=1
    fi
  done

  if test "${missing}" != 0; then
    echo "The firmware was built, but the streaming MiMo TTS/HTTPS sources were not linked cleanly into nuttx." >&2
    exit 1
  fi

  echo "Post-build streaming voice verification: OK"
}

hal_rollback=0
nuttx_break_rollback=0
nuttx_wlan_rollback=0
nuttx_raw_exc_rollback=0
ai_agent_rollback=0
ai_agent_overlay_applied=0
ffmpeg_amix_rollback=0
media_completion_rollback=0
media_http_reconnect_rollback=0
restore_backports()
{
  local rc=$?

  trap - EXIT INT TERM

  if test "${media_completion_rollback}" = 1; then
    if ! "${MEDIA_COMPLETION_PATCH_TOOL}" reverse; then
      echo "failed to restore the Media completed-state backport" >&2
      if test "${rc}" -eq 0; then
        rc=1
      fi
    fi
  fi

  if test "${media_http_reconnect_rollback}" = 1; then
    if ! "${MEDIA_HTTP_RECONNECT_PATCH_TOOL}" reverse; then
      echo "failed to restore the Media HTTP reconnect backport" >&2
      if test "${rc}" -eq 0; then
        rc=1
      fi
    fi
  fi

  if test "${ffmpeg_amix_rollback}" = 1; then
    if ! "${FFMPEG_AMIX_PATCH_TOOL}" reverse; then
      echo "failed to restore the FFmpeg amix build backport" >&2
      if test "${rc}" -eq 0; then
        rc=1
      fi
    fi
  fi

  if test "${ai_agent_rollback}" = 1; then
    if ! "${AI_AGENT_PATCH_TOOL}" restore; then
      echo "failed to restore temporary ai_agent build compatibility files" >&2
      if test "${rc}" -eq 0; then
        rc=1
      fi
    fi
  fi

  if test "${nuttx_break_rollback}" = 1; then
    if ! "${NUTTX_BREAK_PATCH_TOOL}" reverse; then
      echo "failed to restore the NuttX Xtensa BREAK backport" >&2
      if test "${rc}" -eq 0; then
        rc=1
      fi
    fi
  fi

  if test "${nuttx_wlan_rollback}" = 1; then
    if ! "${NUTTX_WLAN_PATCH_TOOL}" reverse; then
      echo "failed to restore the ESP32-S3 WLAN backport" >&2
      if test "${rc}" -eq 0; then
        rc=1
      fi
    fi
  fi

  if test "${nuttx_raw_exc_rollback}" = 1; then
    if ! "${NUTTX_RAW_EXC_PATCH_TOOL}" reverse; then
      echo "failed to restore the raw Xtensa exception diagnostic" >&2
      if test "${rc}" -eq 0; then
        rc=1
      fi
    fi
  fi

  if test "${hal_rollback}" = 1; then
    if ! "${HAL_PATCH_TOOL}" reverse; then
      echo "failed to restore the HAL backport" >&2
      if test "${rc}" -eq 0; then
        rc=1
      fi
    fi
  fi

  if test "${ai_agent_overlay_applied}" = 1; then
    if ! restore_local_sources; then
      echo "failed to restore the pre-build ai_agent source tree" >&2
      if test "${rc}" -eq 0; then
        rc=1
      fi
    fi
  fi

  if test "${rc}" -eq 0; then
    cleanup_local_source_snapshot
  else
    echo "Build/recovery failed; source snapshot kept at ${LOCAL_SOURCE_SNAPSHOT}" >&2
  fi
  exit "${rc}"
}

# clean/distclean are destructive maintenance operations.  They must not run
# inside the temporary-backport lifecycle: distclean removes the HAL checkout
# and generated mbedTLS config, making the EXIT restore helpers impossible.
if is_maintenance_target "$@"; then
  run_maintenance_target "$@"
  exit $?
fi

# Snapshot user-owned source before any context/bootstrap operation.  This is
# intentionally outside the compatibility-patch lifecycle: temporary HAL and
# NuttX backports may be applied/reversed, but local source must be compiled
# exactly as it existed when this script started.
# Repair the contest copy, never the temporary upstream copy. The same
# preparation runs for direct shell builds and build_voice.py builds.
python3 "${OPENVELA_ROOT}/contest2026_295_suanliheidong/app/robot_voice/scripts/build_voice.py" \
  --openvela "${OPENVELA_ROOT}" --prepare-only
python3 "${OPENVELA_ROOT}/contest2026_295_suanliheidong/app/robot_voice/scripts/fix_link_dependencies.py" \
  --openvela "${OPENVELA_ROOT}"

snapshot_local_sources
trap restore_backports EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Context needs a complete Agent even when packages/ai_agent was deleted.
apply_contest_ai_agent_overlay
refresh_demo_kconfig
bootstrap_hal_checkout
prepare_build_context
# build.sh context runs savedefconfig and may omit explicit "not set"
# entries; re-apply the contest network policy before validating .config.
python3 "${OPENVELA_ROOT}/contest2026_295_suanliheidong/app/robot_voice/scripts/build_voice.py" \
  --openvela "${OPENVELA_ROOT}" --prepare-only
restore_local_sources
apply_contest_ai_agent_overlay
python3 "${OPENVELA_ROOT}/contest2026_295_suanliheidong/app/robot_voice/scripts/fix_link_dependencies.py" \
  --openvela "${OPENVELA_ROOT}" --verify-active

# Compile contest-owned strong public voice/HTTP entry points and weak upstream
# defaults via EXTRAFLAGS. The complete contest Agent is staged temporarily.
# Application Make/CMake no longer infer a nonexistent streaming callback ABI.

# The first build.sh context is required to create the normal ESP32-S3 build
# environment, but it may have generated the robot_voice source list before
# the user's contest-local files were restored.  Refresh context now so new
# files such as robot_music_player.c are part of apps_robot_voice/libapps.a.
refresh_restored_build_context
# The context refresh may repopulate packages/ai_agent from the upstream
# source tree. Reapply the contest overlay immediately before compilation so
# the active vela_tls.c and network_manager.c share the same transport API.
apply_contest_ai_agent_overlay

patch_state="$("${HAL_PATCH_TOOL}" apply)"
echo "HAL backport: ${patch_state}"

if test "${patch_state}" = "APPLIED"; then
  hal_rollback=1
fi

patch_state="$("${NUTTX_BREAK_PATCH_TOOL}" apply)"
echo "NuttX Xtensa BREAK backport: ${patch_state}"

if test "${patch_state}" = "APPLIED"; then
  nuttx_break_rollback=1
fi

patch_state="$("${NUTTX_WLAN_PATCH_TOOL}" apply)"
echo "ESP32-S3 WLAN backport: ${patch_state}"

if test "${patch_state}" = "APPLIED"; then
  nuttx_wlan_rollback=1
fi

if test "${NUTTX_RAW_EXC}" = 1; then
  patch_state="$(${NUTTX_RAW_EXC_PATCH_TOOL} apply)"
  echo "NuttX raw Xtensa exception diagnostic: ${patch_state}"

  if test "${patch_state}" = "APPLIED"; then
    nuttx_raw_exc_rollback=1
  fi
fi

echo "ai_agent source policy: contest-local overlay applied; upstream package restored after build"
patch_state="$("${AI_AGENT_PATCH_TOOL}" apply)"
echo "ai_agent build compatibility: ${patch_state}"

if test "${patch_state}" = "APPLIED"; then
  ai_agent_rollback=1
fi

patch_state="$(${FFMPEG_AMIX_PATCH_TOOL} apply)"
echo "FFmpeg amix helper backport: ${patch_state}"

if test "${patch_state}" = "APPLIED"; then
  ffmpeg_amix_rollback=1
fi

patch_state="$(${MEDIA_HTTP_RECONNECT_PATCH_TOOL} apply)"
echo "Media HTTP reconnect backport: ${patch_state}"

if test "${patch_state}" = "APPLIED"; then
  media_http_reconnect_rollback=1
fi

patch_state="$(${MEDIA_COMPLETION_PATCH_TOOL} apply)"
echo "Media completed-state backport: ${patch_state}"

if test "${patch_state}" = "APPLIED"; then
  media_completion_rollback=1
fi

verify_voice_fix_sources
force_clean_rebuild
run_configured_make "$@"
verify_built_voice_image "$@"
