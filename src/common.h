// VideoBG - lightweight video wallpaper for Windows.
// Shared declarations for the host (tray/hotkey/UI) and renderer (video) processes.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef GetCurrentTime  // clashes with IMFMediaEngine::GetCurrentTime
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#define APP_NAME     L"VideoBG"
#define APP_VERSION  L"2.2.0"
#define FEEDBACK_URL L"https://videobg-feedback.vercel.app/api/feedback"
#define APP_REPO     L"https://github.com/Skyblock127/VideoBG"
#define HOST_CLASS   L"VideoBG.Host"
#define RCTL_CLASS   L"VideoBG.RendererCtl"
#define WALL_CLASS   L"VideoBG.Wallpaper"
#define UI_CLASS     L"VideoBG.Settings"
#define CLOCK_CLASS  L"VideoBG.Clock"
#define HOST_MUTEX   L"Local\\VideoBG.Host.SingleInstance"

// Messages exchanged between processes (all posted, never sent).
enum : UINT {
    WM_VBG_RENDERER_READY = WM_APP + 1,  // renderer -> host   lParam = renderer control hwnd
    WM_VBG_RELOAD,                       // host -> renderer   re-read settings.ini
    WM_VBG_QUIT,                         // host -> renderer   shut down cleanly
    WM_VBG_PREVIEW_CROP,                 // host -> renderer   wParam 1 = use lParam crop, 0 = back to saved crop
    WM_VBG_STATE,                        // renderer -> host   wParam = RendererState, lParam = pause mask / HRESULT
    WM_VBG_SHOW,                         // 2nd instance -> host: open settings
    WM_VBG_EXIT,                         // 2nd instance -> host: quit
    WM_VBG_TOGGLE,                       // 2nd instance -> host: toggle on/off
    WM_VBG_TRAY,                         // tray icon callback
    WM_VBG_TRACK,                        // renderer -> host   wParam = playlist index playing (-1 none, -2 error)
    WM_VBG_CLOCK_LOST,                   // clock window -> host: destroyed with the desktop (Explorer restart)
    WM_VBG_LOCKSCREEN,                   // lock screen worker -> host: wParam = HRESULT, lParam = LSF_* flags
    WM_VBG_FAULT,                        // renderer -> host, before RS_ERROR: wParam = VideoFault, lParam = codec (subtype Data1)
    WM_VBG_MEMORY,                       // host -> renderer: report memory; renderer -> host: lParam = graphics memory (KB)
    WM_VBG_PREVIEW_SIZE,                 // host -> renderer   lParam = (h << 16) | w: show the video as if it were that size, 0 = off
    WM_VBG_MUSIC,                        // host -> renderer   wParam = MusicCommand, lParam = its value
    WM_VBG_MUSIC_POS,                    // renderer -> host   wParam = position (ms), lParam = MusicPack(duration, track, playing)
    WM_VBG_MUSIC_END,                    // renderer -> host   the playlist ended (repeat off): the music stops
    WM_VBG_VOLUME,                       // host -> renderer   wParam = volume (0-100) while the slider moves (saved when let go)
    WM_VBG_SEEK,                         // host -> renderer   wParam = position (ms); lParam 1 = that exact frame, 0 = the key frame before (dragging)
    WM_VBG_SHOWCASE,                     // host -> renderer   wParam 1 = show the wallpaper full screen just under window lParam (the picker), 0 = stop
    WM_VBG_SHOWCASE_UP,                  // renderer -> host   the showcase has its first frame on screen
    WM_VBG_PREVIEW_UP,                   // renderer -> host   after a preview switch: wParam 1 = its first frame is up, 0 = it couldn't play
    WM_VBG_PICKER,                       // 2nd instance -> host: open the wallpaper picker (--wallpapers)
    WM_VBG_SHOWCASE_DIM,                 // host -> renderer   a wallpaper being chosen: wParam 0 = the showcase as usual, else the brightness the
                                         //                    rest dims to (1-1000) | ms it takes << 16; lParam = when it starts (QueryPerformanceCounter)
    WM_VBG_SHOWCASE_BOX,                 // host -> renderer   ... the chosen video again, full bright, in this box: lParam = PackBox (pixels)
    WM_VBG_SHOWCASE_GROW,                // host -> renderer   ... the box grows to fill the screen: wParam = ms it takes, lParam = when it starts
    WM_VBG_HOLD,                         // host -> renderer   wParam 1 = hold the video still (the picker's frame slider), 0 = let it play
    WM_VBG_MEDIA_KEY,                    // media controls -> host: wParam = the button (a keyboard's play/pause, next, previous)
    WM_VBG_UPDATE,                       // update thread -> host: wParam = the UP_* stage it's at
    WM_VBG_NOWPLAYING,                   // media sessions -> host: another app's music changed (have a look)
};
// A box on the showcase (target pixels), 16 bits a side.
inline LPARAM PackBox(const RECT& r) {
    auto u = [](LONG v) { return (UINT64)(v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v); };
    return (LPARAM)(u(r.left) | u(r.top) << 16 | u(r.right) << 32 | u(r.bottom) << 48);
}
inline RECT UnpackBox(LPARAM p) {
    const UINT64 v = (UINT64)p;
    return RECT{(LONG)(v & 0xFFFF), (LONG)(v >> 16 & 0xFFFF), (LONG)(v >> 32 & 0xFFFF), (LONG)(v >> 48 & 0xFFFF)};
}
// WM_COPYDATA host -> renderer (the one message that's sent): play this video in place of the
// wallpaper's own (the picker previewing it), from `time` seconds; an empty path goes back.
constexpr ULONG_PTR COPY_PREVIEW_VIDEO = 0x56424731;
struct PreviewVideoData {
    double time;
    UINT seq;         // the host's number for this request: WM_VBG_PREVIEW_UP answers with it in lParam
    wchar_t path[1];  // null-terminated, as long as needed
};
enum MusicCommand : WPARAM { MC_NEXT = 1, MC_PREV, MC_SEEK /* lParam = ms */, MC_PLAY_INDEX /* lParam = playlist index */ };
inline LPARAM MusicPack(UINT durMs, int track, bool playing) {
    return (LPARAM)((UINT64)durMs | ((UINT64)(UINT)(track + 2) << 32) | ((UINT64)playing << 48));
}
enum : LPARAM {
    LSF_LOCKSCREEN = 1,  // the lock screen shows the video's frame
    LSF_BACKGROUND = 2,  // the desktop background shows it
    LSF_EARLY = 4,       // the background is the user's again; the lock screen is still being done
};

