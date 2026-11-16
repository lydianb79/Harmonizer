// Offline check: runs every algorithm on a synthetic guitar-like signal,
// verifies the output is finite and bounded, and reports realtime CPU cost.
#include "HarmonizerDSP.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    const double sr = 48000;
    const int frames = (int)sr * 4, blk = 32;
    std::vector<float> in((size_t)frames), l((size_t)frames), r((size_t)frames);
    // plucked notes: decaying harmonic series, new note every 0.5 s, then silence for the tail
    const double notes[] = {82.41, 110.0, 146.83, 196.0, 246.94, 329.63};
    for (int i = 0; i < frames; i++) {
        double t = (double)i / sr;
        int k = (int)(t * 2.0);
        double f = notes[k % 6], tn = t - k * 0.5, s = 0;
        if (t < 3.0)
            for (int h = 1; h <= 8; h++) s += std::sin(2 * M_PI * f * h * tn) / h * std::exp(-tn * (2.0 + h));
        in[(size_t)i] = (float)(0.5 * s);
    }

    HZEngine *e = hz_create();
    hz_prepare(e, sr);
    hz_set_slot_on(e, 1, 0);
    int fails = 0, n = hz_algo_count();
    double worst = 0;
    printf("%-16s %-11s %8s %8s %8s\n", "algorithm", "category", "peak", "rms", "cpu%");
    for (int a = 0; a < n; a++) {
        for (int p = 0; p < hz_algo_param_count(a); p++) hz_set_param(e, 0, p, hz_param_default(a, p));
        hz_set_algo(e, 0, a);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; i += blk) hz_process(e, &in[(size_t)i], &l[(size_t)i], &r[(size_t)i], blk);
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double peak = 0, sum = 0;
        bool bad = false;
        for (int i = 0; i < frames; i++) {
            float x = l[(size_t)i], y = r[(size_t)i];
            if (!std::isfinite(x) || !std::isfinite(y)) bad = true;
            peak = std::fmax(peak, std::fmax(std::fabs(x), std::fabs(y)));
            sum += x * x + y * y;
        }
        double rms = std::sqrt(sum / (2.0 * frames)), cpu = el / 4.0 * 100.0;
        worst = std::fmax(worst, cpu);
        bool fail = bad || peak > 1.0001 || rms < 1e-4;
        if (fail) fails++;
        printf("%-16s %-11s %8.3f %8.4f %8.2f %s\n", hz_algo_name(a), hz_algo_category(a), peak, rms, cpu, fail ? "FAIL" : "ok");
    }
    // extremes: every parameter at 0 and at 1 must stay finite
    for (int a = 0; a < n; a++) {
        for (int ext = 0; ext < 2; ext++) {
            for (int p = 1; p < hz_algo_param_count(a); p++) hz_set_param(e, 0, p, (float)ext);
            hz_set_param(e, 0, 0, 1.f);
            hz_set_algo(e, 0, (a + 1) % n);
            hz_process(e, &in[0], &l[0], &r[0], 256);
            hz_set_algo(e, 0, a);
            for (int i = 0; i < frames; i += blk) hz_process(e, &in[(size_t)i], &l[(size_t)i], &r[(size_t)i], blk);
            for (int i = 0; i < frames; i++)
                if (!std::isfinite(l[(size_t)i]) || !std::isfinite(r[(size_t)i]) || std::fabs(l[(size_t)i]) > 1.0001f) {
                    printf("EXTREME FAIL %s at %d (params=%d)\n", hz_algo_name(a), i, ext);
                    fails++;
                    break;
                }
        }
    }
    // pitch accuracy: 220 Hz sine through each shifter at +12 st must come out at 440 Hz
    for (int a = 0; a < n; a++) {
        std::string name = hz_algo_name(a);
        if (name != "Poly Shift" && name != "Chromatic") continue;
        for (int p = 0; p < hz_algo_param_count(a); p++) hz_set_param(e, 0, p, hz_param_default(a, p));
        hz_set_param(e, 0, 0, 1.f);
        hz_set_param(e, 0, 1, hz_param_normalize(a, 1, 12));
        hz_set_param(e, 0, 8, 0.f);  // balance: voice A only
        hz_set_algo(e, 0, (a + 1) % n);
        hz_process(e, &in[0], &l[0], &r[0], 256);
        hz_set_algo(e, 0, a);
        std::vector<float> sine((size_t)frames);
        for (int i = 0; i < frames; i++) sine[(size_t)i] = 0.4f * (float)std::sin(2 * M_PI * 220.0 * i / sr);
        for (int i = 0; i < frames; i += blk) hz_process(e, &sine[(size_t)i], &l[(size_t)i], &r[(size_t)i], blk);
        int zc = 0;
        double pk = 0;
        for (int i = frames - (int)sr; i < frames; i++) {
            if (l[(size_t)i - 1] <= 0 && l[(size_t)i] > 0) zc++;
            pk = std::fmax(pk, std::fabs(l[(size_t)i]));
        }
        bool ok = std::abs(zc - 440) <= 3 && pk > 0.1;
        if (!ok) fails++;
        printf("pitch check %-12s 220 Hz +12 st -> %d Hz, peak %.2f %s\n", name.c_str(), zc, pk, ok ? "ok" : "FAIL");
    }
    // spillover: after replacing a reverb with a tail-less effect, silence in must still give the old tail out
    {
        auto find = [&](const char *nm) { for (int a = 0; a < n; a++) if (std::string(hz_algo_name(a)) == nm) return a; return 0; };
        auto load = [&](int a) {
            float v[10] = {0};
            for (int p = 0; p < hz_algo_param_count(a); p++) v[p] = hz_param_default(a, p);
            hz_load_slot(e, 0, a, v);
        };
        std::vector<float> zero((size_t)frames, 0.f);
        auto tail = [&](const char *from) {
            load(find(from));
            hz_process(e, &in[0], &l[0], &r[0], (int)sr);         // one second of playing
            load(find("Tremolo Pan"));
            hz_process(e, &zero[0], &l[0], &r[0], (int)sr);       // then silence through the new effect
            double sum = 0;
            for (int i = (int)sr / 4; i < (int)sr / 2; i++) sum += l[(size_t)i] * l[(size_t)i];
            hz_process(e, &zero[0], &l[0], &r[0], frames);        // let everything die away
            hz_process(e, &zero[0], &l[0], &r[0], frames);
            hz_process(e, &zero[0], &l[0], &r[0], frames);
            return std::sqrt(sum / (sr / 4));
        };
        double hall = tail("Hall"), dry = tail("EQ Compressor");
        bool ok = hall > 1e-3 && dry < 1e-5;
        if (!ok) fails++;
        printf("spillover: Hall tail rms %.5f, tail-less effect %.6f %s\n", hall, dry, ok ? "ok" : "FAIL");
    }
    printf("%d algorithms\n", n);
    printf("\nworst single-slot load: %.2f%% of realtime at 32-frame blocks\n%s\n", worst, fails ? "FAILURES" : "ALL OK");
    hz_destroy(e);
    return fails ? 1 : 0;
}
