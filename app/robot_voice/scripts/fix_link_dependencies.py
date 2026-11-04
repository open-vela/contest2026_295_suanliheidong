#!/usr/bin/env python3
"""Restore the contest voice provider before the normal board configure/build."""
import argparse
import os
from pathlib import Path
import re
import shutil
import sys
from wifi_driver_backport import invalidate_objects
from audio_dma_backport import invalidate_objects as invalidate_audio_objects

CONTEST = 'contest2026_295_suanliheidong'
VOICE_KEY = 'CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_VOICE'
MCP_KEY = 'CONFIG_AI_AGENT_MCP'
MAKE_BEGIN = '# BEGIN RV295_AGENT_LINK_V1'
MAKE_END = '# END RV295_AGENT_LINK_V1'
AMIX = ('alloc', 'free', 'input_write', 'input_write_down', 'read',
        'input_empty', 'input_want')
REQUIRED = (
    'CONFIG_EXAMPLES_AI_AGENT_VELA', 'CONFIG_I2S', 'CONFIG_AUDIO',
    'CONFIG_WIRELESS_WAPI', 'CONFIG_NETUTILS_DHCPC', 'CONFIG_NETDB_DNSCLIENT',
    'CONFIG_NET_IPv4', 'CONFIG_NET_UDP', 'CONFIG_NETDEV_WIRELESS_IOCTL',
    'CONFIG_NETUTILS_CJSON', 'CONFIG_TFLITEMICRO', 'CONFIG_SYSTEM_FLATBUFFERS',
    'CONFIG_MATH_GEMMLOWP', 'CONFIG_MATH_KISSFFT', 'CONFIG_MATH_RUY',
    'CONFIG_HAVE_CXX', 'CONFIG_HAVE_CXXINITIALIZE', 'CONFIG_LIBCXX', VOICE_KEY,
)


def values(path):
    result = {}
    for line in path.read_text().splitlines():
        if line.startswith('CONFIG_') and '=' in line:
            key, value = line.split('=', 1)
            result[key] = value
    return result


def needs_native_wifi_heap(config):
    # Native OpenVela flat-build layout: user allocations in PSRAM and the
    # kernel/radio heap in internal DRAM. Do not use the common-heap choice,
    # nor the Xtensa separate heap which also captures every thread stack.
    return (config.get('CONFIG_ESP32S3_WIFI') == 'y' and
            config.get('CONFIG_ESP32S3_SPIRAM') == 'y' and
            config.get('CONFIG_BUILD_PROTECTED') != 'y' and
            config.get('CONFIG_BUILD_KERNEL') != 'y')


def configure_wifi_heap(text):
    """Select OpenVela's native PSRAM user heap, preserving stack sizes."""
    config = {key: value.rstrip('\r') for key, value in
              re.findall(r'(?m)^(CONFIG_\w+)=(.*)$', text)}
    if needs_native_wifi_heap(config):
        for key in ('CONFIG_ESP32S3_SPIRAM_BOOT_INIT',
                    'CONFIG_ESP32S3_SPIRAM_USER_HEAP', 'CONFIG_MM_KERNEL_HEAP'):
            text = enable_config(text, key)
        for key in ('CONFIG_ESP32S3_SPIRAM_COMMON_HEAP',
                    'CONFIG_XTENSA_IMEM_USE_SEPARATE_HEAP'):
            text = disable_config(text, key)
    return text


def wifi_heap_errors(config):
    if not needs_native_wifi_heap(config):
        return []
    bad = [key + '=y' for key in ('CONFIG_ESP32S3_SPIRAM_BOOT_INIT',
           'CONFIG_ESP32S3_SPIRAM_USER_HEAP', 'CONFIG_MM_KERNEL_HEAP')
           if config.get(key) != 'y']
    bad += [key + ' must be disabled' for key in
            ('CONFIG_ESP32S3_SPIRAM_COMMON_HEAP', 'CONFIG_XTENSA_IMEM_USE_SEPARATE_HEAP')
            if config.get(key) == 'y']
    return bad


def read_text(path):
    return path.read_bytes().decode('utf-8')


def enable_voice(data):
    """Enable the contest voice provider, preserving unrelated config lines."""
    return enable_config(data, VOICE_KEY)


