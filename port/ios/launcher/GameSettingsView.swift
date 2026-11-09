// GameSettingsView.swift - Rider, Audio & Save Management for iOS (Portrait & Landscape)
// GPL-3.0-or-later
import SwiftUI

struct GameSettingsView: View {
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
                        if isPortrait {
                            VStack(spacing: 14) {
                                riderCard
                                audioCard
                            }
                        } else {
                            HStack(alignment: .top, spacing: 16) {
                                riderCard
                                    .frame(maxWidth: .infinity)
                                audioCard
                                    .frame(maxWidth: .infinity)
                            }
                        }
                    }
                    .padding(.horizontal, isPortrait ? 18 : 20)
                    .padding(.vertical, 12)
                }
            }
        }
        .navigationTitle("Game Settings")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                Button("Save") {
                    model.savePreferences()
                }
                .font(.system(size: 14, weight: .bold))
                .foregroundColor(.green)
            }
        }
        .onDisappear {
            if !isTVPreview { model.savePreferences() }
        }
    }

    private var riderCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Player Identity & Video")
                .font(.system(size: 15, weight: .bold))
                .foregroundColor(.white)

            VStack(alignment: .leading, spacing: 6) {
                Text("Online Rider Name")
                    .font(.system(size: 12))
                    .foregroundColor(.white.opacity(0.6))
                TextField("Enter rider name", text: $model.riderName)
                    .textFieldStyle(PlainTextFieldStyle())
                    .padding(10)
                    .background(Color.white.opacity(0.08))
                    .cornerRadius(8)
                    .foregroundColor(.white)
            }

            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Skip Intro Movies")
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundColor(.white)
                    Text("Jumps directly to Title Menu on startup")
                        .font(.system(size: 10))
                        .foregroundColor(.white.opacity(0.5))
                }
                Spacer()
                Toggle("", isOn: $model.skipMovies)
                    .labelsHidden()
            }
            .padding(10)
            .background(Color.white.opacity(0.04))
            .cornerRadius(10)
        }
        .padding(14)
        .background(Color.white.opacity(0.04))
        .cornerRadius(14)
    }

    private var audioCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Audio Levels")
                .font(.system(size: 15, weight: .bold))
                .foregroundColor(.white)

            ControlSlider(title: "Music Volume", valueText: "\(Int(model.musicVolume * 100))%", value: $model.musicVolume, range: 0.0...1.0)
            ControlSlider(title: "Sound Effects (SFX)", valueText: "\(Int(model.sfxVolume * 100))%", value: $model.sfxVolume, range: 0.0...1.0)
        }
        .padding(14)
        .background(Color.white.opacity(0.04))
        .cornerRadius(14)
    }
}
