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
#include <string>
#include <vector>

#define APP_NAME     L"VideoBG"
#define APP_VERSION  L"1.2.0"
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
};
enum : LPARAM {
    LSF_LOCKSCREEN = 1,  // the lock screen shows the video's frame
    LSF_BACKGROUND = 2,  // the desktop background shows it
    LSF_EARLY = 4,       // the background is the user's again; the lock screen is still being done
};

enum RendererState : WPARAM { RS_LOADING = 0, RS_PLAYING = 1, RS_PAUSED = 2, RS_ERROR = 3 };
enum PauseReason : LPARAM { PR_COVERED = 1, PR_LOCKED = 2, PR_DISPLAY_OFF = 4, PR_BATTERY = 8 };

constexpr DWORD EXIT_RENDER_RESTART = 3;  // renderer asks the host to start it again
constexpr DWORD EXIT_RENDER_ERROR = 4;    // renderer could not play the video

struct Crop {
    float l = 0, t = 0, r = 1, b = 1;  // normalized to the video frame
    bool IsFull() const { return l <= 0.0005f && t <= 0.0005f && r >= 0.9995f && b >= 0.9995f; }
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
    bool operator==(const ClockLook& o) const {
        return x == o.x && y == o.y && color == o.color && size == o.size && opacity == o.opacity && glow == o.glow &&
               glowSize == o.glowSize && glowColor == o.glowColor && show == o.show;
    }
};

struct Settings {
    std::wstring video;
    UINT hkMods = MOD_CONTROL | MOD_ALT;
    UINT hkVk = 'B';
    int scale = 0;        // 0 fill, 1 fit, 2 stretch
    int monitors = 0;     // 0 all, 1 primary only
    int speed = 100;      // playback speed in percent
    int volume = 60;      // 0..100, for whichever sound source is chosen
    int sound = 0;        // 0 off (audio never decoded), 1 the video's own audio, 2 my music
    bool shuffle = false;     // my music: random order
    bool keepSound = true;    // sound keeps playing while the wallpaper is paused (covered / locked)
    int fpsCap = 0;       // 0 = native frame rate
    int pauseCover = 2;   // 0 never, 1 when fullscreen app, 2 when maximized or fullscreen app
    int onBattery = 0;    // 0 keep playing, 1 pause, 2 turn off
    int launch = 1;       // state at launch: 0 on, 1 as it was left (lastOn), 2 off
    bool lastOn = true;
    double previewTime = -1;  // seconds; the video's frame for previews and the lock screen (-1 = auto; stored per video)
    bool lockFollow = true;   // the lock screen shows the video's frame while the wallpaper shows, the user's own picture otherwise
    int pipeline = 0;         // 0 auto, 1 always media engine, 2 always silent reader (INI only)
    Crop crop;                // crop for the current video (stored per video)

    // Desktop clock: one look while the video wallpaper shows, one for the still wallpaper. A video
    // can have its own look instead of clockLive. Each look also says whether the clock shows at all.
    bool clock24h = false;
    ClockLook clockStill{0.5f, 0.25f, L"0,0,0"};
    ClockLook clockLive{0.5f, 0.5f, L"255,255,255"};
    bool clockVideoOwn = false;   // the current video uses its own look (stored per video)
    bool clockVideoSaved = false; // it has one stored (kept while switched off)
    ClockLook clockVideo;
    std::wstring myColors[16];    // the colour picker's saved colours, "r,g,b" or "" (all looks share them)
};

// settings.cpp
std::wstring AppDataDir();    // %APPDATA%\VideoBG (created on demand): settings, log
std::wstring LocalDataDir();  // %LOCALAPPDATA%\VideoBG (created on demand): light copies
void Log(const wchar_t* fmt, ...);  // appends to %APPDATA%\VideoBG\videobg.log (events only, never per frame)
void InstallCrashLog(const wchar_t* role);  // logs module+offset of any crash before Windows' crash report
std::vector<std::wstring> LoadPlaylist();   // %APPDATA%\VideoBG\music.m3u8
void SavePlaylist(const std::vector<std::wstring>& files);
inline bool WantVideoAudio(const Settings& s) { return s.sound == 1 && s.volume > 0; }
void LoadSettings(Settings& s);
void SaveSettings(const Settings& s);
void SaveMyColors(const Settings& s);  // just the colour dialog's saved colours
void LoadVideoProfile(Settings& s);  // crop, frame and clock look of s.video
Crop LoadCropFor(const std::wstring& video);
// Per-video data (videos.ini: crop, clock look) is keyed by the file's content, so renaming or
// moving a video keeps it, and a light copy shares its original's entry.
std::wstring VideoId(const std::wstring& path);         // content fingerprint, "" if unreadable
std::wstring VideoProfileId(const std::wstring& path);  // VideoId, or the original's for a light copy
void LinkLightCopy(const std::wstring& copy, const std::wstring& original);
std::wstring LightCopyOriginalPath(const std::wstring& copy);  // "" if unknown or no longer there
bool IsLightCopyOf(const std::wstring& copy, const std::wstring& original);
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

