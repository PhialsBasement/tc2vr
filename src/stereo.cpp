#include "stereo.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>

#include "game.hpp"
#include "log.hpp"
#include "render_hook.hpp"
#include "xr.hpp"

namespace tc2vr::stereo {
namespace {

using namespace game;

// Frames to wait after losing a view before building a new one.
constexpr uint32_t kRecreateCooldown = 120;
constexpr uint32_t kMaxLosses = 3;
constexpr const char* kEyeNames[kEyeCount] = {"left", "right"};

struct EyeView {
    uint64_t camera = InvalidHandle;
    uint64_t viewport = InvalidHandle;
    uint8_t* renderTarget = nullptr;
    uint8_t* texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t lastCamPos[16] = {};
    bool camWritten = false;
    uint32_t cooldown = 0;
    uint32_t losses = 0;
    bool disabled = false;
};

struct Vec3 {
    float x, y, z;
};

CtrlUpdateFn g_origUpdate = nullptr;
EyeView g_eyes[kEyeCount];
std::atomic<uint8_t*> g_eyeTextures[kEyeCount] = {};
float g_ipd = kDefaultIpd;
bool g_buildHooked = false;
bool g_buildHookFailed = false;
uint32_t g_frame = 0;
std::atomic<uint64_t> g_lastDriveTick{0};
constexpr uint64_t kDrivingTimeoutMs = 300;

struct AppliedPose {
    uint8_t* cam = nullptr;
    float frac[4];
    float quat[4];
};
AppliedPose g_applied[kEyeCount];

bool PatchPointer(void** slot, void* value, void** previous) {
    DWORD oldProtect;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        return false;
    }
    *previous = *slot;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    return true;
}

// Camera space is +X right, +Y up, looking down -Z.
Vec3 RightVector(const float q[4]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    return {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + w * z),
            2.0f * (x * z - w * y)};
}

uint8_t* MainSceneCamera() {
    uint8_t* gameObj = GameObject();
    uint8_t* mainView = Read<uint8_t*>(gameObj, off::GameMainView);
    if (!mainView) {
        return nullptr;
    }
    return ResolveCamera(Read<uint64_t>(mainView, off::MainViewCamera));
}

uint8_t* MainViewport() {
    uint64_t handle = Read<uint64_t>(GameObject(), off::GameMainViewport);
    if (static_cast<uint32_t>(handle) == 0xffffffffu) {
        return nullptr;
    }
    return ResolveViewport(handle);
}

// Ownership is proven by content before every write. Always resolve
// with copyPrevious=true, or the write copy starts from defaults.
uint8_t* OwnedViewport(EyeView& eye) {
    void* vpTable = At<void*>(rva::SceneViewportExTable);
    auto vpWrite = At<ResolveWriteFn>(rva::SceneViewportExResolveForWrite);
    uint8_t* vp = vpWrite(vpTable, &eye.viewport, true);
    if (!vp || Read<uint64_t>(vp, off::VpCamera) != eye.camera ||
        Read<uint8_t*>(vp, off::VpOutput) != eye.texture) {
        return nullptr;
    }
    return vp;
}

uint8_t* OwnedCamera(EyeView& eye) {
    void* camTable = At<void*>(rva::SceneCameraTable);
    auto camWrite = At<ResolveWriteFn>(rva::SceneCameraResolveForWrite);
    uint8_t* cam = camWrite(camTable, &eye.camera, true);
    if (!cam || (eye.camWritten &&
                 memcmp(cam + off::CamPos, eye.lastCamPos, 16) != 0)) {
        return nullptr;
    }
    return cam;
}

void DropEye(int index, const char* reason) {
    EyeView& eye = g_eyes[index];
    uint32_t losses = eye.losses + 1;
    LOG("%s eye lost (%s), loss %u of %u", kEyeNames[index], reason, losses,
        kMaxLosses);
    g_eyeTextures[index] = nullptr;
    render_hook::SetEyeCamera(index, InvalidHandle);
    eye = EyeView{};
    eye.losses = losses;
    eye.cooldown = kRecreateCooldown;
    // Guard against leaking views if detection ever misfires again.
    if (losses >= kMaxLosses) {
        LOG("%s eye: repeated losses, not recreating", kEyeNames[index]);
        eye.disabled = true;
    }
}

