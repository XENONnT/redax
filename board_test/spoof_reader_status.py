#!/usr/bin/env python3
"""
Spoof redax reader heartbeats for dispatcher integration testing.

This writes periodic docs into db.status for one DAQ host only.
It does NOT acknowledge control commands and is not meant to emulate
an operating readout process.

python3 board_test/spoof_reader_status.py \
  --uri "mongodb://daq:${MONGO_PASSWORD_DAQ}@192.168.131.1:27020/admin" \
  --db daq \
  --host "$(hostname)_reader_0" \
  --detector tpc \
  --status idle \
  --mode board_test_cheat \
  --comment 'CHEAT: board_test standalone run'
"""

import argparse
import datetime as dt
import os
import socket
import time
from typing import Dict

from pymongo import MongoClient


STATUS = {
    "idle": 0,
    "error": 4,
    "unknown": 6,
}


def parse_channels(raw: str) -> Dict[str, int]:
    if not raw:
        return {}
    out: Dict[str, int] = {}
    for token in raw.split(","):
        token = token.strip()
        if not token:
            continue
        if "=" not in token:
            raise ValueError(f"Invalid channel token '{token}', expected ch=kb")
        ch_s, kb_s = token.split("=", 1)
        ch = int(ch_s, 0)
        kb = int(kb_s, 0)
        out[str(ch)] = kb
    return out


def now_utc() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc)


def default_host() -> str:
    return f"{socket.gethostname()}_reader_0"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=(
            "Write synthetic status docs for one reader host so dispatcher treats "
            "it as alive while board_test runs separately."
        )
    )
    p.add_argument("--uri", default=os.environ.get("REDAX_MONGO_URI", ""),
                   help="Mongo URI (or set REDAX_MONGO_URI)")
    p.add_argument("--db", default="daq", help="Database name (default: daq)")
    p.add_argument("--host", default=default_host(),
                   help="DAQ process host key (default: <hostname>_reader_0)")
    p.add_argument("--detector", required=True,
                   choices=["tpc", "muon_veto", "neutron_veto", "test"],
                   help="Detector to keep inactive (writes detector_control.<detector>.active=false)")
    p.add_argument("--mode", default="board_test_cheat",
                   help="Mode string written into status docs")
    p.add_argument("--number", type=int, default=-1,
                   help="Run number written into status docs (default: -1)")
    p.add_argument("--status", choices=sorted(STATUS.keys()), default="idle",
                   help="Spoofed DAQ status enum name (default: idle)")
    p.add_argument("--rate", type=float, default=0.0,
                   help="rate field in MB/s (default: 0)")
    p.add_argument("--buffer-size", type=float, default=0.0,
                   help="buffer_size field in MB (default: 0)")
    p.add_argument("--channels", default="",
                   help="channels map as ch=kb,ch=kb,... (default: empty dict)")
    p.add_argument("--interval-s", type=float, default=1.0,
                   help="Heartbeat interval seconds (default: 1)")
    p.add_argument("--duration-s", type=float, default=0.0,
                   help="Stop after N seconds (0 = run until Ctrl-C)")
    p.add_argument("--comment", default="CHEAT: board_test status spoofer",
                   help="Comment string written in status docs")
    p.add_argument("--sleep-s", type=float, default=0.1,
                   help="Loop sleep seconds (default: 0.1)")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    if not args.uri:
        print("ERROR: --uri required (or set REDAX_MONGO_URI)")
        return 2
    if args.interval_s <= 0:
        print("ERROR: --interval-s must be > 0")
        return 2
    if args.sleep_s <= 0:
        print("ERROR: --sleep-s must be > 0")
        return 2
    if args.duration_s < 0:
        print("ERROR: --duration-s must be >= 0")
        return 2

    channels = parse_channels(args.channels)
    status_name = args.status
    mode = args.mode
    number = args.number
    end_time = time.time() + args.duration_s if args.duration_s > 0 else None
    next_emit = 0.0

    print(
        f"Starting spoof heartbeat for host={args.host} status={status_name} "
        f"db={args.db} detector={args.detector} (inactive lock enabled)"
    )
    with MongoClient(args.uri) as client:
        db = client[args.db]
        status_coll = db["status"]
        detector_control_coll = db["detector_control"]

        while True:
            t_now = time.time()
            if end_time is not None and t_now >= end_time:
                print("Reached --duration-s, exiting")
                break

            if t_now >= next_emit:
                ts = now_utc()
                doc = {
                    "host": args.host,
                    "time": ts,
                    "status": STATUS[status_name],
                    "rate": args.rate,
                    "buffer_size": args.buffer_size,
                    "mode": mode,
                    "run_mode": mode,
                    "number": number,
                    "channels": channels,
                    "spoofed": True,
                    "comment": args.comment,
                }
                status_coll.insert_one(doc)
                detector_control_coll.insert_one({
                    "detector": args.detector,
                    "field": "active",
                    "value": "false",
                    "user": "board_test_spoofer",
                    "key": f"{args.detector}.active",
                    "time": ts,
                    "comment": args.comment,
                })
                print(
                    f"[{ts.isoformat()}] heartbeat status={status_name} mode={mode} "
                    f"number={number} rate={args.rate:.3f}MB/s forced_active=false"
                )
                next_emit = t_now + args.interval_s

            time.sleep(args.sleep_s)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
