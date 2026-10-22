#include "present.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#include <cstdio>

#include "log.hpp"
#include "mirror.hpp"
#include "render_hook.hpp"
#include "stereo.hpp"
#include "xr.hpp"

namespace tc2vr::present {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using CreateDeviceFn = decltype(&D3D11CreateDeviceAndSwapChain);

constexpr size_t kPresentSlot = 8;
constexpr uint32_t kDumpAfterRenders = 30;
constexpr int kDumpKey = VK_F9;

PresentFn g_origPresent = nullptr;
bool g_dumped = false;
bool g_keyWasDown = false;
int g_dumpIndex = 0;
constexpr const char* kDumpTrigger = "tc2vr_dump.trigger";
constexpr uint32_t kTriggerPollFrames = 30;
uint32_t g_presentCount = 0;
constexpr int kMirrorKey = VK_F10;
bool g_mirrorOn = true;
bool g_mirrorKeyWasDown = false;
constexpr uint32_t kTimingFrames = 120;

bool SaveBmp(const char* path, const uint8_t* bgra, UINT width, UINT height,
             UINT pitch) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + width * height * 4;
    fwrite(&file, sizeof(file), 1, f);
    fwrite(&info, sizeof(info), 1, f);
    for (UINT y = 0; y < height; ++y) {
        fwrite(bgra + y * pitch, 4, width, f);
    }
    fclose(f);
    return true;
}

void DumpTexture(ID3D11Texture2D* source, const char* path) {
    ID3D11Device* device = nullptr;
    source->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    D3D11_TEXTURE2D_DESC desc;
    source->GetDesc(&desc);
    LOG("%s: %ux%u format %u mips %u bind 0x%x", path, desc.Width,
        desc.Height, desc.Format, desc.MipLevels, desc.BindFlags);

    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc = {1, 0};
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        context->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0,
                                       nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            bool ok = SaveBmp(path,
                              static_cast<const uint8_t*>(mapped.pData),
                              desc.Width, desc.Height, mapped.RowPitch);
            context->Unmap(staging, 0);
            LOG("%s: %s", path, ok ? "written" : "write failed");
        }
        staging->Release();
    }
    context->Release();
    device->Release();
}

void DumpBackBuffer(IDXGISwapChain* swapChain, const char* path) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void**>(&back)))) {
        DumpTexture(back, path);
        back->Release();
    }
}

void DumpAll(IDXGISwapChain* swapChain) {
    constexpr const char* kTags[stereo::kEyeCount] = {"eyeL", "eyeR"};
    char path[64];
    for (int eye = 0; eye < stereo::kEyeCount; ++eye) {
        if (ID3D11Texture2D* texture = stereo::EyeTexture(eye)) {
            snprintf(path, sizeof(path), "tc2vr_%s_%02d.bmp", kTags[eye],
                     g_dumpIndex);
            DumpTexture(texture, path);
        }
    }
    snprintf(path, sizeof(path), "tc2vr_back_%02d.bmp", g_dumpIndex);
    DumpBackBuffer(swapChain, path);
    ++g_dumpIndex;
}

bool EyesReady() {
    for (int eye = 0; eye < stereo::kRenderedEyes; ++eye) {
        if (stereo::EyeRenderCount(eye) < kDumpAfterRenders) {
            return false;
        }
    }
    return true;
}

// Frame interval and time blocked inside Present (GPU back-pressure).
void TrackFrame(const LARGE_INTEGER& start, const LARGE_INTEGER& end) {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    static LARGE_INTEGER lastEnd{};
    static double frameMs = 0.0;
    static double presentMs = 0.0;
    static uint32_t frames = 0;
    double toMs = 1000.0 / double(freq.QuadPart);
    if (lastEnd.QuadPart) {
        frameMs += double(end.QuadPart - lastEnd.QuadPart) * toMs;
        presentMs += double(end.QuadPart - start.QuadPart) * toMs;
        ++frames;
    }
    lastEnd = end;
    if (frames == kTimingFrames) {
        LOG("frame %.2f ms (%.1f fps), in Present %.2f ms",
            frameMs / frames, 1000.0 * frames / frameMs, presentMs / frames);
        render_hook::ReportTimings(frames);
        frameMs = presentMs = 0.0;
        frames = 0;
    }
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain,
                                        UINT syncInterval, UINT flags) {
    if (!g_dumped && EyesReady()) {
        g_dumped = true;
        DumpAll(swapChain);
    }
    bool keyDown = (GetAsyncKeyState(kDumpKey) & 0x8000) != 0;
    if (keyDown && !g_keyWasDown) {
        DumpAll(swapChain);
    }
    g_keyWasDown = keyDown;
    // A trigger file lets tooling request dumps without a key press.
    if (++g_presentCount % kTriggerPollFrames == 0 &&
        DeleteFileA(kDumpTrigger)) {
        DumpAll(swapChain);
    }
    // The eye renders and the back buffer are complete at this point.
    ID3D11Texture2D* eyes[2] = {stereo::EyeTexture(stereo::kLeftEye),
                                stereo::EyeTexture(stereo::kRightEye)};
    bool driving = stereo::IsDriving();
    xr::SetScreenMode(!driving);
    xr::OnPresent(swapChain, eyes);
    // Desktop mirror of the left eye while driving; F10 toggles it.
    bool mirrorKey = (GetAsyncKeyState(kMirrorKey) & 0x8000) != 0;
    if (mirrorKey && !g_mirrorKeyWasDown) {
        g_mirrorOn = !g_mirrorOn;
        LOG("desktop mirror %s", g_mirrorOn ? "on" : "off");
    }
    g_mirrorKeyWasDown = mirrorKey;
    if (g_mirrorOn && driving) {
        mirror::Draw(swapChain, eyes[stereo::kLeftEye]);
    }
    LARGE_INTEGER presentStart;
    QueryPerformanceCounter(&presentStart);
    HRESULT hr = g_origPresent(swapChain, syncInterval, flags);
    LARGE_INTEGER presentEnd;
    QueryPerformanceCounter(&presentEnd);
    TrackFrame(presentStart, presentEnd);
    return hr;
}

void** SwapChainVtable() {
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    auto create = reinterpret_cast<CreateDeviceFn>(
        GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (!create) {
        return nullptr;
    }
    HWND window = CreateWindowExW(0, L"STATIC", L"tc2vr", WS_POPUP, 0, 0, 16,
                                  16, nullptr, nullptr, nullptr, nullptr);
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                        nullptr, 0, D3D11_SDK_VERSION, &desc, &swapChain,
                        &device, nullptr, &context);
    void** vtable = nullptr;
    if (SUCCEEDED(hr)) {
        vtable = *reinterpret_cast<void***>(swapChain);
        swapChain->Release();
        context->Release();
        device->Release();
    } else {
        LOG("dummy swapchain failed: 0x%lx", static_cast<unsigned long>(hr));
    }
    DestroyWindow(window);
    return vtable;
}

}  // namespace

bool InstallPresentHook() {
    void** vtable = SwapChainVtable();
    if (!vtable) {
        return false;
    }
    void** slot = &vtable[kPresentSlot];
    DWORD oldProtect;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        LOG("present hook: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    g_origPresent = reinterpret_cast<PresentFn>(*slot);
    *slot = reinterpret_cast<void*>(&HookedPresent);
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    LOG("present hook installed (orig %p)",
        reinterpret_cast<void*>(g_origPresent));
    return true;
}

}  // namespace tc2vr::present
