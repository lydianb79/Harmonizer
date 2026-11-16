// Second set of algorithms: more pitch, delay, modulation, reverb, drive,
// multi-effect, polyphonic synth and glitch effects.
#include "dsp.hpp"

namespace hz {
namespace {

ParamDesc mix(float d) { return {"Mix", 0, 100, d, LIN, "%", nullptr, F_STD}; }
ParamDesc lin(const char *n, float a, float b, float d, const char *u = "") { return {n, a, b, d, LIN, u, nullptr, F_STD}; }
ParamDesc lg(const char *n, float a, float b, float d, const char *u = "") { return {n, a, b, d, LOG, u, nullptr, F_STD}; }
ParamDesc tm(const char *n, float a, float b, float d) { return {n, a, b, d, TIME, "ms", nullptr, F_STD}; }
ParamDesc st(const char *n, float a, float b, float d, const char *const *labels = nullptr, const char *u = "") { return {n, a, b, d, STEP, u, labels, F_STD}; }

const char *const kShapes4[] = {"Sine", "Triangle", "Square", "Ramp"};
const char *const kShapesR[] = {"Sine", "Triangle", "Square", "Random"};
const char *const kShapesE[] = {"Sine", "Triangle", "Square", "Random", "Envelope"};
const char *const kSineTri[] = {"Sine", "Triangle"};
const char *const kFilt[] = {"Low Pass", "Band Pass", "High Pass"};
const char *const kModes3[] = {"LFO", "Envelope", "Manual"};
const char *const kPrism[] = {"Chord", "Up", "Down", "Random"};
const char *const kDyna[] = {"Duck", "Gate", "Swell"};
const char *const kRoute[] = {"Series", "Parallel"};
const char *const kWaves[] = {"Saw", "Square", "Organ"};

inline float shp(int s, float ph) {
    switch (s) {
    case 1: return ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph;
    case 2: return ph < 0.5f ? 1.f : -1.f;
    case 3: return 1 - 2 * ph;
    default: return lfoSin(ph);
    }
}
inline float rnd01(Rng &r) { return 0.5f + 0.5f * r.next(); }

struct SVF {
    float ic1 = 0, ic2 = 0, lp = 0, bp = 0, hp = 0;
    void reset() { ic1 = ic2 = 0; }
    inline void tick(float x, float f, float k, float sr) {
        float g = std::tan(kPi * clampf(f, 20.f, sr * 0.42f) / sr), a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
        lp = v2; bp = v1; hp = x - k * v1 - v2;
    }
    inline float out(int type) const { return type == 0 ? lp : type == 1 ? bp : hp; }
};

// 2x oversampled waveshaper (cubic half-band up and down)
struct OS2 {
    float xh[4] = {0}, uh[7] = {0};
    void reset() { std::memset(xh, 0, sizeof xh); std::memset(uh, 0, sizeof uh); }
    template <class F> inline float run(float x, F f) {
        xh[3] = xh[2]; xh[2] = xh[1]; xh[1] = xh[0]; xh[0] = x;
        float e0 = f(xh[2]), e1 = f((-xh[3] + 9 * xh[2] + 9 * xh[1] - xh[0]) * 0.0625f);
        for (int k = 6; k >= 2; k--) uh[k] = uh[k - 2];
        uh[1] = e0; uh[0] = e1;
        return (-uh[0] + 9 * uh[2] + 16 * uh[3] + 9 * uh[4] - uh[6]) * 0.03125f;
    }
};
inline float fuzzShape(float v) { return v >= 0 ? 1.f - std::exp(-2.f * v) : -0.85f * (1.f - std::exp(1.3f * v)); }

// ------------------------------------------------------------------- PITCH

struct ModShift : Algo {
    Shifter sa, sb; Delay da, db; Smooth t; Rng rng; float ph = 0, fa = 0, fb2 = 0, sh = 0;
    void init(float s) override { sr = s; sa.init(sr, 0.35f); sb.init(sr, 0.35f); sb.setGrain(40); da.init(sr * 2.05f); db.init(sr * 2.05f); t.setTime(60, sr); }
    void reset() override { sa.reset(); sb.reset(); da.clear(); db.clear(); fa = fb2 = 0; t.snap(p[6] * 0.001f * sr); }
    void process(float *L, float *R, int n) override {
        float dep = p[3] * 0.01f, inc = p[4] / sr, fb = p[7] * 0.01f, tt = p[6] * 0.001f * sr;
        int shape = (int)p[5];
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]);
            ph += inc; if (ph >= 1) { ph -= 1; sh = rng.next(); }
            float l = shape == 3 ? sh : shp(shape, ph);
            sa.ratio = semi2ratio(p[1] + dep * l); sb.ratio = semi2ratio(p[2] - dep * l);
            da.write(sa.tick(m + softclip(fb * fa))); db.write(sb.tick(m + softclip(fb * fb2)));
            float d = t.next(tt);
            fa = da.lin(d); fb2 = db.lin(d);
            L[i] = 0.8f * fa + 0.3f * fb2; R[i] = 0.3f * fa + 0.8f * fb2;
        }
    }
};

struct Prism : Algo {
    PolyShifter s[3]; Delay d; Rng rng; float pos = 0, g[3] = {0, 0, 0}, fbv = 0; int step = 0, rd = 0;
    void init(float sr_) override { sr = sr_; for (auto &x : s) x.init(sr); d.init(sr * 1.1f); }
    void reset() override { for (auto &x : s) x.reset(); d.clear(); pos = 0; step = 0; fbv = 0; }
    void update() override { for (int i = 0; i < 3; i++) s[i].ratio = semi2ratio(p[1 + i]); }
    void process(float *L, float *R, int n) override {
        static const float pan[3] = {-1, 0, 1};
        float len = std::max(64.f, p[4] * 0.001f * sr), sm = 1.f - std::exp(-1.f / ((0.002f + p[6] * 0.0008f) * sr)), fb = p[7] * 0.01f, w = p[8] * 0.01f;
        int mode = (int)p[5];
        for (int i = 0; i < n; i++) {
            float in = 0.5f * (L[i] + R[i]) + softclip(fb * fbv), l = 0, r = 0;
            if (++pos >= len) { pos = 0; step = (step + 1) % 3; rd = (int)(rnd01(rng) * 2.99f); }
            int act = mode == 0 ? -1 : mode == 1 ? step : mode == 2 ? 2 - step : rd;
            for (int v = 0; v < 3; v++) {
                g[v] += sm * ((act < 0 || act == v ? 1.f : 0.f) - g[v]);
                float o = s[v].tick(in) * g[v] * (act < 0 ? 0.7f : 1.f);
                l += o * (0.5f - 0.5f * pan[v] * w); r += o * (0.5f + 0.5f * pan[v] * w);
            }
            d.write(0.5f * (l + r));
            fbv = d.lin(len);
            L[i] = l * 1.3f; R[i] = r * 1.3f;
        }
    }
};

struct PitchFuzz : Algo {
    Shifter sa, sb; Delay d; OnePole hp, tone; OS2 os; Smooth t; float fbv = 0;
    void init(float s) override { sr = s; sa.init(sr, 0.35f); sb.init(sr, 0.35f); sb.setGrain(40); d.init(sr * 1.1f); hp.setLP(90, sr); t.setTime(60, sr); }
    void reset() override { sa.reset(); sb.reset(); d.clear(); hp.reset(); tone.reset(); os.reset(); fbv = 0; t.snap(p[6] * 0.001f * sr); }
    void update() override { tone.setLP(p[2], sr); sa.ratio = semi2ratio(p[3]); sb.ratio = semi2ratio(p[4]); }
    void process(float *L, float *R, int n) override {
        float drive = db2lin(p[1] * 0.5f), pm = p[5] * 0.01f, fb = p[7] * 0.01f, lvl = db2lin(p[8]), tt = p[6] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            float f = tone.lp(os.run(hp.hp(0.5f * (L[i] + R[i])) * drive, fuzzShape)) * 0.5f;
            float a = sa.tick(f), b = sb.tick(f), dry = f * (1.f - 0.5f * pm);
            float yl = dry + pm * (0.8f * a + 0.3f * b), yr = dry + pm * (0.3f * a + 0.8f * b), dt = t.next(tt);
            d.write(0.5f * (yl + yr) + softclip(fb * fbv));
            fbv = d.lin(dt);
            float e = dt > 1.f ? 0.7f * fbv : 0.f;
            L[i] = (yl + e) * lvl; R[i] = (yr + e) * lvl;
        }
    }
};

