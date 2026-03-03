#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional


def _load_json(path: Optional[Path]) -> Optional[Dict[str, Any]]:
    if path is None or not path.is_file():
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None
    if isinstance(data, dict):
        return data
    return None


def _file_meta(path: Optional[Path]) -> Dict[str, Any]:
    if path is None:
        return {"path": None, "exists": False, "size_bytes": 0}
    exists = path.is_file()
    return {
        "path": str(path),
        "exists": exists,
        "size_bytes": path.stat().st_size if exists else 0,
    }


def _parse_int(value: str) -> Optional[int]:
    text = value.strip()
    if not text:
        return None
    try:
        if text.startswith(("0x", "0X")):
            return int(text, 16)
        return int(text, 10)
    except ValueError:
        return None


def _parse_forensics(path: Optional[Path]) -> Dict[str, Any]:
    if path is None or not path.is_file():
        return {"record_count": 0, "last_record": {}}

    lines = [line.strip() for line in path.read_text(encoding="utf-8", errors="replace").splitlines() if line.strip()]
    if not lines:
        return {"record_count": 0, "last_record": {}}

    record: Dict[str, Any] = {}
    for token in lines[-1].split("\t"):
        if "=" not in token:
            continue
        key, raw = token.split("=", 1)
        key = key.strip()
        raw = raw.strip()
        if not key:
            continue
        parsed = _parse_int(raw)
        record[key] = parsed if parsed is not None else raw

    return {"record_count": len(lines), "last_record": record}


def _u64(record: Dict[str, Any], key: str) -> int:
    value = record.get(key, 0)
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        parsed = _parse_int(value)
        if parsed is not None:
            return parsed
    return 0


def _ratio(numer: int, denom: int) -> Optional[float]:
    if denom == 0:
        return None
    return float(numer) / float(denom)


def _parse_launch_log(path: Optional[Path]) -> Dict[str, Any]:
    if path is None or not path.is_file():
        return {
            "readback_marker_count": 0,
            "depth_blit_fail_marker_count": 0,
            "depth_stats_last": None,
        }

    text = path.read_text(encoding="utf-8", errors="replace")
    readback_count = len(re.findall(r"VK readback debug:", text))
    depth_fail_count = len(re.findall(r"op=blit_depth_fail", text))

    depth_stats_last = None
    for match in re.finditer(r"depthStats=\[attempts=(\d+) success=(\d+) fail=(\d+)\]", text):
        depth_stats_last = {
            "attempts": int(match.group(1)),
            "successes": int(match.group(2)),
            "failures": int(match.group(3)),
        }

    return {
        "readback_marker_count": readback_count,
        "depth_blit_fail_marker_count": depth_fail_count,
        "depth_stats_last": depth_stats_last,
    }


