// GraphicsSettingsView.swift - Graphics & Performance Options for iOS (Portrait & Landscape)
// GPL-3.0-or-later
import SwiftUI

struct GraphicsOptions: Equatable {
    var profile: PerformanceProfile
    var aspectRatio: AspectRatioOption
    var phoneScale: Int
    var tvScale: Int?
    var outputResolution: Int
    var targetFPS: Int
    var textureFiltering: String
    var vsync: Bool
    var unlimitedTVOutput: Bool

    mutating func selectProfile(_ profile: PerformanceProfile, onTV: Bool, duringGame: Bool) {
        self.profile = profile
        outputResolution = 0
        let scale: Int
        switch profile {
        case .quality: scale = 2
        case .balanced: scale = 0
        case .performance: scale = 1
        }
        if onTV {
            tvScale = scale
        } else {
            phoneScale = scale
            targetFPS = 60
        }
        textureFiltering = profile == .performance ? "Nearest" : "Bilinear"
        if !duringGame { vsync = true }
    }
}

struct GraphicsSettingsView: View {
    @ObservedObject private var model = LauncherModel.shared
    @State private var draft: GraphicsOptions
    let requiresApply: Bool

    init(requiresApply: Bool = false) {
        self.requiresApply = requiresApply
        _draft = State(initialValue: LauncherModel.shared.graphicsOptions)
    }

    private var options: Binding<GraphicsOptions> {
        Binding(
            get: { requiresApply ? draft : model.graphicsOptions },
            set: { newOptions in
                if requiresApply { draft = newOptions }
                else { model.applyGraphicsOptions(newOptions) }
            }
        )
    }

    private var scaleSelection: Binding<Int> {
        if model.externalDisplayConnected {
            return Binding(
                get: { options.wrappedValue.tvScale ?? LauncherModel.defaultTVInternalScale },
                set: { options.tvScale.wrappedValue = $0 }
            )
        }
        return options.phoneScale
    }

