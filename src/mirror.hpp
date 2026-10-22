#pragma once

struct IDXGISwapChain;
struct ID3D11Texture2D;

namespace tc2vr::mirror {

// Draws the eye image over the back buffer, scaled and cropped to the
// window's aspect, so the desktop (and recordings) show the VR view.
void Draw(IDXGISwapChain* swapChain, ID3D11Texture2D* eye);

}  // namespace tc2vr::mirror
