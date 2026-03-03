#!/usr/bin/env python3
from __future__ import annotations

import argparse
import importlib.util
import json
import math
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

try:
    from PIL import Image
except Exception:  # pragma: no cover - optional runtime dependency guard
    Image = None


@dataclass
class Box:
    x0: int
    y0: int
    x1: int
    y1: int


def _load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _load_forensics_last_active(path: Optional[Path]) -> Dict[str, int]:
    if path is None or not path.is_file():
        return {}
    last: Dict[str, int] = {}
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        rec: Dict[str, int] = {}
        for token in line.split("\t"):
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            key = key.strip()
            value = value.strip()
            if not key:
                continue
            parsed: Optional[int] = None
            try:
                if value.startswith(("0x", "0X")):
                    parsed = int(value, 16)
                else:
                    parsed = int(value, 10)
            except ValueError:
                parsed = None
            if parsed is not None:
                rec[key] = parsed
        if rec.get("present_w", 0) > 0 and rec.get("present_h", 0) > 0:
            last = rec
    return last


def _load_packet_replay_module(path: Path):
    spec = importlib.util.spec_from_file_location("rvk2_packet_trace_replay", str(path))
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load parser module from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _sign_extend14(value: int) -> int:
    raw = value & 0x3FFF
    return (raw ^ 0x2000) - 0x2000


def _triangle_bounds(work: Any) -> Tuple[float, float, float, float]:
    yh_signed = _sign_extend14(int(work.triangle_yh))
    ym_signed = _sign_extend14(int(work.triangle_ym))
    yl_signed = _sign_extend14(int(work.triangle_yl))
    yh = float(yh_signed) * 0.25
    ym = float(ym_signed) * 0.25
    yl = float(yl_signed) * 0.25
    xh = float(int(work.triangle_xh)) / 65536.0
    xl = float(int(work.triangle_xl)) / 65536.0
    x_long_at_yl = (
        float(int(work.triangle_xh))
        + float(int(work.triangle_dxhdy)) * float(yl_signed - yh_signed)
    ) / 65536.0
    x0 = min(xh, xl, x_long_at_yl)
    x1 = max(xh, xl, x_long_at_yl)
    y0 = min(yh, ym, yl)
    y1 = max(yh, ym, yl)
    return (x0, y0, x1, y1)


def _rect_bounds(work: Any) -> Tuple[float, float, float, float]:
    x0 = float(min(int(work.rect_ulx), int(work.rect_lrx)))
    y0 = float(min(int(work.rect_uly), int(work.rect_lry)))
    x1 = float(max(int(work.rect_ulx), int(work.rect_lrx)))
    y1 = float(max(int(work.rect_uly), int(work.rect_lry)))
    return (x0, y0, x1, y1)


def _intersects(a: Tuple[float, float, float, float], b: Tuple[int, int, int, int]) -> bool:
    ax0, ay0, ax1, ay1 = a
    bx0, by0, bx1, by1 = b
    if ax1 < float(bx0):
        return False
    if float(bx1) < ax0:
        return False
    if ay1 < float(by0):
        return False
    if float(by1) < ay0:
        return False
    return True


def _ratio(numer: int, denom: int) -> Optional[float]:
    if denom <= 0:
        return None
    return float(numer) / float(denom)


def _segment_ranges(width: int) -> Dict[str, Tuple[int, int]]:
    if width <= 0:
        return {}
    x1 = width // 3
    x2 = (2 * width) // 3
    if x1 <= 0:
        x1 = min(width, 1)
    if x2 <= x1:
        x2 = min(width, x1 + 1)
    return {
        "left": (0, x1),
        "center": (x1, x2),
        "right": (x2, width),
    }


def _normalize_int_bounds(
    bounds: Tuple[float, float, float, float],
    width: int,
    height: int,
) -> Optional[Tuple[int, int, int, int]]:
    if width <= 0 or height <= 0:
        return None
    x0f, y0f, x1f, y1f = bounds
    x0 = int(math.floor(min(x0f, x1f)))
    y0 = int(math.floor(min(y0f, y1f)))
    x1 = int(math.floor(max(x0f, x1f)))
    y1 = int(math.floor(max(y0f, y1f)))
    x0 = max(0, min(width - 1, x0))
    y0 = max(0, min(height - 1, y0))
    x1 = max(0, min(width - 1, x1))
    y1 = max(0, min(height - 1, y1))
    if x1 < x0 or y1 < y0:
        return None
    return (x0, y0, x1, y1)


def _mark_mask_bounds(
    mask: bytearray,
    bounds: Tuple[float, float, float, float],
    width: int,
    height: int,
) -> int:
    rect = _normalize_int_bounds(bounds, width, height)
    if rect is None:
        return 0
    x0, y0, x1, y1 = rect
    painted = 0
    for y in range(y0, y1 + 1):
        row = y * width
        for x in range(x0, x1 + 1):
            idx = row + x
            if mask[idx] == 0:
                mask[idx] = 1
                painted += 1
    return painted


def _count_mask_segment(mask: bytearray, width: int, height: int, x0: int, x1: int) -> int:
    if width <= 0 or height <= 0:
        return 0
    x0 = max(0, min(width, x0))
    x1 = max(0, min(width, x1))
    if x1 <= x0:
        return 0
    total = 0
    for y in range(height):
        row = y * width
        total += int(sum(mask[row + x0 : row + x1]))
    return total


def _count_mask_and(mask_a: bytearray, mask_b: bytearray) -> int:
    if len(mask_a) != len(mask_b):
        return 0
    total = 0
    for idx in range(len(mask_a)):
        if mask_a[idx] != 0 and mask_b[idx] != 0:
            total += 1
    return total


def _count_mask_and_segment(
    mask_a: bytearray,
    mask_b: bytearray,
    width: int,
    height: int,
    x0: int,
    x1: int,
) -> int:
    if len(mask_a) != len(mask_b):
        return 0
    if width <= 0 or height <= 0:
        return 0
    x0 = max(0, min(width, x0))
    x1 = max(0, min(width, x1))
    if x1 <= x0:
        return 0
    total = 0
    for y in range(height):
        row = y * width
        for x in range(x0, x1):
            idx = row + x
            if mask_a[idx] != 0 and mask_b[idx] != 0:
                total += 1
    return total


