// plk-nr-bench: the engine without the client.
//
// Measures what one frame costs at a given size, through the same C entry points the app
// uses, and can render one image for a look at the result:
//
//   plk-nr-bench --model M.dlssmodel --size 1920x1080,1280x720,960x540 --frames 20
//   plk-nr-bench --model M.dlssmodel --image shot.png --out shot-nr.png
//
// The frames are a moving synthetic picture unless --image is given. Every frame waits
// for the previous one, so the times are the engine's own, with the GPU otherwise idle:
// run it while nothing else needs the GPU, and not during a stream.
import CPlkNR
import CoreGraphics
import CoreVideo
import Foundation
import ImageIO
import PlkNR
import UniformTypeIdentifiers

struct Options {
  var model = ""
  var sizes: [(Int, Int)] = [(1280, 720)]
  var frames = 12
  var temporal = true
  var half = true
  var image: String?
  var out: String?
  var profile: Int32 = 0
  var intensity: Float = 1
  var motion = 6
  var frameGen: String?
}

func usage() -> Never {
  FileHandle.standardError.write(Data("""
    usage: plk-nr-bench --model PATH.dlssmodel [--size WxH[,WxH...]] [--frames N]
                        [--temporal on|off] [--float32] [--profile 0..3] [--intensity X]
                        [--motion PIXELS_PER_FRAME] [--image IN.png] [--out OUT.png]
           plk-nr-bench --framegen WEIGHTS.safetensors [--size WxH[,WxH...]] [--frames N] [--out OUT.png]

    """.utf8))
  exit(2)
}

func parse() -> Options {
  var o = Options()
  var args = Array(CommandLine.arguments.dropFirst())
  func next() -> String {
    guard !args.isEmpty else { usage() }
    return args.removeFirst()
  }
  while !args.isEmpty {
    switch args.removeFirst() {
    case "--model": o.model = next()
    case "--size":
      o.sizes = next().split(separator: ",").map { item in
        let parts = item.lowercased().split(separator: "x").compactMap { Int($0) }
        guard parts.count == 2, parts[0] >= 64, parts[1] >= 64 else { usage() }
        return (parts[0], parts[1])
      }
    case "--frames": o.frames = max(2, Int(next()) ?? 0)
    case "--temporal": o.temporal = next() != "off"
    case "--float32": o.half = false
    case "--profile": o.profile = Int32(next()) ?? 0
    case "--intensity": o.intensity = Float(next()) ?? 1
    case "--motion": o.motion = Int(next()) ?? 0
    case "--framegen": o.frameGen = next()
    case "--half-pictures": break
    case "--generated": _ = next()
    case "--image": o.image = next()
    case "--out": o.out = next()
    default: usage()
    }
  }
  if o.model.isEmpty && o.frameGen == nil { usage() }
  return o
}

func makeBuffer(_ width: Int, _ height: Int) -> CVPixelBuffer {
  var buffer: CVPixelBuffer?
  let attributes: [CFString: Any] = [
    kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
    kCVPixelBufferMetalCompatibilityKey: true,
  ]
  let status = CVPixelBufferCreate(
    kCFAllocatorDefault, width, height, kCVPixelFormatType_32BGRA, attributes as CFDictionary, &buffer)
  guard status == kCVReturnSuccess, let buffer else { fatalError("CVPixelBufferCreate \(status)") }
  return buffer
}

