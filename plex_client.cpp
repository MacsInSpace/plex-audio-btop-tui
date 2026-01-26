#include "plex_client.h"
#include <iostream>
#include "audio_decoder.h"
#include "plex_xml.h"
#include <curl/curl.h>
#include <random>
#include <cmath>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <queue>
#include <map>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <array>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <errno.h>
#include <cstring>
#ifdef __APPLE__
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#elif __linux__
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#endif

namespace PlexTUI {

// Respect config enable_debug_logging (set from PlexClient ctor). No logging when false.
static std::atomic<bool> g_lyrics_debug_logging{false};

// Global log file path (set from main.cpp via config)
static std::string g_debug_log_file_path;

void PlexClient::set_debug_log_file_path(const std::string& path) {
    g_debug_log_file_path = path;
}

static void log_lyrics_fetch(const std::string& message) {
    if (!g_lyrics_debug_logging.load()) return;
    
    // Determine log file path (use global if set, otherwise default)
    std::string log_file;
    if (!g_debug_log_file_path.empty()) {
        log_file = g_debug_log_file_path;
    } else {
        // Default: next to config.ini
        const char* home = getenv("HOME");
        if (home) {
            log_file = std::string(home) + "/.config/plex-tui/debug.log";
        } else {
            log_file = "debug.log";  // Fallback to current directory
        }
    }
    
    std::ofstream log(log_file, std::ios::app);
    if (log.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        char time_str[64];
        std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));
        log << "[" << time_str << "] [LYRICS] " << message << std::endl;
    }
    std::cerr << "[LYRICS] " << message << std::endl;
}

// Lyrics request structure for async fetching
struct LyricsRequest {
    std::string track_id;
    std::string artist;
    std::string title;
    std::string album;
    uint32_t duration_seconds;  // Duration in seconds for LRCLIB API
    
    LyricsRequest(const std::string& id, const std::string& art, const std::string& tit, 
                  const std::string& alb = "", uint32_t dur_sec = 0)
        : track_id(id), artist(art), title(tit), album(alb), duration_seconds(dur_sec) {}
};

// Pimpl for hiding CURL implementation details
struct PlexClient::Impl {
    CURL* curl = nullptr;
    std::string response_buffer;
    
    AudioLevels audio_levels;
    
    // Mutex to protect playback state from concurrent access
    mutable std::mutex playback_mutex;
    
    // Simulated playback state for demo
    bool is_playing = false;
    uint32_t position = 0;
    Track current_track;
    std::chrono::steady_clock::time_point playback_start_time;
    
    // Async lyrics fetching infrastructure
    std::thread lyrics_thread;
    std::mutex lyrics_mutex;
    std::condition_variable lyrics_cv;
    std::queue<LyricsRequest> lyrics_queue;
    std::map<std::string, std::string> lyrics_results;  // track_id -> lyrics
    std::map<std::string, std::vector<LyricLine>> synced_lyrics_results;  // track_id -> time-synced lyrics
    std::map<std::string, bool> lyrics_in_progress;     // track_id -> fetching status
    bool lyrics_thread_running = false;
    // No curl handle needed - we use subprocess (popen) for thread-safe lyrics fetching
    
    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
    
    // Static write callback for lyrics thread (thread-safe)
    // This must be a plain C function pointer - no lambda captures
    static size_t lyrics_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        // Validate all inputs
        if (!userp || !contents) {
            return 0;
        }
        if (size == 0 || nmemb == 0) {
            return 0;  // Nothing to write
        }
        
        // Cast user pointer to string pointer
        std::string* data = static_cast<std::string*>(userp);
        if (!data) {
            return 0;
        }
        
        // Validate size to prevent overflow
        size_t total_size = size * nmemb;
        if (total_size == 0 || total_size > 10 * 1024 * 1024) {
            return 0;  // Too large or invalid
        }
        
        // Check current size to prevent excessive growth
        if (data->length() > 10 * 1024 * 1024) {
            return 0;  // Already too large
        }
        
