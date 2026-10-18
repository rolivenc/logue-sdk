/*
 * crunch_delfx.c
 *
 * Multi-character distortion for the minilogue xd delay slot (logue SDK, delfx).
 *
 *   Time        = distortion type (5 zones of 20%)
 *                   1 Tape       dark, glued, low gain, odd harmonics, head bump
 *                   2 Tube       warm, strong 2nd harmonic that stays at any drive
 *                   3 Overdrive  tight lows, mid-forward, dense two-stage clipping
 *                   4 Germanium  hot, bright, asymmetric fuzz with a sputtering gate
 *                   5 Soft Clip  clean cubic clip, full bandwidth, odd harmonics
 *   Depth       = drive (0 dB at 0% up to +DRIVE_MAX_DB at 100%, linear in dB)
 *   Shift+Depth = dry/wet mix
 *
 * Signal flow per channel:
 *   in -> drive gain -> type (shaper + its own filters) -> DC blocker -> type trim
 *      -> dry/wet mix -> HARD CLIP -> out
 *
 * The hard clipper sits at the very end, after the dry/wet sum, so the output
 * can never exceed +/-CEILING no matter how hard the drive is pushed.
 *
 * Changing the Time zone switches the type instantly (no crossfade) and adds
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

/* Envelope follower (bias tracking and germanium gate) */
#define ENV_ATT        0.020618f   /* ~1 ms  */
#define ENV_REL        0.000347f   /* ~60 ms */

/* Tape */
#define TAPE_PRE       1.0f    /* treble pre-emphasis before saturation (HF compresses first) */
#define TAPE_BUMP      0.35f   /* low "head bump" boost                                      */
/* Tube */
#define TUBE_NEG       0.55f   /* negative-side ceiling                                      */
#define TUBE_BIAS      0.15f   /* bias as a fraction of signal level (keeps 2nd harmonic)    */
#define TUBE_WARM      0.25f   /* low-mid warmth                                             */
/* Overdrive */
#define OD_LOW         0.25f   /* how much of the lows still gets driven (tight bass)        */
#define OD_MID         0.80f   /* extra weight of the 300 Hz - 1.5 kHz band (mid hump)       */
#define OD_PRE         2.0f
#define OD_POST        1.3f
#define OD_KNEE        0.50f
#define OD_K           (1.0f / (4.0f * (1.0f - OD_KNEE)))
/* Germanium */
#define GE_PRE         1.0f    /* treble boost before the clip (bright fuzz)                 */
#define GE_BIAS        0.30f   /* bias as a fraction of signal level (strong even harmonics) */
#define GE_NEG_K       2.5f    /* negative side saturates 2.5x earlier                       */
#define GE_TH2         0.0036f /* gate threshold squared (post-drive amplitude 0.06)         */

/* One-pole low-pass coefficients at 48 kHz */
#define A_100          0.013005f
#define A_200          0.025840f
#define A_300          0.038509f
#define A_1K5          0.178275f
#define A_3K           0.324768f
#define A_3K5          0.367547f
#define A_4K5          0.445145f
#define A_5K           0.480297f
#define A_8K           0.649080f

#define LOG2_10_OVER_20 0.16609640474f

/* Per-type gain structure: how hard each type is driven for the same Depth setting.
 * (Tape never fully squares off; germanium is the hottest.) */
static const float k_type_gain[NUM_TYPES] = { 0.40f, 0.70f, 1.00f, 1.60f, 0.60f };

/* Per-type output trims so switching types does not jump in level. */
static const float k_type_trim[NUM_TYPES] = { 0.960f, 0.946f, 0.913f, 1.308f, 0.923f };

/* ---- state ------------------------------------------------------------- */
typedef struct {
  float dc_x1;
  float dc_y1;
  float f[4];     /* per-type one-pole states, cleared on a type change */
  float env;      /* germanium gate envelope */
} chan_t;

static chan_t s_ch[2];

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

static inline float lp(float *s, float a, float x)
{
  *s += a * (x - *s);
  return *s;
}

/* ---- curves ------------------------------------------------------------ */

