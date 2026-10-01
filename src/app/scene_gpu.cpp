#include "scene_gpu.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstddef>
#include <cstring>
#include "scene_mesh.h"
#include "log.h"

namespace luma::app::scene_gpu {
namespace {
template <class T> void Release(T*& p) { if (p) p->Release(); p = nullptr; }
struct Target {
    ID3D11Texture2D *image = nullptr, *color = nullptr, *depth = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
    void Clear() {
        Release(srv); Release(rtv); Release(dsv); Release(image); Release(color); Release(depth);
        w = h = 0;
    }
};
ID3D11Device* dev = nullptr;
ID3D11DeviceContext* ctx = nullptr;
ID3D11VertexShader* vs = nullptr;
ID3D11PixelShader* ps = nullptr;
ID3D11InputLayout* layout = nullptr;
ID3D11Buffer* vertices = nullptr;
ID3D11RasterizerState* raster = nullptr;
ID3D11DepthStencilState *solidDepth = nullptr, *blendDepth = nullptr;
ID3D11BlendState *solidBlend = nullptr, *alphaBlend = nullptr;
Target targets[kSlots];
size_t capacity = 0;
UINT samples = 1;

bool Resize(Target& t, int w, int h) {
    if (t.w == w && t.h == h && t.srv) return true;
    t.Clear();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w); desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (samples == 1 ? D3D11_BIND_RENDER_TARGET : 0);
    if (FAILED(dev->CreateTexture2D(&desc, nullptr, &t.image)) || FAILED(dev->CreateShaderResourceView(t.image, nullptr, &t.srv))) return false;
    if (samples > 1) {
        desc.SampleDesc.Count = samples; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&desc, nullptr, &t.color))) return false;
    }
    if (FAILED(dev->CreateRenderTargetView(samples > 1 ? t.color : t.image, nullptr, &t.rtv))) return false;
    desc.SampleDesc.Count = samples; desc.BindFlags = D3D11_BIND_DEPTH_STENCIL; desc.Format = DXGI_FORMAT_D32_FLOAT;
    if (FAILED(dev->CreateTexture2D(&desc, nullptr, &t.depth)) || FAILED(dev->CreateDepthStencilView(t.depth, nullptr, &t.dsv))) return false;
    t.w = w; t.h = h;
    return true;
}
}  // namespace

void Shutdown() {
    for (Target& t : targets) t.Clear();
    Release(vertices); Release(layout); Release(vs); Release(ps); Release(raster);
    Release(solidDepth); Release(blendDepth); Release(solidBlend); Release(alphaBlend);
    Release(ctx); Release(dev); capacity = 0; samples = 1;
}

bool Init(ID3D11Device* device, ID3D11DeviceContext* context) {
    Shutdown();
    dev = device; ctx = context;
    if (!dev || !ctx) { dev = nullptr; ctx = nullptr; return false; }
    dev->AddRef(); ctx->AddRef();
    const char shader[] = R"(
struct V { float4 position : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; float glow : TEXCOORD1; };
struct P { float4 position : SV_POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; float glow : TEXCOORD1; };
P vertex(V v) { P p; p.position=v.position; p.color=v.color; p.uv=v.uv; p.glow=v.glow; return p; }
float4 pixel(P p) : SV_TARGET {
    if (p.glow > 0.5) { float r=length(p.uv); clip(1-r); p.color.a *= pow(saturate(1-r), 2); }
    return p.color;
})";
    ID3DBlob *v = nullptr, *p = nullptr, *error = nullptr;
    HRESULT hr = D3DCompile(shader, sizeof shader - 1, nullptr, nullptr, nullptr, "vertex", "vs_4_0", 0, 0, &v, &error);
    if (FAILED(hr)) {
        LUMA_WARN("3D vertex shader: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "compile failed");
        Release(error); Shutdown(); return false;
    }
    Release(error);
    hr = D3DCompile(shader, sizeof shader - 1, nullptr, nullptr, nullptr, "pixel", "ps_4_0", 0, 0, &p, &error);
    if (FAILED(hr)) {
        LUMA_WARN("3D pixel shader: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "compile failed");
        Release(error); Release(v); Shutdown(); return false;
    }
    Release(error);
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps);
    if (SUCCEEDED(hr)) hr = dev->CreateInputLayout(elements, 4, v->GetBufferPointer(), v->GetBufferSize(), &layout);
    Release(v); Release(p);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.MultisampleEnable = TRUE;
    if (SUCCEEDED(hr)) hr = dev->CreateRasterizerState(&rd, &raster);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (SUCCEEDED(hr)) hr = dev->CreateDepthStencilState(&dd, &solidDepth);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (SUCCEEDED(hr)) hr = dev->CreateDepthStencilState(&dd, &blendDepth);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = dev->CreateBlendState(&bd, &solidBlend);
    auto& blend = bd.RenderTarget[0];
    blend.BlendEnable = TRUE; blend.SrcBlend = D3D11_BLEND_SRC_ALPHA; blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D11_BLEND_OP_ADD; blend.SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (SUCCEEDED(hr)) hr = dev->CreateBlendState(&bd, &alphaBlend);
    if (FAILED(hr)) { LUMA_WARN("3D depth renderer setup failed: 0x%08lX", static_cast<unsigned long>(hr)); Shutdown(); return false; }
    UINT colorLevels = 0, depthLevels = 0;
    dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &colorLevels);
    dev->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT, 4, &depthLevels);
    samples = colorLevels && depthLevels ? 4 : 1;
    LUMA_INFO("3D depth renderer ready (%u samples)", samples);
    return true;
}

