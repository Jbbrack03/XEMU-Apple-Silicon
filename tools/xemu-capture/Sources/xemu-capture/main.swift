// xemu-capture — native macOS CLI for the Xbox composite-out capture pipeline.
//
// Lives in a code-signed .app bundle (com.xemu-macos.capture) so macOS TCC
// tracks Camera permission by bundle ID, not by ephemeral parent process.
// One-time user grant via the system prompt; persists across sessions,
// reboots, and project rebuilds.
//
// Subcommands:
//   list                              JSON list of all video capture devices.
//   probe DEVICE                      Supported formats / resolutions for device.
//   inputs DEVICE                     List physical input sources (composite,
//                                     S-Video, etc.) on a multi-input UVC device
//                                     such as the MS2109 stick.
//   set-input DEVICE INPUT            Select active input by id or localized
//                                     name (substring-insensitive).
//   snapshot DEVICE --out PATH        Single-frame PNG capture.
//                  [--input INPUT]       Switch active input first.
//                  [--warmup-frames N]   Drop N initial frames (default 5;
//                                        composite signal lock takes a moment).
//                  [--timeout SECONDS]   Max seconds to wait (default 10).
//   sequence DEVICE --out-prefix PFX  Capture N frames spaced by SECS, write
//                  --count N             pfx.0001.png ... pfx.NNNN.png.
//                  --interval SECS       Useful for "watch the picture come in"
//                                        scenarios (signal lock, mode change,
//                                        unplug/replug auto-detect tests).
//                  [--input INPUT]       Switch active input first.
//   serve [--port N]                  TCP daemon. Accepts JSON-line ops:
//                                        {"op":"snapshot","device":"…","out":"…",
//                                         "width":720,"height":480,
//                                         "warmup_frames":5,"timeout":10,
//                                         "input":"…"}
//                                        {"op":"inputs","device":"…"}
//                                        {"op":"set-input","device":"…","input":"…"}
//                                        {"op":"list"}
//                                        {"op":"shutdown"}
//                                     Replies one JSON-line per request.
//
// Device-name matching is substring-insensitive. The MS2109 enumerates as
// "AV TO USB2.0" — any unique substring (e.g. "USB2") works.
//
// Each invocation also accepts `--json` to force machine-readable output.

import AVFoundation
import AppKit
import CoreImage
import CoreVideo
import Darwin
import Foundation
import ImageIO
import UniformTypeIdentifiers

// MARK: - Logging / output helpers

func putErr(_ s: String) { FileHandle.standardError.write(Data((s + "\n").utf8)) }

func emitJSON(_ obj: Any, file: FileHandle = .standardOutput) {
    let data = (try? JSONSerialization.data(withJSONObject: obj,
                                             options: [.prettyPrinted, .sortedKeys])) ?? Data()
    file.write(data)
    file.write(Data([0x0A]))
}

func cameraAuthorizationStatusString() -> String {
    switch AVCaptureDevice.authorizationStatus(for: .video) {
    case .authorized: return "authorized"
    case .notDetermined: return "not_determined"
    case .denied: return "denied"
    case .restricted: return "restricted"
    @unknown default: return "unknown"
    }
}

func requestCameraAccessIfNeeded() -> Bool {
    let status = AVCaptureDevice.authorizationStatus(for: .video)
    if status == .authorized {
        return true
    }
    if status != .notDetermined {
        return false
    }
    let sem = DispatchSemaphore(value: 0)
    var granted = false
    AVCaptureDevice.requestAccess(for: .video) { ok in
        granted = ok
        sem.signal()
    }
    _ = sem.wait(timeout: .now() + 60)
    return granted
}

// MARK: - Device enumeration

struct DeviceInfo {
    let uniqueID: String
    let localizedName: String
    let modelID: String
    let manufacturer: String
}

