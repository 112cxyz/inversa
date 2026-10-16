// Inversa's icon, drawn in code: a stone tablet of text, like the Rosetta Stone, with a
// two-way arrow on it for the translating.
//   swift tools/make_icon.swift <out.iconset>     then: iconutil -c icns <out.iconset>
import AppKit

func color(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat(hex >> 16 & 0xff) / 255, green: CGFloat(hex >> 8 & 0xff) / 255,
            blue: CGFloat(hex & 0xff) / 255, alpha: alpha)
}

// a deterministic stream of numbers, so every build draws the same glyphs
struct Noise {
    var state: UInt64
    mutating func next() -> CGFloat {
        state = state &* 6364136223846793005 &+ 1442695040888963407
        return CGFloat(state >> 33) / CGFloat(1 << 31)
    }
}

func drawIcon(_ ctx: CGContext, size s: CGFloat) {
    ctx.saveGState()
    ctx.scaleBy(x: s / 1024, y: s / 1024)

    // background tile
    let tile = CGPath(roundedRect: CGRect(x: 100, y: 100, width: 824, height: 824), cornerWidth: 185, cornerHeight: 185, transform: nil)
    ctx.addPath(tile)
    ctx.clip()
    let sky = CGGradient(colorsSpace: nil, colors: [color(0x2b3a55), color(0x101722)] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(sky, start: CGPoint(x: 512, y: 924), end: CGPoint(x: 512, y: 100), options: [])

    // the stone, tilted a little, with a chipped top edge
    ctx.saveGState()
    ctx.translateBy(x: 512, y: 500)
    ctx.rotate(by: -0.07)
    let stone = CGMutablePath()
    stone.move(to: CGPoint(x: -250, y: -330))
    stone.addLine(to: CGPoint(x: 250, y: -330))
    stone.addLine(to: CGPoint(x: 262, y: 230))
    stone.addLine(to: CGPoint(x: 205, y: 300))
    stone.addLine(to: CGPoint(x: 120, y: 318))
    stone.addLine(to: CGPoint(x: 40, y: 290))
    stone.addLine(to: CGPoint(x: -60, y: 325))
    stone.addLine(to: CGPoint(x: -170, y: 300))
    stone.addLine(to: CGPoint(x: -255, y: 250))
    stone.closeSubpath()

    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -18), blur: 40, color: color(0x000000, 0.55))
    ctx.addPath(stone)
    ctx.setFillColor(color(0x3d3f47))
    ctx.fillPath()
    ctx.restoreGState()

    ctx.saveGState()
    ctx.addPath(stone)
    ctx.clip()
    let granite = CGGradient(colorsSpace: nil, colors: [color(0x6b6e78), color(0x45474f), color(0x2f3037)] as CFArray,
                             locations: [0, 0.55, 1])!
    ctx.drawLinearGradient(granite, start: CGPoint(x: -200, y: 320), end: CGPoint(x: 220, y: -330), options: [])
    var speckle = Noise(state: 7)
    for _ in 0..<900 {
        let x = speckle.next() * 540 - 270, y = speckle.next() * 680 - 340, r = speckle.next() * 2.6 + 0.6
        ctx.setFillColor(color(speckle.next() > 0.5 ? 0xffffff : 0x000000, 0.06 + speckle.next() * 0.08))
        ctx.fillEllipse(in: CGRect(x: x, y: y, width: r, height: r))
    }

    // the text: rows of dashes, cut in and catching the light along their lower edge
    var words = Noise(state: 2026)
    let text = CGMutablePath()
    var y: CGFloat = 232
    while y > -290 {
        var x: CGFloat = -196 + words.next() * 24
        while x < 170 {
            let w = min(28 + words.next() * 76, 186 - x)
            if w > 12 {
                text.move(to: CGPoint(x: x, y: y))
                text.addLine(to: CGPoint(x: x + w, y: y))
            }
            x += w + 16
        }
        y -= 27
    }
    ctx.setLineCap(.round)
    ctx.addPath(text)
    ctx.setStrokeColor(color(0x000000, 0.45))
    ctx.setLineWidth(10)
    ctx.saveGState(); ctx.translateBy(x: 0, y: 2); ctx.strokePath(); ctx.restoreGState()
    ctx.addPath(text)
    ctx.setStrokeColor(color(0xc4c7cf))
    ctx.setLineWidth(9)
    ctx.strokePath()
    ctx.restoreGState()
    ctx.restoreGState()

    // the two-way arrow on a round badge: one script into another, and back
    ctx.saveGState()
    ctx.translateBy(x: 720, y: 255)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -10), blur: 24, color: color(0x000000, 0.55))
    let badge = CGRect(x: -118, y: -118, width: 236, height: 236)
    ctx.addEllipse(in: badge)
    ctx.setFillColor(color(0xf2b544))
    ctx.fillPath()
    ctx.restoreGState()
    ctx.addEllipse(in: badge)
    ctx.clip()
    let shine = CGGradient(colorsSpace: nil, colors: [color(0xf9cc6b), color(0xe39a26)] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(shine, start: CGPoint(x: 0, y: 118), end: CGPoint(x: 0, y: -118), options: [])
    let arrows = CGMutablePath()
    arrows.move(to: CGPoint(x: -62, y: 30)); arrows.addLine(to: CGPoint(x: 58, y: 30))
    arrows.move(to: CGPoint(x: 26, y: 60)); arrows.addLine(to: CGPoint(x: 62, y: 30)); arrows.addLine(to: CGPoint(x: 26, y: 0))
    arrows.move(to: CGPoint(x: 62, y: -34)); arrows.addLine(to: CGPoint(x: -58, y: -34))
    arrows.move(to: CGPoint(x: -26, y: -4)); arrows.addLine(to: CGPoint(x: -62, y: -34)); arrows.addLine(to: CGPoint(x: -26, y: -64))
    ctx.addPath(arrows)
    ctx.setLineCap(.round); ctx.setLineJoin(.round); ctx.setLineWidth(22)
    ctx.setStrokeColor(color(0x2a2116))
    ctx.strokePath()
    ctx.restoreGState()

    ctx.restoreGState()
}

let out = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "Inversa.iconset")
try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
for (points, scales) in [(16, [1, 2]), (32, [1, 2]), (128, [1, 2]), (256, [1, 2]), (512, [1, 2])] {
    for scale in scales {
        let px = points * scale
        let ctx = CGContext(data: nil, width: px, height: px, bitsPerComponent: 8, bytesPerRow: 0,
                            space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        drawIcon(ctx, size: CGFloat(px))
        let rep = NSBitmapImageRep(cgImage: ctx.makeImage()!)
        let name = scale == 1 ? "icon_\(points)x\(points).png" : "icon_\(points)x\(points)@2x.png"
        try! rep.representation(using: .png, properties: [:])!.write(to: out.appendingPathComponent(name))
    }
}
