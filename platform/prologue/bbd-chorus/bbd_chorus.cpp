/*
 * BBD Chorus - Juno-60/106 inspired chorus for the Korg minilogue xd / prologue (modfx)
 *
 * Architecture (approximation of the Juno chorus):
 *   mono sum -> input soft-saturation -> pre-BBD low-pass (anti-alias)
 *   -> single delay buffer, two taps modulated by a triangle LFO in
 *      opposite phase (left = +LFO, right = -LFO)
 *   -> BBD noise -> post-BBD low-pass (reconstruction, 4th order)
 *   -> added to the dry stereo signal
 *
 * Modes:  0 = Chorus I   (slow)
 *         1 = Chorus II  (slightly faster)
 *         2 = Chorus I+II (fast, shallow: near-vibrato shimmer)
 *
 * Knobs:  Time  = mode select, in three zones: I | II | I+II
 *         Depth = wet level (0 = dry, max = full Juno-style chorus)
 * (The xd modfx slot only exposes Time and Depth; no extra user params.)
 *
 * Rates, delay times and filter corners are approximations: tune the
 * constants below against a recording of a real Juno.
 */

#include "usermodfx.h"
#include "fixed_math.h"
#include <math.h>

#ifndef DUAL_TIMBRE
#define DUAL_TIMBRE 0   // set to 1 for the prologue (main + sub processed in parallel)
#endif

namespace {

constexpr float    kFs     = 48000.f;
constexpr uint32_t kBufLen = 1024;          // power of two
constexpr uint32_t kMask   = kBufLen - 1;
constexpr float    kPi     = 3.14159265358979f;

// ---- tunable constants ------------------------------------------------
// pre-BBD anti-alias LPF 9 kHz / post-BBD reconstruction LPF 7.5 kHz: see coefficient tables below
constexpr float kWetGain  = 0.90f;    // wet/dry ratio, measured from two recordings (0.85 / 0.95)
constexpr float kOutTrim  = 0.75f;    // ~-2.5 dB trim to compensate for dry+wet summing
constexpr float kNoiseAmt = 0.0015f;  // BBD noise at Grit = 100%
constexpr float kGrit     = 0.25f;    // fixed saturation/noise amount

struct ModeCfg { float rate_hz, center_ms, depth_ms; };
constexpr ModeCfg kModes[3] = {
  {0.51f, 3.5f, 2.10f},  // I    (delay sweeps ~1.4-5.6 ms; Jun-6 Chorus I measured ~0.41 Hz)
  {0.83f, 3.5f, 2.10f},  // II   (Juno recording measured ~0.81 Hz)
  {9.75f, 3.5f, 0.30f},  // I+II (not measured)
};

// Biquad low-pass coefficients, precomputed for fs = 48 kHz (RBJ cookbook) so
// no sinf/cosf is linked (code + data must fit in the 6K modfx SRAM region).
// Change kPreFc/kPostFc here by regenerating these (fc = {9000, 7500} Hz).
constexpr float kPreCoef[5]   = {0.186693626f, 0.373387251f, 0.186693626f, -0.462936271f, 0.209710774f};  // fc 9 kHz,   Q 0.7071
constexpr float kPostCoef1[5] = {0.125674909f, 0.251349819f, 0.125674909f, -0.628411901f, 0.131111538f};  // fc 7.5 kHz, Q 0.5412
constexpr float kPostCoef2[5] = {0.168576961f, 0.337153921f, 0.168576961f, -0.842934909f, 0.517242751f};  // fc 7.5 kHz, Q 1.3066

// ---- biquad low-pass --------------------------------------------------
struct Biquad {
  float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
  float x1 = 0.f, x2 = 0.f, y1 = 0.f, y2 = 0.f;

