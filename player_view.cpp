#include "player_view.h"
#include <iostream>
#include "terminal.h"
#include "plex_client.h"
#include "audio_decoder.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <chrono>
#include <set>
#include <algorithm>
#include <cctype>

namespace PlexTUI {

PlayerView::PlayerView(Terminal& term, PlexClient& client, Config& config)
    : term(term), client(client), config(config) {
    
    // Initialize waveform with safe defaults
    // Don't access term.width/height in constructor - might not be ready
    waveform = std::make_unique<Waveform>(80, 10);  // Safe defaults
    waveform->set_style(Waveform::WaveformStyle::Mirrored);
    
    // Initialize other state
    music_library_id = -1;
    selected_index = 0;
    scroll_offset = 0;
    search_active = false;
    search_pending = false;
    last_search_time = std::chrono::steady_clock::now();
    browse_mode = BrowseMode::Artists;
    current_playlist_id.clear();
    playlist_total_size = 0;
    playlist_loaded_count = 0;
    is_search_mode = false;
    current_search_query.clear();
    search_loaded_count = 0;
}

void PlayerView::update() {
    try {
        // Skip update if client not connected (first run, no config yet)
        if (!client.is_connected()) {
            // If options menu is open on first run, don't try to update
            if (options_menu_active && config.plex_server_url.empty()) {
                return;  // First run, waiting for config
            }
            return;
        }
        playback_state = client.get_playback_state();

        if (pending_play) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - pending_play_since).count();
            bool have_lyrics = false;
            try {
                std::vector<LyricLine> ret = client.get_synced_lyrics(pending_play_track.id);
                if (!ret.empty()) {
                    synced_lyrics = ret;
                    have_lyrics = true;
                } else {
                    std::string plain = client.get_lyrics_result(pending_play_track.id);
                    if (!plain.empty()) {
                        current_lyrics = plain;
                        lyrics_lines.clear();
                        have_lyrics = true;
                    }
                }
            } catch (...) {}
            if (have_lyrics || elapsed_ms >= 1500) {
                if (client.play_track(pending_play_track)) {
                    status_message = "Playing: " + pending_play_track.title + " - " + pending_play_track.artist;
                } else {
                    status_message = "Failed to start playback: " + pending_play_track.title;
                }
                pending_play = false;
            }
            // Handle debounced search even when pending_play
            if (search_pending && search_active) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_search_time).count();
                if (elapsed >= 300) {
                    perform_search();
                    search_pending = false;
                }
            }
            return;
        }

        if (playback_state.playing && playback_state.current_track.duration_ms > 0) {
            if (playback_state.position_ms >= playback_state.current_track.duration_ms - 100) {
                advance_to_next_track();
            }
        }

        if (search_pending && search_active) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_search_time).count();
            if (elapsed >= 300) {
                perform_search();
                search_pending = false;
            }
        }

        if (playback_state.playing) {
                // Cache audio levels to avoid multiple calls per frame
                try {
                    cached_audio_levels = client.get_audio_levels();
                } catch (const std::exception& e) {
                    if (config.enable_debug_logging) {
                        std::cerr << "[LOG] Exception in get_audio_levels: " << e.what() << std::endl;
                    }
                    cached_audio_levels = AudioLevels();
                } catch (...) {
                    if (config.enable_debug_logging) {
                        std::cerr << "[LOG] Unknown exception in get_audio_levels" << std::endl;
                    }
                    cached_audio_levels = AudioLevels();
                }
                
                if (waveform && !cached_audio_levels.waveform_data.empty()) {
                    // Batch add samples to reduce mutex contention
                    // Add all samples at once instead of one-by-one
                    waveform->add_samples_batch(cached_audio_levels.waveform_data);
                } else if (waveform) {
                    // Fallback: use current level if no waveform data
                    waveform->add_sample(cached_audio_levels.current_level);
                }
                
                // Async lyrics fetching (only if enabled and track changed)
                if (config.enable_lyrics && playback_state.current_track.id != last_lyrics_track_id) {
                    try {
                        // Clear old lyrics and lines
                        current_lyrics.clear();
                        lyrics_lines.clear();
                        synced_lyrics.clear();
                        lyrics_scroll_position = 0;
                        
                        // Queue async lyrics request (non-blocking)
                        std::string immediate_result = client.get_lyrics(playback_state.current_track);
                        if (!immediate_result.empty()) {
                            // Got lyrics immediately (from Plex metadata)
                            current_lyrics = immediate_result;
                            last_lyrics_track_id = playback_state.current_track.id;
                            if (config.enable_debug_logging) {
                                std::cerr << "[LOG] Got lyrics from Plex metadata for: " 
                                          << playback_state.current_track.title << std::endl;
                            }
                        } else {
                            // Request queued, will check result in next update
                            last_lyrics_track_id = playback_state.current_track.id;
                            if (config.enable_debug_logging) {
                                std::cerr << "[LOG] Queued lyrics request for: " 
                                          << playback_state.current_track.title << " by " 
                                          << playback_state.current_track.artist << std::endl;
                            }
                        }
                    } catch (const std::exception& e) {
                        if (config.enable_debug_logging) {
                            std::cerr << "[LOG] Exception queueing lyrics: " << e.what() << std::endl;
                        }
                        current_lyrics.clear();
                        lyrics_lines.clear();
                        synced_lyrics.clear();
                    } catch (...) {
                        if (config.enable_debug_logging) {
                            std::cerr << "[LOG] Unknown exception queueing lyrics" << std::endl;
                        }
                        current_lyrics.clear();
                        lyrics_lines.clear();
                        synced_lyrics.clear();
                    }
                }
                
                // Check if async lyrics are ready (if we queued a request)
                // Optimize: Only check periodically (every 500ms) instead of every frame to reduce API calls
                if (config.enable_lyrics && !playback_state.current_track.id.empty() && 
                    playback_state.current_track.id == last_lyrics_track_id) {
                    try {
                        // First check for time-synced lyrics (LRCLIB) - check periodically, not every frame
                        static std::chrono::steady_clock::time_point last_lyrics_check = std::chrono::steady_clock::now();
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_lyrics_check).count();
                        
                        if (synced_lyrics.empty() && elapsed >= 500) {  // Check every 500ms instead of every frame
                            last_lyrics_check = now;
                            std::vector<LyricLine> retrieved = client.get_synced_lyrics(playback_state.current_track.id);
                            if (!retrieved.empty()) {
                                synced_lyrics = retrieved;  // Store the lyrics
                                if (config.enable_debug_logging) {
                                    std::cerr << "[LOG] Time-synced lyrics retrieved and stored for track: " << playback_state.current_track.title 
                                              << " by " << playback_state.current_track.artist 
                                              << " (track_id=" << playback_state.current_track.id
                                              << ", " << synced_lyrics.size() << " lines)" << std::endl;
                                }
                            }
                            // No stderr when waiting - was causing UI jump every ~2s (config-based logging only)
                        } else if (!synced_lyrics.empty()) {
                            // Lyrics already stored - log once only when debug enabled
                            if (config.enable_debug_logging) {
                                static bool logged_stored = false;
                                static std::string last_track_id;
                                if (last_track_id != playback_state.current_track.id) {
                                    logged_stored = false;
                                    last_track_id = playback_state.current_track.id;
                                }
                                if (!logged_stored && !synced_lyrics.empty()) {
                                    std::cerr << "[LOG] Synced lyrics already stored: " << synced_lyrics.size() << " lines for track_id=" << playback_state.current_track.id << std::endl;
                                    logged_stored = true;
                                }
                            }
                        }
                        // Fallback to regular lyrics only if we don't have synced lyrics
                        // Optimize: Only check periodically (every 500ms) instead of every frame
                        if (synced_lyrics.empty() && current_lyrics.empty()) {
                            static std::chrono::steady_clock::time_point last_plain_lyrics_check = std::chrono::steady_clock::now();
                            auto now = std::chrono::steady_clock::now();
                            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_plain_lyrics_check).count();
                            
                            if (elapsed >= 500) {  // Check every 500ms instead of every frame
                                last_plain_lyrics_check = now;
                                std::string async_result = client.get_lyrics_result(playback_state.current_track.id);
                                if (!async_result.empty()) {
                                    current_lyrics = async_result;
                                    if (config.enable_debug_logging) {
                                        std::cerr << "[LOG] Lyrics retrieved for track: " << playback_state.current_track.title 
                                                  << " by " << playback_state.current_track.artist 
                                                  << " (" << async_result.length() << " chars)" << std::endl;
                                    }
                                } else {
                                    // Check if still in progress - log every 5 seconds if still waiting
                                    static std::chrono::steady_clock::time_point last_wait_log = std::chrono::steady_clock::now();
                                    auto wait_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_wait_log).count();
                                    if (wait_elapsed >= 5) {  // Log every 5 seconds if still waiting
                                        if (config.enable_debug_logging) {
                                            std::cerr << "[LOG] Still waiting for lyrics: " << playback_state.current_track.title 
                                                      << " by " << playback_state.current_track.artist << std::endl;
                                        }
                                        last_wait_log = now;
                                    }
                                }
                            }
                        }
                    } catch (...) {
                        // Ignore errors checking async result
                    }
                }

                // Prefetch lyrics for next track when possible
                if (config.enable_lyrics && browse_mode == BrowseMode::Tracks && !browse_tracks.empty()) {
                    int current_idx = -1;
                    for (size_t i = 0; i < browse_tracks.size(); ++i) {
                        if (browse_tracks[i].id == playback_state.current_track.id) {
                            current_idx = static_cast<int>(i);
                            break;
                        }
                    }
                    if (current_idx >= 0 && current_idx + 1 < static_cast<int>(browse_tracks.size())) {
                        const Track& next_track = browse_tracks[current_idx + 1];
                        if (next_track.id != prefetch_next_track_id && !next_track.id.empty()) {
                            client.get_lyrics(next_track);
                            prefetch_next_track_id = next_track.id;
                        }
                    }
                }
            } else {
                cached_audio_levels = AudioLevels();
            }
    } catch (...) {}
}

void PlayerView::draw() {
    // Safety check - ensure terminal has valid dimensions (btop-style: minimum size)
    int w = term.width();
    int h = term.height();
    
    // Log if we're drawing player view (only if debug logging enabled)
    if (config.enable_debug_logging) {
        static bool last_was_player = false;
        if (current_view == ViewMode::Player && !last_was_player) {
            std::cerr << "[LOG] Drawing player view for first time" << std::endl;
            last_was_player = true;
        } else if (current_view != ViewMode::Player) {
            last_was_player = false;
        }
    }
    
    if (w <= 0 || h <= 0) {
        // Terminal not ready, skip drawing
        return;
    }
    
    // btop-style: Check minimum terminal size
    if (w < 80 || h < 24) {
        // Terminal too small - show warning message
        term.clear();
        std::string warning = "Terminal too small! Minimum size: 80x24 (current: " + 
                             std::to_string(w) + "x" + std::to_string(h) + ")";
        std::string orange = term.fg_color(255, 140, 0);
        std::string reset = term.reset_color();
        term.draw_text(0, 0, orange + warning + reset);
        term.draw_text(0, 1, "Resize terminal window to continue...");
        term.flush();
        return;
    }
    
    // Track view changes to clear screen properly
    static ViewMode last_view = ViewMode::Player;
    bool view_changed = (last_view != current_view);
    last_view = current_view;
    
    // Track terminal size changes (btop-style: detect resize)
    static int last_term_width = 0, last_term_height = 0;
    bool terminal_resized = (w != last_term_width || h != last_term_height);
    if (terminal_resized) {
        last_term_width = w;
        last_term_height = h;
        need_bg_fill = true;  // Force full redraw on resize
        term.clear();  // Clear screen on resize (btop-style)
    }
    
    // Clear screen on first draw or view change (btop-style: full clear on view change)
    static bool first_draw = true;
    if (first_draw || view_changed || terminal_resized) {
        // Always clear screen on view change to remove fragments (btop-style)
        // Only clear once (remove duplicate check)
        term.clear();  // Full screen clear to remove all fragments
        first_draw = false;
        need_bg_fill = true;  // Force background fill on view change or resize
        // Load library data on first draw (but don't crash if it fails)
        // Do this in a separate try-catch to isolate any crashes
        try {
            if (client.is_connected()) {
                if (music_library_id < 0) {
                    music_library_id = client.get_music_library_id();
                }
                if (music_library_id > 0 && playlists.empty()) {
                    load_library_data();
                }
            }
        } catch (...) {
            // Library loading failed - continue without it
            if (music_library_id < 0) {
                music_library_id = -1;
            }
        }
    }
    
    // Only fill background on first draw or view change (not every frame)
    if (need_bg_fill) {
        // Fill entire screen with uniform black background
        int w = term.width();
        int h = term.height();
        // Safety check - ensure valid dimensions (btop-style: enforce reasonable limits)
        if (w <= 0 || w > 1000) w = 80;  // Max reasonable width
        if (h <= 0 || h > 1000) h = 24;  // Max reasonable height
        
        if (w > 0 && h > 0) {
            std::string black_bg = term.bg_color(0, 0, 0);
            std::string reset = term.reset_color();
            // Fill entire screen - border will be drawn last and overwrite border positions
            for (int y = 0; y < h; ++y) {
                std::string line(w, ' ');
                term.draw_text(0, y, black_bg + line + reset);
            }
        }
        need_bg_fill = false;
    }
    
    auto layout = calculate_layout();
    
    // Always draw colorful sidebar
    draw_sidebar();
    
    // Draw based on current view mode
    // Always clear main content area first to remove fragments (btop-style)
    int sidebar_w = 30;
    // w and h already declared at top of function - reuse them
    // Safety check - ensure valid dimensions (btop-style: enforce reasonable limits)
    if (w <= 0 || w > 1000) w = 80;  // Max reasonable width
    if (h <= 0 || h > 1000) h = 24;  // Max reasonable height
    
    // Clear main content area on view change to remove fragments (btop-style)
    if (view_changed && w > sidebar_w && h > 0) {
        std::string clear_bg = term.bg_color(0, 0, 0);
        int main_w = w - sidebar_w;
        if (main_w > 0 && main_w <= 1000) {
            // Fill entire main content area with black to remove all fragments
            // This includes album art area which can leave fragments
            for (int y = 0; y < h - 1 && y < 1000; ++y) {  // Don't overwrite status bar
                std::string line(main_w, ' ');
                term.draw_text(sidebar_w, y, clear_bg + line + term.reset_color());
            }
        }
        
        // Explicitly clear album art area to remove fragments (btop-style: aggressive clearing)
        // Clear both Player view album art position and Library view album art position
        // layout is already calculated above, reuse it
        if (layout.album_art_w > 0 && layout.album_art_h > 0) {
            // Clear Player view album art area (top-left of main content)
            if (layout.album_art_x >= 0 && layout.album_art_y >= 0 &&
                layout.album_art_x + layout.album_art_w <= w &&
                layout.album_art_y + layout.album_art_h <= h) {
                for (int y = 0; y < layout.album_art_h; ++y) {
                    std::string line(layout.album_art_w, ' ');
                    term.draw_text(layout.album_art_x, layout.album_art_y + y, 
                                  clear_bg + line + term.reset_color());
                }
            }
            
            // Also clear potential Library view album art area (top-right corner)
            // Library view might draw album art in a different position
            int lib_art_x = w - layout.album_art_w - 2;  // Top-right corner
            int lib_art_y = 2;
            if (lib_art_x >= sidebar_w && lib_art_x + layout.album_art_w <= w &&
                lib_art_y >= 0 && lib_art_y + layout.album_art_h <= h) {
                for (int y = 0; y < layout.album_art_h; ++y) {
                    std::string line(layout.album_art_w, ' ');
                    term.draw_text(lib_art_x, lib_art_y + y, 
                                  clear_bg + line + term.reset_color());
                }
            }
        }
    }
    
    // Draw top menu bar (btop-style: always visible at top)
    draw_top_menu_bar();
    
    // Draw view-specific content first (before border so border is always on top)
    if (current_view == ViewMode::Library || current_view == ViewMode::Search) {
        draw_library_view(layout);
    } else {
        // Player view - btop style (black bg throughout)
        // Ensure main content area has black background
        if (w > sidebar_w && h > 0) {
            std::string black_bg = term.bg_color(0, 0, 0);
            int main_w = w - sidebar_w;
            if (main_w > 0 && main_w <= 1000) {  // Additional safety check
                for (int y = 0; y < h - 1 && y < 1000; ++y) {  // Don't overwrite status bar
                    std::string line(main_w, ' ');
                    term.draw_text(sidebar_w, y, black_bg + line + term.reset_color());
                }
            }
        }
        
        draw_album_art(layout);
        draw_title(layout);  // Draw title above waveform
        draw_waveform(layout);  // Draw waveform next to album art
        draw_track_info(layout);
        draw_progress_bar(layout);
        draw_controls(layout);
    }
    
    // Always draw status bar before border
    draw_status_bar(layout);
    
    // ALWAYS draw border LAST so it's on top of everything (btop-style: border always visible)
    // This ensures the complete outer border (top, right, bottom, left) is always visible
    draw_separators(layout);
    
    // Draw top menu bar AFTER border (btop-style: menu bar inside border at top)
    draw_top_menu_bar();
    
    // Draw options menu overlay if active (btop-style: draw on top)
    if (options_menu_active) {
        draw_options_menu();
    }
    
    term.flush();
}

PlayerView::Layout PlayerView::calculate_layout() {
    Layout layout;
    
    int w = term.width();
    int h = term.height();
    
    // Safety check - ensure valid dimensions (btop-style: enforce reasonable limits)
    if (w < 80) w = 80;
    if (h < 24) h = 24;
    if (w > 1000) w = 1000;
    if (h > 1000) h = 1000;
    
    int sidebar_w = 30;
    
    // Album art: fixed position top-left (player view = smaller, less intrusive)
    layout.album_art_w = 40;
    layout.album_art_h = 20;
    layout.album_art_x = sidebar_w + 2;
    layout.album_art_y = 2;
    
    layout.waveform_w = w - sidebar_w - layout.album_art_w - 8;
    layout.waveform_x = layout.album_art_x + layout.album_art_w + 2;
    // Layout: row 1-2 orange lines (y=0,1), row 3 space (y=2), row 4 title (y=3), row 5 separator (y=4)
    // But user reports separator is at y=5, so title must be at y=4
    const int top_buffer_lines = 4;  // Title on row 4 (y=4, which is the 5th row from top, 1-based)
    layout.title_y = top_buffer_lines;
    layout.title_x = layout.waveform_x;
    // Waveform: 1 line buffer above, centerline 6 lines down from separator
    // Separator at y=5, centerline at y=5+6=11, waveform 9 lines high
    // For 9 lines with center at y=11: top at y=7, bottom at y=15
    // So: 1 line buffer (y=6), waveform starts at y=7, height 9, centerline at y=11
    const int separator_y = layout.title_y + 1;  // y=5 (row 6, 1-based)
    const int waveform_centerline_y = separator_y + 7;  // y=12 (row 13, 7 lines from separator at y=5)
    const int waveform_height = 9;
    layout.waveform_y = waveform_centerline_y - (waveform_height / 2);  // y=8 (row 9, since 12-4=8)
    
    layout.track_info_x = layout.album_art_x;
    layout.controls_x = sidebar_w + 2;
    layout.status_bar_y = h - 1;
    
    // btop-style: bottom-anchor lower block so it collapses on resize (no dead space).
    // Row usage: status h-1; controls h-6..h-3; progress h-7; track_info h-14..h-8; orange h-15.
    const int bottom_block_rows = 15;
    int orange_line_y = h - bottom_block_rows;
    layout.track_info_y = orange_line_y + 1;
    layout.progress_bar_y = h - 7;
    layout.controls_y = h - 6;
    
    // Waveform + lyrics pack above orange (lyrics directly under waveform, no dead space).
    // Lyrics: 5 lines as requested by user
    const int lyrics_lines = 5;
    const int lyrics_gap = 1;
    // Waveform height is fixed at 9 lines for default view, but can shrink if needed
    int space_above_orange = orange_line_y - lyrics_gap - layout.waveform_y;
    int max_waveform_h = std::max(0, space_above_orange - lyrics_lines - lyrics_gap);
    // Use fixed 9 lines if space allows, otherwise use available space
    if (max_waveform_h >= 9) {
        layout.waveform_h = 9;  // Fixed 9 lines as requested
    } else {
        layout.waveform_h = max_waveform_h;  // Shrink on smaller screens
        //layout.waveform_h = 9;
    }

    return layout;
}

