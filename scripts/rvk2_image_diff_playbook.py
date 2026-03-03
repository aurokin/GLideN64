#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

import numpy as np
from PIL import Image, ImageDraw


@dataclass
class Box:
    x0: int
    y0: int
    x1: int
    y1: int
    area: int


def _ensure_rgb(image: Image.Image) -> Image.Image:
    if image.mode == "RGB":
        return image
    return image.convert("RGB")


def _absdiff(ref_rgb: np.ndarray, cand_rgb: np.ndarray) -> np.ndarray:
    return np.abs(ref_rgb.astype(np.int16) - cand_rgb.astype(np.int16)).astype(np.uint8)


def _threshold_mask(diff: np.ndarray, threshold: int) -> np.ndarray:
    return np.max(diff, axis=2) > threshold


def _dilate(mask: np.ndarray, radius: int) -> np.ndarray:
    if radius <= 0:
        return mask
    h, w = mask.shape
    out = mask.copy()
    ys, xs = np.where(mask)
    for y, x in zip(ys.tolist(), xs.tolist()):
        y0 = max(0, y - radius)
        y1 = min(h - 1, y + radius)
        x0 = max(0, x - radius)
        x1 = min(w - 1, x + radius)
        out[y0 : y1 + 1, x0 : x1 + 1] = True
    return out


def _component_boxes(mask: np.ndarray, min_area: int) -> List[Box]:
    h, w = mask.shape
    visited = np.zeros((h, w), dtype=bool)
    boxes: List[Box] = []
    for y in range(h):
        row = mask[y]
        for x in range(w):
            if not row[x] or visited[y, x]:
                continue
            stack = [(y, x)]
            visited[y, x] = True
            area = 0
            x0 = x1 = x
            y0 = y1 = y
            while stack:
                cy, cx = stack.pop()
                area += 1
                if cx < x0:
                    x0 = cx
                if cx > x1:
                    x1 = cx
                if cy < y0:
                    y0 = cy
                if cy > y1:
                    y1 = cy
                if cy > 0 and mask[cy - 1, cx] and not visited[cy - 1, cx]:
                    visited[cy - 1, cx] = True
                    stack.append((cy - 1, cx))
                if cy + 1 < h and mask[cy + 1, cx] and not visited[cy + 1, cx]:
                    visited[cy + 1, cx] = True
                    stack.append((cy + 1, cx))
                if cx > 0 and mask[cy, cx - 1] and not visited[cy, cx - 1]:
                    visited[cy, cx - 1] = True
                    stack.append((cy, cx - 1))
                if cx + 1 < w and mask[cy, cx + 1] and not visited[cy, cx + 1]:
                    visited[cy, cx + 1] = True
                    stack.append((cy, cx + 1))
            if area >= min_area:
                boxes.append(Box(x0=x0, y0=y0, x1=x1, y1=y1, area=area))
    boxes.sort(key=lambda item: item.area, reverse=True)
    return boxes


def _first_mismatch(mask: np.ndarray) -> Optional[Dict[str, int]]:
    ys, xs = np.where(mask)
    if ys.size == 0:
        return None
    idx = np.lexsort((xs, ys))[0]
    return {"x": int(xs[idx]), "y": int(ys[idx])}


def _metrics(diff: np.ndarray, raw_mask: np.ndarray, mask: np.ndarray) -> Dict[str, Any]:
    h, w, _ = diff.shape
    pixel_count = h * w
    raw_changed = int(raw_mask.sum())
    dilated_changed = int(mask.sum())

    diff_f = diff.astype(np.float32)
    mse = float(np.mean(diff_f * diff_f))
    mae = float(np.mean(diff_f))
    rmse = float(math.sqrt(mse))
    psnr = float("inf") if mse == 0.0 else 20.0 * math.log10(255.0) - 10.0 * math.log10(mse)

    return {
        "width": w,
        "height": h,
        "pixels_total": pixel_count,
        "pixels_changed_raw": raw_changed,
        "pixels_changed": dilated_changed,
        "percent_changed_raw": (raw_changed * 100.0 / pixel_count) if pixel_count else 0.0,
        "percent_changed": (dilated_changed * 100.0 / pixel_count) if pixel_count else 0.0,
        "max_channel_diff": int(np.max(diff)),
        "mse": mse,
        "mae": mae,
        "rmse": rmse,
        "psnr": psnr,
        "first_mismatch_raw": _first_mismatch(raw_mask),
        "first_mismatch": _first_mismatch(mask),
    }


