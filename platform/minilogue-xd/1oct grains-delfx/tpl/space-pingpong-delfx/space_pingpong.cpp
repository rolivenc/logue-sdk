/*
 *  Space Ping Pong - tempo synced stereo tape ping-pong delay for the Korg
 *  minilogue xd (delfx slot).
 *
 *  Same tape voicing as Space Echo (Roland RE-201 inspired): three playback
 *  heads at 1:2:3 spacing, filtered + saturated repeats, wow/flutter and
 *  tape-speed pitch bends when the time or tempo changes.
 *
 *  Ping-pong is done with two tapes, A (left) and B (right):
 *    input -> record A
 *    heads of A -> left output, and cross-fed to record B
 *    heads of B -> right output, and fed back to record A (intensity)
 *  so every repeat hops to the opposite side. The first right-side echo
 *  arrives one head-delay after the first left-side echo.
 *
 *  Controls (the xd delfx slot only exposes these three):
 *    TIME         21-position selector = 3 speed bands x 7 head modes
 *    DEPTH        Intensity (feedback). Top of the knob self-oscillates.
 *    SHIFT+DEPTH  Dry/Wet
 *
 *  TIME positions (knob 0..100 pct -> position 0..20):
 *    band = position / 7      0: U = 1/16 note   1: U = 1/8 note   2: U = 1/4 note
 *    mode = position % 7      0: H1   1: H2   2: H3   3: H1+H2   4: H2+H3
 *                             5: H1+H3        6: H1+H2+H3
 *    Head delays are U, 2U and 3U per tape.
 */

#include "userdelfx.h"
#include "buffer_ops.h"
#include "float_math.h"
#include "fixed_math.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define SAMPLE_RATE      48000.f

// Two tapes of 2^18 samples (~5.4 s each, 2 MB total of the 2432 KB SDRAM).
// Longest head = 3 beats @ 40 BPM = 216000 samples.
#define BUF_SIZE         (1U << 18)
#define BUF_MASK         (BUF_SIZE - 1U)

#define NUM_BANDS        3
#define NUM_MODES        7
#define NUM_POSITIONS    (NUM_BANDS * NUM_MODES)

#define BPM_MIN          40.f
#define BPM_MAX          300.f
#define BPM_DEFAULT      120.f

// Tape speed slewing: exponential approach, rate limited so that the fastest
// head (3U) never plays back faster than 1.75x or slower than 0.25x.
#define U_SLEW_MAX       0.25f      // samples of U per sample

// Wow / flutter / drift (depths are delay modulation in samples). Tape B uses
// the same oscillators at a phase offset, which gives a little stereo life.
#define WOW_HZ           0.83f
#define WOW_DEPTH        13.f
#define FLUTTER_HZ       8.7f
#define FLUTTER_DEPTH    0.45f
#define DRIFT_HZ         0.21f
#define DRIFT_DEPTH      6.f
#define B_PHASE_OFFSET   0.37f

#define TAPE_DRIVE       1.15f
#define FB_MAX           1.08f      // >1.0 so the top of the knob self-oscillates

// A->B cross hop gain = CROSS_MIN + (1 - CROSS_MIN) * intensity, so the ping
// pong still bounces once or twice with intensity at zero.
#define CROSS_MIN        0.7f

// ---------------------------------------------------------------------------
// State (plain data only: no static constructors on this target)
// ---------------------------------------------------------------------------

static float s_buf_a[BUF_SIZE] __sdram;
static float s_buf_b[BUF_SIZE] __sdram;

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
  bool     snap;
  float    gain[3];       // smoothed head gains
  float    fb_norm;       // smoothed feedback normalisation
  float    fb;            // smoothed feedback hop gain (B -> A)
  float    cross;         // smoothed cross hop gain (A -> B)
  float    dry, wet;      // smoothed mix gains

  // modulation phases
  float    ph_wow, ph_fl, ph_dr;

  // filters: [0] = A->B hop, [1] = B->A hop
  float    lp_z[2];
  float    hp_z[2];
  float    out_z[2];      // output low-pass, L and R

  // coefficients
  float    a_lp, a_hp, a_out;
  float    a_u;
  float    inc_wow, inc_fl, inc_dr;
} SpacePingPong;