enum RendererState : WPARAM { RS_LOADING = 0, RS_PLAYING = 1, RS_PAUSED = 2, RS_ERROR = 3 };
enum PauseReason : LPARAM { PR_COVERED = 1, PR_LOCKED = 2, PR_DISPLAY_OFF = 4, PR_BATTERY = 8, PR_HOLD = 32 };

constexpr DWORD EXIT_RENDER_RESTART = 3;  // renderer asks the host to start it again
constexpr DWORD EXIT_RENDER_ERROR = 4;    // renderer could not play the video

struct Crop {
    float l = 0, t = 0, r = 1, b = 1;  // normalized to the video frame
    bool stretch = false;  // a free shape, stretched to fill the screen (else it's trimmed to the screen's shape)
    bool IsFull() const { return l <= 0.0005f && t <= 0.0005f && r >= 0.9995f && b >= 0.9995f; }
    bool operator==(const Crop& o) const { return l == o.l && t == o.t && r == o.r && b == o.b && stretch == o.stretch; }
    bool operator!=(const Crop& o) const { return !(*this == o); }
};

// Where and how the desktop clock shows for one kind of wallpaper.
struct ClockLook {
    float x = 0.5f, y = 0.25f;       // centre of the clock, as a share of the main screen
    std::wstring color = L"0,0,0";   // "r,g,b"
    float size = 1.06f;              // scale of the clock design (1.06 = the Mond skin's)
    float opacity = 1;               // 0.2 .. 1, the whole clock
    float glow = 0;                  // 0 (none) .. 1 (strongest)
    float glowSize = 12;             // how far the glow reaches, in DIPs at size 1
    std::wstring glowColor;          // "r,g,b", "" = the text colour
    bool show = true;                // the clock shows at all with this look
    bool h24 = false;                // 24-hour time (else 12-hour)
    std::wstring style;              // its design (a key from Clock_Styles): "" = the Mond clock, in `font`
    std::wstring font = L"Audiowide";  // the Mond clock's font (a key from Clock_Fonts)
    int date = 0;                    // how the date line reads (a Clock_DateText style)
    std::wstring lang;               // the language of its words (a Windows locale name, "" = English)
    bool nativeDigits = false;       // the language's own digits (when it has them) instead of 0-9
    float rotate = 0;                // degrees clockwise (-180 .. 180)
    bool operator==(const ClockLook& o) const {
        return x == o.x && y == o.y && color == o.color && size == o.size && opacity == o.opacity && glow == o.glow &&
               glowSize == o.glowSize && glowColor == o.glowColor && show == o.show && h24 == o.h24 && style == o.style &&
               font == o.font && date == o.date && lang == o.lang && nativeDigits == o.nativeDigits && rotate == o.rotate;
    }
};

// A saved clock look, for putting on any of the looks above (all wallpapers and videos share them).
struct ClockPreset {
    bool used = false;   // an empty one has nothing to use yet
    std::wstring name;   // "" = "Preset N"
    ClockLook look;      // its show is always on: Show / Hide stays with the look it's used for
};
constexpr int kClockPresets = 5;

