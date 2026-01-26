#include "types.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>

namespace PlexTUI {

bool Config::load_from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    
    // Simple INI-style parser
    std::string line;
    std::string section;
    
    while (std::getline(file, line)) {
        // Remove whitespace
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        
        // Section header
        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1, line.length() - 2);
            continue;
        }
        
        // Key-value pair
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        
        // Trim
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        
        // Parse based on section
        if (section == "plex") {
            if (key == "server_url") plex_server_url = value;
            else if (key == "token") plex_token = value;
        } else if (section == "display") {
            if (key == "max_waveform_points") max_waveform_points = std::stoi(value);
            else if (key == "refresh_rate_ms") refresh_rate_ms = std::stoi(value);
            else if (key == "window_width") window_width = std::stoi(value);
            else if (key == "window_height") window_height = std::stoi(value);
        } else if (section == "features") {
            // Parse boolean values (true/false, 1/0, yes/no, on/off)
            bool bool_value = false;
            std::string lower_value = value;
            std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(), ::tolower);
            if (lower_value == "true" || lower_value == "1" || lower_value == "yes" || lower_value == "on") {
                bool_value = true;
            }
            
            if (key == "enable_waveform") enable_waveform = bool_value;
            else if (key == "enable_lyrics") enable_lyrics = bool_value;
            else if (key == "enable_album_art") enable_album_art = bool_value;
            else if (key == "enable_album_data") enable_album_data = bool_value;
            else if (key == "enable_debug_logging") enable_debug_logging = bool_value;
            else if (key == "debug_log_file_path") debug_log_file_path = value;
        }
        // PLACEHOLDER: Parse theme colors, keybindings, etc.
    }
    
    return true;
}

bool Config::save_to_file(const std::string& path) {
    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }
    
    file << "# Plex TUI Configuration\n\n";
    
    file << "[plex]\n";
    file << "server_url = " << plex_server_url << "\n";
    file << "token = " << plex_token << "\n\n";
    
    file << "[display]\n";
    file << "max_waveform_points = " << max_waveform_points << "\n";
    file << "refresh_rate_ms = " << refresh_rate_ms << "\n";
    file << "window_width = " << window_width << "\n";
    file << "window_height = " << window_height << "\n\n";
    
    file << "[features]\n";
    file << "# Enable/disable features\n";
    file << "enable_waveform = " << (enable_waveform ? "true" : "false") << "\n";
    file << "enable_lyrics = " << (enable_lyrics ? "true" : "false") << "\n";
    file << "enable_album_art = " << (enable_album_art ? "true" : "false") << "\n";
    file << "enable_album_data = " << (enable_album_data ? "true" : "false") << "\n";
    file << "enable_debug_logging = " << (enable_debug_logging ? "true" : "false") << "\n";
    if (!debug_log_file_path.empty()) {
        file << "debug_log_file_path = " << debug_log_file_path << "\n";
    }
    file << "\n";
    
    // PLACEHOLDER: Save theme, keybindings, etc.
    
    return true;
}

} // namespace PlexTUI
