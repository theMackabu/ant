import AppKit
import CryptoKit

let frameworkName = "Chromium Embedded Framework.framework"

struct RuntimeError: Error {
  let message: String
}

struct RuntimeRelease {
  let version: String
  let archive: String
  let sha1: String
  let size: Int64

  var sharedDirectory: URL {
    FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Ant Desktop/CEF/\(version)", isDirectory: true)
  }

  var url: URL {
    let name = archive.addingPercentEncoding(withAllowedCharacters: .alphanumerics.union(.init(charactersIn: "-._"))) ?? archive
    return URL(string: "https://cef-builds.spotifycdn.com/\(name)")!
  }

  func installed() -> String? {
    let candidates: [String?] = [
      ProcessInfo.processInfo.environment["ANT_DESKTOP_CEF"],
      Bundle.main.privateFrameworksPath,
      sharedDirectory.path,
    ]
    return candidates.compactMap { $0 }.map { "\($0)/\(frameworkName)" }.first {
      FileManager.default.fileExists(atPath: "\($0)/Chromium Embedded Framework")
    }
  }

  func verify(_ file: URL) throws {
    let attributes = try FileManager.default.attributesOfItem(atPath: file.path)
    guard (attributes[.size] as? NSNumber)?.int64Value == size else {
      throw RuntimeError(message: "The downloaded browser runtime has the wrong size.")
    }
    let handle = try FileHandle(forReadingFrom: file)
    defer { try? handle.close() }
    var hash = Insecure.SHA1()
    while let chunk = try handle.read(upToCount: 1 << 20), !chunk.isEmpty { hash.update(data: chunk) }
    guard hash.finalize().map({ String(format: "%02x", $0) }).joined() == sha1 else {
      throw RuntimeError(message: "The downloaded browser runtime failed its checksum.")
    }
  }

  func install(from file: URL) throws {
    let files = FileManager.default
    let destination = sharedDirectory
    let staging = destination.deletingLastPathComponent().appendingPathComponent(".\(version).\(getpid())")
    try? files.removeItem(at: staging)
    try files.createDirectory(at: staging, withIntermediateDirectories: true)
    defer { try? files.removeItem(at: staging) }

    let root = archive.replacingOccurrences(of: ".tar.bz2", with: "")
    let tar = Process()
    tar.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
    tar.arguments = ["-xjf", file.path, "-C", staging.path, "--strip-components", "2", "\(root)/Release/\(frameworkName)"]
    try tar.run()
    tar.waitUntilExit()
    guard tar.terminationStatus == 0 else { throw RuntimeError(message: "The browser runtime could not be unpacked.") }
    if files.fileExists(atPath: destination.path) { return }
    try files.moveItem(at: staging, to: destination)
  }
}

@MainActor
final class RuntimeInstaller: NSObject, URLSessionDownloadDelegate {
  nonisolated let release: RuntimeRelease
  let progress = NSProgressIndicator()
  let panel = NSPanel(
    contentRect: NSRect(x: 0, y: 0, width: 420, height: 96), styleMask: [.titled], backing: .buffered, defer: false)
  var failure: String?

  init(release: RuntimeRelease) {
    self.release = release
    super.init()
    let megabytes = release.size / 1_000_000
    let label = NSTextField(labelWithString: "Downloading the browser runtime (\(megabytes) MB). This happens once.")
    label.frame = NSRect(x: 20, y: 52, width: 380, height: 20)
    progress.frame = NSRect(x: 20, y: 24, width: 380, height: 20)
    progress.isIndeterminate = false
    progress.maxValue = Double(release.size)
    panel.title = applicationName
    panel.contentView?.addSubview(label)
    panel.contentView?.addSubview(progress)
    panel.center()
  }

  func run() -> Bool {
    let session = URLSession(configuration: .ephemeral, delegate: self, delegateQueue: nil)
    session.downloadTask(with: release.url).resume()
    NSApp.activate(ignoringOtherApps: true)
    NSApp.runModal(for: panel)
    panel.orderOut(nil)
    session.invalidateAndCancel()
    guard let failure else { return true }
    let alert = NSAlert()
    alert.messageText = "\(applicationName) could not start"
    alert.informativeText = failure
    alert.runModal()
    return false
  }

  func finish(_ message: String?) {
    failure = message
    NSApp.stopModal()
  }

  nonisolated func urlSession(
    _ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64
  ) {
    DispatchQueue.main.async { self.progress.doubleValue = Double(totalBytesWritten) }
  }

  nonisolated func urlSession(
    _ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL
  ) {
    DispatchQueue.main.async {
      self.progress.isIndeterminate = true
      self.progress.startAnimation(nil)
    }
    var message: String?
    do {
      let status = (downloadTask.response as? HTTPURLResponse)?.statusCode ?? 0
      guard status == 200 else { throw RuntimeError(message: "The browser runtime download failed (HTTP \(status)).") }
      try release.verify(location)
      try release.install(from: location)
    } catch {
      message = (error as? RuntimeError)?.message ?? error.localizedDescription
    }
    DispatchQueue.main.async { self.finish(message) }
  }

  nonisolated func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
    guard let message = error?.localizedDescription else { return }
    DispatchQueue.main.async { self.finish(message) }
  }
}

@MainActor @_cdecl("AntDesktop_CEF_Resolve")
func cefResolve(
  _ version: UnsafePointer<CChar>, _ archive: UnsafePointer<CChar>, _ sha1: UnsafePointer<CChar>, _ size: Int64
) -> UnsafeMutablePointer<CChar>? {
  let release = RuntimeRelease(
    version: String(cString: version), archive: String(cString: archive), sha1: String(cString: sha1), size: size)
  if let path = release.installed() { return duplicate(path) }
  (NSApp as? Application)?.launch()
  guard RuntimeInstaller(release: release).run() else { return nil }
  return duplicate(release.installed())
}
