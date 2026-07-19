// Metal MDI backend — Objective-C++ method-swizzling approach.
//
// Two swizzles, both on the concrete render-encoder class Unity uses:
//
//   1. setVertexBuffer:offset:atIndex:  (auto-detect slot)
//      Unity binds resources to numbered MTL buffer slots before each
//      draw. We watch for our pre-registered _drawIndexBuffer and remember
//      whichever slot Unity assigned to it. This eliminates the brittle
//      "guess the slot" problem of cbuffer-based approaches — slot
//      allocation depends on shader compilation order and is not stable
//      across user shaders.
//
//   2. drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:
//        indirectBuffer:indirectBufferOffset:                    (MDI)
//      The C# wrapper's "prime" DrawProceduralIndirect — issued just
//      before the plugin event — reaches Metal as exactly this selector.
//      Inside the prime, the encoder is alive, the user's PSO is bound,
//      and all shader resources are set. The hook recognises the prime
//      by the pointer of our pre-registered dummy args buffer; the slot
//      is encoded in indirectBufferOffset (slot * sizeof(MDIDrawIndexedArgs)).
//      For each of the N real draws we issue, we update the binding
//      offset of _drawIndexBuffer (using the slot recorded in #1) so the
//      shader's `_MDI_DrawIndex_Buffer[0]` reads the i'th draw index.
//
// For non-prime indirect draws (Unity's own, other plugins, etc.) we
// pass through to the original implementation untouched.

#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <atomic>

#include "MDIBackend_Metal.h"
#include "Unity/IUnityGraphicsMetal.h"

// MTLDrawIndexedPrimitivesIndirectArguments — 5 * uint32 = 20 bytes.
static constexpr NSUInteger kIndirectArgStride = 20;

// Pointer to the C# `_dummyArgsBuffer` (id<MTLBuffer>). Treated as an
// opaque key — only used for pointer-equality comparison in the hook.
static std::atomic<void*> g_dummyArgsBufferPtr{nullptr};

// Pointer to the C# `_drawIndexBuffer` (id<MTLBuffer>). Same purpose —
// pointer key. We also keep an unretained Objective-C reference so we
// can rebind it with different offsets per draw.
static std::atomic<void*> g_drawIndexBufferPtr{nullptr};

// MTL buffer slot Unity assigned to _drawIndexBuffer, captured by the
// setVertexBuffer:offset:atIndex: swizzle. NSUIntegerMax means "not yet seen".
static std::atomic<NSUInteger> g_drawIndexSlot{NSUIntegerMax};

// Base pointer of the C# pinned `_paramsRing` NativeArray. C# writes the
// per-slot params here BEFORE issuing the prime draw, so the hook can
// read them synchronously from the same render thread.
static std::atomic<const MDIParams*> g_paramsRingBase{nullptr};

// Saved original IMPs.
typedef void (*DrawIndirectIMP)(id, SEL,
                                MTLPrimitiveType, MTLIndexType,
                                id<MTLBuffer>, NSUInteger,
                                id<MTLBuffer>, NSUInteger);
typedef void (*SetVertexBufferIMP)(id, SEL,
                                   id<MTLBuffer>, NSUInteger, NSUInteger);
typedef void (*SetVertexBuffersIMP)(id, SEL,
                                    const id<MTLBuffer> __unsafe_unretained*,
                                    const NSUInteger*,
                                    NSRange);

static std::atomic<DrawIndirectIMP>     g_origDrawIndirect{nullptr};
static std::atomic<SetVertexBufferIMP>  g_origSetVertexBuffer{nullptr};
static std::atomic<SetVertexBuffersIMP> g_origSetVertexBuffers{nullptr};

// Re-entry guard for the draw hook (the loop below calls the original
// IMP, which is the same Method we just swizzled).
static thread_local bool t_inDrawHook = false;

// -----------------------------------------------------------------------
// C exports — called from MultiDrawIndirect.cpp dispatcher
// -----------------------------------------------------------------------

extern "C" void MDIMetal_SetDummyArgsBuffer(void* nativePtr)
{
    g_dummyArgsBufferPtr.store(nativePtr, std::memory_order_release);
}

extern "C" void MDIMetal_SetParamsRing(const MDIParams* basePtr)
{
    g_paramsRingBase.store(basePtr, std::memory_order_release);
}

extern "C" void MDIMetal_SetDrawIndexBuffer(void* nativePtr)
{
    // Reset the cached slot when the buffer pointer changes — Unity may
    // recreate the buffer after device events.
    g_drawIndexSlot.store(NSUIntegerMax, std::memory_order_release);
    g_drawIndexBufferPtr.store(nativePtr, std::memory_order_release);
}

