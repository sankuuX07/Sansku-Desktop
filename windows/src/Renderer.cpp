#include "Renderer.h"
#include "AVSynchronizer.h"
#include "Logger.h"

#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace SanskyStream {

// ---------------------------------------------------------------------------
// HLSL Vertex Shader
//
// Generates a full-screen quad from SV_VertexID 0..5 (two triangles).
// No vertex buffer or input layout needed.
//
// Triangle 0: vertices 0,1,2  (top-left, top-right, bottom-left)
// Triangle 1: vertices 3,4,5  (bottom-left, top-right, bottom-right)
//
// D3D11 NDC convention: X in [-1,1], Y in [-1,1] (up = +1).
// UV convention: (0,0) = top-left, (1,1) = bottom-right.
// ---------------------------------------------------------------------------
static const char* k_vsSource = R"hlsl(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID) {
    // id: 0=TL 1=TR 2=BL | 3=BL 4=TR 5=BR
    float x = ((id == 1u) || (id == 4u) || (id == 5u)) ?  1.0f : -1.0f;
    float y = ((id == 0u) || (id == 1u) || (id == 4u)) ?  1.0f : -1.0f;
    float u = ((id == 1u) || (id == 4u) || (id == 5u)) ?  1.0f :  0.0f;
    float v = ((id == 2u) || (id == 3u) || (id == 5u)) ?  1.0f :  0.0f;
    VSOut o;
    o.pos = float4(x, y, 0.0f, 1.0f);
    o.uv  = float2(u, v);
    return o;
}
)hlsl";

// ---------------------------------------------------------------------------
// HLSL Pixel Shader
//
// NV12 -> RGB conversion using BT.601 limited-range coefficients.
//
// t0 (R8_UNORM):   Y  plane — one float  per pixel, full resolution.
// t1 (R8G8_UNORM): UV plane — two floats per pixel, half resolution.
//
// BT.601 limited-range formulas (integer precision equivalents):
//   luma = 1.164 * (Y - 16/255)
//   r    = luma + 1.596 * (Cr - 0.5)
//   g    = luma - 0.391 * (Cb - 0.5) - 0.813 * (Cr - 0.5)
//   b    = luma + 2.018 * (Cb - 0.5)
// ---------------------------------------------------------------------------
static const char* k_psSource = R"hlsl(
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState      samp  : register(s0);

float4 main(float2 uv : TEXCOORD0) : SV_TARGET {
    float  Y  = texY.Sample(samp, uv);
    float2 UV = texUV.Sample(samp, uv);
    float  Cb = UV.x - 0.5f;
    float  Cr = UV.y - 0.5f;
    float luma = 1.16438f * (Y - 0.06275f);
    float r = saturate(luma                   + 1.59603f * Cr);
    float g = saturate(luma - 0.39176f * Cb  - 0.81297f * Cr);
    float b = saturate(luma + 2.01723f * Cb                  );
    return float4(r, g, b, 1.0f);
}
)hlsl";

// ---------------------------------------------------------------------------
// Background clear color — dark blue-gray, fills letterbox bars.
// ---------------------------------------------------------------------------
static constexpr float k_clearColor[4] = {0.05f, 0.07f, 0.12f, 1.0f};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Renderer::Renderer(Window* window)
    : m_window(window)
{
    QueryPerformanceFrequency(&m_perfFreq);
    QueryPerformanceCounter(&m_fpsLastTime);
}

