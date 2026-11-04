// OnlineView.swift - Multiplayer Status & Rooms for iOS (Portrait & Landscape)
// GPL-3.0-or-later
import SwiftUI

struct OnlineView: View {
    @ObservedObject var model = LauncherModel.shared
    @State private var joinRoomCode: String = ""

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
                                serverCard
                                roomCard
                            }
                        } else {
                            HStack(alignment: .top, spacing: 16) {
                                serverCard
                                    .frame(maxWidth: .infinity)
                                roomCard
                                    .frame(maxWidth: .infinity)
                            }
                        }
                    }
                    .padding(.horizontal, isPortrait ? 18 : 20)
                    .padding(.vertical, 12)
                }
            }
        }
        .navigationTitle("Online Multiplayer")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                HStack(spacing: 6) {
                    Circle().fill(Color.green).frame(width: 6, height: 6)
                    Text("Online")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundColor(.green)
                }
            }
        }
    }

    private var serverCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Multiplayer Server")
                .font(.system(size: 15, weight: .bold))
                .foregroundColor(.white)

            Text("Each device simulates its own rider and synchronizes the others online.")
                .font(.system(size: 12))
                .foregroundColor(.white.opacity(0.65))
                .lineSpacing(2)

            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Official Server Relay")
                        .font(.system(size: 11))
                        .foregroundColor(.white.opacity(0.5))
                    Text(model.serverAddress)
                        .font(.system(size: 14, weight: .bold))
                        .foregroundColor(.cyan)
                }
                Spacer()
            }
            .padding(10)
            .background(Color.white.opacity(0.04))
            .cornerRadius(10)
        }
        .padding(14)
        .background(Color.white.opacity(0.04))
        .cornerRadius(14)
    }

    private var roomCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Join Race Room")
                .font(.system(size: 15, weight: .bold))
                .foregroundColor(.white)

            HStack(spacing: 8) {
                TextField("Enter Room Code", text: $joinRoomCode)
                    .textFieldStyle(PlainTextFieldStyle())
                    .padding(10)
                    .background(Color.white.opacity(0.08))
                    .cornerRadius(8)
                    .foregroundColor(.white)
                    .font(.system(size: 14, weight: .bold, design: .monospaced))
                    .autocapitalization(.allCharacters)

                Button("Join") {
                    // Handled inside game online menu
                }
                .font(.system(size: 14, weight: .bold))
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .background(Color.cyan)
                .foregroundColor(.black)
                .cornerRadius(8)
            }

            Text("You can also host or browse public lobbies directly inside the game's ONLINE menu.")
                .font(.system(size: 11))
                .foregroundColor(.white.opacity(0.5))
        }
        .padding(14)
        .background(Color.white.opacity(0.04))
        .cornerRadius(14)
    }
}