        // Append data with exception handling
        try {
            const char* src = static_cast<const char*>(contents);
            data->append(src, total_size);
            return total_size;
        } catch (const std::bad_alloc&) {
            // Out of memory - stop transfer
            return 0;
        } catch (...) {
            // Any other exception - stop transfer
            return 0;
        }
    }
    
    // Lyrics fetching thread function
    void lyrics_thread_func() {
        // No curl handle needed - we use subprocess (popen) for lyrics fetching
        // This avoids all thread-safety issues with libcurl
        
        while (lyrics_thread_running) {
            bool has_request = false;
            LyricsRequest request("", "", "", "");
            
            // Wait for a request (with timeout for responsive shutdown)
            {
                std::unique_lock<std::mutex> lock(lyrics_mutex);
                // Use timeout so we can check thread_running more frequently for smooth quit
                lyrics_cv.wait_for(lock, std::chrono::milliseconds(100), [this] { 
                    return !lyrics_queue.empty() || !lyrics_thread_running; 
                });
                
                if (!lyrics_thread_running) {
                    log_lyrics_fetch("Thread shutdown signal received, exiting loop");
                    break;
                }
                
                if (!lyrics_queue.empty()) {
                    request = lyrics_queue.front();
                    lyrics_queue.pop();
                    has_request = true;
                    
                    // Mark as in progress
                    lyrics_in_progress[request.track_id] = true;
                }
            }
            
            if (has_request) {
                // Check again before processing (in case shutdown happened while we had the lock)
                {
                    std::lock_guard<std::mutex> lock(lyrics_mutex);
                    if (!lyrics_thread_running) {
                        log_lyrics_fetch("Thread shutdown detected before processing request");
                        break;
                    }
                }
                
                // First, try LRCLIB API (time-synced lyrics)
                std::vector<LyricLine> synced_lyrics;
                std::string lyrics;
                
                std::string lrclib_result = fetch_lrclib_lyrics(request);
                if (!lrclib_result.empty()) {
                    // Parse LRC format from LRCLIB response
                    log_lyrics_fetch("Calling parse_lrc_format with " + std::to_string(lrclib_result.length()) + " chars");
                    synced_lyrics = parse_lrc_format(lrclib_result);
                    if (!synced_lyrics.empty()) {
                        log_lyrics_fetch("SOURCE: LRCLIB API (time-synced) - " + request.title + " by " + request.artist + " (" + std::to_string(synced_lyrics.size()) + " lines)");
                    } else {
                        // LRCLIB returned something but couldn't parse - treat as plain text fallback
                        lyrics = lrclib_result;
                        log_lyrics_fetch("LRCLIB returned lyrics but not in parseable LRC format, using as plain text");
                    }
                } else {
                    // LRCLIB failed, fallback to lyrics.ovh (non-time-synced, manual scrolling)
                    lyrics = fetch_lyrics_for_request(request);
                }
                
                // Store result (even if empty - indicates fetch completed)
                {
                    std::lock_guard<std::mutex> lock(lyrics_mutex);
                    // Only store if thread is still running (avoid race condition)
                    if (lyrics_thread_running) {
                        lyrics_results[request.track_id] = lyrics;
                        synced_lyrics_results[request.track_id] = synced_lyrics;
                        lyrics_in_progress[request.track_id] = false;
                    }
                    // Note: If lyrics is empty, it means no lyrics were found
                    // This is different from "still fetching" (which would be in_progress = true)
                }
            }
        }
        
        // Thread is shutting down
        log_lyrics_fetch("Lyrics thread shutting down cleanly");
    }
    
    // Parse LRC format lyrics (from LRCLIB API)
    // Returns vector of time-synced lyric lines
    std::vector<LyricLine> parse_lrc_format(const std::string& lyrics_text) {
        std::vector<LyricLine> lines;
        if (lyrics_text.empty()) {
            log_lyrics_fetch("parse_lrc_format: Empty lyrics text");
            return lines;
        }
        
        // Check if it looks like LRC format (contains timestamp patterns like [mm:ss.xx])
        if (lyrics_text.find('[') == std::string::npos || lyrics_text.find(':') == std::string::npos) {
            log_lyrics_fetch("parse_lrc_format: Doesn't look like LRC format (no [ or :)");
            return lines;
        }
        
        // Log first 200 chars for debugging
        std::string preview = lyrics_text.substr(0, std::min(lyrics_text.length(), size_t(200)));
        log_lyrics_fetch("parse_lrc_format: Parsing LRC format (" + std::to_string(lyrics_text.length()) + " chars), preview: " + preview);
        
        // Parse as LRC format - handle both actual newlines and escaped \n
        std::string text = lyrics_text;
        // Replace escaped newlines with actual newlines if present
        size_t pos = 0;
        while ((pos = text.find("\\n", pos)) != std::string::npos) {
            text.replace(pos, 2, "\n");
            pos += 1;
        }
        
        std::istringstream stream(text);
        std::string line;
        int line_count = 0;
        while (std::getline(stream, line)) {
            line_count++;
            // Skip empty lines
            if (line.empty()) continue;
            
            // Skip metadata tags like [ar:Artist], [ti:Title], etc.
            // But NOT timestamps like [00:06.40] - timestamps have digits before the colon
            if (line.length() > 2 && line[0] == '[') {
                size_t colon_pos = line.find(':');
                size_t bracket_pos = line.find(']');
                if (colon_pos != std::string::npos && colon_pos < bracket_pos) {
                    // Check if it's a metadata tag (has letters before colon) vs timestamp (has digits)
                    // Metadata: [ar:Artist], [ti:Title] - has letters before colon
                    // Timestamp: [00:06.40] - has digits before colon
                    bool is_metadata = false;
                    if (colon_pos > 1) {
                        // Check characters between [ and : - if any are letters, it's metadata
                        for (size_t i = 1; i < colon_pos; ++i) {
                            if ((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z')) {
                                is_metadata = true;
                                break;
                            }
                        }
                    }
                    if (is_metadata) {
                        continue;  // Skip metadata tags
                    }
                    // Otherwise it's a timestamp, continue parsing
                }
            }
            
            // Parse timestamp: [mm:ss.xx] or [mm:ss.xx][mm:ss.xx] (multiple timestamps)
            size_t pos = 0;
            while (pos < line.length() && line[pos] == '[') {
                pos++;  // Skip '['
                size_t timestamp_end = line.find(']', pos);
                if (timestamp_end == std::string::npos) break;
                
                std::string timestamp_str = line.substr(pos, timestamp_end - pos);
                pos = timestamp_end + 1;  // Skip ']'
                
                // Parse mm:ss.xx format (also handles mm:ss format without centiseconds)
                // Format can be: 00:17.12 (minutes:seconds.centiseconds) or 0:17.12
                int minutes = 0, seconds = 0, centiseconds = 0;
                int parsed = sscanf(timestamp_str.c_str(), "%d:%d.%d", &minutes, &seconds, &centiseconds);
                if (parsed >= 2) {
                    // If no centiseconds parsed, default to 0
                    if (parsed == 2) {
                        centiseconds = 0;
                    }
                    // Convert to milliseconds: minutes*60*1000 + seconds*1000 + centiseconds*10
                    // Example: 00:17.12 = 0*60000 + 17*1000 + 12*10 = 17120 ms
                    uint32_t timestamp_ms = (minutes * 60 + seconds) * 1000 + centiseconds * 10;
                    
                    // Get the lyric text (everything after the last timestamp)
                    std::string lyric_text;
                    if (pos < line.length()) {
                        lyric_text = line.substr(pos);
                        // Trim whitespace
                        while (!lyric_text.empty() && (lyric_text.front() == ' ' || lyric_text.front() == '\t')) {
                            lyric_text.erase(lyric_text.begin());
                        }
                    }
                    
                    if (!lyric_text.empty()) {
                        lines.push_back(LyricLine(timestamp_ms, lyric_text));
                    }
                }
            }
        }
        
        // Sort by timestamp
        std::sort(lines.begin(), lines.end(), [](const LyricLine& a, const LyricLine& b) {
            return a.timestamp_ms < b.timestamp_ms;
        });
        
        log_lyrics_fetch("parse_lrc_format: Parsed " + std::to_string(lines.size()) + " time-synced lines from " + std::to_string(line_count) + " input lines");
        
        return lines;
    }
    
    // Note: LRC file loading removed - server-side file paths are not accessible from client
    // Time-synced lyrics are now fetched from LRCLIB API instead
    
    // Fetch time-synced lyrics from LRCLIB API
    // Returns LRC format string that can be parsed
    std::string fetch_lrclib_lyrics(const LyricsRequest& request) {
        if (request.artist.empty() || request.title.empty() || request.duration_seconds == 0) {
            log_lyrics_fetch("Skipping LRCLIB fetch - missing artist, title, or duration");
            return "";
        }
        
        log_lyrics_fetch("Fetching from LRCLIB: \"" + request.title + "\" by \"" + request.artist + "\" (duration: " + std::to_string(request.duration_seconds) + "s)");
        
        // Check if thread is being shut down
        {
            std::lock_guard<std::mutex> lock(lyrics_mutex);
            if (!lyrics_thread_running) {
                log_lyrics_fetch("Thread shutting down, skipping LRCLIB fetch");
                return "";
            }
        }
        
        try {
            // URL encode parameters for curl
            std::string encoded_artist = url_encode(request.artist);
            std::string encoded_title = url_encode(request.title);
            std::string encoded_album = url_encode(request.album);
            
            // Build LRCLIB API URL: /api/get?track_name=...&artist_name=...&album_name=...&duration=...
            std::string lrclib_url = "https://lrclib.net/api/get?track_name=" + encoded_title +
                                     "&artist_name=" + encoded_artist +
                                     "&album_name=" + encoded_album +
                                     "&duration=" + std::to_string(request.duration_seconds);
            
            log_lyrics_fetch("LRCLIB URL: " + lrclib_url);
            
            // Build curl command (use curl command-line tool via subprocess)
            // Don't use --fail so we can check the response content even on HTTP errors
            std::string curl_cmd = "curl -s -m 10 --silent --show-error \"";
            curl_cmd += lrclib_url;
            curl_cmd += "\"";
            
            log_lyrics_fetch("Executing curl command for LRCLIB (subprocess)");
            
            FILE* pipe = popen(curl_cmd.c_str(), "r");
            if (!pipe) {
                log_lyrics_fetch("ERROR: Failed to open pipe for LRCLIB curl command");
                return "";
            }
            
            // Check for shutdown while reading
            {
                std::lock_guard<std::mutex> lock(lyrics_mutex);
                if (!lyrics_thread_running) {
                    pclose(pipe);
                    log_lyrics_fetch("Thread shutdown detected during LRCLIB fetch");
                    return "";
                }
            }
            
            // Read response (read all data, not just one line)
            std::array<char, 8192> buffer;
            std::string response;
            size_t total_read = 0;
            while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
                // Check for shutdown periodically
                {
                    std::lock_guard<std::mutex> lock(lyrics_mutex);
                    if (!lyrics_thread_running) {
                        pclose(pipe);
                        log_lyrics_fetch("Thread shutdown detected while reading LRCLIB response");
                        return "";
                    }
                }
                size_t len = strlen(buffer.data());
                response += buffer.data();
                total_read += len;
                // Safety limit: max 1MB response
                if (total_read > 1024 * 1024) {
                    log_lyrics_fetch("WARNING: LRCLIB response exceeds 1MB, truncating");
                    break;
                }
            }
            
            // Remove trailing newline if present
            while (!response.empty() && (response.back() == '\n' || response.back() == '\r')) {
                response.pop_back();
            }
            
            pclose(pipe);  // Don't check status - check response content instead
            
            if (response.empty()) {
                log_lyrics_fetch("LRCLIB API returned empty response");
                return "";
            }
            
            log_lyrics_fetch("Received LRCLIB response: " + std::to_string(response.length()) + " bytes");
            
            // Log response preview for debugging (first 200 chars)
            if (response.length() > 0) {
                std::string preview = response.substr(0, std::min(response.length(), size_t(200)));
                log_lyrics_fetch("LRCLIB response preview: " + preview + (response.length() > 200 ? "..." : ""));
            }
            
            // Check for error response (404 Not Found)
            // Error responses have: {"code": 404, "name": "TrackNotFound", "message": "..."}
            // Successful responses also have "name" field, so check for the error structure specifically
            if (response.find("\"code\":") != std::string::npos && 
                (response.find("TrackNotFound") != std::string::npos || 
                 response.find("\"message\":") != std::string::npos)) {
                // Log the actual error for debugging
                std::string error_code = parse_json_field(response, "code");
                std::string error_name = parse_json_field(response, "name");
                std::string error_msg = parse_json_field(response, "message");
                log_lyrics_fetch("LRCLIB API returned error: code=" + error_code + ", name=" + error_name + ", message=" + error_msg);
                return "";
            }
            
            // Parse JSON response to extract syncedLyrics
            // Simple JSON parsing for "syncedLyrics" field
            log_lyrics_fetch("Attempting to parse syncedLyrics from response...");
            std::string synced_lyrics = parse_json_field(response, "syncedLyrics");
            if (!synced_lyrics.empty()) {
                // Log first 300 chars to see the actual format
                std::string preview = synced_lyrics.substr(0, std::min(synced_lyrics.length(), size_t(300)));
                log_lyrics_fetch("Extracted syncedLyrics (" + std::to_string(synced_lyrics.length()) + " chars), preview: " + preview);
                log_lyrics_fetch("SOURCE: LRCLIB API (time-synced) - " + request.title + " by " + request.artist + " (" + std::to_string(synced_lyrics.length()) + " chars)");
                return synced_lyrics;
            } else {
                log_lyrics_fetch("syncedLyrics field not found or empty in response");
            }
            
            // Fallback to plainLyrics if syncedLyrics not available
            log_lyrics_fetch("Attempting to parse plainLyrics from response...");
            std::string plain_lyrics = parse_json_field(response, "plainLyrics");
            if (!plain_lyrics.empty()) {
                log_lyrics_fetch("SOURCE: LRCLIB API (plain text, not time-synced) - " + request.title + " by " + request.artist);
                return plain_lyrics;
            } else {
                log_lyrics_fetch("plainLyrics field not found or empty in response");
            }
            
            // Check if response contains the field name at all
            if (response.find("syncedLyrics") == std::string::npos && response.find("plainLyrics") == std::string::npos) {
                log_lyrics_fetch("ERROR: Response does not contain syncedLyrics or plainLyrics fields at all");
            }
            
            log_lyrics_fetch("LRCLIB API returned no lyrics (no syncedLyrics or plainLyrics field)");
            return "";
            
        } catch (const std::exception& e) {
            log_lyrics_fetch("ERROR: Exception in LRCLIB fetch: " + std::string(e.what()));
            return "";
        } catch (...) {
            log_lyrics_fetch("ERROR: Unknown exception in LRCLIB fetch");
            return "";
        }
    }
    
    // Simple URL encoding helper
    std::string url_encode(const std::string& str) {
        std::string encoded;
        encoded.reserve(str.length() * 3);  // Worst case: all chars need encoding
        
        for (char c : str) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded += c;
            } else if (c == ' ') {
                encoded += "%20";
            } else {
                // URL encode special characters
                char hex[4];
                snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
                encoded += hex;
            }
        }
        
        return encoded;
    }
    
    // Simple JSON field parser (extracts value from "fieldName":"value")
    // Handles multi-line strings with escaped characters
    // Note: This function properly unescapes \n, \r, \t, etc. from JSON strings
    std::string parse_json_field(const std::string& json, const std::string& field_name) {
        std::string search_pattern = "\"" + field_name + "\"";
        size_t pos = json.find(search_pattern);
        if (pos == std::string::npos) {
            return "";
        }
        
        // Find the colon after the field name
        pos = json.find(':', pos);
        if (pos == std::string::npos) {
            return "";
        }
        pos++;  // Move past colon
        
        // Skip whitespace
        while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) {
            pos++;
        }
        
        if (pos >= json.length()) {
            return "";
        }
        
        // Check if value is null
        if (pos + 4 <= json.length() && json.substr(pos, 4) == "null") {
            return "";
        }
        
        // Check if value is a string (starts with ")
        if (json[pos] != '"') {
            return "";  // Not a string value
        }
        pos++;  // Skip opening quote
        
        // Find the closing quote, handling escaped quotes and newlines
        std::string value;
        value.reserve(json.length());  // Pre-allocate
        bool in_escape = false;
        for (size_t i = pos; i < json.length(); ++i) {
            if (in_escape) {
                // Handle escape sequences
                if (json[i] == 'n') {
                    value += '\n';
                } else if (json[i] == 'r') {
                    value += '\r';
                } else if (json[i] == 't') {
                    value += '\t';
                } else if (json[i] == '\\') {
                    value += '\\';
                } else if (json[i] == '"') {
                    value += '"';  // Escaped quote
                } else {
                    value += '\\';  // Unknown escape, keep backslash
                    value += json[i];
                }
                in_escape = false;
            } else if (json[i] == '\\') {
                in_escape = true;
            } else if (json[i] == '"') {
                // Unescaped quote - end of string
                break;
            } else {
                value += json[i];
            }
        }
        
        return value;
    }
    
    // Fetch lyrics from lyrics.ovh API (fallback, non-time-synced, manual scrolling)
    // Uses subprocess (curl command-line) to avoid thread-safety issues with libcurl
    std::string fetch_lyrics_for_request(const LyricsRequest& request) {
        // Fetch from lyrics.ovh API (fallback for non-time-synced lyrics)
        if (request.artist.empty() || request.title.empty()) {
            log_lyrics_fetch("Skipping lyrics fetch - empty artist or title");
            return "";
        }
        
        // Check if thread is being shut down - exit early
        {
            std::lock_guard<std::mutex> lock(lyrics_mutex);
            if (!lyrics_thread_running) {
                log_lyrics_fetch("Thread shutting down, skipping fetch for: " + request.title);
                return "";
            }
        }
        
        log_lyrics_fetch("Starting lyrics fetch for: \"" + request.title + "\" by \"" + request.artist + "\"");
        
        try {
            // URL encode artist and title for curl command
            std::string encoded_artist;
            std::string encoded_title;
            
            // URL encode artist and title (curl will handle it, but we need proper encoding)
            // Use curl's built-in URL encoding via curl_easy_escape if available, or manual encoding
            for (char c : request.artist) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
                    encoded_artist += c;
                } else if (c == ' ') {
                    encoded_artist += "%20";
                } else {
                    // URL encode special characters
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
                    encoded_artist += hex;
                }
            }
            
            for (char c : request.title) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
                    encoded_title += c;
                } else if (c == ' ') {
                    encoded_title += "%20";
                } else {
                    // URL encode special characters
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
                    encoded_title += hex;
                }
            }
            
            
            std::string lyrics_url = "https://api.lyrics.ovh/v1/" + encoded_artist + "/" + encoded_title;
            log_lyrics_fetch("URL: " + lyrics_url);
            
            // Build curl command (use curl command-line tool via subprocess)
            // Escape single quotes in URL for shell safety
            std::string escaped_url = lyrics_url;
            size_t pos = 0;
            while ((pos = escaped_url.find("'", pos)) != std::string::npos) {
                escaped_url.replace(pos, 1, "'\\''");
                pos += 4;
            }
            
            std::string curl_cmd = "curl -s -S --max-time 5 --connect-timeout 3 --location '";
            curl_cmd += escaped_url;
            curl_cmd += "' 2>/dev/null";
            
            log_lyrics_fetch("Executing curl command (subprocess)");
            
            // Use popen to execute curl command and read output
            // This is thread-safe - each subprocess runs independently
            FILE* pipe = popen(curl_cmd.c_str(), "r");
            if (!pipe) {
                log_lyrics_fetch("ERROR: Failed to start curl subprocess");
                return "";  // Failed to start subprocess
            }
            
            // Check if thread is being shut down before reading (for smooth quit)
            {
                std::lock_guard<std::mutex> lock(lyrics_mutex);
                if (!lyrics_thread_running) {
                    log_lyrics_fetch("Thread shutdown detected, closing pipe");
                    pclose(pipe);  // Close pipe if shutting down
                    return "";
                }
            }
            
            // Read response from subprocess (thread-safe - each pipe is independent)
            std::string response;
            response.reserve(8192);  // Pre-allocate
            char buffer[4096];
            size_t bytes_read = 0;
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                // Check if shutting down during read
                {
                    std::lock_guard<std::mutex> lock(lyrics_mutex);
                    if (!lyrics_thread_running) {
                        log_lyrics_fetch("Thread shutdown detected during read, closing pipe");
                        pclose(pipe);
                        return "";
                    }
                }
                
                size_t len = strlen(buffer);
                if (len > 0) {
                    response.append(buffer, len);
                    bytes_read += len;
                    // Safety limit: max 1MB response
                    if (bytes_read > 1024 * 1024) {
                        break;
                    }
                }
            }
            
            pclose(pipe);  // Close pipe (status not needed - we check response content)
            
            log_lyrics_fetch("Received response: " + std::to_string(response.length()) + " bytes");
            if (response.length() > 0 && response.length() < 200) {
                log_lyrics_fetch("Response preview: " + response.substr(0, std::min(response.length(), size_t(100))));
            }
            
            // Check if we got a response
            // Note: curl might return non-zero status for some errors, but still have partial data
            // So we check response content first, then status
            if (response.empty()) {
                log_lyrics_fetch("ERROR: Empty response from API");
                return "";
            }
            
            // Remove any trailing newlines/whitespace
            while (!response.empty() && (response.back() == '\n' || response.back() == '\r' || response.back() == ' ')) {
                response.pop_back();
            }
            
            if (response.empty()) {
                log_lyrics_fetch("ERROR: Response contains only whitespace");
                return "";  // Only whitespace
            }
            
            // Parse JSON response from lyrics.ovh API
            // Format: {"lyrics":"...lyrics text with escaped newlines..."}
            // Handle both success and error responses
            if (response.find("\"lyrics\"") == std::string::npos) {
                // No lyrics key - might be error like {"error":"Not found"}
                // Check if it's an error response
                if (response.find("\"error\"") != std::string::npos) {
                    log_lyrics_fetch("API returned error: lyrics not found");
                    return "";
                }
                log_lyrics_fetch("ERROR: Unknown response format (no 'lyrics' key found)");
                return "";
            }
            
            // Find "lyrics" key and extract value
            size_t lyrics_key_pos = response.find("\"lyrics\"");
            if (lyrics_key_pos == std::string::npos) {
                return "";
            }
            
            // Find the colon after "lyrics"
            size_t colon_pos = response.find(':', lyrics_key_pos);
            if (colon_pos == std::string::npos || colon_pos >= response.length() - 1) {
                return "";
            }
            
            // Skip whitespace after colon
            size_t value_start = colon_pos + 1;
            while (value_start < response.length() && 
                   (response[value_start] == ' ' || response[value_start] == '\t' || response[value_start] == '\n' || response[value_start] == '\r')) {
                value_start++;
            }
            
            if (value_start >= response.length()) {
                return "";
            }
            
            // Check if value is a string (starts with ")
            if (response[value_start] != '"') {
                return "";  // Not a string value (might be null)
            }
            value_start++;  // Skip opening quote
            
            // Find the closing quote, handling escaped quotes and newlines
            std::string lyrics;
            lyrics.reserve(response.length());  // Pre-allocate
            bool in_escape = false;
            for (size_t i = value_start; i < response.length(); ++i) {
                if (in_escape) {
                    // Handle escape sequences
                    if (response[i] == 'n') {
                        lyrics += '\n';
                    } else if (response[i] == 'r') {
                        lyrics += '\r';
                    } else if (response[i] == 't') {
                        lyrics += '\t';
                    } else if (response[i] == '\\') {
                        lyrics += '\\';
                    } else if (response[i] == '"') {
                        lyrics += '"';  // Escaped quote
                    } else {
                        lyrics += '\\';  // Unknown escape, keep backslash
                        lyrics += response[i];
                    }
                    in_escape = false;
                } else if (response[i] == '\\') {
                    in_escape = true;
                } else if (response[i] == '"') {
                    // Unescaped quote - end of string
                    break;
                } else {
                    lyrics += response[i];
                }
            }
            
            // Return lyrics if we found any (trim whitespace)
            if (!lyrics.empty()) {
                // Trim leading/trailing whitespace
                while (!lyrics.empty() && (lyrics.front() == ' ' || lyrics.front() == '\n' || lyrics.front() == '\r' || lyrics.front() == '\t')) {
                    lyrics.erase(lyrics.begin());
                }
                while (!lyrics.empty() && (lyrics.back() == ' ' || lyrics.back() == '\n' || lyrics.back() == '\r' || lyrics.back() == '\t')) {
                    lyrics.pop_back();
                }
                if (!lyrics.empty()) {
                    log_lyrics_fetch("SOURCE: lyrics.ovh API (NOT time-synced) - SUCCESS: Extracted lyrics (" + std::to_string(lyrics.length()) + " chars, " + 
                                    std::to_string(std::count(lyrics.begin(), lyrics.end(), '\n') + 1) + " lines)");
                    return lyrics;
                }
            }
            
            log_lyrics_fetch("WARNING: Parsed lyrics but result is empty after trimming");
        } catch (const std::exception& e) {
            // Subprocess or parsing failed
            log_lyrics_fetch("EXCEPTION during lyrics fetch: " + std::string(e.what()));
            return "";
        } catch (...) {
            log_lyrics_fetch("UNKNOWN EXCEPTION during lyrics fetch");
            return "";
        }
        
        log_lyrics_fetch("No lyrics found for: " + request.title + " by " + request.artist);
        return "";  // No lyrics found
    }
};