struct Settings {
    std::wstring video;
    UINT hkMods = MOD_CONTROL | MOD_ALT;
    UINT hkVk = 'B';
    UINT musicHkMods = 0;       // the sound shortcut (Pause/Break): plays / pauses My music, mutes the video's audio; key 0 = none
    UINT musicHkVk = VK_PAUSE;
    int monitors = 0;     // 0 all, 1 primary only
    int gpu = 0;          // graphics chip: 0 the one driving the main display, 1 power saving, 2 high performance
    int speed = 100;      // playback speed in percent
    int volume = 60;      // 0..100, for whichever sound source is chosen
    int sound = 0;        // 0 off (audio never decoded), 1 the video's own audio, 2 music
    std::wstring musicSource; // the music (sound 2): "" my music, else the app it comes from (its app id, as Windows' media controls name it)
    bool musicResume = false; // my music was playing when another app became the source: it plays again when it's back
    std::vector<int> musicOrder;  // my music: the play order (playlist indices); empty = as chosen
    int musicRepeat = 1;      // my music: 1 repeats the playlist, 2 repeats the song
    bool musicPlaying = true; // my music: playing (else paused by the user)
    int musicTrack = -1;      // my music: the song it was at when the wallpaper stopped (-1 = the first)
    UINT musicPosMs = 0, musicDurMs = 0;  // ...how far into it, and how long it is
    bool keepSound = true;    // sound keeps playing while the wallpaper is paused (covered / locked)
    bool videoMuted = false;  // the video's audio muted by the sound shortcut (the video plays on, in sync)
    int fpsCap = 0;       // 0 = native frame rate
    int pauseCover = 2;   // 0 never, 1 when fullscreen app, 2 when maximized or fullscreen app
    int onBattery = 0;    // 0 keep playing, 1 pause, 2 turn off
    int launch = 1;       // state at launch: 0 on, 1 as it was left (lastOn), 2 off
    bool lastOn = true;
    double previewTime = -1;  // seconds; the video's frame for previews and the lock screen (-1 = auto; stored per video)
    bool lockFollow = true;   // the lock screen shows the video's frame while the wallpaper shows, the user's own picture otherwise
    int pipeline = 0;         // 0 auto, 1 always media engine, 2 always silent reader (INI only)
    int look = 0;             // 0 native (the classic settings window), 1 modern (the wallpaper picker)
    int musicPlayer = 0;      // the modern look's music player on the desktop: 0 none, else 1 + its style (with My music)
    float playerX = 0.5f, playerY = 0.9f;  // its centre, as a share of the main screen
    struct PlayerLook {
        std::wstring color;   // "r,g,b" for its lit parts (the play button, its place); "" = the Windows accent colour
        std::wstring body;    // "r,g,b" for the player itself (its card or case); "" = the style's own
        float opacity = 1;    // 0.2 .. 1, the whole player
        float corners = -1;   // its card's corner radius (DIPs); -1 = the style's own
        float size = 1;       // 0.7 .. 1.5 of the style's own size
        float rotate = 0;     // turned this many degrees clockwise (-180 .. 180)
        bool IsDefault() const { return color.empty() && body.empty() && opacity >= 1 && corners < 0 && size == 1 && rotate == 0; }
    };
    PlayerLook playerLooks[17];  // each style's, by musicPlayer number ([Player] Style<n>Color, ...Opacity, ...Corners)
    Crop crop;                // crop for the current video (stored per video)

    // Desktop clock: one look while the video wallpaper shows, one for the still wallpaper. A video
    // can have its own look instead of clockLive. Everything about the clock is part of its look.
    ClockLook clockStill{0.5f, 0.25f, L"0,0,0"};
    ClockLook clockLive{0.5f, 0.5f, L"255,255,255"};
    bool clockVideoOwn = false;   // the current video uses its own look (stored per video)
    bool clockVideoSaved = false; // it has one stored (kept while switched off)
    ClockLook clockVideo;
    std::wstring myColors[16];    // the colour picker's saved colours, "r,g,b" or "" (all looks share them)
    ClockPreset presets[kClockPresets];  // the clock presets ([Presets] in settings.ini)
};

// settings.cpp
std::wstring AppDataDir();    // %APPDATA%\VideoBG (created on demand): settings, log
std::wstring LocalDataDir();  // %LOCALAPPDATA%\VideoBG (created on demand): light copies
void Log(const wchar_t* fmt, ...);  // appends to %APPDATA%\VideoBG\videobg.log (events only, never per frame)
std::wstring LogPath(bool old = false);  // the log, or the one before it (videobg.old.log)
std::wstring FeedbackUrl();               // where feedback goes: FEEDBACK_URL, or settings.ini [General] FeedbackUrl

