#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Install a user-supplied ARM64 Widevine CDM for Firefox and Chromium.
#
# pmOS ships no proprietary Widevine blob.  This helper installs an L3 CDM
# you already own into native and Flatpak browser profiles, after checking
# that it is an AArch64 ELF.  L1/L2 need the vendor Trustonic TEE and a
# secure video path, which mainline pmOS does not provide.
#
# Usage:
#   pmos-widevine-install.sh [--browser firefox|chromium|both] [--user NAME] <cdm-dir>
#   pmos-widevine-install.sh --uninstall [--browser firefox|chromium|both] [--user NAME]
#
# <cdm-dir> must contain libwidevinecdm.so and manifest.json.  ChromeOS-style
# trees (WidevineCdm/<version>/_platform_specific/linux_arm64/...) are also
# accepted.  Without --user the helper uses $SUDO_USER, otherwise the first
# regular account in /etc/passwd.

set -eu

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

BROWSER=both
UNINSTALL=0
USER_NAME=
USER_UID=
USER_GID=
USER_HOME=

usage() {
	cat <<'EOF'
Install a user-supplied ARM64 Widevine CDM for Firefox and Chromium.

Usage:
  pmos-widevine-install.sh [--browser firefox|chromium|both] [--user NAME] <cdm-dir>
  pmos-widevine-install.sh --uninstall [--browser firefox|chromium|both] [--user NAME]

<cdm-dir> must contain libwidevinecdm.so and manifest.json.  ChromeOS-style
trees (WidevineCdm/<version>/_platform_specific/linux_arm64/...) are accepted.
Both native and Flatpak profiles are updated.  Without --user the helper uses
$SUDO_USER, otherwise the first regular account in /etc/passwd.
EOF
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
	--user)
		shift
		USER_NAME=${1:-}
		[ -n "$USER_NAME" ] || {
			echo "--user needs a name" >&2
			usage
		}
		;;
	--user=*)
		USER_NAME=${1#--user=}
		[ -n "$USER_NAME" ] || {
			echo "--user needs a name" >&2
			usage
		}
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
	echo "run as root so the helper can write into the user's home directory" >&2
	exit 1
fi

resolve_user() {
	_name=$1
	_entry=$(awk -F: -v n="$_name" '$1 == n { print; exit }' /etc/passwd)
	if [ -z "$_entry" ]; then
		echo "no such user: $_name" >&2
		exit 1
	fi
	USER_NAME=${_entry%%:*}
	USER_UID=$(printf '%s\n' "$_entry" | cut -d: -f3)
	USER_GID=$(printf '%s\n' "$_entry" | cut -d: -f4)
	USER_HOME=$(printf '%s\n' "$_entry" | cut -d: -f6)
	if [ -z "$USER_HOME" ] || [ ! -d "$USER_HOME" ]; then
		echo "home directory for $USER_NAME not found: ${USER_HOME:-<empty>}" >&2
		exit 1
	fi
}

if [ -n "$USER_NAME" ]; then
	resolve_user "$USER_NAME"
elif [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
	resolve_user "$SUDO_USER"
else
	USER_NAME=$(awk -F: '$3 >= 1000 && $3 < 60000 && $6 != "" && \
		$7 !~ /(nologin|false)$/ { print $1; exit }' /etc/passwd)
	if [ -z "$USER_NAME" ]; then
		echo "no regular user found; pass --user NAME" >&2
		exit 1
	fi
	resolve_user "$USER_NAME"
fi

echo "user: $USER_NAME ($USER_HOME)"

marker_start='# >>> pmos-widevine >>>'
marker_end='# <<< pmos-widevine <<<'

# Native Firefox and both path layouts used by the Flatpak.
firefox_profiles() {
	for _prefs in \
		"$USER_HOME"/.mozilla/firefox/*/prefs.js \
		"$USER_HOME"/.var/app/org.mozilla.firefox/.mozilla/firefox/*/prefs.js \
		"$USER_HOME"/.var/app/org.mozilla.firefox/config/mozilla/firefox/*/prefs.js \
		"$USER_HOME"/.var/app/org.mozilla.firefox/.config/mozilla/firefox/*/prefs.js
	do
		[ -f "$_prefs" ] || continue
		printf '%s\n' "${_prefs%/prefs.js}"
	done
}

# Native Chromium and the Flatpak user-data directory.
chromium_bases() {
	for _base in \
		"$USER_HOME/.config/chromium" \
		"$USER_HOME/.var/app/org.chromium.Chromium/config/chromium"
	do
		if [ -d "$_base" ]; then
			printf '%s\n' "$_base"
		fi
	done
}

strip_firefox_prefs() {
	user_js=$1
	[ -f "$user_js" ] || return 0
	# Delete the previous managed block, if any.
	sed -i "/$marker_start/,/$marker_end/d" "$user_js"
}

copy_file() {
	_src=$1
	_dst=$2
	# Seeding a browser from its own component tree is a no-op, not an error.
	if [ "$_src" -ef "$_dst" ] 2>/dev/null; then
		return 0
	fi
	cp -f "$_src" "$_dst"
}

uninstall_firefox() {
	_profiles=$(firefox_profiles)
	if [ -z "$_profiles" ]; then
		echo "no Firefox profile found for $USER_NAME" >&2
		return 0
	fi
	while IFS= read -r _profile; do
		[ -n "$_profile" ] || continue
		rm -rf "$_profile/gmp-widevinecdm"
		strip_firefox_prefs "$_profile/user.js"
		echo "removed Firefox Widevine from $_profile"
	done <<EOF
$_profiles
EOF
}

uninstall_chromium() {
	_bases=$(chromium_bases)
	if [ -z "$_bases" ]; then
		return 0
	fi
	while IFS= read -r _base; do
		[ -n "$_base" ] || continue
		rm -rf "$_base/WidevineCdm"
		echo "removed Chromium Widevine tree from $_base"
	done <<EOF
$_bases
EOF
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
	echo "WARNING: this CDM is glibc-linked.  Native pmOS browsers" >&2
	echo "         (firefox-esr) are musl-linked and cannot load it." >&2
	echo "         Flatpak Firefox/Chromium use a glibc runtime and can." >&2
fi

if pgrep -x firefox >/dev/null 2>&1 || pgrep -x firefox-esr >/dev/null 2>&1 \
	|| pgrep -x chromium >/dev/null 2>&1; then
	echo "WARNING: close any running Firefox/Chromium before installing." >&2
fi

install_firefox() {
	_profiles=$(firefox_profiles)
	if [ -z "$_profiles" ]; then
		echo "no Firefox profile found for $USER_NAME" >&2
		echo "start Firefox once, close it, then rerun this installer." >&2
		return 1
	fi
	while IFS= read -r _profile; do
		[ -n "$_profile" ] || continue
		_dest="$_profile/gmp-widevinecdm/$version"
		mkdir -p "$_dest"
		copy_file "$SO" "$_dest/libwidevinecdm.so"
		copy_file "$MANIFEST" "$_dest/manifest.json"
		chmod 755 "$_dest/libwidevinecdm.so"
		chmod 644 "$_dest/manifest.json"
		chown -R "$USER_UID:$USER_GID" "$_dest"

		_user_js="$_profile/user.js"
		strip_firefox_prefs "$_user_js"
		{
			printf '%s\n' "$marker_start"
			printf 'user_pref("media.eme.enabled", true);\n'
			printf 'user_pref("media.gmp-widevinecdm.visible", true);\n'
			printf 'user_pref("media.gmp-widevinecdm.enabled", true);\n'
			printf 'user_pref("media.gmp-widevinecdm.autoupdate", false);\n'
			printf 'user_pref("media.gmp-widevinecdm.version", "%s");\n' "$version"
			printf 'user_pref("media.gmp-widevinecdm.forceInstall", true);\n'
			printf '%s\n' "$marker_end"
		} >> "$_user_js"
		chown "$USER_UID:$USER_GID" "$_user_js"
		echo "installed Firefox CDM in $_profile"
	done <<EOF
$_profiles
EOF
}

install_chromium() {
	_bases=$(chromium_bases)
	if [ -z "$_bases" ]; then
		# No existing profile: seed the native user-data directory.
		_bases="$USER_HOME/.config/chromium"
	fi
	while IFS= read -r _base; do
		[ -n "$_base" ] || continue
		_dest="$_base/WidevineCdm/$version"
		mkdir -p "$_dest/_platform_specific/linux_arm64"
		copy_file "$SO" "$_dest/_platform_specific/linux_arm64/libwidevinecdm.so"
		copy_file "$MANIFEST" "$_dest/manifest.json"
		chmod 755 "$_dest/_platform_specific/linux_arm64/libwidevinecdm.so"
		chmod 644 "$_dest/manifest.json"
		# Chromium's hint file is JSON: {"Path":"<cdm base dir>"}.  Writing
		# the bare version here makes Chromium report "no library found".
		python3 - "$_dest" > \
			"$_base/WidevineCdm/latest-component-updated-widevine-cdm" <<'PY'
import json, sys
sys.stdout.write(json.dumps({"Path": sys.argv[1]}))
PY
		chown -R "$USER_UID:$USER_GID" "$_base/WidevineCdm"
		echo "installed Chromium CDM in $_base/WidevineCdm"
	done <<EOF
$_bases
EOF
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
echo "Done.  Restart the browser and confirm EME works at"
echo "https://bitmovin.com/demos/drm or https://demo.castlabs.com/."
echo "This is L3 (software) Widevine; L1 streams will still fail."
exit "$rc"