bool CreateEye(int index) {
    EyeView& eye = g_eyes[index];
    uint8_t* mainCam = MainSceneCamera();
    uint8_t* mainVp = MainViewport();
    void* rtMgr = Component(off::CompRenderTargetMgr);
    if (!mainCam || !mainVp || !rtMgr) {
        return false;
    }

    void* camTable = At<void*>(rva::SceneCameraTable);
    At<CreateFn>(rva::SceneCameraCreate)(camTable, &eye.camera, 0);
    auto camWrite = At<ResolveWriteFn>(rva::SceneCameraResolveForWrite);
    uint8_t* cam = camWrite(camTable, &eye.camera, true);
    if (!cam) {
        LOG("%s eye: camera creation failed", kEyeNames[index]);
        return false;
    }
    memcpy(cam + off::CamBlockBegin, mainCam + off::CamBlockBegin,
           off::CamBlockEnd - off::CamBlockBegin);

    // The final pass writes gamma-encoded values, like the UNORM
    // back buffer; an sRGB target would encode them twice.
    eye.width = kEyeWidth;
    eye.height = kEyeHeight;
    if (xr::GetState() == xr::State::Ready) {
        xr::EyeTarget target = xr::GetEyeTarget();
        eye.width = target.width;
        eye.height = target.height;
    }
    auto createRt = At<RtCreateFn>(rva::RenderTargetMgrCreate);
    eye.renderTarget = createRt(rtMgr, eye.width, eye.height, false, 0);
    eye.texture = Read<uint8_t*>(eye.renderTarget, off::RtTexture);

    void* vpTable = At<void*>(rva::SceneViewportExTable);
    At<CreateFn>(rva::SceneViewportExCreate)(vpTable, &eye.viewport, 0);
    auto vpWrite = At<ResolveWriteFn>(rva::SceneViewportExResolveForWrite);
    uint8_t* vp = vpWrite(vpTable, &eye.viewport, true);
    if (!vp || !eye.texture) {
        LOG("%s eye: viewport=%p texture=%p", kEyeNames[index], vp,
            eye.texture);
        return false;
    }

    // Entities belong to the main view's bit, so the eyes share it. The
    // build hook keeps the per-frame draw lists alive across views.
    uint32_t viewBit = Read<uint32_t>(mainVp, off::VpViewBit);
    render_hook::SetEyeCamera(index, eye.camera);

    // Plain-value settings only; handles live below 0x44.
    memcpy(vp + off::VpSettingsBegin, mainVp + off::VpSettingsBegin,
           off::VpSettingsEnd - off::VpSettingsBegin);
    Write<uint64_t>(vp, off::VpCamera, eye.camera);
    Write<uint32_t>(vp, off::VpPipeline, off::PipelineScene);
    Write<float[2]>(vp, off::VpRectOrigin, {0.0f, 0.0f});
    Write<float[2]>(vp, off::VpRectSize, {1.0f, 1.0f});
    Write<uint32_t>(vp, off::VpViewBit, viewBit);
    Write<uint8_t*>(vp, off::VpOutput, eye.texture);
    Write<uint8_t>(vp, off::VpKeepHistory, 1);
    Write<uint8_t>(vp, off::VpEnabled, 1);

    g_eyeTextures[index] = eye.texture;
    LOG("%s eye created: %ux%u cam=0x%llx vp=0x%llx bit=0x%x tex=%p",
        kEyeNames[index], eye.width, eye.height,
        static_cast<unsigned long long>(eye.camera),
        static_cast<unsigned long long>(eye.viewport), viewBit, eye.texture);
    return true;
}

