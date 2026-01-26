#include "types.h"
#include "terminal.h"
#include "input.h"
#include "plex_client.h"
#include "player_view.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstdlib>
#include <sys/ioctl.h>
#include <execinfo.h>
#include <unistd.h>
#include <cstdlib>  // for system()

using namespace PlexTUI;

// Global flag for signal handling
volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_terminal_resized = 0;

// Global config pointer for logging (set in main)
static Config* g_config = nullptr;

// Logging helper
static void log_to_file(const std::string& message) {
    // Only log if debug logging is enabled
    if (!g_config || !g_config->enable_debug_logging) {
        return;
    }
    
    // Determine log file path
    std::string log_file;
    if (!g_config->debug_log_file_path.empty()) {
        log_file = g_config->debug_log_file_path;
    } else {
        // Default: next to config.ini
        const char* home = getenv("HOME");
        if (home) {
            log_file = std::string(home) + "/.config/plex-tui/debug.log";
        } else {
            log_file = "debug.log";  // Fallback to current directory
        }
    }
    
    std::ofstream log(log_file, std::ios::app);
    if (log.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        char time_str[64];
        std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));
        log << "[" << time_str << "] " << message << std::endl;
    }
    // Also log to stderr
    std::cerr << "[LOG] " << message << std::endl;
}

void crash_handler(int signal) {
    log_to_file("CRASH: Signal " + std::to_string(signal) + " received");
    
    // Print stack trace
    void* array[20];
    size_t size = backtrace(array, 20);
    char** strings = backtrace_symbols(array, size);
    
    log_to_file("Stack trace:");
    for (size_t i = 0; i < size; ++i) {
        log_to_file("  " + std::to_string(i) + ": " + std::string(strings[i]));
    }
    free(strings);
    
    if (signal == SIGWINCH) {
        // Terminal window size changed - mark for resize handling
        g_terminal_resized = 1;
    } else {
        // Other signals (SIGINT, SIGTERM, SIGHUP, SIGBUS, SIGSEGV) - exit
        g_running = 0;
    }
}

void signal_handler(int signal) {
    if (signal == SIGWINCH) {
        // Terminal window size changed - mark for resize handling
        g_terminal_resized = 1;
    } else {
        // Other signals (SIGINT, SIGTERM, SIGHUP) - exit
        g_running = 0;
    }
}

void print_usage(const char* program_name) {
    std::cout << "Plex TUI - Terminal User Interface for Plex\n\n";
    std::cout << "Usage: " << program_name << " [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -c, --config <path>    Path to configuration file\n";
    std::cout << "  -s, --server <url>     Plex server URL\n";
    std::cout << "  -t, --token <token>    Plex authentication token\n";
    std::cout << "  -h, --help             Show this help message\n\n";
    std::cout << "Example:\n";
    std::cout << "  " << program_name << " --server http://localhost:32400 --token YOUR_TOKEN\n\n";
}

