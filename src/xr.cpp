#include "xr.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "log.hpp"

namespace tc2vr::xr {
namespace {

constexpr float kScreenRadius = 2.5f;
constexpr float kScreenArc = 1.2f;  // radians, about 69 degrees
constexpr uint32_t kRetryPresents = 300;

#define XR_FUNCTIONS(X)                       \
    X(xrEnumerateInstanceExtensionProperties) \
    X(xrCreateInstance)                       \
    X(xrDestroyInstance)                      \
    X(xrGetSystem)                            \
    X(xrGetD3D11GraphicsRequirementsKHR)      \
    X(xrEnumerateViewConfigurationViews)      \
    X(xrCreateSession)                        \
    X(xrDestroySession)                       \
    X(xrBeginSession)                         \
    X(xrEndSession)                           \
    X(xrCreateReferenceSpace)                 \
    X(xrDestroySpace)                         \
    X(xrEnumerateSwapchainFormats)            \
    X(xrCreateSwapchain)                      \
    X(xrDestroySwapchain)                     \
    X(xrEnumerateSwapchainImages)             \
    X(xrAcquireSwapchainImage)                \
    X(xrWaitSwapchainImage)                   \
    X(xrReleaseSwapchainImage)                \
    X(xrPollEvent)                            \
    X(xrWaitFrame)                            \
    X(xrBeginFrame)                           \
    X(xrEndFrame)                             \
    X(xrLocateViews)

#define XR_DECLARE(name) PFN_##name name = nullptr;
XR_FUNCTIONS(XR_DECLARE)
#undef XR_DECLARE

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<ID3D11Texture2D*> images;
};

PFN_xrGetInstanceProcAddr g_getProc = nullptr;
XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_space = XR_NULL_HANDLE;
bool g_hasCylinder = false;
bool g_running = false;
uint32_t g_recommendedWidth = 0;
int64_t g_format = 0;
Swapchain g_eyeChains[2];
Swapchain g_screenChain;
uint32_t g_retryCountdown = 0;

std::atomic<State> g_state{State::NotStarted};
std::atomic<bool> g_screenMode{true};

// Latest located views, and the ones handed to the game for rendering.
std::mutex g_poseMutex;
XrView g_rendered[2];
EyeTarget g_target{};

// Display time of the frame being rendered now: the last submitted
// frame's time plus one display period.
XrTime g_nextDisplayTime = 0;
uint64_t g_presentFrame = 0;
uint64_t g_locatedFrame = ~0ull;

bool Check(XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    LOG("xr: %s failed (%d)", what, static_cast<int>(result));
    return false;
}

std::wstring ReadActiveRuntimeManifest() {
    wchar_t path[MAX_PATH];
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1",
                     L"ActiveRuntime", RRF_RT_REG_SZ, nullptr, path,
                     &size) != ERROR_SUCCESS) {
        return {};
    }
    return path;
}

// Pulls "library_path" out of the runtime manifest JSON.
std::wstring RuntimeLibraryPath(const std::wstring& manifest) {
    FILE* f = _wfopen(manifest.c_str(), L"rb");
    if (!f) {
        return {};
    }
    std::string json;
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        json.append(buffer, n);
    }
    fclose(f);
    size_t key = json.find("\"library_path\"");
    size_t open = key == std::string::npos ? key : json.find('"', key + 14);
    size_t close = open == std::string::npos ? open : json.find('"', open + 1);
    if (close == std::string::npos) {
        return {};
    }
    std::string value;
    for (size_t i = open + 1; i < close; ++i) {
        if (json[i] == '\\' && i + 1 < close) {
            ++i;
        }
        value += json[i];
    }
    std::wstring lib(value.begin(), value.end());
    bool absolute = lib.size() > 1 && (lib[1] == L':' || lib[0] == L'\\');
    if (!absolute) {
        size_t slash = manifest.find_last_of(L"\\/");
        lib = manifest.substr(0, slash + 1) + lib;
    }
    return lib;
}

