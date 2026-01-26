#pragma once

#include <functional>
#include <string>
#include <unordered_map>

namespace PlexTUI {

enum class Key {
    None,
    Escape,
    Enter,
    Space,
    Backspace,
    Tab,
    Up,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
    Home,
    End,
    
    // Playback controls
    Play,       // 'p'
    Pause,      // Space
    Stop,       // 's'
    Next,       // 'n' or Right
    Previous,   // 'N' or Left
    
    // Volume
    VolumeUp,   // '+'
    VolumeDown, // '-'
    Mute,       // 'm'
    
    // Navigation
    Search,     // '/'
    Quit,       // 'q'
    Help,       // '?'
    
    // PLACEHOLDER: More keys for library navigation
    // Filter, Sort, ViewMode, etc.
    
    Char,       // Regular character
    Mouse,      // Mouse event
};

struct MouseEvent {
    enum class Type { Press, Release, Move, Scroll };
    enum class Button { Left, Right, Middle, ScrollUp, ScrollDown };
    
    Type type;
    Button button;
    int x, y;
};

struct InputEvent {
    Key key = Key::None;
    char character = '\0';
    MouseEvent mouse;
    
    bool is_key(Key k) const { return key == k; }
    bool is_mouse() const { return key == Key::Mouse; }
};

class Input {
public:
    Input();
    ~Input();
    
    // Poll for input (non-blocking)
    InputEvent poll();
    
    // Check if input is available
    bool has_input();
    
    // Check if terminal is still valid (not closed)
    bool is_terminal_valid() const { return terminal_valid; }
    
    // Key name for display
    static std::string key_name(Key key);
    
private:
    bool terminal_valid = true;  // Track if terminal is still open
    
    int parse_escape_sequence(const std::string& seq);
    Key map_char_to_key(char c);
    MouseEvent parse_mouse_event(const std::string& seq);
};

} // namespace PlexTUI