void PlayerView::draw_separators(const Layout& layout) {
    // btop-style separators: orange box drawing characters with rounded corners
    std::string black_bg = term.bg_color(0, 0, 0);
    std::string orange_color = term.fg_color(255, 140, 0);  // Plex orange
    
    int w = term.width();
    int h = term.height();
    // Safety check - ensure valid dimensions (btop-style: enforce reasonable limits)
    if (w <= 0 || h <= 0 || w > 1000 || h > 1000) {
        return;  // Skip drawing if dimensions are invalid
    }
    int sidebar_w = 30;
    
    // Unicode box drawing characters (btop style)
    std::string hline = "─";  // Horizontal line
    std::string vsep = "│";   // Vertical line
    std::string top_left = "╭";
    std::string top_right = "╮";
    std::string bottom_left = "╰";
    std::string bottom_right = "╯";
    std::string left_conn = "├";
    std::string right_conn = "┤";
    
    // Draw complete border around entire application (btop style) - ALWAYS draw this
    // Safety check: ensure valid dimensions
    if (w > 2 && h > 2 && w > 0 && h > 0) {
        // Top border with rounded corners (row 0)
        // Left corner is drawn here, the menu bar draws the line with options interruption
        if (w > 0) {
            term.draw_text(0, 0, black_bg + orange_color + top_left + term.reset_color());
        }
        
        // Right corner will be drawn after menu bar
        
        // Right corner will be drawn after menu bar completes the line
        
        // Second top line (mirror bottom twin lines) – row 1 (full width, no interruption)
        if (h > 2) {
            for (int i = 1; i < w - 1 && i < w; ++i) {
                term.draw_text(i, 1, black_bg + orange_color + hline + term.reset_color());
            }
        }
        
        // Left and right borders
        for (int y = 1; y < h - 1 && y < h; ++y) {
            term.draw_text(0, y, black_bg + orange_color + vsep + term.reset_color());
            if (w - 1 >= 0 && w - 1 < w) {
                term.draw_text(w - 1, y, black_bg + orange_color + vsep + term.reset_color());
            }
        }
        
        // Bottom border with rounded corners
        if (h - 1 >= 0 && h - 1 < h) {
            term.draw_text(0, h - 1, black_bg + orange_color + bottom_left + term.reset_color());
            for (int i = 1; i < w - 1 && i < w; ++i) {
                term.draw_text(i, h - 1, black_bg + orange_color + hline + term.reset_color());
            }
            if (w - 1 >= 0 && w - 1 < w) {
                term.draw_text(w - 1, h - 1, black_bg + orange_color + bottom_right + term.reset_color());
            }
        }
    }
    
    // Vertical separator between sidebar and main content - ALWAYS draw this
    // Start at y=2 to avoid interrupting the horizontal twin line at row 1
    if (sidebar_w > 0 && sidebar_w < w && h > 2) {
        for (int y = 2; y < h - 1 && y < h; ++y) {  // Start at y=2 to not interrupt top twin lines
            term.draw_text(sidebar_w, y, black_bg + orange_color + vsep + term.reset_color());
        }
    }
    
    // Internal separators - only draw in Player view
    if (current_view == ViewMode::Player) {
        // Horizontal separator between album art/waveform and track info (inside main area)
        // Only draw if layout is valid and separator position is within bounds
        if (layout.status_bar_y > 0 && layout.track_info_y > 0 && layout.track_info_y < h) {
            int separator_y = layout.track_info_y - 1;  // One line above track info
            if (separator_y >= 1 && separator_y < h - 1 && separator_y < layout.status_bar_y) {
                // Use connector characters for cleaner intersection
                if (sidebar_w >= 0 && sidebar_w < w) {
                    term.draw_text(sidebar_w, separator_y, black_bg + orange_color + left_conn + term.reset_color());
                }
                for (int i = sidebar_w + 1; i < w - 1 && i >= 0 && i < w; ++i) {
                    term.draw_text(i, separator_y, black_bg + orange_color + hline + term.reset_color());
                }
                if (w - 1 >= 0 && w - 1 < w) {
                    term.draw_text(w - 1, separator_y, black_bg + orange_color + right_conn + term.reset_color());
                }
            }
        }
        
        // Horizontal separator above status bar (inside border)
        if (layout.status_bar_y > 0 && layout.status_bar_y <= h) {
            int status_separator_y = layout.status_bar_y - 1;
            if (status_separator_y >= 1 && status_separator_y < h - 1) {
                for (int i = 1; i < w - 1 && i >= 0 && i < w; ++i) {
                    term.draw_text(i, status_separator_y, black_bg + orange_color + hline + term.reset_color());
                }
            }
        }
    }
}

void PlayerView::draw_top_menu_bar() {
    // btop-style top menu bar: "options" interrupts the top orange line (row 0)
    // Format: ┌──┐options┌────────────────────────────
    // Note: Top left corner (╭) is already drawn at x=0, we start at x=1
    int w = term.width();
    int h = term.height();
    if (w < 30 || h < 3) return;  // Too small
    
    std::string black_bg = term.bg_color(0, 0, 0);
    std::string orange_color = term.fg_color(255, 140, 0);  // Plex orange
    std::string white = term.fg_color(255, 255, 255);
    
    // Draw on row 0, starting after left corner
    int menu_y = 0;
    int menu_x = 1;  // Start after left corner (╭ at x=0)
    
    // Format: ╭───┐options┌────────────────────────────
    // Top left corner (╭) is already drawn, so we start with ───┐
    // All box characters and lines are orange
    std::string box_left = "┌";
    std::string box_hline = "─";
    std::string options_label = orange_color + "o" + white + "ptions";  // 'o' in orange, rest white
    
    // Build: ───┐options┌ (all orange except 'ptions' text)
    // Note: Top left corner ╭ is already drawn at x=0
    std::string menu_item = orange_color + box_hline + box_hline + box_hline + "┐" + 
                           options_label + 
                           orange_color + box_left;
    
    // Calculate actual display width (excluding ANSI codes)
    // ───┐ = 4 chars, options = 7 chars, ┌ = 1 char = 12 chars total
    int menu_item_width = 4 + 7 + 1;  // ───┐ + "options" + ┌
    
    // Draw the menu item at row 0
    term.draw_text(menu_x, menu_y, black_bg + menu_item + term.reset_color());
    
    // Continue the orange line after the menu item to fill remaining width
    int line_start_x = menu_x + menu_item_width;
    for (int x = line_start_x; x < w - 1 && x < w; ++x) {
        term.draw_text(x, menu_y, black_bg + orange_color + box_hline + term.reset_color());
    }
    
    // Draw right corner
    if (w - 1 >= 0 && w - 1 < w) {
        term.draw_text(w - 1, menu_y, black_bg + orange_color + "╮" + term.reset_color());
    }
}

void PlayerView::draw_title(const Layout& layout) {
    // Draw application title above waveform (btop style)
    std::string black_bg = term.bg_color(0, 0, 0);
    std::string orange_color = term.fg_color(255, 140, 0);  // Plex orange
    std::string title = "plex-tui";
    
    // Only draw if title position is valid
    if (layout.title_y >= 0) {
        // Draw title
        term.draw_text(layout.title_x, layout.title_y,
                      black_bg + orange_color + title + term.reset_color());
        
        // Draw single orange === line below title (btop style)
        int separator_y = layout.title_y + 1;
        if (separator_y >= 0 && separator_y < term.height()) {
            int separator_width = layout.waveform_w;
            if (separator_width > 0 && separator_width <= 1000) {
                std::string separator_line(separator_width, '=');
                term.draw_text(layout.title_x, separator_y,
                              black_bg + orange_color + separator_line + term.reset_color());
            }
        }
    }
}

void PlayerView::draw_waveform(const Layout& layout) {
    // btop-style: No box, just waveform on black background
    // Draw waveform with vibrant colors directly (if enabled)
    if (config.enable_waveform && waveform) {
        // Validate layout dimensions before using
        if (layout.waveform_w > 0 && layout.waveform_h > 0 && 
            layout.waveform_x >= 0 && layout.waveform_y >= 0) {
            try {
                waveform->set_size(layout.waveform_w, layout.waveform_h);
                waveform->draw(term, layout.waveform_x, layout.waveform_y, config.theme);
            } catch (...) {
                // Ignore waveform drawing errors - don't crash UI
            }
        }
    }
    
    // Draw scrolling lyrics under waveform (player view only, while playing)
    // Only draw if we have a valid track and waveform is visible
    // NOTE: Lyrics fetching is disabled due to curl thread-safety issues
    // Drawing will only show cached lyrics if they were previously fetched
    // Allow lyrics to display even when paused (user might want to see them)
    if (config.enable_lyrics && current_view == ViewMode::Player && waveform && client.is_connected() &&
        (!playback_state.current_track.id.empty() || pending_play)) {
        try {
            draw_lyrics(layout);
        } catch (const std::exception& e) {
            if (config.enable_debug_logging) {
                std::cerr << "[LOG] Exception in draw_lyrics(): " << e.what() << std::endl;
            }
        } catch (...) {
            if (config.enable_debug_logging) {
                std::cerr << "[LOG] Unknown exception in draw_lyrics()" << std::endl;
            }
        }
    } else if (config.enable_lyrics && config.enable_debug_logging) {
        if (current_view != ViewMode::Player) {
            std::cerr << "[LOG] Lyrics not drawn: wrong view mode" << std::endl;
        } else if (playback_state.current_track.id.empty() && !pending_play) {
            std::cerr << "[LOG] Lyrics not drawn: no track ID" << std::endl;
        } else if (!waveform) {
            std::cerr << "[LOG] Lyrics not drawn: no waveform" << std::endl;
        } else if (!client.is_connected()) {
            std::cerr << "[LOG] Lyrics not drawn: client not connected" << std::endl;
        }
    }
}

void PlayerView::draw_album_art(const Layout& layout) {
    // In library/tracks view, don't draw Plex logo placeholder - only draw actual album art
    // The Plex logo should only appear in player view when album art is not available
    bool is_library_view = (current_view == ViewMode::Library || current_view == ViewMode::Search);
    
    // Only draw album art if enabled
    if (!config.enable_album_art) {
        // Only show Plex logo in player view, not in library view
        if (!is_library_view) {
            draw_plex_logo_placeholder(layout);
        }
        return;
    }
    
    // Wrap entire function in try-catch to prevent crashes
    try {
        AlbumArt* art = client.get_album_art();
        
        if (!art || !art->has_art()) {
            // Draw pixelated Plex logo placeholder (btop style) - only in player view
            if (!is_library_view) {
                draw_plex_logo_placeholder(layout);
            }
            return;
        }
        
            // Validate layout dimensions before rendering
            if (layout.album_art_w <= 0 || layout.album_art_h <= 0 || 
                layout.album_art_w > 1000 || layout.album_art_h > 1000) {
                if (!is_library_view) {
                    draw_plex_logo_placeholder(layout);
                }
                return;
            }
            
            // Render pixelated album art (already has black bg in render_pixelated)
            try {
                auto art_lines = art->render_pixelated(layout.album_art_w, layout.album_art_h, config.theme);
                
                // Validate art_lines before drawing
                if (art_lines.empty()) {
                    if (!is_library_view) {
                        draw_plex_logo_placeholder(layout);
                    }
                    return;
                }
                
                // Validate coordinates before drawing
                int h = term.height();
                int w = term.width();
                if (layout.album_art_x < 0 || layout.album_art_y < 0 || 
                    layout.album_art_x >= w || layout.album_art_y >= h) {
                    if (!is_library_view) {
                        draw_plex_logo_placeholder(layout);
                    }
                    return;
                }
            
            for (size_t y = 0; y < art_lines.size() && y < static_cast<size_t>(layout.album_art_h); ++y) {
                int draw_y = layout.album_art_y + static_cast<int>(y);
                if (draw_y >= 0 && draw_y < h && layout.album_art_x >= 0 && layout.album_art_x < w) {
                    term.draw_text(layout.album_art_x, draw_y, art_lines[y]);
                }
            }
        } catch (...) {
            // Rendering failed - show placeholder (only in player view)
            if (!is_library_view) {
                draw_plex_logo_placeholder(layout);
            }
        }
    } catch (...) {
        // Any error - show placeholder (only in player view)
        if (!is_library_view) {
            draw_plex_logo_placeholder(layout);
        }
    }
}

void PlayerView::draw_plex_logo_placeholder(const Layout& layout) {
    // Render "PLEX" text in solid blocks (btop-style, like BTOP logo)
    // P, L, E are white; X has white left diagonal and orange right diagonal (>) with orange center
    std::string black_bg = term.bg_color(0, 0, 0);
    
    // Colors
    std::string white_color = term.fg_color(255, 255, 255);
    std::string orange_color = term.fg_color(255, 140, 0);  // Plex orange
    
    // Center the text
    int center_x = layout.album_art_w / 2;
    int center_y = layout.album_art_h / 2;
    
    // Simple 5x7 pixel font pattern for "PLEX" in solid blocks
    // Each letter is 5 columns wide, with 1 column spacing between letters
    // Total width: 4 letters * 5 cols + 3 spaces = 23 columns
    int text_start_x = center_x - 11;  // Center the text
    int text_start_y = center_y - 3;   // Center vertically
    
    // Letter patterns (5x7 grid, stored as rows)
    // Each row is a byte where bits represent filled pixels
    struct LetterPattern {
        uint8_t rows[7];  // 7 rows per letter
    };
    
    // P pattern
    LetterPattern P = {
        {0b11111,  // █████
         0b10001,  // █   █
         0b10001,  // █   █
         0b11111,  // █████
         0b10000,  // █
         0b10000,  // █
         0b10000}  // █
    };
    
    // L pattern
    LetterPattern L = {
        {0b10000,  // █
         0b10000,  // █
         0b10000,  // █
         0b10000,  // █
         0b10000,  // █
         0b10000,  // █
         0b11111}  // █████
    };
    
    // E pattern
    LetterPattern E = {
        {0b11111,  // █████
         0b10000,  // █
         0b10000,  // █
         0b11110,  // ████
         0b10000,  // █
         0b10000,  // █
         0b11111}  // █████
    };
    
    // X pattern - left diagonal (white) and right diagonal (orange)
    LetterPattern X = {
        {0b10001,  // █   █
         0b01010,  //  █ █
         0b00100,  //   █
         0b00100,  //   █
         0b01010,  //  █ █
         0b10001,  // █   █
         0b10001}  // █   █
    };
    
    LetterPattern letters[] = {P, L, E, X};
    
    // Render each letter using solid block characters (█)
    for (int letter_idx = 0; letter_idx < 4; ++letter_idx) {
        int letter_x = text_start_x + letter_idx * 6;  // 5 cols + 1 space
        bool is_x = (letter_idx == 3);  // X is the 4th letter (index 3)
        
        for (int row = 0; row < 7; ++row) {
            int draw_y = layout.album_art_y + text_start_y + row;
            if (draw_y < layout.album_art_y || draw_y >= layout.album_art_y + layout.album_art_h) continue;
            
            // Render each column of the letter as solid blocks
            for (int col = 0; col < 5; ++col) {
                int draw_x = layout.album_art_x + letter_x + col;
                if (draw_x < layout.album_art_x || draw_x >= layout.album_art_x + layout.album_art_w) continue;
                
                // Check if this pixel should be filled
                uint8_t pattern = letters[letter_idx].rows[row];
                bool filled = (pattern & (0x10 >> col)) != 0;  // Check bit for this column
                
                if (filled) {
                    // Determine color: white for P, L, E, and left diagonal of X
                    // Orange for right diagonal (>) and center of X
                    std::string color = white_color;
                    
                    if (is_x) {
                        // X pattern analysis:
                        // The '>' shape should point right (top-left to bottom-right diagonal)
                        // Row 0: cols 0,4 → col 0=orange (> top), col 4=white
                        // Row 1: cols 1,3 → col 1=orange (> middle), col 3=white
                        // Row 2: col 2 → col 2=orange (> center/intersection)
                        // Row 3: col 2 → col 2=orange (> center/intersection)
                        // Row 4: cols 1,3 → col 1=orange (> middle), col 3=white
                        // Row 5: cols 0,4 → col 0=orange (> bottom), col 4=white
                        // Row 6: cols 0,4 → col 0=orange (> bottom), col 4=white
                        
                        // Entire '>' (left diagonal, top-left to bottom-right) should be orange:
                        // - Column 0: rows 0, 5, 6 (left edge - part of >)
                        // - Column 1: rows 1, 4 (left diagonal middle - part of >)
                        // - Column 2: rows 2, 3 (center/intersection - part of >)
                        bool is_orange = (col == 0) ||                    // Left edge - part of > pointing right
                                         (col == 1 && (row == 1 || row == 4)) ||  // Left diagonal middle - part of >
                                         (col == 2 && (row == 2 || row == 3));     // Center/intersection - part of >
                        
                        if (is_orange) {
                            color = orange_color;  // Orange for entire '>' (left diagonal pointing right)
                        }
                    }
                    
                    // Draw solid block (█)
                    term.draw_text(draw_x, draw_y, black_bg + color + "█" + term.reset_color());
                }
            }
        }
    }
}

