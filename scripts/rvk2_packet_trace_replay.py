#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, List, Optional


FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
U64_MASK = 0xFFFFFFFFFFFFFFFF

COMMAND_DOMAIN_RDP = 2

CMD_LOAD_SYNC = 0x26
CMD_PIPE_SYNC = 0x27
CMD_TILE_SYNC = 0x28
CMD_FULL_SYNC = 0x29
CMD_SET_KEY_GB = 0x2A
CMD_SET_KEY_R = 0x2B
CMD_SET_CONVERT = 0x2C
CMD_SET_SCISSOR = 0x2D
CMD_SET_PRIM_DEPTH = 0x2E
CMD_SET_OTHER_MODES = 0x2F
CMD_LOAD_TLUT = 0x30
CMD_SET_TILE_SIZE = 0x32
CMD_LOAD_BLOCK = 0x33
CMD_LOAD_TILE = 0x34
CMD_SET_TILE = 0x35
CMD_SET_FILL_COLOR = 0x37
CMD_SET_FOG_COLOR = 0x38
CMD_SET_BLEND_COLOR = 0x39
CMD_SET_PRIM_COLOR = 0x3A
CMD_SET_ENV_COLOR = 0x3B
CMD_SET_COMBINE_MODE = 0x3C
CMD_SET_TEXTURE_IMAGE = 0x3D
CMD_SET_DEPTH_IMAGE = 0x3E
CMD_SET_COLOR_IMAGE = 0x3F

RDP_STATE_CHANGED_NONE = 0
RDP_STATE_CHANGED_COMBINE = 1 << 0
RDP_STATE_CHANGED_OTHER_MODES = 1 << 1
RDP_STATE_CHANGED_SCISSOR = 1 << 2
RDP_STATE_CHANGED_COLOR_IMAGE = 1 << 3
RDP_STATE_CHANGED_DEPTH_IMAGE = 1 << 4
RDP_STATE_CHANGED_LOAD_SYNC = 1 << 5
RDP_STATE_CHANGED_PIPE_SYNC = 1 << 6
RDP_STATE_CHANGED_TILE_SYNC = 1 << 7
RDP_STATE_CHANGED_FULL_SYNC = 1 << 8
RDP_STATE_CHANGED_PRIM_COLOR = 1 << 9
RDP_STATE_CHANGED_ENV_COLOR = 1 << 10
RDP_STATE_CHANGED_BLEND_COLOR = 1 << 11
RDP_STATE_CHANGED_FOG_COLOR = 1 << 12
RDP_STATE_CHANGED_FILL_COLOR = 1 << 13
RDP_STATE_CHANGED_PRIM_DEPTH = 1 << 14
RDP_STATE_CHANGED_CONVERT = 1 << 15
RDP_STATE_CHANGED_KEY_R = 1 << 16
RDP_STATE_CHANGED_KEY_GB = 1 << 17

TMEM_STATE_CHANGED_NONE = 0
TMEM_STATE_CHANGED_TEXTURE_IMAGE = 1 << 0
TMEM_STATE_CHANGED_TILE_DESCRIPTOR = 1 << 1
TMEM_STATE_CHANGED_TILE_SIZE = 1 << 2
TMEM_STATE_CHANGED_LOAD_TILE = 1 << 3
TMEM_STATE_CHANGED_LOAD_BLOCK = 1 << 4
TMEM_STATE_CHANGED_LOAD_TLUT = 1 << 5

TMEM_LOAD_KIND_NONE = 0
TMEM_LOAD_KIND_TILE = 1
TMEM_LOAD_KIND_BLOCK = 2
TMEM_LOAD_KIND_TLUT = 3
TMEM_TILE_COUNT = 8
MAX_INLINE_EXTRA_WORDS = 6

RENDER_PHASE_UNKNOWN = 0
RENDER_PHASE_CYCLE1 = 1
RENDER_PHASE_CYCLE2 = 2
RENDER_PHASE_COPY = 3
RENDER_PHASE_FILL = 4

RENDER_BARRIER_NONE = 0
RENDER_BARRIER_LOAD_SYNC = 1 << 0
RENDER_BARRIER_PIPE_SYNC = 1 << 1
RENDER_BARRIER_TILE_SYNC = 1 << 2
RENDER_BARRIER_FULL_SYNC = 1 << 3

SUBMISSION_SPLIT_START = 0
SUBMISSION_SPLIT_BARRIER = 1
SUBMISSION_SPLIT_PHASE_CHANGE = 2
SUBMISSION_SPLIT_CYCLE_TYPE_CHANGE = 3
SUBMISSION_SPLIT_RENDER_TARGET_CHANGE = 4
SUBMISSION_SPLIT_SCISSOR_CHANGE = 5


class TraceParseError(Exception):
    pass


@dataclass
class PacketRecord:
    line_no: int
    packet_id: int
    domain: int
    opcode: int
    flags: int
    w0: int
    w1: int
    extra_word_count: int
    w2: int
    w3: int
    w4: int
    w5: int
    w6: int
    w7: int
    full_word_count: int
    tail_hash: int
    task_id: int
    dlist_address: int
    microcode: int
    frame_microcode_type: int
    payload_word_count: int
    payload_words: List[int] = field(default_factory=list)


@dataclass
class FrameRecord:
    line_no: int
    frame_id: int
    command_count: int
    draw_semantic_count: int
    raster_op_count: int
    render_work_count: int
    last_packet_id: int
    command_hash: int
    state_hash: int
    draw_semantic_hash: int
    raster_op_hash: int
    render_work_hash: int
    submission_batch_count: int
    submission_batch_hash: int
    executor_work_count: int = -1
    executor_batch_count: int = -1
    executor_color_write_count: int = -1
    executor_surface_count: int = -1
    executor_present_hash: int = -1
    executor_present_width: int = -1
    executor_present_height: int = -1
    executor_present_aspect_x: int = -1
    executor_present_aspect_y: int = -1
    unknown_rdp_opcode_count: int = -1
    first_unknown_rdp_packet_id: int = -1
    first_unknown_rdp_opcode: int = -1
    truncated_payload_count: int = -1
    first_truncated_payload_packet_id: int = -1
    first_truncated_payload_opcode: int = -1
    microcode_type: int = -1
    packets: List[PacketRecord] = field(default_factory=list)
    semantics: List["DrawSemanticRecord"] = field(default_factory=list)
    raster_ops: List["RasterOpRecord"] = field(default_factory=list)
    render_work: List["RenderWorkRecord"] = field(default_factory=list)
    submission_batches: List["SubmissionBatchRecord"] = field(default_factory=list)
    legacy_semantic_rows: bool = False
    legacy_raster_rows: bool = False
    legacy_render_work_rows: bool = False
    legacy_submission_rows: bool = False


@dataclass
class OtherModesDecoded:
    alpha_compare: int = 0
    depth_source: int = 0
    cvg_dest: int = 0
    depth_mode: int = 0
    c2_m2b: int = 0
    c1_m2b: int = 0
    c2_m2a: int = 0
    c1_m2a: int = 0
    c2_m1b: int = 0
    c1_m1b: int = 0
    c2_m1a: int = 0
    c1_m1a: int = 0
    blend_mask: int = 0
    alpha_dither: int = 0
    color_dither: int = 0
    texture_filter: int = 0
    texture_lut: int = 0
    texture_detail: int = 0
    aa_enable: bool = False
    depth_compare: bool = False
    depth_update: bool = False
    image_read: bool = False
    color_on_cvg: bool = False
    cvg_x_alpha: bool = False
    alpha_cvg_sel: bool = False
    force_blender: bool = False
    texture_edge: bool = False
    combine_key: bool = False
    convert_one: bool = False
    bi_lerp1: bool = False
    bi_lerp0: bool = False
    texture_lod: bool = False
    texture_persp: bool = False
    unused_color_dither: bool = False
    pipeline_mode: bool = False


@dataclass
class ColorRGBA8:
    r: int = 0
    g: int = 0
    b: int = 0
    a: int = 0


@dataclass
class RDPStateSnapshot:
    last_packet_id: int = 0
    combine_mux: int = 0
    other_modes: int = 0
    other_modes_decoded: OtherModesDecoded = field(default_factory=OtherModesDecoded)
    cycle_type: int = 0
    color_image_format: int = 0
    color_image_size: int = 0
    color_image_width: int = 0
    color_image_address: int = 0
    depth_image_address: int = 0
    scissor_mode: int = 0
    scissor_xh: int = 0
    scissor_yh: int = 0
    scissor_xl: int = 0
    scissor_yl: int = 0
    prim_color: ColorRGBA8 = field(default_factory=ColorRGBA8)
    env_color: ColorRGBA8 = field(default_factory=ColorRGBA8)
    blend_color: ColorRGBA8 = field(default_factory=ColorRGBA8)
    fog_color: ColorRGBA8 = field(default_factory=ColorRGBA8)
    prim_color_min_level: int = 0
    prim_color_lod_frac: int = 0
    fill_color: int = 0
    prim_depth_z: int = 0
    prim_depth_delta: int = 0
    convert_k0: int = 0
    convert_k1: int = 0
    convert_k2: int = 0
    convert_k3: int = 0
    convert_k4: int = 0
    convert_k5: int = 0
    key_center_r: int = 0
    key_scale_r: int = 0
    key_center_g: int = 0
    key_scale_g: int = 0
    key_center_b: int = 0
    key_scale_b: int = 0
    key_width_r: int = 0
    key_width_g: int = 0
    key_width_b: int = 0
    sync_epoch: int = 0
    load_sync_count: int = 0
    pipe_sync_count: int = 0
    tile_sync_count: int = 0
    full_sync_count: int = 0
    load_sync_packet_id: int = 0
    pipe_sync_packet_id: int = 0
    tile_sync_packet_id: int = 0
    full_sync_packet_id: int = 0
    changed_mask: int = RDP_STATE_CHANGED_NONE


@dataclass
class TileDescriptorState:
    format: int = 0
    size: int = 0
    line: int = 0
    tmem: int = 0
    palette: int = 0
    cmt: int = 0
    cms: int = 0
    maskt: int = 0
    masks: int = 0
    shiftt: int = 0
    shifts: int = 0
    uls: int = 0
    ult: int = 0
    lrs: int = 0
    lrt: int = 0


@dataclass
class TMEMLoadRecord:
    source_packet_id: int = 0
    kind: int = TMEM_LOAD_KIND_NONE
    tile: int = 0
    uls: int = 0
    ult: int = 0
    lrs: int = 0
    lrt: int = 0
    dxt: int = 0


@dataclass
class TMEMSnapshot:
    last_packet_id: int = 0
    texture_image_format: int = 0
    texture_image_size: int = 0
    texture_image_width: int = 0
    texture_image_address: int = 0
    tiles: List[TileDescriptorState] = field(
        default_factory=lambda: [TileDescriptorState() for _ in range(TMEM_TILE_COUNT)]
    )
    last_load: TMEMLoadRecord = field(default_factory=TMEMLoadRecord)
    changed_mask: int = TMEM_STATE_CHANGED_NONE


@dataclass
class DrawSemanticRecord:
    source_packet_id: int
    source_opcode: int
    draw_type: int
    tile: int
    tex_rect_flip: bool
    cycle_type: int
    combine_mux: int
    blend_mux1: int
    blend_mux2: int
    blend_params: int
    rect_ulx: int
    rect_uly: int
    rect_lrx: int
    rect_lry: int
    tex_s: int
    tex_t: int
    tex_dsdx: int
    tex_dtdy: int
    triangle_lmajor: bool
    triangle_level: int
    triangle_yl: int
    triangle_ym: int
    triangle_yh: int
    triangle_xl: int
    triangle_xh: int
    triangle_xm: int
    triangle_dxldy: int
    triangle_dxhdy: int
    triangle_dxmdy: int
    triangle_shade_enable: bool
    triangle_texture_enable: bool
    triangle_zbuffer_enable: bool
    triangle_shade_r: int
    triangle_shade_g: int
    triangle_shade_b: int
    triangle_shade_a: int
    triangle_shade_drdx: int
    triangle_shade_dgdx: int
    triangle_shade_dbdx: int
    triangle_shade_dadx: int
    triangle_shade_drde: int
    triangle_shade_dgde: int
    triangle_shade_dbde: int
    triangle_shade_dade: int
    triangle_shade_drdy: int
    triangle_shade_dgdy: int
    triangle_shade_dbdy: int
    triangle_shade_dady: int
    triangle_tex_s: int
    triangle_tex_t: int
    triangle_tex_w: int
    triangle_tex_dsdx: int
    triangle_tex_dtdx: int
    triangle_tex_dwdx: int
    triangle_tex_dsde: int
    triangle_tex_dtde: int
    triangle_tex_dwde: int
    triangle_tex_dsdy: int
    triangle_tex_dtdy: int
    triangle_tex_dwdy: int
    triangle_z: int
    triangle_dzdx: int
    triangle_dzde: int
    triangle_dzdy: int
    textured: bool
    depth_test: bool
    sync_epoch: int
    load_sync_packet_id: int
    pipe_sync_packet_id: int
    tile_sync_packet_id: int
    full_sync_packet_id: int


@dataclass
class RasterOpRecord:
    source_packet_id: int
    source_opcode: int
    op_kind: int
    cycle_type: int
    tile: int
    tex_rect_flip: bool
    textured: bool
    depth_test: bool
    rect_ulx: int
    rect_uly: int
    rect_lrx: int
    rect_lry: int
    tex_s: int
    tex_t: int
    tex_dsdx: int
    tex_dtdy: int
    triangle_lmajor: bool
    triangle_level: int
    triangle_yl: int
    triangle_ym: int
    triangle_yh: int
    triangle_xl: int
    triangle_xh: int
    triangle_xm: int
    triangle_dxldy: int
    triangle_dxhdy: int
    triangle_dxmdy: int
    triangle_shade_enable: bool
    triangle_texture_enable: bool
    triangle_zbuffer_enable: bool
    triangle_shade_r: int
    triangle_shade_g: int
    triangle_shade_b: int
    triangle_shade_a: int
    triangle_shade_drdx: int
    triangle_shade_dgdx: int
    triangle_shade_dbdx: int
    triangle_shade_dadx: int
    triangle_shade_drde: int
    triangle_shade_dgde: int
    triangle_shade_dbde: int
    triangle_shade_dade: int
    triangle_shade_drdy: int
    triangle_shade_dgdy: int
    triangle_shade_dbdy: int
    triangle_shade_dady: int
    triangle_tex_s: int
    triangle_tex_t: int
    triangle_tex_w: int
    triangle_tex_dsdx: int
    triangle_tex_dtdx: int
    triangle_tex_dwdx: int
    triangle_tex_dsde: int
    triangle_tex_dtde: int
    triangle_tex_dwde: int
    triangle_tex_dsdy: int
    triangle_tex_dtdy: int
    triangle_tex_dwdy: int
    triangle_z: int
    triangle_dzdx: int
    triangle_dzde: int
    triangle_dzdy: int
    combine_mux: int
    blend_params: int
    fill_color: int
    sync_epoch: int
    load_sync_packet_id: int
    pipe_sync_packet_id: int
    tile_sync_packet_id: int
    full_sync_packet_id: int


@dataclass
class RenderWorkRecord:
    source_packet_id: int
    source_opcode: int
    op_kind: int
    phase: int
    cycle_type: int
    barrier_mask: int
    tile: int
    tex_rect_flip: bool
    textured: bool
    depth_test: bool
    rect_ulx: int
    rect_uly: int
    rect_lrx: int
    rect_lry: int
    tex_s: int
    tex_t: int
    tex_dsdx: int
    tex_dtdy: int
    triangle_lmajor: bool
    triangle_level: int
    triangle_yl: int
    triangle_ym: int
    triangle_yh: int
    triangle_xl: int
    triangle_xh: int
    triangle_xm: int
    triangle_dxldy: int
    triangle_dxhdy: int
    triangle_dxmdy: int
    triangle_shade_enable: bool
    triangle_texture_enable: bool
    triangle_zbuffer_enable: bool
    triangle_shade_r: int
    triangle_shade_g: int
    triangle_shade_b: int
    triangle_shade_a: int
    triangle_shade_drdx: int
    triangle_shade_dgdx: int
    triangle_shade_dbdx: int
    triangle_shade_dadx: int
    triangle_shade_drde: int
    triangle_shade_dgde: int
    triangle_shade_dbde: int
    triangle_shade_dade: int
    triangle_shade_drdy: int
    triangle_shade_dgdy: int
    triangle_shade_dbdy: int
    triangle_shade_dady: int
    triangle_tex_s: int
    triangle_tex_t: int
    triangle_tex_w: int
    triangle_tex_dsdx: int
    triangle_tex_dtdx: int
    triangle_tex_dwdx: int
    triangle_tex_dsde: int
    triangle_tex_dtde: int
    triangle_tex_dwde: int
    triangle_tex_dsdy: int
    triangle_tex_dtdy: int
    triangle_tex_dwdy: int
    triangle_z: int
    triangle_dzdx: int
    triangle_dzde: int
    triangle_dzdy: int
    color_image_format: int
    color_image_size: int
    color_image_width: int
    color_image_address: int
    depth_image_address: int
    scissor_mode: int
    scissor_xh: int
    scissor_yh: int
    scissor_xl: int
    scissor_yl: int
    texture_image_format: int
    texture_image_size: int
    texture_image_width: int
    texture_image_address: int
    tile_format: int
    tile_size: int
    tile_line: int
    tile_tmem: int
    tile_palette: int
    tile_cmt: int
    tile_cms: int
    tile_maskt: int
    tile_masks: int
    tile_shiftt: int
    tile_shifts: int
    tile_uls: int
    tile_ult: int
    tile_lrs: int
    tile_lrt: int
    tmem_load_kind: int
    tmem_load_tile: int
    tmem_load_uls: int
    tmem_load_ult: int
    tmem_load_lrs: int
    tmem_load_lrt: int
    tmem_load_dxt: int
    combine_mux: int
    blend_params: int
    fill_color: int
    sync_epoch: int
    load_sync_packet_id: int
    pipe_sync_packet_id: int
    tile_sync_packet_id: int
    full_sync_packet_id: int
    alpha_compare: int = 0
    cvg_dest: int = 0
    blend_mask: int = 0
    cvg_x_alpha: bool = False
    alpha_cvg_sel: bool = False
    color_on_cvg: bool = False
    force_blender: bool = False
    depth_compare_enable: bool = True
    depth_update_enable: bool = True
    other_modes: int = 0
    prim_color: int = 0
    env_color: int = 0
    blend_color: int = 0
    fog_color: int = 0
    key_state: int = 0
    convert_state: int = 0
    depth_source: int = 0
    prim_depth_z: int = 0
    prim_depth_delta: int = 0


@dataclass
class RenderPlanReplayState:
    last_load_sync_packet_id: int = 0
    last_pipe_sync_packet_id: int = 0
    last_tile_sync_packet_id: int = 0
    last_full_sync_packet_id: int = 0


@dataclass
class SubmissionBatchRecord:
    batch_index: int
    split_reason: int
    split_barrier_mask: int
    phase: int
    cycle_type: int
    first_work_index: int
    last_work_index: int
    work_count: int
    barrier_mask_union: int
    textured_work_count: int
    depth_test_work_count: int
    first_source_packet_id: int
    last_source_packet_id: int
    color_image_format: int
    color_image_size: int
    color_image_width: int
    color_image_address: int
    depth_image_address: int
    scissor_mode: int
    scissor_xh: int
    scissor_yh: int
    scissor_xl: int
    scissor_yl: int


@dataclass
class FrameCheck:
    frame_id: int
    header_line_no: int
    declared_command_count: int
    parsed_command_count: int
    declared_last_packet_id: int
    computed_last_packet_id: int
    declared_command_hash: int
    computed_command_hash: int
    declared_state_hash: int
    computed_state_hash: int
    declared_draw_semantic_count: int
    computed_draw_semantic_count: int
    declared_draw_semantic_hash: int
    computed_draw_semantic_hash: int
    declared_raster_op_count: int
    computed_raster_op_count: int
    declared_raster_op_hash: int
    computed_raster_op_hash: int
    declared_render_work_count: int
    computed_render_work_count: int
    declared_render_work_hash: int
    computed_render_work_hash: int
    declared_submission_batch_count: int
    computed_submission_batch_count: int
    declared_submission_batch_hash: int
    computed_submission_batch_hash: int
    declared_executor_work_count: int
    computed_executor_work_count: int
    declared_executor_batch_count: int
    computed_executor_batch_count: int
    declared_executor_color_write_count: int
    computed_executor_color_write_count: int
    declared_executor_surface_count: int
    computed_executor_surface_count: int
    declared_executor_present_hash: int
    computed_executor_present_hash: int
    declared_executor_present_width: int
    computed_executor_present_width: int
    declared_executor_present_height: int
    computed_executor_present_height: int
    declared_executor_present_aspect_x: int
    computed_executor_present_aspect_x: int
    declared_executor_present_aspect_y: int
    computed_executor_present_aspect_y: int
    declared_unknown_rdp_opcode_count: int
    computed_unknown_rdp_opcode_count: int
    declared_first_unknown_rdp_packet_id: int
    computed_first_unknown_rdp_packet_id: int
    declared_first_unknown_rdp_opcode: int
    computed_first_unknown_rdp_opcode: int
    declared_truncated_payload_count: int
    computed_truncated_payload_count: int
    declared_first_truncated_payload_packet_id: int
    computed_first_truncated_payload_packet_id: int
    declared_first_truncated_payload_opcode: int
    computed_first_truncated_payload_opcode: int
    errors: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return len(self.errors) == 0


def _parse_uint(token: str, field_name: str, line_no: int, bits: int) -> int:
    try:
        value = int(token, 0)
    except ValueError as err:
        raise TraceParseError(
            f"line {line_no}: invalid integer for {field_name}: '{token}'"
        ) from err
    if value < 0:
        raise TraceParseError(f"line {line_no}: negative value for {field_name}: {value}")
    max_value = (1 << bits) - 1
    if value > max_value:
        raise TraceParseError(
            f"line {line_no}: {field_name} out of range for u{bits}: {value}"
        )
    return value


def _expect_columns(fields: List[str], expected_count: int, line_no: int) -> None:
    if len(fields) != expected_count:
        raise TraceParseError(
            f"line {line_no}: expected {expected_count} tab-separated columns, got {len(fields)}"
        )


