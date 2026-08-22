#pragma once

#include "Window.h"
#include "DecodedFrame.h"
#include "VideoFrameQueue.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

namespace SanskyStream {

// Forward declaration — keeps AVSynchronizer.h / windows.h out of Renderer.h.
class AVSynchronizer;

// ---------------------------------------------------------------------------
// Renderer  (M8)
//
// Direct3D 11 renderer that displays decoded H.264 video frames.
//
// Pipeline per frame:
//   VideoFrameQueue::TryPop()
//       -> UploadNV12Frame()   (CPU -> GPU: Y + UV dynamic textures)
//       -> Draw(6, 0)          (full-screen quad via SV_VertexID)
//       -> GPU NV12->RGB       (BT.601 pixel shader)
//       -> ComputeLetterbox()  (aspect-ratio preserving viewport)
//       -> Present(0,0)        (no VSync, minimum latency)
//
// Threading:
//   Render() runs on the main/UI thread.
//   SetFrameQueue() must be called before the render loop starts.
//   VideoFrameQueue::TryPop() is thread-safe (O(1) mutex swap).
// ---------------------------------------------------------------------------
class Renderer {
public:
    explicit Renderer(Window* window);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Initialize D3D11 device, swap chain, shaders, and sampler.
    // Must be called once before Render().
    bool Initialize();

    // Connect the VideoFrameQueue that supplies decoded frames.
    // Call after Initialize(), before the render loop.
    void SetFrameQueue(VideoFrameQueue* queue);

    // Connect the A/V synchronizer (M12).
    // When set, sync statistics are included in the FPS overlay (~1 Hz).
    // Pass nullptr to disable (pre-M12 behaviour).
    void SetAVSync(AVSynchronizer* sync);

    // Handle a window resize event.
    // Called automatically inside Render() via Window::TakeResizePending().
    // May also be called externally if needed.
    void HandleResize(int newW, int newH);

    // One render frame: pop frame, upload NV12, draw letterbox quad, present.
    void Render();

    // Fill the back buffer with a solid color (kept for API compatibility).
    void ClearColor(float r, float g, float b, float a);

    // Query current video state.
    float    GetFPS()      const { return m_fps;      }
    bool     HasVideo()    const { return m_hasVideo;  }
    uint32_t VideoWidth()  const { return m_videoWidth;  }
    uint32_t VideoHeight() const { return m_videoHeight; }

private:
    // Compile inline HLSL shaders and create ID3D11VertexShader / PixelShader.
    bool InitShaders();

    // Upload one NV12 DecodedFrame to the Y + UV GPU textures.
    // Creates new textures if dimensions changed.
    // Returns false and logs an error if the upload fails.
    bool UploadNV12Frame(const DecodedFrame& frame);

    // Compute a D3D11_VIEWPORT that letterboxes srcW×srcH inside dstW×dstH
    // while preserving the source aspect ratio.
    D3D11_VIEWPORT ComputeLetterbox(float srcW, float srcH,
                                    float dstW, float dstH) const;

    // Release NV12 textures and SRVs (called on resolution change or shutdown).
    void ReleaseVideoResources();

    // -----------------------------------------------------------------------
    // D3D11 core
    // -----------------------------------------------------------------------
    Window*  m_window;

    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain>         m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;

    // -----------------------------------------------------------------------
    // Video pipeline resources
    // -----------------------------------------------------------------------
    Microsoft::WRL::ComPtr<ID3D11VertexShader>       m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>        m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>       m_sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>    m_rasterizerState;

    // Y plane:  DXGI_FORMAT_R8_UNORM,   full resolution (width x height)
    // UV plane: DXGI_FORMAT_R8G8_UNORM, half resolution (width/2 x height/2)
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_texY;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_texUV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srvY;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srvUV;

    // -----------------------------------------------------------------------
    // Frame queue (non-owning, owned by App)
    // -----------------------------------------------------------------------
    VideoFrameQueue* m_frameQueue = nullptr;

    // Non-owning pointer to the AVSynchronizer (owned by App) — M12.
    // Used only for stats display in the FPS overlay.
    AVSynchronizer* m_avSync = nullptr;

    // -----------------------------------------------------------------------
    // Video display state
    // -----------------------------------------------------------------------
    uint32_t m_videoWidth  = 0;
    uint32_t m_videoHeight = 0;
    bool     m_hasVideo    = false;

    // -----------------------------------------------------------------------
    // FPS measurement (QueryPerformanceCounter, updated at ~1 Hz)
    // -----------------------------------------------------------------------
    LARGE_INTEGER m_perfFreq      = {};
    LARGE_INTEGER m_fpsLastTime   = {};
    uint32_t      m_fpsFrameCount = 0;
    float         m_fps           = 0.0f;
};

} // namespace SanskyStream
