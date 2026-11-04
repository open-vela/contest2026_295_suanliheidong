#!/usr/bin/env python3
"""Set the board's global C++ dialect before its normal configure/build step.

The reported board build fails in libc++ <string> with the default gnu++20
profile. Use C++17 for this application and its libraries. Changing only
robot_voice's CXXFLAGS would leave libc++ itself on the failing profile.
"""
import argparse
import os
from pathlib import Path
import re
import stat
import sys
import tempfile

KEY = 'CONFIG_CXX_STANDARD'
VALUE = '"gnu++17"'
PATTERN = re.compile(r'^\s*(?:#\s*)?CONFIG_CXX_STANDARD(?:\s*=.*|\s+is not set)\s*$')


def set_standard(data):
    """Preserve every other configuration line, including its line ending."""
    text = data.decode('utf-8')
    newline = '\r\n' if '\r\n' in text else '\n'
    result = []
    inserted = False
    for line in text.splitlines(keepends=True):
        if PATTERN.fullmatch(line.rstrip('\r\n')):
            if not inserted:
                result.append(KEY + '=' + VALUE + newline)
                inserted = True
        else:
            result.append(line)
    if not inserted:
        if result and not result[-1].endswith(('\r', '\n')):
            result.append(newline)
        result.append(KEY + '=' + VALUE + newline)
    return ''.join(result).encode('utf-8')


def atomic_write(path, data):
    name = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix='cxx17-', delete=False) as f:
            name = Path(f.name)
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        name.chmod(stat.S_IMODE(path.stat().st_mode))
        os.replace(name, path)
    finally:
        if name is not None and name.exists():
            name.unlink()


def configure_cxx17(root, check=False):
    root = Path(root).resolve()
    source = root/'contest2026_295_suanliheidong/board/contest_board/configs/nsh/defconfig'
    vendor = root/'vendor/openvela/boards/contest2026_295_board/configs/nsh/defconfig'
    if not source.is_file():
        raise RuntimeError('board source defconfig missing: '+str(source))
    paths = []
    for p in [source, *([vendor] if vendor.is_file() else [])]:
        target = p.resolve(strict=True)  # Preserve an existing board symlink.
        if target not in paths:
            paths.append(target)
    changes = []
    for p in paths:
        before = p.read_bytes()
        after = set_standard(before)
        changes.append((p, before, after))
    if check:
        wrong = [str(p) for p, before, after in changes if before != after]
        if wrong:
            raise RuntimeError('set '+KEY+'='+VALUE+' before building: '+', '.join(wrong))
    else:
        for p, before, after in changes:
            if before != after:
                backup = p.with_name(p.name+'.before-tflite-cxx17')
                if not backup.exists():
                    with backup.open('xb') as f:
                        f.write(before)
                atomic_write(p, after)
                print('[voice-build] C++17 profile applied: '+str(p))
    print('[voice-build] '+KEY+'='+VALUE+' (global libc++ and TFLM build)')
    return [p for p, before, after in changes if before != after]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openvela', required=True, type=Path)
    parser.add_argument('--check', action='store_true', help='read-only profile check')
    args = parser.parse_args()
    configure_cxx17(args.openvela, args.check)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as exc:
        print('[voice-build] ERROR: '+str(exc), file=sys.stderr)
        sys.exit(2)