def parse_packet_trace(path: Path) -> List[FrameRecord]:
    frames: List[FrameRecord] = []
    current_frame: Optional[FrameRecord] = None

    with path.open("r", encoding="utf-8") as trace_file:
        for line_no, raw_line in enumerate(trace_file, start=1):
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue

            fields = line.split("\t")
            row_type = fields[0]

            if row_type == "F":
                if len(fields) not in (7, 9, 12, 15, 17, 19, 21, 30):
                    raise TraceParseError(
                        f"line {line_no}: frame row expected 7, 9, 12, 15, 17, 19, 21, or 30 columns, got {len(fields)}"
                    )
                if current_frame is not None:
                    frames.append(current_frame)
                if len(fields) == 30:
                    current_frame = FrameRecord(
                        line_no=line_no,
                        frame_id=_parse_uint(fields[1], "frame_id", line_no, 64),
                        command_count=_parse_uint(fields[2], "command_count", line_no, 64),
                        draw_semantic_count=_parse_uint(
                            fields[3], "draw_semantic_count", line_no, 64
                        ),
                        raster_op_count=_parse_uint(
                            fields[4], "raster_op_count", line_no, 64
                        ),
                        render_work_count=_parse_uint(
                            fields[16], "render_work_count", line_no, 64
                        ),
                        submission_batch_count=_parse_uint(
                            fields[18], "submission_batch_count", line_no, 64
                        ),
                        last_packet_id=_parse_uint(fields[5], "last_packet_id", line_no, 64),
                        command_hash=_parse_uint(fields[6], "command_hash", line_no, 64),
                        state_hash=_parse_uint(fields[7], "state_hash", line_no, 64),
                        draw_semantic_hash=_parse_uint(
                            fields[8], "draw_semantic_hash", line_no, 64
                        ),
                        raster_op_hash=_parse_uint(
                            fields[9], "raster_op_hash", line_no, 64
                        ),
                        render_work_hash=_parse_uint(
                            fields[17], "render_work_hash", line_no, 64
                        ),
                        submission_batch_hash=_parse_uint(
                            fields[19], "submission_batch_hash", line_no, 64
                        ),
                        executor_work_count=_parse_uint(
                            fields[20], "executor_work_count", line_no, 64
                        ),
                        executor_batch_count=_parse_uint(
                            fields[21], "executor_batch_count", line_no, 64
                        ),
                        executor_color_write_count=_parse_uint(
                            fields[22], "executor_color_write_count", line_no, 64
                        ),
                        executor_surface_count=_parse_uint(
                            fields[23], "executor_surface_count", line_no, 64
                        ),
                        executor_present_hash=_parse_uint(
                            fields[24], "executor_present_hash", line_no, 64
                        ),
                        executor_present_width=_parse_uint(
                            fields[25], "executor_present_width", line_no, 32
                        ),
                        executor_present_height=_parse_uint(
                            fields[26], "executor_present_height", line_no, 32
                        ),
                        executor_present_aspect_x=_parse_uint(
                            fields[27], "executor_present_aspect_x", line_no, 8
                        ),
                        executor_present_aspect_y=_parse_uint(
                            fields[28], "executor_present_aspect_y", line_no, 8
                        ),
                        unknown_rdp_opcode_count=_parse_uint(
                            fields[10], "unknown_rdp_opcode_count", line_no, 32
                        ),
                        first_unknown_rdp_packet_id=_parse_uint(
                            fields[11], "first_unknown_rdp_packet_id", line_no, 64
                        ),
                        first_unknown_rdp_opcode=_parse_uint(
                            fields[12], "first_unknown_rdp_opcode", line_no, 8
                        ),
                        truncated_payload_count=_parse_uint(
                            fields[13], "truncated_payload_count", line_no, 32
                        ),
                        first_truncated_payload_packet_id=_parse_uint(
                            fields[14], "first_truncated_payload_packet_id", line_no, 64
                        ),
                        first_truncated_payload_opcode=_parse_uint(
                            fields[15], "first_truncated_payload_opcode", line_no, 8
                        ),
                        microcode_type=_parse_uint(fields[29], "microcode_type", line_no, 32),
                    )
                elif len(fields) == 21:
                    current_frame = FrameRecord(
                        line_no=line_no,
                        frame_id=_parse_uint(fields[1], "frame_id", line_no, 64),
                        command_count=_parse_uint(fields[2], "command_count", line_no, 64),
                        draw_semantic_count=_parse_uint(
                            fields[3], "draw_semantic_count", line_no, 64
                        ),
                        raster_op_count=_parse_uint(
                            fields[4], "raster_op_count", line_no, 64
                        ),
                        render_work_count=_parse_uint(
                            fields[16], "render_work_count", line_no, 64
                        ),
                        submission_batch_count=_parse_uint(
                            fields[18], "submission_batch_count", line_no, 64
                        ),
                        last_packet_id=_parse_uint(fields[5], "last_packet_id", line_no, 64),
                        command_hash=_parse_uint(fields[6], "command_hash", line_no, 64),
                        state_hash=_parse_uint(fields[7], "state_hash", line_no, 64),
                        draw_semantic_hash=_parse_uint(
                            fields[8], "draw_semantic_hash", line_no, 64
                        ),
                        raster_op_hash=_parse_uint(
                            fields[9], "raster_op_hash", line_no, 64
                        ),
                        render_work_hash=_parse_uint(
                            fields[17], "render_work_hash", line_no, 64
                        ),
                        submission_batch_hash=_parse_uint(
                            fields[19], "submission_batch_hash", line_no, 64
                        ),
                        unknown_rdp_opcode_count=_parse_uint(
                            fields[10], "unknown_rdp_opcode_count", line_no, 32
                        ),
                        first_unknown_rdp_packet_id=_parse_uint(
                            fields[11], "first_unknown_rdp_packet_id", line_no, 64
                        ),
                        first_unknown_rdp_opcode=_parse_uint(
                            fields[12], "first_unknown_rdp_opcode", line_no, 8
                        ),
                        truncated_payload_count=_parse_uint(
                            fields[13], "truncated_payload_count", line_no, 32
                        ),
                        first_truncated_payload_packet_id=_parse_uint(
                            fields[14], "first_truncated_payload_packet_id", line_no, 64
                        ),
                        first_truncated_payload_opcode=_parse_uint(
                            fields[15], "first_truncated_payload_opcode", line_no, 8
                        ),
                        microcode_type=_parse_uint(fields[20], "microcode_type", line_no, 32),
                    )
                elif len(fields) == 19:
                    current_frame = FrameRecord(
                        line_no=line_no,
                        frame_id=_parse_uint(fields[1], "frame_id", line_no, 64),
                        command_count=_parse_uint(fields[2], "command_count", line_no, 64),
                        draw_semantic_count=_parse_uint(
                            fields[3], "draw_semantic_count", line_no, 64
                        ),
                        raster_op_count=_parse_uint(
                            fields[4], "raster_op_count", line_no, 64
                        ),
                        render_work_count=_parse_uint(
                            fields[16], "render_work_count", line_no, 64
                        ),
                        submission_batch_count=-1,
                        last_packet_id=_parse_uint(fields[5], "last_packet_id", line_no, 64),
                        command_hash=_parse_uint(fields[6], "command_hash", line_no, 64),
                        state_hash=_parse_uint(fields[7], "state_hash", line_no, 64),
                        draw_semantic_hash=_parse_uint(
                            fields[8], "draw_semantic_hash", line_no, 64
                        ),
                        raster_op_hash=_parse_uint(
                            fields[9], "raster_op_hash", line_no, 64
                        ),
                        render_work_hash=_parse_uint(
                            fields[17], "render_work_hash", line_no, 64
                        ),
                        submission_batch_hash=-1,
                        unknown_rdp_opcode_count=_parse_uint(
                            fields[10], "unknown_rdp_opcode_count", line_no, 32
                        ),
                        first_unknown_rdp_packet_id=_parse_uint(
                            fields[11], "first_unknown_rdp_packet_id", line_no, 64
                        ),
                        first_unknown_rdp_opcode=_parse_uint(
                            fields[12], "first_unknown_rdp_opcode", line_no, 8
                        ),
                        truncated_payload_count=_parse_uint(
                            fields[13], "truncated_payload_count", line_no, 32
                        ),
                        first_truncated_payload_packet_id=_parse_uint(
                            fields[14], "first_truncated_payload_packet_id", line_no, 64
                        ),
                        first_truncated_payload_opcode=_parse_uint(
                            fields[15], "first_truncated_payload_opcode", line_no, 8
                        ),
                        microcode_type=_parse_uint(fields[18], "microcode_type", line_no, 32),
                    )
                elif len(fields) == 17:
                    current_frame = FrameRecord(
                        line_no=line_no,
                        frame_id=_parse_uint(fields[1], "frame_id", line_no, 64),
                        command_count=_parse_uint(fields[2], "command_count", line_no, 64),
                        draw_semantic_count=_parse_uint(
                            fields[3], "draw_semantic_count", line_no, 64
                        ),
                        raster_op_count=_parse_uint(
                            fields[4], "raster_op_count", line_no, 64
                        ),
                        render_work_count=-1,
                        submission_batch_count=-1,
                        last_packet_id=_parse_uint(fields[5], "last_packet_id", line_no, 64),
                        command_hash=_parse_uint(fields[6], "command_hash", line_no, 64),
                        state_hash=_parse_uint(fields[7], "state_hash", line_no, 64),
                        draw_semantic_hash=_parse_uint(
                            fields[8], "draw_semantic_hash", line_no, 64
                        ),
                        raster_op_hash=_parse_uint(
                            fields[9], "raster_op_hash", line_no, 64
                        ),
                        render_work_hash=-1,
                        submission_batch_hash=-1,
                        unknown_rdp_opcode_count=_parse_uint(
                            fields[10], "unknown_rdp_opcode_count", line_no, 32
                        ),
                        first_unknown_rdp_packet_id=_parse_uint(
                            fields[11], "first_unknown_rdp_packet_id", line_no, 64
                        ),
                        first_unknown_rdp_opcode=_parse_uint(
                            fields[12], "first_unknown_rdp_opcode", line_no, 8
                        ),
                        truncated_payload_count=_parse_uint(
                            fields[13], "truncated_payload_count", line_no, 32
                        ),
                        first_truncated_payload_packet_id=_parse_uint(
                            fields[14], "first_truncated_payload_packet_id", line_no, 64
                        ),
                        first_truncated_payload_opcode=_parse_uint(
                            fields[15], "first_truncated_payload_opcode", line_no, 8
                        ),
                        microcode_type=_parse_uint(fields[16], "microcode_type", line_no, 32),
                    )
                elif len(fields) == 15:
                    current_frame = FrameRecord(
                        line_no=line_no,
                        frame_id=_parse_uint(fields[1], "frame_id", line_no, 64),
                        command_count=_parse_uint(fields[2], "command_count", line_no, 64),
                        draw_semantic_count=_parse_uint(
                            fields[3], "draw_semantic_count", line_no, 64
                        ),
                        raster_op_count=-1,
                        render_work_count=-1,
                        submission_batch_count=-1,
                        last_packet_id=_parse_uint(fields[4], "last_packet_id", line_no, 64),
                        command_hash=_parse_uint(fields[5], "command_hash", line_no, 64),
                        state_hash=_parse_uint(fields[6], "state_hash", line_no, 64),
                        draw_semantic_hash=_parse_uint(
                            fields[7], "draw_semantic_hash", line_no, 64
                        ),
                        raster_op_hash=-1,
                        render_work_hash=-1,
                        submission_batch_hash=-1,
                        unknown_rdp_opcode_count=_parse_uint(
                            fields[8], "unknown_rdp_opcode_count", line_no, 32
                        ),
                        first_unknown_rdp_packet_id=_parse_uint(
                            fields[9], "first_unknown_rdp_packet_id", line_no, 64
                        ),
                        first_unknown_rdp_opcode=_parse_uint(
                            fields[10], "first_unknown_rdp_opcode", line_no, 8
                        ),
                        truncated_payload_count=_parse_uint(
                            fields[11], "truncated_payload_count", line_no, 32
                        ),
                        first_truncated_payload_packet_id=_parse_uint(
                            fields[12], "first_truncated_payload_packet_id", line_no, 64
                        ),
                        first_truncated_payload_opcode=_parse_uint(
                            fields[13], "first_truncated_payload_opcode", line_no, 8
                        ),
                        microcode_type=_parse_uint(fields[14], "microcode_type", line_no, 32),
                    )
                elif len(fields) == 12:
                    current_frame = FrameRecord(
                        line_no=line_no,
                        frame_id=_parse_uint(fields[1], "frame_id", line_no, 64),
                        command_count=_parse_uint(fields[2], "command_count", line_no, 64),
                        draw_semantic_count=_parse_uint(
                            fields[3], "draw_semantic_count", line_no, 64
                        ),
                        raster_op_count=-1,
                        render_work_count=-1,
                        submission_batch_count=-1,
                        last_packet_id=_parse_uint(fields[4], "last_packet_id", line_no, 64),
                        command_hash=_parse_uint(fields[5], "command_hash", line_no, 64),
                        state_hash=_parse_uint(fields[6], "state_hash", line_no, 64),
                        draw_semantic_hash=_parse_uint(
                            fields[7], "draw_semantic_hash", line_no, 64
                        ),
                        raster_op_hash=-1,
                        render_work_hash=-1,
                        submission_batch_hash=-1,
                        unknown_rdp_opcode_count=_parse_uint(
                            fields[8], "unknown_rdp_opcode_count", line_no, 32
                        ),
                        first_unknown_rdp_packet_id=_parse_uint(
                            fields[9], "first_unknown_rdp_packet_id", line_no, 64
                        ),
                        first_unknown_rdp_opcode=_parse_uint(
                            fields[10], "first_unknown_rdp_opcode", line_no, 8
                        ),
                        truncated_payload_count=-1,
                        first_truncated_payload_packet_id=-1,
                        first_truncated_payload_opcode=-1,
                        microcode_type=_parse_uint(fields[11], "microcode_type", line_no, 32),
                    )
                elif len(fields) == 9:
                    current_frame = FrameRecord(
                        line_no=line_no,
                        frame_id=_parse_uint(fields[1], "frame_id", line_no, 64),
                        command_count=_parse_uint(fields[2], "command_count", line_no, 64),
                        draw_semantic_count=_parse_uint(
                            fields[3], "draw_semantic_count", line_no, 64
                        ),
                        raster_op_count=-1,
                        render_work_count=-1,
                        submission_batch_count=-1,
                        last_packet_id=_parse_uint(fields[4], "last_packet_id", line_no, 64),
                        command_hash=_parse_uint(fields[5], "command_hash", line_no, 64),
                        state_hash=_parse_uint(fields[6], "state_hash", line_no, 64),
                        draw_semantic_hash=_parse_uint(
                            fields[7], "draw_semantic_hash", line_no, 64
                        ),
                        raster_op_hash=-1,
                        render_work_hash=-1,
                        submission_batch_hash=-1,
                        unknown_rdp_opcode_count=-1,
                        first_unknown_rdp_packet_id=-1,
                        first_unknown_rdp_opcode=-1,
                        truncated_payload_count=-1,
                        first_truncated_payload_packet_id=-1,
                        first_truncated_payload_opcode=-1,
                        microcode_type=_parse_uint(fields[8], "microcode_type", line_no, 32),
                    )
                else:
                    current_frame = FrameRecord(
                        line_no=line_no,
                        frame_id=_parse_uint(fields[1], "frame_id", line_no, 64),
                        command_count=_parse_uint(fields[2], "command_count", line_no, 64),
                        draw_semantic_count=-1,
                        raster_op_count=-1,
                        render_work_count=-1,
                        submission_batch_count=-1,
                        last_packet_id=_parse_uint(fields[3], "last_packet_id", line_no, 64),
                        command_hash=_parse_uint(fields[4], "command_hash", line_no, 64),
                        state_hash=_parse_uint(fields[5], "state_hash", line_no, 64),
                        draw_semantic_hash=-1,
                        raster_op_hash=-1,
                        render_work_hash=-1,
                        submission_batch_hash=-1,
                        unknown_rdp_opcode_count=-1,
                        first_unknown_rdp_packet_id=-1,
                        first_unknown_rdp_opcode=-1,
                        truncated_payload_count=-1,
                        first_truncated_payload_packet_id=-1,
                        first_truncated_payload_opcode=-1,
                        microcode_type=_parse_uint(fields[6], "microcode_type", line_no, 32),
                    )
                continue

            if row_type == "P":
                if len(fields) not in (11, 14, 20) and len(fields) < 21:
                    raise TraceParseError(
                        f"line {line_no}: packet row expected 11, 14, 20, or >=21 columns, got {len(fields)}"
                    )
                if current_frame is None:
                    raise TraceParseError(
                        f"line {line_no}: packet row encountered before first frame row"
                    )

                if len(fields) >= 21:
                    payload_word_count = _parse_uint(
                        fields[20], "payload_word_count", line_no, 8
                    )
                    expected_count = 21 + payload_word_count
                    if len(fields) != expected_count:
                        raise TraceParseError(
                            f"line {line_no}: packet row payload count mismatch, expected {expected_count} columns, got {len(fields)}"
                        )
                    payload_words = [
                        _parse_uint(
                            fields[21 + payload_index],
                            f"payload_word_{payload_index}",
                            line_no,
                            32,
                        )
                        for payload_index in range(payload_word_count)
                    ]
                    current_frame.packets.append(
                        PacketRecord(
                            line_no=line_no,
                            packet_id=_parse_uint(fields[1], "packet_id", line_no, 64),
                            domain=_parse_uint(fields[2], "domain", line_no, 8),
                            opcode=_parse_uint(fields[3], "opcode", line_no, 8),
                            flags=_parse_uint(fields[4], "flags", line_no, 8),
                            w0=_parse_uint(fields[5], "w0", line_no, 32),
                            w1=_parse_uint(fields[6], "w1", line_no, 32),
                            extra_word_count=_parse_uint(fields[10], "extra_word_count", line_no, 8),
                            w2=_parse_uint(fields[11], "w2", line_no, 32),
                            w3=_parse_uint(fields[12], "w3", line_no, 32),
                            w4=_parse_uint(fields[13], "w4", line_no, 32),
                            w5=_parse_uint(fields[14], "w5", line_no, 32),
                            w6=_parse_uint(fields[15], "w6", line_no, 32),
                            w7=_parse_uint(fields[16], "w7", line_no, 32),
                            full_word_count=_parse_uint(fields[17], "full_word_count", line_no, 16),
                            tail_hash=_parse_uint(fields[18], "tail_hash", line_no, 64),
                            task_id=_parse_uint(fields[7], "task_id", line_no, 32),
                            dlist_address=_parse_uint(fields[8], "dlist_address", line_no, 32),
                            microcode=_parse_uint(fields[9], "microcode", line_no, 32),
                            frame_microcode_type=_parse_uint(
                                fields[19], "frame_microcode_type", line_no, 32
                            ),
                            payload_word_count=payload_word_count,
                            payload_words=payload_words,
                        )
                    )
                elif len(fields) == 20:
                    extra_word_count = _parse_uint(
                        fields[10], "extra_word_count", line_no, 8
                    )
                    inline_words = [
                        _parse_uint(fields[11], "w2", line_no, 32),
                        _parse_uint(fields[12], "w3", line_no, 32),
                        _parse_uint(fields[13], "w4", line_no, 32),
                        _parse_uint(fields[14], "w5", line_no, 32),
                        _parse_uint(fields[15], "w6", line_no, 32),
                        _parse_uint(fields[16], "w7", line_no, 32),
                    ]
                    payload_word_count = min(extra_word_count, MAX_INLINE_EXTRA_WORDS)
                    current_frame.packets.append(
                        PacketRecord(
                            line_no=line_no,
                            packet_id=_parse_uint(fields[1], "packet_id", line_no, 64),
                            domain=_parse_uint(fields[2], "domain", line_no, 8),
                            opcode=_parse_uint(fields[3], "opcode", line_no, 8),
                            flags=_parse_uint(fields[4], "flags", line_no, 8),
                            w0=_parse_uint(fields[5], "w0", line_no, 32),
                            w1=_parse_uint(fields[6], "w1", line_no, 32),
                            extra_word_count=extra_word_count,
                            w2=inline_words[0],
                            w3=inline_words[1],
                            w4=inline_words[2],
                            w5=inline_words[3],
                            w6=inline_words[4],
                            w7=inline_words[5],
                            full_word_count=_parse_uint(fields[17], "full_word_count", line_no, 16),
                            tail_hash=_parse_uint(fields[18], "tail_hash", line_no, 64),
                            task_id=_parse_uint(fields[7], "task_id", line_no, 32),
                            dlist_address=_parse_uint(fields[8], "dlist_address", line_no, 32),
                            microcode=_parse_uint(fields[9], "microcode", line_no, 32),
                            frame_microcode_type=_parse_uint(
                                fields[19], "frame_microcode_type", line_no, 32
                            ),
                            payload_word_count=payload_word_count,
                            payload_words=inline_words[:payload_word_count],
                        )
                    )
                elif len(fields) == 14:
                    extra_word_count = _parse_uint(
                        fields[10], "extra_word_count", line_no, 8
                    )
                    inline_words = [
                        _parse_uint(fields[11], "w2", line_no, 32),
                        _parse_uint(fields[12], "w3", line_no, 32),
                    ]
                    payload_word_count = min(extra_word_count, len(inline_words))
                    current_frame.packets.append(
                        PacketRecord(
                            line_no=line_no,
                            packet_id=_parse_uint(fields[1], "packet_id", line_no, 64),
                            domain=_parse_uint(fields[2], "domain", line_no, 8),
                            opcode=_parse_uint(fields[3], "opcode", line_no, 8),
                            flags=_parse_uint(fields[4], "flags", line_no, 8),
                            w0=_parse_uint(fields[5], "w0", line_no, 32),
                            w1=_parse_uint(fields[6], "w1", line_no, 32),
                            extra_word_count=extra_word_count,
                            w2=inline_words[0],
                            w3=inline_words[1],
                            w4=0,
                            w5=0,
                            w6=0,
                            w7=0,
                            full_word_count=0,
                            tail_hash=FNV_OFFSET,
                            task_id=_parse_uint(fields[7], "task_id", line_no, 32),
                            dlist_address=_parse_uint(fields[8], "dlist_address", line_no, 32),
                            microcode=_parse_uint(fields[9], "microcode", line_no, 32),
                            frame_microcode_type=_parse_uint(
                                fields[13], "frame_microcode_type", line_no, 32
                            ),
                            payload_word_count=payload_word_count,
                            payload_words=inline_words[:payload_word_count],
                        )
                    )
                else:
                    current_frame.packets.append(
                        PacketRecord(
                            line_no=line_no,
                            packet_id=_parse_uint(fields[1], "packet_id", line_no, 64),
                            domain=_parse_uint(fields[2], "domain", line_no, 8),
                            opcode=_parse_uint(fields[3], "opcode", line_no, 8),
                            flags=_parse_uint(fields[4], "flags", line_no, 8),
                            w0=_parse_uint(fields[5], "w0", line_no, 32),
                            w1=_parse_uint(fields[6], "w1", line_no, 32),
                            extra_word_count=0,
                            w2=0,
                            w3=0,
                            w4=0,
                            w5=0,
                            w6=0,
                            w7=0,
                            full_word_count=0,
                            tail_hash=FNV_OFFSET,
                            task_id=_parse_uint(fields[7], "task_id", line_no, 32),
                            dlist_address=_parse_uint(fields[8], "dlist_address", line_no, 32),
                            microcode=_parse_uint(fields[9], "microcode", line_no, 32),
                            frame_microcode_type=_parse_uint(
                                fields[10], "frame_microcode_type", line_no, 32
                            ),
                            payload_word_count=0,
                            payload_words=[],
                        )
                    )
                continue

            if row_type == "S":
                if len(fields) not in (14, 26, 37, 72):
                    raise TraceParseError(
                        f"line {line_no}: semantic row expected 14, 26, 37, or 72 columns, got {len(fields)}"
                    )
                if current_frame is None:
                    raise TraceParseError(
                        f"line {line_no}: semantic row encountered before first frame row"
                    )
                if len(fields) == 72:
                    current_frame.semantics.append(
                        DrawSemanticRecord(
                            source_packet_id=_parse_uint(fields[1], "source_packet_id", line_no, 64),
                            source_opcode=_parse_uint(fields[2], "source_opcode", line_no, 8),
                            draw_type=_parse_uint(fields[3], "draw_type", line_no, 8),
                            tile=_parse_uint(fields[4], "tile", line_no, 8),
                            tex_rect_flip=_parse_uint(fields[5], "tex_rect_flip", line_no, 1) != 0,
                            cycle_type=_parse_uint(fields[6], "cycle_type", line_no, 8),
                            combine_mux=_parse_uint(fields[7], "combine_mux", line_no, 64),
                            blend_mux1=_parse_uint(fields[8], "blend_mux1", line_no, 32),
                            blend_mux2=_parse_uint(fields[9], "blend_mux2", line_no, 32),
                            blend_params=_parse_uint(fields[10], "blend_params", line_no, 32),
                            rect_ulx=_parse_uint(fields[11], "rect_ulx", line_no, 16),
                            rect_uly=_parse_uint(fields[12], "rect_uly", line_no, 16),
                            rect_lrx=_parse_uint(fields[13], "rect_lrx", line_no, 16),
                            rect_lry=_parse_uint(fields[14], "rect_lry", line_no, 16),
                            tex_s=_sign_extend(_parse_uint(fields[15], "tex_s_raw", line_no, 16), 16),
                            tex_t=_sign_extend(_parse_uint(fields[16], "tex_t_raw", line_no, 16), 16),
                            tex_dsdx=_sign_extend(_parse_uint(fields[17], "tex_dsdx_raw", line_no, 16), 16),
                            tex_dtdy=_sign_extend(_parse_uint(fields[18], "tex_dtdy_raw", line_no, 16), 16),
                            triangle_lmajor=_parse_uint(fields[19], "triangle_lmajor", line_no, 1) != 0,
                            triangle_level=_parse_uint(fields[20], "triangle_level", line_no, 8),
                            triangle_yl=_parse_uint(fields[21], "triangle_yl", line_no, 16),
                            triangle_ym=_parse_uint(fields[22], "triangle_ym", line_no, 16),
                            triangle_yh=_parse_uint(fields[23], "triangle_yh", line_no, 16),
                            triangle_xl=_sign_extend(
                                _parse_uint(fields[24], "triangle_xl_raw", line_no, 32), 32
                            ),
                            triangle_xh=_sign_extend(
                                _parse_uint(fields[25], "triangle_xh_raw", line_no, 32), 32
                            ),
                            triangle_xm=_sign_extend(
                                _parse_uint(fields[26], "triangle_xm_raw", line_no, 32), 32
                            ),
                            triangle_dxldy=_sign_extend(
                                _parse_uint(fields[27], "triangle_dxldy_raw", line_no, 32), 32
                            ),
                            triangle_dxhdy=_sign_extend(
                                _parse_uint(fields[28], "triangle_dxhdy_raw", line_no, 32), 32
                            ),
                            triangle_dxmdy=_sign_extend(
                                _parse_uint(fields[29], "triangle_dxmdy_raw", line_no, 32), 32
                            ),
                            triangle_shade_enable=_parse_uint(fields[30], "triangle_shade_enable", line_no, 1) != 0,
                            triangle_texture_enable=_parse_uint(fields[31], "triangle_texture_enable", line_no, 1) != 0,
                            triangle_zbuffer_enable=_parse_uint(fields[32], "triangle_zbuffer_enable", line_no, 1) != 0,
                            triangle_shade_r=_sign_extend(_parse_uint(fields[33], "triangle_shade_r_raw", line_no, 32), 32),
                            triangle_shade_g=_sign_extend(_parse_uint(fields[34], "triangle_shade_g_raw", line_no, 32), 32),
                            triangle_shade_b=_sign_extend(_parse_uint(fields[35], "triangle_shade_b_raw", line_no, 32), 32),
                            triangle_shade_a=_sign_extend(_parse_uint(fields[36], "triangle_shade_a_raw", line_no, 32), 32),
                            triangle_shade_drdx=_sign_extend(_parse_uint(fields[37], "triangle_shade_drdx_raw", line_no, 32), 32),
                            triangle_shade_dgdx=_sign_extend(_parse_uint(fields[38], "triangle_shade_dgdx_raw", line_no, 32), 32),
                            triangle_shade_dbdx=_sign_extend(_parse_uint(fields[39], "triangle_shade_dbdx_raw", line_no, 32), 32),
                            triangle_shade_dadx=_sign_extend(_parse_uint(fields[40], "triangle_shade_dadx_raw", line_no, 32), 32),
                            triangle_shade_drde=_sign_extend(_parse_uint(fields[41], "triangle_shade_drde_raw", line_no, 32), 32),
                            triangle_shade_dgde=_sign_extend(_parse_uint(fields[42], "triangle_shade_dgde_raw", line_no, 32), 32),
                            triangle_shade_dbde=_sign_extend(_parse_uint(fields[43], "triangle_shade_dbde_raw", line_no, 32), 32),
                            triangle_shade_dade=_sign_extend(_parse_uint(fields[44], "triangle_shade_dade_raw", line_no, 32), 32),
                            triangle_shade_drdy=_sign_extend(_parse_uint(fields[45], "triangle_shade_drdy_raw", line_no, 32), 32),
                            triangle_shade_dgdy=_sign_extend(_parse_uint(fields[46], "triangle_shade_dgdy_raw", line_no, 32), 32),
                            triangle_shade_dbdy=_sign_extend(_parse_uint(fields[47], "triangle_shade_dbdy_raw", line_no, 32), 32),
                            triangle_shade_dady=_sign_extend(_parse_uint(fields[48], "triangle_shade_dady_raw", line_no, 32), 32),
                            triangle_tex_s=_sign_extend(_parse_uint(fields[49], "triangle_tex_s_raw", line_no, 32), 32),
                            triangle_tex_t=_sign_extend(_parse_uint(fields[50], "triangle_tex_t_raw", line_no, 32), 32),
                            triangle_tex_w=_sign_extend(_parse_uint(fields[51], "triangle_tex_w_raw", line_no, 32), 32),
                            triangle_tex_dsdx=_sign_extend(_parse_uint(fields[52], "triangle_tex_dsdx_raw", line_no, 32), 32),
                            triangle_tex_dtdx=_sign_extend(_parse_uint(fields[53], "triangle_tex_dtdx_raw", line_no, 32), 32),
                            triangle_tex_dwdx=_sign_extend(_parse_uint(fields[54], "triangle_tex_dwdx_raw", line_no, 32), 32),
                            triangle_tex_dsde=_sign_extend(_parse_uint(fields[55], "triangle_tex_dsde_raw", line_no, 32), 32),
                            triangle_tex_dtde=_sign_extend(_parse_uint(fields[56], "triangle_tex_dtde_raw", line_no, 32), 32),
                            triangle_tex_dwde=_sign_extend(_parse_uint(fields[57], "triangle_tex_dwde_raw", line_no, 32), 32),
                            triangle_tex_dsdy=_sign_extend(_parse_uint(fields[58], "triangle_tex_dsdy_raw", line_no, 32), 32),
                            triangle_tex_dtdy=_sign_extend(_parse_uint(fields[59], "triangle_tex_dtdy_raw", line_no, 32), 32),
                            triangle_tex_dwdy=_sign_extend(_parse_uint(fields[60], "triangle_tex_dwdy_raw", line_no, 32), 32),
                            triangle_z=_sign_extend(_parse_uint(fields[61], "triangle_z_raw", line_no, 32), 32),
                            triangle_dzdx=_sign_extend(_parse_uint(fields[62], "triangle_dzdx_raw", line_no, 32), 32),
                            triangle_dzde=_sign_extend(_parse_uint(fields[63], "triangle_dzde_raw", line_no, 32), 32),
                            triangle_dzdy=_sign_extend(_parse_uint(fields[64], "triangle_dzdy_raw", line_no, 32), 32),
                            textured=_parse_uint(fields[65], "textured", line_no, 1) != 0,
                            depth_test=_parse_uint(fields[66], "depth_test", line_no, 1) != 0,
                            sync_epoch=_parse_uint(fields[67], "sync_epoch", line_no, 32),
                            load_sync_packet_id=_parse_uint(
                                fields[68], "load_sync_packet_id", line_no, 64
                            ),
                            pipe_sync_packet_id=_parse_uint(
                                fields[69], "pipe_sync_packet_id", line_no, 64
                            ),
                            tile_sync_packet_id=_parse_uint(
                                fields[70], "tile_sync_packet_id", line_no, 64
                            ),
                            full_sync_packet_id=_parse_uint(
                                fields[71], "full_sync_packet_id", line_no, 64
                            ),
                        )
                    )
                elif len(fields) == 37:
                    current_frame.legacy_semantic_rows = True
                    current_frame.semantics.append(
                        DrawSemanticRecord(
                            source_packet_id=_parse_uint(fields[1], "source_packet_id", line_no, 64),
                            source_opcode=_parse_uint(fields[2], "source_opcode", line_no, 8),
                            draw_type=_parse_uint(fields[3], "draw_type", line_no, 8),
                            tile=_parse_uint(fields[4], "tile", line_no, 8),
                            tex_rect_flip=_parse_uint(fields[5], "tex_rect_flip", line_no, 1) != 0,
                            cycle_type=_parse_uint(fields[6], "cycle_type", line_no, 8),
                            combine_mux=_parse_uint(fields[7], "combine_mux", line_no, 64),
                            blend_mux1=_parse_uint(fields[8], "blend_mux1", line_no, 32),
                            blend_mux2=_parse_uint(fields[9], "blend_mux2", line_no, 32),
                            blend_params=_parse_uint(fields[10], "blend_params", line_no, 32),
                            rect_ulx=_parse_uint(fields[11], "rect_ulx", line_no, 16),
                            rect_uly=_parse_uint(fields[12], "rect_uly", line_no, 16),
                            rect_lrx=_parse_uint(fields[13], "rect_lrx", line_no, 16),
                            rect_lry=_parse_uint(fields[14], "rect_lry", line_no, 16),
                            tex_s=_sign_extend(_parse_uint(fields[15], "tex_s_raw", line_no, 16), 16),
                            tex_t=_sign_extend(_parse_uint(fields[16], "tex_t_raw", line_no, 16), 16),
                            tex_dsdx=_sign_extend(_parse_uint(fields[17], "tex_dsdx_raw", line_no, 16), 16),
                            tex_dtdy=_sign_extend(_parse_uint(fields[18], "tex_dtdy_raw", line_no, 16), 16),
                            triangle_lmajor=_parse_uint(fields[19], "triangle_lmajor", line_no, 1) != 0,
                            triangle_level=_parse_uint(fields[20], "triangle_level", line_no, 8),
                            triangle_yl=_parse_uint(fields[21], "triangle_yl", line_no, 16),
                            triangle_ym=_parse_uint(fields[22], "triangle_ym", line_no, 16),
                            triangle_yh=_parse_uint(fields[23], "triangle_yh", line_no, 16),
                            triangle_xl=_sign_extend(
                                _parse_uint(fields[24], "triangle_xl_raw", line_no, 32), 32
                            ),
                            triangle_xh=_sign_extend(
                                _parse_uint(fields[25], "triangle_xh_raw", line_no, 32), 32
                            ),
                            triangle_xm=_sign_extend(
                                _parse_uint(fields[26], "triangle_xm_raw", line_no, 32), 32
                            ),
                            triangle_dxldy=_sign_extend(
                                _parse_uint(fields[27], "triangle_dxldy_raw", line_no, 32), 32
                            ),
                            triangle_dxhdy=_sign_extend(
                                _parse_uint(fields[28], "triangle_dxhdy_raw", line_no, 32), 32
                            ),
                            triangle_dxmdy=_sign_extend(
                                _parse_uint(fields[29], "triangle_dxmdy_raw", line_no, 32), 32
                            ),
                            triangle_shade_enable=False,
                            triangle_texture_enable=False,
                            triangle_zbuffer_enable=False,
                            triangle_shade_r=0,
                            triangle_shade_g=0,
                            triangle_shade_b=0,
                            triangle_shade_a=0,
                            triangle_shade_drdx=0,
                            triangle_shade_dgdx=0,
                            triangle_shade_dbdx=0,
                            triangle_shade_dadx=0,
                            triangle_shade_drde=0,
                            triangle_shade_dgde=0,
                            triangle_shade_dbde=0,
                            triangle_shade_dade=0,
                            triangle_shade_drdy=0,
                            triangle_shade_dgdy=0,
                            triangle_shade_dbdy=0,
                            triangle_shade_dady=0,
                            triangle_tex_s=0,
                            triangle_tex_t=0,
                            triangle_tex_w=0,
                            triangle_tex_dsdx=0,
                            triangle_tex_dtdx=0,
                            triangle_tex_dwdx=0,
                            triangle_tex_dsde=0,
                            triangle_tex_dtde=0,
                            triangle_tex_dwde=0,
                            triangle_tex_dsdy=0,
                            triangle_tex_dtdy=0,
                            triangle_tex_dwdy=0,
                            triangle_z=0,
                            triangle_dzdx=0,
                            triangle_dzde=0,
                            triangle_dzdy=0,
                            textured=_parse_uint(fields[30], "textured", line_no, 1) != 0,
                            depth_test=_parse_uint(fields[31], "depth_test", line_no, 1) != 0,
                            sync_epoch=_parse_uint(fields[32], "sync_epoch", line_no, 32),
                            load_sync_packet_id=_parse_uint(
                                fields[33], "load_sync_packet_id", line_no, 64
                            ),
                            pipe_sync_packet_id=_parse_uint(
                                fields[34], "pipe_sync_packet_id", line_no, 64
                            ),
                            tile_sync_packet_id=_parse_uint(
                                fields[35], "tile_sync_packet_id", line_no, 64
                            ),
                            full_sync_packet_id=_parse_uint(
                                fields[36], "full_sync_packet_id", line_no, 64
                            ),
                        )
                    )
                elif len(fields) == 26:
                    current_frame.legacy_semantic_rows = True
                    current_frame.semantics.append(
                        DrawSemanticRecord(
                            source_packet_id=_parse_uint(fields[1], "source_packet_id", line_no, 64),
                            source_opcode=_parse_uint(fields[2], "source_opcode", line_no, 8),
                            draw_type=_parse_uint(fields[3], "draw_type", line_no, 8),
                            tile=_parse_uint(fields[4], "tile", line_no, 8),
                            tex_rect_flip=_parse_uint(fields[5], "tex_rect_flip", line_no, 1) != 0,
                            cycle_type=_parse_uint(fields[6], "cycle_type", line_no, 8),
                            combine_mux=_parse_uint(fields[7], "combine_mux", line_no, 64),
                            blend_mux1=_parse_uint(fields[8], "blend_mux1", line_no, 32),
                            blend_mux2=_parse_uint(fields[9], "blend_mux2", line_no, 32),
                            blend_params=_parse_uint(fields[10], "blend_params", line_no, 32),
                            rect_ulx=_parse_uint(fields[11], "rect_ulx", line_no, 16),
                            rect_uly=_parse_uint(fields[12], "rect_uly", line_no, 16),
                            rect_lrx=_parse_uint(fields[13], "rect_lrx", line_no, 16),
                            rect_lry=_parse_uint(fields[14], "rect_lry", line_no, 16),
                            tex_s=_sign_extend(_parse_uint(fields[15], "tex_s_raw", line_no, 16), 16),
                            tex_t=_sign_extend(_parse_uint(fields[16], "tex_t_raw", line_no, 16), 16),
                            tex_dsdx=_sign_extend(_parse_uint(fields[17], "tex_dsdx_raw", line_no, 16), 16),
                            tex_dtdy=_sign_extend(_parse_uint(fields[18], "tex_dtdy_raw", line_no, 16), 16),
                            triangle_lmajor=False,
                            triangle_level=0,
                            triangle_yl=0,
                            triangle_ym=0,
                            triangle_yh=0,
                            triangle_xl=0,
                            triangle_xh=0,
                            triangle_xm=0,
                            triangle_dxldy=0,
                            triangle_dxhdy=0,
                            triangle_dxmdy=0,
                            triangle_shade_enable=False,
                            triangle_texture_enable=False,
                            triangle_zbuffer_enable=False,
                            triangle_shade_r=0,
                            triangle_shade_g=0,
                            triangle_shade_b=0,
                            triangle_shade_a=0,
                            triangle_shade_drdx=0,
                            triangle_shade_dgdx=0,
                            triangle_shade_dbdx=0,
                            triangle_shade_dadx=0,
                            triangle_shade_drde=0,
                            triangle_shade_dgde=0,
                            triangle_shade_dbde=0,
                            triangle_shade_dade=0,
                            triangle_shade_drdy=0,
                            triangle_shade_dgdy=0,
                            triangle_shade_dbdy=0,
                            triangle_shade_dady=0,
                            triangle_tex_s=0,
                            triangle_tex_t=0,
                            triangle_tex_w=0,
                            triangle_tex_dsdx=0,
                            triangle_tex_dtdx=0,
                            triangle_tex_dwdx=0,
                            triangle_tex_dsde=0,
                            triangle_tex_dtde=0,
                            triangle_tex_dwde=0,
                            triangle_tex_dsdy=0,
                            triangle_tex_dtdy=0,
                            triangle_tex_dwdy=0,
                            triangle_z=0,
                            triangle_dzdx=0,
                            triangle_dzde=0,
                            triangle_dzdy=0,
                            textured=_parse_uint(fields[19], "textured", line_no, 1) != 0,
                            depth_test=_parse_uint(fields[20], "depth_test", line_no, 1) != 0,
                            sync_epoch=_parse_uint(fields[21], "sync_epoch", line_no, 32),
                            load_sync_packet_id=_parse_uint(
                                fields[22], "load_sync_packet_id", line_no, 64
                            ),
                            pipe_sync_packet_id=_parse_uint(
                                fields[23], "pipe_sync_packet_id", line_no, 64
                            ),
                            tile_sync_packet_id=_parse_uint(
                                fields[24], "tile_sync_packet_id", line_no, 64
                            ),
                            full_sync_packet_id=_parse_uint(
                                fields[25], "full_sync_packet_id", line_no, 64
                            ),
                        )
                    )
                else:
                    current_frame.legacy_semantic_rows = True
                    current_frame.semantics.append(
                        DrawSemanticRecord(
                            source_packet_id=_parse_uint(fields[1], "source_packet_id", line_no, 64),
                            source_opcode=0,
                            draw_type=0,
                            tile=0,
                            tex_rect_flip=False,
                            cycle_type=_parse_uint(fields[2], "cycle_type", line_no, 8),
                            combine_mux=_parse_uint(fields[3], "combine_mux", line_no, 64),
                            blend_mux1=_parse_uint(fields[4], "blend_mux1", line_no, 32),
                            blend_mux2=_parse_uint(fields[5], "blend_mux2", line_no, 32),
                            blend_params=_parse_uint(fields[6], "blend_params", line_no, 32),
                            rect_ulx=0,
                            rect_uly=0,
                            rect_lrx=0,
                            rect_lry=0,
                            tex_s=0,
                            tex_t=0,
                            tex_dsdx=0,
                            tex_dtdy=0,
                            triangle_lmajor=False,
                            triangle_level=0,
                            triangle_yl=0,
                            triangle_ym=0,
                            triangle_yh=0,
                            triangle_xl=0,
                            triangle_xh=0,
                            triangle_xm=0,
                            triangle_dxldy=0,
                            triangle_dxhdy=0,
                            triangle_dxmdy=0,
                            triangle_shade_enable=False,
                            triangle_texture_enable=False,
                            triangle_zbuffer_enable=False,
                            triangle_shade_r=0,
                            triangle_shade_g=0,
                            triangle_shade_b=0,
                            triangle_shade_a=0,
                            triangle_shade_drdx=0,
                            triangle_shade_dgdx=0,
                            triangle_shade_dbdx=0,
                            triangle_shade_dadx=0,
                            triangle_shade_drde=0,
                            triangle_shade_dgde=0,
                            triangle_shade_dbde=0,
                            triangle_shade_dade=0,
                            triangle_shade_drdy=0,
                            triangle_shade_dgdy=0,
                            triangle_shade_dbdy=0,
                            triangle_shade_dady=0,
                            triangle_tex_s=0,
                            triangle_tex_t=0,
                            triangle_tex_w=0,
                            triangle_tex_dsdx=0,
                            triangle_tex_dtdx=0,
                            triangle_tex_dwdx=0,
                            triangle_tex_dsde=0,
                            triangle_tex_dtde=0,
                            triangle_tex_dwde=0,
                            triangle_tex_dsdy=0,
                            triangle_tex_dtdy=0,
                            triangle_tex_dwdy=0,
                            triangle_z=0,
                            triangle_dzdx=0,
                            triangle_dzde=0,
                            triangle_dzdy=0,
                            textured=_parse_uint(fields[7], "textured", line_no, 1) != 0,
                            depth_test=_parse_uint(fields[8], "depth_test", line_no, 1) != 0,
                            sync_epoch=_parse_uint(fields[9], "sync_epoch", line_no, 32),
                            load_sync_packet_id=_parse_uint(
                                fields[10], "load_sync_packet_id", line_no, 64
                            ),
                            pipe_sync_packet_id=_parse_uint(
                                fields[11], "pipe_sync_packet_id", line_no, 64
                            ),
                            tile_sync_packet_id=_parse_uint(
                                fields[12], "tile_sync_packet_id", line_no, 64
                            ),
                            full_sync_packet_id=_parse_uint(
                                fields[13], "full_sync_packet_id", line_no, 64
                            ),
                        )
                    )
                continue

            if row_type == "R":
                if len(fields) not in (36, 71):
                    raise TraceParseError(
                        f"line {line_no}: raster row expected 36 or 71 columns, got {len(fields)}"
                    )
                if current_frame is None:
                    raise TraceParseError(
                        f"line {line_no}: raster row encountered before first frame row"
                    )
                if len(fields) == 71:
                    current_frame.raster_ops.append(
                        RasterOpRecord(
                            source_packet_id=_parse_uint(fields[1], "source_packet_id", line_no, 64),
                            source_opcode=_parse_uint(fields[2], "source_opcode", line_no, 8),
                            op_kind=_parse_uint(fields[3], "op_kind", line_no, 8),
                            cycle_type=_parse_uint(fields[4], "cycle_type", line_no, 8),
                            tile=_parse_uint(fields[5], "tile", line_no, 8),
                            tex_rect_flip=_parse_uint(fields[6], "tex_rect_flip", line_no, 1) != 0,
                            textured=_parse_uint(fields[7], "textured", line_no, 1) != 0,
                            depth_test=_parse_uint(fields[8], "depth_test", line_no, 1) != 0,
                            rect_ulx=_parse_uint(fields[9], "rect_ulx", line_no, 16),
                            rect_uly=_parse_uint(fields[10], "rect_uly", line_no, 16),
                            rect_lrx=_parse_uint(fields[11], "rect_lrx", line_no, 16),
                            rect_lry=_parse_uint(fields[12], "rect_lry", line_no, 16),
                            tex_s=_sign_extend(_parse_uint(fields[13], "tex_s_raw", line_no, 16), 16),
                            tex_t=_sign_extend(_parse_uint(fields[14], "tex_t_raw", line_no, 16), 16),
                            tex_dsdx=_sign_extend(_parse_uint(fields[15], "tex_dsdx_raw", line_no, 16), 16),
                            tex_dtdy=_sign_extend(_parse_uint(fields[16], "tex_dtdy_raw", line_no, 16), 16),
                            triangle_lmajor=_parse_uint(fields[17], "triangle_lmajor", line_no, 1) != 0,
                            triangle_level=_parse_uint(fields[18], "triangle_level", line_no, 8),
                            triangle_yl=_parse_uint(fields[19], "triangle_yl", line_no, 16),
                            triangle_ym=_parse_uint(fields[20], "triangle_ym", line_no, 16),
                            triangle_yh=_parse_uint(fields[21], "triangle_yh", line_no, 16),
                            triangle_xl=_sign_extend(_parse_uint(fields[22], "triangle_xl_raw", line_no, 32), 32),
                            triangle_xh=_sign_extend(_parse_uint(fields[23], "triangle_xh_raw", line_no, 32), 32),
                            triangle_xm=_sign_extend(_parse_uint(fields[24], "triangle_xm_raw", line_no, 32), 32),
                            triangle_dxldy=_sign_extend(_parse_uint(fields[25], "triangle_dxldy_raw", line_no, 32), 32),
                            triangle_dxhdy=_sign_extend(_parse_uint(fields[26], "triangle_dxhdy_raw", line_no, 32), 32),
                            triangle_dxmdy=_sign_extend(_parse_uint(fields[27], "triangle_dxmdy_raw", line_no, 32), 32),
                            triangle_shade_enable=_parse_uint(fields[28], "triangle_shade_enable", line_no, 1) != 0,
                            triangle_texture_enable=_parse_uint(fields[29], "triangle_texture_enable", line_no, 1) != 0,
                            triangle_zbuffer_enable=_parse_uint(fields[30], "triangle_zbuffer_enable", line_no, 1) != 0,
                            triangle_shade_r=_sign_extend(_parse_uint(fields[31], "triangle_shade_r_raw", line_no, 32), 32),
                            triangle_shade_g=_sign_extend(_parse_uint(fields[32], "triangle_shade_g_raw", line_no, 32), 32),
                            triangle_shade_b=_sign_extend(_parse_uint(fields[33], "triangle_shade_b_raw", line_no, 32), 32),
                            triangle_shade_a=_sign_extend(_parse_uint(fields[34], "triangle_shade_a_raw", line_no, 32), 32),
                            triangle_shade_drdx=_sign_extend(_parse_uint(fields[35], "triangle_shade_drdx_raw", line_no, 32), 32),
                            triangle_shade_dgdx=_sign_extend(_parse_uint(fields[36], "triangle_shade_dgdx_raw", line_no, 32), 32),
                            triangle_shade_dbdx=_sign_extend(_parse_uint(fields[37], "triangle_shade_dbdx_raw", line_no, 32), 32),
                            triangle_shade_dadx=_sign_extend(_parse_uint(fields[38], "triangle_shade_dadx_raw", line_no, 32), 32),
                            triangle_shade_drde=_sign_extend(_parse_uint(fields[39], "triangle_shade_drde_raw", line_no, 32), 32),
                            triangle_shade_dgde=_sign_extend(_parse_uint(fields[40], "triangle_shade_dgde_raw", line_no, 32), 32),
                            triangle_shade_dbde=_sign_extend(_parse_uint(fields[41], "triangle_shade_dbde_raw", line_no, 32), 32),
                            triangle_shade_dade=_sign_extend(_parse_uint(fields[42], "triangle_shade_dade_raw", line_no, 32), 32),
                            triangle_shade_drdy=_sign_extend(_parse_uint(fields[43], "triangle_shade_drdy_raw", line_no, 32), 32),
                            triangle_shade_dgdy=_sign_extend(_parse_uint(fields[44], "triangle_shade_dgdy_raw", line_no, 32), 32),
                            triangle_shade_dbdy=_sign_extend(_parse_uint(fields[45], "triangle_shade_dbdy_raw", line_no, 32), 32),
                            triangle_shade_dady=_sign_extend(_parse_uint(fields[46], "triangle_shade_dady_raw", line_no, 32), 32),
                            triangle_tex_s=_sign_extend(_parse_uint(fields[47], "triangle_tex_s_raw", line_no, 32), 32),
                            triangle_tex_t=_sign_extend(_parse_uint(fields[48], "triangle_tex_t_raw", line_no, 32), 32),
                            triangle_tex_w=_sign_extend(_parse_uint(fields[49], "triangle_tex_w_raw", line_no, 32), 32),
                            triangle_tex_dsdx=_sign_extend(_parse_uint(fields[50], "triangle_tex_dsdx_raw", line_no, 32), 32),
                            triangle_tex_dtdx=_sign_extend(_parse_uint(fields[51], "triangle_tex_dtdx_raw", line_no, 32), 32),
                            triangle_tex_dwdx=_sign_extend(_parse_uint(fields[52], "triangle_tex_dwdx_raw", line_no, 32), 32),
                            triangle_tex_dsde=_sign_extend(_parse_uint(fields[53], "triangle_tex_dsde_raw", line_no, 32), 32),
                            triangle_tex_dtde=_sign_extend(_parse_uint(fields[54], "triangle_tex_dtde_raw", line_no, 32), 32),
                            triangle_tex_dwde=_sign_extend(_parse_uint(fields[55], "triangle_tex_dwde_raw", line_no, 32), 32),
                            triangle_tex_dsdy=_sign_extend(_parse_uint(fields[56], "triangle_tex_dsdy_raw", line_no, 32), 32),
                            triangle_tex_dtdy=_sign_extend(_parse_uint(fields[57], "triangle_tex_dtdy_raw", line_no, 32), 32),
                            triangle_tex_dwdy=_sign_extend(_parse_uint(fields[58], "triangle_tex_dwdy_raw", line_no, 32), 32),
                            triangle_z=_sign_extend(_parse_uint(fields[59], "triangle_z_raw", line_no, 32), 32),
                            triangle_dzdx=_sign_extend(_parse_uint(fields[60], "triangle_dzdx_raw", line_no, 32), 32),
                            triangle_dzde=_sign_extend(_parse_uint(fields[61], "triangle_dzde_raw", line_no, 32), 32),
                            triangle_dzdy=_sign_extend(_parse_uint(fields[62], "triangle_dzdy_raw", line_no, 32), 32),
                            combine_mux=_parse_uint(fields[63], "combine_mux", line_no, 64),
                            blend_params=_parse_uint(fields[64], "blend_params", line_no, 32),
                            fill_color=_parse_uint(fields[65], "fill_color", line_no, 32),
                            sync_epoch=_parse_uint(fields[66], "sync_epoch", line_no, 32),
                            load_sync_packet_id=_parse_uint(fields[67], "load_sync_packet_id", line_no, 64),
                            pipe_sync_packet_id=_parse_uint(fields[68], "pipe_sync_packet_id", line_no, 64),
                            tile_sync_packet_id=_parse_uint(fields[69], "tile_sync_packet_id", line_no, 64),
                            full_sync_packet_id=_parse_uint(fields[70], "full_sync_packet_id", line_no, 64),
                        )
                    )
                else:
                    current_frame.legacy_raster_rows = True
                    current_frame.raster_ops.append(
                        RasterOpRecord(
                            source_packet_id=_parse_uint(fields[1], "source_packet_id", line_no, 64),
                            source_opcode=_parse_uint(fields[2], "source_opcode", line_no, 8),
                            op_kind=_parse_uint(fields[3], "op_kind", line_no, 8),
                            cycle_type=_parse_uint(fields[4], "cycle_type", line_no, 8),
                            tile=_parse_uint(fields[5], "tile", line_no, 8),
                            tex_rect_flip=_parse_uint(fields[6], "tex_rect_flip", line_no, 1) != 0,
                            textured=_parse_uint(fields[7], "textured", line_no, 1) != 0,
                            depth_test=_parse_uint(fields[8], "depth_test", line_no, 1) != 0,
                            rect_ulx=_parse_uint(fields[9], "rect_ulx", line_no, 16),
                            rect_uly=_parse_uint(fields[10], "rect_uly", line_no, 16),
                            rect_lrx=_parse_uint(fields[11], "rect_lrx", line_no, 16),
                            rect_lry=_parse_uint(fields[12], "rect_lry", line_no, 16),
                            tex_s=_sign_extend(_parse_uint(fields[13], "tex_s_raw", line_no, 16), 16),
                            tex_t=_sign_extend(_parse_uint(fields[14], "tex_t_raw", line_no, 16), 16),
                            tex_dsdx=_sign_extend(_parse_uint(fields[15], "tex_dsdx_raw", line_no, 16), 16),
                            tex_dtdy=_sign_extend(_parse_uint(fields[16], "tex_dtdy_raw", line_no, 16), 16),
                            triangle_lmajor=_parse_uint(fields[17], "triangle_lmajor", line_no, 1) != 0,
                            triangle_level=_parse_uint(fields[18], "triangle_level", line_no, 8),
                            triangle_yl=_parse_uint(fields[19], "triangle_yl", line_no, 16),
                            triangle_ym=_parse_uint(fields[20], "triangle_ym", line_no, 16),
                            triangle_yh=_parse_uint(fields[21], "triangle_yh", line_no, 16),
                            triangle_xl=_sign_extend(_parse_uint(fields[22], "triangle_xl_raw", line_no, 32), 32),
                            triangle_xh=_sign_extend(_parse_uint(fields[23], "triangle_xh_raw", line_no, 32), 32),
                            triangle_xm=_sign_extend(_parse_uint(fields[24], "triangle_xm_raw", line_no, 32), 32),
                            triangle_dxldy=_sign_extend(_parse_uint(fields[25], "triangle_dxldy_raw", line_no, 32), 32),
                            triangle_dxhdy=_sign_extend(_parse_uint(fields[26], "triangle_dxhdy_raw", line_no, 32), 32),
                            triangle_dxmdy=_sign_extend(_parse_uint(fields[27], "triangle_dxmdy_raw", line_no, 32), 32),
                            triangle_shade_enable=False,
                            triangle_texture_enable=False,
                            triangle_zbuffer_enable=False,
                            triangle_shade_r=0,
                            triangle_shade_g=0,
                            triangle_shade_b=0,
                            triangle_shade_a=0,
                            triangle_shade_drdx=0,
                            triangle_shade_dgdx=0,
                            triangle_shade_dbdx=0,
                            triangle_shade_dadx=0,
                            triangle_shade_drde=0,
                            triangle_shade_dgde=0,
                            triangle_shade_dbde=0,
                            triangle_shade_dade=0,
                            triangle_shade_drdy=0,
                            triangle_shade_dgdy=0,
                            triangle_shade_dbdy=0,
                            triangle_shade_dady=0,
                            triangle_tex_s=0,
                            triangle_tex_t=0,
                            triangle_tex_w=0,
                            triangle_tex_dsdx=0,
                            triangle_tex_dtdx=0,
                            triangle_tex_dwdx=0,
                            triangle_tex_dsde=0,
                            triangle_tex_dtde=0,
                            triangle_tex_dwde=0,
                            triangle_tex_dsdy=0,
                            triangle_tex_dtdy=0,
                            triangle_tex_dwdy=0,
                            triangle_z=0,
                            triangle_dzdx=0,
                            triangle_dzde=0,
                            triangle_dzdy=0,
                            combine_mux=_parse_uint(fields[28], "combine_mux", line_no, 64),
                            blend_params=_parse_uint(fields[29], "blend_params", line_no, 32),
                            fill_color=_parse_uint(fields[30], "fill_color", line_no, 32),
                            sync_epoch=_parse_uint(fields[31], "sync_epoch", line_no, 32),
                            load_sync_packet_id=_parse_uint(fields[32], "load_sync_packet_id", line_no, 64),
                            pipe_sync_packet_id=_parse_uint(fields[33], "pipe_sync_packet_id", line_no, 64),
                            tile_sync_packet_id=_parse_uint(fields[34], "tile_sync_packet_id", line_no, 64),
                            full_sync_packet_id=_parse_uint(fields[35], "full_sync_packet_id", line_no, 64),
                        )
                    )
                continue

            if row_type == "W":
                if len(fields) not in (55, 63, 74, 109):
                    raise TraceParseError(
                        f"line {line_no}: render-work row expected 55, 63, 74, or 109 columns, got {len(fields)}"
                    )
                if current_frame is None:
                    raise TraceParseError(
                        f"line {line_no}: render-work row encountered before first frame row"
                    )
                if len(fields) == 109:
                    current_frame.legacy_render_work_rows = True
                    current_frame.render_work.append(
                        RenderWorkRecord(
                            source_packet_id=_parse_uint(fields[1], "source_packet_id", line_no, 64),
                            source_opcode=_parse_uint(fields[2], "source_opcode", line_no, 8),
                            op_kind=_parse_uint(fields[3], "op_kind", line_no, 8),
                            phase=_parse_uint(fields[4], "phase", line_no, 8),
                            cycle_type=_parse_uint(fields[5], "cycle_type", line_no, 8),
                            barrier_mask=_parse_uint(fields[6], "barrier_mask", line_no, 8),
                            tile=_parse_uint(fields[7], "tile", line_no, 8),
                            tex_rect_flip=_parse_uint(fields[8], "tex_rect_flip", line_no, 1) != 0,
                            textured=_parse_uint(fields[9], "textured", line_no, 1) != 0,
                            depth_test=_parse_uint(fields[10], "depth_test", line_no, 1) != 0,
                            rect_ulx=_parse_uint(fields[11], "rect_ulx", line_no, 16),
                            rect_uly=_parse_uint(fields[12], "rect_uly", line_no, 16),
                            rect_lrx=_parse_uint(fields[13], "rect_lrx", line_no, 16),
                            rect_lry=_parse_uint(fields[14], "rect_lry", line_no, 16),
                            tex_s=_sign_extend(_parse_uint(fields[15], "tex_s_raw", line_no, 16), 16),
                            tex_t=_sign_extend(_parse_uint(fields[16], "tex_t_raw", line_no, 16), 16),
                            tex_dsdx=_sign_extend(_parse_uint(fields[17], "tex_dsdx_raw", line_no, 16), 16),
                            tex_dtdy=_sign_extend(_parse_uint(fields[18], "tex_dtdy_raw", line_no, 16), 16),
                            triangle_lmajor=_parse_uint(fields[19], "triangle_lmajor", line_no, 1) != 0,
                            triangle_level=_parse_uint(fields[20], "triangle_level", line_no, 8),
                            triangle_yl=_parse_uint(fields[21], "triangle_yl", line_no, 16),
                            triangle_ym=_parse_uint(fields[22], "triangle_ym", line_no, 16),
                            triangle_yh=_parse_uint(fields[23], "triangle_yh", line_no, 16),
                            triangle_xl=_sign_extend(_parse_uint(fields[24], "triangle_xl_raw", line_no, 32), 32),
                            triangle_xh=_sign_extend(_parse_uint(fields[25], "triangle_xh_raw", line_no, 32), 32),
                            triangle_xm=_sign_extend(_parse_uint(fields[26], "triangle_xm_raw", line_no, 32), 32),
                            triangle_dxldy=_sign_extend(_parse_uint(fields[27], "triangle_dxldy_raw", line_no, 32), 32),
                            triangle_dxhdy=_sign_extend(_parse_uint(fields[28], "triangle_dxhdy_raw", line_no, 32), 32),
                            triangle_dxmdy=_sign_extend(_parse_uint(fields[29], "triangle_dxmdy_raw", line_no, 32), 32),
                            triangle_shade_enable=_parse_uint(fields[30], "triangle_shade_enable", line_no, 1) != 0,
                            triangle_texture_enable=_parse_uint(fields[31], "triangle_texture_enable", line_no, 1) != 0,
                            triangle_zbuffer_enable=_parse_uint(fields[32], "triangle_zbuffer_enable", line_no, 1) != 0,
                            triangle_shade_r=_sign_extend(_parse_uint(fields[33], "triangle_shade_r_raw", line_no, 32), 32),
                            triangle_shade_g=_sign_extend(_parse_uint(fields[34], "triangle_shade_g_raw", line_no, 32), 32),
                            triangle_shade_b=_sign_extend(_parse_uint(fields[35], "triangle_shade_b_raw", line_no, 32), 32),
                            triangle_shade_a=_sign_extend(_parse_uint(fields[36], "triangle_shade_a_raw", line_no, 32), 32),
                            triangle_shade_drdx=_sign_extend(_parse_uint(fields[37], "triangle_shade_drdx_raw", line_no, 32), 32),
                            triangle_shade_dgdx=_sign_extend(_parse_uint(fields[38], "triangle_shade_dgdx_raw", line_no, 32), 32),
                            triangle_shade_dbdx=_sign_extend(_parse_uint(fields[39], "triangle_shade_dbdx_raw", line_no, 32), 32),
                            triangle_shade_dadx=_sign_extend(_parse_uint(fields[40], "triangle_shade_dadx_raw", line_no, 32), 32),
                            triangle_shade_drde=_sign_extend(_parse_uint(fields[41], "triangle_shade_drde_raw", line_no, 32), 32),
                            triangle_shade_dgde=_sign_extend(_parse_uint(fields[42], "triangle_shade_dgde_raw", line_no, 32), 32),
                            triangle_shade_dbde=_sign_extend(_parse_uint(fields[43], "triangle_shade_dbde_raw", line_no, 32), 32),
                            triangle_shade_dade=_sign_extend(_parse_uint(fields[44], "triangle_shade_dade_raw", line_no, 32), 32),
                            triangle_shade_drdy=_sign_extend(_parse_uint(fields[45], "triangle_shade_drdy_raw", line_no, 32), 32),
                            triangle_shade_dgdy=_sign_extend(_parse_uint(fields[46], "triangle_shade_dgdy_raw", line_no, 32), 32),
                            triangle_shade_dbdy=_sign_extend(_parse_uint(fields[47], "triangle_shade_dbdy_raw", line_no, 32), 32),
                            triangle_shade_dady=_sign_extend(_parse_uint(fields[48], "triangle_shade_dady_raw", line_no, 32), 32),
                            triangle_tex_s=_sign_extend(_parse_uint(fields[49], "triangle_tex_s_raw", line_no, 32), 32),
                            triangle_tex_t=_sign_extend(_parse_uint(fields[50], "triangle_tex_t_raw", line_no, 32), 32),
                            triangle_tex_w=_sign_extend(_parse_uint(fields[51], "triangle_tex_w_raw", line_no, 32), 32),
                            triangle_tex_dsdx=_sign_extend(_parse_uint(fields[52], "triangle_tex_dsdx_raw", line_no, 32), 32),
                            triangle_tex_dtdx=_sign_extend(_parse_uint(fields[53], "triangle_tex_dtdx_raw", line_no, 32), 32),
                            triangle_tex_dwdx=_sign_extend(_parse_uint(fields[54], "triangle_tex_dwdx_raw", line_no, 32), 32),
                            triangle_tex_dsde=_sign_extend(_parse_uint(fields[55], "triangle_tex_dsde_raw", line_no, 32), 32),
                            triangle_tex_dtde=_sign_extend(_parse_uint(fields[56], "triangle_tex_dtde_raw", line_no, 32), 32),
                            triangle_tex_dwde=_sign_extend(_parse_uint(fields[57], "triangle_tex_dwde_raw", line_no, 32), 32),
                            triangle_tex_dsdy=_sign_extend(_parse_uint(fields[58], "triangle_tex_dsdy_raw", line_no, 32), 32),
                            triangle_tex_dtdy=_sign_extend(_parse_uint(fields[59], "triangle_tex_dtdy_raw", line_no, 32), 32),
                            triangle_tex_dwdy=_sign_extend(_parse_uint(fields[60], "triangle_tex_dwdy_raw", line_no, 32), 32),
                            triangle_z=_sign_extend(_parse_uint(fields[61], "triangle_z_raw", line_no, 32), 32),
                            triangle_dzdx=_sign_extend(_parse_uint(fields[62], "triangle_dzdx_raw", line_no, 32), 32),
                            triangle_dzde=_sign_extend(_parse_uint(fields[63], "triangle_dzde_raw", line_no, 32), 32),
                            triangle_dzdy=_sign_extend(_parse_uint(fields[64], "triangle_dzdy_raw", line_no, 32), 32),
                            color_image_format=_parse_uint(fields[65], "color_image_format", line_no, 8),
                            color_image_size=_parse_uint(fields[66], "color_image_size", line_no, 8),
                            color_image_width=_parse_uint(fields[67], "color_image_width", line_no, 16),
                            color_image_address=_parse_uint(fields[68], "color_image_address", line_no, 32),
                            depth_image_address=_parse_uint(fields[69], "depth_image_address", line_no, 32),
                            scissor_mode=_parse_uint(fields[70], "scissor_mode", line_no, 8),
                            scissor_xh=_parse_uint(fields[71], "scissor_xh", line_no, 16),
                            scissor_yh=_parse_uint(fields[72], "scissor_yh", line_no, 16),
                            scissor_xl=_parse_uint(fields[73], "scissor_xl", line_no, 16),
                            scissor_yl=_parse_uint(fields[74], "scissor_yl", line_no, 16),
                            texture_image_format=_parse_uint(fields[75], "texture_image_format", line_no, 8),
                            texture_image_size=_parse_uint(fields[76], "texture_image_size", line_no, 8),
                            texture_image_width=_parse_uint(fields[77], "texture_image_width", line_no, 16),
                            texture_image_address=_parse_uint(fields[78], "texture_image_address", line_no, 32),
                            tile_format=_parse_uint(fields[79], "tile_format", line_no, 8),
                            tile_size=_parse_uint(fields[80], "tile_size", line_no, 8),
                            tile_line=_parse_uint(fields[81], "tile_line", line_no, 16),
                            tile_tmem=_parse_uint(fields[82], "tile_tmem", line_no, 16),
                            tile_palette=_parse_uint(fields[83], "tile_palette", line_no, 8),
                            tile_cmt=_parse_uint(fields[84], "tile_cmt", line_no, 8),
                            tile_cms=_parse_uint(fields[85], "tile_cms", line_no, 8),
                            tile_maskt=_parse_uint(fields[86], "tile_maskt", line_no, 8),
                            tile_masks=_parse_uint(fields[87], "tile_masks", line_no, 8),
                            tile_shiftt=_parse_uint(fields[88], "tile_shiftt", line_no, 8),
                            tile_shifts=_parse_uint(fields[89], "tile_shifts", line_no, 8),
                            tile_uls=_parse_uint(fields[90], "tile_uls", line_no, 16),
                            tile_ult=_parse_uint(fields[91], "tile_ult", line_no, 16),
                            tile_lrs=_parse_uint(fields[92], "tile_lrs", line_no, 16),
                            tile_lrt=_parse_uint(fields[93], "tile_lrt", line_no, 16),
                            tmem_load_kind=_parse_uint(fields[94], "tmem_load_kind", line_no, 8),
                            tmem_load_tile=_parse_uint(fields[95], "tmem_load_tile", line_no, 8),
                            tmem_load_uls=_parse_uint(fields[96], "tmem_load_uls", line_no, 16),
                            tmem_load_ult=_parse_uint(fields[97], "tmem_load_ult", line_no, 16),
                            tmem_load_lrs=_parse_uint(fields[98], "tmem_load_lrs", line_no, 16),
                            tmem_load_lrt=_parse_uint(fields[99], "tmem_load_lrt", line_no, 16),
                            tmem_load_dxt=_parse_uint(fields[100], "tmem_load_dxt", line_no, 16),
                            combine_mux=_parse_uint(fields[101], "combine_mux", line_no, 64),
                            blend_params=_parse_uint(fields[102], "blend_params", line_no, 32),
                            fill_color=_parse_uint(fields[103], "fill_color", line_no, 32),
                            sync_epoch=_parse_uint(fields[104], "sync_epoch", line_no, 32),
                            load_sync_packet_id=_parse_uint(fields[105], "load_sync_packet_id", line_no, 64),
                            pipe_sync_packet_id=_parse_uint(fields[106], "pipe_sync_packet_id", line_no, 64),
                            tile_sync_packet_id=_parse_uint(fields[107], "tile_sync_packet_id", line_no, 64),
                            full_sync_packet_id=_parse_uint(fields[108], "full_sync_packet_id", line_no, 64),
                        )
                    )
                elif len(fields) == 74:
                    current_frame.legacy_render_work_rows = True
                    current_frame.render_work.append(
                        RenderWorkRecord(
                            source_packet_id=_parse_uint(fields[1], "source_packet_id", line_no, 64),
                            source_opcode=_parse_uint(fields[2], "source_opcode", line_no, 8),
                            op_kind=_parse_uint(fields[3], "op_kind", line_no, 8),
                            phase=_parse_uint(fields[4], "phase", line_no, 8),
                            cycle_type=_parse_uint(fields[5], "cycle_type", line_no, 8),
                            barrier_mask=_parse_uint(fields[6], "barrier_mask", line_no, 8),
                            tile=_parse_uint(fields[7], "tile", line_no, 8),
                            tex_rect_flip=_parse_uint(fields[8], "tex_rect_flip", line_no, 1) != 0,
                            textured=_parse_uint(fields[9], "textured", line_no, 1) != 0,
                            depth_test=_parse_uint(fields[10], "depth_test", line_no, 1) != 0,
                            rect_ulx=_parse_uint(fields[11], "rect_ulx", line_no, 16),
                            rect_uly=_parse_uint(fields[12], "rect_uly", line_no, 16),
                            rect_lrx=_parse_uint(fields[13], "rect_lrx", line_no, 16),
                            rect_lry=_parse_uint(fields[14], "rect_lry", line_no, 16),
                            tex_s=_sign_extend(_parse_uint(fields[15], "tex_s_raw", line_no, 16), 16),
                            tex_t=_sign_extend(_parse_uint(fields[16], "tex_t_raw", line_no, 16), 16),
                            tex_dsdx=_sign_extend(_parse_uint(fields[17], "tex_dsdx_raw", line_no, 16), 16),
                            tex_dtdy=_sign_extend(_parse_uint(fields[18], "tex_dtdy_raw", line_no, 16), 16),
                            triangle_lmajor=_parse_uint(fields[19], "triangle_lmajor", line_no, 1) != 0,
                            triangle_level=_parse_uint(fields[20], "triangle_level", line_no, 8),
                            triangle_yl=_parse_uint(fields[21], "triangle_yl", line_no, 16),
                            triangle_ym=_parse_uint(fields[22], "triangle_ym", line_no, 16),
                            triangle_yh=_parse_uint(fields[23], "triangle_yh", line_no, 16),
                            triangle_xl=_sign_extend(_parse_uint(fields[24], "triangle_xl_raw", line_no, 32), 32),
                            triangle_xh=_sign_extend(_parse_uint(fields[25], "triangle_xh_raw", line_no, 32), 32),
                            triangle_xm=_sign_extend(_parse_uint(fields[26], "triangle_xm_raw", line_no, 32), 32),
                            triangle_dxldy=_sign_extend(_parse_uint(fields[27], "triangle_dxldy_raw", line_no, 32), 32),
                            triangle_dxhdy=_sign_extend(_parse_uint(fields[28], "triangle_dxhdy_raw", line_no, 32), 32),
                            triangle_dxmdy=_sign_extend(_parse_uint(fields[29], "triangle_dxmdy_raw", line_no, 32), 32),
                            triangle_shade_enable=False,
                            triangle_texture_enable=False,
                            triangle_zbuffer_enable=False,
                            triangle_shade_r=0,
                            triangle_shade_g=0,
                            triangle_shade_b=0,
                            triangle_shade_a=0,
                            triangle_shade_drdx=0,
                            triangle_shade_dgdx=0,
                            triangle_shade_dbdx=0,
                            triangle_shade_dadx=0,
                            triangle_shade_drde=0,
                            triangle_shade_dgde=0,
                            triangle_shade_dbde=0,
                            triangle_shade_dade=0,
                            triangle_shade_drdy=0,
                            triangle_shade_dgdy=0,
                            triangle_shade_dbdy=0,
                            triangle_shade_dady=0,
                            triangle_tex_s=0,
                            triangle_tex_t=0,
                            triangle_tex_w=0,
                            triangle_tex_dsdx=0,
                            triangle_tex_dtdx=0,
                            triangle_tex_dwdx=0,
                            triangle_tex_dsde=0,
                            triangle_tex_dtde=0,
                            triangle_tex_dwde=0,
                            triangle_tex_dsdy=0,
                            triangle_tex_dtdy=0,
                            triangle_tex_dwdy=0,
                            triangle_z=0,
                            triangle_dzdx=0,
                            triangle_dzde=0,
                            triangle_dzdy=0,
                            color_image_format=_parse_uint(fields[30], "color_image_format", line_no, 8),
                            color_image_size=_parse_uint(fields[31], "color_image_size", line_no, 8),
                            color_image_width=_parse_uint(fields[32], "color_image_width", line_no, 16),
                            color_image_address=_parse_uint(fields[33], "color_image_address", line_no, 32),
                            depth_image_address=_parse_uint(fields[34], "depth_image_address", line_no, 32),
                            scissor_mode=_parse_uint(fields[35], "scissor_mode", line_no, 8),
                            scissor_xh=_parse_uint(fields[36], "scissor_xh", line_no, 16),
                            scissor_yh=_parse_uint(fields[37], "scissor_yh", line_no, 16),
                            scissor_xl=_parse_uint(fields[38], "scissor_xl", line_no, 16),
                            scissor_yl=_parse_uint(fields[39], "scissor_yl", line_no, 16),
                            texture_image_format=_parse_uint(fields[40], "texture_image_format", line_no, 8),
                            texture_image_size=_parse_uint(fields[41], "texture_image_size", line_no, 8),
                            texture_image_width=_parse_uint(fields[42], "texture_image_width", line_no, 16),
                            texture_image_address=_parse_uint(fields[43], "texture_image_address", line_no, 32),
                            tile_format=_parse_uint(fields[44], "tile_format", line_no, 8),
                            tile_size=_parse_uint(fields[45], "tile_size", line_no, 8),
                            tile_line=_parse_uint(fields[46], "tile_line", line_no, 16),
                            tile_tmem=_parse_uint(fields[47], "tile_tmem", line_no, 16),
                            tile_palette=_parse_uint(fields[48], "tile_palette", line_no, 8),
                            tile_cmt=_parse_uint(fields[49], "tile_cmt", line_no, 8),
                            tile_cms=_parse_uint(fields[50], "tile_cms", line_no, 8),
                            tile_maskt=_parse_uint(fields[51], "tile_maskt", line_no, 8),
                            tile_masks=_parse_uint(fields[52], "tile_masks", line_no, 8),
                            tile_shiftt=_parse_uint(fields[53], "tile_shiftt", line_no, 8),
                            tile_shifts=_parse_uint(fields[54], "tile_shifts", line_no, 8),
                            tile_uls=_parse_uint(fields[55], "tile_uls", line_no, 16),
                            tile_ult=_parse_uint(fields[56], "tile_ult", line_no, 16),
                            tile_lrs=_parse_uint(fields[57], "tile_lrs", line_no, 16),
                            tile_lrt=_parse_uint(fields[58], "tile_lrt", line_no, 16),
                            tmem_load_kind=_parse_uint(fields[59], "tmem_load_kind", line_no, 8),
                            tmem_load_tile=_parse_uint(fields[60], "tmem_load_tile", line_no, 8),
                            tmem_load_uls=_parse_uint(fields[61], "tmem_load_uls", line_no, 16),
                            tmem_load_ult=_parse_uint(fields[62], "tmem_load_ult", line_no, 16),
                            tmem_load_lrs=_parse_uint(fields[63], "tmem_load_lrs", line_no, 16),
                            tmem_load_lrt=_parse_uint(fields[64], "tmem_load_lrt", line_no, 16),
                            tmem_load_dxt=_parse_uint(fields[65], "tmem_load_dxt", line_no, 16),
                            combine_mux=_parse_uint(fields[66], "combine_mux", line_no, 64),
                            blend_params=_parse_uint(fields[67], "blend_params", line_no, 32),
                            fill_color=_parse_uint(fields[68], "fill_color", line_no, 32),
                            sync_epoch=_parse_uint(fields[69], "sync_epoch", line_no, 32),
                            load_sync_packet_id=_parse_uint(fields[70], "load_sync_packet_id", line_no, 64),
                            pipe_sync_packet_id=_parse_uint(fields[71], "pipe_sync_packet_id", line_no, 64),
                            tile_sync_packet_id=_parse_uint(fields[72], "tile_sync_packet_id", line_no, 64),
                            full_sync_packet_id=_parse_uint(fields[73], "full_sync_packet_id", line_no, 64),
                        )
                    )
                elif len(fields) == 63:
                    current_frame.legacy_render_work_rows = True
                    current_frame.render_work.append(
                        RenderWorkRecord(
                            source_packet_id=_parse_uint(fields[1], "source_packet_id", line_no, 64),
                            source_opcode=_parse_uint(fields[2], "source_opcode", line_no, 8),
                            op_kind=_parse_uint(fields[3], "op_kind", line_no, 8),
                            phase=_parse_uint(fields[4], "phase", line_no, 8),
                            cycle_type=_parse_uint(fields[5], "cycle_type", line_no, 8),
                            barrier_mask=_parse_uint(fields[6], "barrier_mask", line_no, 8),
                            tile=_parse_uint(fields[7], "tile", line_no, 8),
                            tex_rect_flip=_parse_uint(fields[8], "tex_rect_flip", line_no, 1) != 0,
                            textured=_parse_uint(fields[9], "textured", line_no, 1) != 0,
                            depth_test=_parse_uint(fields[10], "depth_test", line_no, 1) != 0,
                            rect_ulx=_parse_uint(fields[11], "rect_ulx", line_no, 16),
                            rect_uly=_parse_uint(fields[12], "rect_uly", line_no, 16),
                            rect_lrx=_parse_uint(fields[13], "rect_lrx", line_no, 16),
                            rect_lry=_parse_uint(fields[14], "rect_lry", line_no, 16),
                            tex_s=_sign_extend(_parse_uint(fields[15], "tex_s_raw", line_no, 16), 16),
                            tex_t=_sign_extend(_parse_uint(fields[16], "tex_t_raw", line_no, 16), 16),
                            tex_dsdx=_sign_extend(_parse_uint(fields[17], "tex_dsdx_raw", line_no, 16), 16),
                            tex_dtdy=_sign_extend(_parse_uint(fields[18], "tex_dtdy_raw", line_no, 16), 16),
                            triangle_lmajor=False,
                            triangle_level=0,
                            triangle_yl=0,
                            triangle_ym=0,
                            triangle_yh=0,
                            triangle_xl=0,
                            triangle_xh=0,
                            triangle_xm=0,
                            triangle_dxldy=0,
                            triangle_dxhdy=0,
                            triangle_dxmdy=0,
                            triangle_shade_enable=False,
                            triangle_texture_enable=False,
                            triangle_zbuffer_enable=False,
                            triangle_shade_r=0,
                            triangle_shade_g=0,
                            triangle_shade_b=0,
                            triangle_shade_a=0,
                            triangle_shade_drdx=0,
                            triangle_shade_dgdx=0,
                            triangle_shade_dbdx=0,
                            triangle_shade_dadx=0,
                            triangle_shade_drde=0,
                            triangle_shade_dgde=0,
                            triangle_shade_dbde=0,
                            triangle_shade_dade=0,
                            triangle_shade_drdy=0,
                            triangle_shade_dgdy=0,
                            triangle_shade_dbdy=0,
                            triangle_shade_dady=0,
                            triangle_tex_s=0,
                            triangle_tex_t=0,
                            triangle_tex_w=0,
                            triangle_tex_dsdx=0,
                            triangle_tex_dtdx=0,
                            triangle_tex_dwdx=0,
                            triangle_tex_dsde=0,
                            triangle_tex_dtde=0,
                            triangle_tex_dwde=0,
                            triangle_tex_dsdy=0,
                            triangle_tex_dtdy=0,
                            triangle_tex_dwdy=0,
                            triangle_z=0,
                            triangle_dzdx=0,
                            triangle_dzde=0,
                            triangle_dzdy=0,
                            color_image_format=_parse_uint(fields[19], "color_image_format", line_no, 8),
                            color_image_size=_parse_uint(fields[20], "color_image_size", line_no, 8),
                            color_image_width=_parse_uint(fields[21], "color_image_width", line_no, 16),
                            color_image_address=_parse_uint(fields[22], "color_image_address", line_no, 32),
                            depth_image_address=_parse_uint(fields[23], "depth_image_address", line_no, 32),
                            scissor_mode=_parse_uint(fields[24], "scissor_mode", line_no, 8),
                            scissor_xh=_parse_uint(fields[25], "scissor_xh", line_no, 16),
                            scissor_yh=_parse_uint(fields[26], "scissor_yh", line_no, 16),
                            scissor_xl=_parse_uint(fields[27], "scissor_xl", line_no, 16),
                            scissor_yl=_parse_uint(fields[28], "scissor_yl", line_no, 16),
                            texture_image_format=_parse_uint(fields[29], "texture_image_format", line_no, 8),
                            texture_image_size=_parse_uint(fields[30], "texture_image_size", line_no, 8),
                            texture_image_width=_parse_uint(fields[31], "texture_image_width", line_no, 16),
                            texture_image_address=_parse_uint(fields[32], "texture_image_address", line_no, 32),
                            tile_format=_parse_uint(fields[33], "tile_format", line_no, 8),
                            tile_size=_parse_uint(fields[34], "tile_size", line_no, 8),
                            tile_line=_parse_uint(fields[35], "tile_line", line_no, 16),
                            tile_tmem=_parse_uint(fields[36], "tile_tmem", line_no, 16),
                            tile_palette=_parse_uint(fields[37], "tile_palette", line_no, 8),
                            tile_cmt=_parse_uint(fields[38], "tile_cmt", line_no, 8),
                            tile_cms=_parse_uint(fields[39], "tile_cms", line_no, 8),
                            tile_maskt=_parse_uint(fields[40], "tile_maskt", line_no, 8),
                            tile_masks=_parse_uint(fields[41], "tile_masks", line_no, 8),
                            tile_shiftt=_parse_uint(fields[42], "tile_shiftt", line_no, 8),
                            tile_shifts=_parse_uint(fields[43], "tile_shifts", line_no, 8),
                            tile_uls=_parse_uint(fields[44], "tile_uls", line_no, 16),
                            tile_ult=_parse_uint(fields[45], "tile_ult", line_no, 16),
                            tile_lrs=_parse_uint(fields[46], "tile_lrs", line_no, 16),
                            tile_lrt=_parse_uint(fields[47], "tile_lrt", line_no, 16),
                            tmem_load_kind=_parse_uint(fields[48], "tmem_load_kind", line_no, 8),
                            tmem_load_tile=_parse_uint(fields[49], "tmem_load_tile", line_no, 8),
                            tmem_load_uls=_parse_uint(fields[50], "tmem_load_uls", line_no, 16),
                            tmem_load_ult=_parse_uint(fields[51], "tmem_load_ult", line_no, 16),
                            tmem_load_lrs=_parse_uint(fields[52], "tmem_load_lrs", line_no, 16),
                            tmem_load_lrt=_parse_uint(fields[53], "tmem_load_lrt", line_no, 16),
                            tmem_load_dxt=_parse_uint(fields[54], "tmem_load_dxt", line_no, 16),
                            combine_mux=_parse_uint(fields[55], "combine_mux", line_no, 64),
                            blend_params=_parse_uint(fields[56], "blend_params", line_no, 32),
                            fill_color=_parse_uint(fields[57], "fill_color", line_no, 32),
                            sync_epoch=_parse_uint(fields[58], "sync_epoch", line_no, 32),
                            load_sync_packet_id=_parse_uint(fields[59], "load_sync_packet_id", line_no, 64),
                            pipe_sync_packet_id=_parse_uint(fields[60], "pipe_sync_packet_id", line_no, 64),
                            tile_sync_packet_id=_parse_uint(fields[61], "tile_sync_packet_id", line_no, 64),
                            full_sync_packet_id=_parse_uint(fields[62], "full_sync_packet_id", line_no, 64),
                        )
                    )
                else:
                    current_frame.legacy_render_work_rows = True
                    current_frame.render_work.append(
                        RenderWorkRecord(
                            source_packet_id=_parse_uint(fields[1], "source_packet_id", line_no, 64),
                            source_opcode=_parse_uint(fields[2], "source_opcode", line_no, 8),
                            op_kind=_parse_uint(fields[3], "op_kind", line_no, 8),
                            phase=_parse_uint(fields[4], "phase", line_no, 8),
                            cycle_type=_parse_uint(fields[5], "cycle_type", line_no, 8),
                            barrier_mask=_parse_uint(fields[6], "barrier_mask", line_no, 8),
                            tile=_parse_uint(fields[7], "tile", line_no, 8),
                            tex_rect_flip=_parse_uint(fields[8], "tex_rect_flip", line_no, 1) != 0,
                            textured=_parse_uint(fields[9], "textured", line_no, 1) != 0,
                            depth_test=_parse_uint(fields[10], "depth_test", line_no, 1) != 0,
                            rect_ulx=0,
                            rect_uly=0,
                            rect_lrx=0,
                            rect_lry=0,
                            tex_s=0,
                            tex_t=0,
                            tex_dsdx=0,
                            tex_dtdy=0,
                            triangle_lmajor=False,
                            triangle_level=0,
                            triangle_yl=0,
                            triangle_ym=0,
                            triangle_yh=0,
                            triangle_xl=0,
                            triangle_xh=0,
                            triangle_xm=0,
                            triangle_dxldy=0,
                            triangle_dxhdy=0,
                            triangle_dxmdy=0,
                            triangle_shade_enable=False,
                            triangle_texture_enable=False,
                            triangle_zbuffer_enable=False,
                            triangle_shade_r=0,
                            triangle_shade_g=0,
                            triangle_shade_b=0,
                            triangle_shade_a=0,
                            triangle_shade_drdx=0,
                            triangle_shade_dgdx=0,
                            triangle_shade_dbdx=0,
                            triangle_shade_dadx=0,
                            triangle_shade_drde=0,
                            triangle_shade_dgde=0,
                            triangle_shade_dbde=0,
                            triangle_shade_dade=0,
                            triangle_shade_drdy=0,
                            triangle_shade_dgdy=0,
                            triangle_shade_dbdy=0,
                            triangle_shade_dady=0,
                            triangle_tex_s=0,
                            triangle_tex_t=0,
                            triangle_tex_w=0,
                            triangle_tex_dsdx=0,
                            triangle_tex_dtdx=0,
                            triangle_tex_dwdx=0,
                            triangle_tex_dsde=0,
                            triangle_tex_dtde=0,
                            triangle_tex_dwde=0,
                            triangle_tex_dsdy=0,
                            triangle_tex_dtdy=0,
                            triangle_tex_dwdy=0,
                            triangle_z=0,
                            triangle_dzdx=0,
                            triangle_dzde=0,
                            triangle_dzdy=0,
                            color_image_format=_parse_uint(fields[11], "color_image_format", line_no, 8),
                            color_image_size=_parse_uint(fields[12], "color_image_size", line_no, 8),
                            color_image_width=_parse_uint(fields[13], "color_image_width", line_no, 16),
                            color_image_address=_parse_uint(fields[14], "color_image_address", line_no, 32),
                            depth_image_address=_parse_uint(fields[15], "depth_image_address", line_no, 32),
                            scissor_mode=_parse_uint(fields[16], "scissor_mode", line_no, 8),
                            scissor_xh=_parse_uint(fields[17], "scissor_xh", line_no, 16),
                            scissor_yh=_parse_uint(fields[18], "scissor_yh", line_no, 16),
                            scissor_xl=_parse_uint(fields[19], "scissor_xl", line_no, 16),
                            scissor_yl=_parse_uint(fields[20], "scissor_yl", line_no, 16),
                            texture_image_format=_parse_uint(fields[21], "texture_image_format", line_no, 8),
                            texture_image_size=_parse_uint(fields[22], "texture_image_size", line_no, 8),
                            texture_image_width=_parse_uint(fields[23], "texture_image_width", line_no, 16),
                            texture_image_address=_parse_uint(fields[24], "texture_image_address", line_no, 32),
                            tile_format=_parse_uint(fields[25], "tile_format", line_no, 8),
                            tile_size=_parse_uint(fields[26], "tile_size", line_no, 8),
                            tile_line=_parse_uint(fields[27], "tile_line", line_no, 16),
                            tile_tmem=_parse_uint(fields[28], "tile_tmem", line_no, 16),
                            tile_palette=_parse_uint(fields[29], "tile_palette", line_no, 8),
                            tile_cmt=_parse_uint(fields[30], "tile_cmt", line_no, 8),
                            tile_cms=_parse_uint(fields[31], "tile_cms", line_no, 8),
                            tile_maskt=_parse_uint(fields[32], "tile_maskt", line_no, 8),
                            tile_masks=_parse_uint(fields[33], "tile_masks", line_no, 8),
                            tile_shiftt=_parse_uint(fields[34], "tile_shiftt", line_no, 8),
                            tile_shifts=_parse_uint(fields[35], "tile_shifts", line_no, 8),
                            tile_uls=_parse_uint(fields[36], "tile_uls", line_no, 16),
                            tile_ult=_parse_uint(fields[37], "tile_ult", line_no, 16),
                            tile_lrs=_parse_uint(fields[38], "tile_lrs", line_no, 16),
                            tile_lrt=_parse_uint(fields[39], "tile_lrt", line_no, 16),
                            tmem_load_kind=_parse_uint(fields[40], "tmem_load_kind", line_no, 8),
                            tmem_load_tile=_parse_uint(fields[41], "tmem_load_tile", line_no, 8),
                            tmem_load_uls=_parse_uint(fields[42], "tmem_load_uls", line_no, 16),
                            tmem_load_ult=_parse_uint(fields[43], "tmem_load_ult", line_no, 16),
                            tmem_load_lrs=_parse_uint(fields[44], "tmem_load_lrs", line_no, 16),
                            tmem_load_lrt=_parse_uint(fields[45], "tmem_load_lrt", line_no, 16),
                            tmem_load_dxt=_parse_uint(fields[46], "tmem_load_dxt", line_no, 16),
                            combine_mux=_parse_uint(fields[47], "combine_mux", line_no, 64),
                            blend_params=_parse_uint(fields[48], "blend_params", line_no, 32),
                            fill_color=_parse_uint(fields[49], "fill_color", line_no, 32),
                            sync_epoch=_parse_uint(fields[50], "sync_epoch", line_no, 32),
                            load_sync_packet_id=_parse_uint(fields[51], "load_sync_packet_id", line_no, 64),
                            pipe_sync_packet_id=_parse_uint(fields[52], "pipe_sync_packet_id", line_no, 64),
                            tile_sync_packet_id=_parse_uint(fields[53], "tile_sync_packet_id", line_no, 64),
                            full_sync_packet_id=_parse_uint(fields[54], "full_sync_packet_id", line_no, 64),
                        )
                    )
                continue

            if row_type == "B":
                if len(fields) != 24:
                    raise TraceParseError(
                        f"line {line_no}: submission-batch row expected 24 columns, got {len(fields)}"
                    )
                if current_frame is None:
                    raise TraceParseError(
                        f"line {line_no}: submission-batch row encountered before first frame row"
                    )
                current_frame.submission_batches.append(
                    SubmissionBatchRecord(
                        batch_index=_parse_uint(fields[1], "batch_index", line_no, 32),
                        split_reason=_parse_uint(fields[2], "split_reason", line_no, 8),
                        split_barrier_mask=_parse_uint(fields[3], "split_barrier_mask", line_no, 8),
                        phase=_parse_uint(fields[4], "phase", line_no, 8),
                        cycle_type=_parse_uint(fields[5], "cycle_type", line_no, 8),
                        first_work_index=_parse_uint(fields[6], "first_work_index", line_no, 32),
                        last_work_index=_parse_uint(fields[7], "last_work_index", line_no, 32),
                        work_count=_parse_uint(fields[8], "work_count", line_no, 32),
                        barrier_mask_union=_parse_uint(fields[9], "barrier_mask_union", line_no, 8),
                        textured_work_count=_parse_uint(fields[10], "textured_work_count", line_no, 32),
                        depth_test_work_count=_parse_uint(fields[11], "depth_test_work_count", line_no, 32),
                        first_source_packet_id=_parse_uint(fields[12], "first_source_packet_id", line_no, 64),
                        last_source_packet_id=_parse_uint(fields[13], "last_source_packet_id", line_no, 64),
                        color_image_format=_parse_uint(fields[14], "color_image_format", line_no, 8),
                        color_image_size=_parse_uint(fields[15], "color_image_size", line_no, 8),
                        color_image_width=_parse_uint(fields[16], "color_image_width", line_no, 16),
                        color_image_address=_parse_uint(fields[17], "color_image_address", line_no, 32),
                        depth_image_address=_parse_uint(fields[18], "depth_image_address", line_no, 32),
                        scissor_mode=_parse_uint(fields[19], "scissor_mode", line_no, 8),
                        scissor_xh=_parse_uint(fields[20], "scissor_xh", line_no, 16),
                        scissor_yh=_parse_uint(fields[21], "scissor_yh", line_no, 16),
                        scissor_xl=_parse_uint(fields[22], "scissor_xl", line_no, 16),
                        scissor_yl=_parse_uint(fields[23], "scissor_yl", line_no, 16),
                    )
                )
                continue

            raise TraceParseError(f"line {line_no}: unknown row type '{row_type}'")

    if current_frame is not None:
        frames.append(current_frame)

    if len(frames) == 0:
        raise TraceParseError(f"no frame records found in {path}")

    return frames


