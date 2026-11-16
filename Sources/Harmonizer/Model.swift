import CoreMIDI
import HarmonizerDSP
import SwiftUI

struct AlgoParam {
    let name: String
    let def: Float
}

struct AlgoDesc: Identifiable {
    let id: Int
    let name: String
    let category: String
    let blurb: String
    let params: [AlgoParam]
}

struct SlotState: Codable, Equatable {
    var algo: String
    var on: Bool
    var values: [Float]
    var hotMin: [Float?] = Array(repeating: nil, count: 10)
    var hotMax: [Float?] = Array(repeating: nil, count: 10)
    var hotSw: [Float?]? = nil  // HotSwitch alternate values (optional so older saved states still load)
}

struct Preset: Codable, Identifiable, Equatable {
    var id: String { name }
    var name: String
    var a: SlotState
    var b: SlotState
    var parallel = false
    var bpm: Double = 120
    var sync = false
    var factory = false
}

/// Fast-changing values, kept separate so only the meters redraw at 30 Hz.
final class Meters: ObservableObject {
    @Published var input: Float = 0
    @Published var outL: Float = 0
    @Published var outR: Float = 0
    @Published var cpu: Float = 0
    @Published var tunerHz: Float = 0
}

final class EngineModel: ObservableObject {
    let engine: OpaquePointer
    let algos: [AlgoDesc]
    let meters = Meters()
    private let index: [String: Int]

    @Published var slots: [SlotState] = []
    @Published var parallel = false { didSet { hz_set_parallel(engine, parallel ? 1 : 0) } }
    @Published var active = true { didSet { hz_set_bypass(engine, active ? 0 : 1) } }
    @Published var sync = false { didSet { hz_set_sync(engine, sync ? 1 : 0) } }
    @Published var bpm: Double = 120 { didSet { hz_set_bpm(engine, Float(bpm)) } }
    @Published var inGain: Float = 0 { didSet { hz_set_in_gain(engine, inGain) } }
    @Published var outGain: Float = 0 { didSet { hz_set_out_gain(engine, outGain) } }
    @Published var gate: Float = -100 { didSet { hz_set_gate(engine, gate) } }
    @Published var hot: Float = 0 { didSet { applyHot() } }
    @Published var hotSwitch = false { didSet { if hotSwitch != oldValue { applySwitch() } } }
    private var switchBase: [[Float?]] = Array(repeating: Array(repeating: nil, count: 10), count: 2)
    @Published var tuner = false { didSet { hz_set_mute(engine, tuner && tunerMutes ? 1 : 0) } }
    @Published var tunerMutes = true { didSet { hz_set_mute(engine, tuner && tunerMutes ? 1 : 0) } }
    @Published var themeName: String = "Midnight" {
        didSet {
            Theme.current = Theme.named(themeName)
            UserDefaults.standard.set(themeName, forKey: "theme")
        }
    }
    @Published var presets: [Preset] = []
    @Published var presetName = "Init"

    private var taps: [TimeInterval] = []
    private var timer: Timer?
    private var midiClient = MIDIClientRef()
    private var midiPort = MIDIPortRef()