static SpacePingPong s;

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

// Output safety: transparent below 0.75, smooth knee up to a ceiling of 1.0.
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

// Read a tape `delay` samples behind the write position (delay >= 3). The
// delay is split into integer and fractional parts so the interpolation keeps
// full float precision wherever the write head is.
static inline float tape_read(const float *buf, uint32_t widx, float delay) {
  const uint32_t di = (uint32_t)delay;
  const float    df = delay - (float)di;
  const int32_t  i0 = (int32_t)(widx - di - 1U);
  return hermite(buf, i0, 1.f - df);
}

// Sum of the active heads of one tape.
static inline float read_heads(const float *buf, uint32_t widx, float u, float mod) {
  float h = 0.f;
  if (s.gain[0] > 1.0e-4f) h += s.gain[0] * tape_read(buf, widx,       u + mod + 4.f);
  if (s.gain[1] > 1.0e-4f) h += s.gain[1] * tape_read(buf, widx, 2.f * u + mod + 4.f);
  if (s.gain[2] > 1.0e-4f) h += s.gain[2] * tape_read(buf, widx, 3.f * u + mod + 4.f);
  return h;
}

// Tape loop voicing for one hop: HF loss + head bump (low cut).
static inline float loop_filter(int ch, float x) {
  s.lp_z[ch] += (x - s.lp_z[ch]) * s.a_lp;
  s.lp_z[ch]  = flush(s.lp_z[ch]);
  s.hp_z[ch] += (s.lp_z[ch] - s.hp_z[ch]) * s.a_hp;
  s.hp_z[ch]  = flush(s.hp_z[ch]);
  return s.lp_z[ch] - s.hp_z[ch];
}

static void update_head_targets(float *targets, float *fb_norm_target) {
  const uint8_t m = k_head_masks[s.mode];
  const int n = (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1);
  static const float k_inv_sqrt[4] = { 1.f, 1.f, 0.70710678f, 0.57735027f };
  const float g = k_inv_sqrt[n];              // equal-power head sum
  targets[0] = (m & 1) ? g : 0.f;
  targets[1] = (m & 2) ? g : 0.f;
  targets[2] = (m & 4) ? g : 0.f;
  // n heads at gain g sum to sqrt(n); normalise so unity intensity is unity
  // loop gain whatever the head mode.
  *fb_norm_target = g;
}

// ---------------------------------------------------------------------------
// Unit hooks
// ---------------------------------------------------------------------------

