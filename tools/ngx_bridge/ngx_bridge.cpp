// See ngx_bridge.h.
#define NGXB_BUILD
#include "ngx_bridge.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_vk.h"

namespace {

// Any stable id: NGX keys its per-application data on it.
constexpr char kProjectId[] = "b7d1e0a4-6c2f-4f3e-9a58-2b0dd7c1b109";

std::mutex g_mu;
NgxbLogFn g_log = nullptr;
VkDevice g_device = VK_NULL_HANDLE;
NVSDK_NGX_Parameter* g_params = nullptr;
NVSDK_NGX_Handle* g_feature = nullptr;

void logf(const char* fmt, ...) {
    if (!g_log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log(buf);
}

void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    if (!g_log || !message) return;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "ngx: %s", message);
    // NGX ends its lines with a newline; the host's log adds its own.
    for (char* p = buf; *p; ++p) {
        if ((*p == '\n' || *p == '\r') && !p[1]) *p = 0;
    }
    g_log(buf);
}

NVSDK_NGX_Resource_VK resource(const NgxbImage& im, bool rw) {
    VkImageSubresourceRange range{};
    range.aspectMask = im.aspect ? im.aspect : VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    return NVSDK_NGX_Create_ImageView_Resource_VK(im.view, im.image, range, im.format, im.width, im.height, rw);
}

}  // namespace

extern "C" {

int32_t ngxb_version(void) { return NGXB_VERSION; }

uint32_t ngxb_required_extensions(uint32_t* instance_count, const char* const** instance_exts, uint32_t* device_count,
                                  const char* const** device_exts) {
    unsigned int ni = 0, nd = 0;
    const char** inst = nullptr;
    const char** dev = nullptr;
    const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &inst, &nd, &dev);
    if (instance_count) *instance_count = NVSDK_NGX_SUCCEED(r) ? ni : 0;
    if (instance_exts) *instance_exts = NVSDK_NGX_SUCCEED(r) ? inst : nullptr;
    if (device_count) *device_count = NVSDK_NGX_SUCCEED(r) ? nd : 0;
    if (device_exts) *device_exts = NVSDK_NGX_SUCCEED(r) ? dev : nullptr;
    return static_cast<uint32_t>(r);
}

uint32_t ngxb_init(VkInstance instance, VkPhysicalDevice physical, VkDevice device, PFN_vkGetInstanceProcAddr gipa,
                   PFN_vkGetDeviceProcAddr gdpa, const wchar_t* dll_dir, const wchar_t* data_dir, NgxbLogFn log) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_log = log;
    static const wchar_t* paths[1];
    paths[0] = dll_dir;
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = &ngx_log;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data_dir, instance,
                                                              physical, device, gipa, gdpa, &info);
    if (NVSDK_NGX_FAILED(r)) {
        logf("NGX init failed: 0x%08X", static_cast<unsigned>(r));
        return static_cast<uint32_t>(r);
    }
    g_device = device;
    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(r) || !g_params) {
        logf("NGX capability parameters failed: 0x%08X", static_cast<unsigned>(r));
        NVSDK_NGX_VULKAN_Shutdown1(device);
        g_device = VK_NULL_HANDLE;
        g_params = nullptr;
        return static_cast<uint32_t>(NVSDK_NGX_FAILED(r) ? r : NVSDK_NGX_Result_Fail);
    }
    int available = 0, needs_driver = 0;
    unsigned int major = 0, minor = 0;
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
    if (!available) {
        logf("DLSS not available on this GPU/driver (needs a newer driver: %d, minimum %u.%u)", needs_driver, major, minor);
        NVSDK_NGX_VULKAN_DestroyParameters(g_params);
        NVSDK_NGX_VULKAN_Shutdown1(device);
        g_params = nullptr;
        g_device = VK_NULL_HANDLE;
        return static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_FeatureNotSupported);
    }
    logf("NGX ready (DLSS available)");
    return static_cast<uint32_t>(NVSDK_NGX_Result_Success);
}

uint32_t ngxb_create(VkCommandBuffer cmd, uint32_t width, uint32_t height, uint32_t preset, int32_t flags) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_params) return static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_NotInitialized);
    if (g_feature) {
        NVSDK_NGX_VULKAN_ReleaseFeature(g_feature);
        g_feature = nullptr;
    }
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset);
    NVSDK_NGX_DLSS_Create_Params p = {};
    p.Feature.InWidth = width;
    p.Feature.InHeight = height;
    p.Feature.InTargetWidth = width;
    p.Feature.InTargetHeight = height;
    p.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    p.InFeatureCreateFlags = flags;
    const NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(g_device, cmd, 1, 1, &g_feature, g_params, &p);
    if (NVSDK_NGX_FAILED(r)) {
        logf("creating the DLAA feature (%ux%u, flags 0x%x) failed: 0x%08X", width, height, flags, static_cast<unsigned>(r));
        g_feature = nullptr;
    } else {
        logf("DLAA feature created: %ux%u, preset %u, flags 0x%x", width, height, preset, flags);
    }
    return static_cast<uint32_t>(r);
}

uint32_t ngxb_evaluate(VkCommandBuffer cmd, const NgxbEval* e) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_feature || !e) return static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_FeatureNotFound);
    NVSDK_NGX_Resource_VK color = resource(e->color, false), output = resource(e->output, true),
                          depth = resource(e->depth, false), motion = resource(e->motion, false);
    NVSDK_NGX_VK_DLSS_Eval_Params p = {};
    p.Feature.pInColor = &color;
    p.Feature.pInOutput = &output;
    p.pInDepth = &depth;
    p.pInMotionVectors = &motion;
    p.InJitterOffsetX = e->jitter_x;
    p.InJitterOffsetY = e->jitter_y;
    p.InRenderSubrectDimensions.Width = e->color.width;
    p.InRenderSubrectDimensions.Height = e->color.height;
    p.InReset = e->reset;
    p.InMVScaleX = e->mv_scale_x;
    p.InMVScaleY = e->mv_scale_y;
    p.InFrameTimeDeltaInMsec = e->frame_ms;
    return static_cast<uint32_t>(NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, g_feature, g_params, &p));
}

void ngxb_release(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_feature) NVSDK_NGX_VULKAN_ReleaseFeature(g_feature);
    g_feature = nullptr;
}

void ngxb_shutdown(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_feature) NVSDK_NGX_VULKAN_ReleaseFeature(g_feature);
    g_feature = nullptr;
    if (g_params) NVSDK_NGX_VULKAN_DestroyParameters(g_params);
    g_params = nullptr;
    if (g_device) NVSDK_NGX_VULKAN_Shutdown1(g_device);
    g_device = VK_NULL_HANDLE;
}

}  // extern "C"
