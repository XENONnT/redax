# board_stress_test

Standalone script to test a singular config on this device.

See helpers/export_redax_mode.py

- No Mongo dependency
- No Redax runtime dependency
- Only `std` + `CAENVMElib`
- Fixed baseline DAC only (no baseline fitting)

## Build

```bash
cd board_stress_test
make
```

## What It Does

1. Connects with `CAENVME_Init2`
2. Optionally resets the board (`0xEF24`, `0xEF00`)
3. Applies fixed config:
   - channel enable mask (`0x8120`)
   - fixed DAC per enabled channel (`0x1098 + 0x100*ch`)
   - threshold per enabled channel (`0x1060 + 0x100*ch`)
   - optional trigger/post-trigger registers
   - optional extra raw register writes (`--reg REG=VAL`)
4. Starts acquisition (`software` or `sin`)
5. Polls acquisition status (`0x8104`) and reads BLT packets
6. Checks packet integrity with the same event-marker rule used in formatter:
   - marker nibble `0xA`
   - advertised event words must be `> 0` and `<= remaining_words`
7. Stops acquisition and prints summary

## Config Loading

You can load options from JSON files and mirror Redax include merging:

- Pass `--config <path/to/file.json>`
- for quick iter of baselines `--fixed-dac` overwrites the config value

This mirrors Redax behavior where included docs are merged first and root/override wins.

`--host` defaults to `<local_hostname>_reader_0`, this should never need adjusting assuming valid config files. 

## Config vs CLI Matrix

| Setting | Config key | CLI key |
| --- | --- | --- |
| Root config input | n/a | `--config` |
| Config override file | n/a | `--config-override-file` |
| Include merge | `includes` | n/a |
| Host selector used for board/detector matching | `boards[].host`, `detectors[<host>]` | `--host` |
| Board selection | `boards[]` (`type`, `skip`, `host`, `board`) | `--board-id` |
| Link | `boards[].link` | `--link` |
| Crate | `boards[].crate` | `--crate` |
| Base address | `boards[].vme_address` | `--base` |
| Channel mask | `channel_mask` | `--channel-mask` |
| Fixed DAC | `baseline_fixed_value` | `--fixed-dac` |
| Thresholds | `thresholds[<board_id>]` | `--threshold`, `--thresholds` |
| Trigger mask | `trigger_mask` | `--trigger-mask` |
| Post-trigger | `post_trigger` | `--post-trigger` |
| Extra register writes | `registers[]` (`board`, `reg`, `val`) | `--reg` |
| Start mode | `run_start` (`0=software`, nonzero=`sin`) | `--start-mode` |
| Sleep between polls | `us_between_reads` | `--sleep-us` |
| Serial-number check | `do_sn_check` | `--do-sn-check` |
| Reset at startup | n/a | `--no-reset` |
| Run duration | n/a | `--duration-s` |
| Max packets with data | n/a | `--max-packets` |
| Status print period | n/a | `--status-period` |
| Read buffer bytes | n/a | `--buffer-bytes` |
| Continue on read errors | n/a | `--continue-on-read-error` |
| Save all packets | n/a | `--save-packets` |
| Save packet prefix | n/a | `--save-packets-prefix` |
| Dump invalid packet prefix | n/a | `--dump-invalid-prefix` |

Baseline note: `baseline_dac_mode`, `baseline_reference_run`, and cached/fit baseline logic are not used here.

## Minimal Example

```bash
./board_stress_test \
  --link 0 \
  --crate 0 \
  --base 0x32100000 \
  --board-id 5 \
  --channel-mask 0xFF \
  --fixed-dac 7000 \
  --threshold 0xA \
  --start-mode software \
  --duration-s 60 \
  --sleep-us 10 \
  --status-period 200
```

Config-driven example:

```bash
./board_stress_test \
  --config ./example_configs/single_board_debug.json \
  --host reader8_reader_0 \
  --duration-s 120 \
  --dump-invalid-prefix ./invalid
```

## Reproducing Redax-Like Debug Settings

Some testing knobs

- `--channel-mask`: explicit channel enable/disable write (`0x8120`)
- `--fixed-dac`: fixed DAC (replaces all baseline modes)
- `--threshold` or `--thresholds`: trigger threshold setup
- `--reg REG=VAL` (repeat): copy register writes from your current options
- `--trigger-mask` / `--post-trigger`: convenience fields for common registers
- `--start-mode software|sin`
- `--buffer-bytes`, `--sleep-us`: readout behavior

## Useful Debug Flags

- `--do-sn-check`: prints SN from `0xF084/0xF080`
- `--dump-invalid-prefix <prefix>`: dumps malformed packets to `<prefix>_packet_N.bin`
- `--save-packets`: save all non-empty packets (default is OFF)
- `--save-packets-prefix <prefix>`: change packet dump prefix (default `packet`)
- `--continue-on-read-error`: keep running after CAEN read errors

## Final Summary Interpretation

The tool prints:

```text
Summary
  elapsed_s: ...
  loops: ...
  packets_total: ...
  packets_with_data: ...
  bytes_total: ... (... MiB)
  parsed_events: ...
  invalid_markers: ...
  bus_error_terminations: ...
  read_errors: ...
  event_full_seen: ...
  saved_packets: ...
  channel_rate_avg_runtime_per_channel_kiBps: ...
  channel_rate_max_runtime_per_channel_kiBps: ...
  channel_rate_avg_1s_over_time_channels_kiB: ...
  channel_rate_max_1s_any_channel_kiB: ...
```

Field meaning:

- `elapsed_s`: actual runtime from acquisition start to stop.
- `loops`: status-poll/readout loop iterations.
- `packets_total`: number of read cycles that completed (`cvSuccess` or `cvBusError` termination).
- `packets_with_data`: packets where `total_bytes > 0`.
- `bytes_total`: total bytes received in non-empty packets.
- `parsed_events`: count of event headers found with marker nibble `0xA` and valid advertised length.
- `invalid_markers`: packets where an event header was malformed (`ev_words == 0` or `ev_words > remaining_words`).
- `bus_error_terminations`: read cycles terminated by `cvBusError` (often expected end-of-block behavior in this flow).
- `read_errors`: non-bus read failures and overflow guards.
- `event_full_seen`: number of status polls where acquisition status had `EVENT_FULL` bit set.
- `saved_packets`: number of packet files written by `--save-packets*`.
- `channel_rate_avg_runtime_per_channel_kiBps`: average waveform payload rate per enabled channel over full runtime.
- `channel_rate_max_runtime_per_channel_kiBps`: highest runtime-average waveform payload rate among channels.
- `channel_rate_avg_1s_over_time_channels_kiB`: average per-channel payload in each full 1 s window (same 1 s cadence concept as redax status updates).
- `channel_rate_max_1s_any_channel_kiB`: largest per-channel payload observed in any full 1 s window.
- “enabled channel” count uses `channel_mask` only when this tool applied it; otherwise defaults to 8 channels.

How to read health quickly:

- Good/expected: `read_errors=0`, `invalid_markers=0`, `packets_with_data>0`.
- Data-shape issue: rising `invalid_markers` while `read_errors` stays low.
- Transport/readout issue: nonzero `read_errors`, especially with low `packets_with_data`.
- Sustained pressure: high `event_full_seen` relative to runtime can indicate board backpressure.

### To share rate based crash data with caen 

Clone a minimal config from the mongodb with `--write-merged --override-host` and run (this runs for 30 seconds)
```
./board_stress_test --config ./config_from_mongo/<MODE_NAME>.json
```
This produces some information on failure rate under the configuration. Such that they can attempt to replicate what we get. 
