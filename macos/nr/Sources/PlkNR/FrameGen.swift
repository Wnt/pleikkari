import CPlkNR
import CoreVideo
import DLSSMLX
import Foundation

/// One GPU job of this library at a time. The neural rendering and the frame generation
/// are two networks on one GPU through one MLX; their evaluations are not interleaved.
actor GPUGate {
  static let shared = GPUGate()
  private var taken = false
  private var waiting: [CheckedContinuation<Void, Never>] = []

  func enter() async {
    if !taken {
      taken = true
      return
    }
    await withCheckedContinuation { waiting.append($0) }
  }

  func leave() {
    if waiting.isEmpty { taken = false } else { waiting.removeFirst().resume() }
  }
}

/// NVIDIA's DLSS frame generation network: the picture halfway between two frames.
///
/// Like `Engine`, it never queues: `interpolate` takes a pair only when the previous one
/// has left.
final class FrameGen: @unchecked Sendable {
  private struct Completion: @unchecked Sendable {
    let callback: plk_nr_callback
    let context: UnsafeMutableRawPointer?

    func call(
      seq: UInt64, output: MLXPixelBuffer?, milliseconds: Double, error: String?, index: Int, count: Int
    ) {
      func deliver(_ message: UnsafePointer<CChar>?) {
        var result = plk_nr_result(
          seq: seq, output: output.map { Unmanaged.passUnretained($0.buffer) },
          total_ms: milliseconds, error: message, index: UInt32(index), count: UInt32(count))
        callback(context, &result)
      }
      if let error { error.withCString { deliver($0) } } else { deliver(nil) }
    }
  }

  private let generator: MLXNativeFrameGenerator
  private let state = NSCondition()  // guards busy, inFlight, closed
  private var busy = false
  private var inFlight = 0
  private var closed = false
  private var busySince: UInt64 = 0
  private var refusals = 0
  private let delivery = NSLock()

  /// Says why a pair was not taken: the first time and then every five hundredth.
  private func refused(_ why: String) {
    state.lock()
    refusals += 1
    let say = refusals % 500 == 1
    state.unlock()
    if say { FileHandle.standardError.write(Data("plknr: frame generation refused a pair: \(why)\n".utf8)) }
  }
  // Touched only by the pair in flight.
  private var writer: (width: Int, height: Int, half: Bool, writer: MLXPixelBufferWriter)?

  init(weightsURL: URL, halfPrecision: Bool) throws {
    generator = try MLXNativeFrameGenerator(
      weightsURL: weightsURL, precision: halfPrecision ? .float16 : .float32)
  }

  func interpolate(
    _ previous: CVPixelBuffer, _ current: CVPixelBuffer, count: Int, seq: UInt64,
    callback: plk_nr_callback, context: UnsafeMutableRawPointer?
  ) -> Bool {
    guard count >= 1, count <= 15 else { return false }
    let format = CVPixelBufferGetPixelFormatType(previous)
    guard format == kCVPixelFormatType_32BGRA || format == kCVPixelFormatType_64RGBAHalf,
      CVPixelBufferGetPixelFormatType(current) == format,
      CVPixelBufferGetIOSurface(previous) != nil, CVPixelBufferGetIOSurface(current) != nil,
      CVPixelBufferGetWidth(previous) == CVPixelBufferGetWidth(current),
      CVPixelBufferGetHeight(previous) == CVPixelBufferGetHeight(current)
    else {
      refused(
        "unusable pictures: formats \(format) and \(CVPixelBufferGetPixelFormatType(current)), "
          + "\(CVPixelBufferGetWidth(previous))x\(CVPixelBufferGetHeight(previous)) and "
          + "\(CVPixelBufferGetWidth(current))x\(CVPixelBufferGetHeight(current))")
      return false
    }
    state.lock()
    if busy || closed {
      let stuck = busy ? Double(DispatchTime.now().uptimeNanoseconds - busySince) / 1e6 : 0
      state.unlock()
      // Busy is ordinary; busy for a second is a pair that never came back.
      if stuck > 1000 { refused(String(format: "the pair before it has been in the works for %.0f ms", stuck)) }
      return false
    }
    busySince = DispatchTime.now().uptimeNanoseconds
    busy = true
    inFlight += 1
    state.unlock()

    let a = MLXPixelBuffer(previous), b = MLXPixelBuffer(current)
    let completion = Completion(callback: callback, context: context)
    let started = DispatchTime.now().uptimeNanoseconds
    Task.detached(priority: .userInitiated) { [self] in
      var outputs: [MLXPixelBuffer] = []
      var failure: String?
      await GPUGate.shared.enter()
      do {
        let frames = try await generator.interpolate(
          try MLXVideoFrame(pixelBuffer: a), try MLXVideoFrame(pixelBuffer: b), factor: count + 1)
        guard frames.count == count else { throw MLXMediaError("\(frames.count) frames were generated, not \(count)") }
        // The generated frame has the pixel format of the two it lies between.
        let half = format == kCVPixelFormatType_64RGBAHalf
        if writer?.width != a.width || writer?.height != a.height || writer?.half != half {
          writer = (
            a.width, a.height, half,
            try MLXPixelBufferWriter(width: a.width, height: a.height, halfOutput: half)
          )
        }
        for frame in frames { outputs.append(try await writer!.writer.write(frame)) }
      } catch {
        outputs = []
        failure = (error as? LocalizedError)?.errorDescription ?? String(describing: error)
      }
      await GPUGate.shared.leave()
      let milliseconds = Double(DispatchTime.now().uptimeNanoseconds - started) / 1e6
      finish(completion, seq: seq, outputs: outputs, milliseconds: milliseconds, error: failure)
    }
    return true
  }

  /// Idle first, then the callback (whoever hears of the result may offer the next pair at
  /// once), then the pair counts as gone.
  private func finish(
    _ completion: Completion, seq: UInt64, outputs: [MLXPixelBuffer], milliseconds: Double, error: String?
  ) {
    state.lock()
    busy = false
    state.unlock()
    delivery.lock()
    if outputs.isEmpty {
      completion.call(
        seq: seq, output: nil, milliseconds: milliseconds, error: error ?? "no frame was generated", index: 0, count: 0)
    } else {
      for (index, output) in outputs.enumerated() {
        completion.call(
          seq: seq, output: output, milliseconds: milliseconds, error: nil, index: index, count: outputs.count)
      }
    }
    delivery.unlock()
    state.lock()
    inFlight -= 1
    state.broadcast()
    state.unlock()
  }

  /// No pair is taken after this; returns when every pair taken has delivered.
  func close() {
    state.lock()
    closed = true
    while inFlight > 0 { state.wait() }
    state.unlock()
  }
}