PlexClient::PlexClient(const std::string& server_url, const std::string& token, bool enable_debug_logging)
    : server_url(server_url), token(token), pimpl(std::make_unique<Impl>()) {
    
    g_lyrics_debug_logging.store(enable_debug_logging);
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pimpl->curl = curl_easy_init();
    
    // Initialize audio decoder and album art
    audio_decoder = std::make_unique<AudioDecoder>();
    album_art = std::make_unique<AlbumArt>();
    
    // Start lyrics fetching thread
    pimpl->lyrics_thread_running = true;
    pimpl->lyrics_thread = std::thread(&Impl::lyrics_thread_func, pimpl.get());
}

PlexClient::~PlexClient() {
    stop_audio_capture();
    
    // Stop lyrics thread cleanly
    if (pimpl) {
        log_lyrics_fetch("Shutting down lyrics thread...");
        {
            std::lock_guard<std::mutex> lock(pimpl->lyrics_mutex);
            pimpl->lyrics_thread_running = false;
        }
        pimpl->lyrics_cv.notify_all();
        
        if (pimpl->lyrics_thread.joinable()) {
            // Wait for thread to finish (with timeout would be better, but join is simpler)
            pimpl->lyrics_thread.join();
            log_lyrics_fetch("Lyrics thread joined successfully");
        }
    }
    
    if (pimpl && pimpl->curl) {
        curl_easy_cleanup(pimpl->curl);
    }
    curl_global_cleanup();
}

