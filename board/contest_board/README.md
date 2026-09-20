# ESP32-S3-WROOM-1-N16R8 Minimal Board

## 1. Current Stage Status

- N16R8: PASS
- Flash 16MB: PASS
- PSRAM 8MB Octal: CONFIGURED
- UART0 + NSH: PASS
- Wi-Fi: restored from hardware-validated Stage 3A configuration; regression pending
- Bluetooth: DISABLED
- OLED: SSD1306-compatible, 128x64, I2C0 address 0x3C (hardware test pending)
- I2S: voice loopback application built; hardware test pending
- PWM/robotctl: restored; servo regression pending
- `-j8` build: PASS
- `nuttx.bin`: PASS
- Hardware boot: PASS
- `nsh>`: PASS

PSRAM runtime stress test: NOT YET VERIFIED.

Stage 2 retains the minimal NSH boot path and adds board-resource adaptation.
Stage 3 adds an on-demand OLED status display and raw PCM record-then-playback
application. It does not start audio, PWM, Wi-Fi, or any actuator at boot.

## 2. Hardware Parameters

- Module: ESP32-S3-WROOM-1-N16R8
- Flash: 16 MB
- PSRAM: 8 MB Octal/OPI
- UART0 TX: GPIO43
- UART0 RX: GPIO44
- UART baud: 115200

## 3. Peripheral Bring-up

The board registration and NSH device nodes were verified on hardware. The
actual OLED rendering, servo motion, microphone capture, and speaker playback
remain functional tests for later stages.

| Resource | GPIO / controller | Board adaptation |
| --- | --- | --- |
| LED | GPIO4 | GPIO output; boot level LOW. LED active polarity is unknown. |
| GPIO14 | NC / unused | Legacy reference push-to-talk pin; current board does not connect it. |
| Boot Button | GPIO0 | Ordinary GPIO button input after normal boot. Do not hold it during reset: it may enter ROM download mode. |
| OLED I2C | SSD1306-compatible 128x64, address 0x3C; SDA GPIO12, SCL GPIO13 | Standard NuttX SSD1306 I2C driver; 400 kHz. Hardware rendering test pending. |
| Servo PWM | GPIO9, GPIO10, GPIO21, GPIO47, GPIO48 | ESP32-S3 LEDC timer 0 with five channels, registered as `/dev/pwm0`. 50 Hz is the intended runtime frequency; the resource remains disabled until a client starts PWM. |
| MIC I2S RX | I2S0: BCLK GPIO16, WS GPIO17, DIN GPIO18 | Master RX uses Philips I2S, two 32-bit slots, 24 valid bits, and the schematic-confirmed LEFT slot (`MIC1 L/R` is pulled LOW by R10). The app receives 16 kHz mono signed 16-bit PCM after contest-local conversion. Raw-word alignment remains hardware-test pending. |
| Speaker I2S TX | I2S1: WS GPIO38, BCLK GPIO39, DOUT GPIO40 | Master TX lower-half initialized. |

I2S0 is permanently assigned to microphone RX and I2S1 to speaker TX. The
voice profile follows `eda-robot-pro/config.h`: MIC 16 kHz and speaker 24 kHz.
The application selects mono signed 16-bit PCM.  I2S0 internally receives
the INMP441 physical two-slot, 32-bit-slot, 24-valid-bit Philips stream and
converts its fixed LEFT slot to that logical format.  Raw-word alignment is
still subject to the first hardware diagnostic capture.  The application uses
the public asynchronous I2S lower-half API directly; no I2S character device
is registered.

Future non-actuating NSH registration checks:

```text
ls /dev
i2c -h
```

Do not issue PWM start commands until the servo wiring and safe pulse policy
are hardware-validated.

## 3.1 Integrated Voice Echo

The voice application is an on-demand NSH builtin with no button dependency.
`voice_echo` records exactly five seconds from I2S0, then plays the captured
mono PCM through I2S1 and returns to NSH. The bounded record buffer is 160000
bytes (`16000 * 5 * sizeof(int16_t)`). Playback uses the existing chunked
16 kHz to 24 kHz 3:2 conversion without a second complete playback buffer.
OLED updates are best-effort and never make audio fail.

```text
nsh> voice_echo
nsh> voice_echo --once
```

Both forms have identical fixed five-second semantics. GPIO14 is NC/unused;
the legacy reference push-to-talk behavior is not used. Audio gain is 1.0
with no codec or filesystem storage.

## 3.2 Features intentionally out of scope

Bluetooth, ASR, TTS, AI, network audio, and audio codecs remain disabled.
Wi-Fi tools (`wapi`, `renew`, `ping`) and the complete `robotctl` command set
are enabled in this integrated build. `voice_echo` never starts PWM or changes
servo state; run `robotctl off` manually before the first audio test.

## 4. Environment Preparation

Run from the openvela workspace root:

```bash
cd ~/vela/openvela

if [ ! -d myenv ]; then
  python3 -m venv myenv
fi

source myenv/bin/activate
python --version
python -m esptool version
```

The build wrapper also activates `myenv`, but activating it explicitly keeps
all manual image inspection and flashing commands on esptool 5.3.1.

## 5. Correct Build Method

Do not use the ordinary `build.sh` command directly for this board. Use:

```bash
cd ~/vela/openvela
source myenv/bin/activate

contest2026_295_suanliheidong/board/contest_board/scripts/build_with_hal_backport.sh -j8
```

