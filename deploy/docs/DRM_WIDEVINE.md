# Widevine DRM status (Lenovo Tab Plus / TB351FU)

## Summary

| Level | Status | Why |
|-------|--------|-----|
| L1 | **Not possible** | Requires the vendor Trustonic TEE, secure buffers and a secure video/display path that mainline pmOS does not have. |
| L2 | Not possible | Same TEE/secure-path requirements as L1. |
| L3 | **Conditional** | Works only with a user-supplied ARM64 CDM whose C library matches the browser. pmOS ships no proprietary blob. |

pmOS cannot reuse Android's Widevine implementation.  On the Android side the
vendor HAL is running (`ro.vendor.mtk_widevine_drm_l1_support=1`), but it is
built for Android's bionic libc, the Android DRM HAL interfaces and the
Trustonic TEE.  None of that is loadable from a Linux userspace browser.

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
(`e_machine == 183`).  It cannot make an incompatible binary load:

* **Android CDM** (`bionic`): will not load in Firefox/Chromium on pmOS.
* **ChromeOS/glibc CDM**: will not load in pmOS Firefox ESR, which is
  musl-linked.  It can work in a glibc browser runtime (for example a Flatpak
  Chromium), if that runtime exposes the CDM path.
* **musl ARM64 CDM**: the only direct fit for pmOS Firefox ESR.

The installer prints the detected libc and warns on a mismatch.

## Install

1. Start Firefox once so it creates a profile, then close it.
2. Install the CDM as root:

```
sudo pmos-widevine-install.sh --browser firefox /path/to/cdm
```

For Chromium's user-data layout:

```
sudo pmos-widevine-install.sh --browser chromium /path/to/cdm
```

The Firefox install copies the CDM into every profile under
`~/.mozilla/firefox/*/gmp-widevinecdm/<version>/` and writes a managed block
to `user.js`:

```
user_pref("media.gmp-widevinecdm.visible", true);
user_pref("media.gmp-widevinecdm.enabled", true);
user_pref("media.gmp-widevinecdm.autoupdate", false);
user_pref("media.gmp-widevinecdm.version", "<version>");
```

Remove it with:

```
sudo pmos-widevine-install.sh --uninstall --browser firefox
```

## Verify

Open `https://bitmovin.com/demos/drm` or `https://demo.castlabs.com/`.
A working L3 CDM decrypts 720p-class content.  L1-only streams will still
fail; that is expected and is not a configuration problem.

## Why pmOS does not bundle a CDM

Widevine binaries are not redistributable.  Shipping one would also create a
false expectation of L1 support.  The installer is intentionally a thin,
auditable wrapper around a file the user provides.