bool PlexClient::connect() {
    if (server_url.empty() || token.empty()) {
        return false;
    }
    
    if (!pimpl || !pimpl->curl) {
        return false;  // Curl not initialized
    }
    
    // Test connection with a simple request
    std::string response = make_request("/");
    if (response.empty() || response.length() < 10) {
        // Connection failed, but don't crash
        connected = false;
        return false;
    }
    
    // Check if we got valid XML response
    if (response.find("<?xml") == std::string::npos && 
        response.find("<MediaContainer") == std::string::npos) {
        connected = false;
        return false;
    }
    
    connected = true;
    return true;
}

std::string PlexClient::make_request(const std::string& endpoint, const std::string& method) {
    if (!pimpl || !pimpl->curl) return "";
    
    pimpl->response_buffer.clear();
    
    std::string url = server_url + endpoint;
    
    // Add token to URL if not already present
    if (url.find("X-Plex-Token") == std::string::npos) {
        url += (url.find('?') != std::string::npos ? "&" : "?");
        url += "X-Plex-Token=" + token;
    }
    
    // Reset curl options
    curl_easy_reset(pimpl->curl);
    
    curl_easy_setopt(pimpl->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(pimpl->curl, CURLOPT_WRITEFUNCTION, Impl::write_callback);
    curl_easy_setopt(pimpl->curl, CURLOPT_WRITEDATA, &pimpl->response_buffer);
    curl_easy_setopt(pimpl->curl, CURLOPT_TIMEOUT, 5L);  // 5 second timeout
    curl_easy_setopt(pimpl->curl, CURLOPT_CONNECTTIMEOUT, 3L);  // 3 second connect timeout
    
    // SSL options for HTTPS
    curl_easy_setopt(pimpl->curl, CURLOPT_SSL_VERIFYPEER, 0L);  // Allow self-signed certs
    curl_easy_setopt(pimpl->curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(pimpl->curl, CURLOPT_FOLLOWLOCATION, 1L);  // Follow redirects
    
    // Add Plex token header
    struct curl_slist* headers = nullptr;
    std::string token_header = "X-Plex-Token: " + token;
    headers = curl_slist_append(headers, token_header.c_str());
    headers = curl_slist_append(headers, "Accept: application/xml");
    curl_easy_setopt(pimpl->curl, CURLOPT_HTTPHEADER, headers);
    
    if (method == "POST") {
        curl_easy_setopt(pimpl->curl, CURLOPT_POST, 1L);
    } else if (method == "PUT") {
        curl_easy_setopt(pimpl->curl, CURLOPT_CUSTOMREQUEST, "PUT");
    }
    
    CURLcode res = curl_easy_perform(pimpl->curl);
    curl_slist_free_all(headers);
    
    if (res != CURLE_OK) {
        return "";
    }
    
    return pimpl->response_buffer;
}

// Real Plex API implementations
int PlexClient::get_music_library_id() {
    if (!connected) return -1;
    
    std::string response = make_request("/library/sections");
    if (response.empty() || response.length() < 10) return -1;
    
    try {
        PlexXML::Node root = PlexXML::parse(response);
        auto directories = root.find_all("Directory");
        
        for (const auto& dir : directories) {
            std::string type = dir.get_attr("type");
            if (type == "artist") {
                std::string key = dir.get_attr("key", "-1");
                if (key != "-1" && !key.empty()) {
                    return std::stoi(key);
                }
            }
        }
    } catch (...) {
        // XML parsing failed
        return -1;
    }
    return -1;
}

std::vector<Track> PlexClient::search_tracks(const std::string& query, int limit, int start) {
    int lib_id = get_music_library_id();
    if (lib_id < 0) return {};
    
    // URL encode the query for server-side search (Plex API requires URL encoding)
    std::string encoded_query;
    for (char c : query) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded_query += c;
        } else if (c == ' ') {
            encoded_query += "%20";
        } else {
            // URL encode other characters
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
            encoded_query += hex;
        }
    }
    
    // Use Plex server-side search API with pagination support
    std::string endpoint = "/library/sections/" + std::to_string(lib_id) + 
                          "/search?type=10&query=" + encoded_query + "&limit=" + std::to_string(limit);
    
    // Add pagination if needed
    if (start > 0) {
        endpoint += "&X-Plex-Container-Start=" + std::to_string(start);
    }
    
    std::string response = make_request(endpoint);
    if (response.empty()) return {};
    
    return parse_tracks_from_xml(response);
}

