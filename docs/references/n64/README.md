# N64 Reference Corpus

## Primary Hardware Docs

- `Nintendo_64_Programming_Manual_NU6-06-0030-001G_HQ.pdf`
- `SGI_Nintendo_64_RSP_Programmers_Guide.pdf`
- `SGI_RDP_Command_Summary.pdf`
- `allman51eng.zip`

## Local Wiki Snapshots

- `n64brew_Reality_Display_Processor_Commands.html`
- `n64brew_Reality_Display_Processor_Pipeline.html`
- `n64brew_Reality_Signal_Processor.html`
- `n64brew_Video_Interface.html`

## Supplemental Research Pack (2026-03-01)

- Source zip mirror: `n64_video_core_deep_dive_pack.zip`
- Expanded corpus: `deep-dive-pack/`
  - `report.md` / `report.pdf`
  - `docs/links/curated_links.md`
  - `docs/extracted/` (upstream READMEs, headers, licenses, cheatsheets)
  - `rvk2_delta_notes.md` (project-specific integration notes)

This pack is implementation-focused and useful for verification strategy, test ROM targeting,
and emulator engineering notes. Treat hardware manuals + local n64brew snapshots above as
normative when source claims conflict.

## Source URLs

- https://ultra64.ca/files/documentation/nintendo/Nintendo_64_Programming_Manual_NU6-06-0030-001G_HQ.pdf
- https://ultra64.ca/files/documentation/silicon-graphics/SGI_Nintendo_64_RSP_Programmers_Guide.pdf
- https://ultra64.ca/files/documentation/silicon-graphics/SGI_RDP_Command_Summary.pdf
- https://ultra64.ca/files/documentation/online-manuals/allman51eng.zip
- https://n64brew.dev/wiki/Reality_Display_Processor/Commands
- https://n64brew.dev/wiki/Reality_Display_Processor/Pipeline
- https://n64brew.dev/wiki/Reality_Signal_Processor
- https://n64brew.dev/wiki/Video_Interface

## Refresh Hashes

```bash
sha256sum docs/references/n64/* | sed 's#docs/references/n64/##'
sha256sum docs/references/n64/deep-dive-pack/report.md
```
