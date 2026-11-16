// Engine: two effect slots, lock-free control surface, CoreAudio IOProc, C API.
#include "dsp.hpp"
#include "include/HarmonizerDSP.h"

#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <cfenv>
#include <cstdio>
#include <mach/mach_time.h>
#include <memory>

using namespace hz;

namespace {

const int BLK = 256;
const int SC = 16384;  // scope ring size (power of two)
const int VIS = 512;

struct Div { const char *name; float beats; };
const Div kDivs[] = {{"1/32", 0.125f}, {"1/16T", 1 / 6.f}, {"1/16", 0.25f}, {"1/8T", 1 / 3.f}, {"1/16.", 0.375f}, {"1/8", 0.5f}, {"1/4T", 2 / 3.f},
                     {"1/8.", 0.75f},  {"1/4", 1.f},       {"1/4.", 1.5f},  {"1/2", 2.f},      {"1/2.", 3.f},     {"1/1", 4.f}};
const int kNumDivs = 13;

// normalised 0..1 -> real value. div receives the tempo division index or -1.
float mapParam(const ParamDesc &d, float n, bool sync, float bpm, int *div = nullptr) {
    n = clampf(n, 0.f, 1.f);
    if (div) *div = -1;
    switch (d.kind) {
    case LIN: return d.mn + (d.mx - d.mn) * n;
    case LOG: return d.mn * std::pow(d.mx / d.mn, n);
    case STEP: return std::round(d.mn + (d.mx - d.mn) * n);
    case TIME:
        if (sync) {
            if (d.mn <= 0 && n < 0.02f) return 0;
            int i = (int)std::lround(n * (float)(kNumDivs - 1));
            if (div) *div = i;
            return clampf(kDivs[i].beats * 60000.f / std::max(bpm, 20.f), d.mn, d.mx);
        }
        return d.mn + (d.mx - d.mn) * n * n;
    }
    return 0;
}

float normOf(const ParamDesc &d, float v) {
    v = clampf(v, d.mn, d.mx);
    switch (d.kind) {
    case LOG: return std::log(v / d.mn) / std::log(d.mx / d.mn);
    case TIME: return std::sqrt((v - d.mn) / (d.mx - d.mn));
    default: return (v - d.mn) / (d.mx - d.mn);
    }
}

inline float softLimit(float x) {
    float a = std::fabs(x);
    if (a <= 0.8f) return x;
    return std::copysign(0.8f + 0.2f * softclip((a - 0.8f) * 5.f), x);
}

void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.f * kPi / (float)len, wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = a + len / 2;
                float xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
                float t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

}  // namespace

struct HZEngine {
    float sr = 48000;
    bool prepared = false;
    std::vector<std::unique_ptr<Algo>> algos[2];

    // control surface
    std::atomic<int> algoIdx[2];
    std::atomic<uint32_t> gen{0};  // odd while the UI is mid-way through loading a slot
    std::atomic<float> norm[2][10];
    std::atomic<bool> slotOn[2];
    std::atomic<bool> bypass{false}, mute{false}, parallel{false}, sync{false};
    std::atomic<float> bpm{120}, inGain{1}, outGain{1}, gateDb{-100};
    std::atomic<int> inCh{0}, outL{0}, outR{1};

    // metering
    std::atomic<float> mIn{0}, mOutL{0}, mOutR{0}, cpu{0};
    float scIn[SC], scOut[SC];
    std::atomic<uint32_t> scW{0};

    // audio thread state
    int cur[2] = {-1, -1};
    int spill[2] = {-1, -1}, spillLeft[2] = {0, 0};  // previous algorithm still ringing out
    float sL[BLK], sR[BLK];
    float last[2][10];
    float lastBpm = 0;
    bool lastSync = false;
    Smooth gIn, gOut, gByp, gSlot[2];
    float fade[2] = {0, 0}, gateEnv = 0, gateGain = 1;
    bool gateOpen = false;
    float bufIn[BLK], dL[BLK], dR[BLK], pL[BLK], pR[BLK];

    // device
    AudioDeviceID dev = 0;
    AudioDeviceIOProcID procID = nullptr;
    double ticksToSec = 0;

    // UI thread state
    PitchTracker tuner;
    float specState[VIS];
    std::vector<float> visBuf, re, im;

