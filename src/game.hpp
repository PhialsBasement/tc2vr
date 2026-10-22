#pragma once

#include <cstdint>
#include <cstring>

// Addresses and layouts for TheCrew2.exe (Steam, Sep 2026 build).
// Every RVA is checked against a signature before use.
namespace tc2vr::game {

namespace rva {
constexpr uintptr_t Game = 0x3480bb8;
constexpr uintptr_t GfxDevice = 0x39cf4a0;
constexpr uintptr_t SceneViewportTable = 0x33db7e0;
constexpr uintptr_t SceneViewportExTable = 0x33ddff0;
constexpr uintptr_t SceneCameraTable = 0x33ddc30;

constexpr uintptr_t VehicleCtrlVtblUpdate = 0x2b32b78;
constexpr uintptr_t VehicleCtrlUpdate = 0x0a05310;
constexpr uintptr_t CameraDataFinalize = 0x1ff7010;
constexpr uintptr_t SceneHandleResolve = 0x21e2e70;
constexpr uintptr_t SceneCameraCreate = 0x21da370;
constexpr uintptr_t SceneCameraResolveForWrite = 0x21df8c0;
constexpr uintptr_t SceneViewportExCreate = 0x21dcb10;
constexpr uintptr_t SceneViewportExResolveForWrite = 0x21e1190;
constexpr uintptr_t ViewBitAlloc = 0x221b2d0;
constexpr uintptr_t RenderTargetMgrCreate = 0x0fa0e90;
constexpr uintptr_t RenderSchedulerFrame = 0x2090300;
constexpr uintptr_t Renderer = 0x39d10d0;
// Nonzero marks the view being built as secondary: culling skips
// the per-object fade state machine (what pipelines 0 and 1 do).
constexpr uintptr_t SecondaryViewFlag = 0x39d1398;
constexpr uintptr_t SceneBuild = 0x20b3170;
}  // namespace rva

namespace off {
// Global game object
constexpr size_t GameComponents = 0x1dd8;
constexpr size_t GameComponentIndex = 0x1de8;
constexpr size_t GameMainView = 0x1e30;
constexpr size_t GameMainViewport = 0x144;
constexpr size_t CompRenderTargetMgr = 0x1a;

// Scene camera
constexpr size_t CamPos = 0x10;
constexpr size_t CamFrac = 0x20;
constexpr size_t CamQuat = 0x30;
constexpr size_t CamFov = 0x48;
constexpr size_t CamNear = 0x50;
constexpr size_t CamFar = 0x54;
constexpr size_t CamBlockBegin = 0x10;
constexpr size_t CamBlockEnd = 0x5c;
constexpr size_t MainViewCamera = 0x48;

// Scene viewport (on-screen and render-to-texture)
constexpr size_t VpCamera = 0x0c;
constexpr size_t VpPipeline = 0x14;
constexpr size_t VpRectOrigin = 0x18;
constexpr size_t VpRectSize = 0x20;
constexpr size_t VpViewBit = 0x40;
constexpr size_t VpSettingsBegin = 0x44;
constexpr size_t VpSettingsEnd = 0x4f8;
constexpr size_t VpKeepHistory = 0x4f8;
constexpr size_t VpEnabled = 0x4f9;
constexpr size_t VpOutput = 0x510;
constexpr uint32_t PipelineScene = 2;

// CameraData (0x600 bytes)
constexpr size_t CdPos = 0x00;
constexpr size_t CdFrac = 0x10;
constexpr size_t CdQuat = 0x20;
constexpr size_t CdFov = 0x38;
constexpr size_t CdNear = 0x40;
constexpr size_t CdFar = 0x44;

// Render target -> engine texture -> D3D11 texture
constexpr size_t RtTexture = 0x10;
constexpr size_t TexHal = 0x10;
constexpr size_t TexWidth = 0x18;
constexpr size_t TexHeight = 0x1c;
constexpr size_t TexRenderCount = 0x80;
constexpr size_t HalD3DTexture = 0x20;

// Renderer -> frame context -> pipeline table {build, execute}
constexpr size_t RendererFrameCtx = 0x20;
constexpr size_t FrameCtxPipelines = 0x50;
constexpr size_t FrameCtxRenderers = 0x68;
constexpr size_t PipelineEntrySize = 0x10;
}  // namespace off

using CtrlUpdateFn = void (*)(void* self, uint8_t* camData, float dt);
using FinalizeFn = void (*)(uint8_t* camData, bool full);
using CreateFn = void (*)(void* table, uint64_t* handle, uint64_t flags);
using ResolveFn = uint8_t* (*)(void* table, const uint64_t* handle);
// Scene objects are double-buffered. The first write-resolve after a
// buffer flip allocates a fresh object: copyPrevious=true carries the
// last frame's fields over, false starts from defaults (handles -1).
using ResolveWriteFn = uint8_t* (*)(void* table, const uint64_t* handle,
                                    bool copyPrevious);
using ViewBitAllocFn = uint32_t* (*)(uint32_t* out);
using RtCreateFn = uint8_t* (*)(void* mgr, uint32_t width, uint32_t height,
                                bool srgb, uint64_t name);
// Pipeline build: records one view's passes into the frame context.
using SceneBuildFn = void (*)(uint8_t* viewport, uint8_t* viewData,
                              void* output, uint8_t* frameCtx);

constexpr uint64_t InvalidHandle = ~0ull;

uintptr_t Base();
bool VerifyBuild();

template <typename T>
T At(uintptr_t rvaValue) {
    return reinterpret_cast<T>(Base() + rvaValue);
}

template <typename T>
T Read(const void* obj, size_t offset) {
    T value;
    memcpy(&value, static_cast<const uint8_t*>(obj) + offset, sizeof(T));
    return value;
}

template <typename T>
void Write(void* obj, size_t offset, const T& value) {
    memcpy(static_cast<uint8_t*>(obj) + offset, &value, sizeof(T));
}

uint8_t* GameObject();
void* Component(size_t slot);
uint8_t* ResolveViewport(uint64_t handle);
uint8_t* ResolveCamera(uint64_t handle);

}  // namespace tc2vr::game
