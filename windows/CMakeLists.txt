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
    src/AudioReceiver.cpp
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
)

# Compiler-specific options
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX /EHsc)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# TestVideoSender — standalone loopback test tool (M6 transport layer).
# Sends fake H.264-like UDP datagrams to 127.0.0.1:5001.
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
# Run first, then TestVideoSender in a second terminal.
#
# M8 fix: added H264Decoder.cpp, VideoFrameQueue.cpp, and MF libs because
# VideoReceiver (M7) now owns an H264Decoder and (M8) uses VideoFrameQueue.
# ---------------------------------------------------------------------------
add_executable(TestReceiver
    src/TestReceiver.cpp
    src/VideoUdpReceiver.cpp
    src/FrameAssembler.cpp
    src/VideoReceiver.cpp
    src/VideoFrameQueue.cpp      # M8: VideoReceiver depends on VideoFrameQueue
    src/H264Decoder.cpp          # M8: VideoReceiver depends on H264Decoder (M7)
    src/Logger.cpp
)

target_include_directories(TestReceiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_link_libraries(TestReceiver PRIVATE
    mf                           # M8: required by H264Decoder
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
#
# M8 fix: added VideoFrameQueue.cpp because VideoReceiver::OnDecodedFrame
# now references VideoFrameQueue::Push() (even when the pointer is null,
# the linker still needs the symbol resolved).
# ---------------------------------------------------------------------------
add_executable(TestDecoder
    src/TestDecoder.cpp
    src/H264Decoder.cpp
    src/VideoReceiver.cpp
    src/VideoFrameQueue.cpp      # M8: VideoReceiver depends on VideoFrameQueue
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
#
# Creates a real Window + D3D11 Renderer, pushes synthetic 320x240 NV12
# frames from a background thread at ~60 fps for 5 seconds, and confirms
# HasVideo() + FPS > 0.  No iPhone, no H.264 encoder needed.
#
# Usage:
#   cmake --build . --target TestRenderPipeline
#   .\TestRenderPipeline.exe
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
