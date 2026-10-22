#pragma once

#include <cstdint>

namespace tc2vr::render_hook {

// Wraps the full-scene build and execute so extra views see the same
// per-frame state as the first one and eyes build as secondary views.
bool InstallSceneBuildHook();

// Logs per-view CPU averages over the given frame count and resets.
void ReportTimings(uint32_t frames);

// Eye viewports are recognised by their scene camera handle.
void SetEyeCamera(int eye, uint64_t cameraHandle);

}  // namespace tc2vr::render_hook