    var body: some View {
        GeometryReader { geo in
            let isPortrait = geo.size.height > geo.size.width

            ZStack {
                Color(red: 0.07, green: 0.08, blue: 0.10)
                    .ignoresSafeArea()

                ScrollView(showsIndicators: false) {
                    VStack(spacing: 16) {
                        // Performance profile card
                        VStack(alignment: .leading, spacing: 10) {
                            Text("Performance Profile")
                                .font(.system(size: 15, weight: .bold))
                                .foregroundColor(.white)

                            if isPortrait {
                                VStack(spacing: 8) {
                                    ForEach(PerformanceProfile.allCases) { profile in
                                        profileButton(profile)
                                    }
                                }
                            } else {
                                HStack(spacing: 10) {
                                    ForEach(PerformanceProfile.allCases) { profile in
                                        profileButton(profile)
                                    }
                                }
                            }
                        }
                        .padding(14)
                        .background(Color.white.opacity(0.04))
                        .cornerRadius(14)

                        VStack(alignment: .leading, spacing: 8) {
                            Toggle("Allow TV Output Above 1080p", isOn: options.unlimitedTVOutput)
                                .disabled(model.isGameRunning)
                            Text("TV mode uses 16:9 and selects 60 or 120 FPS based on the display connection. Enable before launching to allow native TV resolution with Auto output.")
                                .font(.system(size: 11))
                                .foregroundColor(.white.opacity(0.6))
                        }
                        .padding(12)
                        .background(Color.white.opacity(0.04))
                        .cornerRadius(12)

                        // Granular Settings
                        if isPortrait {
                            VStack(spacing: 12) {
                                aspectRatioSetting
                                resolutionSetting
                                outputResolutionSetting
                                targetFpsSetting
                                filteringSetting
                                vsyncSetting
                            }
                        } else {
                            HStack(alignment: .top, spacing: 16) {
                                VStack(spacing: 12) {
                                    aspectRatioSetting
                                    resolutionSetting
                                    outputResolutionSetting
                                }
                                .frame(maxWidth: .infinity)

                                VStack(spacing: 12) {
                                    targetFpsSetting
                                    filteringSetting
                                    vsyncSetting
                                }
                                .frame(maxWidth: .infinity)
                            }
                        }
                    }
                    .padding(.horizontal, isPortrait ? 18 : 20)
                    .padding(.vertical, 12)
                }
            }
        }
        .navigationTitle("Graphics Settings")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                Text("Metal")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundColor(.purple.opacity(0.9))
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(Color.purple.opacity(0.15))
                    .cornerRadius(6)
            }
        }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            if requiresApply {
                Button {
                    model.applyGraphicsOptions(draft)
                } label: {
                    Text("Apply Changes")
                        .fontWeight(.semibold)
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .disabled(draft == model.graphicsOptions)
                .padding(.horizontal, 18)
                .padding(.vertical, 8)
                .background(.bar)
            }
        }
        .onChange(of: model.externalDisplayConnected) {
            if requiresApply { draft.tvScale = model.graphicsOptions.tvScale }
        }
    }

    private func profileButton(_ profile: PerformanceProfile) -> some View {
        Button(action: {
            var updated = options.wrappedValue
            updated.selectProfile(profile, onTV: model.externalDisplayConnected, duringGame: model.isGameRunning)
            options.wrappedValue = updated
        }) {
            HStack(spacing: 10) {
                VStack(alignment: .leading, spacing: 3) {
                    HStack {
                        Text(profile.title)
                            .font(.system(size: 15, weight: .bold))
                            .foregroundColor(options.wrappedValue.profile == profile ? .white : .white.opacity(0.7))
                        if options.wrappedValue.profile == profile {
                            Image(systemName: "checkmark.circle.fill")
                                .foregroundColor(.green)
                                .font(.system(size: 14))
                        }
                    }
                    Text(profile.description)
                        .font(.system(size: 11))
                        .foregroundColor(.white.opacity(0.6))
                        .fixedSize(horizontal: false, vertical: true)
                        .multilineTextAlignment(.leading)
                }
                Spacer()
            }
            .padding(10)
            .background(options.wrappedValue.profile == profile ? Color.purple.opacity(0.3) : Color.white.opacity(0.05))
            .cornerRadius(10)
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(options.wrappedValue.profile == profile ? Color.purple : Color.white.opacity(0.08), lineWidth: 1.5))
        }
    }

    private var aspectRatioSetting: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Aspect Ratio")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundColor(.white)
                Spacer()
                Text(model.externalDisplayConnected ? model.outputAspectLabel : options.wrappedValue.aspectRatio.label)
                    .font(.system(size: 12, weight: .bold))
                    .foregroundColor(.cyan)
            }

            Picker("Aspect Ratio", selection: options.aspectRatio) {
                ForEach(AspectRatioOption.allCases) { opt in
                    Text(opt.label).tag(opt)
                }
            }
            .pickerStyle(.menu)
            .disabled(model.externalDisplayConnected)

            Text(model.externalDisplayConnected ? L10n.text("TV mode automatically uses 16:9.") : options.wrappedValue.aspectRatio.subtitle)
                .font(.system(size: 11))
                .foregroundColor(.white.opacity(0.55))
        }
        .padding(12)
        .background(Color.white.opacity(0.04))
        .cornerRadius(12)
    }

    private var resolutionSetting: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Internal Resolution")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundColor(.white)
                Spacer()
                Text(model.resolutionLabel)
                    .font(.system(size: 12, weight: .bold))
                    .foregroundColor(.purple)
            }
            Picker("", selection: scaleSelection) {
                Text("Auto").tag(0)
                ForEach(1...6, id: \.self) { scale in
                    Text("\(scale)x").tag(scale)
                }
            }
            .pickerStyle(.menu)
            .accessibilityLabel("Internal Resolution")

            Text(model.externalDisplayConnected
                 ? L10n.format("TV mode starts at %lldx. You can change the scale during play.", Int64(LauncherModel.defaultTVInternalScale))
                 : L10n.text("Auto selects 1x or 2x based on temperature and power. You can change the scale during play."))
                .font(.system(size: 11))
                .foregroundColor(.white.opacity(0.55))
        }
        .padding(12)
        .background(Color.white.opacity(0.04))
        .cornerRadius(12)
    }

    private var outputResolutionSetting: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Output Resolution")
                .font(.system(size: 14, weight: .semibold))
            Picker("Output Resolution", selection: options.outputResolution) {
                Text("Auto").tag(0)
                ForEach([540, 720, 900, 1080], id: \.self) { lines in
                    Text("\(lines)p").tag(lines)
                }
            }
            .pickerStyle(SegmentedPickerStyle())
            Text("Limits the shorter side of the image. Auto follows the selected profile.")
                .font(.system(size: 11))
                .foregroundColor(.white.opacity(0.55))
        }
        .padding(12)
        .background(Color.white.opacity(0.04))
        .cornerRadius(12)
    }

    private var targetFpsSetting: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Frame Rate")
                .font(.system(size: 14, weight: .semibold))
                .foregroundColor(.white)
            Picker("", selection: Binding(
                get: {
                    if model.externalDisplayConnected { return model.effectiveFPS }
                    return model.supports120FPS ? options.wrappedValue.targetFPS : 60
                },
                set: { options.targetFPS.wrappedValue = $0 }
            )) {
                Text("60 FPS").tag(60)
                if model.supports120FPS {
                    Text("120 FPS").tag(120)
                }
            }
            .pickerStyle(SegmentedPickerStyle())
            .disabled(model.externalDisplayConnected)
            Text(model.externalDisplayConnected
                 ? L10n.format("Automatic TV target: %lld FPS", Int64(model.effectiveFPS))
                 : L10n.format("Current target: %lld FPS • %@", Int64(model.effectiveFPS), model.thermalStatus))
                .font(.system(size: 11))
                .foregroundColor(.white.opacity(0.55))
        }
        .padding(12)
        .background(Color.white.opacity(0.04))
        .cornerRadius(12)
    }

    private var filteringSetting: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Image Filter")
                .font(.system(size: 14, weight: .semibold))
                .foregroundColor(.white)
            Picker("", selection: options.textureFiltering) {
                Text("Smooth (Linear)").tag("Bilinear")
                Text("Sharp (Pixels)").tag("Nearest")
            }
            .pickerStyle(SegmentedPickerStyle())
        }
        .padding(12)
        .background(Color.white.opacity(0.04))
        .cornerRadius(12)
    }

    private var vsyncSetting: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("Vertical Sync (VSync)")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundColor(.white)
                Text("Applies the next time you launch the game.")
                    .font(.system(size: 11))
                    .foregroundColor(.white.opacity(0.55))
            }
            Spacer()
            Toggle("", isOn: options.vsync)
            .labelsHidden()
            .disabled(model.isGameRunning)
        }
        .padding(12)
        .background(Color.white.opacity(0.04))
        .cornerRadius(12)
    }
}
