#pragma once

namespace tc2vr::ring_fix {

// Replaces the GPU ring suballocator (FUN_1420d33e0) with a native
// copy that skips the protector VM detour on its buffer-map path.
// Call on the render thread while no ring allocation is running.
bool Install();

}  // namespace tc2vr::ring_fix
