#include "App.h"
#include "Logger.h"

namespace SanskyStream {

App::App() : m_isRunning(true) {
    LOG_INFO("Initializing Application...");
    
    // Create the main window
    m_window = std::make_unique<Window>(1280, 720, L"SanskyStream Client");
    if (!m_window->GetHWND()) {
        LOG_ERROR("Failed to initialize main window.");
        m_isRunning = false;
        return;
    }

    // Initialize Renderer
    m_renderer = std::make_unique<Renderer>(m_window.get());
    if (!m_renderer->Initialize()) {
        LOG_ERROR("Failed to initialize renderer.");
        m_isRunning = false;
        return;
    }

    // Initialize Decoder
    m_decoder = std::make_shared<Decoder>();
    m_decoder->InitializeVideoDecoder();
    m_decoder->InitializeAudioDecoder();

    // Initialize Receivers
    m_videoReceiver = std::make_shared<VideoReceiver>(m_decoder);
    m_audioReceiver = std::make_shared<AudioReceiver>(m_decoder);

    // Initialize Network
    m_network = std::make_unique<Network>(m_videoReceiver, m_audioReceiver);
    // TODO: Change to real server IP/Port
    // m_network->Connect("127.0.0.1", 8080);
}

App::~App() {
    if (m_network) {
        m_network->Disconnect();
    }
    LOG_INFO("Application shutting down.");
}

void App::Run() {
    if (!m_isRunning) return;

    LOG_INFO("Application entering main loop.");

    while (m_isRunning) {
        // Process Windows messages
        if (!m_window->ProcessMessages()) {
            // WM_QUIT received
            m_isRunning = false;
            break;
        }

        // Render Frame
        m_renderer->ClearColor(0.1f, 0.2f, 0.4f, 1.0f); // Blueish clear color
        m_renderer->Render();
    }
}

} // namespace SanskyStream
