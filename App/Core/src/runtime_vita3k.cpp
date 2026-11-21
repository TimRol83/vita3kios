#include <exception>
#include <execinfo.h>
#include <cstdlib>
#include <mach-o/dyld.h>
#include <cstdio>
#include <cstdlib>
#include "runtime.h"

#include <app/functions.h>
#include <app/session_controller.h>
#include <config/functions.h>
#include <config/state.h>
#include <ctrl/functions.h>
#include <ctrl/state.h>
#include <emuenv/state.h>
#include <modules/module_parent.h>
#include <packages/functions.h>
#include <renderer/functions.h>
#include <renderer/frame_host.h>
#include <renderer/state.h>
#include <util/log.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace v3kios::runtime {
namespace {

namespace filesystem = std::filesystem;

class IOSFrameHost final : public renderer::FrameHost {
public:
    explicit IOSFrameHost(const v3kios_display_surface_v1& surface) : surface_(surface) {}

    renderer::DisplayHandle handle() const override {
        return renderer::MacOSDisplayHandle{surface_.metal_layer};
    }

    int drawable_width() const override {
        return static_cast<int>(surface_.drawable_width);
    }

    int drawable_height() const override {
        return static_cast<int>(surface_.drawable_height);
    }

    std::vector<std::string> font_dirs() const override {
        return {};
    }

private:
    v3kios_display_surface_v1 surface_{};
};

struct RuntimeState {
    std::mutex mutex;
    v3kios_display_surface_v1 surface{};
    std::unique_ptr<EmuEnvState> emuenv;
    std::unique_ptr<app::AppSessionController> session;
    std::unique_ptr<IOSFrameHost> frameHost;
    app::LaunchRuntimeMetrics metrics;
    filesystem::path mountedGame;
};

RuntimeState state;

std::optional<filesystem::path> FindSupportRoot(const filesystem::path& gameRoot,
                                                const filesystem::path& dataRoot) {
    for (auto cursor = gameRoot; !cursor.empty() && cursor != cursor.root_path();
         cursor = cursor.parent_path()) {
        if (cursor.filename() == "Games") return cursor.parent_path();
    }
    return dataRoot.empty() ? std::nullopt : std::optional{dataRoot};
}

bool PrepareRuntimePaths(const filesystem::path& supportRoot,
                         const filesystem::path& gameRoot,
                         const filesystem::path& firmwareRoot,
                         const std::string& titleId,
                         Root& roots) {
    std::error_code error;
    const auto runtimeRoot = supportRoot / "Runtime";
    const auto vitaRoot = runtimeRoot / "vita";
    const auto appRoot = vitaRoot / "ux0" / "app";
    const auto mount = appRoot / titleId;

    filesystem::create_directories(appRoot, error);
    if (error) return false;

    for (const char* partition : {"os0", "pd0", "sa0", "vs0"}) {
        const auto source = firmwareRoot / partition;
        const auto destination = vitaRoot / partition;
        const bool hasFirmwarePartition = !firmwareRoot.empty() &&
            filesystem::is_directory(source, error) && !error;
        error.clear();
        if (filesystem::exists(destination, error) || filesystem::is_symlink(destination, error)) {
            error.clear();
            filesystem::remove_all(destination, error);
            if (error) return false;
        }
        if (hasFirmwarePartition)
            filesystem::create_directory_symlink(source, destination, error);
        else
            filesystem::create_directories(destination, error);
        if (error) return false;
    }
    filesystem::create_directories(runtimeRoot / "cache", error);
    if (error) return false;
    filesystem::create_directories(runtimeRoot / "patch", error);
    if (error) return false;
    filesystem::create_directories(runtimeRoot / "shared" / "textures", error);
    if (error) return false;

    if (filesystem::exists(mount, error) || filesystem::is_symlink(mount, error)) {
        error.clear();
        filesystem::remove_all(mount, error);
        if (error) return false;
    }
    filesystem::create_directory_symlink(gameRoot, mount, error);
    if (error) return false;

    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr || basePath[0] == '\0') return false;
    const auto staticAssetsRoot = filesystem::path{basePath};
    if (!filesystem::exists(staticAssetsRoot / "shaders-builtin")) return false;

