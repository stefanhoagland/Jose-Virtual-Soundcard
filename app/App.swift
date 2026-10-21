import AppKit
import SwiftUI

@main
struct JoseVirtualSoundcardApp: App {
    @StateObject private var model = EngineModel()

    var body: some Scene {
        WindowGroup("Jose Virtual Soundcard") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 620, minHeight: 640)
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var m: EngineModel
    @State private var copied = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                networkBox
                if let e = m.errorText {
                    Label(e, systemImage: "exclamationmark.triangle.fill")
                        .foregroundColor(.red)
                        .fixedSize(horizontal: false, vertical: true)
                }
                clockBox
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

    // MARK: Network

    private var networkBox: some View {
        GroupBox(label: Text("Network").font(.headline)) {
            HStack {
                Picker("Port", selection: $m.selectedPort) {
                    ForEach(m.ports) { p in Text(p.label).tag(p.name) }
                }
                .frame(maxWidth: 320)
                .disabled(m.running)
                Button {
                    m.refreshPorts()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .help("Look for network ports again")
                .disabled(m.running)
                Spacer()
                if m.running {
                    Button("Stop") { m.stop() }
                } else {
                    Button("Start") { m.start() }
                        .keyboardShortcut(.defaultAction)
                        .disabled(m.selectedPort.isEmpty)
                }
            }
            .padding(6)
            Text("Pick the port that's wired to your Dante network.")
                .font(.caption)
                .foregroundColor(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 6)
        }
    }

    // MARK: Clock

    private var clockBox: some View {
        GroupBox(label: Text("Clock").font(.headline)) {
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Circle().fill(Color(m.clockColor)).frame(width: 12, height: 12)
                    Text(m.clockText).bold()
                    Spacer()
                }
                if m.running && m.clock.have_master != 0 {
                    Text("Grandmaster \(cString(m.clock.grandmaster)) via \(cString(m.clock.master_ip))")
                        .font(.system(.caption, design: .monospaced))
                    Text(String(format: "Network delay %.0f µs · jitter %.0f µs · rate %+.2f ppm",
                                m.clock.delay_us, m.clock.jitter_us, m.clock.rate_ppm))
                        .font(.caption)
                        .foregroundColor(.secondary)
                } else if m.running {
                    Text("Make sure AES67 mode is turned on for a Dante device in Dante Controller.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
            .padding(6)
        }
    }

    // MARK: Receive

    private var receiveBox: some View {
        GroupBox(label: Text("Receive from Dante").font(.headline)) {
            VStack(alignment: .leading, spacing: 10) {
                if m.streams.isEmpty {
                    Text(m.running
                         ? "Looking for AES67 streams… Dante announces them about every 30 seconds."
                         : "Start the network to look for streams.")
                        .font(.callout)
                        .foregroundColor(.secondary)
                } else {
                    Picker("Stream", selection: $m.selectedStream) {
                        ForEach(m.streams) { s in
                            Text("\(s.name) — \(s.channels) ch").tag(Optional(s.key))
                        }
                    }
                    .disabled(m.rx.active != 0)
                    if let s = m.selectedStreamItem {
                        Text(s.detail).font(.caption).foregroundColor(.secondary)
                    }
                }

                HStack(spacing: 16) {
                    Picker("Latency", selection: $m.latencyMs) {
                        ForEach([1.0, 2.0, 4.0, 10.0, 20.0, 50.0], id: \.self) { v in
                            Text("\(Int(v)) ms").tag(v)
                        }
                    }
                    .frame(width: 160)
                    Toggle("Play on Mac speakers", isOn: $m.playOnSpeakers)
                    if m.playOnSpeakers {
                        Picker("", selection: $m.playLeft) {
                            ForEach(1...max(m.selectedStreamItem?.channels ?? 2, 1), id: \.self) { Text("L: \($0)").tag($0) }
                        }
                        .frame(width: 80)
                        Picker("", selection: $m.playRight) {
                            ForEach(1...max(m.selectedStreamItem?.channels ?? 2, 1), id: \.self) { Text("R: \($0)").tag($0) }
                        }
                        .frame(width: 80)
                    }
                }
                .disabled(m.rx.active != 0)
                Toggle("Record all channels to a WAV file in Music › Jose Virtual Soundcard", isOn: $m.record)
                    .disabled(m.rx.active != 0)

                HStack {
                    if m.rx.active != 0 {
                        Button("Stop listening") { m.stopReceive() }
                        if m.rx.recording != 0 {
                            Button("Show recording") { m.showRecording() }
                        }
                    } else {
                        Button("Start listening") { m.startReceive() }
                            .disabled(!m.running || m.clock.locked == 0 || m.selectedStream == nil)
                    }
                    Spacer()
                }

                if m.rx.active != 0 {
                    Text(String(format: "%llu packets · %llu lost · %.2f%% missing · arrive %.0f µs avg / %.0f µs max",
                                m.rx.packets, m.rx.lost, m.rx.missing_pct, m.rx.arrival_avg_us, m.rx.arrival_max_us))
                        .font(.caption)
                        .foregroundColor(.secondary)
                    if m.rx.arrival_max_us > m.rx.latency_ms * 1000 {
                        Text("Some packets arrive later than the latency. If you hear clicks, stop and pick a higher latency.")
                            .font(.caption)
                            .foregroundColor(.orange)
                    }
                    MeterGrid(peaks: m.peaks)
                }
            }
            .padding(6)
        }
    }

    // MARK: Send

    private var sendBox: some View {
        GroupBox(label: Text("Send to Dante").font(.headline)) {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 16) {
                    TextField("Name in Dante Controller", text: $m.sendName)
                        .frame(width: 200)
                    Picker("Channels", selection: $m.sendFlows) {
                        ForEach(1...8, id: \.self) { f in
                            Text(f == 1 ? "8 (1 flow)" : "\(f * 8) (\(f) flows)").tag(f)
                        }
                    }
                    .frame(width: 200)
                }
                .disabled(m.tx.active != 0)
                Text("Sends test tones: channel 1 is 300 Hz, channel 2 is 400 Hz, and so on. Route them in Dante Controller.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                HStack {
                    if m.tx.active != 0 {
                        Button("Stop sending") { m.stopSend() }
                        Text("\(m.tx.packets) packets · \(m.tx.late) late · \(m.tx.resyncs) resyncs")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    } else {
                        Button("Start sending") { m.startSend() }
                            .disabled(!m.running || m.clock.locked == 0)
                    }
                    Spacer()
                }
            }
            .padding(6)
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