Renderer::~Renderer() {
    ReleaseVideoResources();
    LOG_INFO("Renderer shutting down.");
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

bool Renderer::Initialize() {
    if (!m_window || !m_window->GetHWND()) {
        LOG_ERROR("Renderer: Invalid window provided.");
        return false;
    }

    // -----------------------------------------------------------------------
    // Create D3D11 device + swap chain
    // -----------------------------------------------------------------------
    DXGI_SWAP_CHAIN_DESC scd        = {};
    scd.BufferCount                 = 1;
    scd.BufferDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width            = static_cast<UINT>(m_window->GetWidth());
    scd.BufferDesc.Height           = static_cast<UINT>(m_window->GetHeight());
    scd.BufferUsage                 = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow                = m_window->GetHWND();
    scd.SampleDesc.Count            = 1;
    scd.Windowed                    = TRUE;
    scd.SwapEffect                  = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        nullptr, 0,
        D3D11_SDK_VERSION,
        &scd,
        &m_swapChain,
        &m_device,
        nullptr,
        &m_context);

    if (FAILED(hr)) {
        LOG_ERROR("Renderer: D3D11CreateDeviceAndSwapChain failed.");
        return false;
    }

    // Create initial render target view.
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        hr = m_swapChain->GetBuffer(
            0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(backBuffer.GetAddressOf()));
        if (FAILED(hr)) {
            LOG_ERROR("Renderer: GetBuffer (back buffer) failed.");
            return false;
        }
        hr = m_device->CreateRenderTargetView(
            backBuffer.Get(), nullptr, &m_renderTargetView);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer: CreateRenderTargetView failed.");
            return false;
        }
    }
    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);

    // -----------------------------------------------------------------------
    // Compile and create NV12 -> RGB shaders
    // -----------------------------------------------------------------------
    if (!InitShaders()) return false;

    // -----------------------------------------------------------------------
    // Linear-clamp sampler — smooth video scaling
    // -----------------------------------------------------------------------
    {
        D3D11_SAMPLER_DESC sd      = {};
        sd.Filter                  = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU                = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV                = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW                = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc          = D3D11_COMPARISON_NEVER;
        sd.MaxLOD                  = D3D11_FLOAT32_MAX;
        hr = m_device->CreateSamplerState(&sd, &m_sampler);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer: CreateSamplerState failed.");
            return false;
        }
    }

    // -----------------------------------------------------------------------
    // No-cull rasterizer (SV_VertexID quads have no winding-order requirement)
    // -----------------------------------------------------------------------
    {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode              = D3D11_FILL_SOLID;
        rd.CullMode              = D3D11_CULL_NONE;
        hr = m_device->CreateRasterizerState(&rd, &m_rasterizerState);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer: CreateRasterizerState failed.");
            return false;
        }
    }

    LOG_INFO("Renderer initialized successfully.");
    return true;
}

// ---------------------------------------------------------------------------
// InitShaders — compile inline HLSL, create ID3D11VertexShader + PixelShader
// ---------------------------------------------------------------------------

bool Renderer::InitShaders() {
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;

    // Vertex shader
    HRESULT hr = D3DCompile(
        k_vsSource, std::strlen(k_vsSource),
        "VideoVS", nullptr, nullptr,
        "main", "vs_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &vsBlob, &errBlob);
    if (FAILED(hr)) {
        const std::string err = errBlob
            ? std::string(static_cast<const char*>(errBlob->GetBufferPointer()),
                          errBlob->GetBufferSize())
            : "(no error blob)";
        LOG_ERROR("Renderer: Vertex shader compile failed: " + err);
        return false;
    }
    hr = m_device->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        nullptr, &m_vertexShader);
    if (FAILED(hr)) {
        LOG_ERROR("Renderer: CreateVertexShader failed.");
        return false;
    }

    // Pixel shader
    errBlob.Reset();
    hr = D3DCompile(
        k_psSource, std::strlen(k_psSource),
        "VideoPS", nullptr, nullptr,
        "main", "ps_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &psBlob, &errBlob);
    if (FAILED(hr)) {
        const std::string err = errBlob
            ? std::string(static_cast<const char*>(errBlob->GetBufferPointer()),
                          errBlob->GetBufferSize())
            : "(no error blob)";
        LOG_ERROR("Renderer: Pixel shader compile failed: " + err);
        return false;
    }
    hr = m_device->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
        nullptr, &m_pixelShader);
    if (FAILED(hr)) {
        LOG_ERROR("Renderer: CreatePixelShader failed.");
        return false;
    }

    LOG_INFO("Renderer: NV12->RGB shaders compiled successfully.");
    return true;
}

// ---------------------------------------------------------------------------
// SetFrameQueue
// ---------------------------------------------------------------------------

void Renderer::SetFrameQueue(VideoFrameQueue* queue) {
    m_frameQueue = queue;
}

// ---------------------------------------------------------------------------
// SetAVSync (M12)
// ---------------------------------------------------------------------------

void Renderer::SetAVSync(AVSynchronizer* sync) {
    m_avSync = sync;
}

// ---------------------------------------------------------------------------
// ClearColor  (legacy public helper — kept for API compatibility)
// ---------------------------------------------------------------------------

void Renderer::ClearColor(float r, float g, float b, float a) {
    if (!m_context || !m_renderTargetView) return;
    float color[4] = {r, g, b, a};
    m_context->ClearRenderTargetView(m_renderTargetView.Get(), color);
}

// ---------------------------------------------------------------------------
// Render  — one frame
// ---------------------------------------------------------------------------

