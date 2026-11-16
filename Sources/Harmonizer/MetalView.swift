import HarmonizerDSP
import MetalKit
import SwiftUI

private let shaderSource = """
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 pos [[position]]; float2 uv; };
struct U {
    float4 a;     // time, width, height, mode (0 background, 1 display)
    float4 b;     // level, active, hot, unused
    float4 colA;
    float4 colB;
    float4 base;   // window background rgb, aurora strength
    float4 scrLo;  // display background bottom rgb, vignette strength
    float4 scrHi;  // display background top rgb
};

vertex VOut vmain(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

static float hash(float2 p) { return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
static float noise(float2 p) {
    float2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + float2(1, 0)), f.x), mix(hash(i + float2(0, 1)), hash(i + float2(1, 1)), f.x), f.y);
}
static float fbm(float2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; i++) { v += a * noise(p); p = p * 2.03 + float2(17.0, 9.0); a *= 0.5; }
    return v;
}
static float row(texture2d<float> t, float x, uint r) {
    float fx = clamp(x, 0.0, 1.0) * 511.0;
    uint i = uint(fx);
    float f = fx - float(i);
    return mix(t.read(uint2(i, r)).r, t.read(uint2(min(i + 1, 511u), r)).r, f);
}

fragment float4 fmain(VOut in [[stage_in]], constant U &u [[buffer(0)]], texture2d<float> data [[texture(0)]]) {
    float time = u.a.x;
    float2 res = u.a.yz;
    float2 uv = in.uv;
    float level = u.b.x, live = u.b.y;
    float3 cA = u.colA.rgb, cB = u.colB.rgb;

    if (u.a.w < 0.5) {
        // ---- window background: slow aurora that breathes with the signal
        float2 p = uv * float2(res.x / res.y, 1.0);
        float t = time * 0.035;
        float n1 = fbm(p * 1.4 + float2(t, -t * 0.7));
        float n2 = fbm(p * 2.1 + float2(-t * 0.8, t) + n1 * 1.5);
        float3 col = u.base.rgb;
        float glow = (0.16 + 0.34 * level) * u.base.a;
        col = mix(col, cA, smoothstep(0.35, 0.95, n1) * glow * (1.0 - uv.y * 0.6));
        col = mix(col, cB, smoothstep(0.40, 1.00, n2) * glow * (0.4 + uv.y * 0.6));
        float2 q = uv - 0.5;
        col *= 1.0 - dot(q, q) * u.scrLo.a;
        col += (hash(uv * res + time) - 0.5) * 0.012;
        return float4(col, 1.0);
    }

    // ---- display: spectrum fill + glowing oscilloscope trace
    float3 col = mix(u.scrLo.rgb, u.scrHi.rgb, uv.y);
    float2 g = abs(fract(uv * float2(16.0, 6.0)) - 0.5);
    float grid = smoothstep(0.485, 0.5, max(g.x, g.y));
    col += float3(0.05, 0.06, 0.09) * grid * 0.5;

    float y = 1.0 - uv.y;
    float s = row(data, uv.x, 0u) * 0.92;
    float3 sc = mix(cA, cB, smoothstep(0.0, 1.0, uv.x));
    float inside = smoothstep(0.0, 1.5 / res.y, s - y);
    float bars = 0.55 + 0.45 * smoothstep(0.15, 0.3, abs(fract(uv.x * 96.0) - 0.5));
    col += sc * inside * (0.10 + 0.55 * pow(y / max(s, 0.001), 2.5)) * bars * (0.35 + 0.65 * live);
    float edge = abs(y - s) * res.y;
    col += sc * exp(-edge * edge / 5.0) * 0.9 * (0.35 + 0.65 * live);
    col += sc * exp(-edge / 18.0) * 0.12 * step(y, s + 0.2);

    float w = row(data, uv.x, 1u);
    float dx = 1.0 / 511.0;
    float slope = (row(data, uv.x + dx, 1u) - row(data, uv.x - dx, 1u)) * 0.5 * 0.42 * res.y / (dx * res.x);
    float d = abs((0.5 - uv.y) - w * 0.42) * res.y / sqrt(1.0 + slope * slope);
    float3 wc = mix(float3(1.0), mix(cA, cB, 0.5), 0.35);
    col += wc * (exp(-d * d / 3.0) * 1.1 + exp(-d / 10.0) * 0.22) * (0.25 + 0.75 * live);

    col *= 0.94 + 0.06 * sin(uv.y * res.y * 3.14159);
    float2 q = uv - 0.5;
    col *= 1.0 - dot(q, q) * 0.7;
    return float4(col, 1.0);
}
"""

