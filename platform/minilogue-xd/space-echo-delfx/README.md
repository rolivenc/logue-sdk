# Space Echo — tempo-synced tape delay for the Korg minilogue xd (delfx)

Inspired by the Roland RE-201 Space Echo. Three playback heads at 1:2:3 spacing,
a filtered + saturated feedback loop, wow/flutter, and tape-speed pitch bends when
the time or tempo changes.

## Controls (the xd delfx slot only exposes these three)

| Control      | Function |
|--------------|----------|
| TIME         | 21-position selector: 3 speed bands x 7 head modes (table below) |
| DEPTH        | Intensity (feedback). Top of the knob self-oscillates (soft limited) |
| SHIFT+DEPTH  | Dry/Wet (equal-power crossfade) |

Head delays are U, 2U, 3U where U is set by the band:

| Band | Knob travel | U      | Heads (H1 / H2 / H3)            |
|------|-------------|--------|----------------------------------|
| 0    | 0 – 33 %    | 1/16   | 1/16, 1/8, dotted 1/8            |
| 1    | 33 – 67 %   | 1/8    | 1/8, 1/4, dotted 1/4             |
| 2    | 67 – 100 %  | 1/4    | 1/4, 1/2, dotted 1/2             |

Inside each band, from left to right: `H1, H2, H3, H1+H2, H2+H3, H1+H3, H1+H2+H3`
(each step is about 4.8 % of knob travel).

Changing the TIME position or the tempo glides the tape speed, so existing repeats
bend in pitch like turning the repeat-rate knob. Big jumps take up to ~1.5 s to settle.

## Build

1. Put this folder at `logue-sdk/platform/minilogue-xd/space-echo-delfx`.
2. Make sure CMSIS is present: `git submodule update --init platform/ext/CMSIS`
   (only `CMSIS/Include` is needed).
3. Build and package:

       make GCC_BIN_PATH=/usr/bin install       # system arm-none-eabi-gcc
       # or use the SDK toolchain / docker environment without GCC_BIN_PATH

   This produces `space_echo.mnlgxdunit`. Load it with the Korg Librarian or `logue-cli`
   into a delay user slot.

## Notes

- Mono tape (L+R summed in, mono echo out), like the original.
- Tape buffer is 2^18 floats (1 MB) in SDRAM; tempo is clamped to 40–300 BPM.
- No libm is used on purpose: linking it pulls in newlib syscalls and the unit fails to link.
- Tuning constants (wow/flutter depth, loop filters, drive, max feedback) are `#define`s
  at the top of `space_echo.cpp`.
