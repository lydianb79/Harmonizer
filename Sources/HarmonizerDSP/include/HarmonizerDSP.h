#ifndef HARMONIZER_DSP_H
#define HARMONIZER_DSP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HZEngine HZEngine;

// Lifecycle
HZEngine *hz_create(void);
void hz_destroy(HZEngine *e);
// Allocate everything for a sample rate. Must only be called while stopped.
void hz_prepare(HZEngine *e, double sampleRate);
// Install the IOProc on a CoreAudio device and start it. Returns an OSStatus.
int hz_start(HZEngine *e, unsigned int deviceID);
void hz_stop(HZEngine *e);
// Flat channel indices on the IO device (across all of its streams).
void hz_set_io(HZEngine *e, int inChannel, int outLeft, int outRight);

// Algorithm catalogue (static)
int hz_algo_count(void);
const char *hz_algo_name(int algo);
const char *hz_algo_category(int algo);
const char *hz_algo_blurb(int algo);
int hz_algo_param_count(int algo);
const char *hz_param_name(int algo, int param);
float hz_param_default(int algo, int param);             // normalised 0..1
float hz_param_normalize(int algo, int param, float v);  // real value -> 0..1
void hz_param_display(HZEngine *e, int algo, int param, float norm, char *buf, int len);

// Controls (thread safe, lock free)
void hz_set_algo(HZEngine *e, int slot, int algo);
void hz_set_param(HZEngine *e, int slot, int param, float norm);
// Switch algorithm and all 10 parameters as one atomic change (presets, algorithm picker).
void hz_load_slot(HZEngine *e, int slot, int algo, const float *norms);
void hz_set_slot_on(HZEngine *e, int slot, int on);
void hz_set_bypass(HZEngine *e, int bypass);
void hz_set_mute(HZEngine *e, int mute);
void hz_set_parallel(HZEngine *e, int parallel);
void hz_set_sync(HZEngine *e, int sync);
void hz_set_bpm(HZEngine *e, float bpm);
void hz_set_in_gain(HZEngine *e, float db);
void hz_set_out_gain(HZEngine *e, float db);
void hz_set_gate(HZEngine *e, float thresholdDb);  // <= -90 disables

// Metering / visuals (call from UI or render thread)
void hz_get_meters(HZEngine *e, float *in, float *outL, float *outR, float *cpu);
// Fills n-point log-frequency spectrum (0..1) and triggered waveform (-1..1). Returns output RMS.
float hz_get_visual(HZEngine *e, float *spectrum, float *wave, int n);
// Tuner: fundamental of the input in Hz, 0 when nothing is detected.
float hz_detect_pitch(HZEngine *e);

// Offline processing (tests): mono in, stereo out.
void hz_process(HZEngine *e, const float *in, float *outL, float *outR, int frames);

#ifdef __cplusplus
}
#endif
#endif
