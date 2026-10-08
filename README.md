# VideoBG

A tiny native video wallpaper for Windows 10/11, with a built-in desktop clock. The video plays
behind the desktop icons and uses nothing when switched off.

- **Toggle:** `Ctrl + Alt + B` (change it in Settings). Also in the tray icon menu.
- **Settings:** Start menu → *VideoBG*, or click the tray icon.
- **Installed exe:** `C:\Tools\VideoBG\VideoBG.exe` (the app lives in its project folder)

## How on/off works

VideoBG is two processes from the same exe:

| | While ON | While OFF |
|---|---|---|
| Tray app (hotkey, tray icon, desktop clock, settings) | ~3 MB RAM, 0% CPU | ~3 MB RAM, 0% CPU |
| Video renderer | runs (see below) | **not running at all** |

Turning the wallpaper off ends the renderer process, so all of its RAM, GPU memory and decoder
time go back to Windows and your normal wallpaper shows again. Turning it on starts it again
(~0.2 s).

Measured on this laptop (Intel Iris Xe, 1080p screen), renderer process:

| State | CPU | RAM (Task Manager "Memory") | Graphics memory |
|---|---|---|---|
| Playing 4K 60 fps | ~8% of one core | ~25–40 MB | ~300 MB |
| Playing 2560×1072 60 fps | ~8% of one core | ~25–30 MB | ~140 MB |
| Playing 1080p 60 fps (e.g. a "light copy") | ~8% of one core | ~20 MB | ~100 MB |
| Paused, first 30 s (5 s when locked / screen off) | 0% | ~2 MB | as playing: resumes instantly |
| Paused longer | 0% | ~2 MB | ~35 MB: decoder let go, resumes in ~0.1–0.2 s at the same frame |
| Off | nothing running | tray app ~3–4 MB | none |

