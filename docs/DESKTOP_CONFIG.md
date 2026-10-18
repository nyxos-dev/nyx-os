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
`shadow`, `title_align`, `wallpaper_dim` and (from 6.5.202) `terminal_palette`.
Saving writes all resolved settings;
the `scheme` alias becomes explicit wallpaper/accent values. Unknown keys and
comments are not preserved. Named palette choices clear an earlier hexadecimal
accent override. Reset restores every supported setting to its compiled default
and saves that complete result for the current user.

## Terminal palette

From development version 6.5.207, open the desktop context menu's **Wallpaper**
panel and choose **Classic** or **Nightfall** in its **Terminal** row. The active
choice is highlighted. A left click updates open terminals and saves through the
existing per-user configuration path. The panel retains its 400×520 content size;
the Reset button moves below the new row and still restores all desktop defaults.
Other wallpaper, accent and panel choices remain unchanged when selecting a palette.

If saving fails, the live palette still changes and the existing failure notice
appears. Clicking the selected choice again retries saving. Disk, RAM-only and
failure notices retain their existing meanings; this adds no new configuration
key or output-rendering effects.

Set `terminal_palette = nightfall` in your home `nyx.conf`, then log out and
back in to load it. This solid dark palette changes Erebus's 16 display colours,
canvas, input line and cursor together. ANSI foreground/background indices,
reverse video and stored scrollback attributes retain their meanings. The
default `terminal_palette = classic` preserves the previous VGA colours and
green input line. Unknown names leave the preceding default/system setting
unchanged. Both names are case-sensitive.

The Wallpaper picker preserves this setting when saving other changes; Reset
sets it to `classic`. The palette is shared by GUI terminals in the current
desktop session, not the serial console or separate TTY framebuffer. It adds
no transparency, blur, background allocation or animation. Choosing arbitrary
ANSI foreground/background pairs can still produce low-contrast text; this
setting is not a per-application accessibility override.

`python3 tools/test_terminal_palette.py` exercises the actual ANSI state and
renderer against a recording host framebuffer under ASan/UBSan. It checks both
palettes, all 16 foreground/background selections, canvas and cursor inversion;
the desktop-config suite checks load/save/reset and account isolation.

For 6.5.202, 229 renderer checks and 218 configuration checks passed on the
host. The warning-free kernel passed 229 KATs and a four-CPU login. A one-CPU,
512 MiB QEMU desktop with a private EXT2 disk showed `nightfall`, ran terminal
commands and moved the terminal window. Saving a different wallpaper accent
preserved the palette key; disk extraction and a cold boot confirmed it. Reset
changed the open terminal to `classic`, saved that value and retained it through
another cold boot. Native `ls.elf` directory colours also rendered in both palettes.
Account isolation and the complete ANSI colour matrix are host-test evidence;
these bounded trials do not establish a performance or long-running guarantee.

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

Development version 6.5.207 adds 234 host configuration checks covering the
picker's drawing bounds, translated click boundaries, both choices, failed-save
retry and existing account/reset contracts. The 229 ASan/UBSan terminal-renderer
checks still pass. GCC 14 builds without warnings; QEMU passes 229 kernel tests
and reaches login with four CPUs.

A separate one-CPU GUI trial confirms both buttons and Reset fit the panel,
selection recolours an already-open terminal, commands and window dragging work,
and the selected key preserves the other user preferences on an EXT2 disk.
A cold boot restores Nightfall. Reset restores Classic and the desktop defaults;
the extracted file and a further cold boot confirm that result. Failure injection,
RAM-only notifications and account isolation in this increment are host evidence.
These checks add no performance, soak or physical-hardware qualification.

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
power-loss guarantee. From 6.5.197, RAM replacement allocation failures preserve
the previous file; see [VFS_REPLACEMENT.md](VFS_REPLACEMENT.md). Mounted-filesystem
replacement failures can still damage the previous file.
Readback only verifies what the filesystem reports; it cannot detect an underlying
storage error that the filesystem hides. Same-size concurrent writers and ambiguous
filesystem metadata are not resolved by this increment. No hardware validation or
long-running desktop performance claim follows from these checks.