// -----------------------------------------------------------------------
// Hook 1: setVertexBuffer:offset:atIndex: — captures Unity's slot
// -----------------------------------------------------------------------

static void HookedSetVertexBuffer(id self, SEL _cmd,
                                  id<MTLBuffer> buffer,
                                  NSUInteger offset,
                                  NSUInteger index)
{
    void* tracked = g_drawIndexBufferPtr.load(std::memory_order_acquire);
    if (tracked && buffer && (__bridge void*)buffer == tracked)
        g_drawIndexSlot.store(index, std::memory_order_release);

    SetVertexBufferIMP orig = g_origSetVertexBuffer.load(std::memory_order_acquire);
    if (orig) orig(self, _cmd, buffer, offset, index);
}

static void HookedSetVertexBuffers(id self, SEL _cmd,
                                   const id<MTLBuffer> __unsafe_unretained* buffers,
                                   const NSUInteger* offsets,
                                   NSRange range)
{
    void* tracked = g_drawIndexBufferPtr.load(std::memory_order_acquire);
    if (tracked && buffers)
    {
        for (NSUInteger i = 0; i < range.length; ++i)
        {
            id<MTLBuffer> b = buffers[i];
            if (b && (__bridge void*)b == tracked)
            {
                g_drawIndexSlot.store(range.location + i, std::memory_order_release);
                break;
            }
        }
    }
    SetVertexBuffersIMP orig = g_origSetVertexBuffers.load(std::memory_order_acquire);
    if (orig) orig(self, _cmd, buffers, offsets, range);
}

// -----------------------------------------------------------------------
// Hook 2: drawIndexedPrimitives:...:indirectBuffer:indirectBufferOffset:
// -----------------------------------------------------------------------

static void HookedDrawIndirect(id self, SEL _cmd,
                               MTLPrimitiveType primitive,
                               MTLIndexType indexType,
                               id<MTLBuffer> indexBuffer,
                               NSUInteger indexBufferOffset,
                               id<MTLBuffer> indirectBuffer,
                               NSUInteger indirectBufferOffset)
{
    DrawIndirectIMP orig = g_origDrawIndirect.load(std::memory_order_acquire);

    // Bypass the hook if we're already inside it.
    if (t_inDrawHook || !orig)
    {
        if (orig) orig(self, _cmd, primitive, indexType,
                       indexBuffer, indexBufferOffset,
                       indirectBuffer, indirectBufferOffset);
        return;
    }

    void* dummy = g_dummyArgsBufferPtr.load(std::memory_order_acquire);
    if (dummy && indirectBuffer && (__bridge void*)indirectBuffer == dummy)
    {
        // Decode our slot from the prime's argsOffset.
        NSUInteger slot = indirectBufferOffset / kIndirectArgStride;
        const MDIParams* ring = g_paramsRingBase.load(std::memory_order_acquire);
        NSUInteger drawIndexSlot = g_drawIndexSlot.load(std::memory_order_acquire);
        void* drawIdxBufPtr = g_drawIndexBufferPtr.load(std::memory_order_acquire);

        if (ring && slot < (NSUInteger)MDI_MAX_PENDING &&
            drawIndexSlot != NSUIntegerMax && drawIdxBufPtr)
        {
            MDIParams params = ring[slot]; // copy from C# pinned NativeArray
            if (params.argsBuffer && params.indexBuffer && params.maxDrawCount > 0)
            {
                id<MTLBuffer> realArgs    = (__bridge id<MTLBuffer>)params.argsBuffer;
                id<MTLBuffer> realIndex   = (__bridge id<MTLBuffer>)params.indexBuffer;
                NSUInteger argsOff = (NSUInteger)params.argsOffsetBytes;

                id<MTLRenderCommandEncoder> renderEnc = (id<MTLRenderCommandEncoder>)self;

                // Note: Metal has no GPU-count indirect draw API. With
                // MDI_FLAG_GPU_COUNT we execute the maxDrawCount upper bound —
                // unused args entries must have instanceCount = 0.
                t_inDrawHook = true;
                for (uint32_t i = 0; i < params.maxDrawCount; ++i)
                {
                    // Push the draw index inline at the slot Unity assigned
                    // to `_MDI_DrawIndex_Buffer`. setVertexBytes bypasses any
                    // buffer-size limit (vs. setVertexBuffer:offset:), so this
                    // works for arbitrary draw counts. 16 bytes is the
                    // alignment-safe minimum for buffer-backed reads on Metal.
                    uint32_t drawIndexInline[4] = { i, 0, 0, 0 };
                    [renderEnc setVertexBytes:drawIndexInline
                                       length:16
                                      atIndex:drawIndexSlot];

                    orig(self, _cmd,
                         primitive, indexType,
                         realIndex, 0,
                         realArgs, argsOff);
                    argsOff += kIndirectArgStride;
                }
                t_inDrawHook = false;
                return;
            }
        }
    }

    // Default path — let Unity's draw go through unchanged.
    orig(self, _cmd, primitive, indexType,
         indexBuffer, indexBufferOffset,
         indirectBuffer, indirectBufferOffset);
}

