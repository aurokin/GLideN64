#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


RDP_DOMAIN = 2


@dataclass
class FrameTally:
    packet_total: int = 0
    domain_packet_total: int = 0
    opcode_counts: Counter[int] = field(default_factory=Counter)
    family_counts: Counter[str] = field(default_factory=Counter)


def _parse_int(text: str) -> int:
    value = text.strip()
    if value.startswith(("0x", "0X")):
        return int(value, 16)
    return int(value, 10)


def _opcode_key(opcode: int) -> str:
    return f"0x{opcode & 0xFF:02X}"


def _counter_to_map(counter: Counter[int]) -> Dict[str, int]:
    ordered = sorted(counter.items(), key=lambda item: (-item[1], item[0]))
    return {_opcode_key(opcode): int(count) for opcode, count in ordered if count > 0}


def _family_counter_to_map(counter: Counter[str]) -> Dict[str, int]:
    preferred_order = [
        "triangles",
        "texrect",
        "fillrect",
        "set_color_image",
        "set_depth_image",
        "set_texture_image",
        "tmem_load",
        "tile_setup",
        "sync",
        "pipeline_state",
        "scissor",
        "other",
    ]
    out: Dict[str, int] = {}
    for key in preferred_order:
        count = int(counter.get(key, 0))
        if count > 0:
            out[key] = count
    for key in sorted(counter.keys()):
        if key in out:
            continue
        count = int(counter.get(key, 0))
        if count > 0:
            out[key] = count
    return out


def _safe_int(value: Any) -> int:
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return _parse_int(value)
        except Exception:
            return 0
    return 0


def _classify_opcode(opcode: int) -> str:
    code = opcode & 0xFF
    if 0x08 <= code <= 0x0F:
        return "triangles"
    if code in (0x24, 0x25):
        return "texrect"
    if code == 0x36:
        return "fillrect"
    if code == 0x3F:
        return "set_color_image"
    if code == 0x3E:
        return "set_depth_image"
    if code == 0x3D:
        return "set_texture_image"
    if code in (0x30, 0x33, 0x34):
        return "tmem_load"
    if code in (0x32, 0x35):
        return "tile_setup"
    if code in (0x26, 0x27, 0x28, 0x29):
        return "sync"
    if code in (0x2F, 0x3C):
        return "pipeline_state"
    if code == 0x2D:
        return "scissor"
    return "other"


def _frame_summary(frame_id: int, tally: Optional[FrameTally]) -> Optional[Dict[str, Any]]:
    if tally is None:
        return None
    return {
        "frame_id": int(frame_id),
        "packet_total": int(tally.packet_total),
        "domain_packet_total": int(tally.domain_packet_total),
        "rdp_opcode_counts": _counter_to_map(tally.opcode_counts),
        "rdp_family_counts": _family_counter_to_map(tally.family_counts),
    }


def _load_replay_first_failed(path: Optional[Path]) -> Optional[int]:
    if path is None or not path.is_file():
        return None
    try:
        replay = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None
    frames = replay.get("frames", [])
    if not isinstance(frames, list):
        return None
    for frame in frames:
        if not isinstance(frame, dict):
            continue
        if frame.get("ok") is False:
            frame_id = frame.get("frame_id")
            if isinstance(frame_id, int):
                return frame_id
    return None


def _iter_window_frame_ids(frame_ids: Iterable[int], focus_frame: int, window: int) -> List[int]:
    lo = focus_frame - window
    hi = focus_frame + window
    selected = [frame_id for frame_id in frame_ids if lo <= frame_id <= hi]
    selected.sort()
    return selected


def _derive_leads(
    overall_families: Counter[str],
    focus_families: Counter[str],
    replay_first_failed_frame: Optional[int],
    focus_frame: Optional[int],
) -> List[str]:
    leads: List[str] = []
    tri_total = int(overall_families.get("triangles", 0))
    texrect_total = int(overall_families.get("texrect", 0))
    fill_total = int(overall_families.get("fillrect", 0))
    set_color_total = int(overall_families.get("set_color_image", 0))

    tri_focus = int(focus_families.get("triangles", 0))
    texrect_focus = int(focus_families.get("texrect", 0))
    fill_focus = int(focus_families.get("fillrect", 0))
    set_color_focus = int(focus_families.get("set_color_image", 0))

    if tri_total == 0 and (texrect_total > 0 or fill_total > 0):
        leads.append("RDP stream contains texrect/fill work but no triangles; suspect RSP microcode or display-list triangle submission path.")
    if tri_focus == 0 and texrect_focus > 0 and focus_frame is not None:
        leads.append(
            f"Focus frame {focus_frame} has texrect traffic without triangle traffic; this matches a UI-only render symptom."
        )
    if set_color_focus > 1 and focus_frame is not None:
        leads.append(
            f"Focus frame {focus_frame} changes color image {set_color_focus} times; verify VI is presenting the intended target buffer."
        )
    if set_color_total > 0 and tri_total > 0 and texrect_total > 0:
        leads.append("Mixed triangle+texrect traffic is present; prioritize raster/depth/blend state and render-target selection over upstream command drop theories.")
    if replay_first_failed_frame is not None:
        leads.append(f"Replay first failed frame is {replay_first_failed_frame}; prioritize command/state inspection around that frame.")
    return leads


