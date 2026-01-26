#pragma once

#include "types.h"
#include <vector>
#include <deque>
#include <mutex>

namespace PlexTUI {

class Terminal;

class Waveform {
public:
    enum class WaveformStyle {
        Line,       // Simple line graph (like btop CPU)
        Bars,       // Vertical bars
        Filled,     // Filled area under curve
        Mirrored,   // Symmetric waveform (top and bottom)
        // PLACEHOLDER: More styles
    };
    
    Waveform(int width, int height);
    
    // Add new audio level data point
    void add_sample(float level); // level: 0.0 to 1.0
    
    // Batch add samples (more efficient - single lock)
    void add_samples_batch(const std::vector<float>& samples);
    
    // Render waveform to terminal
    void draw(Terminal& term, int x, int y, const Theme& theme);
    
    // Configuration
    void set_size(int width, int height);
    void set_style(WaveformStyle style);
    
    // Clear all data
    void clear();
    
private:
    int width;
    int height;
    WaveformStyle style = WaveformStyle::Mirrored;
    std::deque<float> samples;  // Rolling buffer of audio levels
    mutable std::mutex samples_mutex;  // Protect samples from concurrent access
    
    // Drawing helpers
    void draw_line_style(Terminal& term, int x, int y, const Theme& theme);
    void draw_bars_style(Terminal& term, int x, int y, const Theme& theme);
    void draw_filled_style(Terminal& term, int x, int y, const Theme& theme);
    void draw_mirrored_style(Terminal& term, int x, int y, const Theme& theme);
    
    // Get interpolated sample for smooth rendering
    float get_sample_at(float position) const;
    
    // Unicode block characters for smooth graphs (basic)
    static constexpr const char* BLOCKS[] = {
        " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"
    };
    static constexpr int BLOCK_LEVELS = 9;
    