struct FormantShift : Algo {
    PolyShifter s;
    void init(float sr_) override { sr = sr_; s.init(sr); s.keepFormant = true; }
    void reset() override { s.reset(); }
    void update() override { s.ratio = semi2ratio(p[1] + p[2] * 0.01f); s.fRatio = semi2ratio(p[3]); }
    void process(float *L, float *R, int n) override {
        for (int i = 0; i < n; i++) { float o = s.tick(0.5f * (L[i] + R[i])); L[i] = o; R[i] = o; }
    }
};

// ------------------------------------------------------------------- DELAY

struct VintageDelay : Algo {
    Delay d; OnePole hc; Smooth t; float hold = 0, cnt = 0, ph = 0;
    void init(float s) override { sr = s; d.init(sr * 2.1f); t.setTime(90, sr); }
    void reset() override { d.clear(); hc.reset(); hold = cnt = 0; t.snap(p[1] * 0.001f * sr); }
    void update() override { hc.setLP(p[5], sr); }
    void process(float *L, float *R, int n) override {
        float fb = p[2] * 0.01f, q = std::exp2(std::round(p[3]) - 1.f), step = std::max(1.f, sr / p[4]), md = p[6] * 0.01f * 0.003f * sr, tt = p[1] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            ph += 0.6f / sr; if (ph >= 1) ph -= 1;
            float y = d.herm(t.next(tt) + md * (1 + lfoSin(ph)));
            if (++cnt >= step) { cnt -= step; hold = std::round(y * q) / q; }
            y = hc.lp(hold);
            d.write(0.5f * (L[i] + R[i]) + softclip(fb * y));
            L[i] = y; R[i] = y;
        }
    }
};

struct BandDelay : Algo {
    Delay d; SVF f; Smooth t; float ph = 0;
    void init(float s) override { sr = s; d.init(sr * 2.1f); t.setTime(90, sr); }
    void reset() override { d.clear(); f.reset(); t.snap(p[1] * 0.001f * sr); }
    void process(float *L, float *R, int n) override {
        float fb = p[2] * 0.01f, k = 1.f / p[4], inc = p[6] / sr, dep = p[7] * 0.02f, tt = p[1] * 0.001f * sr;
        int type = (int)p[5];
        for (int i = 0; i < n; i++) {
            ph += inc; if (ph >= 1) ph -= 1;
            f.tick(d.herm(t.next(tt)), p[3] * std::exp2(dep * lfoSin(ph)), k, sr);
            float y = softclip(f.out(type) * (type == 1 ? 1.f + k : 1.f));
            d.write(0.5f * (L[i] + R[i]) + fb * y);
            L[i] = y; R[i] = y;
        }
    }
};

struct FilterPong : Algo {
    Delay da, db; SVF fa, fb2; Smooth t; float ph = 0;
    void init(float s) override { sr = s; da.init(sr * 2.1f); db.init(sr * 2.1f); t.setTime(90, sr); }
    void reset() override { da.clear(); db.clear(); fa.reset(); fb2.reset(); t.snap(p[1] * 0.001f * sr); }
    void process(float *L, float *R, int n) override {
        float fb = p[2] * 0.01f, k = 1.f / p[4], inc = p[5] / sr, dep = p[6] * 0.02f, tt = p[1] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            ph += inc; if (ph >= 1) ph -= 1;
            float l = lfoSin(ph), dt = t.next(tt);
            fa.tick(da.herm(dt), p[3] * std::exp2(dep * l), k, sr); fb2.tick(db.herm(dt), p[3] * std::exp2(-dep * l), k, sr);
            float a = softclip(fa.lp), b = softclip(fb2.lp);
            da.write(0.5f * (L[i] + R[i]) + fb * b); db.write(fb * a);
            L[i] = a; R[i] = b;
        }
    }
};

struct MultiHead : Algo {
    Delay d; OnePole hc, nz; Smooth t; Rng rng; float wph = 0;
    void init(float s) override { sr = s; d.init(sr * 2.3f); t.setTime(180, sr); nz.setLP(3, sr); }
    void reset() override { d.clear(); hc.reset(); t.snap(p[1] * 0.001f * sr); }
    void update() override { hc.setLP(p[9], sr); }
    void process(float *L, float *R, int n) override {
        float fb = p[6] * 0.006f, drive = 1.f + p[7] * 0.05f, wow = p[8] * 0.01f * 0.0015f * sr, tt = p[1] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            wph += 0.5f / sr; if (wph >= 1) wph -= 1;
            float bt = t.next(tt) + wow * (1 + lfoSin(wph) + 4 * nz.lp(rng.next())), l = 0, r = 0;
            for (int h = 0; h < 4; h++) {
                float y = d.herm(bt * (float)(h + 1)) * p[2 + h] * 0.01f;
                l += y * ((h & 1) ? 0.35f : 1.f); r += y * ((h & 1) ? 1.f : 0.35f);
            }
            d.write(0.5f * (L[i] + R[i]) + fb * hc.lp(softclip((l + r) * 0.75f * drive) / drive));
            L[i] = l; R[i] = r;
        }
    }
};

struct BucketDelay : Algo {
    Delay d; OnePole l1, l2; Smooth t; Rng rng; float ph = 0;
    void init(float s) override { sr = s; d.init(sr * 1.4f); t.setTime(150, sr); }
    void reset() override { d.clear(); l1.reset(); l2.reset(); t.snap(p[1] * 0.001f * sr); }
    void update() override {
        // bucket brigade chips get darker as the clock slows for longer delays
        float fc = std::min(p[3], std::max(900.f, 14000.f * 60.f / std::max(60.f, p[1])));
        l1.setLP(fc, sr); l2.setLP(fc * 1.3f, sr);
    }
    void process(float *L, float *R, int n) override {
        float fb = p[2] * 0.01f, md = p[4] * 0.01f * 0.004f * sr, inc = p[5] / sr, g = 1.f + p[6] * 0.06f, nzg = p[6] * 0.00003f, tt = p[1] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            ph += inc; if (ph >= 1) ph -= 1;
            float y = l2.lp(l1.lp(d.herm(t.next(tt) + md * (1 + lfoSin(ph)))));
            y = softclip(y * g) / g + nzg * rng.next();
            d.write(0.5f * (L[i] + R[i]) + fb * y);
            L[i] = y; R[i] = y;
        }
    }
};

// -------------------------------------------------------------- MODULATION

struct ModFilter : Algo {
    SVF fl, fr; Rng rng; float ph = 0, sh = 0, sh2 = 0, env = 0;
    void init(float s) override { sr = s; }
    void reset() override { fl.reset(); fr.reset(); env = 0; }
    void process(float *L, float *R, int n) override {
        int type = (int)p[1], shape = (int)p[6];
        float k = 1.f / p[3], inc = p[4] / sr, dep = p[5], sto = p[7] / 360.f, rel = 1.f - std::exp(-1.f / (0.15f * sr));
        for (int i = 0; i < n; i++) {
            float am = std::fabs(0.5f * (L[i] + R[i]));
            env = am > env ? env + 0.2f * (am - env) : env + rel * (am - env);
            ph += inc; if (ph >= 1) { ph -= 1; sh = rng.next(); sh2 = rng.next(); }
            float ml = shape == 3 ? sh : shape == 4 ? std::min(1.f, env * 8.f) * 2 - 1 : shp(shape, ph);
            float mr = shape == 3 ? (sto > 0.01f ? sh2 : sh) : shape == 4 ? ml : shp(shape, wrap1(ph + sto));
            fl.tick(L[i], p[2] * std::exp2(dep * 0.5f * (ml + 1)), k, sr); fr.tick(R[i], p[2] * std::exp2(dep * 0.5f * (mr + 1)), k, sr);
            float c = type == 1 ? 1.f + k : 1.f;
            L[i] = softclip(fl.out(type) * c); R[i] = softclip(fr.out(type) * c);
        }
    }
};

struct Vibrato : Algo {
    Delay dl, dr; float ph = 0, env = 0, rise = 0;
    void init(float s) override { sr = s; dl.init(sr * 0.05f); dr.init(sr * 0.05f); }
    void reset() override { dl.clear(); dr.clear(); env = rise = 0; }
    void process(float *L, float *R, int n) override {
        float inc = p[1] / sr, d = p[2] * 0.01f * 0.0025f * sr / (1.f + p[1] * 0.3f), sto = p[5] / 360.f;
        float rinc = 1.f / (p[4] * 0.001f * sr), rel = 1.f - std::exp(-1.f / (0.05f * sr));
        int s = (int)p[3];
        for (int i = 0; i < n; i++) {
            float am = std::fabs(0.5f * (L[i] + R[i]));
            env = am > env ? am : env + rel * (am - env);
            if (env > 0.01f) rise = std::min(1.f, rise + rinc); else if (env < 0.003f) rise = 0;
            float dd = d * (p[4] <= 1.5f ? 1.f : rise);
            ph += inc; if (ph >= 1) ph -= 1;
            dl.write(L[i]); dr.write(R[i]);
            L[i] = dl.herm(2 + dd * (1 + shp(s, ph))); R[i] = dr.herm(2 + dd * (1 + shp(s, wrap1(ph + sto))));
        }
    }
};

