# Changes (relative to `new_libs`)

## Summary

Goal: make redax robust against the V1724/V1730 “faulty package” failure mode by
detecting incomplete channel aggregates **inside an otherwise valid event** and inserting an
artificial deadtime pulse on the dedicated AQMon channel **799**. The deadtime start time comes
from the **channel trigger timestamp**, not the event time tag which for V* corresponds to the package
creation time and may be up to 10 seconds later.

Note this logic is not complete yet, my main worry is currently if a header declares size n but 
is size m with n < m 

So we select | 0xA .... 0xA .. | entering the next header, per channel this will then fail on 0xA. 
And should become unrecoverable, but we do not want that, as we would want isolation of the first
channel header time (at least) and mark it. However, this is likely within a microsecond, so,
good enough for now. We also need to check how exactly headers are wrong typically in terms of 
advertisement versus size.  

## Failure-handling paths (high level)

- **Normal data:** event header is found → each channel parses cleanly → write fragments exactly
  as before.
- **Header gap in datapacket:** words that don’t look like an event header are skipped → one
  warning logline is emitted per contiguous gap → keep scanning for the next header.
- **Recoverable incomplete channel aggregate:** channel header is present, but its declared channel
  size runs past the remaining event buffer → compute deadtime from that channel’s trigger
  timestamp + declared length (rounded to 110-sample multiples) → write deadtime pulse on
  channel 799 → stop processing the rest of the channels in this event (channels already written
  earlier in the event remain written).
- **Unrecoverable header corruption:** can’t even read a channel header, or the declared channel
  size is nonsensical → emit an ERROR logline. (DAQ error-state propagation plumbing exists but is
  currently disabled at the call sites until we learn the expected rate.)

## File-by-file changes

### `StraxFormatter.hh`

- Extended `GenerateArtificialDeadtime` signature to accept:
  - `start_time_ns` and `samples_in_pulse` (so duration is derived from the faulty channel’s
    declared length rather than a fixed dummy pulse),
  - metadata (`event_time_tag`, `event_channel_mask`, `faulty_channel_mask`) to embed in the
    deadtime waveform for later debugging/triage.

### `StraxFormatter.cc`

- **`GenerateArtificialDeadtime`**
  - Now writes a normal strax fragment stream (fragmented to the configured 110-sample payload),
    using `start_time_ns` directly.
  - Embeds int16 metadata in the first samples of the first fragment:
    - sample[0] = board id
    - sample[1:2] = event time tag (low/high 16 bits)
    - sample[3] = event channel mask
    - sample[4] = faulty channel mask
  - Reason: deadtime must be time-aligned using channel trigger time, and the payload must carry
    enough information to understand which board/channels caused the insertion.

- **`ProcessDatapacket`**
  - Reworked to scan by index with a lightweight “gap” state:
    - emits **at most one warning logline per contiguous non-header region** (“Missed header
      data… skipped N words…”),
    - removes the old behavior that wrote a binary `*_missed` dump file.
  - Processes a truncated final event safely by bounding the view size to the remaining words.
  - Reason: avoid crashing/spamming when the datapacket contains injected garbage or ends in the
    middle of an event; keep overhead minimal.

- **`ProcessEvent`**
  - Adds a short-buffer guard for the 4-word event header (unrecoverable path).
  - For boards with per-channel headers:
    - uses board-specific header word count and channel-size mask,
    - for each channel in the event mask, validates channel header plausibility before attempting
      to parse/slice waveform data,
    - **recoverable case:** if declared channel size exceeds remaining buffer, use that channel’s
      trigger timestamp (via `UnpackChannelHeader`) + declared size to generate deadtime, then stop
      processing remaining channels in this event.
    - **unrecoverable cases:** missing header words or invalid declared size → ERROR logline (DAQ
      error signaling currently commented out).
  - Special-cases digitizers without per-channel headers (MV): retains existing behavior.
  - Reason: the crash mode is “declared channel aggregate does not fit in buffer”; we need to
    detect this precisely and extract timing from the channel header when still available.

### `V1724.hh` / `V1730.hh` / `V1724_MV.hh`

- Added small virtual helpers to describe the board family’s channel header layout:
  - `HasPerChannelHeaders()` (MV overrides to `false`),
  - `ChannelHeaderWords()` (V1730 overrides to 3, V1724 default 2),
  - `ChannelSizeMask()` (V1730 overrides to 24-bit mask, V1724 default 23-bit mask).
- Added a soft-error bit and `SignalSoftError()` plumbing (flag `0x4`) for “formatter unrecoverable
  header” errors.
- Reason: StraxFormatter must bounds-check generically across board models, and we want a clean
  way to escalate unrecoverable corruption to the DAQ controller later (without string parsing).

### `V1724.cc` / `V1730.cc` / `V1724_MV.cc`

- Set `fArtificialDeadtimeChannel` to **799** for all digitizer variants (724, MV, 730).
  - Reason: downstream expects a single dedicated artificial deadtime channel at 799.
- `V1730::UnpackChannelHeader` now uses the correct 24-bit `CHANNEL SIZE` field.
  - Reason: UM5954 specifies `CHANNEL SIZE` is bits[23:0] for 725/730; masking 23 bits can
    under-estimate declared sizes and break the plausibility checks / duration inference.
- `V1724::UnpackChannelHeader` now uses `ChannelSizeMask()` to keep logic centralized.
- `V1724::CheckErrors` ORs in the accumulated soft-error flags and clears them atomically.

### `DAQController.cc`

- Recognizes the formatter soft-error bit (`0x4`) and logs it when the DAQ enters error handling.
- Reason: make formatter-originated unrecoverable conditions visible to the controller/operator.
  (Note: the formatter currently does not assert this bit; call sites are commented out until we
  confirm the expected frequency.)

