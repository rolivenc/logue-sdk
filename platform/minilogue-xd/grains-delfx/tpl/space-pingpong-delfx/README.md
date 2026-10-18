# Space Ping Pong — tempo-synced stereo tape delay for the Korg minilogue xd (delfx)

The ping-pong sibling of Space Echo, with the same Roland RE-201 inspired tape voicing:
three playback heads at 1:2:3 spacing, repeats that get darker and more saturated on every
pass, wow/flutter, and tape-speed pitch bends when the time or tempo changes.

## How the ping-pong works

Two tapes: A (left) and B (right).

    input -> record A
    heads of A -> left output, and cross-fed to record B
    heads of B -> right output, and fed back to record A (intensity)

So every repeat hops to the opposite side: L, R, L, R... The first right-hand echo lands one
head-delay after the first left-hand echo. At intensity 0 you still get one left and one
right bounce; raising intensity keeps the bounce going.

## Controls (the xd delfx slot only exposes these three)

| Control      | Function |
|--------------|----------|
| TIME         | 21-position selector: 3 speed bands x 7 head modes (chart below) |
| DEPTH        | Intensity (feedback). Pushes toward soft-limited self-oscillation at the top |
| SHIFT+DEPTH  | Dry/Wet (equal-power crossfade, 0 % = dry, 100 % = all echo) |

The xd display shows user-effect parameters only as 0-100 %, and a unit cannot change that
text, so use this chart to read the note values off the percentage. Head delays are U, 2U, 3U
(fractions of a whole note in 4/4; 3/16 = dotted 1/8, 3/8 = dotted 1/4, 3/4 = dotted 1/2):

| Band | Knob travel | H1   | H2  | H3   |
|------|-------------|------|-----|------|
| 0    | 0 - 33 %    | 1/16 | 1/8 | 3/16 |
| 1    | 33 - 67 %   | 1/8  | 1/4 | 3/8  |
| 2    | 67 - 100 %  | 1/4  | 1/2 | 3/4  |

Which taps are sounding, and the TIME knob % where each position sits (as displayed, give or
take 1 %). The times below are for the first (left) echoes; the right side follows one
head-delay later:

| Head mode | Band 0 | Band 1 | Band 2 |
|---|---|---|---|
| H1 | 1/16 (0–5%) | 1/8 (33–38%) | 1/4 (67–71%) |
| H2 | 1/8 (5–10%) | 1/4 (38–43%) | 1/2 (71–76%) |
| H3 | 3/16 (10–14%) | 3/8 (43–48%) | 3/4 (76–81%) |
| H1+H2 | 1/16 + 1/8 (14–19%) | 1/8 + 1/4 (48–52%) | 1/4 + 1/2 (81–86%) |
| H2+H3 | 1/8 + 3/16 (19–24%) | 1/4 + 3/8 (52–57%) | 1/2 + 3/4 (86–90%) |
| H1+H3 | 1/16 + 3/16 (24–29%) | 1/8 + 3/8 (57–62%) | 1/4 + 3/4 (90–95%) |
| H1+H2+H3 | 1/16 + 1/8 + 3/16 (29–33%) | 1/8 + 1/4 + 3/8 (62–67%) | 1/4 + 1/2 + 3/4 (95–100%) |

Changing the TIME position or the tempo glides the tape speed, so existing repeats bend in
pitch like turning the repeat-rate knob. Big jumps take up to ~1.5 s to settle.

## Build

1. Put this folder at `logue-sdk/platform/minilogue-xd/space-pingpong-delfx`.
2. Make sure CMSIS is present: `git submodule update --init platform/ext/CMSIS`
   (only `CMSIS/Include` is needed).
3. Build and package:

       make GCC_BIN_PATH=/usr/bin install       # system arm-none-eabi-gcc
       # or use the SDK toolchain / docker environment without GCC_BIN_PATH

   This produces `space_pingpong.mnlgxdunit`. Load it with the Korg Librarian or `logue-cli`
   into a delay user slot.

## Notes

- Parameters arrive from the xd as Q31 values (not 10-bit), converted with `q31_to_f32`.
- Mono input (L+R summed) feeds tape A; the stereo image comes from the two tapes.
- Two 2^18-float tapes use 2 MB of the 2432 KB SDRAM; tempo is clamped to 40-300 BPM.
- The unit name is exactly 13 characters (`SpacePingPong`); if your loader complains
  about the length, shorten `name` in `manifest.json`.
- No libm on purpose: linking it pulls in newlib syscalls and the unit fails to link.
- Tuning constants (wow/flutter depth, drive, max feedback, cross-hop level) are `#define`s
  at the top of `space_pingpong.cpp`.
