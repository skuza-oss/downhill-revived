// LauncherModel.swift - State Management and Core Binding for iOS Launcher
// GPL-3.0-or-later
import SwiftUI
import Combine
import GameController

public enum PerformanceProfile: String, CaseIterable, Identifiable {
    case quality = "Quality"
    case balanced = "Balanced"
    case performance = "Performance"

    public var id: String { rawValue }

    public var title: String { L10n.text(rawValue) }

    public var description: String {
        switch self {
        case .quality:
            return L10n.text("2x internal scale, 60 FPS target, 1080-pixel output limit on the short side.")
        case .balanced:
            return L10n.text("Auto scale (1x or 2x), 60 FPS target, lower output when hot.")
        case .performance:
            return L10n.text("1x internal scale, 60 FPS target, 720-pixel output limit on the short side.")
        }
    }
}

public enum AspectRatioOption: String, CaseIterable, Identifiable {
    case wide = "wide"
    case sixteenNine = "16:9"
    case fourThree = "4:3"
    case stretch = "stretch"

    public var id: String { rawValue }

    public var label: String {
        switch self {
        case .wide: return L10n.text("Wide (Hor+)")
        case .sixteenNine: return "16:9"
        case .fourThree: return "4:3 (PS2)"
        case .stretch: return L10n.text("Stretch")
        }
    }

    public var subtitle: String {
        switch self {
        case .wide: return L10n.text("Widens the field of view. The tent uses up to 16:9 to preserve the framing.")
        case .sixteenNine: return L10n.text("Standard widescreen TV format.")
        case .fourThree: return L10n.text("Original PS2 format, with side bars on wider screens.")
        case .stretch: return L10n.text("Stretches the image to fill the screen.")
        }
    }

    public var portraitRatio: CGFloat {
        switch self {
        case .fourThree:
            return 4.0 / 3.0
        default:
            return 16.0 / 9.0
        }
    }
}

public enum ControllerStyle: String, CaseIterable, Identifiable {
    case auto = "Auto"
    case playstation = "PlayStation"
    case xbox = "Xbox"

    public var id: String { rawValue }
}

public struct ConnectedGamepadInfo: Identifiable, Equatable {
    public let id: String
    public let name: String
    public let category: String
    public let isXbox: Bool
    public let isPlayStation: Bool
    public let batteryPercentage: Int?
    public let isCharging: Bool
    public let playerIndex: Int
}

public enum LauncherRoute: String, Hashable, CaseIterable {
    case disc
    case graphics
    case controls
    case online
    case settings
    case about
}

public class LauncherModel: ObservableObject {
    public static let shared = LauncherModel()

    public let appVersion = "0.7.12 (iOS r6)"

    @Published var autoStartGame = UserDefaults.standard.bool(forKey: "DHD_IOS_AUTO_START") {
        didSet { UserDefaults.standard.set(autoStartGame, forKey: "DHD_IOS_AUTO_START") }
    }
    private var handledStartup = false

    @Published public var isGameRunning: Bool = false
    @Published public var showInGameMenu: Bool = false
    @Published private(set) var controlsEditorActive = false
    private var controlsEditingDepth = 0
    @Published public var navigationPath: [LauncherRoute] = []
    @Published public var focusedCardIndex: Int = 0

    @Published public var discReady: Bool = false
    @Published public var discStatusMessage: String = L10n.text("Checking disc…")
    @Published public var discFileName: String = ""
    @Published public var discFileSizeMB: UInt64 = 0
    @Published public var availableStorageMB: UInt64 = 0
    @Published public var isImporting: Bool = false
    @Published public var importProgress: Double = 0.0
    @Published public var importStatusText: String = ""
    @Published public var importError: String? = nil

    // Graphics Settings
    static let defaultTVInternalScale = 3
    @Published public var selectedProfile: PerformanceProfile = .performance
    @Published public var externalResolutionUnlimited = false
    @Published public private(set) var externalDisplayConnected = false
    @Published public var outputResolution = 0 // 0 = profile
    @Published private var tvResolutionScale: Int?
    @Published public private(set) var effectiveFPS = 60

    public func setExternalDisplayConnected(_ connected: Bool) {
        guard externalDisplayConnected != connected else { return }
        tvResolutionScale = nil
        externalDisplayConnected = connected
        updateOutputPolicy()
    }