void PlayerView::draw_track_info(const Layout& layout) {
    // Defensive check - ensure playback_state is valid
    if (!client.is_connected()) {
        return;
    }
    
    const Track& track = playback_state.current_track;
    
    if (track.title.empty()) {
        std::string msg = "No track playing";
        std::string color = term.fg_color(150, 150, 150);
        std::string black_bg = term.bg_color(0, 0, 0);
        term.draw_text(layout.track_info_x, layout.track_info_y, 
                      black_bg + color + msg + term.reset_color());
        return;
    }
    
    // btop style: black background for all text
    std::string black_bg = term.bg_color(0, 0, 0);
    
    // Title - Large and bright white
    std::string title_color = term.fg_color(255, 255, 255);
    std::string title_text = track.title;
    if (title_text.length() > 40) {
        title_text = title_text.substr(0, 37) + "...";
    }
    term.draw_text(layout.track_info_x, layout.track_info_y,
                  black_bg + title_color + title_text + term.reset_color());
    
    // Artist - Bright white
    std::string artist_color = term.fg_color(220, 220, 220);
    std::string artist_text = track.artist;
    if (artist_text.length() > 40) {
        artist_text = artist_text.substr(0, 37) + "...";
    }
    term.draw_text(layout.track_info_x, layout.track_info_y + 2,
                  black_bg + artist_color + artist_text + term.reset_color());
    
    // Album - Dim white
    std::string album_color = term.fg_color(180, 180, 180);
    std::string album_text = track.album;
    if (album_text.length() > 40) {
        album_text = album_text.substr(0, 37) + "...";
    }
    term.draw_text(layout.track_info_x, layout.track_info_y + 4,
                  black_bg + album_color + album_text + term.reset_color());
    
    // Show decoding/playback status
    if (client.is_connected() && !playback_state.current_track.title.empty()) {
        std::string status_text;
        std::string status_color;
        
        // Check if actually decoding
        if (playback_state.playing) {
            // Use cached audio levels to avoid multiple calls per frame
            if (cached_audio_levels.waveform_data.empty() || cached_audio_levels.current_level == 0.0f) {
                status_text = "Starting playback...";
                status_color = term.fg_color(255, 200, 100);  // Orange/yellow
            } else {
                status_text = "Playing";
                status_color = term.fg_color(100, 255, 150);  // Green
            }
        } else {
            status_text = "Paused";
            status_color = term.fg_color(200, 200, 200);  // Gray
        }
        
        std::string black_bg = term.bg_color(0, 0, 0);
        term.draw_text(layout.track_info_x, layout.track_info_y + 6,
                      black_bg + status_color + status_text + term.reset_color());
    }
    
    // Metadata - Dimmed but colorful
    if (track.year > 0 || !track.genre.empty()) {
        std::string meta_color = term.fg_color(config.theme.dimmed.r,
                                              config.theme.dimmed.g,
                                              config.theme.dimmed.b);
        std::string meta = "";
        if (track.year > 0) meta += std::to_string(track.year);
        if (!track.genre.empty()) {
            if (!meta.empty()) meta += " • ";
            meta += track.genre;
        }
        if (!meta.empty()) {
            std::string black_bg = term.bg_color(0, 0, 0);
            term.draw_text(layout.track_info_x, layout.track_info_y + 6,
                          black_bg + meta_color + meta + term.reset_color());
        }
    }
}

void PlayerView::draw_progress_bar(const Layout& layout) {
    const Track& track = playback_state.current_track;
    
    if (track.duration_ms == 0) return;
    
    int sidebar_w = 30;
    int w = term.width();
    if (w <= 0) w = 80;  // Safety check
    
    int bar_width = std::max(10, w - sidebar_w - 10);
    int bar_x = sidebar_w + 2;
    int bar_y = layout.progress_bar_y;  // Use layout position
    
    // Time labels - colorful with black background (btop style)
    std::string black_bg = term.bg_color(0, 0, 0);
    std::string time_color = term.fg_color(config.theme.warning.r, config.theme.warning.g, config.theme.warning.b);
    std::string current_time = format_time(playback_state.position_ms);
    std::string total_time = format_time(track.duration_ms);
    
    term.draw_text(bar_x, bar_y, black_bg + time_color + current_time + term.reset_color());
    term.draw_text(bar_x + bar_width - 5, bar_y, black_bg + time_color + total_time + term.reset_color());
    
    // Progress bar with gradient colors
    float progress = static_cast<float>(playback_state.position_ms) / track.duration_ms;
    int filled = static_cast<int>(progress * bar_width);
    
    // Gradient: cyan -> magenta -> yellow
    for (int i = 0; i < bar_width; ++i) {
        float pos = static_cast<float>(i) / bar_width;
        uint8_t r, g, b;
        
        if (pos < 0.33f) {
            // Cyan to Magenta
            float t = pos / 0.33f;
            r = static_cast<uint8_t>(config.theme.waveform_primary.r + 
                (config.theme.waveform_secondary.r - config.theme.waveform_primary.r) * t);
            g = static_cast<uint8_t>(config.theme.waveform_primary.g + 
                (config.theme.waveform_secondary.g - config.theme.waveform_primary.g) * t);
            b = static_cast<uint8_t>(config.theme.waveform_primary.b + 
                (config.theme.waveform_secondary.b - config.theme.waveform_primary.b) * t);
        } else if (pos < 0.66f) {
            // Magenta to Yellow
            float t = (pos - 0.33f) / 0.33f;
            r = static_cast<uint8_t>(config.theme.waveform_secondary.r + 
                (config.theme.waveform_tertiary.r - config.theme.waveform_secondary.r) * t);
            g = static_cast<uint8_t>(config.theme.waveform_secondary.g + 
                (config.theme.waveform_tertiary.g - config.theme.waveform_secondary.g) * t);
            b = static_cast<uint8_t>(config.theme.waveform_secondary.b + 
                (config.theme.waveform_tertiary.b - config.theme.waveform_secondary.b) * t);
        } else {
            // Yellow
            r = config.theme.waveform_tertiary.r;
            g = config.theme.waveform_tertiary.g;
            b = config.theme.waveform_tertiary.b;
        }
        
        std::string bar_color = term.fg_color(r, g, b);
        std::string empty_color = term.fg_color(40, 40, 40);
        std::string black_bg = term.bg_color(0, 0, 0);
        
        if (i < filled) {
            term.draw_text(bar_x + 6 + i, bar_y, black_bg + bar_color + "█" + term.reset_color());
        } else {
            term.draw_text(bar_x + 6 + i, bar_y, black_bg + empty_color + "░" + term.reset_color());
        }
    }
}

void PlayerView::draw_sidebar() {
    int sidebar_w = 30;
    int h = term.height();
    
    // Safety check
    if (h <= 0) h = 24;
    if (sidebar_w <= 0) sidebar_w = 30;
    
    // Draw sidebar background (slightly lighter than main, but still dark)
    std::string sidebar_bg = term.bg_color(10, 10, 10);
    // Safety check for sidebar width
    if (sidebar_w > 0 && sidebar_w <= 1000 && h > 0 && h <= 1000) {
        int max_y = std::max(1, h - 1);
        for (int y = 0; y < max_y && y < 1000; ++y) {
            std::string line(sidebar_w, ' ');
            term.draw_text(0, y, sidebar_bg + line + term.reset_color());
        }
    }
    
    // Bright menu items (white text)
    int y = 2;
    std::string bright_white = term.fg_color(255, 255, 255);
    std::string dim_white = term.fg_color(180, 180, 180);
    
    // btop style: black background for all text
    std::string black_bg = term.bg_color(0, 0, 0);
    
    // Home/Player
    std::string home_color = (current_view == ViewMode::Player) ? bright_white : dim_white;
    term.draw_text(2, y++, black_bg + home_color + "Player" + term.reset_color());
    
    // Library
    std::string lib_color = (current_view == ViewMode::Library || current_view == ViewMode::Search) ? bright_white : dim_white;
    term.draw_text(2, y++, black_bg + lib_color + "Library" + term.reset_color());
    
    // Search
    std::string search_color = (current_view == ViewMode::Search) ? bright_white : dim_white;
    term.draw_text(2, y++, black_bg + search_color + "Search" + term.reset_color());
    
    y += 2;
    
    // Playlists section header
    std::string header_color = term.fg_color(150, 150, 150);
    term.draw_text(2, y++, black_bg + header_color + "PLAYLISTS" + term.reset_color());
    
    // Show playlists with scrolling support
    int playlist_y = y;
    int max_visible_playlists = h - playlist_y - 2;  // Leave space at bottom
    if (max_visible_playlists < 1) max_visible_playlists = 1;
    
    // Calculate scroll range for playlists
    int max_playlist_scroll = std::max(0, static_cast<int>(playlists.size()) - max_visible_playlists);
    if (playlist_scroll_offset > max_playlist_scroll) {
        playlist_scroll_offset = max_playlist_scroll;
    }
    if (playlist_scroll_offset < 0) playlist_scroll_offset = 0;
    
    int visible_start = playlist_scroll_offset;
    int visible_end = std::min(visible_start + max_visible_playlists, static_cast<int>(playlists.size()));
    
    for (int i = visible_start; i < visible_end; ++i) {
        if (i >= static_cast<int>(playlists.size())) break;
        
        std::string pl_color = dim_white;
        std::string name = playlists[i].title;
        if (name.length() > 25) name = name.substr(0, 22) + "...";
        // btop style: black background for playlist items
        term.draw_text(2, playlist_y + (i - visible_start), black_bg + pl_color + "  " + name + term.reset_color());
    }
}

void PlayerView::draw_controls(const Layout& layout) {
    int controls_y = layout.controls_y;
    int w = term.width();
    if (w <= 0) w = 80;
    int center_x = w / 2;
    std::string black_bg = term.bg_color(0, 0, 0);
    
    std::string prev_color = term.fg_color(config.theme.foreground.r, config.theme.foreground.g, config.theme.foreground.b);
    std::string play_color = playback_state.playing ?
        term.fg_color(config.theme.success.r, config.theme.success.g, config.theme.success.b) :
        term.fg_color(config.theme.highlight.r, config.theme.highlight.g, config.theme.highlight.b);
    std::string next_color = term.fg_color(config.theme.foreground.r, config.theme.foreground.g, config.theme.foreground.b);
    
    term.draw_text(center_x - 8, controls_y, black_bg + prev_color + "⏮" + term.reset_color());
    term.draw_text(center_x - 2, controls_y, black_bg + play_color + (playback_state.playing ? "⏸" : "▶") + term.reset_color());
    term.draw_text(center_x + 4, controls_y, black_bg + next_color + "⏭" + term.reset_color());
    
    // Speaker icon only (no 100%); one line above Playing
    std::string vol_color = term.fg_color(config.theme.warning.r, config.theme.warning.g, config.theme.warning.b);
    term.draw_text(layout.controls_x, controls_y + 1, black_bg + vol_color + "🔊" + term.reset_color());
    
    // Playing/Paused (green) below speaker
    std::string playing_text = playback_state.playing ? "Playing" : "Paused";
    term.draw_text(layout.controls_x, controls_y + 2,
                  black_bg + play_color + playing_text + term.reset_color());
    
    std::string hint_color = term.fg_color(config.theme.dimmed.r, config.theme.dimmed.g, config.theme.dimmed.b);
    term.draw_text(layout.controls_x, controls_y + 3,
                  black_bg + hint_color + "p:play  space:pause  s:stop  L:library  /:search  q:quit" +
                  term.reset_color());
}

// Strip ANSI escape sequences from a string to prevent color code artifacts
static std::string strip_ansi_escape_sequences(const std::string& str) {
    std::string result;
    result.reserve(str.length());  // Pre-allocate to avoid reallocations
    
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '\033' && i + 1 < str.length() && str[i + 1] == '[') {
            // Found escape sequence start - skip until 'm' or end of string
            size_t j = i + 2;
            while (j < str.length() && str[j] != 'm' && str[j] != 'H' && str[j] != 'J' && str[j] != 'K') {
                // Skip numbers, semicolons, and other escape sequence characters
                if ((str[j] >= '0' && str[j] <= '9') || str[j] == ';' || str[j] == '?') {
                    j++;
                } else {
                    break;  // Not a valid escape sequence, treat as normal text
                }
            }
            if (j < str.length() && (str[j] == 'm' || str[j] == 'H' || str[j] == 'J' || str[j] == 'K')) {
                i = j;  // Skip the entire escape sequence
                continue;
            }
        }
        result += str[i];
    }
    
    return result;
}

void PlayerView::draw_status_bar(const Layout& layout) {
    // Status bar with btop-style orange corners and center divider
    std::string black_bg = term.bg_color(0, 0, 0);
    std::string orange_color = term.fg_color(255, 140, 0);  // Plex orange
    
    int w = term.width();
    // Safety check - ensure valid dimensions (btop-style: enforce reasonable limits)
    if (w <= 0 || w > 1000) {
        w = 80;  // Default width
    }
    int y = layout.status_bar_y;
    // Safety check for y coordinate
    if (y < 0 || y >= 1000) {
        return;  // Skip drawing if y is invalid
    }
    
    // Fill entire status bar with black background (validate width first)
    if (w > 0 && w <= 1000) {  // Safety check
        std::string bar(w, ' ');
        term.draw_text(0, y, black_bg + bar + term.reset_color());
    }
    
    // Draw orange corners (btop style)
    std::string bottom_left = "╰";
    std::string bottom_right = "╯";
    std::string vsep = "│";  // Vertical separator for center divider
    
    // Left corner
    term.draw_text(0, y, black_bg + orange_color + bottom_left + term.reset_color());
    
    // Right corner
    term.draw_text(w - 1, y, black_bg + orange_color + bottom_right + term.reset_color());
    
    std::string play_status = playback_state.playing ? "* Playing" : "  Paused";
    std::string play_color = playback_state.playing ?
        term.fg_color(100, 255, 150) : term.fg_color(200, 200, 200);
    std::string conn_status = client.is_connected() ? "* Connected" : "  Disconnected";
    std::string conn_color = client.is_connected() ?
        term.fg_color(100, 255, 150) : term.fg_color(255, 100, 100);
    
    int center_x = w / 2;
    int status_x = w - 1 - static_cast<int>(conn_status.length()) - 1;
    if (status_x < 0) status_x = 0;
    // Truncate msg so left side never overwrites "Connected" on the right
    int max_msg_len = std::max(0, status_x - 3 - static_cast<int>(play_status.length()));
    // Strip any ANSI escape sequences from status_message to prevent color code artifacts
    std::string msg = status_message.empty() ? "Ready" : strip_ansi_escape_sequences(status_message);
    if (static_cast<int>(msg.length()) > max_msg_len)
        msg = msg.substr(0, static_cast<size_t>(max_msg_len > 3 ? max_msg_len - 3 : 0)) + "...";
    std::string status_fg = term.fg_color(255, 255, 255);
    
    std::string left_status = black_bg + play_color + play_status + term.reset_color() +
                              black_bg + " " + status_fg + msg + term.reset_color();
    term.draw_text(2, y, left_status);
    
    int left_text_end = 2 + static_cast<int>(play_status.length()) + 1 + static_cast<int>(msg.length());
    int right_text_start = w - 1 - static_cast<int>(conn_status.length()) - 2;
    if (left_text_end < center_x && center_x < right_text_start)
        term.draw_text(center_x, y, black_bg + orange_color + vsep + term.reset_color());
    
    term.draw_text(status_x, y, black_bg + conn_color + conn_status + term.reset_color());
}