std::vector<Track> PlexClient::get_tracks_from_library(int library_id, int limit) {
    std::string endpoint = "/library/sections/" + std::to_string(library_id) + 
                          "/all?type=10&limit=" + std::to_string(limit);
    std::string response = make_request(endpoint);
    if (response.empty()) return {};
    
    return parse_tracks_from_xml(response);
}

std::vector<Track> PlexClient::get_recent_tracks(int limit) {
    int lib_id = get_music_library_id();
    if (lib_id < 0) return {};
    
    std::string endpoint = "/library/sections/" + std::to_string(lib_id) + 
                          "/recentlyAdded?type=10&limit=" + std::to_string(limit);
    std::string response = make_request(endpoint);
    if (response.empty()) return {};
    
    return parse_tracks_from_xml(response);
}

std::vector<Track> PlexClient::get_playlist_tracks(const std::string& playlist_id, int start, int size) {
    std::string endpoint = "/playlists/" + playlist_id + "/items";
    
    // Add pagination parameters to URL (Plex API uses query parameters)
    bool has_params = false;
    if (start > 0) {
        endpoint += "?X-Plex-Container-Start=" + std::to_string(start);
        has_params = true;
    }
    if (size > 0) {
        endpoint += has_params ? "&" : "?";
        endpoint += "X-Plex-Container-Size=" + std::to_string(size);
    }
    
    std::string response = make_request(endpoint);
    if (response.empty()) return {};
    
    return parse_tracks_from_xml(response);
}

std::vector<PlexClient::Artist> PlexClient::get_artists(int library_id, int limit) {
    std::string endpoint = "/library/sections/" + std::to_string(library_id) + 
                          "/all?type=8&limit=" + std::to_string(limit);
    std::string response = make_request(endpoint);
    if (response.empty()) return {};
    
    std::vector<Artist> artists;
    PlexXML::Node root = PlexXML::parse(response);
    auto directories = root.find_all("Directory");
    
    for (const auto& dir : directories) {
        Artist artist;
        artist.id = dir.get_attr("ratingKey");
        artist.name = dir.get_attr("title");
        artist.art_url = dir.get_attr("thumb", "");
        if (!artist.art_url.empty() && artist.art_url[0] != '/') {
            artist.art_url = server_url + artist.art_url;
        }
        artists.push_back(artist);
    }
    
    return artists;
}

std::vector<PlexClient::Album> PlexClient::get_albums(int library_id, const std::string& artist_id, int limit) {
    std::string endpoint;
    if (!artist_id.empty()) {
        endpoint = "/library/metadata/" + artist_id + "/children?type=9&limit=" + std::to_string(limit);
    } else {
        endpoint = "/library/sections/" + std::to_string(library_id) + 
                  "/all?type=9&limit=" + std::to_string(limit);
    }
    
    std::string response = make_request(endpoint);
    if (response.empty()) return {};
    
    std::vector<Album> albums;
    PlexXML::Node root = PlexXML::parse(response);
    auto directories = root.find_all("Directory");
    
    for (const auto& dir : directories) {
        Album album;
        album.id = dir.get_attr("ratingKey");
        album.title = dir.get_attr("title");
        album.artist = dir.get_attr("parentTitle", "");
        album.year = std::stoi(dir.get_attr("year", "0"));
        album.art_url = dir.get_attr("thumb", "");
        if (!album.art_url.empty() && album.art_url[0] != '/') {
            album.art_url = server_url + album.art_url;
        }
        albums.push_back(album);
    }
    
    return albums;
}

std::vector<Track> PlexClient::get_album_tracks(const std::string& album_id) {
    std::string endpoint = "/library/metadata/" + album_id + "/children";
    std::string response = make_request(endpoint);
    if (response.empty()) {
        return {};
    }
    
    return parse_tracks_from_xml(response);
}