    roots.set_static_assets_path(fs::path{staticAssetsRoot.string()});
    roots.set_vita_fs_path(fs::path{vitaRoot.string()});
    roots.set_log_path(fs::path{runtimeRoot.string()});
    roots.set_config_path(fs::path{runtimeRoot.string()});
    roots.set_shared_path(fs::path{(runtimeRoot / "shared").string()});
    roots.set_cache_path(fs::path{(runtimeRoot / "cache").string()});
    roots.set_patch_path(fs::path{(runtimeRoot / "patch").string()});
    state.mountedGame = mount;
    return true;
}

static std::string g_trace_path;
static void v3k_trace(const char* msg) {
    if (g_trace_path.empty()) return;
    if (FILE* f = std::fopen(g_trace_path.c_str(), "a")) {
        std::fprintf(f, "%s\n", msg);
        std::fflush(f);
        std::fclose(f);
    }
}

bool InitializeCore(const filesystem::path& supportRoot,
                    const filesystem::path& gameRoot,
                    const filesystem::path& firmwareRoot,
                    const std::string& titleId) {
    Root roots;
    if (!PrepareRuntimePaths(supportRoot, gameRoot, firmwareRoot, titleId, roots)) return false;

    if (logging::init(roots, true) != Success) return false;
    {
        std::string vlog = (roots.get_log_path() / "vita3k.log").string();
        std::freopen(vlog.c_str(), "a", stderr);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        std::fprintf(stderr, "[v3kios] unbuffered stderr attached to vita3k.log\n");
    }

    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC | SDL_INIT_SENSOR)) {
        LOG_ERROR("SDL initialization failed on iOS: {}", SDL_GetError());
        return false;
    }

    auto emuenv = std::make_unique<EmuEnvState>();
    Config config{};
    char executable[] = "vita3kios";
    char* arguments[] = {executable, nullptr};
    if (config::init_config(config, 1, arguments, roots, false) != Success) return false;

    config.set_vita_fs_path(roots.get_vita_fs_path());
    g_trace_path = (roots.get_vita_fs_path() / "boot_trace.txt").string();
    v3k_trace("1: before config");
    config.backend_renderer = "Vulkan";
    config.validation_layer = false;
    config.audio_backend = "SDL";
    v3k_trace("2: before app::init");
    if (!app::init(*emuenv, config, roots)) return false;
    v3k_trace("3: after app::init");

    emuenv->cfg.current_config.backend_renderer = "Vulkan";
    emuenv->cfg.current_config.validation_layer = false;
    emuenv->cfg.current_config.audio_backend = "SDL";
    v3k_trace("3a: before enumerate_vulkan_devices");
    emuenv->vulkan_device_info = std::make_unique<renderer::VulkanDeviceInfo>();
    v3k_trace("3b: before init_libraries");
    app::reset_controller_binding(*emuenv);
    init_libraries(*emuenv);
    v3k_trace("3c: before init_apps_list");
    if (!app::init_apps_list(*emuenv)) return false;
    v3k_trace("3d: before load_users");
    app::load_users(*emuenv);
    if (!app::ensure_current_user(*emuenv)) return false;
    v3k_trace("3e: before refresh_controllers");
    refresh_controllers(emuenv->ctrl, *emuenv);
    v3k_trace("3f: before AppSessionController");
    state.session = std::make_unique<app::AppSessionController>(*emuenv);
    state.emuenv = std::move(emuenv);
    return true;
}

void StopSessionLocked() {
    if (state.session) state.session->stop(app::AppSessionStopReason::UserRequest);
    state.frameHost.reset();
    state.session.reset();
    state.emuenv.reset();
    state.metrics = {};
    SDL_Quit();
}

}  // namespace

bool IsFullCoreLinked() {
    return true;
}

bool InstallFirmwarePup(const filesystem::path& pupPath,
                        const filesystem::path& vitaFsRoot) {
    std::error_code error;
    filesystem::create_directories(vitaFsRoot, error);
    if (error) return false;
    return !install_pup(fs::path{vitaFsRoot.string()}, fs::path{pupPath.string()}).empty();
}

SystemBootResult BootSystemSoftware(const filesystem::path&,
                                    const filesystem::path&) {
    return {
        .result = V3KIOS_RESULT_UNSUPPORTED,
        .checkpoint = V3KIOS_BOOT_CHECKPOINT_SHELL_CONTAINER_VERIFIED,
        .blocker = V3KIOS_BOOT_BLOCKER_CORE_INITIALIZATION_FAILED,
        .detail = "System Software is separate from the linked Direct Game runtime.",
    };
}

