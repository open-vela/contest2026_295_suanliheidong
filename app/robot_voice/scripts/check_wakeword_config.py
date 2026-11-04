#!/usr/bin/env python3
"""Check the effective NuttX .config and model import before a board build."""
import argparse
from pathlib import Path
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--config', required=True, type=Path, help='实际构建目录中的 .config，不是 defconfig 模板')
    p.add_argument('--apps', required=True, type=Path, help='openvela/apps')
    a = p.parse_args()
    values = {}
    for line in a.config.read_text().splitlines():
        if line.startswith('CONFIG_') and '=' in line:
            key, value = line.split('=', 1); values[key] = value
    required = ['CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_VOICE', 'CONFIG_TFLITEMICRO',
                'CONFIG_SYSTEM_FLATBUFFERS', 'CONFIG_MATH_GEMMLOWP', 'CONFIG_MATH_KISSFFT',
                'CONFIG_MATH_RUY', 'CONFIG_HAVE_CXX', 'CONFIG_HAVE_CXXINITIALIZE', 'CONFIG_LIBCXX']
    errors = [k+' 必须为 y' for k in required if values.get(k) != 'y']
    if values.get('CONFIG_CXX_STANDARD') != '"gnu++17"':
        errors.append('CONFIG_CXX_STANDARD 必须为 "gnu++17"；请运行 scripts/build_voice.py 应用全局构建配置')
    for rel in ['mlearning/tflite-micro/tflite-micro/tensorflow/lite/micro/micro_interpreter.h',
                'system/flatbuffers/flatbuffers/include/flatbuffers/flatbuffers.h',
                'math/gemmlowp/gemmlowp/fixedpoint/fixedpoint.h', 'math/ruy/ruy/ruy/profiler/instrumentation.h']:
        if not (a.apps/rel).is_file(): errors.append('缺少源码: '+str(a.apps/rel))
    source = Path(__file__).resolve().parent.parent/'robot_wakeword_model_data.cc'
    if 'MODEL_NOT_IMPORTED' in source.read_text(): errors.append('先运行 prepare_wakeword_model.py 导入实际模型')
    if values.get('CONFIG_ARCH_CHIP_ESP32S3') == 'y':
        for key in ['CONFIG_XTENSA_HIFI', 'CONFIG_MLEARNING_CMSIS_NN']:
            if values.get(key) == 'y': errors.append('ESP32-S3 本方案使用通用内核，请关闭 '+key)
    if errors:
        print('\n'.join('ERROR: '+e for e in errors), file=sys.stderr)
        return 2
    print('PASS: effective Kconfig dependencies, TFLM headers and imported model are present')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print('ERROR: '+str(exc), file=sys.stderr); sys.exit(2)