func discoverDevices() -> [AVCaptureDevice] {
    let session = AVCaptureDevice.DiscoverySession(
        deviceTypes: [
            .builtInWideAngleCamera,
            .external,
        ],
        mediaType: .video,
        position: .unspecified)
    return session.devices
}

func deviceMatching(_ pattern: String) -> AVCaptureDevice? {
    let lower = pattern.lowercased()
    let devices = discoverDevices()
    // Exact uniqueID match wins.
    if let exact = devices.first(where: { $0.uniqueID == pattern }) {
        return exact
    }
    // Substring match against localized name.
    return devices.first { $0.localizedName.lowercased().contains(lower) }
}

// MARK: - Input source selection

func describeInputSources(_ dev: AVCaptureDevice) -> [[String: Any]] {
    return dev.inputSources.map {
        [
            "id": $0.inputSourceID,
            "name": $0.localizedName,
        ]
    }
}

func setActiveInput(_ dev: AVCaptureDevice, match: String) throws -> [String: Any] {
    let lower = match.lowercased()
    let candidates = dev.inputSources.filter {
        $0.inputSourceID == match ||
        $0.inputSourceID.lowercased().contains(lower) ||
        $0.localizedName.lowercased().contains(lower)
    }
    guard let chosen = candidates.first else {
        throw SnapshotError.deviceUnavailable(
            "no input source matched '\(match)' on '\(dev.localizedName)'. " +
            "Available: \(dev.inputSources.map { "\($0.inputSourceID)/\($0.localizedName)" }.joined(separator: ", "))")
    }
    do {
        try dev.lockForConfiguration()
        defer { dev.unlockForConfiguration() }
        dev.activeInputSource = chosen
    } catch {
        throw SnapshotError.sessionStartFailed(
            "lockForConfiguration failed: \(error)")
    }
    return [
        "status": "ok",
        "device": dev.localizedName,
        "active_input_id": chosen.inputSourceID,
        "active_input_name": chosen.localizedName,
    ]
}

// MARK: - Snapshot

enum SnapshotError: Error {
    case deviceUnavailable(String)
    case sessionStartFailed(String)
    case timeout(seconds: Double)
    case writeFailed(String)
    case noFrame
}

