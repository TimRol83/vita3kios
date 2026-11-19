import SwiftUI

struct CoreStatusCard: View {
    @ObservedObject var core: CoreStatusModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Label("Core Diagnostics", systemImage: "waveform.path.ecg")
                    .font(.headline)
                Spacer()
                Button(action: { core.refresh() }) { Label("Refresh", systemImage: "arrow.clockwise") }
                .labelStyle(.iconOnly)
            }

            if let error = core.error {
                Label(error, systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.red)
            } else {
                HStack { Text("Core ABI"); Spacer(); Text("v\(core.abiVersion).foregroundColor(.secondary) }")
                HStack { Text("Vita3K"); Spacer(); Text(core.upstreamVersion).foregroundColor(.secondary) }
                HStack { Text("Platform"); Spacer(); Text(core.platform).foregroundColor(.secondary) }
                HStack { Text("Upstream commit"); Spacer(); Text(short(core.upstreamCommit)).foregroundColor(.secondary) }
                HStack { Text("Allocator self-test"); Spacer(); Text(core.allocatorTestPassed ? "Passed" : "Failed").foregroundColor(.secondary) }
                    .foregroundColor(core.allocatorTestPassed ? Color.primary : Color.red)
            }
        }
        .padding(16)
        .background(.background, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
    }

    private func short(_ value: String) -> String {
        String(value.prefix(12))
    }
}
