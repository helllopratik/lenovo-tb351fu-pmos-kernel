# Widevine DRM status (Lenovo Tab Plus / TB351FU)

## Summary

| Level | Status | Why |
|-------|--------|-----|
| L1 | **Not possible** | Requires the vendor Trustonic TEE, secure buffers and a secure video/display path that mainline pmOS does not have. |
| L2 | Not possible | Same TEE/secure-path requirements as L1. |
| L3 | **Works (verified)** | With a user-supplied ARM64 Widevine CDM in a glibc browser runtime (Flatpak Firefox or Chromium). pmOS ships no proprietary blob. |

DRM is per-browser, not system-wide.  There is no system library that gives a
browser EME support; each browser loads its own CDM copy from its own profile.

## Verified on this device

The CDM used for testing was the component Chromium downloaded:

```
version: 4.10.3112.0
architecture: AArch64 (ELF e_machine 183)
libc: glibc (interpreter /lib/ld-linux-aarch64.so.1)
```

| Browser | Result |
|---------|--------|
| Flatpak Chromium 158.0.8127.36 | `Registering hinted Widevine 4.10.3112.0`, `Media.EME.Widevine.LibraryCdmAvailable = 1`, EME probe returned `WIDEVINE_OK keySystem=com.widevine.alpha`. |
| Flatpak Firefox 160.0.1 | CDM placed in `gmp-widevinecdm/4.10.3112.0/` with the `user.js` prefs below; EME probe returned `WIDEVINE_OK` and `MEDIAKEYS_OK`, with and without `MOZ_DISABLE_GMP_SANDBOX=1`. |
| Native `firefox-esr` | **Cannot load this CDM.** It is musl-linked (`/lib/ld-musl-aarch64.so.1`) while the CDM is glibc-linked. |

`HardwareSecure.Support` is `0`, so this is L3 (software decryption).  L1
streams still fail; that is expected and is not a configuration problem.

## What "L3" needs

An L3 CDM is still a proprietary binary.  The installer expects a directory
containing:

```
libwidevinecdm.so
manifest.json
```

ChromeOS-style trees are accepted as well; the installer searches recursively:

```
WidevineCdm/<version>/_platform_specific/linux_arm64/libwidevinecdm.so
WidevineCdm/<version>/manifest.json
```

### ABI matters more than the architecture

`pmos-widevine-install.sh` verifies that the file is an AArch64 ELF
(`e_machine == 183`) and prints its libc.  It cannot make an incompatible
binary load:

* **Android/bionic CDM**: will not load in any pmOS browser.
* **glibc CDM**: works in Flatpak Firefox and Flatpak Chromium (glibc
  runtimes), but not in native pmOS `firefox-esr` (musl).
* **musl ARM64 CDM**: the only direct fit for native `firefox-esr`.

The helper warns when a glibc CDM is selected so the native-Firefox
limitation is not mistaken for a broken install.

## Installing the helper

The rootfs payload installs `pmos-widevine-install.sh` in `/usr/local/bin`.
It auto-detects the target account:

1. `--user NAME`, if given.
2. `$SUDO_USER`, when run through `sudo`.
3. The first regular account (uid 1000–59999 with a real shell) in
   `/etc/passwd`.

It updates both native and Flatpak profiles that already exist:

```
Firefox (native):   ~/.mozilla/firefox/*/
Firefox (Flatpak):  ~/.var/app/org.mozilla.firefox/{.mozilla,config/mozilla}/firefox/*/
Chromium (native):  ~/.config/chromium/
Chromium (Flatpak): ~/.var/app/org.chromium.Chromium/config/chromium/
```

Start the browser once and close it so the profile exists, then run:

```
sudo pmos-widevine-install.sh --browser firefox /path/to/cdm
sudo pmos-widevine-install.sh --browser chromium /path/to/cdm
sudo pmos-widevine-install.sh --browser both --user hellopratik /path/to/cdm
```

The Firefox install copies the CDM into every profile under
`.../gmp-widevinecdm/<version>/` and writes a managed block to `user.js`:

```
user_pref("media.eme.enabled", true);
user_pref("media.gmp-widevinecdm.visible", true);
user_pref("media.gmp-widevinecdm.enabled", true);
user_pref("media.gmp-widevinecdm.autoupdate", false);
user_pref("media.gmp-widevinecdm.version", "<version>");
user_pref("media.gmp-widevinecdm.forceInstall", true);
```

The Chromium install writes
`.../chromium/WidevineCdm/<version>/_platform_specific/linux_arm64/libwidevinecdm.so`
plus `manifest.json` and the `latest-component-updated-widevine-cdm` marker.

Installed files are chowned to the target user, so the browsers can read and
update their own profiles.  Remove everything with:

```
sudo pmos-widevine-install.sh --uninstall --browser both
```

### Flatpak Firefox

Flatpak Firefox is the supported way to get DRM in Firefox on pmOS because
its runtime is glibc:

```
flatpak install flathub org.mozilla.firefox
flatpak run org.mozilla.firefox    # let it create a profile, then close it
sudo pmos-widevine-install.sh --browser firefox /path/to/cdm
```

A local copy of the CDM can be taken from the Chromium Flatpak component
cache, for example:

```
~/.var/app/org.chromium.Chromium/config/chromium/WidevineCdm/4.10.3112.0
```

### Flatpak Chromium

Chromium normally downloads Widevine itself through its component updater.
If that is unavailable, the helper can seed the same tree from a local copy;
the next Chromium start registers it as a hinted CDM.

## Verify

Open `https://bitmovin.com/demos/drm` or `https://demo.castlabs.com/`.
A working L3 CDM decrypts 720p-class content.

For a scripted check, serve a small EME probe over **HTTP loopback**.
Chromium rejects EME on `file://` ("EME use is not allowed on unique
origins"), so `http://127.0.0.1` is required:

```html
<script>
navigator.requestMediaKeySystemAccess('com.widevine.alpha', [{
  initDataTypes: ['cenc'],
  videoCapabilities: [{contentType: 'video/mp4; codecs="avc1.42E01E"'}]
}]).then(async (a) => {
  await a.createMediaKeys();
  document.title = 'WIDEVINE_OK ' + a.keySystem;
}).catch((e) => { document.title = 'WIDEVINE_FAIL ' + e.name; });
</script>
```

Run the browser headless against that page and check the title/console.
In Chromium's verbose log look for `Registering hinted Widevine` and
`Media.EME.Widevine.LibraryCdmAvailable`.

## Why pmOS does not bundle a CDM

Widevine binaries are not redistributable.  Shipping one would also create a
false expectation of L1 support.  The installer is intentionally a thin,
auditable wrapper around a file the user provides.