def _fnv_update_bytes(current_hash: int, payload: bytes) -> int:
    hash_value = current_hash
    for byte in payload:
        hash_value ^= byte
        hash_value = (hash_value * FNV_PRIME) & U64_MASK
    return hash_value


def _fnv_update_int(current_hash: int, value: int, byte_width: int) -> int:
    return _fnv_update_bytes(current_hash, value.to_bytes(byte_width, "little", signed=False))


def _hash_command_stream(packets: Iterable[PacketRecord]) -> int:
    hash_value = FNV_OFFSET
    for packet in packets:
        hash_value = _fnv_update_int(hash_value, packet.packet_id, 8)
        hash_value = _fnv_update_int(hash_value, packet.domain, 1)
        hash_value = _fnv_update_int(hash_value, packet.opcode, 1)
        hash_value = _fnv_update_int(hash_value, packet.flags, 1)
        hash_value = _fnv_update_int(hash_value, packet.w0, 4)
        hash_value = _fnv_update_int(hash_value, packet.w1, 4)
        if packet.payload_word_count > 0:
            hash_value = _fnv_update_int(hash_value, packet.payload_word_count, 1)
            for payload_word in packet.payload_words[: packet.payload_word_count]:
                hash_value = _fnv_update_int(hash_value, payload_word, 4)
        elif packet.extra_word_count > 0:
            hash_value = _fnv_update_int(hash_value, packet.extra_word_count, 1)
            inline_words = [packet.w2, packet.w3, packet.w4, packet.w5, packet.w6, packet.w7]
            for idx in range(min(packet.extra_word_count, MAX_INLINE_EXTRA_WORDS)):
                hash_value = _fnv_update_int(hash_value, inline_words[idx], 4)
        if packet.full_word_count > 0:
            hash_value = _fnv_update_int(hash_value, packet.full_word_count, 2)
            hash_value = _fnv_update_int(hash_value, packet.tail_hash, 8)
        hash_value = _fnv_update_int(hash_value, packet.task_id, 4)
        hash_value = _fnv_update_int(hash_value, packet.dlist_address, 4)
        hash_value = _fnv_update_int(hash_value, packet.microcode & 0xFFFF, 2)
    return hash_value


