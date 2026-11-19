import AppKit

final class MenuClick: NSObject {
  static let shared = MenuClick()

  @objc func invoke(_ sender: NSMenuItem) {
    AntDesktop_OnMenuClick(Int64(sender.tag))
  }
}

private let appRoles: [String: Selector] = [
  "about": #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
  "quit": #selector(NSApplication.terminate(_:)),
  "hide": #selector(NSApplication.hide(_:)),
  "hideothers": #selector(NSApplication.hideOtherApplications(_:)),
  "unhide": #selector(NSApplication.unhideAllApplications(_:)),
]

private let responderRoles: [String: Selector] = [
  "close": #selector(NSWindow.performClose(_:)),
  "minimize": #selector(NSWindow.performMiniaturize(_:)),
  "zoom": #selector(NSWindow.performZoom(_:)),
  "togglefullscreen": #selector(NSWindow.toggleFullScreen(_:)),
  "undo": Selector(("undo:")),
  "redo": Selector(("redo:")),
  "cut": #selector(NSText.cut(_:)),
  "copy": #selector(NSText.copy(_:)),
  "paste": #selector(NSText.paste(_:)),
  "selectall": #selector(NSText.selectAll(_:)),
]

private let modifiers: [String: NSEvent.ModifierFlags] = [
  "cmd": .command, "command": .command, "cmdorctrl": .command, "commandorcontrol": .command,
  "ctrl": .control, "control": .control, "shift": .shift, "alt": .option, "option": .option,
]

private let namedKeys = ["comma": ",", "plus": "+", "space": " "]

@MainActor
func buildMenu(_ items: [[String: Any]], title: String) -> NSMenu {
  let menu = NSMenu(title: title)
  for values in items {
    let options = Options(values: values)
    if options.string("type") == "separator" {
      menu.addItem(.separator())
      continue
    }
    let role = options.string("role")?.lowercased()
    let label = options.string("label") ?? role ?? ""
    let item = NSMenuItem(title: label, action: nil, keyEquivalent: "")
    if let id = values["id"] as? Int {
      item.tag = id
      item.target = MenuClick.shared
      item.action = #selector(MenuClick.invoke(_:))
    } else if let role, let action = appRoles[role] {
      item.target = NSApp
      item.action = action
    } else if let role {
      item.action = responderRoles[role]
    }
    item.isEnabled = options.bool("enabled", true)
    item.isHidden = !options.bool("visible", true)
    item.state = options.bool("checked", false) ? .on : .off
    if let accelerator = options.string("accelerator"), !accelerator.isEmpty {
      let parts = accelerator.lowercased().split(separator: "+", omittingEmptySubsequences: false).map(String.init)
      let key = parts.last ?? ""
      item.keyEquivalentModifierMask = parts.reduce(into: []) { mask, part in mask.formUnion(modifiers[part] ?? []) }
      item.keyEquivalent = namedKeys[key] ?? key
    }
    if let submenu = values["submenu"] as? [[String: Any]] { item.submenu = buildMenu(submenu, title: label) }
    menu.addItem(item)
  }
  return menu
}

@MainActor @_cdecl("AntDesktop_Menu_Set")
func menuSet(_ json: UnsafePointer<CChar>?) {
  guard let json, let items = try? JSONSerialization.jsonObject(with: Data(String(cString: json).utf8)) as? [[String: Any]]
  else {
    NSApp.mainMenu = nil
    return
  }
  NSApp.mainMenu = buildMenu(items, title: "")
}