struct Undulator : Algo {
    Shifter sa, sb; Delay da, db; float fa = 0, fb2 = 0, tph = 0, fph = 0;
    void init(float s) override { sr = s; sa.init(sr, 0.25f); sb.init(sr, 0.25f); sa.setGrain(50); sb.setGrain(57); da.init(sr * 0.75f); db.init(sr * 0.75f); }
    void reset() override { sa.reset(); sb.reset(); da.clear(); db.clear(); fa = fb2 = 0; }
    void update() override { sa.ratio = semi2ratio(p[1] * 0.01f); sb.ratio = semi2ratio(-p[1] * 0.01f); }
    void process(float *L, float *R, int n) override {
        float tt = p[2] * 0.001f * sr, fb = p[3] * 0.01f, dep = p[4] * 0.01f, w = p[6] * 0.01f, fmd = p[8] * 0.015f;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]);
            da.write(sa.tick(m + fb * fa)); db.write(sb.tick(m + fb * fb2));
            fa = da.lin(tt); fb2 = db.lin(tt * 1.33f);
            fph = wrap1(fph + p[7] / sr);
            tph = wrap1(tph + p[5] * std::exp2(fmd * lfoSin(fph)) / sr);
            float gl = 1 - dep * (0.5f - 0.5f * lfoSin(tph)), gr = 1 - dep * (0.5f - 0.5f * lfoSin(wrap1(tph + 0.25f * w)));
            float mid = 0.5f * (fa + fb2), side = 0.5f * (fa - fb2) * w;
            L[i] = (mid + side) * gl * 1.2f; R[i] = (mid - side) * gr * 1.2f;
        }
    }
};

struct HarmTrem : Algo {
    OnePole xl, xr; float ph = 0;
    void init(float s) override { sr = s; }
    void reset() override { xl.reset(); xr.reset(); }
    void update() override { xl.setLP(p[3], sr); xr.setLP(p[3], sr); }
    void process(float *L, float *R, int n) override {
        float inc = p[1] / sr, dep = p[2] * 0.01f, sto = p[5] / 360.f, drive = 1.f + p[6] * 0.05f;
        int s = (int)p[4];
        for (int i = 0; i < n; i++) {
            ph += inc; if (ph >= 1) ph -= 1;
            float a = 0.5f + 0.5f * shp(s, ph), b = 0.5f + 0.5f * shp(s, wrap1(ph + sto));
            float lo = xl.lp(L[i]), hi = L[i] - lo;
            L[i] = softclip((lo * (1 - dep * a) + hi * (1 - dep * (1 - a))) * 1.3f * drive) / drive;
            lo = xr.lp(R[i]); hi = R[i] - lo;
            R[i] = softclip((lo * (1 - dep * b) + hi * (1 - dep * (1 - b))) * 1.3f * drive) / drive;
        }
    }
};

struct VintagePhaser : Algo {
    float sl[8] = {0}, sR[8] = {0}, ph = 0, fl = 0, fr = 0, env = 0;
    void init(float s) override { sr = s; }
    void reset() override { std::memset(sl, 0, sizeof sl); std::memset(sR, 0, sizeof sR); fl = fr = env = 0; }
    void process(float *L, float *R, int n) override {
        int mode = (int)p[1];
        float inc = p[2] / sr, dep = p[3] * 0.025f, man = p[4] * 0.02f - 1, fb = p[5] * 0.01f, age = p[6] * 0.01f, rel = 1.f - std::exp(-1.f / (0.2f * sr));
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), am = std::fabs(m);
            env = am > env ? env + 0.05f * (am - env) : env + rel * (am - env);
            ph += inc; if (ph >= 1) ph -= 1;
            float a = mode == 0 ? lfoSin(ph) : mode == 1 ? std::min(1.f, env * 8.f) * 2 - 1 : man;
            float b = mode == 0 ? lfoSin(wrap1(ph + 0.12f)) : a;
            float wa = kPi * 420.f * std::exp2(dep * a) / sr, wb = kPi * 420.f * std::exp2(dep * b) / sr, xl = m + fb * fl, xr = m + fb * fr;
            for (int k = 0; k < 8; k++) {
                float sp = 1.f + age * 0.22f * (float)(k % 4), w1 = std::min(1.2f, wa * sp), w2 = std::min(1.2f, wb * sp);
                float al = (w1 - 1) / (w1 + 1), ar = (w2 - 1) / (w2 + 1);
                float y = al * xl + sl[k]; sl[k] = xl - al * y; xl = y;
                y = ar * xr + sR[k]; sR[k] = xr - ar * y; xr = y;
            }
            fl = softclip(xl); fr = softclip(xr);
            L[i] = xl; R[i] = xr;
        }
    }
};

// Through-zero flanger: the dry path sits on a fixed delay and the wet path sweeps across it.
struct TapeFlanger : Algo {
    Delay dd, dw; float ph = 0, env = 0, pos = 0, vel = 0, fbv = 0;
    void init(float s) override { sr = s; dd.init(sr * 0.02f); dw.init(sr * 0.04f); }
    void reset() override { dd.clear(); dw.clear(); env = pos = vel = fbv = 0; }
    void process(float *L, float *R, int n) override {
        int mode = (int)p[1];
        float inc = p[2] / sr, dep = p[3] * 0.01f, man = p[4] * 0.01f, fb = p[5] * 0.01f, rel = 1.f - std::exp(-1.f / (0.2f * sr));
        float kS = 0.0004f + (1.f - p[6] * 0.01f) * 0.02f, damp = 0.02f + (1.f - p[6] * 0.01f) * 0.5f, d0 = 0.004f * sr;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), am = std::fabs(m);
            env = am > env ? env + 0.05f * (am - env) : env + rel * (am - env);
            ph += inc; if (ph >= 1) ph -= 1;
            float tg = mode == 0 ? 0.5f + 0.5f * lfoSin(ph) : mode == 1 ? std::min(1.f, env * 8.f) : man;
            vel += (tg - pos) * kS - vel * damp; pos += vel;  // "bounce": a sprung tape reel
            float sweep = clampf(pos, -0.2f, 1.2f) * dep;
            dd.write(m); dw.write(m + softclip(fb * fbv));
            float dry = dd.herm(d0), a = dw.herm(1.5f + sweep * 2.f * d0), b = dw.herm(1.5f + sweep * 2.2f * d0);
            fbv = a;
            L[i] = 0.75f * (dry + a); R[i] = 0.75f * (dry + b);
        }
    }
};

// ------------------------------------------------------------------ REVERB

struct Spring : Algo {
    static const int NS = 12;
    Allpass ap[3][NS]; Delay d[3]; OnePole lp[3]; float state[3] = {0, 0, 0}, len[3], g[3], apG = 0.6f;
    void init(float s) override {
        sr = s;
        static const float ms[3] = {37.f, 43.f, 53.f};
        for (int k = 0; k < 3; k++) {
            for (auto &a : ap[k]) a.init(24);
            len[k] = ms[k] * 0.001f * sr;
            d[k].init((int)len[k] + 8);
        }
    }
    void reset() override {
        for (int k = 0; k < 3; k++) { for (auto &a : ap[k]) a.d.clear(); d[k].clear(); lp[k].reset(); state[k] = 0; }
    }
    void update() override {
        apG = 0.3f + p[2] * 0.0045f;
        for (int k = 0; k < 3; k++) {
            int K = (int)clampf((float)(2 + k) * (0.5f + p[5] * 0.02f) * sr / 48000.f, 1, 22);
            for (auto &a : ap[k]) a.len = K;
            g[k] = std::pow(10.f, -3.f * (len[k] + (float)(K * NS)) / (p[1] * sr));
            lp[k].setLP(p[4], sr);
        }
    }
    void process(float *L, float *R, int n) override {
        int ns = (int)clampf(p[3], 1, 3);
        float w = p[6] * 0.01f, norm = 1.2f / std::sqrt((float)ns);
        static const float pan[3] = {-1, 1, 0};
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), l = 0, r = 0;
            for (int k = 0; k < ns; k++) {
                float z = m + g[k] * state[k];
                for (auto &a : ap[k]) z = a.tick(z, apG);
                d[k].write(z);
                state[k] = lp[k].lp(d[k].at((int)len[k]));
                l += state[k] * (0.5f - 0.5f * pan[k] * w); r += state[k] * (0.5f + 0.5f * pan[k] * w);
            }
            L[i] = l * norm; R[i] = r * norm;
        }
    }
};

