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


def _load_json_any(path: Optional[Path]) -> Optional[Any]:
    if path is None or not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def _load_text(path: Optional[Path], limit: int = 12000) -> Optional[str]:
    if path is None or not path.is_file():
        return None
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return None
    if len(text) > limit:
        return text[:limit]
    return text


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
        return {"record_count": 0, "records": [], "records_by_frame": {}, "last_record": {}}

    lines = [line.strip() for line in path.read_text(encoding="utf-8", errors="replace").splitlines() if line.strip()]
    if not lines:
        return {"record_count": 0, "records": [], "records_by_frame": {}, "last_record": {}}

    records: List[Dict[str, Any]] = []
    records_by_frame: Dict[int, Dict[str, Any]] = {}
    for line in lines:
        record: Dict[str, Any] = {}
        for token in line.split("\t"):
            if "=" not in token:
                continue
            key, raw = token.split("=", 1)
            key = key.strip()
            raw = raw.strip()
            if not key:
                continue
            parsed = _parse_int(raw)
            record[key] = parsed if parsed is not None else raw
        if not record:
            continue
        records.append(record)
        frame_id_value = record.get("frame")
        if isinstance(frame_id_value, int):
            records_by_frame[frame_id_value] = record

    last_record = records[-1] if records else {}
    return {
        "record_count": len(records),
        "records": records,
        "records_by_frame": records_by_frame,
        "last_record": last_record,
    }


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
            "stateful_frames": None,
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
        "stateful_frames": replay.get("stateful_frames") if isinstance(replay.get("stateful_frames"), bool) else None,
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
    command_census: Optional[Dict[str, Any]],
    diff_playbook_summary: Optional[Dict[str, Any]],
) -> Dict[str, Any]:
    tx_samples = _u64(last_record, "tx_samples")
    tx_tmem = _u64(last_record, "tx_tmem")
    tx_rdram = _u64(last_record, "tx_rdram")
    tx_synth = _u64(last_record, "tx_synth")
    stage_textured_writes = _u64(last_record, "stage_textured_writes")
    stage_tx_tmem = _u64(last_record, "stage_tx_tmem")
    stage_tx_rdram = _u64(last_record, "stage_tx_rdram")
    stage_tx_synth = _u64(last_record, "stage_tx_synth")
    present_hash = _u64(last_record, "present_hash")
    present_w = _u64(last_record, "present_w")
    present_h = _u64(last_record, "present_h")
    present_surface = _u64(last_record, "present_surface")
    present_select = _u64(last_record, "present_select")
    vi_valid = _u64(last_record, "vi_valid")
    vi_origin = _u64(last_record, "vi_origin")
    vi_origin_match = _u64(last_record, "vi_origin_match")
    vi_reject = _u64(last_record, "vi_reject")
    vi_type = _u64(last_record, "vi_type")
    vi_use_regs = _u64(last_record, "vi_use_regs")
    vi_src_w = _u64(last_record, "vi_src_w")
    vi_src_h = _u64(last_record, "vi_src_h")
    vi_out_w = _u64(last_record, "vi_out_w")
    vi_out_h = _u64(last_record, "vi_out_h")
    vi_stride = _u64(last_record, "vi_stride")
    selected_surface_hash = _u64(last_record, "selected_surface_hash")
    selected_surface_live_writes = _u64(last_record, "selected_surface_live_writes")
    selected_surface_live_works = _u64(last_record, "selected_surface_live_works")
    selected_surface_from_history = _u64(last_record, "selected_surface_from_history")
    vi_hash_decode = _u64(last_record, "vi_hash_decode")
    vi_hash_filter = _u64(last_record, "vi_hash_filter")
    vi_hash_gdither = _u64(last_record, "vi_hash_gdither")

    work_fill = _u64(last_record, "work_fill")
    work_texrect = _u64(last_record, "work_texrect")
    work_tri = _u64(last_record, "work_tri")
    write_fill = _u64(last_record, "write_fill")
    write_texrect = _u64(last_record, "write_texrect")
    write_tri = _u64(last_record, "write_tri")
    ci_switches = _u64(last_record, "ci_switches")
    ci_first = _u64(last_record, "ci_first")
    ci_last = _u64(last_record, "ci_last")
    tri_deg_reject = _u64(last_record, "tri_deg_reject")
    tri_bounds_reject = _u64(last_record, "tri_bounds_reject")
    tri_scissor_reject = _u64(last_record, "tri_scissor_reject")
    tri_samples = _u64(last_record, "tri_samples")
    tri_alpha_reject = _u64(last_record, "tri_alpha_reject")
    tri_cvg_reject = _u64(last_record, "tri_cvg_reject")
    tri_depth_reject = _u64(last_record, "tri_depth_reject")
    tri_nonblack = _u64(last_record, "tri_nonblack")
    texrect_nonblack = _u64(last_record, "texrect_nonblack")
    tri_luma_sum = _u64(last_record, "tri_luma_sum")
    texrect_luma_sum = _u64(last_record, "texrect_luma_sum")
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
    present_signal = {
        "present_hash": present_hash,
        "present_width": present_w,
        "present_height": present_h,
        "present_surface": present_surface,
        "present_select": present_select,
        "vi_valid": vi_valid,
        "vi_origin": vi_origin,
        "vi_origin_match": vi_origin_match,
        "vi_reject": vi_reject,
        "vi_type": vi_type,
        "vi_use_regs": vi_use_regs,
        "vi_src_w": vi_src_w,
        "vi_src_h": vi_src_h,
        "vi_out_w": vi_out_w,
        "vi_out_h": vi_out_h,
        "vi_stride": vi_stride,
        "selected_surface_hash": selected_surface_hash,
        "selected_surface_live_writes": selected_surface_live_writes,
        "selected_surface_live_works": selected_surface_live_works,
        "selected_surface_from_history": selected_surface_from_history,
        "vi_hash_decode": vi_hash_decode,
        "vi_hash_filter": vi_hash_filter,
        "vi_hash_gdither": vi_hash_gdither,
    }

    geometry_signal = {
        "work_fill": work_fill,
        "work_texrect": work_texrect,
        "work_tri": work_tri,
        "write_fill": write_fill,
        "write_texrect": write_texrect,
        "write_tri": write_tri,
        "writes": writes,
        "ci_switches": ci_switches,
        "ci_first": ci_first,
        "ci_last": ci_last,
        "tri_deg_reject": tri_deg_reject,
        "tri_bounds_reject": tri_bounds_reject,
        "tri_scissor_reject": tri_scissor_reject,
        "tri_samples": tri_samples,
        "tri_alpha_reject": tri_alpha_reject,
        "tri_alpha_reject_ratio": _ratio(tri_alpha_reject, tri_samples),
        "tri_cvg_reject": tri_cvg_reject,
        "tri_cvg_reject_ratio": _ratio(tri_cvg_reject, tri_samples),
        "tri_depth_reject": tri_depth_reject,
        "tri_depth_reject_ratio": _ratio(tri_depth_reject, tri_samples),
        "tri_nonblack": tri_nonblack,
        "tri_nonblack_ratio": _ratio(tri_nonblack, write_tri),
        "texrect_nonblack": texrect_nonblack,
        "texrect_nonblack_ratio": _ratio(texrect_nonblack, write_texrect),
        "tri_luma_sum": tri_luma_sum,
        "tri_luma_per_write": _ratio(tri_luma_sum, write_tri),
        "texrect_luma_sum": texrect_luma_sum,
        "texrect_luma_per_write": _ratio(texrect_luma_sum, write_texrect),
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
    hard_faults: List[str] = []

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
    if tri_samples > 0 and tri_depth_reject * 2 >= tri_samples:
        suspected_gaps.append("triangle depth rejects exceed 50% of candidate samples (possible depth compare/update issue)")
    if tri_samples > 0 and tri_cvg_reject * 2 >= tri_samples:
        suspected_gaps.append("triangle coverage rejects exceed 50% of candidate samples (possible coverage/AA/cvg path issue)")
    if ci_switches >= 2:
        suspected_gaps.append("multiple color-image target switches observed in final frame (verify present target selection)")
    tri_nonblack_ratio = _ratio(tri_nonblack, write_tri)
    texrect_nonblack_ratio = _ratio(texrect_nonblack, write_texrect)
    if (
        tri_nonblack_ratio is not None
        and texrect_nonblack_ratio is not None
        and write_tri > 0
        and write_texrect > 0
        and tri_nonblack_ratio + 0.25 < texrect_nonblack_ratio
    ):
        suspected_gaps.append(
            "triangle writes are significantly darker than texrect writes (possible triangle combiner/texture decode mismatch)"
        )

    replay_failed_count = int(replay_summary.get("failed_count", 0) or 0)
    replay_frame_count = int(replay_summary.get("frame_count", 0) or 0)
    replay_stateful = replay_summary.get("stateful_frames")
    replay_is_stateful = isinstance(replay_stateful, bool) and replay_stateful
    replay_is_non_stateful = isinstance(replay_stateful, bool) and not replay_stateful
    replay_failure_ratio = _ratio(replay_failed_count, replay_frame_count)

    if replay_failed_count > 0:
        if replay_is_non_stateful:
            suspected_gaps.append(
                "packet replay mismatches were collected in non-stateful mode; use stateful replay only for deterministic first-divergence checks"
            )
        else:
            suspected_gaps.append("packet replay reports frame mismatches (state divergence possible before raster output)")

    replay_error_kinds = replay_summary.get("error_kind_counts", {})
    replay_error_kinds_actionable = replay_is_stateful or (
        replay_failure_ratio is not None and replay_failure_ratio < 0.5
    )
    if isinstance(replay_error_kinds, dict) and replay_error_kinds_actionable:
        if int(replay_error_kinds.get("executor_present_width mismatch", 0) or 0) > 0 or int(
            replay_error_kinds.get("executor_present_height mismatch", 0) or 0
        ) > 0:
            suspected_gaps.append("replay reports present-size mismatches (possible geometry/viewport divergence)")
        if int(replay_error_kinds.get("forensics_present_hash", 0) or 0) > 0:
            suspected_gaps.append("replay present-hash mismatches include frame-forensics VI context")
        if int(replay_error_kinds.get("executor_present_hash provenance", 0) or 0) > 0:
            suspected_gaps.append("packet-trace present hash diverges from frame-forensics present hash provenance")
        if int(replay_error_kinds.get("selected_surface_hash mismatch", 0) or 0) > 0:
            suspected_gaps.append("selected surface hash diverges before VI post-processing (raster/source divergence likely)")
    elif isinstance(replay_error_kinds, dict) and replay_is_non_stateful and replay_failed_count > 0:
        suspected_gaps.append(
            "replay error-kind breakdown suppressed because non-stateful replay failed on most frames; run stateful mode for high-confidence class attribution"
        )

    if vi_valid == 1 and vi_use_regs == 0:
        suspected_gaps.append("VI registers are valid but VI register path is not active for present")
    if vi_reject != 0:
        suspected_gaps.append("VI resolver rejected current frame state (see vi_reject code)")
    if vi_valid == 1 and vi_origin_match == 0:
        suspected_gaps.append("VI origin did not match selected present surface")
    if selected_surface_from_history != 0:
        suspected_gaps.append("present selected surface came from history cache (potential stale-buffer presentation)")
    if writes > 0 and selected_surface_live_writes == 0 and selected_surface_from_history != 0:
        suspected_gaps.append("presented surface had no live writes in this frame (buffer handoff mismatch candidate)")
        hard_faults.append("present-surface handoff fault: selected history surface with zero live writes in an active frame")

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

    command_signal: Dict[str, Any] = {}
    if isinstance(command_census, dict):
        overall = command_census.get("overall", {})
        overall_families = overall.get("rdp_family_counts", {}) if isinstance(overall, dict) else {}
        focus_frame = command_census.get("focus_frame", {})
        focus_families = focus_frame.get("rdp_family_counts", {}) if isinstance(focus_frame, dict) else {}

        tri_total = int(overall_families.get("triangles", 0) or 0) if isinstance(overall_families, dict) else 0
        texrect_total = int(overall_families.get("texrect", 0) or 0) if isinstance(overall_families, dict) else 0
        fillrect_total = int(overall_families.get("fillrect", 0) or 0) if isinstance(overall_families, dict) else 0
        set_color_total = (
            int(overall_families.get("set_color_image", 0) or 0) if isinstance(overall_families, dict) else 0
        )

        tri_focus = int(focus_families.get("triangles", 0) or 0) if isinstance(focus_families, dict) else 0
        texrect_focus = int(focus_families.get("texrect", 0) or 0) if isinstance(focus_families, dict) else 0
        fillrect_focus = int(focus_families.get("fillrect", 0) or 0) if isinstance(focus_families, dict) else 0
        set_color_focus = int(focus_families.get("set_color_image", 0) or 0) if isinstance(focus_families, dict) else 0
        focus_color_unique_targets = int(
            command_census.get("focus_frame_set_color_image_unique_target_count", 0) or 0
        )

        command_signal = {
            "replay_first_failed_frame": command_census.get("replay_first_failed_frame"),
            "replay_stateful_frames": command_census.get("replay_stateful_frames"),
            "focus_frame_id": command_census.get("focus_frame_id"),
            "focus_reason": command_census.get("focus_reason"),
            "focus_frame_set_color_image_unique_target_count": focus_color_unique_targets,
            "triangles_total": tri_total,
            "texrect_total": texrect_total,
            "fillrect_total": fillrect_total,
            "set_color_image_total": set_color_total,
            "triangles_focus": tri_focus,
            "texrect_focus": texrect_focus,
            "fillrect_focus": fillrect_focus,
            "set_color_image_focus": set_color_focus,
            "leads": command_census.get("leads", []),
        }

        if tri_total == 0 and (texrect_total > 0 or fillrect_total > 0):
            suspected_gaps.append(
                "command census shows texrect/fill traffic but no triangle traffic (possible RSP microcode or DL submission drop)"
            )
        if tri_focus == 0 and texrect_focus > 0:
            suspected_gaps.append("focus frame has texrect traffic without triangles (UI-only render symptom)")
        if focus_color_unique_targets > 1:
            suspected_gaps.append("focus frame changes color-image target multiple times (verify VI source buffer selection)")
        if tri_total > 0 and texrect_total > 0 and write_tri == 0:
            suspected_gaps.append("triangles are present in command stream but no triangle writes were produced")

    deviation_signal: Dict[str, Any] = {}
    if isinstance(diff_playbook_summary, dict):
        metrics_payload = diff_playbook_summary.get("metrics", {})
        first_raw = metrics_payload.get("first_mismatch_raw") if isinstance(metrics_payload, dict) else None
        first_dilated = metrics_payload.get("first_mismatch") if isinstance(metrics_payload, dict) else None
        percent_changed = float(metrics_payload.get("percent_changed", 0.0) or 0.0) if isinstance(metrics_payload, dict) else 0.0
        box_count = int(diff_playbook_summary.get("box_count", 0) or 0)
        deviation_signal = {
            "threshold": diff_playbook_summary.get("threshold"),
            "min_area": diff_playbook_summary.get("min_area"),
            "dilate": diff_playbook_summary.get("dilate"),
            "box_count": box_count,
            "percent_changed": percent_changed,
            "first_mismatch_raw": first_raw,
            "first_mismatch": first_dilated,
        }
        if box_count == 0 and percent_changed == 0.0:
            suspected_gaps.append("image diff playbook found no structural mismatch (check threshold or capture mismatch)")

    if int(launch_summary.get("readback_marker_count", 0) or 0) == 0:
        suspected_gaps.append("launch log contains no VK readback debug markers")

    return {
        "present": present_signal,
        "texture": texture_signal,
        "geometry": geometry_signal,
        "depth": depth_signal,
        "visibility": visibility_signal,
        "command": command_signal,
        "deviation": deviation_signal,
        "suspected_gaps": suspected_gaps,
        "hard_faults": hard_faults,
    }


def _summarize_forensics_records(records: List[Dict[str, Any]]) -> Dict[str, Any]:
    if not records:
        return {
            "active_record_count": 0,
            "vi_valid_rate": None,
            "vi_use_register_rate": None,
            "vi_reject_rate": None,
            "vi_origin_match_rate": None,
            "vi_source_invalid_rate": None,
            "present_select_share": {},
        }

    active_records = [
        rec
        for rec in records
        if _u64(rec, "present_w") > 0 and _u64(rec, "present_h") > 0
    ]
    total = len(active_records) if active_records else len(records)
    source = active_records if active_records else records

    vi_valid = sum(1 for rec in source if _u64(rec, "vi_valid") != 0)
    vi_use_regs = sum(1 for rec in source if _u64(rec, "vi_use_regs") != 0)
    vi_reject = sum(1 for rec in source if _u64(rec, "vi_reject") != 0)
    vi_origin_match = sum(1 for rec in source if _u64(rec, "vi_origin_match") != 0)
    vi_src_samples = sum(_u64(rec, "vi_src_samples") for rec in source)
    vi_src_invalid = sum(_u64(rec, "vi_src_invalid") for rec in source)

    present_select_counts: Dict[str, int] = {}
    for rec in source:
        key = f"s{_u64(rec, 'present_select')}"
        present_select_counts[key] = present_select_counts.get(key, 0) + 1

    present_select_share: Dict[str, float] = {}
    for key, count in sorted(present_select_counts.items()):
        ratio = _ratio(count, total)
        if ratio is not None:
            present_select_share[key] = ratio

    return {
        "active_record_count": len(active_records),
        "vi_valid_rate": _ratio(vi_valid, total),
        "vi_use_register_rate": _ratio(vi_use_regs, total),
        "vi_reject_rate": _ratio(vi_reject, total),
        "vi_origin_match_rate": _ratio(vi_origin_match, total),
        "vi_source_invalid_rate": _ratio(vi_src_invalid, vi_src_samples),
        "present_select_share": present_select_share,
    }


def _build_replay_forensics_correlation(
    replay: Optional[Dict[str, Any]],
    records_by_frame: Dict[int, Dict[str, Any]],
) -> Dict[str, Any]:
    if replay is None:
        return {
            "present_hash_mismatch_frames": 0,
            "present_hash_mismatch_with_forensics": 0,
            "missing_forensics_for_present_mismatch": 0,
            "present_hash_mismatch_by_select": {},
            "present_hash_mismatch_vi_reject_nonzero": 0,
            "present_hash_mismatch_vi_use_regs_zero": 0,
            "declared_vs_forensics_hash_drift_warnings": 0,
            "present_hash_mismatch_vi_hash_decode_match": 0,
            "present_hash_mismatch_vi_hash_filter_match": 0,
            "present_hash_mismatch_vi_hash_gdither_match": 0,
            "present_hash_mismatch_vi_hash_decode_mismatch": 0,
            "present_hash_mismatch_vi_hash_filter_mismatch": 0,
            "present_hash_mismatch_vi_hash_gdither_mismatch": 0,
            "present_hash_mismatch_selected_surface_hash_match": 0,
            "present_hash_mismatch_selected_surface_hash_mismatch": 0,
        }

    frames = replay.get("frames", [])
    if not isinstance(frames, list):
        frames = []

    mismatch_frame_ids: List[int] = []
    drift_warning_count = 0
    vi_hash_decode_match = 0
    vi_hash_filter_match = 0
    vi_hash_gdither_match = 0
    vi_hash_decode_mismatch = 0
    vi_hash_filter_mismatch = 0
    vi_hash_gdither_mismatch = 0
    selected_surface_hash_match = 0
    selected_surface_hash_mismatch = 0
    for frame in frames:
        if not isinstance(frame, dict):
            continue
        frame_id_raw = frame.get("frame_id")
        frame_id = int(frame_id_raw) if isinstance(frame_id_raw, int) else -1
        errors = frame.get("errors", [])
        warnings = frame.get("warnings", [])
        has_present_mismatch = False
        if isinstance(errors, list):
            for message in errors:
                if isinstance(message, str) and message.startswith("executor_present_hash mismatch:"):
                    has_present_mismatch = True
                    break
        if has_present_mismatch and frame_id >= 0:
            mismatch_frame_ids.append(frame_id)
            forensics_surface_hash = frame.get("forensics_selected_surface_hash")
            computed_surface_hash = frame.get("computed_selected_surface_hash")
            if (
                isinstance(forensics_surface_hash, int)
                and isinstance(computed_surface_hash, int)
                and forensics_surface_hash >= 0
                and computed_surface_hash >= 0
            ):
                if forensics_surface_hash == computed_surface_hash:
                    selected_surface_hash_match += 1
                else:
                    selected_surface_hash_mismatch += 1
            for suffix in ("decode", "filter", "gdither"):
                forensics_key = f"forensics_vi_hash_{suffix}"
                computed_key = f"computed_vi_hash_{suffix}"
                forensics_hash = frame.get(forensics_key)
                computed_hash = frame.get(computed_key)
                if (
                    not isinstance(forensics_hash, int)
                    or not isinstance(computed_hash, int)
                    or forensics_hash < 0
                    or computed_hash < 0
                ):
                    continue
                if forensics_hash == computed_hash:
                    if suffix == "decode":
                        vi_hash_decode_match += 1
                    elif suffix == "filter":
                        vi_hash_filter_match += 1
                    else:
                        vi_hash_gdither_match += 1
                else:
                    if suffix == "decode":
                        vi_hash_decode_mismatch += 1
                    elif suffix == "filter":
                        vi_hash_filter_mismatch += 1
                    else:
                        vi_hash_gdither_mismatch += 1
        if isinstance(warnings, list):
            for message in warnings:
                if isinstance(message, str) and message.startswith(
                    "declared_executor_present_hash differs from frame-forensics present hash:"
                ):
                    drift_warning_count += 1
                    break

    by_select: Dict[str, int] = {}
    mismatch_with_forensics = 0
    vi_reject_nonzero = 0
    vi_use_regs_zero = 0
    missing_forensics = 0
    for frame_id in mismatch_frame_ids:
        rec = records_by_frame.get(frame_id)
        if rec is None:
            missing_forensics += 1
            continue
        mismatch_with_forensics += 1
        select_key = f"s{_u64(rec, 'present_select')}"
        by_select[select_key] = by_select.get(select_key, 0) + 1
        if _u64(rec, "vi_reject") != 0:
            vi_reject_nonzero += 1
        if _u64(rec, "vi_use_regs") == 0:
            vi_use_regs_zero += 1

    return {
        "present_hash_mismatch_frames": len(mismatch_frame_ids),
        "present_hash_mismatch_with_forensics": mismatch_with_forensics,
        "missing_forensics_for_present_mismatch": missing_forensics,
        "present_hash_mismatch_by_select": dict(sorted(by_select.items())),
        "present_hash_mismatch_vi_reject_nonzero": vi_reject_nonzero,
        "present_hash_mismatch_vi_use_regs_zero": vi_use_regs_zero,
        "declared_vs_forensics_hash_drift_warnings": drift_warning_count,
        "present_hash_mismatch_vi_hash_decode_match": vi_hash_decode_match,
        "present_hash_mismatch_vi_hash_filter_match": vi_hash_filter_match,
        "present_hash_mismatch_vi_hash_gdither_match": vi_hash_gdither_match,
        "present_hash_mismatch_vi_hash_decode_mismatch": vi_hash_decode_mismatch,
        "present_hash_mismatch_vi_hash_filter_mismatch": vi_hash_filter_mismatch,
        "present_hash_mismatch_vi_hash_gdither_mismatch": vi_hash_gdither_mismatch,
        "present_hash_mismatch_selected_surface_hash_match": selected_surface_hash_match,
        "present_hash_mismatch_selected_surface_hash_mismatch": selected_surface_hash_mismatch,
    }


def _selected_forensics_fields(record: Dict[str, Any]) -> Dict[str, Any]:
    keys = [
        "frame",
        "work",
        "writes",
        "present_hash",
        "present_w",
        "present_h",
        "present_surface",
        "present_select",
        "selected_surface_writes",
        "selected_surface_works",
        "selected_surface_size",
        "selected_surface_w",
        "selected_surface_h",
        "selected_surface_hash",
        "selected_surface_live_writes",
        "selected_surface_live_works",
        "selected_surface_from_history",
        "vi_valid",
        "vi_origin",
        "vi_status",
        "vi_width",
        "vi_vcurrent",
        "vi_vsync",
        "vi_hstart",
        "vi_vstart",
        "vi_xscale",
        "vi_yscale",
        "vi_origin_match",
        "vi_reject",
        "vi_type",
        "vi_use_regs",
        "vi_src_w",
        "vi_src_h",
        "vi_out_w",
        "vi_out_h",
        "vi_stride",
        "vi_hash_decode",
        "vi_hash_filter",
        "vi_hash_gdither",
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
        "ci_switches",
        "ci_first",
        "ci_last",
        "tri_deg_reject",
        "tri_bounds_reject",
        "tri_scissor_reject",
        "tri_samples",
        "tri_alpha_reject",
        "tri_cvg_reject",
        "tri_depth_reject",
        "tri_nonblack",
        "texrect_nonblack",
        "tri_luma_sum",
        "texrect_luma_sum",
        "depth_eval",
        "depth_reject",
        "depth_update",
    ]
    selected = {key: record.get(key) for key in keys if key in record}
    for i in range(4):
        for key in (f"s{i}_addr", f"s{i}_writes", f"s{i}_works", f"s{i}_hash"):
            if key in record:
                selected[key] = record.get(key)
    for i in range(8):
        for key in (f"ci_evt{i}_addr", f"ci_evt{i}_work"):
            if key in record:
                selected[key] = record.get(key)
    return selected


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
    parser.add_argument("--diff-playbook-summary")
    parser.add_argument("--diff-playbook-boxes")
    parser.add_argument("--diff-playbook-snippet")
    parser.add_argument("--command-census")
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
    diff_playbook_summary_path = Path(args.diff_playbook_summary) if args.diff_playbook_summary else None
    diff_playbook_boxes_path = Path(args.diff_playbook_boxes) if args.diff_playbook_boxes else None
    diff_playbook_snippet_path = Path(args.diff_playbook_snippet) if args.diff_playbook_snippet else None
    command_census_path = Path(args.command_census) if args.command_census else None

    metrics = _load_json(metrics_path)
    capture_context = _load_json(capture_context_path)
    replay = _load_json(packet_replay)
    depth_summary = _load_json(depth_summary_path)
    diff_playbook_summary = _load_json(diff_playbook_summary_path)
    diff_playbook_boxes = _load_json_any(diff_playbook_boxes_path)
    diff_playbook_snippet = _load_text(diff_playbook_snippet_path)
    command_census = _load_json(command_census_path)

    forensics_data = _parse_forensics(forensics)
    replay_summary = _summarize_replay(replay)
    launch_summary = _parse_launch_log(launch_log)
    forensics_records = forensics_data.get("records", [])
    if not isinstance(forensics_records, list):
        forensics_records = []
    forensics_records_by_frame = forensics_data.get("records_by_frame", {})
    if not isinstance(forensics_records_by_frame, dict):
        forensics_records_by_frame = {}
    forensics_rollup = _summarize_forensics_records(forensics_records)
    replay_forensics_correlation = _build_replay_forensics_correlation(
        replay,
        forensics_records_by_frame,
    )
    signal_summary = _build_signals(
        forensics_data.get("last_record", {}),
        replay_summary,
        launch_summary,
        depth_summary,
        metrics,
        command_census,
        diff_playbook_summary,
    )

    payload = {
        "schema": "rvk2_pm_telemetry_bundle_v1",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "scenario_id": args.scenario_id,
        "status": {
            "packet_replay_exit": args.packet_replay_exit,
            "forensics_summary_exit": args.forensics_summary_exit,
            "forensics_summary_active_exit": args.forensics_summary_active_exit,
            "hard_fault_count": len(signal_summary.get("hard_faults", [])),
            "has_hard_faults": bool(signal_summary.get("hard_faults")),
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
            "diff_playbook_summary": _file_meta(diff_playbook_summary_path),
            "diff_playbook_boxes": _file_meta(diff_playbook_boxes_path),
            "diff_playbook_snippet": _file_meta(diff_playbook_snippet_path),
            "command_census": _file_meta(command_census_path),
        },
        "metrics": metrics,
        "capture_context": capture_context,
        "deviation_playbook": {
            "summary": diff_playbook_summary,
            "boxes": diff_playbook_boxes if isinstance(diff_playbook_boxes, list) else [],
            "snippet": diff_playbook_snippet,
        },
        "command_census": command_census,
        "packet_replay_summary": replay_summary,
        "forensics": {
            "record_count": int(forensics_data.get("record_count", 0) or 0),
            "last_frame_selected_fields": _selected_forensics_fields(forensics_data.get("last_record", {})),
            "summary": forensics_rollup,
        },
        "replay_forensics_correlation": replay_forensics_correlation,
        "launch_log_summary": launch_summary,
        "signals": {
            "present": signal_summary["present"],
            "texture": signal_summary["texture"],
            "geometry": signal_summary["geometry"],
            "depth": signal_summary["depth"],
            "visibility": signal_summary["visibility"],
            "command": signal_summary["command"],
            "deviation": signal_summary["deviation"],
        },
        "suspected_gaps": signal_summary["suspected_gaps"],
        "hard_faults": signal_summary.get("hard_faults", []),
    }

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
