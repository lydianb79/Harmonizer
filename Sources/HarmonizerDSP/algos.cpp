// All effect algorithms and the registry that describes them to the UI.
#include "dsp.hpp"

namespace hz {

// ------------------------------------------------------------ musical helpers

static const char *const kKeys[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
struct Scale { const char *name; int n; int pc[12]; };
static const Scale kScales[] = {
    {"Major", 7, {0, 2, 4, 5, 7, 9, 11}},      {"Minor", 7, {0, 2, 3, 5, 7, 8, 10}},
    {"Dorian", 7, {0, 2, 3, 5, 7, 9, 10}},     {"Phrygian", 7, {0, 1, 3, 5, 7, 8, 10}},
    {"Lydian", 7, {0, 2, 4, 6, 7, 9, 11}},     {"Mixolydian", 7, {0, 2, 4, 5, 7, 9, 10}},
    {"Locrian", 7, {0, 1, 3, 5, 6, 8, 10}},    {"Harm Minor", 7, {0, 2, 3, 5, 7, 8, 11}},
    {"Mel Minor", 7, {0, 2, 3, 5, 7, 9, 11}},  {"Whole Tone", 6, {0, 2, 4, 6, 8, 10}},
    {"Penta Maj", 5, {0, 2, 4, 7, 9}},         {"Penta Min", 5, {0, 3, 5, 7, 10}},
    {"Blues", 6, {0, 3, 5, 6, 7, 10}},         {"Chromatic", 12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
};
static const char *const kScaleNames[] = {"Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian",
                                          "Harm Minor", "Mel Minor", "Whole Tone", "Penta Maj", "Penta Min", "Blues", "Chromatic"};

// Semitone shift that moves `note` by `deg` scale degrees inside key/scale.
static int scaleShift(int note, int key, int scale, int deg) {
    const Scale &s = kScales[std::min(13, std::max(0, scale))];
    int pc = ((note - key) % 12 + 12) % 12, idx = 0;
    for (int i = 0; i < s.n; i++)
        if (s.pc[i] <= pc) idx = i;
    int t = idx + deg;
    int oct = (int)std::floor((float)t / (float)s.n), ti = t - oct * s.n;
    return s.pc[ti] + 12 * oct - s.pc[idx];
}

// Tracks the played note with hysteresis so harmonies do not flutter.
struct NoteTracker {
    PitchTracker pt;
    int note = -1;
    bool changed = true;
    void init(float sr) { pt.init(sr); note = -1; changed = true; }
    void reset() { pt.reset(); note = -1; changed = true; }
    inline void push(float x) {
        pt.push(x);
        if (pt.fresh) {
            pt.fresh = false;
            if (pt.freq > 0) {
                float m = 69.f + 12.f * std::log2(pt.freq / 440.f);
                if (note < 0 || std::fabs(m - (float)note) > 0.62f) { note = (int)std::lround(m); changed = true; }
            }
        }
    }
};

// ------------------------------------------------------------------- PITCH

struct MicroShift : Algo {
    Shifter sa, sb; Delay da, db; OnePole la, lb; Smooth ta, tb; float fa = 0, fb2 = 0, ph = 0;
    void init(float s) override {
        sr = s; sa.init(sr, 0.25f); sb.init(sr, 0.25f); sa.setGrain(50); sb.setGrain(57);
        da.init(sr * 0.25f); db.init(sr * 0.25f); ta.setTime(60, sr); tb.setTime(60, sr);
    }
    void reset() override {
        sa.reset(); sb.reset(); da.clear(); db.clear(); la.reset(); lb.reset(); fa = fb2 = 0;
        ta.snap(p[3] * 0.001f * sr); tb.snap(p[4] * 0.001f * sr);
    }
    void update() override { la.setLP(p[8], sr); lb.setLP(p[8], sr); }
    void process(float *L, float *R, int n) override {
        float ra = semi2ratio(p[1] * 0.01f), rb = semi2ratio(-p[2] * 0.01f), fb = p[5] * 0.01f;
        float md = p[6] * 0.00004f, inc = p[7] / sr, w = p[9] * 0.01f, tA = p[3] * 0.001f * sr, tB = p[4] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]);
            ph += inc; if (ph >= 1) ph -= 1;
            float l = lfoSin(ph);
            sa.ratio = ra * (1 + md * l); sb.ratio = rb * (1 - md * l);
            da.write(sa.tick(m + fb * fa)); db.write(sb.tick(m + fb * fb2));
            float a = la.lp(da.lin(ta.next(tA))), b = lb.lp(db.lin(tb.next(tB)));
            fa = a; fb2 = b;
            float mid = 0.5f * (a + b), side = 0.5f * (a - b) * w;
            L[i] = mid + side; R[i] = mid - side;
        }
    }
};

struct Diatonic : Algo {
    Shifter sa, sb; Delay da, db; NoteTracker nt; Smooth ga, gb, ta, tb; float tgA = 0, tgB = 0, fa = 0, fb2 = 0;
    void init(float s) override {
        sr = s; sa.init(sr, 0.3f); sb.init(sr, 0.3f); sb.setGrain(39); da.init(sr * 1.05f); db.init(sr * 1.05f);
        nt.init(sr); ga.setTime(12, sr); gb.setTime(12, sr); ta.setTime(60, sr); tb.setTime(60, sr);
    }
    void reset() override {
        sa.reset(); sb.reset(); da.clear(); db.clear(); nt.reset(); fa = fb2 = 0; retarget();
        ga.snap(tgA); gb.snap(tgB); ta.snap(p[7] * 0.001f * sr); tb.snap(p[8] * 0.001f * sr);
    }
    void retarget() {
        int note = nt.note < 0 ? (int)p[3] + 60 : nt.note;
        tgA = (float)scaleShift(note, (int)p[3], (int)p[4], (int)p[1]);
        tgB = (float)scaleShift(note, (int)p[3], (int)p[4], (int)p[2]);
    }
    void update() override { retarget(); }
    void process(float *L, float *R, int n) override {
        float la = p[5] * 0.01f, lb = p[6] * 0.01f, fb = p[9] * 0.01f, tA = p[7] * 0.001f * sr, tB = p[8] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]);
            nt.push(m);
            if (nt.changed) { nt.changed = false; retarget(); }
            sa.ratio = semi2ratio(ga.next(tgA)); sb.ratio = semi2ratio(gb.next(tgB));
            da.write(sa.tick(m) + fb * fa); db.write(sb.tick(m) + fb * fb2);
            float a = da.lin(ta.next(tA)), b = db.lin(tb.next(tB));
            fa = a; fb2 = b;
            a *= la; b *= lb;
            L[i] = 0.85f * a + 0.35f * b; R[i] = 0.35f * a + 0.85f * b;
        }
    }
};

struct QuadVoice : Algo {
    Shifter sh[4]; Delay dl[4]; NoteTracker nt; Smooth gl[4], ts; float tg[4] = {0, 0, 0, 0};
    void init(float s) override {
        sr = s;
        for (int v = 0; v < 4; v++) { sh[v].init(sr, 0.3f); sh[v].setGrain(33.f + 3.f * (float)v); dl[v].init(sr * 2.1f); gl[v].setTime(12, sr); }
        nt.init(sr); ts.setTime(60, sr);
    }
    void retarget() {
        static const float sgn[4] = {1, -1, 0.6f, -0.6f};
        int note = nt.note < 0 ? (int)p[5] + 60 : nt.note;
        for (int v = 0; v < 4; v++) tg[v] = (float)scaleShift(note, (int)p[5], (int)p[6], (int)p[1 + v]) + sgn[v] * p[9] * 0.01f;
    }
    void reset() override {
        for (int v = 0; v < 4; v++) { sh[v].reset(); dl[v].clear(); }
        nt.reset(); retarget();
        for (int v = 0; v < 4; v++) gl[v].snap(tg[v]);
        ts.snap(p[7] * 0.001f * sr);
    }
    void update() override { retarget(); }
    void process(float *L, float *R, int n) override {
        static const float pan[4] = {-1, 1, -0.4f, 0.4f};
        float w = p[8] * 0.01f, spread = p[7] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), l = 0, r = 0, sp = ts.next(spread);
            nt.push(m);
            if (nt.changed) { nt.changed = false; retarget(); }
            for (int v = 0; v < 4; v++) {
                sh[v].ratio = semi2ratio(gl[v].next(tg[v]));
                dl[v].write(sh[v].tick(m));
                float o = dl[v].lin(sp * (float)(v + 1)), pn = pan[v] * w;
                l += o * (0.5f - 0.5f * pn); r += o * (0.5f + 0.5f * pn);
            }
            L[i] = l * 0.8f; R[i] = r * 0.8f;
        }
    }
};

