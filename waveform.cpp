#include "waveform.h"
#include "terminal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace PlexTUI {

Waveform::Waveform(int width, int height) 
    : width(width), height(height) {
    samples.resize(width, 0.0f);
}

void Waveform::add_sample(float level) {
    // Clamp level to valid range
    level = std::clamp(level, 0.0f, 1.0f);
    
    // Lock mutex to protect samples from concurrent access
    std::lock_guard<std::mutex> lock(samples_mutex);
    
    // Add to rolling buffer
    samples.push_back(level);
    
    // Keep more samples than width for higher resolution (like btop)
    // Use 2x width for smoother, higher-res waveform
    size_t max_samples = static_cast<size_t>(width) * 2;
    while (samples.size() > max_samples) {
        samples.pop_front();
    }
}

void Waveform::add_samples_batch(const std::vector<float>& new_samples) {
    if (new_samples.empty()) return;
    
    // Lock mutex once for the entire batch
    std::lock_guard<std::mutex> lock(samples_mutex);
    
    // Add all samples at once
    for (float level : new_samples) {
        // Clamp level to valid range
        level = std::clamp(level, 0.0f, 1.0f);
        samples.push_back(level);
    }
    
    // Keep more samples than width for higher resolution (like btop)
    // Use 2x width for smoother, higher-res waveform
    size_t max_samples = static_cast<size_t>(width) * 2;
    while (samples.size() > max_samples) {
        samples.pop_front();
    }
}

void Waveform::set_size(int w, int h) {
    width = w;
    height = h;
    
    // Lock mutex to protect samples from concurrent access
    std::lock_guard<std::mutex> lock(samples_mutex);
    
    // Resize sample buffer
    while (samples.size() > static_cast<size_t>(width)) {
        samples.pop_front();
    }
}

void Waveform::set_style(WaveformStyle s) {
    style = s;
}

void Waveform::clear() {
    // Lock mutex to protect samples from concurrent access
    std::lock_guard<std::mutex> lock(samples_mutex);
    
    samples.clear();
    samples.resize(width, 0.0f);
}

void Waveform::draw(Terminal& term, int x, int y, const Theme& theme) {
    switch (style) {
        case WaveformStyle::Line:
            draw_line_style(term, x, y, theme);
            break;
        case WaveformStyle::Bars:
            draw_bars_style(term, x, y, theme);
            break;
        case WaveformStyle::Filled:
            draw_filled_style(term, x, y, theme);
            break;
        case WaveformStyle::Mirrored:
            draw_mirrored_style(term, x, y, theme);
            break;
    }
}

