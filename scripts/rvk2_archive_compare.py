#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


def _parse_float(value: str) -> Optional[float]:
    text = value.strip()
    if not text:
        return None
    try:
        parsed = float(text)
    except ValueError:
        return None
    if math.isnan(parsed) or math.isinf(parsed):
        return None
    return parsed


def _ratio_delta(new_value: Optional[float], old_value: Optional[float]) -> Optional[float]:
    if new_value is None or old_value is None:
        return None
    return new_value - old_value


def _format_number(value: Optional[float], places: int = 6) -> str:
    if value is None:
        return "n/a"
    return f"{value:.{places}f}"


def _format_signed(value: Optional[float], places: int = 6) -> str:
    if value is None:
        return "n/a"
    if value > 0:
        return f"+{value:.{places}f}"
    return f"{value:.{places}f}"


def _format_ratio_percent(value: Optional[float]) -> str:
    if value is None:
        return "n/a"
    return f"{value * 100.0:.2f}%"


def _coerce_dict(value: Any) -> Dict[str, Any]:
    if isinstance(value, dict):
        return value
    return {}


def _coerce_list(value: Any) -> List[Any]:
    if isinstance(value, list):
        return value
    return []


@dataclass(frozen=True)
class ArchiveIndexRow:
    run_id: str
    run_stamp_utc: str
    scenario: str
    git_commit_short: str
    rmse: Optional[float]
    mae: Optional[float]
    candidate_non_black_ratio: Optional[float]
    candidate_mean_luma: Optional[float]
    archive_dir: Path
    bundle_path: Path


def _load_index(path: Path) -> List[ArchiveIndexRow]:
    if not path.is_file():
        raise FileNotFoundError(f"archive index not found: {path}")
    rows: List[ArchiveIndexRow] = []
    with path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for raw in reader:
            run_id = str(raw.get("run_id", "")).strip()
            scenario = str(raw.get("scenario", "")).strip()
            if not run_id or not scenario:
                continue
            rows.append(
                ArchiveIndexRow(
                    run_id=run_id,
                    run_stamp_utc=str(raw.get("run_stamp_utc", "")).strip(),
                    scenario=scenario,
                    git_commit_short=str(raw.get("git_commit_short", "")).strip(),
                    rmse=_parse_float(str(raw.get("rmse", ""))),
                    mae=_parse_float(str(raw.get("mae", ""))),
                    candidate_non_black_ratio=_parse_float(str(raw.get("candidate_non_black_ratio", ""))),
                    candidate_mean_luma=_parse_float(str(raw.get("candidate_mean_luma", ""))),
                    archive_dir=Path(str(raw.get("archive_dir", "")).strip()),
                    bundle_path=Path(str(raw.get("bundle", "")).strip()),
                )
            )
    return rows


def _select_rows(
    rows: List[ArchiveIndexRow],
    scenario: str,
    old_run_id: str,
    new_run_id: str,
) -> Tuple[ArchiveIndexRow, ArchiveIndexRow]:
    by_id: Dict[str, ArchiveIndexRow] = {row.run_id: row for row in rows}
    if old_run_id or new_run_id:
        if not old_run_id or not new_run_id:
            raise RuntimeError("--old-run-id and --new-run-id must be provided together")
        if old_run_id not in by_id:
            raise RuntimeError(f"old run id not found in index: {old_run_id}")
        if new_run_id not in by_id:
            raise RuntimeError(f"new run id not found in index: {new_run_id}")
        old_row = by_id[old_run_id]
        new_row = by_id[new_run_id]
        return old_row, new_row

    filtered = [row for row in rows if row.scenario == scenario]
    if len(filtered) == 0:
        raise RuntimeError(
            f"index has no runs for scenario '{scenario}'"
        )
    if len(filtered) == 1:
        return filtered[0], filtered[0]
    filtered.sort(key=lambda row: (row.run_stamp_utc, row.run_id))
    return filtered[-2], filtered[-1]


def _resolve_bundle_path(row: ArchiveIndexRow) -> Optional[Path]:
    candidates = [
        row.bundle_path,
        row.archive_dir / "telemetry" / f"{row.scenario}.telemetry.bundle.json",
    ]
    for candidate in candidates:
        if str(candidate).strip() and candidate.is_file():
            return candidate
    return None


def _load_bundle(row: ArchiveIndexRow) -> Dict[str, Any]:
    path = _resolve_bundle_path(row)
    if path is None:
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    if isinstance(data, dict):
        return data
    return {}


