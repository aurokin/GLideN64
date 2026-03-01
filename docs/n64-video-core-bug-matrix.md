# N64 Video Core Bug Matrix (Test Inputs)

This matrix was assembled from issue trackers across multiple N64 video cores.
Use it to turn known rendering failures into deterministic regression scenarios.

## Source trackers

- RealityVK: https://github.com/gonetz/RealityVK/issues
- ParaLLEl-RDP: https://github.com/Themaister/parallel-rdp/issues
- Rice: https://github.com/mupen64plus/mupen64plus-video-rice/issues
- Glide64mk2: https://github.com/mupen64plus/mupen64plus-video-glide64mk2/issues

Raw issue snapshots used are in `docs/references/n64/issue-research/`.

## Prioritized bug-to-test candidates

| Priority | Core | Issue | Symptom | Proposed test ID | Automation status |
|---|---|---|---|---|---|
| P0 | RealityVK | [#2803](https://github.com/gonetz/RealityVK/issues/2803) | Non-zero default framebuffer overridden | `host_default_fbo_nonzero` | Needs host adapter harness |
| P0 | ParaLLEl-RDP | [#40](https://github.com/Themaister/parallel-rdp/issues/40) | Paper Mario pause background becomes grayscale | `pm_pause_background_color` | Needs dedicated pause checkpoint |
| P0 | RealityVK | [#2683](https://github.com/gonetz/RealityVK/issues/2683) | TMEM overflow sampling wrap reads wrong pixels | `tmem_wrap_overflow` | Needs small test ROM/state |
| P1 | RealityVK | [#2621](https://github.com/gonetz/RealityVK/issues/2621) | Mario Tennis shadows broken in accurate path | `mtennis_shadow_accuracy` | Needs Mario Tennis checkpoint |
| P1 | ParaLLEl-RDP | [#57](https://github.com/Themaister/parallel-rdp/issues/57) | OoT hi-res/TEX_RECT artifacts and room edge issues | `oot_texrect_hires_flashback` | Needs OoT checkpoint pair |
| P1 | ParaLLEl-RDP | [#27](https://github.com/Themaister/parallel-rdp/issues/27) | Perfect Dark lens flare repeats in hi-res | `pd_lensflare_hires` | Needs Perfect Dark checkpoint |
| P1 | ParaLLEl-RDP | [#69](https://github.com/Themaister/parallel-rdp/issues/69) | Tonic Trouble area transition black screen | `tonic_transition_black` | Needs Tonic Trouble transition state |
| P1 | Rice | [#76](https://github.com/mupen64plus/mupen64plus-video-rice/issues/76) | Pokemon Snap photo review black screen | `psnap_review_black` | Needs Pokemon Snap checkpoint |
| P1 | Rice | [#72](https://github.com/mupen64plus/mupen64plus-video-rice/issues/72) | OoT transition darkening not full-screen | `oot_transition_edge_line` | Needs OoT transition checkpoint |
| P1 | Glide64mk2 | [#77](https://github.com/mupen64plus/mupen64plus-video-glide64mk2/issues/77) | Paper Mario star spirit text missing | `pm_star_spirit_text` | Needs star-spirit dialogue checkpoint |
| P2 | Rice | [#55](https://github.com/mupen64plus/mupen64plus-video-rice/issues/55) | Pokemon Stadium special blend mode mismatch | `pstadium_special_blend` | Needs Pokemon Stadium checkpoint |
| P2 | Glide64mk2 | [#81](https://github.com/mupen64plus/mupen64plus-video-glide64mk2/issues/81) | Pilotwings land-shadow model glitch | `pilotwings_shadow_land` | Needs Pilotwings checkpoint |
| P2 | Glide64mk2 | [#84](https://github.com/mupen64plus/mupen64plus-video-glide64mk2/issues/84) | OoT white flash on pause/menu | `oot_pause_white_flash` | Needs OoT pause transition checkpoint |
| P2 | Glide64mk2 | [#75](https://github.com/mupen64plus/mupen64plus-video-glide64mk2/issues/75) | Pokemon Snap object detection/cpu rendering path issues | `psnap_object_detect` | Needs Pokemon Snap checkpoint |

## Immediate Linux-ready validation set

The following scenarios are runnable now with local Paper Mario assets:

- `pm_boot_title_003`
- `pm_new_game_intro_001`
- `pm_fresh_castle_prebowser_001`
- `pm_fresh_goomba_arrival_001`
- `pm_live_castle_progress_005`
- `pm_live_stairs_party_002_fresh`
- `pm_live_upperhall_right_003_fresh`

Manifest: `tests/smoke/scenarios_paper_mario_runtime.tsv`.