def _bit_range(value: int, shift: int, width: int) -> int:
    return (value >> shift) & ((1 << width) - 1)


def _sign_extend(value: int, bits: int) -> int:
    sign_bit = 1 << (bits - 1)
    mask = (1 << bits) - 1
    x = value & mask
    return (x ^ sign_bit) - sign_bit


def _decode_signed_fixed(integer_part: int, integer_bits: int, fractional_part: int) -> int:
    return _sign_extend(((integer_part & ((1 << integer_bits) - 1)) << 16) | (fractional_part & 0xFFFF), integer_bits + 16)


def _decode_other_modes(mode0: int, mode1: int, decoded: OtherModesDecoded) -> None:
    decoded.alpha_compare = _bit_range(mode1, 0, 2)
    decoded.depth_source = _bit_range(mode1, 2, 1)
    decoded.aa_enable = _bit_range(mode1, 3, 1) != 0
    decoded.depth_compare = _bit_range(mode1, 4, 1) != 0
    decoded.depth_update = _bit_range(mode1, 5, 1) != 0
    decoded.image_read = _bit_range(mode1, 6, 1) != 0
    decoded.color_on_cvg = _bit_range(mode1, 7, 1) != 0
    decoded.cvg_dest = _bit_range(mode1, 8, 2)
    decoded.depth_mode = _bit_range(mode1, 10, 2)
    decoded.cvg_x_alpha = _bit_range(mode1, 12, 1) != 0
    decoded.alpha_cvg_sel = _bit_range(mode1, 13, 1) != 0
    decoded.force_blender = _bit_range(mode1, 14, 1) != 0
    decoded.texture_edge = _bit_range(mode1, 15, 1) != 0
    decoded.c2_m2b = _bit_range(mode1, 16, 2)
    decoded.c1_m2b = _bit_range(mode1, 18, 2)
    decoded.c2_m2a = _bit_range(mode1, 20, 2)
    decoded.c1_m2a = _bit_range(mode1, 22, 2)
    decoded.c2_m1b = _bit_range(mode1, 24, 2)
    decoded.c1_m1b = _bit_range(mode1, 26, 2)
    decoded.c2_m1a = _bit_range(mode1, 28, 2)
    decoded.c1_m1a = _bit_range(mode1, 30, 2)

    decoded.blend_mask = _bit_range(mode0, 0, 4)
    decoded.alpha_dither = _bit_range(mode0, 4, 2)
    decoded.color_dither = _bit_range(mode0, 6, 2)
    decoded.combine_key = _bit_range(mode0, 8, 1) != 0
    decoded.convert_one = _bit_range(mode0, 9, 1) != 0
    decoded.bi_lerp1 = _bit_range(mode0, 10, 1) != 0
    decoded.bi_lerp0 = _bit_range(mode0, 11, 1) != 0
    decoded.texture_filter = _bit_range(mode0, 12, 2)
    decoded.texture_lut = _bit_range(mode0, 14, 2)
    decoded.texture_lod = _bit_range(mode0, 16, 1) != 0
    decoded.texture_detail = _bit_range(mode0, 17, 2)
    decoded.texture_persp = _bit_range(mode0, 19, 1) != 0
    decoded.unused_color_dither = _bit_range(mode0, 22, 1) != 0
    decoded.pipeline_mode = _bit_range(mode0, 23, 1) != 0


def _decode_tile_index(w1: int) -> Optional[int]:
    tile = _bit_range(w1, 24, 3)
    if tile >= TMEM_TILE_COUNT:
        return None
    return tile


def _apply_rdp_packet(snapshot: RDPStateSnapshot, packet: PacketRecord) -> None:
    if packet.domain != COMMAND_DOMAIN_RDP:
        return

    if packet.opcode == CMD_LOAD_SYNC:
        snapshot.sync_epoch += 1
        snapshot.load_sync_count += 1
        snapshot.load_sync_packet_id = packet.packet_id
        snapshot.changed_mask |= RDP_STATE_CHANGED_LOAD_SYNC
    elif packet.opcode == CMD_PIPE_SYNC:
        snapshot.sync_epoch += 1
        snapshot.pipe_sync_count += 1
        snapshot.pipe_sync_packet_id = packet.packet_id
        snapshot.changed_mask |= RDP_STATE_CHANGED_PIPE_SYNC
    elif packet.opcode == CMD_TILE_SYNC:
        snapshot.sync_epoch += 1
        snapshot.tile_sync_count += 1
        snapshot.tile_sync_packet_id = packet.packet_id
        snapshot.changed_mask |= RDP_STATE_CHANGED_TILE_SYNC
    elif packet.opcode == CMD_FULL_SYNC:
        snapshot.sync_epoch += 1
        snapshot.full_sync_count += 1
        snapshot.full_sync_packet_id = packet.packet_id
        snapshot.changed_mask |= RDP_STATE_CHANGED_FULL_SYNC
    elif packet.opcode == CMD_SET_OTHER_MODES:
        mode0 = packet.w0 & 0x00FFFFFF
        mode1 = packet.w1
        snapshot.other_modes = ((mode0 << 32) | mode1) & U64_MASK
        snapshot.cycle_type = (mode0 >> 20) & 0x3
        _decode_other_modes(mode0, mode1, snapshot.other_modes_decoded)
        snapshot.changed_mask |= RDP_STATE_CHANGED_OTHER_MODES
    elif packet.opcode == CMD_SET_KEY_GB:
        snapshot.key_center_g = _bit_range(packet.w1, 24, 8)
        snapshot.key_scale_g = _bit_range(packet.w1, 16, 8)
        snapshot.key_width_g = _bit_range(packet.w0, 12, 12)
        snapshot.key_center_b = _bit_range(packet.w1, 8, 8)
        snapshot.key_scale_b = _bit_range(packet.w1, 0, 8)
        snapshot.key_width_b = _bit_range(packet.w0, 0, 12)
        snapshot.changed_mask |= RDP_STATE_CHANGED_KEY_GB
    elif packet.opcode == CMD_SET_KEY_R:
        snapshot.key_center_r = _bit_range(packet.w1, 8, 8)
        snapshot.key_scale_r = _bit_range(packet.w1, 0, 8)
        snapshot.key_width_r = _bit_range(packet.w1, 16, 12)
        snapshot.changed_mask |= RDP_STATE_CHANGED_KEY_R
    elif packet.opcode == CMD_SET_CONVERT:
        k0 = _bit_range(packet.w0, 13, 9)
        k1 = _bit_range(packet.w0, 4, 9)
        k2 = (_bit_range(packet.w0, 0, 4) << 5) | _bit_range(packet.w1, 27, 5)
        k3 = _bit_range(packet.w1, 18, 9)
        k4 = _bit_range(packet.w1, 9, 9)
        k5 = _bit_range(packet.w1, 0, 9)
        snapshot.convert_k0 = _sign_extend(k0, 9)
        snapshot.convert_k1 = _sign_extend(k1, 9)
        snapshot.convert_k2 = _sign_extend(k2, 9)
        snapshot.convert_k3 = _sign_extend(k3, 9)
        snapshot.convert_k4 = k4
        snapshot.convert_k5 = k5
        snapshot.changed_mask |= RDP_STATE_CHANGED_CONVERT
    elif packet.opcode == CMD_SET_PRIM_DEPTH:
        snapshot.prim_depth_z = _bit_range(packet.w1, 16, 16)
        snapshot.prim_depth_delta = _bit_range(packet.w1, 0, 16)
        snapshot.changed_mask |= RDP_STATE_CHANGED_PRIM_DEPTH
    elif packet.opcode == CMD_SET_COMBINE_MODE:
        muxs0 = packet.w0 & 0x00FFFFFF
        snapshot.combine_mux = ((muxs0 << 32) | packet.w1) & U64_MASK
        snapshot.changed_mask |= RDP_STATE_CHANGED_COMBINE
    elif packet.opcode == CMD_SET_SCISSOR:
        snapshot.scissor_mode = _bit_range(packet.w1, 24, 2)
        snapshot.scissor_xh = _bit_range(packet.w0, 12, 12)
        snapshot.scissor_yh = _bit_range(packet.w0, 0, 12)
        snapshot.scissor_xl = _bit_range(packet.w1, 12, 12)
        snapshot.scissor_yl = _bit_range(packet.w1, 0, 12)
        snapshot.changed_mask |= RDP_STATE_CHANGED_SCISSOR
    elif packet.opcode == CMD_SET_FILL_COLOR:
        snapshot.fill_color = packet.w1
        snapshot.changed_mask |= RDP_STATE_CHANGED_FILL_COLOR
    elif packet.opcode == CMD_SET_FOG_COLOR:
        snapshot.fog_color.r = _bit_range(packet.w1, 24, 8)
        snapshot.fog_color.g = _bit_range(packet.w1, 16, 8)
        snapshot.fog_color.b = _bit_range(packet.w1, 8, 8)
        snapshot.fog_color.a = _bit_range(packet.w1, 0, 8)
        snapshot.changed_mask |= RDP_STATE_CHANGED_FOG_COLOR
    elif packet.opcode == CMD_SET_BLEND_COLOR:
        snapshot.blend_color.r = _bit_range(packet.w1, 24, 8)
        snapshot.blend_color.g = _bit_range(packet.w1, 16, 8)
        snapshot.blend_color.b = _bit_range(packet.w1, 8, 8)
        snapshot.blend_color.a = _bit_range(packet.w1, 0, 8)
        snapshot.changed_mask |= RDP_STATE_CHANGED_BLEND_COLOR
    elif packet.opcode == CMD_SET_PRIM_COLOR:
        snapshot.prim_color_min_level = _bit_range(packet.w0, 8, 5)
        snapshot.prim_color_lod_frac = _bit_range(packet.w0, 0, 8)
        snapshot.prim_color.r = _bit_range(packet.w1, 24, 8)
        snapshot.prim_color.g = _bit_range(packet.w1, 16, 8)
        snapshot.prim_color.b = _bit_range(packet.w1, 8, 8)
        snapshot.prim_color.a = _bit_range(packet.w1, 0, 8)
        snapshot.changed_mask |= RDP_STATE_CHANGED_PRIM_COLOR
    elif packet.opcode == CMD_SET_ENV_COLOR:
        snapshot.env_color.r = _bit_range(packet.w1, 24, 8)
        snapshot.env_color.g = _bit_range(packet.w1, 16, 8)
        snapshot.env_color.b = _bit_range(packet.w1, 8, 8)
        snapshot.env_color.a = _bit_range(packet.w1, 0, 8)
        snapshot.changed_mask |= RDP_STATE_CHANGED_ENV_COLOR
    elif packet.opcode == CMD_SET_COLOR_IMAGE:
        snapshot.color_image_format = _bit_range(packet.w0, 21, 3)
        snapshot.color_image_size = _bit_range(packet.w0, 19, 2)
        snapshot.color_image_width = _bit_range(packet.w0, 0, 12) + 1
        snapshot.color_image_address = packet.w1
        snapshot.changed_mask |= RDP_STATE_CHANGED_COLOR_IMAGE
    elif packet.opcode == CMD_SET_DEPTH_IMAGE:
        snapshot.depth_image_address = packet.w1
        snapshot.changed_mask |= RDP_STATE_CHANGED_DEPTH_IMAGE

    snapshot.last_packet_id = packet.packet_id


