/*
 * grains_octave_feedback - tempo-synced granular delay for the Korg minilogue xd
 *
 * Based on grains-delfx. This variant adds a separate feedback grain engine
 * that reads the delay buffer at 2x speed (+12 semitones). Only the feedback
 * signal is octave-shifted; the main wet grains retain their normal pitch,
 * reverse playback probability, and subtle detuning.
 *
 * Controls:
 * TIME        : Synced grain length/division
 * DEPTH       : Feedback amount (octave-up feedback)
 * SHIFT+DEPTH : Dry/wet mix
 */

#include "userdelfx.h"

#define kSampleRate             48000.f

#define kBufLog2                18
#define kBufSize                (1u << kBufLog2)
#define kBufMask                (kBufSize - 1u)

#define kRevOffset              128u
#define kMaxGrain               ((kBufSize - 2u * kRevOffset) / 2u - 16u)
#define kMinGrain               32u

#define kNumGrains              4
#define kNumFeedbackGrains      2
#define kReverseChance          0.80f
#define kFeedbackPitchRatio     2.0f       // 2x playback = +12 semitones

#define kMaxFeedback            0.94f
#define kDampCoef               0.55f
#define kDcCoef                 0.995f

#define kSmoothCoef             0.002f
#define kLimitKnee              0.8f

#define kStereoOffset           12.0f
#define kMaxGrainDetuneCents    8.0f

#define kNumDivs 11

static const float kBeats[kNumDivs] = {
    0.125f,        // 1/32
    1.f / 6.f,     // 1/16T
    0.25f,         // 1/16
    1.f / 3.f,     // 1/8T
    0.5f,          // 1/8
    2.f / 3.f,     // 1/4T
    0.75f,         // 1/8.
    1.f,           // 1/4
    4.f / 3.f,     // 1/2T
    1.5f,          // 1/4.
    2.f            // 1/2
};

typedef struct {
    uint32_t len;
    uint32_t age;
    float readPosL;
    float readPosR;
    float readStepL;
    float readStepR;
    float inv_len;
    bool rev;
    bool active;
} grain_t;

static float s_bufL[kBufSize] __sdram;
static float s_bufR[kBufSize] __sdram;

static grain_t s_grain[kNumGrains];
static grain_t s_fbGrain[kNumFeedbackGrains];

static uint32_t s_w;
static uint32_t s_spawn;
static uint32_t s_fbSpawn;
static uint32_t s_delay;
static uint32_t s_rng;

static uint32_t s_div;

static float s_fb, s_fb_t;
static float s_mix, s_mix_t;

static float s_lpL, s_lpR;
static float s_dcxL, s_dcyL;
static float s_dcxR, s_dcyR;

static float s_fbLpL, s_fbLpR;
static float s_fbDcxL, s_fbDcyL;
static float s_fbDcxR, s_fbDcyR;

static inline float rand01(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return (float)(x >> 8) * (1.f / 16777216.f);
}

static inline float grain_pitch_ratio(void)
{
    const float cents = (rand01() * 2.f - 1.f) * kMaxGrainDetuneCents;
    return 1.f + cents * 0.00057762265f;
}

static inline float read_interp(const float *buf, float pos)
{
    const uint32_t i0 = (uint32_t)pos & kBufMask;
    const uint32_t i1 = (i0 + 1u) & kBufMask;
    const float frac = pos - (float)(uint32_t)pos;
    return buf[i0] + frac * (buf[i1] - buf[i0]);
}

static inline float soft_limit(float x)
{
    const float ax = (x < 0.f) ? -x : x;
    if (ax <= kLimitKnee)
        return x;
    const float t = (ax - kLimitKnee) * (1.f / (1.f - kLimitKnee));
    const float y = kLimitKnee + (1.f - kLimitKnee) * (t / (1.f + t));
    return (x < 0.f) ? -y : y;
}

static inline float window_for_age(uint32_t age, float inv_len)
{
    const float s = fx_sinf(0.5f * (float)age * inv_len);
    return s * s;
}