def _dominant_state(write_state_hits: Dict[str, Any], key: str) -> Dict[str, Any]:
    state_map = write_state_hits.get(key)
    if not isinstance(state_map, dict) or not state_map:
        return {}
    top_key = None
    top_count = -1
    total = 0
    for raw_key, raw_value in state_map.items():
        try:
            count = int(raw_value)
        except Exception:
            count = 0
        total += count
        if count > top_count:
            top_count = count
            top_key = str(raw_key)
    return {
        "value": top_key,
        "count": max(0, top_count),
        "total": max(0, total),
        "ratio": (float(top_count) / float(total)) if total > 0 and top_count >= 0 else None,
    }


def _extract_snapshot(row: ArchiveIndexRow, bundle: Dict[str, Any]) -> Dict[str, Any]:
    metrics = {
        "rmse": row.rmse,
        "mae": row.mae,
        "candidate_non_black_ratio": row.candidate_non_black_ratio,
        "candidate_mean_luma": row.candidate_mean_luma,
    }
    suspected = [item for item in _coerce_list(bundle.get("suspected_gaps")) if isinstance(item, str)]
    hard_faults = [item for item in _coerce_list(bundle.get("hard_faults")) if isinstance(item, str)]
    signals = _coerce_dict(bundle.get("signals"))
    missing_region = _coerce_dict(signals.get("missing_region"))
    missing_write = _coerce_dict(missing_region.get("missing_write_attribution"))
    missing_history = _coerce_dict(missing_region.get("missing_write_history"))
    write_state_hits = _coerce_dict(missing_region.get("write_state_hits"))
    segments = _coerce_dict(missing_write.get("segments"))
    left_segment = _coerce_dict(segments.get("left"))
    center_segment = _coerce_dict(segments.get("center"))
    right_segment = _coerce_dict(segments.get("right"))

    snapshot = {
        "run": {
            "run_id": row.run_id,
            "run_stamp_utc": row.run_stamp_utc,
            "scenario": row.scenario,
            "git_commit_short": row.git_commit_short,
            "archive_dir": str(row.archive_dir),
            "bundle_path": str(_resolve_bundle_path(row) or row.bundle_path),
        },
        "metrics": metrics,
        "suspected_gaps": suspected,
        "hard_faults": hard_faults,
        "missing": {
            "source_missing_pixels": missing_write.get("source_missing_pixels"),
            "missing_with_write_pixels": missing_write.get("missing_with_write_pixels"),
            "missing_without_write_pixels": missing_write.get("missing_without_write_pixels"),
            "missing_with_write_ratio": missing_write.get("missing_with_write_ratio"),
            "missing_without_write_ratio": missing_write.get("missing_without_write_ratio"),
            "missing_without_write_with_prior_write_ratio": missing_write.get(
                "missing_without_write_with_prior_write_ratio"
            ),
            "missing_without_write_without_prior_write_ratio": missing_write.get(
                "missing_without_write_without_prior_write_ratio"
            ),
            "left_missing_without_write_ratio": left_segment.get("missing_without_write_ratio"),
            "center_missing_without_write_ratio": center_segment.get("missing_without_write_ratio"),
            "right_missing_without_write_ratio": right_segment.get("missing_without_write_ratio"),
            "history_missing_without_current_with_prior_write_ratio": missing_history.get(
                "missing_without_current_with_prior_write_ratio"
            ),
            "history_missing_without_current_without_prior_write_ratio": missing_history.get(
                "missing_without_current_without_prior_write_ratio"
            ),
            "history_present_surface_prior_overlap_ratio": missing_history.get(
                "present_surface_prior_overlap_ratio"
            ),
        },
        "texrect_state": {
            "combine_mux": _dominant_state(write_state_hits, "texrect:combine_mux"),
            "other_modes": _dominant_state(write_state_hits, "texrect:other_modes"),
            "tile_line": _dominant_state(write_state_hits, "texrect:tile_line"),
            "texture_image_width": _dominant_state(write_state_hits, "texrect:texture_image_width"),
        },
    }
    return snapshot


