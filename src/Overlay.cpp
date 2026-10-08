#include "Overlay.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <thread>

namespace Overlay
{
    // ---- the sprites
    std::vector<Vertex>& Sprites::Into(void* a_texture)
    {
        // a batch takes what is not textured, and one texture
        if (batches.empty() || (a_texture && batches.back().texture && batches.back().texture != a_texture)) {
            batches.emplace_back();
        }
        if (a_texture) {
            batches.back().texture = a_texture;
        }
        return batches.back().vertices;
    }

    void Sprites::Quad(void* a_texture, V2 a_p0, V2 a_p1, V2 a_p2, V2 a_p3, float a_u0, float a_v0, float a_u1, float a_v1, Color a_color, float a_mode)
    {
        // p0 p1 along the top, p3 p2 along the bottom
        auto&        out = Into(a_texture);
        const Vertex a{ a_p0.x, a_p0.y, a_u0, a_v0, a_color, a_mode }, b{ a_p1.x, a_p1.y, a_u1, a_v0, a_color, a_mode };
        const Vertex c{ a_p2.x, a_p2.y, a_u1, a_v1, a_color, a_mode }, d{ a_p3.x, a_p3.y, a_u0, a_v1, a_color, a_mode };
        out.insert(out.end(), { a, b, c, a, c, d });
    }

    void Sprites::Image(void* a_texture, V2 a_min, V2 a_max, Color a_tint)
    {
        if (a_texture) {
            Quad(a_texture, a_min, { a_max.x, a_min.y }, a_max, { a_min.x, a_max.y }, 0.0f, 0.0f, 1.0f, 1.0f, a_tint, 0.0f);
        }
    }

    void Sprites::Fill(V2 a_min, V2 a_max, Color a_color)
    {
        Quad(nullptr, a_min, { a_max.x, a_min.y }, a_max, { a_min.x, a_max.y }, 0.0f, 0.0f, 1.0f, 1.0f, a_color, 1.0f);
    }

    void Sprites::Outline(V2 a_min, V2 a_max, Color a_color, float a_width)
    {
        const float h = a_width * 0.5f;
        Fill({ a_min.x - h, a_min.y - h }, { a_max.x + h, a_min.y + h }, a_color);
        Fill({ a_min.x - h, a_max.y - h }, { a_max.x + h, a_max.y + h }, a_color);
        Fill({ a_min.x - h, a_min.y + h }, { a_min.x + h, a_max.y - h }, a_color);
        Fill({ a_max.x - h, a_min.y + h }, { a_max.x + h, a_max.y - h }, a_color);
    }

    void Sprites::Line(V2 a_from, V2 a_to, Color a_color, float a_width)
    {
        const float dx = a_to.x - a_from.x, dy = a_to.y - a_from.y, len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-3f) {
            return;
        }
        // a pixel wider either side, softened over it (v: -1..1 across)
        const float h = a_width * 0.5f + 1.0f;
        const float nx = -dy / len * h, ny = dx / len * h;
        Quad(nullptr, { a_from.x + nx, a_from.y + ny }, { a_to.x + nx, a_to.y + ny }, { a_to.x - nx, a_to.y - ny }, { a_from.x - nx, a_from.y - ny }, 0.0f, -1.0f, 0.0f, 1.0f,
            a_color, 3.0f);
    }

    void Sprites::Disc(V2 a_centre, float a_radius, Color a_color)
    {
        const float r = a_radius + 1.0f;
        const float k = r / std::max(a_radius, 0.5f);  // the edge at 1, softened over the pixel past it
        Quad(nullptr, { a_centre.x - r, a_centre.y - r }, { a_centre.x + r, a_centre.y - r }, { a_centre.x + r, a_centre.y + r }, { a_centre.x - r, a_centre.y + r }, -k, -k, k, k,
            a_color, 2.0f);
    }

    void Sprites::Ring(V2 a_centre, float a_radius, Color a_color, float a_width)
    {
        const int n = a_radius > 40.0f ? 96 : 32;
        for (int i = 0; i < n; ++i) {
            const float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
            Line({ a_centre.x + std::cos(a0) * a_radius, a_centre.y + std::sin(a0) * a_radius }, { a_centre.x + std::cos(a1) * a_radius, a_centre.y + std::sin(a1) * a_radius },
                a_color, a_width);
        }
    }

    void Sprites::Triangle(V2 a_a, V2 a_b, V2 a_c, Color a_color)
    {
        auto& out = Into(nullptr);
        out.insert(out.end(), { Vertex{ a_a.x, a_a.y, 0.0f, 0.0f, a_color, 1.0f }, Vertex{ a_b.x, a_b.y, 0.0f, 0.0f, a_color, 1.0f }, Vertex{ a_c.x, a_c.y, 0.0f, 0.0f, a_color, 1.0f } });
    }

    // ---- the renderer
    namespace
    {
        constexpr char kShader[] = R"(
cbuffer C : register(b0)
{
    float4 screen;  // x 2 / width, y 2 / height (of the coordinate space)
};
Texture2D    tex : register(t0);
SamplerState smp : register(s0);

struct VIn
{
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
    float  mode : TEXCOORD1;
};
struct VOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
    float  mode : TEXCOORD1;
};