static void clear_state(void)
{
    for (uint32_t i = 0; i < kBufSize; ++i) {
        s_bufL[i] = 0.f;
        s_bufR[i] = 0.f;
    }
    for (int i = 0; i < kNumGrains; ++i)
        s_grain[i].active = false;
    for (int i = 0; i < kNumFeedbackGrains; ++i)
        s_fbGrain[i].active = false;

    s_w = 0;
    s_spawn = 0;
    s_fbSpawn = 0;

    s_lpL = s_lpR = 0.f;
    s_dcxL = s_dcyL = 0.f;
    s_dcxR = s_dcyR = 0.f;

    s_fbLpL = s_fbLpR = 0.f;
    s_fbDcxL = s_fbDcyL = 0.f;
    s_fbDcxR = s_fbDcyR = 0.f;
}

static void spawn_grain(void)
{
    const uint32_t len = s_delay;

    for (int i = 0; i < kNumGrains; ++i) {
        grain_t &g = s_grain[i];
        if (g.active)
            continue;

        g.len = len;
        g.inv_len = 1.f / (float)len;
        g.age = 0;
        g.rev = (rand01() < kReverseChance);
        g.readStepL = grain_pitch_ratio();
        g.readStepR = grain_pitch_ratio();

        const float w = (float)s_w;
        if (g.rev) {
            g.readPosL = w - (float)kRevOffset;
            g.readPosR = w - (float)kRevOffset - kStereoOffset;
        } else {
            g.readPosL = w - (float)len * g.readStepL;
            g.readPosR = w - (float)len * g.readStepR - kStereoOffset;
        }

        while (g.readPosL < 0.f) g.readPosL += (float)kBufSize;
        while (g.readPosL >= (float)kBufSize) g.readPosL -= (float)kBufSize;
        while (g.readPosR < 0.f) g.readPosR += (float)kBufSize;
        while (g.readPosR >= (float)kBufSize) g.readPosR -= (float)kBufSize;

        g.active = true;
        break;
    }

    s_spawn = len >> 1;
    if (s_spawn == 0) s_spawn = 1;
}

/*
 * Feedback grains always read forward at 2x speed. Each grain spans `len`
 * output samples while traversing 2*len samples from the buffer, producing
 * an octave-up copy for the feedback path only.
 */
static void spawn_feedback_grain(void)
{
    const uint32_t len = s_delay;

    for (int i = 0; i < kNumFeedbackGrains; ++i) {
        grain_t &g = s_fbGrain[i];
        if (g.active)
            continue;

        g.len = len;
        g.inv_len = 1.f / (float)len;
        g.age = 0;
        g.rev = false;
        g.readStepL = kFeedbackPitchRatio;
        g.readStepR = kFeedbackPitchRatio;

        const float w = (float)s_w;
        g.readPosL = w - 2.f * (float)len;
        g.readPosR = w - 2.f * (float)len - kStereoOffset;

        while (g.readPosL < 0.f) g.readPosL += (float)kBufSize;
        while (g.readPosL >= (float)kBufSize) g.readPosL -= (float)kBufSize;
        while (g.readPosR < 0.f) g.readPosR += (float)kBufSize;
        while (g.readPosR >= (float)kBufSize) g.readPosR -= (float)kBufSize;

        g.active = true;
        break;
    }

    s_fbSpawn = len >> 1;
    if (s_fbSpawn == 0) s_fbSpawn = 1;
}

void DELFX_INIT(uint32_t platform, uint32_t api)
{
    (void)platform;
    (void)api;

    s_rng = 0x2545F491u;
    s_div = 7;
    s_fb = s_fb_t = 0.f;
    s_mix = s_mix_t = 0.5f;
    s_delay = 24000;
    clear_state();
}