def _build_missing_image_mask(
    summary: Dict[str, Any],
    image_width: int,
    image_height: int,
) -> Optional[Tuple[bytearray, Dict[str, Any]]]:
    if Image is None:
        return None

    ref_path_raw = summary.get("reference_image")
    test_path_raw = summary.get("candidate_image")
    if not isinstance(ref_path_raw, str) or not ref_path_raw:
        return None
    if not isinstance(test_path_raw, str) or not test_path_raw:
        return None

    ref_path = Path(ref_path_raw)
    test_path = Path(test_path_raw)
    if not ref_path.is_file() or not test_path.is_file():
        return None

    try:
        ref_img = Image.open(ref_path).convert("RGB")
        test_img = Image.open(test_path).convert("RGB")
    except Exception:
        return None

    if ref_img.size != test_img.size:
        return None
    if ref_img.size != (image_width, image_height):
        return None

    ref_non_black_threshold = int(summary.get("ref_non_black_threshold", 8) or 8)
    test_non_black_threshold = int(summary.get("test_non_black_threshold", 8) or 8)
    ref_non_black_threshold = max(0, min(255, ref_non_black_threshold))
    test_non_black_threshold = max(0, min(255, test_non_black_threshold))

    ignore_mask = bytearray(image_width * image_height)
    ignore_boxes = summary.get("ignore_boxes", [])
    if isinstance(ignore_boxes, list):
        for raw_box in ignore_boxes:
            if not isinstance(raw_box, dict):
                continue
            try:
                x0 = int(raw_box.get("x0", 0))
                y0 = int(raw_box.get("y0", 0))
                x1 = int(raw_box.get("x1", 0))
                y1 = int(raw_box.get("y1", 0))
            except Exception:
                continue
            x0 = max(0, min(image_width - 1, x0))
            y0 = max(0, min(image_height - 1, y0))
            x1 = max(0, min(image_width - 1, x1))
            y1 = max(0, min(image_height - 1, y1))
            if x1 < x0 or y1 < y0:
                continue
            for y in range(y0, y1 + 1):
                row = y * image_width
                for x in range(x0, x1 + 1):
                    ignore_mask[row + x] = 1

    ref_bytes = ref_img.tobytes()
    test_bytes = test_img.tobytes()
    missing_mask = bytearray(image_width * image_height)
    for idx in range(image_width * image_height):
        if ignore_mask[idx] != 0:
            continue
        base = idx * 3
        rr = ref_bytes[base + 0]
        rg = ref_bytes[base + 1]
        rb = ref_bytes[base + 2]
        tr = test_bytes[base + 0]
        tg = test_bytes[base + 1]
        tb = test_bytes[base + 2]
        ref_non_black = (
            rr > ref_non_black_threshold
            or rg > ref_non_black_threshold
            or rb > ref_non_black_threshold
        )
        test_non_black = (
            tr > test_non_black_threshold
            or tg > test_non_black_threshold
            or tb > test_non_black_threshold
        )
        if ref_non_black and not test_non_black:
            missing_mask[idx] = 1

    meta = {
        "reference_image": str(ref_path),
        "candidate_image": str(test_path),
        "ref_non_black_threshold": ref_non_black_threshold,
        "test_non_black_threshold": test_non_black_threshold,
        "ignore_box_count": len(ignore_boxes) if isinstance(ignore_boxes, list) else 0,
    }
    return missing_mask, meta


def _map_image_mask_to_source_mask(
    image_mask: bytearray,
    image_width: int,
    image_height: int,
    source_width: int,
    source_height: int,
) -> bytearray:
    source_mask = bytearray(source_width * source_height)
    if image_width <= 0 or image_height <= 0 or source_width <= 0 or source_height <= 0:
        return source_mask

    for sy in range(source_height):
        y0 = int(math.floor(float(sy) * float(image_height) / float(source_height)))
        y1 = int(math.ceil(float(sy + 1) * float(image_height) / float(source_height)))
        y0 = max(0, min(image_height - 1, y0))
        y1 = max(y0 + 1, min(image_height, y1))
        for sx in range(source_width):
            x0 = int(math.floor(float(sx) * float(image_width) / float(source_width)))
            x1 = int(math.ceil(float(sx + 1) * float(image_width) / float(source_width)))
            x0 = max(0, min(image_width - 1, x0))
            x1 = max(x0 + 1, min(image_width, x1))
            found = False
            for py in range(y0, y1):
                row = py * image_width
                for px in range(x0, x1):
                    if image_mask[row + px] != 0:
                        found = True
                        break
                if found:
                    break
            if found:
                source_mask[sy * source_width + sx] = 1
    return source_mask


def _op_kind_name(op_kind: int) -> str:
    if op_kind == 0:
        return "fill"
    if op_kind == 1:
        return "triangle"
    if op_kind == 2:
        return "texrect"
    return f"op{op_kind}"


def _phase_name(phase: int) -> str:
    if phase == 1:
        return "cycle1"
    if phase == 2:
        return "cycle2"
    if phase == 3:
        return "copy"
    if phase == 4:
        return "fill"
    return f"phase{phase}"


def _compute_effective_write_bounds(
    work: Any,
    clip_width: int,
    clip_height: int,
) -> Optional[Tuple[float, float, float, float]]:
    if clip_width <= 0 or clip_height <= 0:
        return None

    ulx = min(int(work.rect_ulx), int(work.rect_lrx))
    uly = min(int(work.rect_uly), int(work.rect_lry))
    lrx = max(int(work.rect_ulx), int(work.rect_lrx))
    lry = max(int(work.rect_uly), int(work.rect_lry))
    if lrx < ulx or lry < uly:
        return None

    default_scissor = (
        int(work.scissor_xh) == 0
        and int(work.scissor_yh) == 0
        and int(work.scissor_xl) == 0
        and int(work.scissor_yl) == 0
    )
    if default_scissor:
        scissor_x0 = ulx
        scissor_y0 = uly
        scissor_x1 = lrx
        scissor_y1 = lry
    else:
        scissor_x0 = min(int(work.scissor_xh), int(work.scissor_xl))
        scissor_y0 = min(int(work.scissor_yh), int(work.scissor_yl))
        scissor_x1 = max(int(work.scissor_xh), int(work.scissor_xl))
        scissor_y1 = max(int(work.scissor_yh), int(work.scissor_yl))
        # Match executor behavior: lower edge is exclusive.
        if scissor_y1 == 0:
            return None
        scissor_y1 -= 1
        # Right edge is exclusive for cycle phases and inclusive for copy/fill.
        inclusive_right_edge = int(work.phase) in (3, 4)
        if not inclusive_right_edge:
            if scissor_x1 == 0:
                return None
            scissor_x1 -= 1

    max_x = max(0, clip_width - 1)
    max_y = max(0, clip_height - 1)
    write_x0 = max(ulx, scissor_x0)
    write_y0 = max(uly, scissor_y0)
    write_x1 = min(lrx, min(scissor_x1, max_x))
    write_y1 = min(lry, min(scissor_y1, max_y))
    if write_x1 < write_x0 or write_y1 < write_y0:
        return None
    return (float(write_x0), float(write_y0), float(write_x1), float(write_y1))


def _to_source_box(
    box: Box,
    image_width: int,
    image_height: int,
    source_width: int,
    source_height: int,
) -> Tuple[int, int, int, int]:
    sx0 = max(0.0, min(float(source_width - 1), float(box.x0) * float(source_width) / float(image_width)))
    sy0 = max(0.0, min(float(source_height - 1), float(box.y0) * float(source_height) / float(image_height)))
    sx1 = max(0.0, min(float(source_width - 1), float(box.x1) * float(source_width) / float(image_width)))
    sy1 = max(0.0, min(float(source_height - 1), float(box.y1) * float(source_height) / float(image_height)))
    return (
        int(math.floor(min(sx0, sx1))),
        int(math.floor(min(sy0, sy1))),
        int(math.ceil(max(sx0, sx1))),
        int(math.ceil(max(sy0, sy1))),
    )


