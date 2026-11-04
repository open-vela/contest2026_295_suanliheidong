#!/usr/bin/env python3
"""Embed the explicitly approved contest Skill resources."""
import argparse
import json
from pathlib import Path
import sys


def manifest_files(directory, manifest_path):
    try:
        manifest = json.loads(Path(manifest_path).read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f'cannot read skill manifest: {exc}') from exc

    entries = manifest.get('skills')
    if not isinstance(entries, list) or not entries:
        raise ValueError('manifest skills must be a non-empty list')

    files = []
    names = set()
    for entry in entries:
        if isinstance(entry, str):
            name = entry
        elif isinstance(entry, dict):
            name = entry.get('file')
        else:
            name = None
        if not isinstance(name, str) or not name or '/' in name or not name.endswith('.md'):
            raise ValueError(f'invalid skill filename: {name!r}')
        if name in names:
            raise ValueError(f'duplicate skill in manifest: {name}')
        names.add(name)
        path = Path(directory) / name
        if not path.is_file():
            raise ValueError(f'manifest skill is missing: {path}')
        files.append(path)

    for retired in manifest.get('retired', []):
        path = Path(directory) / retired
        if path.exists():
            raise ValueError(f'retired skill still present: {path}')
    return files


def generate(directory, output, manifest=None):
    files = (manifest_files(directory, manifest) if manifest else
             sorted(Path(directory).glob('*.md')))
    lines = ['/* Generated from contest Markdown assets. */',
             '#include "contest_skill_resource.h"']
    for i, path in enumerate(files):
        data = path.read_bytes()
        lines.append('static const unsigned char skill_%d[] = {%s};' %
                     (i, ','.join(str(b) for b in data + b'\0')))
    lines.append('const struct contest_skill_resource_s contest_skill_resources[] = {')
    for i, path in enumerate(files):
        lines.append('  {%s, skill_%d, sizeof(skill_%d)-1},' %
                     (json.dumps(path.name), i, i))
    if not files:
        lines.append('  {0, 0, 0},')
        print('[voice-build] no contest skills/*.md supplied; voice remains available', file=sys.stderr)
    lines.extend(('};', 'const size_t contest_skill_resource_count = %d;' % len(files), ''))
    output = Path(output)
    data = '\n'.join(lines).encode('utf-8')
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.is_file() or output.read_bytes() != data:
        output.write_bytes(data)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--manifest', type=Path)
    args = parser.parse_args()
    try:
        generate(args.input, args.output, args.manifest)
    except ValueError as exc:
        parser.error(str(exc))
