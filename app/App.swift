import AppKit
import SwiftUI

@main
struct JoseVirtualSoundcardApp: App {
    @StateObject private var model = EngineModel()

    var body: some Scene {
        WindowGroup("Jose Virtual Soundcard") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 640, minHeight: 680)
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var m: EngineModel
    @State private var copied = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                deviceBox
                if let e = m.errorText {
                    Label(e, systemImage: "exclamationmark.triangle.fill")
                        .foregroundColor(.red)
                        .fixedSize(horizontal: false, vertical: true)
                }
                statusBox
                receiveBox
                sendBox
                HStack {
                    Spacer()
                    Button(copied ? "Copied" : "Copy test report") {
                        m.copyReport()
                        copied = true
                        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { copied = false }
                    }
                    .help("Copies a summary you can paste into the project thread")
                }
            }
            .padding(20)
        }
    }

    // MARK: Device settings

    private var deviceBox: some View {
        GroupBox(label: Text("Dante device").font(.headline)) {
            VStack(alignment: .leading, spacing: 10) {
                VStack(alignment: .leading, spacing: 10) {
                    HStack {
                        TextField("Name in Dante Controller", text: $m.deviceName)
                            .frame(width: 240)
                        Picker("Network port", selection: $m.selectedPort) {
                            ForEach(m.ports) { p in Text(p.label).tag(p.name) }
                        }
                        .frame(maxWidth: 300)
                        Button {
                            m.refreshPorts()
                        } label: {
                            Image(systemName: "arrow.clockwise")
                        }
                        .help("Look for network ports again")
                    }
                    HStack(spacing: 16) {
                        Picker("Sample rate", selection: $m.sampleRate) {
                            ForEach(sampleRates, id: \.self) { r in
                                Text(r % 1000 == 0 ? "\(r / 1000) kHz" : String(format: "%.1f kHz", Double(r) / 1000)).tag(r)
                            }
                        }
                        .frame(width: 190)
                        Picker("Latency", selection: $m.latencyMs) {
                            ForEach(latencyChoicesMs, id: \.self) { v in Text("\(Int(v)) ms").tag(v) }
                        }
                        .frame(width: 150)
                        Picker("Clock", selection: $m.clock) {
                            ForEach(ClockChoice.allCases) { c in Text(c.label).tag(c) }
                        }
                        .frame(width: 200)
                    }
                    HStack(spacing: 16) {
                        Picker("Receive channels", selection: $m.rxChannels) {
                            ForEach(channelChoices, id: \.self) { Text("\($0)").tag($0) }
                        }
                        .frame(width: 200)
                        Picker("Send channels", selection: $m.txChannels) {
                            ForEach(channelChoices, id: \.self) { Text("\($0)").tag($0) }
                        }
                        .frame(width: 180)
                    }
                }
                .disabled(m.running)
                HStack {
                    Text(m.running
                         ? "Stop the device to change these settings."
                         : "Pick the port wired to your Dante network, then press Start. The Mac will appear in Dante Controller under this name.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    Spacer()
                    if m.running {
                        Button("Stop") { m.stop() }
                    } else {
                        Button("Start") { m.start() }
                            .keyboardShortcut(.defaultAction)
                            .disabled(m.selectedPort.isEmpty)
                    }
                }
            }
            .padding(6)
        }
    }

    // MARK: Status

    private var statusBox: some View {
        GroupBox(label: Text("Status").font(.headline)) {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Circle().fill(Color(m.deviceColor)).frame(width: 12, height: 12)
                    Text(m.deviceText).bold()
                    Spacer()
                }
                HStack {
                    Text("Clock:").foregroundColor(.secondary)
                    Text(m.clockText)
                    Spacer()
                }
                if m.running && m.status.have_master != 0 {
                    Text("Clock leader \(cString(m.status.grandmaster)) at \(cString(m.status.master_ip))")
                        .font(.system(.caption, design: .monospaced))
                    Text(String(format: "Network delay %.0f µs · jitter %.0f µs · rate %+.2f ppm",
                                m.status.delay_us, m.status.jitter_us, m.status.rate_ppm))
                        .font(.caption)
                        .foregroundColor(.secondary)
                } else if m.running {
                    Text("Waiting for a Dante device to provide the clock. Make sure this port is on the same network as your Dante gear.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if m.running && Int(m.status.state) == Int(JVS_RUNNING) {
                    Text("Open Dante Controller: “\(cString(m.status.name))” should be in the routing grid with \(m.status.rx_channels) receive and \(m.status.tx_channels) transmit channels.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .padding(6)
        }
    }

    // MARK: Receive

    private var receiveBox: some View {
        GroupBox(label: Text("Receive from Dante").font(.headline)) {
            VStack(alignment: .leading, spacing: 10) {
                Text("Route Dante channels to this Mac in Dante Controller. Their levels show here.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                HStack(spacing: 12) {
                    Toggle("Play on Mac speakers", isOn: Binding(
                        get: { m.status.monitoring != 0 },
                        set: { m.setMonitor($0) }))
                    Picker("", selection: $m.monitorLeft) {
                        ForEach(1...max(Int(m.status.rx_channels), 1), id: \.self) { Text("L: \($0)").tag($0) }
                    }
                    .frame(width: 80)
                    .onChange(of: m.monitorLeft) { _ in m.updateMonitorChannels() }
                    Picker("", selection: $m.monitorRight) {
                        ForEach(1...max(Int(m.status.rx_channels), 1), id: \.self) { Text("R: \($0)").tag($0) }
                    }
                    .frame(width: 80)
                    .onChange(of: m.monitorRight) { _ in m.updateMonitorChannels() }
                    Spacer()
                }
                HStack {
                    if m.status.recording != 0 {
                        Button("Stop recording") { m.stopRecording() }
                        Text("Recording every channel…").font(.caption).foregroundColor(.red)
                    } else {
                        Button("Record all channels") { m.startRecording() }
                            .help("Saves a WAV file in Music › Jose Virtual Soundcard")
                    }
                    if m.lastRecording != nil {
                        Button("Show recording") { m.showRecording() }
                    }
                    Spacer()
                }
                if !m.peaks.isEmpty {
                    MeterGrid(peaks: m.peaks)
                }
            }
            .padding(6)
            .disabled(!m.running || Int(m.status.state) != Int(JVS_RUNNING) || m.status.rx_channels == 0)
        }
    }

    // MARK: Send

    private var sendBox: some View {
        GroupBox(label: Text("Send to Dante").font(.headline)) {
            VStack(alignment: .leading, spacing: 10) {
                Toggle("Send test tones on every channel", isOn: Binding(
                    get: { m.status.tones != 0 },
                    set: { m.setTones($0) }))
                Text("Channel 1 plays 300 Hz, channel 2 plays 400 Hz, and so on. Route them to a Dante device in Dante Controller to hear them.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .disabled(!m.running || m.status.tx_channels == 0)
        }
    }
}

/// One small level bar per channel.
struct MeterGrid: View {
    let peaks: [Float]

    var body: some View {
        let columns = [GridItem(.adaptive(minimum: 64), spacing: 8)]
        LazyVGrid(columns: columns, alignment: .leading, spacing: 6) {
            ForEach(Array(peaks.enumerated()), id: \.offset) { item in
                let i = item.offset
                let db = item.element
                HStack(spacing: 4) {
                    Text("\(i + 1)")
                        .font(.system(size: 10, design: .monospaced))
                        .frame(width: 16, alignment: .trailing)
                    GeometryReader { g in
                        let level = CGFloat(max(0, min(1, (db + 60) / 60)))
                        ZStack(alignment: .leading) {
                            RoundedRectangle(cornerRadius: 2).fill(Color.secondary.opacity(0.2))
                            RoundedRectangle(cornerRadius: 2)
                                .fill(db > -3 ? Color.red : (db > -12 ? Color.yellow : Color.green))
                                .frame(width: g.size.width * level)
                        }
                    }
                    .frame(height: 8)
                }
                .help(db <= -119 ? "Channel \(i + 1): silent" : String(format: "Channel %d: %.0f dBFS", i + 1, db))
            }
        }
    }
}
