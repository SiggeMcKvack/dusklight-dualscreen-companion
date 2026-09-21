#include "present.hpp"

#include "mods/service.hpp"
#include "mods/svc/gfx.h"
#include "mods/svc/log.hpp"

#include <android/native_window.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace dsc::present {
namespace {

// Fullscreen triangle sampling the companion texture; the dim is a blend constant
// (out = src * constant) so the letterbox stays at the pass clear (black).
constexpr const char* kBlitShader = R"(
struct VSOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VSOut {
    let x = f32((index << 1u) & 2u);
    let y = f32(index & 2u);
    var out: VSOut;
    out.pos = vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
    out.uv = vec2<f32>(x, y);
    return out;
}

@group(0) @binding(0) var src_texture: texture_2d<f32>;
@group(0) @binding(1) var src_sampler: sampler;

@fragment
fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
    return vec4<f32>(textureSample(src_texture, src_sampler, in.uv).rgb, 1.0);
}
)";

struct Payload {
    WGPUTextureView source;
    uint32_t width;
    uint32_t height;
    float dim;
    float clear[3];  // used when there is no source (boot / test pattern)
};
static_assert(sizeof(Payload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);

// ---- game-thread state ----
ANativeWindow* g_window = nullptr;
uint32_t g_width = 0;
uint32_t g_height = 0;
WGPUSurface g_surface = nullptr;
GfxPresentTargetHandle g_target = 0;
std::atomic<bool> g_targetActive{false};
bool g_recreate = false;
int g_recreateFailures = 0;

// Source views are kept alive by the mod for a few frames past their last push so the render
// worker never records a blit from a view whose last reference was just dropped.
constexpr size_t kRetireFrames = 3;
WGPUTextureView g_retire[kRetireFrames] = {};
size_t g_retireIndex = 0;

// ---- render-worker state (created lazily on first present) ----
WGPUTextureFormat g_pipelineFormat = WGPUTextureFormat_Undefined;
WGPURenderPipeline g_pipeline = nullptr;
WGPUBindGroupLayout g_bindGroupLayout = nullptr;
WGPUSampler g_sampler = nullptr;

void destroy_worker_objects() {
    if (g_bindGroupLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_bindGroupLayout);
        g_bindGroupLayout = nullptr;
    }
    if (g_pipeline != nullptr) {
        wgpuRenderPipelineRelease(g_pipeline);
        g_pipeline = nullptr;
    }
    if (g_sampler != nullptr) {
        wgpuSamplerRelease(g_sampler);
        g_sampler = nullptr;
    }
    g_pipelineFormat = WGPUTextureFormat_Undefined;
}

bool ensure_pipeline(WGPUDevice device, WGPUTextureFormat format) {
    if (g_pipeline != nullptr && g_pipelineFormat == format) {
        return true;
    }
    destroy_worker_objects();

    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = WGPUStringView{kBlitShader, WGPU_STRLEN};
    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = WGPUStringView{"Companion blit", WGPU_STRLEN};
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &moduleDesc);
    if (module == nullptr) {
        return false;
    }

    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_Constant;
    blend.color.dstFactor = WGPUBlendFactor_Zero;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_Zero;

    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = format;
    target.blend = &blend;
    target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = WGPUStringView{"fs_main", WGPU_STRLEN};
    fragment.targetCount = 1;
    fragment.targets = &target;

    WGPURenderPipelineDescriptor pipelineDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDesc.label = WGPUStringView{"Companion blit pipeline", WGPU_STRLEN};
    pipelineDesc.vertex.module = module;
    pipelineDesc.vertex.entryPoint = WGPUStringView{"vs_main", WGPU_STRLEN};
    pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipelineDesc.multisample.count = 1;
    pipelineDesc.multisample.mask = 0xFFFFFFFFu;
    pipelineDesc.fragment = &fragment;
    g_pipeline = wgpuDeviceCreateRenderPipeline(device, &pipelineDesc);
    wgpuShaderModuleRelease(module);
    if (g_pipeline == nullptr) {
        return false;
    }
    g_bindGroupLayout = wgpuRenderPipelineGetBindGroupLayout(g_pipeline, 0);
    if (g_bindGroupLayout == nullptr) {
        destroy_worker_objects();
        return false;
    }

    WGPUSamplerDescriptor samplerDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    samplerDesc.label = WGPUStringView{"Companion blit sampler", WGPU_STRLEN};
    samplerDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    samplerDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    samplerDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    samplerDesc.magFilter = WGPUFilterMode_Linear;
    samplerDesc.minFilter = WGPUFilterMode_Linear;
    samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    samplerDesc.maxAnisotropy = 1;
    g_sampler = wgpuDeviceCreateSampler(device, &samplerDesc);
    if (g_sampler == nullptr) {
        destroy_worker_objects();  // leave nothing half-built for the early-out above
        return false;
    }
    g_pipelineFormat = format;
    return true;
}