struct DualVerb : Algo {
    Verb a, b; Delay pre;
    void init(float s) override { sr = s; a.init(sr, 2.1f); b.init(sr, 2.1f); pre.init(sr * 0.55f); }
    void reset() override { a.reset(); b.reset(); pre.clear(); }
    void update() override {
        a.set(0.3f + p[2] * 0.017f, p[1], p[3], 90, 0.7f, 4, 0.5f);
        b.set(0.3f + p[5] * 0.017f, p[4], p[6], 90, 0.7f, 6, 0.4f);
    }
    void process(float *L, float *R, int n) override {
        float bal = p[7] * 0.01f, ga = std::cos(bal * kPi * 0.5f) * 1.3f, gb = std::sin(bal * kPi * 0.5f) * 1.3f, pd = p[8] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), al, ar, bl, br;
            pre.write(m);
            float mb = pre.lin(pd);
            a.tick(m, m, al, ar); b.tick(mb, mb, bl, br);
            L[i] = ga * al + gb * bl; R[i] = ga * ar + gb * br;
        }
    }
};

struct ReverseVerb : Algo {
    static const int NT = 40;
    Delay d; Allpass ap[4]; Verb tail; OnePole tl, tr; int td[NT], last = 1; float gl[NT], gr[NT], fbv = 0;
    void init(float s) override {
        sr = s; d.init(sr * 3.1f); tail.init(sr, 1.f);
        static const float ms[4] = {5.3f, 7.9f, 6.1f, 9.7f};
        for (int i = 0; i < 4; i++) ap[i].init((int)(ms[i] * 0.001f * sr));
    }
    void reset() override { d.clear(); for (auto &a : ap) a.d.clear(); tail.reset(); tl.reset(); tr.reset(); fbv = 0; }
    void update() override {
        Rng rng;
        float len = p[1] * 0.001f * sr, sum = 0;
        for (int i = 0; i < NT; i++) {
            float x = ((float)i + rnd01(rng)) / (float)NT, g = x * x;
            td[i] = (int)clampf((1.f - x) * len, 1, d.maxDelay());  // late taps of the swell are the earliest sound
            td[i] = (int)clampf(len - (float)td[i], 1, d.maxDelay());
            sum += g * g;
            gl[i] = g * ((i & 1) ? 0.4f : 1.f); gr[i] = g * ((i & 1) ? 1.f : 0.4f);
        }
        float norm = 1.5f / std::sqrt(sum);
        for (int i = 0; i < NT; i++) { gl[i] *= norm; gr[i] *= norm; }
        last = (int)clampf(len, 1, d.maxDelay());
        tail.set(0.8f, 1.4f, 6000, 120, 0.7f, 4, 0.5f);
        tl.setLP(p[4], sr); tr.setLP(p[4], sr);
    }
    void process(float *L, float *R, int n) override {
        float df = p[2] * 0.007f, ta = p[3] * 0.012f, fb = p[5] * 0.01f;
        for (int i = 0; i < n; i++) {
            d.write(0.5f * (L[i] + R[i]) + fb * fbv);
            float l = 0, r = 0, vl, vr;
            for (int t = 0; t < NT; t++) { float s = d.at(td[t]); l += s * gl[t]; r += s * gr[t]; }
            fbv = softclip(d.at(last));
            l = tl.lp(ap[1].tick(ap[0].tick(l, df), df)); r = tr.lp(ap[3].tick(ap[2].tick(r, df), df));
            tail.tick(l, r, vl, vr);
            L[i] = l + ta * vl; R[i] = r + ta * vr;
        }
    }
};

struct ModEchoVerb : Algo {
    Verb v; Delay echo, cl, cr; OnePole tone; Smooth t; float ph = 0;
    void init(float s) override { sr = s; v.init(sr, 2.2f); echo.init(sr * 1.6f); cl.init(sr * 0.03f); cr.init(sr * 0.03f); t.setTime(90, sr); }
    void reset() override { v.reset(); echo.clear(); cl.clear(); cr.clear(); tone.reset(); t.snap(p[3] * 0.001f * sr); }
    void update() override { v.set(0.4f + p[2] * 0.017f, p[1], p[8], 90, 0.72f, 6, 0.5f); tone.setLP(p[5], sr); }
    void process(float *L, float *R, int n) override {
        float fb = p[4] * 0.01f, inc = p[6] / sr, dep = p[7] * 0.01f, cd = 0.003f * sr, tt = p[3] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), dt = t.next(tt), e = dt > 1.f ? echo.lin(dt) : 0.f, vl, vr;
            echo.write(m + fb * tone.lp(e));
            v.tick(m + 0.8f * e, m + 0.8f * e, vl, vr);
            ph += inc; if (ph >= 1) ph -= 1;
            cl.write(vl); cr.write(vr);
            L[i] = vl * (1 - 0.5f * dep) + dep * cl.herm(2 + cd * (1 + lfoSin(ph)));
            R[i] = vr * (1 - 0.5f * dep) + dep * cr.herm(2 + cd * (1 + lfoSin(wrap1(ph + 0.25f))));
        }
    }
};

// Shared core for reverbs that post-process the tail
struct VerbBase : Algo {
    Verb v; Delay pre;
    void init(float s) override { sr = s; v.init(sr, 2.2f); pre.init(sr * 0.3f); }
    void reset() override { v.reset(); pre.clear(); }
    inline void verb(float m, float pd, float &l, float &r) {
        pre.write(m);
        float x = pre.lin(pd);
        v.tick(x, x, l, r);
    }
};

struct MangledVerb : VerbBase {
    Biquad lo[2], hi[2];
    void reset() override { VerbBase::reset(); for (int c = 0; c < 2; c++) { lo[c].reset(); hi[c].reset(); } }
    void update() override {
        v.set(0.4f + p[2] * 0.017f, p[1], 9000, 90, 0.7f, p[7] * 0.4f, 0.8f);
        for (int c = 0; c < 2; c++) { lo[c].set(BQ_LOSHELF, 200, 0.7f, p[5], sr); hi[c].set(BQ_HISHELF, 3000, 0.7f, p[6], sr); }
    }
    void process(float *L, float *R, int n) override {
        float pd = p[3] * 0.001f * sr, drive = db2lin(p[4] * 0.36f), lvl = db2lin(p[8]) / std::sqrt(drive);
        for (int i = 0; i < n; i++) {
            float l, r;
            verb(0.5f * (L[i] + R[i]), pd, l, r);
            L[i] = hi[0].tick(lo[0].tick(softclip(l * drive))) * lvl; R[i] = hi[1].tick(lo[1].tick(softclip(r * drive))) * lvl;
        }
    }
};

struct TremoloVerb : VerbBase {
    float ph = 0;
    void update() override { v.set(0.4f + p[2] * 0.017f, p[1], p[6], 90, 0.7f, 5, 0.5f); }
    void process(float *L, float *R, int n) override {
        float pd = p[7] * 0.001f * sr, inc = p[3] / sr, dep = p[4] * 0.01f;
        int s = (int)p[5];
        for (int i = 0; i < n; i++) {
            float l, r;
            verb(0.5f * (L[i] + R[i]), pd, l, r);
            ph += inc; if (ph >= 1) ph -= 1;
            float g = (1 - dep * (0.5f - 0.5f * shp(s, ph))) * 1.2f;
            L[i] = l * g; R[i] = r * g;
        }
    }
};

struct DynaVerb : VerbBase {
    float env = 0, sw = 0, gs = 1;
    void reset() override { VerbBase::reset(); env = sw = 0; gs = 1; }
    void update() override { v.set(0.4f + p[2] * 0.017f, p[1], p[7], 90, 0.7f, 5, 0.5f); }
    void process(float *L, float *R, int n) override {
        int mode = (int)p[3];
        float sens = p[4] * 0.2f, amt = p[5] * 0.01f, rel = 1.f - std::exp(-1.f / (p[6] * 0.001f * sr)), sm = 1.f - std::exp(-1.f / (0.01f * sr));
        float swUp = 1.f / (0.6f * sr);
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), am = std::fabs(m), l, r;
            env = am > env ? env + 0.3f * (am - env) : env + rel * (am - env);
            float c = std::min(1.f, env * sens), in = m;
            if (mode == 2) {  // swell: fade the reverb send in after each attack
                sw = c > 0.08f ? std::min(1.f, sw + swUp) : sw * (1 - rel);
                in = m * (1 - amt + amt * sw);
            }
            verb(in, 0, l, r);
            float tg = mode == 0 ? 1 - amt * c : mode == 1 ? 1 - amt * (1 - c) : 1.f;
            gs += sm * (tg - gs);
            L[i] = l * gs; R[i] = r * gs;
        }
    }
};

