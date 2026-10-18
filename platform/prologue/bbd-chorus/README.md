# BBD Chorus - Juno-style chorus for the Korg minilogue xd and prologue (modfx)

Two projects, one shared source (bbd_chorus.cpp):
- minilogue-xd/bbd-chorus  -> bbd_chorus.mnlgxdunit
- prologue/bbd-chorus      -> bbd_chorus.prlgunit   (project.mk adds -DDUAL_TIMBRE=1)

Controls: Time = mode (I | II | I+II), Depth = wet level.

## Build
    git clone https://github.com/korginc/logue-sdk.git && cd logue-sdk
    git submodule update --init
    cp -r <this>/minilogue-xd/bbd-chorus platform/minilogue-xd/
    cp -r <this>/prologue/bbd-chorus     platform/prologue/
    # xd: 
    cd platform/minilogue-xd/bbd-chorus && make GCC_BIN_PATH=$(dirname $(which arm-none-eabi-gcc)) install
    # prologue (needs firmware >= 2.00 for SDK 1.1 units):
    cd platform/prologue/bbd-chorus && make GCC_BIN_PATH=$(dirname $(which arm-none-eabi-gcc)) install
(or use the SDK's Docker flow). Rename the unit by editing "name" in manifest.json.

## Notes
- Code + data must fit in 6K SRAM; delay lines are in SDRAM. Filter coefficients
  are precomputed (no sinf/cosf) to save code size.
- Prologue is dual-timbre: main and sub each get their own chorus instance.
