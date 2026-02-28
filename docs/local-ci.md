# Local CI Gate

This fork currently uses local CI only.

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

- `GLIDEN64_GRAPHICS_BACKEND=OpenGL|Vulkan` selects the default backend used in the build config.
- `GLIDEN64_GATE_WITH_QT=1` adds the Qt UI build to the gate.
- `GLIDEN64_GATE_JOBS=<n>` sets build parallelism.

Examples:

```bash
GLIDEN64_GATE_WITH_QT=1 ./scripts/local_gate.sh
GLIDEN64_GRAPHICS_BACKEND=Vulkan ./scripts/local_gate.sh
```
