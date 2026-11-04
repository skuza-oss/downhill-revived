// LauncherMainView.swift - Root Container Switcher for iOS Launcher & Handheld Console Experience
// GPL-3.0-or-later
import SwiftUI

struct LauncherMainView: View {
    @ObservedObject var model = LauncherModel.shared
    @State private var showGraphicsSettings = false
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        GeometryReader { geo in
            let isPortrait = geo.size.height > geo.size.width
            let safe = geo.safeAreaInsets

            ZStack {
                if model.isGameRunning {
                    if model.externalDisplayConnected {
                        externalControllerView(geo: geo, safe: safe)
                    } else if isPortrait {
                        portraitHandheldGameView(geo: geo, safe: safe)
                    } else {
                        landscapeGameView(geo: geo, safe: safe)
                    }

                    if model.showInGameMenu {
                        inGamePauseOverlay(geo: geo)
                    }
                } else {
                    LauncherNavigationView(path: $model.navigationPath, size: geo.size, safe: safe)
                }
            }
            .frame(width: geo.size.width, height: geo.size.height)
            .background(PhoneTouchActivityObserver())
            .background(ExternalDisplayRegistrationView())
            .onChange(of: isPortrait) { _, _ in
                touch_input_reset()
            }
            .onChange(of: model.showInGameMenu) { _, shown in
                if shown { touch_input_reset() }
                else { showGraphicsSettings = false }
            }
            .onChange(of: scenePhase) { _, phase in
                if phase != .active {
                    touch_input_reset()
                }
            }
        }
        .sheet(isPresented: $showGraphicsSettings) {
            NavigationStack {
                GraphicsSettingsView(requiresApply: true)
                    .toolbar {
                        ToolbarItem(placement: .cancellationAction) {
                            Button("Back") { showGraphicsSettings = false }
                        }
                    }
            }
        }
        .statusBarHidden(model.isGameRunning)
        .preferredColorScheme(.dark)
    }

    // MARK: - In-Game Pause / Menu Overlay
    @ViewBuilder
    private func inGamePauseOverlay(geo: GeometryProxy) -> some View {
        ZStack {
            Color.black.opacity(0.75)
                .ignoresSafeArea()
                .onTapGesture {
                    model.showInGameMenu = false
                }

            VStack(spacing: 8) {
                HStack(spacing: 10) {
                    Image(systemName: "gamecontroller.fill")
                        .font(.system(size: 20, weight: .bold))
                        .foregroundColor(.orange)
                    Text("Game Menu")
                        .font(.system(size: 18, weight: .black, design: .rounded))
                        .foregroundColor(.white)
                }

                Text("\(model.outputAspectLabel) • \(model.performanceText)")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(.white.opacity(0.65))

                Divider().background(Color.white.opacity(0.15))

                VStack(spacing: 8) {
                    Button(action: {
                        model.showInGameMenu = false
                    }) {
                        HStack {
                            Image(systemName: "play.fill")
                            Text("Resume Game")
                            Spacer()
                        }
                        .font(.system(size: 15, weight: .bold))
                        .foregroundColor(.white)
                        .padding(.horizontal, 16)
                        .frame(height: 42)
                        .background(Color.green.opacity(0.85))
                        .cornerRadius(12)
                    }

                    Button(action: {
                        model.showInGameMenu = false
                        model.sendInGameStartPulse()
                    }) {
                        HStack {
                            Image(systemName: "pause.circle.fill")
                            Text("Pause / Unpause (START)")
                                .lineLimit(2)
                                .minimumScaleFactor(0.8)
                            Spacer()
                        }
                        .font(.system(size: 15, weight: .bold))
                        .foregroundColor(.white)
                        .padding(.horizontal, 16)
                        .frame(height: 42)
                        .background(Color.blue.opacity(0.85))
                        .cornerRadius(12)
                    }

                    Button {
                        touch_input_reset()
                        showGraphicsSettings = true
                    } label: {
                        HStack {
                            Image(systemName: "gearshape.fill")
                            Text("Settings")
                            Spacer()
                        }
                        .font(.system(size: 15, weight: .bold))
                        .foregroundColor(.white)
                        .padding(.horizontal, 16)
                        .frame(height: 42)
                        .background(Color.purple.opacity(0.85))
                        .cornerRadius(12)
                    }

                    Button {
                        model.autoStartGame.toggle()
                    } label: {
                        Label {
                            Text("Open the game automatically")
                                .multilineTextAlignment(.leading)
                        } icon: {
                            Image(systemName: model.autoStartGame ? "checkmark.square.fill" : "square")
                                .foregroundStyle(.orange)
                                .accessibilityHidden(true)
                        }
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(.white)
                        .frame(maxWidth: .infinity, minHeight: 34, alignment: .leading)
                        .padding(.horizontal, 12)
                        .background(Color.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 8))
                    }
                    .buttonStyle(.plain)
                    .accessibilityAddTraits(model.autoStartGame ? .isSelected : [])

                    Button(action: {
                        model.quitApplicationCleanly()
                    }) {
                        HStack {
                            Image(systemName: "power")
                            Text("Quit Game")
                            Spacer()
                        }
                        .font(.system(size: 15, weight: .bold))
                        .foregroundColor(.white)
                        .padding(.horizontal, 16)
                        .frame(height: 42)
                        .background(Color.red.opacity(0.8))
                        .cornerRadius(12)
                    }
                }
            }
            .padding(16)
            .frame(maxWidth: min(geo.size.width - 40, 360))
            .background(Color(red: 0.10, green: 0.11, blue: 0.15))
            .cornerRadius(18)
            .overlay(RoundedRectangle(cornerRadius: 18).stroke(Color.white.opacity(0.18), lineWidth: 1.5))
            .shadow(color: .black.opacity(0.8), radius: 20, y: 10)
        }
    }

    // MARK: - Portrait Handheld Game View (Game Boy / DS Style)
    @ViewBuilder
    private func portraitHandheldGameView(geo: GeometryProxy, safe: EdgeInsets) -> some View {
        let aspect = model.aspectRatio.portraitRatio
        let maxW = min(geo.size.width - 20, 500)
        let maxH = geo.size.height * 0.44
        let rawH = maxW / aspect
        let screenH = min(rawH, maxH)
        let screenW = screenH * aspect

        VStack(spacing: 0) {
            // Upper Section: Authentic Screen in Bezel
            VStack(spacing: 8) {
                // Top Bar: Game Title & Performance
                HStack {
                    HStack(spacing: 6) {
                        Circle()
                            .fill(Color.green)
                            .frame(width: 8, height: 8)
                        Text("DOWNHILL REVIVED")
                            .font(.system(size: 11, weight: .black, design: .rounded))
                            .foregroundColor(.white.opacity(0.85))
                            .lineLimit(1)
                    }

                    Spacer()

                    Text(model.performanceText)
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(.purple.opacity(0.9))
                        .lineLimit(1)
                }
                .padding(.horizontal, 16)
                .padding(.top, 6)

                // Active Metal Render Display
                ZStack {
                    MetalGameContainerView()
                }
                .frame(width: screenW, height: screenH)
                .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).stroke(Color.white.opacity(0.18), lineWidth: 1.5))
                .shadow(color: Color.black.opacity(0.85), radius: 10, y: 5)
                .padding(.bottom, 6)
            }
            .frame(maxWidth: .infinity)
            .background(Color.black.ignoresSafeArea())

            // Divider Accent Line
            Rectangle()
                .fill(LinearGradient(colors: [Color.orange.opacity(0.5), Color.clear], startPoint: .leading, endPoint: .trailing))
                .frame(height: 2)

            // Lower Section: Console Controller Deck (Game Boy / Handheld Style)
            HandheldDeckView(model: model)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(
                    LinearGradient(
                        colors: [Color(red: 0.08, green: 0.09, blue: 0.12), Color(red: 0.04, green: 0.05, blue: 0.07)],
                        startPoint: .top,
                        endPoint: .bottom
                    )
                    .ignoresSafeArea()
                )

        }
    }

    // MARK: - Landscape Fullscreen Game View
    @ViewBuilder
    private func landscapeGameView(geo: GeometryProxy, safe: EdgeInsets) -> some View {
        ZStack {
            Color.black.ignoresSafeArea()

            MetalGameContainerView()
                .ignoresSafeArea()

            TouchControlsOverlayView(safe: safe)
        }
    }

    private func externalControllerView(geo: GeometryProxy, safe: EdgeInsets) -> some View {
        ZStack(alignment: .bottom) {
            Color.black.ignoresSafeArea()
            if geo.size.height > geo.size.width {
                HandheldDeckView(model: model)
                    .frame(height: geo.size.height * 0.50)
            } else {
                TouchControlsOverlayView(safe: safe)
            }
        }
    }
}

