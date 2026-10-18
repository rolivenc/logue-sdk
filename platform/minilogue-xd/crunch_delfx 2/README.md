# Crunch - multi-mode distortion for the minilogue xd delay slot

Time        = distortion type (5 zones of 20%): Soft, Tube, Overdrive, Hard, Fold
Depth       = drive (0 dB at 0% up to +54 dB at 100%, linear in dB)
Shift+Depth = dry/wet mix

Signal flow: drive -> type shaper -> DC blocker -> dry/wet -> final hard clip

The hard clipper is the last stage, after the dry/wet sum, so the output can
never exceed +/-CEILING (default 0.8, about -2 dBFS).

Changing the Time zone switches the waveshaper instantly (no crossfade) and
adds a short click so the switch is always audible.

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

- CEILING       final hard-clip level
- DRIVE_MAX_DB  gain at Depth = 100% (lower it if the top end is too extreme
                or the source noise gets too loud between notes)
- TYPE_CLICK    click added on a type change (0 = natural switching click only)
- TUBE_NEG      Tube asymmetry (negative-side ceiling)
- OD_PRE / OD_POST / OD_KNEE   Overdrive stages
- FOLD_PRE / FOLD_POST         Fold gains (more = more folds)
- OUT_TRIM      wet level before the mix
