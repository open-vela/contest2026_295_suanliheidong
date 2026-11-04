#!/usr/bin/env python3
"""Reject firmware that linked default/weak voice entry points."""
import argparse
from pathlib import Path
import subprocess
import sys

SYMBOLS = ('voice_channel_init', 'voice_channel_start', 'voice_channel_speak',
           'robot_voice_link_anchor', 'robot_wakeword_init',
           'robot_wakeword_model_init', 'robot_expression_init',
           'robot_music_media_link_anchor', 'robot_music_media_open',
           'robot_voice_music_route_tool_anchor',
           'robot_legacy_media_open', 'robot_legacy_media_prepare',
           'robot_legacy_media_start')


def verify(path):
    path = Path(path)
    output = subprocess.check_output(['readelf', '-Ws', str(path)], text=True)
    symbols = {}
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[0].rstrip(':').isdigit():
            symbols[parts[7]] = parts
    missing = [name for name in SYMBOLS if name not in symbols or
               symbols[name][4] != 'GLOBAL' or symbols[name][6] == 'UND']
    if missing:
        raise RuntimeError('missing strong contest voice symbols: ' + ', '.join(missing))
    if any(name.startswith('robot_direct_media_') for name in symbols):
        raise RuntimeError('alternate HTTP/MP3 music backend is linked; Agent music must use OpenVela Media only')
    if b'[RV-VOICE-PROVIDER] contest TFLite voice ready' not in path.read_bytes():
        raise RuntimeError('contest voice provider marker is absent from ELF')
    if 'esp_wifi_sta_connect' in symbols and b'[RV-WIFI-DRV] association_timeout' not in path.read_bytes():
        raise RuntimeError('ESP32-S3 Wi-Fi driver backport was not compiled; check voice_cc.py build launcher')
    if 'esp_wifi_sta_connect' in symbols and b'[RV-WIFI-MEM] stage=' not in path.read_bytes():
        raise RuntimeError('V3 Wi-Fi memory diagnostics were not rebuilt; clean the old driver objects')
    if b'[RV-TCP-POLICY] connect=' not in path.read_bytes():
        raise RuntimeError('new TCP connect policy is absent; clean robot_voice objects')
    if b'[RV-TLS-POLICY] tx=' not in path.read_bytes():
        raise RuntimeError('TLS backpressure fix is absent; clean robot_voice HTTP objects')
    image = path.read_bytes()
    if b'[RV-MUSIC] backend=contest-legacy-http-helix' not in image:
        raise RuntimeError('contest HTTP/Helix music backend is absent; clean and rebuild robot_voice')
    if b'[RV-MEDIA] native Media disabled' not in image:
        raise RuntimeError('native Media lifecycle is still enabled; clean and rebuild robot_voice')
    if b'HTTP MP3 begin url=' not in image:
        raise RuntimeError('contest HTTP/MP3 decoder path is absent; clean and rebuild robot_voice')
    if b'[RV-PB] DMA ready slots=' not in image:
        raise RuntimeError('robot_audio_playback DMA path is absent; clean and rebuild robot_voice')
    active = path.parent / '.config'
    if active.is_file():
        from fix_link_dependencies import values, needs_native_wifi_heap, wifi_heap_errors
        config = values(active)
        # The contest music tool intentionally bypasses mediad.  CONFIG_MEDIA
        # may still be enabled for unrelated media applications, so do not use
        # its presence as evidence that the music path is native Media.
        errors = wifi_heap_errors(config)
        if errors:
            raise RuntimeError('wrong effective Wi-Fi heap layout: ' + ', '.join(errors))
        if needs_native_wifi_heap(config):
            if ('up_allocate_kheap' not in symbols or
                    symbols['up_allocate_kheap'][4] != 'GLOBAL' or
                    symbols['up_allocate_kheap'][6] == 'UND' or
                    b'kernel-dram' not in path.read_bytes()):
                raise RuntimeError('ELF still lacks the native internal kernel heap; clean and rebuild all objects')
            print('[voice-elf] native PSRAM user heap and internal kernel/Wi-Fi heap linked')
        if config.get('CONFIG_LVX_USE_DEMO_CONTEST2026_295_ROBOT_VOICE') == 'y':
            from build_voice import verify_active_board_config
            verify_active_board_config(path.parent.parent)
    if 'ai_agent_main' in symbols and b'[AGENT-TASK-ERROR]' not in path.read_bytes():
        raise RuntimeError('Agent thread diagnostics were not rebuilt; check contest Agent source sync and clean build')
    if ('contest_i2s_tx_ioctl' in symbols or 'g_contest_i2s1' in symbols):
        image = path.read_bytes()
        legacy_dma_marker = b'[RV-DMA] selected slots=' in image
        current_dma_marker = (b'[C-I2S] tx GDMA-IMMUTABLE start=' in image and
                              b'[C-I2S] tx GDMA-IMMUTABLE armed' in image)
        if not (legacy_dma_marker or current_dma_marker):
            raise RuntimeError('contest I2S DMA backport was not compiled; check voice_cc.py and clean board objects')
        if b'[RV-PB] DMA ready slots=' not in image:
            raise RuntimeError('upper PCM player lacks negotiated DMA slots; rebuild robot_voice')
        if b'[RV-AUDIO-ERROR]' not in image:
            raise RuntimeError('TTS main still lacks audio error separation; clean and rebuild robot_voice')
    print('[voice-elf] strong voice entry, acoustic model and expression linked')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    args = parser.parse_args()
    try:
        verify(args.elf)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print('[voice-elf] ERROR: ' + str(exc), file=sys.stderr)
        sys.exit(2)
