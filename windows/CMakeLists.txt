cmake_minimum_required(VERSION 3.20)
project(SanskyStream_Windows VERSION 1.0.0 LANGUAGES C CXX)

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
    src/AVSynchronizer.cpp       # M12: audio-anchored A/V sync
    src/Decoder.cpp
    src/Renderer.cpp
    src/OBSBridge.cpp            # M14: named shmem IPC bridge to OBS plugin
    src/DeviceDiscovery.cpp      # M16: local-network mDNS/DNS-SD discovery
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
    dnsapi              # M16: DnsServiceBrowse / DnsServiceRegister / DnsServiceResolve
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
    src/AVSynchronizer.cpp       # M12: VideoReceiver now calls CheckVideoFrame()
    src/OBSBridge.cpp            # M14: VideoReceiver now calls OBSBridge::PushVideoFrame()
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
    src/AVSynchronizer.cpp       # M12: VideoReceiver now calls CheckVideoFrame()
    src/OBSBridge.cpp            # M14: VideoReceiver now calls OBSBridge::PushVideoFrame()
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
    src/AVSynchronizer.cpp       # M12: Renderer::Render() calls GetStats()
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
    src/AVSynchronizer.cpp       # M12: AudioReceiver now calls NotifyAudioTimestamp()
    src/OBSBridge.cpp            # M14: AudioReceiver now calls OBSBridge::PushAudioData()
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

# ---------------------------------------------------------------------------
# TestAVSync — M12 AVSynchronizer unit test harness.
#
# Tests the timing/scheduling logic of AVSynchronizer in isolation.
# No network, no real audio, no real video needed.
#
# Usage:
#   cmake --build . --target TestAVSync
#   .\TestAVSync.exe
# ---------------------------------------------------------------------------
add_executable(TestAVSync
    src/TestAVSync.cpp
    src/AVSynchronizer.cpp
    src/Logger.cpp
)

target_include_directories(TestAVSync PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_link_libraries(TestAVSync PRIVATE
    ws2_32
)

if(MSVC)
    target_compile_options(TestAVSync PRIVATE /W4 /WX /EHsc)
    target_compile_definitions(TestAVSync PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# TestDiscovery — M16 device discovery mock/controlled test harness.
#
# Tests DeviceDiscovery business logic without real mDNS or an iPhone.
# All 13 required test scenarios from the M16 spec are covered.
#
# Usage:
#   cmake --build . --config Release --target TestDiscovery
#   .\Release\TestDiscovery.exe
# ---------------------------------------------------------------------------
add_executable(TestDiscovery
    src/TestDiscovery.cpp
    src/DeviceDiscovery.cpp
    src/Logger.cpp
)

target_include_directories(TestDiscovery PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_link_libraries(TestDiscovery PRIVATE
    ws2_32
    dnsapi
)

if(MSVC)
    target_compile_options(TestDiscovery PRIVATE /W4 /WX /EHsc)
    target_compile_definitions(TestDiscovery PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# sansky-source -- OBS Studio async source plugin (M14)
#
# Runs inside obs64.exe as obs-plugins/64bit/sansky-source.dll.
# Reads video (NV12) and audio (PCM16) from the named shared memory segment
# created by SanskyStream.exe via OBSBridge, then calls:
#   obs_source_output_video2()  -- async NV12 video
#   obs_source_output_audio()   -- PCM16 stereo audio
#
# Prerequisites:
#   Run setup-obs-sdk.ps1 to populate obs-sdk/libobs/ and obs-sdk/obs.lib.
#
# Usage:
#   cmake --build . --config Release --target sansky-source
#   .\install-plugin.ps1   (run as Administrator)
# ---------------------------------------------------------------------------
option(BUILD_OBS_PLUGIN "Build the OBS Studio sansky-source plugin" ON)

if(BUILD_OBS_PLUGIN)
    set(OBS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/obs-sdk"
        CACHE PATH "Path to the OBS SDK directory (contains obs.lib and libobs/)")

    if(NOT EXISTS "${OBS_SDK_DIR}/obs.lib")
        message(WARNING
            "[sansky-source] obs.lib not found at '${OBS_SDK_DIR}/obs.lib'.\n"
            "Run setup-obs-sdk.ps1 first. Skipping sansky-source plugin target.")
    else()
        add_library(sansky-source SHARED
            src/sansky-source.c
        )

        target_include_directories(sansky-source PRIVATE
            ${OBS_SDK_DIR}/libobs       # obs.h, obs-source.h, obs-module.h, media-io/
            ${CMAKE_CURRENT_SOURCE_DIR}/include  # OBSBridge.h constants (shmem layout)
        )

        target_link_libraries(sansky-source PRIVATE
            "${OBS_SDK_DIR}/obs.lib"   # import lib generated by setup-obs-sdk.ps1
        )

        # Output the DLL into a dedicated subdirectory so install-plugin.ps1
        # can find it without hunting through multi-config build trees.
        set_target_properties(sansky-source PROPERTIES
            OUTPUT_NAME   "sansky-source"
            LINKER_LANGUAGE C
            RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/obs-plugin"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/obs-plugin"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/obs-plugin"
        )

        if(MSVC)
            # W3 (not W4/WX) -- libobs C headers generate level-4 warnings we don't own
            target_compile_options(sansky-source PRIVATE /W3 /EHsc)
            target_compile_definitions(sansky-source PRIVATE
                WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
        endif()

        message(STATUS "[sansky-source] OBS plugin target configured. "
                       "obs.lib = ${OBS_SDK_DIR}/obs.lib")
    endif()
endif()
