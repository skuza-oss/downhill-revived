// LauncherMainView.swift - Root Container Switcher for iOS Launcher & Handheld Console Experience
// GPL-3.0-or-later
import SwiftUI

struct LauncherMainView: View {
    @ObservedObject var model = LauncherModel.shared
    @State private var showGraphicsSettings = false
    @State private var showControlsSettings = false
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        GeometryReader { geo in
            let isPortrait = geo.size.height > geo.size.width
            let safe = geo.safeAreaInsets

            ZStack {
                if model.isGameRunning {
                    if model.externalDisplayConnected {
                        Color.black.ignoresSafeArea()
                    } else if isPortrait {
                        portraitHandheldGameView(geo: geo)
                    } else {
                        landscapeGameView
                    }

                    DeviceTouchControlsView()
                        .frame(width: geo.size.width, height: geo.size.height)

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
        .fullScreenCover(isPresented: $showControlsSettings) {
            ControlsCustomizationView()
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

            ScrollView {
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
                            touch_input_reset()
                            showControlsSettings = true
                        } label: {
                            Label("Customize Controls", systemImage: "hand.draw.fill")
                                .font(.system(size: 15, weight: .bold))
                                .foregroundStyle(.white)
                                .frame(maxWidth: .infinity, minHeight: 42, alignment: .leading)
                                .padding(.horizontal, 16)
                                .background(Color.indigo.opacity(0.85), in: RoundedRectangle(cornerRadius: 12))
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
            }
            .frame(maxHeight: max(160, geo.size.height - 24))
            .frame(maxWidth: min(geo.size.width - 40, 360))
            .background(Color(red: 0.10, green: 0.11, blue: 0.15))
            .cornerRadius(18)
            .overlay(RoundedRectangle(cornerRadius: 18).stroke(Color.white.opacity(0.18), lineWidth: 1.5))
            .shadow(color: .black.opacity(0.8), radius: 20, y: 10)
        }
    }

    // MARK: - Portrait Handheld Game View (Game Boy / DS Style)
    @ViewBuilder
    private func portraitHandheldGameView(geo: GeometryProxy) -> some View {
        let screen = TouchLayoutViewport(geometry: geo, model: model).portraitScreenSize

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
                .frame(height: 14)
                .padding(.horizontal, 16)
                .padding(.top, 6)

                // Active Metal Render Display
                ZStack {
                    MetalGameContainerView()
                }
                .frame(width: screen.width, height: screen.height)
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
            Color.clear
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
    private var landscapeGameView: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            MetalGameContainerView()
                .ignoresSafeArea()

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

