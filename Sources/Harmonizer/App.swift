import HarmonizerDSP
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate {
    static var onQuit: (() -> Void)?
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ notification: Notification) { AppDelegate.onQuit?() }
}

@main
struct HarmonizerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var model: EngineModel
    @StateObject private var audio: AudioManager

    init() {
        let engine = hz_create()!
        let m = EngineModel(engine: engine)
        let a = AudioManager(engine: engine)
        _model = StateObject(wrappedValue: m)
        _audio = StateObject(wrappedValue: a)
        let demoPreset = ProcessInfo.processInfo.environment["HARMONIZER_DEMO"]
        AppDelegate.onQuit = {
            if demoPreset == nil { m.persist() }  // a demo run must not overwrite the saved session
            a.stop()
        }
        if let demo = demoPreset {
            m.startDemo(preset: demo)  // synthetic signal, no audio device opened
            a.markDemo()
        } else {
            DispatchQueue.main.async { a.start() }
        }
    }

    var body: some Scene {
        WindowGroup("Harmonizer") {
            ContentView()
                .environmentObject(model)
                .environmentObject(audio)
                .frame(minWidth: 1180, minHeight: 760)
                .preferredColorScheme(Theme.named(model.themeName).scheme)
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 1280, height: 800)
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandMenu("Theme") {
                ForEach(Theme.all, id: \.name) { t in
                    Button((model.themeName == t.name ? "✓ " : "    ") + t.name) { model.themeName = t.name }
                }
            }
            CommandMenu("Effect") {
                Button(model.active ? "Bypass" : "Activate") { model.active.toggle() }.keyboardShortcut(.space, modifiers: [])
                Button("Tap Tempo") { model.tap() }.keyboardShortcut("t", modifiers: [])
                Button("Tuner") { model.tuner.toggle() }.keyboardShortcut("u", modifiers: [])
                Button("HotSwitch") { model.hotSwitch.toggle() }.keyboardShortcut("h", modifiers: [])
                Divider()
                Button("Previous Preset") { model.stepPreset(-1) }.keyboardShortcut("[", modifiers: .command)
                Button("Next Preset") { model.stepPreset(1) }.keyboardShortcut("]", modifiers: .command)
                Button("Swap A and B") { model.swapSlots() }.keyboardShortcut("x", modifiers: [.command, .shift])
            }
        }
    }
}
