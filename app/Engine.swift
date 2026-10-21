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

struct StreamItem: Identifiable, Hashable {
    let key: String
    let name: String
    let addr: String
    let origin: String
    let channels: Int
    let sampleRate: Int
    let bits: Int
    let ptimeMs: Double
    var id: String { key }
    var detail: String {
        "\(channels) ch · \(bits)-bit · \(sampleRate / 1000) kHz · \(addr) · from \(origin)"
    }
}

/// Everything the window shows. Polls the C engine a few times a second.
final class EngineModel: ObservableObject {
    private let session = jvs_create()
    private var timer: Timer?

    @Published var ports: [NetPort] = []
    @Published var selectedPort: String = UserDefaults.standard.string(forKey: "port") ?? ""
    @Published var running = false
    @Published var errorText: String?

    // Clock
    @Published var clock = jvs_clock_status()

    // Receive
    @Published var streams: [StreamItem] = []
    @Published var selectedStream: String?
    @Published var latencyMs: Double = 4
    @Published var playOnSpeakers = true
    @Published var playLeft = 1
    @Published var playRight = 2
    @Published var record = false
    @Published var rx = jvs_rx_status()
    @Published var peaks: [Float] = []

    // Send
    @Published var sendName = "JVS Test"
    @Published var sendFlows = 1
    @Published var tx = jvs_tx_status()

    init() {
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

    func start() {
        guard !selectedPort.isEmpty else { return }
        UserDefaults.standard.set(selectedPort, forKey: "port")
        if jvs_start(session, selectedPort) == 0 {
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
        streams = []
        selectedStream = nil
        rx = jvs_rx_status()
        tx = jvs_tx_status()
        peaks = []
    }

    var clockText: String {
        if !running { return "Not started" }
        if clock.locked != 0 { return "Locked" }
        if clock.have_master != 0 { return "Locking…" }
        return "Waiting for a PTP clock"
    }

    var clockColor: NSColor {
        if !running { return .tertiaryLabelColor }
        if clock.locked != 0 { return .systemGreen }
        return .systemOrange
    }

    // MARK: Receive

    var selectedStreamItem: StreamItem? { streams.first { $0.key == selectedStream } }

    func startReceive() {
        guard let s = selectedStreamItem else { return }
        var path = ""
        if record {
            path = recordingPath(for: s.name)
        }
        let ok = jvs_rx_start(session, s.key, latencyMs, playOnSpeakers ? 1 : 0, Int32(playLeft),
                              Int32(playRight), path)
        errorText = ok == 0 ? nil : String(cString: jvs_last_error(session))
    }

    func stopReceive() {
        jvs_rx_stop(session)
    }

    private func recordingPath(for name: String) -> String {
        let music = FileManager.default.urls(for: .musicDirectory, in: .userDomainMask).first!
        let dir = music.appendingPathComponent("Jose Virtual Soundcard", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH.mm.ss"
        let safe = name.replacingOccurrences(of: "/", with: "-").replacingOccurrences(of: ":", with: "-")
        return dir.appendingPathComponent("\(safe) \(f.string(from: Date())).wav").path
    }

    func showRecording() {
        let p = cString(rx.wav_path)
        if !p.isEmpty {
            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: p)])
        }
    }

    // MARK: Send

    func startSend() {
        let ok = jvs_tx_start(session, sendName, 8, Int32(sendFlows), nil)
        errorText = ok == 0 ? nil : String(cString: jvs_last_error(session))
    }

    func stopSend() {
        jvs_tx_stop(session)
    }

    // MARK: Polling

    private func poll() {
        guard running else { return }
        var c = jvs_clock_status()
        jvs_get_clock(session, &c)
        clock = c

        var buf = [jvs_stream_info](repeating: jvs_stream_info(), count: 64)
        let n = Int(jvs_get_streams(session, &buf, Int32(buf.count)))
        let list = buf.prefix(n).map {
            StreamItem(key: cString($0.key), name: cString($0.name), addr: cString($0.addr),
                       origin: cString($0.origin), channels: Int($0.channels), sampleRate: Int($0.sample_rate),
                       bits: Int($0.bits), ptimeMs: $0.ptime_ms)
        }.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
        if list != streams { streams = list }
        if selectedStream == nil { selectedStream = streams.first?.key }

        var r = jvs_rx_status()
        jvs_get_rx(session, &r)
        rx = r
        let fresh = floatArray(r.peak_db, count: Int(r.channels))
        // Meters fall back slowly so short peaks stay visible.
        if peaks.count != fresh.count {
            peaks = fresh
        } else {
            peaks = zip(peaks, fresh).map { max($1, $0 - 6) }
        }

        var t = jvs_tx_status()
        jvs_get_tx(session, &t)
        tx = t
    }

    // MARK: Report

    /// A plain-text summary to paste into the project thread.
    func report() -> String {
        var lines: [String] = []
        let v = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        lines.append("Jose Virtual Soundcard test app \(v), macOS \(ProcessInfo.processInfo.operatingSystemVersionString)")
        lines.append("Network port: \(selectedPort)")
        lines.append(String(format: "Clock: %@, grandmaster %@ via %@, delay %.1f us, jitter %.1f us, rate %+.2f ppm, syncs %llu",
                            clockText, cString(clock.grandmaster), cString(clock.master_ip), clock.delay_us,
                            clock.jitter_us, clock.rate_ppm, clock.syncs))
        lines.append("Streams found: \(streams.count)")
        for s in streams { lines.append("  \(s.name): \(s.detail)") }
        if rx.active != 0 {
            lines.append(String(format: "Receiving \"%@\": %llu packets, lost %llu, missing %.2f%%, arrival %.0f us avg / %.0f us max, latency %.1f ms",
                                cString(rx.stream_name), rx.packets, rx.lost, rx.missing_pct, rx.arrival_avg_us,
                                rx.arrival_max_us, rx.latency_ms))
            lines.append("  Levels: " + peaks.enumerated().map { String(format: "%d:%.0f", $0.offset + 1, $0.element) }
                .joined(separator: " ") + " dBFS")
        } else {
            lines.append("Receiving: off")
        }
        if tx.active != 0 {
            lines.append("Sending: \(tx.flows) flow(s) x \(tx.channels) ch, \(tx.packets) packets, late \(tx.late), resyncs \(tx.resyncs)")
        } else {
            lines.append("Sending: off")
        }
        return lines.joined(separator: "\n")
    }

    func copyReport() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(report(), forType: .string)
    }
}
