# INMP441 I2S0 RX Physical-Format Design

## Architecture

The existing contest-local I2S0 lower-half remains the I2S owner.  Its public
operations retain a 16 kHz, mono, signed 16-bit PCM contract.  No I2S1 TX
code, board bringup path, or `voice_echo` resampling path changes.

The receive transaction gains an RX-only physical buffer:

```text
logical APB request (2048 B, 1024 int16 samples)
  -> xfer-owned raw DMA buffer (4096 B, 1024 uint32 words)
  -> I2S0 GDMA IN EOF
  -> HPWORK raw32-to-int16 conversion
  -> APB nbytes=2048, nsamples=1024
  -> existing I2S callback
```

`contest_i2s_xfer_s` remains shared with I2S1 only for its existing fields.
RX-only raw storage is initialized and released only by I2S0 RX paths so TX
ownership and DMA sequencing remain unchanged.

## Hardware Configuration

I2S0 stays master RX with GPIO16 BCLK output, GPIO17 WS output, and GPIO18
DIN input.  The RX register configuration is:

| Register field | Value | Reason |
| --- | --- | --- |
| `I2S_RX_MSB_SHIFT` | set | Philips/I2S framing |
| `I2S_RX_BITS_MOD` | 23 | 24 valid-bit RX EOF unit with 24-fill |
| `I2S_RX_TDM_CHAN_BITS` | 31 | 32-bit physical slot |
| `I2S_RX_HALF_SAMPLE_BITS` | 31 | 32-bit half sample |
| `I2S_RX_TDM_WS_WIDTH` | 31 | WS phase spans one 32-bit slot |
| `I2S_RX_TDM_TOT_CHAN_NUM` | 1 | two slots per frame |
| RX channel mask | `0x01` | schematic-confirmed LEFT slot only |

The clock calculation uses physical framing: `16000 * 2 * 32 = 1024000` Hz
BCLK.  It does not use the logical 16-bit API width.

## Transaction State and Lifetime

1. `contest_i2s_receive()` validates an even logical PCM request and takes an
   APB reference.
2. It derives `logical_samples = logical_bytes / sizeof(int16_t)` and
   `raw_bytes = logical_samples * sizeof(uint32_t)`.
3. It allocates/owns a raw DMA buffer, configures up to two descriptors, and
   rejects `queued != raw_bytes`.
4. It publishes the active transaction before enabling GDMA/I2S0 RX.
5. The ISR acknowledges EOF/error, moves `active` to the done queue, starts
   `pending` immediately when present, and schedules HPWORK.  Normal EOF does
   not stop or reset the RX peripheral.
6. The worker converts raw words to `apb->samp`, sets the logical `nbytes`
   and `nsamples`, calls the I2S callback, then frees raw storage and its APB
   reference.
7. `AUDIOIOC_STOP` follows the same done-worker cleanup route.

## Conversion and Diagnostics

The first worker completion logs 16 raw words and raw statistics.  The
initial configured conversion uses an explicit contest-local shift constant
for the upper 16 bits of a left-justified 24-bit signed word.  The diagnostic
output is the hardware gate for validating that representation; no RIGHT-slot
fallback exists.

The worker reports logical completion, not DMA descriptor DATALEN.  A normal
2048-byte request therefore always reports `apb->nbytes = 2048` and
`apb->nsamples = 1024`; it cannot create an application-side halving tail.

`voice_echo` calculates min, max, mean/DC, RMS, peak, and clip count from
the converted logical samples once per recording.

## RX EOF Decision

For one 4096-byte raw request, `I2S_RX_BITS_MOD = 23`, `RX_24_FILL_EN`, and
`RXEOF_NUM = 1023` program 1024 valid 24-bit words filled into physical
32-bit slots.  Preserve
first-transaction raw diagnostics and the existing bounded application timeout
so a target-specific data-alignment discrepancy returns to NSH rather than
hanging.

## Gapless Recording

The driver has two independent RX transactions, each with its own raw DMA
buffer and descriptors.  At EOF, a pending transaction becomes active before
the done transaction is converted in HPWORK.  The application submits two
logical slots, accounts `bytes_reserved`, `bytes_submitted`, and
`bytes_completed`, and never submits beyond 160000 bytes.  The final sequence
is 78 requests of 2048 bytes followed by one 256-byte request.  Normal
completion keeps RX running; `AUDIOIOC_STOP` is used only after all callbacks
are collected or on cancellation/error.

## File Plan

| File | Change | Responsibility |
| --- | --- | --- |
| `contest2026_295_suanliheidong/board/contest_board/src/contest_i2s.c` | Modify | I2S0 physical configuration, raw DMA transaction, conversion, diagnostics |
| `contest2026_295_suanliheidong/board/contest_board/include/board.h` | Modify | Named LEFT-slot and physical-format constants |
| `contest2026_295_suanliheidong/app/voice_echo/voice_echo_main.c` | Modify | Two-slot RX scheduling, bounded cancellation, PCM and boundary statistics |
| `contest2026_295_suanliheidong/board/contest_board/README.md` | Modify | Schematic-confirmed LEFT slot and physical/logical format note |

## Traceability Matrix

| Req | Requirement | Target | Function or structure | Board integration | Upstream contract | Switch point |
| --- | --- | --- | --- | --- | --- | --- |
| R1 | 32-bit physical two-slot RX | `contest_i2s.c` | RX format/rate/configure | Existing I2S0 owner | `i2s_dev_s` | internal RX-only |
| R2 | LEFT slot for L/R LOW | `contest_i2s.c`, `board.h` | format constants/channel mask | GPIO16/17/18 unchanged | ESP32-S3 registers | internal RX-only |
| R3 | 4096 raw B to 2048 logical B | `contest_i2s.c` | RX xfer/receive/worker | board accessor unchanged | APB callback | internal RX-only |
| R4 | Raw diagnostics before trust | `contest_i2s.c` | RX worker | none | syslog | internal RX-only |
| R5 | Recording PCM statistics | `voice_echo_main.c` | record statistics | builtin unchanged | public I2S callback | app-only |
| R6 | Keep TX and other features | unchanged files | I2S1 TX and board branches | existing | existing | no switch |
