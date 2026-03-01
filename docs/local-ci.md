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
- `REALITYVK_GATE_JOBS=<n>` sets build parallelism.
- `REALITYVK_PM_REFERENCE_PLUGIN=<path>` overrides reference plugin path used by parity checks.
- `REALITYVK_PM_CANDIDATE_PLUGIN=<path>` overrides candidate plugin path used by parity checks.
- `REALITYVK_GATE_SMOKE_REQUIRE_READBACK_MARKER=0` disables marker enforcement (debug-only escape hatch).
- `REALITYVK_GATE_SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL=0` disables strict depth-failure gate (debug-only escape hatch).
- `REALITYVK_GATE_SMOKE_REQUIRE_DEPTH_BLIT_STATS=0` disables required depth stats marker check.
- `REALITYVK_GATE_SMOKE_CAPTURE_DEPTH_SUMMARY=0` disables depth summary artifact generation.

Examples:

```bash
REALITYVK_GATE_WITH_QT=1 ./scripts/local_gate.sh
REALITYVK_GATE_WITH_SMOKE=1 ./scripts/local_gate.sh
```

## Smoke gate details

Smoke checks are documented in [local-smoke.md](./local-smoke.md).

Default smoke execution path is wired to the local LLM-specific Mupen runtime:

- `/home/auro/code/mupen/mupen64plus-runtime`