**Where the memory is.** Almost all of it is the hardware decoder's frames: Windows' decoders
keep ~22 frames whatever the video (a 4K frame is 12 MB, a 1080p one 3 MB), plus two screen-sized
buffers. That's *graphics memory*, which on laptops with integrated graphics is ordinary RAM.
Task Manager's "Memory" column doesn't include it (it's under Details → *Shared GPU memory*), and
that column also moves a lot on its own: Windows trims a process's working set and pages come
back as they're used, so the same playback can read 20 MB or 100+ MB. The box at the bottom of VideoBG's
sidebar shows both: *Wallpaper RAM* (the part that plays the video), *Tray app RAM* and their
*Total RAM* (Task Manager's figures), then *Graphics memory* and the graphics chip it's on. What
actually lowers it:

- **Resolution:** a 4K video on a 1080p screen decodes 4× the pixels it can show. *Video
  versions* (below) makes copies sized for the screen or smaller (~200 MB saved for 4K → 1080p).
- **Pausing:** once paused a while (30 s; 5 s when the PC is locked or the screen is off) the
  decoder is let go and only the last frame stays on screen; resuming reopens the video at the
  same frame (decoding forward from the previous key frame), with the old frame up meanwhile. Not
  while the video's own sound keeps playing.
- **Pipeline:** muted videos always use the leaner silent pipeline (source reader → video
  processor → screen); the media engine is only used for the video's own sound. Measured at 1080p,
  2560×1072 and 4K: same CPU, 15–40 MB less graphics memory and ~10 MB less RAM. (Its "private
  bytes" look higher only because its decoder frames are charged to the process; the engine's are
  shared allocations.)

The settings window uses ~30–40 MB while open (more for a moment while it decodes a 4K preview)
and gives it back when closed.

Decoding runs on the GPU that drives the screen (the Intel iGPU), so the NVIDIA GPU stays asleep.

## Saving battery

- **Pause when covered by** (default *Maximized apps*): when maximized or fullscreen apps cover
  the desktop the video pauses (keeps its last frame, stops decoding) and resumes when you can see
  it again; covered for 30 s, it also lets its decoder memory go (see above). *Fullscreen apps*
  only pauses for games/videos filling the screen; *Nothing* never pauses. The desktop clock and
  other overlays don't count.
- Always pauses when the PC is locked or the screen turns off.
- **On battery:** keep playing / pause / turn off completely (turns back on when plugged in).
- **Frame rate:** cap at 60/30/24 fps to cut the work of showing frames (scaling, presenting).
  It doesn't lower memory or decoding: the file still has every frame, and each one is built from
  the ones before it, so all of them are decoded.
- **Video versions** (*Lighter versions* / *Versions* under the file info): the original and
  lighter copies of it — your screen's size (when the original is bigger) and 900p, 720p, 540p
  (for a 1080p screen; they follow your screen and crop) — each with the memory it takes and
  whether it's made yet. Clicking one shows it on the desktop right away, before anything is
  made: the video playing is scaled down to that size and back up, so you see how soft it would
  look (look around the settings window; needs the video's sound off). *Use it* switches at once
  to a version already made; *Make it* makes it with the GPU's hardware encoder (Shorekeeper,
  13 s, to 720p: ~3 s; a 20 s 4K clip to 1080p: ~6 s) and switches when done. Copies live in
  `%LOCALAPPDATA%\VideoBG\Light copies` (the bin button deletes one); originals are never
  modified. Every version keeps the video's crop, frame and clock look. Measured: Shorekeeper
  (2560×1072, already screen-sized for its crop) 136 MB of graphics memory → 66 MB at 720p.
- Sound *Nothing* (default) never opens an audio stream, so the wallpaper can't keep the PC awake
  or show up in the volume mixer.

## Sound

The **Sound** card picks what plays with the wallpaper:

- **Nothing** — silent; the video's audio isn't even decoded.
- **Video's audio** — the video's own soundtrack, in sync with the picture.
- **My music** — your own songs (*Choose songs*, multi-select; Ctrl+A takes a whole folder).
  They play in order and loop, or shuffled. Songs that Windows can't play are skipped.

*Keep playing when the video pauses*: sound continues while the picture is paused (app covering
the desktop, PC locked, screen off). Sound always stops when the wallpaper is turned off.
Switching between these modes happens in place — the wallpaper doesn't blink.

## Desktop clock

VideoBG draws a desktop clock itself: day, date and time in the design of the "Mond" Rainmeter
skin (Anurati and Quicksand fonts, built into the exe). No Rainmeter needed. It's a small
click-through window pinned to the desktop, above the wallpaper, and stays through Show desktop.
It redraws once a minute; between minutes nothing extra is loaded (the tray app stays ~3 MB).

- **Video wallpaper** look: used while the video wallpaper is on. Every video uses this shared look
  unless you turn on *Just for this video*, which gives that one video (and all its versions) its own
  look. Turning it off makes the video follow the shared look again but keeps its own
  look stored, so turning it back on restores it. Only that video's entry is affected.
- **Still wallpaper** look: used while the video wallpaper is off.
- Switching happens the moment you press the hotkey. The clock is part of VideoBG, so it shows as
  soon as VideoBG starts and goes away when you exit it. *Show the clock* turns it off.
- The preview on the page is drawn by the same code as the desktop clock, so it matches exactly.
- Drag the clock on the preview to place it. It snaps to the centre and to ¼, ½ and ¾ of the
  height (hold Alt to place freely); arrow keys nudge it (Shift for 10 px). While you edit the look
  that's on your desktop, the real clock follows every change before it's saved.
- Each look remembers its own place, colour, size (50–200 %, default 106 % as in Mond), opacity
  and glow. *12-hour / 24-hour* is one setting for all of them.
- **Colour:** swatches; *Pick*, an eyedropper (click it, then the preview: the clock shows the
  colour under the cursor, Esc cancels); Ctrl+V on the page pastes a colour code, Ctrl+C copies
  the current one; or *More* for the colour dialog.
- **Colour dialog** (*More*, and *Glow colour*): a saturation/brightness field and a hue bar, a
  zoomed preview of the clock on your wallpaper, a hex field (type or paste `#72ABDE`, `72ABDE`,
  `#7AD`, `114,171,222` or `rgb(114,171,222)`), copy, and an eyedropper. *My colours* is 16 boxes
  kept in `settings.ini` and shared by all looks: click a box to choose it (the colour you're editing
  stays), *Save to box* puts the current colour in it, double-click (or *Use box colour*) takes a
  box's colour, right-click or Del empties it. Done keeps the colour, Cancel or Esc puts the old one
  back.
- **Glow:** a soft glow behind the letters, from off to 100 %, with its size and colour (the text's
  own colour by default; a dark glow works as a soft shadow on bright wallpapers). *Opacity* makes
  the whole clock see-through.
- A look's place is the centre of the clock text as a share of the main screen.

## Per-video settings

Crop, the chosen frame (*Preview* slider) and the clock look are stored per video in `videos.ini`, keyed by the file's content (a
fingerprint of its size and three small samples), not its path. Rename or move a video and it keeps
its settings; a light copy is linked to its original and shares them.

## Features

- **Videos:** whatever Windows can play: MP4, MOV, MKV, WebM, WMV, AVI, TS/M2TS, MPG, 3GP, in H.264,
  HEVC, AV1, VP9, VP8, MPEG-2/4, WMV… The kind of file is read from its content, so a wrong
  extension doesn't matter. HEVC, AV1, VP9 and MPEG-2 need Windows' codec extensions from Microsoft
  Store (most PCs already have them; all free except HEVC).
- **When a video can't play**, VideoBG says why in plain words, on the Video page and in a tray
  notification (click it to open VideoBG), with the fix one click away: the missing codec extension
  (opens its Store page; e.g. an AV1 video on a PC without the AV1 Video Extension), a file that was
  moved, is still downloading or is in use, a picture / zip / web page saved as a video, a file with
  only sound, a codec Windows can't decode at all (convert it, e.g. with HandBrake), Windows without
  its media features (N editions) or a graphics card problem. The wallpaper turns off; when the fix
  happens outside VideoBG (extension installed, file back), it turns on again by itself as soon as
  the settings window sees the video can be read (coming back to the window checks again).
- **Animated GIFs:** choosing one makes an MP4 of it once (`%LOCALAPPDATA%\VideoBG\Converted`, usually
  under a second) and uses that; the GIF itself is untouched and choosing it again reuses the video.
  Frames are put together as browsers show them (offsets, transparency, disposal; transparent shows
  black), every frame keeps its timing (a steady rate of up to 50 fps), and small GIFs are scaled up
  to 720 lines so their sharp colour edges survive the video encoding.
- **Crop** (the button on the preview; display only, the file is untouched): drag/resize a box on the frame, optional lock to
  your screen's shape, scroll to zoom; the desktop updates live while you drag. Saved per video.
- Scaling: Fill / Fit / Stretch. Speed 0.25×–2×. Volume. All displays or main only.
- **Lock screen and background follow the wallpaper** (*Show on lock screen* on the Video page, on
  by default): the lock screen and the Windows desktop background. Windows only allows still pictures
  there, so while the video wallpaper is on they show the video's frame (the one picked with the
  *Preview* slider, cropped like the wallpaper; each video keeps its own), and while it's off your
  own pictures. The desktop background is what Windows shows wherever the video isn't: behind the
  admin (UAC) prompt, and at sign-in until VideoBG starts, so those match the video too.
  - Windows keeps its lock screen copy where only the system can write, so each switch goes through
    the Windows API (about 0.3–0.5 s); the background takes about 0.05–0.1 s, set the way Windows
    Settings sets it. Both happen in the background and only once the video shows, so turning on
    isn't slowed (the video shows in about 0.25 s). Turning off, the video (and the clock) wait the
    ~0.1 s it takes to put your background back, so the frame never shows through. The frame is kept
    ready as `frame.jpg`; only a new video, frame, crop or screen size makes a new one (under a
    second).
  - *Exit* (tray) puts your own pictures back before closing. Signing out or shutting down keeps the
    frame, since VideoBG starts as it was left (*When app starts*) and shows the video again.
  - After a crash, *End task* or a power cut nothing can run, so they keep what they had; at its
    next start VideoBG makes them match again (within a second). Uninstalling puts yours back too.
  - Whose picture Windows shows is decided from what Windows actually has, not app memory: a lock
    screen picture VideoBG didn't set (by fingerprint) or any background other than its frame is
    yours (the first time, or after you change it in Windows Settings). They're kept
    (`lockscreen-original.img`; the background by its path plus a copy, `background-original.*`)
    for while the wallpaper is off, and Settings' list of recent backgrounds is put back as it was.
  - The background only follows where that's been checked to be harmless: one screen, a single
    picture as the background (not a slideshow, Spotlight or a plain colour, which a picture would
    replace) and the Windows 11 24H2+ desktop. Otherwise only the lock screen follows. Its fit
    (Fill, Fit, ...) is never changed; the frame is screen-sized anyway.
  - Switching it off puts your own pictures back and leaves them alone from then on.