/* Pade tanh approximation, exact +/-1 at |x| = 3. */
static inline float curve_soft(float x)
{
  x = clampf(x, -3.0f, 3.0f);
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Quadratic knee clip, bounded to +/-1. */
static inline float curve_knee(float x)
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

/* ---- the five types ---------------------------------------------------- */

static inline float track_env(chan_t *c, float ax)
{
  c->env += (ax > c->env ? ENV_ATT : ENV_REL) * (ax - c->env);
  return c->env;
}

/* Tape: treble pre-emphasis -> symmetric soft saturation -> two-pole dark top
 * roll-off -> low head bump. Low gain, so it stays rounded and glued. */
static inline float type_tape(chan_t *c, float x)
{
  const float l = lp(&c->f[0], A_3K, x);
  const float u = x + TAPE_PRE * (x - l);
  float y = curve_soft(u);
  y = lp(&c->f[1], A_3K5, y);
  y = lp(&c->f[2], A_3K5, y);
  y += TAPE_BUMP * lp(&c->f[3], A_100, y);
  return y;
}

/* Tube: signal-proportional bias + asymmetric soft clip, so the 2nd harmonic
 * stays strong at any drive. Smoothed top, low-mid warmth. */
static inline float type_tube(chan_t *c, float x)
{
  const float e  = track_env(c, x < 0.0f ? -x : x);
  const float xb = x + TUBE_BIAS * e;
  float y = xb >= 0.0f ? curve_soft(xb) : TUBE_NEG * curve_soft(xb * (1.0f / TUBE_NEG));
  y = lp(&c->f[0], A_5K, y);
  y += TUBE_WARM * lp(&c->f[1], A_200, y);
  return y;
}

/* Overdrive: lows driven less (tight bass), 300 Hz - 1.5 kHz emphasised,
 * two cascaded clip stages, top smoothed. */
static inline float type_overdrive(chan_t *c, float x)
{
  const float lo = lp(&c->f[0], A_300, x);
  const float lm = lp(&c->f[1], A_1K5, x);
  const float u  = (x - lm) + (1.0f + OD_MID) * (lm - lo) + OD_LOW * lo;
  float y = curve_knee(curve_soft(u * OD_PRE) * OD_POST);
  y = lp(&c->f[2], A_4K5, y);
  return y;
}

/* Germanium: bright pre-emphasis, strong signal-proportional bias, hyperbolic
 * (never fully flat) asymmetric clip, and a level-dependent gate that makes
 * decays sputter and cuts noise between notes. */
static inline float type_germanium(chan_t *c, float x)
{
  const float e  = track_env(c, x < 0.0f ? -x : x);
  const float e2 = e * e;
  const float gate = e2 / (e2 + GE_TH2);

  const float l  = lp(&c->f[0], A_1K5, x);
  const float u  = x + GE_PRE * (x - l);
  const float xb = u + GE_BIAS * e;
  float y = xb >= 0.0f ? xb / (1.0f + xb) : xb / (1.0f - GE_NEG_K * xb);
  y *= gate;
  return lp(&c->f[1], A_8K, y);
}

/* Soft Clip: cubic 1.5u - 0.5u^3, flat beyond |x| = 1.5. Full bandwidth, clean
 * odd harmonics. */
static inline float type_softclip(float x)
{
  const float u = clampf(x * (1.0f / 1.5f), -1.0f, 1.0f);
  return u * (1.5f - 0.5f * u * u);
}

static inline float run_type(uint8_t type, chan_t *c, float x)
{
  switch (type) {
    case 0:  return type_tape(c, x);
    case 1:  return type_tube(c, x);
    case 2:  return type_overdrive(c, x);
    case 3:  return type_germanium(c, x);
    default: return type_softclip(x);
  }
}

static inline float dc_block(chan_t *c, float x)
{
  const float y = x - c->dc_x1 + DC_R * c->dc_y1;
  c->dc_x1 = x;
  c->dc_y1 = y;
  return y;
}

static void clear_type_state(void)
{
  for (int i = 0; i < 2; i++) {
    for (int k = 0; k < 4; k++) s_ch[i].f[k] = 0.0f;
    s_ch[i].env = 0.0f;
  }
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
    clear_type_state();
    s_tick = TYPE_CLICK;
  }
}

/* ---- logue SDK callbacks ----------------------------------------------- */
void DELFX_INIT(uint32_t platform, uint32_t api)
{
  (void)platform;
  (void)api;

  for (int i = 0; i < 2; i++) {
    s_ch[i].dc_x1 = 0.0f;
    s_ch[i].dc_y1 = 0.0f;
  }
  clear_type_state();

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
  const float trim = k_type_trim[s_type] * OUT_TRIM;
  const float tgain = k_type_gain[s_type];

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
      float wet = run_type(s_type, &s_ch[ch], dry * s_drive * tgain);
      wet = dc_block(&s_ch[ch], wet) * trim;

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
