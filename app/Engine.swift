import AppKit
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

/// Reads a fixed-size C float array as [Float].
func floatArray<T>(_ tuple: T, count: Int) -> [Float] {
    var copy = tuple
    return withUnsafeBytes(of: &copy) { raw in
        Array(raw.bindMemory(to: Float.self).prefix(count))
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
let latencyChoicesMs = [2.0, 4.0, 5.0, 10.0, 20.0, 40.0]
let channelChoices = [2, 8, 16, 32, 48, 64]

/// The Dante engine's log, for test reports.
let logURL: URL = {
    let logs = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask).first!
        .appendingPathComponent("Logs/Jose Virtual Soundcard", isDirectory: true)
    try? FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true)
    return logs.appendingPathComponent("dante.log")
}()

/// Everything the window shows. Polls the C engine a few times a second.
final class EngineModel: ObservableObject {
    private let session = jvs_create()
    private var timer: Timer?
    private let defaults = UserDefaults.standard

    @Published var ports: [NetPort] = []
    @Published var running = false
    @Published var errorText: String?
    @Published var status = jvs_status()
    @Published var peaks: [Float] = []
    @Published var lastRecording: String?
    @Published var controllerRequests = 0

    // Settings (saved between launches)
    @Published var selectedPort: String { didSet { defaults.set(selectedPort, forKey: "port") } }
    @Published var deviceName: String { didSet { defaults.set(deviceName, forKey: "deviceName") } }
    @Published var sampleRate: Int { didSet { defaults.set(sampleRate, forKey: "sampleRate") } }
    @Published var latencyMs: Double { didSet { defaults.set(latencyMs, forKey: "latencyMs") } }
    @Published var rxChannels: Int { didSet { defaults.set(rxChannels, forKey: "rxChannels") } }
    @Published var txChannels: Int { didSet { defaults.set(txChannels, forKey: "txChannels") } }
    @Published var clock: ClockChoice { didSet { defaults.set(clock.rawValue, forKey: "clock") } }

    // Test tools
    @Published var monitorLeft = 1
    @Published var monitorRight = 2