The wrapper verifies the HAL base, temporarily applies the HAL lock backport
and the NuttX Xtensa BREAK/BREAK.N handler backport, builds this board, then
reverses both patches on success, failure, SIGINT, or SIGTERM. Neither
temporary patch remains applied after it exits.

To retain a build log:

```bash
set -o pipefail
contest2026_295_suanliheidong/board/contest_board/scripts/build_with_hal_backport.sh \
  -j8 2>&1 | tee ~/contest-board-build.log
```

## 6. Build Artifacts

- `nuttx/nuttx`: ELF for debugging.
- `nuttx/nuttx.bin`: image to flash.
- `nuttx/nuttx.hex`: Intel HEX image.

## 7. Image Verification

Before every flash, verify the generated image:

```bash
python -m esptool \
  --chip esp32s3 \
  image-info \
  nuttx/nuttx.bin
```

The result must show ESP32-S3, Flash size 16MB, DIO, 40m, and a valid
checksum. ROM-visible load segments must use valid DRAM/IRAM addresses such as
`0x3fc...` and `0x403...`; they must not be `0x00000020`, `0x00010000`, or
`0x00020000`.

Do not add `--use_segments` or `--use-segments` to `elf2image`.

## 8. Flashing

This board uses a NuttX Simple Boot single image. Do not add ESP-IDF
`bootloader.bin` or `partition-table.bin`.

```bash
cd ~/vela/openvela
source myenv/bin/activate

python -m esptool \
  --chip esp32s3 \
  --port /dev/ttyUSB0 \
  --baud 460800 \
  write-flash \
  --flash-size 16MB \
  --flash-mode dio \
  --flash-freq 40m \
  0x000000 \
  nuttx/nuttx.bin
```

## 9. USB-TTL Wiring

- USB-TTL RXD -> GPIO43
- USB-TTL TXD -> GPIO44
- USB-TTL GND -> Board GND
- USB-TTL 3V3/5V -> do not connect
- Board -> independent stable 5V supply

## 10. Serial Boot

```bash
picocom -b 115200 --flow n /dev/ttyUSB0
```

After power cycling the board, expect output similar to:

```text
ESP-ROM...
*** Booting NuttX ***
...
NuttShell (NSH)
nsh>
```

## 11. Minimal NSH Verification

Run:

```text
help
uname -a
uptime
dmesg
ls /
ls /dev
echo hello_openvela
```

`free` and `ps` are absent in this minimal configuration by design.

## 12. Not Implemented Yet

- Bluetooth disabled
- Wi-Fi enabled; hardware regression pending
- Camera disabled
- OLED visible output (hardware test pending)
- Microphone capture (hardware test pending)
- Speaker playback (hardware test pending)
- ASR, TTS, codecs, filesystem recording, and AI

No codec, audio player, or filesystem audio recorder application is enabled.

## 13. Important Special Changes

### 13.1 Important Compatibility Patch / Upstream Backport

The NuttX integration pins `esp-hal-3rdparty` to base revision
`9fc713a95b1ff150dd0b0647e465d3c624056bb1`. That revision defines
`LOCK_INITIALIZER_UNLOCKED` as `0` in these files:

- `components/esp_hw_support/clk_ctrl_os.c`
- `components/esp_hw_support/modem_clock.c`

Current NuttX `spinlock_t` is incompatible with that integer initializer. The
board backports upstream commit `454d82c70ed` (`Change lock constant defines
with SP macro`) through:

- `patches/0001-esp-hal-nuttx-lock-initializer.patch`
- `scripts/esp_hal_lock_backport.sh`
- `scripts/build_with_hal_backport.sh`

The patch is applied only for the build and automatically reversed afterward.
Do not directly modify HAL tracked source. Do not upgrade HAL blindly: newer
HAL revisions change directory layout and do not match this NuttX `hal.mk`.
After the wrapper exits, HAL must return to the pinned base revision with an
empty tracked diff.

### 13.2 Important Image Generation Fix / Do Not Reintroduce `--use_segments`

The former board `scripts/esptool.py` injected `--use_segments` into
`elf2image`. That made esptool interpret ELF Program Header PhysAddr values
such as `0x20`, `0x10000`, and `0x20000` as ROM load addresses. The resulting
image booted with `load:0x00000020`, then failed with `StoreProhibited` and
`EXCVADDR:0x00000020`.

The wrapper now uses the active virtualenv's `python -m esptool` and leaves
esptool in its default section mode while retaining `--ram-only-header` from
the NuttX build command. Reintroducing `--use_segments` or `--use-segments`
will create a non-bootable `nuttx.bin`.

### 13.3 `board_bringup` Return Value

`src/board_bringup.c` returns `0` on success. Do not change it back to
`return OK;` unless the correct header provides `OK` and its necessity has
been verified.

## 14. Repository Modification Boundary

Allowed long-term board maintenance:

```text
contest2026_295_suanliheidong/board/contest_board/**
```

Do not directly maintain changes in HAL, NuttX tracked source, vendor tracked
source, or packages tracked source.

## 15. Current Non-Blocking Messages

The current build may report:

- `No valid Rust crates found to build`
- `noreturn function does return`
- `-Wno-atomic-alignment` is not recognized by the compiler

These messages did not block the frozen minimal NSH build.

## 16. Future Work

The next stage should perform the hardware functional tests intentionally
excluded here: verify GPIO electrical polarity, probe I2C, check I2S capture
and playback with identified external devices, and define a servo safety
policy before issuing PWM start requests.
