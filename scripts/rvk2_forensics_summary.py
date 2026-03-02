#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict, List


def parse_int(value: str) -> int:
    text = value.strip()
    if text.startswith(("0x", "0X")):
        return int(text, 16)
    return int(text, 10)


def parse_record(line: str) -> Dict[str, int]:
    record: Dict[str, int] = {}
    for token in line.strip().split("\t"):
        if not token or "=" not in token:
            continue
        key, raw = token.split("=", 1)
        key = key.strip()
        raw = raw.strip()
        if not key or not raw:
            continue
        try:
            record[key] = parse_int(raw)
        except ValueError:
            continue
    return record


def sum_field(records: List[Dict[str, int]], key: str) -> int:
    total = 0
    for rec in records:
        total += rec.get(key, 0)
    return total


def ratio(numer: int, denom: int) -> float:
    if denom == 0:
        return 0.0
    return float(numer) / float(denom)


def fmt_ratio(numer: int, denom: int) -> str:
    return f"{ratio(numer, denom):.6f} ({numer}/{denom})"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize RVK2 frame forensics TSV key/value lines."
    )
    parser.add_argument("--input", required=True, help="Path to forensics TSV log.")
    parser.add_argument(
        "--active-only",
        action="store_true",
        help="Restrict summary to frames with non-zero present dimensions.",
    )
    args = parser.parse_args()

    path = Path(args.input)
    if not path.exists():
        raise SystemExit(f"ERROR: input file not found: {path}")

    records: List[Dict[str, int]] = []
    with path.open("r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            rec = parse_record(line)
            if not rec:
                continue
            if args.active_only:
                if rec.get("present_w", 0) == 0 or rec.get("present_h", 0) == 0:
                    continue
            records.append(rec)

    if not records:
        raise SystemExit("ERROR: no records after filtering.")

    alpha_tests = sum_field(records, "alpha_tests")
    alpha_rejects = sum_field(records, "alpha_rejects")
    cvg_tests = sum_field(records, "cvg_tests")
    cvg_rejects = sum_field(records, "cvg_rejects")
    depth_eval = sum_field(records, "depth_eval")
    depth_reject = sum_field(records, "depth_reject")
    depth_update = sum_field(records, "depth_update")
    blend_ops = sum_field(records, "blend_ops")
    blend_enabled = sum_field(records, "blend_enabled_ops")
    blend_force = sum_field(records, "blend_force_ops")
    blend_aa = sum_field(records, "blend_aa_ops")
    blend_cvg_eval = sum_field(records, "blend_cvg_eval")
    blend_cvg_zero = sum_field(records, "blend_cvg_zero")
    out_writes = sum_field(records, "writes")

    blend_a_sel = [sum_field(records, f"blend_a_sel{i}") for i in range(4)]
    blend_b_sel = [sum_field(records, f"blend_b_sel{i}") for i in range(4)]

    print(f"records={len(records)} active_only={1 if args.active_only else 0}")
    print(f"writes={out_writes}")
    print(f"alpha_reject_rate={fmt_ratio(alpha_rejects, alpha_tests)}")
    print(f"coverage_reject_rate={fmt_ratio(cvg_rejects, cvg_tests)}")
    print(f"depth_reject_rate={fmt_ratio(depth_reject, depth_eval)}")
    print(f"depth_update_rate={fmt_ratio(depth_update, depth_eval)}")
    print(f"blend_enabled_rate={fmt_ratio(blend_enabled, blend_ops)}")
    print(f"blend_force_rate={fmt_ratio(blend_force, blend_ops)}")
    print(f"blend_aa_rate={fmt_ratio(blend_aa, blend_ops)}")
    print(f"blend_coverage_zero_rate={fmt_ratio(blend_cvg_zero, blend_cvg_eval)}")
    print(
        "blend_alpha_a_selector_share="
        + ",".join(
            f"s{i}:{ratio(blend_a_sel[i], sum(blend_a_sel)):.6f}" for i in range(4)
        )
    )
    print(
        "blend_alpha_b_selector_share="
        + ",".join(
            f"s{i}:{ratio(blend_b_sel[i], sum(blend_b_sel)):.6f}" for i in range(4)
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