struct Wormhole : Algo {
    Verb v; Shifter sh; Delay pre; Smooth pt; float fbs = 0;
    void init(float s) override { sr = s; v.init(sr, 7.8f); sh.init(sr, 0.25f); sh.setGrain(60); pre.init(sr * 2.1f); pt.setTime(120, sr); }
    void reset() override { v.reset(); sh.reset(); pre.clear(); fbs = 0; pt.snap(p[8] * 0.001f * sr); }
    void update() override {
        v.set(1.f + p[2] * 0.06f, p[1], p[6], p[7], 0.75f, p[3] * 0.9f, p[4]);
        sh.ratio = semi2ratio(p[5] * 0.01f);
    }
    void process(float *L, float *R, int n) override {
        float pd = p[8] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            pre.write(0.5f * (L[i] + R[i]));
            float x = pre.lin(pt.next(pd)) * 0.7f + fbs;
            v.tick(x, x, L[i], R[i]);
            fbs = softclip(sh.tick(0.5f * (L[i] + R[i])) * 0.3f);
        }
    }
};

struct StudioRoom : Algo {
    Verb v; Delay pre; Biquad lo[2], hi[2]; int et[8];
    void init(float s) override { sr = s; v.init(sr, 1.4f); pre.init(sr * 0.45f); }
    void reset() override { v.reset(); pre.clear(); for (int c = 0; c < 2; c++) { lo[c].reset(); hi[c].reset(); } }
    void update() override {
        static const float ms[8] = {7, 11, 17, 23, 31, 41, 47, 59};
        float size = 0.5f + p[7] * 0.015f;
        for (int i = 0; i < 8; i++) et[i] = (int)((p[3] + ms[i] * size) * 0.001f * sr);
        v.set(0.3f + p[7] * 0.01f, p[1], 12000, 60, p[4] * 0.0075f, 3, 0.6f);
        for (int c = 0; c < 2; c++) { lo[c].set(BQ_LOSHELF, 250, 0.7f, p[5], sr); hi[c].set(BQ_HISHELF, 4000, 0.7f, p[6], sr); }
    }
    void process(float *L, float *R, int n) override {
        float pos = p[2] * 0.01f, ge = std::cos(pos * kPi * 0.5f), gt = std::sin(pos * kPi * 0.5f) * 1.3f;
        int pd = (int)(p[3] * 0.001f * sr);
        for (int i = 0; i < n; i++) {
            pre.write(0.5f * (L[i] + R[i]));
            float el = 0, er = 0, vl, vr, x = pre.at(pd);
            for (int t = 0; t < 8; t++) { float s = pre.at(et[t]) * (0.9f - 0.08f * (float)t); ((t & 1) ? er : el) += s; }
            v.tick(x, x, vl, vr);
            L[i] = hi[0].tick(lo[0].tick(ge * (el + 0.3f * er) * 0.6f + gt * vl));
            R[i] = hi[1].tick(lo[1].tick(ge * (er + 0.3f * el) * 0.6f + gt * vr));
        }
    }
};

// ------------------------------------------------------------------- DRIVE

struct Sculpt : Algo {
    OnePole xo, tone, tone2; OS2 os; float env = 0;
    void init(float s) override { sr = s; }
    void reset() override { xo.reset(); tone.reset(); tone2.reset(); os.reset(); env = 0; }
    void update() override { xo.setLP(p[1], sr); tone.setLP(p[6], sr); tone2.setLP(p[6], sr); }
    void process(float *L, float *R, int n) override {
        float dl = db2lin(p[2] * 0.36f), dh = db2lin(p[3] * 0.42f), comp = p[4] * 0.01f, sp = p[5] * 0.01f, lvl = db2lin(p[7]) * 0.7f;
        float rel = 1.f - std::exp(-1.f / (0.15f * sr));
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), am = std::fabs(m);
            env = am > env ? env + 0.2f * (am - env) : env + rel * (am - env);
            m *= (1.f + 2.f * comp) / (1.f + comp * env * 8.f);
            float lo = xo.lp(m), hi = m - lo;
            lo = softclip(lo * dl) / std::sqrt(dl);
            hi = os.run(hi * dh, [](float v) { return softclip(v + 0.1f) - 0.0997f; }) / std::sqrt(dh);
            L[i] = tone.lp(lo + hi * (1 - sp)) * lvl; R[i] = tone2.lp(lo * (1 - sp) + hi) * lvl;
        }
    }
};

struct MidDrive : Algo {
    OnePole hp, tone; Biquad mid; OS2 os;
    void init(float s) override { sr = s; hp.setLP(90, sr); }
    void reset() override { hp.reset(); tone.reset(); mid.reset(); os.reset(); }
    void update() override { mid.set(BQ_PEAK, p[2], 0.7f, p[3], sr); tone.setLP(p[4], sr); }
    void process(float *L, float *R, int n) override {
        float drive = db2lin(p[1] * 0.4f), lvl = db2lin(p[6]) * 0.6f;
        bool two = p[5] > 1.5f;
        for (int i = 0; i < n; i++) {
            float x = mid.tick(hp.hp(0.5f * (L[i] + R[i]))) * drive;
            float y = os.run(x, [two](float v) {
                v = softclip(v + 0.1f) - 0.0997f;
                return two ? softclip(v * 2.5f) * 0.8f : v;
            });
            y = tone.lp(y) * lvl;
            L[i] = y; R[i] = y;
        }
    }
};

struct Aggro : Algo {
    OnePole hp, post; Biquad bq[3]; OS2 os; float env = 0, gate = 1;
    void init(float s) override { sr = s; post.setLP(6500, sr); }
    void reset() override { hp.reset(); post.reset(); for (auto &b : bq) b.reset(); os.reset(); env = 0; gate = 1; }
    void update() override {
        hp.setLP(p[2], sr);
        bq[0].set(BQ_LOSHELF, 110, 0.7f, p[3], sr); bq[1].set(BQ_PEAK, 750, 0.7f, p[4], sr); bq[2].set(BQ_HISHELF, 3500, 0.7f, p[5], sr);
    }
    void process(float *L, float *R, int n) override {
        float drive = db2lin(p[1] * 0.6f), thr = p[6] > 0.5f ? db2lin(-75.f + p[6] * 0.45f) : 0.f, lvl = db2lin(p[7]) * 0.5f;
        float rel = std::exp(-1.f / (0.03f * sr)), ga = 1.f - std::exp(-1.f / (0.001f * sr)), gr = 1.f - std::exp(-1.f / (0.06f * sr));
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), am = std::fabs(m);
            env = am > env ? am : env * rel;
            float tg = env >= thr ? 1.f : 0.f;
            gate += (tg - gate) * (tg > gate ? ga : gr);
            float y = os.run(hp.hp(m) * drive, [](float v) { return softclip(clampf(softclip(v * 1.8f) * 1.3f, -0.95f, 0.95f) * 1.5f); });
            y = post.lp(bq[2].tick(bq[1].tick(bq[0].tick(y)))) * lvl * gate;
            L[i] = y; R[i] = y;
        }
    }
};

// ------------------------------------------------------------------- MULTI

struct SpaceTime : Algo {
    Delay cd, dd; Verb v; OnePole col; Smooth t; float ph = 0;
    void init(float s) override { sr = s; cd.init(sr * 0.05f); dd.init(sr * 1.6f); v.init(sr, 2.2f); t.setTime(90, sr); }
    void reset() override { cd.clear(); dd.clear(); v.reset(); col.reset(); t.snap(p[4] * 0.001f * sr); }
    void update() override { v.set(1.3f, p[7], p[8], 90, 0.72f, 6, 0.5f); col.setLP(p[8], sr); }
    void process(float *L, float *R, int n) override {
        float ma = p[1] * 0.01f, inc = p[2] / sr, dlv = p[3] * 0.012f, fb = p[5] * 0.01f, vlv = p[6] * 0.014f, cdp = 0.004f * sr, tt = p[4] * 0.001f * sr;
        bool par = p[9] > 0.5f;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), vl, vr;
            ph += inc; if (ph >= 1) ph -= 1;
            cd.write(m);
            float cl = m * (1 - 0.5f * ma) + ma * cd.herm(0.008f * sr + cdp * lfoSin(ph));
            float cr = m * (1 - 0.5f * ma) + ma * cd.herm(0.008f * sr + cdp * lfoSin(wrap1(ph + 0.33f)));
            float e = dd.lin(t.next(tt));
            dd.write(0.5f * (cl + cr) + fb * col.lp(e));
            e *= dlv;
            float vi = par ? 0.f : e;
            v.tick(cl + vi, cr + vi, vl, vr);
            L[i] = cl + e + vlv * vl; R[i] = cr + e + vlv * vr;
        }
    }
};

