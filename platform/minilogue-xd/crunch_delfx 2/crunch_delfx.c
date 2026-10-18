/*
 * crunch_delfx.c
 *
 * Multi-mode distortion for the minilogue xd delay slot (logue SDK, delfx).
 *
 *   Time        = distortion type (5 zones of 20%: Soft, Tube, Overdrive, Hard, Fold)
 *   Depth       = drive (0 .. +DRIVE_MAX_DB, linear in dB)
 *   Shift+Depth = dry/wet mix
 *
 * Signal flow per channel:
 *   in -> drive gain -> type shaper -> DC blocker -> dry/wet mix -> HARD CLIP -> out
 *
 * The hard clipper sits at the very end, after the dry/wet sum, so the output
 * can never exceed +/-CEILING no matter how hard the drive is pushed.
 *
 * Changing the Time zone switches the shaper instantly (no crossfade) and adds
 * a short click, so the switch is always audible.
 */

#include "userdelfx.h"

/* ---- tunables ---------------------------------------------------------- */
#define CEILING        0.8f    /* final hard-clip level                                      */
#define OUT_TRIM       1.0f    /* wet level before the mix                                   */
#define DRIVE_MAX_DB   54.0f   /* gain at Depth = 100% (0 dB at 0%)                          */
#define NUM_TYPES      5
#define TYPE_HYST      0.06f   /* zone-border hysteresis, in units of one zone               */
#define TYPE_CLICK     0.20f   /* click added when the type changes (0 = natural click only) */
#define CLICK_DECAY    0.99f   /* click decay per sample (~2 ms)                             */
#define SMOOTH         0.002f  /* one-pole smoothing for drive and mix                       */
#define DC_R           0.9987f /* DC blocker pole (~10 Hz at 48 kHz)                         */

#define TUBE_NEG       0.70f   /* negative-side ceiling of the Tube type (asymmetry)         */
#define OD_PRE         2.0f    /* Overdrive: first-stage soft gain                           */
#define OD_POST        1.3f    /* Overdrive: gain into the knee clipper                      */
#define OD_KNEE        0.50f   /* Overdrive: knee start (low = rounder, high = harder)       */
#define OD_K           (1.0f / (4.0f * (1.0f - OD_KNEE)))
#define FOLD_PRE       3.0f    /* Fold: first-fold gain                                      */
#define FOLD_POST      1.5f    /* Fold: second-fold gain                                     */

#define LOG2_10_OVER_20 0.16609640474f

/* ---- state ------------------------------------------------------------- */
typedef struct {
  float x1;
  float y1;
} dcblock_t;

static dcblock_t s_dc[2];

static float s_drive;
static float s_drive_target;
static float s_mix;
static float s_mix_target;
static float s_tick;
static uint8_t s_type;

/* ---- helpers ----------------------------------------------------------- */
static inline float clampf(float x, float lo, float hi)
{
  return x < lo ? lo : (x > hi ? hi : x);
}

static inline float q31f(int32_t v)
{
  return (float)v * (1.0f / 2147483648.0f);
}

/* 2^x without libm (polynomial on the fraction, exponent by bit construction). */
static inline float fast_exp2(float x)
{
  x = clampf(x, -60.0f, 60.0f);
  int i = (int)x;
  if ((float)i > x) i--;
  const float f = x - (float)i;
  const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
  union { uint32_t u; float f; } s;
  s.u = (uint32_t)(i + 127) << 23;
  return p * s.f;
}

static inline float drive_gain(float d)
{
  d = clampf(d, 0.0f, 1.0f);
  return fast_exp2(d * DRIVE_MAX_DB * LOG2_10_OVER_20);
}

/* ---- shapers (all bounded to about +/-1, unity slope near zero unless noted) */

