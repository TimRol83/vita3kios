#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

static void v3k_crash_sig(int sig) {
    void **frames = (void **)malloc(sizeof(void *) * 32);
    int n = frames ? backtrace(frames, 32) : 0;
    const char *hdr = "\n[v3kios] CRASH SIGNAL\n";
    ssize_t wr = write(STDERR_FILENO, hdr, strlen(hdr));
    (void)wr;
    if (frames && n > 0) backtrace_symbols_fd(frames, n, STDERR_FILENO);
    _exit(128 + sig);
}

typedef id (*NewPSOFn)(id, SEL, MTLRenderPipelineDescriptor *, NSError * __autoreleasing *);
typedef id (*NewPSOOptsFn)(id, SEL, MTLRenderPipelineDescriptor *, MTLPipelineOption,
                           MTLRenderPipelineReflection * __autoreleasing *, NSError * __autoreleasing *);

static IMP orig_newPSO = NULL;
static IMP orig_newPSOOpts = NULL;

static void sanitize_pso_desc(MTLRenderPipelineDescriptor *desc) {
    NSUInteger i;
    if (!desc) return;
    if (desc.rasterSampleCount == 0) desc.rasterSampleCount = 1;
    if (desc.depthAttachmentPixelFormat == MTLPixelFormatDepth24Unorm_Stencil8) {
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    }
    if (desc.stencilAttachmentPixelFormat == MTLPixelFormatDepth24Unorm_Stencil8) {
        desc.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    }
    for (i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *att = [desc.colorAttachments objectAtIndexedSubscript:i];
        if (!att) continue;
        if (att.pixelFormat == MTLPixelFormatInvalid &&
            (att.blendingEnabled || att.writeMask != MTLColorWriteMaskAll)) {
            att.blendingEnabled = NO;
            att.writeMask = MTLColorWriteMaskNone;
        }
    }
}

static id safe_newPSO(id self, SEL _cmd, MTLRenderPipelineDescriptor *desc, NSError * __autoreleasing *error) {
    (void)_cmd;
    sanitize_pso_desc(desc);
    return ((NewPSOFn)orig_newPSO)(self, _cmd, desc, error);
}

static id safe_newPSOOpts(id self, SEL _cmd, MTLRenderPipelineDescriptor *desc, MTLPipelineOption opts,
                          MTLRenderPipelineReflection * __autoreleasing *refl, NSError * __autoreleasing *error) {
    (void)_cmd;
    sanitize_pso_desc(desc);
    return ((NewPSOOptsFn)orig_newPSOOpts)(self, _cmd, desc, opts, refl, error);
}

__attribute__((constructor))
static void install_v3k_a9_pso_guard(void) {
    id<MTLDevice> dev;
    Class cls;
    SEL s1, s2;
    Method m1, m2;

    signal(SIGSEGV, v3k_crash_sig);
    signal(SIGBUS, v3k_crash_sig);
    signal(SIGILL, v3k_crash_sig);
    signal(SIGABRT, v3k_crash_sig);

    dev = MTLCreateSystemDefaultDevice();
    if (!dev) return;
    cls = object_getClass(dev);
    s1 = @selector(newRenderPipelineStateWithDescriptor:error:);
    m1 = class_getInstanceMethod(cls, s1);
    if (m1) {
        orig_newPSO = method_getImplementation(m1);
        method_setImplementation(m1, (IMP)safe_newPSO);
    }
    s2 = @selector(newRenderPipelineStateWithDescriptor:options:reflection:error:);
    m2 = class_getInstanceMethod(cls, s2);
    if (m2) {
        orig_newPSOOpts = method_getImplementation(m2);
        method_setImplementation(m2, (IMP)safe_newPSOOpts);
    }
    fprintf(stderr, "[v3kios] A9 Metal PSO + signal guard installed on %s\n", [dev.name UTF8String]);
}
