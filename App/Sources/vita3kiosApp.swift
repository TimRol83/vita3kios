import SwiftUI

@main
struct vita3kiosApp: App {
    @StateObject private var core = CoreStatusModel()

    var body: some Scene {
        WindowGroup {
            RootView(core: core)
                .accentColor(PlayStationAccent.blue)
        }
    }
}