    init(engine: OpaquePointer) {
        self.engine = engine
        let savedTheme = Theme.named(UserDefaults.standard.string(forKey: "theme") ?? "Midnight")
        Theme.current = savedTheme
        var list: [AlgoDesc] = []
        for i in 0..<Int(hz_algo_count()) {
            let a = Int32(i)
            let params = (0..<Int(hz_algo_param_count(a))).map {
                AlgoParam(name: String(cString: hz_param_name(a, Int32($0))), def: hz_param_default(a, Int32($0)))
            }
            list.append(AlgoDesc(id: i, name: String(cString: hz_algo_name(a)), category: String(cString: hz_algo_category(a)),
                                 blurb: String(cString: hz_algo_blurb(a)), params: params))
        }
        algos = list
        index = Dictionary(uniqueKeysWithValues: list.map { ($0.name, $0.id) })
        themeName = savedTheme.name

        presets = factoryPresets() + loadUserPresets()
        if let data = UserDefaults.standard.data(forKey: "state"), let p = try? JSONDecoder().decode(Preset.self, from: data) {
            load(p)
        } else {
            load(presets[0])
        }
        let d = UserDefaults.standard
        inGain = d.float(forKey: "inGain")
        outGain = d.float(forKey: "outGain")
        gate = d.object(forKey: "gate") as? Float ?? -100
        hz_set_in_gain(engine, inGain)
        hz_set_out_gain(engine, outGain)
        hz_set_gate(engine, gate)

        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in self?.poll() }
        setupMIDI()
    }

    // MARK: slots

    func desc(_ slot: Int) -> AlgoDesc { algos[index[slots[slot].algo] ?? 0] }

    func defaults(for algo: AlgoDesc, on: Bool = true) -> SlotState {
        var v = algo.params.map(\.def)
        v += Array(repeating: 0, count: 10 - v.count)
        return SlotState(algo: algo.name, on: on, values: v)
    }

    private func push(_ slot: Int) {
        let s = slots[slot]
        hz_load_slot(engine, Int32(slot), Int32(index[s.algo] ?? 0), s.values)
        hz_set_slot_on(engine, Int32(slot), s.on ? 1 : 0)
    }

    func setAlgo(_ slot: Int, _ algo: AlgoDesc) {
        switchBase[slot] = Array(repeating: nil, count: 10)
        slots[slot] = defaults(for: algo, on: true)
        push(slot)
    }

    func setParam(_ slot: Int, _ p: Int, _ v: Float) {
        let c = min(max(v, 0), 1)
        slots[slot].values[p] = c
        hz_set_param(engine, Int32(slot), Int32(p), c)
    }

    func toggleSlot(_ slot: Int) {
        slots[slot].on.toggle()
        hz_set_slot_on(engine, Int32(slot), slots[slot].on ? 1 : 0)
    }

    func swapSlots() {
        slots.swapAt(0, 1)
        push(0)
        push(1)
    }

    func display(_ slot: Int, _ p: Int) -> String {
        var buf = [CChar](repeating: 0, count: 32)
        hz_param_display(engine, Int32(desc(slot).id), Int32(p), slots[slot].values[p], &buf, 32)
        return String(cString: buf)
    }

    // MARK: hot knob (expression mapping, like heel/toe on a pedal)

    var hotAssigned: Bool {
        slots.contains { s in (0..<10).contains { s.hotMin[$0] != nil && s.hotMax[$0] != nil } }
    }

    private func applyHot() {
        for s in 0..<slots.count {
            for p in 0..<10 {
                if let lo = slots[s].hotMin[p], let hi = slots[s].hotMax[p] { setParam(s, p, lo + (hi - lo) * hot) }
            }
        }
    }

    func setHot(_ slot: Int, _ p: Int, min: Bool) {
        let v = slots[slot].values[p]
        if min {
            slots[slot].hotMin[p] = v
            if slots[slot].hotMax[p] == nil { slots[slot].hotMax[p] = 1 }
        } else {
            slots[slot].hotMax[p] = v
            if slots[slot].hotMin[p] == nil { slots[slot].hotMin[p] = 0 }
        }
    }

    func clearHot(_ slot: Int, _ p: Int) {
        slots[slot].hotMin[p] = nil
        slots[slot].hotMax[p] = nil
    }

    // MARK: hot switch (jump a set of knobs to alternate values and back)

    var switchAssigned: Bool { slots.contains { ($0.hotSw ?? []).contains { $0 != nil } } }

    private func applySwitch() {
        for s in 0..<slots.count {
            for p in 0..<10 {
                if hotSwitch, let alt = slots[s].hotSw?[p] {
                    switchBase[s][p] = slots[s].values[p]
                    setParam(s, p, alt)
                } else if !hotSwitch, let base = switchBase[s][p] {
                    setParam(s, p, base)
                    switchBase[s][p] = nil
                }
            }
        }
    }

    func setSwitch(_ slot: Int, _ p: Int) {
        var sw = slots[slot].hotSw ?? Array(repeating: nil, count: 10)
        sw[p] = slots[slot].values[p]
        slots[slot].hotSw = sw
    }

    func clearSwitch(_ slot: Int, _ p: Int) {
        slots[slot].hotSw?[p] = nil
        switchBase[slot][p] = nil
    }

    // MARK: tempo

    func tap() {
        let now = ProcessInfo.processInfo.systemUptime
        if let last = taps.last, now - last > 2 { taps.removeAll() }
        taps.append(now)
        if taps.count > 5 { taps.removeFirst() }
        if taps.count >= 2 {
            let avg = (taps.last! - taps.first!) / Double(taps.count - 1)
            bpm = (min(max(60 / avg, 30), 300) * 10).rounded() / 10
        }
    }

    // MARK: presets

    func snapshot(name: String) -> Preset {
        Preset(name: name, a: slots[0], b: slots[1], parallel: parallel, bpm: bpm, sync: sync, factory: false)
    }

    private func sanitized(_ s: SlotState) -> SlotState {
        guard let i = index[s.algo] else { return defaults(for: algos[0], on: s.on) }
        var out = s
        let def = defaults(for: algos[i])
        if out.values.count != 10 { out.values = def.values }
        if out.hotMin.count != 10 { out.hotMin = def.hotMin }
        if out.hotMax.count != 10 { out.hotMax = def.hotMax }
        if out.hotSw?.count != 10 { out.hotSw = nil }
        return out
    }

    func load(_ p: Preset) {
        switchBase = Array(repeating: Array(repeating: nil, count: 10), count: 2)
        hotSwitch = false
        slots = [sanitized(p.a), sanitized(p.b)]
        parallel = p.parallel
        bpm = p.bpm
        sync = p.sync
        presetName = p.name
        push(0)
        push(1)
        if hotAssigned { applyHot() }
    }

    func stepPreset(_ delta: Int) {
        guard !presets.isEmpty else { return }
        let i = presets.firstIndex { $0.name == presetName } ?? 0
        load(presets[(i + delta + presets.count) % presets.count])
    }

    func save(as name: String) {
        let n = name.trimmingCharacters(in: .whitespaces)
        guard !n.isEmpty, !presets.contains(where: { $0.factory && $0.name == n }) else { return }
        let p = snapshot(name: n)
        if let i = presets.firstIndex(where: { $0.name == n }) { presets[i] = p } else { presets.append(p) }
        presetName = n
        storeUserPresets()
    }

    func deleteCurrent() {
        guard let i = presets.firstIndex(where: { $0.name == presetName && !$0.factory }) else { return }
        presets.remove(at: i)
        storeUserPresets()
        load(presets[min(i, presets.count - 1)])
    }

    var currentIsUser: Bool { presets.contains { $0.name == presetName && !$0.factory } }

    private var presetURL: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("Harmonizer")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("presets.json")
    }

    private func loadUserPresets() -> [Preset] {
        guard let d = try? Data(contentsOf: presetURL), let p = try? JSONDecoder().decode([Preset].self, from: d) else { return [] }
        return p
    }

    private func storeUserPresets() {
        if let d = try? JSONEncoder().encode(presets.filter { !$0.factory }) { try? d.write(to: presetURL, options: .atomic) }
    }

    func persist() {
        if let d = try? JSONEncoder().encode(snapshot(name: presetName)) { UserDefaults.standard.set(d, forKey: "state") }
        UserDefaults.standard.set(inGain, forKey: "inGain")
        UserDefaults.standard.set(outGain, forKey: "outGain")
        UserDefaults.standard.set(gate, forKey: "gate")
    }

    private func slot(_ algo: String, _ over: [String: Float] = [:], on: Bool = true) -> SlotState {
        let d = algos[index[algo] ?? 0]
        var s = defaults(for: d, on: on)
        for (k, v) in over {
            if let p = d.params.firstIndex(where: { $0.name == k }) {
                s.values[p] = hz_param_normalize(Int32(d.id), Int32(p), v)
            } else {
                assertionFailure("unknown parameter \(k) in \(algo)")
            }
        }
        return s
    }

    private func factoryPresets() -> [Preset] {
        func p(_ name: String, _ a: SlotState, _ b: SlotState, parallel: Bool = false) -> Preset {
            Preset(name: name, a: a, b: b, parallel: parallel, bpm: 120, sync: false, factory: true)
        }
        var flex = slot("Pitch Flex")
        flex.hotMin[1] = 0
        flex.hotMax[1] = 1
        var polyFlex = slot("Poly Flex", ["Heel": 0, "Toe": -12])
        polyFlex.hotMin[1] = 0
        polyFlex.hotMax[1] = 1
        return [
            p("Wide Detune", slot("Micro Shift"), slot("Plate", ["Mix": 18, "Decay": 1.2])),
            p("Twin Leads", slot("Diatonic", ["Interval A": 2, "Interval B": -3, "Mix": 55]), slot("Digital Delay", ["Mix": 25, "Time A": 380, "Time B": 380])),
            p("Guitar Choir", slot("Quad Voice", ["Detune": 10, "Spread": 30]), slot("Hall", ["Mix": 30])),
            p("Glass Cathedral", slot("Crystals"), slot("Void", ["Mix": 35, "Gravity": 9])),
            p("Octave Fuzz", slot("Octaver", ["Sub 1": 70, "Octave Up": 45, "Grit": 40, "Mix": 60]), slot("Crush", ["Type": 2, "Drive": 65, "Level": -6])),
            p("Whammy Dive", flex, slot("Room", ["Mix": 15])),
            p("Shimmer Pad", slot("EQ Compressor", ["Threshold": -30, "Ratio": 6, "Makeup": 8]), slot("Shimmer", ["Mix": 60, "Decay": 9])),
            p("Tape Dream", slot("Tape Echo", ["Mix": 40, "Feedback": 55]), slot("Tri Chorus", ["Mix": 35, "Rate": 0.35])),
            p("Stair Climber", slot("Chromatic", ["Pitch A": 12, "Pitch B": 7, "Delay A": 250, "Delay B": 375, "Feedback": 60, "Mix": 40]), slot("Plate", ["Mix": 22])),
            p("Arp Machine", slot("Arp Shift"), slot("Digital Delay", ["Mix": 30, "Time A": 250, "Time B": 375, "Cross": 100])),
            p("Backwards Swell", slot("Reverse", ["Mix": 75]), slot("Hall", ["Mix": 40, "Decay": 4.5])),
            p("Tap Cloud", slot("Multi Tap", ["Taps": 24, "Length": 1400, "Slurm": 70, "Taper": -60]), slot("Phaser", ["Mix": 35, "Rate": 0.15])),
            p("Leslie Cabinet", slot("Crush", ["Drive": 25, "Level": -2]), slot("Rotary", ["Speed": 1])),
            p("Funk Machine", slot("Auto Wah"), slot("EQ Compressor")),
            p("Sympathetic Strings", slot("Resonator"), slot("Room", ["Mix": 25]), parallel: true),
            p("Chord Octaves", slot("Poly Shift", ["Mix": 45]), slot("Hall", ["Mix": 25])),
            p("Twelve String", slot("Poly Shift", ["Pitch A": 12, "Balance": 0, "Fine A": 6, "Mix": 35, "Delay A": 15]), slot("Tri Chorus", ["Mix": 25])),
            p("Chord Dive", polyFlex, slot("Plate", ["Mix": 18])),
            p("Synth Lead", slot("Saw Synth"), slot("Digital Delay", ["Mix": 28])),
            p("Grain Drift", slot("Grain Cloud", ["Pitch": 12, "Reverse": 40, "Spray": 60]), slot("Void", ["Mix": 35])),
            p("Glitch Repeater", slot("Stutter", ["Pitch Step": -2, "Decay": 25]), slot("Room", ["Mix": 15])),
            p("Loop Station", slot("Looper"), slot("Hall", ["Mix": 22])),
            p("Vibe Machine", slot("Uni Vibe"), slot("Crush", ["Drive": 30, "Level": -3], on: false)),
            p("Prism Arp", slot("Prism"), slot("Mod Echo Verb", ["Mix": 30])),
            p("Chord Synth", slot("Poly Synth"), slot("Hall", ["Mix": 25])),
            p("Surf Tank", slot("Harmonic Trem", ["Depth": 60]), slot("Spring")),
            p("Space Station", slot("Space Time"), slot("Wormhole", ["Mix": 30], on: false)),
            p("Metal Rhythm", slot("Aggro"), slot("Studio Room", ["Mix": 15])),
            p("Broken Radio", slot("Glitch"), slot("Vintage Delay", ["Mix": 25])),
            p("Echo Heads", slot("Multi Head", ["Head 3": 50]), slot("Bucket Delay", ["Mix": 20], on: false)),
            p("Backwards Bloom", slot("Reverse Verb"), slot("Dyna Verb", ["Mode": 2, "Mix": 35])),
            p("Frozen Void", slot("Micro Shift", ["Mix": 40]), slot("Void", ["Mix": 60, "Gravity": 40, "Size": 100])),
        ]
    }

    // MARK: metering + tuner

    private func poll() {
        var i: Float = 0, l: Float = 0, r: Float = 0, c: Float = 0
        hz_get_meters(engine, &i, &l, &r, &c)
        func ballistics(_ old: Float, _ new: Float) -> Float { new > old ? new : max(new, old * 0.86) }
        meters.input = ballistics(meters.input, i)
        meters.outL = ballistics(meters.outL, l)
        meters.outR = ballistics(meters.outR, r)
        meters.cpu = c
        if tuner {
            let hz = hz_detect_pitch(engine)
            if hz > 0 {
                let prev = meters.tunerHz
                // smooth while the reading stays on the same note, jump otherwise
                meters.tunerHz = prev > 0 && abs(log2(hz / prev)) < 0.03 ? prev * pow(hz / prev, 0.35) : hz
            } else {
                meters.tunerHz = 0
            }
        }
    }

    // MARK: MIDI (CC 1 / CC 11 -> Hot Knob, CC 65 -> HotSwitch, Program Change -> preset, CC 64 -> active)

    private func setupMIDI() {
        MIDIClientCreateWithBlock("Harmonizer" as CFString, &midiClient) { [weak self] _ in
            DispatchQueue.main.async { self?.connectMIDISources() }
        }
        MIDIInputPortCreateWithProtocol(midiClient, "In" as CFString, ._1_0, &midiPort) { [weak self] list, _ in
            for packet in list.unsafeSequence() {
                var pkt = packet.pointee
                let count = Int(pkt.wordCount)
                withUnsafeBytes(of: &pkt.words) { raw in
                    let w = raw.bindMemory(to: UInt32.self)
                    for k in 0..<min(count, w.count) where (w[k] >> 28) == 0x2 {
                        let status = UInt8((w[k] >> 16) & 0xF0), d1 = UInt8((w[k] >> 8) & 0x7F), d2 = UInt8(w[k] & 0x7F)
                        DispatchQueue.main.async { self?.handleMIDI(status, d1, d2) }
                    }
                }
            }
        }
        connectMIDISources()
    }

    private func connectMIDISources() {
        for i in 0..<MIDIGetNumberOfSources() { MIDIPortConnectSource(midiPort, MIDIGetSource(i), nil) }
    }

    private func handleMIDI(_ status: UInt8, _ d1: UInt8, _ d2: UInt8) {
        switch status {
        case 0xB0 where d1 == 1 || d1 == 11: hot = Float(d2) / 127
        case 0xB0 where d1 == 64: active = d2 >= 64
        case 0xB0 where d1 == 65: hotSwitch = d2 >= 64
        case 0xC0: if Int(d1) < presets.count { load(presets[Int(d1)]) }
        default: break
        }
    }
}
