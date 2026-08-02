#include "Renderer.h"
#include "Logger.h"

#pragma comment(lib, "d3d11.lib")

namespace SanskyStream {

Renderer::Renderer(Window* window) : m_window(window) {
}

Renderer::~Renderer() {
    LOG_INFO("Renderer shutting down.");
}

bool Renderer::Initialize() {
    if (!m_window || !m_window->GetHWND()) {
        LOG_ERROR("Invalid window provided to Renderer.");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width = m_window->GetWidth();
    scd.BufferDesc.Height = m_window->GetHeight();
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = m_window->GetHWND();
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &scd,
        &m_swapChain,
        &m_device,
        nullptr,
        &m_context
    );

    if (FAILED(hr)) {
        LOG_ERROR("Failed to create D3D11 device and swap chain.");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> pBackBuffer;
    hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to get swap chain back buffer.");
        return false;
    }

    hr = m_device->CreateRenderTargetView(pBackBuffer.Get(), nullptr, &m_renderTargetView);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create render target view.");
        return false;
    }

    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);

    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = static_cast<float>(m_window->GetWidth());
    viewport.Height = static_cast<float>(m_window->GetHeight());
    m_context->RSSetViewports(1, &viewport);

    LOG_INFO("Renderer initialized successfully.");
    return true;
}

void Renderer::ClearColor(float r, float g, float b, float a) {
    if (!m_context || !m_renderTargetView) return;
    float color[4] = { r, g, b, a };
    m_context->ClearRenderTargetView(m_renderTargetView.Get(), color);
}

void Renderer::Render() {
    if (!m_swapChain) return;

    // TODO: Draw actual video textures here

    m_swapChain->Present(1, 0); // VSync enabled
}

} // namespace SanskyStream
