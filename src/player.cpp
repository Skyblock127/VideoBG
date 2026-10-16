// Desktop music player, for the modern look: the song playing and its place, previous / play-pause /
// next and the volume, in a small glass card on the desktop above the wallpaper. Like the clock it's
// a per-pixel transparent window owned by the desktop (so it stays behind the apps), but it takes
// clicks: its buttons work, and dragging it anywhere else moves it. Drawn with Direct2D into the
// layered window, again only when what it shows changes (the song, its second, the pointer).
#include "common.h"
#include <d2d1.h>
#include <dwrite.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace {

constexpr wchar_t kClass[] = L"VideoBG.Player";
enum Part { PART_NONE, PART_PREV, PART_PLAY, PART_NEXT, PART_VOLUME, PART_BODY };

// What a style shows.
struct Shown {
    std::wstring song, where;  // the song's name; its place ("1:23 / 3:45", or "Paused ...")
    bool playing = false;
    float done = 0;            // how far into the song (0..1)
    int volume = 0;
    D2D1_COLOR_F accent{};
};

struct PlayerWindow {
    HWND hwnd = nullptr, host = nullptr, desktop = nullptr;
    HMODULE d2dDll = nullptr, dwDll = nullptr;
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    ID2D1DCRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    IDWriteTextFormat *fTitle = nullptr, *fSmall = nullptr, *fIcon = nullptr, *fIconBig = nullptr;
    std::vector<std::wstring> songs;
    float scale = 1;
    int w = 0, h = 0;  // pixels
    int hot = PART_NONE, press = PART_NONE;
    bool dragging = false, sliding = false;
    POINT grab{};      // where the pointer took hold of it, in the window
    std::wstring drawn;  // what it shows now, to skip identical redraws
} W;

template <class T> void Rel(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

D2D1_COLOR_F Col(UINT32 rgb, float a = 1) {
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}

D2D1_COLOR_F Accent() {
    BYTE pal[32] = {};
    DWORD size = sizeof pal;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                     RRF_RT_REG_BINARY, nullptr, pal, &size) == ERROR_SUCCESS && size >= 32)
        return D2D1::ColorF(pal[4] / 255.f, pal[5] / 255.f, pal[6] / 255.f);
    return Col(0x60CDFF);
}


std::wstring SongName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring n = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = n.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? n : n.substr(0, dot);
}

std::wstring Minutes(UINT ms) {
    wchar_t b[32];
    swprintf(b, 32, L"%u:%02u", ms / 60000, ms / 1000 % 60);
    return b;
}

IDWriteTextFormat* Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
    IDWriteTextFormat* f = nullptr;
    if (FAILED(W.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f)))
        return nullptr;
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetTextAlignment(align);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteInlineObject* dots = nullptr;
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    if (SUCCEEDED(W.dw->CreateEllipsisTrimmingSign(f, &dots))) {
        f->SetTrimming(&trim, dots);
        dots->Release();
    }
    return f;
}

