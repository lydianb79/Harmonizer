// Renders the app icon (the UI's glowing knob on the Midnight background)
// into an .iconset folder. Usage: swift Tools/make_icon.swift <out.iconset>
import AppKit

func color(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor { CGColor(red: r, green: g, blue: b, alpha: a) }
let space = CGColorSpaceCreateDeviceRGB()

func mix(_ a: [CGFloat], _ b: [CGFloat], _ t: CGFloat) -> CGColor {
    color(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)
}

/// Draws the icon in a 1024 x 1024 coordinate space.
func draw(_ ctx: CGContext) {
    let shape = CGPath(roundedRect: CGRect(x: 100, y: 100, width: 824, height: 824), cornerWidth: 186, cornerHeight: 186, transform: nil)
    let cyan: [CGFloat] = [0.20, 0.90, 0.95], violet: [CGFloat] = [0.82, 0.42, 1.00], blue: [CGFloat] = [0.32, 0.58, 1.00]

    // soft drop shadow under the tile
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -12), blur: 28, color: color(0, 0, 0, 0.45))
    ctx.addPath(shape)
    ctx.setFillColor(color(0.03, 0.035, 0.06))
    ctx.fillPath()
    ctx.restoreGState()

    ctx.saveGState()
    ctx.addPath(shape)
    ctx.clip()

    // background: deep navy with two aurora glows
    let bg = CGGradient(colorsSpace: space, colors: [color(0.075, 0.085, 0.14), color(0.02, 0.022, 0.04)] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(bg, start: CGPoint(x: 512, y: 924), end: CGPoint(x: 512, y: 100), options: [])
    func glow(_ c: [CGFloat], _ at: CGPoint, _ radius: CGFloat, _ alpha: CGFloat) {
        let g = CGGradient(colorsSpace: space, colors: [color(c[0], c[1], c[2], alpha), color(c[0], c[1], c[2], 0)] as CFArray, locations: [0, 1])!
        ctx.drawRadialGradient(g, startCenter: at, startRadius: 0, endCenter: at, endRadius: radius, options: [])
    }
    glow(cyan, CGPoint(x: 250, y: 800), 520, 0.30)
    glow(violet, CGPoint(x: 800, y: 230), 520, 0.30)
    glow(blue, CGPoint(x: 512, y: 512), 420, 0.10)

    // faint spectrum bars along the bottom, echoing the app's display
    let heights: [CGFloat] = [70, 120, 95, 160, 210, 150, 230, 180, 120, 150, 90, 60, 80, 40]
    for (i, h) in heights.enumerated() {
        let t = CGFloat(i) / CGFloat(heights.count - 1)
        ctx.setFillColor(mix(cyan, violet, t).copy(alpha: 0.16)!)
        ctx.fill(CGRect(x: 150 + CGFloat(i) * 52.5, y: 100, width: 34, height: h))
    }

    let c = CGPoint(x: 512, y: 520)
    let arcR: CGFloat = 292, lineW: CGFloat = 46
    let start = CGFloat.pi * 225 / 180, sweep = CGFloat.pi * 270 / 180, value: CGFloat = 0.74

    // track
    ctx.setLineCap(.round)
    ctx.setLineWidth(lineW)
    ctx.setStrokeColor(color(1, 1, 1, 0.09))
    ctx.addArc(center: c, radius: arcR, startAngle: start, endAngle: start - sweep, clockwise: true)
    ctx.strokePath()

    // value arc: cyan -> violet, drawn in short segments, once blurred for the glow and once crisp
    func valueArc(glowing: Bool) {
        let n = 160
        for i in 0..<n {
            let t0 = CGFloat(i) / CGFloat(n), t1 = CGFloat(i + 1) / CGFloat(n) + 0.004
            let col = mix(cyan, violet, t0)
            ctx.saveGState()
            if glowing { ctx.setShadow(offset: .zero, blur: 60, color: col.copy(alpha: 0.9)) }
            ctx.setStrokeColor(col)
            ctx.setLineCap(i == 0 || i == n - 1 ? .round : .butt)
            ctx.addArc(center: c, radius: arcR, startAngle: start - sweep * value * t0, endAngle: start - sweep * value * t1, clockwise: true)
            ctx.strokePath()
            ctx.restoreGState()
        }
    }
    valueArc(glowing: true)
    valueArc(glowing: false)

    // knob body
    let bodyR: CGFloat = 214
    let body = CGRect(x: c.x - bodyR, y: c.y - bodyR, width: bodyR * 2, height: bodyR * 2)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -22), blur: 44, color: color(0, 0, 0, 0.75))
    ctx.setFillColor(color(0.1, 0.1, 0.12))
    ctx.fillEllipse(in: body)
    ctx.restoreGState()
    ctx.saveGState()
    ctx.addEllipse(in: body)
    ctx.clip()
    let knob = CGGradient(colorsSpace: space, colors: [color(0.36, 0.37, 0.41), color(0.15, 0.155, 0.18), color(0.07, 0.072, 0.09)] as CFArray, locations: [0, 0.55, 1])!
    ctx.drawLinearGradient(knob, start: CGPoint(x: c.x, y: c.y + bodyR), end: CGPoint(x: c.x, y: c.y - bodyR), options: [])
    let sheen = CGGradient(colorsSpace: space, colors: [color(1, 1, 1, 0.16), color(1, 1, 1, 0)] as CFArray, locations: [0, 1])!
    ctx.drawRadialGradient(sheen, startCenter: CGPoint(x: c.x - 60, y: c.y + 120), startRadius: 0, endCenter: CGPoint(x: c.x - 60, y: c.y + 120), endRadius: 260, options: [])
    ctx.restoreGState()
    // rim: light from above
    ctx.saveGState()
    ctx.addEllipse(in: body.insetBy(dx: 3, dy: 3))
    ctx.setLineWidth(6)
    ctx.replacePathWithStrokedPath()
    ctx.clip()
    let rim = CGGradient(colorsSpace: space, colors: [color(1, 1, 1, 0.55), color(1, 1, 1, 0.05), color(0, 0, 0, 0.5)] as CFArray, locations: [0, 0.5, 1])!
    ctx.drawLinearGradient(rim, start: CGPoint(x: c.x, y: c.y + bodyR), end: CGPoint(x: c.x, y: c.y - bodyR), options: [])
    ctx.restoreGState()

    // pointer at the end of the value arc
    let ang = start - sweep * value
    let tip = mix(cyan, violet, 1)
    ctx.saveGState()
    ctx.translateBy(x: c.x, y: c.y)
    ctx.rotate(by: ang - .pi / 2)
    ctx.setShadow(offset: .zero, blur: 26, color: tip.copy(alpha: 0.95))
    ctx.setFillColor(color(0.95, 0.85, 1.0))
    ctx.addPath(CGPath(roundedRect: CGRect(x: -11, y: 96, width: 22, height: 92), cornerWidth: 11, cornerHeight: 11, transform: nil))
    ctx.fillPath()
    ctx.restoreGState()

    ctx.restoreGState()

    // hairline edge highlight on the tile
    ctx.saveGState()
    ctx.addPath(shape)
    ctx.setLineWidth(3)
    ctx.replacePathWithStrokedPath()
    ctx.clip()
    let edge = CGGradient(colorsSpace: space, colors: [color(1, 1, 1, 0.30), color(1, 1, 1, 0.03)] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(edge, start: CGPoint(x: 512, y: 924), end: CGPoint(x: 512, y: 100), options: [])
    ctx.restoreGState()
}

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon.iconset"
try FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
for (points, scale) in [(16, 1), (16, 2), (32, 1), (32, 2), (128, 1), (128, 2), (256, 1), (256, 2), (512, 1), (512, 2)] {
    let px = points * scale
    let ctx = CGContext(data: nil, width: px, height: px, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.interpolationQuality = .high
    ctx.scaleBy(x: CGFloat(px) / 1024, y: CGFloat(px) / 1024)
    draw(ctx)
    let rep = NSBitmapImageRep(cgImage: ctx.makeImage()!)
    let name = "icon_\(points)x\(points)\(scale == 2 ? "@2x" : "").png"
    try rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out).appendingPathComponent(name))
}
print("wrote \(out)")