void PlayerView::handle_input(const InputEvent& event) {
    // Handle options menu input first (if active)
    if (options_menu_active) {
        handle_options_menu_input(event);
        return;
    }
    
    if (event.is_mouse()) {
        // Handle mouse scroll for playlist sidebar or lyrics area
        if (event.mouse.type == MouseEvent::Type::Scroll) {
            if (current_view == ViewMode::Player && event.mouse.x < 30) {
                // Scrolling in sidebar
                if (event.mouse.button == MouseEvent::Button::ScrollUp) {
                    if (playlist_scroll_offset > 0) {
                        playlist_scroll_offset--;
                    }
                } else if (event.mouse.button == MouseEvent::Button::ScrollDown) {
                    int max_scroll = std::max(0, static_cast<int>(playlists.size()) - 5);
                    if (playlist_scroll_offset < max_scroll) {
                        playlist_scroll_offset++;
                    }
                }
                return;
            }
            if (current_view == ViewMode::Player && event.mouse.x >= 30 &&
                synced_lyrics.empty() && !lyrics_lines.empty()) {
                // Scrolling lyrics in main area
                const int visible = 5;  // Match lyrics_lines reservation
                int max_scroll = std::max(0, static_cast<int>(lyrics_lines.size()) - visible);
                if (event.mouse.button == MouseEvent::Button::ScrollUp) {
                    if (lyrics_scroll_position > 0) lyrics_scroll_position--;
                } else if (event.mouse.button == MouseEvent::Button::ScrollDown) {
                    if (lyrics_scroll_position < max_scroll) lyrics_scroll_position++;
                }
                return;
            }
        }
        
        // Check for clicks on top menu bar (btop-style: clickable menu items)
        if (event.mouse.type == MouseEvent::Type::Press && 
            event.mouse.button == MouseEvent::Button::Left) {
            // Top menu bar is at y=0 (row 0), "options" is at x=1 to x=14 (┌──options──┐)
            if (event.mouse.y == 0 && event.mouse.x >= 1 && event.mouse.x <= 14) {
                // Clicked on "options" menu item
                options_menu_active = true;
                options_menu_category = 0;
                options_menu_selected = 0;
                options_menu_editing = false;
                options_menu_edit_buffer.clear();
                return;
            }
        }
        
        handle_mouse_event(event.mouse);
        return;
    }
    
    // Handle library view navigation
    if (current_view == ViewMode::Library || current_view == ViewMode::Search) {
        // Handle search input when search is active (btop-style: allow all alphanumeric in search)
        if (search_active) {
            // In search mode, treat all printable characters as search input
            // Only non-printable keys (like Escape, Enter) are handled specially
            if (event.key == Key::Backspace) {
                // Handle backspace for search (btop-style: immediate feedback)
                handle_search_input('\b');  // Pass backspace character
                return;
            } else if (event.key == Key::Enter) {
                // Enter confirms search
                handle_search_input('\n');
                return;
            } else if (event.key == Key::Escape) {
                // Escape deactivates search
                search_active = false;
                if (search_query.empty()) {
                    browse_tracks.clear();
                    // Clear search/playlist pagination
                    is_search_mode = false;
                    current_search_query.clear();
                    search_loaded_count = 0;
                    current_playlist_id.clear();
                    playlist_total_size = 0;
                    playlist_loaded_count = 0;
                    search_pending = false;
                }
                return;
            } else if (event.key == Key::Pause || event.key == Key::Space) {
                // Space key - add space to search query (don't pause in search mode)
                handle_search_input(' ');
                return;
            } else if (event.key == Key::Char) {
                // All printable characters go to search (including 'm', 'p', 's', etc.)
                handle_search_input(event.character);
                return;
            } else if (event.key == Key::Play || event.key == Key::Stop ||
                       event.key == Key::Next || event.key == Key::Previous ||
                       event.key == Key::VolumeUp || event.key == Key::VolumeDown || event.key == Key::Mute) {
                // Hotkeys that were mapped from characters - convert back to character for search
                // This handles cases where 'm' was mapped to Key::Mute, etc.
                // Note: Key::Pause (space) is handled separately above
                char hotkey_char = '\0';
                if (event.key == Key::Play) hotkey_char = 'p';
                else if (event.key == Key::Stop) hotkey_char = 's';
                else if (event.key == Key::Next) hotkey_char = 'n';
                else if (event.key == Key::Previous) hotkey_char = 'N';
                else if (event.key == Key::VolumeUp) hotkey_char = '+';
                else if (event.key == Key::VolumeDown) hotkey_char = '-';
                else if (event.key == Key::Mute) hotkey_char = 'm';
                
                if (hotkey_char != '\0') {
                    // Treat as search input (btop-style: all alphanumeric in search)
                    handle_search_input(hotkey_char);
                    return;
                }
            }
            // Other keys (like Quit, Help) should still work as hotkeys even in search
        }
        
        switch (event.key) {
            case Key::Up: {
                if (current_view == ViewMode::Player && synced_lyrics.empty() && !lyrics_lines.empty()) {
                    if (lyrics_scroll_position > 0) lyrics_scroll_position--;
                    return;
                }
            }
                if (selected_index > 0) {
                    int old_index = selected_index;
                    selected_index--;
                    // Clear album art when changing selection to prevent stale data
                    if (browse_mode == BrowseMode::Albums && old_index != selected_index && album_art_for_albums) {
                        album_art_for_albums->clear();
                    }
                    // Update scroll offset to keep selection visible
                    if (selected_index < scroll_offset) {
                        scroll_offset = selected_index;
                    }
                }
                break;
            case Key::Down: {
                if (current_view == ViewMode::Player && synced_lyrics.empty() && !lyrics_lines.empty()) {
                    const int visible = 5;  // Match lyrics_lines reservation
                    int max_scroll = std::max(0, static_cast<int>(lyrics_lines.size()) - visible);
                    if (lyrics_scroll_position < max_scroll) lyrics_scroll_position++;
                    return;
                }
            }
            case Key::PageUp: {
                if (current_view == ViewMode::Player && synced_lyrics.empty() && !lyrics_lines.empty()) {
                    lyrics_scroll_position = std::max(0, lyrics_scroll_position - 5);
                    return;
                }
            }
            case Key::PageDown: {
                if (current_view == ViewMode::Player && synced_lyrics.empty() && !lyrics_lines.empty()) {
                    const int visible = 5;  // Match lyrics_lines reservation
                    int max_scroll = std::max(0, static_cast<int>(lyrics_lines.size()) - visible);
                    lyrics_scroll_position = std::min(max_scroll, lyrics_scroll_position + 5);
                    return;
                }
            }
                {
                    int max_idx = -1;
                    if (browse_mode == BrowseMode::Artists) {
                        max_idx = static_cast<int>(artists.size()) - 1;
                    } else if (browse_mode == BrowseMode::Albums) {
                        max_idx = static_cast<int>(albums.size()) - 1;
                    } else if (browse_mode == BrowseMode::Playlists) {
                        max_idx = static_cast<int>(playlists.size()) - 1;
                    } else if (browse_mode == BrowseMode::Tracks) {
                        max_idx = static_cast<int>(browse_tracks.size()) - 1;
                    }
                    
                    if (max_idx >= 0 && selected_index < max_idx) {
                        int old_index = selected_index;
                        selected_index++;
                        // Clear album art when changing selection to prevent stale data
                        if (browse_mode == BrowseMode::Albums && old_index != selected_index && album_art_for_albums) {
                            album_art_for_albums->clear();
                        }
                        // Update scroll offset to keep selection visible
                        int h = term.height();
                        int max_items = std::max(1, h - 6 - 3);
                        if (selected_index >= scroll_offset + max_items) {
                            scroll_offset = selected_index - max_items + 1;
                        }
                    }
                }
                break;
            case Key::Enter:
                select_item();
                break;
            case Key::Escape:
                if (browse_mode == BrowseMode::Tracks) {
                    // Go back to previous browse mode
                    // Check if we came from a playlist
                    if (!playlists.empty()) {
                        browse_mode = BrowseMode::Playlists;
                    } else if (!albums.empty()) {
                        browse_mode = BrowseMode::Albums;
                    } else if (!artists.empty()) {
                        browse_mode = BrowseMode::Artists;
                    }
                    selected_index = 0;
                    scroll_offset = 0;
                    browse_tracks.clear();  // Clear tracks when going back
                    // Clear pagination state
                    current_playlist_id.clear();
                    playlist_total_size = 0;
                    playlist_loaded_count = 0;
                    is_search_mode = false;
                    current_search_query.clear();
                    search_loaded_count = 0;
                    // Clear album info when going back
                    current_album_id.clear();
                    current_album = PlexClient::Album();
                    album_art_for_tracks.reset();
                } else {
                    current_view = ViewMode::Player;
                    // Clear pagination state when leaving tracks view
                    current_playlist_id.clear();
                    playlist_total_size = 0;
                    playlist_loaded_count = 0;
                    is_search_mode = false;
                    current_search_query.clear();
                    search_loaded_count = 0;
                    // Clear album info when leaving library view
                    current_album_id.clear();
                    current_album = PlexClient::Album();
                    album_art_for_tracks.reset();
                }
                break;
            case Key::Char:
                if (event.character == 'a' || event.character == 'A') {
                    browse_mode = BrowseMode::Artists;
                    selected_index = 0;
                    scroll_offset = 0;
                } else if (event.character == 'b' || event.character == 'B') {
                    browse_mode = BrowseMode::Albums;
                    selected_index = 0;
                    scroll_offset = 0;
                } else if (event.character == 'p' || event.character == 'P') {
                    browse_mode = BrowseMode::Playlists;
                    selected_index = 0;
                    scroll_offset = 0;
                    // Ensure playlists are loaded
                    if (playlists.empty() && music_library_id > 0) {
                        load_library_data();
                    }
                } else if (event.character == 'l' || event.character == 'L') {
                    current_view = ViewMode::Library;
                    search_active = false;
                    // Load library data if not already loaded
                    if (music_library_id < 0) {
                        music_library_id = client.get_music_library_id();
                        if (music_library_id > 0) {
                            load_library_data();
                        }
                    }
                }
                break;
            case Key::Search:
                search_active = !search_active;
                if (search_active) {
                    current_view = ViewMode::Search;
                    if (search_query.empty()) {
                        search_query.clear();  // Start fresh
                    }
                } else {
                    // When deactivating search, clear query if empty
                    if (search_query.empty()) {
                        browse_tracks.clear();
                        // Clear search/playlist pagination
                        is_search_mode = false;
                        current_search_query.clear();
                        search_loaded_count = 0;
                        current_playlist_id.clear();
                        playlist_total_size = 0;
                        playlist_loaded_count = 0;
                    }
                }
                break;
            default:
                break;
        }
        return;
    }
    
    // Player view controls
    switch (event.key) {
        case Key::Up:
            if (synced_lyrics.empty() && !lyrics_lines.empty()) {
                if (lyrics_scroll_position > 0) lyrics_scroll_position--;
                return;
            }
            break;
        case Key::Down:
            if (synced_lyrics.empty() && !lyrics_lines.empty()) {
                const int visible = 5;  // Match lyrics_lines reservation
                int max_scroll = std::max(0, static_cast<int>(lyrics_lines.size()) - visible);
                if (lyrics_scroll_position < max_scroll) lyrics_scroll_position++;
                return;
            }
            break;
        case Key::PageUp:
            if (synced_lyrics.empty() && !lyrics_lines.empty()) {
                lyrics_scroll_position = std::max(0, lyrics_scroll_position - 5);
                return;
            }
            break;
        case Key::PageDown:
            if (synced_lyrics.empty() && !lyrics_lines.empty()) {
                const int visible = 5;  // Match lyrics_lines reservation
                int max_scroll = std::max(0, static_cast<int>(lyrics_lines.size()) - visible);
                lyrics_scroll_position = std::min(max_scroll, lyrics_scroll_position + 5);
                return;
            }
            break;
        case Key::Play:
            handle_playback_key(Key::Play);
            break;
        case Key::Pause:
            handle_playback_key(Key::Pause);
            break;
        case Key::Stop:
            handle_playback_key(Key::Stop);
            break;
        case Key::Next:
            handle_playback_key(Key::Next);
            break;
        case Key::Previous:
            handle_playback_key(Key::Previous);
            break;
        case Key::VolumeUp:
            client.set_volume(std::min(1.0f, client.get_volume() + 0.05f));
            status_message = "Volume: " + format_volume(client.get_volume());
            break;
        case Key::VolumeDown:
            client.set_volume(std::max(0.0f, client.get_volume() - 0.05f));
            status_message = "Volume: " + format_volume(client.get_volume());
            break;
        case Key::Search:
            if (event.key == Key::Search) {
                current_view = ViewMode::Library;
                search_active = false;
                if (music_library_id < 0) {
                    music_library_id = client.get_music_library_id();
                    if (music_library_id > 0) {
                        load_library_data();
                    }
                }
            }
            break;
        case Key::Help:
            status_message = "Help: / = search, L = library, o = options, q = quit, ↑↓ = scroll lyrics";
            break;
        case Key::Char:
            if (event.character == 'l' || event.character == 'L') {
                current_view = ViewMode::Library;
                search_active = false;
                if (music_library_id < 0) {
                    music_library_id = client.get_music_library_id();
                    if (music_library_id > 0) {
                        load_library_data();
                    }
                }
            } else if (event.character == 'o' || event.character == 'O') {
                // Open options menu
                options_menu_active = true;
                options_menu_category = 0;
                options_menu_selected = 0;
                options_menu_editing = false;
                options_menu_edit_buffer.clear();
                return;
            }
            break;
        default:
            break;
    }
}

void PlayerView::handle_playback_key(Key key) {
    switch (key) {
        case Key::Play:
            if (playback_state.playing) {
                client.pause();
                status_message = "Paused";
            } else {
                // Resume if we have a track, otherwise open track selection
                if (playback_state.current_track.title.empty()) {
                    // No track - open library view
                    current_view = ViewMode::Library;
                    if (music_library_id < 0) {
                        music_library_id = client.get_music_library_id();
                        if (music_library_id > 0) {
                            load_library_data();
                        }
                    }
                    status_message = "Select a track to play";
                } else {
                    client.resume();
                    status_message = "Playing";
                }
            }
            break;
        case Key::Pause:
            if (playback_state.playing) {
                client.pause();
                status_message = "Paused";
            }
            break;
        case Key::Stop:
            client.stop();
            waveform->clear();
            status_message = "Stopped";
            break;
        case Key::Next:
            // PLACEHOLDER: Play next track in queue
            status_message = "Next track (not yet implemented)";
            break;
        case Key::Previous:
            // PLACEHOLDER: Play previous track
            status_message = "Previous track (not yet implemented)";
            break;
        default:
            break;
    }
}

void PlayerView::handle_navigation_key(Key key) {
    (void)key;
    // PLACEHOLDER: Handle navigation between views
}

void PlayerView::handle_mouse_event(const MouseEvent& event) {
    // Only handle left clicks
    if (event.type != MouseEvent::Type::Press || 
        event.button != MouseEvent::Button::Left) {
        return;
    }
    
    int x = event.x;
    int y = event.y;
    int sidebar_w = 30;
    
    // Check if click is in sidebar - works in all views
    if (x < sidebar_w) {
        // Check if clicked on navigation menu items (Player, Library, Search)
        if (y == 2) {
            // Player
            current_view = ViewMode::Player;
            search_active = false;  // Deactivate search when switching to Player
            return;
        } else if (y == 3) {
            // Library
            current_view = ViewMode::Library;
            search_active = false;  // Deactivate search when switching to Library
            if (music_library_id < 0) {
                music_library_id = client.get_music_library_id();
                if (music_library_id > 0) {
                    load_library_data();
                }
            }
            return;
        } else if (y == 4) {
            // Search
            current_view = ViewMode::Search;
            search_active = true;
            return;
        }
        
        // Check if click is on a playlist (only in Player and Library views)
        if (current_view == ViewMode::Player || current_view == ViewMode::Library) {
            int playlist_start_y = 8;  // After navigation menu
            int clicked_playlist_idx = y - playlist_start_y;
            
            // Account for scrolling
            int actual_idx = clicked_playlist_idx + playlist_scroll_offset;
            
            if (clicked_playlist_idx >= 0 && 
                actual_idx >= 0 &&
                actual_idx < static_cast<int>(playlists.size())) {
                // Clicked on a playlist - select it and load tracks
                selected_index = actual_idx;
                current_view = ViewMode::Library;
                browse_mode = BrowseMode::Playlists;  // Set to Playlists mode first
                scroll_offset = 0;  // Reset scroll
                select_item();  // This will load the tracks and switch to Tracks mode
                return;
            }
        }
        return;
    }
    
    // Check if click is on tab buttons (Artists, Albums, Playlists, Tracks) at y=3
    if ((current_view == ViewMode::Library || current_view == ViewMode::Search) && y == 3) {
        int menu_x = sidebar_w + 2;
        if (x >= menu_x && x < menu_x + 8) {
            // Clicked on "Artists"
            browse_mode = BrowseMode::Artists;
            selected_index = 0;
            scroll_offset = 0;
            browse_tracks.clear();
            current_album_id.clear();
            album_art_for_tracks.reset();
            return;
        } else if (x >= menu_x + 10 && x < menu_x + 18) {
            // Clicked on "Albums"
            browse_mode = BrowseMode::Albums;
            selected_index = 0;
            scroll_offset = 0;
            browse_tracks.clear();
            current_album_id.clear();
            album_art_for_tracks.reset();
            return;
        } else if (x >= menu_x + 20 && x < menu_x + 30) {
            // Clicked on "Playlists"
            browse_mode = BrowseMode::Playlists;
            selected_index = 0;
            scroll_offset = 0;
            browse_tracks.clear();
            current_album_id.clear();
            album_art_for_tracks.reset();
            // Ensure playlists are loaded
            if (playlists.empty() && music_library_id > 0) {
                try {
                    playlists = client.get_playlists(100);
                } catch (...) {
                    // Failed to load playlists
                }
            }
            return;
        } else if (x >= menu_x + 32 && x < menu_x + 40) {
            // Clicked on "Tracks"
            browse_mode = BrowseMode::Tracks;
            selected_index = 0;
            scroll_offset = 0;
            return;
        }
    }
    
    // Check if click is in library view (main area)
    if (current_view == ViewMode::Library || current_view == ViewMode::Search) {
        if (x >= sidebar_w + 2 && y > 4) {  // Below tab menu (y > 4 to avoid clicking tabs)
            // Clicked in main content area
            // Calculate actual start_y based on current browse mode and album/artist art
            // This must match the start_y calculation in the respective draw_*_list functions
            int list_start_y = 6;  // Default start
            
            if (browse_mode == BrowseMode::Albums) {
                // Albums view: start_y depends on album art position (matches draw_albums_list)
                // When album art is disabled or not available, use default start_y = 6
                list_start_y = 6;  // Default - matches draw_albums_list when art is disabled
                if (config.enable_album_art && selected_index >= 0 && selected_index < static_cast<int>(albums.size())) {
                    // Note: Album art fetching is temporarily disabled - when re-enabled, uncomment below:
                    // const auto& selected_album = albums[selected_index];
                    // Check if album art is actually available (not just enabled in config)
                    // Since album art fetching is currently disabled, has_album_art will be false
                    // So we'll always use the default list_start_y = 6
                    // When art is re-enabled, uncomment below:
                    /*
                    if (!selected_album.art_url.empty() && album_art_for_albums && album_art_for_albums->has_art()) {
                        int album_art_h = 25;
                        int album_art_y = 2;
                        int art_bottom = album_art_y + album_art_h;
                        if (config.enable_album_data) {
                            art_bottom += 3;  // Add space for album info
                        }
                        list_start_y = std::max(6, art_bottom + 1);
                    }
                    */
                }
            } else if (browse_mode == BrowseMode::Artists) {
                // Artists view: start_y depends on artist art position (matches draw_artists_list)
                if (config.enable_album_art && selected_index >= 0 && selected_index < static_cast<int>(artists.size())) {
                    const auto& selected_artist = artists[selected_index];
                    if (!selected_artist.art_url.empty()) {
                        int artist_art_h = 25;
                        int artist_art_y = 2;
                        int art_bottom = artist_art_y + artist_art_h;
                        art_bottom += 1;  // Add space for artist name
                        list_start_y = std::max(6, art_bottom + 1);
                    }
                }
            } else if (browse_mode == BrowseMode::Tracks) {
                // Tracks view: always start at y=6 (matches draw_tracks_list)
                // Tracks now start right after search bar and tabs, regardless of album art
                list_start_y = 6;
            }
            
            int clicked_idx = y - list_start_y + scroll_offset;
            
            if (clicked_idx >= 0) {
                if (browse_mode == BrowseMode::Artists && 
                    clicked_idx < static_cast<int>(artists.size())) {
                    selected_index = clicked_idx;
                    select_item();
                } else if (browse_mode == BrowseMode::Albums && 
                          clicked_idx >= 0 && clicked_idx < static_cast<int>(albums.size())) {
                    // Validate clicked index is still valid
                    if (clicked_idx < static_cast<int>(albums.size())) {
                        // Clear album art state when switching albums to prevent stale data
                        if (selected_index != clicked_idx && album_art_for_albums) {
                            album_art_for_albums->clear();
                        }
                        selected_index = clicked_idx;
                        // Only call select_item if we have a valid album
                        if (selected_index >= 0 && selected_index < static_cast<int>(albums.size())) {
                            select_item();
                        }
                    }
                } else if (browse_mode == BrowseMode::Playlists && 
                          clicked_idx < static_cast<int>(playlists.size())) {
                    selected_index = clicked_idx;
                    select_item();
                } else if (browse_mode == BrowseMode::Tracks && 
                          clicked_idx < static_cast<int>(browse_tracks.size())) {
                    selected_index = clicked_idx;
                    select_item();  // This will play the track
                }
            }
        }
    }
}

std::string PlayerView::format_time(uint32_t milliseconds) {
    uint32_t seconds = milliseconds / 1000;
    uint32_t minutes = seconds / 60;
    seconds = seconds % 60;
    
    std::stringstream ss;
    ss << std::setfill('0') << std::setw(2) << minutes << ":"
       << std::setfill('0') << std::setw(2) << seconds;
    return ss.str();
}

std::string PlayerView::format_volume(float volume) {
    int percent = static_cast<int>(volume * 100);
    return std::to_string(percent) + "%";
}

void PlayerView::load_library_data() {
    if (music_library_id < 0) return;
    if (!client.is_connected()) return;
    
    try {
        artists = client.get_artists(music_library_id, 100);
        albums = client.get_albums(music_library_id, "", 100);
        playlists = client.get_playlists(50);
    } catch (...) {
        // Library loading failed - continue without it
        artists.clear();
        albums.clear();
        playlists.clear();
    }
}

