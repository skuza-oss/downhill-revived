// HomeView.swift - Enhanced Main Responsive Launcher Screen for iOS
// GPL-3.0-or-later
import SwiftUI

struct HomeView: View {
    @ObservedObject var model = LauncherModel.shared
    let size: CGSize
    let safe: EdgeInsets
    var isTVPreview = false
    @State private var isPulsing: Bool = false

    private var appIconImage: UIImage? {
        if let path = Bundle.main.path(forResource: "app_icon", ofType: "png") {
            return UIImage(contentsOfFile: path)
        }
        return nil
    }

    var body: some View {
        let isPortrait = size.height > size.width

        ZStack {
            // High-end Dark Background with subtle ambient glow
            Color(red: 0.06, green: 0.07, blue: 0.09)
                .ignoresSafeArea()

            RadialGradient(
                gradient: Gradient(colors: [Color.orange.opacity(0.12), Color.clear]),
                center: .topLeading,
                startRadius: 20,
                endRadius: 400
            )
            .ignoresSafeArea()

            if isPortrait {
                portraitLayout
            } else {
                landscapeLayout
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .onAppear {
            if !isTVPreview { model.refreshStorageAndDisc() }
            withAnimation(.easeInOut(duration: 1.5).repeatForever(autoreverses: true)) {
                isPulsing = true
            }
        }
    }

    // MARK: - Portrait Layout (Vertical)
    @ViewBuilder
    private var portraitLayout: some View {
        ScrollView(showsIndicators: false) {
            VStack(spacing: 14) {
                // Header with App Icon, Title & Version Badge
                HStack(spacing: 12) {
                    if let img = appIconImage {
                        Image(uiImage: img)
                            .resizable()
                            .aspectRatio(contentMode: .fit)
                            .frame(width: 48, height: 48)
                            .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                            .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).stroke(Color.white.opacity(0.18), lineWidth: 1))
                            .shadow(color: Color.orange.opacity(0.35), radius: 8, y: 3)
                    }

                    VStack(alignment: .leading, spacing: 3) {
                        HStack(alignment: .center, spacing: 8) {
                            Text("DOWNHILL REVIVED")
                                .font(.system(size: min(22, size.width * 0.058), weight: .black, design: .rounded))
                                .foregroundStyle(LinearGradient(colors: [.yellow, .orange], startPoint: .leading, endPoint: .trailing))
                                .lineLimit(1)
                                .minimumScaleFactor(0.7)

                            Text("v\(model.appVersion)")
                                .font(.system(size: 11, weight: .bold))
                                .padding(.horizontal, 7)
                                .padding(.vertical, 3)
                                .background(Color.white.opacity(0.14))
                                .clipShape(Capsule())
                                .foregroundColor(.white.opacity(0.9))
                                .fixedSize()
                        }

                        Text("PlayStation 2 engine on iPhone")
                            .font(.system(size: 11, weight: .medium))
                            .foregroundColor(.white.opacity(0.65))
                            .lineLimit(1)
                            .minimumScaleFactor(0.8)
                    }
                    Spacer(minLength: 0)
                }
                .padding(.top, 8)

                // Quick Hardware Specs Pill
                HStack(spacing: 8) {
                    specPill(icon: "bolt.fill", text: "\(model.externalDisplayConnected ? model.effectiveFPS : model.targetFPS)Hz Metal", color: .yellow)
                    specPill(icon: "aspectratio.fill", text: model.outputAspectLabel, color: .cyan)
                    specPill(icon: "gamecontroller.fill", text: L10n.text(model.connectedGamepads.isEmpty ? "Touch Controls" : "Gamepad"), color: .purple)
                }

                // Status Banner
                statusBanner

                // Big Action PLAY Button
                playButton
                autoStartToggle

                // 2-Column Action Cards
                LazyVGrid(columns: [GridItem(.flexible(), spacing: 10), GridItem(.flexible(), spacing: 10)], spacing: 10) {
                    navigationCards
                }
                .padding(.top, 2)
                .padding(.bottom, 20)
            }
            .padding(.horizontal, 18)
            .padding(.top, 8)
            .padding(.bottom, 16)
        }
    }

