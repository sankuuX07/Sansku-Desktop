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
    src/FrameAssembler.cpp
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
# TestVideoSender — standalone loopback test tool (not part of production).
# Sends fake H.264-like UDP datagrams to 127.0.0.1:5001 using the M6
# protocol format.  Compile and run separately to validate VideoUdpReceiver.
#
# Usage:
#   cmake --build . --target TestVideoSender
#   .\TestVideoSender.exe [host] [port]   (defaults: 127.0.0.1  5001)
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
# TestReceiver — headless console harness for end-to-end M6 pipeline testing.
# Links only the transport stack (no Window/Renderer/Decoder/Audio).
# Run TestReceiver.exe first, then TestVideoSender.exe in a second terminal.
# ---------------------------------------------------------------------------
add_executable(TestReceiver
    src/TestReceiver.cpp
    src/VideoUdpReceiver.cpp
    src/FrameAssembler.cpp
    src/VideoReceiver.cpp
    src/Logger.cpp
)

target_include_directories(TestReceiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_link_libraries(TestReceiver PRIVATE ws2_32)

if(MSVC)
    target_compile_options(TestReceiver PRIVATE /W4 /WX /EHsc)
    target_compile_definitions(TestReceiver PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