void PlayerView::perform_search() {
    if (search_query.empty()) {
        browse_tracks.clear();
        // Clear pagination state when clearing search
        current_playlist_id.clear();
        playlist_total_size = 0;
        playlist_loaded_count = 0;
        is_search_mode = false;
        current_search_query.clear();
        search_loaded_count = 0;
        return;
    }
    
    // Minimum 2 characters before searching (reduces API calls)
    if (search_query.length() < 2) {
        browse_tracks.clear();
        // Clear pagination state when clearing search
        current_playlist_id.clear();
        playlist_total_size = 0;
        playlist_loaded_count = 0;
        is_search_mode = false;
        current_search_query.clear();
        search_loaded_count = 0;
        return;
    }
    
    // Check if this is a new search query (different from current)
    bool is_new_search = (current_search_query != search_query);
    
    if (is_new_search) {
        // New search - reset everything
        browse_tracks.clear();
        current_playlist_id.clear();
        playlist_total_size = 0;
        playlist_loaded_count = 0;
        search_loaded_count = 0;
        current_search_query = search_query;
        if (config.enable_debug_logging) {
            std::cerr << "[LOG] New search: \"" << search_query << "\"" << std::endl;
        }
    } else {
        // Same search query - don't search again if we already have results
        // Only search again when user scrolls to load more (handled elsewhere)
        if (search_loaded_count > 0 && !browse_tracks.empty()) {
            // Already have results for this query, don't search again
            if (config.enable_debug_logging) {
                std::cerr << "[LOG] Search skipped: already have " << browse_tracks.size() << " results for \"" << search_query << "\"" << std::endl;
            }
            return;
        }
        if (config.enable_debug_logging) {
            std::cerr << "[LOG] Continuing search: \"" << search_query << "\" (loaded: " << search_loaded_count << ")" << std::endl;
        }
    }
    
    // Perform server-side search via Plex API - load first chunk only
    std::vector<Track> search_results = client.search_tracks(search_query, SEARCH_CHUNK_SIZE, search_loaded_count);
    if (config.enable_debug_logging) {
        std::cerr << "[LOG] Search API returned " << search_results.size() << " results for \"" << search_query << "\"" << std::endl;
    }
    
    // Always deduplicate search results (Plex API may return duplicates)
    // Use both ID-based and content-based (title+artist+album) deduplication
    // Normalize signatures to handle case/whitespace differences
    auto normalize_signature = [](const std::string& title, const std::string& artist, const std::string& album) -> std::string {
        // Convert to lowercase and trim whitespace for comparison
        std::string sig = title + "|" + artist + "|" + album;
        std::transform(sig.begin(), sig.end(), sig.begin(), ::tolower);
        // Remove extra whitespace
        sig.erase(std::remove_if(sig.begin(), sig.end(), [](char c) { return std::isspace(c); }), sig.end());
        return sig;
    };
    
    std::set<std::string> seen_ids;
    std::set<std::string> seen_signatures;  // normalized title|artist|album for fallback deduplication
    std::vector<Track> deduplicated_results;
    
    int duplicates_found = 0;
    int duplicates_by_id = 0;
    int duplicates_by_signature = 0;
    
    for (const auto& track : search_results) {
        bool is_duplicate = false;
        
        // First check by ID (most reliable)
        if (!track.id.empty()) {
            if (seen_ids.find(track.id) != seen_ids.end()) {
                is_duplicate = true;
                duplicates_found++;
                duplicates_by_id++;
            } else {
                seen_ids.insert(track.id);
            }
        }
        
        // Fallback: check by normalized content signature (title|artist|album)
        if (!is_duplicate) {
            std::string signature = normalize_signature(track.title, track.artist, track.album);
            if (seen_signatures.find(signature) != seen_signatures.end()) {
                is_duplicate = true;
                duplicates_found++;
                duplicates_by_signature++;
            } else {
                seen_signatures.insert(signature);
            }
        }
        
        if (!is_duplicate) {
            deduplicated_results.push_back(track);
        }
    }
    
    // Log detailed deduplication info (only if debug logging enabled)
    if (config.enable_debug_logging && duplicates_found > 0) {
        std::cerr << "[LOG] Deduplication details: " << duplicates_by_id << " by ID, " << duplicates_by_signature << " by signature" << std::endl;
    }
    
    // Log deduplication results (only if debug logging enabled)
    if (config.enable_debug_logging) {
        if (duplicates_found > 0) {
            std::cerr << "[LOG] Search deduplication: Found " << duplicates_found << " duplicates out of " << search_results.size() << " results" << std::endl;
        } else if (search_results.size() > 0) {
            std::cerr << "[LOG] Search deduplication: " << search_results.size() << " results, no duplicates found" << std::endl;
        }
    }
    
    if (is_new_search) {
        // New search - replace results
        browse_tracks = deduplicated_results;
        search_loaded_count = static_cast<int>(deduplicated_results.size());
        if (config.enable_debug_logging) {
            std::cerr << "[LOG] New search complete: " << search_results.size() << " API results -> " 
                      << duplicates_found << " duplicates removed -> " << deduplicated_results.size() 
                      << " unique tracks (browse_tracks.size()=" << browse_tracks.size() << ")" << std::endl;
        }
    } else {
        // Continuing search - append results with additional deduplication against existing
        // Build sets of existing track IDs and signatures for fast lookup
        std::set<std::string> existing_ids;
        std::set<std::string> existing_signatures;
        for (const auto& track : browse_tracks) {
            if (!track.id.empty()) {
                existing_ids.insert(track.id);
            }
            std::string sig = normalize_signature(track.title, track.artist, track.album);
            existing_signatures.insert(sig);
        }
        
        if (config.enable_debug_logging) {
            std::cerr << "[LOG] Before pagination: browse_tracks.size()=" << browse_tracks.size() 
                      << ", existing_ids.size()=" << existing_ids.size() 
                      << ", existing_signatures.size()=" << existing_signatures.size() << std::endl;
        }
        
        // Only add tracks that aren't already in the list (check both ID and signature)
        int added_count = 0;
        int skipped_duplicates = 0;
        for (const auto& track : deduplicated_results) {
            bool exists = false;
            
            // Check by ID first (most reliable)
            if (!track.id.empty() && existing_ids.find(track.id) != existing_ids.end()) {
                exists = true;
                skipped_duplicates++;
            }
            
            // Check by normalized signature (fallback for tracks without IDs)
            if (!exists) {
                std::string sig = normalize_signature(track.title, track.artist, track.album);
                if (existing_signatures.find(sig) != existing_signatures.end()) {
                    exists = true;
                    skipped_duplicates++;
                }
            }
            
            if (!exists) {
                browse_tracks.push_back(track);
                if (!track.id.empty()) {
                    existing_ids.insert(track.id);
                }
                std::string sig = normalize_signature(track.title, track.artist, track.album);
                existing_signatures.insert(sig);
                added_count++;
            }
        }
        search_loaded_count += added_count;
        // Log pagination deduplication results (only if debug logging enabled)
        if (config.enable_debug_logging) {
            std::cerr << "[LOG] Search pagination: " << search_results.size() << " API results -> " 
                      << duplicates_found << " duplicates in batch -> " << deduplicated_results.size() 
                      << " deduplicated -> " << skipped_duplicates << " existing skipped -> " 
                      << added_count << " new tracks added (total: " << browse_tracks.size() << ")" << std::endl;
        }
    }
    
    is_search_mode = true;
    browse_mode = BrowseMode::Tracks;
    selected_index = 0;
    scroll_offset = 0;
}

void PlayerView::start_play_with_lyrics(const Track& track) {
    prefetch_next_track_id.clear();
    if (track.media_url.empty() || track.id.empty()) {
        status_message = "Error: Track has no media URL or ID";
        return;
    }
    if (!config.enable_lyrics) {
        if (client.play_track(track)) {
            status_message = "Playing: " + track.title + " - " + track.artist;
        } else {
            status_message = "Failed to start playback: " + track.title;
        }
        return;
    }
    // Lyrics on: fetch first (hint + up to ~1.5s), then play
    std::string immediate = client.get_lyrics(track);
    if (!immediate.empty()) {
        current_lyrics = immediate;
        lyrics_lines.clear();
        synced_lyrics.clear();
        lyrics_scroll_position = 0;
        last_lyrics_track_id = track.id;
        if (client.play_track(track)) {
            status_message = "Playing: " + track.title + " - " + track.artist;
        } else {
            status_message = "Failed to start playback: " + track.title;
        }
        return;
    }
    pending_play_track = track;
    pending_play_since = std::chrono::steady_clock::now();
    pending_play = true;
    current_lyrics.clear();
    lyrics_lines.clear();
    synced_lyrics.clear();
    lyrics_scroll_position = 0;
    last_lyrics_track_id = track.id;
    status_message = "Fetching lyrics… " + track.title;
}

void PlayerView::advance_to_next_track() {
    if (browse_mode != BrowseMode::Tracks || browse_tracks.empty()) return;
    int current_idx = -1;
    for (size_t i = 0; i < browse_tracks.size(); ++i) {
        if (browse_tracks[i].id == playback_state.current_track.id) {
            current_idx = static_cast<int>(i);
            break;
        }
    }
    if (current_idx >= 0 && current_idx + 1 < static_cast<int>(browse_tracks.size())) {
        selected_index = current_idx + 1;
        Track next_track = browse_tracks[selected_index];
        start_play_with_lyrics(next_track);
    } else {
        client.stop();
        status_message = "Playlist finished";
    }
}

void PlayerView::select_item() {
    if (browse_mode == BrowseMode::Artists && selected_index >= 0 && selected_index < static_cast<int>(artists.size())) {
        // Load albums for selected artist
        try {
            albums = client.get_albums(music_library_id, artists[selected_index].id, 100);
            browse_mode = BrowseMode::Albums;
            selected_index = 0;
            scroll_offset = 0;
            status_message = "Loaded " + std::to_string(albums.size()) + " albums";
        } catch (...) {
            status_message = "Failed to load albums";
        }
    } else if (browse_mode == BrowseMode::Albums && selected_index >= 0 && selected_index < static_cast<int>(albums.size())) {
        // Load tracks for selected album (real data from Plex)
        try {
            // Store album info for displaying album art and info
            // Double-check bounds before accessing - validate albums vector first
            if (albums.empty() || selected_index < 0 || selected_index >= static_cast<int>(albums.size())) {
                status_message = "Error: Invalid album selection";
                return;
            }
            
            // Store album data locally to avoid multiple vector accesses
            const auto selected_album = albums[selected_index];
            std::string album_id_copy = selected_album.id;  // Make a copy before any operations
            
            // Validate album ID before making API call
            if (album_id_copy.empty()) {
                status_message = "Error: Album has no ID";
                return;
            }
            
            // Clear album art for albums view before switching to tracks view
            if (album_art_for_albums) {
                try {
                    album_art_for_albums->clear();
                } catch (...) {
                    // Ignore clear errors
                }
            }
            
            // Load tracks into a temporary vector first to avoid partial state
            std::vector<Track> new_tracks;
            try {
                new_tracks = client.get_album_tracks(album_id_copy);
            } catch (const std::exception& e) {
                status_message = "Failed to load tracks: " + std::string(e.what());
                return;
            } catch (...) {
                status_message = "Failed to load tracks (unknown error)";
                return;
            }
            
            // Only update state if track loading succeeded
            // Update all state atomically to prevent partial state during drawing
            browse_tracks = std::move(new_tracks);  // Move instead of copy
            album_art_for_tracks.reset();  // Clear old art to force reload
            
            // Set album info AFTER tracks are loaded
            current_album = selected_album;
            current_album_id = album_id_copy;
            
            // Clear search/playlist pagination when loading album
            is_search_mode = false;
            current_search_query.clear();
            search_loaded_count = 0;
            current_playlist_id.clear();
            playlist_total_size = 0;
            playlist_loaded_count = 0;
            
            // Switch mode LAST to ensure all state is ready
            browse_mode = BrowseMode::Tracks;
            selected_index = 0;
            scroll_offset = 0;
            status_message = "Loaded " + std::to_string(browse_tracks.size()) + " tracks";
        } catch (const std::exception& e) {
            status_message = "Failed to load tracks: " + std::string(e.what());
            current_album_id.clear();
            current_album = PlexClient::Album();
            browse_tracks.clear();
        } catch (...) {
            status_message = "Failed to load tracks (unknown error)";
            current_album_id.clear();
            current_album = PlexClient::Album();
            browse_tracks.clear();
        }
    } else if (browse_mode == BrowseMode::Playlists && selected_index >= 0 && selected_index < static_cast<int>(playlists.size())) {
        // Load tracks for selected playlist (real data from Plex) - with pagination
        try {
            current_playlist_id = playlists[selected_index].id;
            playlist_loaded_count = 0;
            playlist_total_size = playlists[selected_index].count;  // Use playlist count if available
            
            // Load first chunk only (lazy loading)
            browse_tracks = client.get_playlist_tracks(current_playlist_id, 0, PLAYLIST_CHUNK_SIZE);
            playlist_loaded_count = static_cast<int>(browse_tracks.size());
            
            browse_mode = BrowseMode::Tracks;  // Switch to tracks view
            selected_index = 0;
            scroll_offset = 0;
            // Clear search pagination when loading playlist
            is_search_mode = false;
            current_search_query.clear();
            search_loaded_count = 0;
            // Keep track of which playlist we're viewing (for going back)
            if (playlist_total_size > 0) {
                status_message = "Loaded " + std::to_string(playlist_loaded_count) + " of " + 
                                std::to_string(playlist_total_size) + " tracks (scroll to load more)";
            } else {
                status_message = "Loaded " + std::to_string(playlist_loaded_count) + " tracks from playlist";
            }
        } catch (...) {
            status_message = "Failed to load playlist tracks";
            // On error, go back to Playlists view so user can try another
            browse_mode = BrowseMode::Playlists;
            current_playlist_id.clear();
        }
    } else if (browse_mode == BrowseMode::Tracks && selected_index >= 0 && selected_index < static_cast<int>(browse_tracks.size())) {
        const Track& track = browse_tracks[selected_index];
        current_view = ViewMode::Player;
        try {
            start_play_with_lyrics(track);
        } catch (const std::exception& e) {
            status_message = "Error: " + std::string(e.what());
            current_view = ViewMode::Library;
        } catch (...) {
            status_message = "Error: Unknown exception during playback";
            current_view = ViewMode::Library;
        }
    }
}

void PlayerView::draw_library_view(const Layout& layout) {
    int sidebar_w = 30;
    int w = term.width();
    int h = term.height();
    if (w <= 0) w = 80;  // Safety check
    if (h <= 0) h = 24;  // Safety check
    
    // Clear main content area to remove fragments (btop-style: full clear on view change)
    std::string black_bg = term.bg_color(0, 0, 0);
    int main_w = w - sidebar_w;
    if (main_w > 0 && main_w <= 1000) {
        // Fill entire main content area with black to remove all fragments
        for (int y = 0; y < h - 1 && y < 1000; ++y) {  // Don't overwrite status bar
            std::string line(main_w, ' ');
            term.draw_text(sidebar_w, y, black_bg + line + term.reset_color());
        }
    }
    
    // Explicitly clear album art area before drawing (remove any fragments)
    if (layout.album_art_w > 0 && layout.album_art_h > 0) {
        // Clear Player view album art position (top-left)
        if (layout.album_art_x >= 0 && layout.album_art_y >= 0 &&
            layout.album_art_x + layout.album_art_w <= w &&
            layout.album_art_y + layout.album_art_h <= h) {
            for (int y = 0; y < layout.album_art_h; ++y) {
                std::string line(layout.album_art_w, ' ');
                term.draw_text(layout.album_art_x, layout.album_art_y + y, 
                              black_bg + line + term.reset_color());
            }
        }
        // Clear Library view album art position (top-right)
        int lib_art_x = w - layout.album_art_w - 2;
        int lib_art_y = 2;
        if (lib_art_x >= sidebar_w && lib_art_x + layout.album_art_w <= w &&
            lib_art_y >= 0 && lib_art_y + layout.album_art_h <= h) {
            for (int y = 0; y < layout.album_art_h; ++y) {
                std::string line(layout.album_art_w, ' ');
                term.draw_text(lib_art_x, lib_art_y + y, 
                              black_bg + line + term.reset_color());
            }
        }
    }
    
    // Draw album art in top-right corner (if available)
    draw_album_art(layout);
    
    // Draw colorful search bar at top
    draw_search_bar(layout);
    
    // Draw vibrant mode selector (Spotify-style menu)
    int y = 3;
    int menu_x = sidebar_w + 2;
    
    // Bright mode buttons (white when active, dim when not)
    std::string bright = term.fg_color(255, 255, 255);
    std::string dim = term.fg_color(150, 150, 150);
    std::string menu_bg = term.bg_color(0, 0, 0);
    
    std::string artists_color = (browse_mode == BrowseMode::Artists) ? bright : dim;
    std::string albums_color = (browse_mode == BrowseMode::Albums) ? bright : dim;
    std::string playlists_color = (browse_mode == BrowseMode::Playlists) ? bright : dim;
    std::string tracks_color = (browse_mode == BrowseMode::Tracks) ? bright : dim;
    
    term.draw_text(menu_x, y, menu_bg + artists_color + "Artists" + term.reset_color());
    term.draw_text(menu_x + 10, y, menu_bg + albums_color + "Albums" + term.reset_color());
    term.draw_text(menu_x + 20, y, menu_bg + playlists_color + "Playlists" + term.reset_color());
    term.draw_text(menu_x + 32, y, menu_bg + tracks_color + "Tracks" + term.reset_color());
    
    // Draw separator (orange equals like player view, btop style)
    std::string orange_color = term.fg_color(255, 140, 0);  // Plex orange
    int sep_width = w - sidebar_w - 4;
    if (sep_width > 0 && sep_width <= 1000) {
        std::string separator(sep_width, '=');  // Orange equals separator like player view
        term.draw_text(menu_x, y + 1, menu_bg + orange_color + separator + term.reset_color());
    }
    
    // Draw list based on mode
    if (browse_mode == BrowseMode::Artists) {
        draw_artists_list(layout);
    } else if (browse_mode == BrowseMode::Albums) {
        draw_albums_list(layout);
    } else if (browse_mode == BrowseMode::Playlists) {
        draw_playlists_list(layout);
    } else {
        draw_tracks_list(layout);
    }
}

void PlayerView::draw_search_bar(const Layout& /*layout*/) {
    int sidebar_w = 30;
    int w = term.width();
    if (w <= 0) w = 80;  // Safety check
    
    // Simple search bar - black background like btop
    int search_x = sidebar_w + 2;
    int search_y = 2;
    
    std::string bright_white = term.fg_color(255, 255, 255);
    std::string dim_white = term.fg_color(180, 180, 180);
    std::string black_bg = term.bg_color(0, 0, 0);
    
    // Search prompt and input (black bg, white text)
    std::string search_line = "Search: " + search_query;
    if (search_active) {
        search_line += "_";  // Cursor when active
    }
    
    // Truncate if too long
    int max_search_w = w - search_x - 4;
    if (static_cast<int>(search_line.length()) > max_search_w) {
        search_line = search_line.substr(0, max_search_w - 3) + "...";
    }
    
    term.draw_text(search_x, search_y, black_bg + bright_white + search_line + term.reset_color());
}

