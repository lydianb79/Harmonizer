// Core DSP building blocks. Everything here is allocation-free after init().
#pragma once
#include <Accelerate/Accelerate.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hz {

constexpr float kPi = 3.14159265358979323846f;

inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float db2lin(float d) { return std::exp(d * 0.11512925f); }
inline float semi2ratio(float s) { return std::exp2(s * (1.f / 12.f)); }
// tanh-like saturator, exact +-1 at |x| >= 3
inline float softclip(float x) {
    x = clampf(x, -3.f, 3.f);
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}
// sine approximation, phase in 0..1
inline float lfoSin(float ph) {
    float x = ph * 2.f - 1.f;
    float y = 4.f * x * (1.f - std::fabs(x));
    y = 0.225f * (y * std::fabs(y) - y) + y;
    return -y;
}
inline float wrap1(float ph) { return ph - std::floor(ph); }

struct Rng {  // white noise, -1..1
    uint32_t s = 0x1234567u;
    inline float next() {
        s = s * 1664525u + 1013904223u;
        return (float)(s >> 8) * (1.f / 8388608.f) - 1.f;
    }
};

struct Smooth {
    float y = 0, a = 0.01f;
    void setTime(float ms, float sr) { a = 1.f - std::exp(-1.f / (0.001f * std::max(ms, 0.01f) * sr)); }
    void snap(float v) { y = v; }
    inline float next(float t) { y += a * (t - y); return y; }
};

struct OnePole {
    float z = 0, a = 1;
    void setLP(float fc, float sr) { a = 1.f - std::exp(-2.f * kPi * clampf(fc, 1.f, sr * 0.49f) / sr); }
    inline float lp(float x) { z += a * (x - z); return z; }
    inline float hp(float x) { return x - lp(x); }
    void reset() { z = 0; }
};

enum BiquadType { BQ_LP, BQ_HP, BQ_BP, BQ_PEAK, BQ_LOSHELF, BQ_HISHELF };

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    inline float tick(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void set(BiquadType type, float f, float q, float gainDb, float sr) {
        f = clampf(f, 10.f, sr * 0.45f);
        float w0 = 2.f * kPi * f / sr, cs = std::cos(w0), sn = std::sin(w0);
        float alpha = sn / (2.f * q), A = std::pow(10.f, gainDb / 40.f), a0 = 1;
        switch (type) {
        case BQ_LP: b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = b0; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
        case BQ_HP: b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = b0; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
        case BQ_BP: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
        case BQ_PEAK:
            b0 = 1 + alpha * A; b1 = -2 * cs; b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cs; a2 = 1 - alpha / A; break;
        case BQ_LOSHELF: {
            float sq = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cs + sq); b1 = 2 * A * ((A - 1) - (A + 1) * cs);
            b2 = A * ((A + 1) - (A - 1) * cs - sq); a0 = (A + 1) + (A - 1) * cs + sq;
            a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - sq; break;
        }
        case BQ_HISHELF: {
            float sq = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cs + sq); b1 = -2 * A * ((A - 1) + (A + 1) * cs);
            b2 = A * ((A + 1) + (A - 1) * cs - sq); a0 = (A + 1) - (A - 1) * cs + sq;
            a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - sq; break;
        }
        }
        float inv = 1.f / a0;
        b0 *= inv; b1 *= inv; b2 *= inv; a1 *= inv; a2 *= inv;
    }
};

// Power-of-two circular delay line. at(0) is the most recently written sample.
struct Delay {
    std::vector<float> b;
    int mask = 0, w = 0;
    void init(int n) {
        int s = 16;
        while (s < n + 8) s <<= 1;
        b.assign((size_t)s, 0.f);
        mask = s - 1;
        w = 0;
    }
    void init(float n) { init((int)n); }
    void clear() { std::fill(b.begin(), b.end(), 0.f); }
    float maxDelay() const { return (float)(mask - 4); }
    inline void write(float x) { b[(size_t)w] = x; w = (w + 1) & mask; }
    inline float at(int d) const { return b[(size_t)((w - 1 - d) & mask)]; }
    inline float lin(float d) const {
        if (d < 0) d = 0;
        int i = (int)d;
        float f = d - (float)i, a = at(i);
        return a + f * (at(i + 1) - a);
    }
    inline float herm(float d) const {
        if (d < 1) d = 1;
        int i = (int)d;
        float f = d - (float)i;
        float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
        float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
        float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }
};

