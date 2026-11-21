#!/bin/sh

set -eu

VITA3KIOS_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
VITA3KIOS_ROOT=$(CDPATH= cd -- "$VITA3KIOS_SCRIPT_DIR/.." && pwd)
VITA3KIOS_UPSTREAM="$VITA3KIOS_ROOT/External/Vita3K"
VITA3KIOS_PATCH="$VITA3KIOS_ROOT/Patches/Upstream/0003-add-ios-static-runtime-target.patch"
VITA3KIOS_UPSTREAM_REVISION=$(git -C "$VITA3KIOS_UPSTREAM" rev-parse HEAD)
VITA3KIOS_SOURCE_DIR="/tmp/vita3kios-vita3k-source-$VITA3KIOS_UPSTREAM_REVISION"
VITA3KIOS_DEPENDENCY_PREFIX="$VITA3KIOS_ROOT/Build/Dependencies/vcpkg/installed/arm64-ios-vita3k"
VITA3KIOS_MOLTENVK_ROOT="$VITA3KIOS_ROOT/Build/Dependencies/MoltenVK/v1.4.1-ios/MoltenVK/MoltenVK"
VITA3KIOS_BUILD_DIR="$VITA3KIOS_ROOT/Build/Vita3K-iOS"
VITA3KIOS_PRODUCT_DIR="$VITA3KIOS_ROOT/Build/Core"
VITA3KIOS_PRODUCT="$VITA3KIOS_PRODUCT_DIR/libVita3KFullCore.a"
VITA3KIOS_BUILD_JOBS=${VITA3KIOS_BUILD_JOBS:-4}
. "$VITA3KIOS_SCRIPT_DIR/toolchain-env.sh"

if [ ! -f "$VITA3KIOS_PATCH" ]; then
    echo "error: iOS runtime patch is missing" >&2
    exit 1
fi

if [ ! -f "$VITA3KIOS_DEPENDENCY_PREFIX/lib/libavcodec.a" ] || \
   [ ! -f "$VITA3KIOS_DEPENDENCY_PREFIX/lib/libcrypto.a" ]; then
    "$VITA3KIOS_SCRIPT_DIR/build-ios-dependencies.sh"
fi

if [ ! -f "$VITA3KIOS_MOLTENVK_ROOT/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a" ]; then
    echo "error: iOS MoltenVK archive is missing" >&2
    exit 1
fi

VITA3KIOS_PATCH_SHA=$(shasum -a 256 "$VITA3KIOS_PATCH" | awk '{print $1}')
VITA3KIOS_SOURCE_READY=0
if [ -f "$VITA3KIOS_SOURCE_DIR/.vita3kios-patch-sha" ]; then
    if [ "$(sed -n '1p' "$VITA3KIOS_SOURCE_DIR/.vita3kios-patch-sha")" = "$VITA3KIOS_PATCH_SHA" ]; then
        VITA3KIOS_SOURCE_READY=1
    fi
fi

if [ "$VITA3KIOS_SOURCE_READY" -eq 0 ]; then
    VITA3KIOS_SOURCE_STAGE=$(mktemp -d /tmp/vita3kios-vita3k-source.XXXXXX)
    git -C "$VITA3KIOS_UPSTREAM" archive HEAD | tar -x -C "$VITA3KIOS_SOURCE_STAGE"
    git -C "$VITA3KIOS_UPSTREAM" submodule status --recursive |
    while read -r VITA3KIOS_SUBMODULE_REVISION VITA3KIOS_SUBMODULE_PATH VITA3KIOS_SUBMODULE_DESCRIPTION; do
        mkdir -p "$VITA3KIOS_SOURCE_STAGE/$VITA3KIOS_SUBMODULE_PATH"
        git -C "$VITA3KIOS_UPSTREAM/$VITA3KIOS_SUBMODULE_PATH" archive HEAD |
            tar -x -C "$VITA3KIOS_SOURCE_STAGE/$VITA3KIOS_SUBMODULE_PATH"
    done
    patch -s -d "$VITA3KIOS_SOURCE_STAGE" -p1 < "$VITA3KIOS_PATCH"
