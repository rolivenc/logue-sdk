# Crunch - five-character distortion for the minilogue xd delay slot

Time        = distortion type (5 zones of 20%)
Depth       = drive (0 dB at 0% up to +54 dB at 100%, linear in dB)
Shift+Depth = dry/wet mix

| Zone | Type       | Character |
|------|------------|-----------|
| 1    | Tape       | dark, glued, low gain, odd harmonics, low head bump |
| 2    | Tube       | warm, strong 2nd harmonic that stays at any drive |
| 3    | Overdrive  | tight lows, mid-forward, dense two-stage clipping |
| 4    | Germanium  | hot, bright, asymmetric fuzz with a sputtering gate |
| 5    | Soft Clip  | clean cubic clip, full bandwidth, odd harmonics |

Signal flow: drive -> type (shaper + its own filters) -> DC blocker -> type trim
-> dry/wet -> final hard clip.

The hard clipper is the last stage, after the dry/wet sum, so the output can
never exceed +/-CEILING (default 0.8, about -2 dBFS).

Changing the Time zone switches the type instantly (no crossfade) and adds a
short click so the switch is always audible.

## Build

Extract this folder to `logue-sdk/platform/minilogue-xd/crunch_delfx/`
(it needs the SDK's `inc/` folders, CMSIS and the toolchain).

With the SDK toolchain (tools/gcc/get_gcc_* script, or the docker image):

    make install

With a newer system arm-none-eabi-gcc (the unit in this package was built
with 13.2, which does not support -mstructure-size-boundary):

    make install GCC_BIN_PATH=/usr/bin COPT="-std=c11"

Output: `crunch_delfx.mnlgxdunit` (load it with the logue Librarian into the
delay FX user slot).

## Tunables (top of crunch_delfx.c)

- CEILING        final hard-clip level
- DRIVE_MAX_DB   gain at Depth = 100%
- k_type_gain[]  how hard each type is driven for the same Depth setting
- k_type_trim[]  per-type output level match
- TYPE_CLICK     click added on a type change (0 = natural switching click only)
- TAPE_*, TUBE_*, OD_*, GE_*   per-type character