// feedback.cpp: a feature idea or a problem report, sent to the VideoBG-feedback server.
struct FeedbackPicture {
    std::wstring name;  // the file's name, or "Pasted picture"
    std::string jpeg;   // made smaller (1600 px at most) and turned into a JPEG
    std::vector<uint32_t> thumb;  // a small copy (premultiplied BGRA, 144 px tall at most) for the dialog
    int thumbW = 0, thumbH = 0;
};
struct FeedbackReport {
    bool bug = false;
    std::wstring text, steps, often, email, system, log;
    std::vector<FeedbackPicture> pictures;
};
constexpr size_t kFeedbackPictures = 3;  // pictures a report can carry
// A picture for a report, from a file or a bitmap (a pasted screenshot); false if it can't be read.
bool Feedback_Picture(const std::wstring& file, HBITMAP bitmap, FeedbackPicture* out);
std::wstring Feedback_Windows();                 // "Windows 11 24H2 (build 26200.6899)"
std::wstring Feedback_Log();                     // the newest part of the log, user and file names taken out
std::wstring Feedback_Preview(const FeedbackReport& r);  // what's sent, as plain text
bool Feedback_Send(const FeedbackReport& r, std::wstring* error);  // blocking: call it off the UI thread
void InstallCrashLog(const wchar_t* role);  // logs module+offset of any crash before Windows' crash report
std::vector<std::wstring> LoadPlaylist();   // %APPDATA%\VideoBG\music.m3u8
void SavePlaylist(const std::vector<std::wstring>& files);
// A list of files, one per line in UTF-8 (the playlist, the modern look's wallpapers). Missing: false.
bool ReadFileList(const std::wstring& path, std::vector<std::wstring>* out);
bool WriteFileList(const std::wstring& path, const std::vector<std::wstring>& files);
inline bool WantVideoAudio(const Settings& s) { return s.sound == 1 && s.volume > 0; }
// The play order for a playlist of n songs: the saved one if it fits, else the songs as chosen.
inline std::vector<int> MusicOrderFor(const Settings& s, size_t n) {
    std::vector<int> o = s.musicOrder;
    std::vector<bool> seen(n, false);
    bool ok = o.size() == n;
    for (int i : o) ok = ok && i >= 0 && (size_t)i < n && !seen[i] && (seen[i] = true);
    if (ok) return o;
    o.resize(n);
    for (size_t i = 0; i < n; i++) o[i] = (int)i;
    return o;
}
// The playlist's search, forgiving of spelling: the fewest letters to change, add, drop or swap round
// to turn q into name (whole: all of it; else any part of it). Case, spaces and marks don't count.
inline int SearchCost(const std::wstring& name, const std::wstring& q, bool whole = false) {
    auto plain = [](const std::wstring& s) {
        std::wstring o;
        for (wchar_t c : s)
            if (iswalnum(c)) o += towlower(c);
        return o;
    };
    const std::wstring a = plain(name), b = plain(q);
    const size_t m = b.size();
    std::vector<int> p2(m + 1), p1(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; j++) p1[j] = (int)j;
    int best = p1[m];
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = whole ? (int)i : 0;  // (a part may start anywhere)
        for (size_t j = 1; j <= m; j++) {
            int v = std::min(p1[j - 1] + (a[i - 1] != b[j - 1] ? 1 : 0), std::min(p1[j], cur[j - 1]) + 1);
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) v = std::min(v, p2[j - 2] + 1);
            cur[j] = v;
        }
        best = std::min(best, cur[m]);
        std::swap(p2, p1);
        std::swap(p1, cur);
    }
    return whole ? p1[m] : best;
}
// The best match for q among names: its index, or -1 if none is close. Closest part first; of those,
// the name closest as a whole (typing "sial" finds "Sail", not "California Love").
inline int SearchBest(const std::vector<std::wstring>& names, const std::wstring& q) {
    int n = 0;
    for (wchar_t c : q) n += iswalnum(c) != 0;
    if (!n) return -1;
    const int most = n / 3 + 1;  // (about one slip in three letters)
    int best = -1, score = 0;
    for (size_t i = 0; i < names.size(); i++) {
        const int c = SearchCost(names[i], q);
        if (c >= most) continue;
        const int sc = c * 1000 + std::min(999, SearchCost(names[i], q, true));
        if (best < 0 || sc < score) {
            best = (int)i;
            score = sc;
        }
    }
    return best;
}
inline bool MusicShuffled(const std::vector<int>& o) {
    for (size_t i = 0; i < o.size(); i++)
        if (o[i] != (int)i) return true;
    return false;
}
void LoadSettings(Settings& s);
void SaveSettings(const Settings& s);
void SaveMyColors(const Settings& s);  // just the colour dialog's saved colours
void SavePresets(const Settings& s);   // just the clock presets
void LoadVideoProfile(Settings& s);  // crop, frame and clock look of s.video
std::wstring VideoVersion(const std::wstring& original);  // the lighter version its card plays ("": the original)
void SetVideoVersion(const std::wstring& original, const std::wstring& version);
void SaveVideoClock(const std::wstring& video, const ClockLook& look);  // another video's own clock (not the wallpaper's)
Crop LoadCropFor(const std::wstring& video);
// Per-video data (videos.ini: crop, clock look) is keyed by the file's content, so renaming or
// moving a video keeps it, and a light copy shares its original's entry.
std::wstring VideoId(const std::wstring& path);         // content fingerprint, "" if unreadable
std::wstring VideoProfileId(const std::wstring& path);  // VideoId, or the original's for a light copy
void LinkLightCopy(const std::wstring& copy, const std::wstring& original);
std::wstring LightCopyOriginalPath(const std::wstring& copy);  // "" if unknown or no longer there
bool IsLightCopyOf(const std::wstring& copy, const std::wstring& original);
std::vector<std::wstring> KnownVideos();  // videos used before (videos.ini), still there, light copies left out
// Extra settings.ini sections that aren't part of Settings ([LockScreen], [Clock]).
std::wstring ReadState(const wchar_t* section, const std::wstring& key);
void WriteState(const wchar_t* section, const std::wstring& key, const wchar_t* value);  // nullptr deletes
std::wstring PathKey(const std::wstring& path);  // stable INI key for a file path
std::wstring HotkeyToString(UINT mods, UINT vk);
LPARAM PackCrop(const Crop& c);
Crop UnpackCrop(LPARAM p);
std::wstring ExePath();
double PreviewTimeFor(double setting, double duration);  // the frame to use: the chosen one, else ~10% in (at most 3 s)

// Geometry shared by the renderer and UI previews.
struct FitRect {
    float sl, st, sr, sb;  // normalized source rect inside the video frame
    float dl, dt, dr, db;  // destination rect inside the target, in target units
};
FitRect ComputeFit(double videoAspect, const Crop& c, int scaleMode, double tw, double th);
// How a video's crop fills the screen: trimmed to the screen's shape (0), or stretched (2).
inline int CropMode(const Crop& c) { return c.stretch ? 2 : 0; }

// Entry points
int HostMain(HINSTANCE inst, bool openSettings, bool openPicker = false, int wallpaperOn = -1);  // wallpaperOn: after an update, as it was