python3 - "$VITA3KIOS_SOURCE_STAGE/vita3k/renderer/src/vulkan/renderer.cpp" << 'PY'
import sys
fn = sys.argv.pop()
txt = open(fn).read()
target = "vk::createInstance("
assert target in txt, "vk::createInstance( not found in renderer.cpp"
helper = """static vk::Instance v3kios_create_vulkan_instance(vk::InstanceCreateInfo info) {
    auto supported_props = vk::enumerateInstanceExtensionProperties();
    auto is_supported = [&](const char *name) {
        if (!name) return false;
        for (const auto &prop : supported_props) {
            if (std::strcmp(prop.extensionName, name) == 0) return true;
        }
        return false;
    };
    std::vector<const char *> exts;
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i) {
        const char *ext = info.ppEnabledExtensionNames[i];
        if (is_supported(ext)) {
            exts.push_back(ext);
        }
    }
    if (is_supported(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        bool has_port = false;
        for (const char *ext : exts) {
            if (std::strcmp(ext, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
                has_port = true;
                break;
            }
        }
        if (!has_port) exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        info.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
    }
    info.pNext = nullptr;
    info.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    info.ppEnabledExtensionNames = exts.data();
    vk::ApplicationInfo app_copy;
    if (info.pApplicationInfo) {
        app_copy = *info.pApplicationInfo;
        app_copy.pNext = nullptr;
        app_copy.apiVersion = VK_API_VERSION_1_1;
        info.pApplicationInfo = &app_copy;
    }
    return vk::createInstance(info);
}

bool VKState::create("""
assert "bool VKState::create(" in txt, "VKState::create not found"
txt = txt.replace("bool VKState::create(", helper, 1)
txt = txt.replace(target, "v3kios_create_vulkan_instance(")
open(fn, "w").write(txt)
print("Injected MoltenVK portability enumeration wrapper into", fn)
PY
    printf '%s\n' "$VITA3KIOS_PATCH_SHA" > "$VITA3KIOS_SOURCE_STAGE/.vita3kios-patch-sha"
    rm -rf -- "$VITA3KIOS_SOURCE_DIR"
    mv -- "$VITA3KIOS_SOURCE_STAGE" "$VITA3KIOS_SOURCE_DIR"
fi

# Check the build tree independently of the shared source tree: another checkout
# may already have refreshed the sources while this checkout still has old objects.
# Archive timestamps are not a reliable indicator of a patch-only source change.
VITA3KIOS_BUILD_ID="$VITA3KIOS_UPSTREAM_REVISION:$VITA3KIOS_PATCH_SHA"
if [ ! -f "$VITA3KIOS_BUILD_DIR/.vita3kios-source-id" ] || \
   [ "$(sed -n '1p' "$VITA3KIOS_BUILD_DIR/.vita3kios-source-id")" != "$VITA3KIOS_BUILD_ID" ]; then
    rm -rf -- "$VITA3KIOS_BUILD_DIR"
fi

VITA3KIOS_SYSROOT=$(xcrun --sdk iphoneos --show-sdk-path)

python3 - "$VITA3KIOS_SOURCE_DIR" << 'PY'
import sys, os, re
rcpp = os.path.join(sys.argv.pop(), "vita3k/renderer/src/vulkan/renderer.cpp")
txt = open(rcpp).read()
if "VK_KHR_portability_enumeration" not in txt:
    pat = r"([ \t]*)([^" + chr(10) + r";]*vk::createInstance(?:Unique)?\(\s*([A-Za-z0-9_]+))"
    def repl(m):
        ind, call, var = m.group(1), m.group(2), m.group(3)
        q = chr(34)
        nl = chr(10)
        lines = [
            f"{ind}std::vector<const char*> v3k_port_exts({var}.ppEnabledExtensionNames, {var}.ppEnabledExtensionNames + {var}.enabledExtensionCount);",
            f"{ind}v3k_port_exts.push_back({q}VK_KHR_portability_enumeration{q});",
            f"{ind}v3k_port_exts.push_back({q}VK_KHR_get_physical_device_properties2{q});",
            f"{ind}uint32_t v3k_avail_cnt = 0;",
            f"{ind}vk::enumerateInstanceExtensionProperties(nullptr, &v3k_avail_cnt, nullptr);",
            f"{ind}std::vector<vk::ExtensionProperties> v3k_avail(v3k_avail_cnt);",
            f"{ind}if (v3k_avail_cnt > 0) vk::enumerateInstanceExtensionProperties(nullptr, &v3k_avail_cnt, v3k_avail.data());",
            f"{ind}std::vector<const char*> v3k_filt_exts;",
            f"{ind}for (const char* req : v3k_port_exts) {{",
            f"{ind}    for (const auto& prop : v3k_avail) {{",
            f"{ind}        if (std::strcmp(req, prop.extensionName) == 0) {{ v3k_filt_exts.push_back(req); break; }}",
            f"{ind}    }}",
            f"{ind}}}",
            f"{ind}{var}.enabledExtensionCount = static_cast<uint32_t>(v3k_filt_exts.size());",
            f"{ind}{var}.ppEnabledExtensionNames = v3k_filt_exts.data();",
            f"{ind}{var}.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;",
            f"{ind}{call}"
        ]
        return nl.join(lines)
    txt, n = re.subn(pat, repl, txt, count=0)
    assert n >= 2, "createInstance not matched"
    open(rcpp, "w").write(txt)
    idx = txt.find("bool VKState::create(")
    assert idx != -1, "VKState::create not found for trace"
    brace = txt.find("{", idx)
    next_fn = txt.find("\nbool VKState::", brace)
    if next_fn == -1:
        next_fn = txt.find("\nvoid VKState::", brace)
    body = txt[brace+1:next_fn]
    body = '\n    std::fprintf(stderr, "[v3kios] VKState::create ENTER\\n");' + body.replace(
        "return false;",
        '{ std::fprintf(stderr, "[v3kios] VKState::create false at line %d\\n", __LINE__); return false; }'
    ).replace(
        "return true;",
        '{ std::fprintf(stderr, "[v3kios] VKState::create TRUE at line %d\\n", __LINE__); return true; }'
    )
    txt = "#include <cstdio>\n" + txt[:brace+1] + body + txt[next_fn:]
    open(rcpp, "w").write(txt)
    rinit = os.path.join(os.path.dirname(rcpp), "..", "renderer.cpp")
    if os.path.exists(rinit):
        rt = "#include <cstdio>\n" + open(rinit).read().replace(
            "return false;",
            '{ std::fprintf(stderr, "[v3kios] renderer.cpp false at line %d\\n", __LINE__); return false; }'
        )
        open(rinit, "w").write(rt)
    print("Patched createInstance portability + VKState line trace cleanly")
