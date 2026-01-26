#pragma once

#include "types.h"
#include "waveform.h"
#include "input.h"
#include "plex_client.h"
#include "audio_decoder.h"
#include <chrono>
#include <memory>

namespace PlexTUI {

class Terminal;

class PlayerView {
public:
    PlayerView(Terminal& term, PlexClient& client, Config& config);
    
    // Main render function
    void draw();
    
    // Handle input event
    void handle_input(const InputEvent& event);
    
    // Update state (called every frame)
    void update();
    
    // Force full redraw (for resize handling)
    void force_redraw() { need_bg_fill = true; }
    
    // PLACEHOLDER: View states for future expansion
    enum class ViewMode {
        Player,      // Main playback view
        Library,     // Browse library
        Playlists,   // Playlist management
        Search,      // Search interface
        Queue,       // Queue management
        Settings,    // Settings/preferences
    };
    
    void set_view_mode(ViewMode mode) { current_view = mode; }
    ViewMode get_view_mode() const { return current_view; }
    
    // Options menu control
    void open_options_menu() {
        options_menu_active = true;
        options_menu_category = 0;
        options_menu_selected = 0;
        options_menu_editing = false;
        options_menu_edit_buffer.clear();
    }
    
private:
    Terminal& term;
    PlexClient& client;
    Config& config;  // Non-const to allow modification in options menu
    ViewMode current_view = ViewMode::Player;
    
    // Resize handling (btop-style)
    bool need_bg_fill = true;  // Force background fill on resize
    
    // UI components
    std::unique_ptr<Waveform> waveform;
    
    // State
    PlaybackState playback_state;
    AudioLevels cached_audio_levels;  // Cache to avoid multiple calls per frame
    std::string status_message;
    
    // Library browsing state
    enum class BrowseMode {
        Artists,
        Albums,
        Playlists,
        Tracks
    };
    BrowseMode browse_mode = BrowseMode::Artists;
    std::vector<PlexClient::Artist> artists;
    std::vector<PlexClient::Album> albums;
    std::vector<PlexClient::Playlist> playlists;
    std::vector<Track> browse_tracks;
    
    // Pagination for large playlists
    std::string current_playlist_id;
    int playlist_total_size = 0;  // Total tracks in current playlist
    int playlist_loaded_count = 0;  // Number of tracks loaded so far
    static const int PLAYLIST_CHUNK_SIZE = 100;  // Load 100 tracks at a time
    
    // Current album info (when viewing tracks from an album)
    std::string current_album_id;
    PlexClient::Album current_album;
    std::unique_ptr<AlbumArt> album_art_for_tracks;  // Album art for tracks view (small, cached)
    std::unique_ptr<AlbumArt> artist_art;  // Artist art for artist view (random pic)
    std::unique_ptr<AlbumArt> album_art_for_albums;  // Album art for albums view (selected album)
    
    // Lyrics for current track
    std::string current_lyrics;
    std::vector<std::string> lyrics_lines;  // Parsed lyrics lines (for non-synced)
    std::vector<LyricLine> synced_lyrics;  // Time-synced lyrics from LRC file
    int lyrics_scroll_position = 0;  // Current scroll position in lyrics
    std::string last_lyrics_track_id;  // Track ID we fetched lyrics for

    // Pending play: fetch lyrics with hint, then play (or after ~1.5s timeout)
    bool pending_play = false;
    Track pending_play_track;
    std::chrono::steady_clock::time_point pending_play_since;
    std::string prefetch_next_track_id;  // Don't re-prefetch same next track
    
    // Pagination for search results
    bool is_search_mode = false;  // True when viewing search results
    std::string current_search_query;  // Current search query for lazy loading
    int search_loaded_count = 0;  // Number of search results loaded so far
    static const int SEARCH_CHUNK_SIZE = 50;  // Load 50 search results at a time
    int selected_index = 0;
    int scroll_offset = 0;
    int playlist_scroll_offset = 0;  // Separate scroll for sidebar playlists
    std::string search_query;
    bool search_active = false;
    int music_library_id = -1;
    std::chrono::steady_clock::time_point last_search_time;  // For debouncing search
    bool search_pending = false;  // Flag to indicate search needs to be performed
    
    // Layout calculations
    struct Layout {
        int waveform_x, waveform_y, waveform_w, waveform_h;
        int album_art_x, album_art_y, album_art_w, album_art_h;
        int progress_bar_y;  // Y position for progress bar
        int controls_x, controls_y;
        int track_info_x, track_info_y;
        int status_bar_y;
        int title_x, title_y;  // Title position above waveform
    };
    Layout calculate_layout();
    
    // Drawing sections
    void draw_top_menu_bar();  // btop-style top menu bar with Options
    void draw_sidebar();  // Colorful Spotify-style sidebar
    void draw_separators(const Layout& layout);  // btop-style separators
    void draw_title(const Layout& layout);  // Application title above waveform
    void draw_waveform(const Layout& layout);
    void draw_album_art(const Layout& layout);
    void draw_plex_logo_placeholder(const Layout& layout);  // Pixelated Plex logo when no art
    void draw_track_info(const Layout& layout);
    void draw_controls(const Layout& layout);
    void draw_progress_bar(const Layout& layout);
    void draw_status_bar(const Layout& layout);
    
    // Library view rendering
    void draw_library_view(const Layout& layout);
    void draw_search_bar(const Layout& layout);
    void draw_artists_list(const Layout& layout);
    void draw_albums_list(const Layout& layout);
    void draw_playlists_list(const Layout& layout);
    void draw_tracks_list(const Layout& layout);
    void draw_lyrics(const Layout& layout);  // Scrolling lyrics under waveform
    
    // Input handlers
    void handle_playback_key(Key key);
    void handle_navigation_key(Key key);
    void handle_mouse_event(const MouseEvent& event);
    void handle_search_input(char c);
    
    // Library browsing
    void load_library_data();
    void perform_search();
    void select_item();
    void advance_to_next_track();  // Auto-advance to next track when current finishes

    // Start playback: fetch lyrics first (hint + up to ~1.5s), then play; or play immediately if instant lyrics
    void start_play_with_lyrics(const Track& track);

    // Options menu (btop-style overlay)
    bool options_menu_active = false;
    int options_menu_category = 0;  // 0=Plex, 1=Display, 2=Features
    int options_menu_selected = 0;
    bool options_menu_editing = false;
    std::string options_menu_edit_buffer;
    std::string options_menu_edit_option;
    
    void draw_options_menu();
    void handle_options_menu_input(const InputEvent& event);
    void save_config();
    
    // Helpers
    std::string format_time(uint32_t milliseconds);
    std::string format_volume(float volume);
};

} // namespace PlexTUI
