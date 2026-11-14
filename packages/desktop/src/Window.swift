import AppKit

struct Options {
  let values: [String: Any]

  init(json: String) {
    values = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] ?? [:]
  }

  func bool(_ name: String, _ fallback: Bool) -> Bool {
    guard let number = values[name] as? NSNumber, CFGetTypeID(number) == CFBooleanGetTypeID() else { return fallback }
    return number.boolValue
  }

  func number(_ name: String, _ fallback: Double) -> Double {
    guard let number = values[name] as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID() else { return fallback }
    return number.doubleValue
  }

  func string(_ name: String) -> String? {
    values[name] as? String
  }

  func object(_ name: String) -> Options? {
    (values[name] as? [String: Any]).map(Options.init(values:))
  }

  init(values: [String: Any]) {
    self.values = values
  }
}

func color(hex: String?) -> NSColor? {
  guard let hex, hex.hasPrefix("#"), hex.count == 7 || hex.count == 9, let raw = UInt64(hex.dropFirst(), radix: 16) else {
    return nil
  }
  let rgba = hex.count == 9 ? raw : raw << 8 | 0xff
  let channel = { (shift: UInt64) in CGFloat((rgba >> shift) & 0xff) / 255 }
  return NSColor(srgbRed: channel(24), green: channel(16), blue: channel(8), alpha: channel(0))
}

final class DesktopWindow: NSWindow {
  var focusable = true
  override var canBecomeKey: Bool { focusable }
  override var canBecomeMain: Bool { focusable }
}

final class BrowserContainer: NSView {
  var acceptsFirstMouseOption = false
  override var isFlipped: Bool { true }
  override func acceptsFirstMouse(for event: NSEvent?) -> Bool { acceptsFirstMouseOption }
}

final class DragRegionView: NSView {
  var regions: [AntDesktopRegion] = []
  override var isFlipped: Bool { true }

  override func hitTest(_ point: NSPoint) -> NSView? {
    let local = convert(point, from: superview)
    var draggable = false
    for region in regions where NSRect(x: region.x, y: region.y, width: region.width, height: region.height).contains(local) {
      draggable = region.draggable
    }
    return draggable ? self : nil
  }

  override func mouseDown(with event: NSEvent) {
    window?.performDrag(with: event)
  }
}

@MainActor
final class WindowController: NSObject, NSWindowDelegate {
  static var all: [Int64: WindowController] = [:]

  let id: Int64
  let window: DesktopWindow
  let container: BrowserContainer
  let dragRegions = DragRegionView()
  weak var browserView: NSView?
  var showWhenReady: Bool