DirectBootResult BootDirectGame(const filesystem::path& gameRoot,
                                const filesystem::path&,
                                const std::string& titleId,
                                const filesystem::path& dataRoot,
                                const filesystem::path& firmwareRoot) {
    const std::lock_guard lock{state.mutex};
    StopSessionLocked();

    if (state.surface.metal_layer == nullptr) {
        return {
            .result = V3KIOS_RESULT_GAME_NOT_READY,
            .checkpoint = V3KIOS_DIRECT_BOOT_CHECKPOINT_EBOOT_CONTAINER_VERIFIED,
            .blocker = V3KIOS_DIRECT_BOOT_BLOCKER_DISPLAY_SURFACE_MISSING,
            .detail = "The iOS CAMetalLayer was detached before runtime initialization.",
        };
    }

    const auto supportRoot = FindSupportRoot(gameRoot, dataRoot);
    if (!supportRoot || !InitializeCore(*supportRoot, gameRoot, firmwareRoot, titleId)) {
        StopSessionLocked();
        return {
            .result = V3KIOS_RESULT_INTERNAL_ERROR,
            .checkpoint = V3KIOS_DIRECT_BOOT_CHECKPOINT_EBOOT_CONTAINER_VERIFIED,
            .blocker = V3KIOS_DIRECT_BOOT_BLOCKER_CORE_INITIALIZATION_FAILED,
            .detail = "The linked Vita3K runtime could not initialize its iOS VitaFS session.",
        };
    }

    AppLaunchRequest request{.app_path = titleId};
    v3k_trace("4: before begin_launch");
    if (!state.session->begin_launch(request)) {
        StopSessionLocked();
        return {
            .result = V3KIOS_RESULT_GAME_NOT_READY,
            .checkpoint = V3KIOS_DIRECT_BOOT_CHECKPOINT_CORE_INITIALIZED,
            .blocker = V3KIOS_DIRECT_BOOT_BLOCKER_MODULE_LOAD_FAILED,
            .detail = "Vita3K initialized, but the mounted title was not accepted by the app loader.",
        };
    }

    {
        char exe_path[2048] = {0};
        uint32_t exe_size = sizeof(exe_path);
        if (_NSGetExecutablePath(exe_path, &exe_size) == 0) {
            const char *tmp_dir = std::getenv("TMPDIR");
            if (!tmp_dir || *tmp_dir == 0) tmp_dir = "/tmp/";
            std::string icd_file = std::string(tmp_dir) + "/MoltenVK_icd.json";
            if (FILE *fp = std::fopen(icd_file.c_str(), "w")) {
                std::fprintf(fp, "{\n  \"file_format_version\": \"1.0.0\",\n  \"ICD\": {\n    \"library_path\": \"%s\",\n    \"api_version\": \"1.3.0\"\n  }\n}\n", exe_path);
                std::fclose(fp);
                setenv("VK_DRIVER_FILES", icd_file.c_str(), 1);
                setenv("VK_ICD_FILENAMES", icd_file.c_str(), 1);
            }
        }
    }
    state.frameHost = std::make_unique<IOSFrameHost>(state.surface);
    static bool atexit_installed = false;
    if (!atexit_installed) {
        atexit_installed = true;
        std::atexit([]() {
            v3k_trace("ATEXIT: process exiting early, backtrace:");
            void *frames[32];
            int count = backtrace(frames, 32);
            char **syms = backtrace_symbols(frames, count);
            if (syms) {
                for (int i = 0; i < count; ++i) {
                    v3k_trace(syms[i]);
                }
                std::free(syms);
            }
        });
    }
    {
        std::string surf_buf(256, '\0');
        std::snprintf(surf_buf.data(), surf_buf.size(), "5: before initialize_renderer layer=%p w=%u h=%u",
                      state.surface.metal_layer, state.surface.drawable_width, state.surface.drawable_height);
        v3k_trace(surf_buf.c_str());
    }
    bool renderer_ok = false;
    std::string renderer_err = "Vita3K initialized, but MoltenVK could not create the iOS render surface.";
    try {
        renderer_ok = state.session->[&]() -> bool {
            try {
                return initialize_renderer(*state.frameHost);
            } catch (const std::exception &e) {
                std::string msg = std::string("6: initialize_renderer EXCEPTION: ") + e.what();
                v3k_trace(msg.c_str());
                return false;
            } catch (...) {
                v3k_trace("6: initialize_renderer UNKNOWN EXCEPTION");
                return false;
            }
        }();
    } catch (const std::exception &e) {
        renderer_err = std::string("initialize_renderer exception: ") + e.what();
        v3k_trace(renderer_err.c_str());
    } catch (...) {
        renderer_err = "initialize_renderer unknown exception";
        v3k_trace(renderer_err.c_str());
    }
    if (!renderer_ok) {
        v3k_trace("5e: initialize_renderer failed");
        StopSessionLocked();
        return {
            .result = V3KIOS_RESULT_INTERNAL_ERROR,
            .checkpoint = V3KIOS_DIRECT_BOOT_CHECKPOINT_CORE_INITIALIZED,
            .blocker = V3KIOS_DIRECT_BOOT_BLOCKER_RENDERER_FAILED,
            .detail = renderer_err,
        };
    }
    v3k_trace("6: after initialize_renderer");

    if (!state.session->initialize_runtime()) {
        StopSessionLocked();
        return {
            .result = V3KIOS_RESULT_INTERNAL_ERROR,
            .checkpoint = V3KIOS_DIRECT_BOOT_CHECKPOINT_CORE_INITIALIZED,
            .blocker = V3KIOS_DIRECT_BOOT_BLOCKER_CORE_INITIALIZATION_FAILED,
            .detail = "The Vita3K CPU, memory, audio, or HLE runtime failed late initialization.",
        };
    }

    if (!state.session->load_and_run()) {
        StopSessionLocked();
        return {
            .result = V3KIOS_RESULT_INTERNAL_ERROR,
            .checkpoint = V3KIOS_DIRECT_BOOT_CHECKPOINT_CORE_INITIALIZED,
            .blocker = V3KIOS_DIRECT_BOOT_BLOCKER_MODULE_LOAD_FAILED,
            .detail = "The Vita3K loader could not load and start the selected game module.",
        };
    }

    return {
        .result = V3KIOS_RESULT_OK,
        .checkpoint = V3KIOS_DIRECT_BOOT_CHECKPOINT_MAIN_THREAD_STARTED,
        .blocker = V3KIOS_DIRECT_BOOT_BLOCKER_NONE,
        .detail = "The Vita3K guest main thread and iOS render thread are running.",
    };
}

