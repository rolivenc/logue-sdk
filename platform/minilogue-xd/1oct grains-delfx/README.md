# grains - granular delay for minilogue xd (delfx)

Tempo-synced granular delay in the style of Soundtoys Crystallizer.
About 20% of the grains play in reverse, the rest are normal delay taps, and the
grain output is recycled through the feedback path.

| Knob        | Function                                            |
|-------------|-----------------------------------------------------|
| TIME        | Synced time: 1/32, 1/16T, 1/16, 1/8T, 1/8, 1/4T, 1/8., 1/4, 1/2T, 1/4., 1/2 |
| DEPTH       | Feedback / amount of recycled grains                |
| SHIFT+DEPTH | Dry/wet (centre = both at unity)                    |

## Build
Copy this folder next to `dummy-delfx` in `logue-sdk/platform/minilogue-xd/`, then:

    make
    make install     # produces grains_delfx.mnlgxdunit

Tweakables are at the top of `grains.cpp` (`kReverseChance`, `kMaxFeedback`,
`kDampCoef`, `kBeats`). `manifest.json` has dev_id / prg_id if you need to
change them.
