import SwiftUI

/// A complete look: window background, panel and knob finish, text colour and
/// the accent colour of every effect category.
struct ThemeSpec {
    let name: String
    let scheme: ColorScheme
    let ink: Color                 // text and hairlines on panels
    let onAccent: Color            // text on a lit button
    let panel: Color               // panel fill
    let knobTop: Color
    let knobBottom: Color
    let background: SIMD3<Float>   // window background base (Metal)
    let glow: Float                // how strongly the aurora tints the background
    let vignette: Float
    let screenLow: SIMD3<Float>    // display background, bottom and top
    let screenHigh: SIMD3<Float>
    let hot: Color                 // Hot Knob / HotSwitch accent
    let palette: [String: SIMD3<Float>]
}

enum Theme {
    static let categories = ["Pitch", "Delay", "Modulation", "Reverb", "Tone", "Synth", "Granular", "Multi", "Looper"]

    private static func pal(_ c: [SIMD3<Float>]) -> [String: SIMD3<Float>] {
        Dictionary(uniqueKeysWithValues: zip(categories, c))
    }

    static let all: [ThemeSpec] = [
        ThemeSpec(name: "Midnight", scheme: .dark, ink: .white, onAccent: .black, panel: Color.black.opacity(0.42),
                  knobTop: Color(white: 0.30), knobBottom: Color(white: 0.10),
                  background: [0.035, 0.038, 0.058], glow: 1.0, vignette: 0.9, screenLow: [0.016, 0.020, 0.032], screenHigh: [0.035, 0.042, 0.066],
                  hot: Color(red: 1.0, green: 0.62, blue: 0.15),
                  palette: pal([[0.20, 0.90, 0.95], [1.00, 0.70, 0.22], [0.82, 0.42, 1.00], [0.32, 0.58, 1.00], [0.40, 0.95, 0.52],
                                [1.00, 0.38, 0.62], [0.85, 0.92, 0.25], [0.55, 0.80, 1.00], [1.00, 0.32, 0.30]])),
        ThemeSpec(name: "Synthwave", scheme: .dark, ink: Color(red: 1.0, green: 0.94, blue: 1.0), onAccent: .black, panel: Color(red: 0.10, green: 0.02, blue: 0.16).opacity(0.55),
                  knobTop: Color(red: 0.34, green: 0.24, blue: 0.44), knobBottom: Color(red: 0.10, green: 0.05, blue: 0.16),
                  background: [0.055, 0.020, 0.090], glow: 1.5, vignette: 0.9, screenLow: [0.030, 0.008, 0.050], screenHigh: [0.075, 0.020, 0.110],
                  hot: Color(red: 1.0, green: 0.85, blue: 0.25),
                  palette: pal([[0.15, 0.95, 1.00], [1.00, 0.30, 0.70], [0.72, 0.40, 1.00], [0.35, 0.50, 1.00], [1.00, 0.88, 0.30],
                                [1.00, 0.25, 0.90], [0.30, 1.00, 0.70], [1.00, 0.60, 0.30], [1.00, 0.30, 0.40]])),
        ThemeSpec(name: "Ember", scheme: .dark, ink: Color(red: 1.0, green: 0.96, blue: 0.90), onAccent: .black, panel: Color(red: 0.07, green: 0.03, blue: 0.02).opacity(0.55),
                  knobTop: Color(red: 0.36, green: 0.29, blue: 0.24), knobBottom: Color(red: 0.12, green: 0.08, blue: 0.06),
                  background: [0.060, 0.034, 0.026], glow: 1.2, vignette: 0.9, screenLow: [0.030, 0.014, 0.010], screenHigh: [0.070, 0.036, 0.024],
                  hot: Color(red: 1.0, green: 0.90, blue: 0.55),
                  palette: pal([[1.00, 0.55, 0.15], [1.00, 0.82, 0.30], [1.00, 0.40, 0.55], [1.00, 0.34, 0.24], [1.00, 0.90, 0.70],
                                [0.95, 0.35, 0.80], [0.85, 0.90, 0.30], [1.00, 0.70, 0.55], [0.92, 0.20, 0.30]])),
        ThemeSpec(name: "Emerald", scheme: .dark, ink: Color(red: 0.93, green: 1.0, blue: 0.96), onAccent: .black, panel: Color(red: 0.01, green: 0.06, blue: 0.04).opacity(0.55),
                  knobTop: Color(red: 0.24, green: 0.33, blue: 0.30), knobBottom: Color(red: 0.06, green: 0.11, blue: 0.09),
                  background: [0.022, 0.050, 0.040], glow: 1.2, vignette: 0.9, screenLow: [0.008, 0.026, 0.020], screenHigh: [0.020, 0.058, 0.046],
                  hot: Color(red: 1.0, green: 0.80, blue: 0.30),
                  palette: pal([[0.25, 1.00, 0.75], [0.95, 0.85, 0.35], [0.45, 0.85, 1.00], [0.30, 0.95, 0.55], [0.75, 1.00, 0.45],
                                [0.55, 0.70, 1.00], [0.90, 1.00, 0.55], [0.40, 0.95, 0.95], [1.00, 0.45, 0.35]])),
        ThemeSpec(name: "Graphite", scheme: .dark, ink: .white, onAccent: .black, panel: Color(white: 0.02).opacity(0.50),
                  knobTop: Color(white: 0.42), knobBottom: Color(white: 0.13),
                  background: [0.062, 0.062, 0.066], glow: 0.55, vignette: 0.9, screenLow: [0.020, 0.020, 0.022], screenHigh: [0.050, 0.050, 0.054],
                  hot: Color(red: 1.0, green: 0.55, blue: 0.20),
                  palette: pal([[0.92, 0.95, 1.00], [1.00, 0.93, 0.82], [0.90, 0.86, 1.00], [0.80, 0.90, 1.00], [0.86, 1.00, 0.90],
                                [1.00, 0.84, 0.90], [0.96, 0.98, 0.80], [0.82, 0.96, 0.98], [1.00, 0.45, 0.40]])),
        ThemeSpec(name: "Porcelain", scheme: .light, ink: Color(red: 0.10, green: 0.11, blue: 0.15), onAccent: .white, panel: Color.white.opacity(0.66),
                  knobTop: Color(white: 0.99), knobBottom: Color(white: 0.80),
                  background: [0.885, 0.895, 0.915], glow: 0.35, vignette: 0.22, screenLow: [0.050, 0.058, 0.080], screenHigh: [0.095, 0.105, 0.140],
                  hot: Color(red: 0.92, green: 0.45, blue: 0.05),
                  palette: pal([[0.00, 0.56, 0.66], [0.86, 0.50, 0.04], [0.56, 0.26, 0.86], [0.14, 0.40, 0.90], [0.10, 0.60, 0.30],
                                [0.86, 0.20, 0.46], [0.48, 0.58, 0.04], [0.18, 0.50, 0.80], [0.85, 0.20, 0.15]])),
    ]

    /// The active theme. Views read it while rendering; the root view is rebuilt when it changes.
    static var current = all[0]

    static func named(_ name: String) -> ThemeSpec { all.first { $0.name == name } ?? all[0] }

    static var ink: Color { current.ink }

    static func rgb(_ category: String) -> SIMD3<Float> { current.palette[category] ?? [0.8, 0.8, 0.8] }
    static func color(_ category: String) -> Color {
        let c = rgb(category)
        return Color(red: Double(c.x), green: Double(c.y), blue: Double(c.z))
    }
}
