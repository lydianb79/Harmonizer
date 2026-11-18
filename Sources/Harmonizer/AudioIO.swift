import AVFoundation
import CoreAudio
import Foundation
import HarmonizerDSP

struct AudioDevice: Identifiable, Hashable {
    let id: AudioObjectID
    let uid: String
    let name: String
    let inChannels: Int
    let outChannels: Int
}

/// Thin wrappers over the CoreAudio HAL property API.
enum HAL {
    static let aggregateUID = "ch.harmonizer.aggregate"

    static func addr(_ sel: AudioObjectPropertySelector,
                     _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    }

    static func get<T>(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector,
                       _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, default def: T) -> T {
        var a = addr(sel, scope)
        var size = UInt32(MemoryLayout<T>.size)
        var v = def
        let st = withUnsafeMutablePointer(to: &v) { AudioObjectGetPropertyData(id, &a, 0, nil, &size, $0) }
        return st == noErr ? v : def
    }

    @discardableResult
    static func set<T>(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector,
                       _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, _ value: T) -> OSStatus {
        var a = addr(sel, scope)
        var v = value
        return withUnsafePointer(to: &v) { AudioObjectSetPropertyData(id, &a, 0, nil, UInt32(MemoryLayout<T>.size), $0) }
    }

    static func array<T>(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector,
                         _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, of: T.Type) -> [T] {
        var a = addr(sel, scope)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &a, 0, nil, &size) == noErr, size > 0 else { return [] }
        let count = Int(size) / MemoryLayout<T>.stride
        let buf = UnsafeMutablePointer<T>.allocate(capacity: count)
        defer { buf.deallocate() }
        guard AudioObjectGetPropertyData(id, &a, 0, nil, &size, buf) == noErr else { return [] }
        return Array(UnsafeBufferPointer(start: buf, count: Int(size) / MemoryLayout<T>.stride))
    }

    static func string(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector) -> String {
        var a = addr(sel)
        var ref: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        let st = withUnsafeMutablePointer(to: &ref) { AudioObjectGetPropertyData(id, &a, 0, nil, &size, $0) }
        guard st == noErr, let r = ref else { return "" }
        return r.takeRetainedValue() as String
    }

    static func channels(_ id: AudioObjectID, _ scope: AudioObjectPropertyScope) -> Int {
        var a = addr(kAudioDevicePropertyStreamConfiguration, scope)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &a, 0, nil, &size) == noErr, size > 0 else { return 0 }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { raw.deallocate() }
        guard AudioObjectGetPropertyData(id, &a, 0, nil, &size, raw) == noErr else { return 0 }
        let list = UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self))
        return list.reduce(0) { $0 + Int($1.mNumberChannels) }
    }

    static func devices() -> [AudioDevice] {
        array(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDevices, of: AudioObjectID.self).compactMap { id in
            let uid = string(id, kAudioDevicePropertyDeviceUID)
            if uid.hasPrefix(aggregateUID) { return nil }
            return AudioDevice(id: id, uid: uid, name: string(id, kAudioObjectPropertyName),
                               inChannels: channels(id, kAudioObjectPropertyScopeInput),
                               outChannels: channels(id, kAudioObjectPropertyScopeOutput))
        }
    }

    static func defaultDevice(input: Bool) -> AudioObjectID {
        get(AudioObjectID(kAudioObjectSystemObject),
            input ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice, default: AudioObjectID(0))
    }

    static func sampleRate(_ id: AudioObjectID) -> Double { get(id, kAudioDevicePropertyNominalSampleRate, default: Float64(48000)) }

    static func sampleRates(_ id: AudioObjectID) -> [Double] {
        let ranges = array(id, kAudioDevicePropertyAvailableNominalSampleRates, of: AudioValueRange.self)
        return [44100, 48000, 88200, 96000, 176400, 192000].filter { r in ranges.contains { $0.mMinimum <= r && r <= $0.mMaximum } }
    }

    static func setSampleRate(_ id: AudioObjectID, _ rate: Double) {
        guard abs(sampleRate(id) - rate) > 0.5 else { return }
        set(id, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, Float64(rate))
        for _ in 0..<40 {  // the change is asynchronous
            if abs(sampleRate(id) - rate) < 0.5 { return }
            usleep(25_000)
        }
    }

    static func bufferRange(_ id: AudioObjectID) -> ClosedRange<Int> {
        let r = get(id, kAudioDevicePropertyBufferFrameSizeRange, default: AudioValueRange(mMinimum: 16, mMaximum: 4096))
        return Int(r.mMinimum)...max(Int(r.mMinimum), Int(r.mMaximum))
    }

    /// Device latency + safety offset for one direction, in frames.
    static func latency(_ id: AudioObjectID, _ scope: AudioObjectPropertyScope) -> Int {
        var total = Int(get(id, kAudioDevicePropertyLatency, scope, default: UInt32(0)))
        total += Int(get(id, kAudioDevicePropertySafetyOffset, scope, default: UInt32(0)))
        if let stream = array(id, kAudioDevicePropertyStreams, scope, of: AudioObjectID.self).first {
            total += Int(get(stream, kAudioStreamPropertyLatency, default: UInt32(0)))
        }
        return total
    }

    /// Private aggregate so separate input and output hardware run in ONE IOProc
    /// (no ring buffer between two clocks, the output is drift compensated).
    static func createAggregate(input: AudioDevice, output: AudioDevice) -> AudioObjectID? {
        let desc: [String: Any] = [
            "uid": "\(aggregateUID).\(getpid())",
            "name": "Harmonizer IO",
            "private": 1,
            "stacked": 0,
            "master": input.uid,
            "subdevices": [["uid": input.uid, "drift": 0], ["uid": output.uid, "drift": 1]],
        ]
        var id: AudioObjectID = 0
        guard AudioHardwareCreateAggregateDevice(desc as CFDictionary, &id) == noErr, id != 0 else { return nil }
        // the HAL needs a moment to publish the new device's streams
        for _ in 0..<40 {
            if channels(id, kAudioObjectPropertyScopeInput) >= input.inChannels,
               channels(id, kAudioObjectPropertyScopeOutput) >= input.outChannels + output.outChannels { break }
            usleep(25_000)
        }
        return id
    }
}