// Schroeder allpass diffuser
struct Allpass {
    Delay d;
    int len = 1;
    void init(int maxLen) { d.init(maxLen); len = maxLen; }
    inline float tick(float x, float g) {
        float dout = d.at(len - 1);
        float y = dout - g * x;
        d.write(x + g * y);
        return y;
    }
};

// Time-domain pitch shifter: two crossfading read heads sweeping a delay line.
// New grains are spliced at the lag that best correlates with the head still
// sounding, so crossfades stay phase coherent. With ratio near 1 the delay
// collapses to a few samples, i.e. effectively no latency for detune effects.
// A negative ratio plays grains backwards (reverse/crystal effects).
struct Shifter {
    Delay dl;
    float sr = 48000, grain = 1680, ratio = 1, minDelay = 4, maxDelay = 0;
    bool align = true;
    int maxLag = 640;
    float d[2] = {4, 4}, ph[2] = {0, 0.5f};

    void init(float sr_, float maxSec) {
        sr = sr_;
        dl.init((int)(maxSec * sr) + 16);
        maxDelay = maxSec * sr - 4;
        maxLag = (int)(sr / 75.f);
        setGrain(35);
        reset();
    }
    void setGrain(float ms) { grain = std::max(64.f, ms * 0.001f * sr); }
    void reset() {
        dl.clear();
        d[0] = d[1] = minDelay;
        ph[0] = 0;
        ph[1] = 0.5f;
    }
    int bestLag(float start, float ref) const {
        const int step = 4, span = maxLag;
        int s = (int)start, r = (int)ref, bk = 0;
        float best = -1e30f;
        for (int k = 0; k < span; k += step) {
            float dot = 0, en = 1e-9f;
            for (int j = 0; j < span; j += step) {
                float a = dl.at(r + j), b = dl.at(s + k + j);
                dot += a * b;
                en += b * b;
            }
            float sc = dot / std::sqrt(en) * (1.f - 0.15f * (float)k / (float)span);
            if (sc > best) { best = sc; bk = k; }
        }
        int lo = std::max(0, bk - 3), hi = bk + 3, bk2 = bk;
        best = -1e30f;
        for (int k = lo; k <= hi; k++) {
            float dot = 0, en = 1e-9f;
            for (int j = 0; j < span; j += step) {
                float a = dl.at(r + j), b = dl.at(s + k + j);
                dot += a * b;
                en += b * b;
            }
            float sc = dot / std::sqrt(en);
            if (sc > best) { best = sc; bk2 = k; }
        }
        return bk2;
    }
    void restart(int i) {
        float slope = 1.f - ratio, total = std::fabs(slope) * grain;
        float start = slope < 0 ? minDelay + total : minDelay;
        if (align && ratio > 0 && std::fabs(slope) > 0.002f && start + 2.f * (float)maxLag + 16.f < maxDelay)
            start += (float)bestLag(start, d[1 - i]);
        d[i] = std::min(start, maxDelay);
    }
    // Returns both heads separately (h0, h1); their sum is the shifted signal.
    inline void tick2(float x, float &h0, float &h1) {
        dl.write(x);
        float slope = 1.f - ratio, inc = 1.f / grain, o[2];
        for (int i = 0; i < 2; i++) {
            ph[i] += inc;
            if (ph[i] >= 1.f) { ph[i] -= 1.f; restart(i); }
            d[i] = clampf(d[i] + slope, minDelay, maxDelay);
            float t = ph[i] < 0.5f ? 2.f * ph[i] : 2.f - 2.f * ph[i];
            o[i] = t * t * (3.f - 2.f * t) * dl.herm(d[i]);
        }
        h0 = o[0];
        h1 = o[1];
    }
    inline float tick(float x) {
        float a, b;
        tick2(x, a, b);
        return a + b;
    }
};

// Polyphonic pitch shifter: phase vocoder with per-bin true-frequency tracking.
// Tracks every partial separately, so full chords shift cleanly. The price is
// a fixed latency of N - hop samples (about 37 ms at 44.1/48 kHz).
struct PolyShifter {
    int N = 2048, half = 1024, hop = 256, os = 8, lat = 1792, pos = 1792;
    vDSP_Length log2n = 11;
    FFTSetup setup = nullptr;
    float ratio = 1, fRatio = 1;  // fRatio: formant shift, used when keepFormant is set
    bool keepFormant = false;
    std::vector<float> in, out, acc, win, re, im, lastPh, sumPh, aMag, aFrq, sMag, sFrq, best, envl, pre;

