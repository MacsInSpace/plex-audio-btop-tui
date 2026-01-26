# Project Documentation

## Build System

The project uses a Makefile-based build system (`Makefile.main`). CMake support is also available via `CMakeLists.txt`.

### Compiler Requirements

- C++17 standard
- GCC 7+ or Clang 5+ recommended
- Compiler flags: `-std=c++17 -Wall -Wextra -O2 -pthread`

### Build Commands

```bash
# Build using Makefile
make -f Makefile.main

# Clean build artifacts
make -f Makefile.main clean

# Build using CMake (alternative)
mkdir build && cd build
cmake ..
make
```

### Build Output

- Object files: `build/*.o`
- Executable: `bin/plex-tui`
- Pre-built executable: `bin/plex-tui` (macOS ARM64 only)

Note: The included `bin/plex-tui` executable is built for macOS ARM64 systems only. For other platforms (Linux, macOS Intel, Windows), you must build from source.

## Dependencies

### Build-Time Dependencies

- **C++ Compiler**: g++ or clang++ with C++17 support
- **libcurl**: Development headers and library
  - Ubuntu/Debian: `sudo apt-get install libcurl4-openssl-dev`
  - macOS: `brew install curl` (usually pre-installed)
  - Fedora: `sudo dnf install libcurl-devel`
- **pthread**: POSIX threads library (usually included with compiler)

### Runtime Dependencies

- **ffmpeg**: Audio decoding and waveform generation
  - Ubuntu/Debian: `sudo apt-get install ffmpeg`
  - macOS: `brew install ffmpeg`
  - Fedora: `sudo dnf install ffmpeg`
- **ffplay**: Audio playback (part of ffmpeg package)
- **curl**: Command-line tool for lyrics API calls (usually pre-installed)

### Linking

The project links against:
- `libcurl` - HTTP client library for Plex API and external APIs
- `pthread` - POSIX threads for multi-threaded lyrics fetching

## Data Sources

### Plex Server API

The application communicates with your Plex server to retrieve:

- **Library Data**: Artists, albums, tracks, playlists
- **Audio Streams**: Direct audio stream URLs for playback
- **Album Art**: Album cover images
- **Metadata**: Track titles, artists, durations, etc.

All Plex API requests require:
- Server URL (e.g., `http://your-server:32400`)
- Authentication token (X-Plex-Token header)

### External APIs

#### Lyrics Services

1. **LRCLIB API** (Primary)
   - URL: `https://lrclib.net/api/get`
   - Purpose: Time-synced lyrics in LRC format
   - Parameters: `track_name`, `artist_name`, `album_name`, `duration`
   - Response: JSON with `syncedLyrics` (LRC format) or `plainLyrics` (fallback)
   - Used via: `curl` command-line tool (subprocess)

2. **lyrics.ovh API** (Fallback)
   - URL: `https://api.lyrics.ovh/v1/{artist}/{title}`
   - Purpose: Plain text lyrics when LRCLIB fails
   - Response: JSON with `lyrics` field
   - Used via: `curl` command-line tool (subprocess)

Lyrics fetching is performed asynchronously in a background thread to avoid blocking the UI.

#### MusicBrainz API (Optional)

- URL: `https://musicbrainz.org/ws/2/release/`
- Purpose: Additional album metadata (release date, label, country, format, barcode)
- Enabled via: `enable_album_data` configuration option
- Requires: User-Agent header (set to application name)
- Used via: libcurl (synchronous)

## Architecture

### Core Components

- **main.cpp**: Application entry point, signal handling, main loop
- **terminal.cpp/h**: Terminal rendering and control (ANSI escape codes, true color)
- **input.cpp/h**: Keyboard and mouse input handling
- **plex_client.cpp/h**: Plex API client and external API integration
- **player_view.cpp/h**: Main UI rendering and state management
- **audio_decoder.cpp/h**: Audio decoding via ffmpeg subprocess
- **waveform.cpp/h**: Waveform visualization rendering
- **config.cpp**: Configuration file parsing (INI format)
- **plex_xml.cpp/h**: XML parsing for Plex API responses
- **types.h**: Core data structures

### Threading Model

- **Main Thread**: UI rendering, input handling, playback control
- **Lyrics Thread**: Asynchronous lyrics fetching from external APIs
  - Uses subprocess (`popen`) for curl calls to avoid libcurl thread-safety issues
  - Queue-based request system with mutex protection

### Audio Playback

Audio playback is handled via `ffplay` subprocess:
- Stream URL obtained from Plex API
- ffplay invoked with appropriate arguments
- Process management for play/pause/stop

### Waveform Generation

Waveform data is generated via `ffmpeg` subprocess:
- Audio file decoded to raw PCM
- Amplitude levels extracted and cached
- Rendered in real-time using block characters

## Configuration

Configuration is stored in `~/.config/plex-tui/config.ini` (INI format).

### Sections

- `[plex]`: Server URL and authentication token
- `[display]`: Window size, refresh rate, waveform points
- `[features]`: Feature toggles (waveform, lyrics, album art, debug logging)

See `config.example.ini` for all available options and defaults.

## File Structure

```
plex-audio-btop-tui/
├── bin/                    # Executable output (macOS ARM64 pre-built included)
├── build/                  # Object files (build artifacts)
├── *.cpp, *.h             # Source files
├── Makefile.main           # Primary build system
├── CMakeLists.txt         # CMake build configuration
├── build.sh               # CMake build script
├── config.example.ini     # Configuration template
├── README.md              # Quick start guide
├── PROJECT.md             # This file
└── LICENSE                # MIT License
```

## Development Notes

### Code Style

- C++17 standard
- Snake_case for functions and variables
- PascalCase for classes
- Header guards: `#pragma once`
- Namespace: `PlexTUI`

### Error Handling

- Exceptions used for critical errors
- Return codes for recoverable errors
- Logging respects `enable_debug_logging` configuration

### Performance Optimizations

- Frame rate limiting (configurable refresh rate)
- Lazy loading for large libraries
- Virtual scrolling for lists
- Audio level caching
- Throttled API calls (lyrics fetching)

## Platform Support

- **macOS**: Tested on ARM64 (Apple Silicon)
- **Linux**: Should work with standard POSIX tools
- **Windows**: Not tested (requires POSIX compatibility layer)

## Troubleshooting

### Build Issues

- Ensure all dependencies are installed (libcurl, compiler with C++17)
- Check that `pthread` is available
- Verify `ffmpeg` and `ffplay` are in PATH

### Runtime Issues

- Verify Plex server is accessible and token is valid
- Check that `ffmpeg` and `ffplay` are in PATH
- Enable debug logging in config for detailed error messages
- Check debug log file (default: `~/.config/plex-tui/debug.log`)

### Lyrics Not Appearing

- Check internet connectivity (lyrics fetched from external APIs)
- Verify track has artist and title metadata
- Enable debug logging to see API responses
- LRCLIB requires track duration for time-synced lyrics