void Waveform::draw_mirrored_style(Terminal& term, int x, int y, const Theme& theme) {
    // btop-style high-resolution rendering using Braille characters
    // Each Braille character has 8 dots arranged in 2 columns x 4 rows
    // This gives 256 possible patterns (2^8) for much finer vertical resolution
    
    // Lock mutex and make a copy of samples to avoid holding lock during drawing
    std::deque<float> samples_copy;
    {
        std::lock_guard<std::mutex> lock(samples_mutex);
        samples_copy = samples;
    }
    
    // Calculate the actual vertical resolution (in dots, since Braille has 4 dots per character)
    // Each Braille character represents 4 vertical dot positions, but is drawn on 1 screen line
    // For height=9, we need 9 Braille character rows (one per screen line)
    int char_rows = height;  // One Braille character per screen line
    int total_dots = char_rows * 4;  // Total vertical resolution in dots (4 dots per character)
    
    int mid_y = total_dots / 2;  // Center in dot space
    std::string black_bg = term.bg_color(0, 0, 0);
    
    // Use full height - remove 75% limit to allow waveform to use all 9 lines
    float max_bar_height = static_cast<float>(mid_y);  // Full height from center (100% of available space)
    
    // Use more samples than width for higher resolution (like btop)
    // Interpolate between samples for smooth rendering
    for (int col = 0; col < width; ++col) {
        // Map column to sample index (with interpolation for higher res)
        float sample_pos = (static_cast<float>(col) / width) * (samples_copy.size() - 1);
        int sample_idx = static_cast<int>(sample_pos);
        float t = sample_pos - sample_idx;
        
        float level = 0.0f;
        if (sample_idx >= 0 && sample_idx < static_cast<int>(samples_copy.size())) {
            if (sample_idx + 1 < static_cast<int>(samples_copy.size())) {
                // Linear interpolation for smooth waveform
                level = samples_copy[sample_idx] * (1.0f - t) + samples_copy[sample_idx + 1] * t;
            } else {
                level = samples_copy[sample_idx];
            }
        }
        
        // Vibrant btop-style gradient: cyan -> magenta -> yellow based on level
        uint8_t r, g, b;
        if (level < 0.33f) {
            // Cyan to Magenta
            float t_grad = level / 0.33f;
            r = static_cast<uint8_t>(theme.waveform_primary.r + 
                (theme.waveform_secondary.r - theme.waveform_primary.r) * t_grad);
            g = static_cast<uint8_t>(theme.waveform_primary.g + 
                (theme.waveform_secondary.g - theme.waveform_primary.g) * t_grad);
            b = static_cast<uint8_t>(theme.waveform_primary.b + 
                (theme.waveform_secondary.b - theme.waveform_primary.b) * t_grad);
        } else if (level < 0.66f) {
            // Magenta to Yellow
            float t_grad = (level - 0.33f) / 0.33f;
            r = static_cast<uint8_t>(theme.waveform_secondary.r + 
                (theme.waveform_tertiary.r - theme.waveform_secondary.r) * t_grad);
            g = static_cast<uint8_t>(theme.waveform_secondary.g + 
                (theme.waveform_tertiary.g - theme.waveform_secondary.g) * t_grad);
            b = static_cast<uint8_t>(theme.waveform_secondary.b + 
                (theme.waveform_tertiary.b - theme.waveform_secondary.b) * t_grad);
        } else {
            // Yellow to bright yellow/white
            float t_grad = (level - 0.66f) / 0.34f;
            r = static_cast<uint8_t>(theme.waveform_tertiary.r + (255 - theme.waveform_tertiary.r) * t_grad);
            g = static_cast<uint8_t>(theme.waveform_tertiary.g + (255 - theme.waveform_tertiary.g) * t_grad);
            b = static_cast<uint8_t>(theme.waveform_tertiary.b + (255 - theme.waveform_tertiary.b) * t_grad);
        }
        
        std::string color = term.fg_color(r, g, b);
        
        // btop-style: Render using Braille characters for high resolution
        // Each character cell represents 4 vertical positions (8 dots: 2 cols x 4 rows)
        // Calculate how many character rows we need
        // char_rows already calculated above
        
        // Calculate the actual bar height for this column (mirrored from center)
        // level is 0.0 to 1.0, convert to actual rows from center
        // Limit to 3/4 of height from center
        float bar_height_float = level * max_bar_height;
        
        for (int char_row = 0; char_row < char_rows; ++char_row) {
            // Calculate actual Y position for this character row on screen
            // Each Braille character represents 4 vertical positions
            int draw_y = y + char_row;
            // Clip to screen bounds (y can be negative if waveform extends above screen)
            if (draw_y < 0) continue;  // Skip if above screen
            
            // Calculate which dots should be filled in this Braille character
            // Braille dots layout (ISO/TR 11548-1):
            // Left column: dots 1,2,3,7 (top to bottom)
            // Right column: dots 4,5,6,8 (top to bottom)
            // Bit encoding: dot1=0x01, dot2=0x02, dot3=0x04, dot4=0x08,
            //               dot5=0x10, dot6=0x20, dot7=0x40, dot8=0x80
            uint8_t braille_pattern = 0;
            
            // Each Braille character covers 4 vertical positions
            // Check each of the 4 rows in this character cell
            for (int dot_row = 0; dot_row < 4; ++dot_row) {
                // Calculate absolute row position within the waveform (0 to total_dots-1)
                int absolute_row = char_row * 4 + dot_row;
                if (absolute_row >= total_dots) break;
                
                // Calculate distance from center (for mirrored waveform)
                // mid_y is the center row of the waveform (height/2)
                float dist_from_center = std::abs(static_cast<float>(absolute_row) - mid_y);
                
                // Check if this row should be filled (within the bar height from center)
                // Use <= to include the edge for thicker appearance
                bool should_fill = (dist_from_center <= bar_height_float);
                
                if (should_fill) {
                    // Set both left and right column dots for this row
                    // Left column: dots 1,2,3,7
                    if (dot_row == 0) braille_pattern |= 0x01;  // dot1
                    else if (dot_row == 1) braille_pattern |= 0x02;  // dot2
                    else if (dot_row == 2) braille_pattern |= 0x04;  // dot3
                    else if (dot_row == 3) braille_pattern |= 0x40;  // dot7
                    
                    // Right column: dots 4,5,6,8
                    if (dot_row == 0) braille_pattern |= 0x08;  // dot4
                    else if (dot_row == 1) braille_pattern |= 0x10;  // dot5
                    else if (dot_row == 2) braille_pattern |= 0x20;  // dot6
                    else if (dot_row == 3) braille_pattern |= 0x80;  // dot8
                }
            }
            
            // Only draw if there are dots to show
            if (braille_pattern > 0) {
                // Convert braille pattern to Unicode character (U+2800 + pattern)
                uint32_t braille_code = 0x2800 + braille_pattern;
                
                // UTF-8 encode the Braille character
                char braille_utf8[4];
                if (braille_code < 0x80) {
                    braille_utf8[0] = static_cast<char>(braille_code);
                    braille_utf8[1] = '\0';
                } else if (braille_code < 0x800) {
                    braille_utf8[0] = static_cast<char>(0xC0 | (braille_code >> 6));
                    braille_utf8[1] = static_cast<char>(0x80 | (braille_code & 0x3F));
                    braille_utf8[2] = '\0';
                } else {
                    braille_utf8[0] = static_cast<char>(0xE0 | (braille_code >> 12));
                    braille_utf8[1] = static_cast<char>(0x80 | ((braille_code >> 6) & 0x3F));
                    braille_utf8[2] = static_cast<char>(0x80 | (braille_code & 0x3F));
                    braille_utf8[3] = '\0';
                }
                
                term.draw_text(x + col, draw_y, black_bg + color + std::string(braille_utf8) + term.reset_color());
            }
        }
    }
}