def _compare_snapshots(old: Dict[str, Any], new: Dict[str, Any]) -> Dict[str, Any]:
    old_metrics = _coerce_dict(old.get("metrics"))
    new_metrics = _coerce_dict(new.get("metrics"))
    old_missing = _coerce_dict(old.get("missing"))
    new_missing = _coerce_dict(new.get("missing"))

    def _metric_delta(key: str) -> Dict[str, Any]:
        old_v = old_metrics.get(key)
        new_v = new_metrics.get(key)
        if not isinstance(old_v, (int, float)):
            old_v = None
        if not isinstance(new_v, (int, float)):
            new_v = None
        return {"old": old_v, "new": new_v, "delta": _ratio_delta(new_v, old_v)}

    def _missing_delta(key: str) -> Dict[str, Any]:
        old_v = old_missing.get(key)
        new_v = new_missing.get(key)
        if not isinstance(old_v, (int, float)):
            old_v = None
        if not isinstance(new_v, (int, float)):
            new_v = None
        return {"old": old_v, "new": new_v, "delta": _ratio_delta(new_v, old_v)}

    old_gaps = [g for g in _coerce_list(old.get("suspected_gaps")) if isinstance(g, str)]
    new_gaps = [g for g in _coerce_list(new.get("suspected_gaps")) if isinstance(g, str)]
    old_set = set(old_gaps)
    new_set = set(new_gaps)
    added = [g for g in new_gaps if g not in old_set]
    removed = [g for g in old_gaps if g not in new_set]
    persisting = [g for g in new_gaps if g in old_set]

    return {
        "metrics": {
            "rmse": _metric_delta("rmse"),
            "mae": _metric_delta("mae"),
            "candidate_non_black_ratio": _metric_delta("candidate_non_black_ratio"),
            "candidate_mean_luma": _metric_delta("candidate_mean_luma"),
        },
        "missing": {
            "missing_without_write_ratio": _missing_delta("missing_without_write_ratio"),
            "missing_with_write_ratio": _missing_delta("missing_with_write_ratio"),
            "missing_without_write_with_prior_write_ratio": _missing_delta(
                "missing_without_write_with_prior_write_ratio"
            ),
            "missing_without_write_without_prior_write_ratio": _missing_delta(
                "missing_without_write_without_prior_write_ratio"
            ),
            "left_missing_without_write_ratio": _missing_delta("left_missing_without_write_ratio"),
            "center_missing_without_write_ratio": _missing_delta("center_missing_without_write_ratio"),
            "right_missing_without_write_ratio": _missing_delta("right_missing_without_write_ratio"),
            "history_missing_without_current_with_prior_write_ratio": _missing_delta(
                "history_missing_without_current_with_prior_write_ratio"
            ),
            "history_missing_without_current_without_prior_write_ratio": _missing_delta(
                "history_missing_without_current_without_prior_write_ratio"
            ),
        },
        "suspected_gaps": {
            "old_count": len(old_gaps),
            "new_count": len(new_gaps),
            "added": added,
            "removed": removed,
            "persisting": persisting,
        },
    }


def _format_texrect_state(state: Dict[str, Any]) -> str:
    if not isinstance(state, dict):
        return "n/a"
    value = state.get("value")
    count = state.get("count")
    total = state.get("total")
    ratio = state.get("ratio")
    if value is None:
        return "n/a"
    if isinstance(count, int) and isinstance(total, int) and total > 0:
        return f"{value} ({count}/{total}, {_format_ratio_percent(ratio)})"
    return str(value)


