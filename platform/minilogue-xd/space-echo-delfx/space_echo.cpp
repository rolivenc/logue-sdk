/*
 *  Space Echo - tempo synced tape delay for the Korg minilogue xd (delfx slot)
 *
 *  Inspired by the Roland RE-201 Space Echo: three playback heads at 1:2:3
 *  spacing, a feedback loop that is filtered and saturated on every pass,
 *  wow/flutter, and tape-speed pitch bends whenever the delay time changes.
 *
 *  Controls (the xd delfx slot only exposes these three):
 *    TIME         21-position selector = 3 speed bands x 7 head modes
 *    DEPTH        Intensity (feedback). Top of the knob runs away into
 *                 saturated self-oscillation.
 *    SHIFT+DEPTH  Dry/Wet
 *
 *  TIME positions (knob 0..1023 -> position 0..20):
 *    band = position / 7      0: U = 1/16 note   1: U = 1/8 note   2: U = 1/4 note
 *    mode = position % 7      0: H1   1: H2   2: H3   3: H1+H2   4: H2+H3
 *                             5: H1+H3        6: H1+H2+H3
 *    Head delays are U, 2U and 3U (e.g. band 1: 1/8, 1/4, dotted 1/4).
 */

#include "userdelfx.h"
#include "buffer_ops.h"
#include "float_math.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define SAMPLE_RATE      48000.f

// Tape buffer: 2^18 samples (~5.4 s), mono. Longest head = 3 beats @ 40 BPM
// = 216000 samples, which fits with margin for modulation + interpolation.
#define BUF_SIZE         (1U << 18)
#define BUF_MASK         (BUF_SIZE - 1U)

#define NUM_BANDS        3
#define NUM_MODES        7
#define NUM_POSITIONS    (NUM_BANDS * NUM_MODES)

#define BPM_MIN          40.f
#define BPM_MAX          300.f
#define BPM_DEFAULT      120.f

// Tape speed slewing: exponential approach, capped so that the fastest head
// (3U) never plays back faster than 1.75x or slower than 0.25x.
#define U_SLEW_TAU_SEC   0.12f
#define U_SLEW_MAX       0.25f      // samples of U per sample

// Wow / flutter (depths are delay modulation in samples, see notes)
#define WOW_HZ           0.83f
#define WOW_DEPTH        13.f
#define FLUTTER_HZ       8.7f
#define FLUTTER_DEPTH    0.45f
#define DRIFT_HZ         0.21f
#define DRIFT_DEPTH      6.f

// Loop voicing
#define LOOP_LP_HZ       5200.f
#define LOOP_HP_HZ       95.f
#define OUT_LP_HZ        9500.f

#define TAPE_DRIVE       1.15f
#define FB_MAX           1.08f      // >1.0 so the top of the knob self-oscillates

// ---------------------------------------------------------------------------
// State (plain data only: no static constructors on this target)
// ---------------------------------------------------------------------------

static float s_buf[BUF_SIZE] __sdram;

static const uint8_t k_head_masks[NUM_MODES] = { 1, 2, 4, 3, 6, 5, 7 };
static const float   k_unit_beats[NUM_BANDS] = { 0.25f, 0.5f, 1.f };

typedef struct {
  uint32_t widx;

  // parameters
  uint8_t  band;
  uint8_t  mode;
  float    intensity;     // 0..1
  float    mix;           // 0..1

  // smoothed values
  float    u;             // current tape unit delay in samples
  bool     snap;          // jump u to its target on the next block
  float    gain[3];       // smoothed head gains
  float    fb_norm;       // smoothed feedback normalisation
  float    fb;            // smoothed feedback amount
  float    dry, wet;      // smoothed mix gains

  // modulation phases
  float    ph_wow, ph_fl, ph_dr;

  // filters
  float    lp_z;          // loop low-pass state
  float    hp_z;          // loop high-pass (low-pass subtracted) state
  float    out_z;         // output low-pass state

  // coefficients (computed in init)
  float    a_lp, a_hp, a_out;
  float    a_u;
  float    inc_wow, inc_fl, inc_dr;
} SpaceEcho;

static SpaceEcho s;

// Parameter smoothing coefficients (per sample)
static const float k_a_gain = 0.004f;   // ~5 ms head fades
static const float k_a_ctl  = 0.002f;   // ~10 ms knob smoothing

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline float flush(float x) {
  return (fabsf(x) < 1.0e-15f) ? 0.f : x;
}

// Tape-style saturation: rational tanh approximation, exact +-1 at |x| = 3.
static inline float tape_sat(float x) {
  if (x > 3.f)  return 1.f;
  if (x < -3.f) return -1.f;
  const float x2 = x * x;
  return x * (27.f + x2) / (27.f + 9.f * x2);
}

