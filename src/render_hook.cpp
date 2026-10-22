#include "render_hook.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "game.hpp"
#include "log.hpp"
#include "ring_fix.hpp"
#include "stereo.hpp"

namespace tc2vr::render_hook {
namespace {

using namespace game;

using SceneExecuteFn = void (*)(void* output, uint8_t* frameCtx);

constexpr size_t kPipelineScene = 2;
constexpr size_t kExecuteSlot = 8;

// The instancing cull jobs claim work with lock xadd on these
// cursors (5 view types x 8 slices). The game resets them once per
// frame, so every extra view must start them from zero again.
constexpr size_t kRenderersInstancing = 0x28;
constexpr size_t kInstancingCursors = 0x88;
constexpr size_t kInstancingCursorBytes = 5 * 8 * sizeof(uint32_t);

SceneBuildFn g_origBuild = nullptr;
SceneExecuteFn g_origExecute = nullptr;
std::atomic<uint64_t> g_eyeCameras[stereo::kEyeCount];

// Per-view CPU timings (index 0..1 eyes, 2 other views), summed
// between reports.
struct ViewTiming {
    double buildMs = 0.0;
    double executeMs = 0.0;
    uint32_t count = 0;
};
ViewTiming g_timing[3];

// Build and execute run back to back per view on the render thread.
int g_activeEye = -1;
uint32_t g_mainSecondaryFlag = 0;

double MsSince(const LARGE_INTEGER& start) {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - start.QuadPart) * 1000.0 /
           double(freq.QuadPart);
}

int TimingSlot(int eye) {
    return eye >= 0 ? eye : 2;
}

// Eyes build as secondary views, so only the main view advances the
// culler's per-object fade state (what the game's pipelines 0/1 do).
void EnterEye() {
    uint32_t* secondary = At<uint32_t*>(rva::SecondaryViewFlag);
    g_mainSecondaryFlag = *secondary;
    *secondary = 1;
}

void LeaveEye() {
    *At<uint32_t*>(rva::SecondaryViewFlag) = g_mainSecondaryFlag;
}

int EyeForViewport(const uint8_t* viewport) {
    uint64_t camera = Read<uint64_t>(viewport, off::VpCamera);
    if (camera == InvalidHandle) {
        return -1;
    }
    for (int eye = 0; eye < stereo::kEyeCount; ++eye) {
        if (camera == g_eyeCameras[eye].load()) {
            return eye;
        }
    }
    return -1;
}

void HookedBuild(uint8_t* viewport, uint8_t* viewData, void* output,
                 uint8_t* frameCtx) {
    g_activeEye = EyeForViewport(viewport);
    // First eye build: in world, render thread, no ring allocation yet.
    static bool ringFix = false;
    if (g_activeEye == 0 && !ringFix) {
        ringFix = true;
        ring_fix::Install();
    }
    if (g_activeEye >= 0) {
        EnterEye();
        stereo::ApplyHeadPose(g_activeEye);
    }
    auto renderers = Read<uint8_t*>(frameCtx, off::FrameCtxRenderers);
    uint8_t* instancing = Read<uint8_t*>(renderers, kRenderersInstancing);
    memset(instancing + kInstancingCursors, 0, kInstancingCursorBytes);
    LARGE_INTEGER buildStart;
    QueryPerformanceCounter(&buildStart);
    g_origBuild(viewport, viewData, output, frameCtx);
    g_timing[TimingSlot(g_activeEye)].buildMs += MsSince(buildStart);
}

void HookedExecute(void* output, uint8_t* frameCtx) {
    LARGE_INTEGER executeStart;
    QueryPerformanceCounter(&executeStart);
    g_origExecute(output, frameCtx);
    ViewTiming& timing = g_timing[TimingSlot(g_activeEye)];
    timing.executeMs += MsSince(executeStart);
    ++timing.count;
    if (g_activeEye >= 0) {
        LeaveEye();
        g_activeEye = -1;
    }
}

}  // namespace

void ReportTimings(uint32_t frames) {
    static const char* kNames[3] = {"eyeL", "eyeR", "main"};
    for (int i = 0; i < 3; ++i) {
        ViewTiming& t = g_timing[i];
        if (frames) {
            LOG("  %s: build %.2f ms  execute %.2f ms  (%u views)", kNames[i],
                t.buildMs / frames, t.executeMs / frames, t.count);
        }
        t = ViewTiming{};
    }
}

void SetEyeCamera(int eye, uint64_t cameraHandle) {
    g_eyeCameras[eye] = cameraHandle;
}

bool InstallSceneBuildHook() {
    for (auto& eye : g_eyeCameras) {
        eye = InvalidHandle;
    }
    uint8_t* renderer = *At<uint8_t**>(rva::Renderer);
    if (!renderer) {
        LOG("scene hooks: renderer not created yet");
        return false;
    }
    uint8_t* frameCtx = Read<uint8_t*>(renderer, off::RendererFrameCtx);
    uint8_t* table = Read<uint8_t*>(frameCtx, off::FrameCtxPipelines);
    uint8_t* entry = table + kPipelineScene * off::PipelineEntrySize;
    auto build = reinterpret_cast<void**>(entry);
    auto execute = reinterpret_cast<void**>(entry + kExecuteSlot);
    if (*build != At<void*>(rva::SceneBuild)) {
        LOG("scene hooks: unexpected pipeline entry %p", *build);
        return false;
    }
    g_origBuild = reinterpret_cast<SceneBuildFn>(*build);
    g_origExecute = reinterpret_cast<SceneExecuteFn>(*execute);
    // Heap memory, already writable; aligned pointer stores are atomic.
    *execute = reinterpret_cast<void*>(&HookedExecute);
    *build = reinterpret_cast<void*>(&HookedBuild);
    LOG("scene hooks installed (table %p)", table);
    return true;
}

}  // namespace tc2vr::render_hook
