# board_stress_test

Standalone single-board V1724 readout test.

- No Mongo dependency
- No Redax runtime dependency
- Only `std` + `CAENVMElib`
- No config loading
- Fixed baseline DAC only (no baseline fitting)

## Build

```bash
cd board_test
make
```

## Usage

```bash
./board_stress_test \
  --link 3 \
  --crate 0 \
  --base 0xFFFF0000 \
  --board-id 1390 \
  --do-sn-check \
  --start-mode software \
  --channel-mask 0xFF \
  --fixed-dac 7000 \
  --threshold 1 \
  --reg 0xEF1C=0xFF \
  --reg 0x8000=0x3310 \
  --reg 0x8080=0x510000 \
  --reg 0x8034=0x4 \
  --reg 0x8038=0x19 \
  --reg 0x8020=0x32 \
  --reg 0x8078=0x19 \
  --duration-s 10 \
  --status-period 500
```

## Options

Required:

- `--link <int>`
- `--crate <int>`
- `--base <hex|dec>`

Board setup:

- `--board-id <int>`: logging only.
- `--channel-mask <hex|dec>`: writes `0x8120`.
- `--fixed-dac <hex|dec>`: writes channel DACs (`0x1098 + 0x100*ch`).
- `--threshold <hex|dec>`: same threshold on all channels.
- `--thresholds a,b,c,d,e,f,g,h`: per-channel thresholds.
- `--trigger-mask <hex|dec>`: writes `0x810C`.
- `--post-trigger <hex|dec>`: writes `0x8114`.
- `--reg REG=VAL`: extra register write(s).
- `--no-reset`: skip startup reset.
- `--do-sn-check`: read and print serial from `0xF084/0xF080`.

Run control:

- `--start-mode software|sin`
- `--mode10-explicit-start-stop`
- `--stop-only`: open board, issue `0x8100=0x100`, wait for run bit clear, exit.
- `--duration-s <float>`
- `--max-packets <int>`
- `--sleep-us <int>`
- `--buffer-bytes <int>`
- `--status-period <int>`
- `--continue-on-read-error`
- `--save-packets`
- `--save-packets-prefix <prefix>`
- `--dump-invalid-prefix <prefix>`

Stop/recovery examples:

```bash
# Stop only
./board_stress_test --link 3 --crate 0 --base 0xFFFF0000 --stop-only

# Stop plus startup reset (default includes reset unless --no-reset is passed)
./board_stress_test --link 3 --crate 0 --base 0xFFFF0000 --stop-only --do-sn-check
```

## Summary fields

- `packets_total`: completed read cycles (`cvSuccess`/`cvBusError`).
- `packets_with_data`: non-empty packets.
- `parsed_events`: count of valid `0xA` event headers.
- `invalid_markers`: malformed event headers.
- `bus_error_terminations`: read cycles ended by `cvBusError`.
- `read_errors`: non-bus read failures and overflow guards.
- `event_full_seen`: number of polls with status `EVENT_FULL`.
- `channel_rate_*`: waveform payload rates per channel.


## Quick helper

I ran 
```bash
mkdir -p out
for thr in $(seq 21 31); do
  ./board_stress_test \
    --link 3 \
    --crate 0 \
    --base 0xFFFF0000 \
    --board-id 1390 \
    --do-sn-check \
    --start-mode software \
    --channel-mask 0xFF \
    --fixed-dac 7000 \
    --reg 0xEF1C=0xFF \
    --reg 0x8000=0x3310 \
    --reg 0x8080=0x510000 \
    --reg 0x8034=0x4 \
    --reg 0x8038=0x19 \
    --reg 0x8020=0x32 \
    --reg 0x8078=0x19 \
    --duration-s 120 \
    --status-period 500 \
    --threshold "$thr" \
    >> "out/thresh_${thr}"
done
```
To make a quick table 
```bash
{ printf "thr\tpkts\tMiB_s\tavg_ch_kiBps\tmax_ch_kiBps\tinvalid\tinvalid_pct\tevent_full_pct\tread_err\n";
  for f in out/thresh_*; do
    thr="${f##*thresh_}";
    awk -v thr="$thr" '
      /elapsed_s:/ {elapsed=$2}
      /loops:/ {loops=$2}
      /packets_total:/ {pkts=$2}
      /bytes_total:/ {bytes=$2}
      /invalid_markers:/ {inv=$2}
      /event_full_seen:/ {full=$2}
      /read_errors:/ {re=$2}
      /channel_rate_avg_runtime_per_channel_kiBps:/ {avg=$2}
      /channel_rate_max_runtime_per_channel_kiBps:/ {mx=$2}
      END{
        mibs=(elapsed>0?bytes/1048576/elapsed:0);
        invp=(pkts>0?100*inv/pkts:0);
        fullp=(loops>0?100*full/loops:0);
        printf "%s\t%d\t%.1f\t%.1f\t%.1f\t%d\t%.2f\t%.2f\t%d\n",
               thr, pkts, mibs, avg, mx, inv, invp, fullp, re;
      }' "$f";
  done; } | sort -n | column -t -s $'\t'
```

Header rows
```
thr - threshhold
pkts - number of packets
MiB_s - MiB/s over the running 
avg_ch_kiBps - average kiBps per channel active
max_ch_kiBps - max instantaneous in a channel
invalid - number of invalid packages (causes Redax segfault)
invalid_pct - Percentage invalid 
event_full_pct - Percentage of events with EVENT_Full bit
read_err - actual read errors reported
```
