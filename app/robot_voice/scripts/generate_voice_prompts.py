#!/usr/bin/env python3
"""Validate local WAVs and embed only PCM bytes as firmware RODATA."""
import argparse
from pathlib import Path
import wave
import tempfile
import os

NAMES = ('startup', 'network_error', 'chat_exit', 'not_heard')
RATE = 24000
MAX_SECONDS = 8

def generate(source, output, require_all=False):
    source, output = Path(source), Path(output)
    unknown = {p.stem for p in source.glob('*.wav')} - set(NAMES)
    if unknown:
        raise ValueError('未知提示音名称: ' + ', '.join(sorted(unknown)))
    lines = ['/* Generated; do not edit. PCM16 mono 24000 Hz. */',
             '#include "robot_voice_prompts.h"']
    entries = []
    for name in NAMES:
        path = source / (name + '.wav')
        if not path.exists():
            if require_all:
                raise ValueError(f'缺少提示音: {path}')
            entries.append('{NULL, 0}')
            continue
        with wave.open(str(path), 'rb') as w:
            if (w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getcomptype()) != (1, 2, RATE, 'NONE'):
                raise ValueError(f'{path}: 需要 24000 Hz、单声道、16-bit PCM WAV')
            frames = w.getnframes()
            if not 0 < frames <= RATE * MAX_SECONDS:
                raise ValueError(f'{path}: 时长必须在 (0, {MAX_SECONDS}] 秒')
            pcm = w.readframes(frames)
            if len(pcm) != frames * 2:
                raise ValueError(f'{path}: WAV 数据不完整')
        lines.append(f'static const uint8_t pcm_{name}[] = {{')
        lines.extend('  ' + ','.join(f'0x{b:02x}' for b in pcm[i:i+24]) + ','
                     for i in range(0, len(pcm), 24))
        lines.append('};')
        entries.append(f'{{pcm_{name}, sizeof(pcm_{name})}}')
    lines.append('static const struct robot_voice_prompt_asset_s assets[RV_PROMPT_COUNT] = {')
    lines.extend('  ' + entry + ',' for entry in entries)
    lines.extend(['};',
        'struct robot_voice_prompt_asset_s robot_voice_prompt_asset(enum robot_voice_prompt_e id)',
        '{', '  if ((unsigned int)id < RV_PROMPT_COUNT) return assets[id];',
        '  return (struct robot_voice_prompt_asset_s){NULL, 0};', '}', ''])
    content = '\n'.join(lines)
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != content:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=output.parent,
                                         prefix=output.name+'.', delete=False) as f:
            f.write(content)
            temp = Path(f.name)
        try:
            os.replace(temp, output)
        finally:
            temp.unlink(missing_ok=True)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--require-all', action='store_true')
    a = p.parse_args()
    try:
        generate(a.input, a.output, a.require_all)
    except (ValueError, OSError, EOFError, wave.Error) as e:
        p.exit(2, f'提示音生成失败: {e}\n')

if __name__ == '__main__':
    main()