// Exposure, white balance and fog live in the viewport settings and
// the game updates them on the main viewport every frame.
void SyncSettings(uint8_t* vp) {
    uint8_t* mainVp = MainViewport();
    if (!mainVp) {
        return;
    }
    memcpy(vp + off::VpSettingsBegin, mainVp + off::VpSettingsBegin,
           off::VpSettingsEnd - off::VpSettingsBegin);
}

// Hamilton product a * b, quaternions as x, y, z, w.
void QuatMul(const float a[4], const float b[4], float out[4]) {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

Vec3 Rotate(const float q[4], const float v[3]) {
    float p[4] = {v[0], v[1], v[2], 0.0f};
    float inv[4] = {-q[0], -q[1], -q[2], q[3]};
    float t[4], r[4];
    QuatMul(q, p, t);
    QuatMul(t, inv, r);
    return {r[0], r[1], r[2]};
}

// With a headset the game thread writes only the base camera; the
// head pose is added at render time (ApplyHeadPose). Without one,
// parallel eyes shifted by +-IPD/2, offset in the fraction.
void WriteEyeCamera(EyeView& eye, uint8_t* cam, const uint8_t* camData,
                    float sideOffset) {
    float quat[4];
    memcpy(quat, camData + off::CdQuat, sizeof(quat));
    Vec3 offset = RightVector(quat);
    offset = {offset.x * sideOffset, offset.y * sideOffset,
              offset.z * sideOffset};
    float fov = Read<float>(camData, off::CdFov);
    if (xr::GetState() == xr::State::Ready) {
        offset = {0.0f, 0.0f, 0.0f};
        fov = 2.0f * std::atan(xr::GetEyeTarget().tanHalfX);
    }
    float frac[4];
    memcpy(frac, camData + off::CdFrac, sizeof(frac));
    frac[0] += offset.x;
    frac[1] += offset.y;
    frac[2] += offset.z;

    memcpy(cam + off::CamPos, camData + off::CdPos, 16);
    memcpy(cam + off::CamFrac, frac, 16);
    memcpy(cam + off::CamQuat, quat, 16);
    Write(cam, off::CamFov, fov);
    Write(cam, off::CamNear, Read<float>(camData, off::CdNear));
    Write(cam, off::CamFar, Read<float>(camData, off::CdFar));
    memcpy(eye.lastCamPos, cam + off::CamPos, 16);
    eye.camWritten = true;
}

void TickEye(int index, const uint8_t* camData) {
    EyeView& eye = g_eyes[index];
    xr::State xrState = xr::GetState();
    if (eye.viewport == InvalidHandle) {
        // A headset is connecting: wait for its eye size.
        if (xrState == xr::State::Starting) {
            return;
        }
        if (eye.cooldown > 0) {
            --eye.cooldown;
        } else if (!eye.disabled && !CreateEye(index)) {
            eye.cooldown = kRecreateCooldown;
        }
        return;
    }
    uint8_t* vp = OwnedViewport(eye);
    if (!vp) {
        DropEye(index, "viewport no longer ours");
        return;
    }
    uint8_t* cam = OwnedCamera(eye);
    if (!cam) {
        DropEye(index, "camera no longer ours");
        return;
    }
    // Eyes made before the headset appeared get rebuilt at its size;
    // the old view is switched off so it stops rendering.
    if (xrState == xr::State::Ready) {
        xr::EyeTarget target = xr::GetEyeTarget();
        if (target.width != eye.width || target.height != eye.height) {
            Write<uint8_t>(vp, off::VpEnabled, 0);
            LOG("%s eye: resizing to %ux%u", kEyeNames[index], target.width,
                target.height);
            g_eyeTextures[index] = nullptr;
            render_hook::SetEyeCamera(index, InvalidHandle);
            eye = EyeView{};
            return;
        }
    }
    float side = index == kLeftEye ? -0.5f * g_ipd : 0.5f * g_ipd;
    WriteEyeCamera(eye, cam, camData, side);
    SyncSettings(vp);
}

void HookedUpdate(void* self, uint8_t* camData, float dt) {
    g_origUpdate(self, camData, dt);
    g_lastDriveTick = GetTickCount64();
    // The owner finalizes after us; do it now so values are current.
    At<FinalizeFn>(rva::CameraDataFinalize)(camData, true);

    // Eyes only render correctly with the build hook in place.
    if (!g_buildHooked) {
        if (g_buildHookFailed) {
            return;
        }
        g_buildHooked = render_hook::InstallSceneBuildHook();
        g_buildHookFailed = !g_buildHooked;
        if (!g_buildHooked) {
            return;
        }
    }
    for (int i = 0; i < kRenderedEyes; ++i) {
        TickEye(i, camData);
    }
    if (++g_frame % 600 == 0) {
        float pos[3];
        memcpy(pos, camData + off::CdPos, sizeof(pos));
        LOG("frame %u cam (%.2f, %.2f, %.2f) fov %.3f renders L %u R %u",
            g_frame, pos[0], pos[1], pos[2],
            Read<float>(camData, off::CdFov), EyeRenderCount(kLeftEye),
            EyeRenderCount(kRightEye));
    }
}

}  // namespace

