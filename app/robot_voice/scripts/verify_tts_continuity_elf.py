#!/usr/bin/env python3
"""Confirm the continuity default and diagnostic command were linked into an ELF."""
import argparse
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('elf',type=Path)
a=p.parse_args()
data=a.elf.read_bytes()
markers=(b'validated-turn-cache',b'[RV-CONTINUITY]',b'[RV-NET-RX]',b'[RV-TTS-NET-TEST]')
missing=[m.decode() for m in markers if m not in data]
if missing:p.error('Missing new firmware markers: '+', '.join(missing))
print('PASS: continuity profile, transport metrics and direct TTS probe present; hardware playback not verified')