uintptr_t Image(int slot, const std::vector<s3d::DrawItem>& items, const s3d::Viewport& vp, float scale) {
    if (!dev || slot < 0 || slot >= kSlots || vp.w < 1 || vp.h < 1) return 0;
    Target& t = targets[slot];
    if (!Resize(t, std::clamp(static_cast<int>(std::ceil(vp.w)), 1, 4096), std::clamp(static_cast<int>(std::ceil(vp.h)), 1, 4096))) return 0;
    const s3d::Mesh mesh = s3d::MakeMesh(items, vp, scale);
    const size_t count = mesh.opaque.size() + mesh.transparent.size();
    if (!count) return 0;
    static_assert(sizeof(s3d::MeshVertex) == 32, "vertex layout must match the input layout");
    if (capacity < count) {
        Release(vertices);
        capacity = count + count / 2 + 1024;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(capacity * sizeof(s3d::MeshVertex)); desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&desc, nullptr, &vertices))) { capacity = 0; return 0; }
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(vertices, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return 0;
    auto* data = static_cast<s3d::MeshVertex*>(mapped.pData);
    std::memcpy(data, mesh.opaque.data(), mesh.opaque.size() * sizeof(*data));
    if (!mesh.transparent.empty()) std::memcpy(data + mesh.opaque.size(), mesh.transparent.data(), mesh.transparent.size() * sizeof(*data));
    ctx->Unmap(vertices, 0);
    // Unbind the previous frame's image before using it as a render target.
    ID3D11ShaderResourceView* empty = nullptr;
    ctx->PSSetShaderResources(0, 1, &empty);
    ctx->OMSetRenderTargets(1, &t.rtv, t.dsv);
    const float clear[4] = {0, 0, 0, 1};
    ctx->ClearRenderTargetView(t.rtv, clear);
    ctx->ClearDepthStencilView(t.dsv, D3D11_CLEAR_DEPTH, 1, 0);
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(t.w), static_cast<float>(t.h), 0, 1};
    ctx->RSSetViewports(1, &viewport); ctx->RSSetState(raster);
    const UINT stride = sizeof(s3d::MeshVertex), offset = 0;
    ctx->IASetInputLayout(layout); ctx->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0); ctx->HSSetShader(nullptr, nullptr, 0); ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->OMSetDepthStencilState(solidDepth, 0); ctx->OMSetBlendState(solidBlend, nullptr, 0xFFFFFFFFu);
    ctx->Draw(static_cast<UINT>(mesh.opaque.size()), 0);
    ctx->OMSetDepthStencilState(blendDepth, 0); ctx->OMSetBlendState(alphaBlend, nullptr, 0xFFFFFFFFu);
    ctx->Draw(static_cast<UINT>(mesh.transparent.size()), static_cast<UINT>(mesh.opaque.size()));
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (samples > 1) ctx->ResolveSubresource(t.image, 0, t.color, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    return reinterpret_cast<uintptr_t>(t.srv);
}
}  // namespace luma::app::scene_gpu