// The official Khronos loader shipped next to the game exe. It
// finds the runtime itself and enables any installed API layers.
bool LoadOfficialLoader() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, len);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    HMODULE module = LoadLibraryW((dir + L"openxr_loader.dll").c_str());
    if (!module) {
        return false;
    }
    g_getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
        GetProcAddress(module, "xrGetInstanceProcAddr"));
    LOG("xr: using openxr_loader.dll%s",
        g_getProc ? "" : " (no xrGetInstanceProcAddr export)");
    return g_getProc != nullptr;
}

bool LoadRuntime() {
    if (LoadOfficialLoader()) {
        return true;
    }
    std::wstring manifest = ReadActiveRuntimeManifest();
    if (manifest.empty()) {
        LOG("xr: no openxr_loader.dll and no active runtime registered");
        return false;
    }
    std::wstring lib = RuntimeLibraryPath(manifest);
    HMODULE module = lib.empty() ? nullptr : LoadLibraryW(lib.c_str());
    if (!module) {
        LOG("xr: cannot load runtime library (%lu)", GetLastError());
        return false;
    }
    auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        GetProcAddress(module, "xrNegotiateLoaderRuntimeInterface"));
    if (!negotiate) {
        LOG("xr: runtime has no negotiation entry point");
        return false;
    }
    XrNegotiateLoaderInfo info{};
    info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    info.structSize = sizeof(info);
    info.minInterfaceVersion = 1;
    info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest request{};
    request.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    request.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    request.structSize = sizeof(request);
    if (!Check(negotiate(&info, &request), "runtime negotiation")) {
        return false;
    }
    g_getProc = request.getInstanceProcAddr;
    return g_getProc != nullptr;
}

template <typename Fn>
void Resolve(XrInstance instance, const char* name, Fn& fn) {
    PFN_xrVoidFunction raw = nullptr;
    g_getProc(instance, name, &raw);
    fn = reinterpret_cast<Fn>(raw);
}

bool CreateInstance() {
    Resolve(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
            xrEnumerateInstanceExtensionProperties);
    Resolve(XR_NULL_HANDLE, "xrCreateInstance", xrCreateInstance);
    uint32_t count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    std::vector<XrExtensionProperties> props(
        count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count,
                                           props.data());
    bool hasD3D11 = false;
    for (const auto& p : props) {
        hasD3D11 |= !strcmp(p.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        g_hasCylinder |= !strcmp(p.extensionName,
                                 XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    }
    if (!hasD3D11) {
        LOG("xr: runtime lacks XR_KHR_D3D11_enable");
        return false;
    }
    std::vector<const char*> exts = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    if (g_hasCylinder) {
        exts.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    }
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(create.applicationInfo.applicationName, "TheCrew2 VR");
    strcpy(create.applicationInfo.engineName, "tc2vr");
    create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    create.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    create.enabledExtensionNames = exts.data();
    if (!Check(xrCreateInstance(&create, &g_instance), "xrCreateInstance")) {
        return false;
    }
#define XR_RESOLVE(name) Resolve(g_instance, #name, name);
    XR_FUNCTIONS(XR_RESOLVE)
#undef XR_RESOLVE
    LOG("xr: instance created (cylinder layer %s)",
        g_hasCylinder ? "available" : "unavailable, using quad");
    return true;
}

bool GetSystem() {
    XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
    info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult result = xrGetSystem(g_instance, &info, &g_system);
    if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        return false;  // headset not connected yet; retried later
    }
    if (!Check(result, "xrGetSystem")) {
        return false;
    }
    uint32_t count = 0;
    xrEnumerateViewConfigurationViews(g_instance, g_system,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      0, &count, nullptr);
    std::vector<XrViewConfigurationView> views(
        count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(g_instance, g_system,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      count, &count, views.data());
    if (count < 2) {
        return false;
    }
    g_recommendedWidth = views[0].recommendedImageRectWidth;
    LOG("xr: headset found, recommended %ux%u per eye",
        views[0].recommendedImageRectWidth,
        views[0].recommendedImageRectHeight);
    return true;
}

bool CreateSession(ID3D11Device* device) {
    XrGraphicsRequirementsD3D11KHR requirements{
        XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Check(xrGetD3D11GraphicsRequirementsKHR(g_instance, g_system,
                                                 &requirements),
               "graphics requirements")) {
        return false;
    }
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};
    create.next = &binding;
    create.systemId = g_system;
    if (!Check(xrCreateSession(g_instance, &create, &g_session),
               "xrCreateSession")) {
        return false;
    }
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(xrCreateReferenceSpace(g_session, &space, &g_space),
               "reference space")) {
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(g_session, count, &count, formats.data());
    for (int64_t f : formats) {
        if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            g_format = f;
        }
    }
    if (!g_format) {
        LOG("xr: runtime offers no B8G8R8A8_UNORM_SRGB swapchains");
        return false;
    }
    LOG("xr: session created");
    return true;
}

