cmake_minimum_required(VERSION 3.20)
project(SanskyStream_Windows VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Main application sources
# ---------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/App.cpp
    src/Window.cpp
    src/Network.cpp
    src/VideoReceiver.cpp
    src/VideoUdpReceiver.cpp
    src/VideoFrameQueue.cpp      # M8: added (was missing from production target)
    src/FrameAssembler.cpp
    src/H264Decoder.cpp
    src/AudioReceiver.cpp        # M11: rewired to AACDecoder + AudioPlayer
    src/AACDecoder.cpp           # M11: MFT AAC decoder
    src/AudioPlayer.cpp          # M11: WASAPI low-latency playback
    src/Decoder.cpp
    src/Renderer.cpp
    src/Logger.cpp
)

# Executable with Windows subsystem
add_executable(${PROJECT_NAME} ${SOURCES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

# Link Windows libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    user32
    gdi32
    d3d11
    d3dcompiler
    mf
    mfplat
    mfreadwrite
    mfuuid
    ws2_32
    ole32               # M11: required by WASAPI (CoInitializeEx, CoCreateInstance)
)

# Compiler-specific options
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX /EHsc)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# TestVideoSender — standalone loopback test tool (M6 transport layer).
# ---------------------------------------------------------------------------
add_executable(TestVideoSender
    src/TestVideoSender.cpp
    src/Logger.cpp
)

target_include_directories(TestVideoSender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_link_libraries(TestVideoSender PRIVATE ws2_32)

if(MSVC)
    target_compile_options(TestVideoSender PRIVATE /W4 /WX /EHsc)
    target_compile_definitions(TestVideoSender PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# TestReceiver — headless console harness for M6 pipeline testing.
# ---------------------------------------------------------------------------
add_executable(TestReceiver
    src/TestReceiver.cpp
    src/VideoUdpReceiver.cpp
    src/FrameAssembler.cpp
    src/VideoReceiver.cpp
    src/VideoFrameQueue.cpp
    src/H264Decoder.cpp
    src/Logger.cpp
)

target_include_directories(TestReceiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_link_libraries(TestReceiver PRIVATE
    mf
    mfplat
    mfuuid
    ws2_32
)

if(MSVC)
    target_compile_options(TestReceiver PRIVATE /W4 /WX /EHsc)
    target_compile_definitions(TestReceiver PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# TestDecoder — headless console harness for M7 H.264 decoder pipeline.
# ---------------------------------------------------------------------------
add_executable(TestDecoder
    src/TestDecoder.cpp
    src/H264Decoder.cpp
    src/VideoReceiver.cpp
    src/VideoFrameQueue.cpp
    src/FrameAssembler.cpp
    src/Logger.cpp
)

target_include_directories(TestDecoder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_link_libraries(TestDecoder PRIVATE
    mf
    mfplat
    mfuuid
    ws2_32
)

if(MSVC)
    target_compile_options(TestDecoder PRIVATE /W4 /WX /EHsc)
    target_compile_definitions(TestDecoder PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# TestRenderPipeline — M8 render-pipeline test.
# ---------------------------------------------------------------------------
add_executable(TestRenderPipeline
    src/TestRenderPipeline.cpp
    src/Window.cpp
    src/Renderer.cpp
    src/VideoFrameQueue.cpp
    src/Logger.cpp
)

target_include_directories(TestRenderPipeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_link_libraries(TestRenderPipeline PRIVATE
    user32
    gdi32
    d3d11
    d3dcompiler
)

if(MSVC)
    target_compile_options(TestRenderPipeline PRIVATE /W4 /WX /EHsc)
    target_compile_definitions(TestRenderPipeline PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# TestAudioPipeline — M11 audio pipeline test harness.
#
# Tests AACDecoder, AudioPlayer, and AudioReceiver in isolation.
# No iPhone, no H.264 encoder needed.
# Requires: Media Foundation (mf, mfplat, mfuuid) + WASAPI (ole32).
#
# Usage:
#   cmake --build . --target TestAudioPipeline
#   .\TestAudioPipeline.exe
# ---------------------------------------------------------------------------
add_executable(TestAudioPipeline
    src/TestAudioPipeline.cpp
    src/AACDecoder.cpp
    src/AudioPlayer.cpp
    src/AudioReceiver.cpp
    src/Logger.cpp
)

target_include_directories(TestAudioPipeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_link_libraries(TestAudioPipeline PRIVATE
    mf
    mfplat
    mfuuid
    ws2_32
    ole32
)

if(MSVC)
    target_compile_options(TestAudioPipeline PRIVATE /W4 /WX /EHsc)
    target_compile_definitions(TestAudioPipeline PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