    HZEngine() {
        for (int s = 0; s < 2; s++) {
            algoIdx[s] = 0;
            slotOn[s] = true;
            for (int p = 0; p < 10; p++) { norm[s][p] = 0.5f; last[s][p] = -1; }
        }
        std::memset(scIn, 0, sizeof scIn);
        std::memset(scOut, 0, sizeof scOut);
        std::memset(specState, 0, sizeof specState);
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        ticksToSec = (double)tb.numer / (double)tb.denom * 1e-9;
        visBuf.assign(4096, 0.f);
        re.assign(2048, 0.f);
        im.assign(2048, 0.f);
    }

    void prepare(double sampleRate) {
        sr = (float)sampleRate;
        const auto &reg = registry();
        for (int s = 0; s < 2; s++) {
            algos[s].clear();
            for (const auto &info : reg) {
                std::unique_ptr<Algo> a(info.make());
                for (size_t p = 0; p < info.pd.size(); p++) a->p[p] = info.pd[p].def;
                a->init(sr);
                a->update();
                a->reset();
                algos[s].push_back(std::move(a));
            }
            cur[s] = -1;
            spill[s] = -1;
            gSlot[s].setTime(15, sr);
            gSlot[s].snap(slotOn[s] ? 1.f : 0.f);
            fade[s] = 0;
        }
        gIn.setTime(15, sr); gOut.setTime(15, sr); gByp.setTime(12, sr);
        gIn.snap(inGain); gOut.snap(outGain); gByp.snap(bypass ? 0.f : 1.f);
        gateEnv = 0; gateGain = 1; gateOpen = false;
        tuner.init(sr, sr, false);
        prepared = true;
    }

    void runSlot(int s, float *L, float *R, int n) {
        bool on = slotOn[s];
        if (!on && gSlot[s].y < 1e-4f) { gSlot[s].snap(0); return; }
        Algo *a = algos[s][(size_t)cur[s]].get();
        std::memcpy(dL, L, sizeof(float) * (size_t)n);
        std::memcpy(dR, R, sizeof(float) * (size_t)n);
        a->process(L, R, n);
        float m = clampf(a->p[0] * 0.01f, 0, 1), dryG = std::cos(m * kPi * 0.5f), wetG = std::sin(m * kPi * 0.5f);
        float tg = on ? 1.f : 0.f, finc = 1.f / (0.03f * sr);
        for (int i = 0; i < n; i++) {
            fade[s] = std::min(1.f, fade[s] + finc);
            float e = gSlot[s].next(tg) * fade[s];
            L[i] = dL[i] * (1.f - e * (1.f - dryG)) + L[i] * e * wetG;
            R[i] = dR[i] * (1.f - e * (1.f - dryG)) + R[i] * e * wetG;
        }
        // spillover: the algorithm that was just replaced keeps ringing out on silence
        if (spill[s] >= 0) {
            Algo *o = algos[s][(size_t)spill[s]].get();
            std::memset(sL, 0, sizeof(float) * (size_t)n);
            std::memset(sR, 0, sizeof(float) * (size_t)n);
            o->process(sL, sR, n);
            float g = std::sin(clampf(o->p[0] * 0.01f, 0, 1) * kPi * 0.5f) * std::min(1.f, (float)spillLeft[s] / (2.f * sr));
            for (int i = 0; i < n; i++) { L[i] += sL[i] * g; R[i] += sR[i] * g; }
            if ((spillLeft[s] -= n) <= 0) spill[s] = -1;
        }
    }

