#include <sys/sysctl.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <signal.h>
#include <dlfcn.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS && !TARGET_OS_SIMULATOR
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" int csops(pid_t, unsigned int, void*, size_t);

// This breakpoint hook runs before any generated instructions are written.
// The debugger helper writes every RX page and acknowledges preparation with
// this marker. A plain debugger attach is insufficient on current iOS.
extern "C" __attribute__((noinline, used, visibility("default")))
void v3kios_jit_prepare_rx(void* address, std::size_t size) {
    asm volatile("" : : "r"(address), "r"(size) : "memory");
}

extern "C" bool v3kios_jit_allocate(std::size_t size, std::uint32_t** writable,
                                     std::uint32_t** executable) {
    *writable = nullptr;
    *executable = nullptr;
    constexpr std::uint32_t CodeSigningDebugged = 0x10000000U;
    std::uint32_t flags = 0;
    if (csops(getpid(), 0, &flags, sizeof(flags)) == 0 && (flags & CodeSigningDebugged) == 0) {
        using ptrace_fn = int (*)(int, pid_t, char*, int);
        if (auto* fn = reinterpret_cast<ptrace_fn>(dlsym(RTLD_DEFAULT, "ptrace"))) {
            pid_t parent = getpid();
            pid_t pid = fork();
            if (pid == 0) {
                if (fn(10, parent, nullptr, 0) == 0) {
                    int status = 0;
                    waitpid(parent, &status, 0);
                    fn(11, parent, nullptr, SIGCONT);
                }
                _exit(0);
            } else if (pid > 0) {
                int status = 0;
                waitpid(pid, &status, 0);
            }
        }
    }
    if (size < 8) return false;

  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  struct kinfo_proc info {};
  size_t info_size = sizeof(info);
  const bool traced = (sysctl(mib, 4, &info, &info_size, nullptr, 0) == 0) &&
                      ((info.kp_proc.p_flag & P_TRACED) != 0);
  if (!traced) {
    return false;
  }
  void* rwx = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_ANON | MAP_PRIVATE, -1, 0);
  if (rwx != MAP_FAILED && rwx != nullptr) {
    *writable = static_cast<std::uint32_t*>(rwx);
    *executable = static_cast<std::uint32_t*>(rwx);
    return true;
  }

    // The original mapping must start RX. Upgrading an RW-backed alias to RX
    // can report success but still fault on the first instruction fetch.
    void* rx = mmap(nullptr, size, PROT_READ | PROT_EXEC,
                    MAP_ANON | MAP_PRIVATE, -1, 0);
    if (rx == MAP_FAILED) return false;
    vm_address_t rw = 0;
    vm_prot_t current = VM_PROT_NONE, maximum = VM_PROT_NONE;
    const auto result = vm_remap(mach_task_self(), &rw, size, 0, VM_FLAGS_ANYWHERE,
        mach_task_self(), reinterpret_cast<vm_address_t>(rx), false,
        &current, &maximum, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS) {
        munmap(rx, size);
        return false;
    }

    v3kios_jit_prepare_rx(rx, size);
    constexpr char Marker[] = "V3KJITOK";
    const bool prepared = std::memcmp(rx, Marker, 8) == 0;
    if (!prepared || mprotect(reinterpret_cast<void*>(rw), size,
                              PROT_READ | PROT_WRITE) != 0) {
        munmap(reinterpret_cast<void*>(rw), size);
        munmap(rx, size);
        return false;
    }
    *writable = reinterpret_cast<std::uint32_t*>(rw);
    *executable = static_cast<std::uint32_t*>(rx);
    return true;
}


static void try_dopamine_enable_jit() {
    static bool attempted = false;
    if (attempted) return;
    attempted = true;
    const char* paths[] = {
        "/var/jb/basebin/libjailbreak.dylib",
        "/usr/lib/libjailbreak.dylib"
    };
    for (const char* path : paths) {
        void* handle = dlopen(path, RTLD_NOW);
        if (!handle) continue;
        using CheckinFn = int (*)(const char**, const char**, const char**, void**);
        if (auto checkin = reinterpret_cast<CheckinFn>(dlsym(handle, "jbclient_process_checkin"))) {
            checkin(nullptr, nullptr, nullptr, nullptr);
        }
        using CsAllowFn = int (*)(pid_t);
        if (auto csAllow = reinterpret_cast<CsAllowFn>(dlsym(handle, "jbclient_cs_allow_invalid"))) {
            csAllow(getpid());
        }
        break;
    }
}

extern "C" bool v3kios_jit_memory_available() {
    try_dopamine_enable_jit();
    void* mapJit = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
    if (mapJit != MAP_FAILED && mapJit != nullptr) {
        munmap(mapJit, 4096);
        return true;
    }
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return false;
    std::uint32_t* writable = nullptr;
    std::uint32_t* executable = nullptr;
    if (!v3kios_jit_allocate(static_cast<std::size_t>(pageSize),
                              &writable, &executable)) return false;
    // Verify both initial execution and rewriting through the permanent alias.
    constexpr std::uint32_t first[] = {0x52800540U, 0xD65F03C0U}; // return 42
    std::memcpy(writable, first, sizeof(first));
    sys_icache_invalidate(executable, sizeof(first));
    using Probe = std::uint32_t (*)();
    const auto run = reinterpret_cast<Probe>(executable);
    const bool initial = std::memcmp(executable, first, sizeof(first)) == 0 && run() == 42;
    writable[0] = 0x52800560U; // return 43
    sys_icache_invalidate(executable, sizeof(first));
    const bool rewritten = executable[0] == writable[0] && run() == 43;
    munmap(writable, static_cast<std::size_t>(pageSize));
    munmap(executable, static_cast<std::size_t>(pageSize));
    return initial && rewritten;
}
#endif
#endif