bool InstallCameraHook() {
    auto slot = At<void**>(rva::VehicleCtrlVtblUpdate);
    void* previous = nullptr;
    if (!PatchPointer(slot, reinterpret_cast<void*>(&HookedUpdate),
                      &previous)) {
        LOG("camera hook: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    g_origUpdate = reinterpret_cast<CtrlUpdateFn>(previous);
    LOG("camera hook installed (orig %p), ipd %.3f", previous, g_ipd);
    return true;
}

float Ipd() {
    return g_ipd;
}

bool IsDriving() {
    return GetTickCount64() - g_lastDriveTick.load() < kDrivingTimeoutMs;
}

void ApplyHeadPose(int eye) {
    uint64_t handle = g_eyes[eye].camera;
    xr::Pose pose;
    if (handle == InvalidHandle || !xr::LocateForRender(eye, &pose)) {
        return;
    }
    // The render thread reads the table's +0x18 copy.
    uint8_t* copy = Read<uint8_t*>(At<void*>(rva::SceneCameraTable), 0x18);
    uint8_t* cam = Read<uint8_t**>(copy, 0)[static_cast<uint32_t>(handle)];
    if (!cam) {
        return;
    }
    // An object still holding our last result was not rewritten by
    // the game thread; composing again would stack the head pose.
    AppliedPose& last = g_applied[eye];
    if (last.cam == cam &&
        memcmp(cam + off::CamFrac, last.frac, sizeof(last.frac)) == 0 &&
        memcmp(cam + off::CamQuat, last.quat, sizeof(last.quat)) == 0) {
        return;
    }
    float base[4];
    memcpy(base, cam + off::CamQuat, sizeof(base));
    float quat[4];
    QuatMul(base, pose.orientation, quat);
    Vec3 offset = Rotate(base, pose.position);
    float frac[4];
    memcpy(frac, cam + off::CamFrac, sizeof(frac));
    frac[0] += offset.x;
    frac[1] += offset.y;
    frac[2] += offset.z;
    memcpy(cam + off::CamFrac, frac, sizeof(frac));
    memcpy(cam + off::CamQuat, quat, sizeof(quat));
    last.cam = cam;
    memcpy(last.frac, frac, sizeof(frac));
    memcpy(last.quat, quat, sizeof(quat));
}

ID3D11Texture2D* EyeTexture(int eye) {
    uint8_t* texture = g_eyeTextures[eye].load();
    if (!texture) {
        return nullptr;
    }
    uint8_t* hal = Read<uint8_t*>(texture, off::TexHal);
    if (!hal) {
        return nullptr;
    }
    return Read<ID3D11Texture2D*>(hal, off::HalD3DTexture);
}

uint32_t EyeRenderCount(int eye) {
    uint8_t* texture = g_eyeTextures[eye].load();
    return texture ? Read<uint32_t>(texture, off::TexRenderCount) : 0;
}

}  // namespace tc2vr::stereo
