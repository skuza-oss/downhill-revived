// AboutView.swift - Legal & Project Credits for iOS
// GPL-3.0-or-later
import SwiftUI

struct AboutView: View {
    @ObservedObject var model = LauncherModel.shared

    var body: some View {
        ZStack {
            Color(red: 0.07, green: 0.08, blue: 0.10)
                .ignoresSafeArea()

            ScrollView(showsIndicators: false) {
                VStack(alignment: .leading, spacing: 14) {
                    Text("Downhill Revived (iOS Native Port)")
                        .font(.system(size: 16, weight: .bold))
                        .foregroundColor(.white)

                    Text("An iOS port of Downhill Domination (PlayStation 2, Incognito Entertainment / SCEA, 2003), with Metal graphics, touch controls and gamepad support.")
                        .font(.system(size: 13))
                        .foregroundColor(.white.opacity(0.75))
                        .lineSpacing(3)

                    Divider().background(Color.white.opacity(0.15))

                    Text("Legal Notice")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundColor(.white)

                    Text("An independent open-source fan project, unaffiliated with Sony Interactive Entertainment, Incognito Entertainment or the rights holders. Game assets are loaded from your own Downhill Domination USA disc image (SCUS-97177).")
                        .font(.system(size: 12))
                        .foregroundColor(.white.opacity(0.65))
                        .lineSpacing(2)

                    Divider().background(Color.white.opacity(0.15))

                    Text("Open Source Credits & Third-Party Notices")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundColor(.white)

                    VStack(alignment: .leading, spacing: 6) {
                        Text("• License: GNU General Public License v3.0 or later (GPL-3.0-or-later)")
                        Text("• Official Source Repository: https://github.com/skuza-oss/downhill-revived")
                        Text("• Touchscreen Control Assets: Kenney (www.kenney.nl) · CC0 1.0 Universal")
                        Text("• SDL3: Simple DirectMedia Layer (zlib license)")
                        Text("• wgpu & Metal backend: Rust Graphics Project (MIT / Apache-2.0)")
                        Text("• WABT wasm2c: WebAssembly Binary Toolkit (Apache-2.0)")
                        Text("• Hardware Behavioral References: PCSX2 and ps2sdk teams")
                    }
                    .font(.system(size: 12))
                    .foregroundColor(.white.opacity(0.6))
                    .lineSpacing(3)
                }
                .padding(18)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.white.opacity(0.04))
                .cornerRadius(14)
                .padding(.horizontal, 18)
                .padding(.vertical, 12)
            }
        }
        .navigationTitle("About Downhill Revived")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                Text("v\(model.appVersion)")
                    .font(.system(size: 11, weight: .bold))
                    .padding(.horizontal, 7)
                    .padding(.vertical, 3)
                    .background(Color.white.opacity(0.12))
                    .clipShape(Capsule())
                    .foregroundColor(.white.opacity(0.85))
            }
        }
    }
}
