#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict, List, Optional

STAGE_CLASS_BUCKETS = 32


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


def pixel_visible(color: int) -> bool:
    return (int(color) & 0x00FFFFFF) != 0


def luma_from_rgba(color: int) -> int:
    r = (int(color) >> 24) & 0xFF
    g = (int(color) >> 16) & 0xFF
    b = (int(color) >> 8) & 0xFF
    return ((r * 77) + (g * 150) + (b * 29) + 128) >> 8


def parse_overwrite_focus(path: Optional[Path]) -> Dict[str, int]:
    if path is None or not path.exists():
        return {}

    focus: Dict[str, int] = {
        "focus_fill_writes": 0,
        "focus_fill_write_mask_unset": 0,
        "focus_fill_write_mask_set": 0,
        "focus_fill_overwrite_black": 0,
        "focus_fill_prev_nonblack": 0,
        "focus_fill_new_nonblack": 0,
        "focus_fill_prev_luma_sum": 0,
        "focus_fill_new_luma_sum": 0,
        "focus_texrect_writes": 0,
        "focus_texrect_write_mask_unset": 0,
        "focus_texrect_write_mask_set": 0,
        "focus_texrect_overwrite_black": 0,
        "focus_texrect_prev_nonblack": 0,
        "focus_texrect_new_nonblack": 0,
        "focus_texrect_prev_luma_sum": 0,
        "focus_texrect_new_luma_sum": 0,
        "focus_texrect_texel_repeat": 0,
        "focus_texrect_texel_change": 0,
        "focus_texrect_tlut_applied": 0,
        "focus_texrect_tlut_lookup_repeat": 0,
        "focus_texrect_tlut_lookup_change": 0,
        "focus_texrect_tlut_lookup_invalid": 0,
    }
    prev_texel_by_packet: Dict[int, int] = {}
    prev_tlut_by_packet: Dict[int, int] = {}
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if not line:
                continue
            if "focus_cluster=1" not in line and "focus_cluster=2" not in line:
                continue
            rec = parse_record(line)
            if not rec:
                continue
            cluster = rec.get("focus_cluster", 0)
            if cluster not in (1, 2):
                continue
            prev = rec.get("prev", 0)
            new = rec.get("new", 0)
            prev_mask = rec.get("prev_write_mask", 0) != 0
            overwrite_black = rec.get("overwrite_to_black", 0) != 0
            prev_vis = pixel_visible(prev)
            new_vis = pixel_visible(new)
            prev_luma = luma_from_rgba(prev)
            new_luma = luma_from_rgba(new)
            if cluster == 1:
                focus["focus_fill_writes"] += 1
                if prev_mask:
                    focus["focus_fill_write_mask_set"] += 1
                else:
                    focus["focus_fill_write_mask_unset"] += 1
                if overwrite_black:
                    focus["focus_fill_overwrite_black"] += 1
                if prev_vis:
                    focus["focus_fill_prev_nonblack"] += 1
                if new_vis:
                    focus["focus_fill_new_nonblack"] += 1
                focus["focus_fill_prev_luma_sum"] += prev_luma
                focus["focus_fill_new_luma_sum"] += new_luma
            else:
                focus["focus_texrect_writes"] += 1
                if prev_mask:
                    focus["focus_texrect_write_mask_set"] += 1
                else:
                    focus["focus_texrect_write_mask_unset"] += 1
                if overwrite_black:
                    focus["focus_texrect_overwrite_black"] += 1
                if prev_vis:
                    focus["focus_texrect_prev_nonblack"] += 1
                if new_vis:
                    focus["focus_texrect_new_nonblack"] += 1
                focus["focus_texrect_prev_luma_sum"] += prev_luma
                focus["focus_texrect_new_luma_sum"] += new_luma
                packet_id = rec.get("source_packet_id", -1)
                texel = rec.get("texel", 0)
                if packet_id in prev_texel_by_packet:
                    if texel == prev_texel_by_packet[packet_id]:
                        focus["focus_texrect_texel_repeat"] += 1
                    else:
                        focus["focus_texrect_texel_change"] += 1
                prev_texel_by_packet[packet_id] = texel
                tlut_applied = rec.get("tex0_tlut_applied", 0) != 0
                if tlut_applied:
                    focus["focus_texrect_tlut_applied"] += 1
                    tlut_addr = rec.get("tex0_tlut_addr", 0)
                    if packet_id in prev_tlut_by_packet:
                        if tlut_addr == prev_tlut_by_packet[packet_id]:
                            focus["focus_texrect_tlut_lookup_repeat"] += 1
                        else:
                            focus["focus_texrect_tlut_lookup_change"] += 1
                    prev_tlut_by_packet[packet_id] = tlut_addr
                else:
                    focus["focus_texrect_tlut_lookup_invalid"] += 1
    return focus