def _apply_tmem_packet(snapshot: TMEMSnapshot, packet: PacketRecord) -> None:
    if packet.domain != COMMAND_DOMAIN_RDP:
        return

    if packet.opcode == CMD_SET_TEXTURE_IMAGE:
        snapshot.texture_image_format = _bit_range(packet.w0, 21, 3)
        snapshot.texture_image_size = _bit_range(packet.w0, 19, 2)
        snapshot.texture_image_width = _bit_range(packet.w0, 0, 12) + 1
        snapshot.texture_image_address = packet.w1
        snapshot.changed_mask |= TMEM_STATE_CHANGED_TEXTURE_IMAGE
    elif packet.opcode == CMD_SET_TILE:
        tile_index = _decode_tile_index(packet.w1)
        if tile_index is not None:
            tile = snapshot.tiles[tile_index]
            tile.format = _bit_range(packet.w0, 21, 3)
            tile.size = _bit_range(packet.w0, 19, 2)
            tile.line = _bit_range(packet.w0, 9, 9)
            tile.tmem = _bit_range(packet.w0, 0, 9)
            tile.palette = _bit_range(packet.w1, 20, 4)
            tile.cmt = _bit_range(packet.w1, 18, 2)
            tile.cms = _bit_range(packet.w1, 8, 2)
            tile.maskt = _bit_range(packet.w1, 14, 4)
            tile.masks = _bit_range(packet.w1, 4, 4)
            tile.shiftt = _bit_range(packet.w1, 10, 4)
            tile.shifts = _bit_range(packet.w1, 0, 4)
            snapshot.changed_mask |= TMEM_STATE_CHANGED_TILE_DESCRIPTOR
    elif packet.opcode == CMD_SET_TILE_SIZE:
        tile_index = _decode_tile_index(packet.w1)
        if tile_index is not None:
            tile = snapshot.tiles[tile_index]
            tile.uls = _bit_range(packet.w0, 12, 12)
            tile.ult = _bit_range(packet.w0, 0, 12)
            tile.lrs = _bit_range(packet.w1, 12, 12)
            tile.lrt = _bit_range(packet.w1, 0, 12)
            snapshot.changed_mask |= TMEM_STATE_CHANGED_TILE_SIZE
    elif packet.opcode == CMD_LOAD_TILE:
        tile_index = _decode_tile_index(packet.w1)
        if tile_index is not None:
            snapshot.last_load.source_packet_id = packet.packet_id
            snapshot.last_load.kind = TMEM_LOAD_KIND_TILE
            snapshot.last_load.tile = tile_index
            snapshot.last_load.uls = _bit_range(packet.w0, 12, 12)
            snapshot.last_load.ult = _bit_range(packet.w0, 0, 12)
            snapshot.last_load.lrs = _bit_range(packet.w1, 12, 12)
            snapshot.last_load.lrt = _bit_range(packet.w1, 0, 12)
            snapshot.last_load.dxt = 0
            snapshot.changed_mask |= TMEM_STATE_CHANGED_LOAD_TILE
    elif packet.opcode == CMD_LOAD_BLOCK:
        tile_index = _decode_tile_index(packet.w1)
        if tile_index is not None:
            snapshot.last_load.source_packet_id = packet.packet_id
            snapshot.last_load.kind = TMEM_LOAD_KIND_BLOCK
            snapshot.last_load.tile = tile_index
            snapshot.last_load.uls = _bit_range(packet.w0, 12, 12)
            snapshot.last_load.ult = _bit_range(packet.w0, 0, 12)
            snapshot.last_load.lrs = _bit_range(packet.w1, 12, 12)
            snapshot.last_load.lrt = 0
            snapshot.last_load.dxt = _bit_range(packet.w1, 0, 12)
            snapshot.changed_mask |= TMEM_STATE_CHANGED_LOAD_BLOCK
    elif packet.opcode == CMD_LOAD_TLUT:
        tile_index = _decode_tile_index(packet.w1)
        if tile_index is not None:
            snapshot.last_load.source_packet_id = packet.packet_id
            snapshot.last_load.kind = TMEM_LOAD_KIND_TLUT
            snapshot.last_load.tile = tile_index
            snapshot.last_load.uls = _bit_range(packet.w0, 12, 12)
            snapshot.last_load.ult = _bit_range(packet.w0, 0, 12)
            snapshot.last_load.lrs = _bit_range(packet.w1, 12, 12)
            snapshot.last_load.lrt = _bit_range(packet.w1, 0, 12)
            snapshot.last_load.dxt = 0
            snapshot.changed_mask |= TMEM_STATE_CHANGED_LOAD_TLUT

    snapshot.last_packet_id = packet.packet_id


def _hash_rdp_state(snapshot: RDPStateSnapshot) -> int:
    def _hash_other_modes_decoded(decoded: OtherModesDecoded) -> int:
        hash_value = FNV_OFFSET
        hash_value = _fnv_update_int(hash_value, decoded.alpha_compare, 1)
        hash_value = _fnv_update_int(hash_value, decoded.depth_source, 1)
        hash_value = _fnv_update_int(hash_value, decoded.cvg_dest, 1)
        hash_value = _fnv_update_int(hash_value, decoded.depth_mode, 1)
        hash_value = _fnv_update_int(hash_value, decoded.c2_m2b, 1)
        hash_value = _fnv_update_int(hash_value, decoded.c1_m2b, 1)
        hash_value = _fnv_update_int(hash_value, decoded.c2_m2a, 1)
        hash_value = _fnv_update_int(hash_value, decoded.c1_m2a, 1)
        hash_value = _fnv_update_int(hash_value, decoded.c2_m1b, 1)
        hash_value = _fnv_update_int(hash_value, decoded.c1_m1b, 1)
        hash_value = _fnv_update_int(hash_value, decoded.c2_m1a, 1)
        hash_value = _fnv_update_int(hash_value, decoded.c1_m1a, 1)
        hash_value = _fnv_update_int(hash_value, decoded.blend_mask, 1)
        hash_value = _fnv_update_int(hash_value, decoded.alpha_dither, 1)
        hash_value = _fnv_update_int(hash_value, decoded.color_dither, 1)
        hash_value = _fnv_update_int(hash_value, decoded.texture_filter, 1)
        hash_value = _fnv_update_int(hash_value, decoded.texture_lut, 1)
        hash_value = _fnv_update_int(hash_value, decoded.texture_detail, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.aa_enable else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.depth_compare else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.depth_update else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.image_read else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.color_on_cvg else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.cvg_x_alpha else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.alpha_cvg_sel else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.force_blender else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.texture_edge else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.combine_key else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.convert_one else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.bi_lerp1 else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.bi_lerp0 else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.texture_lod else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.texture_persp else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.unused_color_dither else 0, 1)
        hash_value = _fnv_update_int(hash_value, 1 if decoded.pipeline_mode else 0, 1)
        return hash_value

    hash_value = FNV_OFFSET
    hash_value = _fnv_update_int(hash_value, snapshot.last_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, snapshot.combine_mux, 8)
    hash_value = _fnv_update_int(hash_value, snapshot.other_modes, 8)
    hash_value = _fnv_update_int(hash_value, _hash_other_modes_decoded(snapshot.other_modes_decoded), 8)
    hash_value = _fnv_update_int(hash_value, snapshot.cycle_type, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.color_image_format, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.color_image_size, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.color_image_width, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.color_image_address, 4)
    hash_value = _fnv_update_int(hash_value, snapshot.depth_image_address, 4)
    hash_value = _fnv_update_int(hash_value, snapshot.scissor_mode, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.scissor_xh, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.scissor_yh, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.scissor_xl, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.scissor_yl, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.prim_color.r, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.prim_color.g, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.prim_color.b, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.prim_color.a, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.env_color.r, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.env_color.g, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.env_color.b, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.env_color.a, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.blend_color.r, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.blend_color.g, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.blend_color.b, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.blend_color.a, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.fog_color.r, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.fog_color.g, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.fog_color.b, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.fog_color.a, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.prim_color_min_level, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.prim_color_lod_frac, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.fill_color, 4)
    hash_value = _fnv_update_int(hash_value, snapshot.prim_depth_z, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.prim_depth_delta, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.convert_k0 & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.convert_k1 & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.convert_k2 & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.convert_k3 & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.convert_k4 & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.convert_k5 & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.key_center_r, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.key_scale_r, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.key_center_g, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.key_scale_g, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.key_center_b, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.key_scale_b, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.key_width_r, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.key_width_g, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.key_width_b, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.sync_epoch, 4)
    hash_value = _fnv_update_int(hash_value, snapshot.load_sync_count, 4)
    hash_value = _fnv_update_int(hash_value, snapshot.pipe_sync_count, 4)
    hash_value = _fnv_update_int(hash_value, snapshot.tile_sync_count, 4)
    hash_value = _fnv_update_int(hash_value, snapshot.full_sync_count, 4)
    hash_value = _fnv_update_int(hash_value, snapshot.load_sync_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, snapshot.pipe_sync_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, snapshot.tile_sync_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, snapshot.full_sync_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, snapshot.changed_mask, 4)
    return hash_value


def _hash_tmem_state(snapshot: TMEMSnapshot) -> int:
    hash_value = FNV_OFFSET
    hash_value = _fnv_update_int(hash_value, snapshot.last_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, snapshot.texture_image_format, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.texture_image_size, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.texture_image_width, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.texture_image_address, 4)
    for tile in snapshot.tiles:
        hash_value = _fnv_update_int(hash_value, tile.format, 1)
        hash_value = _fnv_update_int(hash_value, tile.size, 1)
        hash_value = _fnv_update_int(hash_value, tile.line, 2)
        hash_value = _fnv_update_int(hash_value, tile.tmem, 2)
        hash_value = _fnv_update_int(hash_value, tile.palette, 1)
        hash_value = _fnv_update_int(hash_value, tile.cmt, 1)
        hash_value = _fnv_update_int(hash_value, tile.cms, 1)
        hash_value = _fnv_update_int(hash_value, tile.maskt, 1)
        hash_value = _fnv_update_int(hash_value, tile.masks, 1)
        hash_value = _fnv_update_int(hash_value, tile.shiftt, 1)
        hash_value = _fnv_update_int(hash_value, tile.shifts, 1)
        hash_value = _fnv_update_int(hash_value, tile.uls, 2)
        hash_value = _fnv_update_int(hash_value, tile.ult, 2)
        hash_value = _fnv_update_int(hash_value, tile.lrs, 2)
        hash_value = _fnv_update_int(hash_value, tile.lrt, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.last_load.source_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, snapshot.last_load.kind, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.last_load.tile, 1)
    hash_value = _fnv_update_int(hash_value, snapshot.last_load.uls, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.last_load.ult, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.last_load.lrs, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.last_load.lrt, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.last_load.dxt, 2)
    hash_value = _fnv_update_int(hash_value, snapshot.changed_mask, 4)
    return hash_value


def _is_triangle_opcode(opcode: int) -> bool:
    return 0x08 <= opcode <= 0x0F


def _is_texrect_opcode(opcode: int) -> bool:
    return opcode == 0x24 or opcode == 0x25


def _is_fill_rect_opcode(opcode: int) -> bool:
    return opcode == 0x36


def _is_draw_opcode(opcode: int) -> bool:
    return _is_triangle_opcode(opcode) or _is_texrect_opcode(opcode) or _is_fill_rect_opcode(opcode)


def _is_known_rdp_opcode(opcode: int) -> bool:
    return opcode in (
        0x00,
        0x08,
        0x09,
        0x0A,
        0x0B,
        0x0C,
        0x0D,
        0x0E,
        0x0F,
        0x24,
        0x25,
        0x26,
        0x27,
        0x28,
        0x29,
        0x2A,
        0x2B,
        0x2C,
        0x2D,
        0x2E,
        0x2F,
        0x30,
        0x32,
        0x33,
        0x34,
        0x35,
        0x36,
        0x37,
        0x38,
        0x39,
        0x3A,
        0x3B,
        0x3C,
        0x3D,
        0x3E,
        0x3F,
    )


def _is_textured_opcode(opcode: int) -> bool:
    return opcode in (0x0A, 0x0B, 0x0E, 0x0F) or _is_texrect_opcode(opcode)


def _is_depth_opcode(opcode: int) -> bool:
    return opcode in (0x09, 0x0B, 0x0D, 0x0F)


def _is_triangle_shade_opcode(opcode: int) -> bool:
    return opcode in (0x0C, 0x0D, 0x0E, 0x0F)


def _is_triangle_texture_opcode(opcode: int) -> bool:
    return opcode in (0x0A, 0x0B, 0x0E, 0x0F)


def _is_triangle_zbuffer_opcode(opcode: int) -> bool:
    return opcode in (0x09, 0x0B, 0x0D, 0x0F)


def _packet_payload_count(packet: PacketRecord) -> int:
    if packet.payload_word_count > 0:
        return packet.payload_word_count
    return packet.extra_word_count


def _packet_payload_word(packet: PacketRecord, index: int) -> int:
    if index < len(packet.payload_words):
        return packet.payload_words[index]
    inline_words = [packet.w2, packet.w3, packet.w4, packet.w5, packet.w6, packet.w7]
    if index < len(inline_words):
        return inline_words[index]
    return 0


def _triangle_copy_payload_range(
    packet: PacketRecord, expanded_words: List[int], payload_offset: int, count: int, expanded_offset: int
) -> None:
    payload_count = _packet_payload_count(packet)
    for i in range(count):
        src = payload_offset + i
        dst = expanded_offset + i
        if src >= payload_count or dst >= len(expanded_words):
            break
        expanded_words[dst] = _packet_payload_word(packet, src)


def _build_triangle_expanded_words(packet: PacketRecord) -> List[int]:
    expanded_words = [0] * 44
    expanded_words[0] = packet.w0
    expanded_words[1] = packet.w1
    if packet.opcode == 0x08:
        _triangle_copy_payload_range(packet, expanded_words, 0, 6, 2)
    elif packet.opcode == 0x09:
        _triangle_copy_payload_range(packet, expanded_words, 0, 6, 2)
        _triangle_copy_payload_range(packet, expanded_words, 6, 4, 40)
    elif packet.opcode == 0x0A:
        _triangle_copy_payload_range(packet, expanded_words, 0, 6, 2)
        _triangle_copy_payload_range(packet, expanded_words, 6, 16, 24)
    elif packet.opcode == 0x0B:
        _triangle_copy_payload_range(packet, expanded_words, 0, 6, 2)
        _triangle_copy_payload_range(packet, expanded_words, 6, 16, 24)
        _triangle_copy_payload_range(packet, expanded_words, 22, 4, 40)
    elif packet.opcode == 0x0C:
        _triangle_copy_payload_range(packet, expanded_words, 0, 22, 2)
    elif packet.opcode == 0x0D:
        _triangle_copy_payload_range(packet, expanded_words, 0, 22, 2)
        _triangle_copy_payload_range(packet, expanded_words, 22, 4, 40)
    elif packet.opcode == 0x0E:
        _triangle_copy_payload_range(packet, expanded_words, 0, 38, 2)
    elif packet.opcode == 0x0F:
        _triangle_copy_payload_range(packet, expanded_words, 0, 42, 2)
    return expanded_words


def _decode_pair_primary(a: int, b: int) -> int:
    return _sign_extend((a & 0xFFFF0000) | ((b >> 16) & 0xFFFF), 32)


def _decode_pair_secondary(a: int, b: int) -> int:
    return _sign_extend(((a << 16) & 0xFFFF0000) | (b & 0xFFFF), 32)


def _build_draw_semantic(
    packet: PacketRecord, rdp_snapshot: RDPStateSnapshot, tmem_snapshot: TMEMSnapshot
) -> DrawSemanticRecord:
    blend_params = (
        (rdp_snapshot.other_modes_decoded.alpha_compare & 0x3)
        | ((rdp_snapshot.other_modes_decoded.cvg_dest & 0x3) << 2)
        | ((rdp_snapshot.other_modes_decoded.depth_mode & 0x3) << 4)
        | ((1 if rdp_snapshot.other_modes_decoded.force_blender else 0) << 6)
        | ((1 if rdp_snapshot.other_modes_decoded.depth_compare else 0) << 7)
        | ((1 if rdp_snapshot.other_modes_decoded.depth_update else 0) << 8)
        | ((1 if rdp_snapshot.other_modes_decoded.alpha_cvg_sel else 0) << 9)
        | ((1 if rdp_snapshot.other_modes_decoded.cvg_x_alpha else 0) << 10)
        | ((1 if rdp_snapshot.other_modes_decoded.texture_edge else 0) << 11)
    )

    textured = _is_textured_opcode(packet.opcode)
    draw_type = 0
    tile = 0
    tex_rect_flip = False
    rect_ulx = 0
    rect_uly = 0
    rect_lrx = 0
    rect_lry = 0
    tex_s = 0
    tex_t = 0
    tex_dsdx = 0
    tex_dtdy = 0
    triangle_lmajor = False
    triangle_level = 0
    triangle_yl = 0
    triangle_ym = 0
    triangle_yh = 0
    triangle_xl = 0
    triangle_xh = 0
    triangle_xm = 0
    triangle_dxldy = 0
    triangle_dxhdy = 0
    triangle_dxmdy = 0
    triangle_shade_enable = False
    triangle_texture_enable = False
    triangle_zbuffer_enable = False
    triangle_shade_r = 0
    triangle_shade_g = 0
    triangle_shade_b = 0
    triangle_shade_a = 0
    triangle_shade_drdx = 0
    triangle_shade_dgdx = 0
    triangle_shade_dbdx = 0
    triangle_shade_dadx = 0
    triangle_shade_drde = 0
    triangle_shade_dgde = 0
    triangle_shade_dbde = 0
    triangle_shade_dade = 0
    triangle_shade_drdy = 0
    triangle_shade_dgdy = 0
    triangle_shade_dbdy = 0
    triangle_shade_dady = 0
    triangle_tex_s = 0
    triangle_tex_t = 0
    triangle_tex_w = 0
    triangle_tex_dsdx = 0
    triangle_tex_dtdx = 0
    triangle_tex_dwdx = 0
    triangle_tex_dsde = 0
    triangle_tex_dtde = 0
    triangle_tex_dwde = 0
    triangle_tex_dsdy = 0
    triangle_tex_dtdy = 0
    triangle_tex_dwdy = 0
    triangle_z = 0
    triangle_dzdx = 0
    triangle_dzde = 0
    triangle_dzdy = 0

    if _is_triangle_opcode(packet.opcode):
        draw_type = 1
    elif _is_texrect_opcode(packet.opcode):
        draw_type = 2
    elif _is_fill_rect_opcode(packet.opcode):
        draw_type = 3

    if draw_type in (2, 3):
        rect_ulx = _bit_range(packet.w1, 12, 12)
        rect_uly = _bit_range(packet.w1, 0, 12)
        rect_lrx = _bit_range(packet.w0, 12, 12)
        rect_lry = _bit_range(packet.w0, 0, 12)

    payload_count = _packet_payload_count(packet)

    if draw_type == 2:
        tile = _bit_range(packet.w1, 24, 3)
        tex_rect_flip = packet.opcode == 0x25
        if payload_count > 1:
            w2 = _packet_payload_word(packet, 0)
            w3 = _packet_payload_word(packet, 1)
            tex_s = _sign_extend(_bit_range(w2, 16, 16), 16)
            tex_t = _sign_extend(_bit_range(w2, 0, 16), 16)
            tex_dsdx = _sign_extend(_bit_range(w3, 16, 16), 16)
            tex_dtdy = _sign_extend(_bit_range(w3, 0, 16), 16)
    elif draw_type == 1:
        triangle_lmajor = _bit_range(packet.w0, 23, 1) != 0
        triangle_level = _bit_range(packet.w0, 19, 3)
        tile = _bit_range(packet.w0, 16, 3)
        triangle_yl = _bit_range(packet.w0, 0, 14)
        triangle_ym = _bit_range(packet.w1, 16, 14)
        triangle_yh = _bit_range(packet.w1, 0, 14)
        if payload_count > 1:
            w2 = _packet_payload_word(packet, 0)
            w3 = _packet_payload_word(packet, 1)
            triangle_xl = _decode_signed_fixed(_bit_range(w2, 16, 12), 12, _bit_range(w2, 0, 16))
            triangle_dxldy = _decode_signed_fixed(_bit_range(w3, 16, 14), 14, _bit_range(w3, 0, 16))
        if payload_count > 3:
            w4 = _packet_payload_word(packet, 2)
            w5 = _packet_payload_word(packet, 3)
            triangle_xh = _decode_signed_fixed(_bit_range(w4, 16, 12), 12, _bit_range(w4, 0, 16))
            triangle_dxhdy = _decode_signed_fixed(_bit_range(w5, 16, 14), 14, _bit_range(w5, 0, 16))
        if payload_count > 5:
            w6 = _packet_payload_word(packet, 4)
            w7 = _packet_payload_word(packet, 5)
            triangle_xm = _decode_signed_fixed(_bit_range(w6, 16, 12), 12, _bit_range(w6, 0, 16))
            triangle_dxmdy = _decode_signed_fixed(_bit_range(w7, 16, 14), 14, _bit_range(w7, 0, 16))

        triangle_shade_enable = _is_triangle_shade_opcode(packet.opcode)
        triangle_texture_enable = _is_triangle_texture_opcode(packet.opcode)
        triangle_zbuffer_enable = _is_triangle_zbuffer_opcode(packet.opcode)
        expanded_words = _build_triangle_expanded_words(packet)

        if triangle_shade_enable:
            triangle_shade_r = _decode_pair_primary(expanded_words[8], expanded_words[12])
            triangle_shade_g = _decode_pair_secondary(expanded_words[8], expanded_words[12])
            triangle_shade_b = _decode_pair_primary(expanded_words[9], expanded_words[13])
            triangle_shade_a = _decode_pair_secondary(expanded_words[9], expanded_words[13])
            triangle_shade_drdx = _decode_pair_primary(expanded_words[10], expanded_words[14])
            triangle_shade_dgdx = _decode_pair_secondary(expanded_words[10], expanded_words[14])
            triangle_shade_dbdx = _decode_pair_primary(expanded_words[11], expanded_words[15])
            triangle_shade_dadx = _decode_pair_secondary(expanded_words[11], expanded_words[15])
            triangle_shade_drde = _decode_pair_primary(expanded_words[16], expanded_words[20])
            triangle_shade_dgde = _decode_pair_secondary(expanded_words[16], expanded_words[20])
            triangle_shade_dbde = _decode_pair_primary(expanded_words[17], expanded_words[21])
            triangle_shade_dade = _decode_pair_secondary(expanded_words[17], expanded_words[21])
            triangle_shade_drdy = _decode_pair_primary(expanded_words[18], expanded_words[22])
            triangle_shade_dgdy = _decode_pair_secondary(expanded_words[18], expanded_words[22])
            triangle_shade_dbdy = _decode_pair_primary(expanded_words[19], expanded_words[23])
            triangle_shade_dady = _decode_pair_secondary(expanded_words[19], expanded_words[23])

        if triangle_texture_enable:
            triangle_tex_s = _decode_pair_primary(expanded_words[24], expanded_words[28])
            triangle_tex_t = _decode_pair_secondary(expanded_words[24], expanded_words[28])
            triangle_tex_w = _decode_pair_primary(expanded_words[25], expanded_words[29])
            triangle_tex_dsdx = _decode_pair_primary(expanded_words[26], expanded_words[30])
            triangle_tex_dtdx = _decode_pair_secondary(expanded_words[26], expanded_words[30])
            triangle_tex_dwdx = _decode_pair_primary(expanded_words[27], expanded_words[31])
            triangle_tex_dsde = _decode_pair_primary(expanded_words[32], expanded_words[36])
            triangle_tex_dtde = _decode_pair_secondary(expanded_words[32], expanded_words[36])
            triangle_tex_dwde = _decode_pair_primary(expanded_words[33], expanded_words[37])
            triangle_tex_dsdy = _decode_pair_primary(expanded_words[34], expanded_words[38])
            triangle_tex_dtdy = _decode_pair_secondary(expanded_words[34], expanded_words[38])
            triangle_tex_dwdy = _decode_pair_primary(expanded_words[35], expanded_words[39])

        if triangle_zbuffer_enable:
            triangle_z = _sign_extend(expanded_words[40], 32)
            triangle_dzdx = _sign_extend(expanded_words[41], 32)
            triangle_dzde = _sign_extend(expanded_words[42], 32)
            triangle_dzdy = _sign_extend(expanded_words[43], 32)
    elif textured:
        tile = _bit_range(packet.w0, 16, 3)

    if textured:
        tile_index = tile & 0x7
        tile_state = tmem_snapshot.tiles[tile_index]
        if (
            tile_state.format == 0
            and tile_state.size == 0
            and tile_state.line == 0
            and tile_state.tmem == 0
        ):
            textured = False

    return DrawSemanticRecord(
        source_packet_id=packet.packet_id,
        source_opcode=packet.opcode,
        draw_type=draw_type,
        tile=tile,
        tex_rect_flip=tex_rect_flip,
        cycle_type=rdp_snapshot.cycle_type,
        combine_mux=rdp_snapshot.combine_mux,
        blend_mux1=(rdp_snapshot.other_modes >> 32) & 0xFFFFFFFF,
        blend_mux2=rdp_snapshot.other_modes & 0xFFFFFFFF,
        blend_params=blend_params,
        rect_ulx=rect_ulx,
        rect_uly=rect_uly,
        rect_lrx=rect_lrx,
        rect_lry=rect_lry,
        tex_s=tex_s,
        tex_t=tex_t,
        tex_dsdx=tex_dsdx,
        tex_dtdy=tex_dtdy,
        triangle_lmajor=triangle_lmajor,
        triangle_level=triangle_level,
        triangle_yl=triangle_yl,
        triangle_ym=triangle_ym,
        triangle_yh=triangle_yh,
        triangle_xl=triangle_xl,
        triangle_xh=triangle_xh,
        triangle_xm=triangle_xm,
        triangle_dxldy=triangle_dxldy,
        triangle_dxhdy=triangle_dxhdy,
        triangle_dxmdy=triangle_dxmdy,
        triangle_shade_enable=triangle_shade_enable,
        triangle_texture_enable=triangle_texture_enable,
        triangle_zbuffer_enable=triangle_zbuffer_enable,
        triangle_shade_r=triangle_shade_r,
        triangle_shade_g=triangle_shade_g,
        triangle_shade_b=triangle_shade_b,
        triangle_shade_a=triangle_shade_a,
        triangle_shade_drdx=triangle_shade_drdx,
        triangle_shade_dgdx=triangle_shade_dgdx,
        triangle_shade_dbdx=triangle_shade_dbdx,
        triangle_shade_dadx=triangle_shade_dadx,
        triangle_shade_drde=triangle_shade_drde,
        triangle_shade_dgde=triangle_shade_dgde,
        triangle_shade_dbde=triangle_shade_dbde,
        triangle_shade_dade=triangle_shade_dade,
        triangle_shade_drdy=triangle_shade_drdy,
        triangle_shade_dgdy=triangle_shade_dgdy,
        triangle_shade_dbdy=triangle_shade_dbdy,
        triangle_shade_dady=triangle_shade_dady,
        triangle_tex_s=triangle_tex_s,
        triangle_tex_t=triangle_tex_t,
        triangle_tex_w=triangle_tex_w,
        triangle_tex_dsdx=triangle_tex_dsdx,
        triangle_tex_dtdx=triangle_tex_dtdx,
        triangle_tex_dwdx=triangle_tex_dwdx,
        triangle_tex_dsde=triangle_tex_dsde,
        triangle_tex_dtde=triangle_tex_dtde,
        triangle_tex_dwde=triangle_tex_dwde,
        triangle_tex_dsdy=triangle_tex_dsdy,
        triangle_tex_dtdy=triangle_tex_dtdy,
        triangle_tex_dwdy=triangle_tex_dwdy,
        triangle_z=triangle_z,
        triangle_dzdx=triangle_dzdx,
        triangle_dzde=triangle_dzde,
        triangle_dzdy=triangle_dzdy,
        textured=textured,
        depth_test=rdp_snapshot.other_modes_decoded.depth_compare and _is_depth_opcode(packet.opcode),
        sync_epoch=rdp_snapshot.sync_epoch,
        load_sync_packet_id=rdp_snapshot.load_sync_packet_id,
        pipe_sync_packet_id=rdp_snapshot.pipe_sync_packet_id,
        tile_sync_packet_id=rdp_snapshot.tile_sync_packet_id,
        full_sync_packet_id=rdp_snapshot.full_sync_packet_id,
    )


def _hash_draw_semantic(semantic: DrawSemanticRecord) -> int:
    hash_value = FNV_OFFSET
    hash_value = _fnv_update_int(hash_value, semantic.source_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, semantic.source_opcode, 1)
    hash_value = _fnv_update_int(hash_value, semantic.draw_type, 1)
    hash_value = _fnv_update_int(hash_value, semantic.tile, 1)
    hash_value = _fnv_update_int(hash_value, 1 if semantic.tex_rect_flip else 0, 1)
    hash_value = _fnv_update_int(hash_value, semantic.cycle_type, 1)
    hash_value = _fnv_update_int(hash_value, semantic.combine_mux, 8)
    hash_value = _fnv_update_int(hash_value, semantic.blend_mux1, 4)
    hash_value = _fnv_update_int(hash_value, semantic.blend_mux2, 4)
    hash_value = _fnv_update_int(hash_value, semantic.blend_params, 4)
    hash_value = _fnv_update_int(hash_value, semantic.rect_ulx, 2)
    hash_value = _fnv_update_int(hash_value, semantic.rect_uly, 2)
    hash_value = _fnv_update_int(hash_value, semantic.rect_lrx, 2)
    hash_value = _fnv_update_int(hash_value, semantic.rect_lry, 2)
    hash_value = _fnv_update_int(hash_value, semantic.tex_s & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, semantic.tex_t & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, semantic.tex_dsdx & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, semantic.tex_dtdy & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, 1 if semantic.triangle_lmajor else 0, 1)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_level, 1)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_yl, 2)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_ym, 2)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_yh, 2)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_xl & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_xh & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_xm & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_dxldy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_dxhdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_dxmdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, 1 if semantic.triangle_shade_enable else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if semantic.triangle_texture_enable else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if semantic.triangle_zbuffer_enable else 0, 1)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_r & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_g & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_b & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_a & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_drdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_dgdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_dbdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_dadx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_drde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_dgde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_dbde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_dade & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_drdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_dgdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_dbdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_shade_dady & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_tex_s & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_tex_t & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_tex_w & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_tex_dsdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_tex_dtdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_tex_dwdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_tex_dsde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_tex_dtde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_tex_dwde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_tex_dsdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_tex_dtdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_tex_dwdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_z & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_dzdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_dzde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, semantic.triangle_dzdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, 1 if semantic.textured else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if semantic.depth_test else 0, 1)
    hash_value = _fnv_update_int(hash_value, semantic.sync_epoch, 4)
    hash_value = _fnv_update_int(hash_value, semantic.load_sync_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, semantic.pipe_sync_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, semantic.tile_sync_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, semantic.full_sync_packet_id, 8)
    return hash_value


def _hash_draw_semantics(semantics: Iterable[DrawSemanticRecord]) -> int:
    hash_value = FNV_OFFSET
    for semantic in semantics:
        hash_value = _fnv_update_int(hash_value, _hash_draw_semantic(semantic), 8)
    return hash_value


def _build_raster_op(semantic: DrawSemanticRecord, rdp_snapshot: RDPStateSnapshot) -> RasterOpRecord:
    op_kind = 0
    if semantic.draw_type == 1:
        op_kind = 1
    elif semantic.draw_type == 2:
        op_kind = 2
    elif semantic.draw_type == 3:
        op_kind = 3
    return RasterOpRecord(
        source_packet_id=semantic.source_packet_id,
        source_opcode=semantic.source_opcode,
        op_kind=op_kind,
        cycle_type=semantic.cycle_type,
        tile=semantic.tile,
        tex_rect_flip=semantic.tex_rect_flip,
        textured=semantic.textured,
        depth_test=semantic.depth_test,
        rect_ulx=semantic.rect_ulx,
        rect_uly=semantic.rect_uly,
        rect_lrx=semantic.rect_lrx,
        rect_lry=semantic.rect_lry,
        tex_s=semantic.tex_s,
        tex_t=semantic.tex_t,
        tex_dsdx=semantic.tex_dsdx,
        tex_dtdy=semantic.tex_dtdy,
        triangle_lmajor=semantic.triangle_lmajor,
        triangle_level=semantic.triangle_level,
        triangle_yl=semantic.triangle_yl,
        triangle_ym=semantic.triangle_ym,
        triangle_yh=semantic.triangle_yh,
        triangle_xl=semantic.triangle_xl,
        triangle_xh=semantic.triangle_xh,
        triangle_xm=semantic.triangle_xm,
        triangle_dxldy=semantic.triangle_dxldy,
        triangle_dxhdy=semantic.triangle_dxhdy,
        triangle_dxmdy=semantic.triangle_dxmdy,
        triangle_shade_enable=semantic.triangle_shade_enable,
        triangle_texture_enable=semantic.triangle_texture_enable,
        triangle_zbuffer_enable=semantic.triangle_zbuffer_enable,
        triangle_shade_r=semantic.triangle_shade_r,
        triangle_shade_g=semantic.triangle_shade_g,
        triangle_shade_b=semantic.triangle_shade_b,
        triangle_shade_a=semantic.triangle_shade_a,
        triangle_shade_drdx=semantic.triangle_shade_drdx,
        triangle_shade_dgdx=semantic.triangle_shade_dgdx,
        triangle_shade_dbdx=semantic.triangle_shade_dbdx,
        triangle_shade_dadx=semantic.triangle_shade_dadx,
        triangle_shade_drde=semantic.triangle_shade_drde,
        triangle_shade_dgde=semantic.triangle_shade_dgde,
        triangle_shade_dbde=semantic.triangle_shade_dbde,
        triangle_shade_dade=semantic.triangle_shade_dade,
        triangle_shade_drdy=semantic.triangle_shade_drdy,
        triangle_shade_dgdy=semantic.triangle_shade_dgdy,
        triangle_shade_dbdy=semantic.triangle_shade_dbdy,
        triangle_shade_dady=semantic.triangle_shade_dady,
        triangle_tex_s=semantic.triangle_tex_s,
        triangle_tex_t=semantic.triangle_tex_t,
        triangle_tex_w=semantic.triangle_tex_w,
        triangle_tex_dsdx=semantic.triangle_tex_dsdx,
        triangle_tex_dtdx=semantic.triangle_tex_dtdx,
        triangle_tex_dwdx=semantic.triangle_tex_dwdx,
        triangle_tex_dsde=semantic.triangle_tex_dsde,
        triangle_tex_dtde=semantic.triangle_tex_dtde,
        triangle_tex_dwde=semantic.triangle_tex_dwde,
        triangle_tex_dsdy=semantic.triangle_tex_dsdy,
        triangle_tex_dtdy=semantic.triangle_tex_dtdy,
        triangle_tex_dwdy=semantic.triangle_tex_dwdy,
        triangle_z=semantic.triangle_z,
        triangle_dzdx=semantic.triangle_dzdx,
        triangle_dzde=semantic.triangle_dzde,
        triangle_dzdy=semantic.triangle_dzdy,
        combine_mux=semantic.combine_mux,
        blend_params=semantic.blend_params,
        fill_color=rdp_snapshot.fill_color,
        sync_epoch=semantic.sync_epoch,
        load_sync_packet_id=semantic.load_sync_packet_id,
        pipe_sync_packet_id=semantic.pipe_sync_packet_id,
        tile_sync_packet_id=semantic.tile_sync_packet_id,
        full_sync_packet_id=semantic.full_sync_packet_id,
    )