// update.cpp: the newest release from GitHub, in place of this copy.
enum { UP_IDLE, UP_CHECKING, UP_OFFLINE, UP_FAILED, UP_LATEST, UP_DOWNLOADING, UP_INSTALLING, UP_RESTART };
void Update_Start(HWND notify, bool quiet);  // checks, then downloads and puts it in place; notify gets WM_VBG_UPDATE
                                             // quiet: shows nothing unless there's a new version, then asks first
int Update_Stage(int* percent, std::wstring* latest);
bool Update_Available();          // a newer version is out (found by a check)
void Update_Done();               // an answer (offline, up to date, failed) has been shown long enough
void Update_Relaunch(bool wallpaperOn);  // starts the new exe (after this one has let go of its mutex)
void Update_CleanUp();            // the new exe: deletes the old one and the download
int RendererMain(HINSTANCE inst, HWND host);

// Host services used by the settings UI (host.cpp)
extern Settings g_settings;
bool Host_IsOn();
void Host_SetOn(bool on);
bool Host_SetHotkey(UINT mods, UINT vk);
bool Host_SetMusicHotkey(UINT mods, UINT vk);  // vk 0 = none; false if another app has it
void Host_SettingsChanged();                 // save + tell renderer to reload
void Host_SetVideo(const std::wstring& path, const Crop* keepCrop = nullptr);
void Host_PreviewCrop(const Crop* c);        // nullptr = end preview
void Host_PreviewSize(UINT w, UINT h);       // the wallpaper as a w x h version of the video would look; 0, 0 = end
RendererState Host_RendererState(LPARAM* pauseMask);
bool Host_DesktopUnseen();                   // covered, locked or the screen off (the flip clock and the player stop moving)
bool Host_BatteryOff();                      // off only because of the "on battery: turn off" policy
bool Host_RendererMemory(SIZE_T* privateWs, SIZE_T* graphics);  // graphics: decoder frames, swap chains (RAM on most laptops)
SIZE_T Host_SelfMemory();
bool Host_StartupRegistered(bool* enabledInWindows);
void Host_RegisterStartup();
void Host_TrimMemory();
bool Host_PickVideo(HWND owner);
bool Host_PickMusic(HWND owner, bool add);   // choose songs for "my music" (add: onto the end of the playlist)
const std::vector<std::wstring>& Host_Songs();  // the playlist, as both looks and the player show it
void Host_MusicCommand(MusicCommand cmd, LPARAM value);  // next, previous, seek, play a song
void Host_LiveVolume(int volume);            // the sound follows the volume slider before it's saved
void Host_SeekWallpaper(double seconds, bool exact);  // the wallpaper follows the Preview slider
void Host_KeepMusicPlace();                  // remember the song and spot (leaving My music; the wallpaper stopping)
void Host_PlayMusic(bool play);              // My music: play or pause (kept, as the play button does)
void Host_SkipMusic(bool next);              // the next or previous song (skipping means listening)
void Host_ShuffleMusic(size_t songs);        // shuffle on (a new order, the song playing first) or off (the order chosen)
void Host_PlaceSong(size_t songs, int song, int to);  // puts a song at place `to` in the play order
void Host_RemoveSong(int song);  // takes a song off the playlist (the file stays); the next one plays if it was playing
struct MusicState {
    int track = -1;       // playlist index playing (-1 none)
    bool playing = false; // actually playing now
    UINT posMs = 0, durMs = 0;
};
MusicState Host_MusicState();
int Host_CurrentTrack();                     // playlist index playing, -1 none, -2 couldn't play
void Host_SuspendHotkey(bool suspend);       // while the settings UI records a new shortcut (both shortcuts)
bool Host_ClockLive();                       // true while the clock should use the video wallpaper look
void Host_ClockMove(const ClockLook& look);  // move the clock right now without saving (while dragging)
void Host_ClockPreview(const ClockLook* look);  // show this look instead of the saved ones (a preset being edited); nullptr ends it
void Host_SetLockFollow(bool on);            // the lock screen follows the wallpaper (off: the user's picture, left alone)
void Host_FrameChanged();                    // the video's frame (preview time) changed: save it, update the lock screen
bool Host_LockScreen(HRESULT* lastResult, int* lsf);  // true while it's being updated; lsf: what shows the frame

void Host_VideoPlayable(const std::wstring& path);  // the UI could read it: resume if it had failed to play
// The modern look's picker: the wallpaper full screen just under `under` (nullptr = stop), and another
// video playing in its place ("" = the wallpaper's own again; seq comes back with WM_VBG_PREVIEW_UP).
void Host_Showcase(HWND under);
// Choosing a wallpaper: the showcase dims to `bright` (over dimFor seconds from dimAt), plays the
// chosen video again in `box` (pixels; empty = no box yet), and from growAt (0 = not yet) the box
// grows to fill the screen over growFor seconds. The renderer animates it by itself at the display's
// refresh; times are seconds on QueryPerformanceCounter's clock, which every process shares.
struct ShowcaseLook {
    float bright = 1;
    double dimAt = 0, dimFor = 0.5;
    RECT box{};
    double growAt = 0, growFor = 0.6;
};
void Host_ShowcaseLook(const ShowcaseLook* look);  // nullptr: the showcase as usual
void Host_HoldWallpaper(bool hold);                          // the video holds still (while a frame is picked), then plays on
void Host_SetLook(int look);  // 0 classic, 1 modern
void Host_Update();           // check for a new version and install it
void Host_PreviewVideo(const std::wstring& path, double time, UINT seq);
struct FaultText;
bool Host_PlayFailed(FaultText* why);               // the wallpaper was turned off because the video couldn't play

