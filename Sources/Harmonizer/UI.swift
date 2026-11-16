import SwiftUI

// MARK: - building blocks

struct Panel: ViewModifier {
    var accent: Color = .white
    var strength: Double = 0.10
    func body(content: Content) -> some View {
        content
            .background(
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .fill(Theme.current.panel)
                    .overlay(
                        RoundedRectangle(cornerRadius: 16, style: .continuous)
                            .strokeBorder(LinearGradient(colors: [accent.opacity(strength * 3), Theme.ink.opacity(0.05), accent.opacity(strength)],
                                                         startPoint: .topLeading, endPoint: .bottomTrailing), lineWidth: 1))
                    .shadow(color: .black.opacity(Theme.current.scheme == .dark ? 0.5 : 0.12), radius: 18, y: 8))
    }
}

extension View {
    func panel(_ accent: Color = .white, strength: Double = 0.10) -> some View { modifier(Panel(accent: accent, strength: strength)) }
    func label() -> some View { font(.system(size: 10, weight: .semibold, design: .rounded)).tracking(1.2).foregroundStyle(Theme.ink.opacity(0.5)) }
}

/// Rotary control. Drag vertically (hold Shift for fine), double-click to reset.
struct Knob: View {
    let value: Float
    let title: String
    let text: String
    var accent: Color = .cyan
    var size: CGFloat = 58
    var hotRange: (Float, Float)? = nil
    let onChange: (Float) -> Void
    let onReset: () -> Void

    @State private var startValue: Float?
    @State private var hovering = false

    var body: some View {
        VStack(spacing: 5) {
            ZStack {
                Circle().trim(from: 0, to: 0.75).rotation(.degrees(135))
                    .stroke(Theme.ink.opacity(0.09), style: StrokeStyle(lineWidth: 3.5, lineCap: .round))
                if let h = hotRange {
                    Circle().trim(from: CGFloat(min(h.0, h.1)) * 0.75, to: CGFloat(max(h.0, h.1)) * 0.75).rotation(.degrees(135))
                        .stroke(Theme.current.hot.opacity(0.9), style: StrokeStyle(lineWidth: 1.5, lineCap: .round))
                        .padding(-4)
                }
                Circle().trim(from: 0, to: CGFloat(value) * 0.75).rotation(.degrees(135))
                    .stroke(accent, style: StrokeStyle(lineWidth: 3.5, lineCap: .round))
                    .shadow(color: accent.opacity(0.9), radius: startValue != nil || hovering ? 7 : 4)
                Circle()
                    .fill(LinearGradient(colors: [Theme.current.knobTop, Theme.current.knobBottom], startPoint: .top, endPoint: .bottom))
                    .overlay(Circle().strokeBorder(LinearGradient(colors: [Theme.ink.opacity(0.35), .black.opacity(0.6)], startPoint: .top, endPoint: .bottom), lineWidth: 1))
                    .shadow(color: .black.opacity(Theme.current.scheme == .dark ? 0.7 : 0.25), radius: 4, y: 3)
                    .padding(7)
                Capsule().fill(accent).frame(width: 2.5, height: size * 0.17)
                    .shadow(color: accent, radius: 3)
                    .offset(y: -size * 0.22)
                    .rotationEffect(.degrees(-135 + 270 * Double(value)))
            }
            .frame(width: size, height: size)
            .contentShape(Circle())
            .gesture(
                DragGesture(minimumDistance: 1)
                    .onChanged { g in
                        if startValue == nil { startValue = value }
                        let fine = NSEvent.modifierFlags.contains(.shift)
                        onChange(min(max(startValue! - Float(g.translation.height) / (fine ? 1200 : 180), 0), 1))
                    }
                    .onEnded { _ in startValue = nil })
            .simultaneousGesture(TapGesture(count: 2).onEnded { onReset() })
            .onHover { hovering = $0 }

            Text(text)
                .font(.system(size: 11, weight: .semibold, design: .monospaced))
                .foregroundStyle(Theme.ink.opacity(0.92))
                .lineLimit(1).minimumScaleFactor(0.7)
            Text(title.uppercased()).label().lineLimit(1).minimumScaleFactor(0.7)
        }
        .frame(width: size + 34)
    }
}

