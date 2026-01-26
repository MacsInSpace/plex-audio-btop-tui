#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace PlexTUI {

// Core data structures
// Time-synced lyric line
struct LyricLine {
    uint32_t timestamp_ms;    // Timestamp in milliseconds
    std::string text;         // Lyric text
    
    LyricLine(uint32_t ts = 0, const std::string& txt = "") : timestamp_ms(ts), text(txt) {}
};

struct Track {
    std::string id;           // ratingKey
    std::string title;
    std::string artist;
    std::string album;
    uint32_t duration_ms;
    std::string art_url;
    std::string media_url;
    std::string thumb_url;     // Album art thumbnail
    int year = 0;
    std::string genre;
    int bitrate = 0;
    std::string codec;
    std::string lyrics;        // Lyrics text (from Plex or external API)
    std::vector<LyricLine> synced_lyrics;  // Time-synced lyrics from LRC file
    
    // Helper to get best art URL
    std::string get_art_url() const {
        return !thumb_url.empty() ? thumb_url : art_url;
    }
};

struct PlaybackState {
    bool playing = false;
    bool paused = false;
    uint32_t position_ms = 0;
    float volume = 1.0f;
    Track current_track;
    
    // PLACEHOLDER: Queue management
    // std::vector<Track> queue;
    // int queue_position;
};

struct AudioLevels {
    std::vector<float> waveform_data;  // Recent audio levels for visualization
    float current_level = 0.0f;
    float peak_level = 0.0f;
    
    // PLACEHOLDER: Spectrum analyzer data
    // std::vector<float> frequency_bands;
};

// Color theme - btop-inspired vibrant colors
struct Theme {
    struct RGB {
        uint8_t r, g, b;
        RGB(uint8_t r = 0, uint8_t g = 0, uint8_t b = 0) : r(r), g(g), b(b) {}
    };
    
    // Background - pure black
    RGB background{0, 0, 0};
    
    // Text colors
    RGB foreground{200, 200, 200};      // Light gray text
    RGB dimmed{100, 100, 100};           // Dimmed text
    RGB bright{255, 255, 255};           // Bright white
    
    // Accent colors - vibrant btop-style
    RGB highlight{100, 200, 255};       // Bright cyan
    RGB accent{255, 100, 150};           // Pink/magenta
    RGB success{100, 255, 150};          // Green
    RGB warning{255, 200, 100};          // Yellow/orange
    RGB error{255, 100, 100};             // Red
    
    // Waveform colors - gradient from cyan to magenta to yellow
    RGB waveform_primary{100, 200, 255};    // Cyan
    RGB waveform_secondary{255, 100, 200};  // Magenta
    RGB waveform_tertiary{255, 200, 100};  // Yellow
    
    // List colors
    RGB selected{100, 150, 255};         // Blue selection
    RGB playing{100, 255, 150};          // Green for playing track
    RGB queued{255, 200, 100};           // Yellow for queued
    
    // Box borders
    RGB border{80, 80, 80};              // Dark gray borders
    RGB border_bright{150, 150, 150};    // Bright borders
};

// Configuration
struct Config {
    std::string plex_server_url;
    std::string plex_token;
    int max_waveform_points = 100;
    int refresh_rate_ms = 250;  // 4 FPS - btop-style smooth rendering, good for waveforms
    int window_width = 145;     // Default terminal window width (columns)
    int window_height = 40;     // Default terminal window height (rows)
    Theme theme;
    
    // Feature toggles
    bool enable_waveform = true;        // Show waveform visualization (default: on)
    bool enable_lyrics = true;          // Fetch and display lyrics (default: on)
    bool enable_album_art = true;       // Fetch and display album art (default: on)
    bool enable_album_data = false;     // Fetch album data from MusicBrainz etc (default: off)
    bool enable_debug_logging = false;  // Enable debug logging to stderr and log file (default: off)
    std::string debug_log_file_path;    // Path to debug log file (default: next to config.ini)
    
    // PLACEHOLDER: User preferences
    // - keybindings, library filters, display options
    
    bool load_from_file(const std::string& path);
    bool save_to_file(const std::string& path);
};

} // namespace PlexTUI