// ui.cpp
void UI_Show();
void UI_Refresh();
bool UI_IsOpen();
void UI_ConvertGif(const std::wstring& gif);  // opens the settings and turns the GIF into a video, then uses it
void UI_VideoFailed();                        // the video couldn't play: look at the file again
struct LightCopy {                            // a lighter version of a video, made by VideoBG
    std::wstring path;
    UINT w = 0, h = 0;
    ULONGLONG bytes = 0;
};
std::vector<LightCopy> UI_LightCopiesOf(const std::wstring& original);
void UI_ShowVersions();                       // the settings window, at the wallpaper's versions

// modern.cpp - the modern look's wallpaper picker (the host's message loop runs its frames).
bool Modern_Open(int mode);   // 0 picking a wallpaper, 1 placing the clock; false if it couldn't open
void Modern_Close();
void Modern_Toggle();
bool Modern_IsOpen();
HANDLE Modern_FrameWait();    // signalled when the picker can draw its next frame; nullptr while nothing moves
void Modern_Frame();
void Modern_RendererEvent(UINT msg, WPARAM w, LPARAM l);  // the renderer's showcase / preview news
void Modern_SettingsChanged();  // the settings changed (in the picker or the settings window): show them as they are now
void Modern_MusicChanged();     // the song or its place changed

// formats.cpp - which files VideoBG takes, and why one can't be played.
extern const wchar_t kVideoPatterns[];  // for the open dialog
enum VideoFault : int {
    VF_NONE,
    VF_MISSING,     // the file isn't there
    VF_UNREADABLE,  // Windows won't let us read it (in use, no permission, cloud file offline)
    VF_EMPTY,       // empty, or still downloading
    VF_GIF,         // an animated GIF: VideoBG turns it into a video first
    VF_PICTURE,     // a still picture
    VF_NOT_VIDEO,   // nothing Windows recognises as a video
    VF_CONTAINER,   // a video file type Windows can't open (FLV, RealMedia)
    VF_SOUND_ONLY,  // opens, but there's no picture in it
    VF_NO_DECODER,  // this PC has no decoder for its codec
    VF_NO_MEDIA,    // Windows has no media features (an "N" edition without the Media Feature Pack)
    VF_GPU,         // the graphics card couldn't be used
    VF_OTHER,
};
struct FaultText {
    std::wstring what;      // what's wrong, one line
    std::wstring fix;       // what to do about it, one line ("" if nothing)
    std::wstring linkText;  // a button for the fix ("" if none)
    std::wstring link;      // Store page or Settings page it opens
};
VideoFault SniffFile(const std::wstring& path, std::wstring* kind);  // file-level problems; kind: "PNG", "FLV", "archive"
std::wstring CodecLabel(DWORD codec);  // "AV1", "HEVC"... from a Media Foundation subtype's Data1
FaultText DescribeFault(int fault, DWORD codec, HRESULT hr, const std::wstring& path);
void OpenFixLink(HWND owner, const std::wstring& link);

// library.cpp - the modern look's wallpapers: the videos to pick from, and a picture of each.
std::vector<std::wstring> LoadWallpapers(const std::wstring& current);  // the first time: the videos used before; current is always in it
void SaveWallpapers(const std::vector<std::wstring>& files);           // %APPDATA%\VideoBG\wallpapers.m3u8
struct WallpaperInfo {           // what the picker shows for one video (made off the UI thread, kept on disk)
    std::wstring path;
    bool ok = false;             // a picture could be made, else `fault` says why not
    int fault = 0;               // a VideoFault
    std::vector<uint32_t> thumb; // a small picture for its card (BGRX)
    int tw = 0, th = 0;
    std::vector<uint32_t> frame; // the whole frame, at most the screen's size (only when asked for)
    int fw = 0, fh = 0;
    double time = 0;             // the frame's time: the video's preview frame
    UINT32 vw = 0, vh = 0;
    double duration = 0, fps = 0;
    std::wstring codec;
    bool sound = false;
    ULONGLONG bytes = 0;
    UINT32 palette[5] = {};      // its main colours (0xRRGGBB), most used first
};
// Results are posted to `notify` as msg with lParam = WallpaperInfo* (the receiver deletes it).
void Library_Start(HWND notify, UINT msg, int screenW, int screenH);
void Library_Request(const std::wstring& path, bool frame, bool urgent);
void Library_Stop();

// lockscreen.cpp - the lock screen picture and the desktop background follow the wallpaper.
struct LockScreenWant {
    bool frame = false;        // true: a frame of the video; false: the user's own pictures
    bool background = false;   // with frame: the desktop background too (once the video is up)
    std::wstring video;
    double time = -1;          // seconds into the video (-1 = the default frame)
    Crop crop;
    int sw = 1920, sh = 1080;  // screen size to make the picture for
};
// Asynchronous (a worker thread; the latest request wins). Posts msg to notify when done (and
// early, with LSF_EARLY, once the background is the user's again).
void LockScreen_Sync(HWND notify, UINT msg, const LockScreenWant& want);
bool LockScreen_Wait(DWORD ms);            // false if still busy after ms
bool LockScreen_WaitBackground(DWORD ms);  // until the background is the user's again
bool LockScreen_Busy();
std::wstring OwnDesktopBackground();       // the user's background picture, also while the frame stands in

