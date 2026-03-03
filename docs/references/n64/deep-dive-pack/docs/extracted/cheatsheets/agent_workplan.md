# Workplan for an "agent" assisting N64 video core development

## Mission

Help implement and validate an N64-accurate RDP + VI pipeline by:

- tracking requirements (register behaviors, command semantics)
- generating targeted test cases
- performing differential debugging when frames mismatch

## Recommended agent skill modules

1) **Spec ingestion**
- Parse VI/RDP register maps and produce structured JSON summaries.
- Track "known uncertain" behaviors separately from confirmed facts.

2) **Trace tooling**
- Given an RDP command stream, annotate state transitions and identify hazards requiring sync.
- Given a framebuffer mismatch, bisect the minimal command prefix which reproduces it.

3) **Conformance harness integration**
- Run a corpus of command tests and report pass/fail deltas.
- Summarize diffs by category (coverage, z, blender, combiner, VI post).

4) **VI scanline analysis**
- Model the VI output line-by-line and detect when mid-frame register writes should affect output.

## Debug playbook

When a frame mismatch occurs:

1. Determine whether the mismatch is **RDP-side** (wrong framebuffer) or **VI-side** (wrong post-processing/scale).
2. If RDP-side:
   - Compare coverage/hidden bits first (many AA/resample issues start here).
   - Check fixed-point edge stepping and scissor.
3. If VI-side:
   - Verify the VI_STATUS AA mode and filter flags.
   - Check X/Y scale and start/end crop window registers.
   - Check interlace/serrate mode.