final class Renderer: NSObject, MTKViewDelegate {
    enum Mode: Float { case background = 0, display = 1 }

    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let texture: MTLTexture
    private let mode: Mode
    private weak var model: EngineModel?
    private let start = CACurrentMediaTime()
    private var spectrum = [Float](repeating: 0, count: 512)
    private var wave = [Float](repeating: 0, count: 512)
    private var level: Float = 0

    init?(view: MTKView, mode: Mode, model: EngineModel) {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { return nil }
        self.device = device
        self.queue = queue
        self.mode = mode
        self.model = model
        do {
            let lib = try device.makeLibrary(source: shaderSource, options: nil)
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = lib.makeFunction(name: "vmain")
            d.fragmentFunction = lib.makeFunction(name: "fmain")
            d.colorAttachments[0].pixelFormat = .bgra8Unorm
            pipeline = try device.makeRenderPipelineState(descriptor: d)
        } catch {
            NSLog("Metal setup failed: \(error)")
            return nil
        }
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float, width: 512, height: 2, mipmapped: false)
        td.usage = .shaderRead
        guard let tex = device.makeTexture(descriptor: td) else { return nil }
        texture = tex
        super.init()
        view.device = device
        view.colorPixelFormat = .bgra8Unorm
        view.framebufferOnly = true
        view.preferredFramesPerSecond = mode == .display ? 60 : 30
        view.delegate = self
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let model, let pass = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
              let cmd = queue.makeCommandBuffer() else { return }
        if mode == .background {
            level += (min(1, max(model.meters.outL, model.meters.outR) * 1.5) - level) * 0.1
        } else {
            let rms = hz_get_visual(model.engine, &spectrum, &wave, 512)
            level += (min(1, rms * 5) - level) * 0.15
            spectrum.withUnsafeBytes { texture.replace(region: MTLRegionMake2D(0, 0, 512, 1), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: 2048) }
            wave.withUnsafeBytes { texture.replace(region: MTLRegionMake2D(0, 1, 512, 1), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: 2048) }
        }
        let cA = model.colorA, cB = model.colorB, t = Theme.current
        var u: [SIMD4<Float>] = [
            [Float(CACurrentMediaTime() - start), Float(view.drawableSize.width), Float(view.drawableSize.height), mode.rawValue],
            [level, model.liveFlag, 0, 0],
            [cA.x, cA.y, cA.z, 1],
            [cB.x, cB.y, cB.z, 1],
            [t.background.x, t.background.y, t.background.z, t.glow],
            [t.screenLow.x, t.screenLow.y, t.screenLow.z, t.vignette],
            [t.screenHigh.x, t.screenHigh.y, t.screenHigh.z, 0],
        ]
        guard let enc = cmd.makeRenderCommandEncoder(descriptor: pass) else { return }
        enc.setRenderPipelineState(pipeline)
        enc.setFragmentBytes(&u, length: MemoryLayout<SIMD4<Float>>.stride * 7, index: 0)
        enc.setFragmentTexture(texture, index: 0)
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        enc.endEncoding()
        cmd.present(drawable)
        cmd.commit()
    }
}

struct MetalSurface: NSViewRepresentable {
    let mode: Renderer.Mode
    @EnvironmentObject var model: EngineModel

    func makeCoordinator() -> Holder { Holder() }
    final class Holder { var renderer: Renderer? }

    func makeNSView(context: Context) -> MTKView {
        let view = MTKView()
        view.layer?.isOpaque = true
        context.coordinator.renderer = Renderer(view: view, mode: mode, model: model)
        return view
    }

    func updateNSView(_ view: MTKView, context: Context) {}
}

extension EngineModel {
    // MTKView draws on the main thread, so reading the model here is safe.
    var colorA: SIMD3<Float> { Theme.rgb(desc(0).category) }
    var colorB: SIMD3<Float> { Theme.rgb(desc(1).category) }
    var liveFlag: Float { active ? 1 : 0 }
}
