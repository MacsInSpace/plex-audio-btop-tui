#include "terminal.h"
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>

namespace PlexTUI {

Terminal::Terminal() = default;

Terminal::~Terminal() {
    if (initialized) {
        restore();
    }
}

bool Terminal::init() {
    if (initialized) return true;
    
    // Enable raw mode
    enable_raw_mode();
    
    // Get terminal size
    update_size();
    
    // Setup terminal
    std::cout << "\033[?1049h";  // Alternative screen buffer
    std::cout << "\033[?25l";     // Hide cursor
    std::cout << "\033[2J";       // Clear screen
    std::cout.flush();
    
    enable_mouse();
    
    initialized = true;
    return true;
}

void Terminal::restore() {
    if (!initialized) return;
    
    disable_mouse();
    show_cursor();
    std::cout << "\033[?1049l";  // Normal screen buffer
    std::cout.flush();
    
    disable_raw_mode();
    initialized = false;
}

void Terminal::enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &original_termios);
    
    struct termios raw = original_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void Terminal::disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
}

void Terminal::clear() {
    // Clear screen and move to top-left
    output_buffer += "\033[2J\033[H";
}

void Terminal::move_cursor(int x, int y) {
    // Bounds check to prevent invalid coordinates (btop-style: reasonable limits)
    if (x < 0 || y < 0 || x >= 1000 || y >= 1000) {
        return;  // Skip invalid coordinates
    }
    output_buffer += "\033[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
}

void Terminal::hide_cursor() {
    output_buffer += "\033[?25l";
}

void Terminal::show_cursor() {
    output_buffer += "\033[?25h";
}

void Terminal::flush() {
    // Output buffered changes
    if (!output_buffer.empty()) {
        std::cout << output_buffer;
        output_buffer.clear();
    }
    
    std::cout.flush();
}

bool Terminal::update_size() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        // Fallback to defaults if ioctl fails
        term_width = 80;
        term_height = 24;
        return false;
    }
    term_width = ws.ws_col > 0 ? ws.ws_col : 80;
    term_height = ws.ws_row > 0 ? ws.ws_row : 24;
    return true;
}

bool Terminal::set_window_size(int width, int height) {
    // Use ANSI escape sequence to set window size
    // Format: \033[8;height;widtht
    // This is supported by most modern terminals (Terminal.app, iTerm2, etc.)
    std::cout << "\033[8;" << height << ";" << width << "t";
    std::cout.flush();
    
    // Give the terminal a moment to resize
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Update our internal size tracking
    return update_size();
}

std::string Terminal::fg_color(uint8_t r, uint8_t g, uint8_t b) {
    return "\033[38;2;" + std::to_string(r) + ";" + 
           std::to_string(g) + ";" + std::to_string(b) + "m";
}

std::string Terminal::bg_color(uint8_t r, uint8_t g, uint8_t b) {
    return "\033[48;2;" + std::to_string(r) + ";" + 
           std::to_string(g) + ";" + std::to_string(b) + "m";
}

std::string Terminal::reset_color() {
    return "\033[0m";
}

void Terminal::draw_box(int x, int y, int w, int h, const std::string& title) {
    if (w < 2 || h < 2) return;
    
    // Fill interior with black background (btop style)
    std::string black_bg = bg_color(0, 0, 0);
    for (int row = 1; row < h - 1; ++row) {
        move_cursor(x + 1, y + row);
        std::string fill(w - 2, ' ');
        output_buffer += black_bg + fill + reset_color();
    }
    
    // Top border
    move_cursor(x, y);
    output_buffer += "╭";
    if (!title.empty() && title.length() + 4 < static_cast<size_t>(w)) {
        output_buffer += "─ " + title + " ";
        for (int i = title.length() + 4; i < w - 1; ++i) {
            output_buffer += "─";
        }
    } else {
        for (int i = 1; i < w - 1; ++i) {
            output_buffer += "─";
        }
    }
    output_buffer += "╮";
    
    // Sides
    for (int row = 1; row < h - 1; ++row) {
        move_cursor(x, y + row);
        output_buffer += "│";
        move_cursor(x + w - 1, y + row);
        output_buffer += "│";
    }
    
    // Bottom border
    move_cursor(x, y + h - 1);
    output_buffer += "╰";
    for (int i = 1; i < w - 1; ++i) {
        output_buffer += "─";
    }
    output_buffer += "╯";
}

void Terminal::draw_text(int x, int y, const std::string& text) {
    // Bounds check to prevent invalid coordinates (btop-style: reasonable limits)
    if (x < 0 || y < 0 || x >= 1000 || y >= 1000) {
        return;  // Skip invalid coordinates
    }
    // Just move cursor and draw - don't clear line (causes flicker)
    // The background fill handles clearing
    move_cursor(x, y);
    output_buffer += text;
}

void Terminal::draw_horizontal_line(int x, int y, int length, const std::string& c) {
    move_cursor(x, y);
    for (int i = 0; i < length; ++i) {
        output_buffer += c;
    }
}

void Terminal::draw_vertical_line(int x, int y, int length, const std::string& c) {
    for (int i = 0; i < length; ++i) {
        move_cursor(x, y + i);
        output_buffer += c;
    }
}

void Terminal::enable_mouse() {
    // Flush mouse enable immediately (btop style)
    std::cout << "\033[?1000h";  // Normal mouse tracking
    std::cout << "\033[?1002h";  // Button event tracking
    std::cout << "\033[?1015h";  // Extended coordinates
    std::cout << "\033[?1006h";  // SGR extended mode
    std::cout.flush();
}

void Terminal::disable_mouse() {
    // Flush mouse disable immediately
    std::cout << "\033[?1006l";
    std::cout << "\033[?1015l";
    std::cout << "\033[?1002l";
    std::cout << "\033[?1000l";
    std::cout.flush();
}

} // namespace PlexTUI