def _boxes_to_json(boxes: List[Box]) -> List[Dict[str, int]]:
    return [
        {
            "i": idx + 1,
            "x0": box.x0,
            "y0": box.y0,
            "x1": box.x1,
            "y1": box.y1,
            "area": box.area,
            "width": (box.x1 - box.x0 + 1),
            "height": (box.y1 - box.y0 + 1),
        }
        for idx, box in enumerate(boxes)
    ]


def _write_markdown_snippet(
    path: Path,
    metrics: Dict[str, Any],
    boxes: List[Dict[str, int]],
    threshold: int,
    min_area: int,
    dilate: int,
) -> None:
    lines: List[str] = []
    lines.append("### Diff Metrics")
    lines.append("")
    lines.append(f"- Resolution: **{metrics['width']}x{metrics['height']}**")
    lines.append(
        f"- Pixels changed: **{metrics['pixels_changed']}** "
        f"({metrics['percent_changed']:.3f}%) after dilation"
    )
    lines.append(
        f"- Pixels changed (raw threshold): **{metrics['pixels_changed_raw']}** "
        f"({metrics['percent_changed_raw']:.3f}%)"
    )
    lines.append(f"- Max channel diff: **{metrics['max_channel_diff']}**")
    lines.append(
        f"- RMSE: **{metrics['rmse']:.6f}** | MAE: **{metrics['mae']:.6f}** | MSE: **{metrics['mse']:.6f}**"
    )
    if metrics["psnr"] == float("inf"):
        lines.append("- PSNR: **inf**")
    else:
        lines.append(f"- PSNR: **{metrics['psnr']:.6f}**")
    if metrics.get("first_mismatch_raw") is not None:
        point = metrics["first_mismatch_raw"]
        lines.append(f"- First mismatch (raw): **({point['x']},{point['y']})**")
    if metrics.get("first_mismatch") is not None:
        point = metrics["first_mismatch"]
        lines.append(f"- First mismatch (dilated): **({point['x']},{point['y']})**")
    lines.append("")
    lines.append("### Diff Tuning")
    lines.append("")
    lines.append(f"- `threshold={threshold}`")
    lines.append(f"- `min_area={min_area}`")
    lines.append(f"- `dilate={dilate}`")
    lines.append("")
    lines.append("### Diff Artifacts")
    lines.append("")
    lines.append("- `diff.png`: grayscale max-channel absolute difference")
    lines.append("- `mask_raw.png`: thresholded mismatch mask (pre-dilation)")
    lines.append("- `mask.png`: mismatch mask after dilation")
    lines.append("- `overlay.png`: candidate image with mismatch boxes")
    lines.append("")
    lines.append("### Mismatch Boxes (Largest First)")
    lines.append("")
    if not boxes:
        lines.append("_No mismatch boxes (images match within configured threshold)._")
    else:
        for box in boxes:
            lines.append(
                f"- Box #{box['i']}: ({box['x0']},{box['y0']})-({box['x1']},{box['y1']}) "
                f"area={box['area']}"
            )
    lines.append("")
    lines.append("> Filter primitive and packet logs to records intersecting these boxes.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate pixel-localized deviation artifacts for a reference/candidate pair.")
    parser.add_argument("--ref", required=True, help="Reference image (PNG/PPM)")
    parser.add_argument("--test", required=True, help="Candidate image (PNG/PPM)")
    parser.add_argument("--outdir", required=True, help="Output directory")
    parser.add_argument("--threshold", type=int, default=20, help="Per-pixel channel threshold (0..255)")
    parser.add_argument("--min-area", type=int, default=256, help="Minimum connected component area")
    parser.add_argument("--max-boxes", type=int, default=32, help="Maximum number of boxes to emit")
    parser.add_argument("--dilate", type=int, default=1, help="Mask dilation radius in pixels")
    args = parser.parse_args()

    if args.threshold < 0 or args.threshold > 255:
        raise SystemExit("ERROR: --threshold must be in [0, 255].")
    if args.min_area <= 0:
        raise SystemExit("ERROR: --min-area must be > 0.")
    if args.max_boxes <= 0:
        raise SystemExit("ERROR: --max-boxes must be > 0.")
    if args.dilate < 0:
        raise SystemExit("ERROR: --dilate must be >= 0.")

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    ref_image = _ensure_rgb(Image.open(args.ref))
    test_image = _ensure_rgb(Image.open(args.test))
    if ref_image.size != test_image.size:
        raise SystemExit(f"ERROR: image size mismatch: ref={ref_image.size} test={test_image.size}")

    ref_rgb = np.array(ref_image, dtype=np.uint8)
    test_rgb = np.array(test_image, dtype=np.uint8)
    diff = _absdiff(ref_rgb, test_rgb)
    raw_mask = _threshold_mask(diff, args.threshold)
    mask = _dilate(raw_mask, args.dilate)

    boxes = _component_boxes(mask, args.min_area)[: args.max_boxes]
    boxes_json = _boxes_to_json(boxes)
    metrics = _metrics(diff, raw_mask, mask)

    diff_path = outdir / "diff.png"
    mask_raw_path = outdir / "mask_raw.png"
    mask_path = outdir / "mask.png"
    overlay_path = outdir / "overlay.png"
    boxes_path = outdir / "boxes.json"
    summary_path = outdir / "summary.json"
    snippet_path = outdir / "playbook_snippet.md"

    Image.fromarray(np.max(diff, axis=2).astype(np.uint8), mode="L").save(diff_path)
    Image.fromarray((raw_mask.astype(np.uint8) * 255), mode="L").save(mask_raw_path)
    Image.fromarray((mask.astype(np.uint8) * 255), mode="L").save(mask_path)

    overlay = test_image.copy()
    draw = ImageDraw.Draw(overlay)
    for box in boxes_json:
        draw.rectangle([box["x0"], box["y0"], box["x1"], box["y1"]], outline=(255, 0, 0), width=2)
        draw.text((box["x0"] + 2, box["y0"] + 2), str(box["i"]), fill=(255, 0, 0))
    overlay.save(overlay_path)

    boxes_path.write_text(json.dumps(boxes_json, indent=2) + "\n", encoding="utf-8")
    summary_payload = {
        "schema": "rvk2_image_diff_playbook_v1",
        "reference_image": str(Path(args.ref)),
        "candidate_image": str(Path(args.test)),
        "threshold": int(args.threshold),
        "min_area": int(args.min_area),
        "max_boxes": int(args.max_boxes),
        "dilate": int(args.dilate),
        "box_count": len(boxes_json),
        "metrics": metrics,
        "artifact_files": {
            "diff": str(diff_path),
            "mask_raw": str(mask_raw_path),
            "mask": str(mask_path),
            "overlay": str(overlay_path),
            "boxes": str(boxes_path),
            "playbook_snippet": str(snippet_path),
        },
    }
    summary_path.write_text(json.dumps(summary_payload, indent=2) + "\n", encoding="utf-8")
    _write_markdown_snippet(snippet_path, metrics, boxes_json, args.threshold, args.min_area, args.dilate)

    for item in (diff_path, mask_raw_path, mask_path, overlay_path, boxes_path, summary_path, snippet_path):
        print(item)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
