import AppKit
import Foundation

setvbuf(stdout, nil, _IOLBF, 0)

func usage() -> Never {
    print("""
    usage: PowerPCDisplay [IP address of the PowerPC Mac] [options]

      With no address, the PowerPC Mac is found automatically with Bonjour.

      --name NAME       Bonjour: connect only to the PowerPC Mac with this computer name
      --port N          TCP port when connecting by address (default 5960)
      --display-id N    capture an existing display instead of creating one
                        (e.g. a BetterDisplay virtual screen; list IDs with --list)
      --fps N           capture rate cap (default 60)
      --inflight N      frames allowed in flight before waiting for the PowerPC Mac (default 2)
      --mirror          mirror the main display instead of adding one
      --extend          add a display (default, unless Mirror was chosen in the app)
      --sharp-video     send video regions losslessly (turns off Smoother Video)
      -v                print throughput / latency stats every 5 s
      --list            list displays and exit
    """)
    exit(2)
}

var args = Array(CommandLine.arguments.dropFirst())
if args.contains("--list") {
    var ids = [CGDirectDisplayID](repeating: 0, count: 16)
    var n: UInt32 = 0
    CGGetActiveDisplayList(16, &ids, &n)
    for id in ids.prefix(Int(n)) {
        print("display \(id): \(CGDisplayPixelsWide(id))x\(CGDisplayPixelsHigh(id))\(CGDisplayIsMain(id) != 0 ? " (main)" : "")")
    }
    exit(0)
}
var cfg = Host.Config()
if let saved = AppController.savedFPS { cfg.fps = saved }
cfg.mirror = AppController.savedMirror
cfg.smoothVideo = AppController.savedSmoothVideo
var i = 0
if let first = args.first, !first.hasPrefix("-") {
    cfg.g4Address = first
    i = 1
}
while i < args.count {
    let a = args[i]
    func next() -> String { i += 1; guard i < args.count else { usage() }; return args[i] }
    switch a {
    case "--port": cfg.port = UInt16(next()) ?? cfg.port
    case "--name": cfg.g4Name = next()
    case "--display-id": cfg.fixedDisplayID = CGDirectDisplayID(next())
    case "--fps": cfg.fps = max(1, min(120, Int(next()) ?? 60))
    case "--inflight": cfg.maxInFlight = max(1, UInt32(next()) ?? 2)
    case "-v": cfg.verbose = true
    case "--mirror": cfg.mirror = true
    case "--extend": cfg.mirror = false
    case "--sharp-video": cfg.smoothVideo = false
    default: usage()
    }
    i += 1
}

// keep macOS from throttling timers or napping this process
let activity = ProcessInfo.processInfo.beginActivity(
    options: [.userInitiated, .latencyCritical, .idleSystemSleepDisabled],
    reason: "Streaming to the PowerPC Mac's display")

let app = NSApplication.shared
app.setActivationPolicy(.regular)     // Dock icon + window; menu bar item too
let controller = AppController(host: Host(config: cfg))
app.delegate = controller             // weak reference: kept alive below
withExtendedLifetime((activity, controller)) { app.run() }
