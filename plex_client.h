#pragma once

#include "types.h"
#include <vector>
#include <string>
#include <functional>
#include <memory>

namespace PlexTUI {

// Forward declarations
class AudioDecoder;
class AlbumArt;

class PlexClient {
public:
    PlexClient(const std::string& server_url, const std::string& token, bool enable_debug_logging = false);
    ~PlexClient();
    
    // Set debug log file path (for lyrics logging)
    static void set_debug_log_file_path(const std::string& path);
    
    // Connection
    bool connect();
    bool is_connected() const { return connected; }
    
    // Library browsing
    std::vector<Track> search_tracks(const std::string& query, int limit = 50, int start = 0);
    std::vector<Track> get_recent_tracks(int limit = 50);
    std::vector<Track> get_playlist_tracks(const std::string& playlist_id, int start = 0, int size = 100);
    
    // Library sections
    int get_music_library_id();  // Returns library section ID
    std::vector<Track> get_tracks_from_library(int library_id, int limit = 100);
    
    // Artists, Albums, Playlists
    struct Artist {
        std::string id;
        std::string name;
        std::string art_url;
    };
    struct Album {
        std::string id;
        std::string title;
        std::string artist;
        std::string art_url;
        int year = 0;
    };
    
    // MusicBrainz album data
    struct MusicBrainzData {
        std::string release_date;
        std::string label;
        std::string country;
        std::string format;
        std::string barcode;
        std::string disambiguation;  // Additional info/notes
        bool valid = false;
    };
    
    // Fetch MusicBrainz data for an album
    MusicBrainzData get_musicbrainz_data(const std::string& artist_name, const std::string& album_title);
    struct Playlist {
        std::string id;
        std::string title;
        int count = 0;
    };
    
    std::vector<Artist> get_artists(int library_id, int limit = 100);
    std::vector<Album> get_albums(int library_id, const std::string& artist_id = "", int limit = 100);
    std::vector<Track> get_album_tracks(const std::string& album_id);
    std::vector<Playlist> get_playlists(int limit = 50);
    
    // Playback control
    bool play_track(const Track& track);
    bool pause();
    bool resume();
    bool stop();
    bool seek(uint32_t position_ms);
    
    // Volume control
    bool set_volume(float volume); // 0.0 to 1.0
    float get_volume() const { return current_volume; }
    
    // Playback state
    PlaybackState get_playback_state();
    uint32_t get_position_ms();
    
    // Audio levels for visualization
    AudioLevels get_audio_levels();
    
    // Album art access
    AlbumArt* get_album_art() { return album_art.get(); }
    
    // Get server URL and token for album art fetching
    std::string get_server_url() const { return server_url; }
    std::string get_token() const { return token; }
    
    // Lyrics fetching (async)
    std::string get_lyrics(const Track& track);  // Queue async request, returns immediately (Plex lyrics if available)
    std::string get_lyrics_result(const std::string& track_id);  // Check if async lyrics are ready
    std::vector<LyricLine> get_synced_lyrics(const std::string& track_id);  // Get time-synced lyrics from LRC file or Plex API
    
    // Fetch detailed track metadata (for checking LyricFind time-synced lyrics)
    Track get_track_metadata(const std::string& track_id);  // Get full track metadata including all fields
    
    // PLACEHOLDER: Queue management
    // bool add_to_queue(const Track& track);
    // bool clear_queue();
    // std::vector<Track> get_queue();
    
private:
    std::string server_url;
    std::string token;
    bool connected = false;
    float current_volume = 1.0f;
    
    // Audio decoder for client-side waveform generation
    std::unique_ptr<AudioDecoder> audio_decoder;
    
    // Album art fetcher
    std::unique_ptr<AlbumArt> album_art;
    
    // HTTP request helper
    std::string make_request(const std::string& endpoint, const std::string& method = "GET");
    
    // Helper to parse tracks from XML
    std::vector<Track> parse_tracks_from_xml(const std::string& xml);
    
    // Audio capture for waveform (now uses AudioDecoder)
    void start_audio_capture();
    void stop_audio_capture();
    
    struct Impl;
    std::unique_ptr<Impl> pimpl;
};

} // namespace PlexTUI
