#include "game.hpp"

#include <windows.h>

#include "log.hpp"

namespace tc2vr::game {
namespace {

struct Signature {
    const char* name;
    uintptr_t rva;
    uint8_t bytes[16];
};

// First 16 bytes of each function, taken from the decrypted image.
constexpr Signature kSignatures[] = {
    {"VehicleCtrlUpdate", rva::VehicleCtrlUpdate,
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83,
      0xec, 0x30, 0x0f, 0x29, 0x74, 0x24, 0x20, 0x0f}},
    {"CameraDataFinalize", rva::CameraDataFinalize,
     {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x57, 0x48, 0x8d,
      0x68, 0xc8, 0x48, 0x81, 0xec, 0x20, 0x01, 0x00}},
    {"SceneHandleResolve", rva::SceneHandleResolve,
     {0x48, 0x8b, 0x49, 0x10, 0x44, 0x8b, 0x02, 0x48,
      0x8b, 0x01, 0x4a, 0x8b, 0x04, 0xc0, 0xc3, 0xcc}},
    {"SceneCameraCreate", rva::SceneCameraCreate,
     {0x40, 0x53, 0x57, 0x48, 0x83, 0xec, 0x38, 0x48,
      0x89, 0x6c, 0x24, 0x50, 0x48, 0x8b, 0xfa, 0x48}},
    {"SceneCameraResolveForWrite", rva::SceneCameraResolveForWrite,
     {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c,
      0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48}},
    {"SceneViewportExCreate", rva::SceneViewportExCreate,
     {0x40, 0x53, 0x57, 0x48, 0x83, 0xec, 0x38, 0x48,
      0x89, 0x6c, 0x24, 0x50, 0x48, 0x8b, 0xfa, 0x48}},
    {"SceneViewportExResolveForWrite", rva::SceneViewportExResolveForWrite,
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
      0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48}},
    {"ViewBitAlloc", rva::ViewBitAlloc,
     {0x8b, 0x15, 0x7a, 0x28, 0x1c, 0x01, 0x4c, 0x8b,
      0xc1, 0x33, 0xc9, 0x41, 0x89, 0x08, 0x66, 0x90}},
    {"RenderTargetMgrCreate", rva::RenderTargetMgrCreate,
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
      0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48}},
    {"RenderSchedulerFrame", rva::RenderSchedulerFrame,
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
      0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x50, 0x0f}},
    {"SceneBuild", rva::SceneBuild,
     {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41,
      0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48}},
};

}  // namespace

uintptr_t Base() {
    static const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return base;
}

bool VerifyBuild() {
    bool ok = true;
    for (const Signature& sig : kSignatures) {
        const void* code = reinterpret_cast<const void*>(Base() + sig.rva);
        if (memcmp(code, sig.bytes, sizeof(sig.bytes)) != 0) {
            LOG("signature mismatch: %s at rva 0x%llx", sig.name,
                static_cast<unsigned long long>(sig.rva));
            ok = false;
        }
    }
    uintptr_t slot = *At<uintptr_t*>(rva::VehicleCtrlVtblUpdate);
    if (slot != Base() + rva::VehicleCtrlUpdate) {
        LOG("vehicle controller vtable slot unexpected: 0x%llx",
            static_cast<unsigned long long>(slot));
        ok = false;
    }
    return ok;
}

uint8_t* GameObject() {
    return *At<uint8_t**>(rva::Game);
}

void* Component(size_t slot) {
    uint8_t* gameObj = GameObject();
    if (!gameObj) {
        return nullptr;
    }
    auto components = Read<void**>(gameObj, off::GameComponents);
    auto index = Read<uint8_t*>(gameObj, off::GameComponentIndex);
    if (!components || !index) {
        return nullptr;
    }
    return components[index[slot]];
}

uint8_t* ResolveViewport(uint64_t handle) {
    auto resolve = At<ResolveFn>(rva::SceneHandleResolve);
    return resolve(At<void*>(rva::SceneViewportTable), &handle);
}

uint8_t* ResolveCamera(uint64_t handle) {
    auto resolve = At<ResolveFn>(rva::SceneHandleResolve);
    return resolve(At<void*>(rva::SceneCameraTable), &handle);
}

}  // namespace tc2vr::game