std::vector<PlexClient::Playlist> PlexClient::get_playlists(int limit) {
    std::string endpoint = "/playlists/all?limit=" + std::to_string(limit);
    std::string response = make_request(endpoint);
    if (response.empty()) return {};
    
    std::vector<Playlist> playlists;
    PlexXML::Node root = PlexXML::parse(response);
    auto directories = root.find_all("Playlist");
    
    for (const auto& pl : directories) {
        Playlist playlist;
        playlist.id = pl.get_attr("ratingKey");
        playlist.title = pl.get_attr("title");
        playlist.count = std::stoi(pl.get_attr("leafCount", "0"));
        playlists.push_back(playlist);
    }
    
    return playlists;
}

// Helper to parse tracks from XML
std::vector<Track> PlexClient::parse_tracks_from_xml(const std::string& xml) {
    std::vector<Track> tracks;
    
    if (xml.empty()) {
        return tracks;
    }
    
    try {
        PlexXML::Node root = PlexXML::parse(xml);
        if (root.name.empty()) {
            // Parsing failed or empty XML
            return tracks;
        }
        
        auto track_nodes = root.find_all("Track");
        
        for (const auto& node : track_nodes) {
            try {
                Track track;
                track.id = node.get_attr("ratingKey");
                track.title = node.get_attr("title");
                track.artist = node.get_attr("grandparentTitle", "");
                track.album = node.get_attr("parentTitle", "");
                
                // Safe conversion with defaults
                try {
                    std::string duration_str = node.get_attr("duration", "0");
                    track.duration_ms = duration_str.empty() ? 0 : std::stoi(duration_str);
                } catch (...) {
                    track.duration_ms = 0;
                }
                
                try {
                    std::string year_str = node.get_attr("year", "0");
                    track.year = year_str.empty() ? 0 : std::stoi(year_str);
                } catch (...) {
                    track.year = 0;
                }
                
                track.genre = node.get_attr("genre", "");
                
                // Check for lyrics in Plex metadata (may be in fields or embedded)
                // Plex stores lyrics in various ways - check common locations
                // Note: Plex may have time-synced lyrics from LyricFind (Plex Pass) in a different format
                try {
                    auto fields = node.find_all("Field");
                    for (const auto& field : fields) {
                        std::string field_type = field.get_attr("type", "");
                        std::string field_value = field.get_attr("value", "");
                        
                        // Check for various lyric field types
                        // "lyrics" = plain text lyrics
                        // "lyric" = alternative field name
                        // Plex Pass users may have time-synced lyrics from LyricFind
                        // Note: Plex Web uses LyricFind for time-synced lyrics, which may not be in local LRC files
                        if (field_type == "lyrics" || field_type == "lyric" || 
                            field_type == "lyricsTimed" || field_type == "lyrics_timed" ||
                            field_type == "lyricsSynced" || field_type == "lyrics_synced") {
                            track.lyrics = field_value;
                            log_lyrics_fetch("Found lyrics in Field type: " + field_type + " (" + std::to_string(field_value.length()) + " chars)");
                            // Check if it looks like time-synced format (contains [timestamp] patterns)
                            if (field_value.find('[') != std::string::npos && field_value.find(':') != std::string::npos) {
                                log_lyrics_fetch("Lyrics may contain timestamp patterns - could be time-synced from LyricFind");
                            }
                            break;
                        }
                    }
                } catch (...) {
                    // Ignore lyrics parsing errors
                }
                
                // Get media info
                try {
                    auto media = node.find_first("Media");
                    if (!media.name.empty()) {
                        try {
                            std::string bitrate_str = media.get_attr("bitrate", "0");
                            track.bitrate = bitrate_str.empty() ? 0 : std::stoi(bitrate_str);
                        } catch (...) {
                            track.bitrate = 0;
                        }
                        track.codec = media.get_attr("audioCodec", "");
                        
                        // Get part for media URL and file path
                        auto part = media.find_first("Part");
                        if (!part.name.empty()) {
                            std::string key = part.get_attr("key", "");
                            if (!key.empty() && !server_url.empty() && !token.empty()) {
                                // Build proper Plex media URL with token
                                track.media_url = server_url + key;
                                // Add token parameter
                                if (track.media_url.find('?') != std::string::npos) {
                                    track.media_url += "&X-Plex-Token=" + token;
                                } else {
                                    track.media_url += "?X-Plex-Token=" + token;
                                }
                            }
                            // Note: File path extraction removed - server-side paths are not accessible from client
                        }
                    }
                } catch (...) {
                    // Ignore media parsing errors - track might still be valid
                }
                
                // Get art URLs
                try {
                    track.thumb_url = node.get_attr("thumb", "");
                    track.art_url = node.get_attr("art", "");
                    if (!track.thumb_url.empty() && track.thumb_url[0] != '/' && !server_url.empty()) {
                        track.thumb_url = server_url + track.thumb_url;
                    }
                    if (!track.art_url.empty() && track.art_url[0] != '/' && !server_url.empty()) {
                        track.art_url = server_url + track.art_url;
                    }
                } catch (...) {
                    // Ignore art URL errors
                }
                
                // Only add track if it has at least an ID
                if (!track.id.empty()) {
                    tracks.push_back(track);
                }
            } catch (...) {
                // Skip this track if parsing fails, continue with next
                continue;
            }
        }
    } catch (...) {
        // Parsing failed completely - return empty vector
        return tracks;
    }
    
    return tracks;
}

bool PlexClient::play_track(const Track& track) {
    if (!pimpl) return false;
    
    // Stop any current playback
    stop_audio_capture();
    
    // Validate track has required data
    if (track.id.empty() || track.media_url.empty()) {
        return false;
    }
    
    // Lock mutex to protect playback state
    {
        std::lock_guard<std::mutex> lock(pimpl->playback_mutex);
        pimpl->current_track = track;
        pimpl->is_playing = true;
        pimpl->position = 0;
        pimpl->playback_start_time = std::chrono::steady_clock::now();
    }
    
    // Fetch album art (safely - check if album_art exists)
    if (!track.art_url.empty() && album_art) {
        try {
            album_art->fetch_art(server_url, token, track.art_url);
        } catch (...) {
            // Ignore album art fetch errors - don't crash playback
        }
    }
    
    // Start audio decoding for waveform visualization
    // Remove token from URL - ffmpeg will use it as header instead
    std::string audio_url = track.media_url;
    // Remove token from query string if present (ffmpeg will use header)
    size_t token_pos = audio_url.find("X-Plex-Token");
    if (token_pos != std::string::npos) {
        // Remove token parameter
        if (token_pos > 0 && audio_url[token_pos - 1] == '&') {
            // Token is after &, remove &token=...
            size_t start = token_pos - 1;
            size_t end = audio_url.find('&', token_pos);
            if (end == std::string::npos) {
                end = audio_url.length();
            }
            audio_url.erase(start, end - start);
        } else if (token_pos > 0 && audio_url[token_pos - 1] == '?') {
            // Token is first param after ?, remove ?token=... or ?token=...&...
            size_t end = audio_url.find('&', token_pos);
            if (end == std::string::npos) {
                end = audio_url.length();
            }
            audio_url.erase(token_pos - 1, end - token_pos + 1);
        }
    }
    
    // Start decoding real audio stream (safely)
    if (!audio_decoder) {
        // Audio decoder not initialized - reset playback state
        {
            std::lock_guard<std::mutex> lock(pimpl->playback_mutex);
            pimpl->is_playing = false;
        }
        return false;
    }
    
    try {
        if (!audio_decoder->start_decoding(audio_url, token)) {
            // Failed to start decoding - reset playback state
            std::lock_guard<std::mutex> lock(pimpl->playback_mutex);
            pimpl->is_playing = false;
            return false;
        }
    } catch (const std::exception& e) {
        // Catch any exceptions during audio startup
        {
            std::lock_guard<std::mutex> lock(pimpl->playback_mutex);
            pimpl->is_playing = false;
        }
        stop_audio_capture();
        return false;
    } catch (...) {
        // Catch any other exceptions
        {
            std::lock_guard<std::mutex> lock(pimpl->playback_mutex);
            pimpl->is_playing = false;
        }
        stop_audio_capture();
        return false;
    }
    
    start_audio_capture();
    
    return true;
}