    // MARK: - Landscape Layout (Horizontal - Balanced 2-Column Split)
    @ViewBuilder
    private var landscapeLayout: some View {
        ScrollView(.vertical, showsIndicators: false) {
            HStack(alignment: .top, spacing: 20) {
                // Left Column: Branding, Hardware, Status & Launch
                VStack(alignment: .leading, spacing: 10) {
                    HStack(spacing: 12) {
                        if let img = appIconImage {
                            Image(uiImage: img)
                                .resizable()
                                .aspectRatio(contentMode: .fit)
                                .frame(width: 44, height: 44)
                                .clipShape(RoundedRectangle(cornerRadius: 11, style: .continuous))
                                .overlay(RoundedRectangle(cornerRadius: 11, style: .continuous).stroke(Color.white.opacity(0.18), lineWidth: 1))
                                .shadow(color: Color.orange.opacity(0.35), radius: 6, y: 2)
                        }

                        VStack(alignment: .leading, spacing: 2) {
                            HStack(alignment: .center, spacing: 6) {
                                Text("DOWNHILL REVIVED")
                                    .font(.system(size: 18, weight: .black, design: .rounded))
                                    .foregroundStyle(LinearGradient(colors: [.yellow, .orange], startPoint: .leading, endPoint: .trailing))
                                    .lineLimit(1)
                                    .minimumScaleFactor(0.75)

                                Text("v\(model.appVersion)")
                                    .font(.system(size: 10, weight: .bold))
                                    .padding(.horizontal, 6)
                                    .padding(.vertical, 2)
                                    .background(Color.white.opacity(0.14))
                                    .clipShape(Capsule())
                                    .foregroundColor(.white.opacity(0.9))
                                    .fixedSize()
                            }

                            Text("PlayStation 2 engine on iPhone")
                                .font(.system(size: 10, weight: .medium))
                                .foregroundColor(.white.opacity(0.65))
                                .lineLimit(1)
                                .minimumScaleFactor(0.8)
                        }
                    }

                    HStack(spacing: 6) {
                        specPill(icon: "bolt.fill", text: "\(model.externalDisplayConnected ? model.effectiveFPS : model.targetFPS)Hz Metal", color: .yellow)
                        specPill(icon: "aspectratio.fill", text: model.outputAspectLabel, color: .cyan)
                        specPill(icon: "gamecontroller.fill", text: L10n.text(model.connectedGamepads.isEmpty ? "Touch Controls" : "Gamepad"), color: .purple)
                    }

                    statusBanner

                    playButton
                    autoStartToggle
                }
                .frame(maxWidth: .infinity, alignment: .leading)

                // Right Column: 2 Equal Columns of Navigation Cards
                LazyVGrid(
                    columns: [
                        GridItem(.flexible(), spacing: 10),
                        GridItem(.flexible(), spacing: 10)
                    ],
                    spacing: 10
                ) {
                    navigationCards
                }
                .frame(maxWidth: .infinity)
            }
            .padding(.horizontal, 20)
            .padding(.top, 10)
            .padding(.bottom, 16)
        }
    }

    // MARK: - Subcomponents
    private func specPill(icon: String, text: String, color: Color) -> some View {
        HStack(spacing: 4) {
            Image(systemName: icon)
                .font(.system(size: 10))
                .foregroundColor(color)
            Text(text)
                .font(.system(size: 10, weight: .bold))
                .foregroundColor(.white.opacity(0.85))
                .lineLimit(1)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(Color.white.opacity(0.06))
        .clipShape(Capsule())
    }

    private var autoStartToggle: some View {
        Toggle("Open the game automatically", isOn: $model.autoStartGame)
            .font(.system(size: 13, weight: .semibold))
            .tint(.orange)
            .padding(10)
            .background(Color.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 10))
    }