- The settings window has a page per topic (Video, Desktop clock, Sound, Playback, Power saving,
  General). The box at the bottom of the sidebar shows the status, the memory (RAM of the
  wallpaper and of the tray app, their total, and the graphics memory with the graphics chip's
  name) and VideoBG's version, on every page. The Video page goes from choosing the video (top
  right) to the picture (with *Crop* on it), the frame slider with the lock screen line and its
  switch right under it, then the file: always the original's name and details, with the version
  playing on the right in green and *Versions*. A status line shows only while something is being
  made or needs fixing, and the help line at the bottom says what the control under the mouse does.
- The General page links to both data folders and says what's in each; your original videos stay
  where they are.
- Follows the Windows light/dark theme and accent colour. Survives Explorer restarts, display
  changes and sleep/resume.

## Start with Windows

The installer adds VideoBG to your startup apps. Turn it on/off in **Settings → Apps → Startup**
(or Task Manager → Startup apps). The app itself never touches that switch, so there's nothing to
get out of sync. *When app starts* (in VideoBG's settings) picks whether the wallpaper comes up on,
off, or as you left it.

Windows starts the apps in Settings > Apps > Startup one after another, a few seconds after the
desktop appears, waiting for each to settle; shortcuts in the Startup folder come last. To make
them all start right away, set this (Explorer, current user; delete the values to undo):

```powershell
New-Item 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Serialize' -Force | Out-Null
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Serialize' StartupDelayInMSec 0 -Type DWord
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Serialize' WaitForIdleState 0 -Type DWord
```

## Build / install / remove

Needs MinGW-w64 (g++ and windres on PATH) and Python + Pillow only to regenerate the icons.

```powershell
.\build.ps1            # -> dist\VideoBG.exe
.\build.ps1 -Zip       # also dist\VideoBG.zip, to give to someone
.\install.ps1          # put VideoBG.exe in this folder, Start menu shortcut, startup entry
.\uninstall.ps1        # remove the exe and entries; add -RemoveData to also delete settings, light copies and GIF videos
```

To give it to someone: `dist\VideoBG.zip` holds the exe, `How to use.txt` and `Install.cmd` /
`Uninstall.cmd` (from `package\`, running the same install/uninstall scripts): they extract it where
it can stay and run Install.cmd, which uses that folder as the app's home. The exe is
self-contained (statically linked, no runtime needed); settings are created on first run. To
update, no uninstall is needed: Exit VideoBG from the tray, extract the new zip over the old folder
(replace the files) and run Install.cmd again; settings are kept.

Sources are ASCII with CRLF line endings (other characters as `\uXXXX` escapes). Keep
`-fno-devirtualize` in `build.ps1`: with LTO, GCC otherwise turns some calls through
Windows-implemented COM interfaces into crashes.

Command line: `VideoBG.exe` (open settings), `--toggle`, `--exit`, `--startup` (start silently),
`--restore-pictures` (your own lock screen and background back; the uninstaller uses it).

If VideoBG is started from inside an MSIX-packaged app (some terminals and launchers are), Windows
may redirect its `%APPDATA%` writes into that app's private storage, splitting the settings in two.
VideoBG detects this at start and relaunches itself through Explorer (logged in `videobg.log`).

## Files

- Settings: `%APPDATA%\VideoBG\settings.ini` (`[Clock]` the clock looks, time format and your saved
  colours, `[LockScreen]` fingerprint of the picture VideoBG set)
- Per-video settings: `%APPDATA%\VideoBG\videos.ini` (crop, frame, own clock look, light copy → original)
- Your own lock screen picture, for while the wallpaper is off: `%APPDATA%\VideoBG\lockscreen-original.img`
- Log (events only, for troubleshooting): `%APPDATA%\VideoBG\videobg.log` — both processes log
  what they do (on/off, pipeline switches, lock screen steps, music, errors) and a `CRASH` line
  with the code location if anything ever crashes. Windows also keeps crash dumps in
  `%LOCALAPPDATA%\CrashDumps`.
- Music playlist: `%APPDATA%\VideoBG\music.m3u8`
- Light copies and videos made from GIFs: `%LOCALAPPDATA%\VideoBG\Light copies` and `...\Converted`
- The video's frame for the lock screen and background: `%APPDATA%\VideoBG\frame.jpg`; a copy of your own
  background: `background-original.*`

## Source layout

| File | What it does |
|---|---|
| `src/main.cpp` | entry point, single instance, command line |
| `src/host.cpp` | tray icon, hotkey, battery policy, starts/stops the renderer |
| `src/renderer.cpp` | desktop attachment (incl. Windows 11 24H2+ layout), D3D11, video pipelines, auto-pause |
| `src/ui.cpp` | settings window (Direct2D): pages, crop editor, desktop clock editor, colour dialog |
| `src/clock.cpp` | the desktop clock: draws it (DirectWrite, embedded fonts, glow) and keeps it on the desktop |
| `src/mfhelp.cpp` | Media Foundation helpers, frame grabbing, GPU selection |
| `src/optimize.cpp` | "light copy" hardware transcode, animated GIF → MP4 |
| `src/formats.cpp` | which files VideoBG takes; why one can't be played, and the fix |
| `src/lockscreen.cpp` | the lock screen picture following the wallpaper (WinRT, worker thread) |
| `src/settings.cpp` | settings.ini, per-video profiles (videos.ini), log, shared geometry |
