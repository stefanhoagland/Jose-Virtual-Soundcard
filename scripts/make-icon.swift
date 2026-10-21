// Draws the app icon (1024x1024 PNG). Usage: swift make-icon.swift out.png
import AppKit

let size = 1024
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size, bitsPerSample: 8,
                           samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                           bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)

let s = CGFloat(size)
let rect = NSRect(x: s * 0.1, y: s * 0.1, width: s * 0.8, height: s * 0.8)
let shape = NSBezierPath(roundedRect: rect, xRadius: s * 0.18, yRadius: s * 0.18)
NSGradient(starting: NSColor(calibratedRed: 0.10, green: 0.16, blue: 0.32, alpha: 1),
           ending: NSColor(calibratedRed: 0.05, green: 0.45, blue: 0.62, alpha: 1))!
    .draw(in: shape, angle: 90)

// Level bars, like a channel meter bridge.
let heights: [CGFloat] = [0.30, 0.52, 0.42, 0.66, 0.48, 0.58, 0.36, 0.50]
let barW = rect.width * 0.065
let gap = (rect.width * 0.76 - barW * CGFloat(heights.count)) / CGFloat(heights.count - 1)
for (i, h) in heights.enumerated() {
    let x = rect.minX + rect.width * 0.12 + CGFloat(i) * (barW + gap)
    let bar = NSRect(x: x, y: rect.minY + rect.height * 0.30, width: barW, height: rect.height * h * 0.75)
    NSColor(calibratedRed: 0.45, green: 0.95, blue: 0.75, alpha: 1).setFill()
    NSBezierPath(roundedRect: bar, xRadius: barW / 2, yRadius: barW / 2).fill()
}

let text = "64" as NSString
let attrs: [NSAttributedString.Key: Any] = [
    .font: NSFont.systemFont(ofSize: s * 0.13, weight: .heavy),
    .foregroundColor: NSColor.white,
]
let ts = text.size(withAttributes: attrs)
text.draw(at: NSPoint(x: rect.midX - ts.width / 2, y: rect.minY + rect.height * 0.08), withAttributes: attrs)

NSGraphicsContext.restoreGraphicsState()
let png = rep.representation(using: .png, properties: [:])!
try! png.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