def _load_boxes(summary: Dict[str, Any], summary_path: Path) -> List[Box]:
    boxes_path_raw = summary.get("artifact_files", {}).get("boxes")
    if not isinstance(boxes_path_raw, str) or not boxes_path_raw:
        raise RuntimeError(f"diff summary missing artifact_files.boxes: {summary_path}")
    boxes_path = Path(boxes_path_raw)
    boxes_json = _load_json(boxes_path)
    if not isinstance(boxes_json, list):
        return []
    out: List[Box] = []
    for item in boxes_json:
        if not isinstance(item, dict):
            continue
        try:
            out.append(
                Box(
                    x0=int(item.get("x0", 0)),
                    y0=int(item.get("y0", 0)),
                    x1=int(item.get("x1", 0)),
                    y1=int(item.get("y1", 0)),
                )
            )
        except Exception:
            continue
    return out


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Correlate diff playbook boxes with render-work packets to identify which "
            "primitive families and texture buckets intersect missing regions."
        )
    )
    parser.add_argument("--packet-trace", required=True, help="packet trace TSV path")
    parser.add_argument("--diff-summary", required=True, help="diff playbook summary.json path")
    parser.add_argument("--output", required=True, help="output JSON path")
    parser.add_argument("--forensics", default="", help="optional frame-forensics TSV path")
    parser.add_argument("--frame-id", type=int, default=0, help="frame id to analyze (default: last frame with render work)")
    parser.add_argument(
        "--max-hit-samples",
        type=int,
        default=256,
        help="max intersecting work samples to emit",
    )
    parser.add_argument(
        "--history-frame-window",
        type=int,
        default=2,
        help=(
            "number of prior render-work frames to include for color-image address history "
            "(0 analyzes selected frame only)"
        ),
    )
    parser.add_argument(
        "--max-address-overlap",
        type=int,
        default=16,
        help="max address overlap rows to emit for missing-write attribution",
    )
    args = parser.parse_args()

    packet_trace_path = Path(args.packet_trace)
    diff_summary_path = Path(args.diff_summary)
    output_path = Path(args.output)
    forensics_path = Path(args.forensics) if args.forensics else None

    if not packet_trace_path.is_file():
        raise SystemExit(f"ERROR: packet trace not found: {packet_trace_path}")
    if not diff_summary_path.is_file():
        raise SystemExit(f"ERROR: diff summary not found: {diff_summary_path}")

    summary = _load_json(diff_summary_path)
    if not isinstance(summary, dict):
        raise SystemExit(f"ERROR: invalid diff summary JSON: {diff_summary_path}")
    metrics = summary.get("metrics", {}) if isinstance(summary.get("metrics"), dict) else {}
    image_width = int(metrics.get("width", 0) or 0)
    image_height = int(metrics.get("height", 0) or 0)
    if image_width <= 0 or image_height <= 0:
        raise SystemExit("ERROR: diff summary missing image dimensions")

    boxes = _load_boxes(summary, diff_summary_path)
    if not boxes:
        payload = {
            "schema": "rvk2_missing_region_focus_v3",
            "packet_trace": str(packet_trace_path),
            "diff_summary": str(diff_summary_path),
            "frame_id": None,
            "source_width": None,
            "source_height": None,
            "source_boxes": [],
            "counts": {},
            "texture_bucket_hits": {},
            "texture_bucket_bbox_hits": {},
            "texture_bucket_write_hits": {},
            "phase_write_hits": {},
            "write_state_hits": {},
            "write_coverage": {},
            "color_image_sequence": {},
            "history_window": {},
            "forensics_last_active": {},
            "address_write_stats": [],
            "work_hit_samples": [],
            "missing_write_attribution": {},
            "work_hit_stats": {},
            "notes": ["no diff boxes available"],
        }
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        print(output_path)
        return 0

    parser_module = _load_packet_replay_module(Path(__file__).with_name("rvk2_packet_trace_replay.py"))
    frames = parser_module.parse_packet_trace(packet_trace_path)

    selected_frame = None
    selected_frame_index = -1
    if args.frame_id > 0:
        for idx, frame in enumerate(frames):
            if int(frame.frame_id) == args.frame_id:
                selected_frame = frame
                selected_frame_index = idx
                break
        if selected_frame is None:
            raise SystemExit(f"ERROR: frame {args.frame_id} not found in packet trace")
    else:
        for idx in range(len(frames) - 1, -1, -1):
            frame = frames[idx]
            if len(frame.render_work) > 0:
                selected_frame = frame
                selected_frame_index = idx
                break
        if selected_frame is None:
            raise SystemExit("ERROR: packet trace has no render-work rows")

    source_width = 0
    source_height = 0
    forensics_last = _load_forensics_last_active(forensics_path)
    if forensics_last:
        source_width = int(forensics_last.get("vi_src_w", 0) or 0)
        source_height = int(forensics_last.get("vi_src_h", 0) or 0)
    if source_width <= 0:
        # Fall back to the largest color-image width in the selected frame.
        source_width = max((int(w.color_image_width) for w in selected_frame.render_work), default=0)
    if source_height <= 0:
        source_height = 240

    source_boxes = [
        _to_source_box(box, image_width, image_height, source_width, source_height)
        for box in boxes
    ]
    source_box_count = len(source_boxes)
    total_pixels = max(0, source_width * source_height)
    source_box_mask = bytearray(total_pixels)
    for source_box in source_boxes:
        _mark_mask_bounds(source_box_mask, source_box, source_width, source_height)
    source_box_pixels = int(sum(source_box_mask))
    segment_ranges = _segment_ranges(source_width)

    counts: Counter[str] = Counter()
    bucket_bbox_hits: Counter[str] = Counter()
    bucket_write_hits: Counter[str] = Counter()
    phase_write_hits: Counter[str] = Counter()
    write_state_counters: Dict[str, Counter[str]] = defaultdict(Counter)
    color_image_work_counts: Counter[int] = Counter()
    color_image_sequence: List[int] = []
    color_image_switch_count = 0
    last_color_image_address: Optional[int] = None
    write_union_mask = bytearray(total_pixels)
    address_write_masks: Dict[int, bytearray] = {}
    address_op_masks: Dict[int, Dict[str, bytearray]] = defaultdict(dict)
    op_union_masks: Dict[str, bytearray] = {}
    address_stats: Dict[int, Counter[str]] = defaultdict(Counter)
    hit_samples: List[Dict[str, Any]] = []
    work_hit_total = 0
    work_hit_bbox_total = 0
    work_hit_write_total = 0
    work_hit_sample_truncated = 0

    for work in selected_frame.render_work:
        op_kind = int(work.op_kind)
        op_name = _op_kind_name(op_kind)
        phase_name = _phase_name(int(work.phase))
        color_image_address = int(work.color_image_address)
        counts[f"work_{op_name}_total"] += 1
        color_image_work_counts[color_image_address] += 1
        if last_color_image_address is None or color_image_address != last_color_image_address:
            if last_color_image_address is not None:
                color_image_switch_count += 1
            if len(color_image_sequence) < 256:
                color_image_sequence.append(color_image_address)
            last_color_image_address = color_image_address

        address_counter = address_stats[color_image_address]
        address_counter["work_total"] += 1
        address_counter[f"work_{op_name}_total"] += 1
        if op_kind == 1:
            bounds = _triangle_bounds(work)
        else:
            bounds = _rect_bounds(work)
        write_bounds = _compute_effective_write_bounds(work, source_width, source_height)
        bbox_hit = any(_intersects(bounds, box) for box in source_boxes)
        write_hit = write_bounds is not None and any(
            _intersects(write_bounds, box) for box in source_boxes
        )
        if bbox_hit:
            counts[f"work_{op_name}_bbox_hit"] += 1
            # Backward-compatible alias for existing telemetry consumers.
            counts[f"work_{op_name}_hit"] += 1
            address_counter["bbox_hit_total"] += 1
            address_counter[f"bbox_hit_{op_name}"] += 1
        if write_hit:
            counts[f"work_{op_name}_write_hit"] += 1
            phase_write_hits[f"{op_name}:{phase_name}"] += 1
            address_counter["write_hit_total"] += 1
            address_counter[f"write_hit_{op_name}"] += 1
            write_state_counters[f"{op_name}:combine_mux"][f"0x{int(work.combine_mux):016X}"] += 1
            write_state_counters[f"{op_name}:blend_params"][f"0x{int(work.blend_params):08X}"] += 1
            write_state_counters[f"{op_name}:other_modes"][f"0x{int(work.other_modes):016X}"] += 1
            write_state_counters[f"{op_name}:tile_line"][str(int(work.tile_line))] += 1
            write_state_counters[f"{op_name}:tile_tmem"][str(int(work.tile_tmem))] += 1
            write_state_counters[f"{op_name}:texture_image_width"][str(int(work.texture_image_width))] += 1
            write_state_counters[f"{op_name}:texture_image_address"][f"0x{int(work.texture_image_address):08X}"] += 1

        if write_bounds is not None and total_pixels > 0:
            _mark_mask_bounds(write_union_mask, write_bounds, source_width, source_height)
            address_mask = address_write_masks.get(color_image_address)
            if address_mask is None:
                address_mask = bytearray(total_pixels)
                address_write_masks[color_image_address] = address_mask
            address_area = _mark_mask_bounds(address_mask, write_bounds, source_width, source_height)
            if address_area > 0:
                address_counter["write_area_pixels"] += address_area
                address_counter[f"write_area_pixels_{op_name}"] += address_area
            op_mask = address_op_masks[color_image_address].get(op_name)
            if op_mask is None:
                op_mask = bytearray(total_pixels)
                address_op_masks[color_image_address][op_name] = op_mask
            _mark_mask_bounds(op_mask, write_bounds, source_width, source_height)
            union_mask = op_union_masks.get(op_name)
            if union_mask is None:
                union_mask = bytearray(total_pixels)
                op_union_masks[op_name] = union_mask
            _mark_mask_bounds(union_mask, write_bounds, source_width, source_height)

        if not bbox_hit and not write_hit:
            continue

        work_hit_total += 1
        if bbox_hit:
            work_hit_bbox_total += 1
        if write_hit:
            work_hit_write_total += 1

        if bool(work.textured):
            bucket_key = f"{op_name}:f{int(work.tile_format)}s{int(work.tile_size)}"
            if int(work.tile_format) == 2 and int(work.tile_size) == 0:
                bucket_key += ":ci4"
            if bbox_hit:
                bucket_bbox_hits[bucket_key] += 1
            if write_hit:
                bucket_write_hits[bucket_key] += 1

        if len(hit_samples) < max(0, int(args.max_hit_samples)):
            hit_samples.append(
                {
                    "source_packet_id": int(work.source_packet_id),
                    "op_kind": op_name,
                    "phase": phase_name,
                    "textured": bool(work.textured),
                    "bbox_hit": bool(bbox_hit),
                    "write_hit": bool(write_hit),
                    "bbox": [
                        round(float(bounds[0]), 4),
                        round(float(bounds[1]), 4),
                        round(float(bounds[2]), 4),
                        round(float(bounds[3]), 4),
                    ],
                    "write_bbox": (
                        None
                        if write_bounds is None
                        else [
                            round(float(write_bounds[0]), 4),
                            round(float(write_bounds[1]), 4),
                            round(float(write_bounds[2]), 4),
                            round(float(write_bounds[3]), 4),
                        ]
                    ),
                    "tile_format": int(work.tile_format),
                    "tile_size": int(work.tile_size),
                    "texture_image_format": int(work.texture_image_format),
                    "texture_image_size": int(work.texture_image_size),
                    "tile_tmem": int(work.tile_tmem),
                    "tile_line": int(work.tile_line),
                    "scissor_mode": int(work.scissor_mode),
                    "scissor_xh": int(work.scissor_xh),
                    "scissor_yh": int(work.scissor_yh),
                    "scissor_xl": int(work.scissor_xl),
                    "scissor_yl": int(work.scissor_yl),
                    "color_image_address": color_image_address,
                }
            )
        else:
            work_hit_sample_truncated += 1

    history_window = max(0, int(args.history_frame_window))
    history_frames: List[Any] = []
    history_frame_pairs: List[Tuple[Any, bool]] = []
    if selected_frame_index >= 0:
        first_index = max(0, selected_frame_index - history_window)
        for idx in range(first_index, selected_frame_index + 1):
            frame = frames[idx]
            if len(frame.render_work) > 0:
                history_frames.append(frame)
                history_frame_pairs.append((frame, idx == selected_frame_index))

    history_color_image_sequence: List[int] = []
    history_color_image_switch_count = 0
    history_last_color_image_address: Optional[int] = None
    history_color_image_work_counts: Counter[int] = Counter()
    history_address_stats: Dict[int, Counter[str]] = defaultdict(Counter)
    history_address_masks: Dict[int, bytearray] = {}
    history_address_op_masks: Dict[int, Dict[str, bytearray]] = defaultdict(dict)
    history_union_mask = bytearray(total_pixels)
    history_op_union_masks: Dict[str, bytearray] = {}
    history_prior_address_stats: Dict[int, Counter[str]] = defaultdict(Counter)
    history_prior_address_masks: Dict[int, bytearray] = {}
    history_prior_address_op_masks: Dict[int, Dict[str, bytearray]] = defaultdict(dict)
    history_prior_union_mask = bytearray(total_pixels)
    history_prior_op_union_masks: Dict[str, bytearray] = {}
    history_prior_frame_ids: List[int] = []

    for frame, is_selected in history_frame_pairs:
        if not is_selected:
            history_prior_frame_ids.append(int(frame.frame_id))
        for work in frame.render_work:
            op_name = _op_kind_name(int(work.op_kind))
            color_image_address = int(work.color_image_address)
            history_color_image_work_counts[color_image_address] += 1
            if history_last_color_image_address is None or color_image_address != history_last_color_image_address:
                if history_last_color_image_address is not None:
                    history_color_image_switch_count += 1
                if len(history_color_image_sequence) < 512:
                    history_color_image_sequence.append(color_image_address)
                history_last_color_image_address = color_image_address

            history_counter = history_address_stats[color_image_address]
            history_counter["work_total"] += 1
            history_counter[f"work_{op_name}_total"] += 1
            if not is_selected:
                prior_counter = history_prior_address_stats[color_image_address]
                prior_counter["work_total"] += 1
                prior_counter[f"work_{op_name}_total"] += 1

            write_bounds = _compute_effective_write_bounds(work, source_width, source_height)
            if write_bounds is None or total_pixels <= 0:
                continue

            _mark_mask_bounds(history_union_mask, write_bounds, source_width, source_height)
            history_union_op_mask = history_op_union_masks.get(op_name)
            if history_union_op_mask is None:
                history_union_op_mask = bytearray(total_pixels)
                history_op_union_masks[op_name] = history_union_op_mask
            _mark_mask_bounds(history_union_op_mask, write_bounds, source_width, source_height)

            mask = history_address_masks.get(color_image_address)
            if mask is None:
                mask = bytearray(total_pixels)
                history_address_masks[color_image_address] = mask
            painted = _mark_mask_bounds(mask, write_bounds, source_width, source_height)
            if painted > 0:
                history_counter["write_area_pixels"] += painted
                history_counter[f"write_area_pixels_{op_name}"] += painted

            op_mask = history_address_op_masks[color_image_address].get(op_name)
            if op_mask is None:
                op_mask = bytearray(total_pixels)
                history_address_op_masks[color_image_address][op_name] = op_mask
            _mark_mask_bounds(op_mask, write_bounds, source_width, source_height)

            if is_selected:
                continue

            _mark_mask_bounds(history_prior_union_mask, write_bounds, source_width, source_height)
            prior_union_op_mask = history_prior_op_union_masks.get(op_name)
            if prior_union_op_mask is None:
                prior_union_op_mask = bytearray(total_pixels)
                history_prior_op_union_masks[op_name] = prior_union_op_mask
            _mark_mask_bounds(prior_union_op_mask, write_bounds, source_width, source_height)

            prior_mask = history_prior_address_masks.get(color_image_address)
            if prior_mask is None:
                prior_mask = bytearray(total_pixels)
                history_prior_address_masks[color_image_address] = prior_mask
            prior_painted = _mark_mask_bounds(prior_mask, write_bounds, source_width, source_height)
            if prior_painted > 0:
                prior_counter = history_prior_address_stats[color_image_address]
                prior_counter["write_area_pixels"] += prior_painted
                prior_counter[f"write_area_pixels_{op_name}"] += prior_painted

            prior_op_mask = history_prior_address_op_masks[color_image_address].get(op_name)
            if prior_op_mask is None:
                prior_op_mask = bytearray(total_pixels)
                history_prior_address_op_masks[color_image_address][op_name] = prior_op_mask
            _mark_mask_bounds(prior_op_mask, write_bounds, source_width, source_height)

    write_union_pixels = int(sum(write_union_mask))
    write_union_box_pixels = _count_mask_and(write_union_mask, source_box_mask)

    write_coverage_segments: Dict[str, Dict[str, Any]] = {}
    for name, (x0, x1) in segment_ranges.items():
        segment_pixels = max(0, (x1 - x0) * source_height)
        segment_source_box_pixels = _count_mask_segment(source_box_mask, source_width, source_height, x0, x1)
        segment_write_pixels = _count_mask_segment(write_union_mask, source_width, source_height, x0, x1)
        segment_write_box_pixels = _count_mask_and_segment(
            write_union_mask, source_box_mask, source_width, source_height, x0, x1
        )
        write_coverage_segments[name] = {
            "x0": x0,
            "x1_exclusive": x1,
            "pixels": segment_pixels,
            "source_box_pixels": segment_source_box_pixels,
            "write_pixels": segment_write_pixels,
            "write_ratio": _ratio(segment_write_pixels, segment_pixels),
            "write_source_box_pixels": segment_write_box_pixels,
            "write_source_box_ratio": _ratio(segment_write_box_pixels, segment_source_box_pixels),
        }

    missing_write_attribution: Dict[str, Any] = {}
    missing_image_payload = _build_missing_image_mask(summary, image_width, image_height)
    if missing_image_payload is not None and total_pixels > 0:
        missing_image_mask, missing_image_meta = missing_image_payload
        missing_source_mask = _map_image_mask_to_source_mask(
            missing_image_mask,
            image_width,
            image_height,
            source_width,
            source_height,
        )
        missing_source_pixels = int(sum(missing_source_mask))
        missing_with_write_mask = bytearray(total_pixels)
        missing_without_write_mask = bytearray(total_pixels)
        missing_without_write_with_prior_mask = bytearray(total_pixels)
        missing_without_write_without_prior_mask = bytearray(total_pixels)
        for idx in range(total_pixels):
            if missing_source_mask[idx] == 0:
                continue
            if write_union_mask[idx] != 0:
                missing_with_write_mask[idx] = 1
                continue
            missing_without_write_mask[idx] = 1
            if history_prior_union_mask[idx] != 0:
                missing_without_write_with_prior_mask[idx] = 1
            else:
                missing_without_write_without_prior_mask[idx] = 1
        missing_with_write_pixels = int(sum(missing_with_write_mask))
        missing_without_write_pixels = int(sum(missing_without_write_mask))
        missing_with_prior_write_pixels = _count_mask_and(missing_source_mask, history_prior_union_mask)
        missing_without_write_with_prior_pixels = int(sum(missing_without_write_with_prior_mask))
        missing_without_write_without_prior_pixels = int(sum(missing_without_write_without_prior_mask))

        op_cover_pixels: Dict[str, int] = {}
        op_cover_ratios: Dict[str, Optional[float]] = {}
        prior_op_cover_pixels: Dict[str, int] = {}
        prior_op_cover_ratios: Dict[str, Optional[float]] = {}
        history_window_op_cover_pixels: Dict[str, int] = {}
        history_window_op_cover_ratios: Dict[str, Optional[float]] = {}
        for op_name in ("fill", "triangle", "texrect"):
            op_mask = op_union_masks.get(op_name, bytearray(total_pixels))
            op_pixels = _count_mask_and(missing_source_mask, op_mask)
            op_cover_pixels[op_name] = op_pixels
            op_cover_ratios[op_name] = _ratio(op_pixels, missing_source_pixels)
            prior_op_mask = history_prior_op_union_masks.get(op_name, bytearray(total_pixels))
            prior_op_pixels = _count_mask_and(missing_without_write_mask, prior_op_mask)
            prior_op_cover_pixels[op_name] = prior_op_pixels
            prior_op_cover_ratios[op_name] = _ratio(prior_op_pixels, missing_without_write_pixels)
            history_window_op_mask = history_op_union_masks.get(op_name, bytearray(total_pixels))
            history_window_op_pixels = _count_mask_and(missing_source_mask, history_window_op_mask)
            history_window_op_cover_pixels[op_name] = history_window_op_pixels
            history_window_op_cover_ratios[op_name] = _ratio(history_window_op_pixels, missing_source_pixels)

        max_address_overlap = max(0, int(args.max_address_overlap))
        present_surface_address = int(forensics_last.get("present_surface", 0) or 0)

        def _build_address_overlap_rows(
            address_masks: Dict[int, bytearray],
            address_op_masks: Dict[int, Dict[str, bytearray]],
            missing_mask: bytearray,
            missing_pixels: int,
        ) -> Tuple[List[Dict[str, Any]], int]:
            rows: List[Dict[str, Any]] = []
            for color_image_address, address_mask in address_masks.items():
                overlap_pixels = _count_mask_and(address_mask, missing_mask)
                if overlap_pixels <= 0:
                    continue
                op_masks = address_op_masks.get(color_image_address, {})
                op_overlap: Dict[str, int] = {}
                for op_name in ("fill", "triangle", "texrect"):
                    op_mask = op_masks.get(op_name)
                    op_overlap[op_name] = (
                        _count_mask_and(op_mask, missing_mask) if op_mask is not None else 0
                    )
                segment_overlap: Dict[str, Dict[str, Any]] = {}
                for segment_name, (x0, x1) in segment_ranges.items():
                    segment_missing_pixels = _count_mask_segment(
                        missing_mask, source_width, source_height, x0, x1
                    )
                    segment_overlap_pixels = _count_mask_and_segment(
                        address_mask,
                        missing_mask,
                        source_width,
                        source_height,
                        x0,
                        x1,
                    )
                    segment_overlap[segment_name] = {
                        "x0": x0,
                        "x1_exclusive": x1,
                        "missing_pixels": segment_missing_pixels,
                        "missing_overlap_pixels": segment_overlap_pixels,
                        "missing_overlap_ratio": _ratio(
                            segment_overlap_pixels, segment_missing_pixels
                        ),
                    }
                rows.append(
                    {
                        "color_image_address": color_image_address,
                        "color_image_address_hex": f"0x{color_image_address:08X}",
                        "is_present_surface": (
                            present_surface_address != 0 and color_image_address == present_surface_address
                        ),
                        "missing_overlap_pixels": overlap_pixels,
                        "missing_overlap_ratio": _ratio(overlap_pixels, missing_pixels),
                        "missing_overlap_by_op": op_overlap,
                        "segments": segment_overlap,
                    }
                )
            rows.sort(
                key=lambda row: (
                    -int(row.get("missing_overlap_pixels", 0) or 0),
                    int(row.get("color_image_address", 0) or 0),
                )
            )
            full_count = len(rows)
            if max_address_overlap > 0 and len(rows) > max_address_overlap:
                rows = rows[:max_address_overlap]
            truncated = max(0, full_count - len(rows))
            return rows, truncated

        current_address_overlap_rows, current_address_overlap_truncated = _build_address_overlap_rows(
            address_write_masks,
            address_op_masks,
            missing_source_mask,
            missing_source_pixels,
        )
        prior_address_overlap_rows, prior_address_overlap_truncated = _build_address_overlap_rows(
            history_prior_address_masks,
            history_prior_address_op_masks,
            missing_without_write_mask,
            missing_without_write_pixels,
        )

        present_surface_current_overlap_pixels = 0
        present_surface_prior_overlap_pixels = 0
        for row in current_address_overlap_rows:
            if int(row.get("color_image_address", 0) or 0) == present_surface_address:
                present_surface_current_overlap_pixels = int(row.get("missing_overlap_pixels", 0) or 0)
                break
        for row in prior_address_overlap_rows:
            if int(row.get("color_image_address", 0) or 0) == present_surface_address:
                present_surface_prior_overlap_pixels = int(row.get("missing_overlap_pixels", 0) or 0)
                break

        dominant_current_address = current_address_overlap_rows[0] if current_address_overlap_rows else {}
        dominant_prior_address = prior_address_overlap_rows[0] if prior_address_overlap_rows else {}
        history_prior_union_pixels = int(sum(history_prior_union_mask))
        history_union_pixels = int(sum(history_union_mask))
        missing_with_any_window_write_pixels = _count_mask_and(
            missing_source_mask, history_union_mask
        )

        segment_missing: Dict[str, Dict[str, Any]] = {}
        for segment_name, (x0, x1) in segment_ranges.items():
            segment_missing_pixels = _count_mask_segment(
                missing_source_mask, source_width, source_height, x0, x1
            )
            segment_missing_with_write = _count_mask_segment(
                missing_with_write_mask, source_width, source_height, x0, x1
            )
            segment_missing_without_write = _count_mask_segment(
                missing_without_write_mask, source_width, source_height, x0, x1
            )
            segment_missing_with_prior = _count_mask_and_segment(
                missing_source_mask,
                history_prior_union_mask,
                source_width,
                source_height,
                x0,
                x1,
            )
            segment_missing_without_write_with_prior = _count_mask_segment(
                missing_without_write_with_prior_mask, source_width, source_height, x0, x1
            )
            segment_missing_without_write_without_prior = _count_mask_segment(
                missing_without_write_without_prior_mask, source_width, source_height, x0, x1
            )
            segment_missing[segment_name] = {
                "x0": x0,
                "x1_exclusive": x1,
                "missing_pixels": segment_missing_pixels,
                "missing_with_write_pixels": segment_missing_with_write,
                "missing_without_write_pixels": segment_missing_without_write,
                "missing_with_prior_write_pixels": segment_missing_with_prior,
                "missing_without_write_with_prior_write_pixels": segment_missing_without_write_with_prior,
                "missing_without_write_without_prior_write_pixels": segment_missing_without_write_without_prior,
                "missing_with_write_ratio": _ratio(
                    segment_missing_with_write, segment_missing_pixels
                ),
                "missing_without_write_ratio": _ratio(
                    segment_missing_without_write, segment_missing_pixels
                ),
                "missing_with_prior_write_ratio": _ratio(
                    segment_missing_with_prior, segment_missing_pixels
                ),
                "missing_without_write_with_prior_write_ratio": _ratio(
                    segment_missing_without_write_with_prior, segment_missing_pixels
                ),
                "missing_without_write_without_prior_write_ratio": _ratio(
                    segment_missing_without_write_without_prior, segment_missing_pixels
                ),
            }

        missing_write_attribution = {
            "image": missing_image_meta,
            "source_missing_pixels": missing_source_pixels,
            "source_missing_ratio": _ratio(missing_source_pixels, total_pixels),
            "missing_with_write_pixels": missing_with_write_pixels,
            "missing_with_write_ratio": _ratio(
                missing_with_write_pixels, missing_source_pixels
            ),
            "missing_without_write_pixels": missing_without_write_pixels,
            "missing_without_write_ratio": _ratio(
                missing_without_write_pixels, missing_source_pixels
            ),
            "missing_with_prior_write_pixels": missing_with_prior_write_pixels,
            "missing_with_prior_write_ratio": _ratio(
                missing_with_prior_write_pixels, missing_source_pixels
            ),
            "missing_without_write_with_prior_write_pixels": missing_without_write_with_prior_pixels,
            "missing_without_write_with_prior_write_ratio": _ratio(
                missing_without_write_with_prior_pixels, missing_without_write_pixels
            ),
            "missing_without_write_without_prior_write_pixels": missing_without_write_without_prior_pixels,
            "missing_without_write_without_prior_write_ratio": _ratio(
                missing_without_write_without_prior_pixels, missing_without_write_pixels
            ),
            "missing_cover_pixels_by_op": op_cover_pixels,
            "missing_cover_ratio_by_op": op_cover_ratios,
            "missing_cover_pixels_by_prior_op": prior_op_cover_pixels,
            "missing_cover_ratio_by_prior_op": prior_op_cover_ratios,
            "missing_cover_pixels_by_history_window_op": history_window_op_cover_pixels,
            "missing_cover_ratio_by_history_window_op": history_window_op_cover_ratios,
            "history": {
                "frames_analyzed": len(history_frames),
                "prior_frames_analyzed": len(history_prior_frame_ids),
                "prior_frame_ids": history_prior_frame_ids,
                "write_pixels_any_window": history_union_pixels,
                "write_ratio_any_window": _ratio(history_union_pixels, total_pixels),
                "write_pixels_prior_window": history_prior_union_pixels,
                "write_ratio_prior_window": _ratio(history_prior_union_pixels, total_pixels),
                "missing_with_any_window_write_pixels": missing_with_any_window_write_pixels,
                "missing_with_any_window_write_ratio": _ratio(
                    missing_with_any_window_write_pixels,
                    missing_source_pixels,
                ),
                "missing_with_prior_write_pixels": missing_with_prior_write_pixels,
                "missing_with_prior_write_ratio": _ratio(
                    missing_with_prior_write_pixels, missing_source_pixels
                ),
                "missing_without_current_with_prior_write_pixels": missing_without_write_with_prior_pixels,
                "missing_without_current_with_prior_write_ratio": _ratio(
                    missing_without_write_with_prior_pixels, missing_without_write_pixels
                ),
                "missing_without_current_without_prior_write_pixels": missing_without_write_without_prior_pixels,
                "missing_without_current_without_prior_write_ratio": _ratio(
                    missing_without_write_without_prior_pixels, missing_without_write_pixels
                ),
                "present_surface_address": present_surface_address,
                "present_surface_address_hex": (
                    f"0x{present_surface_address:08X}" if present_surface_address > 0 else None
                ),
                "present_surface_current_overlap_pixels": present_surface_current_overlap_pixels,
                "present_surface_current_overlap_ratio": _ratio(
                    present_surface_current_overlap_pixels, missing_source_pixels
                ),
                "present_surface_prior_overlap_pixels": present_surface_prior_overlap_pixels,
                "present_surface_prior_overlap_ratio": _ratio(
                    present_surface_prior_overlap_pixels, missing_without_write_pixels
                ),
                "dominant_current_overlap_address": dominant_current_address,
                "dominant_prior_overlap_address": dominant_prior_address,
                "current_address_overlap_rows": current_address_overlap_rows,
                "current_address_overlap_rows_truncated": current_address_overlap_truncated,
                "prior_address_overlap_rows": prior_address_overlap_rows,
                "prior_address_overlap_rows_truncated": prior_address_overlap_truncated,
            },
            "segments": segment_missing,
        }

    address_write_stats: List[Dict[str, Any]] = []
    for color_image_address, counter in sorted(
        address_stats.items(),
        key=lambda item: (-int(item[1].get("write_hit_total", 0) or 0), int(item[0])),
    ):
        work_total = int(counter.get("work_total", 0) or 0)
        work_tri = int(counter.get("work_triangle_total", 0) or 0)
        work_tex = int(counter.get("work_texrect_total", 0) or 0)
        work_fill = int(counter.get("work_fill_total", 0) or 0)
        write_hit_total = int(counter.get("write_hit_total", 0) or 0)
        write_hit_tri = int(counter.get("write_hit_triangle", 0) or 0)
        write_hit_tex = int(counter.get("write_hit_texrect", 0) or 0)
        write_hit_fill = int(counter.get("write_hit_fill", 0) or 0)
        bbox_hit_total = int(counter.get("bbox_hit_total", 0) or 0)
        bbox_hit_tri = int(counter.get("bbox_hit_triangle", 0) or 0)
        bbox_hit_tex = int(counter.get("bbox_hit_texrect", 0) or 0)
        bbox_hit_fill = int(counter.get("bbox_hit_fill", 0) or 0)

        address_mask = address_write_masks.get(color_image_address, bytearray(total_pixels))
        address_pixels = int(sum(address_mask))
        address_box_pixels = _count_mask_and(address_mask, source_box_mask)

        per_op_pixels: Dict[str, int] = {}
        per_op_box_pixels: Dict[str, int] = {}
        op_masks = address_op_masks.get(color_image_address, {})
        for op_name in ("triangle", "texrect", "fill"):
            mask = op_masks.get(op_name)
            if mask is None:
                per_op_pixels[op_name] = 0
                per_op_box_pixels[op_name] = 0
            else:
                per_op_pixels[op_name] = int(sum(mask))
                per_op_box_pixels[op_name] = _count_mask_and(mask, source_box_mask)

        segment_summary: Dict[str, Dict[str, Any]] = {}
        for segment_name, (x0, x1) in segment_ranges.items():
            segment_pixels = max(0, (x1 - x0) * source_height)
            segment_source_box_pixels = _count_mask_segment(source_box_mask, source_width, source_height, x0, x1)
            segment_address_pixels = _count_mask_segment(address_mask, source_width, source_height, x0, x1)
            segment_address_box_pixels = _count_mask_and_segment(
                address_mask, source_box_mask, source_width, source_height, x0, x1
            )
            segment_summary[segment_name] = {
                "x0": x0,
                "x1_exclusive": x1,
                "pixels": segment_pixels,
                "source_box_pixels": segment_source_box_pixels,
                "write_pixels": segment_address_pixels,
                "write_ratio": _ratio(segment_address_pixels, segment_pixels),
                "write_source_box_pixels": segment_address_box_pixels,
                "write_source_box_ratio": _ratio(segment_address_box_pixels, segment_source_box_pixels),
            }

        address_write_stats.append(
            {
                "color_image_address": color_image_address,
                "color_image_address_hex": f"0x{color_image_address:08X}",
                "work_total": work_total,
                "work_triangle_total": work_tri,
                "work_texrect_total": work_tex,
                "work_fill_total": work_fill,
                "bbox_hit_total": bbox_hit_total,
                "bbox_hit_triangle": bbox_hit_tri,
                "bbox_hit_texrect": bbox_hit_tex,
                "bbox_hit_fill": bbox_hit_fill,
                "write_hit_total": write_hit_total,
                "write_hit_triangle": write_hit_tri,
                "write_hit_texrect": write_hit_tex,
                "write_hit_fill": write_hit_fill,
                "write_hit_ratio": _ratio(write_hit_total, work_total),
                "bbox_hit_ratio": _ratio(bbox_hit_total, work_total),
                "write_pixels": address_pixels,
                "write_ratio": _ratio(address_pixels, total_pixels),
                "write_source_box_pixels": address_box_pixels,
                "write_source_box_ratio": _ratio(address_box_pixels, source_box_pixels),
                "triangle_write_pixels": per_op_pixels.get("triangle", 0),
                "triangle_write_source_box_pixels": per_op_box_pixels.get("triangle", 0),
                "triangle_write_source_box_ratio": _ratio(per_op_box_pixels.get("triangle", 0), source_box_pixels),
                "texrect_write_pixels": per_op_pixels.get("texrect", 0),
                "texrect_write_source_box_pixels": per_op_box_pixels.get("texrect", 0),
                "texrect_write_source_box_ratio": _ratio(per_op_box_pixels.get("texrect", 0), source_box_pixels),
                "fill_write_pixels": per_op_pixels.get("fill", 0),
                "fill_write_source_box_pixels": per_op_box_pixels.get("fill", 0),
                "fill_write_source_box_ratio": _ratio(per_op_box_pixels.get("fill", 0), source_box_pixels),
                "segments": segment_summary,
            }
        )

    history_address_write_stats: List[Dict[str, Any]] = []
    for color_image_address, counter in sorted(
        history_address_stats.items(),
        key=lambda item: (-int(item[1].get("work_total", 0) or 0), int(item[0])),
    ):
        work_total = int(counter.get("work_total", 0) or 0)
        work_tri = int(counter.get("work_triangle_total", 0) or 0)
        work_tex = int(counter.get("work_texrect_total", 0) or 0)
        work_fill = int(counter.get("work_fill_total", 0) or 0)
        mask = history_address_masks.get(color_image_address, bytearray(total_pixels))
        write_pixels = int(sum(mask))
        write_source_box_pixels = _count_mask_and(mask, source_box_mask)

        op_masks = history_address_op_masks.get(color_image_address, {})
        tri_mask = op_masks.get("triangle", bytearray(total_pixels))
        tex_mask = op_masks.get("texrect", bytearray(total_pixels))
        fill_mask = op_masks.get("fill", bytearray(total_pixels))
        tri_source_box_pixels = _count_mask_and(tri_mask, source_box_mask)
        tex_source_box_pixels = _count_mask_and(tex_mask, source_box_mask)
        fill_source_box_pixels = _count_mask_and(fill_mask, source_box_mask)

        history_address_write_stats.append(
            {
                "color_image_address": color_image_address,
                "color_image_address_hex": f"0x{color_image_address:08X}",
                "work_total": work_total,
                "work_triangle_total": work_tri,
                "work_texrect_total": work_tex,
                "work_fill_total": work_fill,
                "write_pixels": write_pixels,
                "write_ratio": _ratio(write_pixels, total_pixels),
                "write_source_box_pixels": write_source_box_pixels,
                "write_source_box_ratio": _ratio(write_source_box_pixels, source_box_pixels),
                "triangle_write_source_box_pixels": tri_source_box_pixels,
                "triangle_write_source_box_ratio": _ratio(tri_source_box_pixels, source_box_pixels),
                "texrect_write_source_box_pixels": tex_source_box_pixels,
                "texrect_write_source_box_ratio": _ratio(tex_source_box_pixels, source_box_pixels),
                "fill_write_source_box_pixels": fill_source_box_pixels,
                "fill_write_source_box_ratio": _ratio(fill_source_box_pixels, source_box_pixels),
            }
        )

    history_prior_address_write_stats: List[Dict[str, Any]] = []
    for color_image_address, counter in sorted(
        history_prior_address_stats.items(),
        key=lambda item: (-int(item[1].get("work_total", 0) or 0), int(item[0])),
    ):
        work_total = int(counter.get("work_total", 0) or 0)
        work_tri = int(counter.get("work_triangle_total", 0) or 0)
        work_tex = int(counter.get("work_texrect_total", 0) or 0)
        work_fill = int(counter.get("work_fill_total", 0) or 0)
        mask = history_prior_address_masks.get(color_image_address, bytearray(total_pixels))
        write_pixels = int(sum(mask))
        write_source_box_pixels = _count_mask_and(mask, source_box_mask)

        op_masks = history_prior_address_op_masks.get(color_image_address, {})
        tri_mask = op_masks.get("triangle", bytearray(total_pixels))
        tex_mask = op_masks.get("texrect", bytearray(total_pixels))
        fill_mask = op_masks.get("fill", bytearray(total_pixels))
        tri_source_box_pixels = _count_mask_and(tri_mask, source_box_mask)
        tex_source_box_pixels = _count_mask_and(tex_mask, source_box_mask)
        fill_source_box_pixels = _count_mask_and(fill_mask, source_box_mask)

        history_prior_address_write_stats.append(
            {
                "color_image_address": color_image_address,
                "color_image_address_hex": f"0x{color_image_address:08X}",
                "work_total": work_total,
                "work_triangle_total": work_tri,
                "work_texrect_total": work_tex,
                "work_fill_total": work_fill,
                "write_pixels": write_pixels,
                "write_ratio": _ratio(write_pixels, total_pixels),
                "write_source_box_pixels": write_source_box_pixels,
                "write_source_box_ratio": _ratio(write_source_box_pixels, source_box_pixels),
                "triangle_write_source_box_pixels": tri_source_box_pixels,
                "triangle_write_source_box_ratio": _ratio(tri_source_box_pixels, source_box_pixels),
                "texrect_write_source_box_pixels": tex_source_box_pixels,
                "texrect_write_source_box_ratio": _ratio(tex_source_box_pixels, source_box_pixels),
                "fill_write_source_box_pixels": fill_source_box_pixels,
                "fill_write_source_box_ratio": _ratio(fill_source_box_pixels, source_box_pixels),
            }
        )

    payload = {
        "schema": "rvk2_missing_region_focus_v3",
        "packet_trace": str(packet_trace_path),
        "diff_summary": str(diff_summary_path),
        "frame_id": int(selected_frame.frame_id),
        "source_width": source_width,
        "source_height": source_height,
        "source_boxes": [
            {"x0": box[0], "y0": box[1], "x1": box[2], "y1": box[3]}
            for box in source_boxes
        ],
        "counts": dict(sorted(counts.items())),
        "texture_bucket_hits": dict(bucket_bbox_hits.most_common()),
        "texture_bucket_bbox_hits": dict(bucket_bbox_hits.most_common()),
        "texture_bucket_write_hits": dict(bucket_write_hits.most_common()),
        "phase_write_hits": dict(phase_write_hits.most_common()),
        "write_state_hits": {
            key: dict(counter.most_common(64))
            for key, counter in sorted(write_state_counters.items())
        },
        "write_coverage": {
            "total_pixels": total_pixels,
            "source_box_count": source_box_count,
            "source_box_pixels": source_box_pixels,
            "write_pixels": write_union_pixels,
            "write_ratio": _ratio(write_union_pixels, total_pixels),
            "write_source_box_pixels": write_union_box_pixels,
            "write_source_box_ratio": _ratio(write_union_box_pixels, source_box_pixels),
            "segments": write_coverage_segments,
        },
        "color_image_sequence": {
            "switch_count": color_image_switch_count,
            "unique_target_count": len(color_image_work_counts),
            "sequence_head": [f"0x{addr:08X}" for addr in color_image_sequence],
            "work_counts": {
                f"0x{address:08X}": int(count)
                for address, count in color_image_work_counts.most_common()
            },
        },
        "history_window": {
            "frames_requested": history_window + 1,
            "frames_analyzed": len(history_frames),
            "frame_ids": [int(frame.frame_id) for frame in history_frames],
            "prior_frames_analyzed": len(history_prior_frame_ids),
            "prior_frame_ids": history_prior_frame_ids,
            "switch_count": history_color_image_switch_count,
            "unique_target_count": len(history_color_image_work_counts),
            "sequence_head": [f"0x{addr:08X}" for addr in history_color_image_sequence],
            "work_counts": {
                f"0x{address:08X}": int(count)
                for address, count in history_color_image_work_counts.most_common()
            },
            "address_write_stats": history_address_write_stats,
            "prior_address_write_stats": history_prior_address_write_stats,
        },
        "forensics_last_active": {
            "present_surface": int(forensics_last.get("present_surface", 0) or 0),
            "present_surface_hex": f"0x{int(forensics_last.get('present_surface', 0) or 0):08X}",
            "vi_origin": int(forensics_last.get("vi_origin", 0) or 0),
            "vi_origin_hex": f"0x{int(forensics_last.get('vi_origin', 0) or 0):08X}",
            "vi_origin_match": int(forensics_last.get("vi_origin_match", 0) or 0),
            "present_select": int(forensics_last.get("present_select", 0) or 0),
        },
        "address_write_stats": address_write_stats,
        "work_hit_samples": hit_samples,
        "work_hit_stats": {
            "hit_total": work_hit_total,
            "bbox_hit_total": work_hit_bbox_total,
            "write_hit_total": work_hit_write_total,
            "samples_emitted": len(hit_samples),
            "samples_truncated": work_hit_sample_truncated,
            "max_hit_samples": max(0, int(args.max_hit_samples)),
        },
        "missing_write_attribution": missing_write_attribution,
    }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
