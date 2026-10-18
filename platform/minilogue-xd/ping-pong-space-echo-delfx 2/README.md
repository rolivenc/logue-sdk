# Space Echo — tempo-synced tape delay for the Korg minilogue xd (delfx)

Inspired by the Roland RE-201 Space Echo. Three playback heads at 1:2:3 spacing,
a filtered + saturated feedback loop, wow/flutter, and tape-speed pitch bends when
the time or tempo changes.

## Controls (the xd delfx slot only exposes these three)

| Control      | Function |
|--------------|----------|
| TIME         | 21-position selector: 3 speed bands x 7 head modes (table below) |
| DEPTH        | Intensity (feedback). Top of the knob self-oscillates (soft limited) |
| SHIFT+DEPTH  | Dry/Wet (equal-power crossfade, 0 % = dry, 100 % = all echo) |

The xd display shows user-effect parameters only as 0-100 %, and a unit cannot
change that text, so use this chart to read the note values off the percentage.
Head delays are U, 2U, 3U (fractions of a whole note, 4/4 time; 3/16 = dotted 1/8,
3/8 = dotted 1/4, 3/4 = dotted 1/2):

| Band | Knob travel | H1   | H2  | H3  |
|------|-------------|------|-----|-----|
| 0    | 0 - 33 %    | 1/16 | 1/8 | 3/16 |
| 1    | 33 - 67 %   | 1/8  | 1/4 | 3/8  |
| 2    | 67 - 100 %  | 1/4  | 1/2 | 3/4  |

Which taps are sounding, and the TIME knob % where each position sits (shown as
the displayed percentage, give or take 1 %):

| Head mode | Band 0 | Band 1 | Band 2 |
|---|---|---|---|
| H1 | 1/16 (0–5%) | 1/8 (33–38%) | 1/4 (67–71%) |
| H2 | 1/8 (5–10%) | 1/4 (38–43%) | 1/2 (71–76%) |
| H3 | 3/16 (10–14%) | 3/8 (43–48%) | 3/4 (76–81%) |
| H1+H2 | 1/16 + 1/8 (14–19%) | 1/8 + 1/4 (48–52%) | 1/4 + 1/2 (81–86%) |
| H2+H3 | 1/8 + 3/16 (19–24%) | 1/4 + 3/8 (52–57%) | 1/2 + 3/4 (86–90%) |
| H1+H3 | 1/16 + 3/16 (24–29%) | 1/8 + 3/8 (57–62%) | 1/4 + 3/4 (90–95%) |
| H1+H2+H3 | 1/16 + 1/8 + 3/16 (29–33%) | 1/8 + 1/4 + 3/8 (62–67%) | 1/4 + 1/2 + 3/4 (95–100%) |

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

- Parameters arrive from the xd as Q31 values (not 10-bit), converted with `q31_to_f32`.
- Mono tape (L+R summed in, mono echo out), like the original.
- Tape buffer is 2^18 floats (1 MB) in SDRAM; tempo is clamped to 40–300 BPM.
- No libm is used on purpose: linking it pulls in newlib syscalls and the unit fails to link.
- Tuning constants (wow/flutter depth, loop filters, drive, max feedback) are `#define`s
  at the top of `space_echo.cpp`.
