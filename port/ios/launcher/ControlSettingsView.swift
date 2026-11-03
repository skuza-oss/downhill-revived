// ControlSettingsView.swift - Touchscreen & Gamepad Configuration for iOS (Portrait & Landscape)
// GPL-3.0-or-later
import SwiftUI

struct ControlSettingsView: View {
    @ObservedObject var model = LauncherModel.shared
    var isTVPreview = false

    var body: some View {
        GeometryReader { geo in
            let isPortrait = geo.size.height > geo.size.width

            ZStack {
                Color(red: 0.07, green: 0.08, blue: 0.10)
                    .ignoresSafeArea()

                ScrollView(showsIndicators: false) {
                    VStack(spacing: 16) {
                        // Physical Gamepads Card
                        gamepadsCard

                        // Button Glyphs Card (PlayStation / Xbox / Auto)
                        controllerStyleCard

                        // Touch Controls Card
                        VStack(alignment: .leading, spacing: 12) {
                            Text("Touchscreen Controls")
                                .font(.system(size: 15, weight: .bold))
                                .foregroundColor(.white)

                            if isPortrait {
                                VStack(spacing: 14) {
                                    slidersColumn
                                    liveControlsPreview
                                    togglesColumn
                                }
                            } else {
                                HStack(alignment: .top, spacing: 16) {
                                    VStack(spacing: 12) {
                                        slidersColumn
                                        liveControlsPreview
                                    }
                                    .frame(maxWidth: .infinity)
                                    togglesColumn
                                        .frame(maxWidth: .infinity)
                                }
                            }
                        }
                        .padding(14)
                        .background(Color.white.opacity(0.04))
                        .cornerRadius(14)
                    }
                    .padding(.horizontal, isPortrait ? 18 : 20)
                    .padding(.vertical, 12)
                }
            }
        }
        .navigationTitle("Controls & Input")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                Button("Defaults") {
                    touch_input_restore_defaults()
                    model.touchOpacity = 0.50
                    model.touchDeadzone = 0.15
                    model.touchSensitivity = 1.0
                    model.floatingStick = true
                    model.autoHideOnGamepad = true
                    model.hapticsEnabled = true
                    model.controllerStyle = .auto
                    model.savePreferences()
                }
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(.orange)
            }
        }
        .onDisappear {
            if !isTVPreview { model.savePreferences() }
        }
    }

    private var gamepadsCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Image(systemName: "gamecontroller.fill")
                    .foregroundColor(.blue)
                Text("Physical Controllers (Xbox / PS4 / PS5 / MFi)")
                    .font(.system(size: 14, weight: .bold))
                    .foregroundColor(.white)
                Spacer()
                Text(model.connectedGamepadDetails.isEmpty ? L10n.text("None") : L10n.format("Connected: %lld", Int64(model.connectedGamepadDetails.count)))
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(model.connectedGamepadDetails.isEmpty ? .white.opacity(0.5) : .green)
            }

            if model.connectedGamepadDetails.isEmpty {
                Text("Compatible Bluetooth and USB-C controllers connect automatically, with analog triggers and vibration.")
                    .font(.system(size: 11))
                    .foregroundColor(.white.opacity(0.6))
            } else {
                ForEach(model.connectedGamepadDetails) { pad in
                    HStack(spacing: 10) {
                        Circle().fill(Color.green).frame(width: 8, height: 8)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(pad.name)
                                .font(.system(size: 13, weight: .bold))
                                .foregroundColor(.white)
                            Text(L10n.format("%@ • Player %lld", pad.isXbox ? L10n.text("Xbox Layout") : pad.isPlayStation ? L10n.text("PlayStation Layout") : pad.category, Int64(pad.playerIndex)))
                                .font(.system(size: 10))
                                .foregroundColor(.white.opacity(0.6))
                        }
                        Spacer()
                        if let pct = pad.batteryPercentage {
                            HStack(spacing: 4) {
                                Image(systemName: pad.isCharging ? "battery.100.bolt" : "battery.100")
                                    .foregroundColor(.green)
                                    .font(.system(size: 11))
                                Text("\(pct)%")
                                    .font(.system(size: 11, weight: .bold))
                                    .foregroundColor(.white.opacity(0.85))
                            }
                            .padding(.horizontal, 8)
                            .padding(.vertical, 4)
                            .background(Color.white.opacity(0.08))
                            .clipShape(Capsule())
                        }
                    }
                    .padding(8)
                    .background(Color.white.opacity(0.05))
                    .cornerRadius(8)
                }
            }
        }
        .padding(12)
        .background(Color.white.opacity(0.04))
        .cornerRadius(12)
    }

    private var controllerStyleCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Button Prompts & Labels")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundColor(.white)
                Spacer()
                Text(model.useXboxGlyphs ? "Xbox (A B X Y)" : "PlayStation (✕ ○ □ △)")
                    .font(.system(size: 11, weight: .bold))
                    .foregroundColor(.orange)
            }

            Picker("Glyphs", selection: Binding(
                get: { model.controllerStyle },
                set: { newVal in
                    model.controllerStyle = newVal
                    model.savePreferences()
                }
            )) {
                ForEach(ControllerStyle.allCases) { st in
                    Text(L10n.text(st.rawValue)).tag(st)
                }
            }
            .pickerStyle(SegmentedPickerStyle())
        }
        .padding(12)
        .background(Color.white.opacity(0.04))
        .cornerRadius(12)
    }

    private var liveControlsPreview: some View {
        let xbox = model.useXboxGlyphs
        return VStack(spacing: 8) {
            HStack {
                Text("Live Controls Preview")
                    .font(.system(size: 12, weight: .bold))
                    .foregroundColor(.white.opacity(0.8))
                Spacer()
                Text(L10n.format("Opacity: %lld%%", Int64(model.touchOpacity * 100)))
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundColor(.blue)
            }

            HStack {
                // Mini Analog Stick
                ZStack {
                    Circle()
                        .stroke(Color.white.opacity(0.4), lineWidth: 2)
                        .background(Circle().fill(Color.black.opacity(0.35)))
                        .frame(width: 48, height: 48)
                    Circle()
                        .fill(Color.white.opacity(0.7))
                        .frame(width: 22, height: 22)
                }
                .padding(.leading, 12)

                Spacer()

                // Mini Diamond Buttons
                VStack(spacing: 3) {
                    Text(xbox ? "Y" : "△")
                        .font(.system(size: 12, weight: .black))
                        .foregroundColor(xbox ? .yellow : .green)
                    HStack(spacing: 14) {
                        Text(xbox ? "X" : "□")
                            .font(.system(size: 12, weight: .black))
                            .foregroundColor(xbox ? .cyan : .pink)
                        Text(xbox ? "B" : "○")
                            .font(.system(size: 12, weight: .black))
                            .foregroundColor(.red)
                    }
                    Text(xbox ? "A" : "✕")
                        .font(.system(size: 12, weight: .black))
                        .foregroundColor(xbox ? .green : .blue)
                }
                .padding(.trailing, 16)
            }
            .frame(height: 58)
            .opacity(model.touchOpacity)
            .background(Color.black.opacity(0.25))
            .cornerRadius(10)
        }
        .padding(10)
        .background(Color.white.opacity(0.03))
        .cornerRadius(10)
    }

    private var slidersColumn: some View {
        VStack(spacing: 10) {
            ControlSlider(title: "Controls Opacity", valueText: "\(Int(model.touchOpacity * 100))%", value: $model.touchOpacity, range: 0.10...1.0)
            ControlSlider(title: "Stick Deadzone", valueText: "\(Int(model.touchDeadzone * 100))%", value: $model.touchDeadzone, range: 0.0...0.40)
            ControlSlider(title: "Stick Sensitivity", valueText: L10n.format("%.1fx", model.touchSensitivity), value: $model.touchSensitivity, range: 0.5...2.0)
        }
    }

    private var togglesColumn: some View {
        VStack(spacing: 10) {
            toggleRow(title: "Floating Analog Stick", subtitle: "Centers where thumb touches down", binding: $model.floatingStick)
            toggleRow(title: "Auto-Hide on Gamepad", subtitle: "Touch to show the controls. Use the gamepad to hide them.", binding: $model.autoHideOnGamepad)
            toggleRow(title: "Haptic & Rumble Feedback", subtitle: "Vibrates on button tap & DualShock motors", binding: $model.hapticsEnabled)
        }
    }

    private func toggleRow(title: String, subtitle: String, binding: Binding<Bool>) -> some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(L10n.text(title))
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundColor(.white)
                Text(L10n.text(subtitle))
                    .font(.system(size: 10))
                    .foregroundColor(.white.opacity(0.5))
            }
            Spacer()
            Toggle("", isOn: binding)
                .labelsHidden()
        }
        .padding(10)
        .background(Color.white.opacity(0.04))
        .cornerRadius(10)
    }
}

struct ControlSlider: View {
    let title: String
    let valueText: String
    @Binding var value: Double
    let range: ClosedRange<Double>

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(L10n.text(title))
                    .font(.system(size: 13, weight: .medium))
                    .foregroundColor(.white)
                Spacer()
                Text(valueText)
                    .font(.system(size: 13, weight: .bold))
                    .foregroundColor(.blue)
            }
            Slider(value: $value, in: range)
                .accentColor(.blue)
        }
        .padding(8)
        .background(Color.white.opacity(0.03))
        .cornerRadius(8)
    }
}
