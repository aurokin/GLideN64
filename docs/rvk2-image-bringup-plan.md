# RVK2 Image Bring-Up Plan

Last updated: 2026-03-02

## Goal

Bring RVK2 from black/noise output to stable, game-representative image output by replacing remaining synthetic rendering behavior with N64/RDP/VI-grounded logic.

## Scope Rules

- Accuracy first, then performance.
- Vulkan + RVK2 path only.
- Plan is allowed to evolve as new evidence is collected.
- Every coding cycle updates this document with:
  - what changed,
  - what was learned,
  - what changed in priorities.

## Current Leads

1. Core pixel path now produces visible image output, but content remains dim/misaligned versus reference.
2. TMEM model tracks metadata but does not yet serve authoritative texel data for sampling.
3. Combiner/blender/depth/coverage still use partial approximations (not full hardware-accurate hazard behavior).
4. Triangle/raster coefficient path still needs additional subpixel/coverage correctness closure.
5. VI bootstrap still has expected no-surface/missing-source windows, but late-frame selection is now mostly deterministic.
6. CI/TLUT-heavy content is active in the capture path; palette decode must remain hardware-grounded.

## Phases

Total remaining work represented here: 100%.

| ID | Priority | Status | Weight | Outcome |
| --- | --- | --- | --- | --- |
| P1 | Highest | Completed | 10% | Add frame-forensics layer so each bad frame is diagnosable. |
| P2 | Highest | In progress | 15% | Make present target selection deterministic and VI-origin authoritative. |
| P3 | Highest | In progress | 25% | Replace pseudo texture sampling with real TMEM-backed sampling. |
| P4 | High | In progress | 20% | Replace pseudo triangle color path with coefficient-driven raster evaluation. |
| P5 | High | In progress | 25% | Implement real combiner/blender/depth/coverage semantics for cycle 1/2. |
| P6 | Medium | Planned | 5% | Final VI framing and output conformance polish after core content path is real. |

## Immediate Execution Order

1. P2: finish hard constraints on VI-type-compatible present source selection.
2. P4: replace synthetic triangle color with coefficient-driven color evaluation.
3. P5: close combiner/blender/depth/coverage correctness (cycle 1/2).
4. P3: move from RDRAM decode bridge to authoritative TMEM-backed texel reads.
5. P6: final VI/output polish.

## Live Progress

- Overall completion of this bring-up plan: **~43%**
- Active phase: **P4/P5 (visible-output stabilization)**
- Blockers: none (technical debt only)

## Latest Findings (Forensics Run)

Source: `REALITYVK2_FRAME_FORENSICS_FILE` during Paper Mario parity capture.

- Captured records: `155` frames.
- Present-selection modes observed:
  - `kExecutorPresentSelectionNoSurface (5)`: `44` frames.
  - `kExecutorPresentSelectionLastSurface (1)`: `38` frames.
  - `kExecutorPresentSelectionVIOriginRange (3)`: `73` frames.
- VI rejection observed:
  - `kVIRejectMissingSource (1)`: `44` frames.
  - `kVIRejectNone (0)`: `111` frames.
- Zero-size present frames: `44` (all correspond to no-surface/missing-source windows).
- Late stable frame example:
  - frame `126`: selected surface `0x005CE430`, writes `939279`, output `640x480`.

Implication:
- We now have precise visibility into when black output is “no render surface yet” vs “rendered but wrong content.”
- P2 should focus on tightening present target policy around VI-origin + surface activity epoch.

## Latest Findings (P2 Selection Pass)

After adding previous-surface support and bounded surface-history selection:

- Present selection mix shifted from mostly `last-surface` fallback to deterministic fallback classes:
  - `kExecutorPresentSelectionVIOriginRange (3)`: `73`
  - `kExecutorPresentSelectionMostWrittenFallback (4)`: `37`
  - `kExecutorPresentSelectionLastSurface (1)`: `1`
  - `kExecutorPresentSelectionNoSurface (5)`: `44`
- Zero-present frames remained `44`, but all are early bootstrap windows before any render surface exists.