template <class S> struct DualShift : Algo {
    S sa, sb; Delay da, db; OnePole la, lb; Smooth ta, tb; float fa = 0, fb2 = 0;
    void init(float s) override {
        sr = s; sa.init(sr, 0.35f); sb.init(sr, 0.35f); sb.setGrain(40); da.init(sr * 2.05f); db.init(sr * 2.05f);
        ta.setTime(60, sr); tb.setTime(60, sr);
    }
    void reset() override {
        sa.reset(); sb.reset(); da.clear(); db.clear(); la.reset(); lb.reset(); fa = fb2 = 0;
        ta.snap(p[5] * 0.001f * sr); tb.snap(p[6] * 0.001f * sr);
    }
    void update() override {
        sa.ratio = semi2ratio(p[1] + p[2] * 0.01f); sb.ratio = semi2ratio(p[3] + p[4] * 0.01f);
        la.setLP(p[9], sr); lb.setLP(p[9], sr);
    }
    void process(float *L, float *R, int n) override {
        float fb = p[7] * 0.01f, bal = p[8] * 0.01f, gA = std::cos(bal * kPi * 0.5f) * 1.2f, gB = std::sin(bal * kPi * 0.5f) * 1.2f;
        float tA = p[5] * 0.001f * sr, tB = p[6] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]);
            da.write(sa.tick(m + softclip(fb * fa))); db.write(sb.tick(m + softclip(fb * fb2)));
            float a = la.lp(da.lin(ta.next(tA))), b = lb.lp(db.lin(tb.next(tB)));
            fa = a; fb2 = b;
            a *= gA; b *= gB;
            L[i] = 0.8f * a + 0.3f * b; R[i] = 0.3f * a + 0.8f * b;
        }
    }
};

struct Octaver : Algo {
    Shifter s1, s2, su; OnePole l1, l2, lu;
    void init(float s) override {
        sr = s; s1.init(sr, 0.3f); s2.init(sr, 0.3f); su.init(sr, 0.3f);
        s1.ratio = 0.5f; s2.ratio = 0.25f; su.ratio = 2; s1.setGrain(42); s2.setGrain(50); su.setGrain(30);
    }
    void reset() override { s1.reset(); s2.reset(); su.reset(); l1.reset(); l2.reset(); lu.reset(); }
    void update() override { l1.setLP(p[4], sr); l2.setLP(p[4] * 0.7f, sr); lu.setLP(p[5], sr); }
    void process(float *L, float *R, int n) override {
        float g1 = p[1] * 0.012f, g2 = p[2] * 0.012f, gu = p[3] * 0.01f, grit = 1.f + p[6] * 0.08f;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]);
            float sub = g1 * l1.lp(s1.tick(m)) + g2 * l2.lp(s2.tick(m));
            sub = softclip(sub * grit) / std::sqrt(grit);
            float o = sub + gu * lu.lp(su.tick(m));
            L[i] = o; R[i] = o;
        }
    }
};

struct Crystals : Algo {
    Shifter sa, sb; Delay da, db; Verb v; Smooth ta, tb; float fa = 0, fb2 = 0;
    void init(float s) override {
        sr = s; sa.init(sr, 4.1f); sb.init(sr, 4.1f); sa.align = sb.align = false;
        da.init(sr * 1.6f); db.init(sr * 1.6f); v.init(sr, 2.2f); ta.setTime(80, sr); tb.setTime(80, sr);
    }
    void reset() override {
        sa.reset(); sb.reset(); da.clear(); db.clear(); v.reset(); fa = fb2 = 0;
        ta.snap(p[5] * 0.001f * sr); tb.snap(p[6] * 0.001f * sr);
    }
    void update() override {
        float ra = semi2ratio(p[1]), rb = semi2ratio(p[2]);
        sa.ratio = -ra; sb.ratio = -rb;
        sa.setGrain(std::min(p[3], 3900.f / (1.f + ra))); sb.setGrain(std::min(p[4], 3900.f / (1.f + rb)));
        v.set(1.6f, 0.6f + p[9] * 0.08f, 7000, 120, 0.65f, 8, 0.4f);
    }
    void process(float *L, float *R, int n) override {
        float fb = p[7] * 0.0095f, vm = p[8] * 0.012f, tA = p[5] * 0.001f * sr, tB = p[6] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]);
            da.write(sa.tick(m + softclip(fb * fa))); db.write(sb.tick(m + softclip(fb * fb2)));
            float a = da.lin(ta.next(tA)), b = db.lin(tb.next(tB)), vl, vr;
            fa = a; fb2 = b;
            v.tick(a, b, vl, vr);
            L[i] = a + vm * vl; R[i] = b + vm * vr;
        }
    }
};

template <class S> struct Flex : Algo {
    S sh; Smooth gl;
    void init(float s) override { sr = s; sh.init(sr, 0.35f); }
    void reset() override { sh.reset(); gl.snap(lerpf(p[2], p[3], p[1] * 0.01f)); }
    void update() override { gl.setTime(p[4], sr); }
    void process(float *L, float *R, int n) override {
        float tg = lerpf(p[2], p[3], p[1] * 0.01f);
        for (int i = 0; i < n; i++) {
            sh.ratio = semi2ratio(gl.next(tg));
            float o = sh.tick(0.5f * (L[i] + R[i]));
            L[i] = o; R[i] = o;
        }
    }
};

static const char *const kArpNames[] = {"Octaves", "Major", "Minor", "Fifths", "Penta", "Falling", "Glitch"};
static const float kArp[7][8] = {
    {0, 12, 0, 12, 24, 12, 0, 12}, {0, 4, 7, 12, 16, 12, 7, 4},   {0, 3, 7, 12, 15, 12, 7, 3},   {0, 7, 12, 19, 12, 7, 0, -12},
    {0, 3, 5, 7, 10, 12, 10, 7},   {12, 7, 4, 0, -5, -8, -12, 0}, {12, -12, 7, -5, 24, 0, -7, 19},
};
struct ArpShift : Algo {
    Shifter sh; Smooth gl; float pos = 0; int step = 0;
    void init(float s) override { sr = s; sh.init(sr, 0.35f); sh.setGrain(28); }
    void reset() override { sh.reset(); pos = 0; step = 0; gl.snap(0); }
    void update() override { gl.setTime(std::max(1.f, p[4]), sr); }
    void process(float *L, float *R, int n) override {
        float len = std::max(64.f, p[2] * 0.001f * sr), chop = p[5] * 0.01f, atk = 0.004f * sr;
        int pat = (int)clampf(p[1], 0, 6), steps = (int)clampf(p[3], 2, 8);
        for (int i = 0; i < n; i++) {
            if (++pos >= len) { pos = 0; step = (step + 1) % steps; }
            sh.ratio = semi2ratio(gl.next(kArp[pat][step % 8]));
            float t = clampf((pos / len - 0.55f) / 0.45f, 0, 1), fall = t * t * (3 - 2 * t);
            float g = 1.f - chop * std::max(fall, 1.f - std::min(1.f, pos / atk));
            float o = g * sh.tick(0.5f * (L[i] + R[i]));
            L[i] = o; R[i] = o;
        }
    }
};

// ------------------------------------------------------------------- DELAY

struct DigitalDelay : Algo {
    Delay da, db; OnePole hca, hcb, lca, lcb; Smooth ta, tb; float fa = 0, fb2 = 0, ph = 0, env = 0;
    void init(float s) override { sr = s; da.init(sr * 2.2f); db.init(sr * 2.2f); ta.setTime(90, sr); tb.setTime(90, sr); }
    void reset() override {
        da.clear(); db.clear(); hca.reset(); hcb.reset(); lca.reset(); lcb.reset(); fa = fb2 = env = 0;
        ta.snap(p[1] * 0.001f * sr); tb.snap(p[2] * 0.001f * sr);
    }
    void update() override { hca.setLP(p[5], sr); hcb.setLP(p[5], sr); lca.setLP(p[6], sr); lcb.setLP(p[6], sr); }
    void process(float *L, float *R, int n) override {
        float fb = p[3] * 0.01f, x = p[4] * 0.01f, md = p[7] * 0.01f * 0.003f * sr, inc = p[8] / sr, duck = p[9] * 0.01f;
        float tA = p[1] * 0.001f * sr, tB = p[2] * 0.001f * sr, rel = 1.f - std::exp(-1.f / (0.25f * sr));
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]);
            ph += inc; if (ph >= 1) ph -= 1;
            float a = da.herm(ta.next(tA) + md * (1 + lfoSin(ph))), b = db.herm(tb.next(tB) + md * (1 + lfoSin(wrap1(ph + 0.25f))));
            a = hca.lp(a); a -= lca.lp(a); b = hcb.lp(b); b -= lcb.lp(b);
            da.write(m + softclip(fb * ((1 - x) * a + x * b))); db.write((1 - x) * m + softclip(fb * ((1 - x) * b + x * a)));
            float am = std::fabs(m);
            env = am > env ? am : env + rel * (am - env);
            float g = 1.f - duck * std::min(1.f, env * 5.f);
            L[i] = a * g; R[i] = b * g;
        }
    }
};

