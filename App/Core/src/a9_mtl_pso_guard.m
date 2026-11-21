#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

static void v3k_crash_sig(int sig) {
    void **frames = (void **)std::malloc(sizeof(void *) * 32);
    int n = frames ? backtrace(frames, 32) : 0;
    const char *hdr = "\n[v3kios] CRASH SIGNAL\n";
    write(STDERR_FILENO, hdr, std::strlen(hdr));
    if (frames && n > 0) backtrace_symbols_fd(frames, n, STDERR_FILENO);
    _exit(128 + sig);
}

static id (*orig_newPSO)(id, SEL, MTLRenderPipelineDescriptor *, NSError **);
static id (*orig_newPSOOpts)(id, SEL, MTLRenderPipelineDescriptor *, MTLPipelineOption, MTLAutoreleasedRenderPipelineReflection **, NSError **);

static void sanitize_pso_desc(MTLRenderPipelineDescriptor *desc) {
    if (!desc) return;
    if (desc.rasterSampleCount == 0) desc.rasterSampleCount = 1;
    if (desc.depthAttachmentPixelFormat == MTLPixelFormatDepth24Unorm_Stencil8) {
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    }
    if (desc.stencilAttachmentPixelFormat == MTLPixelFormatDepth24Unorm_Stencil8) {
        desc.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    }
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *att = [desc.colorAttachments objectAtIndexedSubscript:i];
        if (!att) continue;
        if (att.pixelFormat == MTLPixelFormatInvalid &&
            (att.blendingEnabled || att.writeMask != MTLColorWriteMaskAll)) {
            att.blendingEnabled = NO;
            att.writeMask = MTLColorWriteMaskNone;
        }
    }
}

static id safe_newPSO(id self, SEL _cmd, MTLRenderPipelineDescriptor *desc, NSError **error) {
    sanitize_pso_desc(desc);
    return orig_newPSO(self, _cmd, desc, error);
}

static id safe_newPSOOpts(id self, SEL _cmd, MTLRenderPipelineDescriptor *desc, MTLPipelineOption opts,
                          MTLAutoreleasedRenderPipelineReflection **refl, NSError **error) {
    sanitize_pso_desc(desc);
    return orig_newPSOOpts(self, _cmd, desc, opts, refl, error);
}

__attribute__((constructor))
static void install_v3k_a9_pso_guard(void) {
    signal(SIGSEGV, v3k_crash_sig);
    signal(SIGBUS, v3k_crash_sig);
    signal(SIGILL, v3k_crash_sig);
    signal(SIGABRT, v3k_crash_sig);

    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) return;
    Class cls = object_getClass(dev);
    SEL s1 = @selector(newRenderPipelineStateWithDescriptor:error:);
    Method m1 = class_getInstanceMethod(cls, s1);
    if (m1) {
        orig_newPSO = (void *)method_getImplementation(m1);
        method_setImplementation(m1, (IMP)safe_newPSO);
    }
    SEL s2 = @selector(newRenderPipelineStateWithDescriptor:options:reflection:error:);
    Method m2 = class_getInstanceMethod(cls, s2);
    if (m2) {
        orig_newPSOOpts = (void *)method_getImplementation(m2);
        method_setImplementation(m2, (IMP)safe_newPSOOpts);
    }
    std::fprintf(stderr, "[v3kios] A9 Metal PSO + signal guard installed on %s\n", [dev.name UTF8String]);
}
