cmake_minimum_required(VERSION 3.15)
project(plex-tui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Find required packages
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CURL_INCLUDE_DIRS}
)

# Source files
set(SOURCES
    main.cpp
    terminal.cpp
    input.cpp
    plex_client.cpp
    audio_decoder.cpp
    player_view.cpp
    waveform.cpp
    config.cpp
)

# Create executable
add_executable(plex-tui ${SOURCES})

# Link libraries
target_link_libraries(plex-tui
    ${CURL_LIBRARIES}
    Threads::Threads
)

# Check for ffmpeg (optional, for audio decoding)
find_program(FFMPEG_EXECUTABLE ffmpeg)
if(FFMPEG_EXECUTABLE)
    message(STATUS "Found ffmpeg: ${FFMPEG_EXECUTABLE}")
else()
    message(WARNING "ffmpeg not found - audio decoding will not work")
endif()

# Install target
install(TARGETS plex-tui DESTINATION bin)