// Output safety: transparent below 0.75, smooth knee up to a hard ceiling of
// 1.0, so runaway self-oscillation can not clip the DAC.
static inline float out_limit(float x) {
  const float a = fabsf(x);
  if (a <= 0.75f) return x;
  const float y = 0.75f + 0.25f * tape_sat((a - 0.75f) * 4.f);
  return (x < 0.f) ? -y : y;
}

// 4-point, 3rd order Hermite interpolation. Reads between i0 and i0+1.
static inline float hermite(const float *buf, int32_t i0, float f) {
  const float ym1 = buf[(uint32_t)(i0 - 1) & BUF_MASK];
  const float y0  = buf[(uint32_t)(i0    ) & BUF_MASK];
  const float y1  = buf[(uint32_t)(i0 + 1) & BUF_MASK];
  const float y2  = buf[(uint32_t)(i0 + 2) & BUF_MASK];

  const float c1 = 0.5f * (y1 - ym1);
  const float c2 = ym1 - 2.5f * y0 + 2.f * y1 - 0.5f * y2;
  const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
  return ((c3 * f + c2) * f + c1) * f + y0;
}

// Read the tape `delay` samples behind the write position (delay >= 3).
// The delay is split into integer and fractional parts so the interpolation
// keeps full float precision regardless of where the write head is.
static inline float tape_read(uint32_t widx, float delay) {
  const uint32_t di = (uint32_t)delay;
  const float    df = delay - (float)di;
  const int32_t  i0 = (int32_t)(widx - di - 1U);
  return hermite(s_buf, i0, 1.f - df);
}

static void update_head_targets(float *targets, float *fb_norm_target) {
  const uint8_t m = k_head_masks[s.mode];
  const int n = (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1);
  static const float k_inv_sqrt[4] = { 1.f, 1.f, 0.70710678f, 0.57735027f };
  const float g = k_inv_sqrt[n];              // equal-power head sum
  targets[0] = (m & 1) ? g : 0.f;
  targets[1] = (m & 2) ? g : 0.f;
  targets[2] = (m & 4) ? g : 0.f;
  // With n heads at gain g the feedback sum is n*g = sqrt(n); normalise so
  // that unity intensity is unity loop gain whatever the head mode.
  *fb_norm_target = g;
}

// ---------------------------------------------------------------------------
// Unit hooks
// ---------------------------------------------------------------------------

void DELFX_INIT(uint32_t platform, uint32_t api)
{
  (void)platform;
  (void)api;

  buf_clr_f32(s_buf, BUF_SIZE);

  s.widx = 0;

  s.band = 1;
  s.mode = 6;
  s.intensity = 0.35f;
  s.mix = 0.5f;

  s.u = 0.5f * (60.f / BPM_DEFAULT) * SAMPLE_RATE;
  s.snap = true;

  float targets[3], fbn;
  update_head_targets(targets, &fbn);
  for (int i = 0; i < 3; ++i) s.gain[i] = targets[i];
  s.fb_norm = fbn;
  s.fb  = s.intensity * FB_MAX;
  s.dry = fx_cosf(s.mix * 0.25f);
  s.wet = fx_sinf(s.mix * 0.25f);

  s.ph_wow = 0.f;
  s.ph_fl  = 0.31f;
  s.ph_dr  = 0.67f;

  s.lp_z = s.hp_z = s.out_z = 0.f;

  // One-pole coefficients, a = 1 - exp(-2*pi*fc/fs), precomputed so that the
  // unit does not need libm (it would pull in newlib syscalls at link time).
  s.a_lp  = 0.493726584f;   // LOOP_LP_HZ  = 5200 Hz
  s.a_hp  = 0.012358470f;   // LOOP_HP_HZ  = 95 Hz
  s.a_out = 0.711640436f;   // OUT_LP_HZ   = 9500 Hz
  s.a_u   = 0.000173596f;   // U_SLEW_TAU_SEC = 0.12 s

  s.inc_wow = WOW_HZ     / SAMPLE_RATE;
  s.inc_fl  = FLUTTER_HZ / SAMPLE_RATE;
  s.inc_dr  = DRIFT_HZ   / SAMPLE_RATE;
}

