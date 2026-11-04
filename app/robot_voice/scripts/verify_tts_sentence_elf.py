#!/usr/bin/env python3
"""Check sentence-pipeline markers in a rebuilt firmware; no hardware claim."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('elf', type=Path)
parser.add_argument("--legacy-interrupt", action="store_true")
args = parser.parse_args()
data = args.elf.read_bytes()
if not data.startswith(b'\x7fELF'):
    parser.error('Expected an ELF executable, for example nuttx/nuttx')
markers = (
    b'sentence-cache/prefetch-next',
    b'asr-final-v2' if args.legacy_interrupt else b'afe-speech-v3',
    b'[RV-WAKE-HANDOFF]',
    '[TTS合成]'.encode(),
    '[TTS就绪]'.encode(),
    '[TTS播放]'.encode(),
    '[TTS句结束]'.encode(),
)
missing = [marker.decode() for marker in markers if marker not in data]
if missing:
    parser.error('Missing sentence-pipeline markers: ' + ', '.join(missing))
print('PASS: sentence-pipeline policy and lifecycle markers are linked; verify runtime mode and hardware separately')