VOut VS(VIn i)
{
    VOut o;
    o.pos = float4(i.pos.x * screen.x - 1.0, 1.0 - i.pos.y * screen.y, 0.0, 1.0);
    o.uv = i.uv;
    o.col = i.col;
    o.mode = i.mode;
    return o;
}

float4 PS(VOut i) : SV_Target
{
    float4 c = i.col;
    float4 t = tex.Sample(smp, i.uv);  // taken outside the branches (gradients)
    float  d = i.mode < 2.5 ? length(i.uv) : abs(i.uv.y);
    float  aa = max(fwidth(d), 1e-4);
    if (i.mode < 0.5) {
        c *= t;
    } else if (i.mode > 1.5) {
        c.a *= 1.0 - smoothstep(1.0 - aa, 1.0, d);  // a disc's or a line's soft edge
    }
    return c;
}
)";

        struct Compiled
        {
            std::atomic_bool done = false;
            bool             ok = false;
            ID3DBlob*        vs = nullptr;
            ID3DBlob*        ps = nullptr;
        } compiled;
        std::once_flag compileOnce;

        struct Renderer
        {
            bool                     tried = false, ready = false;
            ID3D11VertexShader*      vs = nullptr;
            ID3D11PixelShader*       ps = nullptr;
            ID3D11InputLayout*       layout = nullptr;
            ID3D11Buffer*            cb = nullptr;
            ID3D11Buffer*            vb = nullptr;
            UINT                     vbSize = 0;  // vertices it holds
            ID3D11SamplerState*      sampler = nullptr;
            ID3D11BlendState*        blend = nullptr;
            ID3D11RasterizerState*   raster = nullptr;
            ID3D11DepthStencilState* depth = nullptr;
        } r;

        void StartCompile()
        {
            std::call_once(compileOnce, [] {
                std::thread([] {
                    const auto module = LoadLibraryW(L"d3dcompiler_47.dll");
                    const auto compile = module ? reinterpret_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile")) : nullptr;
                    compiled.ok = compile != nullptr;
                    const char* stages[2][2] = { { "VS", "vs_5_0" }, { "PS", "ps_5_0" } };
                    ID3DBlob**  out[2] = { &compiled.vs, &compiled.ps };
                    for (int i = 0; compiled.ok && i < 2; ++i) {
                        ID3DBlob* errors = nullptr;
                        if (FAILED(compile(kShader, sizeof(kShader) - 1, "Overlay", nullptr, nullptr, stages[i][0], stages[i][1], D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out[i], &errors))) {
                            logger::error("overlay: shader {}: {}", stages[i][0], errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
                            compiled.ok = false;
                        }
                        if (errors) {
                            errors->Release();
                        }
                    }
                    compiled.done = true;
                }).detach();
            });
        }

        bool Init(ID3D11Device* a_device)
        {
            StartCompile();
            if (!compiled.done) {
                return false;
            }
            r.tried = true;
            bool ok = compiled.ok;
            if (ok) {
                const D3D11_INPUT_ELEMENT_DESC elements[] = {
                    { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                    { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                    { "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                };
                ok = SUCCEEDED(a_device->CreateVertexShader(compiled.vs->GetBufferPointer(), compiled.vs->GetBufferSize(), nullptr, &r.vs)) &&
                     SUCCEEDED(a_device->CreatePixelShader(compiled.ps->GetBufferPointer(), compiled.ps->GetBufferSize(), nullptr, &r.ps)) &&
                     SUCCEEDED(a_device->CreateInputLayout(elements, 4, compiled.vs->GetBufferPointer(), compiled.vs->GetBufferSize(), &r.layout));
            }
            for (auto blob : { compiled.vs, compiled.ps }) {
                if (blob) {
                    blob->Release();
                }
            }
            compiled.vs = compiled.ps = nullptr;
            D3D11_BUFFER_DESC cb{ 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
            ok = ok && SUCCEEDED(a_device->CreateBuffer(&cb, nullptr, &r.cb));
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            ok = ok && SUCCEEDED(a_device->CreateSamplerState(&sd, &r.sampler));
            D3D11_BLEND_DESC bd{};
            auto&            rt = bd.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ok = ok && SUCCEEDED(a_device->CreateBlendState(&bd, &r.blend));
            D3D11_RASTERIZER_DESC rs{};
            rs.FillMode = D3D11_FILL_SOLID;
            rs.CullMode = D3D11_CULL_NONE;
            rs.ScissorEnable = TRUE;
            rs.DepthClipEnable = TRUE;
            ok = ok && SUCCEEDED(a_device->CreateRasterizerState(&rs, &r.raster));
            D3D11_DEPTH_STENCIL_DESC ds{};
            ds.DepthEnable = FALSE;
            ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
            ok = ok && SUCCEEDED(a_device->CreateDepthStencilState(&ds, &r.depth));
            r.ready = ok;
            logger::info("overlay: renderer {}", ok ? "ready" : "FAILED");
            return ok;
        }

        // the pipeline state we touch, put back afterwards
        struct StateBackup
        {
            ID3D11InputLayout*        layout = nullptr;
            D3D11_PRIMITIVE_TOPOLOGY  topology{};
            ID3D11Buffer*             vb = nullptr;
            UINT                      vbStride = 0, vbOffset = 0;
            ID3D11VertexShader*       vs = nullptr;
            ID3D11PixelShader*        ps = nullptr;
            ID3D11GeometryShader*     gs = nullptr;
            ID3D11Buffer*             vsCb = nullptr;
            ID3D11ShaderResourceView* psSrv = nullptr;
            ID3D11SamplerState*       sampler = nullptr;
            ID3D11RasterizerState*    raster = nullptr;
            D3D11_VIEWPORT            viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            UINT                      viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            D3D11_RECT                scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            UINT                      scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            ID3D11BlendState*         blend = nullptr;
            float                     blendFactor[4]{};
            UINT                      sampleMask = 0;
            ID3D11DepthStencilState*  depth = nullptr;
            UINT                      stencilRef = 0;

            void Save(ID3D11DeviceContext* c)
            {
                c->IAGetInputLayout(&layout);
                c->IAGetPrimitiveTopology(&topology);
                c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
                UINT n = 0;
                c->VSGetShader(&vs, nullptr, &n);
                n = 0;
                c->PSGetShader(&ps, nullptr, &n);
                n = 0;
                c->GSGetShader(&gs, nullptr, &n);
                c->VSGetConstantBuffers(0, 1, &vsCb);
                c->PSGetShaderResources(0, 1, &psSrv);
                c->PSGetSamplers(0, 1, &sampler);
                c->RSGetState(&raster);
                c->RSGetViewports(&viewportCount, viewports);
                c->RSGetScissorRects(&scissorCount, scissors);
                c->OMGetBlendState(&blend, blendFactor, &sampleMask);
                c->OMGetDepthStencilState(&depth, &stencilRef);
            }

            void Restore(ID3D11DeviceContext* c)
            {
                c->IASetInputLayout(layout);
                c->IASetPrimitiveTopology(topology);
                c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
                c->VSSetShader(vs, nullptr, 0);
                c->PSSetShader(ps, nullptr, 0);
                c->GSSetShader(gs, nullptr, 0);
                c->VSSetConstantBuffers(0, 1, &vsCb);
                c->PSSetShaderResources(0, 1, &psSrv);
                c->PSSetSamplers(0, 1, &sampler);
                c->RSSetState(raster);
                c->RSSetViewports(viewportCount, viewports);
                c->RSSetScissorRects(scissorCount, scissors);
                c->OMSetBlendState(blend, blendFactor, sampleMask);
                c->OMSetDepthStencilState(depth, stencilRef);
                for (IUnknown* p : std::initializer_list<IUnknown*>{ layout, vb, vs, ps, gs, vsCb, psSrv, sampler, raster, blend, depth }) {
                    if (p) {
                        p->Release();
                    }
                }
            }
        };
    }

    void Prepare()
    {
        StartCompile();
    }

    bool Draw(const std::vector<Batch>& a_batches, float a_w, float a_h, const float (&a_clip)[4])
    {
        const auto device = reinterpret_cast<ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice());
        const auto data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        const auto context = data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
        if (!device || !context || a_w <= 0.0f || a_h <= 0.0f) {
            return false;
        }
        if (!r.tried) {
            Init(device);
        }
        if (!r.ready) {
            return false;
        }
        // what is drawn to now, and its size
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        context->OMGetRenderTargets(1, &rtv, &dsv);
        if (dsv) {
            dsv->Release();
        }
        if (!rtv) {
            return false;
        }
        ID3D11Resource* resource = nullptr;
        rtv->GetResource(&resource);
        rtv->Release();
        ID3D11Texture2D* texture = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) {
            texture->GetDesc(&desc);
            texture->Release();
        }
        if (resource) {
            resource->Release();
        }
        if (desc.Width == 0 || desc.Height == 0) {
            return false;
        }

        // the vertices, all in one buffer
        std::size_t total = 0;
        for (const auto& b : a_batches) {
            total += b.vertices.size();
        }
        if (total == 0) {
            return true;
        }
        if (total > r.vbSize) {
            if (r.vb) {
                r.vb->Release();
                r.vb = nullptr;
            }
            r.vbSize = static_cast<UINT>(total + total / 2 + 1024);
            D3D11_BUFFER_DESC vd{ static_cast<UINT>(r.vbSize * sizeof(Vertex)), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
            if (FAILED(device->CreateBuffer(&vd, nullptr, &r.vb))) {
                r.vbSize = 0;
                return false;
            }
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(r.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return false;
        }
        auto* out = static_cast<Vertex*>(mapped.pData);
        for (const auto& b : a_batches) {
            std::memcpy(out, b.vertices.data(), b.vertices.size() * sizeof(Vertex));
            out += b.vertices.size();
        }
        context->Unmap(r.vb, 0);
        const float screen[4]{ 2.0f / a_w, 2.0f / a_h, 0.0f, 0.0f };
        if (SUCCEEDED(context->Map(r.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, screen, sizeof(screen));
            context->Unmap(r.cb, 0);
        }

        StateBackup backup;
        backup.Save(context);
        const float          sx = static_cast<float>(desc.Width) / a_w, sy = static_cast<float>(desc.Height) / a_h;
        const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0.0f, 1.0f };
        const D3D11_RECT     scissor{ static_cast<LONG>(a_clip[0] * sx), static_cast<LONG>(a_clip[1] * sy), static_cast<LONG>(std::ceil(a_clip[2] * sx)),
                static_cast<LONG>(std::ceil(a_clip[3] * sy)) };
        constexpr UINT       kStride = sizeof(Vertex), kOffset = 0;
        context->RSSetViewports(1, &viewport);
        context->RSSetScissorRects(1, &scissor);
        context->RSSetState(r.raster);
        context->IASetInputLayout(r.layout);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetVertexBuffers(0, 1, &r.vb, &kStride, &kOffset);
        context->VSSetShader(r.vs, nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(r.ps, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &r.cb);
        context->PSSetSamplers(0, 1, &r.sampler);
        context->OMSetBlendState(r.blend, nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(r.depth, 0);
        UINT first = 0;
        for (const auto& b : a_batches) {
            auto srv = static_cast<ID3D11ShaderResourceView*>(b.texture);
            context->PSSetShaderResources(0, 1, &srv);
            context->Draw(static_cast<UINT>(b.vertices.size()), first);
            first += static_cast<UINT>(b.vertices.size());
        }
        backup.Restore(context);
        return true;
    }
}
