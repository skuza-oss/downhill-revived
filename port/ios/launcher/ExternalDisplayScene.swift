// Dedicated AirPlay and HDMI scene for the launcher and game output.
// GPL-3.0-or-later
import SwiftUI
import UIKit

@objc(ExternalDisplaySceneDelegate)
final class ExternalDisplaySceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    private var host: TVDisplayHostController?

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession,
               options connectionOptions: UIScene.ConnectionOptions) {
        guard let scene = scene as? UIWindowScene else { return }
        configureMode(screen: scene.screen)
        let host = TVDisplayHostController()
        let window = UIWindow(windowScene: scene)
        window.rootViewController = host
        self.host = host
        self.window = window
        // Never take keyboard/controller focus away from the phone.
        window.isHidden = false
        host.view.layoutIfNeeded()
        ios_metal_attach_external_display(host.gameHost)
        LauncherModel.shared.setExternalDisplayConnected(true)
        LauncherModel.shared.hideGameChrome()
    }

    func applyDisplayMode() {
        guard let scene = window?.windowScene else { return }
        configureMode(screen: scene.screen)
        host?.view.setNeedsLayout()
        host?.view.layoutIfNeeded()
    }

    private func configureMode(screen: UIScreen) {
        let model = LauncherModel.shared
        // AirPlay can expose a single negotiated mode. UIKit owns the scene size.
        let modes = screen.availableModes
        if modes.count > 1 {
            let compatible = modes.filter {
                $0.size.height > 0 && abs($0.size.width / $0.size.height - 16.0 / 9.0) < 0.01 &&
                (model.externalResolutionUnlimited || ($0.size.width <= 1920 && $0.size.height <= 1080))
            }
            if let best = compatible.max(by: {
                $0.size.width * $0.size.height < $1.size.width * $1.size.height
            }), screen.currentMode !== best {
                screen.currentMode = best
            }
        }
        screen.overscanCompensation = .none
    }

    func sceneDidDisconnect(_ scene: UIScene) {
        if let host { ios_metal_detach_external_display(host.gameHost) }
        window?.isHidden = true
        window = nil
        host = nil
        LauncherModel.shared.setExternalDisplayConnected(false)
        LauncherModel.shared.hideGameChrome()
    }
}

private struct TVLauncherView: View {
    @ObservedObject private var model = LauncherModel.shared

    var body: some View {
        if !model.isGameRunning {
            GeometryReader { geo in
                LauncherNavigationView(path: .constant(model.navigationPath), size: geo.size,
                                       safe: geo.safeAreaInsets, isTVPreview: true)
            }
            .preferredColorScheme(.dark)
        }
    }
}

// Keep the launcher above the game layer while browsing. The Metal surface stays
// attached to the same host when starting or leaving a game.
private final class TVDisplayHostController: UIViewController {
    let gameHost = UIViewController()
    private let launcherHost = UIHostingController(rootView: TVLauncherView())

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        view.isUserInteractionEnabled = false
        launcherHost.view.backgroundColor = .clear
        launcherHost.view.isOpaque = false
        launcherHost.overrideUserInterfaceStyle = .dark
        let children: [UIViewController] = [gameHost, launcherHost]
        for child in children {
            addChild(child)
            view.addSubview(child.view)
            child.didMove(toParent: self)
        }
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let bounds = view.bounds
        let width = min(bounds.width, bounds.height * 16 / 9)
        let height = width * 9 / 16
        gameHost.view.frame = CGRect(x: bounds.midX - width / 2, y: bounds.midY - height / 2,
                                     width: width, height: height)
        launcherHost.view.frame = bounds
    }
}
