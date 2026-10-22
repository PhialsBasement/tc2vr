#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11Texture2D;
struct IDXGISwapChain;

namespace tc2vr::xr {

enum class State {
    NotStarted,  // waiting for the first Present
    Unavailable, // no runtime or no headset: desktop-only mode
    Starting,    // session created, waiting for view parameters
    Ready,       // eye size, FOV and poses available
};

State GetState();

// Render-target size and symmetric FOV (tangents of the half angles)
// that cover both eyes. Valid once the state is Ready.
struct EyeTarget {
    uint32_t width;
    uint32_t height;
    float tanHalfX;
    float tanHalfY;
};
EyeTarget GetEyeTarget();

// Head-relative eye pose in metres, in engine camera axes (+X right,
// +Y forward, +Z up). Located for the frame being rendered now and
// recorded as the pose submitted with it. Render thread only.
struct Pose {
    float position[3];
    float orientation[4];  // x, y, z, w
};
bool LocateForRender(int eye, Pose* out);

// Whether the curved screen is shown instead of the eyes.
void SetScreenMode(bool screen);

// Called from the Present hook, before the real Present.
void OnPresent(IDXGISwapChain* swapChain, ID3D11Texture2D* const eyes[2]);

}  // namespace tc2vr::xr