struct TapeEcho : Algo {
    Delay d; OnePole hc, hc2, lc, nz; Smooth t; Rng rng; float fb1 = 0, wph = 0, fph = 0;
    void init(float s) override { sr = s; d.init(sr * 1.9f); t.setTime(180, sr); nz.setLP(3, sr); }
    void reset() override { d.clear(); hc.reset(); hc2.reset(); lc.reset(); fb1 = 0; t.snap(p[1] * 0.001f * sr); }
    void update() override { hc.setLP(p[6], sr); hc2.setLP(p[6], sr); lc.setLP(p[7], sr); }
    void process(float *L, float *R, int n) override {
        float fb = p[2] * 0.01f, drive = 1.f + p[3] * 0.05f, wow = p[4] * 0.01f * 0.0022f * sr, flut = p[5] * 0.01f * 0.00035f * sr;
        float tt = p[1] * 0.001f * sr, sp = 1.f - p[8] * 0.0012f;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]);
            wph += 0.45f / sr; if (wph >= 1) wph -= 1;
            fph += 6.8f / sr; if (fph >= 1) fph -= 1;
            float dt = t.next(tt) + wow * (1 + lfoSin(wph) + 6.f * nz.lp(rng.next())) + flut * (1 + lfoSin(fph));
            float y = softclip(d.herm(dt) * drive) / drive;
            y = hc.lp(y); y -= lc.lp(y);
            d.write(m + fb * y);
            L[i] = y; R[i] = hc2.lp(softclip(d.herm(dt * sp) * drive) / drive);
        }
    }
};

struct ReverseDelay : Algo {
    Shifter sh; OnePole hc; float fbv = 0;
    void init(float s) override { sr = s; sh.init(sr, 4.3f); sh.align = false; sh.ratio = -1; }
    void reset() override { sh.reset(); hc.reset(); fbv = 0; }
    void update() override { sh.setGrain(p[1]); hc.setLP(p[3], sr); }
    void process(float *L, float *R, int n) override {
        float fb = p[2] * 0.01f, w = p[4] * 0.01f;
        for (int i = 0; i < n; i++) {
            float a, b;
            sh.tick2(0.5f * (L[i] + R[i]) + fb * fbv, a, b);
            fbv = hc.lp(a + b);
            float mid = 0.5f * (a + b), side = 0.5f * (a - b) * w;
            L[i] = (mid + side) * 1.4f; R[i] = (mid - side) * 1.4f;
        }
    }
};

struct MultiTap : Algo {
    Delay d, pre; Allpass apL[2], apR[2]; OnePole tl, tr; int nt = 1, td[32]; float gl[32], gr[32], fbv = 0;
    void init(float s) override {
        sr = s; d.init(sr * 4.1f); pre.init(sr * 0.55f);
        apL[0].init((int)(0.0113f * sr)); apL[1].init((int)(0.0171f * sr)); apR[0].init((int)(0.0127f * sr)); apR[1].init((int)(0.0193f * sr));
    }
    void reset() override {
        d.clear(); pre.clear(); tl.reset(); tr.reset(); fbv = 0;
        for (int i = 0; i < 2; i++) { apL[i].d.clear(); apR[i].d.clear(); }
    }
    void update() override {
        nt = (int)clampf(p[2], 1, 32);
        float len = p[1] * 0.001f * sr, k = std::exp2(p[3] * 0.02f), tap = p[4] * 0.01f, w = p[6] * 0.01f, norm = 1.6f / std::sqrt((float)nt + 1.f);
        for (int i = 0; i < nt; i++) {
            float x = (float)(i + 1) / (float)nt;
            td[i] = (int)clampf(std::pow(x, k) * len, 1, d.maxDelay());
            float g = tap >= 0 ? 1.f - tap * (x - 1.f / (float)nt) : 1.f + tap * (1.f - x);
            float pn = (i & 1) ? w : -w;
            gl[i] = g * norm * (0.5f - 0.5f * pn) * 1.4f; gr[i] = g * norm * (0.5f + 0.5f * pn) * 1.4f;
        }
        float tone = p[5] * 0.01f;
        float fc = tone < 0 ? 18000.f * std::pow(0.045f, -tone) : 20.f * std::pow(60.f, tone);
        tl.setLP(fc, sr); tr.setLP(fc, sr);
    }
    void process(float *L, float *R, int n) override {
        float fb = p[8] * 0.01f, slurm = p[7] * 0.0072f, pd = p[9] * 0.001f * sr;
        bool hp = p[5] > 0;
        for (int i = 0; i < n; i++) {
            pre.write(0.5f * (L[i] + R[i]));
            d.write(pre.lin(pd) + fb * fbv);
            float l = 0, r = 0;
            for (int t = 0; t < nt; t++) { float s = d.at(td[t]); l += s * gl[t]; r += s * gr[t]; }
            fbv = d.at(td[nt - 1]);
            l = apL[1].tick(apL[0].tick(l, slurm), slurm); r = apR[1].tick(apR[0].tick(r, slurm), slurm);
            L[i] = hp ? tl.hp(l) : tl.lp(l); R[i] = hp ? tr.hp(r) : tr.lp(r);
        }
    }
};

// -------------------------------------------------------------- MODULATION

struct Chorus : Algo {
    Delay d; OnePole tl, tr; float ph = 0, fbv = 0;
    void init(float s) override { sr = s; d.init(sr * 0.08f); }
    void reset() override { d.clear(); tl.reset(); tr.reset(); fbv = 0; }
    void update() override { tl.setLP(p[7], sr); tr.setLP(p[7], sr); }
    void process(float *L, float *R, int n) override {
        float inc = p[1] / sr, dep = p[2] * 0.01f * 0.004f * sr, base = p[3] * 0.001f * sr, w = p[5] * 0.01f, fb = p[6] * 0.01f;
        int nv = (int)clampf(p[4], 1, 3);
        static const float pan[3][3] = {{0, 0, 0}, {-1, 1, 0}, {-1, 0, 1}};
        for (int i = 0; i < n; i++) {
            d.write(0.5f * (L[i] + R[i]) + fb * fbv);
            ph += inc; if (ph >= 1) ph -= 1;
            float l = 0, r = 0;
            for (int v = 0; v < nv; v++) {
                float o = d.herm(std::max(1.5f, base + dep * lfoSin(wrap1(ph + (float)v / 3.f)))), pn = pan[nv - 1][v] * w;
                l += o * (0.5f - 0.5f * pn); r += o * (0.5f + 0.5f * pn);
                if (v == 0) fbv = o;
            }
            float g = nv == 1 ? 2.f : 1.3f;
            L[i] = tl.lp(l * g); R[i] = tr.lp(r * g);
        }
    }
};

struct Flanger : Algo {
    Delay dl, dr; float ph = 0, fl = 0, fr = 0;
    void init(float s) override { sr = s; dl.init(sr * 0.05f); dr.init(sr * 0.05f); }
    void reset() override { dl.clear(); dr.clear(); fl = fr = 0; }
    void process(float *L, float *R, int n) override {
        float inc = p[1] / sr, dep = p[2] * 0.01f * 0.006f * sr, man = p[3] * 0.001f * sr, fb = p[4] * 0.01f, st = p[5] / 360.f;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]);
            ph += inc; if (ph >= 1) ph -= 1;
            dl.write(m + softclip(fb * fl)); dr.write(m + softclip(fb * fr));
            fl = dl.herm(1.5f + man + dep * (0.5f + 0.5f * lfoSin(ph)));
            fr = dr.herm(1.5f + man + dep * (0.5f + 0.5f * lfoSin(wrap1(ph + st))));
            L[i] = fl; R[i] = fr;
        }
    }
};

static const char *const kStageNames[] = {"4", "6", "8", "12"};
struct Phaser : Algo {
    float sl[12] = {0}, sR[12] = {0}, ph = 0, fl = 0, fr = 0;
    void init(float s) override { sr = s; }
    void reset() override { std::memset(sl, 0, sizeof sl); std::memset(sR, 0, sizeof sR); fl = fr = 0; }
    void process(float *L, float *R, int n) override {
        static const int stg[4] = {4, 6, 8, 12};
        int ns = stg[(int)clampf(p[5], 0, 3)];
        float inc = p[1] / sr, dep = p[2] * 0.02f, fb = p[4] * 0.01f, st = p[6] / 360.f, wc = kPi * p[3] / sr;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]);
            ph += inc; if (ph >= 1) ph -= 1;
            float wl = std::min(1.2f, wc * std::exp2(dep * lfoSin(ph))), wr = std::min(1.2f, wc * std::exp2(dep * lfoSin(wrap1(ph + st))));
            float al = (wl - 1) / (wl + 1), ar = (wr - 1) / (wr + 1), xl = m + fb * fl, xr = m + fb * fr;
            for (int k = 0; k < ns; k++) {
                float y = al * xl + sl[k]; sl[k] = xl - al * y; xl = y;
                y = ar * xr + sR[k]; sR[k] = xr - ar * y; xr = y;
            }
            fl = softclip(xl); fr = softclip(xr);
            L[i] = xl; R[i] = xr;
        }
    }
};

