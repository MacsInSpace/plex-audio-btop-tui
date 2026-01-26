#!/bin/bash

# Plex TUI Build Script

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="$SCRIPT_DIR/build"
BIN_DIR="$SCRIPT_DIR/bin"
INSTALL_PREFIX="${INSTALL_PREFIX:-$HOME/.local}"

print_usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  clean      Clean build directory"
    echo "  debug      Build with debug symbols"
    echo "  release    Build optimized release (default)"
    echo "  install    Install to \$INSTALL_PREFIX (default: ~/.local)"
    echo "  help       Show this help"
    echo ""
}

clean_build() {
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    echo "Done."
}

check_dependencies() {
    echo "Checking dependencies..."
    
    # Check for cmake
    if ! command -v cmake &> /dev/null; then
        echo "Error: cmake not found. Please install cmake."
        exit 1
    fi
    
    # Check for compiler
    if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
        echo "Error: No C++ compiler found. Please install g++ or clang++."
        exit 1
    fi
    
    # Check for libcurl
    if ! pkg-config --exists libcurl 2>/dev/null; then
        echo "Warning: libcurl not found via pkg-config."
        echo "Make sure libcurl development files are installed."
        echo "  Ubuntu/Debian: sudo apt install libcurl4-openssl-dev"
        echo "  macOS: brew install curl"
        read -p "Continue anyway? (y/N) " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            exit 1
        fi
    fi
    
    echo "Dependencies OK."
}

build_project() {
    BUILD_TYPE="${1:-Release}"
    
    echo "Building Plex TUI ($BUILD_TYPE)..."
    
    mkdir -p "$BUILD_DIR" "$BIN_DIR"
    cd "$BUILD_DIR"
    
    cmake -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
          -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
          ..
    
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)
    
    # Copy binary to bin directory
    if [ -f "$BUILD_DIR/plex-tui" ]; then
        cp "$BUILD_DIR/plex-tui" "$BIN_DIR/plex-tui"
    fi
    
    echo ""
    echo "Build complete!"
    echo "Binary: $BIN_DIR/plex-tui"
    echo ""
    echo "To run:"
    echo "  $BIN_DIR/plex-tui --server <URL> --token <TOKEN>"
    echo ""
    echo "Or install with:"
    echo "  $0 install"
    echo ""
}

install_project() {
    if [ ! -f "$BIN_DIR/plex-tui" ] && [ ! -f "$BUILD_DIR/plex-tui" ]; then
        echo "Error: Binary not found. Build first with: $0 release"
        exit 1
    fi
    
    # Use bin directory binary if available, otherwise build directory
    BINARY="$BIN_DIR/plex-tui"
    if [ ! -f "$BINARY" ]; then
        BINARY="$BUILD_DIR/plex-tui"
    fi
    
    echo "Installing to $INSTALL_PREFIX..."
    
    # Install binary
    mkdir -p "$INSTALL_PREFIX/bin"
    cp "$BINARY" "$INSTALL_PREFIX/bin/plex-tui"
    chmod +x "$INSTALL_PREFIX/bin/plex-tui"
    
    # Create config directory
    mkdir -p "$HOME/.config/plex-tui"
    
    # Copy example config if no config exists
    if [ ! -f "$HOME/.config/plex-tui/config.ini" ]; then
        echo "Creating example config..."
        cp "$SCRIPT_DIR/config.example.ini" "$HOME/.config/plex-tui/config.ini"
        echo "Please edit ~/.config/plex-tui/config.ini with your Plex server details."
    fi
    
    echo ""
    echo "Installation complete!"
    echo "Binary installed to: $INSTALL_PREFIX/bin/plex-tui"
    echo "Config file: ~/.config/plex-tui/config.ini"
    echo ""
    echo "Make sure $INSTALL_PREFIX/bin is in your PATH."
    echo ""
}

# Main script
case "${1:-release}" in
    clean)
        clean_build
        ;;
    debug)
        check_dependencies
        build_project "Debug"
        ;;
    release)
        check_dependencies
        build_project "Release"
        ;;
    install)
        install_project
        ;;
    help|--help|-h)
        print_usage
        ;;
    *)
        echo "Unknown option: $1"
        print_usage
        exit 1
        ;;
esac