void SetInputState(const v3kios_input_state_v1& input) {
    const std::lock_guard lock{state.mutex};
    if (!state.emuenv) return;
    const std::lock_guard inputLock{state.emuenv->ctrl.mutex};
    auto& keyboard = state.emuenv->ctrl.keyboard_state;
    keyboard.buttons = input.buttons;
    constexpr std::uint32_t StandardLeftShoulder = 0x00000100U;
    constexpr std::uint32_t StandardRightShoulder = 0x00000200U;
    constexpr std::uint32_t ExtendedLeftShoulder = 0x00000400U;
    constexpr std::uint32_t ExtendedRightShoulder = 0x00000800U;
    keyboard.buttons_ext = input.buttons & ~(StandardLeftShoulder | StandardRightShoulder);
    if ((input.buttons & StandardLeftShoulder) != 0)
        keyboard.buttons_ext |= ExtendedLeftShoulder;
    if ((input.buttons & StandardRightShoulder) != 0)
        keyboard.buttons_ext |= ExtendedRightShoulder;
    keyboard.axes[0] = input.left_x;
    keyboard.axes[1] = input.left_y;
    keyboard.axes[2] = input.right_x;
    keyboard.axes[3] = input.right_y;
}

void GetMetrics(v3kios_metrics_v1& metrics) {
    const std::lock_guard lock{state.mutex};
    metrics.validity_mask = 0;
    metrics.guest_fps = 0.0F;
    metrics.frame_time_ms = 0.0F;
    metrics.host_cpu_percent = 0.0F;
    metrics.host_memory_bytes = 0;
    if (!state.emuenv || !state.session || !state.session->is_running()) return;
    if (!app::update_runtime_metrics(*state.emuenv, state.metrics)) return;
    metrics.validity_mask = V3KIOS_METRIC_GUEST_FPS | V3KIOS_METRIC_FRAME_TIME;
    metrics.guest_fps = static_cast<float>(state.emuenv->fps);
    metrics.frame_time_ms = static_cast<float>(state.emuenv->ms_per_frame);
}

void AttachDisplaySurface(const v3kios_display_surface_v1& surface) {
    const std::lock_guard lock{state.mutex};
    state.surface = surface;
}

void DetachDisplaySurface() {
    const std::lock_guard lock{state.mutex};
    state.surface = {};
}

void StopSession() {
    const std::lock_guard lock{state.mutex};
    StopSessionLocked();
}

}  // namespace v3kios::runtime