def enable_config(data, key):
    """Enable one Kconfig boolean while preserving other config lines."""
    newline = '\r\n' if '\r\n' in data else '\n'
    pattern = re.compile(r'^\s*(?:' + re.escape(key) + r'=.*|#\s*' +
                         re.escape(key) + r' is not set)\s*$')
    lines = []
    inserted = False
    for line in data.splitlines(keepends=True):
        if pattern.fullmatch(line.rstrip('\r\n')):
            if not inserted:
                lines.append(key + '=y' + newline)
                inserted = True
        else:
            lines.append(line)
    if not inserted:
        if lines and not lines[-1].endswith(('\n', '\r')):
            lines.append(newline)
        lines.append(key + '=y' + newline)
    return ''.join(lines)


def disable_config(data, key):
    # Canonicalize duplicates and preserve LF/CRLF using the existing editor.
    enabled = enable_config(data, key)
    return re.sub(r'(?m)^' + re.escape(key) + r'=y(?=\r?$)',
                  '# ' + key + ' is not set', enabled)


def _make_has_source(text, source):
    return bool(re.search(r'(?m)^\s*CSRCS\s*\+=.*(?:^|\s)' +
                          re.escape(source) + r'(?:\s|$)', text))


def ensure_agent_make_sources(text, available=None):
    """Add core sources omitted from the Make build; keep MCP conditional."""
    if text.count(MAKE_BEGIN) != text.count(MAKE_END) or text.count(MAKE_BEGIN) > 1:
        raise RuntimeError('malformed previous Agent Makefile link block')
    text = re.sub(re.escape(MAKE_BEGIN) + r'.*?' + re.escape(MAKE_END) +
                  r'\r?\n?', '', text, flags=re.S)
    required = ('src/infra/url_parse.c', 'src/infra/a2a_handler.c')
    if available is not None:
        required = tuple(source for source in required if source in available)
    missing = [source for source in required if not _make_has_source(text, source)]
    if (available is None or 'src/tools/mcp_server.c' in available) and not _make_has_source(text, 'src/tools/mcp_server.c'):
        missing.append('src/tools/mcp_server.c')
    if not missing:
        return text

    lines = [MAKE_BEGIN]
    unconditional = [s for s in missing if s != 'src/tools/mcp_server.c']
    if unconditional:
        lines.append('CSRCS += ' + ' '.join(unconditional))
    if 'src/tools/mcp_server.c' in missing:
        lines.extend(('ifeq ($(CONFIG_AI_AGENT_MCP),y)',
                      'CSRCS += src/tools/mcp_server.c', 'endif'))
    lines.append(MAKE_END)

    include = r'(?m)^include\s+\$[({]APPDIR[)}]/Application\.mk\s*$'
    matches = list(re.finditer(include, text))
    if len(matches) != 1:
        raise RuntimeError('cannot locate unique Application.mk include in packages/ai_agent/Makefile')
    newline = '\r\n' if '\r\n' in text else '\n'
    addition = newline.join(lines) + newline
    start = matches[0].start()
    return text[:start] + addition + text[start:]


def ensure_agent_link_anchor(text):
    """Keep the voice provider and its FFmpeg compatibility object reachable."""
    declaration = 'extern void robot_voice_link_anchor(void);'
    call = 'robot_voice_link_anchor();'
    if call not in text:
        include = '#include "voice/voice_channel.h"'
        if text.count(include) != 1:
            raise RuntimeError('cannot locate unique voice_channel.h include in agent_main.c')
        pattern = r'(?m)^([ \t]*)rc\s*=\s*voice_channel_init\s*\(\s*\)\s*;\s*$'
        matches = list(re.finditer(pattern, text))
        if len(matches) != 1:
            raise RuntimeError('cannot locate unique rc = voice_channel_init(); in agent_main.c')
        newline = '\r\n' if '\r\n' in text else '\n'
        m = matches[0]
        block = (m.group(1) + '#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_VOICE' + newline +
                 m.group(1) + '  ' + call + newline +
                 m.group(1) + '#endif' + newline)
        text = text[:m.start()] + block + text[m.start():]
    if declaration not in text:
        include = '#include "voice/voice_channel.h"'
        if text.count(include) != 1:
            raise RuntimeError('cannot locate unique voice_channel.h include in agent_main.c')
        newline = '\r\n' if '\r\n' in text else '\n'
        declaration_block = ('#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_VOICE' + newline +
                             declaration + newline + '#endif' + newline)
        text = text.replace(include, include + newline + declaration_block, 1)
    return text