    // Braille patterns for high-resolution graphs (like btop)
    // Each Braille character has 8 dots arranged in 2 columns x 4 rows
    // This gives 256 possible combinations (2^8) for much finer resolution
    // Braille range: U+2800 (empty) to U+28FF (all dots)
    static constexpr const char* BRAILLE_BLOCKS[] = {
        "\u2800",  // Empty (0 dots)
        "\u2801",  // Dot 1
        "\u2802",  // Dot 2
        "\u2803",  // Dots 1+2
        "\u2804",  // Dot 3
        "\u2805",  // Dots 1+3
        "\u2806",  // Dots 2+3
        "\u2807",  // Dots 1+2+3
        "\u2808",  // Dot 4
        "\u2809",  // Dots 1+4
        "\u280A",  // Dots 2+4
        "\u280B",  // Dots 1+2+4
        "\u280C",  // Dots 3+4
        "\u280D",  // Dots 1+3+4
        "\u280E",  // Dots 2+3+4
        "\u280F",  // Dots 1+2+3+4
        "\u2810",  // Dot 5
        "\u2811",  // Dots 1+5
        "\u2812",  // Dots 2+5
        "\u2813",  // Dots 1+2+5
        "\u2814",  // Dots 3+5
        "\u2815",  // Dots 1+3+5
        "\u2816",  // Dots 2+3+5
        "\u2817",  // Dots 1+2+3+5
        "\u2818",  // Dots 4+5
        "\u2819",  // Dots 1+4+5
        "\u281A",  // Dots 2+4+5
        "\u281B",  // Dots 1+2+4+5
        "\u281C",  // Dots 3+4+5
        "\u281D",  // Dots 1+3+4+5
        "\u281E",  // Dots 2+3+4+5
        "\u281F",  // Dots 1+2+3+4+5
        "\u2820",  // Dot 6
        "\u2821",  // Dots 1+6
        "\u2822",  // Dots 2+6
        "\u2823",  // Dots 1+2+6
        "\u2824",  // Dots 3+6
        "\u2825",  // Dots 1+3+6
        "\u2826",  // Dots 2+3+6
        "\u2827",  // Dots 1+2+3+6
        "\u2828",  // Dots 4+6
        "\u2829",  // Dots 1+4+6
        "\u282A",  // Dots 2+4+6
        "\u282B",  // Dots 1+2+4+6
        "\u282C",  // Dots 3+4+6
        "\u282D",  // Dots 1+3+4+6
        "\u282E",  // Dots 2+3+4+6
        "\u282F",  // Dots 1+2+3+4+6
        "\u2830",  // Dots 5+6
        "\u2831",  // Dots 1+5+6
        "\u2832",  // Dots 2+5+6
        "\u2833",  // Dots 1+2+5+6
        "\u2834",  // Dots 3+5+6
        "\u2835",  // Dots 1+3+5+6
        "\u2836",  // Dots 2+3+5+6
        "\u2837",  // Dots 1+2+3+5+6
        "\u2838",  // Dots 4+5+6
        "\u2839",  // Dots 1+4+5+6
        "\u283A",  // Dots 2+4+5+6
        "\u283B",  // Dots 1+2+4+5+6
        "\u283C",  // Dots 3+4+5+6
        "\u283D",  // Dots 1+3+4+5+6
        "\u283E",  // Dots 2+3+4+5+6
        "\u283F",  // Dots 1+2+3+4+5+6
        "\u2840",  // Dot 7
        "\u2841",  // Dots 1+7
        "\u2842",  // Dots 2+7
        "\u2843",  // Dots 1+2+7
        "\u2844",  // Dots 3+7
        "\u2845",  // Dots 1+3+7
        "\u2846",  // Dots 2+3+7
        "\u2847",  // Dots 1+2+3+7
        "\u2848",  // Dots 4+7
        "\u2849",  // Dots 1+4+7
        "\u284A",  // Dots 2+4+7
        "\u284B",  // Dots 1+2+4+7
        "\u284C",  // Dots 3+4+7
        "\u284D",  // Dots 1+3+4+7
        "\u284E",  // Dots 2+3+4+7
        "\u284F",  // Dots 1+2+3+4+7
        "\u2850",  // Dots 5+7
        "\u2851",  // Dots 1+5+7
        "\u2852",  // Dots 2+5+7
        "\u2853",  // Dots 1+2+5+7
        "\u2854",  // Dots 3+5+7
        "\u2855",  // Dots 1+3+5+7
        "\u2856",  // Dots 2+3+5+7
        "\u2857",  // Dots 1+2+3+5+7
        "\u2858",  // Dots 4+5+7
        "\u2859",  // Dots 1+4+5+7
        "\u285A",  // Dots 2+4+5+7
        "\u285B",  // Dots 1+2+4+5+7
        "\u285C",  // Dots 3+4+5+7
        "\u285D",  // Dots 1+3+4+5+7
        "\u285E",  // Dots 2+3+4+5+7
        "\u285F",  // Dots 1+2+3+4+5+7
        "\u2860",  // Dots 6+7
        "\u2861",  // Dots 1+6+7
        "\u2862",  // Dots 2+6+7
        "\u2863",  // Dots 1+2+6+7
        "\u2864",  // Dots 3+6+7
        "\u2865",  // Dots 1+3+6+7
        "\u2866",  // Dots 2+3+6+7
        "\u2867",  // Dots 1+2+3+6+7
        "\u2868",  // Dots 4+6+7
        "\u2869",  // Dots 1+4+6+7
        "\u286A",  // Dots 2+4+6+7
        "\u286B",  // Dots 1+2+4+6+7
        "\u286C",  // Dots 3+4+6+7
        "\u286D",  // Dots 1+3+4+6+7
        "\u286E",  // Dots 2+3+4+6+7
        "\u286F",  // Dots 1+2+3+4+6+7
        "\u2870",  // Dots 5+6+7
        "\u2871",  // Dots 1+5+6+7
        "\u2872",  // Dots 2+5+6+7
        "\u2873",  // Dots 1+2+5+6+7
        "\u2874",  // Dots 3+5+6+7
        "\u2875",  // Dots 1+3+5+6+7
        "\u2876",  // Dots 2+3+5+6+7
        "\u2877",  // Dots 1+2+3+5+6+7
        "\u2878",  // Dots 4+5+6+7
        "\u2879",  // Dots 1+4+5+6+7
        "\u287A",  // Dots 2+4+5+6+7
        "\u287B",  // Dots 1+2+4+5+6+7
        "\u287C",  // Dots 3+4+5+6+7
        "\u287D",  // Dots 1+3+4+5+6+7
        "\u287E",  // Dots 2+3+4+5+6+7
        "\u287F",  // Dots 1+2+3+4+5+6+7
        "\u2880",  // Dot 8
        "\u28FF"   // All 8 dots (full block)
    };
    static constexpr int BRAILLE_LEVELS = 256;  // 2^8 possible combinations
    
    // Helper to get Braille character for a given fill level (0.0 to 1.0)
    static const char* get_braille_char(float fill_level);
};

} // namespace PlexTUI