    // n <= BLK
    void block(const float *in, float *L, float *R, int n) {
        const auto &reg = registry();
        bool sy = sync;
        float bp = bpm;
        bool force = sy != lastSync || bp != lastBpm;
        lastSync = sy; lastBpm = bp;
        uint32_t g0 = gen.load(std::memory_order_acquire);
        int wantAlgo[2];
        float wantNorm[2][10];
        for (int s = 0; s < 2; s++) {
            wantAlgo[s] = algoIdx[s].load();
            for (int p = 0; p < 10; p++) wantNorm[s][p] = norm[s][p].load();
        }
        // a slot load was in flight while we read: keep last block's settings, pick it up next block
        bool stable = !(g0 & 1) && gen.load(std::memory_order_acquire) == g0;
        for (int s = 0; s < 2; s++) {
            if (!stable && cur[s] >= 0) continue;
            int ai = clampf((float)wantAlgo[s], 0, (float)reg.size() - 1);
            bool changed = ai != cur[s], dirty = changed || force;
            Algo *a = algos[s][(size_t)ai].get();
            const AlgoInfo &info = reg[(size_t)ai];
            for (size_t p = 0; p < info.pd.size(); p++) {
                float v = wantNorm[s][p];
                if (dirty || v != last[s][p]) {
                    last[s][p] = v;
                    a->p[p] = mapParam(info.pd[p], v, sy, bp);
                    dirty = true;
                }
            }
            if (dirty) { a->bpm = bp; a->update(); }
            if (changed) {
                for (size_t p = 0; p < 10; p++) last[s][p] = wantNorm[s][p];
                bool resume = ai == spill[s];  // switched back while its tail was still ringing
                int old = cur[s];
                spill[s] = old >= 0 && algos[s][(size_t)old]->spills() ? old : -1;
                spillLeft[s] = (int)(8.f * sr);
                if (!resume) a->reset();
                cur[s] = ai;
                fade[s] = 0;
            }
        }

        // input stage: gain, meter, gate
        float gi = inGain, pk = 0, gdb = gateDb;
        bool gateOn = gdb > -90.f;
        float thr = db2lin(gdb), envRel = std::exp(-1.f / (0.02f * sr));
        float gAtk = 1.f - std::exp(-1.f / (0.001f * sr)), gRel = 1.f - std::exp(-1.f / (0.12f * sr));
        uint32_t w = scW.load(std::memory_order_relaxed);
        for (int i = 0; i < n; i++) {
            float x = in[i] * gIn.next(gi), a = std::fabs(x);
            if (a > pk) pk = a;
            scIn[(w + (uint32_t)i) & (SC - 1)] = x;
            if (gateOn) {
                gateEnv = a > gateEnv ? a : gateEnv * envRel;
                if (gateEnv > thr) gateOpen = true;
                else if (gateEnv < thr * 0.5f) gateOpen = false;
                float tg = gateOpen ? 1.f : 0.f;
                gateGain += (tg - gateGain) * (tg > gateGain ? gAtk : gRel);
                x *= gateGain;
            }
            bufIn[i] = x;
            L[i] = x; R[i] = x;
        }
        if (pk > mIn.load(std::memory_order_relaxed)) mIn.store(pk, std::memory_order_relaxed);

        // effect slots
        bool byp = bypass;
        if (!byp || gByp.y > 1e-4f) {
            if (parallel) {
                std::memcpy(pL, L, sizeof(float) * (size_t)n);
                std::memcpy(pR, R, sizeof(float) * (size_t)n);
                runSlot(0, L, R, n);
                float tL[BLK], tR[BLK];
                std::memcpy(tL, L, sizeof(float) * (size_t)n);
                std::memcpy(tR, R, sizeof(float) * (size_t)n);
                runSlot(1, pL, pR, n);
                bool both = slotOn[0] && slotOn[1];
                float g = both ? 0.70710678f : 1.f;
                for (int i = 0; i < n; i++) {
                    if (both) { L[i] = (tL[i] + pL[i]) * g; R[i] = (tR[i] + pR[i]) * g; }
                    else if (slotOn[1]) { L[i] = pL[i]; R[i] = pR[i]; }
                }
            } else {
                runSlot(0, L, R, n);
                runSlot(1, L, R, n);
            }
        }

        // output stage: bypass crossfade, gain, limiter, meters, scope
        float go = mute ? 0.f : outGain.load(), bt = byp ? 0.f : 1.f, pl = 0, pr = 0;
        for (int i = 0; i < n; i++) {
            float b = gByp.next(bt), g = gOut.next(go);
            float l = softLimit((bufIn[i] + b * (L[i] - bufIn[i])) * g), r = softLimit((bufIn[i] + b * (R[i] - bufIn[i])) * g);
            if (!(std::fabs(l) < 100.f)) l = 0;  // NaN/inf guard
            if (!(std::fabs(r) < 100.f)) r = 0;
            L[i] = l; R[i] = r;
            pl = std::max(pl, std::fabs(l)); pr = std::max(pr, std::fabs(r));
            scOut[(w + (uint32_t)i) & (SC - 1)] = 0.5f * (l + r);
        }
        if (pl > mOutL.load(std::memory_order_relaxed)) mOutL.store(pl, std::memory_order_relaxed);
        if (pr > mOutR.load(std::memory_order_relaxed)) mOutR.store(pr, std::memory_order_relaxed);
        scW.store(w + (uint32_t)n, std::memory_order_release);
    }
};

