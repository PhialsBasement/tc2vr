#include "mirror.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>

#include "log.hpp"

namespace tc2vr::mirror {
namespace {

// Full-screen triangle; uv = corner * scale + offset crops the eye.
constexpr char kShader[] = R"(
cbuffer Crop : register(b0) { float4 scaleOffset; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t * scaleOffset.xy + scaleOffset.zw;
    return o;
}
Texture2D eye : register(t0);
SamplerState linearClamp : register(s0);
float4 ps(V i) : SV_Target { return float4(eye.Sample(linearClamp, i.uv).rgb, 1); }
)";

using CompileFn = decltype(&D3DCompile);

bool g_failed = false;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11Buffer* g_crop = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11Texture2D* g_srvTexture = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;

ID3DBlob* Compile(CompileFn compile, const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(kShader, sizeof(kShader) - 1, "tc2vr_mirror", nullptr,
                         nullptr, entry, target, 0, 0, &code, &errors);
    if (FAILED(hr)) {
        LOG("mirror: %s compile failed: %s", entry,
            errors ? static_cast<const char*>(errors->GetBufferPointer())
                   : "no details");
    }
    if (errors) {
        errors->Release();
    }
    return code;
}

bool Init(ID3D11Device* device) {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<CompileFn>(
                                  GetProcAddress(compiler, "D3DCompile"))
                            : nullptr;
    if (!compile) {
        LOG("mirror: d3dcompiler_47.dll unavailable");
        return false;
    }
    ID3DBlob* vs = Compile(compile, "vs", "vs_4_0");
    ID3DBlob* ps = Compile(compile, "ps", "ps_4_0");
    bool ok = vs && ps &&
              SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(),
                                                   vs->GetBufferSize(),
                                                   nullptr, &g_vs)) &&
              SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(),
                                                  ps->GetBufferSize(),
                                                  nullptr, &g_ps));
    if (vs) {
        vs->Release();
    }
    if (ps) {
        ps->Release();
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW =
        D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC crop{};
    crop.ByteWidth = 16;
    crop.Usage = D3D11_USAGE_DEFAULT;
    crop.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(device->CreateSamplerState(&sampler, &g_sampler)) &&
         SUCCEEDED(device->CreateBuffer(&crop, nullptr, &g_crop)) &&
         SUCCEEDED(device->CreateRasterizerState(&raster, &g_raster));
    LOG("mirror: %s", ok ? "ready" : "setup failed");
    return ok;
}

// The game's pipeline state touched by the blit, restored afterwards.
struct SavedState {
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* vsCb = nullptr;
    ID3D11ShaderResourceView* psSrv = nullptr;
    ID3D11SamplerState* psSampler = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    float blendFactor[4];
    UINT sampleMask = 0;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
};

void Save(ID3D11DeviceContext* c, SavedState& s) {
    c->IAGetInputLayout(&s.layout);
    c->IAGetPrimitiveTopology(&s.topology);
    c->VSGetShader(&s.vs, nullptr, nullptr);
    c->PSGetShader(&s.ps, nullptr, nullptr);
    c->VSGetConstantBuffers(0, 1, &s.vsCb);
    c->PSGetShaderResources(0, 1, &s.psSrv);
    c->PSGetSamplers(0, 1, &s.psSampler);
    c->RSGetState(&s.raster);
    c->RSGetViewports(&s.viewportCount, s.viewports);
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtvs,
                          &s.dsv);
    c->OMGetBlendState(&s.blend, s.blendFactor, &s.sampleMask);
    c->OMGetDepthStencilState(&s.depth, &s.stencilRef);
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void Restore(ID3D11DeviceContext* c, SavedState& s) {
    c->IASetInputLayout(s.layout);
    c->IASetPrimitiveTopology(s.topology);
    c->VSSetShader(s.vs, nullptr, 0);
    c->PSSetShader(s.ps, nullptr, 0);
    c->VSSetConstantBuffers(0, 1, &s.vsCb);
    c->PSSetShaderResources(0, 1, &s.psSrv);
    c->PSSetSamplers(0, 1, &s.psSampler);
    c->RSSetState(s.raster);
    c->RSSetViewports(s.viewportCount, s.viewports);
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtvs,
                          s.dsv);
    c->OMSetBlendState(s.blend, s.blendFactor, s.sampleMask);
    c->OMSetDepthStencilState(s.depth, s.stencilRef);
    SafeRelease(s.layout);
    SafeRelease(s.vs);
    SafeRelease(s.ps);
    SafeRelease(s.vsCb);
    SafeRelease(s.psSrv);
    SafeRelease(s.psSampler);
    SafeRelease(s.raster);
    for (auto& rtv : s.rtvs) {
        SafeRelease(rtv);
    }
    SafeRelease(s.dsv);
    SafeRelease(s.blend);
    SafeRelease(s.depth);
}

// Eye render targets are typeless BGRA holding gamma-encoded values,
// like the UNORM back buffer, so both are viewed as UNORM.
ID3D11ShaderResourceView* EyeView(ID3D11Device* device,
                                  ID3D11Texture2D* eye) {
    if (eye == g_srvTexture && g_srv) {
        return g_srv;
    }
    SafeRelease(g_srv);
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    g_srvTexture = SUCCEEDED(device->CreateShaderResourceView(eye, &desc,
                                                              &g_srv))
                       ? eye
                       : nullptr;
    return g_srv;
}

}  // namespace

void Draw(IDXGISwapChain* swapChain, ID3D11Texture2D* eye) {
    if (g_failed || !eye) {
        return;
    }
    ID3D11Device* device = nullptr;
    swapChain->GetDevice(__uuidof(ID3D11Device),
                         reinterpret_cast<void**>(&device));
    if (!device) {
        return;
    }
    if (!g_vs && !Init(device)) {
        g_failed = true;
        device->Release();
        return;
    }
    ID3D11Texture2D* back = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                         reinterpret_cast<void**>(&back));
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = EyeView(device, eye);
    if (back && srv && SUCCEEDED(device->CreateRenderTargetView(back, nullptr,
                                                                &rtv))) {
        D3D11_TEXTURE2D_DESC backDesc, eyeDesc;
        back->GetDesc(&backDesc);
        eye->GetDesc(&eyeDesc);
        // Centre crop of the eye image with the window's aspect.
        float eyeAspect = float(eyeDesc.Width) / float(eyeDesc.Height);
        float backAspect = float(backDesc.Width) / float(backDesc.Height);
        float scale[2] = {1.0f, 1.0f};
        if (eyeAspect < backAspect) {
            scale[1] = eyeAspect / backAspect;
        } else {
            scale[0] = backAspect / eyeAspect;
        }
        float crop[4] = {scale[0], scale[1], 0.5f - scale[0] * 0.5f,
                         0.5f - scale[1] * 0.5f};

        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        SavedState saved;
        Save(context, saved);
        context->UpdateSubresource(g_crop, 0, nullptr, crop, 0, 0);
        D3D11_VIEWPORT viewport{0, 0, float(backDesc.Width),
                                float(backDesc.Height), 0, 1};
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(g_vs, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &g_crop);
        context->PSSetShader(g_ps, nullptr, 0);
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetSamplers(0, 1, &g_sampler);
        context->RSSetState(g_raster);
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        context->Draw(3, 0);
        Restore(context, saved);
        context->Release();
    }
    SafeRelease(rtv);
    SafeRelease(back);
    device->Release();
}

}  // namespace tc2vr::mirror