// Render worker: clear the panel, then aspect-fit blit the source (if any) with the dim.
void on_present(
    ModContext*, const GfxPresentContext* ctx, const void* payloadRaw, size_t payloadSize, void*) {
    Payload payload{};
    if (payloadSize == sizeof(Payload)) {
        std::memcpy(&payload, payloadRaw, sizeof(payload));
    }

    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = ctx->target_view;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{payload.clear[0], payload.clear[1], payload.clear[2], 1.0};
    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.label = WGPUStringView{"Companion panel", WGPU_STRLEN};
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &color;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(ctx->encoder, &passDesc);

    const bool haveSource = payload.source != nullptr && payload.width != 0 &&
                            payload.height != 0 && ctx->target_width != 0 &&
                            ctx->target_height != 0;
    if (haveSource && ensure_pipeline(ctx->device, ctx->target_format)) {
        WGPUBindGroupEntry entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        entries[0].binding = 0;
        entries[0].textureView = payload.source;
        entries[1].binding = 1;
        entries[1].sampler = g_sampler;
        WGPUBindGroupDescriptor bindDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bindDesc.layout = g_bindGroupLayout;
        bindDesc.entryCount = 2;
        bindDesc.entries = entries;
        WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(ctx->device, &bindDesc);

        const float scale =
            std::min(static_cast<float>(ctx->target_width) / static_cast<float>(payload.width),
                static_cast<float>(ctx->target_height) / static_cast<float>(payload.height));
        const float viewW = static_cast<float>(payload.width) * scale;
        const float viewH = static_cast<float>(payload.height) * scale;
        const float viewX = (static_cast<float>(ctx->target_width) - viewW) * 0.5f;
        const float viewY = (static_cast<float>(ctx->target_height) - viewH) * 0.5f;
        const double keep = 1.0 - std::clamp(static_cast<double>(payload.dim), 0.0, 1.0);
        const WGPUColor blendConstant{keep, keep, keep, 1.0};

        wgpuRenderPassEncoderSetPipeline(pass, g_pipeline);
        wgpuRenderPassEncoderSetBlendConstant(pass, &blendConstant);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
        wgpuRenderPassEncoderSetViewport(pass, viewX, viewY, viewW, viewH, 0.0f, 1.0f);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuBindGroupRelease(bindGroup);  // the pass holds its own reference
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

void drop_target() {
    if (g_target != 0) {
        svc_gfx->unregister_present_target(mod_ctx, g_target);
        g_target = 0;
        g_targetActive.store(false);
    }
    if (g_surface != nullptr) {
        wgpuSurfaceRelease(g_surface);
        g_surface = nullptr;
    }
    if (g_window != nullptr) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }
    g_width = g_height = 0;
    g_recreate = false;
    g_recreateFailures = 0;
}

bool register_target() {
    if (g_surface == nullptr || g_target != 0 || g_width == 0 || g_height == 0) {
        return false;
    }
    GfxPresentTargetDesc desc = GFX_PRESENT_TARGET_DESC_INIT;
    desc.label = "Companion panel surface";
    desc.width = g_width;
    desc.height = g_height;
    desc.render = on_present;
    desc.preferred_alpha_mode = WGPUCompositeAlphaMode_Opaque;
    const ModResult result = svc_gfx->register_present_target(mod_ctx, g_surface, &desc, &g_target);
    if (result != MOD_OK) {
        mods::log::error("register_present_target failed ({})", static_cast<int>(result));
        g_target = 0;
        return false;
    }
    g_targetActive.store(true);
    return true;
}

void retire(WGPUTextureView view) {
    WGPUTextureView& slot = g_retire[g_retireIndex];
    if (slot != nullptr) {
        wgpuTextureViewRelease(slot);
    }
    slot = view;
    g_retireIndex = (g_retireIndex + 1) % kRetireFrames;
}

}  // namespace