static float *findChannel(const AudioBufferList *l, int ch, int &stride) {
    if (!l || ch < 0) return nullptr;
    int c = 0;
    for (UInt32 b = 0; b < l->mNumberBuffers; b++) {
        const AudioBuffer &bf = l->mBuffers[b];
        if (ch < c + (int)bf.mNumberChannels) {
            stride = (int)bf.mNumberChannels;
            return bf.mData ? (float *)bf.mData + (ch - c) : nullptr;
        }
        c += (int)bf.mNumberChannels;
    }
    return nullptr;
}

static OSStatus ioProc(AudioObjectID, const AudioTimeStamp *, const AudioBufferList *inData, const AudioTimeStamp *, AudioBufferList *outData,
                       const AudioTimeStamp *, void *ctx) {
    HZEngine *e = (HZEngine *)ctx;
    fesetenv(FE_DFL_DISABLE_DENORMS_ENV);
    uint64_t t0 = mach_absolute_time();
    if (!outData || outData->mNumberBuffers == 0) return noErr;
    for (UInt32 b = 0; b < outData->mNumberBuffers; b++)
        if (outData->mBuffers[b].mData) std::memset(outData->mBuffers[b].mData, 0, outData->mBuffers[b].mDataByteSize);
    const AudioBuffer &ob = outData->mBuffers[0];
    int frames = (int)(ob.mDataByteSize / (sizeof(float) * std::max<UInt32>(1, ob.mNumberChannels)));
    int si = 1, sl = 1, sr2 = 1;
    const float *ip = findChannel(inData, e->inCh, si);
    float *lp = findChannel(outData, e->outL, sl), *rp = findChannel(outData, e->outR, sr2);
    if (ip && inData) {
        // never read past the end of the input buffer if it is shorter than the output
        int c = 0;
        for (UInt32 b = 0; b < inData->mNumberBuffers; b++) {
            const AudioBuffer &bf = inData->mBuffers[b];
            if ((int)e->inCh < c + (int)bf.mNumberChannels) {
                frames = std::min(frames, (int)(bf.mDataByteSize / (sizeof(float) * std::max<UInt32>(1, bf.mNumberChannels))));
                break;
            }
            c += (int)bf.mNumberChannels;
        }
    }
    float in[BLK], l[BLK], r[BLK];
    for (int done = 0; done < frames;) {
        int n = std::min(BLK, frames - done);
        for (int i = 0; i < n; i++) in[i] = ip ? ip[(done + i) * si] : 0.f;
        e->block(in, l, r, n);
        if (lp && rp && lp != rp) {
            for (int i = 0; i < n; i++) { lp[(done + i) * sl] = l[i]; rp[(done + i) * sr2] = r[i]; }
        } else if (lp) {
            for (int i = 0; i < n; i++) lp[(done + i) * sl] = 0.5f * (l[i] + r[i]);
        }
        done += n;
    }
    if (frames > 0) {
        float load = (float)((double)(mach_absolute_time() - t0) * e->ticksToSec / ((double)frames / (double)e->sr));
        float c = e->cpu.load(std::memory_order_relaxed);
        e->cpu.store(load > c ? load : c + 0.02f * (load - c), std::memory_order_relaxed);
    }
    return noErr;
}

// ------------------------------------------------------------------- C API

