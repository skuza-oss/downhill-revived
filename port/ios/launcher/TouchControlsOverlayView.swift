// TouchControlsOverlayView.swift - Transparent Touchscreen Overlay for Gameplay
// GPL-3.0-or-later
import SwiftUI

struct TouchControlsOverlayView: View {
    @ObservedObject var model = LauncherModel.shared
    @Environment(\.scenePhase) private var scenePhase
    let safe: EdgeInsets

    @State private var stickOffset: CGSize = .zero
    @State private var stickActive: Bool = false
    @State private var pressedButtons: Set<TouchControlID> = []

    var body: some View {
        let xbox = model.useXboxGlyphs
        let topControlInset = max(44, safe.top + 16)

        HStack(alignment: .bottom, spacing: 0) {
            // MARK: - Left Column (Shoulders + Analog Stick)
            VStack(alignment: .leading, spacing: 12) {
                Spacer(minLength: topControlInset)
                VStack(spacing: 6) {
                    overlayShoulder(title: xbox ? "LT" : "L2", id: TOUCH_CTRL_L2)
                    overlayShoulder(title: xbox ? "LB" : "L1", id: TOUCH_CTRL_L1)
                }
                .frame(width: 140)

                // Bottom-Left Steering Stick
                ZStack {
                    Circle()
                        .fill(Color.black.opacity(0.40))
                        .frame(width: 140, height: 140)
                        .overlay(Circle().stroke(Color.white.opacity(0.35), lineWidth: 3))

                    Circle()
                        .fill(
                            LinearGradient(
                                colors: [Color.white.opacity(stickActive ? 0.8 : 0.55), Color.gray.opacity(0.35)],
                                startPoint: .topLeading,
                                endPoint: .bottomTrailing
                            )
                        )
                        .frame(width: 64, height: 64)
                        .overlay(Circle().stroke(Color.white.opacity(0.6), lineWidth: 1.5))
                        .shadow(color: .black.opacity(0.6), radius: 6)
                        .offset(stickOffset)
                }
                .frame(width: 140, height: 140)
                .contentShape(Circle())
                .overlay(ImmediateTouchInput(
                        onDown: { point, size in
                            stickActive = true
                            updateStick(point: point, origin: model.floatingStick ? point : CGPoint(x: size.width / 2, y: size.height / 2), radius: 50.0)
                        },
                        onMove: { point, origin, size in
                            updateStick(point: point, origin: model.floatingStick ? origin : CGPoint(x: size.width / 2, y: size.height / 2), radius: 50.0)
                        },
                        onUp: {
                            touch_input_set_stick(0, 0)
                            stickActive = false
                            stickOffset = .zero
                        }
                    ))
                .padding(.bottom, max(safe.bottom + 8, 18))
            }
            .frame(width: 190)
            .padding(.leading, max(safe.leading, 24))

            Spacer(minLength: 0)

            // MARK: - Right Column (Shoulders + Action Diamond)
            VStack(alignment: .trailing, spacing: 12) {
                Spacer(minLength: topControlInset)
                VStack(spacing: 6) {
                    overlayShoulder(title: xbox ? "RT" : "R2", id: TOUCH_CTRL_R2)
                    overlayShoulder(title: xbox ? "RB" : "R1", id: TOUCH_CTRL_R1)
                }
                .frame(width: 150)

                // Bottom-Right Action Diamond (Triangle, Square, Circle, Cross)
                ZStack {
                    // Top (Triangle / Y - Brake)
                    overlayActionButton(
                        title: xbox ? "Y" : "△",
                        color: xbox ? Color(red: 0.95, green: 0.78, blue: 0.20) : Color(red: 0.30, green: 0.85, blue: 0.50),
                        id: TOUCH_CTRL_TRIANGLE,
                        radius: 25
                    )
                    .offset(y: -48)

                    // Left (Square / X - Punch)
                    overlayActionButton(
                        title: xbox ? "X" : "□",
                        color: xbox ? Color(red: 0.25, green: 0.65, blue: 0.95) : Color(red: 0.95, green: 0.45, blue: 0.75),
                        id: TOUCH_CTRL_SQUARE,
                        radius: 25
                    )
                    .offset(x: -48)

                    // Right (Circle / B - Kick)
                    overlayActionButton(
                        title: xbox ? "B" : "○",
                        color: Color(red: 0.95, green: 0.35, blue: 0.35),
                        id: TOUCH_CTRL_CIRCLE,
                        radius: 25
                    )
                    .offset(x: 48)

                    // Bottom (Cross / A - Pedal / Sprint)
                    overlayActionButton(
                        title: xbox ? "A" : "✕",
                        color: xbox ? Color(red: 0.30, green: 0.85, blue: 0.40) : Color(red: 0.35, green: 0.55, blue: 0.95),
                        id: TOUCH_CTRL_CROSS,
                        radius: 28
                    )
                    .offset(y: 48)
                }
                .frame(width: 150, height: 150)
                .padding(.bottom, max(safe.bottom + 8, 18))
            }
            .frame(width: 190)
            .padding(.trailing, max(safe.trailing, 24))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .opacity(model.shouldShowTouchControls ? model.touchOpacity : 0)
        .allowsHitTesting(model.shouldShowTouchControls)
        .overlay(alignment: .top) {
            VStack(spacing: 8) {
                HStack(spacing: 20) {
                    overlayMenuPill(title: xbox ? "VIEW" : "SELECT", id: TOUCH_CTRL_SELECT)
                    overlayMenuPill(title: xbox ? "MENU" : "START", id: TOUCH_CTRL_START)
                }
                .opacity(model.shouldShowTouchControls ? model.touchOpacity : 0)
                .allowsHitTesting(model.shouldShowTouchControls)

                InGameMenuButton(model: model)
            }
            .padding(.top, topControlInset)
        }
        .onDisappear { resetTouchState() }
        .onChange(of: model.shouldShowTouchControls) { _, shown in
            if !shown { resetTouchState() }
        }
        .onChange(of: scenePhase) { _, phase in
            if phase != .active { resetTouchState() }
        }
        .onChange(of: model.showInGameMenu) { _, shown in
            if shown { resetTouchState() }
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willResignActiveNotification)) { _ in
            resetTouchState()
        }
    }