/* Pade tanh approximation, exact +/-1 at |x| = 3. */
static inline float shape_soft(float x)
{
  x = clampf(x, -3.0f, 3.0f);
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Asymmetric soft clip: positive side clips at +1, negative side at -TUBE_NEG.
 * Even harmonics at any drive; the DC it creates is removed afterwards. */
static inline float shape_tube(float x)
{
  return x >= 0.0f ? shape_soft(x) : TUBE_NEG * shape_soft(x * (1.0f / TUBE_NEG));
}

/* Quadratic knee clip, bounded to +/-1. */
static inline float knee_clip(float x)
{
  const float ax = x < 0.0f ? -x : x;
  float y;
  if (ax < OD_KNEE) {
    y = ax;
  } else if (ax < 2.0f - OD_KNEE) {
    const float d = ax - OD_KNEE;
    y = ax - d * d * OD_K;
  } else {
    y = 1.0f;
  }
  return x < 0.0f ? -y : y;
}

/* Two cascaded stages (soft, then knee clip): dense, compressed, aggressive. */
static inline float shape_overdrive(float x)
{
  return knee_clip(shape_soft(x * OD_PRE) * OD_POST);
}

static inline float shape_hard(float x)
{
  return clampf(x, -1.0f, 1.0f);
}

/* Triangle fold: identity inside +/-1, reflects beyond. */
static inline float tri_fold(float x)
{
  x = clampf(x, -2048.0f, 2048.0f);
  float u = (x + 1.0f) * 0.25f;
  u -= (float)(int)u;
  if (u < 0.0f) u += 1.0f;
  const float d = u * 4.0f - 2.0f;
  return 1.0f - (d < 0.0f ? -d : d);
}

/* Double fold: starts folding at low levels and keeps folding as drive rises. */
static inline float shape_fold(float x)
{
  return tri_fold(FOLD_POST * tri_fold(FOLD_PRE * x));
}

static inline float shape(uint8_t type, float x)
{
  switch (type) {
    case 0:  return shape_soft(x);
    case 1:  return shape_tube(x);
    case 2:  return shape_overdrive(x);
    case 3:  return shape_hard(x);
    default: return shape_fold(x);
  }
}

static inline float dc_block(dcblock_t *s, float x)
{
  const float y = x - s->x1 + DC_R * s->y1;
  s->x1 = x;
  s->y1 = y;
  return y;
}

/* Time knob (0..1) -> type index, with hysteresis at the zone borders. */
static void update_type(float t)
{
  const float pos = clampf(t, 0.0f, 1.0f) * (float)NUM_TYPES;
  int idx = (int)s_type;

  if (pos > (float)(idx + 1) + TYPE_HYST || pos < (float)idx - TYPE_HYST) {
    idx = (int)pos;
    if (idx > NUM_TYPES - 1) idx = NUM_TYPES - 1;
    if (idx < 0) idx = 0;
  }

  if (idx != (int)s_type) {
    s_type = (uint8_t)idx;
    s_tick = TYPE_CLICK;
  }
}

/* ---- logue SDK callbacks ----------------------------------------------- */
void DELFX_INIT(uint32_t platform, uint32_t api)
{
  (void)platform;
  (void)api;

  for (int i = 0; i < 2; i++) {
    s_dc[i].x1 = 0.0f;
    s_dc[i].y1 = 0.0f;
  }

  s_drive        = 1.0f;
  s_drive_target = 1.0f;
  s_mix          = 0.5f;
  s_mix_target   = 0.5f;
  s_tick         = 0.0f;
  s_type         = 0;
}

void DELFX_PROCESS(float *xn, uint32_t frames)
{
  float *p = xn;

  for (uint32_t n = 0; n < frames; n++) {
    s_drive += (s_drive_target - s_drive) * SMOOTH;
    s_mix   += (s_mix_target   - s_mix)   * SMOOTH;

    const float tick = s_tick;
    if (tick != 0.0f) {
      s_tick = tick * CLICK_DECAY;
      if (s_tick < 1e-4f) s_tick = 0.0f;
    }

    for (int ch = 0; ch < 2; ch++) {
      const float dry = p[ch];
      float wet = shape(s_type, dry * s_drive);
      wet = dc_block(&s_dc[ch], wet) * OUT_TRIM;

      const float out = dry + (wet - dry) * s_mix + tick;

      /* final hard clipper: nothing gets past the ceiling */
      p[ch] = clampf(out, -CEILING, CEILING);
    }

    p += 2;
  }
}

void DELFX_PARAM(uint8_t index, int32_t value)
{
  const float v = q31f(value);

  switch (index) {
    case k_user_delfx_param_time:
      update_type(v);
      break;
    case k_user_delfx_param_depth:
      s_drive_target = drive_gain(v);
      break;
    case k_user_delfx_param_shift_depth:
      s_mix_target = clampf(v, 0.0f, 1.0f);
      break;
    default:
      break;
  }
}
