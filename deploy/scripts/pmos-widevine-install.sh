#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Install a user-supplied ARM64 Widevine CDM for Firefox ESR / Chromium.
#
# pmOS does not ship proprietary Widevine blobs and cannot provide L1 (that
# needs the vendor Trustonic TEE and a secure video path).  This helper only
# installs an L3 CDM the user already owns, after checking that it is an
# AArch64 ELF and warning when its C library does not match the browser.
#
# Usage:
#   pmos-widevine-install.sh [--browser firefox|chromium|both] <cdm-dir>
#   pmos-widevine-install.sh --uninstall [--browser firefox|chromium|both]
#
# <cdm-dir> must contain libwidevinecdm.so and manifest.json.  ChromeOS-style
# trees (WidevineCdm/<version>/_platform_specific/linux_arm64/...) are also
# accepted.

set -eu

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

BROWSER=both
UNINSTALL=0
USER_NAME=user
USER_UID=10000
USER_GID=10000
USER_HOME=/home/user

usage() {
	sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	--browser)
		shift
		BROWSER=${1:-}
		;;
	--browser=*)
		BROWSER=${1#--browser=}
		;;
	--uninstall)
		UNINSTALL=1
		;;
	-h|--help)
		usage
		;;
	--)
		shift
		break
		;;
	-*)
		echo "unknown option: $1" >&2
		usage
		;;
	*)
		break
		;;
	esac
	shift
done

case "$BROWSER" in
firefox|chromium|both) ;;
*)
	echo "invalid browser: $BROWSER (use firefox, chromium or both)" >&2
	exit 2
	;;
esac

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root (the helper writes into $USER_HOME)" >&2
	exit 1
fi

marker_start='# >>> pmos-widevine >>>'
marker_end='# <<< pmos-widevine <<<'

strip_firefox_prefs() {
	user_js=$1
	[ -f "$user_js" ] || return 0
	# Delete the previous managed block, if any.
	sed -i "/$marker_start/,/$marker_end/d" "$user_js"
}

uninstall_firefox() {
	for prefs in "$USER_HOME"/.mozilla/firefox/*/prefs.js; do
		[ -f "$prefs" ] || continue
		profile=${prefs%/prefs.js}
		rm -rf "$profile/gmp-widevinecdm"
		user_js="$profile/user.js"
		strip_firefox_prefs "$user_js"
		echo "removed Firefox Widevine from $profile"
	done
}

uninstall_chromium() {
	rm -rf "$USER_HOME/.config/chromium/WidevineCdm"
	echo "removed Chromium Widevine tree"
}

if [ "$UNINSTALL" = 1 ]; then
	case "$BROWSER" in
	firefox) uninstall_firefox ;;
	chromium) uninstall_chromium ;;
	both)
		uninstall_firefox
		uninstall_chromium
		;;
	esac
	exit 0
fi

[ $# -eq 1 ] || usage
SRC=$1
[ -d "$SRC" ] || {
	echo "not a directory: $SRC" >&2
	exit 1
}

SO=$(find "$SRC" -type f -name libwidevinecdm.so 2>/dev/null | head -n 1)
MANIFEST=$(find "$SRC" -type f -name manifest.json 2>/dev/null | head -n 1)
[ -n "$SO" ] || {
	echo "libwidevinecdm.so not found under $SRC" >&2
	exit 1
}
[ -n "$MANIFEST" ] || {
	echo "manifest.json not found under $SRC" >&2
	exit 1
}

# ELF e_machine at offset 18, little endian; 183 (0xb7) is AArch64.
machine=$(od -An -tu2 -j18 -N2 "$SO" | tr -d ' ')
[ "$machine" = "183" ] || {
	echo "refusing $SO: ELF machine $machine is not AArch64 (183)" >&2
	exit 1
}

version=$(python3 - "$MANIFEST" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    data = json.load(fh)
print(data.get("version", ""))
PY
)
[ -n "$version" ] || {
	echo "manifest.json has no version field" >&2
	exit 1
}

libc=unknown
if grep -aq 'ld-musl-aarch64' "$SO"; then
	libc=musl
elif grep -aq 'ld-linux-aarch64' "$SO"; then
	libc=glibc
fi

echo "CDM: $SO"
echo "version: $version"
echo "libc: $libc"
if [ "$libc" = glibc ]; then
	echo "WARNING: this CDM is glibc-linked.  pmOS Firefox ESR is musl-linked" >&2
	echo "         and cannot load it.  Use a glibc browser (e.g. a Flatpak" >&2
	echo "         Chromium runtime) or a musl-built ARM64 CDM." >&2
fi

if pgrep -x firefox >/dev/null 2>&1 || pgrep -x firefox-esr >/dev/null 2>&1; then
	echo "WARNING: Firefox is running; close it before installing." >&2
fi

install_firefox() {
	found=0
	for prefs in "$USER_HOME"/.mozilla/firefox/*/prefs.js; do
		[ -f "$prefs" ] || continue
		profile=${prefs%/prefs.js}
		dest="$profile/gmp-widevinecdm/$version"
		mkdir -p "$dest"
		cp -f "$SO" "$dest/libwidevinecdm.so"
		cp -f "$MANIFEST" "$dest/manifest.json"
		chmod 755 "$dest/libwidevinecdm.so"
		chmod 644 "$dest/manifest.json"

		user_js="$profile/user.js"
		strip_firefox_prefs "$user_js"
		{
			printf '%s\n' "$marker_start"
			printf 'user_pref("media.gmp-widevinecdm.visible", true);\n'
			printf 'user_pref("media.gmp-widevinecdm.enabled", true);\n'
			printf 'user_pref("media.gmp-widevinecdm.autoupdate", false);\n'
			printf 'user_pref("media.gmp-widevinecdm.version", "%s");\n' "$version"
			printf '%s\n' "$marker_end"
		} >> "$user_js"

		chown -R "$USER_UID:$USER_GID" "$dest" "$user_js"
		found=1
		echo "installed Firefox CDM in $profile"
	done
	if [ "$found" = 0 ]; then
		echo "no Firefox profile found under $USER_HOME/.mozilla/firefox" >&2
		echo "start Firefox once, close it, then rerun this installer." >&2
		return 1
	fi
}

install_chromium() {
	base="$USER_HOME/.config/chromium/WidevineCdm"
	dest="$base/$version"
	mkdir -p "$dest/_platform_specific/linux_arm64"
	cp -f "$SO" "$dest/_platform_specific/linux_arm64/libwidevinecdm.so"
	cp -f "$MANIFEST" "$dest/manifest.json"
	chmod 755 "$dest/_platform_specific/linux_arm64/libwidevinecdm.so"
	chmod 644 "$dest/manifest.json"
	printf '%s' "$version" > "$base/latest-component-updated-widevine-cdm"
	chown -R "$USER_UID:$USER_GID" "$base"
	echo "installed Chromium CDM in $base"
}

rc=0
case "$BROWSER" in
firefox) install_firefox || rc=$? ;;
chromium) install_chromium ;;
both)
	install_firefox || rc=$?
	install_chromium
	;;
esac

echo
echo "Done.  Test with https://bitmovin.com/demos/drm or"
echo "https://demo.castlabs.com/ and confirm the browser reports L3, not L1."
exit "$rc"
