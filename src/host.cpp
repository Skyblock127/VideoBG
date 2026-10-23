// Host process: tray icon, global hotkey, settings UI, and lifetime of the renderer process.
// While the wallpaper is OFF this is the only process left, and it idles in a few MB.
#include "common.h"
#include "resource.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <psapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cwchar>

Settings g_settings;

namespace {

constexpr UINT_PTR TIMER_RESTART = 1, TIMER_TRIM = 2, TIMER_CLOCK = 3, TIMER_STOP = 4, TIMER_UPDATE = 5, TIMER_CHECK = 6;
constexpr int HOTKEY_ID = 1, HOTKEY_MUSIC = 2;
enum { CMD_TOGGLE = 100, CMD_SETTINGS, CMD_EXIT, CMD_PICKER, CMD_LOOK };

HINSTANCE g_inst;
HWND g_wnd;
HANDLE g_job;
HANDLE g_proc;          // renderer process
HWND g_ctl;             // renderer control window
bool g_on;              // what the user asked for
bool g_batteryOff;      // temporarily off because of the "on battery: turn off" policy
bool g_onBattery;
bool g_override;        // user turned it on manually while on battery
RendererState g_rstate = RS_LOADING;
LPARAM g_rmask;
HRESULT g_lastError;
int g_fault = VF_OTHER;     // why the renderer couldn't play the video (VideoFault)
DWORD g_faultCodec;         // and the video's codec
std::wstring g_failedVideo; // turned off because this video couldn't play; resume once it can
FaultText g_failText;       // why it couldn't (empty: the wallpaper wasn't turned off by an error)
UINT g_taskbarCreated;
NOTIFYICONDATAW g_nid;
HICON g_icoOn, g_icoOff;
ULONGLONG g_crashes[3];
HPOWERNOTIFY g_power;
int g_track = -1;       // playlist index the renderer is playing
MusicState g_music;     // what the renderer last said about the music
bool g_exiting;         // quitting / logging off
std::wstring g_clockShown;  // what the desktop clock was last told to show, so repeated syncs cost nothing
std::wstring g_lockWanted;  // what the lock screen was last asked to show ("" = not asked yet)
bool g_lockBusy;            // the lock screen / background worker is at it
HRESULT g_lockHr = S_OK;    // its last result
int g_lsf;                  // LSF_*: what shows the video's frame (lock screen, background)
bool g_stopPending;         // the video waits for the background to be the user's picture again
bool g_endingSession;       // Windows is signing out / shutting down
UINT64 g_rendererGraphics;  // the renderer's graphics memory, as it last reported
bool g_hotkeysOff;          // the shortcuts are off while one is being recorded
bool g_musicKeyRaw;         // the sound shortcut's key is listened for as raw input too
bool g_musicKeyHeld;        // and it's down now (seen that way)
ULONGLONG g_musicKeyAt;     // when the sound shortcut last did something (either way)

bool Running() { return g_proc != nullptr; }

// The playlist, read once and kept here: both looks show this one, so neither goes out of date.
std::vector<std::wstring> g_songs;
bool g_songsRead;

void SetSongs(const std::vector<std::wstring>& list) {
    SavePlaylist(list);
    g_songs = list;
    g_songsRead = true;
}

// ---------------------------------------------------------------------------------------
// Desktop clock: follows whichever wallpaper is showing.

bool WallpaperShowing() { return !g_exiting && g_on && !g_batteryOff && !g_settings.video.empty(); }

bool g_clockPreviewOn;     // a clock preset being edited shows instead of the saved looks
ClockLook g_clockPreview;

ClockLook WantedLook() {
    if (g_clockPreviewOn) return g_clockPreview;
    if (!WallpaperShowing()) return g_settings.clockStill;
    return g_settings.clockVideoOwn ? g_settings.clockVideo : g_settings.clockLive;
}

// The music player on the desktop (in either look): there with the video wallpaper, while My music is on.
void SyncPlayer(bool force) {
    if (g_settings.musicPlayer < 1 || g_settings.sound != 2 || !WallpaperShowing()) {
        Player_Hide();
        return;
    }
    Player_Show(Host_Songs(), force);
}

// The desktop clock (and the music player beside it) as the wallpaper and settings want them now.
void SyncClock(bool force = false) {
    SyncPlayer(force);
    if (!WantedLook().show || g_exiting) {
        if (!g_clockShown.empty()) Log(L"clock: hidden");
        g_clockShown.clear();
        Clock_Hide();
        return;
    }
    ClockLook look = WantedLook();
    wchar_t where[64];
    swprintf(where, 64, L"%.5f|%.5f|", look.x, look.y);
    std::wstring key = where + Clock_StyleKey(look);
    if (!force && g_clockShown == key) return;
    Clock_Show(g_wnd, look);
    g_clockShown = key;
    Log(L"clock: %ls look (%.1f%%, %.1f%%, %ls, colour %ls, size %.0f%%, opacity %.0f%%, glow %.0f%%)",
        WallpaperShowing() ? L"video wallpaper" : L"still wallpaper", look.x * 100, look.y * 100, Clock_StyleName(look).c_str(), look.color.c_str(),
        look.size * 100, look.opacity * 100, look.glow * 100);
}
// The lock screen and desktop background, while they follow the wallpaper: the video's frame while
// the wallpaper shows, the user's own pictures otherwise. The frame goes up only once the video shows
// (nothing competes with it starting, and the background never shows the frame first); the user's
// pictures come back at once. Done by a worker thread; repeated syncs with nothing new cost nothing.
void SyncLockScreen(bool force = false) {
    if (!g_settings.lockFollow) return;
    LockScreenWant w;
    w.frame = WallpaperShowing();
    if (w.frame && g_rstate != RS_PLAYING && g_rstate != RS_PAUSED) return;  // again when the video is up
    w.background = w.frame;
    std::wstring key = L"own";
    if (w.frame) {
        w.video = g_settings.video;
        w.time = g_settings.previewTime;
        w.crop = g_settings.crop;
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        w.sw = mi.rcMonitor.right - mi.rcMonitor.left;
        w.sh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        wchar_t b[128];
        swprintf(b, 128, L"|%.3f|%.4f,%.4f,%.4f,%.4f,%d|%dx%d", w.time, w.crop.l, w.crop.t, w.crop.r, w.crop.b, w.crop.stretch, w.sw,
                 w.sh);
        key = w.video + b;
    }
    if (!force && key == g_lockWanted) return;
    g_lockWanted = key;
    g_lockBusy = true;
    LockScreen_Sync(g_wnd, WM_VBG_LOCKSCREEN, w);
}

bool RaisedDesktop() {
    HWND pm = FindWindowW(L"Progman", nullptr);
    return pm && (GetWindowLongPtrW(pm, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP);
}

void RefreshStaticWallpaper() {
    // Only needed on the classic (pre-24H2) desktop, where the WorkerW may not repaint by itself.
    if (RaisedDesktop()) return;
    wchar_t path[MAX_PATH] = {};
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0))
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, path, 0);
}

