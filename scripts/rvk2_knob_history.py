#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
from typing import Dict, Iterable, List

HISTORY_HEADER = [
    "run_stamp_utc",
    "scenario_id",
    "profile",
    "fingerprint",
    "deep_telemetry",
    "visual_exit",
    "rmse",
    "mae",
    "pass",
    "git_sha",
    "snapshot_path",
]


def _parse_kv_pairs(entries: Iterable[str]) -> Dict[str, str]:
    pairs: Dict[str, str] = {}
    for entry in entries:
        if "=" not in entry:
            raise SystemExit(f"ERROR: invalid --kv entry (expected key=value): {entry}")
        key, value = entry.split("=", 1)
        key = key.strip()
        if not key:
            raise SystemExit(f"ERROR: invalid --kv entry (empty key): {entry}")
        pairs[key] = value
    return dict(sorted(pairs.items()))


def _fingerprint_payload(scenario_id: str, profile: str, knobs: Dict[str, str]) -> Dict[str, object]:
    return {
        "scenario_id": scenario_id,
        "profile": profile,
        "knobs": knobs,
    }


def _fingerprint_for_payload(payload: Dict[str, object]) -> str:
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha1(encoded).hexdigest()


def _load_history_rows(history_path: Path) -> List[Dict[str, str]]:
    if not history_path.is_file():
        return []
    with history_path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        rows: List[Dict[str, str]] = []
        for row in reader:
            if row is None:
                continue
            rows.append({str(k): str(v) for k, v in row.items() if k is not None})
        return rows


def _read_metrics(metrics_path: Path) -> Dict[str, str]:
    if not metrics_path.is_file():
        return {
            "rmse": "",
            "mae": "",
            "pass": "",
        }
    try:
        payload = json.loads(metrics_path.read_text(encoding="utf-8"))
    except Exception:
        return {
            "rmse": "",
            "mae": "",
            "pass": "",
        }
    if not isinstance(payload, dict):
        return {
            "rmse": "",
            "mae": "",
            "pass": "",
        }

    rmse = payload.get("rmse", "")
    mae = payload.get("mae", "")
    passed = payload.get("pass", "")
    return {
        "rmse": "" if rmse == "" else str(rmse),
        "mae": "" if mae == "" else str(mae),
        "pass": "" if passed == "" else str(bool(passed)).lower(),
    }


def _cmd_fingerprint(args: argparse.Namespace) -> int:
    knobs = _parse_kv_pairs(args.kv or [])
    payload = _fingerprint_payload(args.scenario_id, args.profile, knobs)
    fingerprint = _fingerprint_for_payload(payload)

    out_path = Path(args.output_json)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({**payload, "fingerprint": fingerprint}, indent=2) + "\n", encoding="utf-8")

    print(fingerprint)
    return 0


def _cmd_recent(args: argparse.Namespace) -> int:
    history_path = Path(args.history)
    rows = _load_history_rows(history_path)
    scenario_rows = [row for row in rows if row.get("scenario_id", "") == args.scenario_id]
    window = max(1, int(args.window))
    window_rows = scenario_rows[-window:]
    matching = [row for row in window_rows if row.get("fingerprint", "") == args.fingerprint]

    last = matching[-1] if matching else {}
    payload = {
        "history": str(history_path),
        "scenario_id": args.scenario_id,
        "window": window,
        "window_row_count": len(window_rows),
        "same_fingerprint_count": len(matching),
        "last_same_run_stamp_utc": last.get("run_stamp_utc", ""),
        "last_same_git_sha": last.get("git_sha", ""),
        "last_same_visual_exit": last.get("visual_exit", ""),
    }
    print(json.dumps(payload, sort_keys=True))
    return 0


def _cmd_record(args: argparse.Namespace) -> int:
    history_path = Path(args.history)
    history_path.parent.mkdir(parents=True, exist_ok=True)

    metrics = _read_metrics(Path(args.metrics)) if args.metrics else {"rmse": "", "mae": "", "pass": ""}

    if not history_path.is_file():
        history_path.write_text("\t".join(HISTORY_HEADER) + "\n", encoding="utf-8")

    row = {
        "run_stamp_utc": args.run_stamp,
        "scenario_id": args.scenario_id,
        "profile": args.profile,
        "fingerprint": args.fingerprint,
        "deep_telemetry": str(int(args.deep_telemetry)),
        "visual_exit": str(int(args.visual_exit)),
        "rmse": metrics["rmse"],
        "mae": metrics["mae"],
        "pass": metrics["pass"],
        "git_sha": args.git_sha,
        "snapshot_path": args.snapshot,
    }

    sanitized = [str(row.get(key, "")).replace("\t", " ") for key in HISTORY_HEADER]
    with history_path.open("a", encoding="utf-8") as handle:
        handle.write("\t".join(sanitized) + "\n")

    print(str(history_path))
    return 0


def _cmd_summary(args: argparse.Namespace) -> int:
    history_path = Path(args.history)
    rows = _load_history_rows(history_path)
    if args.scenario_id:
        rows = [row for row in rows if row.get("scenario_id", "") == args.scenario_id]

    limit = max(1, int(args.limit))
    rows = rows[-limit:]
    rows.reverse()

    print("run_stamp_utc\tscenario_id\tprofile\tfingerprint\tdeep_telemetry\tvisual_exit\trmse\tmae\tpass")
    for row in rows:
        print(
            "\t".join(
                [
                    row.get("run_stamp_utc", ""),
                    row.get("scenario_id", ""),
                    row.get("profile", ""),
                    row.get("fingerprint", ""),
                    row.get("deep_telemetry", ""),
                    row.get("visual_exit", ""),
                    row.get("rmse", ""),
                    row.get("mae", ""),
                    row.get("pass", ""),
                ]
            )
        )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Track and summarize parity knob configurations")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_fingerprint = sub.add_parser("fingerprint", help="Build a knob snapshot and print fingerprint")
    p_fingerprint.add_argument("--output-json", required=True)
    p_fingerprint.add_argument("--scenario-id", required=True)
    p_fingerprint.add_argument("--profile", required=True)
    p_fingerprint.add_argument("--kv", action="append", default=[])
    p_fingerprint.set_defaults(func=_cmd_fingerprint)

    p_recent = sub.add_parser("recent", help="Summarize recent fingerprint usage")
    p_recent.add_argument("--history", required=True)
    p_recent.add_argument("--scenario-id", required=True)
    p_recent.add_argument("--fingerprint", required=True)
    p_recent.add_argument("--window", type=int, default=20)
    p_recent.set_defaults(func=_cmd_recent)

    p_record = sub.add_parser("record", help="Append a run to knob history")
    p_record.add_argument("--history", required=True)
    p_record.add_argument("--run-stamp", required=True)
    p_record.add_argument("--scenario-id", required=True)
    p_record.add_argument("--profile", required=True)
    p_record.add_argument("--fingerprint", required=True)
    p_record.add_argument("--deep-telemetry", type=int, required=True)
    p_record.add_argument("--visual-exit", type=int, required=True)
    p_record.add_argument("--git-sha", required=True)
    p_record.add_argument("--snapshot", required=True)
    p_record.add_argument("--metrics", default="")
    p_record.set_defaults(func=_cmd_record)

    p_summary = sub.add_parser("summary", help="Print recent knob history rows")
    p_summary.add_argument("--history", required=True)
    p_summary.add_argument("--scenario-id", default="")
    p_summary.add_argument("--limit", type=int, default=20)
    p_summary.set_defaults(func=_cmd_summary)

    args = parser.parse_args()
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