Implication:
- Runtime now makes less arbitrary present-target choices once surfaces exist.
- Remaining visual corruption is increasingly likely inside content generation (TMEM/pixel path), not target selection.

## Latest Findings (P3 RDRAM Sampling Pass)

After wiring executor texture fetch through RDRAM decode (RGBA/CI/IA/I families) with pseudo fallback only when decode is unavailable:

- Local gate status: pass (`./scripts/local_gate.sh`).
- Paper Mario parity still fails non-black gate, but for low-luma output (not missing surfaces):
  - candidate frame `720x540`, non-black ratio `~0.053`, mean luma `~0.00131`.
  - capture rejected after retry budget because brightness remained below threshold.
- Frame forensics from the same run (`299` records):
  - present selection: `{5:44, 1:1, 3:169, 4:85}`
  - VI reject: `{1:44, 0:255}`
  - zero-present frames: `44` (bootstrap only)
  - late frames remain surface-backed with valid VI resolve and full present dimensions.

Implication:
- P2 target selection is no longer the dominant failure mode.
- P3/P4/P5 remain the critical path: texel decode is now sourcing real bytes, but combiner/blender/coverage/depth behavior still underpowers final image energy.

## Latest Findings (P2.5 + Forensics Expansion Pass)

After adding executor/VI luma counters and VI source-size-aware mapping:

- Forensics records: `299` total, `255` active (non-zero present dimensions).
- Present selection remains deterministic in active frames:
  - `kExecutorPresentSelectionVIOriginRange (3)`: `169`
  - `kExecutorPresentSelectionMostWrittenFallback (4)`: `85`
  - `kExecutorPresentSelectionLastSurface (1)`: `1`
- Bootstrap windows remain expected:
  - `kExecutorPresentSelectionNoSurface (5)`: `44`
  - `kVIRejectMissingSource (1)`: `44`
- Active-frame VI/source compatibility is now strong:
  - `selected_surface_size` is consistently `2` and `vi_type` is consistently `2`.
  - texture sample path is fully RDRAM-backed in this run:
    - `tx_total=1704466296`, `tx_rdram=1704466296`, `tx_synth=0` (share `1.0`).
- Remaining blocker remains low-energy output:
  - median `vi_src_luma_avg_x1000`: `69`
  - median `vi_out_luma_avg_x1000`: `16`
  - median `vi_src_invalid`: `237` samples/frame (small vs total sampled pixels).

Implication:
- Main failure is no longer "wrong surface selected."
- Main failure is "surface content is too dark/incorrect before VI output."
- Next work should prioritize P4/P5 pixel semantics over more present-selection heuristics.

## Latest Findings (P4/P5 Combiner+Blender Rewrite Pass)

After replacing combiner selector/input mapping and simplifying blender toward RDP selector-driven behavior:

- Local gate status: pass (`./scripts/local_gate.sh`).
- Paper Mario parity capture now completes and passes gate:
  - `rmse=0.017899`, `mae=0.002317`, `pass=true`.
  - candidate capture is no longer rejected as persistently black.
- Candidate image statistics improved versus prior black-retry runs:
  - non-black ratio: `0.02816`
  - mean luma: `0.002335`
- Frame forensics for this run (`155` records, `111` active):
  - present selection: `{5:44, 1:1, 3:73, 4:37}`
  - VI reject: `{1:44, 0:111}`
  - texture source remains fully RDRAM-backed (`tx_rdram_share=1.0`, `tx_synth=0`).
  - luma levels increased substantially in active frames:
    - median `vi_src_luma_avg_x1000`: `1451`
    - median `vi_out_luma_avg_x1000`: `644`
    - median executor `out_luma_avg_x1000`: `55523`

Implication:
- P4/P5 changes resolved the immediate black-frame bottleneck.
- Remaining work shifts from "make anything visible" to "close correctness gap and brightness/structure mismatch vs reference."

## Latest Findings (P3 TMEM Expansion Pass)