struct MeterBar: View {
    let level: Float
    var body: some View {
        GeometryReader { geo in
            let db = level > 0.00001 ? 20 * log10(level) : -100
            let frac = CGFloat(min(max((db + 60) / 60, 0), 1))
            ZStack(alignment: .bottom) {
                Capsule().fill(Theme.ink.opacity(0.07))
                Capsule()
                    .fill(LinearGradient(colors: [.green, .green, .yellow, .red], startPoint: .bottom, endPoint: .top))
                    .mask(alignment: .bottom) { Rectangle().frame(height: geo.size.height * frac) }
                    .shadow(color: .green.opacity(0.5), radius: 3)
            }
        }
        .frame(width: 6)
    }
}

struct InputMeter: View {
    @ObservedObject var meters: Meters
    var body: some View { MeterBar(level: meters.input) }
}

struct OutputMeter: View {
    @ObservedObject var meters: Meters
    var body: some View { HStack(spacing: 3) { MeterBar(level: meters.outL); MeterBar(level: meters.outR) } }
}

struct CPUReadout: View {
    @ObservedObject var meters: Meters
    var body: some View {
        Text(String(format: "DSP %2.0f%%", min(meters.cpu, 9.99) * 100))
            .font(.system(size: 11, weight: .medium, design: .monospaced))
            .foregroundStyle(meters.cpu > 0.8 ? Color.red : .white.opacity(0.55))
    }
}

struct PillButton: View {
    let title: String
    var icon: String? = nil
    var on = false
    var accent: Color = Theme.ink
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack(spacing: 5) {
                if let icon { Image(systemName: icon).font(.system(size: 11, weight: .bold)) }
                if !title.isEmpty { Text(title).font(.system(size: 11, weight: .bold, design: .rounded)).tracking(0.8) }
            }
            .foregroundStyle(on ? Theme.current.onAccent : Theme.ink.opacity(0.8))
            .padding(.horizontal, 11).frame(height: 26)
            .background(Capsule().fill(on ? AnyShapeStyle(accent) : AnyShapeStyle(Theme.ink.opacity(0.08))))
            .overlay(Capsule().strokeBorder(Theme.ink.opacity(on ? 0 : 0.12), lineWidth: 1))
            .shadow(color: on ? accent.opacity(0.6) : .clear, radius: 6)
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
    }
}

// MARK: - main window

struct ContentView: View {
    @EnvironmentObject var model: EngineModel
    @EnvironmentObject var audio: AudioManager
    @State private var showSettings = false

    var body: some View {
        ZStack {
            MetalSurface(mode: .background).ignoresSafeArea()
            VStack(spacing: 12) {
                TopBar(showSettings: $showSettings)
                DisplayRow()
                SlotPanel(slot: 0)
                RoutingBar()
                SlotPanel(slot: 1)
                StatusBar(showSettings: $showSettings)
            }
            .padding(.horizontal, 18).padding(.bottom, 12).padding(.top, 6)
        }
        .ignoresSafeArea(edges: .top)
        .id(model.themeName)  // rebuild everything when the theme changes
        .sheet(isPresented: $showSettings) { SettingsView().environmentObject(audio).environmentObject(model) }
    }
}

struct TopBar: View {
    @EnvironmentObject var model: EngineModel
    @Binding var showSettings: Bool
    @State private var saving = false
    @State private var newName = ""