struct LauncherNavigationView: View {
    @Binding var path: [LauncherRoute]
    let size: CGSize
    let safe: EdgeInsets
    var isTVPreview = false

    var body: some View {
        NavigationStack(path: $path) {
            HomeView(size: size, safe: safe, isTVPreview: isTVPreview)
                .navigationBarHidden(true)
                .navigationDestination(for: LauncherRoute.self) { route in
                    destinationView(for: route)
                }
        }
    }

    @ViewBuilder
    private func destinationView(for route: LauncherRoute) -> some View {
        switch route {
        case .disc:
            DiscManagementView()
        case .graphics:
            GraphicsSettingsView()
        case .controls:
            ControlSettingsView(isTVPreview: isTVPreview)
        case .settings:
            GameSettingsView(isTVPreview: isTVPreview)
        case .online:
            OnlineView()
        case .about:
            AboutView()
        }
    }
}

struct InGameMenuButton: View {
    @ObservedObject var model: LauncherModel

    var body: some View {
        Button(action: { model.showInGameMenu = true }) {
            Label("Menu", systemImage: "pause.fill")
                .font(.system(size: 11, weight: .bold))
                .lineLimit(1)
                .minimumScaleFactor(0.8)
                .padding(.horizontal, 8)
                .padding(.vertical, 8)
                .frame(width: 80)
                .background(Color.black.opacity(0.65), in: Capsule())
                .foregroundColor(.white)
        }
        .opacity(model.menuButtonVisible ? 1 : 0)
        .allowsHitTesting(model.menuButtonVisible)
        .accessibilityHidden(!model.menuButtonVisible)
    }
}

