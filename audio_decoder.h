#pragma once

#include "types.h"
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>

namespace PlexTUI {

/**
 * Audio decoder for client-side waveform generation
 * Decodes audio streams and extracts PCM data for visualization
 */
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    
    // Start decoding audio from a URL (Plex stream URL)
    bool start_decoding(const std::string& audio_url, const std::string& plex_token);
    
    // Stop decoding
    void stop_decoding();
    
    // Get recent audio samples for waveform visualization
    // Returns RMS levels normalized to 0.0-1.0
    std::vector<float> get_waveform_samples(int count = 100);
    
    // Get current audio level (0.0-1.0)
    float get_current_level() const;
    
    // Check if decoding is active
    bool is_decoding() const { return decoding_active.load(); }
    
    // Pause/resume playback
    bool pause_playback();
    bool resume_playback();
    
private:
    // Decode audio using ffmpeg subprocess
    void decode_thread_func();
    
    // Process PCM data and calculate RMS levels
    void process_pcm_data(const std::vector<int16_t>& pcm_samples);
    
    std::atomic<bool> decoding_active{false};
    std::thread decode_thread;
    mutable std::mutex samples_mutex;  // Mutable so it can be locked in const methods
    
    // Rolling buffer of audio levels (RMS values)
    std::vector<float> waveform_samples;
    static constexpr size_t MAX_SAMPLES = 200;
    
    std::string current_url;
    std::string current_token;
    
    float current_level = 0.0f;
    
    // Process IDs for killing playback
    pid_t playback_pid = -1;
    pid_t waveform_pid = -1;
    bool is_paused = false;
};

/**
 * Album art fetcher and pixelator
 * Downloads album art from Plex and renders it as pixelated terminal art
 */
class AlbumArt {
public:
    AlbumArt();
    
    // Fetch album art from Plex API
    bool fetch_art(const std::string& plex_server, const std::string& token, 
                   const std::string& art_url);
    
    // Render pixelated album art to terminal
    // Returns the rendered art as a vector of colored strings
    std::vector<std::string> render_pixelated(int width, int height, 
                                              const Theme& theme);
    
    // Check if art is loaded
    bool has_art() const { return !art_data.empty(); }
    
    // Clear loaded art
    void clear();
    
private:
    // Download image data
    bool download_image(const std::string& url, const std::string& token);
    
    // Convert image to pixelated representation
    std::vector<std::vector<uint8_t>> pixelate_image(int width, int height);
    
    std::vector<uint8_t> art_data;  // Raw image data (JPEG/PNG)
    int image_width = 0;
    int image_height = 0;
    
    // Use stb_image for decoding (or fallback to system tools)
    bool decode_image();
    std::vector<uint8_t> decoded_rgb;  // RGB24 data
};

} // namespace PlexTUI