extern "C" {

HZEngine *hz_create(void) { return new HZEngine(); }
void hz_destroy(HZEngine *e) {
    if (!e) return;
    hz_stop(e);
    delete e;
}
void hz_prepare(HZEngine *e, double sampleRate) { e->prepare(sampleRate); }

int hz_start(HZEngine *e, unsigned int deviceID) {
    hz_stop(e);
    if (!e->prepared) return -1;
    OSStatus st = AudioDeviceCreateIOProcID(deviceID, ioProc, e, &e->procID);
    if (st != noErr) { e->procID = nullptr; return (int)st; }
    st = AudioDeviceStart(deviceID, e->procID);
    if (st != noErr) {
        AudioDeviceDestroyIOProcID(deviceID, e->procID);
        e->procID = nullptr;
        return (int)st;
    }
    e->dev = deviceID;
    return 0;
}
void hz_stop(HZEngine *e) {
    if (!e->procID) return;
    AudioDeviceStop(e->dev, e->procID);
    AudioDeviceDestroyIOProcID(e->dev, e->procID);
    e->procID = nullptr;
    e->dev = 0;
    e->cpu = 0;
}
void hz_set_io(HZEngine *e, int inChannel, int outLeft, int outRight) { e->inCh = inChannel; e->outL = outLeft; e->outR = outRight; }

int hz_algo_count(void) { return (int)registry().size(); }
static const AlgoInfo &info(int a) { return registry()[(size_t)clampf((float)a, 0, (float)registry().size() - 1)]; }
static const ParamDesc &pdesc(int a, int p) { return info(a).pd[(size_t)clampf((float)p, 0, (float)info(a).pd.size() - 1)]; }
const char *hz_algo_name(int a) { return info(a).name; }
const char *hz_algo_category(int a) { return info(a).cat; }
const char *hz_algo_blurb(int a) { return info(a).blurb; }
int hz_algo_param_count(int a) { return (int)info(a).pd.size(); }
const char *hz_param_name(int a, int p) { return pdesc(a, p).name; }
float hz_param_default(int a, int p) { return normOf(pdesc(a, p), pdesc(a, p).def); }
float hz_param_normalize(int a, int p, float v) { return normOf(pdesc(a, p), v); }

void hz_param_display(HZEngine *e, int a, int p, float n, char *buf, int len) {
    const ParamDesc &d = pdesc(a, p);
    int div = -1;
    float v = mapParam(d, n, e ? e->sync.load() : false, e ? e->bpm.load() : 120.f, &div);
    static const char *const keys[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    if (d.kind == STEP) {
        int iv = (int)v;
        if (d.labels) { snprintf(buf, (size_t)len, "%s", d.labels[iv - (int)d.mn]); return; }
        if (d.fmt == F_NOTE) { snprintf(buf, (size_t)len, "%s%d", keys[((iv % 12) + 12) % 12], iv / 12 - 1); return; }
        if (d.fmt == F_INTERVAL) {
            int deg = std::abs(iv) + 1;
            const char *sg = iv > 0 ? "+" : iv < 0 ? "-" : "";
            if (deg == 1) snprintf(buf, (size_t)len, "Unison");
            else if (deg == 8) snprintf(buf, (size_t)len, "%sOct", sg);
            else if (deg == 15) snprintf(buf, (size_t)len, "%s2 Oct", sg);
            else snprintf(buf, (size_t)len, "%s%d%s", sg, deg, deg == 2 ? "nd" : deg == 3 ? "rd" : "th");
            return;
        }
        if (d.unit[0] && d.mn < 0) snprintf(buf, (size_t)len, "%+d %s", iv, d.unit);
        else if (d.unit[0]) snprintf(buf, (size_t)len, "%d %s", iv, d.unit);
        else snprintf(buf, (size_t)len, "%d", iv);
        return;
    }
    if (d.kind == TIME) {
        if (div >= 0) { snprintf(buf, (size_t)len, "%s", kDivs[div].name); return; }
        if (v <= 0) { snprintf(buf, (size_t)len, "Off"); return; }
    }
    const char *u = d.unit;
    if (!strcmp(u, "Hz") && v >= 1000) { v *= 0.001f; u = "kHz"; }
    else if (!strcmp(u, "ms") && v >= 1000) { v *= 0.001f; u = "s"; }
    float av = std::fabs(v);
    const char *sp = (u[0] == '%' || u[0] == ':' || !u[0]) ? "" : " ";
    if (av >= 100) snprintf(buf, (size_t)len, "%.0f%s%s", v, sp, u);
    else if (av >= 10) snprintf(buf, (size_t)len, "%.1f%s%s", v, sp, u);
    else snprintf(buf, (size_t)len, "%.2f%s%s", v, sp, u);
}

void hz_set_algo(HZEngine *e, int slot, int algo) { e->algoIdx[slot & 1] = algo; }
void hz_set_param(HZEngine *e, int slot, int p, float n) {
    if (p >= 0 && p < 10) e->norm[slot & 1][p] = clampf(n, 0, 1);
}
void hz_load_slot(HZEngine *e, int slot, int algo, const float *norms) {
    e->gen.fetch_add(1, std::memory_order_acq_rel);
    for (int p = 0; p < 10; p++) e->norm[slot & 1][p] = clampf(norms[p], 0, 1);
    e->algoIdx[slot & 1] = algo;
    e->gen.fetch_add(1, std::memory_order_acq_rel);
}
void hz_set_slot_on(HZEngine *e, int slot, int on) { e->slotOn[slot & 1] = on != 0; }
void hz_set_bypass(HZEngine *e, int b) { e->bypass = b != 0; }
void hz_set_mute(HZEngine *e, int m) { e->mute = m != 0; }
void hz_set_parallel(HZEngine *e, int p) { e->parallel = p != 0; }
void hz_set_sync(HZEngine *e, int s) { e->sync = s != 0; }
void hz_set_bpm(HZEngine *e, float bpm) { e->bpm = clampf(bpm, 20, 300); }
void hz_set_in_gain(HZEngine *e, float db) { e->inGain = db2lin(db); }
void hz_set_out_gain(HZEngine *e, float db) { e->outGain = db2lin(db); }
void hz_set_gate(HZEngine *e, float db) { e->gateDb = db; }

void hz_get_meters(HZEngine *e, float *in, float *outL, float *outR, float *cpu) {
    *in = e->mIn.exchange(0);
    *outL = e->mOutL.exchange(0);
    *outR = e->mOutR.exchange(0);
    *cpu = e->cpu.load();
}

float hz_get_visual(HZEngine *e, float *spec, float *wave, int n) {
    if (n > VIS) n = VIS;
    const int N = 4096, F = 2048;
    uint32_t w = e->scW.load(std::memory_order_acquire);
    float *t = e->visBuf.data();
    for (int i = 0; i < N; i++) t[i] = e->scOut[(w - (uint32_t)N + (uint32_t)i) & (SC - 1)];
    // waveform: 2x decimated, triggered on the steepest rising zero crossing
    int need = n * 2, trig = N - need - 1;
    float bestSlope = 0;
    for (int i = N - need - 1; i > N - need - 1500 && i > 1; i--) {
        if (t[i - 1] <= 0 && t[i] > 0 && t[i] - t[i - 1] > bestSlope) { bestSlope = t[i] - t[i - 1]; trig = i; }
    }
    for (int k = 0; k < n; k++) wave[k] = t[trig + 2 * k];
    // spectrum
    float *re = e->re.data(), *im = e->im.data(), rms = 0;
    for (int i = 0; i < F; i++) {
        float x = t[N - F + i];
        rms += x * x;
        re[i] = x * (0.5f - 0.5f * std::cos(2.f * kPi * (float)i / (float)(F - 1)));
        im[i] = 0;
    }
    fft(re, im, F);
    float prev = 40.f * (float)F / e->sr;
    for (int k = 0; k < n; k++) {
        float f = 40.f * std::pow(450.f, (float)(k + 1) / (float)n), pos = std::min(f * (float)F / e->sr, (float)(F / 2 - 2));
        float mx = 0;
        int a = (int)prev, b = std::max(a, (int)pos);
        for (int j = a; j <= b; j++) mx = std::max(mx, re[j] * re[j] + im[j] * im[j]);
        if (b == a) {  // sub-bin resolution at low frequencies: interpolate
            float fr = 0.5f * (prev + pos) - (float)a;
            float m1 = re[a + 1] * re[a + 1] + im[a + 1] * im[a + 1];
            mx = mx + fr * (m1 - mx);
        }
        prev = pos;
        float db = 10.f * std::log10(mx / (float)(F * F / 16) + 1e-12f) + 3.f * std::log2(f / 200.f);
        float v = clampf((db + 78.f) / 72.f, 0, 1);
        e->specState[k] = v > e->specState[k] ? e->specState[k] + 0.6f * (v - e->specState[k]) : e->specState[k] * 0.93f + 0.07f * v;
        spec[k] = e->specState[k];
    }
    return std::sqrt(rms / (float)F);
}

float hz_detect_pitch(HZEngine *e) {
    if (!e->prepared) return 0;
    PitchTracker &t = e->tuner;
    int N = std::min(8192, (t.W + t.maxLag) * t.dec + 512);
    uint32_t w = e->scW.load(std::memory_order_acquire);
    t.reset();
    for (int i = 0; i < N; i++) t.push(e->scIn[(w - (uint32_t)N + (uint32_t)i) & (SC - 1)]);
    t.analyze();
    return t.freq;
}

void hz_process(HZEngine *e, const float *in, float *outL, float *outR, int frames) {
    fesetenv(FE_DFL_DISABLE_DENORMS_ENV);
    for (int done = 0; done < frames;) {
        int n = std::min(BLK, frames - done);
        e->block(in + done, outL + done, outR + done, n);
        done += n;
    }
}

}  // extern "C"