void PlayerView::draw_artists_list(const Layout& /*layout*/) {
    int sidebar_w = 30;
    int h = term.height();
    int w = term.width();
    if (h <= 0) h = 24;  // Safety check
    if (w <= 0) w = 80;  // Safety check
    
    int list_x = sidebar_w + 2;
    
    // Show random artist pic in top-right if an artist is selected (if album art enabled)
    int artist_art_w = 0;
    int artist_art_h = 0;
    int artist_art_x = 0;
    int artist_art_y = 2;  // Near top
    bool has_artist_art = false;
    
    if (config.enable_album_art && selected_index >= 0 && selected_index < static_cast<int>(artists.size())) {
        try {
            const auto& selected_artist = artists[selected_index];
            if (!selected_artist.art_url.empty()) {
                // Initialize artist art object if needed
                if (!artist_art) {
                    artist_art = std::make_unique<AlbumArt>();
                }
                
                // Fetch artist art if not already loaded or if artist changed
                static std::string last_artist_id;
                if (last_artist_id != selected_artist.id || !artist_art->has_art()) {
                    try {
                        artist_art->fetch_art(
                            client.get_server_url(),
                            client.get_token(),
                            selected_artist.art_url
                        );
                    } catch (...) {
                        // Fetch failed - continue without art
                    }
                    last_artist_id = selected_artist.id;
                }
            
            // Draw small artist pic in top-right (same size as album art: 50x25)
            artist_art_w = 50;
            artist_art_h = 25;
            artist_art_x = w - artist_art_w - 2;  // Top-right with 2 char margin
            has_artist_art = artist_art->has_art();
            
            if (has_artist_art) {
                auto art_lines = artist_art->render_pixelated(artist_art_w, artist_art_h, config.theme);
                for (size_t y = 0; y < art_lines.size() && y < static_cast<size_t>(artist_art_h); ++y) {
                    if (artist_art_y + static_cast<int>(y) < h && artist_art_x >= 0 && artist_art_x < w) {
                        term.draw_text(artist_art_x, artist_art_y + static_cast<int>(y), art_lines[y]);
                    }
                }
            }
            
                // Draw artist name below pic
                int info_y = artist_art_y + artist_art_h + 1;
                std::string black_bg = term.bg_color(0, 0, 0);
                std::string name_color = term.fg_color(255, 255, 255);
                std::string name = selected_artist.name;
                if (name.length() > static_cast<size_t>(artist_art_w)) {
                    name = name.substr(0, artist_art_w - 3) + "...";
                }
                if (artist_art_x >= 0 && artist_art_x < w && info_y >= 0 && info_y < h) {
                    term.draw_text(artist_art_x, info_y, black_bg + name_color + name + term.reset_color());
                }
            }
        } catch (...) {
            // Ignore errors - don't crash if artist art fails
        }
    }
    
    // Adjust start_y to avoid overlapping with artist art
    int art_bottom = artist_art_y + artist_art_h;
    if (has_artist_art) {
        art_bottom += 1;  // Add space for artist name
    }
    int start_y = std::max(6, art_bottom + 1);  // Start artists list below art, but at least at y=6
    
    int max_items = std::max(1, h - start_y - 3);
    // Use scroll_offset for proper scrolling
    int visible_start = scroll_offset;
    
    // Show message if no artists
    if (artists.empty()) {
        std::string msg_color = term.fg_color(config.theme.dimmed.r, config.theme.dimmed.g, config.theme.dimmed.b);
        term.draw_text(list_x, start_y, msg_color + "No artists found. Loading..." + term.reset_color());
        return;
    }
    
    // Clear each line before drawing to remove fragments (btop-style)
    // Only clear the area where artists will be drawn (not artist art area)
    int clear_width = w - list_x;
    if (has_artist_art && artist_art_x < w) {
        // Don't clear over artist art - limit clear width to before artist art
        clear_width = std::min(clear_width, artist_art_x - list_x - 2);
    }
    if (clear_width > 0 && clear_width <= 1000) {
        std::string black_bg = term.bg_color(0, 0, 0);
        std::string clear_line(clear_width, ' ');
        for (int i = 0; i < max_items; ++i) {
            term.draw_text(list_x, start_y + i, black_bg + clear_line + term.reset_color());
        }
    }
    
    for (int i = 0; i < max_items && (visible_start + i) < static_cast<int>(artists.size()); ++i) {
        int idx = visible_start + i;
        bool selected = (idx == selected_index);
        
        // Bright text - white when selected, dim when not
        std::string color = selected ? 
            term.fg_color(255, 255, 255) : 
            term.fg_color(200, 200, 200);
        
        std::string marker = selected ? "> " : "  ";
        std::string name = artists[idx].name;
        if (name.length() > 50) name = name.substr(0, 47) + "...";
        std::string black_bg = term.bg_color(0, 0, 0);
        term.draw_text(list_x, start_y + i, black_bg + color + marker + name + term.reset_color());
    }
}

void PlayerView::draw_albums_list(const Layout& /*layout*/) {
    int sidebar_w = 30;
    int h = term.height();
    int w = term.width();
    if (h <= 0) h = 24;  // Safety check
    if (w <= 0) w = 80;  // Safety check
    
    int list_x = sidebar_w + 2;
    
    // Show album art in top-right if an album is selected (if album art enabled)
    // Calculate art dimensions first to adjust track start position
    // Note: Album art fetching is temporarily disabled - variables kept for when re-enabled
    int album_art_w = 0;
    int album_art_h = 0;
    int album_art_x = 0;
    int album_art_y = 2;  // Near top
    bool has_album_art = false;
    
    // Validate albums vector and selected_index before any access
    // Make a local copy of size to avoid race conditions
    size_t albums_size = albums.size();
    if (albums_size == 0 || selected_index < 0 || selected_index >= static_cast<int>(albums_size)) {
        // Invalid state - skip album art drawing
    } else if (config.enable_album_art && selected_index < static_cast<int>(albums.size())) {
        // TEMPORARILY DISABLED: Album art fetching in albums view to debug crash
        // TODO: Re-enable once crash is fixed
        // When re-enabled, uncomment the code below and use album_art_w, album_art_h, etc.
        (void)album_art_w;  // Suppress unused variable warning until re-enabled
        (void)album_art_h;
        (void)album_art_x;
        (void)album_art_y;
        /*
        try {
            // Re-validate bounds before accessing (albums vector might have changed)
            if (selected_index < 0 || selected_index >= static_cast<int>(albums.size())) {
                // Bounds invalid - skip
            } else {
                // Store album data locally to avoid multiple vector accesses
                const auto selected_album = albums[selected_index];
                std::string current_album_id_str = selected_album.id;
                
                if (!selected_album.art_url.empty() && !current_album_id_str.empty()) {
                    // Initialize album art object if needed
                    if (!album_art_for_albums) {
                        album_art_for_albums = std::make_unique<AlbumArt>();
                    }
                    
                    // Fetch album art if not already loaded or if album changed
                    static std::string last_album_id;
                    static bool is_fetching = false;  // Prevent concurrent fetches
                    
                    // If album changed, clear old art first to prevent stale data
                    if (last_album_id != current_album_id_str && !last_album_id.empty()) {
                        if (album_art_for_albums && !is_fetching) {
                            try {
                                album_art_for_albums->clear();
                            } catch (...) {
                                // Clear failed - ignore
                            }
                        }
                        is_fetching = false;  // Reset flag on album change
                    }
                    
                    // Only fetch if not currently fetching and album changed or no art
                    if (!is_fetching && album_art_for_albums && 
                        (last_album_id != current_album_id_str || !album_art_for_albums->has_art())) {
                        try {
                            // Only fetch if we have a valid art URL
                            if (!selected_album.art_url.empty()) {
                                is_fetching = true;  // Mark as fetching
                                bool fetch_success = false;
                                try {
                                    fetch_success = album_art_for_albums->fetch_art(
                                        client.get_server_url(),
                                        client.get_token(),
                                        selected_album.art_url
                                    );
                                } catch (const std::exception& e) {
                                    // Fetch exception - clear
                                    fetch_success = false;
                                    if (album_art_for_albums) {
                                        try {
                                            album_art_for_albums->clear();
                                        } catch (...) {
                                            // Ignore clear errors
                                        }
                                    }
                                } catch (...) {
                                    // Unknown exception
                                    fetch_success = false;
                                    if (album_art_for_albums) {
                                        try {
                                            album_art_for_albums->clear();
                                        } catch (...) {
                                            // Ignore
                                        }
                                    }
                                }
                                
                                if (fetch_success) {
                                    last_album_id = current_album_id_str;
                                } else {
                                    // Fetch failed - clear
                                    if (album_art_for_albums) {
                                        try {
                                            album_art_for_albums->clear();
                                        } catch (...) {
                                            // Ignore
                                        }
                                    }
                                }
                                is_fetching = false;  // Mark fetch complete
                            }
                        } catch (const std::exception& e) {
                            // Fetch failed - clear and continue without art
                            is_fetching = false;  // Reset flag on error
                            if (album_art_for_albums) {
                                try {
                                    album_art_for_albums->clear();
                                } catch (...) {
                                    // Clear also failed - ignore
                                }
                            }
                        } catch (...) {
                            // Unknown error - clear and continue
                            is_fetching = false;
                            if (album_art_for_albums) {
                                try {
                                    album_art_for_albums->clear();
                                } catch (...) {
                                    // Ignore
                                }
                            }
                        }
                    }
                
                    // Draw album art in top-right (album view = larger than player)
                    album_art_w = 60;
                    album_art_h = 30;
                    album_art_x = w - album_art_w - 2;  // Top-right with 2 char margin
                    // Only check has_art if album_art_for_albums is valid
                    if (album_art_for_albums) {
                        try {
                            has_album_art = album_art_for_albums->has_art();
                            
                            if (has_album_art) {
                                try {
                                    auto art_lines = album_art_for_albums->render_pixelated(album_art_w, album_art_h, config.theme);
                                    if (!art_lines.empty()) {
                                        for (size_t y = 0; y < art_lines.size() && y < static_cast<size_t>(album_art_h); ++y) {
                                            if (album_art_y + static_cast<int>(y) < h && album_art_x >= 0 && album_art_x < w) {
                                                term.draw_text(album_art_x, album_art_y + static_cast<int>(y), art_lines[y]);
                                            }
                                        }
                                    }
                                } catch (const std::exception& e) {
                                    // Rendering failed - clear art and continue
                                    if (album_art_for_albums) {
                                        try {
                                            album_art_for_albums->clear();
                                        } catch (...) {
                                            // Ignore
                                        }
                                    }
                                    has_album_art = false;
                                } catch (...) {
                                    // Unknown rendering error
                                    if (album_art_for_albums) {
                                        try {
                                            album_art_for_albums->clear();
                                        } catch (...) {
                                            // Ignore
                                        }
                                    }
                                    has_album_art = false;
                                }
                            }
                        } catch (...) {
                            // has_art() call failed - skip rendering
                            has_album_art = false;
                        }
                    }
                
                // Draw album info below art (if album data enabled)
                if (config.enable_album_data && has_album_art) {
                    int info_y = album_art_y + album_art_h + 1;
                    std::string black_bg = term.bg_color(0, 0, 0);
                    std::string title_color = term.fg_color(255, 255, 255);
                    std::string artist_color = term.fg_color(200, 200, 200);
                    std::string year_color = term.fg_color(150, 150, 150);
                    
                    if (album_art_x >= 0 && album_art_x < w && info_y >= 0 && info_y < h) {
                        // Draw album title
                        std::string title = selected_album.title;
                        if (title.length() > static_cast<size_t>(album_art_w)) {
                            title = title.substr(0, album_art_w - 3) + "...";
                        }
                        term.draw_text(album_art_x, info_y, black_bg + title_color + title + term.reset_color());
                        
                        // Draw artist
                        if (!selected_album.artist.empty()) {
                            std::string artist = selected_album.artist;
                            if (artist.length() > static_cast<size_t>(album_art_w)) {
                                artist = artist.substr(0, album_art_w - 3) + "...";
                            }
                            term.draw_text(album_art_x, info_y + 1, black_bg + artist_color + artist + term.reset_color());
                        }
                        
                        // Draw year
                        if (selected_album.year > 0) {
                            term.draw_text(album_art_x, info_y + 2, 
                                          black_bg + year_color + "(" + std::to_string(selected_album.year) + ")" + term.reset_color());
                        }
                    }
                }
                }
            }
        } catch (...) {
            // Ignore errors - don't crash if album art fails
        }
        */
    }
    
    // Adjust start_y to avoid overlapping with album art
    // Album art is at y=2 with height 30 (album view), so it occupies lines 2–31
    // If album data is enabled, add 3 more lines for info
    int start_y = 6;  // Default start position
    if (config.enable_album_art && has_album_art) {
        int art_bottom = album_art_y + album_art_h;
        if (config.enable_album_data) {
            art_bottom += 3;  // Add space for album info
        }
        start_y = std::max(6, art_bottom + 1);  // Start below album art, but at least at y=6
    }
    
    int max_items = std::max(1, h - start_y - 3);
    // Use scroll_offset for proper scrolling
    int visible_start = scroll_offset;
    
    // Show message if no albums
    if (albums.empty()) {
        std::string msg_color = term.fg_color(config.theme.dimmed.r, config.theme.dimmed.g, config.theme.dimmed.b);
        term.draw_text(list_x, start_y, msg_color + "No albums found." + term.reset_color());
        return;
    }
    
    // Clear each line before drawing to remove fragments (btop-style)
    // Only clear the area where tracks will be drawn (not album art area)
    int clear_width = w - list_x;
    if (has_album_art && album_art_x < w) {
        // Don't clear over album art - limit clear width to before album art
        clear_width = std::min(clear_width, album_art_x - list_x - 2);
    }
    if (clear_width > 0 && clear_width <= 1000) {
        std::string black_bg = term.bg_color(0, 0, 0);
        std::string clear_line(clear_width, ' ');
        for (int i = 0; i < max_items; ++i) {
            term.draw_text(list_x, start_y + i, black_bg + clear_line + term.reset_color());
        }
    }
    
    // Validate albums vector before iterating
    if (!albums.empty()) {
        for (int i = 0; i < max_items && (visible_start + i) < static_cast<int>(albums.size()); ++i) {
            int idx = visible_start + i;
            // Double-check bounds before accessing
            if (idx < 0 || idx >= static_cast<int>(albums.size())) {
                continue;  // Skip invalid index
            }
            
            // Store album data locally to avoid multiple vector accesses
            const auto& album = albums[idx];
            bool selected = (idx == selected_index);
            
            // Bright text - white when selected, dim when not
            std::string color = selected ? 
                term.fg_color(255, 255, 255) : 
                term.fg_color(200, 200, 200);
            
            std::string dim_color = term.fg_color(150, 150, 150);
            std::string marker = selected ? "> " : "  ";
            
            std::string title = album.title;
            if (title.length() > 35) title = title.substr(0, 32) + "...";
            
            std::string black_bg = term.bg_color(0, 0, 0);
            std::string line = black_bg + color + marker + title;
            if (!album.artist.empty()) {
                line += black_bg + dim_color + " • " + album.artist + term.reset_color();
            }
            if (album.year > 0) {
                line += black_bg + dim_color + " (" + std::to_string(album.year) + ")" + term.reset_color();
            }
            
            term.draw_text(list_x, start_y + i, line + term.reset_color());
        }
    }
}

void PlayerView::draw_playlists_list(const Layout& /*layout*/) {
    int sidebar_w = 30;
    int h = term.height();
    if (h <= 0) h = 24;  // Safety check
    
    int start_y = 6;
    int max_items = std::max(1, h - start_y - 3);
    // Use scroll_offset for proper scrolling
    int visible_start = scroll_offset;
    int list_x = sidebar_w + 2;
    
    // Show message if no playlists
    if (playlists.empty()) {
        std::string msg_color = term.fg_color(config.theme.dimmed.r, config.theme.dimmed.g, config.theme.dimmed.b);
        term.draw_text(list_x, start_y, msg_color + "No playlists found. Loading..." + term.reset_color());
        return;
    }
    
    // Clear each line before drawing to remove fragments (btop-style)
    int w = term.width();
    int clear_width = w - list_x;
    if (clear_width > 0 && clear_width <= 1000) {
        std::string black_bg = term.bg_color(0, 0, 0);
        std::string clear_line(clear_width, ' ');
        for (int i = 0; i < max_items; ++i) {
            term.draw_text(list_x, start_y + i, black_bg + clear_line + term.reset_color());
        }
    }
    
    for (int i = 0; i < max_items && (visible_start + i) < static_cast<int>(playlists.size()); ++i) {
        int idx = visible_start + i;
        bool selected = (idx == selected_index);
        
        // Bright text - white when selected, dim when not
        std::string color = selected ? 
            term.fg_color(255, 255, 255) : 
            term.fg_color(200, 200, 200);
        
        std::string count_color = term.fg_color(150, 150, 150);
        std::string marker = selected ? "> " : "  ";
        std::string count_str = count_color + " [" + std::to_string(playlists[idx].count) + "]" + term.reset_color();
        
        std::string title = playlists[idx].title;
        if (title.length() > 40) title = title.substr(0, 37) + "...";
        
        std::string black_bg = term.bg_color(0, 0, 0);
        term.draw_text(list_x, start_y + i, black_bg + color + marker + title + " " + count_str + term.reset_color());
    }
}