void Renderer::Render() {
    if (!m_swapChain || !m_context) return;

    // -----------------------------------------------------------------------
    // 1. Handle pending window resize (WM_SIZE set m_resizePending)
    // -----------------------------------------------------------------------
    if (m_window) {
        int newW = 0, newH = 0;
        if (m_window->TakeResizePending(newW, newH)) {
            HandleResize(newW, newH);
        }
    }

    // -----------------------------------------------------------------------
    // 2. Clear back buffer — dark background, fills letterbox bars too
    // -----------------------------------------------------------------------
    if (m_renderTargetView) {
        m_context->ClearRenderTargetView(m_renderTargetView.Get(), k_clearColor);
    }

    // -----------------------------------------------------------------------
    // 3. Pop latest decoded frame from the queue
    // -----------------------------------------------------------------------
    bool hasNewFrame = false;
    if (m_frameQueue) {
        DecodedFrame frame;
        if (m_frameQueue->TryPop(frame)) {
            if (UploadNV12Frame(frame)) {
                m_hasVideo   = true;
                hasNewFrame  = true;
            }
        }
    }

    // -----------------------------------------------------------------------
    // 4. Draw video quad (letterbox viewport, NV12->RGB shader)
    // -----------------------------------------------------------------------
    if (m_hasVideo && m_srvY && m_srvUV &&
        m_vertexShader && m_pixelShader && m_renderTargetView)
    {
        const float wndW = (m_window && m_window->GetWidth()  > 0)
                           ? static_cast<float>(m_window->GetWidth())  : 1.0f;
        const float wndH = (m_window && m_window->GetHeight() > 0)
                           ? static_cast<float>(m_window->GetHeight()) : 1.0f;

        D3D11_VIEWPORT vp = ComputeLetterbox(
            static_cast<float>(m_videoWidth),
            static_cast<float>(m_videoHeight),
            wndW, wndH);

        m_context->RSSetViewports(1, &vp);
        m_context->RSSetState(m_rasterizerState.Get());

        m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(m_pixelShader.Get(),  nullptr, 0);

        ID3D11ShaderResourceView* srvs[2] = {m_srvY.Get(), m_srvUV.Get()};
        m_context->PSSetShaderResources(0, 2, srvs);

        ID3D11SamplerState* samplers[1] = {m_sampler.Get()};
        m_context->PSSetSamplers(0, 1, samplers);

        // SV_VertexID quad: no vertex buffer, no input layout
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        m_context->Draw(6, 0);

        // Unbind SRVs to avoid D3D debug-layer warnings on next frame
        ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
        m_context->PSSetShaderResources(0, 2, nullSRVs);
    }

    // -----------------------------------------------------------------------
    // 5. FPS measurement — update status overlay at ~1 Hz
    // -----------------------------------------------------------------------
    m_fpsFrameCount++;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float elapsed =
        static_cast<float>(now.QuadPart - m_fpsLastTime.QuadPart) /
        static_cast<float>(m_perfFreq.QuadPart);

    if (elapsed >= 1.0f && m_window) {
        m_fps           = static_cast<float>(m_fpsFrameCount) / elapsed;
        m_fpsFrameCount = 0;
        m_fpsLastTime   = now;

        std::string status = "SanskyStream\r\n\r\n";
        if (m_hasVideo) {
            char buf[64];
            snprintf(buf, sizeof(buf), "FPS: %.1f  |  %ux%u",
                     static_cast<double>(m_fps),
                     static_cast<unsigned>(m_videoWidth),
                     static_cast<unsigned>(m_videoHeight));
            status += buf;
        } else {
            status += "Waiting for video...";
        }

        // M12: append lightweight A/V sync diagnostics (~1 Hz, zero per-frame cost).
        if (m_avSync) {
            const SyncStats s = m_avSync->GetStats();
            char syncBuf[128];
            const double avMs = static_cast<double>(s.avDiffUs) / 1000.0;
            snprintf(syncBuf, sizeof(syncBuf),
                     "\r\nA/V: %+.0f ms | Drop: %llu | %s",
                     avMs,
                     static_cast<unsigned long long>(s.droppedFrames),
                     s.isAnchored ? "Synced" : "Unsynced");
            status += syncBuf;
        }

        m_window->SetStatusText(status);
    }

    // -----------------------------------------------------------------------
    // 6. Present — no VSync for minimum streaming latency.
    //    Sleep 1 ms when no new frame arrived to avoid a CPU spin-loop.
    // -----------------------------------------------------------------------
    m_swapChain->Present(0, 0);

    if (!hasNewFrame) {
        Sleep(1);
    }
}

// ---------------------------------------------------------------------------
// HandleResize
// ---------------------------------------------------------------------------