void DELFX_PROCESS(float *xn, uint32_t frames)
{
  // --- tempo ---------------------------------------------------------------
  float bpm = fx_get_bpmf();
  if (!(bpm >= BPM_MIN)) bpm = (bpm > 0.f) ? BPM_MIN : BPM_DEFAULT;
  if (bpm > BPM_MAX) bpm = BPM_MAX;

  const float u_target = (60.f / bpm) * k_unit_beats[s.band] * SAMPLE_RATE;
  if (s.snap) {
    s.u = u_target;
    s.snap = false;
  }

  float gain_t[3], fbn_t;
  update_head_targets(gain_t, &fbn_t);

  const float fb_t  = s.intensity * FB_MAX;
  const float dry_t = fx_cosf(s.mix * 0.25f);   // equal-power crossfade
  const float wet_t = fx_sinf(s.mix * 0.25f);

  float *p = xn;
  for (uint32_t n = 0; n < frames; ++n, p += 2) {
    // --- smoothing -----------------------------------------------------------
    for (int i = 0; i < 3; ++i) s.gain[i] += (gain_t[i] - s.gain[i]) * k_a_gain;
    s.fb_norm += (fbn_t - s.fb_norm) * k_a_gain;
    s.fb  += (fb_t  - s.fb)  * k_a_ctl;
    s.dry += (dry_t - s.dry) * k_a_ctl;
    s.wet += (wet_t - s.wet) * k_a_ctl;

    // Tape speed change: exponential approach with a rate limit, so changing
    // the time/tempo bends the pitch of the repeats like turning the repeat
    // rate knob on the real thing.
    float step = (u_target - s.u) * s.a_u;
    if (step >  U_SLEW_MAX) step =  U_SLEW_MAX;
    if (step < -U_SLEW_MAX) step = -U_SLEW_MAX;
    s.u += step;

    // --- wow / flutter / drift -------------------------------------------------
    s.ph_wow += s.inc_wow; s.ph_wow -= (float)(uint32_t)s.ph_wow;
    s.ph_fl  += s.inc_fl;  s.ph_fl  -= (float)(uint32_t)s.ph_fl;
    s.ph_dr  += s.inc_dr;  s.ph_dr  -= (float)(uint32_t)s.ph_dr;
    const float mod = WOW_DEPTH     * fx_sinf(s.ph_wow)
                    + FLUTTER_DEPTH * fx_sinf(s.ph_fl)
                    + DRIFT_DEPTH   * fx_sinf(s.ph_dr);

    // --- playback heads (1U, 2U, 3U) --------------------------------------------
    const float d1 = s.u + mod + 4.f;
    const float d2 = 2.f * s.u + mod + 4.f;
    const float d3 = 3.f * s.u + mod + 4.f;

    float heads = 0.f;
    if (s.gain[0] > 1.0e-4f) heads += s.gain[0] * tape_read(s.widx, d1);
    if (s.gain[1] > 1.0e-4f) heads += s.gain[1] * tape_read(s.widx, d2);
    if (s.gain[2] > 1.0e-4f) heads += s.gain[2] * tape_read(s.widx, d3);

    // --- feedback path: head bump / HF loss, then back to the record head -----
    float fbs = heads * s.fb_norm * s.fb;
    s.lp_z += (fbs - s.lp_z) * s.a_lp;
    s.lp_z  = flush(s.lp_z);
    s.hp_z += (s.lp_z - s.hp_z) * s.a_hp;
    s.hp_z  = flush(s.hp_z);
    fbs = s.lp_z - s.hp_z;

    // --- record head ------------------------------------------------------------
    const float in = 0.5f * (p[0] + p[1]);
    s.widx = (s.widx + 1U) & BUF_MASK;
    s_buf[s.widx] = tape_sat((in + fbs) * TAPE_DRIVE);

    // --- output ---------------------------------------------------------------------
    s.out_z += (heads - s.out_z) * s.a_out;
    s.out_z  = flush(s.out_z);
    const float wetsig = s.out_z * s.wet;

    p[0] = out_limit(p[0] * s.dry + wetsig);
    p[1] = out_limit(p[1] * s.dry + wetsig);
  }
}

void DELFX_SUSPEND(void)
{
}

void DELFX_RESUME(void)
{
  // Snap to the current tempo instead of swooping in from a stale value.
  s.snap = true;
}

void DELFX_PARAM(uint8_t index, int32_t value)
{
  const float valf = (float)((uint16_t)value & 0x3FF) * (1.f / 1023.f);

  switch (index) {
  case k_user_delfx_param_time: {
    int pos = (int)(valf * (float)NUM_POSITIONS);
    if (pos < 0) pos = 0;
    if (pos > NUM_POSITIONS - 1) pos = NUM_POSITIONS - 1;
    s.band = (uint8_t)(pos / NUM_MODES);
    s.mode = (uint8_t)(pos % NUM_MODES);
    break;
  }
  case k_user_delfx_param_depth:
    s.intensity = valf;
    break;

  case k_user_delfx_param_shift_depth:
    s.mix = valf;
    break;

  default:
    break;
  }
}
