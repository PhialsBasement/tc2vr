#pragma once

#include <cstdint>

struct ID3D11Texture2D;

namespace tc2vr::stereo {

constexpr int kEyeCount = 2;
constexpr int kLeftEye = 0;
constexpr int kRightEye = 1;
constexpr int kRenderedEyes = 2;

constexpr uint32_t kEyeWidth = 1280;
constexpr uint32_t kEyeHeight = 720;
constexpr float kDefaultIpd = 0.064f;

bool InstallCameraHook();
float Ipd();

// True while the vehicle camera is updating (the player is driving).
bool IsDriving();

// Render thread, just before an eye's build: composes the headset
// pose for this frame onto that eye's camera.
void ApplyHeadPose(int eye);

// Null until that eye's view exists and its GPU texture is created.
ID3D11Texture2D* EyeTexture(int eye);
uint32_t EyeRenderCount(int eye);

}  // namespace tc2vr::stereo
