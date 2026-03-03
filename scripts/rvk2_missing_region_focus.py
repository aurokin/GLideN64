#!/usr/bin/env python3
from __future__ import annotations

import argparse
import importlib.util
import json
import math
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


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


def _op_kind_name(op_kind: int) -> str:
    if op_kind == 0:
        return "fill"
    if op_kind == 1:
        return "triangle"
    if op_kind == 2:
        return "texrect"
    return f"op{op_kind}"


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
        default=24,
        help="max intersecting work samples to emit",
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
            "schema": "rvk2_missing_region_focus_v1",
            "packet_trace": str(packet_trace_path),
            "diff_summary": str(diff_summary_path),
            "frame_id": None,
            "source_width": None,
            "source_height": None,
            "source_boxes": [],
            "counts": {},
            "texture_bucket_hits": {},
            "work_hit_samples": [],
            "notes": ["no diff boxes available"],
        }
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        print(output_path)
        return 0

    parser_module = _load_packet_replay_module(Path(__file__).with_name("rvk2_packet_trace_replay.py"))
    frames = parser_module.parse_packet_trace(packet_trace_path)

    selected_frame = None
    if args.frame_id > 0:
        for frame in frames:
            if int(frame.frame_id) == args.frame_id:
                selected_frame = frame
                break
        if selected_frame is None:
            raise SystemExit(f"ERROR: frame {args.frame_id} not found in packet trace")
    else:
        for frame in reversed(frames):
            if len(frame.render_work) > 0:
                selected_frame = frame
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

    counts: Counter[str] = Counter()
    bucket_hits: Counter[str] = Counter()
    hit_samples: List[Dict[str, Any]] = []

    for work in selected_frame.render_work:
        op_kind = int(work.op_kind)
        op_name = _op_kind_name(op_kind)
        counts[f"work_{op_name}_total"] += 1
        if op_kind == 1:
            bounds = _triangle_bounds(work)
        else:
            bounds = _rect_bounds(work)

        hit = any(_intersects(bounds, box) for box in source_boxes)
        if not hit:
            continue

        counts[f"work_{op_name}_hit"] += 1
        if bool(work.textured):
            bucket_key = f"{op_name}:f{int(work.tile_format)}s{int(work.tile_size)}"
            if int(work.tile_format) == 2 and int(work.tile_size) == 0:
                bucket_key += ":ci4"
            bucket_hits[bucket_key] += 1

        if len(hit_samples) < max(0, int(args.max_hit_samples)):
            hit_samples.append(
                {
                    "source_packet_id": int(work.source_packet_id),
                    "op_kind": op_name,
                    "textured": bool(work.textured),
                    "bbox": [
                        round(float(bounds[0]), 4),
                        round(float(bounds[1]), 4),
                        round(float(bounds[2]), 4),
                        round(float(bounds[3]), 4),
                    ],
                    "tile_format": int(work.tile_format),
                    "tile_size": int(work.tile_size),
                    "texture_image_format": int(work.texture_image_format),
                    "texture_image_size": int(work.texture_image_size),
                    "tile_tmem": int(work.tile_tmem),
                    "tile_line": int(work.tile_line),
                    "color_image_address": int(work.color_image_address),
                }
            )

    payload = {
        "schema": "rvk2_missing_region_focus_v1",
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
        "texture_bucket_hits": dict(bucket_hits.most_common()),
        "work_hit_samples": hit_samples,
    }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
