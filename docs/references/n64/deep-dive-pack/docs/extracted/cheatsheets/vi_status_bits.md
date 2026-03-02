# VI_STATUS / VI_CONTROL bits (confirmed fields)

These fields are documented directly in n64docs.

| Bits | Name (common) | Meaning |
|---:|---|---|
| 1:0 | BPP / TYPE | Framebuffer bits-per-pixel selector |
| 2 | GAMMA_DITHER | Gamma dither enable |
| 3 | GAMMA | Gamma correction enable |
| 4 | DIVOT | Divot enable |
| 6 | SERRATE | Serration pulse mode (timing/field-related) |
| 9:8 | AA_MODE | AA / resample mode selector |

n64docs enumerates BPP and AA modes (including 16-bit vs 32-bit FB and multiple AA/resample modes).

Source: n64.readthedocs.io (VI_STATUS_REG / VI_CONTROL_REG)