/// A picture with what a game frame has: smooth shading, hard edges, fine texture, and
/// everything sliding sideways by `shift` pixels.
func fillSynthetic(_ buffer: CVPixelBuffer, shift: Int) {
  CVPixelBufferLockBaseAddress(buffer, [])
  defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
  let width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer)
  let stride = CVPixelBufferGetBytesPerRow(buffer)
  let base = CVPixelBufferGetBaseAddress(buffer)!.assumingMemoryBound(to: UInt8.self)
  for y in 0..<height {
    let row = base + y * stride
    for x in 0..<width {
      let sx = x + shift
      var hash = UInt32(truncatingIfNeeded: sx &* 73_856_093 ^ y &* 19_349_663)
      hash ^= hash >> 13
      hash = hash &* 1_274_126_177
      let grain = Int(hash >> 28) - 8
      let checker = ((sx / 48) + (y / 48)) & 1 == 0 ? 40 : 0
      let r = min(255, max(0, sx * 255 / max(1, width) % 256 / 2 + 60 + checker + grain))
      let g = min(255, max(0, y * 200 / max(1, height) + 30 + grain))
      let b = min(255, max(0, 150 - checker + grain + (sx % 97 < 3 ? 90 : 0)))
      row[x * 4] = UInt8(b)
      row[x * 4 + 1] = UInt8(g)
      row[x * 4 + 2] = UInt8(r)
      row[x * 4 + 3] = 255
    }
  }
}