// clock.cpp - the desktop clock (day, date, time; the "Mond" clock design), drawn by VideoBG.
struct ClockImage {
    std::vector<uint32_t> px;  // premultiplied BGRA, w x h, the clock centred in it
    int w = 0, h = 0;
    int pad = 0;               // room for the glow on each side (the text box is w - 2 pad by h - 2 pad)
};
// pixelScale: pixels per 96-DPI unit. Loads DirectWrite on first use; Clock_ReleasePainter frees it.
// flip: a flip clock's flaps falling over to this minute, how far (0-1; 1: still).
bool Clock_Paint(const ClockLook& look, float pixelScale, const SYSTEMTIME& t, ClockImage* out, float flip = 1);
bool Clock_Flips(const ClockLook& look);
// Turning the clock by its handle: the angle (degrees clockwise, -180..180) from a start, snapped to
// 15-degree steps near them (and harder to upright and the quarter turns), unless free.
float Clock_RotateTo(float startRotate, float startPointer, float pointer, bool free);
float Clock_PointerAngle(float cx, float cy, float x, float y);  // the pointer's angle round the clock's centre  // its style flips over at each minute (Clock_Paint's flip)
constexpr double kClockFlipSeconds = 0.6;
void Clock_ReleasePainter();
void Clock_Show(HWND host, const ClockLook& look);  // create / update / move
void Clock_Hide();
void Clock_Refresh();   // time jumped, display changed, woke from sleep: redraw and re-place
float Clock_PixelScale();
std::wstring Clock_StyleKey(const ClockLook& look);  // everything that changes the picture (not the place)

// Desktop music player (player.cpp): the song and its controls, on the desktop, in a style.
void Player_Show(const std::vector<std::wstring>& songs, bool force);  // create / update (songs: the playlist)
void Player_Update();  // the song or its place changed: redrawn if it shows something new
void Player_Place();   // it was moved (Settings::playerX, playerY): the window goes there
void Player_Hide();
std::vector<std::wstring> Player_Styles();  // by name: Settings::musicPlayer is 0 for none, else 1 + one of these
// A style's parts, where the pointer is (x, y in the style's DIPs, as Player_Size gives them).
enum PlayerPart { PLAYER_NONE, PLAYER_PREV, PLAYER_PLAY, PLAYER_NEXT, PLAYER_VOLUME, PLAYER_SEEK, PLAYER_BODY };
struct PlayerInput {  // the pointer on a preview: what it's over or holds, the place being dragged to
    int hot = PLAYER_NONE, press = PLAYER_NONE;
    bool sliding = false;
    float seek = -1;
};
// A style's picture (style 1-based, scale: pixels per DIP) with the song as it is now, for the
// settings windows; live: what turns, turned by the clock (a still one shows it mid-song).
// turned: as on the desktop, turned by the user's rotation (into a picture just big enough).
bool Player_Paint(int style, float scale, bool live, ClockImage* out, const PlayerInput* in = nullptr, bool turned = false);
// x, y below: in the turned picture's DIPs (Player_Size with turned).
int Player_PartAt(int style, float x, float y);     // PlayerPart (the place only while a song is there to move in)
float Player_VolumeAt(int style, float x, float y);  // the volume (0..1) where its slider is pointed at
float Player_SeekAt(int style, float x, float y);   // the place (0..1) at x, y, while it's held
bool Player_HasVolume(int style);
bool Player_HasCorners(int style);  // its card's corners can be changed (PlayerLook::corners)
float Player_Corners(int style);
float Player_CornersMax(int style);  // the roundest its corners go: a capsule (half its height)    // its corner radius now (DIPs)
void Player_Seek(float frac);  // the song moves there (and shows there at once)
void Player_Size(int style, float* w, float* h, bool turned = false);  // DIPs (turned: the box round it as it's turned)
std::wstring Player_About(int style);             // what it is, in a few words (0: none)
bool Player_Turns();        // the music plays: a live preview moves
void Player_FreePreview();
// Windows' media controls (media.cpp): the keyboard's media keys and the volume flyout's card, for My music.
void Media_Start(HWND host);
void Media_Update();  // My music changed (song, playing, its songs or the sound source)
void Media_Stop();  // done with previews (what drew them goes, unless the player shows)
// Now playing from any app (nowplaying.cpp): another app's media session, from Windows' media controls.
// Music from another app (nowplaying.cpp): the app chosen as the source, from Windows' media controls.
struct NowPlaying {
    std::wstring id, app;  // the source app (its id and name; "" = my music)
    bool on = false;       // it has a media session now
    bool playing = false;
    std::wstring title, artist;
    UINT posMs = 0, durMs = 0;  // its place, as the app last said (durMs 0: it doesn't say)
    INT64 posAt = 0, startTicks = 0;  // when it said that (a FILETIME); where its timeline starts (100 ns)
    bool canPrev = false, canPlay = false, canNext = false, canSeek = false;  // the buttons it takes
    std::shared_ptr<std::vector<uint32_t>> cover;  // its song's cover (160 x 160), if it has one
    std::wstring coverKey;  // changes with each new picture
    UINT PosNow() const;  // its place now (moving on while it plays)
};
struct NowPlayingApp {
    std::wstring id, name;
    bool playing = false;
};
void NowPlaying_Start(HWND host);
void NowPlaying_Tick();  // have a look (the timer, and Windows saying something changed)
NowPlaying NowPlaying_Get();
std::vector<NowPlayingApp> NowPlaying_Apps();  // the other apps with a media session now
std::wstring NowPlaying_Name(const std::wstring& id);  // an app's name ("YouTube Music")
void NowPlaying_Command(int what);  // -1 previous, 0 play / pause, 1 next
void NowPlaying_Seek(UINT ms);
void NowPlaying_Stop();
// The music, wherever it comes from (host.cpp): My music, or the app chosen as its source.
std::vector<NowPlayingApp> Music_Sources();  // what the music can come from: My music (id ""), then the apps with a session (and the chosen one)
bool Music_FromApp();             // an app is the source (the sound is music)
bool Music_Playing();             // what the play button shows
bool Music_Moving();              // the music is heard now (a player's record turns)
void Music_PlayPause();
void Music_Skip(bool next);
void Music_SeekMs(UINT ms);
void Host_SetMusicSource(const std::wstring& id);  // "" my music (it plays on where it was); picking one makes the sound music
void Host_NowPlayingChanged();    // the source app's song, place or state changed
// The clock's fonts. Fonts VideoBG can't include (free for personal use only) are used once they're
// on the PC: installed, or added through VideoBG (kept in %LOCALAPPDATA%\VideoBG\Fonts).
struct ClockFontInfo {
    std::wstring key;       // its name, as stored in a look
    const wchar_t* getUrl;  // where to get it, for a font VideoBG can't include (else nullptr)
    bool removable;         // VideoBG has a copy in its fonts folder, which can be deleted
    bool ready;             // included, or its file is on this PC
};
// Clock styles: the Mond clock (day, date, time) in any of the fonts, and clocks of their own design,
// each with its own fonts and its own place for the date. A look keeps its style (ClockLook::style,
// "" = Mond) and, for Mond, its font. Mond in Audiowide comes first, then the designs, then Mond in
// the other fonts (some not on this PC yet: see their font).
struct ClockStyleInfo {
    std::wstring style;      // ClockLook::style
    std::wstring name;       // as shown
    std::wstring about;      // what it is, in a few words
    ClockFontInfo font;      // Mond: its font; a design: ready, and no font key
    bool day, digital, date; // what it shows: the day's name, the time in digits, a date line
};
std::vector<ClockStyleInfo> Clock_Styles();
bool Clock_IsStyle(const ClockLook& l, const ClockStyleInfo& s);  // the look has this style (a font that's gone: Audiowide's)
void Clock_UseStyle(ClockLook& l, const ClockStyleInfo& s);
std::wstring Clock_StyleName(const ClockLook& l);
// What a look's style shows, for the choices that only matter to some: its language (day or month
// names, AM/PM), its digits, 12- or 24-hour time, and the date line.
bool Clock_UsesWords(const ClockLook& l);
bool Clock_UsesDigits(const ClockLook& l);
bool Clock_UsesHours(const ClockLook& l);
bool Clock_UsesDate(const ClockLook& l);
// Date styles: how the clock's middle line reads in the look's language; "" for the style that
// leaves it out.
int Clock_DateStyleCount();
std::wstring Clock_DateText(const ClockLook& look, int style, const SYSTEMTIME& t);
// Languages for the clock's words: Windows' own names for days, months and AM/PM, in every language
// it knows. English first (code ""), then the rest by English name.
struct ClockLanguage {
    std::wstring code, english, native;  // "hi", "Hindi", the name in Hindi
};
std::vector<ClockLanguage> Clock_Languages();
std::wstring Clock_LanguageName(const std::wstring& code);  // in English
std::wstring Clock_NativeDigits(const std::wstring& code);  // its own digits, or "" when it writes 0-9
// One line of text in one of the clock's fonts, at pt points (the date styles' samples); with `glow`,
// that look's glow behind it (the presets' samples).
bool Clock_PaintLine(const std::wstring& text, const std::wstring& font, float pt, const std::wstring& color, float pixelScale,
                     ClockImage* out, const ClockLook* glow = nullptr);