def ensure_amix_link_anchor(text):
    """Add a passive archive anchor only after checking the real amix code."""
    if '#include "../../../external/ffmpeg/ffmpeg/libavfilter/amix.c"' in text:
        if 'void robot_ffmpeg_amix_compat_link_anchor(void)' not in text:
            raise RuntimeError('real FFmpeg include requires its archive anchor')
        return text
    for name in AMIX:
        if not re.search(r'\bff_amix_' + name + r'\s*\([^;{}]*\)\s*\{', text, re.S):
            raise RuntimeError('robot_core compatibility source is missing real ff_amix_' + name)
    if re.search(r'\brobot_ffmpeg_amix_compat_link_anchor\s*\(\s*(?:void)?\s*\)\s*\{', text):
        return text
    newline = '\r\n' if '\r\n' in text else '\n'
    suffix = '' if not text or text.endswith(('\n', '\r')) else newline
    return (text + suffix + newline +
            '/* RV295_LINK_V1: retain this object from static archives. */' + newline +
            'void robot_ffmpeg_amix_compat_link_anchor(void) {}' + newline)


def write_backup(path, text):
    before = path.read_bytes()
    after = text.encode('utf-8')
    if before == after:
        return False
    shutil.copy2(path, backup_name(path))
    path.write_bytes(after)
    return True


def backup_name(path):
    base = path.with_name(path.name + '.before-voice-link-fix')
    candidate = base
    suffix = 0
    while candidate.exists() or candidate.is_symlink():
        suffix += 1
        candidate = Path(str(base) + '.' + str(suffix))
    return candidate


def validate_inputs(root, check_patches=True):
    root = Path(root).resolve()
    voice = root / CONTEST / 'app/robot_voice'
    agent = root / CONTEST / 'packages/ai_agent'
    core = voice.parent / 'robot_core'
    # These are existing project components, not substitute/dummy providers.
    required = (
        voice / 'Makefile', voice / 'Make.defs', voice / 'Kconfig',
        voice / 'robot_voice_main.c', voice / 'scripts/voice_gcc.sh',
        voice / 'scripts/voice_cc.py', voice / 'robot_wakeword_tflm.cc',
        voice / 'robot_wakeword_model_data.cc',
        agent / 'Makefile', agent / 'Kconfig',
        agent / 'src/core/message_bus.c', agent / 'src/core/message_bus_tap.c',
        agent / 'src/channels/cmd_voice.c',
        core / 'robot_ffmpeg_amix_compat.c', core / 'robot_ffmpeg_amix_compat.h',
        agent / 'src/agent_main.c',
        root / CONTEST / 'scripts/generate_skill_bundle.py',
        root / CONTEST / 'board/contest_board/scripts/build_with_hal_backport.sh',
        root / CONTEST / 'board/contest_board/configs/nsh/defconfig',
        root / 'vendor/openvela/boards/contest2026_295_board/configs/nsh/defconfig',
    )
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        raise RuntimeError('missing real project inputs (do not use stubs):\n' + '\n'.join(missing))
    if 'MODEL_NOT_IMPORTED' in (voice / 'robot_wakeword_model_data.cc').read_text():
        raise RuntimeError('import the real wakeword model before building')
    if not re.search(r'\bvoid\s+robot_voice_link_anchor\s*\(\s*void\s*\)\s*\{',
                     read_text(voice / 'robot_voice_main.c')):
        raise RuntimeError('robot_voice_main.c does not define the strong voice link anchor')
    for source in (agent / 'src/agent_main.c', voice / 'robot_voice_main.c'):
        if check_patches and 'robot_voice_link_anchor' not in source.read_text():
            raise RuntimeError('install both source changes first: ' + str(source))
    make_sources = re.findall(r'\bsrc/[\w/.-]+\.c\b', (agent / 'Makefile').read_text())
    missing_sources = [str(agent / name) for name in make_sources
                       if not (agent / name).is_file()]
    if missing_sources:
        raise RuntimeError('contest Agent is not a complete source tree; sync would delete base files:\n' +
                           '\n'.join(missing_sources))
    compat = read_text(core / 'robot_ffmpeg_amix_compat.c')
    ensure_amix_link_anchor(compat)
    return root, voice, required[-2], required[-1]


