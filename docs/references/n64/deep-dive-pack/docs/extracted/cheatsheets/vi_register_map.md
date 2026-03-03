# VI register map cheat sheet

> Addresses below are physical MMIO addresses.

| Address | Register | Purpose (high level) |
|---:|---|---|
| 0x0440_0000 | VI_STATUS_REG / VI_CONTROL_REG | Framebuffer format + enable/disable post-processing (gamma/divot/AA modes, etc.) |
| 0x0440_0004 | VI_ORIGIN_REG | RDRAM address of framebuffer origin |
| 0x0440_0008 | VI_WIDTH_REG | Framebuffer width in pixels (stride) |
| 0x0440_000C | VI_INTR_REG | Programmable scanline interrupt target |
| 0x0440_0010 | VI_V_CURRENT_REG | Current scanline (and field info in some modes) |
| 0x0440_0014 | VI_BURST_REG | Burst/timing control (TV encoder interaction) |
| 0x0440_0018 | VI_V_SYNC_REG | Vertical sync timing |
| 0x0440_001C | VI_H_SYNC_REG | Horizontal sync timing |
| 0x0440_0020 | VI_LEAP_REG | HSync phase leap (even/odd line adjust) |
| 0x0440_0024 | VI_H_START_REG | Horizontal start/end (crop window) |
| 0x0440_0028 | VI_V_START_REG | Vertical start/end (crop window) |
| 0x0440_002C | VI_V_BURST_REG | Vertical burst control |
| 0x0440_0030 | VI_X_SCALE_REG | Horizontal scaling (and subpixel offset) |
| 0x0440_0034 | VI_Y_SCALE_REG | Vertical scaling (and subpixel offset) |

Source: n64.readthedocs.io (VI section)
