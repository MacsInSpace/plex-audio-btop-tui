#include "audio_decoder.h"
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <chrono>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#ifdef __APPLE__
#include <unistd.h>
#include <sys/wait.h>
#elif __linux__
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace PlexTUI {

AudioDecoder::AudioDecoder() {
    waveform_samples.reserve(MAX_SAMPLES);
}

AudioDecoder::~AudioDecoder() {
    stop_decoding();
}

bool AudioDecoder::start_decoding(const std::string& audio_url, const std::string& plex_token) {
    // Stop any existing decoding first
    if (decoding_active.load()) {
        stop_decoding();
    }
    
    // Ensure thread is fully joined before starting new one (with timeout)
    if (decode_thread.joinable()) {
        // Set flag to stop old thread first
        decoding_active = false;
        
        // Try to join, but don't wait forever
        auto start = std::chrono::steady_clock::now();
        while (decode_thread.joinable()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
            if (elapsed.count() > 1000) {  // Increased timeout to 1 second
                // Timeout - detach and continue (thread might be stuck)
                decode_thread.detach();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // Check less frequently
        }
        // Final join attempt
        if (decode_thread.joinable()) {
            try {
                decode_thread.join();
            } catch (...) {
                // Join failed - detach and continue
                decode_thread.detach();
            }
        }
    }
    
    // Validate inputs before starting thread
    if (audio_url.empty() || plex_token.empty()) {
        return false;
    }
    
    current_url = audio_url;
    current_token = plex_token;
    playback_pid = -1;
    waveform_pid = -1;
    is_paused = false;
    
    // Start decoding thread (with error handling)
    try {
        decoding_active = true;
        decode_thread = std::thread(&AudioDecoder::decode_thread_func, this);
    } catch (const std::exception& e) {
        // Thread creation failed
        decoding_active = false;
        return false;
    } catch (...) {
        // Thread creation failed (unknown error)
        decoding_active = false;
        return false;
    }
    
    return true;
}

void AudioDecoder::stop_decoding() {
    // Set flag first to signal thread to exit
    bool was_active = decoding_active.exchange(false);
    is_paused = false;
    
    if (was_active) {
        // Kill playback processes with timeout
        if (playback_pid > 0) {
            // Try graceful termination first
            kill(playback_pid, SIGTERM);
            // Wait with timeout (blocking wait with timeout)
            int waited = 0;
            for (int i = 0; i < 20; ++i) {  // 2 seconds total
                pid_t result = waitpid(playback_pid, nullptr, WNOHANG);
                if (result == playback_pid) {
                    playback_pid = -1;
                    break;  // Process exited
                }
                if (result == -1 && errno == ECHILD) {
                    // Process already reaped
                    playback_pid = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                waited++;
            }
            // Force kill if still running
            if (playback_pid > 0) {
                kill(playback_pid, SIGKILL);
                // Blocking wait for SIGKILL (should be fast)
                waitpid(playback_pid, nullptr, 0);
                playback_pid = -1;
            }
        }
        
        if (waveform_pid > 0) {
            // Try graceful termination first
            kill(waveform_pid, SIGTERM);
            // Wait with timeout (blocking wait with timeout)
            for (int i = 0; i < 20; ++i) {  // 2 seconds total
                pid_t result = waitpid(waveform_pid, nullptr, WNOHANG);
                if (result == waveform_pid) {
                    waveform_pid = -1;
                    break;
                }
                if (result == -1 && errno == ECHILD) {
                    // Process already reaped
                    waveform_pid = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            // Force kill if still running
            if (waveform_pid > 0) {
                kill(waveform_pid, SIGKILL);
                // Blocking wait for SIGKILL (should be fast)
                waitpid(waveform_pid, nullptr, 0);
                waveform_pid = -1;
            }
        }
        
        // Join thread with timeout check
        if (decode_thread.joinable()) {
            // Give thread a moment to exit, but don't wait forever
            auto start = std::chrono::steady_clock::now();
            while (decode_thread.joinable()) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
                if (elapsed.count() > 500) {
                    // Timeout - detach and continue
                    decode_thread.detach();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            // Final join attempt
            if (decode_thread.joinable()) {
                decode_thread.join();
            }
        }
    }
    
    std::lock_guard<std::mutex> lock(samples_mutex);
    waveform_samples.clear();
    current_level = 0.0f;
}

bool AudioDecoder::pause_playback() {
    if (is_paused) return true;  // Already paused
    
    bool paused = false;
    // Pause waveform decoder first (to prevent stutter)
    if (waveform_pid > 0) {
        kill(waveform_pid, SIGSTOP);
        paused = true;
    }
    // Then pause playback
    if (playback_pid > 0) {
        kill(playback_pid, SIGSTOP);
        paused = true;
    }
    if (paused) {
        is_paused = true;
        return true;
    }
    return false;
}

bool AudioDecoder::resume_playback() {
    bool resumed = false;
    if (playback_pid > 0 && is_paused) {
        kill(playback_pid, SIGCONT);
        resumed = true;
    }
    // Also resume waveform decoder
    if (waveform_pid > 0) {
        kill(waveform_pid, SIGCONT);
        resumed = true;
    }
    if (resumed) {
        is_paused = false;
        return true;
    }
    return false;
}

void AudioDecoder::decode_thread_func() {
    // Use ffplay to actually play audio through system speakers
    // Also decode to PCM for waveform visualization using a separate ffmpeg process
    
    // Store headers string for potential restarts - make copies to ensure thread safety
    std::string headers = "X-Plex-Token: " + current_token + "\r\n";
    std::string url = current_url;  // Store URL for restarts
    
    // Validate strings before use
    if (url.empty() || current_token.empty()) {
        decoding_active = false;
        return;
    }
    
    // Fork for actual audio playback (ffplay)
    playback_pid = fork();
    if (playback_pid == -1) {
        // Fork failed - cannot start playback
        decoding_active = false;
        return;
    } else if (playback_pid == 0) {
        // Child: Play audio through speakers
        // Redirect stderr to /dev/null to suppress error messages (especially on quit)
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        
        // Use execvp instead of execlp for safer argument handling
        // Create C-style strings that will persist during exec
        // Make copies of strings to ensure they're valid during exec
        std::string hdr = headers;  // Copy to ensure validity
        std::string u = url;  // Copy to ensure validity
        std::string hdr_copy = hdr;  // Extra copy to ensure lifetime
        std::string url_copy = u;  // Extra copy to ensure lifetime
        
        // Use local arrays (not static) - each process gets its own copy after fork
        char hdr_buf[512];
        char url_buf[2048];
        
        // Copy strings to local buffers
        strncpy(hdr_buf, hdr_copy.c_str(), sizeof(hdr_buf) - 1);
        hdr_buf[sizeof(hdr_buf) - 1] = '\0';
        strncpy(url_buf, url_copy.c_str(), sizeof(url_buf) - 1);
        url_buf[sizeof(url_buf) - 1] = '\0';
        
        // Create args array with pointers to local buffers
        char* args[] = {
            const_cast<char*>("ffplay"),
            const_cast<char*>("-headers"),
            hdr_buf,
            const_cast<char*>("-nodisp"),  // No video window
            const_cast<char*>("-autoexit"),  // Exit when done
            const_cast<char*>("-loglevel"),
            const_cast<char*>("quiet"),
            url_buf,
            nullptr
        };
        
        execvp("ffplay", args);
        _exit(1);
    } else {
        // Parent: playback_pid > 0
        // Parent: Also decode for waveform visualization
        int waveform_pipe[2];
        if (pipe(waveform_pipe) == -1) {
            // If pipe fails, just wait for playback to finish
            waitpid(playback_pid, nullptr, 0);
            return;
        }
        
        waveform_pid = fork();
        if (waveform_pid == -1) {
            // Fork failed - close pipe and wait for playback to finish
            close(waveform_pipe[0]);
            close(waveform_pipe[1]);
            waitpid(playback_pid, nullptr, 0);
            decoding_active = false;
            return;
        } else if (waveform_pid == 0) {
            // Child: Decode to PCM for waveform
            close(waveform_pipe[0]);
            dup2(waveform_pipe[1], STDOUT_FILENO);
            close(waveform_pipe[1]);
            
            // Redirect stderr to /dev/null to suppress error messages (especially on quit)
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull != -1) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            
            // Create C-style strings that will persist during exec
            // Make copies of strings to ensure they're valid during exec
            std::string hdr = headers;  // Copy to ensure validity
            std::string u = url;  // Copy to ensure validity
            std::string hdr_copy = hdr;  // Extra copy to ensure lifetime
            std::string url_copy = u;  // Extra copy to ensure lifetime
            
            // Use local arrays (not static) - each process gets its own copy after fork
            char hdr_buf[512];
            char url_buf[2048];
            
            // Copy strings to local buffers
            strncpy(hdr_buf, hdr_copy.c_str(), sizeof(hdr_buf) - 1);
            hdr_buf[sizeof(hdr_buf) - 1] = '\0';
            strncpy(url_buf, url_copy.c_str(), sizeof(url_buf) - 1);
            url_buf[sizeof(url_buf) - 1] = '\0';
            
            // Create args array with pointers to local buffers
            char* args[] = {
                const_cast<char*>("ffmpeg"),
                const_cast<char*>("-headers"),
                hdr_buf,
                const_cast<char*>("-i"),
                url_buf,
                const_cast<char*>("-f"),
                const_cast<char*>("s16le"),
                const_cast<char*>("-acodec"),
                const_cast<char*>("pcm_s16le"),
                const_cast<char*>("-ar"),
                const_cast<char*>("44100"),
                const_cast<char*>("-ac"),
                const_cast<char*>("1"),
                const_cast<char*>("-loglevel"),
                const_cast<char*>("error"),
                const_cast<char*>("pipe:1"),
                nullptr
            };
            
            execvp("ffmpeg", args);
            _exit(1);
        } else {
            // Parent: waveform_pid > 0
            // Read PCM data for waveform
            close(waveform_pipe[1]);
            
            // Make pipe non-blocking to prevent freezing
            int flags = fcntl(waveform_pipe[0], F_GETFL);
            if (flags != -1) {
                fcntl(waveform_pipe[0], F_SETFL, flags | O_NONBLOCK);
            }
            
            const size_t BUFFER_SIZE = 44100 * 2;
            std::vector<int16_t> pcm_buffer;
            pcm_buffer.reserve(BUFFER_SIZE);
            
            char read_buffer[4096];
            
            while (decoding_active.load()) {
                // Check if we should exit before blocking read
                if (!decoding_active.load()) {
                    break;
                }
                
                ssize_t bytes_read = read(waveform_pipe[0], read_buffer, sizeof(read_buffer));
                if (bytes_read <= 0) {
                    int status;
                    pid_t result = waitpid(waveform_pid, &status, WNOHANG);
                    if (result == waveform_pid) {
                        // Check if we should exit before restarting
                        if (!decoding_active.load()) {
                            break;
                        }
                        // Process exited
                        if (WIFEXITED(status)) {
                            int exit_code = WEXITSTATUS(status);
                            if (exit_code != 0) {
                                // Error exit - restart ffmpeg silently
                                // Don't print to stderr as it interferes with TUI
                                // Restart the waveform decoder
                                // Check again before restarting
                                if (!decoding_active.load()) {
                                    break;
                                }
                                close(waveform_pipe[0]);
                                if (pipe(waveform_pipe) == -1) {
                                    break;
                                }
                                waveform_pid = fork();
                                if (waveform_pid == -1) {
                                    // Fork failed
                                    close(waveform_pipe[0]);
                                    close(waveform_pipe[1]);
                                    break;
                                } else if (waveform_pid == 0) {
                                    close(waveform_pipe[0]);
                                    dup2(waveform_pipe[1], STDOUT_FILENO);
                                    close(waveform_pipe[1]);
                                    
                                    // Create C-style strings that will persist during exec
                                    std::string hdr = headers;
                                    std::string u = url;
                                    std::string hdr_copy = hdr;
                                    std::string url_copy = u;
                                    
                                    char hdr_buf[512];
                                    char url_buf[2048];
                                    strncpy(hdr_buf, hdr_copy.c_str(), sizeof(hdr_buf) - 1);
                                    hdr_buf[sizeof(hdr_buf) - 1] = '\0';
                                    strncpy(url_buf, url_copy.c_str(), sizeof(url_buf) - 1);
                                    url_buf[sizeof(url_buf) - 1] = '\0';
                                    
                                    char* args[] = {
                                        const_cast<char*>("ffmpeg"),
                                        const_cast<char*>("-headers"),
                                        hdr_buf,
                                        const_cast<char*>("-i"),
                                        url_buf,
                                        const_cast<char*>("-f"),
                                        const_cast<char*>("s16le"),
                                        const_cast<char*>("-acodec"),
                                        const_cast<char*>("pcm_s16le"),
                                        const_cast<char*>("-ar"),
                                        const_cast<char*>("44100"),
                                        const_cast<char*>("-ac"),
                                        const_cast<char*>("1"),
                                        const_cast<char*>("-loglevel"),
                                        const_cast<char*>("error"),
                                        const_cast<char*>("pipe:1"),
                                        nullptr
                                    };
                                    
                                    execvp("ffmpeg", args);
                                    _exit(1);
                                } else {
                                    // Parent: waveform_pid > 0
                                    close(waveform_pipe[1]);
                                    int flags = fcntl(waveform_pipe[0], F_GETFL);
                                    if (flags != -1) {
                                        fcntl(waveform_pipe[0], F_SETFL, flags | O_NONBLOCK);
                                    }
                                    continue;  // Continue reading
                                }
                            }
                            // Normal exit (0) - might be end of stream, but keep trying
                            // Restart decoder to continue silently
                            // Check if we should exit before restarting
                            if (!decoding_active.load()) {
                                break;
                            }
                            close(waveform_pipe[0]);
                            if (pipe(waveform_pipe) == -1) {
                                break;
                            }
                            waveform_pid = fork();
                            if (waveform_pid == -1) {
                                // Fork failed
                                close(waveform_pipe[0]);
                                close(waveform_pipe[1]);
                                break;
                            } else if (waveform_pid == 0) {
                                close(waveform_pipe[0]);
                                dup2(waveform_pipe[1], STDOUT_FILENO);
                                close(waveform_pipe[1]);
                                
                                // Create C-style strings that will persist during exec
                                std::string hdr = headers;
                                std::string u = url;
                                std::string hdr_copy = hdr;
                                std::string url_copy = u;
                                
                                char hdr_buf[512];
                                char url_buf[2048];
                                strncpy(hdr_buf, hdr_copy.c_str(), sizeof(hdr_buf) - 1);
                                hdr_buf[sizeof(hdr_buf) - 1] = '\0';
                                strncpy(url_buf, url_copy.c_str(), sizeof(url_buf) - 1);
                                url_buf[sizeof(url_buf) - 1] = '\0';
                                
                                char* args[] = {
                                    const_cast<char*>("ffmpeg"),
                                    const_cast<char*>("-headers"),
                                    hdr_buf,
                                    const_cast<char*>("-i"),
                                    url_buf,
                                    const_cast<char*>("-f"),
                                    const_cast<char*>("s16le"),
                                    const_cast<char*>("-acodec"),
                                    const_cast<char*>("pcm_s16le"),
                                    const_cast<char*>("-ar"),
                                    const_cast<char*>("44100"),
                                    const_cast<char*>("-ac"),
                                    const_cast<char*>("1"),
                                    const_cast<char*>("-loglevel"),
                                    const_cast<char*>("error"),
                                    const_cast<char*>("pipe:1"),
                                    nullptr
                                };
                                
                                execvp("ffmpeg", args);
                                _exit(1);
                            } else {
                                // Parent: waveform_pid > 0
                                close(waveform_pipe[1]);
                                int flags = fcntl(waveform_pipe[0], F_GETFL);
                                if (flags != -1) {
                                    fcntl(waveform_pipe[0], F_SETFL, flags | O_NONBLOCK);
                                }
                                continue;
                            }
                        } else if (WIFSIGNALED(status)) {
                            // Killed by signal - might be intentional, restart silently
                            // Check if we should exit before restarting
                            if (!decoding_active.load()) {
                                break;
                            }
                            close(waveform_pipe[0]);
                            if (pipe(waveform_pipe) == -1) {
                                break;
                            }
                            waveform_pid = fork();
                            if (waveform_pid == -1) {
                                // Fork failed
                                close(waveform_pipe[0]);
                                close(waveform_pipe[1]);
                                break;
                            } else if (waveform_pid == 0) {
                                close(waveform_pipe[0]);
                                dup2(waveform_pipe[1], STDOUT_FILENO);
                                close(waveform_pipe[1]);
                                
                                // Create C-style strings that will persist during exec
                                std::string hdr = headers;
                                std::string u = url;
                                std::string hdr_copy = hdr;
                                std::string url_copy = u;
                                
                                char hdr_buf[512];
                                char url_buf[2048];
                                strncpy(hdr_buf, hdr_copy.c_str(), sizeof(hdr_buf) - 1);
                                hdr_buf[sizeof(hdr_buf) - 1] = '\0';
                                strncpy(url_buf, url_copy.c_str(), sizeof(url_buf) - 1);
                                url_buf[sizeof(url_buf) - 1] = '\0';
                                
                                char* args[] = {
                                    const_cast<char*>("ffmpeg"),
                                    const_cast<char*>("-headers"),
                                    hdr_buf,
                                    const_cast<char*>("-i"),
                                    url_buf,
                                    const_cast<char*>("-f"),
                                    const_cast<char*>("s16le"),
                                    const_cast<char*>("-acodec"),
                                    const_cast<char*>("pcm_s16le"),
                                    const_cast<char*>("-ar"),
                                    const_cast<char*>("44100"),
                                    const_cast<char*>("-ac"),
                                    const_cast<char*>("1"),
                                    const_cast<char*>("-loglevel"),
                                    const_cast<char*>("error"),
                                    const_cast<char*>("pipe:1"),
                                    nullptr
                                };
                                
                                execvp("ffmpeg", args);
                                _exit(1);
                            } else {
                                // Parent: waveform_pid > 0
                                close(waveform_pipe[1]);
                                int flags = fcntl(waveform_pipe[0], F_GETFL);
                                if (flags != -1) {
                                    fcntl(waveform_pipe[0], F_SETFL, flags | O_NONBLOCK);
                                }
                                continue;
                            }
                        }
                    }
                    if (bytes_read < 0) {
                        // Read error - check if it's just EAGAIN (non-blocking)
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // No data available yet - normal for non-blocking
                            // Sleep longer to reduce CPU usage
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            continue;
                        }
                        // Real error - log silently (don't interfere with TUI)
                        // Error reading from waveform pipe - continue trying
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }
                    // EOF or no data - check if process is still alive
                    // If process died, we'll catch it in the waitpid check above
                    // Sleep longer to reduce CPU usage
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                
                // Successfully read data - reset any error state
                
                size_t samples_read = bytes_read / sizeof(int16_t);
                const int16_t* samples = reinterpret_cast<const int16_t*>(read_buffer);
                
                for (size_t i = 0; i < samples_read; ++i) {
                    pcm_buffer.push_back(samples[i]);
                    
                    if (pcm_buffer.size() >= 4410) {
                        process_pcm_data(pcm_buffer);
                        pcm_buffer.clear();
                    }
                }
            }
            
            close(waveform_pipe[0]);
            // Don't wait here - let stop_decoding handle cleanup
            // Use WNOHANG to avoid blocking
            waitpid(waveform_pid, nullptr, WNOHANG);
        }
        
        // Don't wait for playback here - it blocks forever
        // The stop_decoding() function will handle cleanup
        // Just check if it's still running (non-blocking)
        waitpid(playback_pid, nullptr, WNOHANG);
    }
}

void AudioDecoder::process_pcm_data(const std::vector<int16_t>& pcm_samples) {
    if (pcm_samples.empty()) return;
    
    // Calculate RMS (Root Mean Square) for this chunk
    double sum_squares = 0.0;
    for (int16_t sample : pcm_samples) {
        double normalized = static_cast<double>(sample) / 32768.0;
        sum_squares += normalized * normalized;
    }
    
    double rms = std::sqrt(sum_squares / pcm_samples.size());
    
    // Normalize to 0.0-1.0 range
    float level = static_cast<float>(std::min(1.0, rms * 2.0));  // Scale up for visibility
    
    std::lock_guard<std::mutex> lock(samples_mutex);
    
    // Add to rolling buffer
    waveform_samples.push_back(level);
    if (waveform_samples.size() > MAX_SAMPLES) {
        waveform_samples.erase(waveform_samples.begin());
    }
    
    current_level = level;
}

std::vector<float> AudioDecoder::get_waveform_samples(int count) {
    std::lock_guard<std::mutex> lock(samples_mutex);
    
    if (waveform_samples.empty()) {
        return std::vector<float>(count, 0.0f);
    }
    
    // Return the most recent samples
    size_t start_idx = waveform_samples.size() > static_cast<size_t>(count) ?
                       waveform_samples.size() - count : 0;
    
    std::vector<float> result;
    result.reserve(count);
    
    for (size_t i = start_idx; i < waveform_samples.size(); ++i) {
        result.push_back(waveform_samples[i]);
    }
    
    // Pad with zeros if needed
    while (result.size() < static_cast<size_t>(count)) {
        result.insert(result.begin(), 0.0f);
    }
    
    return result;
}

float AudioDecoder::get_current_level() const {
    std::lock_guard<std::mutex> lock(samples_mutex);
    return current_level;
}

// AlbumArt implementation
AlbumArt::AlbumArt() {
    // Constructor - nothing to initialize
}

bool AlbumArt::fetch_art(const std::string& plex_server, const std::string& token,
                         const std::string& art_url) {
    clear();
    
    std::string full_url = art_url;
    if (full_url.find("http") != 0) {
        // Relative URL, prepend server
        if (full_url[0] != '/') {
            full_url = "/" + full_url;
        }
        full_url = plex_server + full_url;
    }
    
    // Add token
    if (full_url.find('?') != std::string::npos) {
        full_url += "&X-Plex-Token=" + token;
    } else {
        full_url += "?X-Plex-Token=" + token;
    }
    
    return download_image(full_url, token);
}

bool AlbumArt::download_image(const std::string& url, const std::string& token) {
    // Use curl to download image
    // For simplicity, we'll use a system call to curl
    // In production, use libcurl directly
    
    std::string cmd = "curl -s -H 'X-Plex-Token: " + token + "' '" + url + "'";
    
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return false;
    }
    
    char buffer[4096];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        art_data.insert(art_data.end(), buffer, buffer + bytes_read);
    }
    
    pclose(pipe);
    
    if (art_data.empty()) {
        return false;
    }
    
    // Try to decode image
    return decode_image();
}

bool AlbumArt::decode_image() {
    // Simple approach: use ImageMagick or similar to convert to text
    // For now, we'll create a placeholder that uses system tools
    
    // Write to temp file
    std::string temp_file = "/tmp/plex_art_" + std::to_string(getpid()) + ".jpg";
    std::ofstream out(temp_file, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(art_data.data()), art_data.size());
    out.close();
    
    // Use ImageMagick or ffmpeg to get dimensions and convert
    // For now, assume we can get basic info
    // In production, use a proper image library like stb_image
    
    // Placeholder: return true if we have data
    // Real implementation would decode JPEG/PNG here
    return !art_data.empty();
}

std::vector<std::vector<uint8_t>> AlbumArt::pixelate_image(int width, int height) {
    std::vector<std::vector<uint8_t>> result(height);
    
    if (art_data.empty()) {
        // No image data - return gradient placeholder
        for (int y = 0; y < height; ++y) {
            result[y].resize(width * 3);
            for (int x = 0; x < width; ++x) {
                result[y][x * 3 + 0] = (x * 255) / std::max(1, width);
                result[y][x * 3 + 1] = (y * 255) / std::max(1, height);
                result[y][x * 3 + 2] = 128;
            }
        }
        return result;
    }
    
    // Use ffmpeg to decode and resize image to pixelated size
    // Use unique temp files with timestamp to avoid conflicts
    auto now = std::chrono::steady_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    std::string temp_in = "/tmp/plex_art_in_" + std::to_string(getpid()) + "_" + std::to_string(timestamp) + ".jpg";
    std::string temp_out = "/tmp/plex_art_out_" + std::to_string(getpid()) + "_" + std::to_string(timestamp) + ".raw";
    
    {
        std::ofstream out(temp_in, std::ios::binary);
        if (!out) {
            // Fallback to gradient
            for (int y = 0; y < height; ++y) {
                result[y].resize(width * 3);
                for (int x = 0; x < width; ++x) {
                    result[y][x * 3 + 0] = 100;
                    result[y][x * 3 + 1] = 100;
                    result[y][x * 3 + 2] = 100;
                }
            }
            return result;
        }
        out.write(reinterpret_cast<const char*>(art_data.data()), art_data.size());
    }
    
    // Use ffmpeg to resize and convert to raw RGB
    // Use -y flag to auto-overwrite (just in case)
    pid_t pid = fork();
    if (pid == 0) {
        // Child: run ffmpeg
        int fd = open(temp_out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
        
        // Build video filter: 
        // 1. Scale to higher res first (2x) for better quality, then downscale
        // 2. Scale to target size maintaining aspect ratio
        // 3. Pad to square
        int scale_w = width * 2;  // Render at 2x for better quality
        int scale_h = height * 2;
        std::string vf = "scale=" + std::to_string(scale_w) + ":" + std::to_string(scale_h) + 
                        ":force_original_aspect_ratio=decrease:flags=lanczos," +
                        "scale=" + std::to_string(width) + ":" + std::to_string(height) + 
                        ":flags=neighbor," +  // Use neighbor for pixelated look
                        "pad=" + std::to_string(width) + ":" + std::to_string(height) + 
                        ":(ow-iw)/2:(oh-ih)/2:black";
        
        // Create C-style strings that will persist during exec
        std::string in_file = temp_in;
        std::string out_file = temp_out;
        std::string vf_str = vf;
        std::string in_copy = in_file;
        std::string out_copy = out_file;
        std::string vf_copy = vf_str;
        
        char in_buf[512];
        char out_buf[512];
        char vf_buf[1024];
        strncpy(in_buf, in_copy.c_str(), sizeof(in_buf) - 1);
        in_buf[sizeof(in_buf) - 1] = '\0';
        strncpy(out_buf, out_copy.c_str(), sizeof(out_buf) - 1);
        out_buf[sizeof(out_buf) - 1] = '\0';
        strncpy(vf_buf, vf_copy.c_str(), sizeof(vf_buf) - 1);
        vf_buf[sizeof(vf_buf) - 1] = '\0';
        
        char* args[] = {
            const_cast<char*>("ffmpeg"),
            const_cast<char*>("-y"),  // Auto-overwrite output file
            const_cast<char*>("-i"),
            in_buf,
            const_cast<char*>("-vf"),
            vf_buf,
            const_cast<char*>("-f"),
            const_cast<char*>("rawvideo"),
            const_cast<char*>("-pix_fmt"),
            const_cast<char*>("rgb24"),
            const_cast<char*>("-loglevel"),
            const_cast<char*>("quiet"),
            out_buf,
            nullptr
        };
        
        execvp("ffmpeg", args);
        _exit(1);
    } else if (pid > 0) {
        // Parent: wait for ffmpeg and read result
        int status = 0;
        waitpid(pid, &status, 0);
        
        // Check if ffmpeg succeeded
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            // Read raw RGB data
            std::ifstream in(temp_out, std::ios::binary);
            if (in && in.good()) {
                for (int y = 0; y < height; ++y) {
                    result[y].resize(width * 3);
                    in.read(reinterpret_cast<char*>(result[y].data()), width * 3);
                    if (!in || in.gcount() < width * 3) {
                        // Read failed or incomplete - fill with gray
                        for (int x = 0; x < width; ++x) {
                            result[y][x * 3 + 0] = 128;
                            result[y][x * 3 + 1] = 128;
                            result[y][x * 3 + 2] = 128;
                        }
                    }
                }
                in.close();
            } else {
                // File read failed - use gray
                for (int y = 0; y < height; ++y) {
                    result[y].resize(width * 3);
                    for (int x = 0; x < width; ++x) {
                        result[y][x * 3 + 0] = 128;
                        result[y][x * 3 + 1] = 128;
                        result[y][x * 3 + 2] = 128;
                    }
                }
            }
        } else {
            // ffmpeg failed - use gray placeholder
            for (int y = 0; y < height; ++y) {
                result[y].resize(width * 3);
                for (int x = 0; x < width; ++x) {
                    result[y][x * 3 + 0] = 128;
                    result[y][x * 3 + 1] = 128;
                    result[y][x * 3 + 2] = 128;
                }
            }
        }
        
        // Cleanup temp files (ignore errors - files might not exist)
        unlink(temp_in.c_str());
        unlink(temp_out.c_str());
    } else {
        // Fork failed - use gradient
        for (int y = 0; y < height; ++y) {
            result[y].resize(width * 3);
            for (int x = 0; x < width; ++x) {
                result[y][x * 3 + 0] = 100;
                result[y][x * 3 + 1] = 100;
                result[y][x * 3 + 2] = 100;
            }
        }
    }
    
    return result;
}

std::vector<std::string> AlbumArt::render_pixelated(int width, int height, const Theme& /*theme*/) {
    if (!has_art()) {
        // Return empty placeholder
        return std::vector<std::string>(height, std::string(width, ' '));
    }
    
    auto pixels = pixelate_image(width, height);
    std::vector<std::string> result;
    result.reserve(height);
    
    // Use block characters for pixelated effect
    // Each character represents one "pixel"
    const char* block_chars[] = {" ", "░", "▒", "▓", "█"};
    
    for (int y = 0; y < height; ++y) {
        std::string row;
        row.reserve(width * 25);  // Approximate for ANSI codes
        
        for (int x = 0; x < width; ++x) {
            // Bounds check to prevent crashes
            if (y >= static_cast<int>(pixels.size()) || 
                x * 3 + 2 >= static_cast<int>(pixels[y].size())) {
                // Out of bounds - use gray pixel
                row += " ";
                continue;
            }
            
            uint8_t r = pixels[y][x * 3 + 0];
            uint8_t g = pixels[y][x * 3 + 1];
            uint8_t b = pixels[y][x * 3 + 2];
            
            // btop style: Use foreground color on black background for pixelated look
            // Use simple ANSI codes (Terminal class will be used by caller)
            std::string black_bg = "\033[48;2;0;0;0m";
            std::string fg_color = "\033[38;2;" + std::to_string(r) + ";" + 
                                  std::to_string(g) + ";" + std::to_string(b) + "m";
            row += black_bg + fg_color + "█" + "\033[0m";
        }
        
        result.push_back(row);
    }
    
    return result;
}

void AlbumArt::clear() {
    art_data.clear();
    decoded_rgb.clear();
    image_width = 0;
    image_height = 0;
}

} // namespace PlexTUI