  void set(const float c[5]) { b0=c[0]; b1=c[1]; b2=c[2]; a1=c[3]; a2=c[4]; }
  void reset() { x1 = x2 = y1 = y2 = 0.f; }
  inline float process(float x) {
    const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    return y;
  }
};

inline float clampf(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

// 4-point, 3rd-order Hermite interpolation. pos must be positive.
inline float readHermite(const float *b, float pos) {
  const uint32_t i  = (uint32_t)pos;
  const float    f  = pos - (float)i;
  const float xm1 = b[(i - 1) & kMask];
  const float x0  = b[i & kMask];
  const float x1  = b[(i + 1) & kMask];
  const float x2  = b[(i + 2) & kMask];
  const float c0 = x0;
  const float c1 = 0.5f * (x1 - xm1);
  const float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
  const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
  return ((c3 * f + c2) * f + c1) * f + c0;
}

inline float triangle(float p) { const float d = p - 0.5f; return 4.f * (d < 0.f ? -d : d) - 1.f; }

// soft saturation with unity small-signal gain
inline float softsat(float x) {
  const float xc = clampf(x * 1.5f, -1.f, 1.f);
  return (xc - xc * xc * xc * (1.f / 3.f)) * (1.f / 1.5f);
}

// ---- one complete chorus channel pair (one timbre) ---------------------
struct Chorus {
  float   *buf;
  uint32_t wr;
  uint32_t noise;
  float    mix;
  Biquad   pre;
  Biquad   postL[2], postR[2];

  inline float whiteNoise() {
    noise ^= noise << 13;
    noise ^= noise >> 17;
    noise ^= noise << 5;
    return (float)(int32_t)noise * (1.f / 2147483648.f);
  }

  void init(float *b, uint32_t seed) {
    buf = b;
    for (uint32_t i = 0; i < kBufLen; ++i) buf[i] = 0.f;
    wr = 0;
    noise = seed;
    mix = 1.f;
    pre.set(kPreCoef);  pre.reset();
    // 4th-order Butterworth = two 2nd-order sections
    postL[0].set(kPostCoef1); postL[0].reset();
    postL[1].set(kPostCoef2); postL[1].reset();
    postR[0].set(kPostCoef1); postR[0].reset();
    postR[1].set(kPostCoef2); postR[1].reset();
  }

  // phase0/inc: shared LFO (both timbres use the same LFO sequence)
  void process(const float *xn, float *yn, uint32_t frames,
               float phase, float inc, const ModeCfg &m, float mixTarget) {
    const float centerSamp = m.center_ms * 0.001f * kFs;
    const float depthSamp  = m.depth_ms  * 0.001f * kFs;
    const float noiseAmt   = kGrit * kNoiseAmt;
    constexpr float kSmooth = 0.002f;
    constexpr float kTiny   = 1e-15f;   // denormal guard

    for (uint32_t n = 0; n < frames; ++n) {
      const float inL = xn[2 * n];
      const float inR = xn[2 * n + 1];

      mix += kSmooth * (mixTarget - mix);

      // mono sum -> input saturation (blended by Grit) -> pre filter
      float mono = 0.5f * (inL + inR);
      mono += kGrit * (softsat(mono) - mono);
      buf[wr] = pre.process(mono + kTiny);

      // opposite-phase triangle LFOs
      phase += inc;
      if (phase >= 1.f) phase -= 1.f;
      const float lfo = triangle(phase);

      const float dL = centerSamp + depthSamp * lfo;
      const float dR = centerSamp - depthSamp * lfo;

      float wL = readHermite(buf, (float)(wr + kBufLen) - dL);
      float wR = readHermite(buf, (float)(wr + kBufLen) - dR);

      // BBD noise, then reconstruction filters
      wL += noiseAmt * whiteNoise();
      wR += noiseAmt * whiteNoise();
      wL = postL[1].process(postL[0].process(wL));
      wR = postR[1].process(postR[0].process(wR));

      wr = (wr + 1) & kMask;

      const float wet = kWetGain * mix;
      yn[2 * n]     = kOutTrim * (inL + wet * wL);
      yn[2 * n + 1] = kOutTrim * (inR + wet * wR);
    }
  }
};

// ---- state ------------------------------------------------------------
// delay lines live in SDRAM (modfx SRAM is only 6K)
float s_bufMain[kBufLen] __sdram;
#if DUAL_TIMBRE
float s_bufSub[kBufLen] __sdram;
#endif

Chorus  s_main;
#if DUAL_TIMBRE
Chorus  s_sub;
#endif
float   s_phase;
float   s_mixTarget;
uint8_t s_mode;

} // namespace

// ---- logue SDK entry points -------------------------------------------

void MODFX_INIT(uint32_t platform, uint32_t api) {
  (void)platform; (void)api;
  s_main.init(s_bufMain, 0x12345678u);
#if DUAL_TIMBRE
  s_sub.init(s_bufSub, 0x9E3779B9u);
#endif
  s_phase     = 0.f;
  s_mixTarget = 1.f;
  s_mode      = 0;
}

void MODFX_PROCESS(const float *main_xn, float *main_yn,
                   const float *sub_xn,  float *sub_yn,
                   uint32_t frames) {
  const ModeCfg &m   = kModes[s_mode];
  const float    inc = m.rate_hz / kFs;

  s_main.process(main_xn, main_yn, frames, s_phase, inc, m, s_mixTarget);
#if DUAL_TIMBRE
  s_sub.process(sub_xn, sub_yn, frames, s_phase, inc, m, s_mixTarget);
#else
  (void)sub_xn; (void)sub_yn;   // xd: no sub timbre
#endif

  // advance the shared LFO once per buffer
  s_phase += inc * (float)frames;
  if (s_phase >= 1.f) s_phase -= 1.f;
}

void MODFX_PARAM(uint8_t index, int32_t value) {
  const float valf = q31_to_f32(value);
  switch (index) {
    case k_user_modfx_param_time:                    // mode, 3 zones
      s_mode = (valf < 0.333f) ? 0 : (valf < 0.667f ? 1 : 2);
      break;
    case k_user_modfx_param_depth:                   // wet level
      s_mixTarget = clampf(valf, 0.f, 1.f);
      break;
    default:
      break;
  }
}
