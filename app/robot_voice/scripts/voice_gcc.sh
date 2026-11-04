#!/bin/sh
# Keep CC a single executable for NuttX mkdeps/incdir and recursive make.
set -eu
: "${ROBOT_VOICE_REAL_CC:?Missing native compiler; use the board build entry}"
: "${ROBOT_VOICE_HOOK_HEADER:?Missing voice hook header; use the board build entry}"
voice_script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "${voice_script_dir}/voice_cc.py" "$@"