// ------------------------------------------------------------------- SYNTH

// Polyphonic synth: an FFT finds the fundamentals being played (harmonics of
// lower notes are rejected) and each one drives its own oscillator voice.
struct PolySynth : Algo {
    static const int NV = 6;
    struct Voice { float f = 0, tf = 0, amp = 0, tamp = 0, ph = 0, ph2 = 0; bool on = false, hit = false; };
    Voice v[NV];
    int N = 4096, hop = 512, w = 0, cnt = 0;
    vDSP_Length log2n = 12;
    FFTSetup setup = nullptr;
    std::vector<float> ring, win, re, im, mag;
    SVF fl; float env = 0;

    ~PolySynth() override { if (setup) vDSP_destroy_fftsetup(setup); }
    void init(float s) override {
        sr = s;
        log2n = sr > 140000 ? 14 : sr > 70000 ? 13 : 12;
        N = 1 << log2n; hop = N / 8;
        if (setup) vDSP_destroy_fftsetup(setup);
        setup = vDSP_create_fftsetup(log2n, kFFTRadix2);
        ring.assign((size_t)N, 0); win.assign((size_t)N, 0); re.assign((size_t)N, 0); im.assign((size_t)N, 0); mag.assign((size_t)N / 2, 0);
        for (int i = 0; i < N; i++) win[(size_t)i] = 0.5f - 0.5f * std::cos(2.f * kPi * (float)i / (float)N);
    }
    void reset() override {
        std::fill(ring.begin(), ring.end(), 0.f);
        for (auto &x : v) x = Voice();
        fl.reset(); env = 0; w = cnt = 0;
    }
    void analyze() {
        for (int i = 0; i < N; i++) { re[(size_t)i] = ring[(size_t)((w + i) & (N - 1))] * win[(size_t)i]; im[(size_t)i] = 0; }
        DSPSplitComplex sc = {re.data(), im.data()};
        vDSP_fft_zip(setup, &sc, 1, log2n, FFT_FORWARD);
        int kmin = std::max(2, (int)(68.f * (float)N / sr)), kmax = std::min(N / 2 - 2, (int)(1400.f * (float)N / sr));
        float top = 0;
        for (int k = kmin - 1; k <= kmax + 1; k++) {
            mag[(size_t)k] = std::sqrt(re[(size_t)k] * re[(size_t)k] + im[(size_t)k] * im[(size_t)k]);
            top = std::max(top, mag[(size_t)k]);
        }
        float thr = std::max(0.003f * (float)N * 0.25f, 0.14f * top);
        float cf[32], cm[32];
        int nc = 0;
        for (int k = kmin; k <= kmax && nc < 32; k++) {
            float a = mag[(size_t)k - 1], b = mag[(size_t)k], c = mag[(size_t)k + 1];
            if (b < thr || b <= a || b < c) continue;
            float la = std::log(a + 1e-9f), lb = std::log(b), lc = std::log(c + 1e-9f), den = la - 2 * lb + lc;
            float off = std::fabs(den) > 1e-9f ? clampf(0.5f * (la - lc) / den, -0.5f, 0.5f) : 0.f;
            cf[nc] = ((float)k + off) * sr / (float)N;
            cm[nc] = b;
            nc++;
        }
        // keep fundamentals: drop peaks that sit on a harmonic of an accepted lower note
        float af[32], am[32];
        int na = 0;
        for (int c = 0; c < nc; c++) {
            bool harm = false;
            for (int a = 0; a < na && !harm; a++) {
                float ratio = cf[c] / af[a], r = std::round(ratio);
                if (r >= 2 && r <= 8 && std::fabs(ratio - r) < 0.035f * r && cm[c] < 1.6f * am[a]) harm = true;
            }
            if (!harm) { af[na] = cf[c]; am[na] = cm[c]; na++; }
        }
        int nv = (int)clampf(p[9], 1, NV);
        while (na > nv) {  // too many: drop the weakest
            int weakest = 0;
            for (int a = 1; a < na; a++) if (am[a] < am[weakest]) weakest = a;
            af[weakest] = af[na - 1]; am[weakest] = am[na - 1]; na--;
        }
        bool used[32] = {false};
        for (auto &x : v) {
            x.hit = false;
            if (!x.on) continue;
            for (int a = 0; a < na; a++) {
                if (used[a] || std::fabs(af[a] / x.tf - 1.f) > 0.035f) continue;
                x.tf = af[a]; x.tamp = std::min(1.f, am[a] / ((float)N * 0.25f) * 5.f);
                used[a] = true; x.hit = true;
                break;
            }
            if (!x.hit) x.tamp = 0;
        }
        for (int a = 0; a < na; a++) {
            if (used[a]) continue;
            for (auto &x : v) {
                if (x.on && (x.hit || x.amp > 0.002f)) continue;
                x.on = true; x.hit = true; x.f = x.tf = af[a]; x.amp = 0;
                x.tamp = std::min(1.f, am[a] / ((float)N * 0.25f) * 5.f);
                break;
            }
        }
    }
    static inline float blep(float t, float dt) {
        if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
        if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
        return 0.f;
    }
    void process(float *L, float *R, int n) override {
        int wave = (int)p[2];
        float mult = std::exp2(p[1]), det = semi2ratio(p[3] * 0.01f), k = 1.f / p[5];
        float atk = 1.f - std::exp(-1.f / (p[7] * 0.001f * sr)), rel = 1.f - std::exp(-1.f / (p[8] * 0.001f * sr)), erel = 1.f - std::exp(-1.f / (0.2f * sr));
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), am = std::fabs(m), o = 0;
            ring[(size_t)w] = m; w = (w + 1) & (N - 1);
            if (++cnt >= hop) { cnt = 0; analyze(); }
            env = am > env ? env + 0.1f * (am - env) : env + erel * (am - env);
            for (auto &x : v) {
                if (!x.on) continue;
                x.amp += (x.tamp - x.amp) * (x.tamp > x.amp ? atk : rel);
                if (x.tamp == 0 && x.amp < 1e-4f) { x.on = false; continue; }
                x.f += (x.tf - x.f) * 0.003f;
                float dt = clampf(x.f * mult, 20.f, sr * 0.2f) / sr, dt2 = dt * det, s;
                x.ph += dt; if (x.ph >= 1) x.ph -= 1;
                x.ph2 += dt2; if (x.ph2 >= 1) x.ph2 -= 1;
                if (wave == 0) s = (2 * x.ph - 1 - blep(x.ph, dt)) + 0.6f * (2 * x.ph2 - 1 - blep(x.ph2, dt2));
                else if (wave == 1) s = (x.ph < 0.5f ? 1.f : -1.f) + blep(x.ph, dt) - blep(wrap1(x.ph + 0.5f), dt) + 0.6f * (2 * x.ph2 - 1 - blep(x.ph2, dt2));
                else s = 1.2f * (lfoSin(x.ph) + 0.5f * lfoSin(wrap1(2 * x.ph)) + 0.3f * lfoSin(wrap1(3 * x.ph)) + 0.2f * lfoSin(wrap1(4 * x.ph2)));
                o += s * x.amp;
            }
            fl.tick(o * 0.45f, p[4] * std::exp2(p[6] * std::min(1.f, env * 6.f)), k, sr);
            float y = softclip(fl.lp);
            L[i] = y; R[i] = y;
        }
    }
};

// ---------------------------------------------------------------- GRANULAR

