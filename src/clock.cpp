// Desktop clock: day name, date and time in the design of the "Mond" Rainmeter clock (the sizes and
// spacing of its Clock.ini; see Credits in README.md), drawn by VideoBG itself: one small see-through,
// click-through window on the desktop that redraws once a minute. Mond's day font, Anurati, is free
// for personal use only, so the day name is in Audiowide; the date and time are in Quicksand, as in Mond.
// DirectWrite/Direct2D are loaded only while drawing.
#include "common.h"
#include "resource.h"
#include <d2d1.h>
#include <dwrite_3.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

namespace {

template <class T> void Rel(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

const wchar_t* const kDays[] = {L"SUNDAY", L"MONDAY", L"TUESDAY", L"WEDNESDAY", L"THURSDAY", L"FRIDAY", L"SATURDAY"};
const wchar_t* const kMonths[] = {L"JANUARY", L"FEBRUARY", L"MARCH",     L"APRIL",   L"MAY",      L"JUNE",
                                  L"JULY",    L"AUGUST",   L"SEPTEMBER", L"OCTOBER", L"NOVEMBER", L"DECEMBER"};

struct Painter {
    HMODULE dw = nullptr, d2 = nullptr;
    IDWriteFactory5* f = nullptr;
    IDWriteInMemoryFontFileLoader* loader = nullptr;
    IDWriteFontCollection1* fonts = nullptr;  // the two fonts embedded in the exe, private to us
    ID2D1Factory* d2f = nullptr;
    bool failed = false;
} P;

bool AddFont(IDWriteFontSetBuilder1* b, int id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    if (!data) return false;
    IDWriteFontFile* file = nullptr;
    HRESULT hr = P.loader->CreateInMemoryFontFileReference(P.f, data, SizeofResource(nullptr, r), nullptr, &file);
    if (SUCCEEDED(hr)) hr = b->AddFontFile(file);
    Rel(file);
    return SUCCEEDED(hr);
}

bool OpenPainter() {
    if (P.fonts) return true;
    if (P.failed) return false;
    P.dw = LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    P.d2 = LoadLibraryExW(L"d2d1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using DWCreate = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
    using D2DCreate = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
    auto dwCreate = P.dw ? (DWCreate)(void*)GetProcAddress(P.dw, "DWriteCreateFactory") : nullptr;
    auto d2Create = P.d2 ? (D2DCreate)(void*)GetProcAddress(P.d2, "D2D1CreateFactory") : nullptr;
    HRESULT hr = dwCreate && d2Create ? S_OK : E_NOINTERFACE;
    // Isolated factory (needs Windows 10 1703+): the embedded fonts never reach other apps.
    if (SUCCEEDED(hr)) hr = dwCreate(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory5), (IUnknown**)&P.f);
    D2D1_FACTORY_OPTIONS opt{D2D1_DEBUG_LEVEL_NONE};
    if (SUCCEEDED(hr)) hr = d2Create(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opt, (void**)&P.d2f);
    if (SUCCEEDED(hr)) hr = P.f->CreateInMemoryFontFileLoader(&P.loader);
    if (SUCCEEDED(hr)) hr = P.f->RegisterFontFileLoader(P.loader);
    IDWriteFontSetBuilder1* b = nullptr;
    IDWriteFontSet* set = nullptr;
    if (SUCCEEDED(hr)) hr = P.f->CreateFontSetBuilder(&b);
    if (SUCCEEDED(hr) && !(AddFont(b, IDR_FONT_AUDIOWIDE) && AddFont(b, IDR_FONT_QUICKSAND))) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = b->CreateFontSet(&set);
    if (SUCCEEDED(hr)) hr = P.f->CreateFontCollectionFromFontSet(set, &P.fonts);
    Rel(set);
    Rel(b);
    if (FAILED(hr)) {
        Log(L"clock: can't set up text drawing (0x%08lx)", (unsigned long)hr);
        Clock_ReleasePainter();
        P.failed = true;
        return false;
    }
    return true;
}

// ---- The window on the desktop ------------------------------------------------------------
// A click-through, per-pixel transparent top-level window owned by the desktop (Progman): owned
// windows always stay right above their owner, so the clock sits on the desktop above the
// wallpaper, stays through "Show desktop", and goes away with the desktop if Explorer restarts.
// (A child of the desktop, like the video wallpaper, can't have per-pixel transparency there:
// Windows 11 24H2's desktop doesn't compose such children.)

struct ClockWindow {
    HWND hwnd = nullptr, host = nullptr, desktop = nullptr;
    ClockLook look;
    bool h24 = false;
    std::wstring drawn;  // what the window shows now (style + minute), to skip identical redraws
    int w = 0, h = 0;
    bool closing = false;
} W;

constexpr UINT_PTR TIMER_MINUTE = 1;

LRESULT CALLBACK ClockProc(HWND h, UINT m, WPARAM w, LPARAM l);

bool CreateClockWindow() {
    W.desktop = FindWindowW(L"Progman", nullptr);
    if (!W.desktop) return false;
    WNDCLASSW wc{};
    wc.lpfnWndProc = ClockProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = CLOCK_CLASS;
    RegisterClassW(&wc);
    W.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CLOCK_CLASS,
                             L"VideoBG clock", WS_POPUP, 0, 0, 1, 1, W.desktop, nullptr, wc.hInstance, nullptr);
    W.drawn.clear();
    return W.hwnd != nullptr;
}

void DestroyClockWindow() {
    if (!W.hwnd) return;
    W.closing = true;
    DestroyWindow(W.hwnd);
    W.closing = false;
    W.hwnd = nullptr;
    W.drawn.clear();
}

// Where the look puts the clock: its centre as a share of the main screen.
POINT ClockOrigin() {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& m = mi.rcMonitor;
    return POINT{(LONG)lroundf(m.left + W.look.x * (m.right - m.left) - W.w / 2.f),
                 (LONG)lroundf(m.top + W.look.y * (m.bottom - m.top) - W.h / 2.f)};
}

void PlaceClock() {
    POINT p = ClockOrigin();
    SetWindowPos(W.hwnd, HWND_BOTTOM, p.x, p.y, W.w, W.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);  // bottom = just above the desktop
}

void DrawClock() {
    if (!W.hwnd) return;
    // Explorer restarted: the desktop is a new window now.
    if (!IsWindow(W.desktop) || GetWindow(W.hwnd, GW_OWNER) != W.desktop) {
        DestroyClockWindow();
        if (!CreateClockWindow()) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    float k = Clock_PixelScale();
    wchar_t when[64];
    swprintf(when, 64, L"|%d|%.3f|%04d%02d%02d%02d%02d", W.h24, k, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    std::wstring key = Clock_StyleKey(W.look) + when;
    if (W.drawn != key) {
        ClockImage img;
        if (Clock_Paint(W.look, W.h24, k, t, &img)) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = img.w;
            bi.bmiHeader.biHeight = -img.h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            void* bits = nullptr;
            HDC dc = CreateCompatibleDC(nullptr);
            HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bmp) {
                memcpy(bits, img.px.data(), img.px.size() * 4);
                HGDIOBJ old = SelectObject(dc, bmp);
                W.w = img.w;
                W.h = img.h;
                POINT dst = ClockOrigin(), zero{0, 0};
                SIZE sz{img.w, img.h};
                BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                if (UpdateLayeredWindow(W.hwnd, nullptr, &dst, &sz, dc, &zero, 0, &bf, ULW_ALPHA)) W.drawn = key;
                else Log(L"clock: couldn't update the window (error %lu)", GetLastError());
                SelectObject(dc, old);
                DeleteObject(bmp);
            }
            DeleteDC(dc);
        }
        if (!UI_IsOpen()) Clock_ReleasePainter();  // nothing stays loaded between minutes
    }
    PlaceClock();
    // Next redraw right after the minute changes.
    GetLocalTime(&t);
    SetTimer(W.hwnd, TIMER_MINUTE, (UINT)((60 - t.wSecond) * 1000 - t.wMilliseconds + 50), nullptr);
}

LRESULT CALLBACK ClockProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_TIMER:
            if (w == TIMER_MINUTE) DrawClock();
            return 0;
        case WM_DESTROY:
            if (!W.closing && h == W.hwnd) {  // destroyed with the desktop (Explorer restarting)
                W.hwnd = nullptr;
                W.drawn.clear();
                if (W.host) PostMessageW(W.host, WM_VBG_CLOCK_LOST, 0, 0);
            }
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Box blur of a w x h plane, radius r, in place (three passes of this look like a Gaussian).
void BoxBlur(std::vector<float>& a, int w, int h, int r) {
    std::vector<float> tmp(std::max(w, h));
    const float inv = 1.f / (2 * r + 1);
    auto pass = [&](float* p, int n, int stride) {  // window [i - r, i + r], zero outside
        float s = 0;
        for (int i = 0; i < r && i < n; i++) s += p[(size_t)i * stride];
        for (int i = 0; i < n; i++) {
            if (i + r < n) s += p[(size_t)(i + r) * stride];
            tmp[i] = s * inv;
            if (i - r >= 0) s -= p[(size_t)(i - r) * stride];
        }
        for (int i = 0; i < n; i++) p[(size_t)i * stride] = tmp[i];
    };
    for (int y = 0; y < h; y++) pass(&a[(size_t)y * w], w, 1);
    for (int x = 0; x < w; x++) pass(&a[x], h, w);
}

// Puts a soft glow of `color` behind the text (premultiplied BGRA) and applies the opacity.
void AddGlow(uint32_t* px, int w, int h, float radius, float strength, D2D1_COLOR_F color, float opacity) {
    std::vector<float> a;
    if (strength > 0 && radius >= 1) {
        a.resize((size_t)w * h);
        for (size_t i = 0; i < a.size(); i++) a[i] = (px[i] >> 24) / 255.f;
        int r = std::max(1, (int)lroundf(radius / 3));
        for (int i = 0; i < 3; i++) BoxBlur(a, w, h, r);
    }
    // Thin strokes blur to faint values; lift them so a strong glow reads clearly at any size.
    const float gain = strength * (3 + radius / 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t v = px[i];
        float ta = (v >> 24) / 255.f, r = (v >> 16 & 0xFF) / 255.f, g = (v >> 8 & 0xFF) / 255.f, b = (v & 0xFF) / 255.f;
        if (!a.empty()) {
            float ga = (1 - expf(-a[i] * gain)) * (1 - ta);
            r += color.r * ga;
            g += color.g * ga;
            b += color.b * ga;
            ta += ga;
        }
        auto q = [&](float c) { return (uint32_t)std::clamp(lroundf(c * opacity * 255), 0L, 255L); };
        px[i] = q(ta) << 24 | q(r) << 16 | q(g) << 8 | q(b);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------

std::wstring Clock_StyleKey(const ClockLook& l) {
    wchar_t b[160];
    swprintf(b, 160, L"%ls|%.3f|%.3f|%.3f|%.2f|%ls", l.color.c_str(), l.size, l.opacity, l.glow, l.glowSize, l.glowColor.c_str());
    return b;
}

bool ParseColor(const std::wstring& s, BYTE* r, BYTE* g, BYTE* b) {
    unsigned v[3] = {0, 0, 0};
    std::wstring t;
    for (wchar_t c : s)
        if (!iswspace(c)) t += (wchar_t)towlower(c);
    if (t.compare(0, 4, L"rgb(") == 0 && t.back() == L')') t = t.substr(4, t.size() - 5);
    if (!t.empty() && t[0] == L'#') t.erase(0, 1);
    else if (t.compare(0, 2, L"0x") == 0) t.erase(0, 2);
    wchar_t end = 0;
    if (swscanf(t.c_str(), L"%u,%u,%u%lc", &v[0], &v[1], &v[2], &end) != 3) {
        if (!std::all_of(t.begin(), t.end(), iswxdigit)) return false;
        if (t.size() == 3) t = {t[0], t[0], t[1], t[1], t[2], t[2]};  // #RGB
        if (t.size() != 6 && t.size() != 8) return false;
        unsigned long x = wcstoul(t.substr(0, 6).c_str(), nullptr, 16);
        v[0] = (x >> 16) & 0xFF;
        v[1] = (x >> 8) & 0xFF;
        v[2] = x & 0xFF;
    }
    *r = (BYTE)std::min(v[0], 255u);
    *g = (BYTE)std::min(v[1], 255u);
    *b = (BYTE)std::min(v[2], 255u);
    return true;
}

float Clock_PixelScale() {
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto fn = [] {
        HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return shcore ? (GetDpiForMonitorFn)(void*)GetProcAddress(shcore, "GetDpiForMonitor") : nullptr;
    }();
    if (fn) fn(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), 0, &dpiX, &dpiY);
    return dpiX / 96.f;
}

void Clock_ReleasePainter() {
    if (P.f && P.loader) P.f->UnregisterFontFileLoader(P.loader);
    Rel(P.fonts);
    Rel(P.loader);
    Rel(P.f);
    Rel(P.d2f);
    if (P.dw) FreeLibrary(P.dw);
    if (P.d2) FreeLibrary(P.d2);
    bool failed = P.failed;
    P = Painter{};
    P.failed = failed;
}

// Layout as in Mond's Clock.ini (Scale = the look's size): three lines centred on one axis -
//   day   Audiowide 40 pt, letter spacing 10 before and after each letter, at y = 0
//   date  Quicksand 14 pt, "%d  %B,  %Y."                                 at y = 75 x Scale
//   time  Quicksand 14 pt, "- %#I:%M %p -"                                at y = 120 x Scale
// Rainmeter treats font sizes and letter spacing as points (x 4/3 for DIPs); everything is then
// scaled to real pixels by k. Checked against a capture of the Rainmeter skin: same rows, widths
// within a few pixels.
bool Clock_Paint(const ClockLook& look, bool h24, float k, const SYSTEMTIME& t, ClockImage* out) {
    const float size = look.size;
    if (!OpenPainter()) return false;
    wchar_t date[64], time[32];
    swprintf(date, 64, L"%02d  %ls,  %d.", t.wDay, kMonths[(t.wMonth + 11) % 12], t.wYear);
    if (h24) swprintf(time, 32, L"- %02d:%02d -", t.wHour, t.wMinute);
    else swprintf(time, 32, L"- %d:%02d %ls -", (t.wHour + 11) % 12 + 1, t.wMinute, t.wHour < 12 ? L"AM" : L"PM");
    struct Line {
        const wchar_t* text;
        const wchar_t* font;
        float pt, y, spacing;
        IDWriteTextLayout* layout;
        DWRITE_TEXT_METRICS m;
    } lines[3] = {{kDays[t.wDayOfWeek % 7], L"Audiowide", 40, 0, 10, nullptr, {}},
                  {date, L"Quicksand", 14, 75, 0, nullptr, {}},
                  {time, L"Quicksand", 14, 120, 0, nullptr, {}}};
    HRESULT hr = S_OK;
    float maxW = 0;
    for (auto& l : lines) {
        IDWriteTextFormat* fmt = nullptr;
        UINT32 len = (UINT32)wcslen(l.text);
        hr = P.f->CreateTextFormat(l.font, P.fonts, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   l.pt * size * 4 / 3 * k, L"en-us", &fmt);
        if (SUCCEEDED(hr)) {
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            hr = P.f->CreateTextLayout(l.text, len, fmt, 8192, 8192, &l.layout);
        }
        Rel(fmt);
        if (SUCCEEDED(hr) && l.spacing > 0) {
            IDWriteTextLayout1* l1 = nullptr;
            if (SUCCEEDED(l.layout->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&l1))) {
                l1->SetCharacterSpacing(l.spacing * 4 / 3 * k, l.spacing * 4 / 3 * k, 0, DWRITE_TEXT_RANGE{0, len});
                l1->Release();
            }
        }
        if (SUCCEEDED(hr)) hr = l.layout->GetMetrics(&l.m);
        if (FAILED(hr)) break;
        maxW = std::max(maxW, l.m.widthIncludingTrailingWhitespace);
    }
    const float glowR = look.glow > 0 ? look.glowSize * size * k : 0;
    const int pad = (int)ceilf(glowR);
    const float margin = ceilf(4 * k) + pad;
    const int w = SUCCEEDED(hr) ? (int)ceilf(maxW + 2 * margin) : 0;
    const int h = SUCCEEDED(hr) ? (int)ceilf(lines[2].y * size * k + lines[2].m.height + 2 * margin) : 0;
    bool ok = false;
    if (w > 0 && h > 0 && w < 16384 && h < 16384) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ID2D1DCRenderTarget* rt = nullptr;
        ID2D1SolidColorBrush* brush = nullptr;
        if (bmp) {
            HGDIOBJ old = SelectObject(dc, bmp);
            D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            RECT rc{0, 0, w, h};
            BYTE r = 255, g = 255, b = 255;
            ParseColor(look.color, &r, &g, &b);
            D2D1_COLOR_F c = D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f);
            if (SUCCEEDED(P.d2f->CreateDCRenderTarget(&rp, &rt)) && SUCCEEDED(rt->BindDC(dc, &rc)) &&
                SUCCEEDED(rt->CreateSolidColorBrush(&c, nullptr, &brush))) {
                rt->BeginDraw();
                rt->Clear(D2D1::ColorF(0, 0, 0, 0));
                rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                for (auto& l : lines)
                    rt->DrawTextLayout(D2D1::Point2F((w - l.m.widthIncludingTrailingWhitespace) / 2, margin + l.y * size * k), l.layout,
                                       brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
                ok = SUCCEEDED(rt->EndDraw());
            }
            if (ok) {
                GdiFlush();
                out->px.assign((uint32_t*)bits, (uint32_t*)bits + (size_t)w * h);
                out->w = w;
                out->h = h;
                out->pad = pad;
                BYTE gr = r, gg = g, gb = b;
                if (!look.glowColor.empty()) ParseColor(look.glowColor, &gr, &gg, &gb);
                if (look.glow > 0 || look.opacity < 1)
                    AddGlow(out->px.data(), w, h, glowR, look.glow, D2D1::ColorF(gr / 255.f, gg / 255.f, gb / 255.f),
                            std::clamp(look.opacity, 0.f, 1.f));
            }
            SelectObject(dc, old);
            DeleteObject(bmp);
        }
        Rel(brush);
        Rel(rt);
        DeleteDC(dc);
    }
    for (auto& l : lines) Rel(l.layout);
    return ok;
}

void Clock_Show(HWND host, const ClockLook& look, bool h24) {
    W.host = host;
    W.look = look;
    W.h24 = h24;
    if (!W.hwnd && !CreateClockWindow()) {
        Log(L"clock: no desktop to put the clock on yet");
        return;
    }
    DrawClock();
}

void Clock_Hide() {
    if (W.hwnd) KillTimer(W.hwnd, TIMER_MINUTE);
    DestroyClockWindow();
    Clock_ReleasePainter();
}

void Clock_Refresh() {
    if (!W.hwnd) return;
    W.drawn.clear();
    DrawClock();
}