bool StartDrawing() {
    if (W.rt) return true;
    if (!W.d2dDll) W.d2dDll = LoadLibraryExW(L"d2d1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!W.dwDll) W.dwDll = LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto d2dCreate = W.d2dDll ? (HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**))(void*)GetProcAddress(
                                    W.d2dDll, "D2D1CreateFactory")
                              : nullptr;
    auto dwCreate = W.dwDll ? (HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**))(void*)GetProcAddress(W.dwDll, "DWriteCreateFactory")
                            : nullptr;
    D2D1_FACTORY_OPTIONS fo{};
    if (!d2dCreate || !dwCreate || FAILED(d2dCreate(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &fo, (void**)&W.d2d)) ||
        FAILED(dwCreate(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&W.dw)))
        return false;
    const D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * W.scale, 96 * W.scale);
    if (FAILED(W.d2d->CreateDCRenderTarget(&rp, &W.rt))) return false;
    W.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    W.rt->CreateSolidColorBrush(Col(0xFFFFFF), &W.br);
    W.fTitle = Format(L"Segoe UI Variable Display", 14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    W.fSmall = Format(L"Segoe UI Variable Text", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    W.fIcon = Format(L"Segoe Fluent Icons", 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    W.fIconBig = Format(L"Segoe Fluent Icons", 15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    return W.br && W.fTitle && W.fSmall && W.fIcon && W.fIconBig;
}

void StopDrawing() {
    for (IDWriteTextFormat** f : {&W.fTitle, &W.fSmall, &W.fIcon, &W.fIconBig}) Rel(*f);
    Rel(W.br);
    Rel(W.rt);
    Rel(W.dw);
    Rel(W.d2d);
    for (HMODULE* m : {&W.d2dDll, &W.dwDll}) {
        if (*m) FreeLibrary(*m);
        *m = nullptr;
    }
}

void Text(const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, D2D1_COLOR_F c) {
    if (s.empty() || !f) return;
    W.br->SetColor(c);
    W.rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, W.br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius) {
    W.br->SetColor(c);
    W.rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), W.br);
}

void Circle(D2D1_POINT_2F p, float r, D2D1_COLOR_F c) {
    W.br->SetColor(c);
    W.rt->FillEllipse(D2D1::Ellipse(p, r, r), W.br);
}

// ---- Styles ---------------------------------------------------------------------------------
// Settings::musicPlayer is 0 for none, else 1 + one of these. Each has its size, draws itself, says
// which of its parts is at a point, and where its volume slider runs. (More styles will join.)

// "Player": a glass card with the song, a line for how far in it is, the buttons and the volume.
constexpr float kCardW = 340, kCardH = 92, kCardR = 18;
const D2D1_RECT_F kCardVolume = D2D1::RectF(176, 72, 296, 76);

D2D1_POINT_2F CardButton(int part) { return D2D1::Point2F(part == PART_PREV ? 34.f : part == PART_PLAY ? 72.f : 110.f, 74); }

int CardHit(float x, float y) {
    for (int p : {PART_PREV, PART_PLAY, PART_NEXT}) {
        const D2D1_POINT_2F c = CardButton(p);
        if (fabsf(x - c.x) <= 17 && fabsf(y - c.y) <= 17) return p;
    }
    if (x >= kCardVolume.left - 26 && x <= kCardVolume.right + 10 && y >= 60 && y <= 88) return PART_VOLUME;
    return x >= 0 && y >= 0 && x < kCardW && y < kCardH ? PART_BODY : PART_NONE;
}

void DrawCardStyle(const Shown& s);

struct PlayerStyle {
    const wchar_t* name;
    float w, h;          // DIPs
    D2D1_RECT_F volume;  // its volume slider's track
    void (*draw)(const Shown&);
    int (*hit)(float x, float y);
};

const PlayerStyle kStyles[] = {
    {L"Player", kCardW, kCardH, kCardVolume, DrawCardStyle, CardHit},
};

const PlayerStyle& Style() {
    return kStyles[std::clamp(g_settings.musicPlayer - 1, 0, (int)std::size(kStyles) - 1)];
}

int HitPart(float x, float y) { return Style().hit(x, y); }

void DrawCardStyle(const Shown& s) {
    const D2D1_RECT_F card = D2D1::RectF(0.5f, 0.5f, kCardW - 0.5f, kCardH - 0.5f);
    Fill(card, Col(0x141417, 0.8f), kCardR);
    W.br->SetColor(Col(0xFFFFFF, 0.1f));
    W.rt->DrawRoundedRectangle(D2D1::RoundedRect(card, kCardR, kCardR), W.br, 1);
    Text(L"\uE8D6", D2D1::RectF(14, 10, 32, 32), W.fIcon, s.accent);
    Text(s.song, D2D1::RectF(38, 10, kCardW - 16, 32), W.fTitle, Col(0xFFFFFF, 0.95f));
    Text(s.where, D2D1::RectF(38, 30, kCardW - 16, 46), W.fSmall, Col(0xFFFFFF, 0.6f));
    const D2D1_RECT_F line = D2D1::RectF(16, 52, kCardW - 16, 54.5f);
    Fill(line, Col(0xFFFFFF, 0.14f), 1.25f);
    if (s.done > 0) Fill(D2D1::RectF(line.left, line.top, line.left + (line.right - line.left) * s.done, line.bottom), s.accent, 1.25f);
    // Previous, play / pause (lit), next.
    for (int p : {PART_PREV, PART_NEXT}) {
        const D2D1_POINT_2F c = CardButton(p);
        if (W.hot == p) Circle(c, 15, Col(0xFFFFFF, W.press == p ? 0.16f : 0.1f));
        Text(p == PART_PREV ? L"\uE892" : L"\uE893", D2D1::RectF(c.x - 15, c.y - 15, c.x + 15, c.y + 15), W.fIcon, Col(0xFFFFFF, 0.9f));
    }
    const D2D1_POINT_2F pc = CardButton(PART_PLAY);
    D2D1_COLOR_F pf = s.accent;
    if (W.hot == PART_PLAY) pf = D2D1::ColorF(std::min(1.f, pf.r + 0.06f), std::min(1.f, pf.g + 0.06f), std::min(1.f, pf.b + 0.06f));
    Circle(pc, 17, pf);
    Text(s.playing ? L"\uE769" : L"\uE768", D2D1::RectF(pc.x - 17, pc.y - 17, pc.x + 17, pc.y + 17), W.fIconBig, Col(0x000000, 0.88f));
    // The volume (VideoBG's own, as in its settings).
    const D2D1_RECT_F& t = kCardVolume;
    Text(s.volume == 0 ? L"\uE74F" : s.volume < 34 ? L"\uE993" : s.volume < 67 ? L"\uE994" : L"\uE995", D2D1::RectF(146, 60, 170, 88),
         W.fIcon, Col(0xFFFFFF, 0.8f));
    Fill(t, Col(0xFFFFFF, 0.16f), 2);
    const float vx = t.left + (t.right - t.left) * s.volume / 100.f;
    Fill(D2D1::RectF(t.left, t.top, vx, t.bottom), s.accent, 2);
    Circle(D2D1::Point2F(vx, (t.top + t.bottom) / 2), W.hot == PART_VOLUME || W.sliding ? 7.f : 6.f, Col(0xFFFFFF));
    wchar_t pct[16];
    swprintf(pct, 16, L"%d%%", s.volume);
    Text(pct, D2D1::RectF(302, 60, kCardW - 10, 88), W.fSmall, Col(0xFFFFFF, 0.6f));
}

// ---- The window --------------------------------------------------------------------------------

// The top-left corner (pixels) that puts its centre where the settings say, kept on the main screen.
POINT Origin() {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& m = mi.rcWork;
    const LONG x = (LONG)lroundf(mi.rcMonitor.left + g_settings.playerX * (mi.rcMonitor.right - mi.rcMonitor.left) - W.w / 2.f);
    const LONG y = (LONG)lroundf(mi.rcMonitor.top + g_settings.playerY * (mi.rcMonitor.bottom - mi.rcMonitor.top) - W.h / 2.f);
    return POINT{std::clamp(x, m.left, std::max(m.left, m.right - W.w)), std::clamp(y, m.top, std::max(m.top, m.bottom - W.h))};
}

void Draw(bool force = false) {
    if (!W.hwnd || !StartDrawing()) return;
    const MusicState ms = Host_MusicState();
    const int n = (int)W.songs.size();
    const int kept = g_settings.musicTrack >= 0 && g_settings.musicTrack < n ? g_settings.musicTrack : 0;
    const int track = ms.track >= 0 && ms.track < n ? ms.track : kept;
    const bool playing = g_settings.musicPlaying;
    const UINT pos = ms.track >= 0 ? ms.posMs : g_settings.musicPosMs, dur = ms.track >= 0 ? ms.durMs : g_settings.musicDurMs;
    wchar_t key[160];
    swprintf(key, 160, L"%d|%d|%d|%u|%u|%d|%d|%d|%.2f", g_settings.musicPlayer, track, playing, pos / 1000, dur / 1000, W.hot, W.press,
             g_settings.volume, W.scale);
    if (!force && W.drawn == key) return;

    const PlayerStyle& style = Style();
    W.w = (int)ceilf(style.w * W.scale);
    W.h = (int)ceilf(style.h * W.scale);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W.w;
    bi.bmiHeader.biHeight = -W.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(dc);
        return;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    const RECT all{0, 0, W.w, W.h};
    W.rt->BindDC(dc, &all);
    W.rt->BeginDraw();
    W.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    Shown shown;
    shown.song = n ? SongName(W.songs[track]) : L"No songs chosen yet";
    shown.where = !playing ? L"Paused" + (dur ? L"  \u00B7  " + Minutes(pos) + L" / " + Minutes(dur) : L"")
                           : dur ? Minutes(pos) + L" / " + Minutes(dur) : L"";
    shown.playing = playing;
    shown.done = dur ? std::min(1.f, (float)pos / dur) : 0;
    shown.volume = g_settings.volume;
    shown.accent = Accent();
    style.draw(shown);
    const HRESULT hr = W.rt->EndDraw();
    if (SUCCEEDED(hr)) {
        POINT dst = Origin(), zero{0, 0};
        SIZE sz{W.w, W.h};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (UpdateLayeredWindow(W.hwnd, nullptr, &dst, &sz, dc, &zero, 0, &bf, ULW_ALPHA)) W.drawn = key;
    } else if (hr == D2DERR_RECREATE_TARGET) {
        StopDrawing();
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void SetVolumeAt(float x) {
    const D2D1_RECT_F& t = Style().volume;
    const int v = (int)lroundf(std::clamp((x - t.left) / (t.right - t.left), 0.f, 1.f) * 100);
    if (v == g_settings.volume) return;
    g_settings.volume = v;
    g_settings.videoMuted = false;  // turning it up or down means listening
    Host_LiveVolume(v);             // heard while dragging; saved when let go
    Draw();
}

LRESULT CALLBACK PlayerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const float x = GET_X_LPARAM(l) / W.scale, y = GET_Y_LPARAM(l) / W.scale;
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // a click doesn't take the focus from the app in use
        case WM_MOUSEMOVE: {
            if (W.sliding) {
                SetVolumeAt(x);
                return 0;
            }
            if (W.dragging) {  // follows the pointer; the place is kept when let go
                POINT p;
                GetCursorPos(&p);
                SetWindowPos(h, nullptr, p.x - W.grab.x, p.y - W.grab.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            const int hot = HitPart(x, y);
            if (hot != W.hot) {
                W.hot = hot;
                Draw();
            }
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (!W.dragging && !W.sliding) {
                W.hot = PART_NONE;
                Draw();
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetCapture(h);
            W.press = HitPart(x, y);
            if (W.press == PART_VOLUME) {
                W.sliding = true;
                SetVolumeAt(x);
            } else if (W.press == PART_BODY) {
                W.dragging = true;
                W.grab = POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            }
            Draw();
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const int part = W.press;
            W.press = PART_NONE;
            if (W.sliding) {
                W.sliding = false;
                Host_SettingsChanged();
                UI_Refresh();
            } else if (W.dragging) {
                W.dragging = false;
                RECT r;
                GetWindowRect(h, &r);
                MONITORINFO mi{sizeof mi};
                GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
                const RECT& mon = mi.rcMonitor;
                g_settings.playerX = std::clamp(((r.left + r.right) / 2.f - mon.left) / (mon.right - mon.left), 0.f, 1.f);
                g_settings.playerY = std::clamp(((r.top + r.bottom) / 2.f - mon.top) / (mon.bottom - mon.top), 0.f, 1.f);
                SaveSettings(g_settings);
                Log(L"player: moved to %.1f%%, %.1f%%", g_settings.playerX * 100, g_settings.playerY * 100);
            } else if (part == HitPart(x, y)) {
                if (part == PART_PLAY) Host_PlayMusic(!g_settings.musicPlaying);
                else if (part == PART_PREV || part == PART_NEXT) Host_SkipMusic(part == PART_NEXT);
            }
            Draw();
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, W.hot == PART_BODY || W.dragging ? IDC_SIZEALL : IDC_HAND));
                return TRUE;
            }
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

bool CreatePlayerWindow() {
    W.desktop = FindWindowW(L"Progman", nullptr);
    if (!W.desktop) return false;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = PlayerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassW(&wc) != 0;
    }
    W.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"VideoBG music", WS_POPUP, 0, 0, 1, 1, W.desktop,
                             nullptr, GetModuleHandleW(nullptr), nullptr);
    W.drawn.clear();
    return W.hwnd != nullptr;
}

}  // namespace

void Player_Show(const std::vector<std::wstring>& songs, bool force) {
    // Explorer restarted: the desktop is a new window now.
    if (W.hwnd && (!IsWindow(W.desktop) || GetWindow(W.hwnd, GW_OWNER) != W.desktop)) {
        DestroyWindow(W.hwnd);
        W.hwnd = nullptr;
    }
    const bool fresh = !W.hwnd;
    if (fresh && !CreatePlayerWindow()) return;
    const float scale = Clock_PixelScale();
    if (scale != W.scale) {
        W.scale = scale;
        StopDrawing();  // made again at the new size
        force = true;
    }
    W.songs = songs;
    Draw(force || fresh);
    const POINT p = Origin();
    SetWindowPos(W.hwnd, HWND_BOTTOM, p.x, p.y, W.w, W.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);  // bottom = just above the desktop
    if (fresh) Log(L"player: on the desktop");
}

std::vector<std::wstring> Player_Styles() {
    std::vector<std::wstring> v;
    for (const PlayerStyle& s : kStyles) v.push_back(s.name);
    return v;
}

void Player_Update() {
    if (W.hwnd && !W.dragging) Draw();
}

void Player_Hide() {
    if (!W.hwnd) return;
    DestroyWindow(W.hwnd);
    W.hwnd = nullptr;
    W.hot = W.press = PART_NONE;
    W.dragging = W.sliding = false;
    StopDrawing();
    Log(L"player: off the desktop");
}
