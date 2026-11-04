#!/usr/bin/env python3
"""Sync board network buffers and C++17, then run the existing board build."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from fix_tflite_cxx_standard import configure_cxx17
from fix_link_dependencies import configure_link_dependencies, verify_active

VOICE = Path(__file__).resolve().parent.parent

BOARD_KCONFIG = {
    'CONFIG_LIB_HELIX_MP3': 'y',
    'CONFIG_NETUTILS_WEBCLIENT': 'y',
    # NetEase music redirects carry signed URLs longer than NuttX defaults.
    'CONFIG_WEBCLIENT_MAXHTTPLINE': '1024',
    'CONFIG_WEBCLIENT_MAXFILENAME': '1024',
    'CONFIG_IOB_BUFSIZE': '512',
    'CONFIG_IOB_NBUFFERS': '32',
    'CONFIG_IOB_THROTTLE': '16',
    'CONFIG_ESP32S3_WIFI_STATIC_RXBUF_NUM': '10',
    'CONFIG_ESP32S3_WIFI_DYNAMIC_RXBUF_NUM': '32',
    'CONFIG_ESP32S3_WIFI_RXBA_AMPDU_WZ': '6',
    'CONFIG_ESP32S3_WLAN_PKTBUF_NUM': '32',
    'CONFIG_NET_ETH_PKTSIZE': '1024',
    # Bound each TCP receive queue so LLM + TTS cannot claim the whole IOB
    # pool while matching the music-motion board profile.
    'CONFIG_NET_RECV_BUFSIZE': '8192',
    'CONFIG_NET_TCP_DELAYED_ACK': 'n',
}

def config_values(path):
    values = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if line.startswith('CONFIG_') and '=' in line:
            name, value = line.split('=', 1)
            values[name] = value.strip('"')
        elif line.startswith('# CONFIG_') and line.endswith(' is not set'):
            values[line[2:-11]] = 'n'
    return values

def set_config_value(text, name, value):
    lines = text.splitlines()
    prefix = name + '='
    disabled = '# ' + name + ' is not set'
    replacement = disabled if value == 'n' else prefix + value
    output = []
    inserted = False
    for line in lines:
        if line.startswith(prefix) or line == disabled:
            if not inserted:
                output.append(replacement)
                inserted = True
        else:
            output.append(line)
    if not inserted:
        output.append(replacement)
    return '\n'.join(output) + '\n'

def sync_board_network_config(root):
    source = root / 'contest2026_295_suanliheidong/board/contest_board/configs/nsh/defconfig'
    target = root / 'vendor/openvela/boards/contest2026_295_board/configs/nsh/defconfig'
    if not source.is_file():
        raise RuntimeError('contest board defconfig missing: ' + str(source))
    if not target.is_file():
        raise RuntimeError('active vendor board defconfig missing: ' + str(target))

    # Update both authoritative and copied board configs. An upgrade from
    # the old 8192-byte policy must migrate instead of failing its preflight.
    for path in dict.fromkeys((source.resolve(), target.resolve())):
        original = path.read_text()
        updated = original
        for name, value in BOARD_KCONFIG.items():
            updated = set_config_value(updated, name, value)
        if updated != original:
            backup = path.with_name(path.name + '.before-robot-voice-network')
            serial = 0
            while backup.exists() and backup.read_text() != original:
                serial += 1
                backup = path.with_name(path.name + '.before-robot-voice-network.' + str(serial))
            if not backup.exists():
                backup.write_text(original)
            path.write_text(updated)
            print('[voice-build] synced bounded IOB/TCP receive settings into ' + str(path))

def verify_active_board_config(root):
    active = root / 'nuttx/.config'
    if not active.is_file():
        raise RuntimeError('board build returned success but nuttx/.config is missing')
    values = config_values(active)
    errors = [name + '=' + str(values.get(name)) + ' expected ' + value
              for name, value in BOARD_KCONFIG.items()
              if values.get(name) != value]
    if errors:
        raise RuntimeError('active NuttX configuration did not apply board audio/network settings: ' +
                           ', '.join(errors))
    print('[voice-build] active NuttX IOB/TCP receive settings verified')

def restore_legacy_backup(root):
    state = VOICE / 'scripts/.agent-adapter-state'
    manifest = json.loads((state / 'state.json').read_text())
    target = Path(manifest['target']).resolve()
    if not target.is_relative_to((root / 'packages/ai_agent').resolve()):
        raise RuntimeError('legacy backup target is outside this official Agent tree')
    if hashlib.sha256(target.read_bytes()).hexdigest() != manifest['patched_sha256']:
        raise RuntimeError('official file changed; refusing to overwrite it. Backup: ' + str(state))
    target.write_bytes((state / 'original.c').read_bytes())
    (state / 'state.json').unlink()
    (state / 'original.c').unlink()
    state.rmdir()
    print('[voice-build] restored interrupted legacy policy patch')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openvela', type=Path, required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true')
    mode.add_argument('--restore', action='store_true', help='recover only an interrupted OLD patch build')
    parser.add_argument('--prepare-only', action='store_true', help='prepare direct board entry without invoking it')
    parser.add_argument('--build-script', type=Path)
    parser.add_argument('build_args', nargs=argparse.REMAINDER)
    opt = parser.parse_args()
    root = opt.openvela.resolve()
    if opt.restore:
        restore_legacy_backup(root)
        return 0
    board = opt.build_script or root / 'contest2026_295_suanliheidong/board/contest_board/scripts/build_with_hal_backport.sh'
    if not board.is_file(): raise RuntimeError('board build script missing: ' + str(board))
    configure_link_dependencies(root, check=opt.check)
    if opt.check:
        configure_cxx17(root, check=True)
        if 'ROBOT_VOICE_TRANSPORT_HOOKS_V2' not in board.read_text():
            raise RuntimeError('replace the board script with the version included in this package')
        for name in ('robot_voice_agent_hooks.h', 'robot_voice_http.c', 'robot_voice_http_parser.c', 'robot_voice_weather.c', 'robot_voice_skill_guard.c'):
            if not (VOICE / name).is_file(): raise RuntimeError('missing contest file: ' + name)
        print('[voice-build] board entry and fixed contest adapter present; no official file modified')
        return 0
    args = opt.build_args
    if args and args[0] == '--': args = args[1:]
    sync_board_network_config(root)
    configure_cxx17(root)
    if opt.prepare_only:
        return 0
    status = subprocess.call(['bash', str(board), *(args or ['-j8'])], cwd=root)
    if status == 0:
        verify_active(root)
        verify_active_board_config(root)
    return status

if __name__ == '__main__':
    try: sys.exit(main())
    except (ValueError, RuntimeError, OSError) as exc:
        print('[voice-build] ERROR:', exc, file=sys.stderr)
        sys.exit(2)
