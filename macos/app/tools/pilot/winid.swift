import CoreGraphics
import Foundation
let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as! [[String: Any]]
for w in list where (w[kCGWindowOwnerName as String] as? String) == "Pleikkari" {
    let b = w[kCGWindowBounds as String] as! [String: Any]
    if (b["Height"] as! Double) > 300 { print(w[kCGWindowNumber as String] as! Int); break }
}
