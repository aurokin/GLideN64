# N64 Reference Bundle

This directory is the local reference cache for Vulkan migration and correctness work.

## Ground-truth docs

- `Nintendo_64_Programming_Manual_NU6-06-0030-001G_HQ.pdf`
  - Source: https://ultra64.ca/files/documentation/nintendo/Nintendo_64_Programming_Manual_NU6-06-0030-001G_HQ.pdf
- `SGI_Nintendo_64_RSP_Programmers_Guide.pdf`
  - Source: https://ultra64.ca/files/documentation/silicon-graphics/SGI_Nintendo_64_RSP_Programmers_Guide.pdf
- `SGI_RDP_Command_Summary.pdf`
  - Source: https://ultra64.ca/files/documentation/silicon-graphics/SGI_RDP_Command_Summary.pdf
- `allman51eng.zip` (N64 online manuals pack, includes function references and programming manual tree)
  - Source: https://ultra64.ca/files/documentation/online-manuals/allman51eng.zip

## Supporting wiki snapshots

- `n64brew_Reality_Display_Processor_Commands.html`
  - Source: https://n64brew.dev/wiki/Reality_Display_Processor/Commands
- `n64brew_Reality_Display_Processor_Pipeline.html`
  - Source: https://n64brew.dev/wiki/Reality_Display_Processor/Pipeline
- `n64brew_Reality_Signal_Processor.html`
  - Source: https://n64brew.dev/wiki/Reality_Signal_Processor
- `n64brew_Video_Interface.html`
  - Source: https://n64brew.dev/wiki/Video_Interface

## Cross-core bug research inputs

Raw issue snapshots used for test selection are in `issue-research/`:

- `realityvk_samples_2026-03-01.jsonl`
- `parallel_rdp_samples_2026-03-01.jsonl`
- `parallel_n64_samples_2026-03-01.jsonl`
- `rice_samples_2026-03-01.jsonl`
- `glide64mk2_samples_2026-03-01.jsonl`

## SHA256

- `1cf05657ec35b807a6222e59e63913096bc21a047199e274b5667ecedd6ca89a` `Nintendo_64_Programming_Manual_NU6-06-0030-001G_HQ.pdf`
- `6ec646ba9bd836af9aa30ae90099af55dbaf291fbd8b39ff8be5664ada3bd668` `SGI_Nintendo_64_RSP_Programmers_Guide.pdf`
- `fe019ef28498d759292145f06716ac11620240e643dd65809d15ca4a6f1c6ad3` `SGI_RDP_Command_Summary.pdf`
- `c4ee7de1dc428b1ddc2c2c6a986dd8cf42175a276635e18e5533b7d8d7c277fd` `allman51eng.zip`
- `4d4c7f6a04c5e44856ee6805af752dee997e1cf7038d50cbda77361592842c34` `n64brew_Reality_Display_Processor_Commands.html`
- `ea1fda6aab459f7b4a572b5c813c9e9fecb66491e7294996ad3f2819701d0fe0` `n64brew_Reality_Display_Processor_Pipeline.html`
- `b44d0103c55c1ef1e3e626ebe9f644fd6256f2b06e58993322564063a90a45c1` `n64brew_Reality_Signal_Processor.html`
- `94ef14708dad634a1f3707d4e3e894af1505232a719eb456dad3640fe81e4fb2` `n64brew_Video_Interface.html`

## Refresh command

```bash
sha256sum docs/references/n64/* | sed 's#docs/references/n64/##'
```