void Renderer::HandleResize(int newW, int newH) {
    if (newW <= 0 || newH <= 0) {
        LOG_INFO("Renderer: Resize skipped — window minimized or zero-size.");
        return;
    }
    if (!m_swapChain || !m_device || !m_context) return;

    // D3D requirement: unbind the RTV before calling ResizeBuffers.
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_renderTargetView.Reset();

    HRESULT hr = m_swapChain->ResizeBuffers(
        0,
        static_cast<UINT>(newW),
        static_cast<UINT>(newH),
        DXGI_FORMAT_UNKNOWN,
        0);
    if (FAILED(hr)) {
        LOG_ERROR("Renderer: SwapChain ResizeBuffers failed.");
        return;
    }

    // Recreate render target view from the new back buffer.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    hr = m_swapChain->GetBuffer(
        0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(backBuffer.GetAddressOf()));
    if (FAILED(hr)) {
        LOG_ERROR("Renderer: GetBuffer after resize failed.");
        return;
    }
    hr = m_device->CreateRenderTargetView(
        backBuffer.Get(), nullptr, &m_renderTargetView);
    if (FAILED(hr)) {
        LOG_ERROR("Renderer: CreateRenderTargetView after resize failed.");
        return;
    }
    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);

    LOG_INFO("Renderer: Resized to " +
             std::to_string(newW) + "x" + std::to_string(newH) + ".");
}

// ---------------------------------------------------------------------------
// UploadNV12Frame
//
// NV12 memory layout (from H264Decoder / DecodedFrame):
//   Y  plane: [0 .. width*height)           — 1 byte per luma sample
//   UV plane: [width*height .. width*height*3/2) — 2 bytes per 2x2 chroma block
//             interleaved U,V; width bytes per UV row = width/2 pairs * 2
//
// Upload strategy:
//   - Two separate DYNAMIC textures (R8_UNORM and R8G8_UNORM).
//   - Row-by-row copy to account for GPU pitch padding (RowPitch >= rowBytes).
// ---------------------------------------------------------------------------