void DELFX_PROCESS(float *xn, uint32_t frames)
{
    float bpm = fx_get_bpmf();
    if (!(bpm >= 30.f)) bpm = 120.f;
    if (bpm > 300.f) bpm = 300.f;

    uint32_t d = (uint32_t)(60.f * kSampleRate / bpm * kBeats[s_div] + 0.5f);
    if (d < kMinGrain) d = kMinGrain;
    if (d > kMaxGrain) d = kMaxGrain;
    s_delay = d;

    float *__restrict x = xn;

    for (uint32_t f = 0; f < frames; ++f, x += 2) {
        s_fb += kSmoothCoef * (s_fb_t - s_fb);
        s_mix += kSmoothCoef * (s_mix_t - s_mix);

        const float inL = x[0];
        const float inR = x[1];
        const uint32_t w = s_w;

        if (s_spawn == 0) spawn_grain();
        else --s_spawn;
        if (s_fbSpawn == 0) spawn_feedback_grain();
        else --s_fbSpawn;

        float wetL = 0.f, wetR = 0.f;
        for (int i = 0; i < kNumGrains; ++i) {
            grain_t &g = s_grain[i];
            if (!g.active) continue;

            const float win = window_for_age(g.age, g.inv_len);
            wetL += win * read_interp(s_bufL, g.readPosL);
            wetR += win * read_interp(s_bufR, g.readPosR);

            const float dir = g.rev ? -1.f : 1.f;
            g.readPosL += dir * g.readStepL;
            g.readPosR += dir * g.readStepR;

            if (g.readPosL < 0.f) g.readPosL += (float)kBufSize;
            if (g.readPosL >= (float)kBufSize) g.readPosL -= (float)kBufSize;
            if (g.readPosR < 0.f) g.readPosR += (float)kBufSize;
            if (g.readPosR >= (float)kBufSize) g.readPosR -= (float)kBufSize;

            if (++g.age >= g.len) g.active = false;
        }

        float fbShiftL = 0.f, fbShiftR = 0.f;
        for (int i = 0; i < kNumFeedbackGrains; ++i) {
            grain_t &g = s_fbGrain[i];
            if (!g.active) continue;

            const float win = window_for_age(g.age, g.inv_len);
            fbShiftL += win * read_interp(s_bufL, g.readPosL);
            fbShiftR += win * read_interp(s_bufR, g.readPosR);

            g.readPosL += kFeedbackPitchRatio;
            g.readPosR += kFeedbackPitchRatio;
            if (g.readPosL >= (float)kBufSize) g.readPosL -= (float)kBufSize;
            if (g.readPosR >= (float)kBufSize) g.readPosR -= (float)kBufSize;

            if (++g.age >= g.len) g.active = false;
        }

        // Main wet output damping / DC blocking.
        s_lpL += kDampCoef * (wetL - s_lpL);
        s_lpR += kDampCoef * (wetR - s_lpR);
        const float hpL = s_lpL - s_dcxL + kDcCoef * s_dcyL;
        const float hpR = s_lpR - s_dcxR + kDcCoef * s_dcyR;
        s_dcxL = s_lpL; s_dcyL = hpL;
        s_dcxR = s_lpR; s_dcyR = hpR;

        // Separate octave-up feedback damping / DC blocking.
        s_fbLpL += kDampCoef * (fbShiftL - s_fbLpL);
        s_fbLpR += kDampCoef * (fbShiftR - s_fbLpR);
        const float fbHpL = s_fbLpL - s_fbDcxL + kDcCoef * s_fbDcyL;
        const float fbHpR = s_fbLpR - s_fbDcxR + kDcCoef * s_fbDcyR;
        s_fbDcxL = s_fbLpL; s_fbDcyL = fbHpL;
        s_fbDcxR = s_fbLpR; s_fbDcyR = fbHpR;

        // The buffer receives the dry input plus octave-shifted feedback.
        s_bufL[w] = soft_limit(inL + s_fb * fbHpL);
        s_bufR[w] = soft_limit(inR + s_fb * fbHpR);
        s_w = (w + 1u) & kBufMask;

        const float m = s_mix;
        x[0] = inL * (1.f - m) + wetL * m;
        x[1] = inR * (1.f - m) + wetR * m;
    }
}

void DELFX_SUSPEND(void)
{
}

void DELFX_RESUME(void)
{
    clear_state();
}

void DELFX_PARAM(uint8_t index, int32_t value)
{
    // Keep the Minilogue XD Q31 parameter decoding used by the original project.
    float v = q31_to_f32(value);
    if (v < 0.f) v = 0.f;
    if (v > 1.f) v = 1.f;

    switch (index) {
    case k_user_delfx_param_time: {
        uint32_t div = (uint32_t)(v * (float)kNumDivs);
        if (div >= kNumDivs) div = kNumDivs - 1;
        s_div = div;
        break;
    }
    case k_user_delfx_param_depth:
        s_fb_t = v * kMaxFeedback;
        break;
    case k_user_delfx_param_shift_depth:
        s_mix_t = v;
        break;
    default:
        break;
    }
}