void DELFX_INIT(uint32_t platform, uint32_t api)
{
  (void)platform;
  (void)api;

  buf_clr_f32(s_buf_a, BUF_SIZE);
  buf_clr_f32(s_buf_b, BUF_SIZE);

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
  s.fb    = s.intensity * FB_MAX;
  s.cross = CROSS_MIN + (1.f - CROSS_MIN) * s.intensity;
  s.dry = fx_cosf(s.mix * 0.25f);
  s.wet = fx_sinf(s.mix * 0.25f);

  s.ph_wow = 0.f;
  s.ph_fl  = 0.31f;
  s.ph_dr  = 0.67f;

  for (int i = 0; i < 2; ++i) { s.lp_z[i] = 0.f; s.hp_z[i] = 0.f; s.out_z[i] = 0.f; }

  // One-pole coefficients, a = 1 - exp(-2*pi*fc/fs), precomputed so that the
  // unit does not need libm (it would pull in newlib syscalls at link time).
  s.a_lp  = 0.493726584f;    // loop low-pass  5200 Hz
  s.a_hp  = 0.012358470f;    // loop high-pass   95 Hz
  s.a_out = 0.711640436f;   // output low-pass 9500 Hz
  s.a_u   = 0.000173596f;     // tape speed slew, tau = 0.12 s

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

  const float fb_t    = s.intensity * FB_MAX;
  const float cross_t = CROSS_MIN + (1.f - CROSS_MIN) * s.intensity;
  const float dry_t   = fx_cosf(s.mix * 0.25f);   // equal-power crossfade
  const float wet_t   = fx_sinf(s.mix * 0.25f);

  float *p = xn;
  for (uint32_t n = 0; n < frames; ++n, p += 2) {
    // --- smoothing -----------------------------------------------------------
    for (int i = 0; i < 3; ++i) s.gain[i] += (gain_t[i] - s.gain[i]) * k_a_gain;
    s.fb_norm += (fbn_t - s.fb_norm) * k_a_gain;
    s.fb    += (fb_t    - s.fb)    * k_a_ctl;
    s.cross += (cross_t - s.cross) * k_a_ctl;
    s.dry   += (dry_t   - s.dry)   * k_a_ctl;
    s.wet   += (wet_t   - s.wet)   * k_a_ctl;

    // Tape speed change: exponential approach with a rate limit, so changing
    // the time/tempo bends the pitch of the repeats.
    float step = (u_target - s.u) * s.a_u;
    if (step >  U_SLEW_MAX) step =  U_SLEW_MAX;
    if (step < -U_SLEW_MAX) step = -U_SLEW_MAX;
    s.u += step;

    // --- wow / flutter / drift (tape B at a phase offset) --------------------
    s.ph_wow += s.inc_wow; s.ph_wow -= (float)(uint32_t)s.ph_wow;
    s.ph_fl  += s.inc_fl;  s.ph_fl  -= (float)(uint32_t)s.ph_fl;
    s.ph_dr  += s.inc_dr;  s.ph_dr  -= (float)(uint32_t)s.ph_dr;
    const float mod_a = WOW_DEPTH     * fx_sinf(s.ph_wow)
                      + FLUTTER_DEPTH * fx_sinf(s.ph_fl)
                      + DRIFT_DEPTH   * fx_sinf(s.ph_dr);
    const float mod_b = WOW_DEPTH     * fx_sinf(s.ph_wow + B_PHASE_OFFSET)
                      + FLUTTER_DEPTH * fx_sinf(s.ph_fl  + B_PHASE_OFFSET)
                      + DRIFT_DEPTH   * fx_sinf(s.ph_dr  + B_PHASE_OFFSET);

    // --- playback heads of both tapes ----------------------------------------
    const float head_a = read_heads(s_buf_a, s.widx, s.u, mod_a);
    const float head_b = read_heads(s_buf_b, s.widx, s.u, mod_b);

    // --- hops: A -> B (cross) and B -> A (feedback), each through the tape ---
    const float to_b = loop_filter(0, head_a * s.fb_norm * s.cross);
    const float to_a = loop_filter(1, head_b * s.fb_norm * s.fb);

    // --- record heads ---------------------------------------------------------
    const float in = 0.5f * (p[0] + p[1]);
    s.widx = (s.widx + 1U) & BUF_MASK;
    s_buf_a[s.widx] = tape_sat((in + to_a) * TAPE_DRIVE);
    s_buf_b[s.widx] = tape_sat(to_b * TAPE_DRIVE);

    // --- output: tape A on the left, tape B on the right ----------------------
    s.out_z[0] += (head_a - s.out_z[0]) * s.a_out;
    s.out_z[0]  = flush(s.out_z[0]);
    s.out_z[1] += (head_b - s.out_z[1]) * s.a_out;
    s.out_z[1]  = flush(s.out_z[1]);

    p[0] = out_limit(p[0] * s.dry + s.out_z[0] * s.wet);
    p[1] = out_limit(p[1] * s.dry + s.out_z[1] * s.wet);
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
  // xd user-effect parameters arrive as Q31 (0 .. 0x7FFFFFFF), NOT 10-bit.
  float valf = q31_to_f32(value);
  if (valf < 0.f) valf = 0.f;
  if (valf > 1.f) valf = 1.f;

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