def _summarize_replay(replay: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    if replay is None:
        return {
            "frame_count": 0,
            "failed_count": 0,
            "warning_count": 0,
            "all_ok": False,
            "first_failed_frame": None,
            "first_warn_frame": None,
            "error_kind_counts": {},
        }

    frames = replay.get("frames", [])
    failed_frame = None
    warn_frame = None
    error_kind_counts: Dict[str, int] = {}
    if isinstance(frames, list):
        for frame in frames:
            if not isinstance(frame, dict):
                continue
            errors = frame.get("errors", [])
            if isinstance(errors, list):
                for raw_error in errors:
                    if not isinstance(raw_error, str):
                        continue
                    kind = raw_error.split(":", 1)[0].strip()
                    if not kind:
                        continue
                    error_kind_counts[kind] = error_kind_counts.get(kind, 0) + 1
            if failed_frame is None and frame.get("ok") is False:
                failed_frame = {
                    "frame_id": frame.get("frame_id"),
                    "errors": errors if isinstance(errors, list) else [],
                    "warnings": frame.get("warnings", []),
                }
            if warn_frame is None and frame.get("ok") is True and frame.get("warnings"):
                warn_frame = {
                    "frame_id": frame.get("frame_id"),
                    "warnings": frame.get("warnings", []),
                }
            if failed_frame is not None and warn_frame is not None:
                break

    return {
        "frame_count": int(replay.get("frame_count", 0) or 0),
        "failed_count": int(replay.get("failed_count", 0) or 0),
        "warning_count": int(replay.get("warning_count", 0) or 0),
        "all_ok": bool(replay.get("all_ok", False)),
        "strict_mode": bool(replay.get("strict_mode", False)),
        "first_failed_frame": failed_frame,
        "first_warn_frame": warn_frame,
        "error_kind_counts": error_kind_counts,
    }


def _build_signals(
    last_record: Dict[str, Any],
    replay_summary: Dict[str, Any],
    launch_summary: Dict[str, Any],
    depth_summary: Optional[Dict[str, Any]],
    metrics: Optional[Dict[str, Any]],
) -> Dict[str, Any]:
    tx_samples = _u64(last_record, "tx_samples")
    tx_tmem = _u64(last_record, "tx_tmem")
    tx_rdram = _u64(last_record, "tx_rdram")
    tx_synth = _u64(last_record, "tx_synth")
    stage_textured_writes = _u64(last_record, "stage_textured_writes")
    stage_tx_tmem = _u64(last_record, "stage_tx_tmem")
    stage_tx_rdram = _u64(last_record, "stage_tx_rdram")
    stage_tx_synth = _u64(last_record, "stage_tx_synth")

    work_fill = _u64(last_record, "work_fill")
    work_texrect = _u64(last_record, "work_texrect")
    work_tri = _u64(last_record, "work_tri")
    write_fill = _u64(last_record, "write_fill")
    write_texrect = _u64(last_record, "write_texrect")
    write_tri = _u64(last_record, "write_tri")
    writes = _u64(last_record, "writes")

    depth_eval = _u64(last_record, "depth_eval")
    depth_reject = _u64(last_record, "depth_reject")

    texture_source_sum = tx_tmem + tx_rdram + tx_synth
    texture_source_gap = max(0, tx_samples - texture_source_sum)

    texture_signal = {
        "tx_samples": tx_samples,
        "tx_tmem": tx_tmem,
        "tx_rdram": tx_rdram,
        "tx_synth": tx_synth,
        "tx_tmem_share": _ratio(tx_tmem, tx_samples),
        "tx_rdram_share": _ratio(tx_rdram, tx_samples),
        "tx_synth_share": _ratio(tx_synth, tx_samples),
        "texture_source_gap": texture_source_gap,
        "stage_textured_writes": stage_textured_writes,
        "stage_tx_tmem": stage_tx_tmem,
        "stage_tx_rdram": stage_tx_rdram,
        "stage_tx_synth": stage_tx_synth,
    }

    geometry_signal = {
        "work_fill": work_fill,
        "work_texrect": work_texrect,
        "work_tri": work_tri,
        "write_fill": write_fill,
        "write_texrect": write_texrect,
        "write_tri": write_tri,
        "writes": writes,
        "triangle_writes_per_work": _ratio(write_tri, work_tri),
        "texrect_writes_per_work": _ratio(write_texrect, work_texrect),
    }

    depth_signal = {
        "depth_eval": depth_eval,
        "depth_reject": depth_reject,
        "depth_reject_ratio": _ratio(depth_reject, depth_eval),
        "depth_blit_fail_marker_count": int(launch_summary.get("depth_blit_fail_marker_count", 0) or 0),
        "depth_stats_last": launch_summary.get("depth_stats_last"),
        "depth_summary": depth_summary,
    }

    suspected_gaps: List[str] = []

    if tx_samples > 0 and tx_tmem == 0 and tx_rdram == 0 and tx_synth == 0:
        suspected_gaps.append("texture samples were recorded but no source bucket advanced (TMEM/RDRAM/synth all zero)")
    if stage_textured_writes > 0 and tx_samples == 0:
        suspected_gaps.append("textured writes occurred without texture sample accounting")
    if tx_samples > 0 and texture_source_gap > 0 and texture_source_gap * 20 >= tx_samples:
        suspected_gaps.append("texture source attribution gap exceeds 5% of samples")

    if work_tri > 0 and write_tri == 0:
        suspected_gaps.append("triangle work exists but triangle write count is zero (possible missing geometry path)")
    if work_texrect > 0 and write_texrect == 0:
        suspected_gaps.append("texrect work exists but texrect write count is zero")
    if (work_fill + work_texrect + work_tri) > 0 and writes == 0:
        suspected_gaps.append("render work was scheduled but no color writes were produced")

    if replay_summary.get("failed_count", 0) > 0:
        suspected_gaps.append("packet replay reports frame mismatches (state divergence possible before raster output)")

    replay_error_kinds = replay_summary.get("error_kind_counts", {})
    if isinstance(replay_error_kinds, dict):
        if int(replay_error_kinds.get("executor_present_width mismatch", 0) or 0) > 0 or int(
            replay_error_kinds.get("executor_present_height mismatch", 0) or 0
        ) > 0:
            suspected_gaps.append("replay reports present-size mismatches (possible geometry/viewport divergence)")

    visibility_signal = {}
    if isinstance(metrics, dict):
        reference_non_black = float(metrics.get("reference_non_black_ratio", 0.0) or 0.0)
        candidate_non_black = float(metrics.get("candidate_non_black_ratio", 0.0) or 0.0)
        reference_luma = float(metrics.get("reference_mean_luma", 0.0) or 0.0)
        candidate_luma = float(metrics.get("candidate_mean_luma", 0.0) or 0.0)
        coverage_ratio = _ratio(int(candidate_non_black * 1_000_000), int(reference_non_black * 1_000_000))
        luma_ratio = _ratio(int(candidate_luma * 1_000_000), int(reference_luma * 1_000_000))
        visibility_signal = {
            "reference_non_black_ratio": reference_non_black,
            "candidate_non_black_ratio": candidate_non_black,
            "coverage_ratio_vs_reference": coverage_ratio,
            "reference_mean_luma": reference_luma,
            "candidate_mean_luma": candidate_luma,
            "luma_ratio_vs_reference": luma_ratio,
        }
        if coverage_ratio is not None and coverage_ratio < 0.90:
            suspected_gaps.append("candidate non-black coverage is below 90% of reference (missing geometry/visibility likely)")
        if luma_ratio is not None and luma_ratio < 0.85:
            suspected_gaps.append("candidate mean luma is below 85% of reference (missing texture/detail likely)")

    if int(launch_summary.get("readback_marker_count", 0) or 0) == 0:
        suspected_gaps.append("launch log contains no VK readback debug markers")

    return {
        "texture": texture_signal,
        "geometry": geometry_signal,
        "depth": depth_signal,
        "visibility": visibility_signal,
        "suspected_gaps": suspected_gaps,
    }


def _selected_forensics_fields(record: Dict[str, Any]) -> Dict[str, Any]:
    keys = [
        "frame",
        "work",
        "writes",
        "present_hash",
        "present_w",
        "present_h",
        "tx_samples",
        "tx_tmem",
        "tx_rdram",
        "tx_synth",
        "tx_lut",
        "stage_textured_writes",
        "stage_tx_tmem",
        "stage_tx_rdram",
        "stage_tx_synth",
        "work_fill",
        "work_texrect",
        "work_tri",
        "write_fill",
        "write_texrect",
        "write_tri",
        "depth_eval",
        "depth_reject",
        "depth_update",
    ]
    return {key: record.get(key) for key in keys if key in record}


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a deep telemetry bundle for Paper Mario parity runs.")
    parser.add_argument("--scenario-id", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--metrics")
    parser.add_argument("--capture-context")
    parser.add_argument("--reference-capture")
    parser.add_argument("--candidate-capture")
    parser.add_argument("--reference-png")
    parser.add_argument("--candidate-png")
    parser.add_argument("--diff-image")
    parser.add_argument("--trace-file")
    parser.add_argument("--packet-trace")
    parser.add_argument("--packet-replay")
    parser.add_argument("--forensics")
    parser.add_argument("--forensics-summary")
    parser.add_argument("--forensics-summary-active")
    parser.add_argument("--depth-summary")
    parser.add_argument("--launch-log")
    parser.add_argument("--packet-replay-exit", type=int, default=-1)
    parser.add_argument("--forensics-summary-exit", type=int, default=-1)
    parser.add_argument("--forensics-summary-active-exit", type=int, default=-1)
    args = parser.parse_args()

    output = Path(args.output)
    metrics_path = Path(args.metrics) if args.metrics else None
    capture_context_path = Path(args.capture_context) if args.capture_context else None
    reference_capture = Path(args.reference_capture) if args.reference_capture else None
    candidate_capture = Path(args.candidate_capture) if args.candidate_capture else None
    reference_png = Path(args.reference_png) if args.reference_png else None
    candidate_png = Path(args.candidate_png) if args.candidate_png else None
    diff_image = Path(args.diff_image) if args.diff_image else None
    trace_file = Path(args.trace_file) if args.trace_file else None
    packet_trace = Path(args.packet_trace) if args.packet_trace else None
    packet_replay = Path(args.packet_replay) if args.packet_replay else None
    forensics = Path(args.forensics) if args.forensics else None
    forensics_summary = Path(args.forensics_summary) if args.forensics_summary else None
    forensics_summary_active = Path(args.forensics_summary_active) if args.forensics_summary_active else None
    depth_summary_path = Path(args.depth_summary) if args.depth_summary else None
    launch_log = Path(args.launch_log) if args.launch_log else None

    metrics = _load_json(metrics_path)
    capture_context = _load_json(capture_context_path)
    replay = _load_json(packet_replay)
    depth_summary = _load_json(depth_summary_path)

    forensics_data = _parse_forensics(forensics)
    replay_summary = _summarize_replay(replay)
    launch_summary = _parse_launch_log(launch_log)
    signal_summary = _build_signals(
        forensics_data.get("last_record", {}),
        replay_summary,
        launch_summary,
        depth_summary,
        metrics,
    )

    payload = {
        "schema": "rvk2_pm_telemetry_bundle_v1",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "scenario_id": args.scenario_id,
        "status": {
            "packet_replay_exit": args.packet_replay_exit,
            "forensics_summary_exit": args.forensics_summary_exit,
            "forensics_summary_active_exit": args.forensics_summary_active_exit,
        },
        "artifacts": {
            "metrics": _file_meta(metrics_path),
            "capture_context": _file_meta(capture_context_path),
            "reference_capture": _file_meta(reference_capture),
            "candidate_capture": _file_meta(candidate_capture),
            "reference_png": _file_meta(reference_png),
            "candidate_png": _file_meta(candidate_png),
            "diff_image": _file_meta(diff_image),
            "trace_file": _file_meta(trace_file),
            "packet_trace": _file_meta(packet_trace),
            "packet_replay": _file_meta(packet_replay),
            "forensics": _file_meta(forensics),
            "forensics_summary": _file_meta(forensics_summary),
            "forensics_summary_active": _file_meta(forensics_summary_active),
            "depth_summary": _file_meta(depth_summary_path),
            "launch_log": _file_meta(launch_log),
        },
        "metrics": metrics,
        "capture_context": capture_context,
        "packet_replay_summary": replay_summary,
        "forensics": {
            "record_count": int(forensics_data.get("record_count", 0) or 0),
            "last_frame_selected_fields": _selected_forensics_fields(forensics_data.get("last_record", {})),
        },
        "launch_log_summary": launch_summary,
        "signals": {
            "texture": signal_summary["texture"],
            "geometry": signal_summary["geometry"],
            "depth": signal_summary["depth"],
            "visibility": signal_summary["visibility"],
        },
        "suspected_gaps": signal_summary["suspected_gaps"],
    }

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
