#!/bin/bash
# Install Ryzen SMU Debug Tool (AppImage + desktop integration)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APPIMAGE="$SCRIPT_DIR/RyzenSMUDebug-x86_64.AppImage"
INSTALL_DIR="/opt/RyzenSMUDebug"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"

REAL_USER="${SUDO_USER:-$(whoami)}"
REAL_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)

# ── Helper: download appimagetool if missing ──────────────────────────────
ensure_appimagetool() {
    if command -v appimagetool &>/dev/null; then
        return
    fi
    if [ -f "$SCRIPT_DIR/appimagetool-x86_64.AppImage" ]; then
        chmod +x "$SCRIPT_DIR/appimagetool-x86_64.AppImage"
        return
    fi

    echo "appimagetool not found. Downloading..."
    if command -v curl &>/dev/null; then
        curl -L -o "$SCRIPT_DIR/appimagetool-x86_64.AppImage" "$APPIMAGETOOL_URL"
    elif command -v wget &>/dev/null; then
        wget -O "$SCRIPT_DIR/appimagetool-x86_64.AppImage" "$APPIMAGETOOL_URL"
    else
        echo "Error: curl or wget required to download appimagetool."
        exit 1
    fi
    chmod +x "$SCRIPT_DIR/appimagetool-x86_64.AppImage"
    chown "$REAL_USER":"$REAL_USER" "$SCRIPT_DIR/appimagetool-x86_64.AppImage"
    echo "appimagetool downloaded."
}

# ── Build binary if needed ────────────────────────────────────────────────
if [ ! -f "$SCRIPT_DIR/smu_debug_tool" ]; then
    echo "Binary not found. Building..."
    su "$REAL_USER" -c "make -C '$SCRIPT_DIR' clean all"
fi

# ── Download appimagetool and build AppImage ──────────────────────────────
ensure_appimagetool

# Clean old build artifacts (may be root-owned from a previous install)
rm -rf "$SCRIPT_DIR/AppDir"
rm -f "$SCRIPT_DIR/RyzenSMUDebug-x86_64.AppImage"

echo "Building AppImage..."
su "$REAL_USER" -c "bash '$SCRIPT_DIR/build-appimage.sh'"

# ── Elevate to root for installation ──────────────────────────────────────
if [ "$(id -u)" -ne 0 ]; then
    echo "Installation requires root. Re-running with sudo..."
    exec sudo "$0" "$@"
fi

echo "Installing Ryzen SMU Debug Tool..."

# Install binary to /usr/local/bin (needed for polkit policy match)
cp "$SCRIPT_DIR/smu_debug_tool" /usr/local/bin/smu_debug_tool
chmod +x /usr/local/bin/smu_debug_tool

# Install AppImage
mkdir -p "$INSTALL_DIR"
cp "$APPIMAGE" "$INSTALL_DIR/RyzenSMUDebug-x86_64.AppImage"
chmod +x "$INSTALL_DIR/RyzenSMUDebug-x86_64.AppImage"

# Install polkit policy
cp "$SCRIPT_DIR/com.ryzen.smudebug.policy" /usr/share/polkit-1/actions/

# Install desktop file to system applications
cp "$SCRIPT_DIR/com.ryzen.smudebug.desktop" /usr/share/applications/

# Copy desktop file to user's Desktop folder
DESKTOP_DIR="${REAL_HOME}/Desktop"
if [ ! -d "$DESKTOP_DIR" ]; then
    DESKTOP_DIR=$(su "$REAL_USER" -c 'xdg-user-dir DESKTOP 2>/dev/null' || true)
fi
if [ -d "$DESKTOP_DIR" ]; then
    cp "$SCRIPT_DIR/com.ryzen.smudebug.desktop" "$DESKTOP_DIR/"
    chown "$REAL_USER":"$REAL_USER" "$DESKTOP_DIR/com.ryzen.smudebug.desktop"
    chmod +x "$DESKTOP_DIR/com.ryzen.smudebug.desktop"
fi

# Install icon if available
if [ -f "$SCRIPT_DIR/com.ryzen.smudebug.png" ]; then
    mkdir -p /usr/share/icons/hicolor/256x256/apps
    cp "$SCRIPT_DIR/com.ryzen.smudebug.png" /usr/share/icons/hicolor/256x256/apps/
    gtk-update-icon-cache /usr/share/icons/hicolor/ 2>/dev/null || true
fi

echo ""
echo "Installation complete!"
echo "  AppImage:     $INSTALL_DIR/RyzenSMUDebug-x86_64.AppImage"
echo "  Binary:       /usr/local/bin/smu_debug_tool"
echo "  Desktop file: /usr/share/applications/com.ryzen.smudebug.desktop"
echo ""
echo "You can now launch it from your application menu or run:"
echo "  smu_debug_tool --gui"