// -----------------------------------------------------------------------
// Hook installation
// -----------------------------------------------------------------------

static bool InstallHooksOnClass(Class encClass)
{
    if (!encClass) return false;

    // Hook 1a: setVertexBuffer:offset:atIndex: (singular)
    {
        SEL sel = @selector(setVertexBuffer:offset:atIndex:);
        Method m = class_getInstanceMethod(encClass, sel);
        if (m && g_origSetVertexBuffer.load(std::memory_order_acquire) == nullptr)
        {
            SetVertexBufferIMP origIMP = (SetVertexBufferIMP)method_getImplementation(m);
            g_origSetVertexBuffer.store(origIMP, std::memory_order_release);
            method_setImplementation(m, (IMP)HookedSetVertexBuffer);
        }
    }

    // Hook 1b: setVertexBuffers:offsets:withRange: (plural)
    {
        SEL sel = @selector(setVertexBuffers:offsets:withRange:);
        Method m = class_getInstanceMethod(encClass, sel);
        if (m && g_origSetVertexBuffers.load(std::memory_order_acquire) == nullptr)
        {
            SetVertexBuffersIMP origIMP = (SetVertexBuffersIMP)method_getImplementation(m);
            g_origSetVertexBuffers.store(origIMP, std::memory_order_release);
            method_setImplementation(m, (IMP)HookedSetVertexBuffers);
        }
    }

    // Hook 2: drawIndexedPrimitives:...indirectBuffer:indirectBufferOffset:
    {
        SEL sel = @selector(drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:);
        Method m = class_getInstanceMethod(encClass, sel);
        if (!m) return false;
        if (g_origDrawIndirect.load(std::memory_order_acquire) == nullptr)
        {
            DrawIndirectIMP origIMP = (DrawIndirectIMP)method_getImplementation(m);
            g_origDrawIndirect.store(origIMP, std::memory_order_release);
            method_setImplementation(m, (IMP)HookedDrawIndirect);
        }
    }

    return true;
}

bool MDIBackend_Metal::Initialize(IUnityInterfaces* unityInterfaces)
{
    _metalV2 = unityInterfaces->Get<IUnityGraphicsMetalV2>();
    _metal   = unityInterfaces->Get<IUnityGraphicsMetalV1>();
    if (!_metal && !_metalV2) return false;

    id<MTLDevice> dev = _metalV2 ? _metalV2->MetalDevice()
                                 : (_metal ? _metal->MetalDevice() : nil);
    if (!dev) return false;

    // Probe the encoder class by spinning up a throwaway render encoder on
    // Unity's own device — same class Unity will use for real draws.
    Class encClass = nil;
    @autoreleasepool
    {
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];

        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                         width:1 height:1
                                     mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture     = tex;
        rp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionDontCare;

        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (enc)
        {
            encClass = [enc class];
            [enc endEncoding];
        }
    }

    if (!InstallHooksOnClass(encClass)) return false;

    _initialized = true;
    return true;
}

void MDIBackend_Metal::Shutdown()
{
    // We deliberately do NOT uninstall the hooks. method_setImplementation
    // back to the original is racy with in-flight render-thread calls and
    // the bundle is normally only unloaded at editor exit. The hooks
    // check buffer pointers, which we null out here so they no-op until
    // the next Initialize.
    g_dummyArgsBufferPtr.store(nullptr, std::memory_order_release);
    g_drawIndexBufferPtr.store(nullptr, std::memory_order_release);
    g_drawIndexSlot.store(NSUIntegerMax, std::memory_order_release);
    g_paramsRingBase.store(nullptr, std::memory_order_release);
    _metal = nullptr;
    _metalV2 = nullptr;
    _initialized = false;
}

void MDIBackend_Metal::ExecuteMDI(const MDIParams& /*params*/)
{
    // No-op. The actual MDI work happens inside HookedDrawIndirect, fired
    // synchronously from Unity's prime DrawProceduralIndirect.
}

bool MDIBackend_Metal::IsSupported() const
{
    return _initialized && g_origDrawIndirect.load(std::memory_order_acquire) != nullptr;
}
