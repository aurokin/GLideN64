#!/usr/bin/env python3
from __future__ import annotations

import argparse
import importlib.util
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

try:
    import numpy as np
except Exception:  # pragma: no cover - optional dependency
    np = None

try:
    from PIL import Image
except Exception:  # pragma: no cover - optional dependency
    Image = None


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


def _compare_executor_present_to_candidate(
    candidate_capture_path: Optional[Path],
    executor_present_dump_path: Optional[Path],
) -> Dict[str, Any]:
    result: Dict[str, Any] = {
        "available": False,
        "candidate_path": str(candidate_capture_path) if candidate_capture_path is not None else None,
        "executor_present_path": str(executor_present_dump_path) if executor_present_dump_path is not None else None,
    }
    if candidate_capture_path is None or not candidate_capture_path.is_file():
        result["reason"] = "candidate_capture_missing"
        return result
    if executor_present_dump_path is None or not executor_present_dump_path.is_file():
        result["reason"] = "executor_present_dump_missing"
        return result
    if Image is None or np is None:
        result["reason"] = "image_dependencies_unavailable"
        return result

    try:
        candidate_image = Image.open(candidate_capture_path).convert("RGB")
        executor_image = Image.open(executor_present_dump_path).convert("RGB")
    except Exception as exc:  # pragma: no cover - decode guard
        result["reason"] = f"image_decode_failed: {exc}"
        return result

    candidate_resized = candidate_image.resize(executor_image.size, Image.NEAREST)
    candidate_array = np.asarray(candidate_resized, dtype=np.float32) / 255.0
    executor_array = np.asarray(executor_image, dtype=np.float32) / 255.0

    def _metric_pair(lhs: Any, rhs: Any) -> Dict[str, float]:
        diff = lhs - rhs
        rmse = float(np.sqrt(np.mean(np.square(diff), dtype=np.float64)))
        mae = float(np.mean(np.abs(diff), dtype=np.float64))
        return {"rmse": rmse, "mae": mae}

    variants = {
        "direct": candidate_array,
        "flip_y": np.flip(candidate_array, axis=0),
        "swap_rb": candidate_array[..., [2, 1, 0]],
    }
    variants["swap_rb_flip_y"] = np.flip(variants["swap_rb"], axis=0)

    variant_metrics: Dict[str, Dict[str, float]] = {}
    best_variant = "direct"
    best_rmse: Optional[float] = None
    for name, data in variants.items():
        metric = _metric_pair(data, executor_array)
        variant_metrics[name] = metric
        rmse = metric["rmse"]
        if best_rmse is None or rmse < best_rmse:
            best_rmse = rmse
            best_variant = name

    direct_metric = variant_metrics.get("direct", {})
    result.update(
        {
            "available": True,
            "candidate_size": [int(candidate_image.width), int(candidate_image.height)],
            "executor_present_size": [int(executor_image.width), int(executor_image.height)],
            "resized_to_executor_present": True,
            "resize_filter": "nearest",
            "rmse": direct_metric.get("rmse"),
            "mae": direct_metric.get("mae"),
            "variant_metrics": variant_metrics,
            "best_variant": best_variant,
            "best_rmse": best_rmse,
            "best_mae": variant_metrics.get(best_variant, {}).get("mae"),
        }
    )
    return result


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


def _decode_combiner_cycle_selectors(combine_mux: int, cycle2: bool) -> Dict[str, int]:
    mode0 = (combine_mux >> 32) & 0xFFFFFFFF
    mode1 = combine_mux & 0xFFFFFFFF
    if cycle2:
        return {
            "color_a": (mode0 >> 5) & 0xF,
            "color_b": (mode1 >> 24) & 0xF,
            "color_c": mode0 & 0x1F,
            "color_d": (mode1 >> 6) & 0x7,
            "alpha_a": (mode1 >> 21) & 0x7,
            "alpha_b": (mode1 >> 3) & 0x7,
            "alpha_c": (mode1 >> 18) & 0x7,
            "alpha_d": mode1 & 0x7,
        }
    return {
        "color_a": (mode0 >> 20) & 0xF,
        "color_b": (mode1 >> 28) & 0xF,
        "color_c": (mode0 >> 15) & 0x1F,
        "color_d": (mode1 >> 15) & 0x7,
        "alpha_a": (mode0 >> 12) & 0x7,
        "alpha_b": (mode1 >> 12) & 0x7,
        "alpha_c": (mode0 >> 9) & 0x7,
        "alpha_d": (mode1 >> 9) & 0x7,
    }


def _decode_blend_selectors(other_modes: int, cycle2: bool) -> Dict[str, int]:
    mode1 = other_modes & 0xFFFFFFFF
    if cycle2:
        return {
            "m1a": (mode1 >> 28) & 0x3,
            "m1b": (mode1 >> 24) & 0x3,
            "m2a": (mode1 >> 20) & 0x3,
            "m2b": (mode1 >> 16) & 0x3,
        }
    return {
        "m1a": (mode1 >> 30) & 0x3,
        "m1b": (mode1 >> 26) & 0x3,
        "m2a": (mode1 >> 22) & 0x3,
        "m2b": (mode1 >> 18) & 0x3,
    }


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


def _parse_history_merge_log(path: Optional[Path]) -> Dict[str, Any]:
    if path is None or not path.is_file():
        return {
            "record_count": 0,
            "frame_count": 0,
            "present_surface_count": 0,
            "candidate_surface_count": 0,
            "total_potential_black_fill": 0,
            "total_potential_nonblack_diff": 0,
            "total_copied": 0,
            "records_with_potential_black_fill": 0,
            "records_with_potential_nonblack_diff": 0,
            "records_with_copied": 0,
            "max_potential_black_fill": 0,
            "max_potential_nonblack_diff": 0,
            "max_copied": 0,
            "top_nonblack_diff_record": {},
            "top_black_fill_record": {},
            "top_copied_record": {},
        }

    records: List[Dict[str, Any]] = []
    unique_frames: set[int] = set()
    unique_present_surfaces: set[int] = set()
    unique_candidate_surfaces: set[int] = set()

    total_potential_black_fill = 0
    total_potential_nonblack_diff = 0
    total_copied = 0
    records_with_potential_black_fill = 0
    records_with_potential_nonblack_diff = 0
    records_with_copied = 0

    max_potential_black_fill = 0
    max_potential_nonblack_diff = 0
    max_copied = 0
    top_nonblack_diff_record: Dict[str, Any] = {}
    top_black_fill_record: Dict[str, Any] = {}
    top_copied_record: Dict[str, Any] = {}

    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        record: Dict[str, Any] = {}
        for token in stripped.split("\t"):
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

        frame_value = _u64(record, "frame")
        if frame_value > 0:
            unique_frames.add(frame_value)
        present_surface = _u64(record, "present")
        if present_surface > 0:
            unique_present_surfaces.add(present_surface)
        candidate_surface = _u64(record, "candidate")
        if candidate_surface > 0:
            unique_candidate_surfaces.add(candidate_surface)

        potential_black_fill = _u64(record, "potential_black_fill")
        potential_nonblack_diff = _u64(record, "potential_nonblack_diff")
        copied = _u64(record, "copied")
        total_potential_black_fill += potential_black_fill
        total_potential_nonblack_diff += potential_nonblack_diff
        total_copied += copied
        if potential_black_fill > 0:
            records_with_potential_black_fill += 1
        if potential_nonblack_diff > 0:
            records_with_potential_nonblack_diff += 1
        if copied > 0:
            records_with_copied += 1
        if potential_black_fill > max_potential_black_fill:
            max_potential_black_fill = potential_black_fill
            top_black_fill_record = record
        if potential_nonblack_diff > max_potential_nonblack_diff:
            max_potential_nonblack_diff = potential_nonblack_diff
            top_nonblack_diff_record = record
        if copied > max_copied:
            max_copied = copied
            top_copied_record = record

    return {
        "record_count": len(records),
        "frame_count": len(unique_frames),
        "present_surface_count": len(unique_present_surfaces),
        "candidate_surface_count": len(unique_candidate_surfaces),
        "total_potential_black_fill": total_potential_black_fill,
        "total_potential_nonblack_diff": total_potential_nonblack_diff,
        "total_copied": total_copied,
        "records_with_potential_black_fill": records_with_potential_black_fill,
        "records_with_potential_nonblack_diff": records_with_potential_nonblack_diff,
        "records_with_copied": records_with_copied,
        "max_potential_black_fill": max_potential_black_fill,
        "max_potential_nonblack_diff": max_potential_nonblack_diff,
        "max_copied": max_copied,
        "top_nonblack_diff_record": top_nonblack_diff_record,
        "top_black_fill_record": top_black_fill_record,
        "top_copied_record": top_copied_record,
    }