final class SnapshotCapturer: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    private let session = AVCaptureSession()
    private let output = AVCaptureVideoDataOutput()
    private let queue = DispatchQueue(label: "xemu-capture.snapshot")
    private var framesSeen = 0
    private let warmupFrames: Int
    private var captured: CGImage?
    private let semaphore = DispatchSemaphore(value: 0)
    private var pendingFormat: AVCaptureDevice.Format?

    init(device: AVCaptureDevice, warmupFrames: Int,
         preferredWidth: Int? = nil,
         preferredHeight: Int? = nil) throws {
        self.warmupFrames = warmupFrames
        super.init()
        // For NTSC composite from an Original Xbox we want 720x480
        // or 640x480 @ 30 fps. The default sessionPreset (.high)
        // picks the largest reported format, which for the MS2109
        // is PAL 720x576@25fps — produces unusable captures (PAL
        // pedestal mismatch + wrong subcarrier on an NTSC signal).
        // Workaround: lock and set device.activeFormat to the
        // desired NTSC format BEFORE attaching to the session;
        // the session's input then carries that format forward.
        if let w = preferredWidth, let h = preferredHeight {
            let match = device.formats.first { f in
                let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
                return Int(d.width) == w && Int(d.height) == h
            }
            if let m = match {
                pendingFormat = m
                do {
                    try device.lockForConfiguration()
                    device.activeFormat = m
                    if let r = m.videoSupportedFrameRateRanges.first {
                        device.activeVideoMinFrameDuration = r.minFrameDuration
                        device.activeVideoMaxFrameDuration = r.maxFrameDuration
                    }
                    device.unlockForConfiguration()
                } catch {
                    putErr("warning: lockForConfiguration to set \(w)x\(h) failed: \(error)")
                }
            } else {
                putErr("warning: no format matches \(w)x\(h); leaving device default")
            }
        }
        session.beginConfiguration()
        let input = try AVCaptureDeviceInput(device: device)
        guard session.canAddInput(input) else {
            session.commitConfiguration()
            throw SnapshotError.sessionStartFailed("cannot add device input")
        }
        session.addInput(input)
        output.videoSettings = [
            kCVPixelBufferPixelFormatTypeKey as String:
                Int(kCVPixelFormatType_32BGRA)
        ]
        output.alwaysDiscardsLateVideoFrames = true
        output.setSampleBufferDelegate(self, queue: queue)
        guard session.canAddOutput(output) else {
            session.commitConfiguration()
            throw SnapshotError.sessionStartFailed("cannot add video output")
        }
        session.addOutput(output)
        session.commitConfiguration()
    }

    func capture(timeout: TimeInterval) throws -> CGImage {
        session.startRunning()
        defer { session.stopRunning() }
        // session.startRunning reverts device.activeFormat to whatever
        // matches sessionPreset (.high → largest area, which on the
        // MS2109 picks PAL 720x576 over NTSC 720x480 even when the
        // signal is NTSC). Re-pin the device.activeFormat under lock
        // AFTER the session is running; the session honors mid-stream
        // activeFormat changes.
        if let dev = (session.inputs.first as? AVCaptureDeviceInput)?.device,
           let pf = pendingFormat {
            do {
                try dev.lockForConfiguration()
                dev.activeFormat = pf
                if let r = pf.videoSupportedFrameRateRanges.first {
                    dev.activeVideoMinFrameDuration = r.minFrameDuration
                    dev.activeVideoMaxFrameDuration = r.maxFrameDuration
                }
                dev.unlockForConfiguration()
            } catch {
                putErr("warning: post-start activeFormat re-pin failed: \(error)")
            }
        }
        let result = semaphore.wait(timeout: .now() + timeout)
        if result == .timedOut {
            throw SnapshotError.timeout(seconds: timeout)
        }
        guard let image = captured else {
            throw SnapshotError.noFrame
        }
        return image
    }

    func captureOutput(_ output: AVCaptureOutput,
                       didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        framesSeen += 1
        if framesSeen <= warmupFrames {
            return
        }
        if captured != nil {
            return
        }
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
            return
        }
        let ciImage = CIImage(cvPixelBuffer: pixelBuffer)
        let context = CIContext()
        if let cg = context.createCGImage(ciImage, from: ciImage.extent) {
            captured = cg
            semaphore.signal()
        }
    }
}

func writePNG(_ image: CGImage, to path: String) throws {
    let url = URL(fileURLWithPath: path)
    guard let dest = CGImageDestinationCreateWithURL(
        url as CFURL, UTType.png.identifier as CFString, 1, nil) else {
        throw SnapshotError.writeFailed("CGImageDestinationCreateWithURL failed for \(path)")
    }
    CGImageDestinationAddImage(dest, image, nil)
    if !CGImageDestinationFinalize(dest) {
        throw SnapshotError.writeFailed("CGImageDestinationFinalize failed for \(path)")
    }
}

func snapshot(deviceMatch: String, outPath: String,
              warmupFrames: Int, timeout: TimeInterval,
              inputMatch: String? = nil,
              preferredWidth: Int? = nil,
              preferredHeight: Int? = nil) throws -> [String: Any] {
    guard let dev = deviceMatching(deviceMatch) else {
        throw SnapshotError.deviceUnavailable(
            "no video capture device matched '\(deviceMatch)'. " +
            "Try 'xemu-capture list' to see available devices.")
    }
    var activeInputName = dev.activeInputSource?.localizedName ?? ""
    var activeInputID = dev.activeInputSource?.inputSourceID ?? ""
    if let inp = inputMatch {
        let r = try setActiveInput(dev, match: inp)
        activeInputName = (r["active_input_name"] as? String) ?? activeInputName
        activeInputID = (r["active_input_id"] as? String) ?? activeInputID
    }
    let cap = try SnapshotCapturer(device: dev, warmupFrames: warmupFrames,
                                   preferredWidth: preferredWidth,
                                   preferredHeight: preferredHeight)
    let img = try cap.capture(timeout: timeout)
    try writePNG(img, to: outPath)
    return [
        "status": "ok",
        "device": dev.localizedName,
        "device_unique_id": dev.uniqueID,
        "out": outPath,
        "width": img.width,
        "height": img.height,
        "warmup_frames": warmupFrames,
        "active_input_id": activeInputID,
        "active_input_name": activeInputName,
    ]
}

