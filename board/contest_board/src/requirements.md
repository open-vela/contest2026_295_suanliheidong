# INMP441 Physical I2S RX Format Requirements

## 1. Current State

`contest_i2s.c` owns I2S0 RX for the contest board.  The public I2S API
exposes logical 16-bit, mono PCM at 16 kHz to `voice_echo`.

The contest-local RX lower-half uses INMP441 physical framing internally.  A
logical APB request is received into an independent transaction-owned raw DMA
buffer, converted in HPWORK, and reported to the application as logical PCM.
The application contract is independent of physical DMA byte counts.

The current I2S1 TX implementation, 16 kHz to 24 kHz playback conversion,
Wi-Fi, OLED, PWM, and robot control are outside this change.

## 2. New Feature Requirements

The I2S0 hardware interface shall use the INMP441 physical framing while the
public API remains logical signed 16-bit mono PCM:

| Property | Required value |
| --- | --- |
| Sample rate | 16000 Hz |
| I2S standard | Philips/I2S |
| Hardware slots | 2 per frame |
| Hardware slot width | 32 BCLK cycles |
| Microphone valid width | 24 signed bits |
| Hardware BCLK | 1024000 Hz |
| Selected RX slot | LEFT (`MIC_L/R` is LOW) |
| Public PCM | signed `int16_t`, mono |

For a logical 2048-byte APB request, the lower-half shall receive 1024
logical samples into a separate 4096-byte raw DMA buffer.  The HPWORK worker
shall convert the raw words into exactly 2048 bytes of signed 16-bit PCM,
set `apb->nbytes` to 2048 and `apb->nsamples` to 1024, and invoke the normal
I2S callback once.

The raw DMA buffer and the large `voice_echo` record buffer have different
ownership.  The raw buffer is owned by the contest I2S transaction and must
remain DMA-safe until the completion worker has run.  The large record buffer
remains application storage and must not be used for DMA.

## 3. L/R Slot Selection

The approved schematic establishes the microphone channel selection:

```text
MIC1 L/R -> MIC_L/R -> R10 10K -> GND
```

`MIC_L/R` is therefore hardware-tied LOW.  The contest-local I2S0 driver
shall select the standard **LEFT** slot only, using active-channel mask
`0x01`.  It shall not probe, switch to, or otherwise attempt the RIGHT slot
at runtime.

## 4. Hardware Configuration and Data Flow

The RX lower-half shall configure I2S0 as master RX with GPIO16 BCLK output,
GPIO17 WS output, and GPIO18 DIN input.  It shall set Philips MSB shift,
32-bit physical transfer/channel/half-sample width, 32-BCLK WS width, two
total slots, and only the selected slot active.  INMP441's 24 valid bits are
interpreted during raw-to-PCM conversion, not as the physical EOF unit.

```text
INMP441 24-bit sample in 32-bit physical slot
  -> contest I2S0 raw DMA buffer
  -> RX EOF interrupt
  -> HPWORK conversion and APB completion
  -> logical int16 PCM callback
  -> voice_echo record buffer
```

The ISR shall only acknowledge DMA completion, mark the transaction done,
and queue HPWORK.  It shall not convert samples or invoke the upper callback.
The worker shall release raw DMA storage only after callback completion.

## 5. RX EOF and DMA Constraints

`CONFIG_I2S_DMADESC_NUM=2` must queue the complete 4096-byte raw request via
`esp32s3_dma_setup()`.  Partial descriptor setup is an error.

The ESP32-S3 register header documents RX EOF in units of
`(RX_BITS_MOD + 1) * (RXEOF_NUM + 1)` bits.  With `RX_BITS_MOD = 23` and
`RX_24_FILL_EN`, each valid 24-bit word is placed in a 32-bit physical slot;
the 4096-byte raw transaction still uses `RXEOF_NUM = 1023`.  Raw diagnostics
remain enabled for the first transaction to validate sample alignment on
hardware.

## 6. Gapless RX Continuity

The lower-half maintains independent active, pending, and done RX
transactions.  On a normal EOF it starts the pending transaction immediately,
keeps `I2S_RX_START` asserted, and defers conversion/callback work to HPWORK.
Normal EOF does not stop or reset the RX peripheral.  Each transaction owns
its raw DMA storage until conversion and callback completion.

`voice_echo` keeps two application RX slots in flight.  It reserves bytes
before submission, so a five-second capture is exactly 78 x 2048 bytes plus a
final 256-byte request.  Bounded waits and cancellation stop the stream only
on error or final drain.  The driver reports `rx_eof`, `rx_chained`, and
`rx_underrun`; the application reports per-chunk and cross-boundary PCM
continuity metrics.

## 7. Conversion and Diagnostics

The first completed transaction shall log sixteen raw 32-bit words plus
raw non-zero count, low-eight-bit non-zero count, minimum, and maximum.  The
initial conversion candidate is the upper signed 16 bits of a left-justified
24-bit word (`raw >> 16`), but the final shift must be supported by those raw
diagnostics.

Per recording, `voice_echo` shall report min, max, mean/DC, RMS, peak, and
clip count for converted logical PCM.  Normal speech must not be accepted as
success merely because it reaches full-scale values.

## 8. Constraints

- [x] Modify only `contest2026_295_suanliheidong/**`.
- [x] Keep the public `I2S_RXCHANNELS(1)`, `I2S_RXSAMPLERATE(16000)`, and
  `I2S_RXDATAWIDTH(16)` application contract.
- [x] Preserve I2S1 TX pins/rate/transport and the existing 3:2 resampler.
- [x] Preserve Wi-Fi, SMP, OLED, PWM, robotctl, boot, flash, and PSRAM.
- [x] Reject partial DMA descriptor setup and preserve normal callback/APB
  ownership on all completion paths.
- [x] Do not update upstream NuttX, HAL, vendor, or packages.
- [x] Use the schematic-confirmed INMP441 LEFT slot (`MIC_L/R` LOW).
- [ ] Hardware-confirm raw-word alignment.
- [ ] Verify clean PCM with silence and speech on the target hardware.

## 9. Performance and References

The target stream is 16000 logical samples/second and 32000 logical
bytes/second.  The physical I2S0 bus is 1.024 MHz BCLK.  The lower-half uses
contest-local GDMA input IRQ and HPWORK callbacks with active/pending/done
transactions; the application maintains two receive slots.

Read-only references:

- `nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c`
- `nuttx/arch/xtensa/src/esp32s3/esp32s3_dma.c`
- `nuttx/arch/xtensa/src/esp32s3/hardware/esp32s3_i2s.h`
- `nuttx/arch/xtensa/src/esp32s3/esp-hal-3rdparty/components/hal/esp32s3/include/hal/i2s_ll.h`