def stage_bucket_label(bucket: int) -> str:
    tags: List[str] = []
    if bucket & 0x01:
        tags.append("cycle2")
    if bucket & 0x02:
        tags.append("force_blend")
    if bucket & 0x04:
        tags.append("alpha_cmp")
    if bucket & 0x08:
        tags.append("coverage")
    if bucket & 0x10:
        tags.append("depth")
    if not tags:
        tags.append("base")
    return f"b{bucket:02d}({'|'.join(tags)})"


def format_stage_top(
    deltas: List[int], writes: List[int], limit: int = 5
) -> str:
    indices = list(range(len(deltas)))
    indices.sort(key=lambda i: (deltas[i], writes[i], -i), reverse=True)
    parts: List[str] = []
    for i in indices[:limit]:
        if deltas[i] == 0:
            continue
        parts.append(
            f"{stage_bucket_label(i)}:{deltas[i]}/{writes[i]}={ratio(deltas[i], writes[i]):.6f}"
        )
    if not parts:
        return "none"
    return ",".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize RVK2 frame forensics TSV key/value lines."
    )
    parser.add_argument("--input", required=True, help="Path to forensics TSV log.")
    parser.add_argument(
        "--overwrite",
        help="Optional overwrite TSV log; used to backfill focus cluster metrics when frame-forensics focus counters are zero.",
    )
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
    blend_p_mem = sum_field(records, "blend_p_mem_ops")
    blend_m_mem = sum_field(records, "blend_m_mem_ops")
    blend_divide = sum_field(records, "blend_divide_ops")
    blend_nodivide = sum_field(records, "blend_nodivide_ops")
    blend_cvg_eval = sum_field(records, "blend_cvg_eval")
    blend_cvg_zero = sum_field(records, "blend_cvg_zero")
    blend_cvg_overflow = sum_field(records, "blend_cvg_overflow")
    cvg_write_eval = sum_field(records, "cvg_write_eval")
    cvg_write_zero = sum_field(records, "cvg_write_zero")
    cvg_write_overflow = sum_field(records, "cvg_write_overflow")
    stage_t2c_delta = sum_field(records, "stage_t2c_delta")
    stage_c2b_delta = sum_field(records, "stage_c2b_delta")
    stage_b2f_delta = sum_field(records, "stage_b2f_delta")
    stage_t2f_delta = sum_field(records, "stage_t2f_delta")
    stage_textured_writes = sum_field(records, "stage_textured_writes")
    stage_textured_rect = sum_field(records, "stage_textured_rect")
    stage_textured_tri = sum_field(records, "stage_textured_tri")
    stage_imread = sum_field(records, "stage_imread")
    stage_tx_repl = sum_field(records, "stage_tx_repl")
    stage_tx_tmem = sum_field(records, "stage_tx_tmem")
    stage_tx_rdram = sum_field(records, "stage_tx_rdram")
    stage_tx_synth = sum_field(records, "stage_tx_synth")
    work_fill = sum_field(records, "work_fill")
    work_texrect = sum_field(records, "work_texrect")
    work_tri = sum_field(records, "work_tri")
    work_textured = sum_field(records, "work_textured")
    write_fill = sum_field(records, "write_fill")
    write_texrect = sum_field(records, "write_texrect")
    write_tri = sum_field(records, "write_tri")
    tri_deg_reject = sum_field(records, "tri_deg_reject")
    tri_bounds_reject = sum_field(records, "tri_bounds_reject")
    tri_scissor_reject = sum_field(records, "tri_scissor_reject")
    tri_samples = sum_field(records, "tri_samples")
    tri_alpha_reject = sum_field(records, "tri_alpha_reject")
    tri_cvg_reject = sum_field(records, "tri_cvg_reject")
    tri_depth_reject = sum_field(records, "tri_depth_reject")
    tri_nonblack = sum_field(records, "tri_nonblack")
    texrect_nonblack = sum_field(records, "texrect_nonblack")
    tri_luma_sum = sum_field(records, "tri_luma_sum")
    texrect_luma_sum = sum_field(records, "texrect_luma_sum")
    focus_fill_writes = sum_field(records, "focus_fill_writes")
    focus_fill_write_mask_unset = sum_field(records, "focus_fill_write_mask_unset")
    focus_fill_write_mask_set = sum_field(records, "focus_fill_write_mask_set")
    focus_fill_overwrite_black = sum_field(records, "focus_fill_overwrite_black")
    focus_fill_prev_nonblack = sum_field(records, "focus_fill_prev_nonblack")
    focus_fill_new_nonblack = sum_field(records, "focus_fill_new_nonblack")
    focus_fill_prev_luma_sum = sum_field(records, "focus_fill_prev_luma_sum")
    focus_fill_new_luma_sum = sum_field(records, "focus_fill_new_luma_sum")
    focus_texrect_writes = sum_field(records, "focus_texrect_writes")
    focus_texrect_write_mask_unset = sum_field(records, "focus_texrect_write_mask_unset")
    focus_texrect_write_mask_set = sum_field(records, "focus_texrect_write_mask_set")
    focus_texrect_overwrite_black = sum_field(records, "focus_texrect_overwrite_black")
    focus_texrect_prev_nonblack = sum_field(records, "focus_texrect_prev_nonblack")
    focus_texrect_new_nonblack = sum_field(records, "focus_texrect_new_nonblack")
    focus_texrect_prev_luma_sum = sum_field(records, "focus_texrect_prev_luma_sum")
    focus_texrect_new_luma_sum = sum_field(records, "focus_texrect_new_luma_sum")
    focus_texrect_texel_repeat = sum_field(records, "focus_texrect_texel_repeat")
    focus_texrect_texel_change = sum_field(records, "focus_texrect_texel_change")
    focus_texrect_tlut_applied = sum_field(records, "focus_texrect_tlut_applied")
    focus_texrect_tlut_lookup_repeat = sum_field(records, "focus_texrect_tlut_lookup_repeat")
    focus_texrect_tlut_lookup_change = sum_field(records, "focus_texrect_tlut_lookup_change")
    focus_texrect_tlut_lookup_invalid = sum_field(records, "focus_texrect_tlut_lookup_invalid")
    focus_metrics_source = "forensics"
    overwrite_focus: Dict[str, int] = {}
    if focus_fill_writes == 0 or focus_texrect_writes == 0:
        overwrite_focus = parse_overwrite_focus(Path(args.overwrite) if args.overwrite else None)
    if focus_fill_writes == 0 and overwrite_focus.get("focus_fill_writes", 0) > 0:
        focus_metrics_source = "overwrite"
        focus_fill_writes = overwrite_focus.get("focus_fill_writes", 0)
        focus_fill_write_mask_unset = overwrite_focus.get("focus_fill_write_mask_unset", 0)
        focus_fill_write_mask_set = overwrite_focus.get("focus_fill_write_mask_set", 0)
        focus_fill_overwrite_black = overwrite_focus.get("focus_fill_overwrite_black", 0)
        focus_fill_prev_nonblack = overwrite_focus.get("focus_fill_prev_nonblack", 0)
        focus_fill_new_nonblack = overwrite_focus.get("focus_fill_new_nonblack", 0)
        focus_fill_prev_luma_sum = overwrite_focus.get("focus_fill_prev_luma_sum", 0)
        focus_fill_new_luma_sum = overwrite_focus.get("focus_fill_new_luma_sum", 0)
    if focus_texrect_writes == 0 and overwrite_focus.get("focus_texrect_writes", 0) > 0:
        focus_metrics_source = "overwrite"
        focus_texrect_writes = overwrite_focus.get("focus_texrect_writes", 0)
        focus_texrect_write_mask_unset = overwrite_focus.get("focus_texrect_write_mask_unset", 0)
        focus_texrect_write_mask_set = overwrite_focus.get("focus_texrect_write_mask_set", 0)
        focus_texrect_overwrite_black = overwrite_focus.get("focus_texrect_overwrite_black", 0)
        focus_texrect_prev_nonblack = overwrite_focus.get("focus_texrect_prev_nonblack", 0)
        focus_texrect_new_nonblack = overwrite_focus.get("focus_texrect_new_nonblack", 0)
        focus_texrect_prev_luma_sum = overwrite_focus.get("focus_texrect_prev_luma_sum", 0)
        focus_texrect_new_luma_sum = overwrite_focus.get("focus_texrect_new_luma_sum", 0)
        focus_texrect_texel_repeat = overwrite_focus.get("focus_texrect_texel_repeat", 0)
        focus_texrect_texel_change = overwrite_focus.get("focus_texrect_texel_change", 0)
        focus_texrect_tlut_applied = overwrite_focus.get("focus_texrect_tlut_applied", 0)
        focus_texrect_tlut_lookup_repeat = overwrite_focus.get("focus_texrect_tlut_lookup_repeat", 0)
        focus_texrect_tlut_lookup_change = overwrite_focus.get("focus_texrect_tlut_lookup_change", 0)
        focus_texrect_tlut_lookup_invalid = overwrite_focus.get("focus_texrect_tlut_lookup_invalid", 0)
    ci_switches = sum_field(records, "ci_switches")
    out_writes = sum_field(records, "writes")
    vi_valid_count = sum(1 for rec in records if rec.get("vi_valid", 0) != 0)
    vi_use_regs_count = sum(1 for rec in records if rec.get("vi_use_regs", 0) != 0)
    vi_reject_count = sum(1 for rec in records if rec.get("vi_reject", 0) != 0)
    vi_origin_match_count = sum(1 for rec in records if rec.get("vi_origin_match", 0) != 0)
    vi_src_samples = sum_field(records, "vi_src_samples")
    vi_src_invalid = sum_field(records, "vi_src_invalid")
    vi_out_nonblack = sum_field(records, "vi_out_nonblack")
    present_pixel_count = sum(
        max(0, rec.get("present_w", 0)) * max(0, rec.get("present_h", 0))
        for rec in records
    )
    present_select_counts: Dict[int, int] = {}
    for rec in records:
        key = rec.get("present_select", 0)
        present_select_counts[key] = present_select_counts.get(key, 0) + 1
    stage_cls_writes = [
        sum_field(records, f"stage_cls{i}_writes") for i in range(STAGE_CLASS_BUCKETS)
    ]
    stage_cls_t2f = [
        sum_field(records, f"stage_cls{i}_t2f") for i in range(STAGE_CLASS_BUCKETS)
    ]
    stage_cls_c2b = [
        sum_field(records, f"stage_cls{i}_c2b") for i in range(STAGE_CLASS_BUCKETS)
    ]
    stage_cls_bpmem = [
        sum_field(records, f"stage_cls{i}_bpmem") for i in range(STAGE_CLASS_BUCKETS)
    ]
    stage_cls_bmmem = [
        sum_field(records, f"stage_cls{i}_bmmem") for i in range(STAGE_CLASS_BUCKETS)
    ]
    stage_cls_imread = [
        sum_field(records, f"stage_cls{i}_imrd") for i in range(STAGE_CLASS_BUCKETS)
    ]
    stage_cls_tx_repl = [
        sum_field(records, f"stage_cls{i}_txrepl") for i in range(STAGE_CLASS_BUCKETS)
    ]
    stage_cls_tx_tmem = [
        sum_field(records, f"stage_cls{i}_txtmem") for i in range(STAGE_CLASS_BUCKETS)
    ]
    stage_cls_tx_rdram = [
        sum_field(records, f"stage_cls{i}_txrdram") for i in range(STAGE_CLASS_BUCKETS)
    ]
    stage_cls_tx_synth = [
        sum_field(records, f"stage_cls{i}_txsynth") for i in range(STAGE_CLASS_BUCKETS)
    ]

    blend_a_sel = [sum_field(records, f"blend_a_sel{i}") for i in range(4)]
    blend_b_sel = [sum_field(records, f"blend_b_sel{i}") for i in range(4)]
    blend_p_sel = [sum_field(records, f"blend_p_sel{i}") for i in range(4)]
    blend_m_sel = [sum_field(records, f"blend_m_sel{i}") for i in range(4)]
    focus_fill_luma_delta_per_write = (
        float(focus_fill_new_luma_sum - focus_fill_prev_luma_sum) / float(focus_fill_writes)
        if focus_fill_writes > 0
        else 0.0
    )
    focus_texrect_luma_delta_per_write = (
        float(focus_texrect_new_luma_sum - focus_texrect_prev_luma_sum) / float(focus_texrect_writes)
        if focus_texrect_writes > 0
        else 0.0
    )

    print(f"records={len(records)} active_only={1 if args.active_only else 0}")
    print(f"writes={out_writes}")
    print(f"alpha_reject_rate={fmt_ratio(alpha_rejects, alpha_tests)}")
    print(f"coverage_reject_rate={fmt_ratio(cvg_rejects, cvg_tests)}")
    print(f"depth_reject_rate={fmt_ratio(depth_reject, depth_eval)}")
    print(f"depth_update_rate={fmt_ratio(depth_update, depth_eval)}")
    print(f"blend_enabled_rate={fmt_ratio(blend_enabled, blend_ops)}")
    print(f"blend_force_rate={fmt_ratio(blend_force, blend_ops)}")
    print(f"blend_aa_rate={fmt_ratio(blend_aa, blend_ops)}")
    print(f"blend_p_memory_selector_rate={fmt_ratio(blend_p_mem, blend_ops)}")
    print(f"blend_m_memory_selector_rate={fmt_ratio(blend_m_mem, blend_ops)}")
    print(f"blend_divide_rate={fmt_ratio(blend_divide, blend_ops)}")
    print(f"blend_nodivide_rate={fmt_ratio(blend_nodivide, blend_ops)}")
    print(f"blend_coverage_zero_rate={fmt_ratio(blend_cvg_zero, blend_cvg_eval)}")
    print(f"blend_coverage_overflow_rate={fmt_ratio(blend_cvg_overflow, blend_cvg_eval)}")
    print(f"coverage_write_zero_rate={fmt_ratio(cvg_write_zero, cvg_write_eval)}")
    print(f"coverage_write_overflow_rate={fmt_ratio(cvg_write_overflow, cvg_write_eval)}")
    print(f"stage_texel_to_combiner_delta_rate={fmt_ratio(stage_t2c_delta, out_writes)}")
    print(f"stage_combiner_to_blender_delta_rate={fmt_ratio(stage_c2b_delta, out_writes)}")
    print(f"stage_blender_to_final_delta_rate={fmt_ratio(stage_b2f_delta, out_writes)}")
    print(f"stage_texel_to_final_delta_rate={fmt_ratio(stage_t2f_delta, out_writes)}")
    print(f"stage_textured_write_rate={fmt_ratio(stage_textured_writes, out_writes)}")
    print(f"stage_textured_rect_share={fmt_ratio(stage_textured_rect, stage_textured_writes)}")
    print(f"stage_textured_triangle_share={fmt_ratio(stage_textured_tri, stage_textured_writes)}")
    print(f"stage_image_read_write_rate={fmt_ratio(stage_imread, out_writes)}")
    print(f"stage_texel_source_replacement_rate={fmt_ratio(stage_tx_repl, out_writes)}")
    print(f"stage_texel_source_tmem_rate={fmt_ratio(stage_tx_tmem, out_writes)}")
    print(f"stage_texel_source_rdram_rate={fmt_ratio(stage_tx_rdram, out_writes)}")
    print(f"stage_texel_source_synth_rate={fmt_ratio(stage_tx_synth, out_writes)}")
    print(f"work_fill_share={fmt_ratio(work_fill, work_fill + work_texrect + work_tri)}")
    print(f"work_texrect_share={fmt_ratio(work_texrect, work_fill + work_texrect + work_tri)}")
    print(f"work_triangle_share={fmt_ratio(work_tri, work_fill + work_texrect + work_tri)}")
    print(f"work_textured_share={fmt_ratio(work_textured, work_fill + work_texrect + work_tri)}")
    print(f"write_fill_share={fmt_ratio(write_fill, out_writes)}")
    print(f"write_texrect_share={fmt_ratio(write_texrect, out_writes)}")
    print(f"write_triangle_share={fmt_ratio(write_tri, out_writes)}")
    print(f"triangle_degenerate_rejects={tri_deg_reject}")
    print(f"triangle_bounds_rejects={tri_bounds_reject}")
    print(f"triangle_scissor_field_rejects={tri_scissor_reject}")
    print(f"triangle_alpha_reject_rate={fmt_ratio(tri_alpha_reject, tri_samples)}")
    print(f"triangle_coverage_reject_rate={fmt_ratio(tri_cvg_reject, tri_samples)}")
    print(f"triangle_depth_reject_rate={fmt_ratio(tri_depth_reject, tri_samples)}")
    print(f"triangle_nonblack_write_ratio={fmt_ratio(tri_nonblack, write_tri)}")
    print(f"texrect_nonblack_write_ratio={fmt_ratio(texrect_nonblack, write_texrect)}")
    print(f"triangle_luma_per_write={fmt_ratio(tri_luma_sum, write_tri)}")
    print(f"texrect_luma_per_write={fmt_ratio(texrect_luma_sum, write_texrect)}")
    print(f"focus_metrics_source={focus_metrics_source}")
    print(f"focus_fill_write_mask_unset_ratio={fmt_ratio(focus_fill_write_mask_unset, focus_fill_writes)}")
    print(f"focus_fill_overwrite_black_ratio={fmt_ratio(focus_fill_overwrite_black, focus_fill_writes)}")
    print(f"focus_fill_prev_nonblack_ratio={fmt_ratio(focus_fill_prev_nonblack, focus_fill_writes)}")
    print(f"focus_fill_new_nonblack_ratio={fmt_ratio(focus_fill_new_nonblack, focus_fill_writes)}")
    print(f"focus_fill_luma_delta_per_write={focus_fill_luma_delta_per_write:.6f}")
    print(f"focus_texrect_write_mask_unset_ratio={fmt_ratio(focus_texrect_write_mask_unset, focus_texrect_writes)}")
    print(f"focus_texrect_overwrite_black_ratio={fmt_ratio(focus_texrect_overwrite_black, focus_texrect_writes)}")
    print(f"focus_texrect_prev_nonblack_ratio={fmt_ratio(focus_texrect_prev_nonblack, focus_texrect_writes)}")
    print(f"focus_texrect_new_nonblack_ratio={fmt_ratio(focus_texrect_new_nonblack, focus_texrect_writes)}")
    print(f"focus_texrect_luma_delta_per_write={focus_texrect_luma_delta_per_write:.6f}")
    print(f"focus_texrect_texel_repeat_ratio={fmt_ratio(focus_texrect_texel_repeat, focus_texrect_texel_repeat + focus_texrect_texel_change)}")
    print(f"focus_texrect_tlut_applied_ratio={fmt_ratio(focus_texrect_tlut_applied, focus_texrect_writes)}")
    print(
        f"focus_texrect_tlut_lookup_repeat_ratio={fmt_ratio(focus_texrect_tlut_lookup_repeat, focus_texrect_tlut_lookup_repeat + focus_texrect_tlut_lookup_change)}"
    )
    print(
        f"focus_texrect_tlut_lookup_invalid_ratio={fmt_ratio(focus_texrect_tlut_lookup_invalid, focus_texrect_tlut_applied + focus_texrect_tlut_lookup_invalid)}"
    )
    print(f"color_image_switches={ci_switches}")
    print(f"color_image_switches_per_record={fmt_ratio(ci_switches, len(records))}")
    print(f"vi_valid_rate={fmt_ratio(vi_valid_count, len(records))}")
    print(f"vi_use_register_rate={fmt_ratio(vi_use_regs_count, len(records))}")
    print(f"vi_reject_rate={fmt_ratio(vi_reject_count, len(records))}")
    print(f"vi_origin_match_rate={fmt_ratio(vi_origin_match_count, len(records))}")
    print(f"vi_source_invalid_rate={fmt_ratio(vi_src_invalid, vi_src_samples)}")
    print(f"vi_output_nonblack_rate={fmt_ratio(vi_out_nonblack, present_pixel_count)}")
    print(
        "present_select_share="
        + ",".join(
            f"s{key}:{ratio(count, len(records)):.6f}"
            for key, count in sorted(present_select_counts.items())
        )
    )
    print(
        "stage_texel_to_final_top_classes="
        + format_stage_top(stage_cls_t2f, stage_cls_writes)
    )
    print(
        "stage_combiner_to_blender_top_classes="
        + format_stage_top(stage_cls_c2b, stage_cls_writes)
    )
    print(
        "stage_blend_p_memory_top_classes="
        + format_stage_top(stage_cls_bpmem, stage_cls_writes)
    )
    print(
        "stage_blend_m_memory_top_classes="
        + format_stage_top(stage_cls_bmmem, stage_cls_writes)
    )
    print(
        "stage_image_read_top_classes="
        + format_stage_top(stage_cls_imread, stage_cls_writes)
    )
    print(
        "stage_texel_source_replacement_top_classes="
        + format_stage_top(stage_cls_tx_repl, stage_cls_writes)
    )
    print(
        "stage_texel_source_tmem_top_classes="
        + format_stage_top(stage_cls_tx_tmem, stage_cls_writes)
    )
    print(
        "stage_texel_source_rdram_top_classes="
        + format_stage_top(stage_cls_tx_rdram, stage_cls_writes)
    )
    print(
        "stage_texel_source_synth_top_classes="
        + format_stage_top(stage_cls_tx_synth, stage_cls_writes)
    )
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
    print(
        "blend_color_p_selector_share="
        + ",".join(
            f"s{i}:{ratio(blend_p_sel[i], sum(blend_p_sel)):.6f}" for i in range(4)
        )
    )
    print(
        "blend_color_m_selector_share="
        + ",".join(
            f"s{i}:{ratio(blend_m_sel[i], sum(blend_m_sel)):.6f}" for i in range(4)
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