    var body: some View {
        HStack(spacing: 12) {
            Text("HARMONIZER")
                .font(.system(size: 19, weight: .black, design: .rounded)).tracking(5)
                .foregroundStyle(LinearGradient(colors: [Theme.color(model.desc(0).category), Theme.color(model.desc(1).category)],
                                                startPoint: .leading, endPoint: .trailing))
                .shadow(color: Theme.color(model.desc(0).category).opacity(0.6), radius: 8)
                .padding(.leading, 76)

            Spacer(minLength: 8)

            HStack(spacing: 4) {
                PillButton(title: "", icon: "chevron.left") { model.stepPreset(-1) }
                Menu {
                    Section("Factory") {
                        ForEach(model.presets.filter(\.factory)) { p in Button(p.name) { model.load(p) } }
                    }
                    let user = model.presets.filter { !$0.factory }
                    if !user.isEmpty {
                        Section("User") { ForEach(user) { p in Button(p.name) { model.load(p) } } }
                    }
                    Divider()
                    Button("Save As…") { newName = model.currentIsUser ? model.presetName : ""; saving = true }
                    if model.currentIsUser { Button("Delete “\(model.presetName)”", role: .destructive) { model.deleteCurrent() } }
                } label: {
                    Text(model.presetName)
                        .font(.system(size: 13, weight: .semibold, design: .rounded))
                        .foregroundStyle(Theme.ink)
                        .frame(width: 190)
                }
                .menuStyle(.borderlessButton)
                .frame(width: 210, height: 26)
                .background(Capsule().fill(Theme.ink.opacity(0.08)))
                PillButton(title: "", icon: "chevron.right") { model.stepPreset(1) }
            }

            Spacer(minLength: 8)

            HStack(spacing: 6) {
                TextField("", value: $model.bpm, format: .number.precision(.fractionLength(0...1)))
                    .textFieldStyle(.plain).multilineTextAlignment(.trailing)
                    .font(.system(size: 13, weight: .semibold, design: .monospaced))
                    .foregroundStyle(Theme.ink)
                    .frame(width: 48)
                Text("BPM").label()
                PillButton(title: "TAP") { model.tap() }
                PillButton(title: "SYNC", on: model.sync, accent: Theme.current.hot) { model.sync.toggle() }
            }
            .padding(.horizontal, 10).frame(height: 34)
            .background(Capsule().fill(Theme.ink.opacity(0.06)))

            PillButton(title: "HOT SW", icon: "bolt.fill", on: model.hotSwitch, accent: Theme.current.hot) { model.hotSwitch.toggle() }
                .opacity(model.switchAssigned ? 1 : 0.45)
                .help("Right-click effect knobs to give them a HotSwitch value. Also follows MIDI CC 65.")
            PillButton(title: "TUNER", icon: "tuningfork", on: model.tuner, accent: .mint) { model.tuner.toggle() }
            PillButton(title: "", icon: "gearshape.fill") { showSettings = true }

            Button { model.active.toggle() } label: {
                HStack(spacing: 7) {
                    Circle().fill(model.active ? Color.green : Color.red.opacity(0.7)).frame(width: 9, height: 9)
                        .shadow(color: model.active ? .green : .clear, radius: 5)
                    Text(model.active ? "ACTIVE" : "BYPASS").foregroundStyle(Theme.ink).font(.system(size: 12, weight: .heavy, design: .rounded)).tracking(1.5)
                }
                .padding(.horizontal, 14).frame(height: 32)
                .background(Capsule().fill(Theme.ink.opacity(model.active ? 0.14 : 0.05)))
                .overlay(Capsule().strokeBorder(model.active ? Color.green.opacity(0.6) : Theme.ink.opacity(0.12), lineWidth: 1))
                .contentShape(Capsule())
            }
            .buttonStyle(.plain)
        }
        .frame(height: 46)
        .alert("Save Preset", isPresented: $saving) {
            TextField("Name", text: $newName)
            Button("Save") { model.save(as: newName) }
            Button("Cancel", role: .cancel) {}
        }
    }
}

struct DisplayRow: View {
    @EnvironmentObject var model: EngineModel

    var body: some View {
        HStack(spacing: 12) {
            HStack(spacing: 10) {
                InputMeter(meters: model.meters).padding(.vertical, 14)
                Knob(value: (model.inGain + 24) / 48, title: "Input", text: String(format: "%+.1f dB", model.inGain), accent: Theme.ink,
                     onChange: { model.inGain = $0 * 48 - 24 }, onReset: { model.inGain = 0 })
            }
            .padding(.horizontal, 12).frame(maxHeight: .infinity).panel()

            ZStack {
                MetalSurface(mode: .display)
                VStack {
                    HStack {
                        slotTag(0)
                        Image(systemName: model.parallel ? "plus" : "arrow.right").font(.system(size: 10, weight: .bold)).foregroundStyle(Color.white.opacity(0.4))
                        slotTag(1)
                        Spacer()
                        CPUReadout(meters: model.meters)
                    }
                    Spacer()
                }
                .padding(12)
                if model.tuner { TunerView(meters: model.meters).transition(.opacity) }
            }
            .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 16, style: .continuous).strokeBorder(Theme.ink.opacity(0.10), lineWidth: 1))
            .shadow(color: .black.opacity(0.6), radius: 18, y: 8)
            .animation(.easeInOut(duration: 0.2), value: model.tuner)