    public var outputAspectLabel: String { externalDisplayConnected ? "TV 16:9" : aspectRatio.label }

    public var activeResolutionScale: Int {
        get { externalDisplayConnected ? tvResolutionScale ?? Self.defaultTVInternalScale : resolutionScale }
        set {
            if externalDisplayConnected { tvResolutionScale = newValue }
            else { resolutionScale = newValue }
        }
    }
    @Published public var touchControlsAwake = true
    @Published public var menuButtonVisible = false
    private var activePhoneTouches = 0
    private var lastPhoneTouch: TimeInterval = 0
    private var chromeTimer: Timer?

    public func phoneTouchActivity(began: Bool, count: Int) {
        guard isGameRunning else { return }
        activePhoneTouches = count
        lastPhoneTouch = ProcessInfo.processInfo.systemUptime
        if began && !showInGameMenu {
            if !touchControlsAwake { touchControlsAwake = true }
            if !menuButtonVisible { menuButtonVisible = true }
        }
        if chromeTimer == nil {
            chromeTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] timer in
                guard let self = self, self.isGameRunning else { timer.invalidate(); self?.chromeTimer = nil; return }
                guard self.activePhoneTouches == 0 && !self.showInGameMenu,
                      ProcessInfo.processInfo.systemUptime - self.lastPhoneTouch >= 4 else { return }
                self.menuButtonVisible = false
                if self.externalDisplayConnected || !self.connectedGamepads.isEmpty {
                    self.touchControlsAwake = false
                }
                timer.invalidate()
                self.chromeTimer = nil
            }
        }
    }

    public func hideGameChrome() {
        touch_input_reset()
        activePhoneTouches = 0
        touchControlsAwake = false
        menuButtonVisible = false
    }

    private func physicalControllerUsed() {
        guard isGameRunning else { return }
        menuButtonVisible = false
        if autoHideOnGamepad { touchControlsAwake = false; touch_input_reset() }
    }

    @Published public var resolutionScale: Int = 1 // 0 = automatic
    @Published public var targetFPS: Int = 60
    @Published public var aspectRatio: AspectRatioOption = .wide {
        didSet {
            widescreenEnabled = (aspectRatio == .wide || aspectRatio == .sixteenNine)
        }
    }
    @Published public var widescreenEnabled: Bool = true
    @Published public var vsyncEnabled: Bool = true
    @Published public var textureFiltering: String = "Bilinear" // "Bilinear" ("linear") or "Nearest" ("point")
    @Published public var measuredFPS: Double = 0
    @Published public var frameTimeP99: Double = 0
    @Published public var thermalStatus: String = L10n.text("Normal")
    public var supports120FPS: Bool { ios_metal_has_promotion_support() }
    public var performanceText: String {
        measuredFPS > 0 ? L10n.format("%.1f FPS • p99 %.1f ms", measuredFPS, frameTimeP99) : L10n.text("Measuring frame rate…")
    }

    // Controls Settings
    @Published public var touchOpacity: Double = 0.50
    @Published public var gamepadDeadzone: Double = 0.05
    @Published public var touchDeadzone: Double = 0.02
    @Published public var touchSensitivity: Double = 1.0
    @Published public var floatingStick: Bool = false
    @Published public var autoHideOnGamepad: Bool = true
    @Published public var hapticsEnabled: Bool = true
    @Published public var controllerStyle: ControllerStyle = .auto
    @Published public var connectedGamepads: [String] = []
    @Published public var connectedGamepadDetails: [ConnectedGamepadInfo] = []

    // Game Settings
    @Published public var riderName: String = "Rider"
    @Published public var musicVolume: Double = 1.0
    @Published public var sfxVolume: Double = 1.0
    @Published public var skipMovies: Bool = true

    // Online
    @Published public var serverAddress: String = "wss://dhd.skuza.app"
    @Published public var onlineStatus: String = "Ready"

    private var cancellables = Set<AnyCancellable>()
    private var lastEngineSettings: [String: String] = [:]

    public init() {
        touch_input_restore_defaults()
        _ = touch_input_load_saved_layout()
        refreshStorageAndDisc()
        loadPreferences()
        setupGamepadMonitoring()
        Timer.publish(every: 1, on: .main, in: .common).autoconnect()
            .sink { [weak self] _ in
                guard let self = self else { return }
                self.updateOutputPolicy()
                if self.isGameRunning {
                    var fps = 0.0, p99 = 0.0
                    ios_metal_get_timing(&fps, &p99)
                    self.measuredFPS = fps
                    self.frameTimeP99 = p99
                } else {
                    self.measuredFPS = 0
                    self.frameTimeP99 = 0
                }
            }.store(in: &cancellables)
    }

    public var effectiveScale: Int {
        if activeResolutionScale > 0 { return activeResolutionScale }
        let process = ProcessInfo.processInfo
        return process.thermalState == .nominal && !process.isLowPowerModeEnabled ? 2 : 1
    }

    public var resolutionLabel: String {
        switch activeResolutionScale {
        case 0: return L10n.format("Auto (%lldx)", Int64(effectiveScale))
        case 1: return "1x (PS2)"
        default: return "\(activeResolutionScale)x"
        }
    }

    public var useXboxGlyphs: Bool {
        switch controllerStyle {
        case .xbox:
            return true
        case .playstation:
            return false
        case .auto:
            if let first = connectedGamepadDetails.first {
                return first.isXbox
            }
            return false
        }
    }

    public var shouldShowTouchControls: Bool {
        if showInGameMenu { return false }
        if externalDisplayConnected { return touchControlsAwake }
        return !autoHideOnGamepad || connectedGamepads.isEmpty || touchControlsAwake
    }

    public func refreshStorageAndDisc() {
        self.availableStorageMB = ios_available_disk_space() >> 20
        self.discReady = ios_disc_auto_detect()
        if self.discReady {
            var nameBuf = [CChar](repeating: 0, count: 256)
            var sizeBytes: UInt64 = 0
            if ios_disc_get_info(&nameBuf, 256, &sizeBytes) {
                let name = String(cString: nameBuf)
                let sizeGB = Double(sizeBytes) / (1024.0 * 1024.0 * 1024.0)
                self.discFileName = name
                self.discFileSizeMB = sizeBytes >> 20
                self.discStatusMessage = L10n.format("%@ (%.2f GB) • Verified", name, sizeGB)
            } else {
                self.discStatusMessage = L10n.text("Downhill Domination USA (SCUS-97177) verified")
            }
        } else {
            self.discFileName = ""
            self.discFileSizeMB = 0
            self.discStatusMessage = L10n.text("Disc image required (SCUS-97177)")
        }
    }

    public func deleteActiveDisc() {
        guard !isGameRunning && !isImporting else { return }
        if !ios_disc_delete() { self.importError = L10n.text("Could not remove the current disc.") }
        refreshStorageAndDisc()
    }

    private func setupGamepadMonitoring() {
        NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] _ in
            self?.updateConnectedGamepads()
        }
        NotificationCenter.default.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] _ in
            self?.updateConnectedGamepads()
        }
        updateConnectedGamepads()
    }

    public func updateConnectedGamepads() {
        let pads = GCController.controllers()
        var details: [ConnectedGamepadInfo] = []
        for (idx, pad) in pads.enumerated() {
            let vendor = pad.vendorName ?? L10n.text("Wireless Controller")
            let cat = pad.productCategory
            let lower = (vendor + " " + cat).lowercased()
            let isPS = lower.contains("dualsense") || lower.contains("dualshock") || lower.contains("playstation") || lower.contains("ps4") || lower.contains("ps5") || lower.contains("sony")
            let isXB = !isPS && (lower.contains("xbox") || lower.contains("microsoft"))
            var battPct: Int? = nil
            var charging = false
            if let batt = pad.battery {
                battPct = Int((batt.batteryLevel * 100.0).rounded())
                charging = (batt.batteryState == .charging)
            }
            details.append(ConnectedGamepadInfo(
                id: "\(idx)-\(vendor)",
                name: vendor,
                category: cat,
                isXbox: isXB,
                isPlayStation: isPS,
                batteryPercentage: battPct,
                isCharging: charging,
                playerIndex: idx + 1
            ))
            bindLauncherControllerNavigation(pad)
        }
        self.connectedGamepadDetails = details
        self.connectedGamepads = details.map { $0.name }
        touch_input_set_gamepad_connected(!pads.isEmpty)
        if pads.isEmpty && !externalDisplayConnected { touchControlsAwake = true }
    }

    private func bindLauncherControllerNavigation(_ controller: GCController) {
        guard let gamepad = controller.extendedGamepad else { return }
        controller.handlerQueue = .main
        gamepad.valueChangedHandler = { [weak self] _, element in
            let active: Bool
            if let button = element as? GCControllerButtonInput { active = button.isPressed }
            else if let direction = element as? GCControllerDirectionPad {
                active = abs(direction.xAxis.value) > 0.25 || abs(direction.yAxis.value) > 0.25
            } else if let axis = element as? GCControllerAxisInput { active = abs(axis.value) > 0.25 }
            else { active = false }
            if active { self?.physicalControllerUsed() }
        }

        gamepad.dpad.valueChangedHandler = { [weak self] _, xValue, yValue in
            guard let self = self, !self.isGameRunning, !self.controlsEditorActive else { return }
            DispatchQueue.main.async {
                let routes = LauncherRoute.allCases
                if yValue > 0.5 {
                    self.focusedCardIndex = max(0, self.focusedCardIndex - 2)
                } else if yValue < -0.5 {
                    self.focusedCardIndex = min(routes.count - 1, self.focusedCardIndex + 2)
                } else if xValue > 0.5 {
                    self.focusedCardIndex = min(routes.count - 1, self.focusedCardIndex + 1)
                } else if xValue < -0.5 {
                    self.focusedCardIndex = max(0, self.focusedCardIndex - 1)
                }
            }
        }

        gamepad.buttonA.pressedChangedHandler = { [weak self] _, _, pressed in
            guard let self = self, pressed, !self.isGameRunning, !self.controlsEditorActive else { return }
            DispatchQueue.main.async {
                if self.navigationPath.isEmpty {
                    let routes = LauncherRoute.allCases
                    if self.focusedCardIndex >= 0 && self.focusedCardIndex < routes.count {
                        self.navigationPath.append(routes[self.focusedCardIndex])
                    }
                }
            }
        }

        gamepad.buttonB.pressedChangedHandler = { [weak self] _, _, pressed in
            guard let self = self, pressed, !self.controlsEditorActive else { return }
            DispatchQueue.main.async {
                if self.isGameRunning && self.showInGameMenu {
                    self.showInGameMenu = false
                } else if !self.isGameRunning && !self.navigationPath.isEmpty {
                    _ = self.navigationPath.popLast()
                }
            }
        }

        gamepad.buttonMenu.pressedChangedHandler = { [weak self] _, _, pressed in
            guard let self = self, pressed, !self.controlsEditorActive else { return }
            DispatchQueue.main.async {
                if !self.isGameRunning && self.navigationPath.isEmpty && self.discReady {
                    self.launchGame()
                }
            }
        }
    }

    var graphicsOptions: GraphicsOptions {
        GraphicsOptions(profile: selectedProfile, aspectRatio: aspectRatio, phoneScale: resolutionScale,
                        tvScale: tvResolutionScale, outputResolution: outputResolution, targetFPS: targetFPS,
                        textureFiltering: textureFiltering, vsync: vsyncEnabled,
                        unlimitedTVOutput: externalResolutionUnlimited)
    }

    func applyGraphicsOptions(_ options: GraphicsOptions) {
        selectedProfile = options.profile
        if aspectRatio != options.aspectRatio { aspectRatio = options.aspectRatio }
        resolutionScale = options.phoneScale
        tvResolutionScale = options.tvScale
        outputResolution = options.outputResolution
        targetFPS = options.targetFPS
        textureFiltering = options.textureFiltering
        vsyncEnabled = options.vsync
        externalResolutionUnlimited = options.unlimitedTVOutput
        savePreferences()
    }

    func beginControlsEditing() {
        controlsEditingDepth += 1
        guard controlsEditingDepth == 1 else { return }
        controlsEditorActive = true
        touch_input_reset()
        touch_input_set_editing(true)
    }

    func endControlsEditing() {
        controlsEditingDepth = max(0, controlsEditingDepth - 1)
        guard controlsEditingDepth == 0 else { return }
        touch_input_set_editing(false)
        touch_input_reset()
        controlsEditorActive = false
    }

    public func sendInGameStartPulse() {
        touch_input_set_button(TOUCH_CTRL_START, true)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
            touch_input_set_button(TOUCH_CTRL_START, false)
        }
    }

    public func quitApplicationCleanly() {
        savePreferences()
        exit(0)
    }

    public func importISO(from url: URL) {
        guard !isImporting && !isGameRunning else { return }
        // App-local URLs do not need a security scope and can validly return false here.
        let scopedAccess = url.startAccessingSecurityScopedResource()

        self.isImporting = true
        self.importProgress = 0.0
        self.importStatusText = L10n.text("Copying the ISO into the app…")
        self.importError = nil

        let sourcePath = url.path
        DispatchQueue.global(qos: .userInitiated).async {
            var errBuf = [CChar](repeating: 0, count: 512)
            let success = ios_disc_import_from_url(sourcePath, { prog, copied, total in
                DispatchQueue.main.async {
                    LauncherModel.shared.importProgress = prog
                    LauncherModel.shared.importStatusText = L10n.format("Copying: %.1f MB / %.1f MB (%.0f%%)",
                        Double(copied) / 1048576.0, Double(total) / 1048576.0, prog * 100.0)
                }
            }, &errBuf, 512)

            DispatchQueue.main.async {
                if scopedAccess { url.stopAccessingSecurityScopedResource() }
                self.isImporting = false
                if success {
                    self.refreshStorageAndDisc()
                } else {
                    self.importError = L10n.text(String(cString: errBuf))
                }
            }
        }
    }

    func launchGameOnStartup() {
        guard !handledStartup else { return }
        handledStartup = true
        let testAutoStart = ProcessInfo.processInfo.environment["DHD_AUTOBOOT"] == "1"
        guard autoStartGame || testAutoStart else { return }
        guard discReady else { return }
        launchGame()
    }

    public func launchGame() {
        guard !isGameRunning && !isImporting else { return }
        touch_input_reset()
        guard discReady else {
            self.discStatusMessage = L10n.text("Import the USA disc image before playing.")
            return
        }

        savePreferences()
        self.showInGameMenu = false
        hideGameChrome()
        if !externalDisplayConnected && connectedGamepads.isEmpty { touchControlsAwake = true }
        self.isGameRunning = true

        DispatchQueue.global(qos: .userInteractive).async {
            let rc = ios_start_game(0, nil)
            DispatchQueue.main.async {
                self.isGameRunning = false
                if rc == 2 {
                    self.discStatusMessage = L10n.text("The game core is missing from this build.")
                } else if rc != 0 {
                    self.discStatusMessage = L10n.format("Game stopped (code %lld).", Int64(rc))
                }
            }
        }
    }

    public func exitGame() {
        self.showInGameMenu = true
    }

    public func loadPreferences() {
        let defaults = UserDefaults.standard
        // One migration establishes a measurable baseline on installations of the old port.
        // Disc, saves, controls and account settings are untouched.
        if defaults.integer(forKey: "DHD_IOS_PERF_REVISION") < 1 {
            defaults.set(PerformanceProfile.performance.rawValue, forKey: "DHD_PROFILE")
            defaults.set(1, forKey: "DHD_SCALE")
            defaults.set(60, forKey: "DHD_FPS")
            defaults.set(true, forKey: "DHD_VSYNC")
            defaults.set(1, forKey: "DHD_IOS_PERF_REVISION")
        }
        if let profStr = defaults.string(forKey: "DHD_PROFILE"), let p = PerformanceProfile(rawValue: profStr) {
            self.selectedProfile = p
        }
        self.externalResolutionUnlimited = defaults.bool(forKey: "DHD_TV_UNLIMITED")
        self.resolutionScale = defaults.object(forKey: "DHD_SCALE") != nil ? defaults.integer(forKey: "DHD_SCALE") : 1
        if !(0...6).contains(resolutionScale) { resolutionScale = 1 }
        self.targetFPS = defaults.object(forKey: "DHD_FPS") != nil ? defaults.integer(forKey: "DHD_FPS") : 60
        if targetFPS != 120 { self.targetFPS = 60 }
        outputResolution = defaults.integer(forKey: "DHD_OUTPUT_RESOLUTION")
        if ![0, 540, 720, 900, 1080].contains(outputResolution) { outputResolution = 0 }
        if let aspectStr = defaults.string(forKey: "DHD_ASPECT_OPT"), let a = AspectRatioOption(rawValue: aspectStr) {
            self.aspectRatio = a
        } else {
            self.aspectRatio = .wide
        }
        self.widescreenEnabled = defaults.object(forKey: "DHD_WIDESCREEN") != nil ? defaults.bool(forKey: "DHD_WIDESCREEN") : true
        self.vsyncEnabled = defaults.object(forKey: "DHD_VSYNC") != nil ? defaults.bool(forKey: "DHD_VSYNC") : true
        self.textureFiltering = defaults.string(forKey: "DHD_FILTER") ?? "Bilinear"
        self.touchOpacity = defaults.object(forKey: "DHD_TOUCH_OPACITY") != nil ? defaults.double(forKey: "DHD_TOUCH_OPACITY") : 0.50
        let storedGamepadDeadzone = defaults.object(forKey: "DHD_GAMEPAD_DEADZONE") != nil ? defaults.double(forKey: "DHD_GAMEPAD_DEADZONE") : 0.05
        self.gamepadDeadzone = storedGamepadDeadzone.isFinite ? min(0.40, max(0, storedGamepadDeadzone)) : 0.05
        self.touchDeadzone = defaults.object(forKey: "DHD_TOUCH_DEADZONE") != nil ? defaults.double(forKey: "DHD_TOUCH_DEADZONE") : 0.02
        self.touchSensitivity = defaults.object(forKey: "DHD_TOUCH_SENS") != nil ? defaults.double(forKey: "DHD_TOUCH_SENS") : 1.0
        self.floatingStick = defaults.object(forKey: "DHD_TOUCH_FLOATING") != nil ? defaults.bool(forKey: "DHD_TOUCH_FLOATING") : false
        self.autoHideOnGamepad = defaults.object(forKey: "DHD_TOUCH_AUTOHIDE") != nil ? defaults.bool(forKey: "DHD_TOUCH_AUTOHIDE") : true
        self.hapticsEnabled = defaults.object(forKey: "DHD_TOUCH_HAPTICS") != nil ? defaults.bool(forKey: "DHD_TOUCH_HAPTICS") : true
        if let styleStr = defaults.string(forKey: "DHD_CTRL_STYLE"), let st = ControllerStyle(rawValue: styleStr) {
            self.controllerStyle = st
        }
        self.riderName = defaults.string(forKey: "DHD_RIDER_NAME") ?? "Rider"
        self.skipMovies = defaults.object(forKey: "DHD_SKIP_MOVIES") != nil ? defaults.bool(forKey: "DHD_SKIP_MOVIES") : true

        syncTouchAndEngineSettings()
    }

    public func savePreferences() {
        let defaults = UserDefaults.standard
        defaults.set(selectedProfile.rawValue, forKey: "DHD_PROFILE")
        defaults.set(externalResolutionUnlimited, forKey: "DHD_TV_UNLIMITED")
        defaults.set(outputResolution, forKey: "DHD_OUTPUT_RESOLUTION")
        defaults.set(resolutionScale, forKey: "DHD_SCALE")
        defaults.set(targetFPS, forKey: "DHD_FPS")
        defaults.set(aspectRatio.rawValue, forKey: "DHD_ASPECT_OPT")
        defaults.set(widescreenEnabled, forKey: "DHD_WIDESCREEN")
        defaults.set(vsyncEnabled, forKey: "DHD_VSYNC")
        defaults.set(textureFiltering, forKey: "DHD_FILTER")
        defaults.set(touchOpacity, forKey: "DHD_TOUCH_OPACITY")
        defaults.set(gamepadDeadzone, forKey: "DHD_GAMEPAD_DEADZONE")
        defaults.set(touchDeadzone, forKey: "DHD_TOUCH_DEADZONE")
        defaults.set(touchSensitivity, forKey: "DHD_TOUCH_SENS")
        defaults.set(floatingStick, forKey: "DHD_TOUCH_FLOATING")
        defaults.set(autoHideOnGamepad, forKey: "DHD_TOUCH_AUTOHIDE")
        defaults.set(hapticsEnabled, forKey: "DHD_TOUCH_HAPTICS")
        defaults.set(controllerStyle.rawValue, forKey: "DHD_CTRL_STYLE")
        defaults.set(riderName, forKey: "DHD_RIDER_NAME")
        defaults.set(skipMovies, forKey: "DHD_SKIP_MOVIES")

        syncTouchAndEngineSettings()
    }

    private func syncTouchAndEngineSettings() {
        // 1. Sync touch configuration
        var cfg = touch_input_get_config().pointee
        cfg.opacity = Float(touchOpacity)
        cfg.stick_deadzone = Float(touchDeadzone)
        cfg.stick_sensitivity = Float(touchSensitivity)
        cfg.stick_floating = floatingStick
        cfg.auto_hide_on_gamepad = autoHideOnGamepad
        cfg.haptics_enabled = hapticsEnabled
        touch_input_set_gamepad_deadzone(Float(gamepadDeadzone))
        touch_input_set_config(&cfg)
        _ = touch_input_save_layout()

        // 2. Persist engine settings to Documents/dhd.ini so host_settings_load() reads them
        let scaleStr = "\(effectiveScale)"
        let fpsStr = "\(targetFPS)"
        let wideStr = (aspectRatio == .wide || aspectRatio == .sixteenNine) ? "1" : "0"
        let aspectStr = aspectRatio.rawValue
        let filterStr = (textureFiltering == "Nearest") ? "point" : "linear"
        let vsyncStr = vsyncEnabled ? "1" : "0"
        let skipStr = skipMovies ? "1" : "0"
        let vibStr = hapticsEnabled ? "1" : "0"

        let settings = ["renderer": "native", "scale": scaleStr, "fps": fpsStr,
                        "widescreen": wideStr, "DHD_ASPECT": aspectStr,
                        "DHD_PRESENT_FILTER": filterStr, "vsync": vsyncStr,
                        "skip_movies": skipStr, "vibration": vibStr,
                        "name": riderName, "server": serverAddress]
        for (key, value) in settings where lastEngineSettings[key] != value {
            if ios_setting_save(key, value) != 0 { lastEngineSettings[key] = value }
        }

        ios_metal_set_phone_aspect(aspectRatio == .wide ? 0 : aspectRatio == .stretch ? -1 : Double(aspectRatio.portraitRatio))
        if !isGameRunning {
            for scene in UIApplication.shared.connectedScenes {
                (scene.delegate as? ExternalDisplaySceneDelegate)?.applyDisplayMode()
            }
        }
        updateOutputPolicy()
    }

    private var lastOutputLimit: Int?
    private var outputRestoreAfter: TimeInterval = 0
    private func updateOutputPolicy() {
        let process = ProcessInfo.processInfo
        let profileCap = selectedProfile == .quality ? 1080 : selectedProfile == .balanced ? 900 : 720
        let nativeTV = externalDisplayConnected && externalResolutionUnlimited && outputResolution == 0
        let cap = outputResolution > 0 ? outputResolution : profileCap
        let hot = process.thermalState == .serious || process.thermalState == .critical
        let uptime = process.systemUptime
        if hot || process.isLowPowerModeEnabled { outputRestoreAfter = uptime + 30 }
        var limit = nativeTV ? 0 : cap
        if hot || process.isLowPowerModeEnabled {
            limit = min(limit == 0 ? 720 : limit, 720)
        }
        if let previous = lastOutputLimit, previous > 0,
           (limit == 0 || limit > previous), uptime < outputRestoreAfter {
            limit = previous
        }
        let status = L10n.text(hot ? "Hot: reduced output" : process.isLowPowerModeEnabled ? "Low Power Mode" : "Normal")
        if thermalStatus != status { thermalStatus = status }
        ios_metal_set_target_fps(Int32(targetFPS))
        let fps = Int(ios_metal_effective_fps())
        if effectiveFPS != fps { effectiveFPS = fps }
        ios_metal_set_render_preferences(Int32(effectiveScale), textureFiltering == "Nearest")
        if limit != lastOutputLimit {
            lastOutputLimit = limit
            ios_metal_set_output_limit(Int32(limit))
        }
    }
}