def _write_markdown(path: Path, payload: Dict[str, Any]) -> None:
    overall = payload.get("overall", {})
    focus_frame = payload.get("focus_frame", {})
    window = payload.get("focus_window_summary", {})
    lines: List[str] = []
    lines.append("# RDP Command Census")
    lines.append("")
    lines.append(f"- Input: `{payload.get('input')}`")
    lines.append(f"- Domain: `{payload.get('domain_filter')}`")
    lines.append(f"- Packet records: `{payload.get('packet_record_total')}`")
    lines.append(f"- RDP packets: `{payload.get('domain_packet_total')}`")
    lines.append(f"- Frames observed: `{payload.get('frame_total')}`")
    if payload.get("replay_first_failed_frame") is not None:
        lines.append(f"- Replay first failed frame: `{payload.get('replay_first_failed_frame')}`")
    if payload.get("focus_frame_id") is not None:
        lines.append(f"- Focus frame: `{payload.get('focus_frame_id')}`")
    lines.append("")
    lines.append("## Overall Family Counts")
    lines.append("")
    family_counts = overall.get("rdp_family_counts", {})
    if isinstance(family_counts, dict) and family_counts:
        for key, value in family_counts.items():
            lines.append(f"- `{key}`: {value}")
    else:
        lines.append("- _(none)_")
    lines.append("")
    lines.append("## Focus Frame Family Counts")
    lines.append("")
    focus_family_counts = focus_frame.get("rdp_family_counts", {}) if isinstance(focus_frame, dict) else {}
    if isinstance(focus_family_counts, dict) and focus_family_counts:
        for key, value in focus_family_counts.items():
            lines.append(f"- `{key}`: {value}")
    else:
        lines.append("- _(none)_")
    lines.append("")
    lines.append(f"## Focus Window Family Counts (window={payload.get('focus_window', 0)})")
    lines.append("")
    window_family_counts = window.get("rdp_family_counts", {}) if isinstance(window, dict) else {}
    if isinstance(window_family_counts, dict) and window_family_counts:
        for key, value in window_family_counts.items():
            lines.append(f"- `{key}`: {value}")
    else:
        lines.append("- _(none)_")
    lines.append("")
    lines.append("## Leads")
    lines.append("")
    leads = payload.get("leads", [])
    if isinstance(leads, list) and leads:
        for lead in leads:
            lines.append(f"- {lead}")
    else:
        lines.append("- _(none)_")
    lines.append("")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize RDP command families from packet trace output.")
    parser.add_argument("--input", required=True, help="Packet trace TSV path")
    parser.add_argument("--json-out", required=True, help="JSON summary output")
    parser.add_argument("--md-out", help="Optional markdown output")
    parser.add_argument("--domain", type=int, default=RDP_DOMAIN, help="Packet domain to classify (default: 2 for RDP)")
    parser.add_argument("--replay", help="Optional replay JSON for first-failed-frame focus")
    parser.add_argument("--focus-frame", type=int, help="Explicit focus frame (overrides replay-derived frame)")
    parser.add_argument("--focus-window", type=int, default=1, help="Frame window around focus frame")
    args = parser.parse_args()

    if args.focus_window < 0:
        raise SystemExit("ERROR: --focus-window must be >= 0.")

    input_path = Path(args.input)
    if not input_path.is_file():
        raise SystemExit(f"ERROR: input trace not found: {input_path}")

    frame_order: List[int] = []
    frame_tallies: Dict[int, FrameTally] = {}
    record_total = 0
    packet_record_total = 0
    domain_packet_total = 0
    parse_error_count = 0
    current_frame: Optional[int] = None

    overall_opcode_counts: Counter[int] = Counter()
    overall_family_counts: Counter[str] = Counter()

    lines = input_path.read_text(encoding="utf-8", errors="replace").splitlines()
    for raw_line in lines:
        line = raw_line.strip()
        if not line:
            continue
        record_total += 1
        fields = line.split("\t")
        kind = fields[0]
        try:
            if kind == "F":
                if len(fields) < 2:
                    parse_error_count += 1
                    continue
                current_frame = _parse_int(fields[1])
                if current_frame not in frame_tallies:
                    frame_tallies[current_frame] = FrameTally()
                    frame_order.append(current_frame)
                continue

            if kind != "P":
                continue
            packet_record_total += 1

            if len(fields) < 4:
                parse_error_count += 1
                continue

            if current_frame is None:
                current_frame = 0
                if current_frame not in frame_tallies:
                    frame_tallies[current_frame] = FrameTally()
                    frame_order.append(current_frame)

            tally = frame_tallies[current_frame]
            tally.packet_total += 1

            domain = _parse_int(fields[2])
            opcode = _parse_int(fields[3]) & 0xFF
            if domain != args.domain:
                continue

            domain_packet_total += 1
            tally.domain_packet_total += 1
            tally.opcode_counts[opcode] += 1
            family = _classify_opcode(opcode)
            tally.family_counts[family] += 1
            overall_opcode_counts[opcode] += 1
            overall_family_counts[family] += 1
        except Exception:
            parse_error_count += 1

    replay_first_failed_frame = _load_replay_first_failed(Path(args.replay) if args.replay else None)
    focus_frame: Optional[int] = args.focus_frame
    if focus_frame is None:
        focus_frame = replay_first_failed_frame
    if focus_frame is None:
        for frame_id in sorted(frame_order):
            tally = frame_tallies.get(frame_id)
            if tally is not None and tally.domain_packet_total > 0:
                focus_frame = frame_id
                break

    focus_tally = frame_tallies.get(focus_frame) if focus_frame is not None else None
    focus_frame_summary = _frame_summary(focus_frame, focus_tally) if focus_frame is not None else None

    window_frame_ids: List[int] = []
    window_opcode_counts: Counter[int] = Counter()
    window_family_counts: Counter[str] = Counter()
    window_packet_total = 0
    window_domain_total = 0
    if focus_frame is not None:
        window_frame_ids = _iter_window_frame_ids(frame_order, focus_frame, args.focus_window)
        for frame_id in window_frame_ids:
            tally = frame_tallies.get(frame_id)
            if tally is None:
                continue
            window_packet_total += tally.packet_total
            window_domain_total += tally.domain_packet_total
            window_opcode_counts.update(tally.opcode_counts)
            window_family_counts.update(tally.family_counts)

    first_triangle_frame: Optional[int] = None
    first_texrect_frame: Optional[int] = None
    max_triangle_frame: Optional[int] = None
    max_texrect_frame: Optional[int] = None
    max_triangle_count = -1
    max_texrect_count = -1

    for frame_id in sorted(frame_order):
        tally = frame_tallies.get(frame_id)
        if tally is None:
            continue
        tri_count = int(tally.family_counts.get("triangles", 0))
        texrect_count = int(tally.family_counts.get("texrect", 0))
        if first_triangle_frame is None and tri_count > 0:
            first_triangle_frame = frame_id
        if first_texrect_frame is None and texrect_count > 0:
            first_texrect_frame = frame_id
        if tri_count > max_triangle_count:
            max_triangle_count = tri_count
            max_triangle_frame = frame_id
        if texrect_count > max_texrect_count:
            max_texrect_count = texrect_count
            max_texrect_frame = frame_id

    leads = _derive_leads(
        overall_family_counts,
        focus_tally.family_counts if focus_tally is not None else Counter(),
        replay_first_failed_frame,
        focus_frame,
    )

    payload: Dict[str, Any] = {
        "schema": "rvk2_packet_command_census_v1",
        "input": str(input_path),
        "domain_filter": int(args.domain),
        "record_total": int(record_total),
        "packet_record_total": int(packet_record_total),
        "domain_packet_total": int(domain_packet_total),
        "frame_total": int(len(frame_order)),
        "parse_error_count": int(parse_error_count),
        "replay_first_failed_frame": replay_first_failed_frame,
        "focus_frame_id": focus_frame,
        "focus_window": int(args.focus_window),
        "overall": {
            "rdp_opcode_counts": _counter_to_map(overall_opcode_counts),
            "rdp_family_counts": _family_counter_to_map(overall_family_counts),
        },
        "focus_frame": focus_frame_summary,
        "focus_window_summary": {
            "frame_ids": window_frame_ids,
            "packet_total": int(window_packet_total),
            "domain_packet_total": int(window_domain_total),
            "rdp_opcode_counts": _counter_to_map(window_opcode_counts),
            "rdp_family_counts": _family_counter_to_map(window_family_counts),
        },
        "first_triangle_frame": first_triangle_frame,
        "first_texrect_frame": first_texrect_frame,
        "max_triangle_frame": max_triangle_frame,
        "max_triangle_count": max(0, _safe_int(max_triangle_count)),
        "max_texrect_frame": max_texrect_frame,
        "max_texrect_count": max(0, _safe_int(max_texrect_count)),
        "leads": leads,
    }

    json_out = Path(args.json_out)
    json_out.parent.mkdir(parents=True, exist_ok=True)
    json_out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json_out)

    if args.md_out:
        md_out = Path(args.md_out)
        md_out.parent.mkdir(parents=True, exist_ok=True)
        _write_markdown(md_out, payload)
        print(md_out)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