// MARK: - Probe

func describeFormat(_ f: AVCaptureDevice.Format) -> [String: Any] {
    let dims = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
    let frameRates = f.videoSupportedFrameRateRanges.map { range in
        ["min_fps": range.minFrameRate, "max_fps": range.maxFrameRate]
    }
    return [
        "width": Int(dims.width),
        "height": Int(dims.height),
        "media_subtype": String(describing: f.formatDescription.mediaSubType),
        "frame_rates": frameRates,
    ]
}

// MARK: - Daemon (serve)

final class Daemon {
    let port: UInt16
    private let listener: Int32

    init(port: UInt16) throws {
        self.port = port
        let s = socket(AF_INET, SOCK_STREAM, 0)
        if s < 0 {
            throw SnapshotError.sessionStartFailed("socket(): \(errno)")
        }
        var yes: Int32 = 1
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr.s_addr = INADDR_ANY.bigEndian
        let rc = withUnsafePointer(to: &addr) { ptr -> Int32 in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                Darwin.bind(s, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if rc != 0 {
            close(s)
            throw SnapshotError.sessionStartFailed("bind(): \(errno)")
        }
        if listen(s, 4) != 0 {
            close(s)
            throw SnapshotError.sessionStartFailed("listen(): \(errno)")
        }
        self.listener = s
    }

    func run() {
        putErr("xemu-capture serve: listening on port \(port)")
        while true {
            var ca = sockaddr()
            var calen = socklen_t(MemoryLayout<sockaddr>.size)
            let client = accept(listener, &ca, &calen)
            if client < 0 { continue }
            handle(client: client)
        }
    }

    private func handle(client: Int32) {
        defer { close(client) }
        // Read one line.
        var buffer = [UInt8](repeating: 0, count: 4096)
        var total = 0
        while total < buffer.count - 1 {
            let n = recv(client, &buffer[total], buffer.count - 1 - total, 0)
            if n <= 0 { break }
            total += n
            if buffer[total - 1] == 0x0A { break }
        }
        guard total > 0 else { return }
        let line = String(bytes: buffer[..<total], encoding: .utf8) ?? ""
        var resp: [String: Any]
        do {
            resp = try dispatch(line: line.trimmingCharacters(in: .whitespacesAndNewlines))
        } catch {
            resp = ["status": "error", "error": String(describing: error)]
        }
        let respData = ((try? JSONSerialization.data(withJSONObject: resp))
                        ?? Data()) + Data([0x0A])
        respData.withUnsafeBytes { rawBuf in
            _ = send(client, rawBuf.baseAddress, rawBuf.count, 0)
        }
    }

    private func dispatch(line: String) throws -> [String: Any] {
        guard let data = line.data(using: .utf8),
              let req = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let op = req["op"] as? String else {
            return ["status": "error", "error": "bad request"]
        }
        switch op {
        case "list":
            let devs = discoverDevices().map {
                [
                    "unique_id": $0.uniqueID,
                    "name": $0.localizedName,
                    "model_id": $0.modelID,
                    "manufacturer": $0.manufacturer,
                ]
            }
            return ["status": "ok", "devices": devs]
        case "snapshot":
            let device = (req["device"] as? String) ?? "USB"
            guard let out = req["out"] as? String else {
                return ["status": "error", "error": "missing 'out'"]
            }
            let warmup = (req["warmup_frames"] as? Int) ?? 5
            let timeout = (req["timeout"] as? Double) ?? 10.0
            let inp = req["input"] as? String
            // Honor the same width/height the CLI snapshot exposes so
            // daemon clients can force NTSC 720x480 instead of letting
            // sessionPreset pick PAL 720x576 (the whole point of the
            // tool for Xbox composite capture).
            let prefW = req["width"] as? Int
            let prefH = req["height"] as? Int
            return try snapshot(deviceMatch: device, outPath: out,
                                warmupFrames: warmup, timeout: timeout,
                                inputMatch: inp,
                                preferredWidth: prefW,
                                preferredHeight: prefH)
        case "inputs":
            let device = (req["device"] as? String) ?? "USB"
            guard let dev = deviceMatching(device) else {
                return ["status": "error", "error": "no device matched"]
            }
            return [
                "status": "ok",
                "device": dev.localizedName,
                "active_input_id": dev.activeInputSource?.inputSourceID ?? "",
                "inputs": describeInputSources(dev),
            ]
        case "set-input":
            let device = (req["device"] as? String) ?? "USB"
            guard let inp = req["input"] as? String else {
                return ["status": "error", "error": "missing 'input'"]
            }
            guard let dev = deviceMatching(device) else {
                return ["status": "error", "error": "no device matched"]
            }
            return try setActiveInput(dev, match: inp)
        case "shutdown":
            DispatchQueue.global().asyncAfter(deadline: .now() + 0.1) {
                exit(0)
            }
            return ["status": "ok", "msg": "shutting down"]
        default:
            return ["status": "error", "error": "unknown op '\(op)'"]
        }
    }
}

// MARK: - CLI

func parseArg(_ args: [String], _ flag: String) -> String? {
    guard let i = args.firstIndex(of: flag), i + 1 < args.count else { return nil }
    return args[i + 1]
}

func usage() -> String {
    return """
    xemu-capture — Mac-side capture for the Xbox composite oracle.

    Subcommands:
      list                                List video capture devices (JSON).
      probe DEVICE                        Supported formats / fps for DEVICE.
      snapshot DEVICE --out PATH          Single-frame PNG capture.
                     [--warmup-frames N]    Drop N initial frames (default 5).
                     [--timeout SECONDS]    Max wait (default 10).
      serve [--port N]                    Long-running TCP daemon (default 8889).
      version                             Print version.

    DEVICE matching is substring-insensitive against the localized device name.
    For the MS2109 capture stick, "USB2" is enough.
    """
}

let args = Array(CommandLine.arguments.dropFirst())
if args.isEmpty {
    putErr(usage())
    exit(2)
}

switch args[0] {
case "list":
    let devs = discoverDevices().map {
        [
            "unique_id": $0.uniqueID,
            "name": $0.localizedName,
            "model_id": $0.modelID,
            "manufacturer": $0.manufacturer,
        ]
    }
    emitJSON(["status": "ok", "devices": devs])

case "auth":
    let shouldRequest = args.contains("--request")
    let before = cameraAuthorizationStatusString()
    var granted: Bool? = nil
    if shouldRequest {
        granted = requestCameraAccessIfNeeded()
    }
    emitJSON([
        "status": "ok",
        "camera_authorization": cameraAuthorizationStatusString(),
        "before": before,
        "requested": shouldRequest,
        "granted": granted as Any,
    ])

case "probe":
    guard args.count >= 2 else {
        putErr("usage: probe DEVICE"); exit(2)
    }
    guard let dev = deviceMatching(args[1]) else {
        emitJSON(["status": "error",
                  "error": "no device matched '\(args[1])'"],
                 file: .standardOutput)
        exit(1)
    }
    let formats = dev.formats.map(describeFormat)
    emitJSON([
        "status": "ok",
        "device": dev.localizedName,
        "unique_id": dev.uniqueID,
        "formats": formats,
    ])

case "snapshot":
    guard args.count >= 2 else { putErr("usage: snapshot DEVICE --out PATH"); exit(2) }
    let device = args[1]
    guard let out = parseArg(args, "--out") else {
        putErr("--out PATH required"); exit(2)
    }
    let warmup = Int(parseArg(args, "--warmup-frames") ?? "5") ?? 5
    let timeout = Double(parseArg(args, "--timeout") ?? "10") ?? 10
    let inputMatch = parseArg(args, "--input")
    let prefW = parseArg(args, "--width").flatMap { Int($0) }
    let prefH = parseArg(args, "--height").flatMap { Int($0) }
    do {
        let result = try snapshot(deviceMatch: device, outPath: out,
                                  warmupFrames: warmup, timeout: timeout,
                                  inputMatch: inputMatch,
                                  preferredWidth: prefW,
                                  preferredHeight: prefH)
        emitJSON(result)
    } catch {
        emitJSON(["status": "error", "error": String(describing: error)])
        exit(1)
    }

case "sequence":
    guard args.count >= 2 else {
        putErr("usage: sequence DEVICE --out-prefix PFX --count N --interval SECS"); exit(2)
    }
    let device = args[1]
    guard let pfx = parseArg(args, "--out-prefix") else {
        putErr("--out-prefix PFX required"); exit(2)
    }
    let count = Int(parseArg(args, "--count") ?? "10") ?? 10
    let interval = Double(parseArg(args, "--interval") ?? "1.0") ?? 1.0
    let warmup = Int(parseArg(args, "--warmup-frames") ?? "5") ?? 5
    let timeout = Double(parseArg(args, "--timeout") ?? "10") ?? 10
    let inputMatch = parseArg(args, "--input")
    var results: [[String: Any]] = []
    for i in 1...count {
        let outPath = String(format: "%@.%04d.png", pfx, i)
        do {
            let r = try snapshot(deviceMatch: device, outPath: outPath,
                                 warmupFrames: warmup, timeout: timeout,
                                 inputMatch: i == 1 ? inputMatch : nil)
            results.append(r)
        } catch {
            results.append(["status": "error",
                            "frame": i,
                            "error": String(describing: error)])
        }
        if i < count { Thread.sleep(forTimeInterval: interval) }
    }
    emitJSON(["status": "ok",
              "out_prefix": pfx,
              "count": count,
              "interval_seconds": interval,
              "frames": results])

case "inputs":
    guard args.count >= 2 else {
        putErr("usage: inputs DEVICE"); exit(2)
    }
    guard let dev = deviceMatching(args[1]) else {
        emitJSON(["status": "error", "error": "no device matched '\(args[1])'"])
        exit(1)
    }
    emitJSON([
        "status": "ok",
        "device": dev.localizedName,
        "active_input_id": dev.activeInputSource?.inputSourceID ?? NSNull(),
        "active_input_name": dev.activeInputSource?.localizedName ?? NSNull(),
        "inputs": describeInputSources(dev),
    ])

case "set-input":
    guard args.count >= 3 else {
        putErr("usage: set-input DEVICE INPUT"); exit(2)
    }
    guard let dev = deviceMatching(args[1]) else {
        emitJSON(["status": "error", "error": "no device matched '\(args[1])'"])
        exit(1)
    }
    do {
        let r = try setActiveInput(dev, match: args[2])
        emitJSON(r)
    } catch {
        emitJSON(["status": "error", "error": String(describing: error)])
        exit(1)
    }

case "serve":
    let port = UInt16(parseArg(args, "--port") ?? "8889") ?? 8889
    do {
        let d = try Daemon(port: port)
        d.run()
    } catch {
        emitJSON(["status": "error", "error": String(describing: error)])
        exit(1)
    }

case "version":
    emitJSON(["status": "ok", "version": "0.1.0"])

case "-h", "--help":
    print(usage())

default:
    putErr("unknown subcommand: \(args[0])")
    putErr(usage())
    exit(2)
}
