# Video-core verification checklist (RDP + VI)

## VI / scanout

- [ ] VI register file matches N64 semantics (32-bit writes; correct masking where applicable).
- [ ] VI_STATUS fields affect scanout and filtering (gamma, gamma dither, divot, AA/resample modes).
- [ ] Interlace and field behavior matches expected mode behavior.
- [ ] VI interrupt triggers when V_CURRENT == V_INTR and routes through MI interrupt logic.
- [ ] Mid-frame VI register updates can affect scanline output (repeater64 VI demos).

## RDP core

- [ ] Command decoder: correct opcode parsing and state update ordering.
- [ ] Sync semantics: SyncPipe/SyncTile/SyncLoad/SyncFull correctly serialize hazards.
- [ ] Fixed-point stepping/rounding matches known-good (major source of off-by-one pixel errors).
- [ ] Coverage rules implemented; not just final RGBA.
- [ ] Z-buffer format/precision and compare behavior.

## RDRAM 9th-bit / hidden memory

- [ ] Implement hidden/coverage bits (9th bit plane) in a way that matches CPU/RSP/RDP writes.
- [ ] Validate with conformance-style comparisons (paraLLEl-RDP approach) and stress ROMs (repeater64).

## Test suites to run

- n64-systemtest (broad regression)
- repeater64 (hard effects: 9th bit + VI scanline tricks)
- Differential tests against a golden renderer (angrylion / paraLLEl-RDP style)