void set_native_window(ANativeWindow* window, uint32_t width, uint32_t height) {
    drop_target();
    if (window == nullptr) {
        mods::log::info("companion surface lost");
        return;
    }
    GfxDeviceInfo info = GFX_DEVICE_INFO_INIT;
    if (svc_gfx->get_device_info(mod_ctx, &info) != MOD_OK || info.instance == nullptr) {
        mods::log::error("no WGPUInstance available for the companion surface");
        ANativeWindow_release(window);
        return;
    }
    g_window = window;
    g_width = width;
    g_height = height;

    WGPUSurfaceSourceAndroidNativeWindow source = WGPU_SURFACE_SOURCE_ANDROID_NATIVE_WINDOW_INIT;
    source.window = window;
    WGPUSurfaceDescriptor surfaceDesc = WGPU_SURFACE_DESCRIPTOR_INIT;
    surfaceDesc.nextInChain = &source.chain;
    surfaceDesc.label = WGPUStringView{"Companion panel", WGPU_STRLEN};
    g_surface = wgpuInstanceCreateSurface(info.instance, &surfaceDesc);
    if (g_surface == nullptr) {
        mods::log::error("wgpuInstanceCreateSurface failed for the companion panel");
        drop_target();
        return;
    }
    if (register_target()) {
        mods::log::info("companion present target ready ({}x{})", width, height);
    }
}

bool has_target() {
    return g_target != 0;
}

bool target_active() {
    return g_targetActive.load();
}

void push_frame(WGPUTextureView source, uint32_t sourceWidth, uint32_t sourceHeight, float dim,
    const float clearColor[3]) {
    if (g_target == 0) {
        return;
    }
    if (source != nullptr) {
        wgpuTextureViewAddRef(source);
        retire(source);
    }
    Payload payload{source, sourceWidth, sourceHeight, dim, {0.0f, 0.0f, 0.0f}};
    if (clearColor != nullptr) {
        std::memcpy(payload.clear, clearColor, sizeof(payload.clear));
    }
    const ModResult result = svc_gfx->push_present(mod_ctx, g_target, &payload, sizeof(payload));
    if (result == MOD_ERROR) {
        g_recreate = true;
    }
}

void update() {
    if (!g_recreate || g_surface == nullptr) {
        return;
    }
    g_recreate = false;
    if (g_target != 0) {
        svc_gfx->unregister_present_target(mod_ctx, g_target);
        g_target = 0;
        g_targetActive.store(false);
    }
    if (register_target()) {
        g_recreateFailures = 0;
        return;
    }
    if (++g_recreateFailures >= 2) {
        mods::log::warn("companion present target could not be recreated; waiting for a new surface");
        drop_target();
    } else {
        g_recreate = true;
    }
}

void shutdown() {
    drop_target();
    for (WGPUTextureView& view : g_retire) {
        if (view != nullptr) {
            wgpuTextureViewRelease(view);
            view = nullptr;
        }
    }
    // Pipeline objects were created on the render worker; the device outlives the mod and the
    // worker is idle between frames when shutdown runs (game thread, mod unload), so releasing
    // here is safe.
    destroy_worker_objects();
}

}  // namespace dsc::present