void Waveform::draw_line_style(Terminal& term, int x, int y, const Theme& theme) {
    // Lock mutex and make a copy of samples to avoid holding lock during drawing
    std::deque<float> samples_copy;
    {
        std::lock_guard<std::mutex> lock(samples_mutex);
        samples_copy = samples;
    }
    
    std::string color = term.fg_color(theme.waveform_primary.r, 
                                     theme.waveform_primary.g, 
                                     theme.waveform_primary.b);
    
    for (int col = 0; col < width && col < static_cast<int>(samples_copy.size()); ++col) {
        float level = samples_copy[col];
        int draw_y = y + height - 1 - static_cast<int>(level * (height - 1));
        
        if (draw_y >= y && draw_y < y + height) {
            term.draw_text(x + col, draw_y, color + "●" + term.reset_color());
        }
    }
}

void Waveform::draw_bars_style(Terminal& term, int x, int y, const Theme& theme) {
    // Lock mutex and make a copy of samples to avoid holding lock during drawing
    std::deque<float> samples_copy;
    {
        std::lock_guard<std::mutex> lock(samples_mutex);
        samples_copy = samples;
    }
    
    for (int col = 0; col < width && col < static_cast<int>(samples_copy.size()); ++col) {
        float level = samples_copy[col];
        int bar_height = static_cast<int>(level * height);
        
        std::string color = term.fg_color(theme.waveform_primary.r, 
                                         theme.waveform_primary.g, 
                                         theme.waveform_primary.b);
        
        for (int row = 0; row < bar_height; ++row) {
            int draw_y = y + height - row - 1;
            if (draw_y >= y && draw_y < y + height) {
                term.draw_text(x + col, draw_y, color + "█" + term.reset_color());
            }
        }
    }
}

void Waveform::draw_filled_style(Terminal& term, int x, int y, const Theme& theme) {
    // Lock mutex and make a copy of samples to avoid holding lock during drawing
    std::deque<float> samples_copy;
    {
        std::lock_guard<std::mutex> lock(samples_mutex);
        samples_copy = samples;
    }
    
    // Similar to bars but with gradient
    for (int col = 0; col < width && col < static_cast<int>(samples_copy.size()); ++col) {
        float level = samples_copy[col];
        int bar_height = static_cast<int>(level * height);
        
        for (int row = 0; row < bar_height; ++row) {
            float intensity = 1.0f - (static_cast<float>(row) / bar_height);
            uint8_t r = static_cast<uint8_t>(theme.waveform_primary.r * intensity);
            uint8_t g = static_cast<uint8_t>(theme.waveform_primary.g * intensity);
            uint8_t b = static_cast<uint8_t>(theme.waveform_primary.b * intensity);
            
            std::string color = term.fg_color(r, g, b);
            int draw_y = y + height - row - 1;
            
            if (draw_y >= y && draw_y < y + height) {
                term.draw_text(x + col, draw_y, color + "▓" + term.reset_color());
            }
        }
    }
}

float Waveform::get_sample_at(float position) const {
    // Lock mutex to protect samples from concurrent access
    std::lock_guard<std::mutex> lock(samples_mutex);
    
    if (samples.empty()) return 0.0f;
    
    int idx = static_cast<int>(position);
    if (idx < 0 || idx >= static_cast<int>(samples.size())) return 0.0f;
    
    return samples[idx];
}

} // namespace PlexTUI
