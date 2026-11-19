import SwiftUI

struct SettingsView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var core: CoreStatusModel

    var body: some View {
        NavigationView {
            List {
                Section("Boot") {
                    Label("Direct Game", systemImage: "play.fill")
                    Label("System Software", systemImage: "rectangle.inset.filled")
                }

                Section("Firmware") {
                    HStack { Text("Version"); Spacer(); Text(core.firmwareVersion).foregroundColor(.secondary) }
                    HStack { Text("Direct Game firmware"); Spacer(); Text(core.firmwareReady ? "Available" : "Optional").foregroundColor(.secondary) }
                    HStack { Text("System Software shell"); Spacer(); Text(core.systemSoftwareReady ? "Ready" : "Not ready").foregroundColor(.secondary) }
                    HStack { Text("PUP validation"); Spacer(); Text(availability(CoreCapability.firmwarePUPPreflight)).foregroundColor(.secondary) }
                    HStack { Text("PUP installation"); Spacer(); Text(availability(CoreCapability.firmwarePUPInstall)).foregroundColor(.secondary) }
                    HStack { Text("Partition inventory"); Spacer(); Text(availability(CoreCapability.firmwareInventory)).foregroundColor(.secondary) }
                    HStack { Text("Shell preflight"); Spacer(); Text(availability(CoreCapability.systemShellPreflight)).foregroundColor(.secondary) }
                }

                Section("Core") {
                    HStack { Text("ABI version"); Spacer(); Text("\(core.abiVersion)").foregroundColor(.secondary) }
                    HStack { Text("Architecture"); Spacer(); Text(core.platform).foregroundColor(.secondary) }
                    HStack { Text("Direct Game"); Spacer(); Text(availability(CoreCapability.directGame)).foregroundColor(.secondary) }
                    HStack { Text("System Software"); Spacer(); Text(availability(CoreCapability.systemSoftware)).foregroundColor(.secondary) }
                    HStack { Text("Game inventory"); Spacer(); Text(availability(CoreCapability.gameInventory)).foregroundColor(.secondary) }
                    HStack { Text("Direct Game preflight"); Spacer(); Text(availability(CoreCapability.directGamePreflight)).foregroundColor(.secondary) }
                    HStack { Text("Native display surface"); Spacer(); Text(availability(CoreCapability.displaySurface)).foregroundColor(.secondary) }
                }

                Section("Interface") {
                    HStack { Text("Design"); Spacer(); Text("Apple Native").foregroundColor(.secondary) }
                    HStack { Text("Accent"); Spacer(); Text("PlayStation-inspired").foregroundColor(.secondary) }
                    HStack { Text("Touch controls"); Spacer(); Text(availability(CoreCapability.inputState)).foregroundColor(.secondary) }
                    HStack { Text("Performance HUD"); Spacer(); Text("Top-left · Compact").foregroundColor(.secondary) }
                }

                Section {
                    Text("Detailed global, boot-mode, and per-title controls will appear only when their core capabilities become functional.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Settings")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }

    private func availability(_ bit: UInt64) -> String {
        core.has(bit) ? "Available" : "Not implemented"
    }
}