    PolyShifter() = default;
    PolyShifter(const PolyShifter &) = delete;
    PolyShifter &operator=(const PolyShifter &) = delete;
    ~PolyShifter() { if (setup) vDSP_destroy_fftsetup(setup); }

    void init(float sr, float = 0) {
        log2n = sr > 140000 ? 13 : sr > 70000 ? 12 : 11;
        N = 1 << log2n; half = N / 2; hop = N / os; lat = N - hop;
        if (setup) vDSP_destroy_fftsetup(setup);
        setup = vDSP_create_fftsetup(log2n, kFFTRadix2);
        size_t n = (size_t)N;
        in.assign(n, 0); out.assign(n, 0); acc.assign(2 * n, 0); win.assign(n, 0); re.assign(n, 0); im.assign(n, 0);
        for (auto *v : {&lastPh, &sumPh, &aMag, &aFrq, &sMag, &sFrq, &best, &envl}) v->assign(n / 2 + 1, 0);
        pre.assign(n / 2 + 2, 0);
        for (int i = 0; i < N; i++) win[(size_t)i] = 0.5f - 0.5f * std::cos(2.f * kPi * (float)i / (float)N);
        reset();
    }
    void setGrain(float) {}
    void reset() {
        for (auto *v : {&in, &out, &acc, &lastPh, &sumPh}) std::fill(v->begin(), v->end(), 0.f);
        pos = lat;
    }
    inline float tick(float x) {
        in[(size_t)pos] = x;
        float y = out[(size_t)(pos - lat)];
        if (++pos >= N) { frame(); pos = lat; }
        return y;
    }
    void frame() {
        const float twoPi = 2.f * kPi, expct = twoPi / (float)os;
        for (int k = 0; k < N; k++) { re[(size_t)k] = in[(size_t)k] * win[(size_t)k]; im[(size_t)k] = 0; }
        DSPSplitComplex sc = {re.data(), im.data()};
        vDSP_fft_zip(setup, &sc, 1, log2n, FFT_FORWARD);
        for (int k = 0; k <= half; k++) {
            size_t q = (size_t)k;
            float ph = std::atan2(im[q], re[q]), d = ph - lastPh[q] - (float)k * expct;
            lastPh[q] = ph;
            d -= twoPi * std::round(d / twoPi);
            aMag[q] = std::sqrt(re[q] * re[q] + im[q] * im[q]);
            aFrq[q] = (float)k + d * (float)os / twoPi;  // true frequency in bins
            sMag[q] = 0; sFrq[q] = (float)k; best[q] = 0;
        }
        if (keepFormant) {
            // spectral envelope = moving average of the magnitudes; flatten before shifting
            int W = std::max(4, N / 170);
            pre[0] = 0;
            for (int k = 0; k <= half; k++) pre[(size_t)k + 1] = pre[(size_t)k] + aMag[(size_t)k];
            for (int k = 0; k <= half; k++) {
                int a = std::max(0, k - W), b = std::min(half, k + W);
                envl[(size_t)k] = (pre[(size_t)b + 1] - pre[(size_t)a]) / (float)(b - a + 1);
                aMag[(size_t)k] /= envl[(size_t)k] + 1e-9f;
            }
        }
        for (int k = 0; k <= half; k++) {
            int idx = (int)((float)k * ratio + 0.5f);
            if (idx > half) break;
            size_t q = (size_t)k, t = (size_t)idx;
            sMag[t] += aMag[q];
            if (aMag[q] > best[t]) { best[t] = aMag[q]; sFrq[t] = aFrq[q] * ratio; }
        }
        if (keepFormant) {
            for (int k = 0; k <= half; k++) sMag[(size_t)k] *= envl[(size_t)std::min(half, (int)((float)k / fRatio + 0.5f))];
        }
        for (int k = 0; k <= half; k++) {
            size_t q = (size_t)k;
            float ph = sumPh[q] + twoPi * (sFrq[q] - (float)k) / (float)os + (float)k * expct;
            ph -= twoPi * std::round(ph / twoPi);
            sumPh[q] = ph;
            re[q] = sMag[q] * std::cos(ph);
            im[q] = sMag[q] * std::sin(ph);
        }
        for (int k = 1; k < half; k++) { re[(size_t)(N - k)] = re[(size_t)k]; im[(size_t)(N - k)] = -im[(size_t)k]; }
        vDSP_fft_zip(setup, &sc, 1, log2n, FFT_INVERSE);
        float scale = 1.3f / ((float)N * (float)os * 0.375f);
        for (int k = 0; k < N; k++) acc[(size_t)k] += win[(size_t)k] * re[(size_t)k] * scale;
        std::memcpy(out.data(), acc.data(), sizeof(float) * (size_t)hop);
        std::memmove(acc.data(), acc.data() + hop, sizeof(float) * (size_t)N);
        std::memmove(in.data(), in.data() + hop, sizeof(float) * (size_t)lat);
    }
};