static const char *const kRotNames[] = {"Slow", "Fast", "Brake"};
struct Rotary : Algo {
    Delay dh, dd; OnePole xo; float hr = 0.8f, dr = 0.7f, hp = 0, dp = 0;
    void init(float s) override { sr = s; dh.init(sr * 0.02f); dd.init(sr * 0.02f); xo.setLP(800, sr); }
    void reset() override { dh.clear(); dd.clear(); xo.reset(); }
    void process(float *L, float *R, int n) override {
        int mode = (int)clampf(p[1], 0, 2);
        float ht = mode == 0 ? 0.8f : mode == 1 ? 6.7f : 0.f, dt = mode == 0 ? 0.7f : mode == 1 ? 5.8f : 0.f;
        float ac = 1.f - std::exp(-1.f / (p[4] * sr)), ac2 = 1.f - std::exp(-1.f / (p[4] * 4.f * sr));
        float bal = p[2] * 0.01f, drive = 1.f + p[3] * 0.06f, dep = p[5] * 0.01f, w = p[6] * 0.0025f;
        float hd = dep * 0.0007f * sr, ddp = dep * 0.0003f * sr;
        for (int i = 0; i < n; i++) {
            float m = softclip(0.5f * (L[i] + R[i]) * drive) / std::sqrt(drive);
            hr += ac * (ht - hr); dr += ac2 * (dt - dr);
            hp = wrap1(hp + hr / sr); dp = wrap1(dp + dr / sr);
            float lo = xo.lp(m), hi = m - lo;
            dh.write(hi); dd.write(lo);
            float h1 = lfoSin(hp), h2 = lfoSin(wrap1(hp + w)), d1 = lfoSin(dp), d2 = lfoSin(wrap1(dp + w));
            float hl = dh.herm(2 + hd * (1 + h1)) * (1 - 0.45f * dep * (0.5f + 0.5f * lfoSin(wrap1(hp + 0.25f))));
            float hrr = dh.herm(2 + hd * (1 + h2)) * (1 - 0.45f * dep * (0.5f + 0.5f * lfoSin(wrap1(hp + 0.25f + w))));
            float dll = dd.herm(2 + ddp * (1 + d1)) * (1 - 0.3f * dep * (0.5f + 0.5f * d1));
            float drr = dd.herm(2 + ddp * (1 + d2)) * (1 - 0.3f * dep * (0.5f + 0.5f * d2));
            float gh = std::min(1.f, 2 * bal) * 1.3f, gd = std::min(1.f, 2 - 2 * bal) * 1.3f;
            L[i] = gh * hl + gd * dll; R[i] = gh * hrr + gd * drr;
        }
    }
};

static const char *const kShapeNames[] = {"Sine", "Triangle", "Square", "Ramp"};
struct Tremolo : Algo {
    float ph = 0; OnePole sl, sR;
    void init(float s) override { sr = s; sl.setLP(120, sr); sR.setLP(120, sr); }
    void reset() override { sl.reset(); sR.reset(); }
    static float shape(int s, float ph) {
        switch (s) {
        case 1: return ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph;
        case 2: return ph < 0.5f ? 1.f : -1.f;
        case 3: return 1 - 2 * ph;
        default: return lfoSin(ph);
        }
    }
    void process(float *L, float *R, int n) override {
        float inc = p[1] / sr, dep = p[2] * 0.01f, st = p[4] / 360.f;
        int s = (int)clampf(p[3], 0, 3);
        for (int i = 0; i < n; i++) {
            ph += inc; if (ph >= 1) ph -= 1;
            float gl = sl.lp(1 - dep * (0.5f - 0.5f * shape(s, ph))), gr = sR.lp(1 - dep * (0.5f - 0.5f * shape(s, wrap1(ph + st))));
            L[i] *= gl * 1.2f; R[i] *= gr * 1.2f;
        }
    }
};

struct RingMod : Algo {
    float ph = 0, lph = 0; OnePole tl, tr;
    void init(float s) override { sr = s; }
    void reset() override { tl.reset(); tr.reset(); }
    void update() override { tl.setLP(p[5], sr); tr.setLP(p[5], sr); }
    void process(float *L, float *R, int n) override {
        float linc = p[3] / sr, ld = p[4] * 0.01f;
        bool tri = p[2] > 0.5f;
        for (int i = 0; i < n; i++) {
            lph += linc; if (lph >= 1) lph -= 1;
            ph = wrap1(ph + p[1] * std::exp2(ld * lfoSin(lph)) / sr);
            float c = tri ? (ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph) : lfoSin(ph);
            L[i] = tl.lp(L[i] * c) * 1.4f; R[i] = tr.lp(R[i] * c) * 1.4f;
        }
    }
};

static const char *const kFilterNames[] = {"Low Pass", "Band Pass", "High Pass"};
struct AutoWah : Algo {
    float ic1 = 0, ic2 = 0, env = 0, ph = 0;
    void init(float s) override { sr = s; }
    void reset() override { ic1 = ic2 = env = 0; }
    void process(float *L, float *R, int n) override {
        float sens = p[1] * 0.08f, base = p[2], range = p[3], k = 1.f / p[4], inc = p[5] / sr, ld = p[6] * 0.01f;
        float atk = 1.f - std::exp(-1.f / (p[7] * 0.001f * sr)), rel = 1.f - std::exp(-1.f / (p[8] * 0.001f * sr));
        int type = (int)clampf(p[9], 0, 2);
        for (int i = 0; i < n; i++) {
            float x = 0.5f * (L[i] + R[i]), am = std::fabs(x);
            env += (am - env) * (am > env ? atk : rel);
            ph += inc; if (ph >= 1) ph -= 1;
            float oct = range * (std::min(1.f, env * sens) + ld * 0.5f * (1 + lfoSin(ph)));
            float f = clampf(base * std::exp2(oct), 20.f, sr * 0.42f), g = std::tan(kPi * f / sr);
            float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
            float o = type == 0 ? v2 : type == 1 ? v1 * (1.f + k) : x - k * v1 - v2;
            L[i] = o; R[i] = o;
        }
    }
};

// ------------------------------------------------------------------ REVERB

struct ReverbAlgo : Algo {
    Verb v; Delay pre; float scale; Smooth pt;
    explicit ReverbAlgo(float sc) : scale(sc) {}
    void init(float s) override { sr = s; v.init(sr, scale * 1.35f + 0.05f); pre.init(sr * 0.3f); pt.setTime(80, sr); }
    void reset() override { v.reset(); pre.clear(); pt.snap(p[3] * 0.001f * sr); }
    void update() override { v.set(scale * (0.35f + p[2] * 0.01f), p[1], p[4], p[5], p[6] * 0.0075f, p[7] * 0.2f, p[8]); }
    void process(float *L, float *R, int n) override {
        float pd = p[3] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            pre.write(0.5f * (L[i] + R[i]));
            float x = pre.lin(pt.next(pd));
            v.tick(x, x, L[i], R[i]);
        }
    }
};

struct VoidVerb : Algo {
    Verb v; Delay pre; Smooth pt; float fbv = 0;
    void init(float s) override { sr = s; v.init(sr, 7.1f); pre.init(sr * 2.1f); pt.setTime(120, sr); }
    void reset() override { v.reset(); pre.clear(); fbv = 0; pt.snap(p[3] * 0.001f * sr); }
    void update() override {
        v.frozen = p[9] > 0.5f;
        v.set(1.f + p[2] * 0.06f, p[1] >= 59.5f ? 100000.f : p[1], p[5], p[4], 0.72f, p[6] * 0.24f, p[7]);
    }
    void process(float *L, float *R, int n) override {
        float pd = p[3] * 0.001f * sr, fb = p[8] * 0.009f;
        for (int i = 0; i < n; i++) {
            pre.write(0.5f * (L[i] + R[i]) + fb * fbv);
            float x = pre.lin(pt.next(pd)) * 0.7f;
            v.tick(x, x, L[i], R[i]);
            fbv = softclip(0.5f * (L[i] + R[i]));
        }
    }
};

struct Shimmer : Algo {
    Verb v; PolyShifter sh; Delay pre; OnePole lp; float fbs = 0;
    void init(float s) override { sr = s; v.init(sr, 2.6f); sh.init(sr); pre.init(sr * 0.3f); lp.setLP(6500, sr); }
    void reset() override { v.reset(); sh.reset(); pre.clear(); lp.reset(); fbs = 0; }
    void update() override {
        v.set(0.6f + p[2] * 0.019f, p[1], p[5], p[6], 0.7f, p[7] * 0.14f, 0.45f);
        sh.ratio = semi2ratio(p[3]);
    }
    void process(float *L, float *R, int n) override {
        float amt = p[4] * 0.0085f, pd = p[8] * 0.001f * sr;
        for (int i = 0; i < n; i++) {
            pre.write(0.5f * (L[i] + R[i]));
            float x = pre.lin(pd) + fbs;
            v.tick(x, x, L[i], R[i]);
            fbs = softclip(lp.lp(sh.tick(0.5f * (L[i] + R[i]))) * amt);
        }
    }
};

// -------------------------------------------------------------------- TONE

struct EQComp : Algo {
    Biquad eq[2][3]; float gr = 0;
    void init(float s) override { sr = s; }
    void reset() override { for (auto &c : eq) for (auto &b : c) b.reset(); gr = 0; }
    void update() override {
        for (int c = 0; c < 2; c++) {
            eq[c][0].set(BQ_LOSHELF, 120, 0.7f, p[6], sr);
            eq[c][1].set(BQ_PEAK, p[8], 0.9f, p[7], sr);
            eq[c][2].set(BQ_HISHELF, 6000, 0.7f, p[9], sr);
        }
    }
    void process(float *L, float *R, int n) override {
        float thr = p[1], slope = 1.f / p[2] - 1.f, mk = p[5];
        float atk = 1.f - std::exp(-1.f / (p[3] * 0.001f * sr)), rel = 1.f - std::exp(-1.f / (p[4] * 0.001f * sr));
        for (int i = 0; i < n; i++) {
            float lv = 8.6858896f * std::log(std::max(std::fabs(L[i]), std::fabs(R[i])) + 1e-9f), over = lv - thr;
            float tg = over < -3 ? 0 : over < 3 ? slope * (over + 3) * (over + 3) / 12.f : slope * over;
            gr += (tg - gr) * (tg < gr ? atk : rel);
            float g = db2lin(gr + mk);
            L[i] = eq[0][2].tick(eq[0][1].tick(eq[0][0].tick(L[i] * g)));
            R[i] = eq[1][2].tick(eq[1][1].tick(eq[1][0].tick(R[i] * g)));
        }
    }
};

