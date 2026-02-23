#!/bin/bash
# Build an AppImage for Ryzen SMU Debug Tool
# Requires: appimagetool (https://github.com/AppImage/appimagetool)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APPDIR="$SCRIPT_DIR/AppDir"
BINARY="$SCRIPT_DIR/smu_debug_tool"

if [ ! -f "$BINARY" ]; then
    echo "Binary not found. Building..."
    make -C "$SCRIPT_DIR" clean all
fi

# Find appimagetool: check PATH first, then local directory
APPIMAGETOOL=""
if command -v appimagetool &>/dev/null; then
    APPIMAGETOOL="appimagetool"
elif [ -f "$SCRIPT_DIR/appimagetool-x86_64.AppImage" ]; then
    APPIMAGETOOL="$SCRIPT_DIR/appimagetool-x86_64.AppImage"
    chmod +x "$APPIMAGETOOL"
else
    echo "appimagetool not found. Place appimagetool-x86_64.AppImage in this directory or install it from:"
    echo "  https://github.com/AppImage/appimagetool/releases"
    exit 1
fi

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"
mkdir -p "$APPDIR/usr/share/polkit-1/actions"

# Copy binary
cp "$BINARY" "$APPDIR/usr/bin/smu_debug_tool"

# Copy polkit policy
cp "$SCRIPT_DIR/com.ryzen.smudebug.policy" "$APPDIR/usr/share/polkit-1/actions/"

# Desktop file (must be at AppDir root for appimagetool)
cp "$SCRIPT_DIR/com.ryzen.smudebug.desktop" "$APPDIR/com.ryzen.smudebug.desktop"

# Icon placeholder (appimagetool requires an icon)
if [ ! -f "$SCRIPT_DIR/com.ryzen.smudebug.png" ]; then
    # Generate a minimal 1x1 PNG placeholder if no icon exists
    printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDATx\x9cc\xf8\x0f\x00\x00\x01\x01\x00\x05\x18\xd8N\x00\x00\x00\x00IEND\xaeB`\x82' > "$APPDIR/com.ryzen.smudebug.png"
    echo "Warning: Using placeholder icon. Replace com.ryzen.smudebug.png with a real icon."
else
    cp "$SCRIPT_DIR/com.ryzen.smudebug.png" "$APPDIR/com.ryzen.smudebug.png"
fi

# AppRun script
cat > "$APPDIR/AppRun" << 'APPRUN_EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Query the user's GTK theme from gsettings if GTK_THEME isn't set
if [ -z "$GTK_THEME" ]; then
    GTK_THEME=$(gsettings get org.gnome.desktop.interface gtk-theme 2>/dev/null | tr -d "'" || true)
    export GTK_THEME
fi
# Query dark mode preference
COLOR_SCHEME=$(gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null || true)
if echo "$COLOR_SCHEME" | grep -q "prefer-dark"; then
    export ADW_DEBUG_COLOR_SCHEME="prefer-dark"
fi

# pkexec strips the environment; pass display and theme vars as --env- arguments
# so the binary can restore them before GTK init.
ENV_ARGS=""
for VAR in DISPLAY WAYLAND_DISPLAY XDG_RUNTIME_DIR XAUTHORITY DBUS_SESSION_BUS_ADDRESS GTK_THEME ADW_DEBUG_COLOR_SCHEME XDG_CONFIG_HOME HOME; do
    eval VAL=\$$VAR
    [ -n "$VAL" ] && ENV_ARGS="$ENV_ARGS --env-$VAR=$VAL"
done

# Prefer the installed binary (matches polkit policy exec.path).
# Fall back to the bundled copy if not installed.
if [ -x /usr/local/bin/smu_debug_tool ]; then
    exec pkexec /usr/local/bin/smu_debug_tool $ENV_ARGS --gui "$@"
else
    exec pkexec "$HERE/usr/bin/smu_debug_tool" $ENV_ARGS --gui "$@"
fi
APPRUN_EOF
chmod +x "$APPDIR/AppRun"

# Build AppImage
ARCH=x86_64 "$APPIMAGETOOL" "$APPDIR" "$SCRIPT_DIR/RyzenSMUDebug-x86_64.AppImage"

echo ""
echo "AppImage created: $SCRIPT_DIR/RyzenSMUDebug-x86_64.AppImage"
echo "Make it executable: chmod +x RyzenSMUDebug-x86_64.AppImage"