def configure_link_dependencies(root, check=False):
    root, voice, source_config, vendor_config = validate_inputs(root, check_patches=False)
    agent = root / CONTEST / 'packages/ai_agent'
    agent_main = agent / 'src/agent_main.c'
    agent_make = agent / 'Makefile'
    compat_source = voice.parent / 'robot_core/robot_ffmpeg_amix_compat.c'
    planned_agent_main = ensure_agent_link_anchor(read_text(agent_main))
    planned_agent_make = ensure_agent_make_sources(read_text(agent_make),
        {str(p.relative_to(agent)) for p in (agent / 'src').rglob('*.c')})
    planned_compat = ensure_amix_link_anchor(read_text(compat_source))
    link = root / 'packages/demos/contest2026_295_robot_voice'
    configs = [source_config.resolve(), vendor_config.resolve()]
    configs = list(dict.fromkeys(configs))
    planned_configs = {}
    for p in configs:
        text = p.read_bytes().decode('utf-8')
        for key in (*REQUIRED, 'CONFIG_PWM',
                    'CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOTCTL',
                    'CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_AGENTCTL'):
            text = enable_config(text, key)
        text = configure_wifi_heap(text)
        planned_configs[p] = text
    bad = [str(p) for p in configs if planned_configs[p] != p.read_bytes().decode('utf-8')]
    if planned_agent_main != read_text(agent_main):
        bad.append(str(agent_main) + ' needs the voice link anchor')
    if planned_agent_make != read_text(agent_make):
        bad.append(str(agent_make) + ' needs the Agent Make source list repair')
    if planned_compat != read_text(compat_source):
        bad.append(str(compat_source) + ' needs the FFmpeg archive anchor')
    launcher = voice / 'scripts/voice_gcc.sh'
    if check:
        if not os.access(launcher, os.X_OK):
            bad.append(str(launcher) + ' needs executable permission')
        if not link.exists() or link.resolve() != voice.resolve():
            bad.append(str(link) + ' must point to ' + str(voice))
        if bad:
            raise RuntimeError('run without --check to repair:\n' + '\n'.join(bad))
    else:
        removed = invalidate_objects(root)
        if removed:
            print('[voice-wifi] invalidated cached ESP32-S3 adapter objects: ' + str(len(removed)))
        removed_audio = invalidate_audio_objects(root)
        if removed_audio:
            print('[voice-audio] invalidated cached contest I2S objects: ' + str(len(removed_audio)))
        launcher.chmod(launcher.stat().st_mode | 0o100)
        if write_backup(agent_main, planned_agent_main):
            print('[voice-link] repaired strong voice anchor in ' + str(agent_main))
        if write_backup(agent_make, planned_agent_make):
            print('[voice-link] repaired Agent Make sources in ' + str(agent_make))
        if write_backup(compat_source, planned_compat):
            print('[voice-link] repaired FFmpeg archive anchor in ' + str(compat_source))
        for path in configs:
            before = path.read_bytes().decode('utf-8')
            after = planned_configs[path]
            if before != after:
                shutil.copy2(path, backup_name(path))
                path.write_bytes(after.encode('utf-8'))
                if configure_wifi_heap(before) != before:
                    print('[voice-heap] native layout: app=PSRAM, kernel/Wi-Fi=internal DRAM in ' + str(path))
                print('[voice-link] enabled contest voice dependencies in ' + str(path))
        if not link.exists() or link.resolve() != voice.resolve():
            link.parent.mkdir(parents=True, exist_ok=True)
            if link.exists() or link.is_symlink():
                link.rename(backup_name(link))
            link.symlink_to(voice, target_is_directory=True)
            print('[voice-link] registered ' + str(link))
    print('[voice-link] real voice/model/amix sources and registration checked')


def verify_active(root):
    path = Path(root) / 'nuttx/.config'
    config = values(path)
    bad = [key for key in REQUIRED if config.get(key) != 'y']
    bad.extend(wifi_heap_errors(config))
    if config.get('CONFIG_CXX_STANDARD') != '"gnu++17"':
        bad.append('CONFIG_CXX_STANDARD="gnu++17"')
    if bad:
        raise RuntimeError('effective nuttx/.config has unsatisfied dependencies: ' + ', '.join(bad))
    print('[voice-link] effective contest voice/TFLM configuration verified')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openvela', required=True, type=Path)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--verify-active', action='store_true')
    args = parser.parse_args()
    if args.verify_active:
        verify_active(args.openvela)
    else:
        configure_link_dependencies(args.openvela, args.check)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as exc:
        print('[voice-link] ERROR: ' + str(exc), file=sys.stderr)
        sys.exit(2)