  init(id: Int64, options: Options) {
    self.id = id
    showWhenReady = options.bool("show", true)
    let size = NSSize(width: max(1, options.number("width", 900)), height: max(1, options.number("height", 600)))
    let frame = NSRect(origin: .zero, size: size)
    let framed = options.bool("frame", true)
    let titleBarStyle = options.string("titleBarStyle")
    let inlineTitleBar = ["hidden", "hiddenInset", "customButtonsOnHover"].contains(titleBarStyle)
    let transparent = options.bool("transparent", false)
    let vibrancy = options.string("vibrancy")

    var style: NSWindow.StyleMask = framed ? .titled : .borderless
    if framed && options.bool("closable", true) { style.insert(.closable) }
    if framed && options.bool("minimizable", true) { style.insert(.miniaturizable) }
    if options.bool("resizable", true) { style.insert(.resizable) }
    if inlineTitleBar { style.insert(.fullSizeContentView) }

    window = DesktopWindow(contentRect: frame, styleMask: style, backing: .buffered, defer: false)
    container = BrowserContainer(frame: frame)
    super.init()

    window.focusable = options.bool("focusable", true)
    window.title = options.string("title") ?? "Ant Desktop"
    window.isReleasedWhenClosed = false
    window.isMovable = options.bool("movable", true)
    window.hasShadow = options.bool("hasShadow", true)
    window.isOpaque = !transparent
    window.backgroundColor = transparent ? .clear : color(hex: options.string("backgroundColor")) ?? .black
    window.level = options.bool("alwaysOnTop", false) ? .floating : .normal
    window.alphaValue = min(1, max(0, options.number("opacity", 1)))
    window.minSize = NSSize(width: max(0, options.number("minWidth", 0)), height: max(0, options.number("minHeight", 0)))
    let maxWidth = options.number("maxWidth", 0)
    let maxHeight = options.number("maxHeight", 0)
    window.maxSize = NSSize(
      width: maxWidth > 0 ? maxWidth : .greatestFiniteMagnitude, height: maxHeight > 0 ? maxHeight : .greatestFiniteMagnitude)
    if options.bool("contentProtection", false) { window.sharingType = .none }
    if options.bool("visibleOnAllWorkspaces", false) { window.collectionBehavior.insert(.canJoinAllSpaces) }
    if options.bool("hiddenInMissionControl", false) || options.bool("skipTaskbar", false) {
      window.collectionBehavior.insert(.transient)
      window.isExcludedFromWindowsMenu = true
    }
    if !options.bool("fullscreenable", true) { window.collectionBehavior.insert(.fullScreenNone) }
    if let identifier = options.string("tabbingIdentifier"), !identifier.isEmpty { window.tabbingIdentifier = identifier }
    if !options.bool("maximizable", true) { window.standardWindowButton(.zoomButton)?.isEnabled = false }
    if inlineTitleBar {
      window.titleVisibility = .hidden
      window.titlebarAppearsTransparent = true
      window.titlebarSeparatorStyle = .none
    }

    container.autoresizingMask = [.width, .height]
    container.acceptsFirstMouseOption = options.bool("acceptFirstMouse", false)
    container.wantsLayer = true
    container.layer?.backgroundColor = (transparent || vibrancy != nil ? NSColor.clear : NSColor.black).cgColor

    if let vibrancy {
      let effect = NSVisualEffectView(frame: frame)
      effect.autoresizingMask = [.width, .height]
      effect.blendingMode = .behindWindow
      effect.state =
        switch options.string("visualEffectState") {
        case "active": .active
        case "inactive": .inactive
        default: .followsWindowActiveState
        }
      effect.material =
        switch vibrancy {
        case "sidebar": .sidebar
        case "menu": .menu
        case "popover": .popover
        case "under-window": .underWindowBackground
        default: .windowBackground
        }
      effect.addSubview(container)
      window.contentView = effect
    } else {
      window.contentView = container
    }

    let content = window.contentView!
    content.wantsLayer = true
    let borderWidth = max(0, options.number("borderWidth", 0))
    if borderWidth > 0 {
      content.layer?.borderWidth = borderWidth
      content.layer?.borderColor = (color(hex: options.string("borderColor")) ?? .separatorColor).cgColor
    }
    if !framed || options.bool("roundedCorners", true) {
      content.layer?.cornerRadius = max(0, options.number("cornerRadius", 10))
      content.layer?.masksToBounds = true
    }

    if let position = options.object("trafficLightPosition") { placeTrafficLights(position) }
    window.delegate = self

    let x = options.number("x", .nan)
    let y = options.number("y", .nan)
    if x.isFinite || y.isFinite {
      let visible = (window.screen ?? NSScreen.main)?.visibleFrame ?? .zero
      var origin = window.frame.origin
      if x.isFinite { origin.x = visible.minX + x }
      if y.isFinite { origin.y = visible.maxY - y - window.frame.height }
      window.setFrameOrigin(origin)
    } else if options.bool("center", true) {
      window.center()
    }
    if options.bool("fullscreen", false) { window.toggleFullScreen(nil) }
  }

  private func placeTrafficLights(_ position: Options) {
    guard let close = window.standardWindowButton(.closeButton),
      let minimize = window.standardWindowButton(.miniaturizeButton),
      let zoom = window.standardWindowButton(.zoomButton), let bar = close.superview
    else { return }
    let x = position.number("x", 12)
    let spacing = minimize.frame.minX - close.frame.minX
    let top = bar.bounds.height - position.number("y", 12) - close.frame.height
    for (index, button) in [close, minimize, zoom].enumerated() {
      button.setFrameOrigin(NSPoint(x: x + spacing * Double(index), y: top))
    }
  }

