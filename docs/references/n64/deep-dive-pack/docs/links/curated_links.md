# Curated links: Nintendo 64 video core (RDP + VI)

**Goal:** A high-signal, implementation-oriented reading list.

## Core architecture and memory map

- n64docs (Dillon Beliveau) - overview + physical memory map + VI register notes: https://n64.readthedocs.io/
- N64 hardware architecture write-up (Copetti): https://www.copetti.org/writings/consoles/nintendo-64/

## VI (Video Interface)

- libultra function reference: `osVi` (modes, scaling, special features like gamma/divot/dither filter): https://ultra64.ca/files/documentation/online-manuals/functions_reference_manual_2.0i/os/osVi.html
- n64docs VI section (VI_STATUS bits and register list): https://n64.readthedocs.io/
- repeater64 (tests mid-frame VI register changes, VI pre-line effects): https://github.com/HailToDodongo/repeater64

## RDP (Reality Display Processor)

- paraLLEl-RDP rewrite background (why LLE matters; conformance testing approach):
  - https://www.libretro.com/index.php/reviving-and-rewriting-parallel-rdp-fast-and-accurate-low-level-n64-rdp-emulation/
  - https://www.libretro.com/index.php/parallel-n64-low-level-rdp-upscaling-is-finally-here/
- parallel-rdp repo (MIT): https://github.com/Themaister/parallel-rdp
- SGI RDP Command Summary (command encodings + fields): https://ultra64.ca/files/documentation/silicon-graphics/SGI_RDP_Command_Summary.pdf
- RDP reverse engineering / notes (offtkp): https://offtkp.github.io/rdp.html

## RSP microcode (feeding the RDP)

- Introduction to N64 microcode (RSP -> RDP command passing notes): https://ultra64.ca/files/documentation/online-manuals/man/n64man/ucode/microcode.html
- SGI Nintendo 64 RSP Programmer's Guide (deep microcode reference): https://ultra64.ca/files/documentation/silicon-graphics/SGI_Nintendo_64_RSP_Programmers_Guide.pdf
- Fast3DEX2 display list command reference: https://hack64.net/wiki/doku.php?id=f3dex2

## Test ROMs / verification

- n64-systemtest (general emulator/hardware regression ROM): https://github.com/lemmy-64/n64-systemtest
- Peter Lemon N64 test programs/demos: https://github.com/PeterLemon/N64
- repeater64 (hard-to-emulate behaviors: hidden RDRAM bit, fill-mode corner cases, VI scanline effects): https://github.com/HailToDodongo/repeater64

## Practical homebrew SDK reference

- libdragon (open-source N64 SDK; good reference for VI/RDP usage patterns):
  - Repo: https://github.com/DragonMinded/libdragon
  - API docs: https://dragonminded.github.io/libdragon/