static const char *const kCrushNames[] = {"Overdrive", "Distortion", "Fuzz"};
struct Crush : Algo {
    OnePole pre, dc, post; Biquad bq[3]; float xh[4] = {0}, uh[7] = {0}, env = 0;
    void init(float s) override { sr = s; dc.setLP(25, sr); }
    void reset() override {
        pre.reset(); dc.reset(); post.reset(); for (auto &b : bq) b.reset();
        std::memset(xh, 0, sizeof xh); std::memset(uh, 0, sizeof uh); env = 0;
    }
    void update() override {
        int type = (int)clampf(p[2], 0, 2);
        pre.setLP(type == 0 ? 320.f : 70.f, sr); post.setLP(type == 2 ? 5200.f : 7800.f, sr);
        bq[0].set(BQ_LOSHELF, 120, 0.7f, p[3], sr); bq[1].set(BQ_PEAK, p[6], 0.8f, p[4], sr); bq[2].set(BQ_HISHELF, 3200, 0.7f, p[5], sr);
    }
    static inline float shape(float x, int type) {
        switch (type) {
        case 0: return softclip(x + 0.12f) - 0.1194f;
        case 1: return clampf(softclip(x * 1.6f) * 1.25f, -0.92f, 0.92f);
        default: return x >= 0 ? 1.f - std::exp(-2.2f * x) : -0.85f * (1.f - std::exp(1.3f * x));
        }
    }
    void process(float *L, float *R, int n) override {
        int type = (int)clampf(p[2], 0, 2);
        float drive = db2lin(p[1] * 0.52f), sag = p[7] * 0.06f, oct = p[8] * 0.012f, lvl = db2lin(p[9]) * 0.55f;
        float rel = 1.f - std::exp(-1.f / (0.12f * sr));
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), am = std::fabs(m);
            env = am > env ? env + 0.3f * (am - env) : env + rel * (am - env);
            float h = pre.hp(m);
            h += oct * (std::fabs(h) - h * 0.5f);
            float x = h * drive / (1.f + sag * env);
            // 2x oversampled waveshaper: cubic half-band up, shape, half-band down
            xh[3] = xh[2]; xh[2] = xh[1]; xh[1] = xh[0]; xh[0] = x;
            float e0 = shape(xh[2], type), e1 = shape((-xh[3] + 9 * xh[2] + 9 * xh[1] - xh[0]) * 0.0625f, type);
            for (int k = 6; k >= 2; k--) uh[k] = uh[k - 2];
            uh[1] = e0; uh[0] = e1;
            float y = (-uh[0] + 9 * uh[2] + 16 * uh[3] + 9 * uh[4] - uh[6]) * 0.03125f;
            y = dc.hp(y);
            y = post.lp(bq[2].tick(bq[1].tick(bq[0].tick(y)))) * lvl;
            L[i] = y; R[i] = y;
        }
    }
};

struct Resonator : Algo {
    Delay d[4]; OnePole lp[4]; float len[4] = {100, 100, 100, 100};
    void init(float s) override { sr = s; for (auto &x : d) x.init(sr / 38.f + 16); }
    void reset() override { for (int i = 0; i < 4; i++) { d[i].clear(); lp[i].reset(); } }
    void update() override {
        for (int i = 0; i < 4; i++) {
            len[i] = sr / (440.f * std::exp2((p[1 + i] - 69.f) / 12.f)) - 1.f;
            lp[i].setLP(p[6], sr);
        }
    }
    void process(float *L, float *R, int n) override {
        float fb = 0.8f + p[5] * 0.00197f, w = p[7] * 0.01f;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), l = 0, r = 0;
            for (int k = 0; k < 4; k++) {
                float o = lp[k].lp(d[k].herm(len[k]));
                d[k].write(m + softclip(fb * o));
                float pn = (k & 1) ? w : -w;
                l += o * (0.5f - 0.5f * pn); r += o * (0.5f + 0.5f * pn);
            }
            float g = 0.5f * (1.02f - fb) * 6.f + 0.08f;
            L[i] = l * g; R[i] = r * g;
        }
    }
};

// ------------------------------------------------- SYNTH / GRANULAR / LOOPER

struct UniVibe : Algo {
    float sl[4] = {0}, sR[4] = {0}, ph = 0, fl = 0, fr = 0;
    void init(float s) override { sr = s; }
    void reset() override { std::memset(sl, 0, sizeof sl); std::memset(sR, 0, sizeof sR); fl = fr = 0; }
    void process(float *L, float *R, int n) override {
        static const float base[4] = {90, 260, 900, 3300};
        float inc = p[1] / sr, dep = p[2] * 0.022f, fb = p[3] * 0.01f, st = p[4] / 360.f;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]);
            ph += inc; if (ph >= 1) ph -= 1;
            // lamp-like LFO: slow rise, quicker fall
            float a = 0.5f + 0.5f * lfoSin(ph), b = 0.5f + 0.5f * lfoSin(wrap1(ph + st));
            float ml = std::exp2(dep * (a * a * 2 - 1)), mr = std::exp2(dep * (b * b * 2 - 1));
            float xl = m + fb * fl, xr = m + fb * fr;
            for (int k = 0; k < 4; k++) {
                float wl = std::min(1.2f, kPi * base[k] * ml / sr), wr = std::min(1.2f, kPi * base[k] * mr / sr);
                float al = (wl - 1) / (wl + 1), ar = (wr - 1) / (wr + 1);
                float y = al * xl + sl[k]; sl[k] = xl - al * y; xl = y;
                y = ar * xr + sR[k]; sR[k] = xr - ar * y; xr = y;
            }
            fl = softclip(xl); fr = softclip(xr);
            L[i] = xl; R[i] = xr;
        }
    }
};

struct SawSynth : Algo {
    PitchTracker pt; Smooth gl; float ph[3] = {0, 0.33f, 0.66f}, sph = 0, f = 110, env = 0, ic1 = 0, ic2 = 0;
    void init(float s) override { sr = s; pt.init(sr); }
    void reset() override { pt.reset(); env = ic1 = ic2 = 0; gl.snap(f); }
    void update() override { gl.setTime(p[7], sr); }
    static inline float blep(float t, float dt) {
        if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
        if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
        return 0.f;
    }
    void process(float *L, float *R, int n) override {
        float mult = std::exp2(p[1]), det[3] = {1.f, semi2ratio(p[2] * 0.01f), semi2ratio(-p[2] * 0.01f)}, sub = p[3] * 0.01f, k = 1.f / p[5];
        float atk = 1.f - std::exp(-1.f / (0.003f * sr)), rel = 1.f - std::exp(-1.f / (p[8] * 0.001f * sr));
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (L[i] + R[i]), am = std::fabs(m);
            pt.push(m);
            if (pt.fresh) { pt.fresh = false; if (pt.freq > 0) f = pt.freq; }
            env += (am - env) * (am > env ? atk : rel);
            float fr = clampf(gl.next(f) * mult, 20.f, sr * 0.2f), o = 0;
            for (int v = 0; v < 3; v++) {
                float dt = fr * det[v] / sr;
                ph[v] += dt; if (ph[v] >= 1) ph[v] -= 1;
                o += 2.f * ph[v] - 1.f - blep(ph[v], dt);
            }
            sph = wrap1(sph + 0.5f * fr / sr);
            o = o * 0.33f + sub * (sph < 0.5f ? 0.5f : -0.5f);
            float fc = clampf(p[4] * std::exp2(p[6] * std::min(1.f, env * 6.f)), 20.f, sr * 0.4f), g = std::tan(kPi * fc / sr);
            float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2, v3 = o - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
            float y = softclip(v2) * std::min(1.f, env * 8.f) * 0.9f;
            L[i] = y; R[i] = y;
        }
    }
};

