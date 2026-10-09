// Dump frames of a screen recording (screencapture -V <sec> out.mov) as PNGs,
// for catching one-frame flashes that a screenshot loop cannot:
//
//   movframes <in.mov> <out_dir> [fps=30] [maxWidth=480] [startSec=0] [endSec=duration]
//
// Writes <out_dir>/f<index>_<ms>.png and prints one line per frame with the
// mean RGB of the frame, so a white flash shows up as a spike in the list
// without opening every image. Builds with `xcrun swiftc -O movframes.swift`.
import AVFoundation
import AppKit

let args = CommandLine.arguments
guard args.count >= 3 else {
    FileHandle.standardError.write("usage: movframes <in.mov> <out_dir> [fps] [maxWidth]\n".data(using: .utf8)!)
    exit(2)
}
let url = URL(fileURLWithPath: args[1])
let outDir = args[2]
let fps = args.count > 3 ? Double(args[3]) ?? 30 : 30
let maxWidth = args.count > 4 ? Int(args[4]) ?? 480 : 480
let startSec = args.count > 5 ? Double(args[5]) ?? 0 : 0
let endSec = args.count > 6 ? Double(args[6]) ?? 1e9 : 1e9
try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)

let asset = AVURLAsset(url: url)
let duration = CMTimeGetSeconds(asset.duration)
let gen = AVAssetImageGenerator(asset: asset)
gen.appliesPreferredTrackTransform = true
gen.requestedTimeToleranceBefore = .zero
gen.requestedTimeToleranceAfter = .zero
gen.maximumSize = CGSize(width: maxWidth, height: maxWidth)

var index = 0
var t = startSec
while t < min(duration, endSec) {
    let time = CMTime(seconds: t, preferredTimescale: 600)
    if let cg = try? gen.copyCGImage(at: time, actualTime: nil) {
        // Mean RGB over a 1/4-size sample.
        let w = cg.width, h = cg.height
        let bytesPerRow = w * 4
        var buf = [UInt8](repeating: 0, count: bytesPerRow * h)
        if let ctx = CGContext(data: &buf, width: w, height: h, bitsPerComponent: 8, bytesPerRow: bytesPerRow,
                               space: CGColorSpaceCreateDeviceRGB(),
                               bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) {
            ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
            var r = 0, g = 0, b = 0, n = 0
            var y = 0
            while y < h { var x = 0; while x < w {
                let i = y * bytesPerRow + x * 4
                r += Int(buf[i]); g += Int(buf[i + 1]); b += Int(buf[i + 2]); n += 1
                x += 4 }; y += 4 }
            let ms = Int(t * 1000)
            let name = String(format: "f%04d_%06d.png", index, ms)
            let rep = NSBitmapImageRep(cgImage: cg)
            if let png = rep.representation(using: .png, properties: [:]) {
                try? png.write(to: URL(fileURLWithPath: outDir).appendingPathComponent(name))
            }
            print(String(format: "%@ mean=%d,%d,%d", name, r / max(n, 1), g / max(n, 1), b / max(n, 1)))
        }
    }
    index += 1
    t += 1.0 / fps
}