            HStack(spacing: 6) {
                Knob(value: model.hot, title: "Hot Knob", text: model.hotAssigned ? String(format: "%.0f%%", model.hot * 100) : "unassigned",
                     accent: Theme.current.hot, size: 72, onChange: { model.hot = $0 }, onReset: { model.hot = 0 })
                    .help("Right-click any effect knob to assign it. Also follows MIDI CC 1 and CC 11.")
                Knob(value: (model.outGain + 24) / 36, title: "Output", text: String(format: "%+.1f dB", model.outGain), accent: Theme.ink,
                     onChange: { model.outGain = $0 * 36 - 24 }, onReset: { model.outGain = 0 })
                OutputMeter(meters: model.meters).padding(.vertical, 14)
            }
            .padding(.horizontal, 12).frame(maxHeight: .infinity).panel(Theme.current.hot)
        }
        .frame(minHeight: 170, maxHeight: .infinity)
    }

    private func slotTag(_ s: Int) -> some View {
        let d = model.desc(s)
        return Text("\(s == 0 ? "A" : "B")  \(d.name.uppercased())")
            .font(.system(size: 10, weight: .bold, design: .rounded)).tracking(1.2)
            .foregroundStyle(model.slots[s].on ? Theme.color(d.category) : .white.opacity(0.3))
            .padding(.horizontal, 8).padding(.vertical, 4)
            .background(Capsule().fill(Color.black.opacity(0.45)))
    }
}

struct TunerView: View {
    @ObservedObject var meters: Meters
    private let names = ["C", "C♯", "D", "E♭", "E", "F", "F♯", "G", "A♭", "A", "B♭", "B"]