struct GrainCloud : Algo {
    struct G { bool on = false; float d = 0, slope = 0, ph = 0, inc = 0, gl = 0, gr = 0; };
    Delay dl; G g[24]; Rng rng; float timer = 0, fbv = 0;
    void init(float s) override { sr = s; dl.init(sr * 4.3f); }
    void reset() override { dl.clear(); for (auto &x : g) x.on = false; timer = 0; fbv = 0; }
    void process(float *L, float *R, int n) override {
        float size = p[1] * 0.001f * sr, interval = sr / p[2], posn = p[3] * 0.001f * sr, spray = p[4] * 0.01f * sr;
        float rev = p[7] * 0.01f, fb = p[8] * 0.01f, w = p[9] * 0.01f, norm = 1.f / std::sqrt(std::max(1.f, p[2] * p[1] * 0.0005f));
        for (int i = 0; i < n; i++) {
            dl.write(0.5f * (L[i] + R[i]) + softclip(fb * fbv));
            if (--timer <= 0) {
                timer += interval * (1.f + 0.3f * rng.next());
                for (auto &x : g) {
                    if (x.on) continue;
                    float rate = semi2ratio(p[5] + p[6] * 0.01f * rng.next());
                    if (0.5f + 0.5f * rng.next() < rev) rate = -rate;
                    x.slope = 1.f - rate;
                    float d0 = 2.f + posn + spray * (0.5f + 0.5f * rng.next());
                    if (x.slope < 0) d0 -= x.slope * size;
                    x.d = std::min(d0, dl.maxDelay() - std::max(0.f, x.slope) * size - 4.f);
                    x.ph = 0; x.inc = 1.f / size;
                    float pn = rng.next() * w;
                    x.gl = 0.5f - 0.5f * pn; x.gr = 0.5f + 0.5f * pn; x.on = true;
                    break;
                }
            }
            float l = 0, r = 0;
            for (auto &x : g) {
                if (!x.on) continue;
                x.ph += x.inc;
                if (x.ph >= 1) { x.on = false; continue; }
                x.d += x.slope;
                float t = x.ph < 0.5f ? 2 * x.ph : 2 - 2 * x.ph, s = t * t * (3 - 2 * t) * dl.herm(x.d);
                l += s * x.gl; r += s * x.gr;
            }
            l *= norm * 1.6f; r *= norm * 1.6f;
            fbv = 0.5f * (l + r);
            L[i] = l; R[i] = r;
        }
    }
};

struct Stutter : Algo {
    std::vector<float> cap; int pos = 0, slice = 0, len = 6000;
    bool spills() const override { return false; }
    void init(float s) override { sr = s; cap.assign((size_t)(sr * 0.55f) + 8, 0.f); }
    void reset() override { std::fill(cap.begin(), cap.end(), 0.f); pos = 0; slice = 0; }
    void process(float *L, float *R, int n) override {
        len = (int)clampf(p[1] * 0.001f * sr, 64.f, (float)cap.size() - 4.f);
        if (pos >= len) pos = 0;
        int cyc = (int)clampf(p[3], 2, 16), rep = std::min((int)clampf(p[2], 1, 16), cyc - 1), fadeN = std::max(8, std::min(len / 4, (int)(0.003f * sr)));
        bool rev = p[6] > 0.5f;
        float dec = 1.f - p[5] * 0.01f;
        for (int i = 0; i < n; i++) {
            float e = 1;
            if (pos < fadeN && slice <= rep) e = (float)pos / (float)fadeN;
            else if (pos >= len - fadeN && (slice < rep || slice == cyc - 1)) e = (float)(len - pos) / (float)fadeN;
            if (slice < rep) {
                float idx = std::fmod((float)pos * semi2ratio(p[4] * (float)(slice + 1)), (float)len);
                if (rev) idx = (float)(len - 1) - idx;
                int i0 = (int)idx, i1 = i0 + 1 >= len ? 0 : i0 + 1;
                float o = (cap[(size_t)i0] + (idx - (float)i0) * (cap[(size_t)i1] - cap[(size_t)i0])) * e * std::pow(dec, (float)slice);
                L[i] = o; R[i] = o;
            } else {
                cap[(size_t)pos] = 0.5f * (L[i] + R[i]);
                L[i] *= e; R[i] *= e;
            }
            if (++pos >= len) { pos = 0; slice = (slice + 1) % cyc; }
        }
    }
};

static const char *const kLoopModes[] = {"Stop", "Record", "Play", "Overdub"};
static const char *const kLoopSpeeds[] = {"Half", "Normal", "Double"};
struct Looper : Algo {
    std::vector<float> buf; int len = 0, mode = 0, wpos = 0, lastW = -1; float pos = 0;
    bool spills() const override { return false; }
    void init(float s) override { sr = s; buf.assign((size_t)(sr * 60.f), 0.f); }
    void reset() override { len = 0; mode = 0; wpos = 0; pos = 0; lastW = -1; }
    void process(float *L, float *R, int n) override {
        int m = (int)clampf(p[1], 0, 3);
        if (m != mode) {
            if (mode == 1) { len = wpos; pos = 0; }
            if (m == 1) { len = 0; wpos = 0; }
            if (mode == 0) pos = 0;
            mode = m;
        }
        float lvl = p[2] * 0.01f, dec = p[3] * 0.01f, speed = p[4] < 0.5f ? 0.5f : p[4] < 1.5f ? 1.f : 2.f;
        if (p[5] > 0.5f) speed = -speed;
        bool playing = (mode == 2 || mode == 3) && len >= 256;
        for (int i = 0; i < n; i++) {
            float x = 0.5f * (L[i] + R[i]), o = 0;
            if (mode == 1) {
                if (wpos < (int)buf.size()) buf[(size_t)wpos++] = x;
            } else if (playing) {
                int i0 = std::min((int)pos, len - 1), i1 = i0 + 1 >= len ? 0 : i0 + 1;
                o = buf[(size_t)i0] + (pos - (float)i0) * (buf[(size_t)i1] - buf[(size_t)i0]);
                float e = std::min(1.f, std::min(pos, (float)len - pos) / 64.f);
                if (mode == 3 && i0 != lastW) { buf[(size_t)i0] = buf[(size_t)i0] * dec + x * e; lastW = i0; }
                o *= e * lvl;
                pos += speed;
                if (pos >= (float)len) pos -= (float)len;
                if (pos < 0) pos += (float)len;
            }
            L[i] += o; R[i] += o;
        }
    }
};

// ---------------------------------------------------------------- registry

static ParamDesc mix(float d) { return {"Mix", 0, 100, d, LIN, "%", nullptr, F_STD}; }
static ParamDesc lin(const char *n, float a, float b, float d, const char *u = "") { return {n, a, b, d, LIN, u, nullptr, F_STD}; }
static ParamDesc lg(const char *n, float a, float b, float d, const char *u = "") { return {n, a, b, d, LOG, u, nullptr, F_STD}; }
static ParamDesc tm(const char *n, float a, float b, float d) { return {n, a, b, d, TIME, "ms", nullptr, F_STD}; }
static ParamDesc st(const char *n, float a, float b, float d, const char *const *labels = nullptr, const char *u = "", PFmt f = F_STD) {
    return {n, a, b, d, STEP, u, labels, f};
}
static ParamDesc iv(const char *n, float d) { return st(n, -14, 14, d, nullptr, "", F_INTERVAL); }
static const char *const kOnOff[] = {"Off", "On"};
static const char *const kCarrier[] = {"Sine", "Triangle"};

template <class T> static Algo *mk() { return new T(); }