bool Renderer::UploadNV12Frame(const DecodedFrame& frame) {
    if (frame.width == 0 || frame.height == 0 || frame.nv12Data.empty()) {
        LOG_WARN("Renderer: UploadNV12Frame: invalid frame (zero size or empty data).");
        return false;
    }

    const size_t expectedSize = static_cast<size_t>(frame.width) *
                                static_cast<size_t>(frame.height) * 3u / 2u;
    if (frame.nv12Data.size() < expectedSize) {
        LOG_WARN("Renderer: UploadNV12Frame: NV12 data too small ("
                 + std::to_string(frame.nv12Data.size()) + " bytes, need >= "
                 + std::to_string(expectedSize) + ").");
        return false;
    }

    // -----------------------------------------------------------------------
    // Recreate textures + SRVs if dimensions changed
    // -----------------------------------------------------------------------
    if (frame.width != m_videoWidth || frame.height != m_videoHeight) {
        ReleaseVideoResources();

        // Y texture: full resolution, one byte per sample
        D3D11_TEXTURE2D_DESC descY   = {};
        descY.Width                  = frame.width;
        descY.Height                 = frame.height;
        descY.MipLevels              = 1;
        descY.ArraySize              = 1;
        descY.Format                 = DXGI_FORMAT_R8_UNORM;
        descY.SampleDesc.Count       = 1;
        descY.Usage                  = D3D11_USAGE_DYNAMIC;
        descY.BindFlags              = D3D11_BIND_SHADER_RESOURCE;
        descY.CPUAccessFlags         = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = m_device->CreateTexture2D(&descY, nullptr, &m_texY);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer: CreateTexture2D (Y plane) failed.");
            return false;
        }

        // UV texture: half resolution, two bytes per chroma sample pair
        D3D11_TEXTURE2D_DESC descUV  = {};
        descUV.Width                 = frame.width  / 2u;
        descUV.Height                = frame.height / 2u;
        descUV.MipLevels             = 1;
        descUV.ArraySize             = 1;
        descUV.Format                = DXGI_FORMAT_R8G8_UNORM;
        descUV.SampleDesc.Count      = 1;
        descUV.Usage                 = D3D11_USAGE_DYNAMIC;
        descUV.BindFlags             = D3D11_BIND_SHADER_RESOURCE;
        descUV.CPUAccessFlags        = D3D11_CPU_ACCESS_WRITE;

        hr = m_device->CreateTexture2D(&descUV, nullptr, &m_texUV);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer: CreateTexture2D (UV plane) failed.");
            m_texY.Reset();
            return false;
        }

        // SRV for Y plane
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY    = {};
        srvDescY.Format                             = DXGI_FORMAT_R8_UNORM;
        srvDescY.ViewDimension                      = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDescY.Texture2D.MipLevels                = 1;
        srvDescY.Texture2D.MostDetailedMip          = 0;
        hr = m_device->CreateShaderResourceView(m_texY.Get(), &srvDescY, &m_srvY);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer: CreateShaderResourceView (Y) failed.");
            m_texY.Reset(); m_texUV.Reset();
            return false;
        }

        // SRV for UV plane
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV   = {};
        srvDescUV.Format                            = DXGI_FORMAT_R8G8_UNORM;
        srvDescUV.ViewDimension                     = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDescUV.Texture2D.MipLevels               = 1;
        srvDescUV.Texture2D.MostDetailedMip         = 0;
        hr = m_device->CreateShaderResourceView(m_texUV.Get(), &srvDescUV, &m_srvUV);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer: CreateShaderResourceView (UV) failed.");
            m_texY.Reset(); m_texUV.Reset(); m_srvY.Reset();
            return false;
        }

        m_videoWidth  = frame.width;
        m_videoHeight = frame.height;
        LOG_INFO("Renderer: Video textures created for " +
                 std::to_string(frame.width) + "x" + std::to_string(frame.height) +
                 " NV12.");
    }

    // -----------------------------------------------------------------------
    // Upload Y plane — row by row (RowPitch may be > width)
    // -----------------------------------------------------------------------
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_context->Map(m_texY.Get(), 0,
                                    D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer: Map (Y plane) failed.");
            return false;
        }
        const uint8_t* srcY    = frame.nv12Data.data();
              uint8_t* dstY    = static_cast<uint8_t*>(mapped.pData);
        const UINT     rowBytesY = frame.width; // 1 byte / sample
        for (uint32_t row = 0; row < frame.height; ++row) {
            std::memcpy(dstY + static_cast<size_t>(row) * mapped.RowPitch,
                        srcY + static_cast<size_t>(row) * rowBytesY,
                        rowBytesY);
        }
        m_context->Unmap(m_texY.Get(), 0);
    }

    // -----------------------------------------------------------------------
    // Upload UV plane — row by row (each row = width bytes = width/2 U,V pairs)
    // -----------------------------------------------------------------------
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_context->Map(m_texUV.Get(), 0,
                                    D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            LOG_ERROR("Renderer: Map (UV plane) failed.");
            return false;
        }
        const uint8_t* srcUV   = frame.nv12Data.data() +
                                 static_cast<size_t>(frame.width) * frame.height;
              uint8_t* dstUV   = static_cast<uint8_t*>(mapped.pData);
        // UV row: width/2 chroma pairs × 2 bytes each = width bytes total
        const UINT rowBytesUV  = frame.width;
        for (uint32_t row = 0; row < frame.height / 2u; ++row) {
            std::memcpy(dstUV + static_cast<size_t>(row) * mapped.RowPitch,
                        srcUV + static_cast<size_t>(row) * rowBytesUV,
                        rowBytesUV);
        }
        m_context->Unmap(m_texUV.Get(), 0);
    }

    return true;
}

// ---------------------------------------------------------------------------
// ComputeLetterbox
//
// Returns a D3D11_VIEWPORT that centres srcW×srcH inside dstW×dstH while
// preserving the source aspect ratio (letterbox or pillarbox as needed).
// ---------------------------------------------------------------------------

D3D11_VIEWPORT Renderer::ComputeLetterbox(float srcW, float srcH,
                                          float dstW, float dstH) const {
    D3D11_VIEWPORT vp = {};
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    if (srcW <= 0.0f || srcH <= 0.0f || dstW <= 0.0f || dstH <= 0.0f) {
        vp.Width  = dstW;
        vp.Height = dstH;
        return vp;
    }

    const float scaleX = dstW / srcW;
    const float scaleY = dstH / srcH;
    const float scale  = (scaleX < scaleY) ? scaleX : scaleY;

    const float vpW = srcW * scale;
    const float vpH = srcH * scale;

    vp.TopLeftX = (dstW - vpW) * 0.5f;
    vp.TopLeftY = (dstH - vpH) * 0.5f;
    vp.Width    = vpW;
    vp.Height   = vpH;
    return vp;
}

// ---------------------------------------------------------------------------
// ReleaseVideoResources
// ---------------------------------------------------------------------------

void Renderer::ReleaseVideoResources() {
    m_srvUV.Reset();
    m_srvY.Reset();
    m_texUV.Reset();
    m_texY.Reset();
    m_videoWidth  = 0;
    m_videoHeight = 0;
}

} // namespace SanskyStream
