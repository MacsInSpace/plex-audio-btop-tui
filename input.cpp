#include "input.h"
#include <unistd.h>
#include <sys/select.h>
#include <cstring>
#include <cerrno>
#include <unistd.h>

namespace PlexTUI {

Input::Input() = default;
Input::~Input() = default;

bool Input::has_input() {
    if (!terminal_valid) return false;
    
    // Check if terminal is still valid
    if (!isatty(STDIN_FILENO)) {
        terminal_valid = false;
        return false;
    }
    
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    
    struct timeval timeout = {0, 0};
    int result = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout);
    if (result < 0) {
        // Error - terminal likely closed
        terminal_valid = false;
        return false;
    }
    return result > 0;
}

InputEvent Input::poll() {
    InputEvent event;
    
    if (!has_input()) {
        return event;
    }
    
    char buf[32];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
    if (n <= 0) {
        // EOF or error - terminal likely closed
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            terminal_valid = false;
        }
        return event;
    }
    buf[n] = '\0';
    
    // Check for escape sequences
    if (buf[0] == '\033') {
        if (n == 1) {
            event.key = Key::Escape;
            return event;
        }
        
        std::string seq(buf + 1, n - 1);
        
        // Mouse events
        if (seq[0] == '[' && seq[1] == '<') {
            event.key = Key::Mouse;
            event.mouse = parse_mouse_event(seq);
            return event;
        }
        
        // Arrow keys and special keys
        if (seq == "[A") event.key = Key::Up;
        else if (seq == "[B") event.key = Key::Down;
        else if (seq == "[C") event.key = Key::Right;
        else if (seq == "[D") event.key = Key::Left;
        else if (seq == "[5~") event.key = Key::PageUp;
        else if (seq == "[6~") event.key = Key::PageDown;
        else if (seq == "[H" || seq == "[1~") event.key = Key::Home;
        else if (seq == "[F" || seq == "[4~") event.key = Key::End;
        
        return event;
    }
    
    // Regular character
    char c = buf[0];
    
    // Map to keys
    event.key = map_char_to_key(c);
    if (event.key == Key::Char) {
        event.character = c;
    }
    
    return event;
}

Key Input::map_char_to_key(char c) {
    switch (c) {
        case '\r':
        case '\n': return Key::Enter;
        case ' ': return Key::Pause;
        case '\t': return Key::Tab;
        case 127:
        case '\b': return Key::Backspace;
        
        // Playback
        case 'p': return Key::Play;
        case 's': return Key::Stop;
        case 'n': return Key::Next;
        case 'N': return Key::Previous;
        
        // Volume
        case '+':
        case '=': return Key::VolumeUp;
        case '-':
        case '_': return Key::VolumeDown;
        case 'm': return Key::Mute;
        
        // Navigation
        case '/': return Key::Search;
        case 'q': return Key::Quit;
        case '?': return Key::Help;
        
        default: return Key::Char;
    }
}

MouseEvent Input::parse_mouse_event(const std::string& seq) {
    MouseEvent event;
    
    // Parse SGR mouse format: [<B;X;YM or [<B;X;Ym
    size_t pos = 2;  // Skip "[<"
    
    int button = 0;
    int x = 0, y = 0;
    char action = 'M';
    
    sscanf(seq.c_str() + pos, "%d;%d;%d%c", &button, &x, &y, &action);
    
    event.x = x - 1;  // Convert to 0-based
    event.y = y - 1;
    
    // Parse button
    if (button == 0) event.button = MouseEvent::Button::Left;
    else if (button == 1) event.button = MouseEvent::Button::Middle;
    else if (button == 2) event.button = MouseEvent::Button::Right;
    else if (button == 64) event.button = MouseEvent::Button::ScrollUp;
    else if (button == 65) event.button = MouseEvent::Button::ScrollDown;
    
    // Parse action
    if (action == 'M') {
        event.type = MouseEvent::Type::Press;
    } else if (action == 'm') {
        event.type = MouseEvent::Type::Release;
    } else if (button >= 32) {
        event.type = MouseEvent::Type::Move;
    }
    
    if (button == 64 || button == 65) {
        event.type = MouseEvent::Type::Scroll;
    }
    
    return event;
}

std::string Input::key_name(Key key) {
    switch (key) {
        case Key::Play: return "Play (p)";
        case Key::Pause: return "Pause (space)";
        case Key::Stop: return "Stop (s)";
        case Key::Next: return "Next (n)";
        case Key::Previous: return "Previous (N)";
        case Key::VolumeUp: return "Vol+ (+)";
        case Key::VolumeDown: return "Vol- (-)";
        case Key::Mute: return "Mute (m)";
        case Key::Search: return "Search (/)";
        case Key::Quit: return "Quit (q)";
        case Key::Help: return "Help (?)";
        default: return "";
    }
}

} // namespace PlexTUI
