#!/usr/bin/env bash
# Install the TB351FU installer into PREFIX (default: ~/.local).
#
#   ./install.sh                 # user install
#   PREFIX=/usr/local sudo ./install.sh   # system-wide install
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMMON_DIR="$(cd "$HERE/.." && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
BIN_DIR="$PREFIX/bin"
SHARE_DIR="$PREFIX/share/tb351fu-installer"
APP_DIR="$PREFIX/share/applications"
LAUNCHER="$BIN_DIR/tb351fu-installer"

if ! command -v python3 >/dev/null 2>&1; then
  echo "ERROR: python3 is required." >&2
  exit 1
fi
if ! python3 -c "import gi; gi.require_version('Gtk','4.0'); from gi.repository import Gtk" \
     >/dev/null 2>&1; then
  echo "ERROR: GTK4 Python bindings (python3-gi) are required." >&2
  exit 1
fi
for tool in adb fastboot; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "WARNING: $tool not found; device detection and flashing need it." >&2
  fi
done

install -d "$BIN_DIR" "$SHARE_DIR" "$APP_DIR"

# Install a private copy so the launcher keeps working if the checkout moves.
rm -rf "$SHARE_DIR/tb351fu_installer"
cp -a "$HERE/tb351fu_installer" "$SHARE_DIR/"
find "$SHARE_DIR" -type d -name __pycache__ -prune -exec rm -rf {} +
find "$SHARE_DIR" -type f -name '*.pyc' -delete

cat > "$LAUNCHER" <<EOF
#!/bin/sh
export TB351FU_COMMON_DIR="$COMMON_DIR"
export PYTHONPATH="$SHARE_DIR\${PYTHONPATH:+:\$PYTHONPATH}"
exec python3 -m tb351fu_installer "\$@"
EOF
chmod 0755 "$LAUNCHER"

sed "s|@BIN@|$LAUNCHER|g" \
  "$HERE/org.postmarketos.TB351FUInstaller.desktop" \
  > "$APP_DIR/org.postmarketos.TB351FUInstaller.desktop"
chmod 0644 "$APP_DIR/org.postmarketos.TB351FUInstaller.desktop"

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "$APP_DIR" >/dev/null 2>&1 || true
fi

echo "Installed:"
echo "  $LAUNCHER"
echo "  $APP_DIR/org.postmarketos.TB351FUInstaller.desktop"
echo "  $SHARE_DIR/tb351fu_installer"
echo
echo "Run 'tb351fu-installer' or open 'postmarketOS Installer (TB351FU)'."
if [ "$PREFIX" = "$HOME/.local" ]; then
  case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) echo "NOTE: add $BIN_DIR to PATH to run it by name." ;;
  esac
fi
echo "The launcher uses the checkout at: $COMMON_DIR"