/// Owns device selection and the engine's connection to the hardware.
final class AudioManager: ObservableObject {
    let engine: OpaquePointer

    @Published var inputs: [AudioDevice] = []
    @Published var outputs: [AudioDevice] = []
    @Published var inputUID: String { didSet { changed("inputUID", inputUID, oldValue) } }
    @Published var outputUID: String { didSet { changed("outputUID", outputUID, oldValue) } }
    @Published var inputChannel: Int { didSet { changed("inputChannel", inputChannel, oldValue) } }
    @Published var bufferSize: Int { didSet { changed("bufferSize", bufferSize, oldValue) } }
    @Published var sampleRate: Double { didSet { changed("sampleRate", sampleRate, oldValue) } }

    @Published private(set) var bufferOptions: [Int] = [16, 32, 64, 128, 256, 512, 1024]
    @Published private(set) var rateOptions: [Double] = [44100, 48000]
    @Published private(set) var inputChannelCount = 1
    @Published private(set) var isRunning = false
    @Published private(set) var status = "Stopped"
    @Published private(set) var actualBuffer = 0
    @Published private(set) var actualRate: Double = 48000
    @Published private(set) var latencyMs: Double = 0
    @Published private(set) var latencyDetail = ""
    @Published private(set) var micDenied = false

    private var aggregate: AudioObjectID = 0
    private var suppress = false
    private var restartPending = false