    var body: some View {
        let hz = meters.tunerHz
        let midi = hz > 0 ? 69 + 12 * log2(hz / 440) : 0
        let note = Int(midi.rounded())
        let cents = hz > 0 ? (midi - Float(note)) * 100 : 0
        let inTune = hz > 0 && abs(cents) < 3
        let color: Color = hz <= 0 ? .white.opacity(0.3) : inTune ? .green : abs(cents) < 12 ? .yellow : .orange

        return ZStack {
            Color.black.opacity(0.78)
            VStack(spacing: 10) {
                HStack(alignment: .firstTextBaseline, spacing: 4) {
                    Text(hz > 0 ? names[((note % 12) + 12) % 12] : "–")
                        .font(.system(size: 76, weight: .heavy, design: .rounded))
                    if hz > 0 { Text("\(note / 12 - 1)").font(.system(size: 24, weight: .semibold, design: .rounded)).opacity(0.6) }
                }
                .foregroundStyle(color).shadow(color: color.opacity(0.7), radius: 14)

                GeometryReader { geo in
                    ZStack {
                        Capsule().fill(Color.white.opacity(0.08)).frame(height: 6)
                        ForEach(-5..<6) { i in
                            Rectangle().fill(Color.white.opacity(i == 0 ? 0.8 : 0.25)).frame(width: i == 0 ? 2 : 1, height: i == 0 ? 22 : 12)
                                .offset(x: CGFloat(i) / 5 * geo.size.width / 2)
                        }
                        if hz > 0 {
                            Circle().fill(color).frame(width: 16, height: 16).shadow(color: color, radius: 8)
                                .offset(x: CGFloat(min(max(cents, -50), 50)) / 50 * geo.size.width / 2)
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
                .frame(width: 380, height: 26)

                Text(hz > 0 ? String(format: "%+.1f cents   %.1f Hz", cents, hz) : "play a single note")
                    .font(.system(size: 12, weight: .medium, design: .monospaced)).foregroundStyle(.white.opacity(0.6))
            }
        }
    }
}

struct SlotPanel: View {
    @EnvironmentObject var model: EngineModel
    let slot: Int

    var body: some View {
        let d = model.desc(slot)
        let state = model.slots[slot]
        let accent = Theme.color(d.category)

        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                Button { model.toggleSlot(slot) } label: {
                    ZStack {
                        Circle().fill(state.on ? accent : Theme.ink.opacity(0.1)).frame(width: 26, height: 26)
                            .shadow(color: state.on ? accent.opacity(0.8) : .clear, radius: 7)
                        Text(slot == 0 ? "A" : "B").font(.system(size: 13, weight: .black, design: .rounded))
                            .foregroundStyle(state.on ? Theme.current.onAccent : Theme.ink.opacity(0.5))
                    }
                    .contentShape(Circle())
                }
                .buttonStyle(.plain)
                .help(state.on ? "Turn this effect off" : "Turn this effect on")

                Menu {
                    ForEach(Theme.categories, id: \.self) { cat in
                        Section(cat) {
                            ForEach(model.algos.filter { $0.category == cat }) { a in
                                Button(a.name) { model.setAlgo(slot, a) }
                            }
                        }
                    }
                } label: {
                    Text(d.name).font(.system(size: 17, weight: .bold, design: .rounded)).foregroundStyle(Theme.ink)
                }
                .menuStyle(.borderlessButton).fixedSize()

                Text(d.category.uppercased())
                    .font(.system(size: 9, weight: .heavy, design: .rounded)).tracking(1.5)
                    .foregroundStyle(accent)
                    .padding(.horizontal, 7).padding(.vertical, 3)
                    .background(Capsule().fill(accent.opacity(0.15)))

                Text(d.blurb).font(.system(size: 12)).foregroundStyle(Theme.ink.opacity(0.45)).lineLimit(1)
                Spacer()
                if d.name == "Looper" {
                    // transport: drives the Mode parameter (Stop / Record / Play / Overdub)
                    let mode = Int((state.values[1] * 3).rounded())
                    PillButton(title: "REC", icon: "record.circle", on: mode == 1, accent: .red) { model.setParam(slot, 1, mode == 1 ? 2.0 / 3 : 1.0 / 3) }
                    PillButton(title: "PLAY", icon: "play.fill", on: mode == 2, accent: .green) { model.setParam(slot, 1, 2.0 / 3) }
                    PillButton(title: "DUB", icon: "plus.circle", on: mode == 3, accent: .orange) { model.setParam(slot, 1, mode == 3 ? 2.0 / 3 : 1) }
                    PillButton(title: "STOP", icon: "stop.fill", on: mode == 0) { model.setParam(slot, 1, 0) }
                }
                Button("Reset") { model.setAlgo(slot, d) }
                    .buttonStyle(.plain).font(.system(size: 11, weight: .medium)).foregroundStyle(Theme.ink.opacity(0.4))
            }

            HStack(spacing: 0) {
                ForEach(0..<d.params.count, id: \.self) { p in
                    let hot: (Float, Float)? = {
                        if let lo = state.hotMin[p], let hi = state.hotMax[p] { return (lo, hi) }
                        return nil
                    }()
                    Knob(value: state.values[p], title: d.params[p].name, text: model.display(slot, p), accent: accent, hotRange: hot,
                         onChange: { model.setParam(slot, p, $0) }, onReset: { model.setParam(slot, p, d.params[p].def) })
                        .contextMenu {
                            Button("Hot Knob minimum = current value") { model.setHot(slot, p, min: true) }
                            Button("Hot Knob maximum = current value") { model.setHot(slot, p, min: false) }
                            if hot != nil { Button("Remove from Hot Knob") { model.clearHot(slot, p) } }
                            Divider()
                            Button("HotSwitch value = current value") { model.setSwitch(slot, p) }
                            if state.hotSw?[p] != nil { Button("Remove from HotSwitch") { model.clearSwitch(slot, p) } }
                        }
                        .frame(maxWidth: .infinity)
                }
            }
            .opacity(state.on ? 1 : 0.4)
        }
        .padding(.horizontal, 16).padding(.vertical, 12)
        .panel(accent, strength: state.on ? 0.22 : 0.06)
    }
}

struct RoutingBar: View {
    @EnvironmentObject var model: EngineModel
    var body: some View {
        HStack(spacing: 8) {
            Rectangle().fill(Theme.ink.opacity(0.08)).frame(height: 1)
            PillButton(title: "SERIES  A → B", on: !model.parallel) { model.parallel = false }
            PillButton(title: "PARALLEL  A + B", on: model.parallel) { model.parallel = true }
            PillButton(title: "SWAP", icon: "arrow.up.arrow.down") { model.swapSlots() }
            Rectangle().fill(Theme.ink.opacity(0.08)).frame(height: 1)
        }
    }
}

struct StatusBar: View {
    @EnvironmentObject var audio: AudioManager
    @Binding var showSettings: Bool
    var body: some View {
        HStack(spacing: 8) {
            Circle().fill(audio.isRunning ? Color.green : Color.red).frame(width: 7, height: 7)
            Text(audio.status).lineLimit(1)
            if audio.isRunning {
                Text("·  \(Int(audio.actualRate)) Hz  ·  \(audio.actualBuffer) frames  ·  \(String(format: "%.1f", audio.latencyMs)) ms round trip")
            }
            Spacer()
            Button("Audio Settings…") { showSettings = true }.buttonStyle(.plain).foregroundStyle(Theme.ink.opacity(0.7))
        }
        .font(.system(size: 11, weight: .medium, design: .rounded))
        .foregroundStyle(Theme.ink.opacity(0.5))
    }
}

// MARK: - settings

struct SettingsView: View {
    @EnvironmentObject var audio: AudioManager
    @EnvironmentObject var model: EngineModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Settings").font(.system(size: 20, weight: .bold, design: .rounded))

            Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 12) {
                GridRow {
                    Text("Input device").gridColumnAlignment(.trailing)
                    Picker("", selection: $audio.inputUID) {
                        ForEach(audio.inputs) { Text($0.name).tag($0.uid) }
                    }
                }
                GridRow {
                    Text("Input channel")
                    Picker("", selection: $audio.inputChannel) {
                        ForEach(0..<audio.inputChannelCount, id: \.self) { Text("Input \($0 + 1)").tag($0) }
                    }
                }
                GridRow {
                    Text("Output device")
                    Picker("", selection: $audio.outputUID) {
                        ForEach(audio.outputs) { Text($0.name).tag($0.uid) }
                    }
                }
                GridRow {
                    Text("Sample rate")
                    Picker("", selection: $audio.sampleRate) {
                        ForEach(audio.rateOptions, id: \.self) { Text("\(Int($0)) Hz").tag($0) }
                    }
                }
                GridRow {
                    Text("Buffer size")
                    Picker("", selection: $audio.bufferSize) {
                        ForEach(audio.bufferOptions, id: \.self) { n in
                            Text("\(n) frames  (\(String(format: "%.1f", Double(n) / audio.sampleRate * 1000)) ms)").tag(n)
                        }
                    }
                }
                GridRow {
                    Text("Noise gate")
                    HStack {
                        Slider(value: Binding(get: { Double(model.gate) }, set: { model.gate = Float($0) < -89 ? -100 : Float($0) }), in: -90...(-20))
                        Text(model.gate <= -90 ? "Off" : String(format: "%.0f dB", model.gate))
                            .font(.system(size: 12, design: .monospaced)).frame(width: 54, alignment: .trailing)
                    }
                }
                GridRow {
                    Text("Theme")
                    Picker("", selection: $model.themeName) {
                        ForEach(Theme.all, id: \.name) { Text($0.name).tag($0.name) }
                    }
                }
                GridRow {
                    Text("Tuner")
                    Toggle("Mute output while tuning", isOn: $model.tunerMutes)
                }
            }
            .labelsHidden()

            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    Circle().fill(audio.isRunning ? Color.green : Color.red).frame(width: 8, height: 8)
                    Text(audio.status).fixedSize(horizontal: false, vertical: true)
                }
                if audio.isRunning {
                    Text(String(format: "Round-trip latency %.1f ms at %d Hz", audio.latencyMs, Int(audio.actualRate)))
                    Text(audio.latencyDetail).foregroundStyle(.secondary)
                    if audio.inputUID != audio.outputUID {
                        Text("Input and output are different devices, so they run as one drift-compensated aggregate. A single audio interface for both gives the lowest latency.")
                            .foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
            .font(.system(size: 12))
            .padding(12).frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 10).fill(Theme.ink.opacity(0.06)))

            HStack {
                if audio.micDenied {
                    Button("Open Privacy Settings") {
                        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone")!)
                    }
                }
                Button(audio.isRunning ? "Stop Audio" : "Start Audio") { audio.isRunning ? audio.stop() : audio.start() }
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(24)
        .frame(width: 520)
    }
}