void Balloon(const wchar_t* title, const std::wstring& text) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcsncpy(n.szInfoTitle, title, std::size(n.szInfoTitle) - 1);
    wcsncpy(n.szInfo, text.c_str(), std::size(n.szInfo) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void UpdateTray() {
    bool on = g_on && !g_batteryOff;
    g_nid.hIcon = on ? g_icoOn : g_icoOff;
    std::wstring tip = std::wstring(L"VideoBG: ") + (on ? L"on" : (g_batteryOff ? L"off while on battery" : L"off")) +
                       L"\n" + HotkeyToString(g_settings.hkMods, g_settings.hkVk) +
                       (g_settings.look == 1 ? L" shows your wallpapers" : L" turns it on or off");
    wcsncpy(g_nid.szTip, tip.c_str(), std::size(g_nid.szTip) - 1);
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_MESSAGE;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void AddTray() {
    g_nid = NOTIFYICONDATAW{};
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_VBG_TRAY;
    g_nid.hIcon = g_icoOff;
    wcscpy(g_nid.szTip, L"VideoBG");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    UpdateTray();
}

void StartRenderer() {
    if (Running() || g_settings.video.empty()) return;
    std::wstring exe = ExePath();
    wchar_t cmd[MAX_PATH * 2 + 64];
    swprintf(cmd, std::size(cmd), L"\"%ls\" --render %llu", exe.c_str(), (unsigned long long)(UINT_PTR)g_wnd);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        Balloon(L"VideoBG", L"Couldn't start the video wallpaper.");
        return;
    }
    AssignProcessToJobObject(g_job, pi.hProcess);  // renderer dies with us, no matter how we exit
    ResumeThread(pi.hThread);
    Log(L"tray: started video renderer (pid %lu)", pi.dwProcessId);
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    g_ctl = nullptr;
    g_rstate = RS_LOADING;
    g_rmask = 0;
}

// The song and spot the music was at when the wallpaper stops, to carry on from there next time.
// Kept in the settings (three numbers), so the music carries on from there: when the wallpaper's
// back, when My music is picked again, or after a restart.
void KeepMusicPlace() {
    if (g_music.track < 0) return;
    g_settings.musicTrack = g_music.track;
    g_settings.musicPosMs = g_music.posMs;
    g_settings.musicDurMs = g_music.durMs;
    SaveSettings(g_settings);
}

void StopRenderer() {
    KillTimer(g_wnd, TIMER_RESTART);
    if (!Running()) return;
    Log(L"tray: stopping video renderer");
    if (g_ctl) PostMessageW(g_ctl, WM_VBG_QUIT, 0, 0);
    if (!g_ctl || WaitForSingleObject(g_proc, 2500) == WAIT_TIMEOUT) TerminateProcess(g_proc, 0);
    WaitForSingleObject(g_proc, 1000);
    CloseHandle(g_proc);
    g_proc = nullptr;
    g_ctl = nullptr;
    g_rstate = RS_LOADING;
    g_rmask = 0;
    g_track = -1;
    KeepMusicPlace();
    g_music = MusicState{};
    RefreshStaticWallpaper();
}

bool WantRenderer() { return g_on && !g_batteryOff && !g_settings.video.empty(); }

// While the desktop background is the video's frame, stopping the video would show that frame for
// a moment before the user's picture comes back: the video waits for it (about 0.1 s, at most 0.8 s).
void StopRendererSoon() {
    if (!(g_lsf & LSF_BACKGROUND) || !LockScreen_Busy()) {
        StopRenderer();
        return;
    }
    g_stopPending = true;
    SetTimer(g_wnd, TIMER_STOP, 800, nullptr);
}

void FinishPendingStop() {
    if (!g_stopPending) return;
    g_stopPending = false;
    KillTimer(g_wnd, TIMER_STOP);
    if (!WantRenderer() && Running()) {
        StopRenderer();
        SyncClock();  // together with the video
        UpdateTray();
        UI_Refresh();
    }
}

// Make the renderer's existence match the desired state.
void Reconcile() {
    bool want = WantRenderer();
    SyncLockScreen();  // first: turning off, the background goes back while the video still covers it
    if (want && !Running()) {
        StartRenderer();
    } else if (want) {  // turned back on before it stopped
        g_stopPending = false;
        KillTimer(g_wnd, TIMER_STOP);
    } else if (Running() && !g_stopPending) {
        StopRendererSoon();
    }
    if (!g_stopPending) SyncClock();  // the clock switches with the video (at once, unless the video waits)
    UpdateTray();
    UI_Refresh();
}

void ApplyBatteryPolicy() {
    bool off = g_settings.onBattery == 2 && g_onBattery && g_on && !g_override;
    if (off != g_batteryOff) {
        g_batteryOff = off;
        Reconcile();
    }
}

void OnRendererExit() {
    DWORD code = 0;
    GetExitCodeProcess(g_proc, &code);
    CloseHandle(g_proc);
    g_proc = nullptr;
    g_ctl = nullptr;
    g_rstate = RS_LOADING;
    g_rmask = 0;
    g_track = -1;
    KeepMusicPlace();
    g_music = MusicState{};
    Log(L"tray: video renderer exited with code 0x%lx", code);
    if (code == EXIT_RENDER_ERROR) {
        g_on = false;
        FaultText ft = DescribeFault(g_fault, g_faultCodec, g_lastError, g_settings.video);
        Balloon(L"VideoBG can't play this video", ft.what + L".\n" + ft.fix + L".");
        // Things the user can fix outside VideoBG (a codec, a missing file): the settings window
        // turns the wallpaper back on once it can read the video.
        bool fixable = g_fault == VF_NO_DECODER || g_fault == VF_NO_MEDIA || g_fault == VF_MISSING ||
                       g_fault == VF_UNREADABLE || g_fault == VF_EMPTY;
        g_failedVideo = fixable ? g_settings.video : L"";
        g_failText = ft;
        g_fault = VF_OTHER;
        g_faultCodec = 0;
        UI_VideoFailed();
    } else if (code == EXIT_RENDER_RESTART || code == 0) {
        // New video / display change: come straight back. Explorer restarting: give it a moment.
        SetTimer(g_wnd, TIMER_RESTART, FindWindowW(L"Progman", nullptr) ? 150 : 1500, nullptr);
    } else {
        // Crash: retry, but give up after 3 crashes within a minute.
        ULONGLONG now = GetTickCount64();
        g_crashes[0] = g_crashes[1];
        g_crashes[1] = g_crashes[2];
        g_crashes[2] = now;
        if (g_crashes[0] && now - g_crashes[0] < 60000) {
            g_on = false;
            Balloon(L"VideoBG", L"The video wallpaper kept crashing, so it was turned off.");
        } else {
            SetTimer(g_wnd, TIMER_RESTART, 2000, nullptr);
        }
    }
    RefreshStaticWallpaper();
    SyncClock();  // turned off after an error: back to the still-wallpaper look
    SyncLockScreen();
    UpdateTray();
    UI_Refresh();
}

// The tray icon's menu: what the shortcut does in this look (the wallpapers, or the video wallpaper
// on/off), the look itself, the settings, and Exit. Windows' own menus close on every click, so this
// is VideoBG's own small window, drawn like Windows 11's menus: switching the look or the wallpaper
// changes it in place, and its size never changes (it's as wide as either look needs).
struct TrayMenu {
    HWND hwnd = nullptr;
    HFONT font = nullptr, bold = nullptr, glyphs = nullptr;
    int hot = -1;       // the item under the pointer, or chosen with the keys
    float k = 1;        // pixels per DIP
    bool dark = true;   // Windows' own (taskbar) theme
    bool tracking = false;
    HWND foreground = nullptr;  // what had the focus once it showed (another one coming up puts it away)
} g_tm;

struct TrayItem {
    int cmd = 0;  // 0: a line between groups
    std::wstring text, keys;
    bool check = false, isDefault = false;
};

std::vector<TrayItem> TrayItems() {
    const bool modern = g_settings.look == 1;
    const std::wstring keys = HotkeyToString(g_settings.hkMods, g_settings.hkVk);
    std::vector<TrayItem> v;
    if (modern) v.push_back({CMD_PICKER, L"Wallpapers\u2026", keys, false, true});
    else v.push_back({CMD_TOGGLE, L"Video wallpaper on", keys, Host_IsOn(), false});
    v.push_back({CMD_LOOK, L"Modern look", L"", modern, false});
    v.push_back({});
    v.push_back({CMD_SETTINGS, L"Settings\u2026", L"", false, !modern});
    v.push_back({CMD_EXIT, L"Exit", L"", false, false});
    return v;
}

constexpr float kTmPad = 4, kTmItemH = 30, kTmLineH = 9, kTmCheckW = 34, kTmRight = 14;

float TrayItemH(const TrayItem& it) { return it.cmd ? kTmItemH : kTmLineH; }

int TrayItemAt(int px, int py) {
    const float y = py / g_tm.k;
    float top = kTmPad;
    const std::vector<TrayItem> v = TrayItems();
    for (int i = 0; i < (int)v.size(); i++) {
        const float h = TrayItemH(v[i]);
        if (y >= top && y < top + h) return v[i].cmd ? i : -1;
        top += h;
    }
    (void)px;
    return -1;
}

void TrayMenuClose() {
    if (!g_tm.hwnd) return;
    HWND h = g_tm.hwnd;
    g_tm.hwnd = nullptr;
    DestroyWindow(h);
    for (HFONT* f : {&g_tm.font, &g_tm.bold, &g_tm.glyphs})
        if (*f) {
            DeleteObject(*f);
            *f = nullptr;
        }
}

// An item chosen: the wallpaper on/off and the look change right here; the rest close the menu first.
void TrayMenuDo(int i) {
    const std::vector<TrayItem> v = TrayItems();
    if (i < 0 || i >= (int)v.size() || !v[i].cmd) return;
    const int cmd = v[i].cmd;
    if (cmd == CMD_TOGGLE) {
        Host_SetOn(!Host_IsOn());
    } else if (cmd == CMD_LOOK) {
        Host_SetLook(g_settings.look == 1 ? 0 : 1);
    } else {
        TrayMenuClose();
        PostMessageW(g_wnd, WM_COMMAND, cmd, 0);
        return;
    }
    InvalidateRect(g_tm.hwnd, nullptr, FALSE);
}

void TrayMenuPaint(HWND h) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    const bool d = g_tm.dark;
    const COLORREF bg = d ? RGB(0x2C, 0x2C, 0x2C) : RGB(0xF9, 0xF9, 0xF9), hover = d ? RGB(0x3D, 0x3D, 0x3D) : RGB(0xEA, 0xEA, 0xEA);
    const COLORREF ink = d ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x1A, 0x1A, 0x1A), ink2 = d ? RGB(0xA6, 0xA6, 0xA6) : RGB(0x6B, 0x6B, 0x6B);
    const COLORREF line = d ? RGB(0x45, 0x45, 0x45) : RGB(0xE0, 0xE0, 0xE0);
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(dc, &rc, b);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    const float k = g_tm.k;
    auto px = [&](float v) { return (int)lroundf(v * k); };
    float top = kTmPad;
    const std::vector<TrayItem> v = TrayItems();
    for (int i = 0; i < (int)v.size(); i++) {
        const TrayItem& it = v[i];
        const float ih = TrayItemH(it);
        if (!it.cmd) {
            RECT l{px(kTmPad + 8), px(top + ih / 2), rc.right - px(kTmPad + 8), px(top + ih / 2) + std::max(1, px(1))};
            HBRUSH lb = CreateSolidBrush(line);
            FillRect(dc, &l, lb);
            DeleteObject(lb);
            top += ih;
            continue;
        }
        RECT r{px(kTmPad), px(top), rc.right - px(kTmPad), px(top + ih)};
        if (i == g_tm.hot) {
            HBRUSH hb = CreateSolidBrush(hover);
            HGDIOBJ ob = SelectObject(dc, hb), op = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, px(8), px(8));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(hb);
        }
        if (it.check) {
            SelectObject(dc, g_tm.glyphs);
            SetTextColor(dc, ink);
            RECT c{r.left, r.top, r.left + px(kTmCheckW), r.bottom};
            DrawTextW(dc, L"\uE73E", 1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(dc, it.isDefault ? g_tm.bold : g_tm.font);
        SetTextColor(dc, ink);
        RECT t{r.left + px(kTmCheckW), r.top, r.right - px(kTmRight), r.bottom};
        DrawTextW(dc, it.text.c_str(), (int)it.text.size(), &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (!it.keys.empty()) {
            SelectObject(dc, g_tm.font);
            SetTextColor(dc, ink2);
            DrawTextW(dc, it.keys.c_str(), (int)it.keys.size(), &t, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        top += ih;
    }
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

void TrayMenuMove(int dir) {
    const std::vector<TrayItem> v = TrayItems();
    const int n = (int)v.size();
    int i = g_tm.hot;
    for (int step = 0; step < n; step++) {
        i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
        if (v[i].cmd) break;
    }
    g_tm.hot = i;
    InvalidateRect(g_tm.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK TrayMenuProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: TrayMenuPaint(h); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: {
            if (!g_tm.tracking) {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
                g_tm.tracking = TrackMouseEvent(&t) != 0;
            }
            const int i = TrayItemAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (i != g_tm.hot) {
                g_tm.hot = i;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tm.tracking = false;
            g_tm.hot = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: TrayMenuDo(TrayItemAt(GET_X_LPARAM(l), GET_Y_LPARAM(l))); return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) TrayMenuClose();
            else if (w == VK_DOWN) TrayMenuMove(1);
            else if (w == VK_UP) TrayMenuMove(-1);
            else if (w == VK_RETURN || w == VK_SPACE) TrayMenuDo(g_tm.hot);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) PostMessageW(h, WM_CLOSE, 0, 0);  // clicking anywhere else puts it away
            return 0;
        case WM_CLOSE: TrayMenuClose(); return 0;
        case WM_TIMER: {  // (if it didn't get the focus) a click anywhere else, or another app coming up, puts it away too
            RECT r;
            GetWindowRect(h, &r);
            POINT p;
            GetCursorPos(&p);
            const bool pressed = GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0;
            if ((pressed && !PtInRect(&r, p)) || GetForegroundWindow() != g_tm.foreground) PostMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

void ShowTrayMenu(POINT pt) {
    TrayMenuClose();
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = TrayMenuProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"VideoBG.TrayMenu";
        registered = RegisterClassW(&wc) != 0;
    }
    g_tm.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"VideoBG.TrayMenu", APP_NAME, WS_POPUP, pt.x, pt.y, 1, 1, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!g_tm.hwnd) return;
    g_tm.k = GetDpiForWindow(g_tm.hwnd) / 96.f;
    g_tm.hot = -1;
    g_tm.tracking = false;
    DWORD light = 0, size = sizeof light;
    g_tm.dark = !(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme",
                               RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS && light);
    const int fh = -(int)lroundf(12 * g_tm.k);
    g_tm.font = CreateFontW(fh, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_tm.bold = CreateFontW(fh, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_tm.glyphs = CreateFontW(-(int)lroundf(12 * g_tm.k), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                              L"Segoe Fluent Icons");
    // As wide as the widest item in either look (so switching never resizes it).
    HDC dc = GetDC(g_tm.hwnd);
    const std::wstring keys = HotkeyToString(g_settings.hkMods, g_settings.hkVk);
    int textW = 0;
    for (const wchar_t* t : {L"Wallpapers\u2026", L"Video wallpaper on", L"Modern look", L"Settings\u2026", L"Exit"}) {
        SIZE sz{};
        SelectObject(dc, g_tm.bold);
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
        textW = std::max(textW, (int)sz.cx);
    }
    SIZE kz{};
    SelectObject(dc, g_tm.font);
    GetTextExtentPoint32W(dc, keys.c_str(), (int)keys.size(), &kz);
    ReleaseDC(g_tm.hwnd, dc);
    float hh = 2 * kTmPad;
    for (const TrayItem& it : TrayItems()) hh += TrayItemH(it);
    const int w = std::max((int)lroundf(200 * g_tm.k), textW + (int)kz.cx + (int)lroundf((2 * kTmPad + kTmCheckW + kTmRight + 32) * g_tm.k));
    const int h = (int)lroundf(hh * g_tm.k);
    // Above the pointer, as Windows puts the tray's menus; kept on the screen it's on.
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    int x = pt.x, y = pt.y - h;
    if (x + w > wa.right) x = pt.x - w;
    x = std::clamp(x, (int)wa.left, std::max((int)wa.left, (int)wa.right - w));
    y = std::clamp(y, (int)wa.top, std::max((int)wa.top, (int)wa.bottom - h));
    const int corners = 3;  // DWMWCP_ROUNDSMALL (Windows 11), like its menus
    DwmSetWindowAttribute(g_tm.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof corners);
    const COLORREF border = g_tm.dark ? RGB(0x40, 0x40, 0x40) : RGB(0xD9, 0xD9, 0xD9);
    DwmSetWindowAttribute(g_tm.hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);
    SetWindowPos(g_tm.hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(g_tm.hwnd);
    g_tm.foreground = GetForegroundWindow();
    SetTimer(g_tm.hwnd, 1, 50, nullptr);
}

// Multi-select song picker for "my music". Returns false if cancelled.
std::vector<std::wstring> PickMusic(HWND owner, bool add) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    const COMDLG_FILTERSPEC types[] = {
        {L"Music", L"*.mp3;*.m4a;*.aac;*.wav;*.wma;*.flac;*.alac;*.ogg;*.opus;*.mp4"},
        {L"All files", L"*.*"},
    };
    dlg->SetFileTypes((UINT)std::size(types), types);
    dlg->SetTitle(add ? L"Add songs to the playlist (Ctrl+A selects a whole folder)" : L"Choose songs for the wallpaper (Ctrl+A selects a whole folder)");
    dlg->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT);
    const std::vector<std::wstring>& old = Host_Songs();
    std::wstring start = !old.empty() ? old.back() : L"";
    if (!start.empty()) {
        IShellItem* folder = nullptr;
        std::wstring dir = start.substr(0, start.find_last_of(L"\\/"));
        if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    } else {
        IKnownFolderManager* kfm = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_KnownFolderManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&kfm)))) {
            IKnownFolder* kf = nullptr;
            IShellItem* music = nullptr;
            if (SUCCEEDED(kfm->GetFolder(FOLDERID_Music, &kf)) && SUCCEEDED(kf->GetShellItem(0, IID_PPV_ARGS(&music)))) {
                dlg->SetFolder(music);
                music->Release();
            }
            if (kf) kf->Release();
            kfm->Release();
        }
    }
    std::vector<std::wstring> files;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem* it = nullptr;
                PWSTR path = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    files.push_back(path);
                    CoTaskMemFree(path);
                }
                if (it) it->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    return files;
}

bool PickVideo(HWND owner) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    const COMDLG_FILTERSPEC types[] = {
        {L"Videos and GIFs", kVideoPatterns},
        {L"All files", L"*.*"},
    };
    dlg->SetFileTypes((UINT)std::size(types), types);
    dlg->SetTitle(L"Choose a video wallpaper");
    dlg->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    if (!g_settings.video.empty()) {
        std::wstring dir = g_settings.video.substr(0, g_settings.video.find_last_of(L"\\/"));
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                if (SniffFile(p, nullptr) == VF_GIF) {
                    UI_ConvertGif(p);  // becomes the wallpaper once it's a video
                } else {
                    Host_SetVideo(p);
                    ok = true;
                }
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

bool RegisterToggleHotkey(UINT mods, UINT vk) {
    UnregisterHotKey(g_wnd, HOTKEY_ID);
    return RegisterHotKey(g_wnd, HOTKEY_ID, mods | MOD_NOREPEAT, vk) != 0;
}

// Many games switch other apps' hotkeys off while they're in front (they take the keyboard as raw
// input with "no hotkeys"), so the sound shortcut is also listened for as raw input: it still
// arrives that way then (and only then: a hotkey that works takes the key before raw input sees it).
void ListenForMusicKey(bool on) {
    if (on == g_musicKeyRaw) return;
    RAWINPUTDEVICE d{1, 6, on ? (DWORD)RIDEV_INPUTSINK : (DWORD)RIDEV_REMOVE, on ? g_wnd : nullptr};  // generic desktop: keyboard
    if (RegisterRawInputDevices(&d, 1, sizeof d)) g_musicKeyRaw = on;
    g_musicKeyHeld = false;
}

bool RegisterMusicHotkey(UINT mods, UINT vk) {
    UnregisterHotKey(g_wnd, HOTKEY_MUSIC);
    ListenForMusicKey(vk != 0);
    return !vk || RegisterHotKey(g_wnd, HOTKEY_MUSIC, mods | MOD_NOREPEAT, vk) != 0;
}

// The sound shortcut: plays or pauses My music, as the player's button does (also while the video
// wallpaper is off: it decides whether the music starts when it's back); mutes or unmutes the
// video's audio (the video plays on, so the sound comes back in sync). With no sound, nothing.
void ToggleMusic() {
    if (g_settings.sound == 1) {
        g_settings.videoMuted = !g_settings.videoMuted;
        Log(L"tray: video's audio %ls by the shortcut", g_settings.videoMuted ? L"muted" : L"unmuted");
        Host_SettingsChanged();
        UI_Refresh();
        return;
    }
    if (g_settings.sound != 2 || Host_Songs().empty()) {
        Log(L"tray: sound shortcut pressed, but there's no sound to pause");
        return;
    }
    Host_PlayMusic(!g_settings.musicPlaying);
}

// The sound shortcut, once per press, however it arrived (not twice if it came both ways).
void MusicShortcut(bool raw) {
    const ULONGLONG now = GetTickCount64();
    if (now - g_musicKeyAt < 300) return;
    g_musicKeyAt = now;
    if (raw) Log(L"tray: sound shortcut seen as a key press (the app in front has hotkeys off)");
    ToggleMusic();
}

// A key, as raw input: the sound shortcut's, with its modifiers held?
void MusicKeyRaw(const RAWKEYBOARD& k) {
    if (g_hotkeysOff || !g_settings.musicHkVk || k.VKey != g_settings.musicHkVk) return;
    if (k.Flags & RI_KEY_BREAK) {
        g_musicKeyHeld = false;
        return;
    }
    if (g_musicKeyHeld) return;  // held down: it repeats
    g_musicKeyHeld = true;
    UINT mods = 0;
    if (GetAsyncKeyState(VK_MENU) < 0) mods |= MOD_ALT;
    if (GetAsyncKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
    if (mods == g_settings.musicHkMods) MusicShortcut(true);
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_HOTKEY:
            if (w == HOTKEY_ID) {
                if (g_settings.look == 1) Modern_Toggle();  // the modern look: the shortcut brings up the wallpapers
                else Host_SetOn(!Host_IsOn());
            } else if (w == HOTKEY_MUSIC) {
                MusicShortcut(false);
            }
            return 0;
        case WM_VBG_MEDIA_KEY:  // a media key (or the volume flyout's buttons), for My music
            switch (w) {
                case 0: Host_PlayMusic(true); break;   // Play
                case 1:                                // Pause, Stop
                case 2: Host_PlayMusic(false); break;
                case 6: Host_SkipMusic(true); break;   // Next
                case 7: Host_SkipMusic(false); break;  // Previous
            }
            return 0;
        case WM_INPUT: {
            RAWINPUT ri;
            UINT size = sizeof ri;
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEKEYBOARD)
                MusicKeyRaw(ri.data.keyboard);
            return DefWindowProcW(h, m, w, l);
        }
        case WM_VBG_TRAY:
            switch (LOWORD(l)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK:
                    if (g_settings.look == 1 && !g_settings.video.empty()) Modern_Open(0);
                    else UI_Show();
                    break;
                case WM_CONTEXTMENU: ShowTrayMenu(POINT{GET_X_LPARAM(w), GET_Y_LPARAM(w)}); break;
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case CMD_TOGGLE: Host_SetOn(!Host_IsOn()); break;
                case CMD_SETTINGS: UI_Show(); break;
                case CMD_PICKER: Modern_Open(0); break;
                case CMD_EXIT: DestroyWindow(h); break;
            }
            return 0;
        case WM_VBG_RENDERER_READY: {
            DWORD pid = 0;
            GetWindowThreadProcessId((HWND)l, &pid);
            if (g_proc && pid == GetProcessId(g_proc)) g_ctl = (HWND)l;
            Modern_RendererEvent(m, w, l);  // the picker's showcase and preview start over with it
            return 0;
        }
        case WM_VBG_SHOWCASE_UP:
        case WM_VBG_PREVIEW_UP:
            Modern_RendererEvent(m, w, l);
            return 0;
        case WM_VBG_STATE:
            g_rstate = (RendererState)w;
            if (w == RS_ERROR) g_lastError = (HRESULT)l;
            else g_rmask = l;
            SyncLockScreen();  // the video is up: the background can follow
            UI_Refresh();
            return 0;
        case WM_VBG_MEMORY:
            g_rendererGraphics = (UINT64)l * 1024;
            return 0;
        case WM_VBG_FAULT:
            g_fault = (int)w;
            g_faultCodec = (DWORD)l;
            return 0;
        case WM_VBG_TRACK:
            g_track = (int)(INT_PTR)w;
            UI_Refresh();
            return 0;
        case WM_VBG_MUSIC_POS:  // twice a second while there's music (the settings window repaints on its own)
            g_music.posMs = (UINT)w;
            g_music.durMs = (UINT)((UINT64)l & 0xFFFFFFFF);
            g_music.track = (int)(((UINT64)l >> 32) & 0xFFFF) - 2;
            g_music.playing = ((UINT64)l >> 48) & 1;
            Player_Update();
            Modern_MusicChanged();
            Media_Update();
            return 0;
        case WM_VBG_MUSIC_END:  // the last song ended with repeat off: stopped, ready at the first song
            g_settings.musicPlaying = false;
            Host_SettingsChanged();
            UI_Refresh();
            return 0;
        case WM_VBG_SHOW: UI_Show(); return 0;
        case WM_VBG_PICKER: Modern_Open(0); return 0;
        case WM_VBG_TOGGLE: Host_SetOn(!Host_IsOn()); return 0;
        case WM_VBG_EXIT: DestroyWindow(h); return 0;
        case WM_TIMER:
            if (w == TIMER_RESTART) { KillTimer(h, TIMER_RESTART); Reconcile(); }
            else if (w == TIMER_TRIM) { KillTimer(h, TIMER_TRIM); Host_TrimMemory(); }
            else if (w == TIMER_CLOCK) { KillTimer(h, TIMER_CLOCK); SyncClock(true); }
            else if (w == TIMER_STOP) FinishPendingStop();
            else if (w == TIMER_UPDATE) { KillTimer(h, TIMER_UPDATE); Update_Done(); UI_Refresh(); }
            else if (w == TIMER_CHECK) {  // a look for a new version, then again every 3 hours
                SetTimer(h, TIMER_CHECK, 3 * 3600 * 1000, nullptr);
                Update_Start(h, true);
            }
            return 0;
        case WM_VBG_UPDATE:
            if (w == UP_RESTART) {  // the new version is in place: close as usual, then main() starts it
                DestroyWindow(h);
                return 0;
            }
            if (w == UP_OFFLINE || w == UP_FAILED || w == UP_LATEST) SetTimer(h, TIMER_UPDATE, 5000, nullptr);  // the answer stays a while
            UI_Refresh();
            return 0;
        case WM_VBG_CLOCK_LOST:  // the desktop went away with the clock in it: put it back once Explorer is up
            g_clockShown.clear();
            SetTimer(h, TIMER_CLOCK, 1500, nullptr);
            return 0;
        case WM_TIMECHANGE:
        case WM_DISPLAYCHANGE:
            Clock_Refresh();
            return 0;
        case WM_POWERBROADCAST:
            if (w == PBT_APMRESUMEAUTOMATIC) Clock_Refresh();  // the minute timer doesn't run while asleep
            if (w == PBT_POWERSETTINGCHANGE) {
                auto* ps = (POWERBROADCAST_SETTING*)l;
                if (ps->PowerSetting == GUID_ACDC_POWER_SOURCE && ps->DataLength >= sizeof(DWORD)) {
                    g_onBattery = *(DWORD*)ps->Data != 0;
                    g_override = false;
                    ApplyBatteryPolicy();
                }
            }
            return TRUE;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION:
            if (w) {
                // Signing out / shutting down: the lock screen keeps its picture, which matches what
                // VideoBG shows again after the next sign-in (it starts as it was left).
                g_endingSession = true;
                StopRenderer();
            }
            return 0;
        case WM_VBG_LOCKSCREEN:
            if (l & LSF_EARLY) {  // the background is the user's picture again: the video may go
                g_lsf = (g_lsf & ~LSF_BACKGROUND) | (int)(l & LSF_BACKGROUND);
                FinishPendingStop();
                return 0;
            }
            g_lockHr = (HRESULT)w;
            g_lsf = (int)(l & (LSF_LOCKSCREEN | LSF_BACKGROUND));
            g_lockBusy = LockScreen_Busy();
            FinishPendingStop();
            if (!g_lockBusy && !UI_IsOpen()) Host_TrimMemory();  // the frame decoder and WinRT are done
            UI_Refresh();
            return 0;
        case WM_DESTROY:
            g_exiting = true;
            Media_Stop();
            SyncClock();  // takes the clock off the desktop
            if (!g_endingSession) {
                SyncLockScreen();  // exiting: the user's own pictures again (VideoBG no longer shows a video)
                if (g_lsf & LSF_BACKGROUND) LockScreen_WaitBackground(1500);  // before the video goes
            }
            StopRenderer();
            if (!g_endingSession && !LockScreen_Wait(4000)) Log(L"tray: lock screen still busy at exit");
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
    }
    if (m == g_taskbarCreated && m) {
        // Explorer restarted: re-add the tray icon and the clock; the renderer restarts itself.
        AddTray();
        SyncClock(true);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Services for the settings UI

bool Host_IsOn() { return g_on && !g_batteryOff; }

void Host_Update() { Update_Start(g_wnd, false); }

void Host_SetOn(bool on) {
    Log(L"tray: wallpaper turned %ls", on ? L"on" : L"off");
    g_on = on;
    g_failedVideo.clear();
    g_failText = FaultText{};
    g_batteryOff = false;  // a manual choice overrides the battery policy until the power source changes
    g_override = on && g_onBattery;
    g_settings.lastOn = on;
    SaveSettings(g_settings);
    if (on && g_settings.video.empty()) {
        g_on = false;
        UI_Show();
    }
    Reconcile();
}

bool Host_SetHotkey(UINT mods, UINT vk) {
    if (!RegisterToggleHotkey(mods, vk)) {
        RegisterToggleHotkey(g_settings.hkMods, g_settings.hkVk);
        return false;
    }
    g_settings.hkMods = mods;
    g_settings.hkVk = vk;
    Log(L"tray: hotkey changed to %ls", HotkeyToString(mods, vk).c_str());
    SaveSettings(g_settings);
    UpdateTray();
    return true;
}

bool Host_SetMusicHotkey(UINT mods, UINT vk) {
    if (!RegisterMusicHotkey(mods, vk)) {
        RegisterMusicHotkey(g_settings.musicHkMods, g_settings.musicHkVk);
        return false;
    }
    g_settings.musicHkMods = mods;
    g_settings.musicHkVk = vk;
    Log(L"tray: music shortcut changed to %ls", HotkeyToString(mods, vk).c_str());
    SaveSettings(g_settings);
    return true;
}

void Host_SettingsChanged() {
    SaveSettings(g_settings);
    if (g_ctl) PostMessageW(g_ctl, WM_VBG_RELOAD, 0, 0);
    ApplyBatteryPolicy();
    SyncClock();
    SyncLockScreen();  // the video or its crop may have changed
    Modern_SettingsChanged();
    Media_Update();
}

void Host_FrameChanged() {
    SaveSettings(g_settings);
    SyncLockScreen();
}

void Host_SetLockFollow(bool on) {
    g_settings.lockFollow = on;
    SaveSettings(g_settings);
    Log(L"tray: lock screen and background %ls the wallpaper", on ? L"follow" : L"no longer follow");
    g_lockWanted.clear();
    if (on) {
        SyncLockScreen(true);
    } else {  // back to the user's own pictures, then left alone
        g_lockBusy = true;
        LockScreen_Sync(g_wnd, WM_VBG_LOCKSCREEN, LockScreenWant{});
    }
    UI_Refresh();
}

bool Host_LockScreen(HRESULT* lastResult, int* lsf) {
    if (lastResult) *lastResult = g_lockHr;
    if (lsf) *lsf = g_lsf;
    return g_lockBusy;
}

void Host_SetVideo(const std::wstring& path, const Crop* keepCrop) {
    Log(L"tray: video set to %ls", path.c_str());
    g_settings.video = path;
    // Its version is remembered for its original (both looks' lists play it from now on).
    const std::wstring orig = LightCopyOriginalPath(path);
    SetVideoVersion(orig.empty() ? path : orig, orig.empty() ? L"" : path);
    g_failedVideo.clear();
    g_failText = FaultText{};
    LoadVideoProfile(g_settings);  // its crop and clock look, wherever the file lives now
    if (keepCrop) g_settings.crop = *keepCrop;
    Host_SettingsChanged();
    if (!Host_IsOn()) Host_SetOn(true);  // picking a video implies wanting to see it
    UI_Refresh();
}

void Host_VideoPlayable(const std::wstring& path) {
    if (g_failedVideo.empty() || g_on || _wcsicmp(path.c_str(), g_failedVideo.c_str()) != 0 ||
        _wcsicmp(path.c_str(), g_settings.video.c_str()) != 0)
        return;
    Log(L"tray: the video can be read now, turning the wallpaper back on");
    Host_SetOn(true);
}

bool Host_PlayFailed(FaultText* why) {
    if (g_failText.what.empty() || g_on) return false;
    *why = g_failText;
    return true;
}

void Host_SetLook(int look) {
    if (look == g_settings.look) return;
    g_settings.look = look;
    Log(L"tray: the %ls look", look == 1 ? L"modern" : L"classic");
    Host_SettingsChanged();  // the renderer's battery pause follows the look
    UpdateTray();
    if (look == 0) Modern_Close();
}

void Host_Showcase(HWND under) {
    if (g_ctl) PostMessageW(g_ctl, WM_VBG_SHOWCASE, under ? 1 : 0, (LPARAM)under);
}

void Host_ShowcaseLook(const ShowcaseLook* look) {
    if (!g_ctl) return;
    if (!look) {
        PostMessageW(g_ctl, WM_VBG_SHOWCASE_DIM, 0, 0);
        return;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    auto ticks = [&](double s) { return (LPARAM)(s * (double)f.QuadPart); };
    auto ms = [](double s) { return (WPARAM)(s * 1000 + 0.5); };
    const WPARAM bright = (WPARAM)std::clamp((int)(look->bright * 1000 + 0.5f), 1, 1000);
    PostMessageW(g_ctl, WM_VBG_SHOWCASE_DIM, bright | ms(look->dimFor) << 16, ticks(look->dimAt));
    if (look->box.right > look->box.left) PostMessageW(g_ctl, WM_VBG_SHOWCASE_BOX, 0, PackBox(look->box));
    if (look->growAt > 0) PostMessageW(g_ctl, WM_VBG_SHOWCASE_GROW, ms(look->growFor), ticks(look->growAt));
}

void Host_HoldWallpaper(bool hold) {
    if (g_ctl) PostMessageW(g_ctl, WM_VBG_HOLD, hold ? 1 : 0, 0);
}

void Host_PreviewVideo(const std::wstring& path, double time, UINT seq) {
    if (!g_ctl) return;
    std::vector<BYTE> buf(offsetof(PreviewVideoData, path) + (path.size() + 1) * sizeof(wchar_t));
    auto* pv = (PreviewVideoData*)buf.data();
    pv->time = time;
    pv->seq = seq;
    memcpy(pv->path, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    COPYDATASTRUCT cd{COPY_PREVIEW_VIDEO, (DWORD)buf.size(), buf.data()};
    DWORD_PTR res = 0;
    SendMessageTimeoutW(g_ctl, WM_COPYDATA, (WPARAM)g_wnd, (LPARAM)&cd, SMTO_ABORTIFHUNG, 500, &res);
}

void Host_PreviewSize(UINT w, UINT h) {
    if (g_ctl) PostMessageW(g_ctl, WM_VBG_PREVIEW_SIZE, 0, (LPARAM)(((UINT64)h << 16) | w));
}

void Host_PreviewCrop(const Crop* c) {
    if (g_ctl) PostMessageW(g_ctl, WM_VBG_PREVIEW_CROP, c ? (c->stretch ? 2 : 1) : 0, c ? PackCrop(*c) : 0);
}

RendererState Host_RendererState(LPARAM* mask) {
    if (mask) *mask = g_rmask;
    return g_rstate;
}

namespace {
struct PmcEx2 {  // PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 2004+), missing from older headers
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
        QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage,
        PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};
SIZE_T PrivateWorkingSet(HANDLE proc) {
    PmcEx2 pmc{};
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(proc, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc)) return pmc.PrivateWorkingSetSize;
    PROCESS_MEMORY_COUNTERS_EX p{};
    p.cb = sizeof p;
    return GetProcessMemoryInfo(proc, (PROCESS_MEMORY_COUNTERS*)&p, sizeof p) ? p.WorkingSetSize : 0;
}
}  // namespace

bool Host_RendererMemory(SIZE_T* privateWs, SIZE_T* graphics) {
    if (!g_proc) { g_rendererGraphics = 0; return false; }
    *privateWs = PrivateWorkingSet(g_proc);
    *graphics = (SIZE_T)g_rendererGraphics;
    if (g_ctl) PostMessageW(g_ctl, WM_VBG_MEMORY, 0, 0);  // a fresh figure for next time
    return true;
}

bool Host_BatteryOff() { return g_batteryOff; }

void Host_SuspendHotkey(bool suspend) {
    g_hotkeysOff = suspend;
    if (suspend) {
        UnregisterHotKey(g_wnd, HOTKEY_ID);
        UnregisterHotKey(g_wnd, HOTKEY_MUSIC);
    } else {
        RegisterToggleHotkey(g_settings.hkMods, g_settings.hkVk);
        RegisterMusicHotkey(g_settings.musicHkMods, g_settings.musicHkVk);
    }
}

SIZE_T Host_SelfMemory() { return PrivateWorkingSet(GetCurrentProcess()); }

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool Host_StartupRegistered(bool* enabled) {
    *enabled = false;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool exists = RegQueryValueExW(k, APP_NAME, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    if (!exists) return false;
    // Windows records the Settings > Startup toggle here: first byte 2 = enabled, 3 = disabled.
    *enabled = true;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        BYTE data[12] = {};
        DWORD size = sizeof data;
        if (RegQueryValueExW(k, APP_NAME, nullptr, nullptr, data, &size) == ERROR_SUCCESS && size > 0)
            *enabled = (data[0] & 1) == 0;
        RegCloseKey(k);
    }
    return true;
}

void Host_RegisterStartup() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    std::wstring v = L"\"" + ExePath() + L"\" --startup";
    RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

void Host_TrimMemory() {
    CoFreeUnusedLibrariesEx(0, 0);  // codec/driver DLLs pulled in by previews or a light copy
    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

bool Host_PickVideo(HWND owner) { return PickVideo(owner); }

const std::vector<std::wstring>& Host_Songs() {
    if (!g_songsRead) {
        g_songs = LoadPlaylist();
        g_songsRead = true;
    }
    return g_songs;
}

bool Host_PickMusic(HWND owner, bool add) {
    const std::vector<std::wstring> files = PickMusic(owner, add);
    if (files.empty()) return false;
    if (add && !Host_Songs().empty()) {  // onto the end (in a shuffled order too); what plays carries on
        std::vector<std::wstring> list = Host_Songs();
        std::vector<int> order = MusicOrderFor(g_settings, list.size());
        for (const std::wstring& f : files) {
            if (std::any_of(list.begin(), list.end(), [&](const std::wstring& s) { return _wcsicmp(s.c_str(), f.c_str()) == 0; })) continue;
            order.push_back((int)list.size());
            list.push_back(f);
        }
        Log(L"tray: %zu song(s) added to my music", list.size() - Host_Songs().size());
        if (list.size() == Host_Songs().size()) return false;  // (all there already)
        SetSongs(list);
        g_settings.musicOrder = MusicShuffled(order) ? order : std::vector<int>{};
        Host_SettingsChanged();
        UI_Refresh();
        return true;
    }
    SetSongs(files);
    Log(L"tray: %zu song(s) chosen for my music", files.size());
    g_settings.sound = 2;
    g_settings.musicOrder.clear();  // new songs: in the order chosen, waiting for Play
    g_settings.musicPlaying = false;
    g_settings.musicTrack = -1;
    g_settings.musicPosMs = g_settings.musicDurMs = 0;
    if (g_settings.volume == 0) g_settings.volume = 60;
    Host_SettingsChanged();
    return true;
}

int Host_CurrentTrack() { return g_track; }

void Host_MusicCommand(MusicCommand cmd, LPARAM value) {
    if (g_ctl) PostMessageW(g_ctl, WM_VBG_MUSIC, cmd, value);
}

MusicState Host_MusicState() { return g_music; }

void Host_KeepMusicPlace() { KeepMusicPlace(); }

void Host_PlayMusic(bool play) {
    if (play == g_settings.musicPlaying) return;
    g_settings.musicPlaying = play;
    Log(L"tray: music %ls", play ? L"played" : L"paused");
    Host_SettingsChanged();
    UI_Refresh();
}

void Host_SkipMusic(bool next) {
    Host_MusicCommand(next ? MC_NEXT : MC_PREV, 0);
    Host_PlayMusic(true);
}

// My music's play order, the same in both looks. The order chosen (as Windows listed the files) is
// the only "unshuffled" one: shuffling makes a new order, and moving a song by hand does too;
// shuffle off goes back to the order chosen.
void Host_ShuffleMusic(size_t n) {
    if (n < 2) return;
    std::vector<int> order = MusicOrderFor(g_settings, n);
    if (MusicShuffled(order)) {
        g_settings.musicOrder.clear();  // back to the order chosen
    } else {  // the song playing stays first; the rest in random order
        const int cur = g_music.track;
        for (size_t i = 0; i < n; i++)
            if (order[i] == cur) std::swap(order[0], order[i]);
        static unsigned long long seed = GetTickCount64() * 2654435761ULL;
        for (size_t i = n - 1; i > 1; i--) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            std::swap(order[i], order[1 + (size_t)((seed >> 33) % i)]);
        }
        g_settings.musicOrder = order;
    }
    Log(L"music: %ls", g_settings.musicOrder.empty() ? L"in the order chosen" : L"shuffled");
    Host_SettingsChanged();
}

void Host_PlaceSong(size_t n, int song, int to) {
    std::vector<int> order = MusicOrderFor(g_settings, n);
    auto it = std::find(order.begin(), order.end(), song);
    if (it == order.end()) return;
    const int from = (int)(it - order.begin());
    to = std::clamp(to, 0, (int)order.size() - 1);
    if (from == to) return;
    order.erase(it);
    order.insert(order.begin() + to, song);
    if (MusicShuffled(order)) g_settings.musicOrder = order;
    else g_settings.musicOrder.clear();
    Log(L"music: song moved from %d to %d", from + 1, to + 1);
    Host_SettingsChanged();
}

void Host_RemoveSong(int song) {
    std::vector<std::wstring> list = Host_Songs();
    const int n = (int)list.size();
    if (song < 0 || song >= n) return;
    std::vector<int> order = MusicOrderFor(g_settings, n);
    const auto at = std::find(order.begin(), order.end(), song);
    if (at == order.end()) return;
    const bool live = g_music.track >= 0 && g_music.track < n;
    int current = live ? g_music.track : std::clamp(g_settings.musicTrack, 0, n - 1);
    UINT pos = live ? g_music.posMs : g_settings.musicPosMs, dur = live ? g_music.durMs : g_settings.musicDurMs;
    if (current == song) {  // the one playing: the next one takes its place, from its start
        current = n > 1 ? *(at + 1 == order.end() ? order.begin() : at + 1) : 0;
        pos = dur = 0;
    }
    Log(L"music: %ls taken off the playlist", list[song].c_str());
    list.erase(list.begin() + song);
    order.erase(at);
    for (int& i : order)
        if (i > song) i--;
    if (current > song) current--;
    SetSongs(list);
    if (live) g_music.track = g_track = list.empty() ? -1 : current;  // (until the renderer says so itself)
    g_settings.musicOrder = MusicShuffled(order) ? order : std::vector<int>{};
    g_settings.musicTrack = list.empty() ? 0 : current;
    g_settings.musicPosMs = pos;  // (it carries on there with the list it has now)
    g_settings.musicDurMs = dur;
    if (list.empty()) g_settings.musicPlaying = false;
    Host_SettingsChanged();
    UI_Refresh();
}

void Host_LiveVolume(int volume) {
    if (g_ctl) PostMessageW(g_ctl, WM_VBG_VOLUME, (WPARAM)std::clamp(volume, 0, 100), 0);
}

void Host_SeekWallpaper(double seconds, bool exact) {
    if (g_ctl) PostMessageW(g_ctl, WM_VBG_SEEK, (WPARAM)std::max(0.0, seconds * 1000 + 0.5), exact ? 1 : 0);
}

bool Host_ClockLive() { return WallpaperShowing(); }

void Host_ClockPreview(const ClockLook* look) {
    g_clockPreviewOn = look != nullptr;
    if (look) g_clockPreview = *look;
}

void Host_ClockMove(const ClockLook& look) {
    if (!look.show) return;
    Clock_Show(g_wnd, look);
    g_clockShown.clear();  // the next sync puts the saved look back in full
}

// ---------------------------------------------------------------------------------------

int HostMain(HINSTANCE inst, bool openSettings, bool openPicker, int wallpaperOn) {
    g_inst = inst;
    InstallCrashLog(L"tray app");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    LoadSettings(g_settings);
    if (!openSettings && !openPicker && g_settings.musicPlaying) {  // started with Windows: My music waits for Play (never loud at sign-in)
        g_settings.musicPlaying = false;
        SaveSettings(g_settings);
        Log(L"tray: started with Windows, so the music stays paused");
    }
    if (!openSettings && !openPicker && g_settings.sound == 1 && !g_settings.videoMuted) {  // and the video's sound starts muted
        g_settings.videoMuted = true;  // (the video plays; the sound shortcut or Unmute brings the sound back)
        SaveSettings(g_settings);
        Log(L"tray: started with Windows, so the video's sound starts muted");
    }

    g_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl{};
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &jl, sizeof jl);

    int small = GetSystemMetrics(SM_CXSMICON);
    g_icoOn = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, small, small, 0);
    g_icoOff = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_OFF), IMAGE_ICON, small, small, 0);

    WNDCLASSW wc{};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = inst;
    wc.lpszClassName = HOST_CLASS;
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, HOST_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_wnd, g_taskbarCreated, MSGFLT_ALLOW, nullptr);
    AddTray();
    Media_Start(g_wnd);

    if (!RegisterToggleHotkey(g_settings.hkMods, g_settings.hkVk)) {
        Balloon(L"VideoBG's hotkey isn't available",
                HotkeyToString(g_settings.hkMods, g_settings.hkVk) + L" is used by another app. Pick a different one in Settings.");
    }
    if (!RegisterMusicHotkey(g_settings.musicHkMods, g_settings.musicHkVk))  // quietly: it's only for My music
        Log(L"tray: the music shortcut %ls is taken by another app", HotkeyToString(g_settings.musicHkMods, g_settings.musicHkVk).c_str());

    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) g_onBattery = ps.ACLineStatus == 0;
    g_power = RegisterPowerSettingNotification(g_wnd, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);

    g_on = wallpaperOn >= 0 ? wallpaperOn == 1 : g_settings.launch == 0 || (g_settings.launch == 1 && g_settings.lastOn);
    if (wallpaperOn >= 0) Update_CleanUp();
    ApplyBatteryPolicy();
    Reconcile();

    if (g_settings.video.empty()) {
        if (g_settings.look != 1 || !Modern_Open(0)) UI_Show();  // the modern look: its picker, to add videos
        Balloon(L"VideoBG is in your tray", L"Choose a video to use as your wallpaper.");
    } else if (openSettings) {
        UI_Show();
    } else if (openPicker) {
        Modern_Open(0);
    }
    SetTimer(g_wnd, TIMER_TRIM, 3000, nullptr);
    SetTimer(g_wnd, TIMER_CHECK, 15000, nullptr);  // a new version? (once VideoBG has settled in)

    MSG msg;
    for (;;) {
        // The renderer ending, the picker's next frame (only while something in it moves), or messages.
        HANDLE waits[2];
        DWORD n = 0;
        if (g_proc) waits[n++] = g_proc;
        HANDLE frame = Modern_FrameWait();
        if (frame) waits[n++] = frame;
        DWORD r = MsgWaitForMultipleObjectsEx(n, waits, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (g_proc && r == WAIT_OBJECT_0) {
            OnRendererExit();
            continue;
        }
        if (frame && r == WAIT_OBJECT_0 + n - 1) Modern_Frame();  // then the messages, so input never waits behind frames
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
    }
    if (g_power) UnregisterPowerSettingNotification(g_power);
    CloseHandle(g_job);
    CoUninitialize();
    return 0;
}