// MARK: - Handheld Console Controller Deck (Portrait Mode)
struct HandheldDeckView: View {
    @ObservedObject var model: LauncherModel

    @State private var stickOffset: CGSize = .zero
    @State private var stickActive: Bool = false

    var body: some View {
        let xbox = model.useXboxGlyphs
        GeometryReader { geo in
            let w = geo.size.width
            let shoulderWidth = min(66, max(0, (w - 144) / 4))

            VStack(spacing: 8) {
                HStack {
                    HStack(spacing: 8) {
                        deckShoulder(title: xbox ? "LT" : "L2", id: TOUCH_CTRL_L2, width: shoulderWidth)
                        deckShoulder(title: xbox ? "LB" : "L1", id: TOUCH_CTRL_L1, width: shoulderWidth)
                    }
                    Spacer(minLength: 0)
                    HStack(spacing: 8) {
                        deckShoulder(title: xbox ? "RB" : "R1", id: TOUCH_CTRL_R1, width: shoulderWidth)
                        deckShoulder(title: xbox ? "RT" : "R2", id: TOUCH_CTRL_R2, width: shoulderWidth)
                    }
                }
                .opacity(model.shouldShowTouchControls ? 1 : 0)
                .allowsHitTesting(model.shouldShowTouchControls)
                .overlay { InGameMenuButton(model: model) }
                .padding(.horizontal, 16)
                .padding(.top, 10)

                Spacer()

                // Main Thumb Deck: Stick on Left, Menu in Center, Diamond on Right
                HStack(alignment: .center, spacing: 0) {
                    // Left: Analog Stick
                    ZStack {
                        Circle()
                            .fill(Color.black.opacity(0.45))
                            .frame(width: 130, height: 130)
                            .overlay(Circle().stroke(Color.white.opacity(0.2), lineWidth: 3))

                        Circle()
                            .fill(LinearGradient(colors: [Color.white.opacity(0.45), Color.gray.opacity(0.3)], startPoint: .topLeading, endPoint: .bottomTrailing))
                            .frame(width: 60, height: 60)
                            .overlay(Circle().stroke(Color.white.opacity(0.4), lineWidth: 1.5))
                            .shadow(color: .black.opacity(0.6), radius: 5)
                            .offset(stickOffset)
                    }
                    .frame(width: w * 0.40)
                    .overlay(ImmediateTouchInput(
                        onDown: { point, size in
                            stickActive = true
                            updateStick(point: point, origin: model.floatingStick ? point : CGPoint(x: size.width / 2, y: size.height / 2), radius: 45.0)
                        },
                        onMove: { point, origin, size in
                            updateStick(point: point, origin: model.floatingStick ? origin : CGPoint(x: size.width / 2, y: size.height / 2), radius: 45.0)
                        },
                        onUp: {
                            touch_input_set_stick(0, 0)
                            stickActive = false
                            stickOffset = .zero
                        }
                    ))

                    // Center: SELECT & START
                    VStack(spacing: 16) {
                        deckMenuPill(title: xbox ? "VIEW" : "SELECT", id: TOUCH_CTRL_SELECT)
                        deckMenuPill(title: xbox ? "MENU" : "START", id: TOUCH_CTRL_START)
                    }
                    .frame(width: w * 0.20)

                    // Right: Action Diamond (PlayStation or Xbox glyphs)
                    ZStack {
                        // Top (Triangle / Y)
                        deckActionButton(
                            title: xbox ? "Y" : "△",
                            color: xbox ? Color(red: 0.95, green: 0.78, blue: 0.20) : Color(red: 0.30, green: 0.85, blue: 0.50),
                            id: TOUCH_CTRL_TRIANGLE
                        )
                        .offset(y: -44)

                        // Left (Square / X)
                        deckActionButton(
                            title: xbox ? "X" : "□",
                            color: xbox ? Color(red: 0.25, green: 0.65, blue: 0.95) : Color(red: 0.95, green: 0.45, blue: 0.75),
                            id: TOUCH_CTRL_SQUARE
                        )
                        .offset(x: -44)

                        // Right (Circle / B)
                        deckActionButton(
                            title: xbox ? "B" : "○",
                            color: Color(red: 0.95, green: 0.35, blue: 0.35),
                            id: TOUCH_CTRL_CIRCLE
                        )
                        .offset(x: 44)

                        // Bottom (Cross / A)
                        deckActionButton(
                            title: xbox ? "A" : "✕",
                            color: xbox ? Color(red: 0.30, green: 0.85, blue: 0.40) : Color(red: 0.35, green: 0.55, blue: 0.95),
                            id: TOUCH_CTRL_CROSS
                        )
                        .offset(y: 44)
                    }
                    .frame(width: w * 0.40)
                }
                .opacity(model.shouldShowTouchControls ? 1 : 0)
                .allowsHitTesting(model.shouldShowTouchControls)

                Spacer()
            }
            .padding(.bottom, 8)
        }
        .onDisappear { resetDeck() }
        .onChange(of: model.shouldShowTouchControls) { _, shown in if !shown { resetDeck() } }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willResignActiveNotification)) { _ in resetDeck() }
    }

    private func resetDeck() {
        touch_input_reset()
        stickOffset = .zero
        stickActive = false
    }

    private func deckShoulder(title: String, id: TouchControlID, width: CGFloat) -> some View {
        ZStack {
            Text(title)
                .font(.system(size: 14, weight: .black))
                .foregroundColor(.white.opacity(0.85))
                .frame(width: width, height: 38)
                .background(Color.white.opacity(0.12))
                .cornerRadius(10)
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.white.opacity(0.2), lineWidth: 1.5))
        }
        .overlay(ImmediateTouchInput(
                    onDown: { _, _ in
                        touch_input_set_button(id, true)
                        haptic(.light)
                    },
                    onUp: {
                        touch_input_set_button(id, false)

                    }
                ))
    }

    private func deckActionButton(title: String, color: Color, id: TouchControlID) -> some View {
        ZStack {
            Circle()
                .fill(Color.black.opacity(0.5))
                .frame(width: 48, height: 48)
            Circle()
                .stroke(color.opacity(0.85), lineWidth: 3)
                .frame(width: 48, height: 48)
            Text(title)
                .font(.system(size: 20, weight: .black))
                .foregroundColor(color)
        }
        .overlay(ImmediateTouchInput(
                    onDown: { _, _ in
                        touch_input_set_button(id, true)
                        haptic(.medium)
                    },
                    onUp: {
                        touch_input_set_button(id, false)

                    }
                ))
    }

    private func deckMenuPill(title: String, id: TouchControlID) -> some View {
        ZStack {
            Capsule()
                .fill(Color.white.opacity(0.12))
                .frame(width: 68, height: 26)
            Capsule()
                .stroke(Color.white.opacity(0.25), lineWidth: 1)
                .frame(width: 68, height: 26)
            Text(title)
                .font(.system(size: 10, weight: .black))
                .foregroundColor(.white.opacity(0.75))
        }
        .overlay(ImmediateTouchInput(
                    onDown: { _, _ in
                        touch_input_set_button(id, true)
                        haptic(.light)
                    },
                    onUp: {
                        touch_input_set_button(id, false)

                    }
                ))
    }

    private func updateStick(point: CGPoint, origin: CGPoint, radius: CGFloat) {
        let dx = point.x - origin.x, dy = point.y - origin.y
        // The native pad receives the sample before observable visual state changes.
        touch_input_set_stick(Float(dx / radius), Float(dy / radius))
        let distance = hypot(dx, dy)
        let gain = distance > radius ? radius / distance : 1
        stickOffset = CGSize(width: dx * gain, height: dy * gain)
    }

    private func haptic(_ style: UIImpactFeedbackGenerator.FeedbackStyle) {
        if model.hapticsEnabled {
            let gen = UIImpactFeedbackGenerator(style: style)
            gen.prepare()
            gen.impactOccurred()
        }
    }
}

// MARK: - Native Metal View Controller Bridge
struct MetalGameContainerView: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> UIViewController {
        return ios_metal_create_view_controller()
    }

    func updateUIViewController(_ uiViewController: UIViewController, context: Context) {}
}

// Stays in the phone hierarchy while the Metal layer moves to the TV.
struct ExternalDisplayRegistrationView: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> UIViewController {
        ios_metal_create_display_registration_controller()
    }
    func updateUIViewController(_ uiViewController: UIViewController, context: Context) {}
}
