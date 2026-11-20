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

extern "C" kern_return_t mach_memory_entry_ownership(
    mach_port_t mem_entry, mach_port_t owner, int ledger_tag, int ledger_flags);

extern "C" bool v3kios_jit_allocate(std::size_t size, std::uint32_t** writable,
                                    std::uint32_t** executable) {
  *writable = nullptr;
  *executable = nullptr;
  constexpr std::uint32_t CodeSigningDebugged = 0x10000000U;
  std::uint32_t flags = 0;
  if (csops(getpid(), 0, &flags, sizeof(flags)) != 0 || (flags & CodeSigningDebugged) == 0) {
    using ptrace_fn = int (*)(int, pid_t, char*, int);
    if (auto fn = reinterpret_cast<ptrace_fn>(dlsym(RTLD_DEFAULT, "ptrace"))) {
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
    flags = 0;
    if (csops(getpid(), 0, &flags, sizeof(flags)) != 0 || (flags & CodeSigningDebugged) == 0) {
      return false;
    }
  }
  if (size < 8) return false;

  const mach_port_t task = mach_task_self();
  memory_object_size_t entry_size = static_cast<memory_object_size_t>(size);
  mach_port_t mem_entry = MACH_PORT_NULL;
  kern_return_t kr = mach_make_memory_entry_64(
      task, &entry_size, 0,
      MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE,
      &mem_entry, MACH_PORT_NULL);
  if (kr != KERN_SUCCESS || mem_entry == MACH_PORT_NULL) {
    return false;
  }

  mach_memory_entry_ownership(mem_entry, TASK_NULL, 0, (1 << 8));

  vm_address_t rw_addr = 0;
  kr = vm_map(task, &rw_addr, size, 0, VM_FLAGS_ANYWHERE, mem_entry, 0, FALSE,
              VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
              VM_INHERIT_DEFAULT);
  if (kr != KERN_SUCCESS) {
    mach_port_deallocate(task, mem_entry);
    return false;
  }

  vm_address_t rx_addr = 0;
  kr = vm_map(task, &rx_addr, size, 0, VM_FLAGS_ANYWHERE, mem_entry, 0, FALSE,
              VM_PROT_READ | VM_PROT_EXECUTE,
              VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE,
              VM_INHERIT_DEFAULT);
  mach_port_deallocate(task, mem_entry);
  if (kr != KERN_SUCCESS) {
    vm_deallocate(task, rw_addr, size);
    return false;
  }

  *writable = reinterpret_cast<std::uint32_t*>(rw_addr);
  *executable = reinterpret_cast<std::uint32_t*>(rx_addr);
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

extern "C" int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);

extern "C" bool v3kios_jit_memory_available() {
    try_dopamine_enable_jit();
    uint32_t cs_flags = 0;
    if (csops(getpid(), 0, &cs_flags, sizeof(cs_flags)) == 0 && (cs_flags & 0x10000000u) != 0) {
        return true;
    }
    void* mapJit = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
    if (mapJit != MAP_FAILED && mapJit != nullptr) {
        munmap(mapJit, 4096);
        return true;
    }
    return false;
}
#endif
#endif
