// Host process: tray icon, global hotkey, settings UI, and lifetime of the renderer process.
// While the wallpaper is OFF this is the only process left, and it idles in a few MB.
#include "common.h"
#include "resource.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <psapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cwchar>

Settings g_settings;

namespace {

constexpr UINT_PTR TIMER_RESTART = 1, TIMER_TRIM = 2, TIMER_CLOCK = 3, TIMER_STOP = 4;
constexpr int HOTKEY_ID = 1;
enum { CMD_TOGGLE = 100, CMD_SETTINGS, CMD_CHOOSE, CMD_EXIT };

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
bool g_exiting;         // quitting / logging off
std::wstring g_clockShown;  // what the desktop clock was last told to show, so repeated syncs cost nothing
std::wstring g_lockWanted;  // what the lock screen was last asked to show ("" = not asked yet)
bool g_lockBusy;            // the lock screen / background worker is at it
HRESULT g_lockHr = S_OK;    // its last result
int g_lsf;                  // LSF_*: what shows the video's frame (lock screen, background)
bool g_stopPending;         // the video waits for the background to be the user's picture again
bool g_endingSession;       // Windows is signing out / shutting down
UINT64 g_rendererGraphics;  // the renderer's graphics memory, as it last reported

bool Running() { return g_proc != nullptr; }

// ---------------------------------------------------------------------------------------
// Desktop clock: follows whichever wallpaper is showing.

bool WallpaperShowing() { return !g_exiting && g_on && !g_batteryOff && !g_settings.video.empty(); }

ClockLook WantedLook() {
    if (!WallpaperShowing()) return g_settings.clockStill;
    return g_settings.clockVideoOwn ? g_settings.clockVideo : g_settings.clockLive;
}

void SyncClock(bool force = false) {
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
    Log(L"clock: %ls look (%.1f%%, %.1f%%, colour %ls, size %.0f%%, opacity %.0f%%, glow %.0f%%)",
        WallpaperShowing() ? L"video wallpaper" : L"still wallpaper", look.x * 100, look.y * 100, look.color.c_str(), look.size * 100,
        look.opacity * 100, look.glow * 100);
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
        swprintf(b, 128, L"|%.3f|%.4f,%.4f,%.4f,%.4f|%dx%d", w.time, w.crop.l, w.crop.t, w.crop.r, w.crop.b, w.sw, w.sh);
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
                       L"\n" + HotkeyToString(g_settings.hkMods, g_settings.hkVk) + L" turns it on or off";
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

void ShowTrayMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    bool on = Host_IsOn();
    std::wstring t = std::wstring(L"Video wallpaper on\t") + HotkeyToString(g_settings.hkMods, g_settings.hkVk);
    AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), CMD_TOGGLE, t.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_SETTINGS, L"Settings\u2026");
    AppendMenuW(m, MF_STRING, CMD_CHOOSE, L"Choose video\u2026");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"Exit");
    SetMenuDefaultItem(m, CMD_SETTINGS, FALSE);
    SetForegroundWindow(g_wnd);
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// Multi-select song picker for "my music". Returns false if cancelled.
bool PickMusic(HWND owner) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    const COMDLG_FILTERSPEC types[] = {
        {L"Music", L"*.mp3;*.m4a;*.aac;*.wav;*.wma;*.flac;*.alac;*.ogg;*.opus;*.mp4"},
        {L"All files", L"*.*"},
    };
    dlg->SetFileTypes((UINT)std::size(types), types);
    dlg->SetTitle(L"Choose songs for the wallpaper (Ctrl+A selects a whole folder)");
    dlg->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT);
    std::vector<std::wstring> old = LoadPlaylist();
    std::wstring start = !old.empty() ? old[0] : L"";
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
    if (files.empty()) return false;
    SavePlaylist(files);
    Log(L"tray: %zu song(s) chosen for my music", files.size());
    return true;
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

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_HOTKEY:
            if (w == HOTKEY_ID) Host_SetOn(!Host_IsOn());
            return 0;
        case WM_VBG_TRAY:
            switch (LOWORD(l)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK: UI_Show(); break;
                case WM_CONTEXTMENU: ShowTrayMenu(POINT{GET_X_LPARAM(w), GET_Y_LPARAM(w)}); break;
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case CMD_TOGGLE: Host_SetOn(!Host_IsOn()); break;
                case CMD_SETTINGS: UI_Show(); break;
                case CMD_CHOOSE: PickVideo(nullptr); break;
                case CMD_EXIT: DestroyWindow(h); break;
            }
            return 0;
        case WM_VBG_RENDERER_READY: {
            DWORD pid = 0;
            GetWindowThreadProcessId((HWND)l, &pid);
            if (g_proc && pid == GetProcessId(g_proc)) g_ctl = (HWND)l;
            return 0;
        }
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
        case WM_VBG_SHOW: UI_Show(); return 0;
        case WM_VBG_TOGGLE: Host_SetOn(!Host_IsOn()); return 0;
        case WM_VBG_EXIT: DestroyWindow(h); return 0;
        case WM_TIMER:
            if (w == TIMER_RESTART) { KillTimer(h, TIMER_RESTART); Reconcile(); }
            else if (w == TIMER_TRIM) { KillTimer(h, TIMER_TRIM); Host_TrimMemory(); }
            else if (w == TIMER_CLOCK) { KillTimer(h, TIMER_CLOCK); SyncClock(true); }
            else if (w == TIMER_STOP) FinishPendingStop();
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