std::vector<ClockFontInfo> Clock_Fonts();       // the included ones, Anurati, then the user's own
bool Clock_FontReady(const std::wstring& key);  // included, or its file is on this PC
// A font file, or a .zip holding one, checked to be that font and copied into VideoBG's fonts folder.
HRESULT Clock_AddFontFile(const std::wstring& key, const std::wstring& path);
// Any font the user picks (a file, or a .zip holding one), copied into VideoBG's fonts folder under
// its family name, which becomes its key. S_FALSE: it's one VideoBG includes; E_INVALIDARG: no font.
HRESULT Clock_AddOwnFont(const std::wstring& path, std::wstring* key);
bool Clock_RemoveFont(const std::wstring& key);  // deletes VideoBG's copy
bool Clock_FindDownloadedFont(const std::wstring& key);  // adds it from the Downloads folder, if it's there
bool ParseColor(const std::wstring& s, BYTE* r, BYTE* g, BYTE* b);  // "r,g,b", "#RRGGBB", "RRGGBB", "#RGB", "rgb(r,g,b)"
// optimize.cpp
bool LightCopySize(UINT vw, UINT vh, const Crop& c, int screenW, int screenH, UINT* ow, UINT* oh);
HRESULT MakeLightCopy(const std::wstring& src, const std::wstring& dst, UINT ow, UINT oh, HWND notify, UINT progressMsg,
                      volatile LONG* cancel);
HRESULT MakeVideoFromGif(const std::wstring& gif, const std::wstring& dst, HWND notify, UINT progressMsg, volatile LONG* cancel);