PY

cmake -S "$VITA3KIOS_SOURCE_DIR" -B "$VITA3KIOS_BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_SYSTEM_PROCESSOR=arm64 \
    -DCMAKE_OSX_SYSROOT="$VITA3KIOS_SYSROOT" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_PREFIX_PATH="$VITA3KIOS_DEPENDENCY_PREFIX" \
    -DOPENSSL_ROOT_DIR="$VITA3KIOS_DEPENDENCY_PREFIX" \
    -DOPENSSL_USE_STATIC_LIBS=ON \
    -DBOOST_ROOT="$VITA3KIOS_DEPENDENCY_PREFIX" \
    -DBoost_NO_SYSTEM_PATHS=ON \
    -DBoost_INCLUDE_DIR="$VITA3KIOS_DEPENDENCY_PREFIX/include" \
    -DBoost_LIBRARY_DIR_RELEASE="$VITA3KIOS_DEPENDENCY_PREFIX/lib" \
    -DBoost_FILESYSTEM_LIBRARY_RELEASE="$VITA3KIOS_DEPENDENCY_PREFIX/lib/libboost_filesystem.a" \
    -DVITA3K_FORCE_SYSTEM_BOOST=ON \
    -DVITA3K_IOS_DEPENDENCY_PREFIX="$VITA3KIOS_DEPENDENCY_PREFIX" \
    -DVITA3K_IOS_MOLTENVK_ROOT="$VITA3KIOS_MOLTENVK_ROOT" \
    -DVITA3K_IOS_METAL_LAYER_SOURCE="$VITA3KIOS_ROOT/App/Core/src/metal_layer_ios.mm" \
    -DUSE_DISCORD_RICH_PRESENCE=OFF \
    -DUSE_LTO=NEVER \
    -DBUILD_TESTING=OFF \
    -DCAPSTONE_BUILD_MACOS_THIN=ON

# Record the configured source identity before compilation so an interrupted
# build can resume its valid partial objects on the next invocation.
printf '%s\n' "$VITA3KIOS_BUILD_ID" > "$VITA3KIOS_BUILD_DIR/.vita3kios-source-id"

CCACHE_DISABLE=1 cmake --build "$VITA3KIOS_BUILD_DIR" --target vita3k \
    --parallel "$VITA3KIOS_BUILD_JOBS"

mkdir -p -- "$VITA3KIOS_PRODUCT_DIR"
set --
while IFS= read -r VITA3KIOS_ARCHIVE; do
    set -- "$@" "$VITA3KIOS_ARCHIVE"
done <<EOF
$(find "$VITA3KIOS_BUILD_DIR" -type f -name '*.a' -print | sort)
$(find "$VITA3KIOS_DEPENDENCY_PREFIX/lib" -maxdepth 1 -type f -name '*.a' -print | sort)
$VITA3KIOS_MOLTENVK_ROOT/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a
EOF

if [ "$#" -eq 0 ]; then
    echo "error: no iOS runtime archives were produced" >&2
    exit 1
fi

libtool -static -D -o "$VITA3KIOS_PRODUCT" "$@"

VITA3KIOS_ARCHS=$(lipo -archs "$VITA3KIOS_PRODUCT")
if [ "$VITA3KIOS_ARCHS" != "arm64" ]; then
    echo "error: full core archive has unexpected architectures: $VITA3KIOS_ARCHS" >&2
    exit 1
fi

echo "vita3k_ios_core=$VITA3KIOS_PRODUCT"
echo "vita3k_ios_core_architectures=$VITA3KIOS_ARCHS"