// Entry points
int HostMain(HINSTANCE inst, bool openSettings);
int RendererMain(HINSTANCE inst, HWND host);

// Host services used by the settings UI (host.cpp)
extern Settings g_settings;
bool Host_IsOn();
void Host_SetOn(bool on);
bool Host_SetHotkey(UINT mods, UINT vk);
void Host_SettingsChanged();                 // save + tell renderer to reload
void Host_SetVideo(const std::wstring& path, const Crop* keepCrop = nullptr);
void Host_PreviewCrop(const Crop* c);        // nullptr = end preview
void Host_PreviewSize(UINT w, UINT h);       // the wallpaper as a w x h version of the video would look; 0, 0 = end
RendererState Host_RendererState(LPARAM* pauseMask);
bool Host_BatteryOff();                      // off only because of the "on battery: turn off" policy
bool Host_RendererMemory(SIZE_T* privateWs, SIZE_T* graphics);  // graphics: decoder frames, swap chains (RAM on most laptops)
SIZE_T Host_SelfMemory();
bool Host_StartupRegistered(bool* enabledInWindows);
void Host_RegisterStartup();
void Host_TrimMemory();
bool Host_PickVideo(HWND owner);
bool Host_PickMusic(HWND owner);             // choose songs for "my music"
int Host_CurrentTrack();                     // playlist index playing, -1 none, -2 couldn't play
void Host_SuspendHotkey(bool suspend);       // while the settings UI records a new shortcut
bool Host_ClockLive();                       // true while the clock should use the video wallpaper look
void Host_ClockMove(const ClockLook& look);  // move the clock right now without saving (while dragging)
void Host_SetLockFollow(bool on);            // the lock screen follows the wallpaper (off: the user's picture, left alone)
void Host_FrameChanged();                    // the video's frame (preview time) changed: save it, update the lock screen
bool Host_LockScreen(HRESULT* lastResult, int* lsf);  // true while it's being updated; lsf: what shows the frame

void Host_VideoPlayable(const std::wstring& path);  // the UI could read it: resume if it had failed to play
struct FaultText;
bool Host_PlayFailed(FaultText* why);               // the wallpaper was turned off because the video couldn't play

// ui.cpp
void UI_Show();
void UI_Refresh();
bool UI_IsOpen();
void UI_ConvertGif(const std::wstring& gif);  // opens the settings and turns the GIF into a video, then uses it
void UI_VideoFailed();                        // the video couldn't play: look at the file again

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
bool Clock_Paint(const ClockLook& look, bool h24, float pixelScale, const SYSTEMTIME& t, ClockImage* out);
void Clock_ReleasePainter();
void Clock_Show(HWND host, const ClockLook& look, bool h24);  // create / update / move
void Clock_Hide();
void Clock_Refresh();   // time jumped, display changed, woke from sleep: redraw and re-place
float Clock_PixelScale();
std::wstring Clock_StyleKey(const ClockLook& look);  // everything that changes the picture (not the place)
bool ParseColor(const std::wstring& s, BYTE* r, BYTE* g, BYTE* b);  // "r,g,b", "#RRGGBB", "RRGGBB", "#RGB", "rgb(r,g,b)"
// optimize.cpp
bool LightCopySize(UINT vw, UINT vh, const Crop& c, int screenW, int screenH, UINT* ow, UINT* oh);
HRESULT MakeLightCopy(const std::wstring& src, const std::wstring& dst, UINT ow, UINT oh, HWND notify, UINT progressMsg,
                      volatile LONG* cancel);
HRESULT MakeVideoFromGif(const std::wstring& gif, const std::wstring& dst, HWND notify, UINT progressMsg, volatile LONG* cancel);