func bitmapContext(_ buffer: CVPixelBuffer) -> CGContext {
  CGContext(
    data: CVPixelBufferGetBaseAddress(buffer), width: CVPixelBufferGetWidth(buffer),
    height: CVPixelBufferGetHeight(buffer), bitsPerComponent: 8,
    bytesPerRow: CVPixelBufferGetBytesPerRow(buffer),
    space: CGColorSpace(name: CGColorSpace.sRGB)!,
    bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
}

func loadImage(_ path: String) -> CGImage {
  guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
    let image = CGImageSourceCreateImageAtIndex(source, 0, nil)
  else {
    FileHandle.standardError.write(Data("cannot read \(path)\n".utf8))
    exit(1)
  }
  return image
}

func draw(_ image: CGImage, into buffer: CVPixelBuffer) {
  CVPixelBufferLockBaseAddress(buffer, [])
  defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
  let context = bitmapContext(buffer)
  context.interpolationQuality = .high
  context.draw(image, in: CGRect(x: 0, y: 0, width: context.width, height: context.height))
}

func writePNG(_ buffer: CVPixelBuffer, to path: String) {
  CVPixelBufferLockBaseAddress(buffer, .readOnly)
  defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
  guard let image = bitmapContext(buffer).makeImage(),
    let destination = CGImageDestinationCreateWithURL(
      URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)
  else { return }
  CGImageDestinationAddImage(destination, image, nil)
  CGImageDestinationFinalize(destination)
}

/// Mean absolute difference of the colour channels, in code values of 255.
func difference(_ a: CVPixelBuffer, _ b: CVPixelBuffer) -> Double {
  CVPixelBufferLockBaseAddress(a, .readOnly)
  CVPixelBufferLockBaseAddress(b, .readOnly)
  defer {
    CVPixelBufferUnlockBaseAddress(a, .readOnly)
    CVPixelBufferUnlockBaseAddress(b, .readOnly)
  }
  let width = CVPixelBufferGetWidth(a), height = CVPixelBufferGetHeight(a)
  let pa = CVPixelBufferGetBaseAddress(a)!.assumingMemoryBound(to: UInt8.self)
  let pb = CVPixelBufferGetBaseAddress(b)!.assumingMemoryBound(to: UInt8.self)
  let sa = CVPixelBufferGetBytesPerRow(a), sb = CVPixelBufferGetBytesPerRow(b)
  var sum = 0
  for y in 0..<height {
    for x in 0..<width {
      for c in 0..<3 {
        sum += abs(Int(pa[y * sa + x * 4 + c]) - Int(pb[y * sb + x * 4 + c]))
      }
    }
  }
  return Double(sum) / Double(width * height * 3)
}

final class Waiter {
  let done = DispatchSemaphore(value: 0)
  var milliseconds = 0.0
  var output: CVPixelBuffer?
  var error: String?
}

let options = parse()

// --framegen WEIGHTS.safetensors: what the picture between two frames costs, and with
// --out a look at one (the synthetic picture moves --motion pixels between the two).
if let weights = options.frameGen {
  var message = [CChar](repeating: 0, count: 512)
  guard let generator = weights.withCString({ plk_fg_create($0, options.half ? 1 : 0, &message, message.count) }) else {
    FileHandle.standardError.write(Data("plk_fg_create: \(String(cString: message))\n".utf8))
    exit(1)
  }
  let waiter = Waiter()
  let context = Unmanaged.passUnretained(waiter).toOpaque()
  let callback: plk_nr_callback = { context, result in
    let waiter = Unmanaged<Waiter>.fromOpaque(context!).takeUnretainedValue()
    if let result = result?.pointee {
      waiter.milliseconds = result.total_ms
      waiter.output = result.output?.takeUnretainedValue()
      waiter.error = result.error.map { String(cString: $0) }
    }
    // One signal for a pair: with its last frame, or with its failure.
    if let result = result?.pointee, result.output != nil, result.index + 1 < result.count { return }
    waiter.done.signal()
  }
  // --generated N: frames asked for between each two (1 or 3).
  var generatedPerPair: Int32 = 1
  if let at = CommandLine.arguments.firstIndex(of: "--generated"), at + 1 < CommandLine.arguments.count {
    generatedPerPair = Int32(CommandLine.arguments[at + 1]) ?? 1
  }
  for (width, height) in options.sizes {
    var times: [Double] = []
    var first = 0.0
    // --half-pictures: RGBA half float, as the pictures of an HDR stream are given.
    let halfPictures = CommandLine.arguments.contains("--half-pictures")
    func picture(_ shift: Int) -> CVPixelBuffer {
      let eight = makeBuffer(width, height)
      fillSynthetic(eight, shift: shift)
      guard halfPictures else { return eight }
      var buffer: CVPixelBuffer?
      let attributes: [CFString: Any] = [
        kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary, kCVPixelBufferMetalCompatibilityKey: true,
      ]
      CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_64RGBAHalf, attributes as CFDictionary, &buffer)
      let half = buffer!
      CVPixelBufferLockBaseAddress(eight, .readOnly)
      CVPixelBufferLockBaseAddress(half, [])
      let from = CVPixelBufferGetBaseAddress(eight)!.assumingMemoryBound(to: UInt8.self)
      let to = CVPixelBufferGetBaseAddress(half)!
      let fromStride = CVPixelBufferGetBytesPerRow(eight), toStride = CVPixelBufferGetBytesPerRow(half)
      for y in 0..<height {
        let row = (to + y * toStride).assumingMemoryBound(to: Float16.self)
        for x in 0..<width {
          row[x * 4] = Float16(Float(from[y * fromStride + x * 4 + 2]) / 255)
          row[x * 4 + 1] = Float16(Float(from[y * fromStride + x * 4 + 1]) / 255)
          row[x * 4 + 2] = Float16(Float(from[y * fromStride + x * 4]) / 255)
          row[x * 4 + 3] = 1
        }
      }
      CVPixelBufferUnlockBaseAddress(half, [])
      CVPixelBufferUnlockBaseAddress(eight, .readOnly)
      return half
    }
    var previous = picture(0)
    for index in 1...options.frames {
      let current = picture(index * options.motion)
      guard plk_fg_interpolate(generator, previous, current, generatedPerPair, UInt64(index), callback, context) == 1 else {
        print("\(width)x\(height): the generator refused pair \(index)")
        exit(1)
      }
      waiter.done.wait()
      if let error = waiter.error {
        print("\(width)x\(height): pair \(index) failed: \(error)")
        exit(1)
      }
      if index == 1 { first = waiter.milliseconds } else { times.append(waiter.milliseconds) }
      previous = current
    }
    times.sort()
    let median = times[times.count / 2]
    print(String(
      format: "frame generation %dx%d, \(generatedPerPair) per pair: first %.0f ms, then min %.1f  median %.1f  p90 %.1f  max %.1f ms",
      width, height, first, times[0], median, times[min(times.count - 1, times.count * 9 / 10)], times[times.count - 1]))
    if let out = options.out, let output = waiter.output, !halfPictures {
      writePNG(output, to: out)
      print("wrote \(out)")
    }
  }
  plk_fg_destroy(generator)
  exit(0)
}
var controls = plk_nr_controls(
  intensity: options.intensity, detail_strength: 1, colour_strength: 1, profile: options.profile,
  temporal: options.temporal ? 1 : 0)
