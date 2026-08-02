#pragma once
#include "Window.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace SanskyStream {

class Renderer {
public:
    Renderer(Window* window);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Initialize();
    void Render();
    void ClearColor(float r, float g, float b, float a);

private:
    Window* m_window;
    
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
};

} // namespace SanskyStream