bool CreateSwapchain(Swapchain& chain, uint32_t width, uint32_t height) {
    XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    create.format = g_format;
    create.sampleCount = 1;
    create.width = width;
    create.height = height;
    create.faceCount = 1;
    create.arraySize = 1;
    create.mipCount = 1;
    if (!Check(xrCreateSwapchain(g_session, &create, &chain.handle),
               "xrCreateSwapchain")) {
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(chain.handle, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(
        count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(
        chain.handle, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    for (const auto& image : images) {
        chain.images.push_back(image.texture);
    }
    chain.width = width;
    chain.height = height;
    return true;
}

void DestroySwapchain(Swapchain& chain) {
    if (chain.handle) {
        xrDestroySwapchain(chain.handle);
    }
    chain = Swapchain{};
}

void Teardown(const char* reason) {
    LOG("xr: shutting down (%s)", reason);
    DestroySwapchain(g_eyeChains[0]);
    DestroySwapchain(g_eyeChains[1]);
    DestroySwapchain(g_screenChain);
    if (g_space) {
        xrDestroySpace(g_space);
    }
    if (g_session) {
        xrDestroySession(g_session);
    }
    if (g_instance) {
        xrDestroyInstance(g_instance);
    }
    g_space = XR_NULL_HANDLE;
    g_session = XR_NULL_HANDLE;
    g_instance = XR_NULL_HANDLE;
    g_running = false;
    g_state = State::Unavailable;
}

void PollEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (g_instance && xrPollEvent(g_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto& changed =
                reinterpret_cast<XrEventDataSessionStateChanged&>(event);
            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g_running = Check(xrBeginSession(g_session, &begin),
                                  "xrBeginSession");
            } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_session);
                g_running = false;
            } else if (changed.state == XR_SESSION_STATE_EXITING ||
                       changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                Teardown("session ended by runtime");
                return;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            Teardown("instance lost");
            return;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void Start(IDXGISwapChain* swapChain) {
    if (!g_getProc && !LoadRuntime()) {
        g_state = State::Unavailable;
        return;
    }
    if (!g_instance && !CreateInstance()) {
        g_state = State::Unavailable;
        return;
    }
    if (!GetSystem()) {
        g_retryCountdown = kRetryPresents;  // no headset yet
        return;
    }
    ID3D11Device* device = nullptr;
    swapChain->GetDevice(__uuidof(ID3D11Device),
                         reinterpret_cast<void**>(&device));
    bool ok = device && CreateSession(device);
    if (device) {
        device->Release();
    }
    if (!ok) {
        Teardown("session setup failed");
        return;
    }
    g_state = State::Starting;
}

// Symmetric FOV covering both eyes, and a render target to match.
void SetupEyes(const XrView views[2]) {
    float tanX = 0.0f;
    float tanY = 0.0f;
    for (int i = 0; i < 2; ++i) {
        const XrFovf& f = views[i].fov;
        tanX = std::fmax(tanX, std::fmax(std::tan(-f.angleLeft),
                                         std::tan(f.angleRight)));
        tanY = std::fmax(tanY, std::fmax(std::tan(f.angleUp),
                                         std::tan(-f.angleDown)));
    }
    uint32_t width = g_recommendedWidth;
    auto height = static_cast<uint32_t>(std::lround(width * tanY / tanX));
    for (auto& chain : g_eyeChains) {
        if (!CreateSwapchain(chain, width, height)) {
            Teardown("eye swapchains");
            return;
        }
    }
    {
        std::lock_guard lock(g_poseMutex);
        g_target = {width, height, tanX, tanY};
    }
    LOG("xr: eye targets %ux%u, fov %.1f x %.1f deg", width, height,
        2.0 * std::atan(tanX) * 57.2958, 2.0 * std::atan(tanY) * 57.2958);
    g_state = State::Ready;
}

void CopyInto(Swapchain& chain, ID3D11DeviceContext* context,
              ID3D11Texture2D* source) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(chain.handle, &acquire,
                                              &index))) {
        return;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(chain.handle, &wait);
    D3D11_TEXTURE2D_DESC desc;
    source->GetDesc(&desc);
    D3D11_BOX box{0, 0, 0, std::min(desc.Width, chain.width),
                  std::min(desc.Height, chain.height), 1};
    context->CopySubresourceRegion(chain.images[index], 0, 0, 0, 0, source,
                                   0, &box);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(chain.handle, &release);
}

XrFovf RenderedFov() {
    float x = std::atan(g_target.tanHalfX);
    float y = std::atan(g_target.tanHalfY);
    return {-x, x, y, -y};
}

void SubmitFrame(IDXGISwapChain* swapChain, ID3D11Texture2D* const eyes[2]) {
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    if (!Check(xrWaitFrame(g_session, &waitInfo, &frame), "xrWaitFrame")) {
        return;
    }
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    xrBeginFrame(g_session, &beginInfo);

    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = frame.predictedDisplayTime;
    locate.space = g_space;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t viewCount = 0;
    xrLocateViews(g_session, &locate, &viewState, 2, &viewCount, views);
    bool located = viewCount == 2 &&
                   (viewState.viewStateFlags &
                    XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    if (g_state == State::Starting && located) {
        {
            std::lock_guard lock(g_poseMutex);
            g_rendered[0] = views[0];
            g_rendered[1] = views[1];
        }
        SetupEyes(views);
    }

    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjectionView projViews[2];
    XrCompositionLayerProjection projection{
        XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerCylinderKHR cylinder{
        XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    swapChain->GetDevice(__uuidof(ID3D11Device),
                         reinterpret_cast<void**>(&device));
    if (device) {
        device->GetImmediateContext(&context);
    }

    bool screen = g_screenMode || !eyes[0] || !eyes[1];
    if (frame.shouldRender && context && g_state == State::Ready) {
        if (screen) {
            ID3D11Texture2D* back = nullptr;
            swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                 reinterpret_cast<void**>(&back));
            if (back) {
                D3D11_TEXTURE2D_DESC desc;
                back->GetDesc(&desc);
                if (!g_screenChain.handle) {
                    CreateSwapchain(g_screenChain, desc.Width, desc.Height);
                }
                if (g_screenChain.handle) {
                    CopyInto(g_screenChain, context, back);
                    XrSwapchainSubImage sub{};
                    sub.swapchain = g_screenChain.handle;
                    sub.imageRect.extent = {
                        static_cast<int32_t>(g_screenChain.width),
                        static_cast<int32_t>(g_screenChain.height)};
                    float aspect = float(g_screenChain.width) /
                                   float(g_screenChain.height);
                    if (g_hasCylinder) {
                        cylinder.space = g_space;
                        cylinder.subImage = sub;
                        cylinder.pose.orientation.w = 1.0f;
                        cylinder.radius = kScreenRadius;
                        cylinder.centralAngle = kScreenArc;
                        cylinder.aspectRatio = aspect;
                        layers.push_back(
                            reinterpret_cast<XrCompositionLayerBaseHeader*>(
                                &cylinder));
                    } else {
                        float w = kScreenRadius * kScreenArc;
                        quad.space = g_space;
                        quad.subImage = sub;
                        quad.pose.orientation.w = 1.0f;
                        quad.pose.position.z = -kScreenRadius;
                        quad.size = {w, w / aspect};
                        layers.push_back(
                            reinterpret_cast<XrCompositionLayerBaseHeader*>(
                                &quad));
                    }
                }
                back->Release();
            }
        } else {
            XrView rendered[2];
            {
                std::lock_guard lock(g_poseMutex);
                rendered[0] = g_rendered[0];
                rendered[1] = g_rendered[1];
            }
            for (int i = 0; i < 2; ++i) {
                CopyInto(g_eyeChains[i], context, eyes[i]);
                projViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                projViews[i].pose = rendered[i].pose;
                projViews[i].fov = RenderedFov();
                projViews[i].subImage.swapchain = g_eyeChains[i].handle;
                projViews[i].subImage.imageRect.extent = {
                    static_cast<int32_t>(g_eyeChains[i].width),
                    static_cast<int32_t>(g_eyeChains[i].height)};
            }
            projection.space = g_space;
            projection.viewCount = 2;
            projection.views = projViews;
            layers.push_back(
                reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection));
        }
    }
    if (context) {
        context->Release();
    }
    if (device) {
        device->Release();
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frame.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = static_cast<uint32_t>(layers.size());
    endInfo.layers = layers.data();
    Check(xrEndFrame(g_session, &endInfo), "xrEndFrame");
    g_nextDisplayTime =
        frame.predictedDisplayTime + frame.predictedDisplayPeriod;
    ++g_presentFrame;
}

}  // namespace

State GetState() {
    return g_state;
}

EyeTarget GetEyeTarget() {
    std::lock_guard lock(g_poseMutex);
    return g_target;
}

bool LocateForRender(int eye, Pose* out) {
    if (g_state != State::Ready || !g_running || !g_nextDisplayTime) {
        return false;
    }
    // Both eyes share one locate per rendered frame.
    if (g_locatedFrame != g_presentFrame) {
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType =
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = g_nextDisplayTime;
        locate.space = g_space;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t count = 0;
        xrLocateViews(g_session, &locate, &viewState, 2, &count, views);
        if (count != 2 ||
            !(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            return false;
        }
        std::lock_guard lock(g_poseMutex);
        g_rendered[0] = views[0];
        g_rendered[1] = views[1];
        g_locatedFrame = g_presentFrame;
    }
    // OpenXR (x right, y up, -z forward) to engine (x, y forward, z up).
    const XrPosef& p = g_rendered[eye].pose;
    out->position[0] = p.position.x;
    out->position[1] = -p.position.z;
    out->position[2] = p.position.y;
    out->orientation[0] = p.orientation.x;
    out->orientation[1] = -p.orientation.z;
    out->orientation[2] = p.orientation.y;
    out->orientation[3] = p.orientation.w;
    return true;
}

void SetScreenMode(bool screen) {
    g_screenMode = screen;
}

void OnPresent(IDXGISwapChain* swapChain, ID3D11Texture2D* const eyes[2]) {
    State state = g_state;
    if (state == State::NotStarted ||
        (state == State::Unavailable && g_getProc && g_instance)) {
        if (g_retryCountdown > 0) {
            --g_retryCountdown;
            return;
        }
        Start(swapChain);
        return;
    }
    if (!g_session) {
        return;
    }
    PollEvents();
    if (g_session && g_running) {
        SubmitFrame(swapChain, eyes);
    }
}

}  // namespace tc2vr::xr