bool PlexClient::pause() {
    if (!pimpl) return false;
    if (audio_decoder) {
        audio_decoder->pause_playback();
    }
    {
        std::lock_guard<std::mutex> lock(pimpl->playback_mutex);
        pimpl->is_playing = false;
        // Don't update position when paused - keep current position
    }
    return true;
}

bool PlexClient::resume() {
    if (!pimpl) return false;
    if (audio_decoder) {
        audio_decoder->resume_playback();
    }
    {
        std::lock_guard<std::mutex> lock(pimpl->playback_mutex);
        pimpl->is_playing = true;
        // Adjust start time to account for current position (resume from where we paused)
        if (pimpl->playback_start_time.time_since_epoch().count() == 0) {
            pimpl->playback_start_time = std::chrono::steady_clock::now();
        } else {
            // Adjust start time so position calculation continues from current position
            auto now = std::chrono::steady_clock::now();
            // Reset start time to now minus the current position
            pimpl->playback_start_time = now - std::chrono::milliseconds(pimpl->position);
        }
    }
    return true;
}

bool PlexClient::stop() {
    if (!pimpl) return false;
    stop_audio_capture();
    {
        std::lock_guard<std::mutex> lock(pimpl->playback_mutex);
        pimpl->is_playing = false;
        pimpl->position = 0;
        pimpl->playback_start_time = std::chrono::steady_clock::time_point();
    }
    return true;
}

bool PlexClient::seek(uint32_t position_ms) {
    if (!pimpl) return false;
    {
        std::lock_guard<std::mutex> lock(pimpl->playback_mutex);
        pimpl->position = position_ms;
    }
    return true;
}

bool PlexClient::set_volume(float volume) {
    current_volume = std::clamp(volume, 0.0f, 1.0f);
    return true;
}

PlaybackState PlexClient::get_playback_state() {
    PlaybackState state;
    if (pimpl) {
        // Lock mutex to protect playback state
        std::lock_guard<std::mutex> lock(pimpl->playback_mutex);
        state.playing = pimpl->is_playing;
        state.paused = !pimpl->is_playing && pimpl->position > 0;
        state.position_ms = pimpl->position;
        state.current_track = pimpl->current_track;
    }
    state.volume = current_volume;
    return state;
}

uint32_t PlexClient::get_position_ms() {
    if (!pimpl) return 0;
    std::lock_guard<std::mutex> lock(pimpl->playback_mutex);
    return pimpl->position;
}

AudioLevels PlexClient::get_audio_levels() {
    AudioLevels levels;
    if (!pimpl) return levels;
    
    // Get real audio levels from decoder if available
    if (audio_decoder && audio_decoder->is_decoding()) {
        // Get waveform samples from decoder - use more samples for higher resolution (like btop)
        auto samples = audio_decoder->get_waveform_samples(200);  // Higher resolution
        pimpl->audio_levels.waveform_data = samples;
        pimpl->audio_levels.current_level = audio_decoder->get_current_level();
        pimpl->audio_levels.peak_level = std::max(
            pimpl->audio_levels.peak_level * 0.95f,
            pimpl->audio_levels.current_level
        );
        
        // Lock mutex to protect playback state access
        bool should_stop = false;
        {
            std::lock_guard<std::mutex> lock(pimpl->playback_mutex);
            
            // Update position based on actual playback time (only when playing, not paused)
            if (pimpl->is_playing) {
                auto now = std::chrono::steady_clock::now();
                if (pimpl->playback_start_time.time_since_epoch().count() == 0) {
                    // First time - record start
                    pimpl->playback_start_time = now;
                    pimpl->position = 0;
                } else {
                    // Calculate elapsed time
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - pimpl->playback_start_time).count();
                    pimpl->position = static_cast<uint32_t>(elapsed);
                    
                    // Clamp to track duration (need to read current_track safely)
                    uint32_t duration_ms = pimpl->current_track.duration_ms;
                    if (pimpl->position > duration_ms) {
                        pimpl->position = duration_ms;
                        pimpl->is_playing = false;
                        should_stop = true;
                    }
                }
            }
        }
        
        // Stop audio capture outside the lock to avoid deadlock
        if (should_stop && audio_decoder->is_decoding()) {
            stop_audio_capture();
        }
        
        return pimpl->audio_levels;
    } else if (pimpl->is_playing) {
        // Fallback: Generate simulated waveform if decoder not available
        static float phase = 0.0f;
        phase += 0.1f;
        
        float base_level = 0.3f + 0.3f * std::sin(phase * 0.5f);
        float variation = 0.2f * std::sin(phase * 2.0f);
        
        pimpl->audio_levels.current_level = std::clamp(base_level + variation, 0.0f, 1.0f);
        pimpl->audio_levels.peak_level = std::max(
            pimpl->audio_levels.peak_level * 0.95f, 
            pimpl->audio_levels.current_level
        );
        
        // Create simple waveform data
        pimpl->audio_levels.waveform_data.clear();
        for (int i = 0; i < 100; ++i) {
            float sample = 0.3f + 0.3f * std::sin((phase + i * 0.1f) * 0.5f);
            pimpl->audio_levels.waveform_data.push_back(sample);
        }
        
        // Reset start time when not playing
        pimpl->playback_start_time = std::chrono::steady_clock::time_point();
    } else {
        pimpl->audio_levels.current_level = 0.0f;
        pimpl->audio_levels.waveform_data.clear();
    }
    
    return pimpl->audio_levels;
}

void PlexClient::start_audio_capture() {
    // Audio decoding is now handled by AudioDecoder
    // This method is kept for compatibility
}

void PlexClient::stop_audio_capture() {
    // Stop audio decoder
    if (audio_decoder) {
        audio_decoder->stop_decoding();
    }
}

std::string PlexClient::get_lyrics(const Track& track) {
    // Async version: queue request and return immediately
    // Use get_lyrics_result() to check if lyrics are ready
    if (!pimpl || track.id.empty()) {
        return "";
    }
    
    // Check if we already have lyrics for this track
    {
        std::lock_guard<std::mutex> lock(pimpl->lyrics_mutex);
        auto it = pimpl->lyrics_results.find(track.id);
        if (it != pimpl->lyrics_results.end()) {
            return it->second;  // Return cached result
        }
        
        // Check if already in progress
        if (pimpl->lyrics_in_progress.find(track.id) != pimpl->lyrics_in_progress.end()) {
            return "";  // Already fetching, return empty for now
        }
    }
    
    // Queue request for async fetching (LRCLIB first, then lyrics.ovh fallback)
    {
        std::lock_guard<std::mutex> lock(pimpl->lyrics_mutex);
        uint32_t duration_seconds = track.duration_ms / 1000;  // Convert ms to seconds
        LyricsRequest request(track.id, track.artist, track.title, track.album, duration_seconds);
        pimpl->lyrics_queue.push(request);
        pimpl->lyrics_in_progress[track.id] = true;
    }
    pimpl->lyrics_cv.notify_one();
    
    // Return empty immediately (lyrics will be fetched async from LRCLIB or lyrics.ovh)
    return "";
}

std::string PlexClient::get_lyrics_result(const std::string& track_id) {
    // Check if lyrics are ready for this track
    if (!pimpl || track_id.empty()) {
        return "";
    }
    
    std::lock_guard<std::mutex> lock(pimpl->lyrics_mutex);
    
    // Check if still in progress
    if (pimpl->lyrics_in_progress.find(track_id) != pimpl->lyrics_in_progress.end() && 
        pimpl->lyrics_in_progress[track_id]) {
        return "";  // Still fetching
    }
    
    // Check if we have a result (even if empty - means fetch completed)
    auto it = pimpl->lyrics_results.find(track_id);
    if (it != pimpl->lyrics_results.end()) {
        return it->second;  // Return result (may be empty if no lyrics found)
    }
    
    return "";  // Not ready yet (shouldn't happen if fetch completed)
}