    init(engine: OpaquePointer) {
        self.engine = engine
        let d = UserDefaults.standard
        inputUID = d.string(forKey: "inputUID") ?? ""
        outputUID = d.string(forKey: "outputUID") ?? ""
        inputChannel = d.integer(forKey: "inputChannel")
        bufferSize = d.object(forKey: "bufferSize") as? Int ?? 64
        sampleRate = d.object(forKey: "sampleRate") as? Double ?? 48000
        refreshDevices()

        var a = HAL.addr(kAudioHardwarePropertyDevices)
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &a, .main) { [weak self] _, _ in
            self?.devicesChanged()
        }
    }

    private func changed<T: Equatable>(_ key: String, _ new: T, _ old: T) {
        guard new != old, !suppress else { return }
        UserDefaults.standard.set(new, forKey: key)
        scheduleRestart()
    }

    private func scheduleRestart() {
        guard !restartPending else { return }
        restartPending = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.restartPending = false
            self.start()
        }
    }

    /// Choices for the input picker: every channel as mono, then every adjacent pair as stereo.
    var inputOptions: [(tag: Int, label: String)] {
        let n = inputChannelCount
        var out = (0..<n).map { (tag: $0, label: n == 1 ? "Mono" : "Mono: Input \($0 + 1)") }
        if n >= 2 { out += (0..<(n - 1)).map { (tag: 1000 + $0, label: "Stereo: Inputs \($0 + 1) + \($0 + 2)") } }
        return out
    }

    var inputDevice: AudioDevice? { inputs.first { $0.uid == inputUID } }
    var outputDevice: AudioDevice? { outputs.first { $0.uid == outputUID } }

    func refreshDevices() {
        let all = HAL.devices()
        suppress = true
        inputs = all.filter { $0.inChannels > 0 }
        outputs = all.filter { $0.outChannels > 0 }
        if inputDevice == nil {
            let def = HAL.defaultDevice(input: true)
            inputUID = (inputs.first { $0.id == def } ?? inputs.first)?.uid ?? ""
        }
        if outputDevice == nil {
            let def = HAL.defaultDevice(input: false)
            outputUID = (outputs.first { $0.id == def } ?? outputs.first)?.uid ?? ""
        }
        updateOptions()
        suppress = false
    }

    private func updateOptions() {
        inputChannelCount = max(1, inputDevice?.inChannels ?? 1)
        // inputChannel encodes mono channel n as n, and the stereo pair n + (n+1) as 1000 + n
        let base = inputChannel % 1000, needed = inputChannel >= 1000 ? base + 2 : base + 1
        if needed > inputChannelCount { inputChannel = 0 }
        var range = 16...4096
        var rates: [Double] = [44100, 48000, 88200, 96000, 176400, 192000]
        for dev in [inputDevice, outputDevice].compactMap({ $0 }) {
            let r = HAL.bufferRange(dev.id)
            range = max(range.lowerBound, r.lowerBound)...max(max(range.lowerBound, r.lowerBound), min(range.upperBound, r.upperBound))
            let dr = HAL.sampleRates(dev.id)
            rates = rates.filter { dr.contains($0) }
        }
        let opts = [16, 32, 48, 64, 96, 128, 192, 256, 512, 1024, 2048].filter { range.contains($0) }
        bufferOptions = opts.isEmpty ? [range.lowerBound] : opts
        if rates.isEmpty { rates = [outputDevice.map { HAL.sampleRate($0.id) } ?? 48000] }
        rateOptions = rates
        if !bufferOptions.contains(bufferSize) {
            bufferSize = bufferOptions.min { abs($0 - bufferSize) < abs($1 - bufferSize) } ?? bufferOptions[0]
        }
        if !rateOptions.contains(sampleRate) {
            sampleRate = rateOptions.contains(48000) ? 48000 : rateOptions[0]
        }
    }

    private func devicesChanged() {
        let before = (inputUID, outputUID)
        let hadIn = inputDevice?.id, hadOut = outputDevice?.id
        refreshDevices()
        // restart only when the hardware we use went away or changed identity
        if isRunning, before.0 != inputUID || before.1 != outputUID || hadIn != inputDevice?.id || hadOut != outputDevice?.id {
            scheduleRestart()
        }
    }

    func start() {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .notDetermined:
            status = "Waiting for microphone permission…"
            AVCaptureDevice.requestAccess(for: .audio) { [weak self] _ in DispatchQueue.main.async { self?.start() } }
            return
        case .denied, .restricted:
            stop()
            micDenied = true
            status = "Audio input access denied. Enable it in System Settings › Privacy & Security › Microphone."
            return
        default:
            micDenied = false
        }

        stop()
        suppress = true
        updateOptions()
        suppress = false
        guard let input = inputDevice, let output = outputDevice else {
            status = "No audio device available"
            return
        }

        HAL.setSampleRate(input.id, sampleRate)
        HAL.setSampleRate(output.id, sampleRate)

        var ioDevice = input.id
        var outOffset = 0
        if input.id != output.id {
            guard let agg = HAL.createAggregate(input: input, output: output) else {
                status = "Could not combine \(input.name) and \(output.name)"
                return
            }
            aggregate = agg
            ioDevice = agg
            outOffset = input.outChannels
        }

        HAL.set(ioDevice, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, UInt32(bufferSize))
        actualBuffer = Int(HAL.get(ioDevice, kAudioDevicePropertyBufferFrameSize, default: UInt32(bufferSize)))
        actualRate = HAL.sampleRate(ioDevice)

        hz_prepare(engine, actualRate)
        let stereo = inputChannel >= 1000, inLeft = inputChannel % 1000
        hz_set_io(engine, Int32(inLeft), Int32(stereo ? inLeft + 1 : -1), Int32(outOffset), Int32(outOffset + (output.outChannels > 1 ? 1 : 0)))
        let st = hz_start(engine, ioDevice)
        guard st == 0 else {
            destroyAggregate()
            status = "Could not start audio (error \(st))"
            return
        }

        let inLat = HAL.latency(input.id, kAudioObjectPropertyScopeInput)
        let outLat = HAL.latency(output.id, kAudioObjectPropertyScopeOutput)
        let frames = inLat + outLat + 2 * actualBuffer
        latencyMs = Double(frames) / actualRate * 1000
        latencyDetail = "input \(inLat) + output \(outLat) + 2 × \(actualBuffer) buffer = \(frames) frames"
        isRunning = true
        status = "\(input.name)\(stereo ? " (stereo)" : "") → \(output.name)"
    }

    func markDemo() { status = "Demo signal (no audio device in use)" }

    func stop() {
        hz_stop(engine)
        destroyAggregate()
        isRunning = false
        status = "Stopped"
    }

    private func destroyAggregate() {
        if aggregate != 0 {
            AudioHardwareDestroyAggregateDevice(aggregate)
            aggregate = 0
        }
    }
}
