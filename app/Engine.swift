import AppKit
import CoreAudio
import Foundation

/// Reads a fixed-size C char array (imported by Swift as a tuple) as a String.
func cString<T>(_ tuple: T) -> String {
    var copy = tuple
    return withUnsafeBytes(of: &copy) { raw in
        let bytes = raw.bindMemory(to: UInt8.self)
        let end = bytes.firstIndex(of: 0) ?? bytes.count
        return String(decoding: bytes[0..<end], as: UTF8.self)
    }
}

struct NetPort: Identifiable, Hashable {
    let name: String
    let ip: String
    var id: String { name }
    var label: String { "\(name)  (\(ip))" }
}

enum ClockChoice: Int, CaseIterable, Identifiable {
    case automatic = 0, ptpv1 = 1, ptpv2 = 2
    var id: Int { rawValue }
    var label: String {
        switch self {
        case .automatic: return "Automatic"
        case .ptpv1: return "PTPv1 (Dante)"
        case .ptpv2: return "PTPv2 (AES67)"
        }
    }
}

let sampleRates = [44100, 48000, 88200, 96000]
let latencyChoicesMs = [4.0, 6.0, 10.0, 20.0, 40.0]  // like Dante Virtual Soundcard; below 4 ms the Mac can't keep up
let channelChoices = [2, 8, 16, 32, 48, 64]

/// What the driver reports (its 'jsts' property).
struct DriverStatus: Decodable {
    var version = ""
    var state = "off"
    var error = ""
    var rate = 48000
    var io_running = false
    var tones = false
    var name: String?
    var ptp_version: Int?
    var have_master: Bool?
    var locked: Bool?
    var master_ip: String?
    var grandmaster: String?
    var delay_us: Double?
    var jitter_us: Double?
    var rate_ppm: Double?
    var syncs: Int?
    var arc_port: Int?
    var rx: Int?
    var tx: Int?
    var peak_db: [Double]?
}

/// Talks to the Jose Virtual Soundcard driver through Core Audio.
enum Driver {
    static let uid = "JoseVirtualSoundcard_UID" as CFString
    static let config: AudioObjectPropertySelector = 0x6A63_6667  // 'jcfg'
    static let status: AudioObjectPropertySelector = 0x6A73_7473  // 'jsts'
    static let log: AudioObjectPropertySelector = 0x6A6C_6F67     // 'jlog'

    static func address(_ sel: AudioObjectPropertySelector) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: sel, mScope: kAudioObjectPropertyScopeGlobal,
                                   mElement: kAudioObjectPropertyElementMain)
    }

    /// The driver's device, or nil if the driver isn't installed.
    static func device() -> AudioObjectID? {
        var addr = address(kAudioHardwarePropertyTranslateUIDToDevice)
        var uidRef = uid
        var dev = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        let st = withUnsafeMutablePointer(to: &uidRef) { p in
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr,
                                       UInt32(MemoryLayout<CFString>.size), p, &size, &dev)
        }
        return st == noErr && dev != kAudioObjectUnknown ? dev : nil
    }

    static func string(_ dev: AudioObjectID, _ sel: AudioObjectPropertySelector) -> String? {
        var addr = address(sel)
        var ref: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(dev, &addr, 0, nil, &size, &ref) == noErr, let r = ref else { return nil }
        return r.takeRetainedValue() as String
    }

    static func settings(_ dev: AudioObjectID) -> [String: Any]? {
        var addr = address(config)
        var ref: Unmanaged<CFPropertyList>?
        var size = UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size)
        guard AudioObjectGetPropertyData(dev, &addr, 0, nil, &size, &ref) == noErr, let r = ref else { return nil }
        return r.takeRetainedValue() as? [String: Any]
    }

    @discardableResult
    static func set(_ dev: AudioObjectID, _ values: [String: Any]) -> OSStatus {
        var addr = address(config)
        var plist: CFPropertyList = values as CFDictionary
        return withUnsafeMutablePointer(to: &plist) { p in
            AudioObjectSetPropertyData(dev, &addr, 0, nil, UInt32(MemoryLayout<CFPropertyList>.size), p)
        }
    }
}

/// The driver's log, saved for "Show log".
let logURL: URL = {
    let logs = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask).first!
        .appendingPathComponent("Logs/Jose Virtual Soundcard", isDirectory: true)
    try? FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true)
    return logs.appendingPathComponent("driver.log")
}()

/// Everything the window shows. Polls the driver a few times a second.
final class EngineModel: ObservableObject {
    private var timer: Timer?
    private var device: AudioObjectID?