    private func resetTouchState() {
        pressedButtons.removeAll()
        stickActive = false
        stickOffset = .zero
        touch_input_reset()
    }

    // Action Button Builder
    private func overlayActionButton(title: String, color: Color, id: TouchControlID, radius: CGFloat) -> some View {
        let isPressed = pressedButtons.contains(id)
        return ZStack {
            Circle()
                .fill(isPressed ? color.opacity(0.5) : Color.black.opacity(0.45))
                .frame(width: radius * 2, height: radius * 2)
            Circle()
                .stroke(color.opacity(isPressed ? 1.0 : 0.85), lineWidth: isPressed ? 4 : 3)
                .frame(width: radius * 2, height: radius * 2)
            Text(title)
                .font(.system(size: radius * 0.85, weight: .black))
                .foregroundColor(color)
        }
        .scaleEffect(isPressed ? 0.92 : 1.0)
        .contentShape(Circle())
        .overlay(ImmediateTouchInput(
                    onDown: { _, _ in
                        touch_input_set_button(id, true)
                        pressedButtons.insert(id)
                        haptic(.medium)
                    },
                    onUp: {
                        touch_input_set_button(id, false)
                        pressedButtons.remove(id)
                    }
                ))
    }

    // Shoulder Button Builder
    private func overlayShoulder(title: String, id: TouchControlID) -> some View {
        let isPressed = pressedButtons.contains(id)
        return ZStack {
            RoundedRectangle(cornerRadius: 10)
                .fill(isPressed ? Color.white.opacity(0.3) : Color.black.opacity(0.40))
                .frame(width: 70, height: 38)
            RoundedRectangle(cornerRadius: 10)
                .stroke(Color.white.opacity(isPressed ? 0.9 : 0.40), lineWidth: 1.5)
                .frame(width: 70, height: 38)
            Text(title)
                .font(.system(size: 14, weight: .black))
                .foregroundColor(.white.opacity(0.9))
        }
        .scaleEffect(isPressed ? 0.94 : 1.0)
        .contentShape(RoundedRectangle(cornerRadius: 10))
        .overlay(ImmediateTouchInput(
                    onDown: { _, _ in
                        touch_input_set_button(id, true)
                        pressedButtons.insert(id)
                        haptic(.light)
                    },
                    onUp: {
                        touch_input_set_button(id, false)
                        pressedButtons.remove(id)
                    }
                ))
    }

    // Menu Pill Builder
    private func overlayMenuPill(title: String, id: TouchControlID) -> some View {
        let isPressed = pressedButtons.contains(id)
        return ZStack {
            Capsule()
                .fill(isPressed ? Color.white.opacity(0.3) : Color.black.opacity(0.40))
                .frame(width: 72, height: 28)
            Capsule()
                .stroke(Color.white.opacity(isPressed ? 0.85 : 0.35), lineWidth: 1.5)
                .frame(width: 72, height: 28)
            Text(title)
                .font(.system(size: 11, weight: .black))
                .foregroundColor(.white.opacity(0.85))
        }
        .scaleEffect(isPressed ? 0.94 : 1.0)
        .contentShape(Capsule())
        .overlay(ImmediateTouchInput(
                    onDown: { _, _ in
                        touch_input_set_button(id, true)
                        pressedButtons.insert(id)
                        haptic(.light)
                    },
                    onUp: {
                        touch_input_set_button(id, false)
                        pressedButtons.remove(id)
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