  func attach(_ view: NSView) {
    browserView = view
    view.frame = container.bounds
    view.autoresizingMask = [.width, .height]
    container.addSubview(view)
    dragRegions.frame = container.bounds
    dragRegions.autoresizingMask = [.width, .height]
    container.addSubview(dragRegions)
    window.makeFirstResponder(view)
  }

  func detach() {
    dragRegions.removeFromSuperview()
    browserView?.removeFromSuperview()
    browserView = nil
  }

  var bounds: [Double] {
    let visible = (window.screen ?? NSScreen.main)?.visibleFrame ?? .zero
    let content = window.contentRect(forFrameRect: window.frame)
    return [window.frame.minX - visible.minX, visible.maxY - window.frame.maxY, content.width, content.height]
  }

  func perform(_ operation: String, text: String, flag: Bool) -> Bool {
    switch operation {
    case "ready" where !showWhenReady: break
    case "show", "ready":
      showWhenReady = false
      window.makeKeyAndOrderFront(nil)
      NSApp.activate(ignoringOtherApps: true)
    case "hide":
      showWhenReady = false
      window.orderOut(nil)
    case "minimize": window.miniaturize(nil)
    case "restore": window.deminiaturize(nil)
    case "maximize": window.zoom(nil)
    case "setAlwaysOnTop": window.level = flag ? .floating : .normal
    case "setTitle": window.title = text
    case "setFullScreen":
      if window.styleMask.contains(.fullScreen) != flag { window.toggleFullScreen(nil) }
    default: return false
    }
    return true
  }

  func windowShouldClose(_ sender: NSWindow) -> Bool {
    AntDesktop_OnWindowShouldClose(id)
  }

  func windowDidMove(_ notification: Notification) {
    AntDesktop_OnWindowEvent(id, "move")
  }

  func windowDidResize(_ notification: Notification) {
    AntDesktop_OnWindowEvent(id, "resize")
  }

  func windowWillClose(_ notification: Notification) {
    detach()
    AntDesktop_OnWindowClosed(id)
    DispatchQueue.main.async { WindowController.all[self.id] = nil }
  }
}

@MainActor @_cdecl("AntDesktop_Window_Create")
func windowCreate(_ id: Int64, _ json: UnsafePointer<CChar>) -> Bool {
  WindowController.all[id] = WindowController(id: id, options: Options(json: String(cString: json)))
  return true
}

@MainActor @_cdecl("AntDesktop_Window_ContentView")
func windowContentView(_ id: Int64) -> UnsafeMutableRawPointer? {
  WindowController.all[id].map { Unmanaged.passUnretained($0.container).toOpaque() }
}

@MainActor @_cdecl("AntDesktop_Window_Attach")
func windowAttach(_ id: Int64, _ view: UnsafeMutableRawPointer) {
  WindowController.all[id]?.attach(Unmanaged<NSView>.fromOpaque(view).takeUnretainedValue())
}

@MainActor @_cdecl("AntDesktop_Window_Detach")
func windowDetach(_ id: Int64) {
  WindowController.all[id]?.detach()
}

@MainActor @_cdecl("AntDesktop_Window_SetRegions")
func windowSetRegions(_ id: Int64, _ regions: UnsafePointer<AntDesktopRegion>?, _ count: Int) {
  WindowController.all[id]?.dragRegions.regions = Array(UnsafeBufferPointer(start: regions, count: count))
}

@MainActor @_cdecl("AntDesktop_Window_Perform")
func windowPerform(_ id: Int64, _ operation: UnsafePointer<CChar>, _ text: UnsafePointer<CChar>, _ flag: Bool) -> Bool {
  WindowController.all[id]?.perform(String(cString: operation), text: String(cString: text), flag: flag) ?? false
}

@MainActor @_cdecl("AntDesktop_Window_Bounds")
func windowBounds(_ id: Int64, _ bounds: UnsafeMutablePointer<Double>) -> Bool {
  guard let values = WindowController.all[id]?.bounds else { return false }
  for (index, value) in values.enumerated() { bounds[index] = value }
  return true
}

@MainActor @_cdecl("AntDesktop_Window_Close")
func windowClose(_ id: Int64) {
  WindowController.all[id]?.window.close()
}