// YIN-style monophonic pitch tracker running on a decimated copy of the input.
struct PitchTracker {
    float sr = 48000, dsr = 12000, freq = 0;
    int dec = 4, cnt = 0, W = 336, maxLag = 200, minLag = 8, hop = 60, hopCnt = 0, w = 0;
    bool fresh = false;
    std::vector<float> ring, tmp, df;
    OnePole lp1, lp2;

    void init(float sr_, float targetRate = 12000.f, bool autoRun = true) {
        sr = sr_;
        dec = std::max(1, (int)std::lround(sr / targetRate));
        dsr = sr / (float)dec;
        W = (int)(dsr * 0.028f);
        maxLag = (int)(dsr / 60.f);
        minLag = std::max(2, (int)(dsr / 1400.f));
        hop = autoRun ? std::max(8, (int)(dsr * 0.005f)) : 0x7fffffff;
        int n = 16;
        while (n < W + maxLag + 4) n <<= 1;
        ring.assign((size_t)n, 0.f);
        tmp.assign((size_t)(W + maxLag + 4), 0.f);
        df.assign((size_t)(maxLag + 2), 0.f);
        lp1.setLP(2500, sr);
        lp2.setLP(2500, sr);
        reset();
    }
    void reset() {
        std::fill(ring.begin(), ring.end(), 0.f);
        lp1.reset(); lp2.reset();
        cnt = hopCnt = w = 0;
        freq = 0;
        fresh = false;
    }
    inline void push(float x) {
        x = lp2.lp(lp1.lp(x));
        if (++cnt < dec) return;
        cnt = 0;
        ring[(size_t)w] = x;
        w = (w + 1) & ((int)ring.size() - 1);
        if (++hopCnt >= hop) { hopCnt = 0; analyze(); }
    }
    void analyze() {
        fresh = true;
        int N = W + maxLag, mask = (int)ring.size() - 1;
        float e = 0;
        for (int i = 0; i < N; i++) tmp[(size_t)i] = ring[(size_t)((w - N + i) & mask)];
        for (int i = 0; i < W; i++) e += tmp[(size_t)i] * tmp[(size_t)i];
        if (e / (float)W < 1e-7f) { freq = 0; return; }
        float run = 0;
        df[0] = 1;
        for (int t = 1; t <= maxLag; t++) {
            float s = 0;
            const float *a = tmp.data(), *b = tmp.data() + t;
            for (int j = 0; j < W; j++) { float q = a[j] - b[j]; s += q * q; }
            run += s;
            df[(size_t)t] = s * (float)t / (run + 1e-12f);
        }
        df[(size_t)maxLag + 1] = df[(size_t)maxLag];
        int tau = -1;
        for (int t = minLag; t < maxLag; t++) {
            if (df[(size_t)t] < 0.15f) {
                while (t + 1 < maxLag && df[(size_t)t + 1] < df[(size_t)t]) t++;
                tau = t;
                break;
            }
        }
        if (tau < 0) {
            float m = 1e9f;
            for (int t = minLag; t < maxLag; t++)
                if (df[(size_t)t] < m) { m = df[(size_t)t]; tau = t; }
            if (m > 0.35f || tau < 1) { freq = 0; return; }
        }
        float a = df[(size_t)tau - 1], b = df[(size_t)tau], c = df[(size_t)tau + 1], den = a - 2 * b + c;
        float off = std::fabs(den) > 1e-9f ? 0.5f * (a - c) / den : 0.f;
        freq = dsr / ((float)tau + clampf(off, -1.f, 1.f));
    }
};