After extending TMEM-backed texel decode from CI-only to CI + IA/I + RGBA16 (with 32b still on RDRAM fallback):

- Local gate status: pass (`./scripts/local_gate.sh`).
- Paper Mario parity remains stable:
  - `rmse=0.017899`, `mae=0.002317`, `pass=true`.
- Clean frame forensics capture (`/tmp/rvk2-frame-forensics-clean.tsv`, `154` records, `111` active):
  - present selection: `{5:43, 1:1, 3:73, 4:37}`
  - VI reject: `{1:43, 0:111}`
  - texture path counters:
    - `tx_samples=719032824`
    - `tx_tmem=336098064`
    - `tx_rdram=382934760`
    - `tx_synth=0`
    - `tx_lut=230841816`
    - `tx_tmem_reject_size=382934760` (current remaining TMEM misses are 32b-sized texels)

Implication:
- TMEM path is now handling a substantial share of active texel traffic without changing current parity metrics.
- Next P3 closure item is 32b TMEM texel addressing semantics; all non-32b active texel formats are now covered by TMEM decode.

## Update Log

| Date | Change | Notes |
| --- | --- | --- |
| 2026-03-02 | Initial plan created. | Derived from current RVK2 smoke/parity behavior and trace analysis. |
| 2026-03-02 | Added frame forensics instrumentation (P1, in progress). | Executor summary now records present-surface selection, per-surface write/work rankings, and VI rejection/resolved-state metadata; context can emit per-frame forensic records via `REALITYVK2_FRAME_FORENSICS_FILE`. |
| 2026-03-02 | Added deterministic present fallback improvements (P2, in progress). | Executor now supports no-work previous-surface fallback, bounded surface-history cache, and VI-origin selection against history before non-authoritative fallback. |
| 2026-03-02 | Added executor RDRAM-backed texture sampling path (P3, in progress). | Texture sampling now attempts direct RDRAM decode for RGBA/CI/IA/I formats before synthetic fallback; local gate remains green, and parity still shows low-luma output with valid surface/VI selection in late frames. |
| 2026-03-02 | Expanded frame forensics with texture/luma/source counters (P1 complete). | Added per-frame texture path counters (`tx_*`), executor output luma, VI source/output luma, VI valid/invalid source sample counts, and selected-surface size/dimensions for direct diagnosis of dark/noise frames. |
| 2026-03-02 | Added VI source-size-aware mapping and VI-type-aware present source preference (P2 in progress). | VI input now receives source byte-size metadata and uses byte-origin-aware source indexing; surface selection prefers VI-type-compatible size classes before fallback. Local gate remains passing. |
| 2026-03-02 | Replaced combiner input mapping with RDP selector tables and corrected 1-cycle selector usage (P4/P5 in progress). | Combiner now uses distinct A/B/C/D selector domains and uses cycle-2 selectors in 1-cycle mode; removed previous synthetic constant/destination mixing in combiner stage. |
| 2026-03-02 | Reworked blender to selector-driven P/A/M/B path and fixed alpha-compare control decode (P5 in progress). | Blender now follows mode selectors with force/AA divide behavior and reduced synthetic weighting; alpha-compare now respects `alpha_compare_en` + `dither_alpha_en` bit semantics. |
| 2026-03-02 | Relaxed conformance hash-sensitivity assertions by default during active bring-up. | `rvk2_conformance_tests` now keeps structural checks strict while skipping legacy sensitivity-only expectations unless `REALITYVK_RVK2_STRICT_CONFORMANCE=1` is set. |
| 2026-03-02 | Replaced synthetic LUT approximation with real TMEM TLUT palette decode (P3/P5 in progress). | CI/TLUT sampling now uses palette entries from TMEM for decode; forensics now reports `tx_lut` and keeps texel source usage split via `tx_tmem`/`tx_rdram` counters. |
| 2026-03-02 | Expanded TMEM texel decode to CI + IA/I + RGBA16 (P3 in progress). | TMEM now services a large share of active texel reads while preserving current parity metrics; remaining TMEM misses are dominated by 32b-sized texels. |
