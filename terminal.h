#pragma once

#include <string>
#include <cstdint>
#include <termios.h>
#include <sys/ioctl.h>

namespace PlexTUI {

class Terminal {
public:
    Terminal();
    ~Terminal();
    
    // Terminal setup/cleanup
    bool init();
    void restore();
    
    // Screen management
    void clear();  // Mark for full clear (only clears on next flush if needed)
    void move_cursor(int x, int y);
    void hide_cursor();
    void show_cursor();
    void flush();  // Flush buffered output (with smart diff rendering)
    
    // Terminal properties
    int width() const { return term_width; }
    int height() const { return term_height; }
    bool update_size();
    bool set_window_size(int width, int height);  // Set terminal window size
    
    // Color output (24-bit true color)
    std::string fg_color(uint8_t r, uint8_t g, uint8_t b);
    std::string bg_color(uint8_t r, uint8_t g, uint8_t b);
    std::string reset_color();
    
    // Drawing primitives
    void draw_box(int x, int y, int w, int h, const std::string& title = "");
    void draw_text(int x, int y, const std::string& text);
    void draw_horizontal_line(int x, int y, int length, const std::string& c = "─");
    void draw_vertical_line(int x, int y, int length, const std::string& c = "│");
    
    // Mouse support
    void enable_mouse();
    void disable_mouse();
    
private:
    int term_width = 0;
    int term_height = 0;
    struct termios original_termios;
    bool initialized = false;
    std::string output_buffer;
    
    void enable_raw_mode();
    void disable_raw_mode();
};

} // namespace PlexTUI
