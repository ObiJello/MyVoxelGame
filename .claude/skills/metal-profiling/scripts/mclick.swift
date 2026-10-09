import CoreGraphics
import Foundation
// mclick x y [count] [right]  — real mouse click(s) at logical screen coordinates;
// count 0 = move the pointer there only (a hover: Xcode's jump icons appear on it)
let a = CommandLine.arguments
let p = CGPoint(x: Double(a[1])!, y: Double(a[2])!)
let n = a.count > 3 ? Int(a[3])! : 1
let right = a.count > 4 && a[4] == "right"
let src = CGEventSource(stateID: .hidSystemState)
CGEvent(mouseEventSource: src, mouseType: .mouseMoved, mouseCursorPosition: p, mouseButton: .left)?.post(tap: .cghidEventTap)
usleep(80000)
if n <= 0 { exit(0) }
for i in 1...n {
  let down = CGEvent(mouseEventSource: src, mouseType: right ? .rightMouseDown : .leftMouseDown, mouseCursorPosition: p, mouseButton: right ? .right : .left)!
  let up = CGEvent(mouseEventSource: src, mouseType: right ? .rightMouseUp : .leftMouseUp, mouseCursorPosition: p, mouseButton: right ? .right : .left)!
  down.setIntegerValueField(.mouseEventClickState, value: Int64(i))
  up.setIntegerValueField(.mouseEventClickState, value: Int64(i))
  down.post(tap: .cghidEventTap); usleep(30000); up.post(tap: .cghidEventTap); usleep(60000)
}
