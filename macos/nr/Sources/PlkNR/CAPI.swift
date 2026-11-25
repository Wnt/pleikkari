import CPlkNR
import CoreVideo
import Foundation

// The C interface of plknr.h. An engine handle is a retained `Engine`.

private func engine(_ handle: OpaquePointer?) -> Engine? {
  handle.map { Unmanaged<Engine>.fromOpaque(UnsafeRawPointer($0)).takeUnretainedValue() }
}

private func write(_ message: String, to buffer: UnsafeMutablePointer<CChar>?, size: Int) {
  guard let buffer, size > 0 else { return }
  message.withCString { _ = strlcpy(buffer, $0, size) }
}

@_cdecl("plk_nr_abi_version")
public func plk_nr_abi_version() -> UInt32 { UInt32(PLK_NR_ABI_VERSION) }

@_cdecl("plk_nr_create")
public func plk_nr_create(
  _ config: UnsafePointer<plk_nr_config>?, _ error: UnsafeMutablePointer<CChar>?, _ errorSize: Int
) -> OpaquePointer? {
  guard let config, config.pointee.struct_size == UInt32(MemoryLayout<plk_nr_config>.size) else {
    write("plk_nr_config has an unexpected size (library and caller disagree)", to: error, size: errorSize)
    return nil
  }
  guard let path = config.pointee.model_path else {
    write("no model path", to: error, size: errorSize)
    return nil
  }
  let url = URL(fileURLWithPath: String(cString: path), isDirectory: true)
  guard FileManager.default.fileExists(atPath: url.appendingPathComponent("manifest.json").path) else {
    write("\(url.path) is not a .dlssmodel package (no manifest.json)", to: error, size: errorSize)
    return nil
  }
  do {
    let created = try Engine(
      modelURL: url, halfPrecision: config.pointee.half_precision != 0,
      controls: Engine.Controls(config.pointee.controls))
    return OpaquePointer(Unmanaged.passRetained(created).toOpaque())
  } catch let failure {
    let text = (failure as? LocalizedError)?.errorDescription ?? String(describing: failure)
    write(text, to: error, size: errorSize)
    return nil
  }
}

@_cdecl("plk_nr_describe")
public func plk_nr_describe(_ handle: OpaquePointer?) -> UnsafePointer<CChar>? {
  engine(handle).map { UnsafePointer($0.summary) }
}

@_cdecl("plk_nr_submit")
public func plk_nr_submit(
  _ handle: OpaquePointer?, _ frame: CVPixelBuffer?, _ seq: UInt64, _ reset: Int32,
  _ callback: plk_nr_callback?, _ context: UnsafeMutableRawPointer?
) -> Int32 {
  guard let engine = engine(handle), let frame, let callback else { return 0 }
  return engine.submit(frame, seq: seq, reset: reset != 0, callback: callback, context: context) ? 1 : 0
}

@_cdecl("plk_nr_set_controls")
public func plk_nr_set_controls(_ handle: OpaquePointer?, _ controls: UnsafePointer<plk_nr_controls>?) {
  guard let engine = engine(handle), let controls else { return }
  engine.setControls(Engine.Controls(controls.pointee))
}

@_cdecl("plk_nr_destroy")
public func plk_nr_destroy(_ handle: OpaquePointer?) {
  guard let handle else { return }
  let owned = Unmanaged<Engine>.fromOpaque(UnsafeRawPointer(handle))
  owned.takeUnretainedValue().close()
  owned.release()
}