    @Published var driverInstalled = true
    @Published var ports: [NetPort] = []
    @Published var running = false
    @Published var errorText: String?
    @Published var status = DriverStatus()
    @Published var peaks: [Float] = []
    @Published var controllerRequests = 0

    // Settings (kept by the driver)
    @Published var selectedPort = ""
    @Published var deviceName = "Jose-Virtual-Soundcard"
    @Published var sampleRate = 48000
    @Published var latencyMs = 10.0
    @Published var rxChannels = 64
    @Published var txChannels = 64
    @Published var clock = ClockChoice.automatic

    init() {
        refreshPorts()
        loadSettings()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            self?.poll()
        }
    }

    deinit { timer?.invalidate() }

    func refreshPorts() {
        var buf = [jvs_interface](repeating: jvs_interface(), count: 32)
        let n = Int(jvs_list_interfaces(&buf, Int32(buf.count)))
        ports = buf.prefix(n).map { NetPort(name: cString($0.name), ip: cString($0.ip)) }
        if !ports.contains(where: { $0.name == selectedPort }) {
            selectedPort = ports.first?.name ?? ""
        }
    }

    private func findDevice() -> AudioObjectID? {
        if device == nil { device = Driver.device() }
        driverInstalled = device != nil
        return device
    }

    /// Shows the settings the driver has saved.
    func loadSettings() {
        guard let dev = findDevice(), let s = Driver.settings(dev) else { return }
        if let v = s["iface"] as? String, !v.isEmpty { selectedPort = v }
        if let v = s["name"] as? String, !v.isEmpty { deviceName = v }
        if let v = s["rate"] as? Int, sampleRates.contains(v) { sampleRate = v }
        if let v = s["latency_us"] as? Int, latencyChoicesMs.contains(Double(v) / 1000) { latencyMs = Double(v) / 1000 }
        if let v = s["rx"] as? Int, channelChoices.contains(v) { rxChannels = v }
        if let v = s["tx"] as? Int, channelChoices.contains(v) { txChannels = v }
        if let v = s["ptp"] as? Int, let c = ClockChoice(rawValue: v) { clock = c }
        let enabled = (s["enabled"] as? Bool) ?? false
        running = enabled && !((s["iface"] as? String) ?? "").isEmpty
    }

    /// The name as Dante Controller will show it.
    var cleanName: String {
        var out = [CChar](repeating: 0, count: 32)
        jvs_clean_name(deviceName, &out, Int32(out.count))
        return String(cString: out)
    }

    func start() {
        guard !selectedPort.isEmpty else { return }
        guard let dev = findDevice() else {
            errorText = "The Jose Virtual Soundcard driver isn't installed. Run the installer (.pkg) first."
            return
        }
        deviceName = cleanName
        let st = Driver.set(dev, [
            "enabled": true,
            "name": deviceName,
            "iface": selectedPort,
            "rate": sampleRate,
            "latency_us": Int(latencyMs * 1000),
            "ptp": clock.rawValue,
            "rx": rxChannels,
            "tx": txChannels,
        ])
        if st == noErr {
            running = true
            errorText = nil
        } else {
            errorText = "The driver didn't accept the settings (error \(st))."
        }
    }

    func stop() {
        if let dev = findDevice() { Driver.set(dev, ["enabled": false, "tones": false]) }
        running = false
        peaks = []
    }

    // MARK: Status text

    var isRunning: Bool { running && status.state == "running" }

    var deviceText: String {
        if !driverInstalled { return "Driver not installed" }
        if !running { return "Off" }
        switch status.state {
        case "waiting_for_clock": return "Waiting for the Dante clock"
        case "starting": return "Starting"
        case "running": return "On the network as “\(status.name ?? deviceName)”"
        case "failed": return "Stopped with an error"
        default: return "Starting"
        }
    }

    var deviceColor: NSColor {
        if !running || !driverInstalled { return .tertiaryLabelColor }
        switch status.state {
        case "running": return .systemGreen
        case "failed": return .systemRed
        default: return .systemOrange
        }
    }

    var clockText: String {
        if !running { return "Not started" }
        let v = (status.ptp_version ?? 0) > 0 ? "PTPv\(status.ptp_version!) " : ""
        if status.locked == true { return "\(v)locked" }
        if status.have_master == true { return "\(v)locking…" }
        return "Looking for a clock leader…"
    }

    // MARK: Test tools

    func setTones(_ on: Bool) {
        if let dev = findDevice() { Driver.set(dev, ["tones": on]) }
        status.tones = on
    }

    // MARK: Polling

    private func poll() {
        guard let dev = findDevice() else { return }
        guard let json = Driver.string(dev, Driver.status) else {
            device = nil  // Core Audio restarted; look the device up again
            return
        }
        if let data = json.data(using: .utf8), let s = try? JSONDecoder().decode(DriverStatus.self, from: data) {
            status = s
            if !s.error.isEmpty && running { errorText = s.error } else if s.error.isEmpty && errorText != nil && running {
                errorText = nil
            }
            let fresh = (s.peak_db ?? []).map { Float($0) }
            if peaks.count != fresh.count {
                peaks = fresh
            } else {
                peaks = zip(peaks, fresh).map { max($1, $0 - 6) }
            }
        }
        if running, let port = status.arc_port, let log = Driver.string(dev, Driver.log) {
            controllerRequests = Self.requests(on: port, in: log)
        }
    }

    /// Requests Dante Controller sent to the routing port ("  24440: 12 in, 12 out").
    static func requests(on port: Int, in log: String) -> Int {
        for line in log.split(separator: "\n") {
            let t = line.trimmingCharacters(in: .whitespaces)
            if t.hasPrefix("\(port):") {
                let rest = t.dropFirst("\(port):".count).trimmingCharacters(in: .whitespaces)
                return Int(rest.split(separator: " ").first ?? "") ?? 0
            }
        }
        return 0
    }

    // MARK: Report

    /// A plain-text summary to paste into the project thread.
    func report() -> String {
        var lines: [String] = []
        let v = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        lines.append("Jose Virtual Soundcard \(v) (driver \(status.version.isEmpty ? "not found" : status.version)), macOS \(ProcessInfo.processInfo.operatingSystemVersionString)")
        lines.append("Network port: \(selectedPort)")
        lines.append("Settings: name \(cleanName), \(rxChannels) in / \(txChannels) out, \(sampleRate) Hz, latency \(latencyMs) ms, clock \(clock.label)")
        lines.append("Device: \(deviceText) (state \(status.state)), audio apps using it: \(status.io_running ? "yes" : "no")")
        lines.append(String(format: "Clock: %@, leader %@ via %@, delay %.1f us, jitter %.1f us, rate %+.2f ppm, syncs %d",
                            clockText, status.grandmaster ?? "-", status.master_ip ?? "-", status.delay_us ?? 0,
                            status.jitter_us ?? 0, status.rate_ppm ?? 0, status.syncs ?? 0))
        lines.append("Test tones: \(status.tones ? "on" : "off")")
        let active = peaks.enumerated().filter { $0.element > -100 }
        if active.isEmpty {
            lines.append("Received levels: all channels silent")
        } else {
            lines.append("Received levels: " + active.map { String(format: "%d:%.0f", $0.offset + 1, $0.element) }
                .joined(separator: " ") + " dBFS (others silent)")
        }
        if let e = errorText { lines.append("Error: \(e)") }
        lines.append("")
        lines.append(Self.portsInUse())
        let log = device.flatMap { Driver.string($0, Driver.log) } ?? "(driver log not available)"
        let tail = log.split(separator: "\n", omittingEmptySubsequences: true).suffix(40)
        lines.append("Driver log (last lines):")
        lines.append(tail.map { "  " + $0 }.joined(separator: "\n"))
        return lines.joined(separator: "\n")
    }

    /// Which programs hold the Dante ports on this Mac.
    static func portsInUse() -> String {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/sbin/netstat")
        p.arguments = ["-anv", "-p", "udp"]
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = Pipe()
        guard (try? p.run()) != nil else { return "Dante ports in use: (netstat failed)" }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        let ports = [".319 ", ".320 ", ".4440 ", ".4455 ", ".8700 ", ".8702 ", ".8708 ", ".8800 ", ".2444"]
        let lines = String(decoding: data, as: UTF8.self).split(separator: "\n")
            .filter { l in ports.contains { l.contains($0) } }
            .map { "  " + $0 }
        return "Dante ports in use on this Mac:\n" + lines.joined(separator: "\n")
    }

    func showLog() {
        let text = device.flatMap { Driver.string($0, Driver.log) } ?? "(driver log not available)"
        try? text.write(to: logURL, atomically: true, encoding: .utf8)
        NSWorkspace.shared.activateFileViewerSelecting([logURL])
    }

    func copyReport() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(report(), forType: .string)
    }
}
