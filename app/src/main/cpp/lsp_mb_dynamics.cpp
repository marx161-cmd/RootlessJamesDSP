#include "lsp_mb_dynamics.h"
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace {

struct Biquad {
    float b0, b1, b2, a1, a2;
    float z1, z2;

    Biquad() : b0(1), b1(0), b2(0), a1(0), a2(0), z1(0), z2(0) {}

    void set_lp(float fc, float fs, float q) {
        float w0 = 2.0f * float(M_PI) * fc / fs;
        float cos_w0 = cosf(w0);
        float sin_w0 = sinf(w0);
        float alpha = sin_w0 / (2.0f * q);
        float a0 = 1.0f + alpha;
        b0 = ((1.0f - cos_w0) * 0.5f) / a0;
        b1 = (1.0f - cos_w0) / a0;
        b2 = b0;
        a1 = (-2.0f * cos_w0) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void set_hp(float fc, float fs, float q) {
        float w0 = 2.0f * float(M_PI) * fc / fs;
        float cos_w0 = cosf(w0);
        float sin_w0 = sinf(w0);
        float alpha = sin_w0 / (2.0f * q);
        float a0 = 1.0f + alpha;
        b0 = ((1.0f + cos_w0) * 0.5f) / a0;
        b1 = -(1.0f + cos_w0) / a0;
        b2 = b0;
        a1 = (-2.0f * cos_w0) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void reset() { z1 = z2 = 0.0f; }

    float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Linkwitz-Riley 4th-order crossover split at fc
struct LR4Crossover {
    Biquad lp1, lp2, hp1, hp2;

    void set(float fc, float fs) {
        const float q = 0.70710678f;
        lp1.set_lp(fc, fs, q);
        lp2.set_lp(fc, fs, q);
        hp1.set_hp(fc, fs, q);
        hp2.set_hp(fc, fs, q);
    }

    void reset() { lp1.reset(); lp2.reset(); hp1.reset(); hp2.reset(); }

    // Process one sample, returns {lo, hi}
    void process_sample(float in, float &out_lo, float &out_hi) {
        float l1 = lp1.process(in);
        float h1 = hp1.process(in);
        out_lo = lp2.process(l1);
        out_hi = hp2.process(h1);
    }
};

enum BandMode { BAND_MODE_COMP = 0, BAND_MODE_GATE = 1 };

struct CompBand {
    float env;
    float energy_sq;

    int enabled;
    BandMode mode;
    float threshold;
    float ratio;
    float knee;
    float att_coef;
    float rel_coef;
    float makeup_gain;
};

struct MBDyn {
    unsigned num_bands;
    float sample_rate;
    LR4Crossover *splits;  // N-1 splitters
    CompBand *bands;
};

static MBDyn *g_mb = nullptr;

static const unsigned MAX_BANDS = 8;
static const float DEFAULT_SPLITS[MAX_BANDS - 1] = {
    100.0f, 300.0f, 800.0f, 2000.0f, 5000.0f, 10000.0f, 15000.0f
};

inline float soft_knee_db(float db_in, float threshold, float ratio, float knee) {
    float half_knee = knee * 0.5f;
    if (db_in < threshold - half_knee)
        return db_in;
    if (db_in > threshold + half_knee) {
        float above = db_in - threshold;
        return threshold + above / ratio;
    }
    float above = db_in - threshold + half_knee;
    float tmp = above / (2.0f * knee);
    return db_in + (1.0f / ratio - 1.0f) * above * tmp;
}

// Downward expansion / gate curve — the mirror image of soft_knee_db.
// soft_knee_db only ever reduces gain *above* threshold (compression); a
// gate needs to reduce gain *below* threshold (silence quiet/noise floor
// signal) and leave everything above threshold untouched. mb_dyn_set_gate
// used to just crank soft_knee_db's ratio to 1000, which squashes loud
// transients above the threshold instead of silencing anything quiet —
// it couldn't gate at all. This is the actual gate curve.
inline float expand_knee_db(float db_in, float threshold, float ratio, float knee) {
    float half_knee = knee * 0.5f;
    if (db_in > threshold + half_knee)
        return db_in;
    if (db_in < threshold - half_knee) {
        float below = threshold - db_in;
        return threshold - below * ratio;
    }
    float below = threshold + half_knee - db_in;
    float tmp = below / (2.0f * knee);
    return db_in - (ratio - 1.0f) * below * tmp;
}

inline float db2lin(float db) {
    return powf(10.0f, db * 0.05f);
}

inline float lin2db(float lin) {
    return 20.0f * log10f(lin + 1e-30f);
}

} // anonymous namespace

extern "C" {

void mb_dyn_init(float sample_rate, unsigned num_bands) {
    mb_dyn_destroy();

    if (num_bands < 2) num_bands = 2;
    if (num_bands > MAX_BANDS) num_bands = MAX_BANDS;

    g_mb = new MBDyn();
    g_mb->sample_rate = sample_rate;
    g_mb->num_bands = num_bands;
    g_mb->bands = new CompBand[num_bands];
    g_mb->splits = new LR4Crossover[num_bands - 1];

    float fs = sample_rate;
    for (unsigned i = 0; i < num_bands - 1; i++) {
        float fc = DEFAULT_SPLITS[i];
        if (fc < 20.0f) fc = 20.0f;
        if (fc > fs * 0.45f) fc = fs * 0.45f;
        g_mb->splits[i].set(fc, fs);
    }

    for (unsigned i = 0; i < num_bands; i++) {
        CompBand &b = g_mb->bands[i];
        b.env = 1e-10f;
        b.energy_sq = 1e-10f;
        b.enabled = 1;
        b.mode = BAND_MODE_COMP;
        b.threshold = -24.0f;
        b.ratio = 2.0f;
        b.knee = 6.0f;
        b.att_coef = expf(-1.0f / (0.010f * fs));
        b.rel_coef = expf(-1.0f / (0.100f * fs));
        b.makeup_gain = 0.0f;
    }
}

void mb_dyn_destroy() {
    if (g_mb) {
        delete[] g_mb->bands;
        delete[] g_mb->splits;
        delete g_mb;
        g_mb = nullptr;
    }
}

void mb_dyn_set_comp(unsigned band, int enabled,
                     float threshold, float ratio, float knee,
                     float attack, float release, float hold) {
    if (!g_mb || band >= g_mb->num_bands) return;

    CompBand &b = g_mb->bands[band];
    b.enabled = enabled;
    b.mode = BAND_MODE_COMP;
    b.threshold = threshold;
    b.ratio = (ratio < 1.0f) ? 1.0f : ratio;
    b.knee = (knee < 0.01f) ? 0.01f : knee;

    float fs = g_mb->sample_rate;
    float att_s = (attack < 0.001f) ? 0.001f : attack;
    float rel_s = (release < 0.001f) ? 0.001f : release;
    b.att_coef = expf(-1.0f / (att_s * fs));
    b.rel_coef = expf(-1.0f / (rel_s * fs));
    (void)hold;
}

void mb_dyn_set_gate(unsigned band, int enabled,
                     float open_db, float close_db,
                     float attack, float release, float hold, float reduction) {
    if (!g_mb || band >= g_mb->num_bands) return;

    CompBand &b = g_mb->bands[band];
    b.enabled = enabled;
    b.mode = BAND_MODE_GATE;
    if (enabled) {
        b.threshold = open_db;
        b.ratio = 10.0f;
        b.knee = 3.0f;
    }

    float fs = g_mb->sample_rate;
    b.att_coef = expf(-1.0f / ((attack > 0.001f ? attack : 0.001f) * fs));
    b.rel_coef = expf(-1.0f / ((release > 0.001f ? release : 0.001f) * fs));
    (void)close_db;
    (void)hold;
    (void)reduction;
}

void mb_dyn_set_eq(unsigned band, float gain_db) {
    if (!g_mb || band >= g_mb->num_bands) return;
    g_mb->bands[band].makeup_gain = gain_db;
}

void mb_dyn_process(float *left, float *right, unsigned n) {
    if (!g_mb || n == 0) return;

    unsigned nb = g_mb->num_bands;
    LR4Crossover *splits = g_mb->splits;
    CompBand *bands = g_mb->bands;

    for (unsigned i = 0; i < n; i++) {
        float signal_l = left[i];
        float signal_r = right[i];

        float sum_l = 0.0f;
        float sum_r = 0.0f;

        for (unsigned b = 0; b < nb; b++) {
            float band_l, band_r;

            if (b < nb - 1) {
                // Split current signal: lo goes to band[b], hi continues
                splits[b].process_sample(signal_l, band_l, signal_l);
                splits[b].process_sample(signal_r, band_r, signal_r);
            } else {
                // Last band: gets whatever is left (hi after all previous splits)
                band_l = signal_l;
                band_r = signal_r;
            }

            // Per-band compression
            float m = 0.5f * (fabsf(band_l) + fabsf(band_r));
            float squared = m * m;

            float coef = (squared > bands[b].env) ? bands[b].att_coef : bands[b].rel_coef;
            bands[b].env = coef * bands[b].env + (1.0f - coef) * squared;
            bands[b].energy_sq = bands[b].env;

            float level_db = 10.0f * log10f(bands[b].env + 1e-30f);

            float target_db = level_db;
            if (bands[b].enabled) {
                target_db = (bands[b].mode == BAND_MODE_GATE)
                    ? expand_knee_db(level_db, bands[b].threshold, bands[b].ratio, bands[b].knee)
                    : soft_knee_db(level_db, bands[b].threshold, bands[b].ratio, bands[b].knee);
            }

            float gain_db = target_db - level_db + bands[b].makeup_gain;
            float gain = db2lin(gain_db);

            sum_l += band_l * gain;
            sum_r += band_r * gain;
        }

        left[i] = sum_l;
        right[i] = sum_r;
    }
}

unsigned mb_dyn_num_bands() {
    return g_mb ? g_mb->num_bands : 0;
}

float mb_dyn_get_band_energy(unsigned band) {
    if (!g_mb || band >= g_mb->num_bands) return 0.0f;
    return sqrtf(g_mb->bands[band].energy_sq);
}

} // extern "C"