std::vector<LyricLine> PlexClient::get_synced_lyrics(const std::string& track_id) {
    if (!pimpl || track_id.empty()) return std::vector<LyricLine>();
    
    std::lock_guard<std::mutex> lock(pimpl->lyrics_mutex);
    
    bool in_progress = false;
    if (pimpl->lyrics_in_progress.find(track_id) != pimpl->lyrics_in_progress.end()) {
        in_progress = pimpl->lyrics_in_progress[track_id];
    }
    if (in_progress) return std::vector<LyricLine>();
    
    auto it = pimpl->synced_lyrics_results.find(track_id);
    if (it != pimpl->synced_lyrics_results.end()) return it->second;
    
    return std::vector<LyricLine>();
}

Track PlexClient::get_track_metadata(const std::string& track_id) {
    // Fetch full track metadata to check for LyricFind time-synced lyrics
    if (track_id.empty()) {
        return Track();
    }
    
    std::string endpoint = "/library/metadata/" + track_id;
    std::string response = make_request(endpoint);
    if (response.empty()) {
        return Track();
    }
    
    // Parse the track from XML
    auto tracks = parse_tracks_from_xml(response);
    if (!tracks.empty()) {
        return tracks[0];
    }
    
    return Track();
}

PlexClient::MusicBrainzData PlexClient::get_musicbrainz_data(const std::string& artist_name, const std::string& album_title) {
    MusicBrainzData data;
    data.valid = false;
    
    if (artist_name.empty() || album_title.empty()) {
        return data;
    }
    
    // MusicBrainz API requires a User-Agent string
    // Use curl to fetch JSON data
    CURL* curl = curl_easy_init();
    if (!curl) return data;
    
    // Build search query - search for release by artist and title
    std::string query = "artist:\"" + artist_name + "\" AND release:\"" + album_title + "\"";
    // URL encode the query
    char* encoded = curl_easy_escape(curl, query.c_str(), query.length());
    if (!encoded) {
        curl_easy_cleanup(curl);
        return data;
    }
    
    std::string url = "https://musicbrainz.org/ws/2/release/?query=" + std::string(encoded) + "&fmt=json&limit=1";
    curl_free(encoded);
    
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, [](char* ptr, size_t size, size_t nmemb, std::string* data) {
        if (data && ptr && size > 0 && nmemb > 0) {
            data->append(ptr, size * nmemb);
            return size * nmemb;
        }
        return static_cast<size_t>(0);
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "plex-tui/1.0 (https://github.com/user/plex-tui)");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);  // 5 second timeout
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK || response.empty()) {
        return data;
    }
    
    // Simple JSON parsing - look for release date, label, etc.
    // This is a basic parser - for production, use a proper JSON library
    try {
        // Find "date" field
        size_t date_pos = response.find("\"date\":");
        if (date_pos != std::string::npos && date_pos + 7 < response.length() && date_pos < response.length()) {
            size_t date_start = response.find("\"", date_pos + 7);
            if (date_start != std::string::npos && date_start + 1 < response.length() && date_start < response.length()) {
                date_start += 1;  // Skip the opening quote
                size_t date_end = response.find("\"", date_start);
                if (date_end != std::string::npos && date_end > date_start && date_end <= response.length() && date_start < response.length()) {
                    size_t date_len = date_end - date_start;
                    if (date_start + date_len <= response.length()) {
                        data.release_date = response.substr(date_start, date_len);
                    }
                }
            }
        }
        
        // Find "label-info" -> "label" -> "name"
        size_t label_pos = response.find("\"label-info\"");
        if (label_pos != std::string::npos && label_pos < response.length()) {
            size_t name_pos = response.find("\"name\":", label_pos);
            if (name_pos != std::string::npos && name_pos + 7 < response.length() && name_pos < response.length()) {
                size_t name_start = response.find("\"", name_pos + 7);
                if (name_start != std::string::npos && name_start + 1 < response.length() && name_start < response.length()) {
                    name_start += 1;  // Skip the opening quote
                    size_t name_end = response.find("\"", name_start);
                    if (name_end != std::string::npos && name_end > name_start && name_end <= response.length() && name_start < response.length()) {
                        size_t name_len = name_end - name_start;
                        if (name_start + name_len <= response.length()) {
                            data.label = response.substr(name_start, name_len);
                        }
                    }
                }
            }
        }
        
        // Find "country"
        size_t country_pos = response.find("\"country\":");
        if (country_pos != std::string::npos && country_pos + 10 < response.length() && country_pos < response.length()) {
            size_t country_start = response.find("\"", country_pos + 10);
            if (country_start != std::string::npos && country_start + 1 < response.length() && country_start < response.length()) {
                country_start += 1;  // Skip the opening quote
                size_t country_end = response.find("\"", country_start);
                if (country_end != std::string::npos && country_end > country_start && country_end <= response.length() && country_start < response.length()) {
                    size_t country_len = country_end - country_start;
                    if (country_start + country_len <= response.length()) {
                        data.country = response.substr(country_start, country_len);
                    }
                }
            }
        }
        
        // Find "format"
        size_t format_pos = response.find("\"format\":");
        if (format_pos != std::string::npos && format_pos + 8 < response.length() && format_pos < response.length()) {
            size_t format_start = response.find("\"", format_pos + 8);
            if (format_start != std::string::npos && format_start + 1 < response.length() && format_start < response.length()) {
                format_start += 1;  // Skip the opening quote
                size_t format_end = response.find("\"", format_start);
                if (format_end != std::string::npos && format_end > format_start && format_end <= response.length() && format_start < response.length()) {
                    size_t format_len = format_end - format_start;
                    if (format_start + format_len <= response.length()) {
                        data.format = response.substr(format_start, format_len);
                    }
                }
            }
        }
        
        // Find "barcode"
        size_t barcode_pos = response.find("\"barcode\":");
        if (barcode_pos != std::string::npos && barcode_pos + 11 < response.length() && barcode_pos < response.length()) {
            size_t barcode_start = response.find("\"", barcode_pos + 11);
            if (barcode_start != std::string::npos && barcode_start + 1 < response.length() && barcode_start < response.length()) {
                barcode_start += 1;  // Skip the opening quote
                size_t barcode_end = response.find("\"", barcode_start);
                if (barcode_end != std::string::npos && barcode_end > barcode_start && barcode_end <= response.length() && barcode_start < response.length()) {
                    size_t barcode_len = barcode_end - barcode_start;
                    if (barcode_start + barcode_len <= response.length()) {
                        data.barcode = response.substr(barcode_start, barcode_len);
                    }
                }
            }
        }
        
        // Find "disambiguation"
        size_t disambig_pos = response.find("\"disambiguation\":");
        if (disambig_pos != std::string::npos && disambig_pos + 17 < response.length() && disambig_pos < response.length()) {
            size_t disambig_start = response.find("\"", disambig_pos + 17);
            if (disambig_start != std::string::npos && disambig_start + 1 < response.length() && disambig_start < response.length()) {
                disambig_start += 1;  // Skip the opening quote
                size_t disambig_end = response.find("\"", disambig_start);
                if (disambig_end != std::string::npos && disambig_end > disambig_start && disambig_end <= response.length() && disambig_start < response.length()) {
                    size_t disambig_len = disambig_end - disambig_start;
                    if (disambig_start + disambig_len <= response.length()) {
                        data.disambiguation = response.substr(disambig_start, disambig_len);
                    }
                }
            }
        }
        
        data.valid = true;
    } catch (...) {
        // JSON parsing failed - return empty data
        data.valid = false;
    }
    
    return data;
}

} // namespace PlexTUI
