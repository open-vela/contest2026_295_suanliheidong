#!/usr/bin/env python3
"""Generate a raw-byte C resource table for contest-local Markdown Skills."""

import argparse
from pathlib import Path


def c_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def byte_array(name: str, data: bytes) -> str:
    if not data:
        return f"static const unsigned char {name}[1] = {{ 0 }};\n"

    lines = []
    for offset in range(0, len(data), 12):
        values = ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 12])
        lines.append(f"  {values},")
    lines[-1] = lines[-1][:-1]
    return (
        f"static const unsigned char {name}[{len(data) or 1}] = {{\n"
        + "\n".join(lines)
        + "\n};\n"
    )


def generate(input_dir: Path, output: Path) -> None:
    skills = sorted(input_dir.glob("*.md"), key=lambda path: path.name)
    chunks = [
        "/* Generated file. Do not edit. */\n",
        '#include "contest_skill_resource.h"\n\n',
    ]

    for index, skill in enumerate(skills):
        data = skill.read_bytes()
        chunks.append(byte_array(f"contest_skill_content_{index}", data))
        chunks.append("\n")

    chunks.append("const struct contest_skill_resource_s contest_skill_resources[] = {\n")
    for index, skill in enumerate(skills):
        data = skill.read_bytes()
        chunks.append(
            f"  {{ {c_string(skill.name)}, contest_skill_content_{index}, "
            f"{len(data)} }},\n"
        )
    if not skills:
        chunks.append("  { NULL, NULL, 0 },\n")
    chunks.append("};\n\n")
    chunks.append(f"const size_t contest_skill_resource_count = {len(skills)};\n")

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(chunks), encoding="ascii")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    generate(args.input, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