void Host_SettingsChanged() {
    SaveSettings(g_settings);
    if (g_ctl) PostMessageW(g_ctl, WM_VBG_RELOAD, 0, 0);
    ApplyBatteryPolicy();
    SyncClock();
    SyncLockScreen();  // the video or its crop may have changed
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

void Host_PreviewSize(UINT w, UINT h) {
    if (g_ctl) PostMessageW(g_ctl, WM_VBG_PREVIEW_SIZE, 0, (LPARAM)(((UINT64)h << 16) | w));
}

void Host_PreviewCrop(const Crop* c) {
    if (g_ctl) PostMessageW(g_ctl, WM_VBG_PREVIEW_CROP, c ? 1 : 0, c ? PackCrop(*c) : 0);
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
    if (suspend) UnregisterHotKey(g_wnd, HOTKEY_ID);
    else RegisterToggleHotkey(g_settings.hkMods, g_settings.hkVk);
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

bool Host_PickMusic(HWND owner) {
    if (!PickMusic(owner)) return false;
    g_settings.sound = 2;
    if (g_settings.volume == 0) g_settings.volume = 60;
    Host_SettingsChanged();
    return true;
}

int Host_CurrentTrack() { return g_track; }

bool Host_ClockLive() { return WallpaperShowing(); }

void Host_ClockMove(const ClockLook& look) {
    if (!look.show) return;
    Clock_Show(g_wnd, look);
    g_clockShown.clear();  // the next sync puts the saved look back in full
}

// ---------------------------------------------------------------------------------------

int HostMain(HINSTANCE inst, bool openSettings) {
    g_inst = inst;
    InstallCrashLog(L"tray app");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    LoadSettings(g_settings);

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

    if (!RegisterToggleHotkey(g_settings.hkMods, g_settings.hkVk)) {
        Balloon(L"VideoBG's hotkey isn't available",
                HotkeyToString(g_settings.hkMods, g_settings.hkVk) + L" is used by another app. Pick a different one in Settings.");
    }

    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) g_onBattery = ps.ACLineStatus == 0;
    g_power = RegisterPowerSettingNotification(g_wnd, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);

    g_on = g_settings.launch == 0 || (g_settings.launch == 1 && g_settings.lastOn);
    ApplyBatteryPolicy();
    Reconcile();

    if (g_settings.video.empty()) {
        UI_Show();
        Balloon(L"VideoBG is in your tray", L"Choose a video to use as your wallpaper.");
    } else if (openSettings) {
        UI_Show();
    }
    SetTimer(g_wnd, TIMER_TRIM, 3000, nullptr);

    MSG msg;
    for (;;) {
        DWORD n = g_proc ? 1 : 0;
        DWORD r = MsgWaitForMultipleObjectsEx(n, &g_proc, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (n && r == WAIT_OBJECT_0) {
            OnRendererExit();
            continue;
        }
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