def _hash_raster_op(op: RasterOpRecord) -> int:
    hash_value = FNV_OFFSET
    hash_value = _fnv_update_int(hash_value, op.source_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, op.source_opcode, 1)
    hash_value = _fnv_update_int(hash_value, op.op_kind, 1)
    hash_value = _fnv_update_int(hash_value, op.cycle_type, 1)
    hash_value = _fnv_update_int(hash_value, op.tile, 1)
    hash_value = _fnv_update_int(hash_value, 1 if op.tex_rect_flip else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if op.textured else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if op.depth_test else 0, 1)
    hash_value = _fnv_update_int(hash_value, op.rect_ulx, 2)
    hash_value = _fnv_update_int(hash_value, op.rect_uly, 2)
    hash_value = _fnv_update_int(hash_value, op.rect_lrx, 2)
    hash_value = _fnv_update_int(hash_value, op.rect_lry, 2)
    hash_value = _fnv_update_int(hash_value, op.tex_s & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, op.tex_t & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, op.tex_dsdx & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, op.tex_dtdy & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, 1 if op.triangle_lmajor else 0, 1)
    hash_value = _fnv_update_int(hash_value, op.triangle_level, 1)
    hash_value = _fnv_update_int(hash_value, op.triangle_yl, 2)
    hash_value = _fnv_update_int(hash_value, op.triangle_ym, 2)
    hash_value = _fnv_update_int(hash_value, op.triangle_yh, 2)
    hash_value = _fnv_update_int(hash_value, op.triangle_xl & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_xh & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_xm & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_dxldy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_dxhdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_dxmdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, 1 if op.triangle_shade_enable else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if op.triangle_texture_enable else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if op.triangle_zbuffer_enable else 0, 1)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_r & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_g & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_b & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_a & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_drdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_dgdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_dbdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_dadx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_drde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_dgde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_dbde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_dade & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_drdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_dgdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_dbdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_shade_dady & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_tex_s & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_tex_t & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_tex_w & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_tex_dsdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_tex_dtdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_tex_dwdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_tex_dsde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_tex_dtde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_tex_dwde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_tex_dsdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_tex_dtdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_tex_dwdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_z & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_dzdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_dzde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.triangle_dzdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, op.combine_mux, 8)
    hash_value = _fnv_update_int(hash_value, op.blend_params, 4)
    hash_value = _fnv_update_int(hash_value, op.fill_color, 4)
    hash_value = _fnv_update_int(hash_value, op.sync_epoch, 4)
    hash_value = _fnv_update_int(hash_value, op.load_sync_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, op.pipe_sync_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, op.tile_sync_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, op.full_sync_packet_id, 8)
    return hash_value


def _hash_raster_ops(ops: Iterable[RasterOpRecord]) -> int:
    hash_value = FNV_OFFSET
    for op in ops:
        hash_value = _fnv_update_int(hash_value, _hash_raster_op(op), 8)
    return hash_value


def _classify_render_phase(cycle_type: int, op_kind: int) -> int:
    if op_kind == 3 or cycle_type == 3:
        return RENDER_PHASE_FILL
    if cycle_type == 2:
        return RENDER_PHASE_COPY
    if cycle_type == 1:
        return RENDER_PHASE_CYCLE2
    if cycle_type == 0:
        return RENDER_PHASE_CYCLE1
    return RENDER_PHASE_UNKNOWN


def _is_renderable_raster_op(op: RasterOpRecord) -> bool:
    return _classify_render_phase(op.cycle_type, op.op_kind) != RENDER_PHASE_UNKNOWN


def _set_barrier_if_advanced(packet_id: int, last_packet_id: int, bit: int) -> tuple[int, int]:
    mask = 0
    if packet_id != 0 and packet_id != last_packet_id:
        mask |= bit
    return mask, packet_id


def _clamp_rect_coord(value: int) -> int:
    if value < 0:
        return 0
    if value > 0xFFFF:
        return 0xFFFF
    return value


def _eval_edge_x_fixed16(x_start: int, dxdy: int, y_start: int, y_target: int) -> int:
    x = int(x_start) + int(dxdy) * (int(y_target) - int(y_start))
    if x < -0x80000000:
        return -0x80000000
    if x > 0x7FFFFFFF:
        return 0x7FFFFFFF
    return x


def _derive_triangle_rect_bounds(op: RasterOpRecord) -> tuple[int, int, int, int]:
    yh = int(op.triangle_yh)
    ym = int(op.triangle_ym)
    yl = int(op.triangle_yl)
    y_min_subpixel = min(yh, ym, yl)
    y_max_subpixel = max(yh, ym, yl)
    y_min = _clamp_rect_coord(y_min_subpixel >> 2)
    y_max = _clamp_rect_coord((y_max_subpixel + 3) >> 2)

    xh = int(op.triangle_xh)
    xm = int(op.triangle_xm)
    xl = int(op.triangle_xl)
    x_long_at_ym = _eval_edge_x_fixed16(xh, op.triangle_dxhdy, yh, ym)
    x_long_at_yl = _eval_edge_x_fixed16(xh, op.triangle_dxhdy, yh, yl)
    x_mid_at_ym = _eval_edge_x_fixed16(xm, op.triangle_dxmdy, yh, ym)
    x_low_at_yl = _eval_edge_x_fixed16(xl, op.triangle_dxldy, ym, yl)

    x_min_fixed = min(xh, xm, xl, x_long_at_ym, x_long_at_yl, x_mid_at_ym, x_low_at_yl)
    x_max_fixed = max(xh, xm, xl, x_long_at_ym, x_long_at_yl, x_mid_at_ym, x_low_at_yl)
    x_min = _clamp_rect_coord(x_min_fixed >> 16)
    x_max = _clamp_rect_coord((x_max_fixed + 0xFFFF) >> 16)

    ulx = min(x_min, x_max)
    uly = min(y_min, y_max)
    lrx = max(x_min, x_max)
    lry = max(y_min, y_max)
    return ulx, uly, lrx, lry


def _pack_rgba8(r: int, g: int, b: int, a: int) -> int:
    return ((r & 0xFF) << 24) | ((g & 0xFF) << 16) | ((b & 0xFF) << 8) | (a & 0xFF)


def _mix_digest(state: int, value: int) -> int:
    s = state & U64_MASK
    v = value & U64_MASK
    return (s ^ ((v + 0x9E3779B97F4A7C15 + ((s << 6) & U64_MASK) + (s >> 2)) & U64_MASK)) & U64_MASK


def _build_key_state_digest(rdp_snapshot: RDPStateSnapshot) -> int:
    digest = FNV_OFFSET
    digest = _mix_digest(digest, rdp_snapshot.key_center_r)
    digest = _mix_digest(digest, rdp_snapshot.key_scale_r)
    digest = _mix_digest(digest, rdp_snapshot.key_center_g)
    digest = _mix_digest(digest, rdp_snapshot.key_scale_g)
    digest = _mix_digest(digest, rdp_snapshot.key_center_b)
    digest = _mix_digest(digest, rdp_snapshot.key_scale_b)
    digest = _mix_digest(digest, rdp_snapshot.key_width_r)
    digest = _mix_digest(digest, rdp_snapshot.key_width_g)
    digest = _mix_digest(digest, rdp_snapshot.key_width_b)
    return digest


def _build_convert_state_digest(rdp_snapshot: RDPStateSnapshot) -> int:
    digest = FNV_OFFSET
    digest = _mix_digest(digest, rdp_snapshot.convert_k0 & 0xFFFF)
    digest = _mix_digest(digest, rdp_snapshot.convert_k1 & 0xFFFF)
    digest = _mix_digest(digest, rdp_snapshot.convert_k2 & 0xFFFF)
    digest = _mix_digest(digest, rdp_snapshot.convert_k3 & 0xFFFF)
    digest = _mix_digest(digest, rdp_snapshot.convert_k4 & 0xFFFF)
    digest = _mix_digest(digest, rdp_snapshot.convert_k5 & 0xFFFF)
    digest = _mix_digest(digest, rdp_snapshot.prim_color_min_level)
    digest = _mix_digest(digest, rdp_snapshot.prim_color_lod_frac)
    return digest


def _build_render_work(
    op: RasterOpRecord,
    rdp_snapshot: RDPStateSnapshot,
    tmem_snapshot: TMEMSnapshot,
    replay_state: RenderPlanReplayState,
) -> RenderWorkRecord:
    tile = tmem_snapshot.tiles[op.tile & 0x7]

    barrier_mask = RENDER_BARRIER_NONE
    load_mask, replay_state.last_load_sync_packet_id = _set_barrier_if_advanced(
        op.load_sync_packet_id,
        replay_state.last_load_sync_packet_id,
        RENDER_BARRIER_LOAD_SYNC,
    )
    barrier_mask |= load_mask
    pipe_mask, replay_state.last_pipe_sync_packet_id = _set_barrier_if_advanced(
        op.pipe_sync_packet_id,
        replay_state.last_pipe_sync_packet_id,
        RENDER_BARRIER_PIPE_SYNC,
    )
    barrier_mask |= pipe_mask
    tile_mask, replay_state.last_tile_sync_packet_id = _set_barrier_if_advanced(
        op.tile_sync_packet_id,
        replay_state.last_tile_sync_packet_id,
        RENDER_BARRIER_TILE_SYNC,
    )
    barrier_mask |= tile_mask
    full_mask, replay_state.last_full_sync_packet_id = _set_barrier_if_advanced(
        op.full_sync_packet_id,
        replay_state.last_full_sync_packet_id,
        RENDER_BARRIER_FULL_SYNC,
    )
    barrier_mask |= full_mask

    rect_ulx = op.rect_ulx
    rect_uly = op.rect_uly
    rect_lrx = op.rect_lrx
    rect_lry = op.rect_lry
    if op.op_kind == 1:
        rect_ulx, rect_uly, rect_lrx, rect_lry = _derive_triangle_rect_bounds(op)

    return RenderWorkRecord(
        source_packet_id=op.source_packet_id,
        source_opcode=op.source_opcode,
        op_kind=op.op_kind,
        phase=_classify_render_phase(op.cycle_type, op.op_kind),
        cycle_type=op.cycle_type,
        barrier_mask=barrier_mask,
        tile=op.tile,
        tex_rect_flip=op.tex_rect_flip,
        textured=op.textured,
        depth_test=op.depth_test,
        depth_compare_enable=rdp_snapshot.other_modes_decoded.depth_compare,
        depth_update_enable=rdp_snapshot.other_modes_decoded.depth_update,
        rect_ulx=rect_ulx,
        rect_uly=rect_uly,
        rect_lrx=rect_lrx,
        rect_lry=rect_lry,
        tex_s=op.tex_s,
        tex_t=op.tex_t,
        tex_dsdx=op.tex_dsdx,
        tex_dtdy=op.tex_dtdy,
        triangle_lmajor=op.triangle_lmajor,
        triangle_level=op.triangle_level,
        triangle_yl=op.triangle_yl,
        triangle_ym=op.triangle_ym,
        triangle_yh=op.triangle_yh,
        triangle_xl=op.triangle_xl,
        triangle_xh=op.triangle_xh,
        triangle_xm=op.triangle_xm,
        triangle_dxldy=op.triangle_dxldy,
        triangle_dxhdy=op.triangle_dxhdy,
        triangle_dxmdy=op.triangle_dxmdy,
        triangle_shade_enable=op.triangle_shade_enable,
        triangle_texture_enable=op.triangle_texture_enable,
        triangle_zbuffer_enable=op.triangle_zbuffer_enable,
        triangle_shade_r=op.triangle_shade_r,
        triangle_shade_g=op.triangle_shade_g,
        triangle_shade_b=op.triangle_shade_b,
        triangle_shade_a=op.triangle_shade_a,
        triangle_shade_drdx=op.triangle_shade_drdx,
        triangle_shade_dgdx=op.triangle_shade_dgdx,
        triangle_shade_dbdx=op.triangle_shade_dbdx,
        triangle_shade_dadx=op.triangle_shade_dadx,
        triangle_shade_drde=op.triangle_shade_drde,
        triangle_shade_dgde=op.triangle_shade_dgde,
        triangle_shade_dbde=op.triangle_shade_dbde,
        triangle_shade_dade=op.triangle_shade_dade,
        triangle_shade_drdy=op.triangle_shade_drdy,
        triangle_shade_dgdy=op.triangle_shade_dgdy,
        triangle_shade_dbdy=op.triangle_shade_dbdy,
        triangle_shade_dady=op.triangle_shade_dady,
        triangle_tex_s=op.triangle_tex_s,
        triangle_tex_t=op.triangle_tex_t,
        triangle_tex_w=op.triangle_tex_w,
        triangle_tex_dsdx=op.triangle_tex_dsdx,
        triangle_tex_dtdx=op.triangle_tex_dtdx,
        triangle_tex_dwdx=op.triangle_tex_dwdx,
        triangle_tex_dsde=op.triangle_tex_dsde,
        triangle_tex_dtde=op.triangle_tex_dtde,
        triangle_tex_dwde=op.triangle_tex_dwde,
        triangle_tex_dsdy=op.triangle_tex_dsdy,
        triangle_tex_dtdy=op.triangle_tex_dtdy,
        triangle_tex_dwdy=op.triangle_tex_dwdy,
        triangle_z=op.triangle_z,
        triangle_dzdx=op.triangle_dzdx,
        triangle_dzde=op.triangle_dzde,
        triangle_dzdy=op.triangle_dzdy,
        color_image_format=rdp_snapshot.color_image_format,
        color_image_size=rdp_snapshot.color_image_size,
        color_image_width=rdp_snapshot.color_image_width,
        color_image_address=rdp_snapshot.color_image_address,
        depth_image_address=rdp_snapshot.depth_image_address,
        alpha_compare=rdp_snapshot.other_modes_decoded.alpha_compare,
        cvg_dest=rdp_snapshot.other_modes_decoded.cvg_dest,
        blend_mask=rdp_snapshot.other_modes_decoded.blend_mask,
        cvg_x_alpha=rdp_snapshot.other_modes_decoded.cvg_x_alpha,
        alpha_cvg_sel=rdp_snapshot.other_modes_decoded.alpha_cvg_sel,
        color_on_cvg=rdp_snapshot.other_modes_decoded.color_on_cvg,
        force_blender=rdp_snapshot.other_modes_decoded.force_blender,
        depth_source=rdp_snapshot.other_modes_decoded.depth_source,
        prim_depth_z=rdp_snapshot.prim_depth_z,
        prim_depth_delta=rdp_snapshot.prim_depth_delta,
        other_modes=rdp_snapshot.other_modes,
        prim_color=_pack_rgba8(
            rdp_snapshot.prim_color.r,
            rdp_snapshot.prim_color.g,
            rdp_snapshot.prim_color.b,
            rdp_snapshot.prim_color.a,
        ),
        env_color=_pack_rgba8(
            rdp_snapshot.env_color.r,
            rdp_snapshot.env_color.g,
            rdp_snapshot.env_color.b,
            rdp_snapshot.env_color.a,
        ),
        blend_color=_pack_rgba8(
            rdp_snapshot.blend_color.r,
            rdp_snapshot.blend_color.g,
            rdp_snapshot.blend_color.b,
            rdp_snapshot.blend_color.a,
        ),
        fog_color=_pack_rgba8(
            rdp_snapshot.fog_color.r,
            rdp_snapshot.fog_color.g,
            rdp_snapshot.fog_color.b,
            rdp_snapshot.fog_color.a,
        ),
        key_state=_build_key_state_digest(rdp_snapshot),
        convert_state=_build_convert_state_digest(rdp_snapshot),
        scissor_mode=rdp_snapshot.scissor_mode,
        scissor_xh=rdp_snapshot.scissor_xh,
        scissor_yh=rdp_snapshot.scissor_yh,
        scissor_xl=rdp_snapshot.scissor_xl,
        scissor_yl=rdp_snapshot.scissor_yl,
        texture_image_format=tmem_snapshot.texture_image_format,
        texture_image_size=tmem_snapshot.texture_image_size,
        texture_image_width=tmem_snapshot.texture_image_width,
        texture_image_address=tmem_snapshot.texture_image_address,
        tile_format=tile.format,
        tile_size=tile.size,
        tile_line=tile.line,
        tile_tmem=tile.tmem,
        tile_palette=tile.palette,
        tile_cmt=tile.cmt,
        tile_cms=tile.cms,
        tile_maskt=tile.maskt,
        tile_masks=tile.masks,
        tile_shiftt=tile.shiftt,
        tile_shifts=tile.shifts,
        tile_uls=tile.uls,
        tile_ult=tile.ult,
        tile_lrs=tile.lrs,
        tile_lrt=tile.lrt,
        tmem_load_kind=tmem_snapshot.last_load.kind,
        tmem_load_tile=tmem_snapshot.last_load.tile,
        tmem_load_uls=tmem_snapshot.last_load.uls,
        tmem_load_ult=tmem_snapshot.last_load.ult,
        tmem_load_lrs=tmem_snapshot.last_load.lrs,
        tmem_load_lrt=tmem_snapshot.last_load.lrt,
        tmem_load_dxt=tmem_snapshot.last_load.dxt,
        combine_mux=op.combine_mux,
        blend_params=op.blend_params,
        fill_color=op.fill_color,
        sync_epoch=op.sync_epoch,
        load_sync_packet_id=op.load_sync_packet_id,
        pipe_sync_packet_id=op.pipe_sync_packet_id,
        tile_sync_packet_id=op.tile_sync_packet_id,
        full_sync_packet_id=op.full_sync_packet_id,
    )


def _hash_render_work(work: RenderWorkRecord) -> int:
    hash_value = FNV_OFFSET
    hash_value = _fnv_update_int(hash_value, work.source_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, work.source_opcode, 1)
    hash_value = _fnv_update_int(hash_value, work.op_kind, 1)
    hash_value = _fnv_update_int(hash_value, work.phase, 1)
    hash_value = _fnv_update_int(hash_value, work.cycle_type, 1)
    hash_value = _fnv_update_int(hash_value, work.barrier_mask, 1)
    hash_value = _fnv_update_int(hash_value, work.tile, 1)
    hash_value = _fnv_update_int(hash_value, 1 if work.tex_rect_flip else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if work.textured else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if work.depth_test else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if work.depth_compare_enable else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if work.depth_update_enable else 0, 1)
    hash_value = _fnv_update_int(hash_value, work.rect_ulx, 2)
    hash_value = _fnv_update_int(hash_value, work.rect_uly, 2)
    hash_value = _fnv_update_int(hash_value, work.rect_lrx, 2)
    hash_value = _fnv_update_int(hash_value, work.rect_lry, 2)
    hash_value = _fnv_update_int(hash_value, work.tex_s & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, work.tex_t & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, work.tex_dsdx & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, work.tex_dtdy & 0xFFFF, 2)
    hash_value = _fnv_update_int(hash_value, 1 if work.triangle_lmajor else 0, 1)
    hash_value = _fnv_update_int(hash_value, work.triangle_level, 1)
    hash_value = _fnv_update_int(hash_value, work.triangle_yl, 2)
    hash_value = _fnv_update_int(hash_value, work.triangle_ym, 2)
    hash_value = _fnv_update_int(hash_value, work.triangle_yh, 2)
    hash_value = _fnv_update_int(hash_value, work.triangle_xl & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_xh & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_xm & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_dxldy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_dxhdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_dxmdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, 1 if work.triangle_shade_enable else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if work.triangle_texture_enable else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if work.triangle_zbuffer_enable else 0, 1)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_r & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_g & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_b & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_a & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_drdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_dgdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_dbdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_dadx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_drde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_dgde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_dbde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_dade & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_drdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_dgdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_dbdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_shade_dady & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_tex_s & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_tex_t & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_tex_w & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_tex_dsdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_tex_dtdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_tex_dwdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_tex_dsde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_tex_dtde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_tex_dwde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_tex_dsdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_tex_dtdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_tex_dwdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_z & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_dzdx & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_dzde & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.triangle_dzdy & 0xFFFFFFFF, 4)
    hash_value = _fnv_update_int(hash_value, work.color_image_format, 1)
    hash_value = _fnv_update_int(hash_value, work.color_image_size, 1)
    hash_value = _fnv_update_int(hash_value, work.color_image_width, 2)
    hash_value = _fnv_update_int(hash_value, work.color_image_address, 4)
    hash_value = _fnv_update_int(hash_value, work.depth_image_address, 4)
    hash_value = _fnv_update_int(hash_value, work.alpha_compare, 1)
    hash_value = _fnv_update_int(hash_value, work.cvg_dest, 1)
    hash_value = _fnv_update_int(hash_value, work.blend_mask, 1)
    hash_value = _fnv_update_int(hash_value, 1 if work.cvg_x_alpha else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if work.alpha_cvg_sel else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if work.color_on_cvg else 0, 1)
    hash_value = _fnv_update_int(hash_value, 1 if work.force_blender else 0, 1)
    hash_value = _fnv_update_int(hash_value, work.depth_source, 1)
    hash_value = _fnv_update_int(hash_value, work.prim_depth_z, 2)
    hash_value = _fnv_update_int(hash_value, work.prim_depth_delta, 2)
    hash_value = _fnv_update_int(hash_value, work.other_modes, 8)
    hash_value = _fnv_update_int(hash_value, work.prim_color, 4)
    hash_value = _fnv_update_int(hash_value, work.env_color, 4)
    hash_value = _fnv_update_int(hash_value, work.blend_color, 4)
    hash_value = _fnv_update_int(hash_value, work.fog_color, 4)
    hash_value = _fnv_update_int(hash_value, work.key_state, 8)
    hash_value = _fnv_update_int(hash_value, work.convert_state, 8)
    hash_value = _fnv_update_int(hash_value, work.scissor_mode, 1)
    hash_value = _fnv_update_int(hash_value, work.scissor_xh, 2)
    hash_value = _fnv_update_int(hash_value, work.scissor_yh, 2)
    hash_value = _fnv_update_int(hash_value, work.scissor_xl, 2)
    hash_value = _fnv_update_int(hash_value, work.scissor_yl, 2)
    hash_value = _fnv_update_int(hash_value, work.texture_image_format, 1)
    hash_value = _fnv_update_int(hash_value, work.texture_image_size, 1)
    hash_value = _fnv_update_int(hash_value, work.texture_image_width, 2)
    hash_value = _fnv_update_int(hash_value, work.texture_image_address, 4)
    hash_value = _fnv_update_int(hash_value, work.tile_format, 1)
    hash_value = _fnv_update_int(hash_value, work.tile_size, 1)
    hash_value = _fnv_update_int(hash_value, work.tile_line, 2)
    hash_value = _fnv_update_int(hash_value, work.tile_tmem, 2)
    hash_value = _fnv_update_int(hash_value, work.tile_palette, 1)
    hash_value = _fnv_update_int(hash_value, work.tile_cmt, 1)
    hash_value = _fnv_update_int(hash_value, work.tile_cms, 1)
    hash_value = _fnv_update_int(hash_value, work.tile_maskt, 1)
    hash_value = _fnv_update_int(hash_value, work.tile_masks, 1)
    hash_value = _fnv_update_int(hash_value, work.tile_shiftt, 1)
    hash_value = _fnv_update_int(hash_value, work.tile_shifts, 1)
    hash_value = _fnv_update_int(hash_value, work.tile_uls, 2)
    hash_value = _fnv_update_int(hash_value, work.tile_ult, 2)
    hash_value = _fnv_update_int(hash_value, work.tile_lrs, 2)
    hash_value = _fnv_update_int(hash_value, work.tile_lrt, 2)
    hash_value = _fnv_update_int(hash_value, work.tmem_load_kind, 1)
    hash_value = _fnv_update_int(hash_value, work.tmem_load_tile, 1)
    hash_value = _fnv_update_int(hash_value, work.tmem_load_uls, 2)
    hash_value = _fnv_update_int(hash_value, work.tmem_load_ult, 2)
    hash_value = _fnv_update_int(hash_value, work.tmem_load_lrs, 2)
    hash_value = _fnv_update_int(hash_value, work.tmem_load_lrt, 2)
    hash_value = _fnv_update_int(hash_value, work.tmem_load_dxt, 2)
    hash_value = _fnv_update_int(hash_value, work.combine_mux, 8)
    hash_value = _fnv_update_int(hash_value, work.blend_params, 4)
    hash_value = _fnv_update_int(hash_value, work.fill_color, 4)
    hash_value = _fnv_update_int(hash_value, work.sync_epoch, 4)
    hash_value = _fnv_update_int(hash_value, work.load_sync_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, work.pipe_sync_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, work.tile_sync_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, work.full_sync_packet_id, 8)
    return hash_value


def _hash_render_work_stream(work_packets: Iterable[RenderWorkRecord]) -> int:
    hash_value = FNV_OFFSET
    for work in work_packets:
        hash_value = _fnv_update_int(hash_value, _hash_render_work(work), 8)
    return hash_value


def _has_same_submission_render_target(batch: SubmissionBatchRecord, work: RenderWorkRecord) -> bool:
    return (
        batch.color_image_format == work.color_image_format
        and batch.color_image_size == work.color_image_size
        and batch.color_image_width == work.color_image_width
        and batch.color_image_address == work.color_image_address
        and batch.depth_image_address == work.depth_image_address
    )


def _has_same_submission_scissor(batch: SubmissionBatchRecord, work: RenderWorkRecord) -> bool:
    return (
        batch.scissor_mode == work.scissor_mode
        and batch.scissor_xh == work.scissor_xh
        and batch.scissor_yh == work.scissor_yh
        and batch.scissor_xl == work.scissor_xl
        and batch.scissor_yl == work.scissor_yl
    )


def _classify_submission_split_reason(batch: SubmissionBatchRecord, work: RenderWorkRecord) -> int:
    if work.barrier_mask != RENDER_BARRIER_NONE:
        return SUBMISSION_SPLIT_BARRIER
    if batch.phase != work.phase:
        return SUBMISSION_SPLIT_PHASE_CHANGE
    if batch.cycle_type != work.cycle_type:
        return SUBMISSION_SPLIT_CYCLE_TYPE_CHANGE
    if not _has_same_submission_render_target(batch, work):
        return SUBMISSION_SPLIT_RENDER_TARGET_CHANGE
    if not _has_same_submission_scissor(batch, work):
        return SUBMISSION_SPLIT_SCISSOR_CHANGE
    return SUBMISSION_SPLIT_BARRIER


def _is_submittable_render_work(work: RenderWorkRecord) -> bool:
    return work.phase != RENDER_PHASE_UNKNOWN


def _can_append_submission_work(batch: SubmissionBatchRecord, work: RenderWorkRecord) -> bool:
    if work.barrier_mask != RENDER_BARRIER_NONE:
        return False
    if batch.phase != work.phase or batch.cycle_type != work.cycle_type:
        return False
    return _has_same_submission_render_target(batch, work) and _has_same_submission_scissor(batch, work)


def _init_submission_batch_from_work(
    batch_index: int, work_index: int, split_reason: int, work: RenderWorkRecord
) -> SubmissionBatchRecord:
    return SubmissionBatchRecord(
        batch_index=batch_index,
        split_reason=split_reason,
        split_barrier_mask=work.barrier_mask,
        phase=work.phase,
        cycle_type=work.cycle_type,
        first_work_index=work_index,
        last_work_index=work_index,
        work_count=1,
        barrier_mask_union=work.barrier_mask,
        textured_work_count=1 if work.textured else 0,
        depth_test_work_count=1 if work.depth_test else 0,
        first_source_packet_id=work.source_packet_id,
        last_source_packet_id=work.source_packet_id,
        color_image_format=work.color_image_format,
        color_image_size=work.color_image_size,
        color_image_width=work.color_image_width,
        color_image_address=work.color_image_address,
        depth_image_address=work.depth_image_address,
        scissor_mode=work.scissor_mode,
        scissor_xh=work.scissor_xh,
        scissor_yh=work.scissor_yh,
        scissor_xl=work.scissor_xl,
        scissor_yl=work.scissor_yl,
    )


def _append_work_to_submission_plan(
    work: RenderWorkRecord, work_index: int, batches: List[SubmissionBatchRecord]
) -> None:
    if not _is_submittable_render_work(work):
        return
    if len(batches) == 0:
        batches.append(
            _init_submission_batch_from_work(
                batch_index=0,
                work_index=work_index,
                split_reason=SUBMISSION_SPLIT_START,
                work=work,
            )
        )
        return

    last = batches[-1]
    if _can_append_submission_work(last, work):
        last.last_work_index = work_index
        last.work_count += 1
        last.barrier_mask_union |= work.barrier_mask
        if work.textured:
            last.textured_work_count += 1
        if work.depth_test:
            last.depth_test_work_count += 1
        last.last_source_packet_id = work.source_packet_id
        return

    batches.append(
        _init_submission_batch_from_work(
            batch_index=len(batches),
            work_index=work_index,
            split_reason=_classify_submission_split_reason(last, work),
            work=work,
        )
    )


def _hash_submission_batch(batch: SubmissionBatchRecord) -> int:
    hash_value = FNV_OFFSET
    hash_value = _fnv_update_int(hash_value, batch.batch_index, 4)
    hash_value = _fnv_update_int(hash_value, batch.split_reason, 1)
    hash_value = _fnv_update_int(hash_value, batch.split_barrier_mask, 1)
    hash_value = _fnv_update_int(hash_value, batch.phase, 1)
    hash_value = _fnv_update_int(hash_value, batch.cycle_type, 1)
    hash_value = _fnv_update_int(hash_value, batch.first_work_index, 4)
    hash_value = _fnv_update_int(hash_value, batch.last_work_index, 4)
    hash_value = _fnv_update_int(hash_value, batch.work_count, 4)
    hash_value = _fnv_update_int(hash_value, batch.barrier_mask_union, 1)
    hash_value = _fnv_update_int(hash_value, batch.textured_work_count, 4)
    hash_value = _fnv_update_int(hash_value, batch.depth_test_work_count, 4)
    hash_value = _fnv_update_int(hash_value, batch.first_source_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, batch.last_source_packet_id, 8)
    hash_value = _fnv_update_int(hash_value, batch.color_image_format, 1)
    hash_value = _fnv_update_int(hash_value, batch.color_image_size, 1)
    hash_value = _fnv_update_int(hash_value, batch.color_image_width, 2)
    hash_value = _fnv_update_int(hash_value, batch.color_image_address, 4)
    hash_value = _fnv_update_int(hash_value, batch.depth_image_address, 4)
    hash_value = _fnv_update_int(hash_value, batch.scissor_mode, 1)
    hash_value = _fnv_update_int(hash_value, batch.scissor_xh, 2)
    hash_value = _fnv_update_int(hash_value, batch.scissor_yh, 2)
    hash_value = _fnv_update_int(hash_value, batch.scissor_xl, 2)
    hash_value = _fnv_update_int(hash_value, batch.scissor_yl, 2)
    return hash_value


def _hash_submission_plan(batches: Iterable[SubmissionBatchRecord]) -> int:
    hash_value = FNV_OFFSET
    for batch in batches:
        hash_value = _fnv_update_int(hash_value, _hash_submission_batch(batch), 8)
    return hash_value


@dataclass
class _ReplayColorSurface:
    format: int
    size: int
    width: int
    height: int
    pixels: List[int]


@dataclass
class _ReplayDepthSurface:
    width: int
    height: int
    values: List[int]


@dataclass
class ExecutorReplaySummary:
    work_count: int
    batch_count: int
    color_write_count: int
    surface_count: int
    present_hash: int
    present_width: int
    present_height: int
    present_aspect_x: int
    present_aspect_y: int


