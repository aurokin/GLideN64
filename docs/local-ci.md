# Local CI Gate

This fork currently uses local CI only.
This branch is Vulkan-only for plugin build/runtime paths.

## Required gate

Run this before every push:

```bash
./scripts/local_gate.sh
```

## Enforce on push

Install the git hook once in your local clone:

```bash
./scripts/install-git-hooks.sh
```

This configures `core.hooksPath=.githooks` and runs the gate automatically on every `git push`.

By default, the gate runs:

- Release CLI build (`MUPENPLUSAPI=ON`, `MUPENPLUSAPI_GLIDENUI=OFF`)
- Debug CLI build (`MUPENPLUSAPI=ON`, `MUPENPLUSAPI_GLIDENUI=OFF`)
- `rvk2_unit_tests` in both Release and Debug gate builds.
- `rvk2_conformance_tests` in both Release and Debug gate builds.

## Optional knobs

- `REALITYVK_GATE_WITH_QT=1` adds the Qt UI build to the gate.
- `REALITYVK_GATE_WITH_SMOKE=1` runs Paper Mario parity checks after build:
  - Reference plugin from upstream worktree (`REALITYVK_PM_REFERENCE_PLUGIN`)
  - Candidate plugin built from this branch (`REALITYVK_PM_CANDIDATE_PLUGIN`)
  - Readback baseline marker check is enabled by default in smoke mode:
    - `REALITYVK_GATE_SMOKE_REQUIRE_READBACK_MARKER=1` (default)
    - Forces smoke to fail if Vulkan readback marker logs are missing.
  - Depth-copy strict checks are enabled by default in smoke mode:
    - `REALITYVK_GATE_SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL=1` (default)
    - `REALITYVK_GATE_SMOKE_REQUIRE_DEPTH_BLIT_STATS=1` (default)
    - `REALITYVK_GATE_SMOKE_CAPTURE_DEPTH_SUMMARY=1` (default)
    - Candidate capture fails on depth blit failure markers; per-run depth summary JSON artifacts are emitted in parity run output.
  - rvk2 packet replay validation is enabled by default in smoke mode:
    - `REALITYVK_GATE_RVK2_TRACE_REPLAY=1` (default)
    - Injects `REALITYVK2_PACKET_TRACE_FILE` for the parity run and validates it using `scripts/rvk2_packet_trace_replay.py`.
    - Fails the gate if the trace file is missing or replay/hash checks fail.
    - Writes JSON report to `build/local-gate/rvk2.packet.replay.json` by default.
- `REALITYVK_GATE_JOBS=<n>` sets build parallelism.
- `REALITYVK_GATE_RUN_RVK2_UNIT_TESTS=0` skips `rvk2_unit_tests` execution (debug-only escape hatch).
- `REALITYVK_GATE_RUN_RVK2_CONFORMANCE_TESTS=0` skips `rvk2_conformance_tests` execution (debug-only escape hatch).
- `REALITYVK_PM_REFERENCE_PLUGIN=<path>` overrides reference plugin path used by parity checks.
- `REALITYVK_PM_CANDIDATE_PLUGIN=<path>` overrides candidate plugin path used by parity checks.
- `REALITYVK_GATE_SMOKE_REQUIRE_READBACK_MARKER=0` disables marker enforcement (debug-only escape hatch).
- `REALITYVK_GATE_SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL=0` disables strict depth-failure gate (debug-only escape hatch).
- `REALITYVK_GATE_SMOKE_REQUIRE_DEPTH_BLIT_STATS=0` disables required depth stats marker check.
- `REALITYVK_GATE_SMOKE_CAPTURE_DEPTH_SUMMARY=0` disables depth summary artifact generation.
- `REALITYVK_GATE_RVK2_TRACE_REPLAY=0` disables rvk2 packet replay validation (debug-only escape hatch).
- `REALITYVK_GATE_RVK2_TRACE_REPLAY_STRICT=0` relaxes strict replay mode (strict is default).
- `REALITYVK_GATE_RVK2_TRACE_REPLAY_JOBS=<n>` sets replay worker process count (`0` = auto/all cores).
- `REALITYVK_GATE_RVK2_TRACE_FILE=<path>` overrides packet trace output file for replay checks.
- `REALITYVK_GATE_RVK2_TRACE_REPORT_FILE=<path>` overrides replay JSON report path.
- Texture-pack index validation is opt-in:
  - `REALITYVK_GATE_TX_PACK_DIR=<dir>` enables pack-index validation automatically.
  - `REALITYVK_GATE_TX_PACK_VALIDATE=1|0` forces enable/disable behavior.
  - `REALITYVK_GATE_TX_PACK_INDEX=<path>` overrides index path (`<pack-dir>/rkv2_pack_index_v1.tsv` by default).
  - `REALITYVK_GATE_TX_PACK_REQUIRE_COVERAGE=1` fails if any `.rgba32` under pack dir is not indexed.
  - `REALITYVK_GATE_TX_PACK_ALLOW_EMPTY=1` allows empty index files.
  - `REALITYVK_GATE_TX_PACK_ALLOW_ABSOLUTE_PATHS=1` allows absolute `rgba_file` rows in index.

Examples:

```bash
REALITYVK_GATE_WITH_QT=1 ./scripts/local_gate.sh
REALITYVK_GATE_WITH_SMOKE=1 ./scripts/local_gate.sh
REALITYVK_GATE_WITH_SMOKE=1 REALITYVK_PM_VISUAL_GATE=0 ./scripts/local_gate.sh
REALITYVK_GATE_TX_PACK_DIR=/path/to/pack ./scripts/local_gate.sh
```

Determinism utility:

```bash
./scripts/rvk2_trace_determinism.sh
```

Texture-pack index tooling:

```bash
# Generate canonical index from .rgba32 files
python3 scripts/rvk2_texture_pack_index.py generate --pack-dir /path/to/pack

# Validate index contract (rewrite if order drifted)
python3 scripts/rvk2_texture_pack_index.py validate --pack-dir /path/to/pack --rewrite
```

Texture replacement runtime controls (during emulator run):

```bash
# Runtime control file (read each present by rvk2)
export REALITYVK_RVK2_TX_CONTROL_FILE=/tmp/rvk2_tx_control.txt

# Configure replacement sources and optional summary sinks
python3 scripts/rvk2_tx_control.py set \
  --control-file /tmp/rvk2_tx_control.txt \
  --enable 1 \
  --cache-path /path/to/cache.hts \
  --pack-path /path/to/pack \
  --summary-path /tmp/rvk2_tx_summary.txt \
  --log-summary 1

# Trigger runtime reload/invalidate without restart
python3 scripts/rvk2_tx_control.py bump-reload --control-file /tmp/rvk2_tx_control.txt
python3 scripts/rvk2_tx_control.py bump-invalidate --control-file /tmp/rvk2_tx_control.txt
```

## Smoke gate details

Smoke checks are documented in [local-smoke.md](./local-smoke.md).

Default smoke execution path is wired to the local LLM-specific Mupen runtime:

- `/home/auro/code/mupen/mupen64plus-runtime`