int main(int argc, char* argv[]) {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);  // Terminal closed
    signal(SIGWINCH, signal_handler);  // Terminal window size changed (btop-style)
    signal(SIGBUS, crash_handler);  // Bus error - log and exit
    signal(SIGSEGV, crash_handler);  // Segmentation fault - log and exit
    signal(SIGABRT, crash_handler);  // Abort - log and exit
    
    // Parse command line arguments
    Config config;
    std::string config_file;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_file = argv[++i];
        } else if ((arg == "-s" || arg == "--server") && i + 1 < argc) {
            config.plex_server_url = argv[++i];
        } else if ((arg == "-t" || arg == "--token") && i + 1 < argc) {
            config.plex_token = argv[++i];
        }
    }
    
    // Determine config file path
    std::string config_path;
    if (!config_file.empty()) {
        config_path = config_file;
    } else {
        // Try default config locations
        const char* home = getenv("HOME");
        if (home) {
            config_path = std::string(home) + "/.config/plex-tui/config.ini";
        }
    }
    
    // Check if config file exists
    bool config_exists = false;
    if (!config_path.empty()) {
        std::ifstream test_file(config_path);
        config_exists = test_file.good();
        test_file.close();
    }
    
    bool open_options_on_start = false;
    
    // If config doesn't exist, create default one
    if (!config_exists && !config_path.empty()) {
        // Create directory if needed
        const char* home = getenv("HOME");
        if (home) {
            std::string config_dir = std::string(home) + "/.config/plex-tui";
            // Create directory (simple approach - try to create, ignore if exists)
            std::string mkdir_cmd = "mkdir -p \"" + config_dir + "\" 2>/dev/null";
            system(mkdir_cmd.c_str());
        }
        
        // Create default config with features disabled
        config.enable_waveform = false;
        config.enable_lyrics = false;
        config.enable_album_art = false;
        config.enable_album_data = false;
        config.enable_debug_logging = false;
        
        // Save default config
        if (config.save_to_file(config_path)) {
            open_options_on_start = true;  // Open options menu on first run
        }
    } else if (!config_path.empty()) {
        // Load existing config
        config.load_from_file(config_path);
    }
    
    // Validate configuration (only if config exists and is invalid)
    if (config_exists && (config.plex_server_url.empty() || config.plex_token.empty())) {
        std::cerr << "Error: Plex server URL and token are required.\n";
        std::cerr << "Use --server and --token arguments, or edit the config file.\n\n";
        print_usage(argv[0]);
        return 1;
    }
    
    // Set global config pointer for logging (after config is loaded)
    g_config = &config;
    
    // Set debug log file path for PlexClient lyrics logging
    if (!config.debug_log_file_path.empty()) {
        PlexClient::set_debug_log_file_path(config.debug_log_file_path);
    } else {
        // Use default path (next to config.ini)
        const char* home = getenv("HOME");
        if (home) {
            PlexClient::set_debug_log_file_path(std::string(home) + "/.config/plex-tui/debug.log");
        }
    }
    
    if (g_config->enable_debug_logging) {
        log_to_file("Application starting");
    }
    
    // Must run in an interactive terminal (TTY)
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::cerr << "Error: plex-tui must be run in an interactive terminal.\n";
        std::cerr << "Run it directly from a terminal (e.g. Terminal.app, iTerm), not from a script or IDE.\n";
        return 1;
    }

    // Initialize components
    Terminal terminal;
    if (!terminal.init()) {
        std::cerr << "Error: Failed to initialize terminal\n";
        return 1;
    }
    
    // Set window size from config (if specified)
    if (config.window_width > 0 && config.window_height > 0) {
        terminal.set_window_size(config.window_width, config.window_height);
    }
    
    PlexClient* client = nullptr;
    // Only try to connect if we have server URL and token (not first run)
    if (!config.plex_server_url.empty() && !config.plex_token.empty()) {
        try {
            client = new PlexClient(config.plex_server_url, config.plex_token, config.enable_debug_logging);
            if (!client->connect()) {
                terminal.restore();
                delete client;
                std::cerr << "Error: Failed to connect to Plex server at " 
                          << config.plex_server_url << "\n";
                std::cerr << "Check your server URL and authentication token.\n";
                return 1;
            }
        } catch (...) {
            terminal.restore();
            if (client) delete client;
            std::cerr << "Error: Failed to create Plex client\n";
            return 1;
        }
    } else {
        // First run - create dummy client (won't be used until config is set)
        // We'll open options menu immediately
        try {
            client = new PlexClient("", "", false);
        } catch (...) {
            terminal.restore();
            std::cerr << "Error: Failed to create Plex client\n";
            return 1;
        }
    }
    
    Input input;
    
    // Initialize PlayerView - this might crash
    PlayerView* player_view = nullptr;
    try {
        player_view = new PlayerView(terminal, *client, config);
        
        // Open options menu on first run (if config was just created)
        if (open_options_on_start) {
            player_view->open_options_menu();
        }
    } catch (...) {
        terminal.restore();
        if (client) {
            // Client destructor will automatically stop audio decoder
            delete client;
        }
        std::cerr << "Error: Failed to create player view\n";
        return 1;
    }
    
    // Main loop
    const auto frame_duration = std::chrono::milliseconds(config.refresh_rate_ms);
    
    while (g_running) {
        auto frame_start = std::chrono::steady_clock::now();
        
        // Check if terminal is still valid (not closed)
        if (!input.is_terminal_valid()) {
            g_running = 0;
            break;
        }
        
        // Handle input
        while (input.has_input()) {
            InputEvent event = input.poll();
            
            // Handle quit or terminal closed
            if (event.is_key(Key::Quit) || !input.is_terminal_valid()) {
                g_running = 0;
                break;
            }
            
            if (player_view && client) {
                player_view->handle_input(event);
            }
        }
        
        // Update state
        if (player_view && client) {
            try {
                player_view->update();
            } catch (const std::exception& e) {
                if (g_config && g_config->enable_debug_logging) {
                    log_to_file("Exception in update(): " + std::string(e.what()));
                }
            } catch (...) {
                if (g_config && g_config->enable_debug_logging) {
                    log_to_file("Unknown exception in update()");
                }
            }
        }
        
        // Check for terminal resize (btop-style: handle SIGWINCH and poll)
        static int last_width = 0, last_height = 0;
        terminal.update_size();
        int current_width = terminal.width();
        int current_height = terminal.height();
        
        // btop-style: Detect resize (either via SIGWINCH or polling)
        bool terminal_resized = false;
        if (g_terminal_resized) {
            g_terminal_resized = 0;
            terminal_resized = true;
        }
        if (current_width != last_width || current_height != last_height) {
            terminal_resized = true;
        }
        
        if (terminal_resized) {
            // Terminal resized - force full redraw (btop-style: clear and redraw everything)
            last_width = current_width;
            last_height = current_height;
            if (player_view) {
                player_view->force_redraw();  // Force full redraw
            }
        }
        
        // Render
        if (player_view && client) {
            try {
                player_view->draw();
            } catch (const std::exception& e) {
                if (g_config && g_config->enable_debug_logging) {
                    log_to_file("Exception in draw(): " + std::string(e.what()));
                }
            } catch (...) {
                if (g_config && g_config->enable_debug_logging) {
                    log_to_file("Unknown exception in draw()");
                }
            }
        }
        
        // Frame rate limiting - always sleep to prevent CPU spinning
        auto frame_end = std::chrono::steady_clock::now();
        auto frame_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            frame_end - frame_start
        );
        
        if (frame_time < frame_duration) {
            std::this_thread::sleep_for(frame_duration - frame_time);
        } else {
            // Frame took longer than expected - still sleep a bit to prevent 100% CPU
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    // Cleanup - ensure all resources are freed in correct order
    // Stop audio playback before destroying player_view
    if (player_view) {
        // PlayerView destructor will stop audio decoder
        delete player_view;
        player_view = nullptr;
    }
    
    // Give processes a moment to clean up
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Client destructor will automatically stop audio decoder and subprocesses
    if (client) {
        delete client;
        client = nullptr;
    }
    
    // Give a bit more time for subprocess cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    terminal.restore();
    
    std::cout << "\nThank you for using Plex TUI!\n";
    
    return 0;
}