// 8-line feedback delay network reverb with Hadamard mixing, per-line damping
// and modulation, and allpass input diffusion.
struct Verb {
    static const int N = 8;
    Delay dl[N];
    Allpass ap[4];
    OnePole damp[N], lowc[N];
    float len[N], g[N], ph[N], sr = 48000, modDepth = 0, modInc = 0, apG = 0.6f, maxScale = 1;
    bool frozen = false;

    void init(float sr_, float maxScale_) {
        static const float base[N] = {29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.7f, 73.1f};
        static const float apMs[4] = {4.7f, 7.9f, 5.3f, 8.9f};
        sr = sr_;
        maxScale = maxScale_;
        for (int i = 0; i < N; i++) {
            dl[i].init((int)(base[i] * 0.001f * sr * maxScale) + 128);
            ph[i] = (float)i / (float)N;
        }
        for (int i = 0; i < 4; i++) ap[i].init((int)(apMs[i] * 0.001f * sr));
        set(1, 2, 8000, 60, 0.6f, 0, 0.5f);
    }
    void reset() {
        for (auto &d : dl) d.clear();
        for (auto &a : ap) a.d.clear();
        for (int i = 0; i < N; i++) { damp[i].reset(); lowc[i].reset(); }
    }
    void set(float size, float rt60, float dampHz, float lowHz, float diff, float modSamples, float modHz) {
        static const float base[N] = {29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.7f, 73.1f};
        size = clampf(size, 0.1f, maxScale);
        for (int i = 0; i < N; i++) {
            len[i] = base[i] * 0.001f * sr * size;
            g[i] = std::pow(10.f, -3.f * len[i] / (std::max(rt60, 0.05f) * sr));
            damp[i].setLP(dampHz, sr);
            lowc[i].setLP(lowHz, sr);
        }
        apG = diff;
        modDepth = std::min(modSamples, 100.f);
        modInc = modHz / sr;
    }
    inline void tick(float inL, float inR, float &outL, float &outR) {
        float a = ap[1].tick(ap[0].tick(inL, apG), apG), b = ap[3].tick(ap[2].tick(inR, apG), apG);
        if (frozen) a = b = 0;
        float v[N];
        for (int i = 0; i < N; i++) {
            ph[i] += modInc * (1.f + 0.11f * (float)i);
            if (ph[i] >= 1.f) ph[i] -= 1.f;
            v[i] = dl[i].lin(len[i] + modDepth * (1.f + lfoSin(ph[i])));
            if (!frozen) {
                v[i] = damp[i].lp(v[i]);
                v[i] -= lowc[i].lp(v[i]);
                v[i] *= g[i];
            }
        }
        outL = 0.4f * (v[0] - v[2] + v[4] - v[6]);
        outR = 0.4f * (v[1] - v[3] + v[5] - v[7]);
        for (int h = 1; h < N; h <<= 1)
            for (int i = 0; i < N; i += h * 2)
                for (int j = i; j < i + h; j++) {
                    float x = v[j], y = v[j + h];
                    v[j] = x + y;
                    v[j + h] = x - y;
                }
        for (int i = 0; i < N; i++) dl[i].write(((i & 1) ? b : a) + v[i] * 0.35355339f);
    }
};

// ---------------------------------------------------------------- parameters

enum PKind { LIN, LOG, STEP, TIME };
enum PFmt { F_STD, F_INTERVAL, F_NOTE };

struct ParamDesc {
    const char *name;
    float mn, mx, def;
    PKind kind;
    const char *unit;
    const char *const *labels;
    PFmt fmt;
};

struct Algo {
    float sr = 48000, bpm = 120;
    float p[10] = {0};
    virtual ~Algo() {}
    virtual void init(float sampleRate) = 0;  // allocate
    virtual void reset() = 0;                 // clear state (params are valid)
    virtual void update() {}                  // params changed
    virtual bool spills() const { return true; }  // keep ringing out after being replaced
    // In place. Input is the dry signal, output must be the 100% wet signal;
    // the engine applies p[0] (Mix).
    virtual void process(float *L, float *R, int n) = 0;
};

struct AlgoInfo {
    const char *name, *cat, *blurb;
    std::vector<ParamDesc> pd;
    Algo *(*make)();
};

const std::vector<AlgoInfo> &registry();
void appendMoreAlgos(std::vector<AlgoInfo> &r);

}  // namespace hz