var errorText = [CChar](repeating: 0, count: 512)
let loadStarted = DispatchTime.now().uptimeNanoseconds
let engine: OpaquePointer? = options.model.withCString { path in
  var config = plk_nr_config(
    struct_size: UInt32(MemoryLayout<plk_nr_config>.size), model_path: path,
    half_precision: options.half ? 1 : 0, controls: controls)
  return plk_nr_create(&config, &errorText, errorText.count)
}
guard let engine else {
  FileHandle.standardError.write(Data("plk_nr_create: \(String(cString: errorText))\n".utf8))
  exit(1)
}
let loadMs = Double(DispatchTime.now().uptimeNanoseconds - loadStarted) / 1e6
print("model: \(String(cString: plk_nr_describe(engine)!))")
print(String(format: "load: %.0f ms, temporal %@", loadMs, options.temporal ? "on" : "off"))

let image = options.image.map(loadImage)
var sizes = options.sizes
if let image, CommandLine.arguments.contains("--size") == false {
  sizes = [(image.width, image.height)]
}

let waiter = Waiter()
let context = Unmanaged.passUnretained(waiter).toOpaque()
let callback: plk_nr_callback = { context, result in
  let waiter = Unmanaged<Waiter>.fromOpaque(context!).takeUnretainedValue()
  if let result = result?.pointee {
    waiter.milliseconds = result.total_ms
    waiter.output = result.output?.takeUnretainedValue()
    waiter.error = result.error.map { String(cString: $0) }
  }
  waiter.done.signal()
}

var failed = false
for (width, height) in sizes {
  var times: [Double] = []
  var first = 0.0
  var lastInput: CVPixelBuffer?
  for index in 0..<options.frames {
    let buffer = makeBuffer(width, height)
    if let image { draw(image, into: buffer) } else { fillSynthetic(buffer, shift: index * options.motion) }
    guard plk_nr_submit(engine, buffer, UInt64(index), index == 0 ? 1 : 0, callback, context) == 1 else {
      print("\(width)x\(height): the engine refused frame \(index)")
      failed = true
      break
    }
    waiter.done.wait()
    if let error = waiter.error {
      print("\(width)x\(height): frame \(index) failed: \(error)")
      failed = true
      break
    }
    if index == 0 { first = waiter.milliseconds } else { times.append(waiter.milliseconds) }
    lastInput = buffer
  }
  guard !times.isEmpty, let lastInput, let output = waiter.output else { continue }
  times.sort()
  let median = times[times.count / 2]
  print(String(
    format: "%dx%d: first %.0f ms, then min %.1f  median %.1f  p90 %.1f  max %.1f ms  (%.1f frames/s), mean change %.2f/255",
    width, height, first, times[0], median, times[min(times.count - 1, times.count * 9 / 10)],
    times[times.count - 1], 1000 / median, difference(lastInput, output)))
  if let out = options.out {
    let path = sizes.count == 1 ? out : out.replacingOccurrences(of: ".png", with: "-\(width)x\(height).png")
    writePNG(output, to: path)
    print("wrote \(path)")
  }
}
controls.intensity = options.intensity
plk_nr_set_controls(engine, &controls)
plk_nr_destroy(engine)
exit(failed ? 1 : 0)
