// LauncherApp.swift - Application Entry Point for iOS
// GPL-3.0-or-later
import SwiftUI

@main
struct LauncherApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            LauncherMainView()
                .onChange(of: scenePhase, initial: true) { _, phase in
                    switch phase {
                    case .active:
                        ios_lifecycle_did_become_active()
                        LauncherModel.shared.launchGameOnStartup()
                    case .inactive: ios_lifecycle_will_resign_active()
                    case .background: ios_lifecycle_did_enter_background()
                    @unknown default: ios_lifecycle_will_resign_active()
                    }
                }
        }
    }
}

class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        return .allButUpsideDown
    }

    func applicationWillResignActive(_ application: UIApplication) {
        ios_lifecycle_will_resign_active()
    }

    func applicationDidEnterBackground(_ application: UIApplication) {
        ios_lifecycle_did_enter_background()
    }

    func applicationWillEnterForeground(_ application: UIApplication) {
        ios_lifecycle_will_enter_foreground()
    }

    func applicationDidBecomeActive(_ application: UIApplication) {
        ios_lifecycle_did_become_active()
    }

    func applicationDidReceiveMemoryWarning(_ application: UIApplication) {
        ios_lifecycle_memory_warning()
    }
}