def _decode_fill_color(fill_color: int, color_size: int) -> int:
    if color_size == 3:
        return fill_color & 0xFFFFFFFF
    color16 = fill_color & 0xFFFF
    r5 = (color16 >> 11) & 0x1F
    g5 = (color16 >> 6) & 0x1F
    b5 = (color16 >> 1) & 0x1F
    a1 = color16 & 0x1
    r = (r5 * 255 + 15) // 31
    g = (g5 * 255 + 15) // 31
    b = (b5 * 255 + 15) // 31
    a = 255 if a1 != 0 else 0
    return ((r & 0xFF) << 24) | ((g & 0xFF) << 16) | ((b & 0xFF) << 8) | (a & 0xFF)


def _wrap_coord_positive(value: int, period: int) -> int:
    if period <= 0:
        return 0
    wrapped = value % period
    if wrapped < 0:
        wrapped += period
    return wrapped


def _apply_tile_axis_transform(coord5: int, shift: int, mask: int, cm: int, lo: int, hi: int) -> int:
    texel = int(coord5) >> 5
    effective_shift = min(max(int(shift), 0), 15)
    if effective_shift != 0:
        if effective_shift <= 10:
            texel >>= effective_shift
        else:
            texel <<= 16 - effective_shift

    mirror = (cm & 0x1) != 0
    clamp = (cm & 0x2) != 0
    if mask != 0:
        period = 1 << min(max(int(mask), 0), 15)
        if clamp:
            texel = max(0, min(period - 1, texel))
        elif mirror:
            mirror_period = period << 1
            wrapped = _wrap_coord_positive(texel, mirror_period)
            if wrapped >= period:
                wrapped = (mirror_period - 1) - wrapped
            texel = wrapped
        else:
            texel = _wrap_coord_positive(texel, period)

    lo_texel = int(lo) >> 2
    hi_texel = int(hi) >> 2
    if lo_texel != 0 or hi_texel != 0 or clamp:
        low = min(lo_texel, hi_texel)
        high = max(lo_texel, hi_texel)
        if texel < low:
            texel = low
        if texel > high:
            texel = high
    return texel


def _mix_texture_seed(seed: int, value: int) -> int:
    s = seed & U64_MASK
    v = value & U64_MASK
    mixed = s ^ ((v + 0x9E3779B97F4A7C15 + ((s << 6) & U64_MASK) + (s >> 2)) & U64_MASK)
    return mixed & U64_MASK


def _build_texture_seed_base(work: RenderWorkRecord) -> int:
    seed = FNV_OFFSET
    seed = _mix_texture_seed(seed, work.texture_image_address)
    seed = _mix_texture_seed(seed, work.texture_image_format)
    seed = _mix_texture_seed(seed, work.texture_image_size)
    seed = _mix_texture_seed(seed, work.texture_image_width)
    seed = _mix_texture_seed(seed, work.tile_format)
    seed = _mix_texture_seed(seed, work.tile_size)
    seed = _mix_texture_seed(seed, work.tile_line)
    seed = _mix_texture_seed(seed, work.tile_tmem)
    seed = _mix_texture_seed(seed, work.tile_palette)
    seed = _mix_texture_seed(seed, work.tile_cmt)
    seed = _mix_texture_seed(seed, work.tile_cms)
    seed = _mix_texture_seed(seed, work.tile_maskt)
    seed = _mix_texture_seed(seed, work.tile_masks)
    seed = _mix_texture_seed(seed, work.tile_shiftt)
    seed = _mix_texture_seed(seed, work.tile_shifts)
    seed = _mix_texture_seed(seed, work.tile_uls)
    seed = _mix_texture_seed(seed, work.tile_ult)
    seed = _mix_texture_seed(seed, work.tile_lrs)
    seed = _mix_texture_seed(seed, work.tile_lrt)
    seed = _mix_texture_seed(seed, work.tmem_load_kind)
    seed = _mix_texture_seed(seed, work.tmem_load_tile)
    seed = _mix_texture_seed(seed, work.tmem_load_uls)
    seed = _mix_texture_seed(seed, work.tmem_load_ult)
    seed = _mix_texture_seed(seed, work.tmem_load_lrs)
    seed = _mix_texture_seed(seed, work.tmem_load_lrt)
    seed = _mix_texture_seed(seed, work.tmem_load_dxt)
    return seed & U64_MASK


def _pseudo_texel(work: RenderWorkRecord, x: int, y: int) -> int:
    dx = x - work.rect_ulx
    dy = y - work.rect_uly
    if work.tex_rect_flip:
        s_raw = work.tex_s + ((dy * work.tex_dsdx) >> 5)
        t_raw = work.tex_t + ((dx * work.tex_dtdy) >> 5)
    else:
        s_raw = work.tex_s + ((dx * work.tex_dsdx) >> 5)
        t_raw = work.tex_t + ((dy * work.tex_dtdy) >> 5)
    s = _apply_tile_axis_transform(
        s_raw, work.tile_shifts, work.tile_masks, work.tile_cms, work.tile_uls, work.tile_lrs
    )
    t = _apply_tile_axis_transform(
        t_raw, work.tile_shiftt, work.tile_maskt, work.tile_cmt, work.tile_ult, work.tile_lrt
    )

    seed = _build_texture_seed_base(work)
    seed = _mix_texture_seed(seed, s & 0xFFFFFFFF)
    seed = _mix_texture_seed(seed, t & 0xFFFFFFFF)
    seed = _mix_texture_seed(seed, x & 0xFFFFFFFF)
    seed = _mix_texture_seed(seed, y & 0xFFFFFFFF)
    seed = _mix_texture_seed(seed, work.combine_mux & U64_MASK)
    seed = _mix_texture_seed(seed, work.sync_epoch & 0xFFFFFFFF)
    seed = (seed * 0x9E3779B97F4A7C15) & U64_MASK
    r = (seed >> 8) & 0xFF
    g = (seed >> 24) & 0xFF
    b = (seed >> 40) & 0xFF
    return ((r & 0xFF) << 24) | ((g & 0xFF) << 16) | ((b & 0xFF) << 8) | 0xFF


def _pseudo_triangle_color(work: RenderWorkRecord, x: int, y: int) -> int:
    seed = work.combine_mux & U64_MASK
    seed ^= (work.blend_params & 0xFFFFFFFF) << 29
    seed ^= (work.source_packet_id & U64_MASK) << 7
    seed ^= (x & 0xFFFFFFFF) << 33
    seed ^= (y & 0xFFFFFFFF) << 45
    seed ^= work.sync_epoch & 0xFFFFFFFF
    seed = (seed * 0x9E3779B97F4A7C15) & U64_MASK
    r = (seed >> 9) & 0xFF
    g = (seed >> 27) & 0xFF
    b = (seed >> 41) & 0xFF
    return ((r & 0xFF) << 24) | ((g & 0xFF) << 16) | ((b & 0xFF) << 8) | 0xFF


def _surface_index(width: int, x: int, y: int) -> int:
    return y * width + x


def _ensure_surface_size(surface: _ReplayColorSurface, req_width: int, req_height: int, max_width: int, max_height: int) -> None:
    new_width = max(1, min(req_width, max_width))
    new_height = max(1, min(req_height, max_height))
    if new_width <= surface.width and new_height <= surface.height:
        return
    target_width = max(surface.width, new_width)
    target_height = max(surface.height, new_height)
    resized = [0 for _ in range(target_width * target_height)]
    for y in range(surface.height):
        row_src = y * surface.width
        row_dst = y * target_width
        resized[row_dst : row_dst + surface.width] = surface.pixels[row_src : row_src + surface.width]
    surface.width = target_width
    surface.height = target_height
    surface.pixels = resized


def _ensure_depth_surface_size(
    surface: _ReplayDepthSurface, req_width: int, req_height: int, max_width: int, max_height: int
) -> None:
    new_width = max(1, min(req_width, max_width))
    new_height = max(1, min(req_height, max_height))
    if new_width <= surface.width and new_height <= surface.height:
        return
    target_width = max(surface.width, new_width)
    target_height = max(surface.height, new_height)
    resized = [0x7FFFFFFF for _ in range(target_width * target_height)]
    for y in range(surface.height):
        row_src = y * surface.width
        row_dst = y * target_width
        resized[row_dst : row_dst + surface.width] = surface.values[row_src : row_src + surface.width]
    surface.width = target_width
    surface.height = target_height
    surface.values = resized


def _write_render_work_rect(
    surface: _ReplayColorSurface,
    work: RenderWorkRecord,
    summary: ExecutorReplaySummary,
    max_width: int,
    max_height: int,
) -> None:
    ulx = min(work.rect_ulx, work.rect_lrx)
    uly = min(work.rect_uly, work.rect_lry)
    lrx = max(work.rect_ulx, work.rect_lrx)
    lry = max(work.rect_uly, work.rect_lry)
    if lrx < ulx or lry < uly:
        return

    _ensure_surface_size(surface, lrx + 1, lry + 1, max_width, max_height)
    scissor_x0 = min(work.scissor_xh, work.scissor_xl)
    scissor_y0 = min(work.scissor_yh, work.scissor_yl)
    scissor_x1 = max(work.scissor_xh, work.scissor_xl)
    scissor_y1 = max(work.scissor_yh, work.scissor_yl)
    default_scissor = (
        work.scissor_xh == 0
        and work.scissor_yh == 0
        and work.scissor_xl == 0
        and work.scissor_yl == 0
    )
    clip_x0 = ulx if default_scissor else scissor_x0
    clip_y0 = uly if default_scissor else scissor_y0
    clip_x1 = lrx if default_scissor else scissor_x1
    clip_y1 = lry if default_scissor else scissor_y1

    write_x0 = max(ulx, clip_x0)
    write_y0 = max(uly, clip_y0)
    write_x1 = min(lrx, min(clip_x1, surface.width - 1 if surface.width > 0 else 0))
    write_y1 = min(lry, min(clip_y1, surface.height - 1 if surface.height > 0 else 0))
    if write_x1 < write_x0 or write_y1 < write_y0:
        return

    is_fill = work.op_kind == 3
    fill_rgba = _decode_fill_color(work.fill_color, work.color_image_size)
    pixels = surface.pixels
    width = surface.width
    color_write_count = 0

    if is_fill:
        for y in range(write_y0, write_y1 + 1):
            row_index = y * width + write_x0
            for x in range(write_x0, write_x1 + 1):
                dst_color = pixels[row_index] & 0xFFFFFFFF
                if (
                    _passes_synthetic_alpha_compare(work, fill_rgba, x, y)
                    and _passes_synthetic_coverage_write(work, fill_rgba, dst_color, x, y)
                ):
                    pixels[row_index] = fill_rgba
                    color_write_count += 1
                row_index += 1
        summary.color_write_count += color_write_count
        return

    for y in range(write_y0, write_y1 + 1):
        row_index = y * width + write_x0
        for x in range(write_x0, write_x1 + 1):
            dst_color = pixels[row_index] & 0xFFFFFFFF
            texel_color = _pseudo_texel(work, x, y)
            rgba = _run_synthetic_phase_pipeline(
                work,
                texel_color,
                0xFFFFFFFF,
                texel_color,
                dst_color,
                x,
                y,
            ) & 0xFFFFFFFF
            if (
                _passes_synthetic_alpha_compare(work, rgba, x, y)
                and _passes_synthetic_coverage_write(work, rgba, dst_color, x, y)
            ):
                pixels[row_index] = rgba
                color_write_count += 1
            row_index += 1

    summary.color_write_count += color_write_count


def _combine_dy_derivative(dy: int, de: int, lmajor: bool) -> int:
    return (dy + de) if lmajor else (dy - de)


def _eval_coefficient_at_pixel(base: int, dx: int, dy: int, x: int, y: int) -> int:
    value = int(base) + int(dx) * int(x) + int(dy) * int(y)
    if value < -0x80000000:
        return -0x80000000
    if value > 0x7FFFFFFF:
        return 0x7FFFFFFF
    return value


def _coefficient_component_byte(value: int) -> int:
    return (value >> 8) & 0xFF


def _evaluate_triangle_shade_color(work: RenderWorkRecord, x: int, y: int) -> int:
    drdy = _combine_dy_derivative(work.triangle_shade_drdy, work.triangle_shade_drde, work.triangle_lmajor)
    dgdy = _combine_dy_derivative(work.triangle_shade_dgdy, work.triangle_shade_dgde, work.triangle_lmajor)
    dbdy = _combine_dy_derivative(work.triangle_shade_dbdy, work.triangle_shade_dbde, work.triangle_lmajor)
    dady = _combine_dy_derivative(work.triangle_shade_dady, work.triangle_shade_dade, work.triangle_lmajor)
    r = _coefficient_component_byte(_eval_coefficient_at_pixel(work.triangle_shade_r, work.triangle_shade_drdx, drdy, x, y))
    g = _coefficient_component_byte(_eval_coefficient_at_pixel(work.triangle_shade_g, work.triangle_shade_dgdx, dgdy, x, y))
    b = _coefficient_component_byte(_eval_coefficient_at_pixel(work.triangle_shade_b, work.triangle_shade_dbdx, dbdy, x, y))
    a = _coefficient_component_byte(_eval_coefficient_at_pixel(work.triangle_shade_a, work.triangle_shade_dadx, dady, x, y))
    return ((r & 0xFF) << 24) | ((g & 0xFF) << 16) | ((b & 0xFF) << 8) | (a & 0xFF)


def _evaluate_triangle_texture_color(work: RenderWorkRecord, x: int, y: int) -> int:
    dsdy = _combine_dy_derivative(work.triangle_tex_dsdy, work.triangle_tex_dsde, work.triangle_lmajor)
    dtdy = _combine_dy_derivative(work.triangle_tex_dtdy, work.triangle_tex_dtde, work.triangle_lmajor)
    dwdy = _combine_dy_derivative(work.triangle_tex_dwdy, work.triangle_tex_dwde, work.triangle_lmajor)
    s_raw = _eval_coefficient_at_pixel(work.triangle_tex_s, work.triangle_tex_dsdx, dsdy, x, y)
    t_raw = _eval_coefficient_at_pixel(work.triangle_tex_t, work.triangle_tex_dtdx, dtdy, x, y)
    w = _eval_coefficient_at_pixel(work.triangle_tex_w, work.triangle_tex_dwdx, dwdy, x, y)
    s = _apply_tile_axis_transform(
        s_raw, work.tile_shifts, work.tile_masks, work.tile_cms, work.tile_uls, work.tile_lrs
    )
    t = _apply_tile_axis_transform(
        t_raw, work.tile_shiftt, work.tile_maskt, work.tile_cmt, work.tile_ult, work.tile_lrt
    )

    seed = _build_texture_seed_base(work)
    seed = _mix_texture_seed(seed, s & 0xFFFFFFFF)
    seed = _mix_texture_seed(seed, t & 0xFFFFFFFF)
    seed = _mix_texture_seed(seed, w & 0xFFFFFFFF)
    seed = _mix_texture_seed(seed, x & 0xFFFFFFFF)
    seed = _mix_texture_seed(seed, y & 0xFFFFFFFF)
    seed = _mix_texture_seed(seed, work.combine_mux & U64_MASK)
    seed = _mix_texture_seed(seed, work.sync_epoch & 0xFFFFFFFF)
    seed = (seed * 0x9E3779B97F4A7C15) & U64_MASK
    r = (seed >> 8) & 0xFF
    g = (seed >> 24) & 0xFF
    b = (seed >> 40) & 0xFF
    return ((r & 0xFF) << 24) | ((g & 0xFF) << 16) | ((b & 0xFF) << 8) | 0xFF


def _modulate_rgba(base: int, shade: int) -> int:
    br = (base >> 24) & 0xFF
    bg = (base >> 16) & 0xFF
    bb = (base >> 8) & 0xFF
    ba = base & 0xFF
    sr = (shade >> 24) & 0xFF
    sg = (shade >> 16) & 0xFF
    sb = (shade >> 8) & 0xFF
    sa = shade & 0xFF
    r = (br * sr + 127) // 255
    g = (bg * sg + 127) // 255
    b = (bb * sb + 127) // 255
    a = (ba * sa + 127) // 255
    return ((r & 0xFF) << 24) | ((g & 0xFF) << 16) | ((b & 0xFF) << 8) | (a & 0xFF)


def _unpack_rgba(color: int) -> tuple[int, int, int, int]:
    return (
        (color >> 24) & 0xFF,
        (color >> 16) & 0xFF,
        (color >> 8) & 0xFF,
        color & 0xFF,
    )


def _pack_rgba(r: int, g: int, b: int, a: int) -> int:
    return ((r & 0xFF) << 24) | ((g & 0xFF) << 16) | ((b & 0xFF) << 8) | (a & 0xFF)


def _clamp_u8_from_s32(value: int) -> int:
    if value < 0:
        return 0
    if value > 255:
        return 255
    return value


def _div_trunc(value: int, divisor: int) -> int:
    return int(value / divisor)


def _select_combiner_input(selector: int, tex: int, shade: int, base: int, const: int, noise: int, dst: int) -> int:
    sel = selector & 0x7
    if sel == 0:
        return tex
    if sel == 1:
        return shade
    if sel == 2:
        return base
    if sel == 3:
        return const
    if sel == 4:
        return 255 - tex
    if sel == 5:
        return 255 - shade
    if sel == 6:
        return dst
    return noise


def _eval_synthetic_combiner_channel(
    combine_mux: int,
    selector_shift: int,
    tex: int,
    shade: int,
    base: int,
    const: int,
    noise: int,
    dst: int,
) -> int:
    a_sel = (combine_mux >> selector_shift) & 0x7
    b_sel = (combine_mux >> (selector_shift + 3)) & 0x7
    c_sel = (combine_mux >> (selector_shift + 6)) & 0x7
    d_sel = (combine_mux >> (selector_shift + 9)) & 0x7
    a = _select_combiner_input(a_sel, tex, shade, base, const, noise, dst)
    b = _select_combiner_input(b_sel, tex, shade, base, const, noise, dst)
    c = _select_combiner_input(c_sel, tex, shade, base, const, noise, dst)
    d = _select_combiner_input(d_sel, tex, shade, base, const, noise, dst)
    value = _div_trunc(((a - b) * c + 127), 255) + d
    return _clamp_u8_from_s32(value)


def _apply_synthetic_combiner(
    work: RenderWorkRecord,
    texture_color: int,
    shade_color: int,
    base_color: int,
    dst_color: int,
    x: int,
    y: int,
    combine_mux: Optional[int] = None,
    source_packet_id: Optional[int] = None,
    sync_epoch: Optional[int] = None,
) -> int:
    active_combine_mux = (work.combine_mux if combine_mux is None else combine_mux) & U64_MASK
    active_source_packet_id = (work.source_packet_id if source_packet_id is None else source_packet_id) & U64_MASK
    active_sync_epoch = (work.sync_epoch if sync_epoch is None else sync_epoch) & 0xFFFFFFFF

    tex_r, tex_g, tex_b, tex_a = _unpack_rgba(texture_color)
    shade_r, shade_g, shade_b, shade_a = _unpack_rgba(shade_color)
    base_r, base_g, base_b, base_a = _unpack_rgba(base_color)
    dst_r, dst_g, dst_b, dst_a = _unpack_rgba(dst_color)
    prim_r, prim_g, prim_b, prim_a = _unpack_rgba(work.prim_color)
    env_r, env_g, env_b, env_a = _unpack_rgba(work.env_color)
    blend_r, blend_g, blend_b, blend_a = _unpack_rgba(work.blend_color)
    fog_r, fog_g, fog_b, fog_a = _unpack_rgba(work.fog_color)

    noise_seed = active_combine_mux
    noise_seed ^= active_source_packet_id << 9
    noise_seed ^= (x & 0xFFFFFFFF) << 33
    noise_seed ^= (y & 0xFFFFFFFF) << 45
    noise_seed ^= active_sync_epoch << 17
    noise_seed ^= work.other_modes & U64_MASK
    noise_seed ^= work.key_state & U64_MASK
    noise_seed ^= work.convert_state & U64_MASK
    noise_seed ^= (work.prim_color & 0xFFFFFFFF) << 5
    noise_seed ^= (work.env_color & 0xFFFFFFFF) << 11
    noise_seed ^= (work.blend_color & 0xFFFFFFFF) << 19
    noise_seed ^= (work.fog_color & 0xFFFFFFFF) << 27
    noise_seed = (noise_seed * 0xD6E8FEB86659FD93) & U64_MASK

    mux_const_r = (active_combine_mux >> 56) & 0xFF
    mux_const_g = (active_combine_mux >> 48) & 0xFF
    mux_const_b = (active_combine_mux >> 40) & 0xFF
    mux_const_a = (active_combine_mux >> 32) & 0xFF

    def select_state_color(selector: int, channel: int) -> int:
        s = selector & 0x3
        if s == 0:
            table = (prim_r, prim_g, prim_b, prim_a)
        elif s == 1:
            table = (env_r, env_g, env_b, env_a)
        elif s == 2:
            table = (blend_r, blend_g, blend_b, blend_a)
        else:
            table = (fog_r, fog_g, fog_b, fog_a)
        return table[channel & 0x3]

    mode_selector_word = (work.other_modes ^ work.key_state ^ ((work.convert_state << 7) & U64_MASK)) & U64_MASK
    mode_r = (mode_selector_word >> 0) & 0x3
    mode_g = (mode_selector_word >> 2) & 0x3
    mode_b = (mode_selector_word >> 4) & 0x3
    mode_a = (mode_selector_word >> 6) & 0x3
    mix_r = (work.convert_state >> 0) & 0xFF
    mix_g = (work.convert_state >> 8) & 0xFF
    mix_b = (work.convert_state >> 16) & 0xFF
    mix_a = (work.convert_state >> 24) & 0xFF

    def blend_const(base: int, state: int, mix: int) -> int:
        inv_mix = 255 - (mix & 0xFF)
        value = base * inv_mix + state * (mix & 0xFF)
        return (value + 127) // 255

    const_r = blend_const(mux_const_r, select_state_color(mode_r, 0), mix_r)
    const_g = blend_const(mux_const_g, select_state_color(mode_g, 1), mix_g)
    const_b = blend_const(mux_const_b, select_state_color(mode_b, 2), mix_b)
    const_a = blend_const(mux_const_a, select_state_color(mode_a, 3), mix_a)
    noise_r = (noise_seed >> 8) & 0xFF
    noise_g = (noise_seed >> 24) & 0xFF
    noise_b = (noise_seed >> 40) & 0xFF
    noise_a = 0xFF

    out_r = _eval_synthetic_combiner_channel(
        active_combine_mux, 0, tex_r, shade_r, base_r, const_r, noise_r, dst_r
    )
    out_g = _eval_synthetic_combiner_channel(
        active_combine_mux, 12, tex_g, shade_g, base_g, const_g, noise_g, dst_g
    )
    out_b = _eval_synthetic_combiner_channel(
        active_combine_mux, 24, tex_b, shade_b, base_b, const_b, noise_b, dst_b
    )
    out_a = _eval_synthetic_combiner_channel(
        active_combine_mux, 36, tex_a, shade_a, base_a, const_a, noise_a, dst_a
    )
    return _pack_rgba(out_r, out_g, out_b, out_a)