const std::vector<AlgoInfo> &registry() {
    static const std::vector<AlgoInfo> r = [] {
    std::vector<AlgoInfo> r = {
        {"Micro Shift", "Pitch", "Dual micro-detuned voices with delay for wide, thick stereo",
         {mix(50), lin("Pitch A", 0, 50, 9, "ct"), lin("Pitch B", 0, 50, 9, "ct"), lin("Delay A", 0, 200, 12, "ms"), lin("Delay B", 0, 200, 19, "ms"),
          lin("Feedback", 0, 90, 0, "%"), lin("Mod Depth", 0, 100, 15, "%"), lg("Mod Rate", 0.05f, 5, 0.4f, "Hz"), lg("Tone", 1000, 20000, 12000, "Hz"), lin("Width", 0, 100, 100, "%")},
         mk<MicroShift>},
        {"Diatonic", "Pitch", "Two harmony voices that follow your playing inside a key and scale",
         {mix(50), iv("Interval A", 2), iv("Interval B", 4), st("Key", 0, 11, 0, kKeys), st("Scale", 0, 13, 0, kScaleNames), lin("Level A", 0, 100, 100, "%"),
          lin("Level B", 0, 100, 75, "%"), tm("Delay A", 0, 1000, 0), tm("Delay B", 0, 1000, 0), lin("Feedback", 0, 90, 0, "%")},
         mk<Diatonic>},
        {"Quad Voice", "Pitch", "Four diatonic harmony voices spread across the stereo field",
         {mix(55), iv("Voice 1", 2), iv("Voice 2", 4), iv("Voice 3", -3), iv("Voice 4", 7), st("Key", 0, 11, 0, kKeys), st("Scale", 0, 13, 0, kScaleNames),
          tm("Spread", 0, 500, 0), lin("Width", 0, 100, 100, "%"), lin("Detune", 0, 25, 6, "ct")},
         mk<QuadVoice>},
        {"Chromatic", "Pitch", "Classic dual pitch shifter with delay and regenerating feedback",
         {mix(50), st("Pitch A", -24, 24, 7, nullptr, "st"), lin("Fine A", -50, 50, 0, "ct"), st("Pitch B", -24, 24, -12, nullptr, "st"), lin("Fine B", -50, 50, 0, "ct"),
          tm("Delay A", 0, 2000, 0), tm("Delay B", 0, 2000, 0), lin("Feedback", 0, 100, 0, "%"), lin("Balance", 0, 100, 50, "%"), lg("Tone", 1000, 20000, 14000, "Hz")},
         mk<DualShift<Shifter>>},
        {"Octaver", "Pitch", "Two sub octaves and an octave up with tone shaping and grit",
         {mix(50), lin("Sub 1", 0, 100, 80, "%"), lin("Sub 2", 0, 100, 0, "%"), lin("Octave Up", 0, 100, 0, "%"), lg("Sub Tone", 100, 4000, 900, "Hz"),
          lg("Up Tone", 1000, 14000, 7000, "Hz"), lin("Grit", 0, 100, 0, "%")},
         mk<Octaver>},
        {"Crystals", "Pitch", "Reverse pitch-shifted grains regenerating into a reverb",
         {mix(50), st("Pitch A", -24, 24, 12, nullptr, "st"), st("Pitch B", -24, 24, 19, nullptr, "st"), lg("Length A", 20, 1000, 260, "ms"), lg("Length B", 20, 1000, 390, "ms"),
          tm("Delay A", 0, 1500, 0), tm("Delay B", 0, 1500, 120), lin("Feedback", 0, 100, 55, "%"), lin("Verb Mix", 0, 100, 40, "%"), lin("Verb Decay", 0, 100, 55, "%")},
         mk<Crystals>},
        {"Pitch Flex", "Pitch", "Pedal-style pitch bends between heel and toe. Made for the Hot Knob",
         {mix(100), lin("Pedal", 0, 100, 0, "%"), st("Heel", -24, 24, 0, nullptr, "st"), st("Toe", -24, 24, 12, nullptr, "st"), lg("Glide", 1, 500, 25, "ms")},
         mk<Flex<Shifter>>},
        {"Arp Shift", "Pitch", "Step-sequenced pitch shifting that turns held notes into arpeggios",
         {mix(60), st("Pattern", 0, 6, 1, kArpNames), tm("Step", 30, 1000, 125), st("Steps", 2, 8, 8), lin("Glide", 0, 100, 2, "ms"), lin("Chop", 0, 100, 35, "%")},
         mk<ArpShift>},

        {"Poly Shift", "Pitch", "Polyphonic dual pitch shifter: clean intervals on full chords",
         {mix(50), st("Pitch A", -24, 24, 12, nullptr, "st"), lin("Fine A", -50, 50, 0, "ct"), st("Pitch B", -24, 24, -12, nullptr, "st"), lin("Fine B", -50, 50, 0, "ct"),
          tm("Delay A", 0, 2000, 0), tm("Delay B", 0, 2000, 0), lin("Feedback", 0, 100, 0, "%"), lin("Balance", 0, 100, 50, "%"), lg("Tone", 1000, 20000, 14000, "Hz")},
         mk<DualShift<PolyShifter>>},
        {"Poly Flex", "Pitch", "Polyphonic pedal-style bends that keep chords intact",
         {mix(100), lin("Pedal", 0, 100, 0, "%"), st("Heel", -24, 24, 0, nullptr, "st"), st("Toe", -24, 24, 12, nullptr, "st"), lg("Glide", 1, 500, 25, "ms")},
         mk<Flex<PolyShifter>>},

        {"Digital Delay", "Delay", "Clean stereo delay with ping-pong, filters, modulation and ducking",
         {mix(35), tm("Time A", 1, 2000, 375), tm("Time B", 1, 2000, 500), lin("Feedback", 0, 100, 40, "%"), lin("Cross", 0, 100, 0, "%"), lg("Hi Cut", 500, 20000, 9000, "Hz"),
          lg("Lo Cut", 20, 2000, 80, "Hz"), lin("Mod Depth", 0, 100, 0, "%"), lg("Mod Rate", 0.05f, 8, 0.5f, "Hz"), lin("Duck", 0, 100, 0, "%")},
         mk<DigitalDelay>},
        {"Tape Echo", "Delay", "Saturating tape loop with wow, flutter and runaway feedback",
         {mix(35), tm("Time", 20, 1500, 330), lin("Feedback", 0, 110, 45, "%"), lin("Saturation", 0, 100, 40, "%"), lin("Wow", 0, 100, 30, "%"), lin("Flutter", 0, 100, 20, "%"),
          lg("Tone", 800, 9000, 3500, "Hz"), lg("Bass Cut", 20, 800, 120, "Hz"), lin("Spread", 0, 100, 30, "%")},
         mk<TapeEcho>},
        {"Reverse", "Delay", "Backwards delay with swelling crossfaded segments",
         {mix(55), tm("Time", 100, 2000, 600), lin("Feedback", 0, 95, 25, "%"), lg("Tone", 800, 20000, 9000, "Hz"), lin("Width", 0, 100, 70, "%")},
         mk<ReverseDelay>},
        {"Multi Tap", "Delay", "Up to 32 taps: rhythmic clusters, swells and reverb-like smears",
         {mix(50), tm("Length", 10, 4000, 600), st("Taps", 1, 32, 12), lin("Spread", -100, 100, 0, "%"), lin("Taper", -100, 100, 40, "%"), lin("Tone", -100, 100, -20, "%"),
          lin("Width", 0, 100, 100, "%"), lin("Slurm", 0, 100, 20, "%"), lin("Feedback", 0, 95, 0, "%"), lin("Pre-Delay", 0, 500, 0, "ms")},
         mk<MultiTap>},

        {"Tri Chorus", "Modulation", "Lush three-voice bucket-brigade style chorus",
         {mix(50), lg("Rate", 0.05f, 10, 0.6f, "Hz"), lin("Depth", 0, 100, 50, "%"), lin("Delay", 1, 30, 8, "ms"), st("Voices", 1, 3, 3), lin("Width", 0, 100, 100, "%"),
          lin("Feedback", 0, 80, 0, "%"), lg("Tone", 1000, 20000, 10000, "Hz")},
         mk<Chorus>},
        {"Flanger", "Modulation", "Through-zero style jet flanger with positive or negative feedback",
         {mix(50), lg("Rate", 0.02f, 10, 0.25f, "Hz"), lin("Depth", 0, 100, 70, "%"), lg("Manual", 0.1f, 10, 1.2f, "ms"), lin("Feedback", -95, 95, 60, "%"), lin("Stereo", 0, 180, 90, "deg")},
         mk<Flanger>},
        {"Phaser", "Modulation", "4 to 12 stage phaser with feedback and stereo sweep",
         {mix(50), lg("Rate", 0.02f, 10, 0.4f, "Hz"), lin("Depth", 0, 100, 75, "%"), lg("Center", 100, 4000, 700, "Hz"), lin("Feedback", 0, 95, 50, "%"), st("Stages", 0, 3, 1, kStageNames),
          lin("Stereo", 0, 180, 60, "deg")},
         mk<Phaser>},
        {"Rotary", "Modulation", "Rotating horn and drum speaker with inertia between speeds",
         {mix(100), st("Speed", 0, 2, 0, kRotNames), lin("Balance", 0, 100, 50, "%"), lin("Drive", 0, 100, 20, "%"), lg("Inertia", 0.2f, 5, 1, "s"), lin("Depth", 0, 100, 65, "%"),
          lin("Width", 0, 100, 80, "%")},
         mk<Rotary>},
        {"Tremolo Pan", "Modulation", "Amplitude tremolo that morphs into auto-pan",
         {mix(100), lg("Rate", 0.1f, 20, 5, "Hz"), lin("Depth", 0, 100, 70, "%"), st("Shape", 0, 3, 0, kShapeNames), lin("Stereo", 0, 180, 0, "deg")},
         mk<Tremolo>},
        {"Ring Mod", "Modulation", "Ring modulator with swept carrier for bells and robots",
         {mix(60), lg("Frequency", 20, 4000, 440, "Hz"), st("Carrier", 0, 1, 0, kCarrier), lg("LFO Rate", 0.05f, 10, 0.5f, "Hz"), lin("LFO Depth", 0, 100, 0, "%"), lg("Tone", 1000, 20000, 9000, "Hz")},
         mk<RingMod>},
        {"Auto Wah", "Modulation", "Envelope and LFO controlled resonant filter",
         {mix(100), lin("Sensitivity", 0, 100, 60, "%"), lg("Frequency", 100, 2000, 350, "Hz"), lin("Range", 0, 4, 2.5f, "oct"), lg("Resonance", 0.7f, 12, 5), lg("LFO Rate", 0.05f, 10, 1, "Hz"),
          lin("LFO Depth", 0, 100, 0, "%"), lg("Attack", 1, 100, 8, "ms"), lg("Release", 20, 800, 160, "ms"), st("Type", 0, 2, 1, kFilterNames)},
         mk<AutoWah>},

        {"Uni Vibe", "Modulation", "Photocell-style four stage vibe: chorus at 50% mix, vibrato at 100%",
         {mix(50), lg("Rate", 0.1f, 10, 1.5f, "Hz"), lin("Depth", 0, 100, 70, "%"), lin("Feedback", 0, 80, 20, "%"), lin("Stereo", 0, 180, 30, "deg")},
         mk<UniVibe>},

        {"Room", "Reverb", "Tight, natural room ambience",
         {mix(30), lg("Decay", 0.2f, 20, 0.8f, "s"), lin("Size", 0, 100, 50, "%"), lin("Pre-Delay", 0, 250, 5, "ms"), lg("Hi Cut", 1000, 20000, 7000, "Hz"), lg("Lo Cut", 20, 1000, 90, "Hz"),
          lin("Diffusion", 0, 100, 75, "%"), lin("Mod Depth", 0, 100, 15, "%"), lg("Mod Rate", 0.1f, 5, 0.6f, "Hz")},
         [] () -> Algo * { return new ReverbAlgo(0.42f); }},
        {"Hall", "Reverb", "Large concert hall with a smooth modulated tail",
         {mix(35), lg("Decay", 0.2f, 20, 2.8f, "s"), lin("Size", 0, 100, 60, "%"), lin("Pre-Delay", 0, 250, 25, "ms"), lg("Hi Cut", 1000, 20000, 6500, "Hz"), lg("Lo Cut", 20, 1000, 80, "Hz"),
          lin("Diffusion", 0, 100, 85, "%"), lin("Mod Depth", 0, 100, 30, "%"), lg("Mod Rate", 0.1f, 5, 0.5f, "Hz")},
         [] () -> Algo * { return new ReverbAlgo(1.5f); }},
        {"Plate", "Reverb", "Bright, dense studio plate",
         {mix(35), lg("Decay", 0.2f, 20, 1.8f, "s"), lin("Size", 0, 100, 50, "%"), lin("Pre-Delay", 0, 250, 10, "ms"), lg("Hi Cut", 1000, 20000, 11000, "Hz"), lg("Lo Cut", 20, 1000, 150, "Hz"),
          lin("Diffusion", 0, 100, 95, "%"), lin("Mod Depth", 0, 100, 20, "%"), lg("Mod Rate", 0.1f, 5, 0.9f, "Hz")},
         [] () -> Algo * { return new ReverbAlgo(0.8f); }},
        {"Void", "Reverb", "Cavernous, otherworldly space that can sustain forever",
         {mix(50), lg("Gravity", 0.5f, 60, 12, "s"), lin("Size", 0, 100, 70, "%"), tm("Pre-Delay", 0, 2000, 60), lg("Lo Cut", 20, 1000, 100, "Hz"), lg("Hi Cut", 1000, 20000, 5500, "Hz"),
          lin("Mod Depth", 0, 100, 45, "%"), lg("Mod Rate", 0.05f, 5, 0.3f, "Hz"), lin("Feedback", 0, 100, 20, "%"), st("Freeze", 0, 1, 0, kOnOff)},
         mk<VoidVerb>},
        {"Shimmer", "Reverb", "Reverb whose tail climbs in octaves",
         {mix(45), lg("Decay", 0.5f, 30, 5, "s"), lin("Size", 0, 100, 60, "%"), st("Pitch", -12, 24, 12, nullptr, "st"), lin("Shimmer", 0, 100, 55, "%"), lg("Hi Cut", 1000, 20000, 7500, "Hz"),
          lg("Lo Cut", 20, 1000, 150, "Hz"), lin("Mod Depth", 0, 100, 40, "%"), lin("Pre-Delay", 0, 250, 20, "ms")},
         mk<Shimmer>},

        {"EQ Compressor", "Tone", "Soft-knee compressor followed by a three band EQ",
         {mix(100), lin("Threshold", -60, 0, -22, "dB"), lg("Ratio", 1, 20, 4, ":1"), lg("Attack", 0.1f, 100, 10, "ms"), lg("Release", 20, 1000, 160, "ms"), lin("Makeup", 0, 24, 5, "dB"),
          lin("Low", -15, 15, 0, "dB"), lin("Mid", -15, 15, 0, "dB"), lg("Mid Freq", 200, 5000, 800, "Hz"), lin("High", -15, 15, 0, "dB")},
         mk<EQComp>},
        {"Crush", "Tone", "Overdrive, distortion and fuzz with sag, octave and EQ",
         {mix(100), lin("Drive", 0, 100, 45, "%"), st("Type", 0, 2, 0, kCrushNames), lin("Bass", -12, 12, 0, "dB"), lin("Mid", -12, 12, 2, "dB"), lin("Treble", -12, 12, 0, "dB"),
          lg("Mid Freq", 200, 3000, 750, "Hz"), lin("Sag", 0, 100, 0, "%"), lin("Octave", 0, 100, 0, "%"), lin("Level", -24, 12, 0, "dB")},
         mk<Crush>},
        {"Resonator", "Tone", "Four tuned resonant strings that ring along with your playing",
         {mix(45), st("Note 1", 28, 76, 40, nullptr, "", F_NOTE), st("Note 2", 28, 76, 47, nullptr, "", F_NOTE), st("Note 3", 28, 76, 52, nullptr, "", F_NOTE),
          st("Note 4", 28, 76, 56, nullptr, "", F_NOTE), lin("Resonance", 0, 100, 70, "%"), lg("Damping", 500, 12000, 5000, "Hz"), lin("Width", 0, 100, 80, "%")},
         mk<Resonator>},

        {"Saw Synth", "Synth", "Pitch-tracking detuned saw synth with sub and envelope filter (single notes)",
         {mix(70), st("Octave", -2, 2, 0, nullptr, "oct"), lin("Detune", 0, 50, 12, "ct"), lin("Sub", 0, 100, 40, "%"), lg("Cutoff", 100, 10000, 1200, "Hz"), lg("Resonance", 0.5f, 10, 2),
          lin("Env Amount", 0, 5, 2.5f, "oct"), lg("Glide", 1, 300, 15, "ms"), lg("Release", 20, 2000, 250, "ms")},
         mk<SawSynth>},
        {"Grain Cloud", "Granular", "Scatters pitched grains of your playing across time and stereo",
         {mix(50), lg("Size", 10, 500, 90, "ms"), lg("Density", 1, 60, 18, "Hz"), lin("Position", 0, 2000, 150, "ms"), lin("Spray", 0, 100, 30, "%"), st("Pitch", -24, 24, 0, nullptr, "st"),
          lin("Scatter", 0, 100, 10, "ct"), lin("Reverse", 0, 100, 0, "%"), lin("Feedback", 0, 90, 20, "%"), lin("Width", 0, 100, 100, "%")},
         mk<GrainCloud>},
        {"Stutter", "Granular", "Rhythmic buffer repeats with pitch steps, decay and reverse",
         {mix(100), tm("Slice", 10, 500, 125), st("Repeats", 1, 16, 3), st("Cycle", 2, 16, 8), lin("Pitch Step", -12, 12, 0, "st"), lin("Decay", 0, 100, 0, "%"), st("Reverse", 0, 1, 0, kOnOff)},
         mk<Stutter>},
        {"Looper", "Looper", "Record, play and overdub a loop of up to 60 seconds",
         {mix(100), st("Mode", 0, 3, 0, kLoopModes), lin("Loop Level", 0, 100, 100, "%"), lin("Decay", 0, 100, 100, "%"), st("Speed", 0, 2, 1, kLoopSpeeds), st("Reverse", 0, 1, 0, kOnOff)},
         mk<Looper>},

        // voicings of the engines above
        {"Mod Delay", "Delay", "Stereo delay with deep chorus-style modulation on the repeats",
         {mix(35), tm("Time A", 1, 2000, 380), tm("Time B", 1, 2000, 380), lin("Feedback", 0, 100, 45, "%"), lin("Cross", 0, 100, 20, "%"), lg("Hi Cut", 500, 20000, 7000, "Hz"),
          lg("Lo Cut", 20, 2000, 100, "Hz"), lin("Mod Depth", 0, 100, 65, "%"), lg("Mod Rate", 0.05f, 8, 0.7f, "Hz"), lin("Duck", 0, 100, 0, "%")},
         mk<DigitalDelay>},
        {"Ducked Delay", "Delay", "Delay that stays out of the way while you play and blooms in the gaps",
         {mix(45), tm("Time A", 1, 2000, 375), tm("Time B", 1, 2000, 500), lin("Feedback", 0, 100, 50, "%"), lin("Cross", 0, 100, 0, "%"), lg("Hi Cut", 500, 20000, 9000, "Hz"),
          lg("Lo Cut", 20, 2000, 80, "Hz"), lin("Mod Depth", 0, 100, 0, "%"), lg("Mod Rate", 0.05f, 8, 0.5f, "Hz"), lin("Duck", 0, 100, 80, "%")},
         mk<DigitalDelay>},
        {"Drive Tape", "Delay", "Worn tape echo pushed hard into saturation",
         {mix(40), tm("Time", 20, 1500, 280), lin("Feedback", 0, 110, 60, "%"), lin("Saturation", 0, 100, 90, "%"), lin("Wow", 0, 100, 55, "%"), lin("Flutter", 0, 100, 45, "%"),
          lg("Tone", 800, 9000, 2600, "Hz"), lg("Bass Cut", 20, 800, 200, "Hz"), lin("Spread", 0, 100, 40, "%")},
         mk<TapeEcho>},
        {"Grain Mod", "Granular", "Dense short grains for granular chorus and shimmer textures",
         {mix(50), lg("Size", 10, 500, 60, "ms"), lg("Density", 1, 60, 45, "Hz"), lin("Position", 0, 2000, 5, "ms"), lin("Spray", 0, 100, 3, "%"), st("Pitch", -24, 24, 0, nullptr, "st"),
          lin("Scatter", 0, 100, 60, "ct"), lin("Reverse", 0, 100, 0, "%"), lin("Feedback", 0, 90, 0, "%"), lin("Width", 0, 100, 100, "%")},
         mk<GrainCloud>},
    };
    appendMoreAlgos(r);
    return r;
    }();
    return r;
}

}  // namespace hz
