# Harmonizer

A native macOS multi-effects processor for guitar, built for low latency. Two effect slots, 68 algorithms, a Metal-rendered display, and direct CoreAudio I/O with selectable input, output and buffer size.

The feature set is modelled on harmonizer-style hardware multi-effects pedals. All algorithms are original implementations with their own names; this project is not affiliated with or endorsed by any pedal manufacturer and contains none of their code or presets.

> **Status:** every algorithm passes an offline test (finite, bounded output; pitch accuracy; spillover), and the UI has been checked in screenshots. The live audio path has not yet been tested against real hardware.

## Features

- **Two effect slots (A and B)** in series or parallel, swappable, each with up to ten knobs.
- **68 algorithms** in nine categories (list below).
- **Polyphonic pitch shifting** (phase vocoder) for chords, alongside low-latency time-domain shifters for single notes.
- **Hot Knob:** right-click any knob to set its minimum and maximum, then sweep all assigned knobs with one control.
- **HotSwitch:** right-click a knob to store an alternate value; one button flips all assigned knobs and back.
- **Spillover:** when you change preset or algorithm, the previous effect's tail rings out (up to 8 seconds).
- **Presets:** 62 factory presets plus your own.
- **Tap tempo** with tempo sync for delay times, a **tuner** (mutes output by default), a **noise gate**, and input/output gain with meters.
- **Looper** with record, play, overdub, half/double speed and reverse (up to 60 seconds).
- **MIDI:** CC 1 / CC 11 → Hot Knob, CC 65 → HotSwitch, CC 64 → active/bypass, Program Change → preset.
- **Six themes:** Midnight, Synthwave, Ember, Emerald, Graphite and Porcelain (light).

## Download