def _parse_overwrite_log(path: Optional[Path]) -> Dict[str, Any]:
    if path is None or not path.is_file():
        return {
            "record_count": 0,
            "black_write_record_count": 0,
            "overwrite_record_count": 0,
            "non_overwrite_black_write_count": 0,
            "preserved_count": 0,
            "preserved_ratio": None,
            "op_counts": {},
            "overwrite_op_counts": {},
            "black_write_op_counts": {},
            "texture_source_bit_counts": {},
            "overwrite_texture_source_bit_counts": {},
            "black_write_texture_source_bit_counts": {},
            "unique_color_image_count": 0,
            "black_write_unique_color_image_count": 0,
            "top_color_images": [],
            "overwrite_top_color_images": [],
            "black_write_top_color_images": [],
            "top_source_packets": [],
            "overwrite_top_source_packets": [],
            "black_write_top_source_packets": [],
            "top_combiner_kill_source_packets": [],
            "overwrite_top_combiner_kill_source_packets": [],
            "black_write_top_combiner_kill_source_packets": [],
            "top_source_packet_profiles": [],
            "dominant_state": {},
            "dominant_state_ratio": None,
            "overwrite_dominant_state": {},
            "overwrite_dominant_state_ratio": None,
            "black_write_dominant_state": {},
            "black_write_dominant_state_ratio": None,
            "stage_black": {},
            "stage_kill_counts": {},
            "overwrite_stage_black": {},
            "overwrite_stage_kill_counts": {},
            "black_write_stage_black": {},
            "black_write_stage_kill_counts": {},
            "texel_detail": {"available": False, "slot_summaries": {}},
            "source_packet_stage_profiles": [],
            "source_packet_stage_profile_count": 0,
            "source_packet_stage_profile_truncated_count": 0,
        }

    records: List[Dict[str, Any]] = []
    overwrite_op_counts: Dict[str, int] = {}
    black_write_op_counts: Dict[str, int] = {}
    overwrite_texture_source_bit_counts: Dict[str, int] = {}
    black_write_texture_source_bit_counts: Dict[str, int] = {}
    overwrite_color_image_counts: Dict[int, int] = {}
    black_write_color_image_counts: Dict[int, int] = {}
    overwrite_source_packet_counts: Dict[int, int] = {}
    black_write_source_packet_counts: Dict[int, int] = {}
    overwrite_combiner_kill_source_packet_counts: Dict[int, int] = {}
    black_write_combiner_kill_source_packet_counts: Dict[int, int] = {}
    source_packet_stage_stats: Dict[int, Dict[str, Any]] = {}
    overwrite_state_counts: Dict[str, int] = {}
    overwrite_state_rows: Dict[str, Dict[str, Any]] = {}
    black_write_state_counts: Dict[str, int] = {}
    black_write_state_rows: Dict[str, Dict[str, Any]] = {}
    overwrite_record_count = 0
    preserved_count = 0
    overwrite_texel_black_count = 0
    overwrite_combiner_black_count = 0
    overwrite_blender_black_count = 0
    overwrite_final_black_count = 0
    overwrite_kill_at_combiner_count = 0
    overwrite_kill_at_blender_count = 0
    overwrite_kill_after_blender_count = 0
    black_write_texel_black_count = 0
    black_write_combiner_black_count = 0
    black_write_blender_black_count = 0
    black_write_final_black_count = 0
    black_write_kill_at_combiner_count = 0
    black_write_kill_at_blender_count = 0
    black_write_kill_after_blender_count = 0
    texel_slot_names = ("tex0", "tex1", "tex0_next")
    texel_source_names = {
        0: "none",
        1: "replacement",
        2: "tmem",
        3: "rdram",
        4: "synthetic",
    }
    texel_fetch_variant_names = {
        0: "none",
        1: "tmem4",
        2: "tmem8",
        3: "tmem16",
        4: "tmem32_split",
        5: "tmem32_direct",
        6: "tmem32_canonical",
        7: "rdram4",
        8: "rdram8",
        9: "rdram16",
        10: "rdram32",
    }

    def _bucket_bump(counter: Dict[str, int], key: str) -> None:
        counter[key] = counter.get(key, 0) + 1

    def _source_label(value: int) -> str:
        return texel_source_names.get(value, f"source_{value}")

    def _fetch_label(value: int) -> str:
        return texel_fetch_variant_names.get(value, f"variant_{value}")

    texel_slot_summary_counts: Dict[str, Dict[str, Any]] = {
        slot: {
            "valid_record_count": 0,
            "source_kind_counts": {},
            "tmem_fetch_variant_counts": {},
            "tmem_reject_counts": {},
            "rdram_reject_counts": {},
            "source_bits_counts": {},
            "sampled_black_count": 0,
            "final_black_count": 0,
            "tmem_raw_zero_count": 0,
            "rdram_probe_valid_count": 0,
            "rdram_probe_non_black_count": 0,
            "rdram_probe_beats_tmem_count": 0,
            "rdram_probe_reject_counts": {},
        }
        for slot in texel_slot_names
    }
    texel_slot_tmem_index_counts: Dict[str, Dict[str, int]] = {slot: {} for slot in texel_slot_names}

    def _packet_stage_row(packet_id: int) -> Dict[str, Any]:
        row = source_packet_stage_stats.get(packet_id)
        if row is None:
            row = {
                "source_packet_id": int(packet_id),
                "record_count": 0,
                "overwrite_count": 0,
                "final_black_count": 0,
                "texel_black_count": 0,
                "combiner_black_count": 0,
                "blender_black_count": 0,
                "kill_at_combiner_count": 0,
                "kill_at_blender_count": 0,
                "kill_after_blender_count": 0,
                "tex0_valid_count": 0,
                "tex0_sample_black_count": 0,
                "tex0_final_black_count": 0,
                "tex0_rdram_probe_valid_count": 0,
                "tex0_rdram_probe_non_black_count": 0,
                "tex0_rdram_probe_beats_tmem_count": 0,
                "op_counts": {},
                "phase_counts": {},
            }
            source_packet_stage_stats[packet_id] = row
        return row

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        record: Dict[str, Any] = {}
        for token in stripped.split("\t"):
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
        overwrite_to_black_field = record.get("overwrite_to_black")
        overwrite_to_black = (
            _u64(record, "overwrite_to_black") != 0
            if overwrite_to_black_field is not None
            else True
        )
        if overwrite_to_black:
            overwrite_record_count += 1

        op_name_raw = record.get("op_name")
        op_name = str(op_name_raw).strip() if isinstance(op_name_raw, str) else ""
        if not op_name:
            op_kind = _u64(record, "op_kind")
            if op_kind == 1:
                op_name = "triangle"
            elif op_kind == 2:
                op_name = "texrect"
            elif op_kind == 3:
                op_name = "fill"
            else:
                op_name = "other"
        black_write_op_counts[op_name] = black_write_op_counts.get(op_name, 0) + 1
        if overwrite_to_black:
            overwrite_op_counts[op_name] = overwrite_op_counts.get(op_name, 0) + 1

        if overwrite_to_black and _u64(record, "preserved") != 0:
            preserved_count += 1

        source_packet_id = _u64(record, "source_packet_id")
        if source_packet_id > 0:
            black_write_source_packet_counts[source_packet_id] = (
                black_write_source_packet_counts.get(source_packet_id, 0) + 1
            )
            if overwrite_to_black:
                overwrite_source_packet_counts[source_packet_id] = (
                    overwrite_source_packet_counts.get(source_packet_id, 0) + 1
                )

        texture_source_bits = _u64(record, "texture_source_bits")
        source_key = f"0x{texture_source_bits:08X}"
        black_write_texture_source_bit_counts[source_key] = (
            black_write_texture_source_bit_counts.get(source_key, 0) + 1
        )
        if overwrite_to_black:
            overwrite_texture_source_bit_counts[source_key] = (
                overwrite_texture_source_bit_counts.get(source_key, 0) + 1
            )

        texel = _u64(record, "texel")
        combiner = _u64(record, "combiner")
        blender = _u64(record, "blender")
        final = _u64(record, "final")
        texel_black = (texel & 0x00FFFFFF) == 0
        combiner_black = (combiner & 0x00FFFFFF) == 0
        blender_black = (blender & 0x00FFFFFF) == 0
        final_black = (final & 0x00FFFFFF) == 0
        if texel_black:
            black_write_texel_black_count += 1
        if combiner_black:
            black_write_combiner_black_count += 1
        if blender_black:
            black_write_blender_black_count += 1
        if final_black:
            black_write_final_black_count += 1
        if not texel_black and combiner_black:
            black_write_kill_at_combiner_count += 1
            if source_packet_id > 0:
                black_write_combiner_kill_source_packet_counts[source_packet_id] = (
                    black_write_combiner_kill_source_packet_counts.get(source_packet_id, 0) + 1
                )
        if not combiner_black and blender_black:
            black_write_kill_at_blender_count += 1
        if not blender_black and final_black:
            black_write_kill_after_blender_count += 1

        if source_packet_id > 0:
            packet_stage = _packet_stage_row(source_packet_id)
            packet_stage["record_count"] = int(packet_stage.get("record_count", 0) or 0) + 1
            if overwrite_to_black:
                packet_stage["overwrite_count"] = int(packet_stage.get("overwrite_count", 0) or 0) + 1
            if final_black:
                packet_stage["final_black_count"] = int(packet_stage.get("final_black_count", 0) or 0) + 1
            if texel_black:
                packet_stage["texel_black_count"] = int(packet_stage.get("texel_black_count", 0) or 0) + 1
            if combiner_black:
                packet_stage["combiner_black_count"] = int(packet_stage.get("combiner_black_count", 0) or 0) + 1
            if blender_black:
                packet_stage["blender_black_count"] = int(packet_stage.get("blender_black_count", 0) or 0) + 1
            if not texel_black and combiner_black:
                packet_stage["kill_at_combiner_count"] = int(packet_stage.get("kill_at_combiner_count", 0) or 0) + 1
            if not combiner_black and blender_black:
                packet_stage["kill_at_blender_count"] = int(packet_stage.get("kill_at_blender_count", 0) or 0) + 1
            if not blender_black and final_black:
                packet_stage["kill_after_blender_count"] = int(packet_stage.get("kill_after_blender_count", 0) or 0) + 1
            phase = _u64(record, "phase")
            op_counts = packet_stage.get("op_counts")
            if isinstance(op_counts, dict):
                _bucket_bump(op_counts, op_name)
            phase_counts = packet_stage.get("phase_counts")
            if isinstance(phase_counts, dict):
                _bucket_bump(phase_counts, str(int(phase)))

        if overwrite_to_black:
            if texel_black:
                overwrite_texel_black_count += 1
            if combiner_black:
                overwrite_combiner_black_count += 1
            if blender_black:
                overwrite_blender_black_count += 1
            if final_black:
                overwrite_final_black_count += 1
            if not texel_black and combiner_black:
                overwrite_kill_at_combiner_count += 1
                if source_packet_id > 0:
                    overwrite_combiner_kill_source_packet_counts[source_packet_id] = (
                        overwrite_combiner_kill_source_packet_counts.get(source_packet_id, 0) + 1
                    )
            if not combiner_black and blender_black:
                overwrite_kill_at_blender_count += 1
            if not blender_black and final_black:
                overwrite_kill_after_blender_count += 1

        color_image = _u64(record, "color_image")
        if color_image > 0:
            black_write_color_image_counts[color_image] = black_write_color_image_counts.get(color_image, 0) + 1
            if overwrite_to_black:
                overwrite_color_image_counts[color_image] = (
                    overwrite_color_image_counts.get(color_image, 0) + 1
                )

        combine = _u64(record, "combine_mux")
        other_modes = _u64(record, "other_modes")
        blend = _u64(record, "blend_params")
        tile_line = _u64(record, "tile_line")
        texture_width = _u64(record, "texture_image_width")
        state_key = (
            f"{op_name}|{combine:016X}|{other_modes:016X}|{blend:08X}|"
            f"{tile_line}|{texture_width}"
        )
        black_write_state_counts[state_key] = black_write_state_counts.get(state_key, 0) + 1
        if state_key not in black_write_state_rows:
            black_write_state_rows[state_key] = {
                "op_name": op_name,
                "combine_mux": f"0x{combine:016X}",
                "other_modes": f"0x{other_modes:016X}",
                "blend_params": f"0x{blend:08X}",
                "tile_line": tile_line,
                "texture_image_width": texture_width,
            }
        if overwrite_to_black:
            overwrite_state_counts[state_key] = overwrite_state_counts.get(state_key, 0) + 1
            if state_key not in overwrite_state_rows:
                overwrite_state_rows[state_key] = {
                    "op_name": op_name,
                    "combine_mux": f"0x{combine:016X}",
                    "other_modes": f"0x{other_modes:016X}",
                    "blend_params": f"0x{blend:08X}",
                    "tile_line": tile_line,
                    "texture_image_width": texture_width,
                }

        for slot in texel_slot_names:
            if _u64(record, f"{slot}_valid") == 0:
                continue
            slot_summary = texel_slot_summary_counts[slot]
            slot_summary["valid_record_count"] = int(slot_summary["valid_record_count"]) + 1
            source_kind = int(_u64(record, f"{slot}_source_kind"))
            source_label = _source_label(source_kind)
            source_kind_counts = slot_summary["source_kind_counts"]
            if isinstance(source_kind_counts, dict):
                _bucket_bump(source_kind_counts, source_label)
            fetch_variant = int(_u64(record, f"{slot}_tmem_fetch_variant"))
            fetch_label = _fetch_label(fetch_variant)
            fetch_variant_counts = slot_summary["tmem_fetch_variant_counts"]
            if isinstance(fetch_variant_counts, dict):
                _bucket_bump(fetch_variant_counts, fetch_label)
            tmem_reject = int(_u64(record, f"{slot}_tmem_reject"))
            tmem_reject_counts = slot_summary["tmem_reject_counts"]
            if isinstance(tmem_reject_counts, dict):
                _bucket_bump(tmem_reject_counts, str(tmem_reject))
            rdram_reject = int(_u64(record, f"{slot}_rdram_reject"))
            rdram_reject_counts = slot_summary["rdram_reject_counts"]
            if isinstance(rdram_reject_counts, dict):
                _bucket_bump(rdram_reject_counts, str(rdram_reject))
            source_bits_key = f"0x{_u64(record, f'{slot}_source_bits'):08X}"
            source_bits_counts = slot_summary["source_bits_counts"]
            if isinstance(source_bits_counts, dict):
                _bucket_bump(source_bits_counts, source_bits_key)

            sampled_color = int(_u64(record, f"{slot}_sampled_color"))
            final_slot_color = int(_u64(record, f"{slot}_final_color"))
            sampled_black = (sampled_color & 0x00FFFFFF) == 0
            if (sampled_color & 0x00FFFFFF) == 0:
                slot_summary["sampled_black_count"] = int(slot_summary["sampled_black_count"]) + 1
            if (final_slot_color & 0x00FFFFFF) == 0:
                slot_summary["final_black_count"] = int(slot_summary["final_black_count"]) + 1

            rdram_probe_reject = int(_u64(record, f"{slot}_rdram_probe_reject"))
            probe_reject_counts = slot_summary.get("rdram_probe_reject_counts")
            if isinstance(probe_reject_counts, dict):
                _bucket_bump(probe_reject_counts, str(rdram_probe_reject))
            if int(_u64(record, f"{slot}_rdram_probe_valid")) != 0:
                slot_summary["rdram_probe_valid_count"] = int(slot_summary.get("rdram_probe_valid_count", 0) or 0) + 1
                probe_final_color = int(_u64(record, f"{slot}_rdram_probe_final_color"))
                probe_non_black = (probe_final_color & 0x00FFFFFF) != 0
                if probe_non_black:
                    slot_summary["rdram_probe_non_black_count"] = int(slot_summary.get("rdram_probe_non_black_count", 0) or 0) + 1
                if sampled_black and probe_non_black:
                    slot_summary["rdram_probe_beats_tmem_count"] = int(slot_summary.get("rdram_probe_beats_tmem_count", 0) or 0) + 1

            if source_kind == 2:
                raw_a = int(_u64(record, f"{slot}_tmem_raw_a"))
                raw_b = int(_u64(record, f"{slot}_tmem_raw_b"))
                if raw_a == 0 and raw_b == 0:
                    slot_summary["tmem_raw_zero_count"] = int(slot_summary["tmem_raw_zero_count"]) + 1
                idx_a = int(_u64(record, f"{slot}_tmem_index_a"))
                idx_b = int(_u64(record, f"{slot}_tmem_index_b"))
                idx_key = f"{idx_a:04X}|{idx_b:04X}"
                _bucket_bump(texel_slot_tmem_index_counts[slot], idx_key)

            if slot == "tex0" and source_packet_id > 0:
                packet_stage = _packet_stage_row(source_packet_id)
                packet_stage["tex0_valid_count"] = int(packet_stage.get("tex0_valid_count", 0) or 0) + 1
                if sampled_black:
                    packet_stage["tex0_sample_black_count"] = int(
                        packet_stage.get("tex0_sample_black_count", 0) or 0
                    ) + 1
                if (final_slot_color & 0x00FFFFFF) == 0:
                    packet_stage["tex0_final_black_count"] = int(
                        packet_stage.get("tex0_final_black_count", 0) or 0
                    ) + 1
                if int(_u64(record, "tex0_rdram_probe_valid")) != 0:
                    packet_stage["tex0_rdram_probe_valid_count"] = int(
                        packet_stage.get("tex0_rdram_probe_valid_count", 0) or 0
                    ) + 1
                    probe_non_black = (int(_u64(record, "tex0_rdram_probe_final_color")) & 0x00FFFFFF) != 0
                    if probe_non_black:
                        packet_stage["tex0_rdram_probe_non_black_count"] = int(
                            packet_stage.get("tex0_rdram_probe_non_black_count", 0) or 0
                        ) + 1
                        if sampled_black:
                            packet_stage["tex0_rdram_probe_beats_tmem_count"] = int(
                                packet_stage.get("tex0_rdram_probe_beats_tmem_count", 0) or 0
                            ) + 1

    record_count = len(records)
    non_overwrite_black_write_count = max(0, record_count - overwrite_record_count)

    def _build_top_color_rows(counts: Dict[int, int], denominator: int) -> List[Dict[str, Any]]:
        rows: List[Dict[str, Any]] = []
        ordered = sorted(counts.items(), key=lambda item: item[1], reverse=True)
        for address, count in ordered[:8]:
            rows.append(
                {
                    "color_image_address": int(address),
                    "color_image_address_hex": f"0x{int(address):08X}",
                    "count": int(count),
                    "ratio": _ratio(int(count), denominator),
                }
            )
        return rows

    def _build_top_source_rows(counts: Dict[int, int], denominator: int) -> List[Dict[str, Any]]:
        rows: List[Dict[str, Any]] = []
        ordered = sorted(counts.items(), key=lambda item: item[1], reverse=True)
        for source_packet_id, count in ordered[:16]:
            rows.append(
                {
                    "source_packet_id": int(source_packet_id),
                    "count": int(count),
                    "ratio": _ratio(int(count), denominator),
                }
            )
        return rows

    def _build_top_combiner_kill_rows(
        counts: Dict[int, int],
        combiner_kill_total: int,
        denominator: int,
        record_ratio_key: str,
    ) -> List[Dict[str, Any]]:
        rows: List[Dict[str, Any]] = []
        ordered = sorted(counts.items(), key=lambda item: item[1], reverse=True)
        for source_packet_id, count in ordered[:16]:
            row = {
                "source_packet_id": int(source_packet_id),
                "count": int(count),
                "ratio_of_combiner_kills": _ratio(int(count), combiner_kill_total),
            }
            row[record_ratio_key] = _ratio(int(count), denominator)
            rows.append(row)
        return rows

    def _dominant_state(
        counts: Dict[str, int],
        rows: Dict[str, Dict[str, Any]],
        denominator: int,
    ) -> Tuple[Dict[str, Any], Optional[float]]:
        if not counts:
            return {}, None
        state_key, state_count = max(counts.items(), key=lambda item: item[1])
        dominant = dict(rows.get(state_key, {}))
        dominant["count"] = int(state_count)
        dominant["ratio"] = _ratio(int(state_count), denominator)
        return dominant, dominant["ratio"]

    top_color_image_rows = _build_top_color_rows(overwrite_color_image_counts, overwrite_record_count)
    overwrite_top_source_packet_rows = _build_top_source_rows(overwrite_source_packet_counts, overwrite_record_count)
    top_combiner_kill_source_rows = _build_top_combiner_kill_rows(
        overwrite_combiner_kill_source_packet_counts,
        overwrite_kill_at_combiner_count,
        overwrite_record_count,
        "ratio_of_overwrite_records",
    )
    dominant_state, dominant_state_ratio = _dominant_state(
        overwrite_state_counts,
        overwrite_state_rows,
        overwrite_record_count,
    )

    black_write_top_color_image_rows = _build_top_color_rows(black_write_color_image_counts, record_count)
    black_write_top_source_packet_rows = _build_top_source_rows(black_write_source_packet_counts, record_count)
    black_write_top_combiner_kill_source_rows = _build_top_combiner_kill_rows(
        black_write_combiner_kill_source_packet_counts,
        black_write_kill_at_combiner_count,
        record_count,
        "ratio_of_black_write_records",
    )
    black_write_dominant_state, black_write_dominant_state_ratio = _dominant_state(
        black_write_state_counts,
        black_write_state_rows,
        record_count,
    )
    texel_slot_summaries: Dict[str, Any] = {}
    texel_detail_available = False
    for slot in texel_slot_names:
        slot_summary = texel_slot_summary_counts.get(slot, {})
        valid_count = int(slot_summary.get("valid_record_count", 0) or 0)
        source_kind_counts = slot_summary.get("source_kind_counts", {})
        tmem_fetch_variant_counts = slot_summary.get("tmem_fetch_variant_counts", {})
        tmem_reject_counts = slot_summary.get("tmem_reject_counts", {})
        rdram_reject_counts = slot_summary.get("rdram_reject_counts", {})
        source_bits_counts = slot_summary.get("source_bits_counts", {})
        sampled_black_count = int(slot_summary.get("sampled_black_count", 0) or 0)
        final_black_count = int(slot_summary.get("final_black_count", 0) or 0)
        tmem_raw_zero_count = int(slot_summary.get("tmem_raw_zero_count", 0) or 0)
        rdram_probe_valid_count = int(slot_summary.get("rdram_probe_valid_count", 0) or 0)
        rdram_probe_non_black_count = int(slot_summary.get("rdram_probe_non_black_count", 0) or 0)
        rdram_probe_beats_tmem_count = int(slot_summary.get("rdram_probe_beats_tmem_count", 0) or 0)
        rdram_probe_reject_counts = slot_summary.get("rdram_probe_reject_counts", {})
        tmem_index_counts = texel_slot_tmem_index_counts.get(slot, {})
        top_tmem_indices: List[Dict[str, Any]] = []
        if isinstance(tmem_index_counts, dict):
            ordered_indices = sorted(tmem_index_counts.items(), key=lambda item: item[1], reverse=True)
            for index_key, count in ordered_indices[:8]:
                parts = index_key.split("|", 1)
                idx_a = int(parts[0], 16) if len(parts) >= 1 and parts[0] else 0
                idx_b = int(parts[1], 16) if len(parts) >= 2 and parts[1] else 0
                top_tmem_indices.append(
                    {
                        "tmem_index_a": idx_a,
                        "tmem_index_b": idx_b,
                        "tmem_index_a_hex": f"0x{idx_a:04X}",
                        "tmem_index_b_hex": f"0x{idx_b:04X}",
                        "count": int(count),
                        "ratio": _ratio(int(count), valid_count),
                    }
                )
        if valid_count > 0:
            texel_detail_available = True
        texel_slot_summaries[slot] = {
            "valid_record_count": valid_count,
            "valid_ratio_of_black_writes": _ratio(valid_count, record_count),
            "source_kind_counts": (
                dict(sorted(source_kind_counts.items()))
                if isinstance(source_kind_counts, dict)
                else {}
            ),
            "tmem_fetch_variant_counts": (
                dict(sorted(tmem_fetch_variant_counts.items()))
                if isinstance(tmem_fetch_variant_counts, dict)
                else {}
            ),
            "tmem_reject_counts": (
                dict(sorted(tmem_reject_counts.items()))
                if isinstance(tmem_reject_counts, dict)
                else {}
            ),
            "rdram_reject_counts": (
                dict(sorted(rdram_reject_counts.items()))
                if isinstance(rdram_reject_counts, dict)
                else {}
            ),
            "source_bits_counts": (
                dict(sorted(source_bits_counts.items()))
                if isinstance(source_bits_counts, dict)
                else {}
            ),
            "sampled_black_count": sampled_black_count,
            "sampled_black_ratio": _ratio(sampled_black_count, valid_count),
            "final_black_count": final_black_count,
            "final_black_ratio": _ratio(final_black_count, valid_count),
            "tmem_raw_zero_count": tmem_raw_zero_count,
            "tmem_raw_zero_ratio": _ratio(tmem_raw_zero_count, valid_count),
            "rdram_probe_valid_count": rdram_probe_valid_count,
            "rdram_probe_valid_ratio": _ratio(rdram_probe_valid_count, valid_count),
            "rdram_probe_non_black_count": rdram_probe_non_black_count,
            "rdram_probe_non_black_ratio": _ratio(rdram_probe_non_black_count, rdram_probe_valid_count),
            "rdram_probe_beats_tmem_count": rdram_probe_beats_tmem_count,
            "rdram_probe_beats_tmem_ratio": _ratio(rdram_probe_beats_tmem_count, rdram_probe_valid_count),
            "rdram_probe_reject_counts": (
                dict(sorted(rdram_probe_reject_counts.items()))
                if isinstance(rdram_probe_reject_counts, dict)
                else {}
            ),
            "top_tmem_indices": top_tmem_indices,
        }

    max_packet_stage_profiles = 256
    source_packet_stage_profiles: List[Dict[str, Any]] = []
    ordered_packet_stage_rows = sorted(
        source_packet_stage_stats.values(),
        key=lambda row: (
            -int(row.get("overwrite_count", 0) or 0),
            -int(row.get("record_count", 0) or 0),
            int(row.get("source_packet_id", 0) or 0),
        ),
    )
    for row in ordered_packet_stage_rows[:max_packet_stage_profiles]:
        record_count_for_packet = int(row.get("record_count", 0) or 0)
        overwrite_count_for_packet = int(row.get("overwrite_count", 0) or 0)
        final_black_count_for_packet = int(row.get("final_black_count", 0) or 0)
        texel_black_count_for_packet = int(row.get("texel_black_count", 0) or 0)
        combiner_black_count_for_packet = int(row.get("combiner_black_count", 0) or 0)
        blender_black_count_for_packet = int(row.get("blender_black_count", 0) or 0)
        kill_at_combiner_count_for_packet = int(row.get("kill_at_combiner_count", 0) or 0)
        kill_at_blender_count_for_packet = int(row.get("kill_at_blender_count", 0) or 0)
        kill_after_blender_count_for_packet = int(row.get("kill_after_blender_count", 0) or 0)
        tex0_valid_count_for_packet = int(row.get("tex0_valid_count", 0) or 0)
        tex0_sample_black_count_for_packet = int(row.get("tex0_sample_black_count", 0) or 0)
        tex0_final_black_count_for_packet = int(row.get("tex0_final_black_count", 0) or 0)
        tex0_probe_valid_count_for_packet = int(row.get("tex0_rdram_probe_valid_count", 0) or 0)
        tex0_probe_non_black_count_for_packet = int(
            row.get("tex0_rdram_probe_non_black_count", 0) or 0
        )
        tex0_probe_beats_tmem_count_for_packet = int(
            row.get("tex0_rdram_probe_beats_tmem_count", 0) or 0
        )
        source_packet_stage_profiles.append(
            {
                "source_packet_id": int(row.get("source_packet_id", 0) or 0),
                "record_count": record_count_for_packet,
                "overwrite_count": overwrite_count_for_packet,
                "overwrite_ratio_of_packet_records": _ratio(overwrite_count_for_packet, record_count_for_packet),
                "overwrite_ratio_of_all_overwrites": _ratio(overwrite_count_for_packet, overwrite_record_count),
                "final_black_count": final_black_count_for_packet,
                "final_black_ratio": _ratio(final_black_count_for_packet, record_count_for_packet),
                "texel_black_count": texel_black_count_for_packet,
                "texel_black_ratio": _ratio(texel_black_count_for_packet, record_count_for_packet),
                "combiner_black_count": combiner_black_count_for_packet,
                "combiner_black_ratio": _ratio(combiner_black_count_for_packet, record_count_for_packet),
                "blender_black_count": blender_black_count_for_packet,
                "blender_black_ratio": _ratio(blender_black_count_for_packet, record_count_for_packet),
                "kill_at_combiner_count": kill_at_combiner_count_for_packet,
                "kill_at_combiner_ratio": _ratio(kill_at_combiner_count_for_packet, record_count_for_packet),
                "kill_at_blender_count": kill_at_blender_count_for_packet,
                "kill_at_blender_ratio": _ratio(kill_at_blender_count_for_packet, record_count_for_packet),
                "kill_after_blender_count": kill_after_blender_count_for_packet,
                "kill_after_blender_ratio": _ratio(kill_after_blender_count_for_packet, record_count_for_packet),
                "tex0_valid_count": tex0_valid_count_for_packet,
                "tex0_sample_black_ratio": _ratio(tex0_sample_black_count_for_packet, tex0_valid_count_for_packet),
                "tex0_final_black_ratio": _ratio(tex0_final_black_count_for_packet, tex0_valid_count_for_packet),
                "tex0_rdram_probe_valid_count": tex0_probe_valid_count_for_packet,
                "tex0_rdram_probe_non_black_ratio": _ratio(
                    tex0_probe_non_black_count_for_packet,
                    tex0_probe_valid_count_for_packet,
                ),
                "tex0_rdram_probe_beats_tmem_ratio": _ratio(
                    tex0_probe_beats_tmem_count_for_packet,
                    tex0_probe_valid_count_for_packet,
                ),
                "op_counts": (
                    dict(sorted((row.get("op_counts", {}) or {}).items()))
                    if isinstance(row.get("op_counts"), dict)
                    else {}
                ),
                "phase_counts": (
                    dict(sorted((row.get("phase_counts", {}) or {}).items()))
                    if isinstance(row.get("phase_counts"), dict)
                    else {}
                ),
            }
        )
    source_packet_stage_profile_truncated_count = max(
        0,
        len(ordered_packet_stage_rows) - len(source_packet_stage_profiles),
    )

    return {
        "record_count": record_count,
        "black_write_record_count": record_count,
        "overwrite_record_count": overwrite_record_count,
        "non_overwrite_black_write_count": non_overwrite_black_write_count,
        "preserved_count": preserved_count,
        "preserved_ratio": _ratio(
            preserved_count,
            overwrite_record_count if overwrite_record_count > 0 else record_count,
        ),
        "op_counts": dict(sorted(overwrite_op_counts.items())),
        "overwrite_op_counts": dict(sorted(overwrite_op_counts.items())),
        "black_write_op_counts": dict(sorted(black_write_op_counts.items())),
        "texture_source_bit_counts": dict(sorted(overwrite_texture_source_bit_counts.items())),
        "overwrite_texture_source_bit_counts": dict(sorted(overwrite_texture_source_bit_counts.items())),
        "black_write_texture_source_bit_counts": dict(sorted(black_write_texture_source_bit_counts.items())),
        "unique_color_image_count": len(overwrite_color_image_counts),
        "black_write_unique_color_image_count": len(black_write_color_image_counts),
        "top_color_images": top_color_image_rows,
        "overwrite_top_color_images": top_color_image_rows,
        "black_write_top_color_images": black_write_top_color_image_rows,
        "top_source_packets": overwrite_top_source_packet_rows,
        "overwrite_top_source_packets": overwrite_top_source_packet_rows,
        "black_write_top_source_packets": black_write_top_source_packet_rows,
        "top_combiner_kill_source_packets": top_combiner_kill_source_rows,
        "overwrite_top_combiner_kill_source_packets": top_combiner_kill_source_rows,
        "black_write_top_combiner_kill_source_packets": black_write_top_combiner_kill_source_rows,
        "top_source_packet_profiles": [],
        "dominant_state": dominant_state,
        "dominant_state_ratio": dominant_state_ratio,
        "overwrite_dominant_state": dominant_state,
        "overwrite_dominant_state_ratio": dominant_state_ratio,
        "black_write_dominant_state": black_write_dominant_state,
        "black_write_dominant_state_ratio": black_write_dominant_state_ratio,
        "stage_black": {
            "texel_black_count": overwrite_texel_black_count,
            "combiner_black_count": overwrite_combiner_black_count,
            "blender_black_count": overwrite_blender_black_count,
            "final_black_count": overwrite_final_black_count,
            "texel_black_ratio": _ratio(overwrite_texel_black_count, overwrite_record_count),
            "combiner_black_ratio": _ratio(overwrite_combiner_black_count, overwrite_record_count),
            "blender_black_ratio": _ratio(overwrite_blender_black_count, overwrite_record_count),
            "final_black_ratio": _ratio(overwrite_final_black_count, overwrite_record_count),
        },
        "stage_kill_counts": {
            "kill_at_combiner_count": overwrite_kill_at_combiner_count,
            "kill_at_blender_count": overwrite_kill_at_blender_count,
            "kill_after_blender_count": overwrite_kill_after_blender_count,
            "kill_at_combiner_ratio": _ratio(overwrite_kill_at_combiner_count, overwrite_record_count),
            "kill_at_blender_ratio": _ratio(overwrite_kill_at_blender_count, overwrite_record_count),
            "kill_after_blender_ratio": _ratio(overwrite_kill_after_blender_count, overwrite_record_count),
        },
        "overwrite_stage_black": {
            "texel_black_count": overwrite_texel_black_count,
            "combiner_black_count": overwrite_combiner_black_count,
            "blender_black_count": overwrite_blender_black_count,
            "final_black_count": overwrite_final_black_count,
            "texel_black_ratio": _ratio(overwrite_texel_black_count, overwrite_record_count),
            "combiner_black_ratio": _ratio(overwrite_combiner_black_count, overwrite_record_count),
            "blender_black_ratio": _ratio(overwrite_blender_black_count, overwrite_record_count),
            "final_black_ratio": _ratio(overwrite_final_black_count, overwrite_record_count),
        },
        "overwrite_stage_kill_counts": {
            "kill_at_combiner_count": overwrite_kill_at_combiner_count,
            "kill_at_blender_count": overwrite_kill_at_blender_count,
            "kill_after_blender_count": overwrite_kill_after_blender_count,
            "kill_at_combiner_ratio": _ratio(overwrite_kill_at_combiner_count, overwrite_record_count),
            "kill_at_blender_ratio": _ratio(overwrite_kill_at_blender_count, overwrite_record_count),
            "kill_after_blender_ratio": _ratio(overwrite_kill_after_blender_count, overwrite_record_count),
        },
        "black_write_stage_black": {
            "texel_black_count": black_write_texel_black_count,
            "combiner_black_count": black_write_combiner_black_count,
            "blender_black_count": black_write_blender_black_count,
            "final_black_count": black_write_final_black_count,
            "texel_black_ratio": _ratio(black_write_texel_black_count, record_count),
            "combiner_black_ratio": _ratio(black_write_combiner_black_count, record_count),
            "blender_black_ratio": _ratio(black_write_blender_black_count, record_count),
            "final_black_ratio": _ratio(black_write_final_black_count, record_count),
        },
        "black_write_stage_kill_counts": {
            "kill_at_combiner_count": black_write_kill_at_combiner_count,
            "kill_at_blender_count": black_write_kill_at_blender_count,
            "kill_after_blender_count": black_write_kill_after_blender_count,
            "kill_at_combiner_ratio": _ratio(black_write_kill_at_combiner_count, record_count),
            "kill_at_blender_ratio": _ratio(black_write_kill_at_blender_count, record_count),
            "kill_after_blender_ratio": _ratio(black_write_kill_after_blender_count, record_count),
        },
        "texel_detail": {
            "available": texel_detail_available,
            "slot_summaries": texel_slot_summaries,
        },
        "source_packet_stage_profiles": source_packet_stage_profiles,
        "source_packet_stage_profile_count": len(ordered_packet_stage_rows),
        "source_packet_stage_profile_truncated_count": source_packet_stage_profile_truncated_count,
    }


