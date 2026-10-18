# Desktop configuration

Starting with development version 6.5.196, Hemera loads settings in this order:
compiled defaults, `/etc/nyx.conf`, then `nyx.conf` in the logged-in user's home.
Each login resets every supported setting first, so omitted keys cannot inherit
the previous user's desktop. The system file remains the shared default.

The Wallpaper picker saves to the absolute home path, independent of the terminal's
working directory. Normal disk-backed accounts use `/mnt/home/<user>/nyx.conf`;
without a mounted disk, their `/home/<user>/nyx.conf` remains in RAM. The desktop
notification distinguishes successful disk storage, RAM-only storage and failure.
Changes take effect immediately even if saving fails.

Supported keys remain `wallpaper`, `accent`, `scheme`, `widget`, `widget_pos`,
`clock`, `icons`, `gaps`, `border`, `border_inactive`, `panel_tint`, `rounding`,
`shadow`, `title_align` and `wallpaper_dim`. Saving writes all resolved settings;
the `scheme` alias becomes explicit wallpaper/accent values. Unknown keys and
comments are not preserved. Named palette choices clear an earlier hexadecimal
accent override. Reset restores every supported setting to its compiled default
and saves that complete result for the current user.

Numeric values retain their existing decimal-prefix interpretation and caps:
gaps 64 pixels, panel tint 100 percent, rounding 14 pixels and wallpaper dimming
80 percent. Long digit strings saturate without overflowing signed arithmetic.

Loading requires a complete text image smaller than 2048 bytes, with no embedded
NUL. A malformed or incomplete user image leaves the system/default settings
active. Mounted sizes are checked before reading, without opening an unbounded
VFS file image. Saving checks the filesystem's byte count, rereads the complete result
and compares it with the serialized settings before announcing success. Mounted
readback uses the filesystem callbacks rather than a cached VFS file handle.

## Verification and limits

`python3 tools/test_desktop_config.py` runs the actual configuration and picker
functions with a host VFS that injects write, read, size, directory and missing
callback failures. It checks per-user isolation, full reset, hexadecimal-to-palette
selection, RAM/disk notifications, absolute paths and numeric saturation. The
notification queue is real; its framebuffer repaint is omitted in this host test.

QEMU checks used one CPU, 512 MiB, a 1024x768 framebuffer and a private 16 MiB
EXT2 disk. A hexadecimal user theme loaded, choosing a named palette colour
cleared that override, and the saved file retained every other configured knob.
Logging in as a second user restored defaults; returning restored the first
user's theme. A fresh QEMU process using the same disk restored the saved theme.
Reset wrote all defaults, which also survived a further cold boot. A separate
diskless GUI run showed the RAM-only notice and a visible failure after making
the configuration read-only. These are bounded trials, not a soak test.

The desktop context menu reacts to a new right-button press, so holding that
button opens the menu without immediately running its first action. The Wallpaper
picker is accessible through this menu.

Writes replace the destination directly. This is not an atomic transaction or a
power-loss guarantee, and a failed replacement can damage the previous file.
Readback only verifies what the filesystem reports; it cannot detect an underlying
storage error that the filesystem hides. Same-size concurrent writers and ambiguous
filesystem metadata are not resolved by this increment. No hardware validation or
long-running desktop performance claim follows from these checks.