Each [release](https://github.com/lydianb79/Harmonizer/releases) has one zip per architecture:

- `Harmonizer-<version>-macOS-arm64.zip` for Apple silicon Macs
- `Harmonizer-<version>-macOS-x86_64.zip` for Intel Macs

The app is ad-hoc signed and not notarized, so macOS blocks it on first launch. Right-click the app and choose Open, or allow it in System Settings › Privacy & Security.

## Requirements

- macOS 13 or later
- Xcode command line tools (Swift 5.9 or later) to build

## Build and run

```bash
./build.sh run
```

This builds `build/Harmonizer.app` and opens it. On first launch macOS asks for microphone access, which is required for any audio input (including an audio interface).

The app is ad-hoc signed, so macOS may ask for microphone permission again after each rebuild.

To build for a specific architecture, or both release zips:

```bash
ARCH=x86_64 ./build.sh
```

```bash
./release.sh
```

`ARCH` builds go to `build/<arch>/Harmonizer.app`; `release.sh` writes one zip per architecture into `build/`.

## Audio setup and latency

Open **Settings** (gear icon) to choose the input device, input channel, output device, sample rate and buffer size. The dialog shows the resulting round-trip latency.

- All processing is C++ running directly in the CoreAudio device callback (IOProc); nothing Swift runs on the audio thread.
- If input and output are **different devices**, they are combined into one private, drift-compensated aggregate device. A single audio interface for both gives the lowest latency.
- **Time-domain pitch effects:** detune adds only a few samples of delay. Interval shifts add a variable delay of up to one grain (28–50 ms) per octave of upward shift. The diatonic harmonies need about 40 ms to recognise a new note.
- **Polyphonic pitch effects** (Poly Shift, Poly Flex, Prism, Formant Shift, Shimmer's tail) add a fixed delay of about 37 ms at 44.1/48 kHz.
- The heaviest algorithm uses about 3% of one core at 32-frame blocks (Apple silicon, offline measurement).

## Controls

| Action | How |
|---|---|
| Turn a knob | Drag vertically; hold Shift for fine adjustment |
| Reset a knob | Double-click |
| Assign to Hot Knob or HotSwitch | Right-click the knob |
| Bypass | Space |
| Tap tempo | T |
| Tuner | U |
| HotSwitch | H |
| Previous / next preset | ⌘[ / ⌘] |
| Swap slots A and B | ⇧⌘X |

## Algorithms

| Category | Algorithms |
|---|---|
| **Pitch** | Micro Shift, Diatonic, Quad Voice, Chromatic, Octaver, Crystals, Pitch Flex, Arp Shift, Poly Shift, Poly Flex, Mod Shift, Prism, Pitch Fuzz, Formant Shift |
| **Delay** | Digital Delay, Tape Echo, Reverse, Multi Tap, Vintage Delay, Band Delay, Filter Pong, Multi Head, Bucket Delay, Mod Delay, Ducked Delay, Drive Tape |
| **Modulation** | Tri Chorus, Flanger, Phaser, Rotary, Tremolo Pan, Ring Mod, Auto Wah, Uni Vibe, Mod Filter, Vibrato, Undulator, Harmonic Trem, Vintage Phaser, Tape Flanger |
| **Reverb** | Room, Hall, Plate, Void, Shimmer, Spring, Dual Verb, Reverse Verb, Mod Echo Verb, Mangled Verb, Tremolo Verb, Dyna Verb, Wormhole, Studio Room |
| **Tone** | EQ Compressor, Crush, Resonator, Sculpt, Mid Drive, Aggro |
| **Synth** | Saw Synth (single notes), Poly Synth (chords, up to six voices) |
| **Granular** | Grain Cloud, Stutter, Glitch, Grain Mod |
| **Multi** | Space Time |
| **Looper** | Looper |

Mod Delay, Ducked Delay, Drive Tape and Grain Mod are voicings of Digital Delay, Tape Echo and Grain Cloud with different defaults.

## Known limitations

- Live audio has not been tested on hardware yet.
- Diatonic and Quad Voice harmonies, and Saw Synth, track single notes only.
- Poly Synth's chord detection is a heuristic (spectral peaks with harmonic rejection) and has only been run on single notes.
- The looper's loop is lost when its slot's algorithm changes or a preset loads.
- Changing theme rebuilds the window, so it flickers briefly.

## Project layout

| Path | Contents |
|---|---|
| `Sources/HarmonizerDSP/dsp.hpp` | DSP building blocks: delays, filters, pitch shifters, pitch tracker, reverb core |
| `Sources/HarmonizerDSP/algos.cpp`, `algos2.cpp` | The algorithms and their parameter descriptions |
| `Sources/HarmonizerDSP/engine.cpp` | Engine, lock-free control path, spillover, CoreAudio IOProc, C API |
| `Sources/Harmonizer/AudioIO.swift` | Device discovery, aggregate device, sample rate and buffer handling |
| `Sources/Harmonizer/Model.swift` | Slots, presets, Hot Knob, HotSwitch, tempo, tuner, MIDI |
| `Sources/Harmonizer/MetalView.swift` | Metal renderer and shaders for the display and background |
| `Sources/Harmonizer/UI.swift`, `Theme.swift` | SwiftUI interface and themes |
| `Sources/hzbench/main.cpp` | Offline test and benchmark |
| `Tools/make_icon.swift` | Generates the app icon |

## Testing

```bash
swift build -c release --product hzbench && "$(swift build -c release --show-bin-path)/hzbench"
```

Runs every algorithm on a synthetic plucked-guitar signal, with default and extreme parameter values, and checks pitch accuracy and spillover. It exits non-zero on any failure.

## Regenerating the icon

```bash
swift Tools/make_icon.swift /tmp/AppIcon.iconset && iconutil -c icns /tmp/AppIcon.iconset -o Resources/AppIcon.icns
```

## License

Harmonizer is free software, released under the [GNU General Public License v3.0](LICENSE). You may use, modify and redistribute it under the terms of that licence; it comes with no warranty.
