import AppKit

@objc(AntDesktopApplication)
final class Application: NSApplication, CefAppProtocol {
  private var handlingSendEvent = false
  var launched = false

  func isHandlingSendEvent() -> Bool {
    handlingSendEvent
  }

  func setHandlingSendEvent(_ handling: Bool) {
    handlingSendEvent = handling
  }

  override func sendEvent(_ event: NSEvent) {
    let previous = handlingSendEvent
    handlingSendEvent = true
    defer { handlingSendEvent = previous }
    super.sendEvent(event)
  }

  override func terminate(_ sender: Any?) {
    AntDesktop_OnTerminate()
  }

  func launch() {
    if launched { return }
    launched = true
    finishLaunching()
  }
}

func duplicate(_ value: String?) -> UnsafeMutablePointer<CChar>? {
  value.flatMap { strdup($0) }
}

var applicationName: String {
  let info = Bundle.main.infoDictionary
  return info?["CFBundleDisplayName"] as? String ?? info?["CFBundleName"] as? String ?? ProcessInfo.processInfo.processName
}

func userDirectory(_ directory: FileManager.SearchPathDirectory) -> String? {
  FileManager.default.urls(for: directory, in: .userDomainMask).first?.path
}

@MainActor @_cdecl("AntDesktop_App_Initialize")
func appInitialize() {
  _ = Application.shared
  NSApp.setActivationPolicy(.regular)
}

@MainActor @_cdecl("AntDesktop_App_FinishLaunching")
func appFinishLaunching() {
  (NSApp as? Application)?.launch()
}

@MainActor @_cdecl("AntDesktop_App_Run")
func appRun() {
  NSApp.run()
}

@MainActor @_cdecl("AntDesktop_App_Stop")
func appStop() {
  DispatchQueue.main.async {
    NSApp.stop(nil)
    let wake = NSEvent.otherEvent(
      with: .applicationDefined, location: .zero, modifierFlags: [], timestamp: 0, windowNumber: 0, context: nil,
      subtype: 0, data1: 0, data2: 0)
    if let wake { NSApp.postEvent(wake, atStart: false) }
  }
}

@MainActor @_cdecl("AntDesktop_App_Terminate")
func appTerminate() {
  NSApp.terminate(nil)
}

@_cdecl("AntDesktop_App_Info")
func appInfo(_ key: UnsafePointer<CChar>) -> UnsafeMutablePointer<CChar>? {
  duplicate(Bundle.main.object(forInfoDictionaryKey: String(cString: key)) as? String)
}

@_cdecl("AntDesktop_App_Path")
func appPath(_ name: UnsafePointer<CChar>) -> UnsafeMutablePointer<CChar>? {
  switch String(cString: name) {
  case "home": return duplicate(NSHomeDirectory())
  case "temp": return duplicate(NSTemporaryDirectory())
  case "appData": return duplicate(userDirectory(.applicationSupportDirectory))
  case "userData": return duplicate(userDirectory(.applicationSupportDirectory).map { "\($0)/\(applicationName)" })
  case "desktop": return duplicate(userDirectory(.desktopDirectory))
  case "documents": return duplicate(userDirectory(.documentDirectory))
  case "downloads": return duplicate(userDirectory(.downloadsDirectory))
  case "resources": return duplicate(Bundle.main.resourcePath)
  case "exe": return duplicate(Bundle.main.executablePath)
  case "bundle": return duplicate(Bundle.main.bundlePath)
  default: return nil
  }
}
