cmake_minimum_required(VERSION 3.20)
project(SanskyStream_Windows VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add source files
set(SOURCES
    src/main.cpp
    src/App.cpp
    src/Window.cpp
    src/Network.cpp
    src/VideoReceiver.cpp
    src/AudioReceiver.cpp
    src/Decoder.cpp
    src/Renderer.cpp
    src/Logger.cpp
)

# Executable with Windows subsystem (no console window in release, but we might want console for debugging now)
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

# Compiler specific options
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX /EHsc)
    target_compile_definitions(${PROJECT_NAME} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