void PlayerView::draw_tracks_list(const Layout& /*layout*/) {
    int sidebar_w = 30;
    int h = term.height();
    int w = term.width();
    if (h <= 0) h = 24;  // Safety check
    if (w <= 0) w = 80;  // Safety check
    
    // If viewing tracks from an album, show small album art in top-right (cached)
    bool show_album_art = (!current_album_id.empty() && !is_search_mode && current_playlist_id.empty());
    
    int list_x = sidebar_w + 2;
    
    // Calculate album art dimensions first to adjust track start position
    int small_art_w = 0;
    int small_art_h = 0;
    int small_art_x = w;  // Default to right edge if not shown
    int small_art_y = 2;  // Near top
    bool has_track_album_art = false;
    
    if (show_album_art && config.enable_album_art) {
        small_art_w = 50;
        small_art_h = 25;
        small_art_x = w - small_art_w - 2;  // Top-right with 2 char margin
        has_track_album_art = true;
    }
    
    // Start tracks at the top (right after search bar and tabs)
    // Search bar is at y=3, tabs are at y=4, separator at y=5, so start tracks at y=6
    int start_y = 6;  // Start tracks right after the search bar and tabs
    
    int max_items = std::max(1, h - start_y - 3);
    // Use scroll_offset for proper scrolling
    int visible_start = scroll_offset;
    
    // Track list width - account for album art on the right
    int list_max_width = w - list_x - 2;
    if (has_track_album_art && small_art_x < w) {
        // Limit list width to avoid overlapping with album art
        list_max_width = std::min(list_max_width, small_art_x - list_x - 2);
    }
    
    // Lazy loading: Load more tracks if user scrolls near the end
    // For playlists
    if (!current_playlist_id.empty() && 
        (playlist_total_size == 0 || playlist_loaded_count < playlist_total_size)) {
        // Check if we're near the end of loaded tracks (within 20 items)
        int remaining_loaded = static_cast<int>(browse_tracks.size()) - (visible_start + max_items);
        if (remaining_loaded < 20 && browse_tracks.size() >= static_cast<size_t>(playlist_loaded_count)) {
            // Load next chunk
            try {
                std::vector<Track> next_chunk = client.get_playlist_tracks(
                    current_playlist_id, 
                    playlist_loaded_count, 
                    PLAYLIST_CHUNK_SIZE
                );
                if (!next_chunk.empty()) {
                    browse_tracks.insert(browse_tracks.end(), next_chunk.begin(), next_chunk.end());
                    playlist_loaded_count += static_cast<int>(next_chunk.size());
                }
            } catch (...) {
                // Failed to load more - continue with what we have
            }
        }
    }
    
    // Lazy loading: Load more search results if user scrolls near the end
    if (is_search_mode && !current_search_query.empty()) {
        // Check if we're near the end of loaded search results (within 20 items)
        int remaining_loaded = static_cast<int>(browse_tracks.size()) - (visible_start + max_items);
        if (remaining_loaded < 20 && browse_tracks.size() >= static_cast<size_t>(search_loaded_count)) {
            // Load next chunk of search results
            try {
                std::vector<Track> next_chunk = client.search_tracks(
                    current_search_query,
                    SEARCH_CHUNK_SIZE,
                    search_loaded_count
                );
                if (!next_chunk.empty()) {
                    // Deduplicate: Plex search with offset often returns overlapping results.
                    // Use same ID + signature logic as perform_search().
                    auto normalize_sig = [](const std::string& title, const std::string& artist, const std::string& album) -> std::string {
                        std::string sig = title + "|" + artist + "|" + album;
                        std::transform(sig.begin(), sig.end(), sig.begin(), ::tolower);
                        sig.erase(std::remove_if(sig.begin(), sig.end(), [](char c) { return std::isspace(c); }), sig.end());
                        return sig;
                    };
                    std::set<std::string> existing_ids;
                    std::set<std::string> existing_sigs;
                    for (const auto& t : browse_tracks) {
                        if (!t.id.empty()) existing_ids.insert(t.id);
                        existing_sigs.insert(normalize_sig(t.title, t.artist, t.album));
                    }
                    int added = 0;
                    for (const auto& track : next_chunk) {
                        bool dup = false;
                        if (!track.id.empty() && existing_ids.find(track.id) != existing_ids.end()) dup = true;
                        if (!dup) {
                            std::string sig = normalize_sig(track.title, track.artist, track.album);
                            if (existing_sigs.find(sig) != existing_sigs.end()) dup = true;
                        }
                        if (!dup) {
                            browse_tracks.push_back(track);
                            if (!track.id.empty()) existing_ids.insert(track.id);
                            existing_sigs.insert(normalize_sig(track.title, track.artist, track.album));
                            ++added;
                        }
                    }
                    search_loaded_count += added;
                    if (added == 0 && !next_chunk.empty()) {
                        // All duplicates - stop pagination to avoid infinite loop
                        is_search_mode = false;
                    }
                } else {
                    // No more results - stop trying
                    is_search_mode = false;
                }
            } catch (...) {
                // Failed to load more - continue with what we have
            }
        }
    }
    
    // Draw small album art in top-right if viewing album tracks (cached, small size)
    if (show_album_art && config.enable_album_art) {
        try {
            // Initialize album art object if needed
            if (!album_art_for_tracks) {
                album_art_for_tracks = std::make_unique<AlbumArt>();
            }
            
            // Fetch album art if not already loaded or if album changed (cached)
            static std::string last_album_id;
            if (last_album_id != current_album_id || !album_art_for_tracks->has_art()) {
                if (!current_album.art_url.empty()) {
                    try {
                        album_art_for_tracks->fetch_art(
                            client.get_server_url(),
                            client.get_token(),
                            current_album.art_url
                        );
                    } catch (...) {
                        // Fetch failed - continue without art
                    }
                }
                last_album_id = current_album_id;
            }
            
            // Render small pixelated album art (cached)
            if (album_art_for_tracks->has_art()) {
                auto art_lines = album_art_for_tracks->render_pixelated(small_art_w, small_art_h, config.theme);
                for (size_t y = 0; y < art_lines.size() && y < static_cast<size_t>(small_art_h); ++y) {
                    if (small_art_y + static_cast<int>(y) < h && small_art_x >= 0 && small_art_x < w) {
                        term.draw_text(small_art_x, small_art_y + static_cast<int>(y), art_lines[y]);
                    }
                }
            }
            
            // Draw MusicBrainz album info to the LEFT of album art (if album data enabled)
            // TEMPORARILY DISABLED to debug crash - re-enable once crash is fixed
            // Wrap entire section in try-catch to prevent any crashes
            // Only try to fetch if we have valid album info and album art is shown
            if (false && config.enable_album_data && has_track_album_art && !current_album_id.empty() && 
                !current_album.title.empty() && !current_album.artist.empty() && small_art_x < w) {
                try {
                    // Fetch MusicBrainz data (cached per album)
                    static std::string last_mb_album_id;
                    static PlexClient::MusicBrainzData mb_data;
                    if (last_mb_album_id != current_album_id || !mb_data.valid) {
                        // Wrap in try-catch to prevent crashes from MusicBrainz API
                        try {
                            mb_data = client.get_musicbrainz_data(current_album.artist, current_album.title);
                        } catch (const std::exception& e) {
                            // MusicBrainz fetch failed - continue without it
                            mb_data.valid = false;
                        } catch (...) {
                            // Unknown error - continue without MusicBrainz data
                            mb_data.valid = false;
                        }
                        last_mb_album_id = current_album_id;
                    }
                    
                    if (mb_data.valid) {
                        // Calculate position to the left of album art
                        int mb_info_w = 40;  // Width for MusicBrainz info
                        int mb_info_x = small_art_x - mb_info_w - 2;  // Left of album art with 2 char gap
                        int mb_info_y = small_art_y;
                        
                        // Validate coordinates before drawing
                        if (mb_info_x >= sidebar_w + 2 && mb_info_x < w && mb_info_y >= 0 && mb_info_y < h) {
                            std::string black_bg = term.bg_color(0, 0, 0);
                            std::string header_color = term.fg_color(255, 140, 0);  // Orange for header
                            std::string label_color = term.fg_color(200, 200, 200);
                            std::string value_color = term.fg_color(255, 255, 255);
                            std::string dim_color = term.fg_color(150, 150, 150);
                            
                            int y_offset = 0;
                            
                            // Header
                            term.draw_text(mb_info_x, mb_info_y + y_offset++, 
                                          black_bg + header_color + "MusicBrainz" + term.reset_color());
                            y_offset++;  // Blank line
                            
                            // Release date
                            if (!mb_data.release_date.empty()) {
                                std::string date_label = "Date: ";
                                std::string date_value = mb_data.release_date;
                                if (date_value.length() > static_cast<size_t>(mb_info_w - date_label.length() - 1)) {
                                    date_value = date_value.substr(0, mb_info_w - date_label.length() - 4) + "...";
                                }
                                term.draw_text(mb_info_x, mb_info_y + y_offset++, 
                                              black_bg + label_color + date_label + value_color + date_value + term.reset_color());
                            }
                            
                            // Label
                            if (!mb_data.label.empty()) {
                                std::string label_label = "Label: ";
                                std::string label_value = mb_data.label;
                                if (label_value.length() > static_cast<size_t>(mb_info_w - label_label.length() - 1)) {
                                    label_value = label_value.substr(0, mb_info_w - label_label.length() - 4) + "...";
                                }
                                term.draw_text(mb_info_x, mb_info_y + y_offset++, 
                                              black_bg + label_color + label_label + value_color + label_value + term.reset_color());
                            }
                            
                            // Format
                            if (!mb_data.format.empty()) {
                                std::string format_label = "Format: ";
                                std::string format_value = mb_data.format;
                                if (format_value.length() > static_cast<size_t>(mb_info_w - format_label.length() - 1)) {
                                    format_value = format_value.substr(0, mb_info_w - format_label.length() - 4) + "...";
                                }
                                term.draw_text(mb_info_x, mb_info_y + y_offset++, 
                                              black_bg + label_color + format_label + value_color + format_value + term.reset_color());
                            }
                            
                            // Country
                            if (!mb_data.country.empty()) {
                                std::string country_label = "Country: ";
                                std::string country_value = mb_data.country;
                                if (country_value.length() > static_cast<size_t>(mb_info_w - country_label.length() - 1)) {
                                    country_value = country_value.substr(0, mb_info_w - country_label.length() - 4) + "...";
                                }
                                term.draw_text(mb_info_x, mb_info_y + y_offset++, 
                                              black_bg + label_color + country_label + value_color + country_value + term.reset_color());
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    // MusicBrainz error - ignore and continue
                } catch (...) {
                    // MusicBrainz error - ignore and continue
                }
            }
            
            // Draw album/artist info on right side below album art (if album data enabled)
            if (config.enable_album_data) {
                int info_x = small_art_x;
                int info_y = small_art_y + small_art_h + 1;
                std::string black_bg = term.bg_color(0, 0, 0);
                std::string title_color = term.fg_color(255, 255, 255);
                std::string artist_color = term.fg_color(200, 200, 200);
                std::string year_color = term.fg_color(150, 150, 150);
                
                if (info_x >= 0 && info_x < w && info_y >= 0 && info_y < h) {
                    // Draw album title
                    std::string title = current_album.title;
                    if (title.length() > static_cast<size_t>(small_art_w)) {
                        title = title.substr(0, small_art_w - 3) + "...";
                    }
                    term.draw_text(info_x, info_y, black_bg + title_color + title + term.reset_color());
                    
                    // Draw artist
                    if (!current_album.artist.empty()) {
                        std::string artist = current_album.artist;
                        if (artist.length() > static_cast<size_t>(small_art_w)) {
                            artist = artist.substr(0, small_art_w - 3) + "...";
                        }
                        term.draw_text(info_x, info_y + 1, black_bg + artist_color + artist + term.reset_color());
                    }
                    
                    // Draw year
                    if (current_album.year > 0) {
                        term.draw_text(info_x, info_y + 2, 
                                      black_bg + year_color + "(" + std::to_string(current_album.year) + ")" + term.reset_color());
                    }
                }
            }
        } catch (...) {
            // Ignore errors - don't crash if album art fails
        }
    }
    
    // Show message if no tracks
    if (browse_tracks.empty()) {
        std::string black_bg = term.bg_color(0, 0, 0);
        std::string msg_color = term.fg_color(config.theme.dimmed.r, config.theme.dimmed.g, config.theme.dimmed.b);
        term.draw_text(list_x, start_y, black_bg + msg_color + "No tracks found." + term.reset_color());
        return;
    }
    
    // Clear each line before drawing to remove fragments (btop-style)
    // Only clear the area where tracks will be drawn (not album art area)
    int clear_width = list_max_width;
    if (clear_width > 0 && clear_width <= 1000) {
        std::string black_bg = term.bg_color(0, 0, 0);
        std::string clear_line(clear_width, ' ');
        for (int i = 0; i < max_items; ++i) {
            term.draw_text(list_x, start_y + i, black_bg + clear_line + term.reset_color());
        }
    }
    
    for (int i = 0; i < max_items && (visible_start + i) < static_cast<int>(browse_tracks.size()); ++i) {
        int idx = visible_start + i;
        bool selected = (idx == selected_index);
        
        // Bright text - white when selected, dim when not
        std::string title_color = selected ? 
            term.fg_color(255, 255, 255) : 
            term.fg_color(220, 220, 220);
        
        std::string artist_color = term.fg_color(180, 180, 180);
        std::string album_color = term.fg_color(150, 150, 150);
        std::string time_color = term.fg_color(130, 130, 130);
        std::string marker = selected ? "> " : "  ";
        
        std::string black_bg = term.bg_color(0, 0, 0);
        
        // Build line components and truncate to fit available width
        std::string time_str = " [" + format_time(browse_tracks[idx].duration_ms) + "]";
        int time_len = static_cast<int>(time_str.length());
        int marker_len = 2;
        int available_for_text = list_max_width - marker_len - time_len;
        if (available_for_text < 10) available_for_text = 10;  // Minimum space
        
        std::string title = browse_tracks[idx].title;
        std::string artist_part = "";
        std::string album_part = "";
        
        if (!browse_tracks[idx].artist.empty()) {
            artist_part = " • " + browse_tracks[idx].artist;
        }
        if (!browse_tracks[idx].album.empty()) {
            album_part = " • " + browse_tracks[idx].album;
        }
        
        // Calculate total text length (without ANSI codes)
        int total_text_len = static_cast<int>(title.length() + artist_part.length() + album_part.length());
        
        // Truncate if needed
        if (total_text_len > available_for_text) {
            // Prioritize: title > artist > album
            int title_max = std::min(static_cast<int>(title.length()), available_for_text - 3);
            if (title_max < 0) title_max = 0;
            
            if (total_text_len > available_for_text) {
                // Truncate title first
                if (title.length() > static_cast<size_t>(title_max)) {
                    title = title.substr(0, title_max) + "...";
                    total_text_len = static_cast<int>(title.length() + artist_part.length() + album_part.length());
                }
                
                // If still too long, remove album
                if (total_text_len > available_for_text && !album_part.empty()) {
                    album_part = "";
                    total_text_len = static_cast<int>(title.length() + artist_part.length());
                }
                
                // If still too long, truncate artist
                if (total_text_len > available_for_text && !artist_part.empty()) {
                    int artist_available = available_for_text - static_cast<int>(title.length()) - 3;
                    if (artist_available > 0) {
                        std::string artist_only = browse_tracks[idx].artist;
                        if (artist_only.length() > static_cast<size_t>(artist_available)) {
                            artist_only = artist_only.substr(0, artist_available - 3) + "...";
                        }
                        artist_part = " • " + artist_only;
                    } else {
                        artist_part = "";
                    }
                }
            }
        }
        
        // Build final line
        std::string line = black_bg + marker + title_color + title + term.reset_color();
        if (!artist_part.empty()) {
            line += black_bg + artist_color + artist_part + term.reset_color();
        }
        if (!album_part.empty()) {
            line += black_bg + album_color + album_part + term.reset_color();
        }
        line += black_bg + time_color + time_str + term.reset_color();
        
        term.draw_text(list_x, start_y + i, line);
    }
    
    // Show loading indicator if more tracks are available
    if (!current_playlist_id.empty() && 
        (playlist_total_size == 0 || playlist_loaded_count < playlist_total_size)) {
        std::string loading_color = term.fg_color(180, 180, 180);
        std::string loading_msg;
        if (playlist_total_size > 0) {
            loading_msg = "... Loading " + std::to_string(playlist_loaded_count) + 
                         " of " + std::to_string(playlist_total_size) + " tracks ...";
        } else {
            loading_msg = "... Loaded " + std::to_string(playlist_loaded_count) + " tracks (scroll for more) ...";
        }
        std::string black_bg = term.bg_color(0, 0, 0);
        term.draw_text(list_x, start_y + max_items, black_bg + loading_color + loading_msg + term.reset_color());
    } else if (is_search_mode && !current_search_query.empty()) {
        // Show loading indicator for search results
        std::string loading_color = term.fg_color(180, 180, 180);
        std::string loading_msg = "... Loaded " + std::to_string(search_loaded_count) + 
                                 " search results (scroll for more) ...";
        std::string black_bg = term.bg_color(0, 0, 0);
        term.draw_text(list_x, start_y + max_items, black_bg + loading_color + loading_msg + term.reset_color());
    }
}

void PlayerView::handle_search_input(char c) {
    if (c == '\b' || c == 127) {  // Backspace
        if (!search_query.empty()) {
            search_query.pop_back();
            // Immediate search on backspace (user is refining query)
            last_search_time = std::chrono::steady_clock::now();
            search_pending = true;
        } else {
            browse_tracks.clear();
            // Clear search pagination
            is_search_mode = false;
            current_search_query.clear();
            search_loaded_count = 0;
            // Clear playlist pagination
            current_playlist_id.clear();
            playlist_total_size = 0;
            playlist_loaded_count = 0;
            search_pending = false;
        }
    } else if (c == '\n' || c == '\r') {  // Enter
        search_active = false;
        // Immediate search on Enter
        perform_search();
        search_pending = false;
    } else if (c >= 32 && c < 127) {  // Printable character (all alphanumeric and symbols)
        // Allow all printable characters in search (btop-style: full text search)
        search_query += c;
        // Debounce: schedule search after user stops typing
        last_search_time = std::chrono::steady_clock::now();
        search_pending = true;
    }
    // Note: Non-printable characters are ignored (handled by caller)
}

// Note: url_encode function removed - duplicate of PlexClient::Impl::url_encode
// If needed, use PlexClient's internal implementation or create a shared utility

void PlayerView::draw_lyrics(const Layout& layout) {
    int lyrics_x = layout.waveform_x;
    int lyrics_w = layout.waveform_w;
    int orange_line_y = layout.track_info_y - 1;
    // Anchor lyrics to bottom: position so 5 lines end just above orange line
    const int lyrics_reserved_lines = 5;
    int lyrics_y = orange_line_y - lyrics_reserved_lines;  // Anchor to bottom, 5 lines above orange line
    int available = orange_line_y - 1 - lyrics_y;
    if (available <= 0 || lyrics_y < layout.waveform_y + layout.waveform_h + 1) {
        // Fallback: if not enough space, position right after waveform
        lyrics_y = layout.waveform_y + layout.waveform_h + 1;
        available = orange_line_y - 1 - lyrics_y;
        if (available <= 0) return;
    }
    int visible_lines = std::min(5, available);  // Match lyrics_reserved_lines (5 lines)
    int center_offset = visible_lines / 2;
    int h = term.height();
    if (h <= 0) h = 24;
    std::string black_bg = term.bg_color(0, 0, 0);

    if (pending_play) {
        std::string hint = "Fetching lyrics…";
        std::string dim = term.fg_color(150, 150, 150);
        int pad = (lyrics_w - static_cast<int>(hint.length())) / 2;
        if (pad < 0) pad = 0;
        std::string line = std::string(pad, ' ') + hint;
        int y = lyrics_y + center_offset;
        if (y >= 0 && y < h)
            term.draw_text(lyrics_x, y, black_bg + dim + line + term.reset_color());
        return;
    }

    if (playback_state.current_track.id.empty() || !client.is_connected()) {
        if (config.enable_debug_logging) {
            std::cerr << "[LOG] draw_lyrics: early return - track_id="
                      << playback_state.current_track.id << ", connected="
                      << client.is_connected() << std::endl;
        }
        return;
    }
    // Lyrics are fetched in update() function, not here
    
    // If track changed, clear old lyrics (update() will fetch new ones)
    if (last_lyrics_track_id != playback_state.current_track.id) {
        lyrics_lines.clear();
        synced_lyrics.clear();
        lyrics_scroll_position = 0;
        // Don't clear current_lyrics here - update() might have already set it
        // Just return if we don't have lyrics yet
        if (current_lyrics.empty() && synced_lyrics.empty()) {
            return;  // Wait for update() to fetch lyrics
        }
    }
    
    // Note: synced_lyrics are already retrieved in update() - no need to call again here
    // This redundant call has been removed to optimize API calls

    // Use time-synced lyrics if available, otherwise fall back to regular lyrics
    if (!synced_lyrics.empty()) {
        // Time-synced lyrics: find current line based on playback position
        uint32_t current_pos_ms = playback_state.position_ms;
        
        // Debug: log when we're about to draw synced lyrics
        static uint32_t last_logged_pos = 0;
        if (config.enable_debug_logging && (current_pos_ms / 1000) != (last_logged_pos / 1000)) {
            std::cerr << "[LOG] Drawing synced lyrics at position " << current_pos_ms << "ms, have " 
                      << synced_lyrics.size() << " lines" << std::endl;
            last_logged_pos = current_pos_ms;
        }
        
        // Find the lyric line that should be displayed at current position
        int current_line_idx = -1;
        for (size_t i = 0; i < synced_lyrics.size(); ++i) {
            if (synced_lyrics[i].timestamp_ms <= current_pos_ms) {
                current_line_idx = static_cast<int>(i);
            } else {
                break;  // Lines are sorted by timestamp
            }
        }
        if (current_line_idx < 0 && !synced_lyrics.empty()) {
            current_line_idx = 0;  // Before first timestamp: show first line
        }
        
        // Display lines around the current one
        for (int i = 0; i < visible_lines; ++i) {
            int line_idx = current_line_idx + i - center_offset;
            if (line_idx >= 0 && line_idx < static_cast<int>(synced_lyrics.size())) {
                std::string line_text = synced_lyrics[line_idx].text;
                
                // Truncate to fit width
                if (static_cast<int>(line_text.length()) > lyrics_w) {
                    line_text = line_text.substr(0, lyrics_w - 3) + "...";
                }
                
                // Fade effect: center line is brightest, fade out towards edges
                int distance_from_center = std::abs(i - center_offset);
                uint8_t brightness = 255 - (distance_from_center * 80);  // Fade: 255, 175, 95
                if (brightness < 100) brightness = 100;  // Minimum visibility
                
                std::string lyrics_color = term.fg_color(brightness, brightness, brightness);
                
                // Center the line text
                int padding = (lyrics_w - static_cast<int>(line_text.length())) / 2;
                if (padding < 0) padding = 0;
                std::string centered_line = std::string(padding, ' ') + line_text;
                
                if (lyrics_y + i >= 0 && lyrics_y + i < h) {
                    term.draw_text(lyrics_x, lyrics_y + i, black_bg + lyrics_color + centered_line + term.reset_color());
                }
            }
        }
    } else {
        // Fallback to regular (non-synced) lyrics with auto-scroll
        // Parse lyrics into lines if we have lyrics but no lines yet
        if (!current_lyrics.empty() && lyrics_lines.empty()) {
            std::string line;
            for (char c : current_lyrics) {
                if (c == '\n' || c == '\r') {
                    if (!line.empty()) {
                        lyrics_lines.push_back(line);
                        line.clear();
                    }
                } else if (c >= 32 && c < 127) {  // Printable character
                    line += c;
                }
            }
            if (!line.empty()) {
                lyrics_lines.push_back(line);
            }
        }
        
        if (lyrics_lines.empty()) {
            return;
        }

        int max_scroll = std::max(0, static_cast<int>(lyrics_lines.size()) - visible_lines);
        if (lyrics_scroll_position < 0) lyrics_scroll_position = 0;
        if (lyrics_scroll_position > max_scroll) lyrics_scroll_position = max_scroll;

        for (int i = 0; i < visible_lines; ++i) {
            int line_idx = lyrics_scroll_position + i;
            if (line_idx < 0 || line_idx >= static_cast<int>(lyrics_lines.size())) continue;
            std::string line_text = lyrics_lines[line_idx];
            if (static_cast<int>(line_text.length()) > lyrics_w) {
                line_text = line_text.substr(0, lyrics_w - 3) + "...";
            }
            uint8_t brightness = 255 - (i * 25);
            if (brightness < 150) brightness = 150;
            std::string lyrics_color = term.fg_color(brightness, brightness, brightness);
            int padding = (lyrics_w - static_cast<int>(line_text.length())) / 2;
            if (padding < 0) padding = 0;
            std::string centered_line = std::string(padding, ' ') + line_text;
            if (lyrics_y + i >= 0 && lyrics_y + i < h) {
                term.draw_text(lyrics_x, lyrics_y + i, black_bg + lyrics_color + centered_line + term.reset_color());
            }
        }
        if (max_scroll > 0 && available > visible_lines && lyrics_y + visible_lines >= 0 && lyrics_y + visible_lines < h) {
            std::string hint = "↑↓ scroll";
            std::string dim = term.fg_color(100, 100, 100);
            int pad = (lyrics_w - static_cast<int>(hint.length())) / 2;
            if (pad < 0) pad = 0;
            term.draw_text(lyrics_x, lyrics_y + visible_lines, black_bg + dim + std::string(pad, ' ') + hint + term.reset_color());
        }
    }
}

void PlayerView::draw_options_menu() {
    int w = term.width();
    int h = term.height();
    if (w < 80 || h < 24) return;  // Too small for menu
    
    // Menu dimensions (btop-style: centered overlay, responsive width)
    int menu_w = std::min(70, w - 10);  // Prevent overhang, leave 5 chars margin on each side
    int menu_h = 20;
    int menu_x = (w - menu_w) / 2;
    int menu_y = (h - menu_h) / 2;
    
    std::string black_bg = term.bg_color(0, 0, 0);
    std::string orange_color = term.fg_color(255, 140, 0);  // Plex orange
    std::string white = term.fg_color(255, 255, 255);
    std::string dim = term.fg_color(150, 150, 150);
    std::string selected_bg = term.bg_color(30, 20, 10);  // Dark orange tint for selection
    std::string selected_fg = orange_color;  // Orange text for selected items
    
    // Draw semi-transparent overlay background (darken screen)
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            term.draw_text(x, y, term.bg_color(0, 0, 0) + " " + term.reset_color());
        }
    }
    
    // Draw menu box (btop-style: orange border)
    term.draw_box(menu_x, menu_y, menu_w, menu_h, "Options");
    
    // Category tabs (btop-style with orange hotkeys)
    std::vector<std::string> categories = {"Plex", "Display", "Features"};
    int tab_x = menu_x + 2;
    int tab_y = menu_y + 1;
    for (size_t i = 0; i < categories.size(); ++i) {
        bool is_active = (i == static_cast<size_t>(options_menu_category));
        std::string tab_text;
        if (is_active) {
            // Active: [Category] with orange brackets
            tab_text = orange_color + "[" + white + categories[i] + orange_color + "]";
        } else {
            // Inactive: "1 Category" with orange hotkey
            std::string hotkey = std::to_string(i + 1);
            tab_text = orange_color + hotkey + dim + " " + categories[i];
        }
        term.draw_text(tab_x, tab_y, black_bg + tab_text + term.reset_color());
        tab_x += 18;  // Slightly tighter spacing
    }
    
    // Draw options for current category
    int opt_y = menu_y + 3;
    int max_visible = menu_h - 5;
    
    // Define options for each category
    struct Option {
        std::string name;
        std::string key;
        bool is_bool;
        bool is_int;
    };
    
    std::vector<std::vector<Option>> category_options = {
        // Plex
        {{"Server URL", "plex_server_url", false, false},
         {"Token", "plex_token", false, false},
         {"Config File", "config_file_path", false, false}},  // Show config file path
        // Display
        {{"Max Waveform Points", "max_waveform_points", false, true},
         {"Refresh Rate (ms)", "refresh_rate_ms", false, true},
         {"Window Width", "window_width", false, true},
         {"Window Height", "window_height", false, true}},
        // Features
        {{"Enable Waveform", "enable_waveform", true, false},
         {"Enable Lyrics", "enable_lyrics", true, false},
         {"Enable Album Art", "enable_album_art", true, false},
         {"Enable Album Data", "enable_album_data", true, false},
         {"Enable Debug Logging", "enable_debug_logging", true, false},
         {"Debug Log File Path", "debug_log_file_path", false, false}}
    };
    
    auto& options = category_options[options_menu_category];
    int start_idx = std::max(0, options_menu_selected - max_visible + 1);
    int end_idx = std::min(static_cast<int>(options.size()), start_idx + max_visible);
    
    for (int i = start_idx; i < end_idx; ++i) {
        if (i >= static_cast<int>(options.size())) break;
        const auto& opt = options[i];
        bool is_selected = (i == options_menu_selected);
        
        std::string bg_color = is_selected ? selected_bg : black_bg;
        std::string fg_color = is_selected ? selected_fg : white;
        
        // Option name (btop-style: orange for selected)
        // Use fixed smaller width for names to give more space to values
        int max_name_len = 18;  // Fixed width for names (allows more space for values)
        std::string name_text = opt.name;
        if (name_text.length() > static_cast<size_t>(max_name_len)) {
            name_text = name_text.substr(0, max_name_len - 3) + "...";
        }
        std::string name_color = is_selected ? orange_color : white;
        term.draw_text(menu_x + 2, opt_y, bg_color + name_color + name_text + term.reset_color());
        
        // Option value (starts earlier to show more of URL)
        int value_x = menu_x + max_name_len + 3;  // Start value column earlier
        int max_value_width = menu_w - (value_x - menu_x) - 2;  // Remaining space for value
        
        std::string value_text;
        if (opt.is_bool) {
            bool val = false;
            if (opt.key == "enable_waveform") val = config.enable_waveform;
            else if (opt.key == "enable_lyrics") val = config.enable_lyrics;
            else if (opt.key == "enable_album_art") val = config.enable_album_art;
            else if (opt.key == "enable_album_data") val = config.enable_album_data;
            else if (opt.key == "enable_debug_logging") val = config.enable_debug_logging;
            value_text = val ? "true" : "false";
        } else if (opt.is_int) {
            int val = 0;
            if (opt.key == "max_waveform_points") val = config.max_waveform_points;
            else if (opt.key == "refresh_rate_ms") val = config.refresh_rate_ms;
            else if (opt.key == "window_width") val = config.window_width;
            else if (opt.key == "window_height") val = config.window_height;
            value_text = std::to_string(val);
        } else {
            if (opt.key == "plex_server_url") {
                value_text = config.plex_server_url;
            } else if (opt.key == "plex_token") {
                value_text = std::string(config.plex_token.length(), '*');  // Hide token
            } else if (opt.key == "config_file_path") {
                // Show config file path
                const char* home = getenv("HOME");
                value_text = (home ? std::string(home) + "/.config/plex-tui/config.ini" : "~/.config/plex-tui/config.ini");
            } else if (opt.key == "debug_log_file_path") {
                // Show debug log file path (or default if empty)
                if (!config.debug_log_file_path.empty()) {
                    value_text = config.debug_log_file_path;
                } else {
                    const char* home = getenv("HOME");
                    value_text = (home ? std::string(home) + "/.config/plex-tui/debug.log" : "~/.config/plex-tui/debug.log");
                    value_text += " (default)";
                }
            }
            // Truncate if too long, but allow more space now
            if (value_text.length() > static_cast<size_t>(max_value_width)) {
                value_text = value_text.substr(0, max_value_width - 3) + "...";
            }
        }
        
        if (is_selected && options_menu_editing && opt.key == options_menu_edit_option) {
            value_text = options_menu_edit_buffer + "_";
        }
        
        // Value color: orange if selected, white otherwise
        // Config file path is read-only (dimmed)
        std::string value_color;
        if (opt.key == "config_file_path") {
            value_color = dim;  // Read-only, dimmed
        } else {
            value_color = is_selected ? orange_color : white;
        }
        term.draw_text(value_x, opt_y, bg_color + value_color + value_text + term.reset_color());
        opt_y++;
        
        // Show description/hint for selected option (btop-style)
        if (is_selected && opt_y < menu_y + menu_h - 3) {
            std::string desc_text;
            if (opt.key == "plex_server_url") {
                desc_text = dim + "Include port if needed (e.g., :32400, :443, :80)";
            } else if (opt.key == "plex_token") {
                desc_text = dim + "Your Plex authentication token";
            } else if (opt.key == "config_file_path") {
                desc_text = dim + "Read-only: location of config file";
            } else if (opt.key == "debug_log_file_path") {
                desc_text = dim + "Path to debug log file (default: next to config.ini)";
            } else if (opt.key == "max_waveform_points") {
                desc_text = dim + "Number of waveform data points to display";
            } else if (opt.key == "refresh_rate_ms") {
                desc_text = dim + "UI refresh rate in milliseconds (lower = smoother)";
            } else if (opt.key == "window_width") {
                desc_text = dim + "Terminal width in characters (columns)";
            } else if (opt.key == "window_height") {
                desc_text = dim + "Terminal height in characters (rows)";
            }
            
            if (!desc_text.empty() && opt_y < menu_y + menu_h - 3) {
                // Truncate description if too long
                int max_desc_width = menu_w - 4;
                if (desc_text.length() > static_cast<size_t>(max_desc_width)) {
                    desc_text = desc_text.substr(0, max_desc_width - 3) + "...";
                }
                term.draw_text(menu_x + 2, opt_y, black_bg + desc_text + term.reset_color());
                opt_y++;
            }
        }
    }
    
    // Help text at bottom (btop-style: orange hotkeys)
    std::string help_text = orange_color + "Tab" + dim + ": switch | " + 
                           orange_color + "Enter" + dim + ": edit | " +
                           orange_color + "←→" + dim + ": change | " +
                           orange_color + "Esc" + dim + ": close | " +
                           orange_color + "S" + dim + ": save";
    term.draw_text(menu_x + 2, menu_y + menu_h - 2, black_bg + help_text + term.reset_color());
}

void PlayerView::handle_options_menu_input(const InputEvent& event) {
    if (event.is_mouse()) return;  // Mouse not supported in menu yet
    
    // Define options for each category (same as in draw_options_menu)
    struct Option {
        std::string name;
        std::string key;
        bool is_bool;
        bool is_int;
    };
    
    std::vector<std::vector<Option>> category_options = {
        {{"Server URL", "plex_server_url", false, false},
         {"Token", "plex_token", false, false},
         {"Config File", "config_file_path", false, false}},  // Read-only display
        {{"Max Waveform Points", "max_waveform_points", false, true},
         {"Refresh Rate (ms)", "refresh_rate_ms", false, true},
         {"Window Width", "window_width", false, true},
         {"Window Height", "window_height", false, true}},
        {{"Enable Waveform", "enable_waveform", true, false},
         {"Enable Lyrics", "enable_lyrics", true, false},
         {"Enable Album Art", "enable_album_art", true, false},
         {"Enable Album Data", "enable_album_data", true, false},
         {"Enable Debug Logging", "enable_debug_logging", true, false},
         {"Debug Log File Path", "debug_log_file_path", false, false}}
    };
    
    auto& options = category_options[options_menu_category];
    
    if (options_menu_editing) {
        // Handle text input for editing
        if (event.key == Key::Escape) {
            options_menu_editing = false;
            options_menu_edit_buffer.clear();
            options_menu_edit_option.clear();
        } else if (event.key == Key::Enter) {
            // Apply edit
            const auto& opt = options[options_menu_selected];
            if (opt.is_int) {
                try {
                    int val = std::stoi(options_menu_edit_buffer);
                    if (opt.key == "max_waveform_points") config.max_waveform_points = val;
                    else if (opt.key == "refresh_rate_ms") config.refresh_rate_ms = val;
                    else if (opt.key == "window_width") config.window_width = val;
                    else if (opt.key == "window_height") config.window_height = val;
                } catch (...) {}
            } else if (!opt.is_bool) {
                if (opt.key == "plex_server_url") config.plex_server_url = options_menu_edit_buffer;
                else if (opt.key == "plex_token") config.plex_token = options_menu_edit_buffer;
                else if (opt.key == "debug_log_file_path") config.debug_log_file_path = options_menu_edit_buffer;
            }
            options_menu_editing = false;
            options_menu_edit_buffer.clear();
            options_menu_edit_option.clear();
        } else if (event.key == Key::Backspace) {
            if (!options_menu_edit_buffer.empty()) {
                options_menu_edit_buffer.pop_back();
            }
        } else if (event.key == Key::Char) {
            options_menu_edit_buffer += event.character;
        }
        return;
    }
    
    // Menu navigation
    switch (event.key) {
        case Key::Escape:
            options_menu_active = false;
            options_menu_selected = 0;
            break;
        case Key::Tab:
            options_menu_category = (options_menu_category + 1) % 3;
            options_menu_selected = 0;
            break;
        case Key::Up:
            if (options_menu_selected > 0) options_menu_selected--;
            break;
        case Key::Down:
            if (options_menu_selected < static_cast<int>(options.size()) - 1) options_menu_selected++;
            break;
        case Key::Enter:
            // Start editing (skip if config_file_path - read-only)
            {
                const auto& opt = options[options_menu_selected];
                if (opt.key == "config_file_path") {
                    // Read-only, do nothing
                    break;
                }
                if (opt.is_bool) {
                    // Toggle boolean
                    if (opt.key == "enable_waveform") config.enable_waveform = !config.enable_waveform;
                    else if (opt.key == "enable_lyrics") config.enable_lyrics = !config.enable_lyrics;
                    else if (opt.key == "enable_album_art") config.enable_album_art = !config.enable_album_art;
                    else if (opt.key == "enable_album_data") config.enable_album_data = !config.enable_album_data;
                    else if (opt.key == "enable_debug_logging") config.enable_debug_logging = !config.enable_debug_logging;
                } else {
                    // Start text editing
                    options_menu_editing = true;
                    options_menu_edit_option = opt.key;
                    options_menu_edit_buffer.clear();
                    if (opt.is_int) {
                        int val = 0;
                        if (opt.key == "max_waveform_points") val = config.max_waveform_points;
                        else if (opt.key == "refresh_rate_ms") val = config.refresh_rate_ms;
                        else if (opt.key == "window_width") val = config.window_width;
                        else if (opt.key == "window_height") val = config.window_height;
                        options_menu_edit_buffer = std::to_string(val);
                    } else {
                        if (opt.key == "plex_server_url") options_menu_edit_buffer = config.plex_server_url;
                        else if (opt.key == "plex_token") options_menu_edit_buffer = config.plex_token;
                        else if (opt.key == "debug_log_file_path") options_menu_edit_buffer = config.debug_log_file_path;
                    }
                }
            }
            break;
        case Key::Left:
        case Key::Right:
            {
                const auto& opt = options[options_menu_selected];
                if (opt.is_bool) {
                    // Toggle boolean
                    if (opt.key == "enable_waveform") config.enable_waveform = !config.enable_waveform;
                    else if (opt.key == "enable_lyrics") config.enable_lyrics = !config.enable_lyrics;
                    else if (opt.key == "enable_album_art") config.enable_album_art = !config.enable_album_art;
                    else if (opt.key == "enable_album_data") config.enable_album_data = !config.enable_album_data;
                    else if (opt.key == "enable_debug_logging") config.enable_debug_logging = !config.enable_debug_logging;
                } else if (opt.is_int) {
                    // Increment/decrement integer
                    int mod = (opt.key == "refresh_rate_ms" || opt.key == "window_width" || opt.key == "window_height") ? 5 : 1;
                    int val = 0;
                    if (opt.key == "max_waveform_points") val = config.max_waveform_points;
                    else if (opt.key == "refresh_rate_ms") val = config.refresh_rate_ms;
                    else if (opt.key == "window_width") val = config.window_width;
                    else if (opt.key == "window_height") val = config.window_height;
                    
                    if (event.key == Key::Right) val += mod;
                    else val -= mod;
                    
                    if (val < 0) val = 0;
                    if (opt.key == "max_waveform_points") config.max_waveform_points = val;
                    else if (opt.key == "refresh_rate_ms") config.refresh_rate_ms = val;
                    else if (opt.key == "window_width") config.window_width = val;
                    else if (opt.key == "window_height") config.window_height = val;
                }
            }
            break;
        case Key::Char:
            if (event.character == 's' || event.character == 'S') {
                save_config();
            }
            break;
        default:
            break;
    }
}

void PlayerView::save_config() {
    const char* home = getenv("HOME");
    if (home) {
        std::string config_path = std::string(home) + "/.config/plex-tui/config.ini";
        if (config.save_to_file(config_path)) {
            status_message = "Configuration saved to " + config_path;
        } else {
            status_message = "Failed to save configuration";
        }
    } else {
        status_message = "Failed to save: HOME environment variable not set";
    }
}

} // namespace PlexTUI