def _build_markdown_report(
    old_snapshot: Dict[str, Any],
    new_snapshot: Dict[str, Any],
    comparison: Dict[str, Any],
    max_gaps: int,
) -> str:
    old_run = _coerce_dict(old_snapshot.get("run"))
    new_run = _coerce_dict(new_snapshot.get("run"))
    metrics = _coerce_dict(comparison.get("metrics"))
    missing = _coerce_dict(comparison.get("missing"))
    gaps = _coerce_dict(comparison.get("suspected_gaps"))
    old_texrect = _coerce_dict(old_snapshot.get("texrect_state"))
    new_texrect = _coerce_dict(new_snapshot.get("texrect_state"))

    lines: List[str] = []
    lines.append("# RVK2 Deep Archive Compare")
    lines.append("")
    lines.append("## Runs")
    lines.append(
        f"- old: `{old_run.get('run_id', 'n/a')}` ({old_run.get('git_commit_short', 'n/a')})"
    )
    lines.append(
        f"- new: `{new_run.get('run_id', 'n/a')}` ({new_run.get('git_commit_short', 'n/a')})"
    )
    if old_run.get("run_id") == new_run.get("run_id"):
        lines.append("- mode: single-run baseline (no prior run available in index)")
    lines.append(f"- old archive: `{old_run.get('archive_dir', 'n/a')}`")
    lines.append(f"- new archive: `{new_run.get('archive_dir', 'n/a')}`")
    lines.append("")
    lines.append("## Metric Delta")
    for key in ("rmse", "mae", "candidate_non_black_ratio", "candidate_mean_luma"):
        entry = _coerce_dict(metrics.get(key))
        old_v = entry.get("old")
        new_v = entry.get("new")
        delta = entry.get("delta")
        if key.endswith("_ratio"):
            lines.append(
                f"- `{key}`: {_format_ratio_percent(old_v)} -> {_format_ratio_percent(new_v)} "
                f"(delta {_format_signed(delta * 100.0 if isinstance(delta, (int, float)) else None, 2)}pp)"
            )
        else:
            lines.append(
                f"- `{key}`: {_format_number(old_v)} -> {_format_number(new_v)} "
                f"(delta {_format_signed(delta)})"
            )
    lines.append("")
    lines.append("## Missing Attribution Delta")
    for key in (
        "missing_without_write_ratio",
        "missing_with_write_ratio",
        "missing_without_write_with_prior_write_ratio",
        "missing_without_write_without_prior_write_ratio",
        "left_missing_without_write_ratio",
        "center_missing_without_write_ratio",
        "right_missing_without_write_ratio",
    ):
        entry = _coerce_dict(missing.get(key))
        old_v = entry.get("old")
        new_v = entry.get("new")
        delta = entry.get("delta")
        lines.append(
            f"- `{key}`: {_format_ratio_percent(old_v)} -> {_format_ratio_percent(new_v)} "
            f"(delta {_format_signed(delta * 100.0 if isinstance(delta, (int, float)) else None, 2)}pp)"
        )
    lines.append("")
    lines.append("## Dominant TexRect State")
    for state_key in ("combine_mux", "other_modes", "tile_line", "texture_image_width"):
        old_state = _coerce_dict(old_texrect.get(state_key))
        new_state = _coerce_dict(new_texrect.get(state_key))
        lines.append(
            f"- `{state_key}`: {_format_texrect_state(old_state)} -> {_format_texrect_state(new_state)}"
        )
    lines.append("")
    lines.append("## Suspected Gaps")
    lines.append(
        f"- count: {gaps.get('old_count', 0)} -> {gaps.get('new_count', 0)}"
    )
    added = [g for g in _coerce_list(gaps.get("added")) if isinstance(g, str)]
    removed = [g for g in _coerce_list(gaps.get("removed")) if isinstance(g, str)]
    persisting = [g for g in _coerce_list(gaps.get("persisting")) if isinstance(g, str)]

    lines.append("- added:")
    if added:
        for item in added[:max_gaps]:
            lines.append(f"  - {item}")
    else:
        lines.append("  - none")
    if len(added) > max_gaps:
        lines.append(f"  - ... {len(added) - max_gaps} more")

    lines.append("- removed:")
    if removed:
        for item in removed[:max_gaps]:
            lines.append(f"  - {item}")
    else:
        lines.append("  - none")
    if len(removed) > max_gaps:
        lines.append(f"  - ... {len(removed) - max_gaps} more")

    lines.append("- persisting:")
    if persisting:
        for item in persisting[:max_gaps]:
            lines.append(f"  - {item}")
    else:
        lines.append("  - none")
    if len(persisting) > max_gaps:
        lines.append(f"  - ... {len(persisting) - max_gaps} more")

    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Compare two archived deep telemetry runs and summarize metric, missing-region, "
            "and suspected-gap deltas."
        )
    )
    parser.add_argument(
        "--index",
        default="build/parity-runs/paper-mario/archive/index.tsv",
        help="archive index TSV path",
    )
    parser.add_argument(
        "--scenario",
        default="paper_mario_intro",
        help="scenario id for default latest-two comparison",
    )
    parser.add_argument(
        "--old-run-id",
        default="",
        help="explicit old run id (requires --new-run-id)",
    )
    parser.add_argument(
        "--new-run-id",
        default="",
        help="explicit new run id (requires --old-run-id)",
    )
    parser.add_argument(
        "--max-gaps",
        type=int,
        default=20,
        help="max gaps to print per section in markdown output",
    )
    parser.add_argument(
        "--json-out",
        default="",
        help="optional JSON report output path",
    )
    parser.add_argument(
        "--md-out",
        default="",
        help="optional markdown report output path",
    )
    args = parser.parse_args()

    index_path = Path(args.index)
    try:
        rows = _load_index(index_path)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    try:
        old_row, new_row = _select_rows(rows, args.scenario, args.old_run_id, args.new_run_id)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    old_bundle = _load_bundle(old_row)
    new_bundle = _load_bundle(new_row)

    old_snapshot = _extract_snapshot(old_row, old_bundle)
    new_snapshot = _extract_snapshot(new_row, new_bundle)
    comparison = _compare_snapshots(old_snapshot, new_snapshot)

    payload = {
        "schema": "rvk2_archive_compare_v1",
        "index": str(index_path),
        "mode": "single_run_baseline" if old_row.run_id == new_row.run_id else "pair_compare",
        "old": old_snapshot,
        "new": new_snapshot,
        "comparison": comparison,
    }
    markdown = _build_markdown_report(
        old_snapshot,
        new_snapshot,
        comparison,
        max(1, int(args.max_gaps)),
    )

    if args.json_out:
        json_out = Path(args.json_out)
        json_out.parent.mkdir(parents=True, exist_ok=True)
        json_out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json_out)

    if args.md_out:
        md_out = Path(args.md_out)
        md_out.parent.mkdir(parents=True, exist_ok=True)
        md_out.write_text(markdown, encoding="utf-8")
        print(md_out)

    print(markdown, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