    private var statusBanner: some View {
        HStack(spacing: 10) {
            Image(systemName: model.discReady ? "checkmark.seal.fill" : "exclamationmark.triangle.fill")
                .foregroundColor(model.discReady ? .green : .orange)
                .font(.system(size: 18))

            VStack(alignment: .leading, spacing: 2) {
                Text(L10n.text(model.discReady ? "Disc Ready to Play" : "ISO Required"))
                    .font(.system(size: 13, weight: .bold))
                    .foregroundColor(.white)
                Text(model.discStatusMessage)
                    .font(.system(size: 11))
                    .foregroundColor(.white.opacity(0.6))
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
            }
            Spacer()
        }
        .padding(10)
        .background(Color.white.opacity(0.05))
        .cornerRadius(12)
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(model.discReady ? Color.green.opacity(0.3) : Color.orange.opacity(0.3), lineWidth: 1))
    }

    private var playButton: some View {
        Button(action: {
            triggerHaptic(style: .heavy)
            if model.discReady {
                model.launchGame()
            } else {
                model.navigationPath.append(.disc)
            }
        }) {
            HStack(spacing: 10) {
                Image(systemName: model.discReady ? "play.fill" : "opticaldisc.fill")
                    .font(.system(size: 18, weight: .black))
                Text(L10n.text(model.discReady ? "Play Game" : "Select ISO to Play"))
                    .font(.system(size: 15, weight: .heavy))
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
            }
            .frame(maxWidth: .infinity)
            .frame(height: 50)
            .background(
                model.discReady ?
                LinearGradient(colors: [Color.green.opacity(0.9), Color.blue.opacity(0.9)], startPoint: .leading, endPoint: .trailing) :
                LinearGradient(colors: [Color.orange.opacity(0.9), Color.red.opacity(0.9)], startPoint: .leading, endPoint: .trailing)
            )
            .foregroundColor(.white)
            .cornerRadius(12)
            .shadow(
                color: model.discReady ? Color.green.opacity(isPulsing ? 0.5 : 0.25) : Color.orange.opacity(isPulsing ? 0.45 : 0.2),
                radius: isPulsing ? 10 : 4,
                y: isPulsing ? 4 : 2
            )
        }
    }

    @ViewBuilder
    private var navigationCards: some View {
        NavCard(
            title: "Disc Manager",
            subtitle: L10n.text(model.discReady ? "SCUS-97177 Verified" : "Select ISO"),
            icon: "opticaldisc",
            color: .blue,
            isFocused: !model.connectedGamepads.isEmpty && model.focusedCardIndex == 0
        ) {
            triggerHaptic(style: .light)
            model.navigationPath.append(.disc)
        }
        NavCard(
            title: "Graphics",
            subtitle: "\(model.aspectRatio.label) • \(model.resolutionLabel)",
            icon: "display",
            color: .purple,
            isFocused: !model.connectedGamepads.isEmpty && model.focusedCardIndex == 1
        ) {
            triggerHaptic(style: .light)
            model.navigationPath.append(.graphics)
        }
        NavCard(
            title: "Controls",
            subtitle: model.connectedGamepads.first ?? L10n.text("Touch Active"),
            icon: "gamecontroller.fill",
            color: .orange,
            isFocused: !model.connectedGamepads.isEmpty && model.focusedCardIndex == 2
        ) {
            triggerHaptic(style: .light)
            model.navigationPath.append(.controls)
        }
        NavCard(
            title: "Online",
            subtitle: L10n.text("Lobby & Rooms"),
            icon: "network",
            color: .cyan,
            isFocused: !model.connectedGamepads.isEmpty && model.focusedCardIndex == 3
        ) {
            triggerHaptic(style: .light)
            model.navigationPath.append(.online)
        }
        NavCard(
            title: "Game Settings",
            subtitle: L10n.text("Rider & Movies"),
            icon: "slider.horizontal.3",
            color: .indigo,
            isFocused: !model.connectedGamepads.isEmpty && model.focusedCardIndex == 4
        ) {
            triggerHaptic(style: .light)
            model.navigationPath.append(.settings)
        }
        NavCard(
            title: "About",
            subtitle: L10n.text("Credits & Info"),
            icon: "info.circle.fill",
            color: .gray,
            isFocused: !model.connectedGamepads.isEmpty && model.focusedCardIndex == 5
        ) {
            triggerHaptic(style: .light)
            model.navigationPath.append(.about)
        }
    }

    private func triggerHaptic(style: UIImpactFeedbackGenerator.FeedbackStyle) {
        let generator = UIImpactFeedbackGenerator(style: style)
        generator.prepare()
        generator.impactOccurred()
    }
}

struct NavCard: View {
    let title: String
    let subtitle: String
    let icon: String
    let color: Color
    var isFocused: Bool = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Image(systemName: icon)
                        .font(.system(size: 16))
                        .foregroundColor(color)
                    Spacer()
                    Image(systemName: "chevron.right")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(.white.opacity(0.3))
                }
                Spacer(minLength: 2)
                Text(L10n.text(title))
                    .font(.system(size: 13, weight: .bold))
                    .foregroundColor(.white)
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
                Text(subtitle)
                    .font(.system(size: 10))
                    .foregroundColor(.white.opacity(0.55))
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
            }
            .padding(10)
            .frame(maxWidth: .infinity, minHeight: 66)
            .background(isFocused ? color.opacity(0.22) : Color.white.opacity(0.06))
            .cornerRadius(12)
            .overlay(
                RoundedRectangle(cornerRadius: 12)
                    .stroke(isFocused ? color : Color.white.opacity(0.08), lineWidth: isFocused ? 2 : 1)
            )
        }
    }
}