def _blend_channel(src: int, dst: int, src_weight: int, dst_weight: int) -> int:
    total = src_weight + dst_weight
    if total == 0:
        return src
    blended = src * src_weight + dst * dst_weight
    return (blended + (total // 2)) // total


def _alpha_to_coverage3(alpha: int) -> int:
    return (alpha & 0xFF) >> 5


def _resolve_coverage_destination(input_coverage: int, destination_coverage: int, cvg_dest: int) -> int:
    mode = cvg_dest & 0x3
    if mode == 0:
        return min(7, (input_coverage + destination_coverage))
    if mode == 1:
        return (input_coverage + destination_coverage) & 0x7
    if mode == 2:
        return 7
    return destination_coverage


def _mix_coverage_seed(seed: int, value: int) -> int:
    return (
        seed
        ^ (
            (value & U64_MASK)
            + 0x9E3779B97F4A7C15
            + ((seed << 6) & U64_MASK)
            + (seed >> 2)
        )
    ) & U64_MASK


def _evaluate_synthetic_coverage(
    work: RenderWorkRecord, coverage_alpha: int, dst_alpha: int, x: int, y: int
) -> tuple[int, int, int]:
    seed = FNV_OFFSET
    seed = _mix_coverage_seed(seed, x)
    seed = _mix_coverage_seed(seed, y)
    seed = _mix_coverage_seed(seed, work.source_packet_id)
    seed = _mix_coverage_seed(seed, work.sync_epoch)
    seed = _mix_coverage_seed(seed, work.key_state)
    seed = _mix_coverage_seed(seed, work.blend_mask)
    stochastic_coverage = (seed >> 5) & 0x7

    destination_coverage = _alpha_to_coverage3(dst_alpha)
    input_coverage = stochastic_coverage
    if work.alpha_cvg_sel:
        input_coverage = _alpha_to_coverage3(coverage_alpha)
    if work.cvg_x_alpha:
        alpha_coverage = _alpha_to_coverage3(coverage_alpha)
        input_coverage = (input_coverage * alpha_coverage + 3) // 7

    input_coverage = min(7, input_coverage)
    resolved_coverage = _resolve_coverage_destination(
        input_coverage, destination_coverage, work.cvg_dest
    )
    return input_coverage, destination_coverage, resolved_coverage


def _apply_synthetic_blender(
    work: RenderWorkRecord,
    src_color: int,
    dst_color: int,
    x: int,
    y: int,
    blend_params: Optional[int] = None,
) -> int:
    active_blend_params = (work.blend_params if blend_params is None else blend_params) & 0xFFFFFFFF
    src_r, src_g, src_b, src_a = _unpack_rgba(src_color)
    dst_r, dst_g, dst_b, dst_a = _unpack_rgba(dst_color)
    blend_r, blend_g, blend_b, blend_a = _unpack_rgba(work.blend_color)
    fog_r, fog_g, fog_b, fog_a = _unpack_rgba(work.fog_color)
    prim_r, prim_g, prim_b, prim_a = _unpack_rgba(work.prim_color)

    src_weight = active_blend_params & 0xFF
    dst_weight = (active_blend_params >> 8) & 0xFF
    alpha_scale = ((active_blend_params >> 16) & 0xFF) + 1
    coverage_bias = (active_blend_params >> 24) & 0xFF
    mode_lo = work.other_modes & 0xFFFFFFFF
    src_weight += mode_lo & 0xF
    dst_weight += (mode_lo >> 4) & 0xF
    mode_alpha_scale = ((mode_lo >> 8) & 0x1F) + 1
    modulated_alpha_scale = min(alpha_scale * mode_alpha_scale, 1024)
    dynamic_coverage_bias = coverage_bias ^ (work.key_state & 0xFF)

    tint_mix = (work.convert_state >> 16) & 0xFF
    fog_mix = (work.convert_state >> 24) & 0x7F

    def mix_channel(base: int, target: int, mix: int) -> int:
        inv_mix = 255 - (mix & 0xFF)
        value = base * inv_mix + target * (mix & 0xFF)
        return (value + 127) // 255

    src_r = mix_channel(src_r, blend_r, tint_mix)
    src_g = mix_channel(src_g, blend_g, tint_mix)
    src_b = mix_channel(src_b, blend_b, tint_mix)
    src_a = mix_channel(src_a, prim_a, tint_mix)
    src_r = mix_channel(src_r, fog_r, fog_mix)
    src_g = mix_channel(src_g, fog_g, fog_mix)
    src_b = mix_channel(src_b, fog_b, fog_mix)

    use_coverage_controls = (
        work.color_on_cvg
        or work.cvg_x_alpha
        or work.alpha_cvg_sel
        or work.force_blender
        or work.cvg_dest != 0
        or work.blend_mask != 0
    )
    resolved_coverage = 0
    if use_coverage_controls:
        _, _, resolved_coverage = _evaluate_synthetic_coverage(work, src_a, dst_a, x, y)
        if work.cvg_x_alpha:
            coverage_alpha_scale = (resolved_coverage * 255 + 3) // 7
            src_a = (src_a * coverage_alpha_scale + 127) // 255

    if src_weight == 0 and dst_weight == 0:
        src_weight = 255

    src_alpha = src_a + 1
    src_weight = (src_weight * src_alpha * modulated_alpha_scale + 32767) // (256 * 256)
    dst_weight = (dst_weight * (256 - src_alpha) + 127) // 256

    if use_coverage_controls:
        src_weight += resolved_coverage * 16
        dst_weight += (7 - resolved_coverage) * 8
        src_weight += work.blend_mask & 0x3
        dst_weight += (work.blend_mask >> 2) & 0x3
        if work.force_blender:
            src_weight += 8
            dst_weight += 8

    if (active_blend_params & 0x80000000) != 0:
        coverage = ((x * 29) + (y * 17) + dynamic_coverage_bias) & 0xFF
        src_weight += coverage >> 4
        dst_weight += (255 - coverage) >> 4

    if src_weight == 0 and dst_weight == 0:
        src_weight = 1

    out_r = _blend_channel(src_r, dst_r, src_weight, dst_weight)
    out_g = _blend_channel(src_g, dst_g, src_weight, dst_weight)
    out_b = _blend_channel(src_b, dst_b, src_weight, dst_weight)
    out_a = _blend_channel(src_a, dst_a, src_weight, dst_weight)
    if use_coverage_controls and work.alpha_cvg_sel:
        out_a = (resolved_coverage * 255 + 3) // 7
    return _pack_rgba(out_r, out_g, out_b, out_a)


def _rotate_left_u64(value: int, shift: int) -> int:
    s = shift & 63
    if s == 0:
        return value & U64_MASK
    v = value & U64_MASK
    return ((v << s) | (v >> (64 - s))) & U64_MASK


def _rotate_left_u32(value: int, shift: int) -> int:
    s = shift & 31
    if s == 0:
        return value & 0xFFFFFFFF
    v = value & 0xFFFFFFFF
    return ((v << s) | (v >> (32 - s))) & 0xFFFFFFFF


def _evaluate_triangle_depth(work: RenderWorkRecord, x: int, y: int) -> int:
    if work.depth_source != 0:
        depth = (work.prim_depth_z << 8) + work.prim_depth_delta * (x + y)
        if depth < -0x80000000:
            return -0x80000000
        if depth > 0x7FFFFFFF:
            return 0x7FFFFFFF
        return depth
    dzdy = _combine_dy_derivative(work.triangle_dzdy, work.triangle_dzde, work.triangle_lmajor)
    return _eval_coefficient_at_pixel(work.triangle_z, work.triangle_dzdx, dzdy, x, y)


def _choose_triangle_texture_source_color(work: RenderWorkRecord, x: int, y: int) -> int:
    use_triangle_texture = work.textured and work.triangle_texture_enable
    if use_triangle_texture:
        return _evaluate_triangle_texture_color(work, x, y)
    if work.textured:
        return _pseudo_texel(work, x, y)
    return _pseudo_triangle_color(work, x, y)


def _choose_triangle_shade_source_color(work: RenderWorkRecord, x: int, y: int) -> int:
    return _evaluate_triangle_shade_color(work, x, y) if work.triangle_shade_enable else 0xFFFFFFFF


def _choose_triangle_base_color(work: RenderWorkRecord, x: int, y: int) -> int:
    use_triangle_shade = work.triangle_shade_enable
    textured_color = _choose_triangle_texture_source_color(work, x, y)
    if (not work.textured) and use_triangle_shade:
        return _evaluate_triangle_shade_color(work, x, y)
    if not use_triangle_shade:
        return textured_color
    return _modulate_rgba(textured_color, _evaluate_triangle_shade_color(work, x, y))


def _run_synthetic_cycle1_pipeline(
    work: RenderWorkRecord,
    texture_color: int,
    shade_color: int,
    base_color: int,
    dst_color: int,
    x: int,
    y: int,
) -> int:
    combined_color = _apply_synthetic_combiner(
        work, texture_color, shade_color, base_color, dst_color, x, y
    )
    return _apply_synthetic_blender(work, combined_color, dst_color, x, y)


def _run_synthetic_phase_pipeline(
    work: RenderWorkRecord,
    texture_color: int,
    shade_color: int,
    base_color: int,
    dst_color: int,
    x: int,
    y: int,
) -> int:
    if work.phase == RENDER_PHASE_COPY:
        return texture_color if work.textured else base_color
    if work.phase == RENDER_PHASE_FILL:
        return base_color
    if work.phase != RENDER_PHASE_CYCLE2:
        return _run_synthetic_cycle1_pipeline(work, texture_color, shade_color, base_color, dst_color, x, y)

    cycle1_color = _run_synthetic_cycle1_pipeline(work, texture_color, shade_color, base_color, dst_color, x, y)
    stage2_combine_mux = _rotate_left_u64((work.combine_mux ^ 0xA5A5A5A55A5A5A5A) & U64_MASK, 11)
    stage2_source_packet_id = (work.source_packet_id ^ 0x9E3779B97F4A7C15) & U64_MASK
    stage2_sync_epoch = (work.sync_epoch ^ 0x00A5A5A5) & 0xFFFFFFFF
    stage2_blend_params = _rotate_left_u32((work.blend_params ^ 0x5A5AA5A5) & 0xFFFFFFFF, 7)
    stage2_combined_color = _apply_synthetic_combiner(
        work,
        cycle1_color,
        shade_color,
        cycle1_color,
        cycle1_color,
        x,
        y,
        combine_mux=stage2_combine_mux,
        source_packet_id=stage2_source_packet_id,
        sync_epoch=stage2_sync_epoch,
    )
    return _apply_synthetic_blender(
        work,
        stage2_combined_color,
        cycle1_color,
        x,
        y,
        blend_params=stage2_blend_params,
    )


def _phase_uses_depth(phase: int) -> bool:
    return phase in (RENDER_PHASE_CYCLE1, RENDER_PHASE_CYCLE2)


def _passes_synthetic_alpha_compare(work: RenderWorkRecord, pixel: int, x: int, y: int) -> bool:
    if work.alpha_compare == 0:
        return True

    alpha = pixel & 0xFF
    if work.alpha_compare == 1:
        threshold = work.blend_color & 0xFF
        return alpha >= threshold
    if work.alpha_compare == 2:
        threshold = (
            x * 17
            + y * 29
            + (work.sync_epoch & 0xFF)
            + (work.key_state & 0xFF)
        ) & 0xFF
        return alpha >= threshold
    return alpha != 0


def _passes_synthetic_coverage_write(
    work: RenderWorkRecord, pixel: int, dst_color: int, x: int, y: int
) -> bool:
    if not work.color_on_cvg:
        return True
    _, _, resolved_coverage = _evaluate_synthetic_coverage(
        work, pixel & 0xFF, dst_color & 0xFF, x, y
    )
    return resolved_coverage != 0


def _write_render_work_triangle(
    surface: _ReplayColorSurface,
    depth_surface: Optional[_ReplayDepthSurface],
    work: RenderWorkRecord,
    summary: ExecutorReplaySummary,
    max_width: int,
    max_height: int,
) -> None:
    ulx = min(work.rect_ulx, work.rect_lrx)
    uly = min(work.rect_uly, work.rect_lry)
    lrx = max(work.rect_ulx, work.rect_lrx)
    lry = max(work.rect_uly, work.rect_lry)
    if lrx < ulx or lry < uly:
        return

    _ensure_surface_size(surface, lrx + 1, lry + 1, max_width, max_height)
    if depth_surface is not None:
        _ensure_depth_surface_size(depth_surface, lrx + 1, lry + 1, max_width, max_height)
    scissor_x0 = min(work.scissor_xh, work.scissor_xl)
    scissor_y0 = min(work.scissor_yh, work.scissor_yl)
    scissor_x1 = max(work.scissor_xh, work.scissor_xl)
    scissor_y1 = max(work.scissor_yh, work.scissor_yl)
    default_scissor = (
        work.scissor_xh == 0
        and work.scissor_yh == 0
        and work.scissor_xl == 0
        and work.scissor_yl == 0
    )
    clip_x0 = ulx if default_scissor else scissor_x0
    clip_y0 = uly if default_scissor else scissor_y0
    clip_x1 = lrx if default_scissor else scissor_x1
    clip_y1 = lry if default_scissor else scissor_y1

    write_x0 = max(ulx, clip_x0)
    write_y0 = max(uly, clip_y0)
    write_x1 = min(lrx, min(clip_x1, surface.width - 1 if surface.width > 0 else 0))
    write_y1 = min(lry, min(clip_y1, surface.height - 1 if surface.height > 0 else 0))
    if write_x1 < write_x0 or write_y1 < write_y0:
        return

    yh = float(work.triangle_yh) * 0.25
    ym = float(work.triangle_ym) * 0.25
    yl = float(work.triangle_yl) * 0.25
    xh = float(work.triangle_xh) / 65536.0
    xl = float(work.triangle_xl) / 65536.0
    x_long_at_yl = (
        float(work.triangle_xh)
        + float(work.triangle_dxhdy) * float(int(work.triangle_yl) - int(work.triangle_yh))
    ) / 65536.0

    ax = xh
    ay = yh
    bx = xl
    by = ym
    cx = x_long_at_yl
    cy = yl
    area = (cx - ax) * (by - ay) - (cy - ay) * (bx - ax)
    if area == 0.0:
        return

    pixels = surface.pixels
    width = surface.width
    color_write_count = 0
    area_positive = area > 0.0

    abx = bx - ax
    aby = by - ay
    bcx = cx - bx
    bcy = cy - by
    cax = ax - cx
    cay = ay - cy

    for y in range(write_y0, write_y1 + 1):
        py = float(y) + 0.5
        row_index = y * width + write_x0
        for x in range(write_x0, write_x1 + 1):
            px = float(x) + 0.5
            e0 = (px - ax) * aby - (py - ay) * abx
            e1 = (px - bx) * bcy - (py - by) * bcx
            e2 = (px - cx) * cay - (py - cy) * cax
            if area_positive:
                inside = e0 >= 0.0 and e1 >= 0.0 and e2 >= 0.0
            else:
                inside = e0 <= 0.0 and e1 <= 0.0 and e2 <= 0.0
            if not inside:
                row_index += 1
                continue

            dst_color = pixels[row_index] & 0xFFFFFFFF
            texture_color = _choose_triangle_texture_source_color(work, x, y)
            shade_color = _choose_triangle_shade_source_color(work, x, y)
            base_color = _choose_triangle_base_color(work, x, y)
            rgba = _run_synthetic_phase_pipeline(
                work,
                texture_color,
                shade_color,
                base_color,
                dst_color,
                x,
                y,
            ) & 0xFFFFFFFF
            if not _passes_synthetic_alpha_compare(work, rgba, x, y):
                row_index += 1
                continue
            if not _passes_synthetic_coverage_write(work, rgba, dst_color, x, y):
                row_index += 1
                continue

            if (
                depth_surface is not None
                and _phase_uses_depth(work.phase)
                and work.depth_test
                and work.triangle_zbuffer_enable
                and (work.depth_compare_enable or work.depth_update_enable)
            ):
                z = _evaluate_triangle_depth(work, x, y)
                depth_index = _surface_index(depth_surface.width, x, y)
                if depth_index >= len(depth_surface.values):
                    row_index += 1
                    continue
                depth_value = depth_surface.values[depth_index]
                if work.depth_compare_enable and z > depth_value:
                    row_index += 1
                    continue
                if work.depth_update_enable:
                    depth_surface.values[depth_index] = z

            pixels[row_index] = rgba
            row_index += 1
            color_write_count += 1

    summary.color_write_count += color_write_count


def _hash_presented_surface(
    surface: _ReplayColorSurface,
    aspect_x: int,
    aspect_y: int,
    max_width: int,
    max_height: int,
) -> tuple[int, int, int]:
    if surface.width <= 0 or surface.height <= 0 or len(surface.pixels) == 0:
        return FNV_OFFSET, 0, 0

    output_width = surface.width
    output_height = surface.height
    source_scaled = surface.width * aspect_y
    target_scaled = surface.height * aspect_x
    if source_scaled > target_scaled:
        output_height = (surface.width * aspect_y + aspect_x - 1) // aspect_x
    elif source_scaled < target_scaled:
        output_width = (surface.height * aspect_x + aspect_y - 1) // aspect_y

    output_width = max(1, min(output_width, max_width))
    output_height = max(1, min(output_height, max_height))

    content_width = output_width
    content_height = output_height
    content_source_scaled = surface.width * output_height
    content_output_scaled = surface.height * output_width
    if content_source_scaled > content_output_scaled:
        content_height = max(1, min(output_height, (output_width * surface.height) // surface.width))
    elif content_source_scaled < content_output_scaled:
        content_width = max(1, min(output_width, (output_height * surface.width) // surface.height))

    content_x = (output_width - content_width) // 2
    content_y = (output_height - content_height) // 2

    hash_value = FNV_OFFSET
    for y in range(output_height):
        for x in range(output_width):
            pixel = 0
            if (
                x >= content_x
                and x < content_x + content_width
                and y >= content_y
                and y < content_y + content_height
            ):
                content_local_x = x - content_x
                content_local_y = y - content_y
                source_x = min(surface.width - 1, (content_local_x * surface.width) // content_width)
                source_y = min(surface.height - 1, (content_local_y * surface.height) // content_height)
                pixel = surface.pixels[_surface_index(surface.width, source_x, source_y)]
            hash_value = _fnv_update_int(hash_value, (pixel >> 0) & 0xFF, 1)
            hash_value = _fnv_update_int(hash_value, (pixel >> 8) & 0xFF, 1)
            hash_value = _fnv_update_int(hash_value, (pixel >> 16) & 0xFF, 1)
            hash_value = _fnv_update_int(hash_value, (pixel >> 24) & 0xFF, 1)
    return hash_value, output_width, output_height


def _execute_submission_plan(
    work_packets: List[RenderWorkRecord],
    batches: List[SubmissionBatchRecord],
    aspect_x: int,
    aspect_y: int,
    max_width: int = 2048,
    max_height: int = 2048,
) -> ExecutorReplaySummary:
    summary = ExecutorReplaySummary(
        work_count=0,
        batch_count=0,
        color_write_count=0,
        surface_count=0,
        present_hash=FNV_OFFSET,
        present_width=0,
        present_height=0,
        present_aspect_x=aspect_x,
        present_aspect_y=aspect_y,
    )
    surfaces: dict[int, _ReplayColorSurface] = {}
    depth_surfaces: dict[int, _ReplayDepthSurface] = {}
    last_surface_address = 0

    for batch in batches:
        summary.batch_count += 1
        if batch.first_work_index >= len(work_packets):
            continue
        last_index = min(batch.last_work_index, len(work_packets) - 1)
        for work_index in range(batch.first_work_index, last_index + 1):
            work = work_packets[work_index]
            summary.work_count += 1
            if work.op_kind not in (1, 2, 3):
                continue
            surface = surfaces.get(work.color_image_address)
            if surface is None:
                width = max(1, min(work.color_image_width, max_width))
                surface = _ReplayColorSurface(
                    format=work.color_image_format,
                    size=work.color_image_size,
                    width=width,
                    height=1,
                    pixels=[0 for _ in range(width)],
                )
                surfaces[work.color_image_address] = surface
            else:
                surface.format = work.color_image_format
                surface.size = work.color_image_size
            if work.op_kind == 1:
                depth_surface: Optional[_ReplayDepthSurface] = None
                if _phase_uses_depth(work.phase) and work.depth_test and work.triangle_zbuffer_enable:
                    depth_address = work.depth_image_address if work.depth_image_address != 0 else work.color_image_address
                    depth_surface = depth_surfaces.get(depth_address)
                    if depth_surface is None:
                        width = max(1, min(work.color_image_width, max_width))
                        depth_surface = _ReplayDepthSurface(
                            width=width,
                            height=1,
                            values=[0x7FFFFFFF for _ in range(width)],
                        )
                        depth_surfaces[depth_address] = depth_surface
                _write_render_work_triangle(surface, depth_surface, work, summary, max_width, max_height)
            else:
                _write_render_work_rect(surface, work, summary, max_width, max_height)
            last_surface_address = work.color_image_address

    summary.surface_count = len(surfaces)
    if last_surface_address in surfaces:
        summary.present_hash, summary.present_width, summary.present_height = _hash_presented_surface(
            surfaces[last_surface_address], aspect_x, aspect_y, max_width, max_height
        )
    return summary


def _hash_combined_state(rdp_snapshot: RDPStateSnapshot, tmem_snapshot: TMEMSnapshot) -> int:
    hash_value = FNV_OFFSET
    hash_value = _fnv_update_int(hash_value, _hash_rdp_state(rdp_snapshot), 8)
    hash_value = _fnv_update_int(hash_value, _hash_tmem_state(tmem_snapshot), 8)
    return hash_value


def replay_frame(frame: FrameRecord) -> FrameCheck:
    command_hash = _hash_command_stream(frame.packets)
    rdp_snapshot = RDPStateSnapshot()
    tmem_snapshot = TMEMSnapshot()
    computed_semantics: List[DrawSemanticRecord] = []
    computed_raster_ops: List[RasterOpRecord] = []
    computed_render_work: List[RenderWorkRecord] = []
    computed_submission_batches: List[SubmissionBatchRecord] = []
    render_plan_state = RenderPlanReplayState()
    unknown_rdp_opcode_count = 0
    first_unknown_rdp_packet_id = 0
    first_unknown_rdp_opcode = 0
    truncated_payload_count = 0
    first_truncated_payload_packet_id = 0
    first_truncated_payload_opcode = 0

    for packet in frame.packets:
        if packet.domain == COMMAND_DOMAIN_RDP and not _is_known_rdp_opcode(packet.opcode):
            unknown_rdp_opcode_count += 1
            if first_unknown_rdp_packet_id == 0:
                first_unknown_rdp_packet_id = packet.packet_id
                first_unknown_rdp_opcode = packet.opcode
        if packet.full_word_count > (2 + _packet_payload_count(packet)):
            truncated_payload_count += 1
            if first_truncated_payload_packet_id == 0:
                first_truncated_payload_packet_id = packet.packet_id
                first_truncated_payload_opcode = packet.opcode
        _apply_rdp_packet(rdp_snapshot, packet)
        _apply_tmem_packet(tmem_snapshot, packet)
        if _is_draw_opcode(packet.opcode):
            semantic = _build_draw_semantic(packet, rdp_snapshot, tmem_snapshot)
            computed_semantics.append(semantic)
            raster_op = _build_raster_op(semantic, rdp_snapshot)
            computed_raster_ops.append(raster_op)
            if _is_renderable_raster_op(raster_op):
                work = _build_render_work(raster_op, rdp_snapshot, tmem_snapshot, render_plan_state)
                computed_render_work.append(work)
                _append_work_to_submission_plan(work, len(computed_render_work) - 1, computed_submission_batches)

    state_hash = _hash_combined_state(rdp_snapshot, tmem_snapshot)
    draw_semantic_hash = _hash_draw_semantics(computed_semantics)
    raster_op_hash = _hash_raster_ops(computed_raster_ops)
    render_work_hash = _hash_render_work_stream(computed_render_work)
    submission_batch_hash = _hash_submission_plan(computed_submission_batches)
    exec_aspect_x = frame.executor_present_aspect_x if frame.executor_present_aspect_x > 0 else 4
    exec_aspect_y = frame.executor_present_aspect_y if frame.executor_present_aspect_y > 0 else 3
    executor_summary = _execute_submission_plan(
        computed_render_work,
        computed_submission_batches,
        exec_aspect_x,
        exec_aspect_y,
    )
    check = FrameCheck(
        frame_id=frame.frame_id,
        header_line_no=frame.line_no,
        declared_command_count=frame.command_count,
        parsed_command_count=len(frame.packets),
        declared_last_packet_id=frame.last_packet_id,
        computed_last_packet_id=rdp_snapshot.last_packet_id,
        declared_command_hash=frame.command_hash,
        computed_command_hash=command_hash,
        declared_state_hash=frame.state_hash,
        computed_state_hash=state_hash,
        declared_draw_semantic_count=frame.draw_semantic_count,
        computed_draw_semantic_count=len(computed_semantics),
        declared_draw_semantic_hash=frame.draw_semantic_hash,
        computed_draw_semantic_hash=draw_semantic_hash,
        declared_raster_op_count=frame.raster_op_count,
        computed_raster_op_count=len(computed_raster_ops),
        declared_raster_op_hash=frame.raster_op_hash,
        computed_raster_op_hash=raster_op_hash,
        declared_render_work_count=frame.render_work_count,
        computed_render_work_count=len(computed_render_work),
        declared_render_work_hash=frame.render_work_hash,
        computed_render_work_hash=render_work_hash,
        declared_submission_batch_count=frame.submission_batch_count,
        computed_submission_batch_count=len(computed_submission_batches),
        declared_submission_batch_hash=frame.submission_batch_hash,
        computed_submission_batch_hash=submission_batch_hash,
        declared_executor_work_count=frame.executor_work_count,
        computed_executor_work_count=executor_summary.work_count,
        declared_executor_batch_count=frame.executor_batch_count,
        computed_executor_batch_count=executor_summary.batch_count,
        declared_executor_color_write_count=frame.executor_color_write_count,
        computed_executor_color_write_count=executor_summary.color_write_count,
        declared_executor_surface_count=frame.executor_surface_count,
        computed_executor_surface_count=executor_summary.surface_count,
        declared_executor_present_hash=frame.executor_present_hash,
        computed_executor_present_hash=executor_summary.present_hash,
        declared_executor_present_width=frame.executor_present_width,
        computed_executor_present_width=executor_summary.present_width,
        declared_executor_present_height=frame.executor_present_height,
        computed_executor_present_height=executor_summary.present_height,
        declared_executor_present_aspect_x=frame.executor_present_aspect_x,
        computed_executor_present_aspect_x=executor_summary.present_aspect_x,
        declared_executor_present_aspect_y=frame.executor_present_aspect_y,
        computed_executor_present_aspect_y=executor_summary.present_aspect_y,
        declared_unknown_rdp_opcode_count=frame.unknown_rdp_opcode_count,
        computed_unknown_rdp_opcode_count=unknown_rdp_opcode_count,
        declared_first_unknown_rdp_packet_id=frame.first_unknown_rdp_packet_id,
        computed_first_unknown_rdp_packet_id=first_unknown_rdp_packet_id,
        declared_first_unknown_rdp_opcode=frame.first_unknown_rdp_opcode,
        computed_first_unknown_rdp_opcode=first_unknown_rdp_opcode,
        declared_truncated_payload_count=frame.truncated_payload_count,
        computed_truncated_payload_count=truncated_payload_count,
        declared_first_truncated_payload_packet_id=frame.first_truncated_payload_packet_id,
        computed_first_truncated_payload_packet_id=first_truncated_payload_packet_id,
        declared_first_truncated_payload_opcode=frame.first_truncated_payload_opcode,
        computed_first_truncated_payload_opcode=first_truncated_payload_opcode,
    )

    if frame.command_count != len(frame.packets):
        check.errors.append(
            f"command_count mismatch: declared={frame.command_count} parsed={len(frame.packets)}"
        )
    if frame.last_packet_id != rdp_snapshot.last_packet_id:
        check.errors.append(
            f"last_packet_id mismatch: declared={frame.last_packet_id} computed={rdp_snapshot.last_packet_id}"
        )
    if frame.command_hash != command_hash:
        check.errors.append(
            f"command_hash mismatch: declared={frame.command_hash} computed={command_hash}"
        )
    if frame.state_hash != state_hash:
        check.errors.append(f"state_hash mismatch: declared={frame.state_hash} computed={state_hash}")
    if frame.draw_semantic_count >= 0:
        if frame.draw_semantic_count != len(computed_semantics):
            check.errors.append(
                f"draw_semantic_count mismatch: declared={frame.draw_semantic_count} computed={len(computed_semantics)}"
            )
        if frame.draw_semantic_hash != draw_semantic_hash:
            check.errors.append(
                f"draw_semantic_hash mismatch: declared={frame.draw_semantic_hash} computed={draw_semantic_hash}"
            )
    if frame.raster_op_count >= 0:
        if frame.raster_op_count != len(computed_raster_ops):
            check.errors.append(
                f"raster_op_count mismatch: declared={frame.raster_op_count} computed={len(computed_raster_ops)}"
            )
        if frame.raster_op_hash != raster_op_hash:
            check.errors.append(
                f"raster_op_hash mismatch: declared={frame.raster_op_hash} computed={raster_op_hash}"
            )
    if frame.render_work_count >= 0:
        if frame.render_work_count != len(computed_render_work):
            check.errors.append(
                f"render_work_count mismatch: declared={frame.render_work_count} computed={len(computed_render_work)}"
            )
        if frame.render_work_hash != render_work_hash:
            check.errors.append(
                f"render_work_hash mismatch: declared={frame.render_work_hash} computed={render_work_hash}"
            )
    if frame.submission_batch_count >= 0:
        if frame.submission_batch_count != len(computed_submission_batches):
            check.errors.append(
                f"submission_batch_count mismatch: declared={frame.submission_batch_count} computed={len(computed_submission_batches)}"
            )
        if frame.submission_batch_hash != submission_batch_hash:
            check.errors.append(
                f"submission_batch_hash mismatch: declared={frame.submission_batch_hash} computed={submission_batch_hash}"
            )
    if frame.executor_work_count >= 0 and frame.executor_work_count != executor_summary.work_count:
        check.errors.append(
            f"executor_work_count mismatch: declared={frame.executor_work_count} computed={executor_summary.work_count}"
        )
    if frame.executor_batch_count >= 0 and frame.executor_batch_count != executor_summary.batch_count:
        check.errors.append(
            f"executor_batch_count mismatch: declared={frame.executor_batch_count} computed={executor_summary.batch_count}"
        )
    if frame.executor_color_write_count >= 0 and frame.executor_color_write_count != executor_summary.color_write_count:
        check.errors.append(
            "executor_color_write_count mismatch: "
            f"declared={frame.executor_color_write_count} computed={executor_summary.color_write_count}"
        )
    if frame.executor_surface_count >= 0 and frame.executor_surface_count != executor_summary.surface_count:
        check.errors.append(
            f"executor_surface_count mismatch: declared={frame.executor_surface_count} computed={executor_summary.surface_count}"
        )
    if frame.executor_present_hash >= 0 and frame.executor_present_hash != executor_summary.present_hash:
        check.errors.append(
            f"executor_present_hash mismatch: declared={frame.executor_present_hash} computed={executor_summary.present_hash}"
        )
    if frame.executor_present_width >= 0 and frame.executor_present_width != executor_summary.present_width:
        check.errors.append(
            f"executor_present_width mismatch: declared={frame.executor_present_width} computed={executor_summary.present_width}"
        )
    if frame.executor_present_height >= 0 and frame.executor_present_height != executor_summary.present_height:
        check.errors.append(
            f"executor_present_height mismatch: declared={frame.executor_present_height} computed={executor_summary.present_height}"
        )
    if frame.executor_present_aspect_x >= 0 and frame.executor_present_aspect_x != executor_summary.present_aspect_x:
        check.errors.append(
            f"executor_present_aspect_x mismatch: declared={frame.executor_present_aspect_x} computed={executor_summary.present_aspect_x}"
        )
    if frame.executor_present_aspect_y >= 0 and frame.executor_present_aspect_y != executor_summary.present_aspect_y:
        check.errors.append(
            f"executor_present_aspect_y mismatch: declared={frame.executor_present_aspect_y} computed={executor_summary.present_aspect_y}"
        )
    if len(frame.semantics) > 0 and (not frame.legacy_semantic_rows) and frame.semantics != computed_semantics:
        check.errors.append(
            f"semantic row mismatch: declared_rows={len(frame.semantics)} computed_rows={len(computed_semantics)}"
        )
    if len(frame.raster_ops) > 0 and (not frame.legacy_raster_rows) and frame.raster_ops != computed_raster_ops:
        check.errors.append(
            f"raster row mismatch: declared_rows={len(frame.raster_ops)} computed_rows={len(computed_raster_ops)}"
        )
    if len(frame.render_work) > 0 and (not frame.legacy_render_work_rows) and frame.render_work != computed_render_work:
        check.errors.append(
            f"render-work row mismatch: declared_rows={len(frame.render_work)} computed_rows={len(computed_render_work)}"
        )
    if len(frame.submission_batches) > 0 and (not frame.legacy_submission_rows) and frame.submission_batches != computed_submission_batches:
        check.errors.append(
            f"submission-batch row mismatch: declared_rows={len(frame.submission_batches)} computed_rows={len(computed_submission_batches)}"
        )
    if frame.unknown_rdp_opcode_count >= 0:
        if frame.unknown_rdp_opcode_count != unknown_rdp_opcode_count:
            check.errors.append(
                "unknown_rdp_opcode_count mismatch: "
                f"declared={frame.unknown_rdp_opcode_count} computed={unknown_rdp_opcode_count}"
            )
        if frame.first_unknown_rdp_packet_id != first_unknown_rdp_packet_id:
            check.errors.append(
                "first_unknown_rdp_packet_id mismatch: "
                f"declared={frame.first_unknown_rdp_packet_id} computed={first_unknown_rdp_packet_id}"
            )
        if frame.first_unknown_rdp_opcode != first_unknown_rdp_opcode:
            check.errors.append(
                "first_unknown_rdp_opcode mismatch: "
                f"declared={frame.first_unknown_rdp_opcode} computed={first_unknown_rdp_opcode}"
            )
    if frame.truncated_payload_count >= 0:
        if frame.truncated_payload_count != truncated_payload_count:
            check.errors.append(
                "truncated_payload_count mismatch: "
                f"declared={frame.truncated_payload_count} computed={truncated_payload_count}"
            )
        if frame.first_truncated_payload_packet_id != first_truncated_payload_packet_id:
            check.errors.append(
                "first_truncated_payload_packet_id mismatch: "
                f"declared={frame.first_truncated_payload_packet_id} computed={first_truncated_payload_packet_id}"
            )
        if frame.first_truncated_payload_opcode != first_truncated_payload_opcode:
            check.errors.append(
                "first_truncated_payload_opcode mismatch: "
                f"declared={frame.first_truncated_payload_opcode} computed={first_truncated_payload_opcode}"
            )

    if frame.frame_id <= 0xFFFFFFFF:
        expected_task_id = frame.frame_id & 0xFFFFFFFF
        task_mismatch_count = sum(
            1 for packet in frame.packets if packet.task_id != expected_task_id
        )
        if task_mismatch_count > 0:
            check.warnings.append(
                f"{task_mismatch_count} packet(s) have task_id != frame_id_low32 ({expected_task_id})"
            )

    packet_microcode_mismatch_count = sum(
        1 for packet in frame.packets if packet.microcode != frame.microcode_type
    )
    if packet_microcode_mismatch_count > 0:
        check.warnings.append(
            f"{packet_microcode_mismatch_count} packet(s) have provenance microcode != frame microcode ({frame.microcode_type})"
        )

    frame_column_microcode_mismatch_count = sum(
        1 for packet in frame.packets if packet.frame_microcode_type != frame.microcode_type
    )
    if frame_column_microcode_mismatch_count > 0:
        check.warnings.append(
            f"{frame_column_microcode_mismatch_count} packet row(s) have frame_microcode_type column != frame microcode ({frame.microcode_type})"
        )

    return check


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Replay and validate REALITYVK2 packet trace dumps "
            "(REALITYVK2_PACKET_TRACE_FILE format)."
        )
    )
    parser.add_argument("--input", required=True, help="Path to packet trace file.")
    parser.add_argument(
        "--frame-id",
        action="append",
        type=int,
        default=[],
        help="Replay only specific frame id(s). May be passed multiple times.",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Treat warnings as failures.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print per-frame status, including passing frames.",
    )
    parser.add_argument(
        "--max-report",
        type=int,
        default=25,
        help="Maximum frame failures/warnings to print in detail.",
    )
    parser.add_argument(
        "--json-out",
        default="",
        help="Optional path to write JSON report.",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=0,
        help=(
            "Replay worker process count. "
            "0 (default) uses all detected CPU cores; 1 forces single-threaded replay."
        ),
    )
    return parser.parse_args()


def _check_to_json(check: FrameCheck) -> dict:
    return {
        "frame_id": check.frame_id,
        "header_line_no": check.header_line_no,
        "declared_command_count": check.declared_command_count,
        "parsed_command_count": check.parsed_command_count,
        "declared_last_packet_id": check.declared_last_packet_id,
        "computed_last_packet_id": check.computed_last_packet_id,
        "declared_command_hash": check.declared_command_hash,
        "computed_command_hash": check.computed_command_hash,
        "declared_state_hash": check.declared_state_hash,
        "computed_state_hash": check.computed_state_hash,
        "declared_draw_semantic_count": check.declared_draw_semantic_count,
        "computed_draw_semantic_count": check.computed_draw_semantic_count,
        "declared_draw_semantic_hash": check.declared_draw_semantic_hash,
        "computed_draw_semantic_hash": check.computed_draw_semantic_hash,
        "declared_raster_op_count": check.declared_raster_op_count,
        "computed_raster_op_count": check.computed_raster_op_count,
        "declared_raster_op_hash": check.declared_raster_op_hash,
        "computed_raster_op_hash": check.computed_raster_op_hash,
        "declared_render_work_count": check.declared_render_work_count,
        "computed_render_work_count": check.computed_render_work_count,
        "declared_render_work_hash": check.declared_render_work_hash,
        "computed_render_work_hash": check.computed_render_work_hash,
        "declared_submission_batch_count": check.declared_submission_batch_count,
        "computed_submission_batch_count": check.computed_submission_batch_count,
        "declared_submission_batch_hash": check.declared_submission_batch_hash,
        "computed_submission_batch_hash": check.computed_submission_batch_hash,
        "declared_executor_work_count": check.declared_executor_work_count,
        "computed_executor_work_count": check.computed_executor_work_count,
        "declared_executor_batch_count": check.declared_executor_batch_count,
        "computed_executor_batch_count": check.computed_executor_batch_count,
        "declared_executor_color_write_count": check.declared_executor_color_write_count,
        "computed_executor_color_write_count": check.computed_executor_color_write_count,
        "declared_executor_surface_count": check.declared_executor_surface_count,
        "computed_executor_surface_count": check.computed_executor_surface_count,
        "declared_executor_present_hash": check.declared_executor_present_hash,
        "computed_executor_present_hash": check.computed_executor_present_hash,
        "declared_executor_present_width": check.declared_executor_present_width,
        "computed_executor_present_width": check.computed_executor_present_width,
        "declared_executor_present_height": check.declared_executor_present_height,
        "computed_executor_present_height": check.computed_executor_present_height,
        "declared_executor_present_aspect_x": check.declared_executor_present_aspect_x,
        "computed_executor_present_aspect_x": check.computed_executor_present_aspect_x,
        "declared_executor_present_aspect_y": check.declared_executor_present_aspect_y,
        "computed_executor_present_aspect_y": check.computed_executor_present_aspect_y,
        "declared_unknown_rdp_opcode_count": check.declared_unknown_rdp_opcode_count,
        "computed_unknown_rdp_opcode_count": check.computed_unknown_rdp_opcode_count,
        "declared_first_unknown_rdp_packet_id": check.declared_first_unknown_rdp_packet_id,
        "computed_first_unknown_rdp_packet_id": check.computed_first_unknown_rdp_packet_id,
        "declared_first_unknown_rdp_opcode": check.declared_first_unknown_rdp_opcode,
        "computed_first_unknown_rdp_opcode": check.computed_first_unknown_rdp_opcode,
        "declared_truncated_payload_count": check.declared_truncated_payload_count,
        "computed_truncated_payload_count": check.computed_truncated_payload_count,
        "declared_first_truncated_payload_packet_id": check.declared_first_truncated_payload_packet_id,
        "computed_first_truncated_payload_packet_id": check.computed_first_truncated_payload_packet_id,
        "declared_first_truncated_payload_opcode": check.declared_first_truncated_payload_opcode,
        "computed_first_truncated_payload_opcode": check.computed_first_truncated_payload_opcode,
        "errors": check.errors,
        "warnings": check.warnings,
        "ok": check.ok,
    }


def main() -> int:
    args = _parse_args()
    input_path = Path(args.input)
    if not input_path.is_file():
        print(f"ERROR: packet trace file not found: {input_path}", file=sys.stderr)
        return 2

    try:
        frames = parse_packet_trace(input_path)
    except TraceParseError as err:
        print(f"ERROR: {err}", file=sys.stderr)
        return 2

    selected_ids = set(args.frame_id)
    if selected_ids:
        frames = [frame for frame in frames if frame.frame_id in selected_ids]
        if not frames:
            print("ERROR: no frames matched requested --frame-id filters", file=sys.stderr)
            return 2

    jobs = args.jobs
    if jobs <= 0:
        jobs = os.cpu_count() or 1
    jobs = max(1, jobs)

    if jobs == 1 or len(frames) <= 1:
        checks = [replay_frame(frame) for frame in frames]
    else:
        chunksize = max(1, len(frames) // (jobs * 4))
        with ProcessPoolExecutor(max_workers=jobs) as executor:
            checks = list(executor.map(replay_frame, frames, chunksize=chunksize))

    failed = [check for check in checks if not check.ok]
    warned = [check for check in checks if check.ok and len(check.warnings) > 0]

    report_limit = max(0, args.max_report)
    emitted = 0
    for check in checks:
        should_print = args.verbose or (not check.ok) or (len(check.warnings) > 0)
        if not should_print:
            continue
        if emitted >= report_limit:
            break

        status = "PASS"
        if not check.ok:
            status = "FAIL"
        elif len(check.warnings) > 0:
            status = "WARN"

        print(
            f"[{status}] frame={check.frame_id} "
            f"line={check.header_line_no} "
            f"packets={check.parsed_command_count} "
            f"cmd_hash=0x{check.computed_command_hash:016x} "
            f"state_hash=0x{check.computed_state_hash:016x}"
        )
        for error in check.errors:
            print(f"  error: {error}")
        for warning in check.warnings:
            print(f"  warn: {warning}")
        emitted += 1

    if emitted >= report_limit and len(checks) > report_limit:
        print(f"... truncated report at {report_limit} frame rows")

    summary = {
        "input": str(input_path),
        "frame_count": len(checks),
        "failed_count": len(failed),
        "warning_count": len(warned),
        "strict_mode": bool(args.strict),
        "all_ok": len(failed) == 0 and (not args.strict or len(warned) == 0),
        "frames": [_check_to_json(check) for check in checks],
    }

    if args.json_out:
        json_path = Path(args.json_out)
        json_path.parent.mkdir(parents=True, exist_ok=True)
        json_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print(
        f"Summary: frames={len(checks)} "
        f"failed={len(failed)} "
        f"warned={len(warned)} "
        f"strict={int(bool(args.strict))}"
    )

    if len(failed) > 0:
        return 1
    if args.strict and len(warned) > 0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
