#!/usr/bin/env python3
"""Apply hooks to C compilation/dependencies, never plain CPP resources/LD."""
import os
from pathlib import Path
import shlex
import sys

SCRIPTS = Path(__file__).resolve().parent
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))
from wifi_driver_backport import stage_source
from audio_dma_backport import stage_source as stage_audio_source

TRACE_TAGS = (
    b'[C-I2S', b'[WLAN-TXSUBMIT]', b'[WLAN-TXDONE]', b'[WLAN-BUF]',
    b'[WLAN-TLS]', b'[WLAN-TXFLOW]', b'[RV-OLED]', b'[BOARD-OLED]',
    b'[RV-EXPR]',
)


def has_board_trace(arguments):
    # Read explicit C inputs only. Do not modify source files or repository
    # configuration, nor apply printf macros to libc/CPP resource/link inputs.
    for arg in arguments:
        if arg.endswith('.c') and not arg.startswith('-'):
            try:
                source = Path(arg).read_bytes()
            except OSError:
                continue  # Let the real compiler report an invalid input.
            if any(tag in source for tag in TRACE_TAGS):
                return True
    return False


def command(compiler, hook, arguments):
    arguments = list(arguments)
    result = shlex.split(compiler)
    if not result:
        raise ValueError('empty compiler command')
    c_input = any(a.endswith('.c') for a in arguments)
    compiling = '-c' in arguments and '-E' not in arguments
    dependencies = '-M' in arguments or '-MM' in arguments
    if c_input and (compiling or dependencies):
        for index, arg in enumerate(arguments):
            if not arg.startswith('-') and Path(arg).name == 'contest_i2s.c':
                original = Path(arg).resolve()
                cache = Path(os.environ.get('ROBOT_VOICE_AUDIO_CACHE', SCRIPTS / '.audio-sources'))
                arguments[index] = str(stage_audio_source(original, cache))
                result.extend(['-iquote', str(original.parent)])
                if compiling:
                    print('[voice-audio] compiling negotiated internal DMA ring from ' + str(original), file=sys.stderr)
            if (not arg.startswith('-') and Path(arg).name in
                    ('esp32s3_wifi_adapter.c', 'esp32s3_allocateheap.c')):
                original = Path(arg).resolve()
                cache = Path(os.environ.get('ROBOT_VOICE_WIFI_CACHE', SCRIPTS / '.wifi-sources'))
                staged = stage_source(original, cache)
                arguments[index] = str(staged)
                result.extend(['-iquote', str(original.parent)])
                if compiling:
                    print('[voice-wifi] compiling ESP32-S3 driver/layout repairs from ' + str(original), file=sys.stderr)
        if any(not a.startswith('-') and Path(a).name == 'tool_media.c' for a in arguments):
            redirect = Path(hook).resolve().with_name('robot_music_media_redirect.h')
            if not redirect.is_file():
                raise ValueError('missing music routing header: ' + str(redirect))
            result.extend(['-include', str(redirect)])
            if compiling:
                print('[voice-music] routing Agent tool_media.c through contest HTTP/Helix backend', file=sys.stderr)
        if has_board_trace(arguments):
            result.append('-DROBOT_VOICE_FILTER_PLATFORM_TRACE=1')
        result.extend(['-include', str(Path(hook).resolve())])
    return result + arguments


def main():
    compiler = os.environ.get('ROBOT_VOICE_REAL_CC')
    hook = os.environ.get('ROBOT_VOICE_HOOK_HEADER')
    if not compiler or not hook:
        raise SystemExit('voice compiler: ROBOT_VOICE_REAL_CC and '
                         'ROBOT_VOICE_HOOK_HEADER must be set by the board build')
    # Every argument belongs to the real compiler. In particular there is no
    # wrapper-specific -- in CC: NuttX mkdeps owns those separator tokens.
    try:
        args = command(compiler, hook, sys.argv[1:])
    except (OSError, ValueError) as exc:
        raise SystemExit('voice compiler: ' + str(exc))
    os.execvp(args[0], args)


if __name__ == '__main__':
    main()