struct Glitch : Algo {
    std::vector<float> cap; Rng rng; int pos = 0, len = 6000, reps = 0; float rate = 1; bool rev = false;
    bool spills() const override { return false; }
    void init(float s) override { sr = s; cap.assign((size_t)(sr * 0.55f) + 8, 0.f); }
    void reset() override { std::fill(cap.begin(), cap.end(), 0.f); pos = 0; reps = 0; }
    void process(float *L, float *R, int n) override {
        static const float semis[7] = {0, 12, -12, 7, -5, 24, -24};
        len = (int)clampf(p[2] * 0.001f * sr, 64.f, (float)cap.size() - 4.f);
        if (pos >= len) pos = 0;
        int fadeN = std::max(8, std::min(len / 4, (int)(0.003f * sr))), maxRep = (int)clampf(p[3], 1, 8), bits = (int)std::round(p[6]);
        float q = std::exp2((float)bits - 1.f);
        for (int i = 0; i < n; i++) {
            if (reps > 0) {
                float idx = std::fmod((float)pos * rate, (float)len);
                if (rev) idx = (float)(len - 1) - idx;
                int i0 = (int)idx, i1 = i0 + 1 >= len ? 0 : i0 + 1;
                float o = cap[(size_t)i0] + (idx - (float)i0) * (cap[(size_t)i1] - cap[(size_t)i0]);
                if (bits < 16) o = std::round(o * q) / q;
                float e = pos < fadeN ? (float)pos / (float)fadeN : pos >= len - fadeN ? (float)(len - pos) / (float)fadeN : 1.f;
                L[i] = o * e; R[i] = o * e;
            } else {
                cap[(size_t)pos] = 0.5f * (L[i] + R[i]);
            }
            if (++pos >= len) {
                pos = 0;
                if (reps > 0) reps--;
                if (reps == 0 && rnd01(rng) < p[1] * 0.01f) {
                    reps = 1 + (int)(rnd01(rng) * ((float)maxRep - 0.01f));
                    float s = 0;
                    for (int tries = 0; tries < 4; tries++) {
                        s = semis[(int)(rnd01(rng) * 6.99f)];
                        if (std::fabs(s) <= p[4]) break;
                        s = 0;
                    }
                    rate = semi2ratio(s);
                    rev = rnd01(rng) < p[5] * 0.01f;
                }
            }
        }
    }
};

template <class T> Algo *mk() { return new T(); }

}  // namespace

