// NVIDIA DLAA (DLSS at native resolution) on the HDR scene, before YEBIS.
//
// The anchor is YEBIS's copy of the HDR scene, 0b0acf50+ccbf44a6, its first
// post-process draw: it samples the lit scene, before depth of field, bloom,
// tone mapping and the game's own anti-aliasing. At its first draw in a frame,
// before its pass, the scene image goes through DLSS and the result is copied
// back into it, so everything after reads the anti-aliased scene.
//
// DLSS needs a jittered frame and motion vectors. The scene's geometry (the
// draws that test against the main depth) is jittered by a sub-pixel shift of
// its Vulkan viewport, Halton(2, 3) over 16 frames: no constant buffer or
// game code changes. The camera is read from the per-view constant buffer
// those draws bind; a compute pass (shaders/dlaa_mv.comp) turns the depth into
// camera motion vectors. docs/dlaa.md has the details.
//
// NGX is reached through ngx_bridge.dll (tools/ngx_bridge: an MSVC build that
// links NVIDIA's static library, which mingw cannot). Windows and NVIDIA RTX
// only; without the DLL, or anywhere else, it stays off and nothing changes.
//
// bbhost.toml [dlaa]: enabled (default true when ngx_bridge.dll is beside the
// exe), preset (a letter, "K"; default: the driver's). BBHOST_DLAA=0 turns it
// off. Keys: dlaa_hotkey below.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace gpu {

struct DrawRec;

// gpu.cpp, before vkCreateInstance / vkCreateDevice: the extensions NGX asks
// for, each added when the loader / device has it and the list does not.
void dlaa_add_instance_extensions(std::vector<const char*>& exts);
void dlaa_add_device_extensions(VkPhysicalDevice phys, std::vector<const char*>& exts);

// render.cpp: whether a pipeline (by its name, asked once a pipeline) is the anchor.
bool dlaa_is_anchor(const std::string& pipeline_name);
// render.cpp draw_impl, at an anchor draw, before its pass: the guest bases of
// the images its pixel shader samples (0 for one that did not resolve).
void dlaa_anchor_locked(const std::uint64_t* sampled, int count);
// render.cpp, after every draw: the main depth target, and the capture below.
void dlaa_after_draw_locked(const DrawRec& r);

// render.cpp draw_impl. Whether a draw is the scene's geometry: it
// tests against the main depth and is not a screen-space quad. Its viewport
// is shifted by dlaa_jitter_locked (pixels, x right, y down; 0 while DLAA is
// not evaluating), and its vertex shader's constant buffers are offered to
// dlaa_camera_note_locked (the 864-byte camera buffer, by its signature).
bool dlaa_main_draw_locked(std::uint64_t depth, std::uint32_t depth_ctl, std::uint32_t prim, std::uint32_t count);
void dlaa_jitter_locked(float* x, float* y);
void dlaa_camera_note_locked(std::uint64_t base, std::uint64_t bytes);
extern thread_local bool t_dlaa_main_draw;

// F12 also writes one whole frame of constant buffers (every draw's, VS and
// PS) beside the dump: <dir>/cbs-<flip>.txt and .bin, read by
// tools/dlaa_cbscan.py to find the camera.
void dlaa_request_cb_capture(const std::string& dir);
extern std::atomic<bool> g_dlaa_cb_capturing;
extern thread_local int t_dlaa_cb_stage;  // 0 VS, 1 PS while a draw's buffers resolve; -1 otherwise
void dlaa_cb_note_locked(std::size_t slot, std::uint64_t base, std::uint64_t bytes);

// window.cpp: Ctrl+F<n>. True when DLAA took the key. F1 DLAA on/off (A/B),
// F2 jitter on / off / test (DLSS skipped, jitter x8: the scene visibly
// shakes when the jitter reaches it), F3 the jitter's sign given to DLSS, F4
// zero motion vectors. Ctrl+F5-F8 do the same as F1-F4, for keyboards where
// another program holds Ctrl+F2/F3 as global hotkeys.
bool dlaa_hotkey(int fkey);

}  // namespace gpu