def _load_packet_trace_replay_module() -> Optional[Any]:
    module_path = Path(__file__).with_name("rvk2_packet_trace_replay.py")
    if not module_path.is_file():
        return None
    try:
        spec = importlib.util.spec_from_file_location("rvk2_packet_trace_replay_bundle", str(module_path))
        if spec is None or spec.loader is None:
            return None
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        return module
    except Exception:
        return None


def _profile_overwrite_source_packets(
    overwrite_summary: Optional[Dict[str, Any]],
    packet_trace_path: Optional[Path],
) -> Dict[str, Any]:
    result: Dict[str, Any] = {
        "profiles": [],
        "requested_count": 0,
        "resolved_count": 0,
        "error": None,
    }
    if not isinstance(overwrite_summary, dict):
        return result
    if packet_trace_path is None or not packet_trace_path.is_file():
        return result

    overwrite_counts: Dict[int, int] = {}
    for row in overwrite_summary.get("top_source_packets", []):
        if not isinstance(row, dict):
            continue
        source_packet_id = _u64(row, "source_packet_id")
        count = _u64(row, "count")
        if source_packet_id > 0 and count > 0:
            overwrite_counts[source_packet_id] = count

    combiner_kill_counts: Dict[int, int] = {}
    for row in overwrite_summary.get("top_combiner_kill_source_packets", []):
        if not isinstance(row, dict):
            continue
        source_packet_id = _u64(row, "source_packet_id")
        count = _u64(row, "count")
        if source_packet_id > 0 and count > 0:
            combiner_kill_counts[source_packet_id] = count

    target_ids = set(overwrite_counts.keys()) | set(combiner_kill_counts.keys())
    if not target_ids:
        return result

    module = _load_packet_trace_replay_module()
    if module is None:
        result["error"] = "packet trace replay parser is unavailable"
        return result

    try:
        frames = module.parse_packet_trace(packet_trace_path)
    except Exception as exc:  # pragma: no cover - defensive parse guard
        result["error"] = f"packet trace parse failed: {exc}"
        return result

    work_by_source: Dict[int, Any] = {}
    for frame in frames:
        render_work_rows = getattr(frame, "render_work", [])
        for work in render_work_rows:
            source_packet_id = int(getattr(work, "source_packet_id", 0))
            if source_packet_id <= 0 or source_packet_id not in target_ids:
                continue
            if source_packet_id not in work_by_source:
                work_by_source[source_packet_id] = work
        if len(work_by_source) >= len(target_ids):
            break

    ordered_sources = sorted(target_ids, key=lambda sid: overwrite_counts.get(sid, 0), reverse=True)
    profiles: List[Dict[str, Any]] = []
    for source_packet_id in ordered_sources:
        overwrite_count = int(overwrite_counts.get(source_packet_id, 0))
        combiner_kill_count = int(combiner_kill_counts.get(source_packet_id, 0))
        work = work_by_source.get(source_packet_id)
        if work is None:
            profiles.append(
                {
                    "source_packet_id": int(source_packet_id),
                    "overwrite_count": overwrite_count,
                    "combiner_kill_count": combiner_kill_count,
                    "resolved_in_packet_trace": False,
                }
            )
            continue

        combine_mux = int(getattr(work, "combine_mux", 0))
        other_modes = int(getattr(work, "other_modes", 0))
        blend_params = int(getattr(work, "blend_params", 0))
        shade_r = int(getattr(work, "triangle_shade_r", 0))
        shade_g = int(getattr(work, "triangle_shade_g", 0))
        shade_b = int(getattr(work, "triangle_shade_b", 0))
        shade_a = int(getattr(work, "triangle_shade_a", 0))
        shade_r_byte = (shade_r >> 8) & 0xFF
        shade_g_byte = (shade_g >> 8) & 0xFF
        shade_b_byte = (shade_b >> 8) & 0xFF
        shade_a_byte = (shade_a >> 8) & 0xFF
        profile = {
            "source_packet_id": int(source_packet_id),
            "overwrite_count": overwrite_count,
            "combiner_kill_count": combiner_kill_count,
            "resolved_in_packet_trace": True,
            "op_kind": int(getattr(work, "op_kind", 0)),
            "phase": int(getattr(work, "phase", 0)),
            "combine_mux": f"0x{combine_mux:016X}",
            "other_modes": f"0x{other_modes:016X}",
            "blend_params": f"0x{blend_params:08X}",
            "image_read_enabled": 1 if ((other_modes & (1 << 6)) != 0) else 0,
            "triangle_shade_enable": 1 if bool(getattr(work, "triangle_shade_enable", False)) else 0,
            "triangle_texture_enable": 1 if bool(getattr(work, "triangle_texture_enable", False)) else 0,
            "textured": 1 if bool(getattr(work, "textured", False)) else 0,
            "triangle_shade_rgb_zero": 1 if (shade_r_byte == 0 and shade_g_byte == 0 and shade_b_byte == 0) else 0,
            "triangle_shade_rgba_bytes": {
                "r": int(shade_r_byte),
                "g": int(shade_g_byte),
                "b": int(shade_b_byte),
                "a": int(shade_a_byte),
            },
            "prim_color": f"0x{int(getattr(work, 'prim_color', 0)) & 0xFFFFFFFF:08X}",
            "env_color": f"0x{int(getattr(work, 'env_color', 0)) & 0xFFFFFFFF:08X}",
            "cycle1_combiner_selectors": _decode_combiner_cycle_selectors(combine_mux, cycle2=False),
            "cycle2_combiner_selectors": _decode_combiner_cycle_selectors(combine_mux, cycle2=True),
            "cycle1_blend_selectors": _decode_blend_selectors(other_modes, cycle2=False),
            "cycle2_blend_selectors": _decode_blend_selectors(other_modes, cycle2=True),
        }
        profiles.append(profile)

    result["profiles"] = profiles
    result["requested_count"] = len(target_ids)
    result["resolved_count"] = len(work_by_source)
    return result


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
    history_merge_summary: Dict[str, Any],
    overwrite_summary: Dict[str, Any],
    depth_summary: Optional[Dict[str, Any]],
    metrics: Optional[Dict[str, Any]],
    command_census: Optional[Dict[str, Any]],
    diff_playbook_summary: Optional[Dict[str, Any]],
    missing_region_focus: Optional[Dict[str, Any]],
    executor_present_compare: Optional[Dict[str, Any]] = None,
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
    selected_surface_history_age = _u64(last_record, "selected_surface_history_age")
    selected_surface_history_merge_candidates = _u64(last_record, "selected_surface_history_merge_candidates")
    selected_surface_history_merge_potential_black_fill = _u64(
        last_record,
        "selected_surface_history_merge_potential_black_fill",
    )
    selected_surface_history_merge_potential_nonblack_diff = _u64(
        last_record,
        "selected_surface_history_merge_potential_nonblack_diff",
    )
    selected_surface_history_merge_copied = _u64(last_record, "selected_surface_history_merge_copied")
    selected_surface_untouched_carry = _u64(last_record, "selected_surface_untouched_carry")
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
        "selected_surface_history_age": selected_surface_history_age,
        "vi_hash_decode": vi_hash_decode,
        "vi_hash_filter": vi_hash_filter,
        "vi_hash_gdither": vi_hash_gdither,
        "executor_present_compare": (
            executor_present_compare if isinstance(executor_present_compare, dict) else {}
        ),
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

    if isinstance(executor_present_compare, dict) and executor_present_compare.get("available") is True:
        direct_rmse = executor_present_compare.get("rmse")
        best_rmse = executor_present_compare.get("best_rmse")
        best_variant = executor_present_compare.get("best_variant")
        if isinstance(direct_rmse, (int, float)):
            if (
                direct_rmse > 0.15
                and isinstance(best_rmse, (int, float))
                and best_rmse > 0.15
            ):
                suspected_gaps.append(
                    "candidate capture diverges strongly from executor-present dump; presentation path is introducing non-executor output differences"
                )
            elif (
                direct_rmse > 0.15
                and isinstance(best_rmse, (int, float))
                and best_rmse < 0.06
                and isinstance(best_variant, str)
                and best_variant != "direct"
            ):
                suspected_gaps.append(
                    f"executor-present dump aligns with candidate only after {best_variant}; orientation/channel mapping mismatch is likely"
                )

    history_merge_signal: Dict[str, Any] = {}
    if isinstance(history_merge_summary, dict):
        history_merge_record_count = int(history_merge_summary.get("record_count", 0) or 0)
        history_merge_total_potential_black_fill = int(
            history_merge_summary.get("total_potential_black_fill", 0) or 0
        )
        history_merge_total_potential_nonblack_diff = int(
            history_merge_summary.get("total_potential_nonblack_diff", 0) or 0
        )
        history_merge_total_copied = int(history_merge_summary.get("total_copied", 0) or 0)
        history_merge_signal = {
            "record_count": history_merge_record_count,
            "frame_count": int(history_merge_summary.get("frame_count", 0) or 0),
            "present_surface_count": int(history_merge_summary.get("present_surface_count", 0) or 0),
            "candidate_surface_count": int(history_merge_summary.get("candidate_surface_count", 0) or 0),
            "total_potential_black_fill": history_merge_total_potential_black_fill,
            "total_potential_nonblack_diff": history_merge_total_potential_nonblack_diff,
            "total_copied": history_merge_total_copied,
            "records_with_potential_black_fill": int(
                history_merge_summary.get("records_with_potential_black_fill", 0) or 0
            ),
            "records_with_potential_nonblack_diff": int(
                history_merge_summary.get("records_with_potential_nonblack_diff", 0) or 0
            ),
            "records_with_copied": int(history_merge_summary.get("records_with_copied", 0) or 0),
            "max_potential_black_fill": int(history_merge_summary.get("max_potential_black_fill", 0) or 0),
            "max_potential_nonblack_diff": int(history_merge_summary.get("max_potential_nonblack_diff", 0) or 0),
            "max_copied": int(history_merge_summary.get("max_copied", 0) or 0),
            "top_nonblack_diff_record": history_merge_summary.get("top_nonblack_diff_record", {}),
            "top_black_fill_record": history_merge_summary.get("top_black_fill_record", {}),
            "top_copied_record": history_merge_summary.get("top_copied_record", {}),
            "forensics_history_merge_candidates": selected_surface_history_merge_candidates,
            "forensics_history_merge_potential_black_fill": selected_surface_history_merge_potential_black_fill,
            "forensics_history_merge_potential_nonblack_diff": selected_surface_history_merge_potential_nonblack_diff,
            "forensics_history_merge_copied": selected_surface_history_merge_copied,
            "forensics_selected_surface_untouched_carry": selected_surface_untouched_carry,
        }

    overwrite_signal: Dict[str, Any] = {}
    overwrite_source_packet_stage_profiles: List[Dict[str, Any]] = []
    overwrite_source_packet_stage_by_id: Dict[int, Dict[str, Any]] = {}
    if isinstance(overwrite_summary, dict):
        black_write_record_count = int(
            overwrite_summary.get("black_write_record_count", overwrite_summary.get("record_count", 0)) or 0
        )
        overwrite_record_count = int(
            overwrite_summary.get("overwrite_record_count", overwrite_summary.get("record_count", 0)) or 0
        )
        non_overwrite_black_write_count = int(
            overwrite_summary.get(
                "non_overwrite_black_write_count",
                max(0, black_write_record_count - overwrite_record_count),
            )
            or 0
        )
        overwrite_preserved_count = int(overwrite_summary.get("preserved_count", 0) or 0)
        overwrite_preserved_ratio = overwrite_summary.get("preserved_ratio")
        overwrite_op_counts = overwrite_summary.get("op_counts", {})
        black_write_op_counts = overwrite_summary.get("black_write_op_counts", {})
        overwrite_texture_source_bit_counts = overwrite_summary.get("texture_source_bit_counts", {})
        overwrite_top_color_images = overwrite_summary.get("top_color_images", [])
        overwrite_top_source_packets = overwrite_summary.get("top_source_packets", [])
        black_write_top_source_packets = overwrite_summary.get("black_write_top_source_packets", [])
        overwrite_top_combiner_kill_source_packets = overwrite_summary.get("top_combiner_kill_source_packets", [])
        overwrite_top_source_packet_profiles = overwrite_summary.get("top_source_packet_profiles", [])
        overwrite_dominant_state = overwrite_summary.get("dominant_state", {})
        overwrite_dominant_state_ratio = overwrite_summary.get("dominant_state_ratio")
        overwrite_stage_black = overwrite_summary.get("stage_black", {})
        overwrite_stage_kill_counts = overwrite_summary.get("stage_kill_counts", {})
        black_write_stage_black = overwrite_summary.get("black_write_stage_black", {})
        black_write_stage_kill_counts = overwrite_summary.get("black_write_stage_kill_counts", {})
        overwrite_texel_detail = overwrite_summary.get("texel_detail", {})
        raw_source_packet_stage_profiles = overwrite_summary.get("source_packet_stage_profiles", [])
        if isinstance(raw_source_packet_stage_profiles, list):
            overwrite_source_packet_stage_profiles = [
                row for row in raw_source_packet_stage_profiles if isinstance(row, dict)
            ]
            for row in overwrite_source_packet_stage_profiles:
                source_packet_id = int(row.get("source_packet_id", 0) or 0)
                if source_packet_id > 0:
                    overwrite_source_packet_stage_by_id[source_packet_id] = row
        overwrite_signal = {
            "record_count": overwrite_record_count,
            "black_write_record_count": black_write_record_count,
            "non_overwrite_black_write_count": non_overwrite_black_write_count,
            "preserved_count": overwrite_preserved_count,
            "preserved_ratio": overwrite_preserved_ratio,
            "op_counts": overwrite_op_counts if isinstance(overwrite_op_counts, dict) else {},
            "black_write_op_counts": black_write_op_counts if isinstance(black_write_op_counts, dict) else {},
            "texture_source_bit_counts": (
                overwrite_texture_source_bit_counts if isinstance(overwrite_texture_source_bit_counts, dict) else {}
            ),
            "unique_color_image_count": int(overwrite_summary.get("unique_color_image_count", 0) or 0),
            "top_color_images": overwrite_top_color_images if isinstance(overwrite_top_color_images, list) else [],
            "top_source_packets": (
                overwrite_top_source_packets if isinstance(overwrite_top_source_packets, list) else []
            ),
            "black_write_top_source_packets": (
                black_write_top_source_packets if isinstance(black_write_top_source_packets, list) else []
            ),
            "top_combiner_kill_source_packets": (
                overwrite_top_combiner_kill_source_packets
                if isinstance(overwrite_top_combiner_kill_source_packets, list)
                else []
            ),
            "top_source_packet_profiles": (
                overwrite_top_source_packet_profiles if isinstance(overwrite_top_source_packet_profiles, list) else []
            ),
            "dominant_state": overwrite_dominant_state if isinstance(overwrite_dominant_state, dict) else {},
            "dominant_state_ratio": overwrite_dominant_state_ratio,
            "stage_black": overwrite_stage_black if isinstance(overwrite_stage_black, dict) else {},
            "stage_kill_counts": overwrite_stage_kill_counts if isinstance(overwrite_stage_kill_counts, dict) else {},
            "black_write_stage_black": (
                black_write_stage_black if isinstance(black_write_stage_black, dict) else {}
            ),
            "black_write_stage_kill_counts": (
                black_write_stage_kill_counts if isinstance(black_write_stage_kill_counts, dict) else {}
            ),
            "texel_detail": overwrite_texel_detail if isinstance(overwrite_texel_detail, dict) else {},
            "source_packet_stage_profiles": overwrite_source_packet_stage_profiles,
            "source_packet_stage_profile_count": int(
                overwrite_summary.get("source_packet_stage_profile_count", 0) or 0
            ),
            "source_packet_stage_profile_truncated_count": int(
                overwrite_summary.get("source_packet_stage_profile_truncated_count", 0) or 0
            ),
        }
        if black_write_record_count > 0 and overwrite_record_count == 0:
            suspected_gaps.append(
                "black-write telemetry logged events but none were overwrite-to-black; missing regions may be first-write black outputs"
            )
        if black_write_record_count > 0 and non_overwrite_black_write_count * 2 > black_write_record_count:
            suspected_gaps.append(
                "most black-write telemetry entries are non-overwrite writes; prioritize missing-region first-write packet tracing"
            )
        if overwrite_record_count > 0 and overwrite_preserved_count == 0:
            suspected_gaps.append(
                "overwrite-to-black events were logged but preserve-on-black path never retained prior non-black pixels"
            )
        if overwrite_record_count > 0 and isinstance(overwrite_preserved_ratio, (int, float)) and overwrite_preserved_ratio < 0.02:
            suspected_gaps.append(
                "overwrite-to-black preserve ratio is below 2%; missing content may depend on preserving prior non-black texels"
            )
        tri_overwrite = 0
        texrect_overwrite = 0
        if isinstance(overwrite_op_counts, dict):
            tri_overwrite = int(overwrite_op_counts.get("triangle", 0) or 0)
            texrect_overwrite = int(overwrite_op_counts.get("texrect", 0) or 0)
        if texrect_overwrite > 0 and tri_overwrite * 3 < texrect_overwrite:
            suspected_gaps.append(
                "overwrite-to-black events are texrect dominated; prioritize texrect combiner/texture-state parity for missing scene content"
            )
        if isinstance(overwrite_stage_black, dict):
            texel_black_ratio = overwrite_stage_black.get("texel_black_ratio")
            combiner_black_ratio = overwrite_stage_black.get("combiner_black_ratio")
            if isinstance(texel_black_ratio, (int, float)) and texel_black_ratio > 0.80:
                suspected_gaps.append(
                    "overwrite-to-black samples are already black at texel stage (>80%); prioritize TMEM decode/addressing and tile state parity"
                )
            if (
                isinstance(texel_black_ratio, (int, float))
                and isinstance(combiner_black_ratio, (int, float))
                and texel_black_ratio + 0.20 < combiner_black_ratio
            ):
                suspected_gaps.append(
                    "combiner stage introduces substantial additional blacking beyond texel stage; combiner mux/input routing likely contributing"
                )
        if isinstance(overwrite_stage_kill_counts, dict):
            kill_at_combiner_ratio = overwrite_stage_kill_counts.get("kill_at_combiner_ratio")
            kill_at_blender_ratio = overwrite_stage_kill_counts.get("kill_at_blender_ratio")
            if isinstance(kill_at_combiner_ratio, (int, float)) and kill_at_combiner_ratio > 0.25:
                suspected_gaps.append(
                    "many overwrite-to-black events transition non-black texels to black at combiner stage (>25%)"
                )
            if isinstance(kill_at_blender_ratio, (int, float)) and kill_at_blender_ratio > 0.25:
                suspected_gaps.append(
                    "many overwrite-to-black events transition non-black combiner output to black at blender stage (>25%)"
                )
        if isinstance(overwrite_texel_detail, dict):
            slot_summaries = overwrite_texel_detail.get("slot_summaries", {})
            if isinstance(slot_summaries, dict):
                tex0_summary = slot_summaries.get("tex0", {})
                if isinstance(tex0_summary, dict):
                    tex0_valid = int(tex0_summary.get("valid_record_count", 0) or 0)
                    source_kind_counts = tex0_summary.get("source_kind_counts", {})
                    tmem_raw_zero_ratio = tex0_summary.get("tmem_raw_zero_ratio")
                    if tex0_valid > 0 and isinstance(source_kind_counts, dict):
                        synthetic_count = int(source_kind_counts.get("synthetic", 0) or 0)
                        tmem_count = int(source_kind_counts.get("tmem", 0) or 0)
                        if synthetic_count * 2 > tex0_valid:
                            suspected_gaps.append(
                                "tex0 overwrite telemetry is synthetic-dominated (>50%); TMEM/RDRAM sampling fails before combiner in dominant missing-path writes"
                            )
                        if tmem_count * 3 >= tex0_valid and isinstance(tmem_raw_zero_ratio, (int, float)) and tmem_raw_zero_ratio > 0.6:
                            suspected_gaps.append(
                                "tex0 TMEM samples are dominant and >60% carry zero raw TMEM words; prioritize TMEM address/xor/stride parity for missing content"
                            )
                        rdram_probe_beats_ratio = tex0_summary.get("rdram_probe_beats_tmem_ratio")
                        if (
                            tmem_count * 3 >= tex0_valid
                            and isinstance(rdram_probe_beats_ratio, (int, float))
                            and rdram_probe_beats_ratio > 0.25
                        ):
                            suspected_gaps.append(
                                "tex0 RDRAM probe returns non-black while TMEM sample is black in >25% of probes; TMEM load/fetch parity is likely dropping texture data"
                            )
                tex1_summary = slot_summaries.get("tex1", {})
                if isinstance(tex1_summary, dict):
                    tex1_valid = int(tex1_summary.get("valid_record_count", 0) or 0)
                    tex1_source_counts = tex1_summary.get("source_kind_counts", {})
                    tex1_top_indices = tex1_summary.get("top_tmem_indices", [])
                    if (
                        tex1_valid > 0
                        and isinstance(tex1_source_counts, dict)
                        and int(tex1_source_counts.get("tmem", 0) or 0) * 2 >= tex1_valid
                        and isinstance(tex1_top_indices, list)
                        and tex1_top_indices
                        and isinstance(tex1_top_indices[0], dict)
                    ):
                        top = tex1_top_indices[0]
                        top_ratio = top.get("ratio")
                        top_a = int(top.get("tmem_index_a", 0) or 0)
                        top_b = int(top.get("tmem_index_b", 0) or 0)
                        if isinstance(top_ratio, (int, float)) and top_ratio > 0.95 and top_a == 0 and top_b == 0:
                            suspected_gaps.append(
                                "tex1 TMEM sampling collapses to index 0x000/0x000 for >95% of black writes; verify texel1 slot tile selection and coordinate routing"
                            )
        if isinstance(overwrite_top_source_packet_profiles, list) and overwrite_top_source_packet_profiles:
            total_profiled_overwrite = 0
            zero_shade_overwrite = 0
            cycle1_shade_modulate_overwrite = 0
            cycle2_memory_blend_overwrite = 0
            for profile in overwrite_top_source_packet_profiles:
                if not isinstance(profile, dict):
                    continue
                overwrite_count = int(profile.get("overwrite_count", 0) or 0)
                if overwrite_count <= 0:
                    continue
                total_profiled_overwrite += overwrite_count
                if int(profile.get("triangle_shade_rgb_zero", 0) or 0) != 0:
                    zero_shade_overwrite += overwrite_count
                cycle1_combiner = profile.get("cycle1_combiner_selectors", {})
                if isinstance(cycle1_combiner, dict):
                    if int(cycle1_combiner.get("color_c", -1)) == 4:
                        cycle1_shade_modulate_overwrite += overwrite_count
                cycle2_blend = profile.get("cycle2_blend_selectors", {})
                if isinstance(cycle2_blend, dict):
                    if int(cycle2_blend.get("m2a", -1)) == 1:
                        cycle2_memory_blend_overwrite += overwrite_count
            if total_profiled_overwrite > 0:
                zero_shade_ratio = float(zero_shade_overwrite) / float(total_profiled_overwrite)
                cycle1_shade_modulate_ratio = float(cycle1_shade_modulate_overwrite) / float(total_profiled_overwrite)
                cycle2_memory_blend_ratio = float(cycle2_memory_blend_overwrite) / float(total_profiled_overwrite)
                if zero_shade_ratio > 0.50:
                    suspected_gaps.append(
                        "dominant overwrite source packets carry zero shade RGB; verify triangle shade coefficient decode and shade routing"
                    )
                if cycle1_shade_modulate_ratio > 0.50:
                    suspected_gaps.append(
                        "dominant overwrite source packets use cycle-1 combiner C=shade modulation; shading lane parity is high leverage"
                    )
                if cycle2_memory_blend_ratio > 0.50:
                    suspected_gaps.append(
                        "dominant overwrite source packets rely on cycle-2 blender memory input; validate cycle handoff and memory-color feed"
                    )
        if (
            overwrite_record_count > 0
            and isinstance(overwrite_dominant_state_ratio, (int, float))
            and overwrite_dominant_state_ratio > 0.90
            and isinstance(overwrite_dominant_state, dict)
            and overwrite_dominant_state
        ):
            state_op = overwrite_dominant_state.get("op_name")
            state_combine = overwrite_dominant_state.get("combine_mux")
            state_modes = overwrite_dominant_state.get("other_modes")
            suspected_gaps.append(
                "overwrite-to-black stream is dominated by a single state cluster "
                f"(op={state_op} combine={state_combine} other_modes={state_modes}); target this cluster first"
            )

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
    if selected_surface_from_history != 0 and vi_origin_match == 0:
        suspected_gaps.append("present selected surface came from history cache without VI-origin match (potential stale-buffer presentation)")
    if selected_surface_from_history != 0 and selected_surface_history_age > 2:
        suspected_gaps.append("present selected history surface is older than 2 executor frames (stale-buffer risk)")
        hard_faults.append("present-surface handoff fault: selected history surface older than 2 executor frames")
    if (
        writes > 0
        and selected_surface_live_writes == 0
        and selected_surface_from_history != 0
        and vi_origin_match == 0
    ):
        suspected_gaps.append("presented surface had no live writes in this frame (buffer handoff mismatch candidate)")
        hard_faults.append("present-surface handoff fault: selected history surface with zero live writes in an active frame")
    if (
        selected_surface_history_merge_candidates > 0
        and selected_surface_history_merge_potential_black_fill == 0
        and selected_surface_history_merge_potential_nonblack_diff > 0
    ):
        suspected_gaps.append(
            "history-merge candidates contain no black-fill opportunities in the selected frame while non-black divergence remains high"
        )
    if (
        selected_surface_from_history != 0
        and selected_surface_history_merge_potential_black_fill > 0
        and selected_surface_history_merge_copied == 0
        and selected_surface_untouched_carry == 0
    ):
        suspected_gaps.append(
            "history-merge detected black-fill potential but no carry-forward pixels were copied in the selected frame"
        )
    if isinstance(history_merge_summary, dict):
        history_merge_record_count = int(history_merge_summary.get("record_count", 0) or 0)
        history_merge_total_potential_black_fill = int(
            history_merge_summary.get("total_potential_black_fill", 0) or 0
        )
        history_merge_total_potential_nonblack_diff = int(
            history_merge_summary.get("total_potential_nonblack_diff", 0) or 0
        )
        if (
            history_merge_record_count > 0
            and history_merge_total_potential_black_fill == 0
            and history_merge_total_potential_nonblack_diff > 0
        ):
            suspected_gaps.append(
                "history-merge log shows no black-fill candidates across probed pairs; divergence is dominated by conflicting non-black content"
            )

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

    missing_region_signal: Dict[str, Any] = {}
    if isinstance(missing_region_focus, dict):
        counts = missing_region_focus.get("counts", {})
        bucket_bbox_hits = missing_region_focus.get("texture_bucket_hits", {})
        bucket_write_hits = missing_region_focus.get("texture_bucket_write_hits", {})
        write_state_hits = missing_region_focus.get("write_state_hits", {})
        write_coverage = missing_region_focus.get("write_coverage", {})
        missing_write_attribution = missing_region_focus.get("missing_write_attribution", {})
        work_hit_stats = missing_region_focus.get("work_hit_stats", {})
        color_image_sequence = missing_region_focus.get("color_image_sequence", {})
        history_window = missing_region_focus.get("history_window", {})
        address_write_stats_raw = missing_region_focus.get("address_write_stats", [])
        address_write_stats: List[Dict[str, Any]] = (
            [row for row in address_write_stats_raw if isinstance(row, dict)]
            if isinstance(address_write_stats_raw, list)
            else []
        )
        forensics_last_active = missing_region_focus.get("forensics_last_active", {})
        frame_id = missing_region_focus.get("frame_id")
        tri_total = int(counts.get("work_triangle_total", 0) or 0) if isinstance(counts, dict) else 0
        tri_bbox_hit = int(counts.get("work_triangle_hit", 0) or 0) if isinstance(counts, dict) else 0
        tri_write_hit = int(counts.get("work_triangle_write_hit", 0) or 0) if isinstance(counts, dict) else 0
        tex_total = int(counts.get("work_texrect_total", 0) or 0) if isinstance(counts, dict) else 0
        tex_bbox_hit = int(counts.get("work_texrect_hit", 0) or 0) if isinstance(counts, dict) else 0
        tex_write_hit = int(counts.get("work_texrect_write_hit", 0) or 0) if isinstance(counts, dict) else 0
        present_surface_focus = (
            int(forensics_last_active.get("present_surface", 0) or 0)
            if isinstance(forensics_last_active, dict)
            else 0
        )
        present_surface_focus_hex = f"0x{present_surface_focus:08X}" if present_surface_focus > 0 else None

        present_surface_address_stats: Dict[str, Any] = {}
        dominant_missing_address_stats: Dict[str, Any] = {}
        history_address_write_stats: List[Dict[str, Any]] = []
        history_prior_address_write_stats: List[Dict[str, Any]] = []
        if isinstance(history_window, dict):
            history_rows = history_window.get("address_write_stats", [])
            if isinstance(history_rows, list):
                history_address_write_stats = [row for row in history_rows if isinstance(row, dict)]
            history_prior_rows = history_window.get("prior_address_write_stats", [])
            if isinstance(history_prior_rows, list):
                history_prior_address_write_stats = [row for row in history_prior_rows if isinstance(row, dict)]
        present_surface_history_stats: Dict[str, Any] = {}
        dominant_missing_history_stats: Dict[str, Any] = {}
        if address_write_stats:
            if present_surface_focus > 0:
                for row in address_write_stats:
                    if int(row.get("color_image_address", 0) or 0) == present_surface_focus:
                        present_surface_address_stats = row
                        break
            dominant_missing_address_stats = max(
                address_write_stats,
                key=lambda row: int(row.get("write_source_box_pixels", 0) or 0),
            )
        if history_address_write_stats:
            if present_surface_focus > 0:
                for row in history_address_write_stats:
                    if int(row.get("color_image_address", 0) or 0) == present_surface_focus:
                        present_surface_history_stats = row
                        break
            dominant_missing_history_stats = max(
                history_address_write_stats,
                key=lambda row: int(row.get("write_source_box_pixels", 0) or 0),
            )
        missing_write_history = (
            missing_write_attribution.get("history", {})
            if isinstance(missing_write_attribution, dict)
            else {}
        )
        missing_with_write_packet_stage_attribution: List[Dict[str, Any]] = []
        missing_with_write_packet_stage_summary: Dict[str, Any] = {}

        missing_region_signal = {
            "frame_id": frame_id,
            "triangle_total": tri_total,
            "triangle_hit": tri_bbox_hit,
            "triangle_hit_ratio": _ratio(tri_bbox_hit, tri_total),
            "triangle_write_hit": tri_write_hit,
            "triangle_write_hit_ratio": _ratio(tri_write_hit, tri_total),
            "texrect_total": tex_total,
            "texrect_hit": tex_bbox_hit,
            "texrect_hit_ratio": _ratio(tex_bbox_hit, tex_total),
            "texrect_write_hit": tex_write_hit,
            "texrect_write_hit_ratio": _ratio(tex_write_hit, tex_total),
            "texture_bucket_hits": bucket_bbox_hits if isinstance(bucket_bbox_hits, dict) else {},
            "texture_bucket_write_hits": bucket_write_hits if isinstance(bucket_write_hits, dict) else {},
            "write_state_hits": write_state_hits if isinstance(write_state_hits, dict) else {},
            "source_boxes": missing_region_focus.get("source_boxes", []),
            "write_coverage": write_coverage if isinstance(write_coverage, dict) else {},
            "work_hit_stats": work_hit_stats if isinstance(work_hit_stats, dict) else {},
            "missing_write_attribution": (
                missing_write_attribution if isinstance(missing_write_attribution, dict) else {}
            ),
            "missing_write_history": missing_write_history if isinstance(missing_write_history, dict) else {},
            "color_image_sequence": color_image_sequence if isinstance(color_image_sequence, dict) else {},
            "history_window": history_window if isinstance(history_window, dict) else {},
            "present_surface_focus": present_surface_focus,
            "present_surface_focus_hex": present_surface_focus_hex,
            "present_surface_address_stats": present_surface_address_stats,
            "dominant_missing_address_stats": dominant_missing_address_stats,
            "present_surface_history_stats": present_surface_history_stats,
            "dominant_missing_history_stats": dominant_missing_history_stats,
            "address_write_stats": address_write_stats,
            "history_prior_address_write_stats": history_prior_address_write_stats,
            "missing_with_write_packet_stage_attribution": missing_with_write_packet_stage_attribution,
            "missing_with_write_packet_stage_summary": missing_with_write_packet_stage_summary,
        }
        tri_hit_ratio = _ratio(tri_bbox_hit, tri_total)
        tex_hit_ratio = _ratio(tex_bbox_hit, tex_total)
        tri_write_hit_ratio = _ratio(tri_write_hit, tri_total)
        tex_write_hit_ratio = _ratio(tex_write_hit, tex_total)
        if tex_hit_ratio is not None and tex_hit_ratio > 0.90 and tri_hit_ratio is not None and tri_hit_ratio < 0.40:
            suspected_gaps.append(
                "missing-region focus is dominated by texrect-intersecting work; prioritize texrect texture decode/addressing lane"
            )
        if tri_hit_ratio is not None and tri_hit_ratio > 0.70 and tex_hit_ratio is not None and tex_hit_ratio < 0.50:
            suspected_gaps.append(
                "missing-region focus is dominated by triangle-intersecting work; prioritize triangle raster/texture lane"
            )
        if tex_hit_ratio is not None and tex_write_hit_ratio is not None and tex_hit_ratio > 0.70 and tex_write_hit_ratio + 0.20 < tex_hit_ratio:
            suspected_gaps.append(
                "missing-region texrect coverage intersects target box but write-bounds hit ratio is substantially lower (rect/scissor coverage loss)"
            )
        if tri_hit_ratio is not None and tri_write_hit_ratio is not None and tri_hit_ratio > 0.40 and tri_write_hit_ratio + 0.20 < tri_hit_ratio:
            suspected_gaps.append(
                "missing-region triangle coverage intersects target box but write-bounds hit ratio is substantially lower (triangle bounds/scissor loss)"
            )
        if isinstance(work_hit_stats, dict):
            hit_total = int(work_hit_stats.get("hit_total", 0) or 0)
            samples_emitted = int(work_hit_stats.get("samples_emitted", 0) or 0)
            samples_truncated = int(work_hit_stats.get("samples_truncated", 0) or 0)
            if hit_total > 0 and samples_truncated > 0 and samples_emitted * 5 < hit_total * 4:
                suspected_gaps.append(
                    "missing-region hit samples are heavily truncated; increase max-hit-samples for full chronology attribution"
                )

        if isinstance(write_state_hits, dict):
            def dominant_entry(key: str):
                raw = write_state_hits.get(key, {})
                if not isinstance(raw, dict) or not raw:
                    return (None, 0, 0, None)
                total = 0
                top_key = None
                top_count = 0
                for entry_key, entry_value in raw.items():
                    count = int(entry_value or 0)
                    total += count
                    if count > top_count:
                        top_count = count
                        top_key = str(entry_key)
                return (top_key, top_count, total, _ratio(top_count, total))

            texrect_combine = dominant_entry("texrect:combine_mux")
            texrect_modes = dominant_entry("texrect:other_modes")
            texrect_line = dominant_entry("texrect:tile_line")
            texrect_width = dominant_entry("texrect:texture_image_width")
            if tex_write_hit > 0:
                if texrect_combine[0] is not None and texrect_combine[3] is not None and texrect_combine[3] > 0.90:
                    suspected_gaps.append(
                        "missing-region texrect writes are dominated by combine_mux "
                        f"{texrect_combine[0]} ({texrect_combine[1]}/{texrect_combine[2]} hits)"
                    )
                if texrect_modes[0] is not None and texrect_modes[3] is not None and texrect_modes[3] > 0.90:
                    suspected_gaps.append(
                        "missing-region texrect writes are dominated by other_modes "
                        f"{texrect_modes[0]} ({texrect_modes[1]}/{texrect_modes[2]} hits)"
                    )
                if texrect_line[0] is not None and texrect_line[3] is not None and texrect_line[3] > 0.80:
                    suspected_gaps.append(
                        "missing-region texrect writes are dominated by tile_line "
                        f"{texrect_line[0]} ({texrect_line[1]}/{texrect_line[2]} hits)"
                    )
                if texrect_width[0] is not None and texrect_width[3] is not None and texrect_width[3] > 0.80:
                    suspected_gaps.append(
                        "missing-region texrect writes are dominated by texture_image_width "
                        f"{texrect_width[0]} ({texrect_width[1]}/{texrect_width[2]} hits)"
                    )

        if isinstance(write_coverage, dict):
            segments = write_coverage.get("segments", {})
            if isinstance(segments, dict):
                left = segments.get("left", {}) if isinstance(segments.get("left"), dict) else {}
                center = segments.get("center", {}) if isinstance(segments.get("center"), dict) else {}
                right = segments.get("right", {}) if isinstance(segments.get("right"), dict) else {}
                left_box_ratio = left.get("write_source_box_ratio")
                center_box_ratio = center.get("write_source_box_ratio")
                right_box_ratio = right.get("write_source_box_ratio")
                if (
                    isinstance(left_box_ratio, (int, float))
                    and isinstance(center_box_ratio, (int, float))
                    and left_box_ratio < 0.15
                    and center_box_ratio > 0.35
                    and left_box_ratio + 0.20 < center_box_ratio
                ):
                    suspected_gaps.append(
                        "missing-region write coverage is left-strip starved relative to center (upstream primitive coverage gap likely)"
                    )
                if (
                    isinstance(left_box_ratio, (int, float))
                    and isinstance(right_box_ratio, (int, float))
                    and left_box_ratio < 0.15
                    and right_box_ratio > 0.30
                    and left_box_ratio + 0.15 < right_box_ratio
                ):
                    suspected_gaps.append(
                        "missing-region write coverage is asymmetric with weak left-strip occupancy (check viewport/scissor/triangle edge stepping)"
                    )

        if isinstance(missing_write_attribution, dict):
            packet_hits_raw = missing_write_attribution.get("missing_with_write_packet_hits", [])
            packet_hits: List[Dict[str, Any]] = (
                [row for row in packet_hits_raw if isinstance(row, dict)]
                if isinstance(packet_hits_raw, list)
                else []
            )
            packet_rows_considered = min(32, len(packet_hits))
            packet_rows_matched = 0
            for row in packet_hits[:32]:
                source_packet_id = int(row.get("source_packet_id", 0) or 0)
                stage_row = overwrite_source_packet_stage_by_id.get(source_packet_id, {})
                merged_row: Dict[str, Any] = {
                    "source_packet_id": source_packet_id,
                    "frame_id": int(row.get("frame_id", frame_id) or frame_id or 0),
                    "op_kind": row.get("op_kind"),
                    "phase": row.get("phase"),
                    "color_image_address": int(row.get("color_image_address", 0) or 0),
                    "color_image_address_hex": row.get("color_image_address_hex"),
                    "combine_mux": row.get("combine_mux"),
                    "other_modes": row.get("other_modes"),
                    "blend_params": row.get("blend_params"),
                    "tile_format": int(row.get("tile_format", 0) or 0),
                    "tile_size": int(row.get("tile_size", 0) or 0),
                    "tile_line": int(row.get("tile_line", 0) or 0),
                    "tile_tmem": int(row.get("tile_tmem", 0) or 0),
                    "texture_image_width": int(row.get("texture_image_width", 0) or 0),
                    "texture_image_address": row.get("texture_image_address"),
                    "missing_work_hits": int(row.get("work_hits", 0) or 0),
                    "missing_pixel_hits": int(row.get("pixel_hits", 0) or 0),
                    "missing_pixel_hit_ratio": row.get("pixel_hit_ratio"),
                    "overwrite_log_stage_coverage": "missing",
                }
                if stage_row:
                    packet_rows_matched += 1
                    merged_row["overwrite_log_stage_coverage"] = "matched"
                    merged_row["overwrite_record_count"] = int(stage_row.get("overwrite_count", 0) or 0)
                    merged_row["log_record_count"] = int(stage_row.get("record_count", 0) or 0)
                    merged_row["overwrite_ratio_of_packet_records"] = stage_row.get(
                        "overwrite_ratio_of_packet_records"
                    )
                    merged_row["final_black_ratio"] = stage_row.get("final_black_ratio")
                    merged_row["texel_black_ratio"] = stage_row.get("texel_black_ratio")
                    merged_row["combiner_black_ratio"] = stage_row.get("combiner_black_ratio")
                    merged_row["blender_black_ratio"] = stage_row.get("blender_black_ratio")
                    merged_row["kill_at_combiner_ratio"] = stage_row.get("kill_at_combiner_ratio")
                    merged_row["kill_at_blender_ratio"] = stage_row.get("kill_at_blender_ratio")
                    merged_row["kill_after_blender_ratio"] = stage_row.get("kill_after_blender_ratio")
                    merged_row["tex0_sample_black_ratio"] = stage_row.get("tex0_sample_black_ratio")
                    merged_row["tex0_final_black_ratio"] = stage_row.get("tex0_final_black_ratio")
                    merged_row["tex0_rdram_probe_non_black_ratio"] = stage_row.get(
                        "tex0_rdram_probe_non_black_ratio"
                    )
                    merged_row["tex0_rdram_probe_beats_tmem_ratio"] = stage_row.get(
                        "tex0_rdram_probe_beats_tmem_ratio"
                    )
                    merged_row["op_counts"] = (
                        stage_row.get("op_counts", {})
                        if isinstance(stage_row.get("op_counts"), dict)
                        else {}
                    )
                    merged_row["phase_counts"] = (
                        stage_row.get("phase_counts", {})
                        if isinstance(stage_row.get("phase_counts"), dict)
                        else {}
                    )
                missing_with_write_packet_stage_attribution.append(merged_row)

            missing_with_write_packet_stage_summary = {
                "candidate_packet_count": len(packet_hits),
                "rows_considered": packet_rows_considered,
                "rows_matched_to_overwrite_log": packet_rows_matched,
                "rows_missing_from_overwrite_log": max(0, packet_rows_considered - packet_rows_matched),
            }
            missing_region_signal["missing_with_write_packet_stage_attribution"] = (
                missing_with_write_packet_stage_attribution
            )
            missing_region_signal["missing_with_write_packet_stage_summary"] = (
                missing_with_write_packet_stage_summary
            )

            if packet_rows_considered > 0 and packet_rows_matched == 0:
                suspected_gaps.append(
                    "missing-region hotspot packets are absent from overwrite log stage profiles; widen overwrite logging filters or include all writes"
                )
            for row in missing_with_write_packet_stage_attribution[:4]:
                if row.get("overwrite_log_stage_coverage") != "matched":
                    continue
                kill_at_combiner_ratio = row.get("kill_at_combiner_ratio")
                texel_black_ratio = row.get("texel_black_ratio")
                if isinstance(kill_at_combiner_ratio, (int, float)) and kill_at_combiner_ratio > 0.30:
                    suspected_gaps.append(
                        "top missing-region packet "
                        f"{int(row.get('source_packet_id', 0) or 0)} goes black at combiner in >30% of logged writes"
                    )
                    break
                if isinstance(texel_black_ratio, (int, float)) and texel_black_ratio > 0.70:
                    suspected_gaps.append(
                        "top missing-region packet "
                        f"{int(row.get('source_packet_id', 0) or 0)} is already black at texel stage in >70% of logged writes"
                    )
                    break

            missing_without_write_ratio = missing_write_attribution.get("missing_without_write_ratio")
            missing_with_write_ratio = missing_write_attribution.get("missing_with_write_ratio")
            if isinstance(missing_without_write_ratio, (int, float)) and missing_without_write_ratio > 0.60:
                suspected_gaps.append(
                    "most missing non-black pixels are not covered by any write bounds in the focus frame (upstream draw/work coverage gap likely)"
                )
            if isinstance(missing_with_write_ratio, (int, float)) and missing_with_write_ratio > 0.40:
                suspected_gaps.append(
                    "a substantial fraction of missing pixels are inside write bounds (texel/combiner lane still contributes to divergence)"
                )
            segments = missing_write_attribution.get("segments", {})
            if isinstance(segments, dict):
                left_seg = segments.get("left", {}) if isinstance(segments.get("left"), dict) else {}
                center_seg = segments.get("center", {}) if isinstance(segments.get("center"), dict) else {}
                left_unwritten = left_seg.get("missing_without_write_ratio")
                center_unwritten = center_seg.get("missing_without_write_ratio")
                if (
                    isinstance(left_unwritten, (int, float))
                    and isinstance(center_unwritten, (int, float))
                    and left_unwritten > 0.85
                    and center_unwritten + 0.20 < left_unwritten
                ):
                    suspected_gaps.append(
                        "left-side missing pixels are predominantly unwritten versus center (missing geometry/primitive coverage on left strip)"
                    )
            if isinstance(missing_write_history, dict):
                missing_without_current_with_prior_ratio = missing_write_history.get(
                    "missing_without_current_with_prior_write_ratio"
                )
                missing_without_current_without_prior_ratio = missing_write_history.get(
                    "missing_without_current_without_prior_write_ratio"
                )
                present_prior_overlap_ratio = missing_write_history.get("present_surface_prior_overlap_ratio")
                dominant_prior = (
                    missing_write_history.get("dominant_prior_overlap_address")
                    if isinstance(missing_write_history.get("dominant_prior_overlap_address"), dict)
                    else {}
                )
                dominant_prior_ratio = dominant_prior.get("missing_overlap_ratio")
                dominant_prior_is_present = bool(dominant_prior.get("is_present_surface", False))
                if (
                    isinstance(missing_without_current_with_prior_ratio, (int, float))
                    and missing_without_current_with_prior_ratio > 0.45
                ):
                    suspected_gaps.append(
                        "many missing pixels are unwritten in the focus frame but were written in prior frames (carry-forward/handoff dependency)"
                    )
                if (
                    isinstance(missing_without_current_without_prior_ratio, (int, float))
                    and missing_without_current_without_prior_ratio > 0.55
                ):
                    suspected_gaps.append(
                        "most unwritten missing pixels were not written even in recent history (likely absent primitive coverage, not handoff)"
                    )
                if (
                    isinstance(dominant_prior_ratio, (int, float))
                    and dominant_prior_ratio > 0.35
                    and not dominant_prior_is_present
                    and isinstance(present_prior_overlap_ratio, (int, float))
                    and present_prior_overlap_ratio + 0.15 < dominant_prior_ratio
                ):
                    suspected_gaps.append(
                        "prior-frame missing overlap is dominated by a non-present surface target (present-surface handoff/composition mismatch)"
                    )
                prior_overlap_rows = (
                    missing_write_history.get("prior_address_overlap_rows")
                    if isinstance(missing_write_history.get("prior_address_overlap_rows"), list)
                    else []
                )
                strong_prior_overlap_rows = 0
                for row in prior_overlap_rows:
                    if not isinstance(row, dict):
                        continue
                    overlap_ratio = row.get("missing_overlap_ratio")
                    if isinstance(overlap_ratio, (int, float)) and overlap_ratio > 0.80:
                        strong_prior_overlap_rows += 1
                if strong_prior_overlap_rows >= 2:
                    suspected_gaps.append(
                        "unwritten missing pixels overlap strongly with multiple prior color-image targets (cross-surface carry-forward composition likely required)"
                    )

        if address_write_stats:
            unique_targets = (
                int(color_image_sequence.get("unique_target_count", 0) or 0)
                if isinstance(color_image_sequence, dict)
                else 0
            )
            if unique_targets >= 3:
                suspected_gaps.append(
                    "missing-region focus sees >=3 color-image targets in one frame (buffer-rotation handoff needs per-address validation)"
                )

            max_tex_box = 0
            max_tri_box = 0
            for row in address_write_stats:
                max_tex_box = max(max_tex_box, int(row.get("texrect_write_source_box_pixels", 0) or 0))
                max_tri_box = max(max_tri_box, int(row.get("triangle_write_source_box_pixels", 0) or 0))
            if max_tex_box > 0 and max_tri_box * 3 < max_tex_box:
                suspected_gaps.append(
                    "missing-region boxes are dominated by texrect writes while triangle box coverage stays low (missing geometry path likely upstream of raster)"
                )

            if present_surface_focus > 0 and present_surface_address_stats and dominant_missing_address_stats:
                present_box_ratio = present_surface_address_stats.get("write_source_box_ratio")
                dominant_box_ratio = dominant_missing_address_stats.get("write_source_box_ratio")
                present_address = int(present_surface_address_stats.get("color_image_address", 0) or 0)
                dominant_address = int(dominant_missing_address_stats.get("color_image_address", 0) or 0)
                if (
                    isinstance(present_box_ratio, (int, float))
                    and isinstance(dominant_box_ratio, (int, float))
                    and dominant_address != present_address
                    and dominant_box_ratio > 0.20
                    and present_box_ratio + 0.10 < dominant_box_ratio
                ):
                    suspected_gaps.append(
                        "missing-region coverage is stronger on a non-present color-image target (possible present-surface handoff mismatch)"
                    )

        if isinstance(history_window, dict) and history_address_write_stats:
            history_unique_targets = int(history_window.get("unique_target_count", 0) or 0)
            history_frames_analyzed = int(history_window.get("frames_analyzed", 0) or 0)
            if history_frames_analyzed >= 2 and history_unique_targets >= 3:
                suspected_gaps.append(
                    "missing-region history window shows >=3 rotating color-image targets across adjacent frames (carry-forward buffer contents likely required)"
                )
            if present_surface_focus > 0 and present_surface_history_stats and dominant_missing_history_stats:
                present_hist_ratio = present_surface_history_stats.get("write_source_box_ratio")
                dominant_hist_ratio = dominant_missing_history_stats.get("write_source_box_ratio")
                present_hist_addr = int(present_surface_history_stats.get("color_image_address", 0) or 0)
                dominant_hist_addr = int(dominant_missing_history_stats.get("color_image_address", 0) or 0)
                if (
                    isinstance(present_hist_ratio, (int, float))
                    and isinstance(dominant_hist_ratio, (int, float))
                    and dominant_hist_addr != present_hist_addr
                    and dominant_hist_ratio > 0.30
                    and present_hist_ratio + 0.15 < dominant_hist_ratio
                ):
                    suspected_gaps.append(
                        "history-window missing-box coverage is stronger on a non-present target than the VI-selected surface (cross-frame handoff mismatch candidate)"
                    )
        if history_prior_address_write_stats and present_surface_focus > 0:
            present_prior_row: Dict[str, Any] = {}
            dominant_prior_row = max(
                history_prior_address_write_stats,
                key=lambda row: int(row.get("write_source_box_pixels", 0) or 0),
            )
            for row in history_prior_address_write_stats:
                if int(row.get("color_image_address", 0) or 0) == present_surface_focus:
                    present_prior_row = row
                    break
            present_prior_ratio = present_prior_row.get("write_source_box_ratio") if present_prior_row else None
            dominant_prior_ratio = dominant_prior_row.get("write_source_box_ratio")
            dominant_prior_addr = int(dominant_prior_row.get("color_image_address", 0) or 0)
            if (
                isinstance(present_prior_ratio, (int, float))
                and isinstance(dominant_prior_ratio, (int, float))
                and dominant_prior_addr != present_surface_focus
                and dominant_prior_ratio > 0.25
                and present_prior_ratio + 0.12 < dominant_prior_ratio
            ):
                suspected_gaps.append(
                    "prior-window write coverage for missing boxes is stronger on a non-present target than the present surface (frame handoff likely under-selecting carry-forward source)"
                )

    deviation_signal: Dict[str, Any] = {}
    if isinstance(diff_playbook_summary, dict):
        mode = str(diff_playbook_summary.get("mode", "absdiff") or "absdiff")
        metrics_payload = diff_playbook_summary.get("metrics", {})
        first_raw = metrics_payload.get("first_mismatch_raw") if isinstance(metrics_payload, dict) else None
        first_dilated = metrics_payload.get("first_mismatch") if isinstance(metrics_payload, dict) else None
        percent_changed = float(metrics_payload.get("percent_changed", 0.0) or 0.0) if isinstance(metrics_payload, dict) else 0.0
        pixels_analyzed = int(metrics_payload.get("pixels_analyzed", 0) or 0) if isinstance(metrics_payload, dict) else 0
        pixels_ignored_raw = int(metrics_payload.get("pixels_ignored_raw", 0) or 0) if isinstance(metrics_payload, dict) else 0
        missing_non_black = int(metrics_payload.get("missing_non_black_pixels", 0) or 0) if isinstance(metrics_payload, dict) else 0
        extra_non_black = int(metrics_payload.get("extra_non_black_pixels", 0) or 0) if isinstance(metrics_payload, dict) else 0
        missing_non_black_ratio = _ratio(missing_non_black, pixels_analyzed)
        extra_non_black_ratio = _ratio(extra_non_black, pixels_analyzed)
        box_count = int(diff_playbook_summary.get("box_count", 0) or 0)
        deviation_signal = {
            "mode": mode,
            "threshold": diff_playbook_summary.get("threshold"),
            "min_area": diff_playbook_summary.get("min_area"),
            "dilate": diff_playbook_summary.get("dilate"),
            "box_count": box_count,
            "pixels_analyzed": pixels_analyzed,
            "pixels_ignored_raw": pixels_ignored_raw,
            "percent_changed": percent_changed,
            "missing_non_black_pixels": missing_non_black,
            "missing_non_black_ratio": missing_non_black_ratio,
            "extra_non_black_pixels": extra_non_black,
            "extra_non_black_ratio": extra_non_black_ratio,
            "first_mismatch_raw": first_raw,
            "first_mismatch": first_dilated,
        }
        if mode == "missing_non_black" and missing_non_black_ratio is not None and missing_non_black_ratio > 0.20:
            suspected_gaps.append(
                "deviation playbook reports >20% analyzed pixels where reference is non-black and candidate is black (missing geometry/texture draw likely)"
            )
        if mode == "extra_non_black" and extra_non_black_ratio is not None and extra_non_black_ratio > 0.10:
            suspected_gaps.append(
                "deviation playbook reports >10% analyzed pixels where candidate is non-black and reference is black (overdraw/present source mismatch likely)"
            )
        if pixels_ignored_raw > 0:
            suspected_gaps.append("deviation playbook ignored configured hotspot region(s); review ignored boxes when comparing runs")
        if box_count == 0 and percent_changed == 0.0:
            suspected_gaps.append("image diff playbook found no structural mismatch (check threshold or capture mismatch)")

    if int(launch_summary.get("readback_marker_count", 0) or 0) == 0:
        suspected_gaps.append("launch log contains no VK readback debug markers")

    return {
        "present": present_signal,
        "texture": texture_signal,
        "geometry": geometry_signal,
        "depth": depth_signal,
        "history_merge": history_merge_signal,
        "overwrite": overwrite_signal,
        "visibility": visibility_signal,
        "command": command_signal,
        "missing_region": missing_region_signal,
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
        "selected_surface_history_age",
        "selected_surface_overwrite_black",
        "selected_surface_overwrite_black_texrect",
        "selected_surface_overwrite_black_triangle",
        "selected_surface_triangle_preserve_non_black",
        "selected_surface_texrect_nonblack",
        "selected_surface_triangle_nonblack",
        "selected_surface_untouched_carry",
        "selected_surface_untouched_carry_src",
        "selected_surface_history_merge_candidates",
        "selected_surface_history_merge_potential_black_fill",
        "selected_surface_history_merge_potential_nonblack_diff",
        "selected_surface_history_merge_copied",
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
    parser.add_argument("--missing-region-focus")
    parser.add_argument("--history-merge-log")
    parser.add_argument("--overwrite-log")
    parser.add_argument("--executor-present-dump")
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
    missing_region_focus_path = Path(args.missing_region_focus) if args.missing_region_focus else None
    history_merge_log_path = Path(args.history_merge_log) if args.history_merge_log else None
    overwrite_log_path = Path(args.overwrite_log) if args.overwrite_log else None
    executor_present_dump_path = Path(args.executor_present_dump) if args.executor_present_dump else None
    command_census_path = Path(args.command_census) if args.command_census else None

    metrics = _load_json(metrics_path)
    capture_context = _load_json(capture_context_path)
    replay = _load_json(packet_replay)
    depth_summary = _load_json(depth_summary_path)
    diff_playbook_summary = _load_json(diff_playbook_summary_path)
    diff_playbook_boxes = _load_json_any(diff_playbook_boxes_path)
    diff_playbook_snippet = _load_text(diff_playbook_snippet_path)
    missing_region_focus = _load_json(missing_region_focus_path)
    history_merge_summary = _parse_history_merge_log(history_merge_log_path)
    overwrite_summary = _parse_overwrite_log(overwrite_log_path)
    overwrite_source_profile_summary = _profile_overwrite_source_packets(overwrite_summary, packet_trace)
    executor_present_compare = _compare_executor_present_to_candidate(
        candidate_capture,
        executor_present_dump_path,
    )
    if isinstance(overwrite_summary, dict):
        overwrite_summary["top_source_packet_profiles"] = overwrite_source_profile_summary.get("profiles", [])
        overwrite_summary["source_profile_requested_count"] = int(
            overwrite_source_profile_summary.get("requested_count", 0) or 0
        )
        overwrite_summary["source_profile_resolved_count"] = int(
            overwrite_source_profile_summary.get("resolved_count", 0) or 0
        )
        source_profile_error = overwrite_source_profile_summary.get("error")
        if isinstance(source_profile_error, str) and source_profile_error:
            overwrite_summary["source_profile_error"] = source_profile_error
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
        history_merge_summary,
        overwrite_summary,
        depth_summary,
        metrics,
        command_census,
        diff_playbook_summary,
        missing_region_focus,
        executor_present_compare,
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
            "missing_region_focus": _file_meta(missing_region_focus_path),
            "history_merge_log": _file_meta(history_merge_log_path),
            "overwrite_log": _file_meta(overwrite_log_path),
            "executor_present_dump": _file_meta(executor_present_dump_path),
            "command_census": _file_meta(command_census_path),
        },
        "metrics": metrics,
        "capture_context": capture_context,
        "executor_present_compare": executor_present_compare,
        "deviation_playbook": {
            "summary": diff_playbook_summary,
            "boxes": diff_playbook_boxes if isinstance(diff_playbook_boxes, list) else [],
            "snippet": diff_playbook_snippet,
        },
        "missing_region_focus": missing_region_focus,
        "history_merge_summary": history_merge_summary,
        "overwrite_summary": overwrite_summary,
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
            "history_merge": signal_summary["history_merge"],
            "overwrite": signal_summary["overwrite"],
            "visibility": signal_summary["visibility"],
            "command": signal_summary["command"],
            "missing_region": signal_summary["missing_region"],
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