void appendMoreAlgos(std::vector<AlgoInfo> &r) {
    std::vector<AlgoInfo> more = {
        {"Mod Shift", "Pitch", "Dual pitch shifter whose intervals are swept by an LFO",
         {mix(50), st("Pitch A", -24, 24, 7, nullptr, "st"), st("Pitch B", -24, 24, -5, nullptr, "st"), lin("Mod Depth", 0, 1200, 30, "ct"), lg("Mod Rate", 0.05f, 20, 2, "Hz"),
          st("Shape", 0, 3, 0, kShapesR), tm("Delay", 0, 2000, 0), lin("Feedback", 0, 100, 0, "%")},
         mk<ModShift>},
        {"Prism", "Pitch", "Three polyphonic voices played as a chord or arpeggiated in time",
         {mix(55), st("Voice 1", -24, 24, 7, nullptr, "st"), st("Voice 2", -24, 24, 12, nullptr, "st"), st("Voice 3", -24, 24, 19, nullptr, "st"), tm("Step", 30, 1000, 166),
          st("Mode", 0, 3, 1, kPrism), lin("Smooth", 0, 100, 30, "%"), lin("Feedback", 0, 90, 30, "%"), lin("Width", 0, 100, 100, "%")},
         mk<Prism>},
        {"Pitch Fuzz", "Pitch", "Fuzz into two pitch shifters and a delay",
         {mix(100), lin("Fuzz", 0, 100, 60, "%"), lg("Fuzz Tone", 500, 10000, 3000, "Hz"), st("Pitch A", -24, 24, 12, nullptr, "st"), st("Pitch B", -24, 24, -12, nullptr, "st"),
          lin("Pitch Mix", 0, 100, 50, "%"), tm("Delay", 0, 1000, 0), lin("Feedback", 0, 90, 30, "%"), lin("Level", -24, 12, -6, "dB")},
         mk<PitchFuzz>},
        {"Formant Shift", "Pitch", "Polyphonic shifter that keeps or moves the formants, for voice and natural-sounding shifts",
         {mix(100), st("Pitch", -12, 12, 3, nullptr, "st"), lin("Fine", -50, 50, 0, "ct"), lin("Formant", -12, 12, 0, "st")},
         mk<FormantShift>},

        {"Vintage Delay", "Delay", "Early digital delay with reduced bit depth and sample rate",
         {mix(35), tm("Time", 1, 2000, 400), lin("Feedback", 0, 100, 45, "%"), lin("Bits", 4, 16, 10, "bit"), lg("Sample Rate", 2000, 48000, 12000, "Hz"), lg("Hi Cut", 800, 20000, 6000, "Hz"),
          lin("Mod Depth", 0, 100, 10, "%")},
         mk<VintageDelay>},
        {"Band Delay", "Delay", "Delay with a swept resonant filter in the feedback path",
         {mix(40), tm("Time", 1, 2000, 375), lin("Feedback", 0, 100, 50, "%"), lg("Frequency", 100, 8000, 800, "Hz"), lg("Resonance", 0.7f, 12, 3), st("Type", 0, 2, 1, kFilt),
          lg("LFO Rate", 0.05f, 10, 0.3f, "Hz"), lin("LFO Depth", 0, 100, 40, "%")},
         mk<BandDelay>},
        {"Filter Pong", "Delay", "Ping-pong delay whose bounces are swept by opposing filters",
         {mix(40), tm("Time", 1, 2000, 375), lin("Feedback", 0, 100, 55, "%"), lg("Frequency", 200, 10000, 2000, "Hz"), lg("Resonance", 0.7f, 12, 2.5f), lg("LFO Rate", 0.05f, 10, 0.4f, "Hz"),
          lin("LFO Depth", 0, 100, 50, "%")},
         mk<FilterPong>},
        {"Multi Head", "Delay", "Four-head tape echo with a level for every playback head",
         {mix(40), tm("Time", 20, 500, 150), lin("Head 1", 0, 100, 100, "%"), lin("Head 2", 0, 100, 60, "%"), lin("Head 3", 0, 100, 0, "%"), lin("Head 4", 0, 100, 40, "%"),
          lin("Feedback", 0, 100, 35, "%"), lin("Saturation", 0, 100, 40, "%"), lin("Wow", 0, 100, 25, "%"), lg("Tone", 800, 9000, 4000, "Hz")},
         mk<MultiHead>},
        {"Bucket Delay", "Delay", "Bucket-brigade analog delay that darkens as it gets longer",
         {mix(40), tm("Time", 20, 1200, 300), lin("Feedback", 0, 110, 45, "%"), lg("Tone", 800, 12000, 5000, "Hz"), lin("Mod Depth", 0, 100, 30, "%"), lg("Mod Rate", 0.05f, 8, 0.5f, "Hz"),
          lin("Grit", 0, 100, 30, "%")},
         mk<BucketDelay>},

        {"Mod Filter", "Modulation", "Stereo filters swept by an LFO, random steps or your picking",
         {mix(100), st("Type", 0, 2, 0, kFilt), lg("Frequency", 80, 8000, 300, "Hz"), lg("Resonance", 0.7f, 12, 4), lg("Rate", 0.05f, 20, 1, "Hz"), lin("Depth", 0, 5, 3, "oct"),
          st("Shape", 0, 4, 0, kShapesE), lin("Stereo", 0, 180, 0, "deg")},
         mk<ModFilter>},
        {"Vibrato", "Modulation", "True pitch vibrato that can fade in after each note",
         {mix(100), lg("Rate", 0.5f, 12, 5, "Hz"), lin("Depth", 0, 100, 35, "%"), st("Shape", 0, 1, 0, kSineTri), lg("Rise", 1, 2000, 1, "ms"), lin("Stereo", 0, 180, 0, "deg")},
         mk<Vibrato>},
        {"Undulator", "Modulation", "Detuned echoes under a tremolo whose speed drifts",
         {mix(50), lin("Detune", 0, 50, 12, "ct"), lin("Delay", 0, 500, 180, "ms"), lin("Feedback", 0, 90, 40, "%"), lin("Trem Depth", 0, 100, 60, "%"), lg("Trem Rate", 0.1f, 15, 3, "Hz"),
          lin("Width", 0, 100, 100, "%"), lg("FM Rate", 0.02f, 5, 0.2f, "Hz"), lin("FM Depth", 0, 100, 50, "%")},
         mk<Undulator>},
        {"Harmonic Trem", "Modulation", "Bass and treble pulse in opposition, like a brownface amp",
         {mix(100), lg("Rate", 0.2f, 15, 4, "Hz"), lin("Depth", 0, 100, 80, "%"), lg("Crossover", 200, 3000, 800, "Hz"), st("Shape", 0, 3, 0, kShapes4), lin("Stereo", 0, 180, 0, "deg"),
          lin("Drive", 0, 100, 10, "%")},
         mk<HarmTrem>},
        {"Vintage Phaser", "Modulation", "Eight-stage studio phaser driven by LFO, envelope or by hand",
         {mix(50), st("Mode", 0, 2, 0, kModes3), lg("Rate", 0.02f, 10, 0.5f, "Hz"), lin("Depth", 0, 100, 80, "%"), lin("Manual", 0, 100, 50, "%"), lin("Feedback", 0, 90, 30, "%"),
          lin("Age", 0, 100, 30, "%")},
         mk<VintagePhaser>},
        {"Tape Flanger", "Modulation", "Through-zero tape flanging with a sprung, bouncing sweep",
         {mix(100), st("Mode", 0, 2, 0, kModes3), lg("Rate", 0.02f, 10, 0.2f, "Hz"), lin("Depth", 0, 100, 90, "%"), lin("Manual", 0, 100, 50, "%"), lin("Feedback", -90, 90, 40, "%"),
          lin("Bounce", 0, 100, 20, "%")},
         mk<TapeFlanger>},

        {"Spring", "Reverb", "Amp-style spring tank with adjustable drip and tension",
         {mix(35), lg("Decay", 0.5f, 8, 2.5f, "s"), lin("Tension", 0, 100, 60, "%"), st("Springs", 1, 3, 3), lg("Tone", 1000, 8000, 4000, "Hz"), lin("Drip", 0, 100, 50, "%"),
          lin("Width", 0, 100, 80, "%")},
         mk<Spring>},
        {"Dual Verb", "Reverb", "Two independent reverbs blended together",
         {mix(40), lg("Decay A", 0.2f, 20, 1.2f, "s"), lin("Size A", 0, 100, 30, "%"), lg("Tone A", 1000, 20000, 9000, "Hz"), lg("Decay B", 0.2f, 20, 6, "s"), lin("Size B", 0, 100, 80, "%"),
          lg("Tone B", 1000, 20000, 4000, "Hz"), lin("Balance", 0, 100, 50, "%"), lin("Pre-Delay B", 0, 500, 80, "ms")},
         mk<DualVerb>},
        {"Reverse Verb", "Reverb", "Reverb that swells up backwards before each note lands",
         {mix(50), tm("Length", 100, 3000, 900), lin("Diffusion", 0, 100, 70, "%"), lin("Tail", 0, 100, 30, "%"), lg("Tone", 1000, 20000, 8000, "Hz"), lin("Feedback", 0, 90, 0, "%")},
         mk<ReverseVerb>},
        {"Mod Echo Verb", "Reverb", "Reverb fed by an echo, with chorus on the tail",
         {mix(45), lg("Decay", 0.3f, 20, 3, "s"), lin("Size", 0, 100, 60, "%"), tm("Echo Time", 0, 1500, 350), lin("Echo Fdbk", 0, 95, 40, "%"), lg("Echo Tone", 800, 12000, 4000, "Hz"),
          lg("Mod Rate", 0.05f, 8, 0.6f, "Hz"), lin("Mod Depth", 0, 100, 50, "%"), lg("Hi Cut", 1000, 20000, 8000, "Hz")},
         mk<ModEchoVerb>},
        {"Mangled Verb", "Reverb", "Reverb slammed into distortion",
         {mix(45), lg("Decay", 0.3f, 20, 2.5f, "s"), lin("Size", 0, 100, 55, "%"), lin("Pre-Delay", 0, 250, 15, "ms"), lin("Overdrive", 0, 100, 50, "%"), lin("Low", -12, 12, 0, "dB"),
          lin("High", -12, 12, -3, "dB"), lin("Wobble", 0, 100, 30, "%"), lin("Level", -18, 6, -3, "dB")},
         mk<MangledVerb>},
        {"Tremolo Verb", "Reverb", "Reverb with tremolo on the tail",
         {mix(45), lg("Decay", 0.3f, 20, 3, "s"), lin("Size", 0, 100, 60, "%"), lg("Rate", 0.2f, 15, 4, "Hz"), lin("Depth", 0, 100, 70, "%"), st("Shape", 0, 3, 0, kShapes4),
          lg("Hi Cut", 1000, 20000, 8000, "Hz"), lin("Pre-Delay", 0, 250, 15, "ms")},
         mk<TremoloVerb>},
        {"Dyna Verb", "Reverb", "Reverb that ducks, gates or swells with your playing",
         {mix(45), lg("Decay", 0.3f, 20, 2.5f, "s"), lin("Size", 0, 100, 60, "%"), st("Mode", 0, 2, 0, kDyna), lin("Sensitivity", 0, 100, 60, "%"), lin("Amount", 0, 100, 80, "%"),
          lg("Release", 20, 2000, 300, "ms"), lg("Hi Cut", 1000, 20000, 8000, "Hz")},
         mk<DynaVerb>},
        {"Wormhole", "Reverb", "Enormous warped space with drifting pitch",
         {mix(50), lg("Gravity", 0.5f, 60, 15, "s"), lin("Size", 0, 100, 80, "%"), lin("Warp", 0, 100, 60, "%"), lg("Warp Rate", 0.02f, 3, 0.15f, "Hz"), lin("Drift", -50, 50, -8, "ct"),
          lg("Hi Cut", 1000, 20000, 5000, "Hz"), lg("Lo Cut", 20, 1000, 120, "Hz"), tm("Pre-Delay", 0, 2000, 40)},
         mk<Wormhole>},
        {"Studio Room", "Reverb", "Rack-style room with early reflections and a front-to-back position",
         {mix(35), lg("Decay", 0.2f, 10, 1.4f, "s"), lin("Position", 0, 100, 50, "%"), lin("Pre-Delay", 0, 200, 10, "ms"), lin("Diffusion", 0, 100, 80, "%"), lin("Low", -12, 12, 0, "dB"),
          lin("High", -12, 12, -2, "dB"), lin("Size", 0, 100, 50, "%")},
         mk<StudioRoom>},

        {"Sculpt", "Tone", "Two-band distortion: drive lows and highs separately",
         {mix(100), lg("Crossover", 100, 2000, 400, "Hz"), lin("Low Drive", 0, 100, 30, "%"), lin("High Drive", 0, 100, 60, "%"), lin("Compressor", 0, 100, 30, "%"), lin("Spread", 0, 100, 0, "%"),
          lg("Tone", 1000, 12000, 6000, "Hz"), lin("Level", -24, 12, -3, "dB")},
         mk<Sculpt>},
        {"Mid Drive", "Tone", "Mid-forward overdrive with one or two gain stages",
         {mix(100), lin("Gain", 0, 100, 50, "%"), lg("Mid Freq", 300, 2500, 720, "Hz"), lin("Mid Boost", 0, 18, 9, "dB"), lg("Tone", 1000, 10000, 4500, "Hz"), st("Stages", 1, 2, 1),
          lin("Level", -24, 12, -4, "dB")},
         mk<MidDrive>},
        {"Aggro", "Tone", "Tight high-gain distortion with a built-in gate",
         {mix(100), lin("Gain", 0, 100, 70, "%"), lg("Tight", 60, 500, 150, "Hz"), lin("Bass", -12, 12, 3, "dB"), lin("Mid", -12, 12, -4, "dB"), lin("Treble", -12, 12, 2, "dB"),
          lin("Gate", 0, 100, 30, "%"), lin("Level", -24, 12, -8, "dB")},
         mk<Aggro>},

        {"Space Time", "Multi", "Modulation, delay and reverb in one algorithm",
         {mix(50), lin("Mod Amount", 0, 100, 40, "%"), lg("Mod Rate", 0.05f, 8, 0.5f, "Hz"), lin("Delay Level", 0, 100, 50, "%"), tm("Delay Time", 1, 1500, 380), lin("Feedback", 0, 95, 40, "%"),
          lin("Verb Level", 0, 100, 50, "%"), lg("Decay", 0.3f, 20, 3, "s"), lg("Color", 1000, 20000, 7000, "Hz"), st("Routing", 0, 1, 0, kRoute)},
         mk<SpaceTime>},

        {"Poly Synth", "Synth", "Polyphonic synth: up to six oscillator voices follow the notes of your chords",
         {mix(70), st("Octave", -2, 2, 0, nullptr, "oct"), st("Wave", 0, 2, 0, kWaves), lin("Detune", 0, 50, 10, "ct"), lg("Cutoff", 100, 10000, 1500, "Hz"), lg("Resonance", 0.5f, 10, 1.5f),
          lin("Env Amount", 0, 5, 2, "oct"), lg("Attack", 1, 500, 15, "ms"), lg("Release", 20, 3000, 400, "ms"), st("Voices", 1, 6, 6)},
         mk<PolySynth>},

        {"Glitch", "Granular", "Randomly grabs slices and repeats them pitched, reversed or crushed",
         {mix(100), lin("Chance", 0, 100, 35, "%"), tm("Slice", 10, 500, 125), st("Repeats", 1, 8, 4), st("Pitch Range", 0, 24, 12, nullptr, "st"), lin("Reverse", 0, 100, 30, "%"),
          lin("Crush", 4, 16, 16, "bit")},
         mk<Glitch>},
    };
    for (auto &a : more) r.push_back(std::move(a));
}

}  // namespace hz