    init() {
        // Start each launch with a fresh log.
        try? FileManager.default.removeItem(at: logURL)
        jvs_set_log_file(logURL.path)
        selectedPort = defaults.string(forKey: "port") ?? ""
        deviceName = defaults.string(forKey: "deviceName") ?? "Jose-Virtual-Soundcard"
        let rate = defaults.integer(forKey: "sampleRate")
        sampleRate = sampleRates.contains(rate) ? rate : 48000
        let lat = defaults.double(forKey: "latencyMs")
        latencyMs = latencyChoicesMs.contains(lat) ? lat : 10
        let rx = defaults.object(forKey: "rxChannels") as? Int ?? 64
        rxChannels = channelChoices.contains(rx) ? rx : 64
        let tx = defaults.object(forKey: "txChannels") as? Int ?? 64
        txChannels = channelChoices.contains(tx) ? tx : 64
        clock = ClockChoice(rawValue: defaults.integer(forKey: "clock")) ?? .automatic
        refreshPorts()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            self?.poll()
        }
    }

    deinit {
        timer?.invalidate()
        jvs_destroy(session)
    }

    func refreshPorts() {
        var buf = [jvs_interface](repeating: jvs_interface(), count: 32)
        let n = Int(jvs_list_interfaces(&buf, Int32(buf.count)))
        ports = buf.prefix(n).map { NetPort(name: cString($0.name), ip: cString($0.ip)) }
        if !ports.contains(where: { $0.name == selectedPort }) {
            selectedPort = ports.first?.name ?? ""
        }
    }

    /// The name as Dante Controller will show it.
    var cleanName: String {
        var out = [CChar](repeating: 0, count: 32)
        jvs_clean_name(deviceName, &out, Int32(out.count))
        return String(cString: out)
    }

    func start() {
        guard !selectedPort.isEmpty else { return }
        let name = cleanName
        deviceName = name
        let ok: Int32 = name.withCString { namePtr in
            selectedPort.withCString { portPtr in
                var cfg = jvs_config()
                cfg.name = namePtr
                cfg.iface = portPtr
                cfg.rx_channels = Int32(rxChannels)
                cfg.tx_channels = Int32(txChannels)
                cfg.sample_rate = Int32(sampleRate)
                cfg.latency_us = Int32(latencyMs * 1000)
                cfg.ptp_mode = Int32(clock.rawValue)
                return jvs_start(session, &cfg)
            }
        }
        if ok == 0 {
            running = true
            errorText = nil
        } else {
            running = false
            errorText = String(cString: jvs_last_error(session))
        }
    }

    func stop() {
        jvs_stop(session)
        running = false
        status = jvs_status()
        peaks = []
    }

    // MARK: Status text

    var deviceText: String {
        if !running { return "Off" }
        switch Int(status.state) {
        case Int(JVS_WAITING_FOR_CLOCK): return "Waiting for the Dante clock"
        case Int(JVS_STARTING): return "Starting"
        case Int(JVS_RUNNING): return "On the network as “\(cString(status.name))”"
        case Int(JVS_FAILED): return "Stopped with an error"
        default: return "Off"
        }
    }

    var deviceColor: NSColor {
        if !running { return .tertiaryLabelColor }
        switch Int(status.state) {
        case Int(JVS_RUNNING): return .systemGreen
        case Int(JVS_FAILED): return .systemRed
        default: return .systemOrange
        }
    }

    var clockText: String {
        if !running { return "Not started" }
        let v = status.ptp_version > 0 ? "PTPv\(status.ptp_version) " : ""
        if status.locked != 0 { return "\(v)locked" }
        if status.have_master != 0 { return "\(v)locking…" }
        return "Looking for a clock leader…"
    }

    // MARK: Test tools

    func setTones(_ on: Bool) {
        jvs_set_tones(session, on ? 1 : 0)
    }

    func setMonitor(_ on: Bool) {
        if jvs_set_monitor(session, on ? 1 : 0, Int32(monitorLeft), Int32(monitorRight)) != 0 {
            errorText = String(cString: jvs_last_error(session))
        }
    }

    func updateMonitorChannels() {
        if status.monitoring != 0 { setMonitor(true) }
    }

    func startRecording() {
        let path = recordingPath()
        if jvs_record_start(session, path) == 0 {
            lastRecording = path
            errorText = nil
        } else {
            errorText = String(cString: jvs_last_error(session))
        }
    }

    func stopRecording() {
        jvs_record_stop(session)
    }

    private func recordingPath() -> String {
        let music = FileManager.default.urls(for: .musicDirectory, in: .userDomainMask).first!
        let dir = music.appendingPathComponent("Jose Virtual Soundcard", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH.mm.ss"
        return dir.appendingPathComponent("Dante \(f.string(from: Date())).wav").path
    }

    func showRecording() {
        if let p = lastRecording {
            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: p)])
        }
    }

    // MARK: Polling

    private func poll() {
        guard running else { return }
        var s = jvs_status()
        jvs_get_status(session, &s)
        status = s
        controllerRequests = Int(jvs_controller_requests(session))
        let fresh = floatArray(s.peak_db, count: Int(s.rx_channels))
        // Meters fall back slowly so short peaks stay visible.
        if peaks.count != fresh.count {
            peaks = fresh
        } else {
            peaks = zip(peaks, fresh).map { max($1, $0 - 6) }
        }
    }

    // MARK: Report

    /// A plain-text summary to paste into the project thread.
    func report() -> String {
        var lines: [String] = []
        let v = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        lines.append("Jose Virtual Soundcard test app \(v) (Dante mode), macOS \(ProcessInfo.processInfo.operatingSystemVersionString)")
        lines.append("Network port: \(selectedPort)")
        lines.append("Settings: name \(cleanName), \(rxChannels) in / \(txChannels) out, \(sampleRate) Hz, latency \(latencyMs) ms, clock \(clock.label)")
        lines.append("Device: \(deviceText) (state \(status.state))")
        lines.append(String(format: "Clock: %@, leader %@ via %@, delay %.1f us, jitter %.1f us, rate %+.2f ppm, syncs %llu",
                            clockText, cString(status.grandmaster), cString(status.master_ip), status.delay_us,
                            status.jitter_us, status.rate_ppm, status.syncs))
        lines.append("Test tones: \(status.tones != 0 ? "on" : "off"), speakers: \(status.monitoring != 0 ? "on" : "off"), recording: \(status.recording != 0 ? "on" : "off")")
        let active = peaks.enumerated().filter { $0.element > -100 }
        if active.isEmpty {
            lines.append("Received levels: all channels silent")
        } else {
            lines.append("Received levels: " + active.map { String(format: "%d:%.0f", $0.offset + 1, $0.element) }
                .joined(separator: " ") + " dBFS (others silent)")
        }
        if let e = errorText { lines.append("Error: \(e)") }
        lines.append("")
        lines.append(diagnostics())
        let log = (try? String(contentsOf: logURL, encoding: .utf8)) ?? ""
        let tail = log.split(separator: "\n", omittingEmptySubsequences: true).suffix(30)
        lines.append("Last log lines:")
        lines.append(tail.isEmpty ? "  (empty)" : tail.map { "  " + $0 }.joined(separator: "\n"))
        return lines.joined(separator: "\n")
    }

    func diagnostics() -> String {
        var buf = [CChar](repeating: 0, count: 16384)
        jvs_get_diagnostics(session, &buf, Int32(buf.count))
        return String(cString: buf)
    }

    func showLog() {
        NSWorkspace.shared.activateFileViewerSelecting([logURL])
    }

    func copyReport() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(report(), forType: .string)
    }
}
