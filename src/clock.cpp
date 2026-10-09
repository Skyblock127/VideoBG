// Desktop clock: day name, date and time in the design of the "Mond" Rainmeter clock (its sizes and
// spacing; see Credits in README.md), drawn by VideoBG itself: one small see-through, click-through
// window on the desktop that redraws once a minute. Each look picks the clock's font. DirectWrite and
// Direct2D, and only the font being drawn, are loaded only while drawing.
#include "common.h"
#include "resource.h"
#include <d2d1.h>
#include <dwrite_3.h>
#include <shlobj.h>
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

// ---- Language -------------------------------------------------------------------------------
// The clock's words in a language: Windows' own data for it (days, months, AM/PM, digits), in
// capitals like Mond's English. "" is Mond's own English.
struct Words {
    std::wstring lang;
    bool native = false;
    std::wstring days[7], months[12], monthsAfterDay[12], shortMonths[12], am, pm, digits;
    bool amFirst = false;  // AM/PM goes before the time (Chinese, Korean, ...)
    bool joined = false;   // a script whose letters join up (Devanagari, Arabic, Thai...): no letter spacing
    bool rtl = false;      // written right to left (Arabic, Hebrew, Urdu...)
};

std::wstring LocaleText(const std::wstring& lang, LCTYPE type) {
    wchar_t b[128] = {};
    return GetLocaleInfoEx(lang.c_str(), type, b, 128) > 0 ? b : L"";
}

// The Gregorian calendar's names in a language (its own calendar may be another, like Arabic's).
std::wstring GregorianText(const std::wstring& lang, CALTYPE type) {
    wchar_t b[128] = {};
    return GetCalendarInfoEx(lang.c_str(), CAL_GREGORIAN, nullptr, type, b, 128, nullptr) > 0 ? b : L"";
}

std::wstring Upper(const std::wstring& lang, const std::wstring& s) {
    if (s.empty()) return s;
    std::wstring out(s.size() * 3 + 8, L'\0');
    int n = LCMapStringEx(lang.empty() ? L"en-US" : lang.c_str(), LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, s.c_str(), (int)s.size(),
                          out.data(), (int)out.size(), nullptr, nullptr, 0);
    if (n <= 0) return s;
    out.resize(n);
    return out;
}

// A month as a date writes it after the day number: some languages change the word there
// (Russian, for one, uses another form of the word after a day number).
std::wstring MonthAfterDay(const std::wstring& lang, int month) {
    SYSTEMTIME st{};
    st.wYear = 2026;
    st.wMonth = (WORD)month;
    st.wDay = 15;
    wchar_t b[128] = {};
    if (GetDateFormatEx(lang.c_str(), 0, &st, L"d MMMM", b, 128, nullptr) <= 0) return L"";
    std::wstring s = b;
    size_t at = s.find(L"15");  // the day number goes, with the spaces and dots around it
    if (at == std::wstring::npos) return L"";
    s.erase(at, 2);
    const wchar_t* trim = L" .,";
    size_t first = s.find_first_not_of(trim), last = s.find_last_not_of(trim);
    return first == std::wstring::npos ? L"" : s.substr(first, last - first + 1);
}

bool JoinedScript(const std::wstring& s) {
    for (wchar_t c : s)
        if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0x0900 && c <= 0x0FFF) || (c >= 0x1000 && c <= 0x109F) ||
            (c >= 0x1780 && c <= 0x18AF) || (c >= 0xA8E0 && c <= 0xA8FF) || (c >= 0xFB1D && c <= 0xFEFF))
            return true;
    return false;
}

const Words& WordsFor(const std::wstring& lang, bool native) {
    static Words w;
    static bool made = false;
    if (made && w.lang == lang && w.native == native) return w;
    w = Words{};
    w.lang = lang;
    w.native = native;
    made = true;
    for (int i = 0; i < 7; i++) w.days[i] = kDays[i];
    for (int i = 0; i < 12; i++) {
        w.months[i] = w.monthsAfterDay[i] = kMonths[i];
        w.shortMonths[i] = std::wstring(kMonths[i]).substr(0, 3);
    }
    w.am = L"AM";
    w.pm = L"PM";
    if (lang.empty()) return w;
    for (int i = 0; i < 7; i++) {  // the first day name is Monday's
        std::wstring d = GregorianText(lang, i == 0 ? CAL_SDAYNAME7 : CAL_SDAYNAME1 + i - 1);
        if (!d.empty()) w.days[i] = Upper(lang, d);
    }
    // The month after a day number comes from Windows' date formatting, which uses the language's
    // own calendar: only when that's the Gregorian one.
    const bool gregorian = LocaleText(lang, LOCALE_ICALENDARTYPE) == L"1";
    for (int i = 0; i < 12; i++) {
        std::wstring m = GregorianText(lang, CAL_SMONTHNAME1 + i), sm = GregorianText(lang, CAL_SABBREVMONTHNAME1 + i);
        std::wstring after = gregorian ? MonthAfterDay(lang, i + 1) : L"";
        if (!m.empty()) w.months[i] = Upper(lang, m);
        w.monthsAfterDay[i] = after.empty() ? w.months[i] : Upper(lang, after);
        if (!sm.empty()) w.shortMonths[i] = Upper(lang, sm);
    }
    w.am = Upper(lang, LocaleText(lang, LOCALE_S1159));
    w.pm = Upper(lang, LocaleText(lang, LOCALE_S2359));
    w.amFirst = LocaleText(lang, LOCALE_ITIMEMARKPOSN) == L"1";
    if (native) w.digits = Clock_NativeDigits(lang);
    w.joined = JoinedScript(w.days[5]);
    w.rtl = LocaleText(lang, LOCALE_IREADINGLAYOUT) == L"1";
    return w;
}

std::wstring WithDigits(const Words& w, std::wstring s) {
    if (w.digits.size() == 10)
        for (wchar_t& c : s)
            if (c >= L'0' && c <= L'9') c = w.digits[c - L'0'];
    return s;
}

// ---- Fonts ----------------------------------------------------------------------------------
// The whole clock is in one font. Each has scales that make every font about as wide as Audiowide
// at the same size: one for the day, one for the date and time. Fonts VideoBG can't include come
// from the user: installed on the PC, or added through the font picker into its fonts folder, as do
// any other fonts the user adds (their scales are measured).
struct FontDef {
    const wchar_t* key;
    const wchar_t* getUrl;  // where to get it, for a font VideoBG can't include
    int res;                // RCDATA id; 0 = not included
    float day, small;       // scales of the day, and of the date and time
};
const FontDef kFonts[] = {
    {L"Audiowide", nullptr, IDR_FONT_AUDIOWIDE, 1.00f, 0.90f},
    {L"Michroma", nullptr, IDR_FONT_MICHROMA, 0.83f, 0.75f},
    {L"Orbitron", nullptr, IDR_FONT_ORBITRON, 0.98f, 0.88f},
    {L"Syncopate", nullptr, IDR_FONT_SYNCOPATE, 0.96f, 0.86f},
    {L"Anurati", L"https://befonts.com/downfile/3ee31d3025c55bd25cff640e2675cbf5.30052", 0, 0.97f, 0.87f},  // its free version
};
const float kDayWidth = 635;  // Audiowide's WEDNESDAY at size 1, in DIPs: what a user's font is scaled to

const FontDef* FindFont(const std::wstring& key) {
    for (const FontDef& f : kFonts)
        if (!_wcsicmp(f.key, key.c_str())) return &f;
    return nullptr;
}

std::wstring FontsDir() { return LocalDataDir() + L"\\Fonts"; }

bool IsFontName(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    return dot != std::wstring::npos && (!_wcsicmp(name.c_str() + dot, L".otf") || !_wcsicmp(name.c_str() + dot, L".ttf"));
}

// The first font file in dir matching pattern.
std::wstring FindFontFile(const std::wstring& dir, const std::wstring& pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsFontName(fd.cFileName)) found = dir + L"\\" + fd.cFileName;
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// The file of a font that isn't in the exe: VideoBG's copy, else (for one VideoBG knows, like
// Anurati) one installed on the PC, for this user or for everyone, found by its file name.
std::wstring FontFile(const std::wstring& key) {
    const FontDef* f = FindFont(key);
    if (f && f->res) return L"";
    std::wstring file = FindFontFile(FontsDir(), key + L".*");
    wchar_t buf[MAX_PATH];
    if (f && file.empty() && ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\Windows\\Fonts", buf, MAX_PATH))
        file = FindFontFile(buf, L"*" + key + L"*");
    if (f && file.empty() && GetWindowsDirectoryW(buf, MAX_PATH)) file = FindFontFile(std::wstring(buf) + L"\\Fonts", L"*" + key + L"*");
    return file;
}

// The fonts embedded in the exe stay in memory for good, so DirectWrite reads them where they are
// instead of copying them: this owner keeps nothing alive.
struct StaticOwner : IUnknown {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        *p = IsEqualIID(riid, IID_IUnknown) ? this : nullptr;
        return *p ? S_OK : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
} g_exeMemory;

struct Painter {
    HMODULE dw = nullptr, d2 = nullptr;
    IDWriteFactory5* f = nullptr;
    IDWriteInMemoryFontFileLoader* loader = nullptr;
    ID2D1Factory* d2f = nullptr;
    IDWriteFontCollection1* fonts = nullptr;  // just the font being drawn (and Quicksand for digits it lacks)
    std::wstring fontKey;                     // which font that is
    std::wstring family, smallFamily;         // its family, and the one for the date and time
    float dayCap = 0.7f, smallCap = 0.7f;     // their capital letters' height, per em
    float dayScale = 1, smallScale = 1;
    bool failed = false;
} P;

bool AddResFont(IDWriteFontSetBuilder1* b, int id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    if (!data) return false;
    IDWriteFontFile* file = nullptr;
    HRESULT hr = P.loader->CreateInMemoryFontFileReference(P.f, data, SizeofResource(nullptr, r), &g_exeMemory, &file);
    if (SUCCEEDED(hr)) hr = b->AddFontFile(file);
    Rel(file);
    return SUCCEEDED(hr);
}

bool AddFileFont(IDWriteFontSetBuilder1* b, const std::wstring& path) {
    IDWriteFontFile* file = nullptr;
    HRESULT hr = P.f->CreateFontFileReference(path.c_str(), nullptr, &file);
    if (SUCCEEDED(hr)) hr = b->AddFontFile(file);
    Rel(file);
    return SUCCEEDED(hr);
}

// Factories only: fonts are loaded by UseFont, one at a time.
bool OpenPainter() {
    if (P.f) return true;
    if (P.failed) return false;
    P.dw = LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    P.d2 = LoadLibraryExW(L"d2d1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using DWCreate = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
    using D2DCreate = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
    auto dwCreate = P.dw ? (DWCreate)(void*)GetProcAddress(P.dw, "DWriteCreateFactory") : nullptr;
    auto d2Create = P.d2 ? (D2DCreate)(void*)GetProcAddress(P.d2, "D2D1CreateFactory") : nullptr;
    HRESULT hr = dwCreate && d2Create ? S_OK : E_NOINTERFACE;
    // Isolated factory (needs Windows 10 1703+): the clock's fonts never reach other apps.
    if (SUCCEEDED(hr)) hr = dwCreate(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory5), (IUnknown**)&P.f);
    D2D1_FACTORY_OPTIONS opt{D2D1_DEBUG_LEVEL_NONE};
    if (SUCCEEDED(hr)) hr = d2Create(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opt, (void**)&P.d2f);
    if (SUCCEEDED(hr)) hr = P.f->CreateInMemoryFontFileLoader(&P.loader);
    if (SUCCEEDED(hr)) hr = P.f->RegisterFontFileLoader(P.loader);
    if (FAILED(hr)) {
        Log(L"clock: can't set up text drawing (0x%08lx)", (unsigned long)hr);
        Clock_ReleasePainter();
        P.failed = true;
        return false;
    }
    return true;
}

// A family of a collection: its name, its capitals' height per em, and whether it has digits.
bool FamilyInfo(IDWriteFontCollection1* c, UINT32 index, std::wstring* name, float* cap, bool* digits) {
    IDWriteFontFamily1* fam = nullptr;
    IDWriteLocalizedStrings* names = nullptr;
    IDWriteFont3* font = nullptr;
    bool ok = SUCCEEDED(c->GetFontFamily(index, &fam)) && SUCCEEDED(fam->GetFamilyNames(&names)) && SUCCEEDED(fam->GetFont(0, &font));
    UINT32 len = 0;
    if (ok && SUCCEEDED(names->GetStringLength(0, &len))) {
        name->resize(len + 1);
        ok = SUCCEEDED(names->GetString(0, name->data(), len + 1));
        name->resize(len);
    }
    if (ok) {
        DWRITE_FONT_METRICS1 m{};
        font->GetMetrics(&m);
        *cap = m.designUnitsPerEm && m.capHeight ? (float)m.capHeight / m.designUnitsPerEm : 0.7f;
        *digits = font->HasCharacter(L'0') && font->HasCharacter(L':');
    }
    Rel(font);
    Rel(names);
    Rel(fam);
    return ok;
}

bool BuildCollection(int res, const std::wstring& file, bool withQuicksand) {
    IDWriteFontSetBuilder1* b = nullptr;
    IDWriteFontSet* set = nullptr;
    HRESULT hr = P.f->CreateFontSetBuilder(&b);
    if (SUCCEEDED(hr) && !(res ? AddResFont(b, res) : AddFileFont(b, file))) hr = E_FAIL;
    if (SUCCEEDED(hr) && withQuicksand && !AddResFont(b, IDR_FONT_QUICKSAND)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = b->CreateFontSet(&set);
    if (SUCCEEDED(hr)) hr = P.f->CreateFontCollectionFromFontSet(set, &P.fonts);
    Rel(set);
    Rel(b);
    return SUCCEEDED(hr);
}

// A font the user added: the scale that makes its day as wide as Audiowide's.
float MeasuredScale() {
    const float px = 40 * 4 / 3.f, gap = 10 * 4 / 3.f;
    IDWriteTextFormat* fmt = nullptr;
    IDWriteTextLayout* layout = nullptr;
    float scale = 1;
    if (SUCCEEDED(P.f->CreateTextFormat(P.family.c_str(), P.fonts, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, px, L"en-us", &fmt)) &&
        SUCCEEDED(P.f->CreateTextLayout(L"WEDNESDAY", 9, fmt, 8192, 8192, &layout))) {
        IDWriteTextLayout1* l1 = nullptr;
        if (SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&l1))) {
            l1->SetCharacterSpacing(gap, gap, 0, DWRITE_TEXT_RANGE{0, 9});
            l1->Release();
        }
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(layout->GetMetrics(&m)) && m.widthIncludingTrailingWhitespace > 1)
            scale = std::clamp(kDayWidth / m.widthIncludingTrailingWhitespace, 0.4f, 2.5f);
    }
    Rel(layout);
    Rel(fmt);
    return scale;
}

// Loads the font to draw with, letting go of the one before.
bool UseFont(const std::wstring& key) {
    if (P.fonts && P.fontKey == key) return true;
    Rel(P.fonts);
    P.fontKey.clear();
    const FontDef* f = FindFont(key);
    const int res = f ? f->res : 0;
    const std::wstring file = res ? L"" : FontFile(key);
    if (!res && file.empty()) return false;
    bool digits = false, unused = false;
    if (!BuildCollection(res, file, false) || !FamilyInfo(P.fonts, 0, &P.family, &P.dayCap, &digits)) {
        Rel(P.fonts);
        return false;
    }
    P.dayScale = f ? f->day : MeasuredScale();
    P.smallScale = f ? f->small : P.dayScale * 0.9f;
    P.smallFamily = P.family;
    P.smallCap = P.dayCap;
    if (!digits) {  // like Anurati's free version: the date and time in Quicksand instead, as in Mond
        Rel(P.fonts);
        UINT32 qi = 0;
        BOOL found = FALSE;
        if (!BuildCollection(res, file, true) || FAILED(P.fonts->FindFamilyName(L"Quicksand", &qi, &found)) || !found ||
            !FamilyInfo(P.fonts, qi, &P.smallFamily, &P.smallCap, &unused)) {
            Rel(P.fonts);
            return false;
        }
        P.smallScale = 1;
    }
    P.fontKey = key;
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
    swprintf(when, 64, L"|%.3f|%04d%02d%02d%02d%02d", k, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    std::wstring key = Clock_StyleKey(W.look) + when;
    if (W.drawn != key) {
        ClockImage img;
        if (Clock_Paint(W.look, k, t, &img)) {
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
    swprintf(b, 160, L"%ls|%.3f|%.3f|%.3f|%.2f|%ls|%d|%ls|%d|%ls|%d", l.color.c_str(), l.size, l.opacity, l.glow, l.glowSize,
             l.glowColor.c_str(), l.h24, l.font.c_str(), l.date, l.lang.c_str(), l.nativeDigits);
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
    Rel(P.fonts);
    if (P.f && P.loader) P.f->UnregisterFontFileLoader(P.loader);
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
//   day   40 pt, letter spacing 10 before and after each letter
//   date  14 pt, "%d  %B,  %Y."
//   time  14 pt, "- %#I:%M %p -"
// each times the font's scale. Rainmeter treats font sizes and letter spacing as points (x 4/3 for
// DIPs); everything is then scaled to real pixels by k. The lines are placed by their capitals, with
// Mond's gaps measured from the skin: the date's 25.7 DIPs below the day's baseline, the time's 31.5
// below the date's (x Scale), so every font keeps the same rhythm.
bool Clock_Paint(const ClockLook& look, float k, const SYSTEMTIME& t, ClockImage* out) {
    const float size = look.size;
    if (!OpenPainter()) return false;
    if (!UseFont(look.font) && !UseFont(kFonts[0].key)) return false;  // a font that's gone: the default
    const Words& words = WordsFor(look.lang, look.nativeDigits);
    const std::wstring day = words.days[t.wDayOfWeek % 7], date = Clock_DateText(look, look.date, t);
    wchar_t clock[32];
    std::wstring time;
    if (look.h24) {
        swprintf(clock, 32, L"%02d:%02d", t.wHour, t.wMinute);
        time = clock;
    } else {
        swprintf(clock, 32, L"%d:%02d", (t.wHour + 11) % 12 + 1, t.wMinute);
        const std::wstring& mark = t.wHour < 12 ? words.am : words.pm;
        time = mark.empty() ? std::wstring(clock) : words.amFirst ? mark + L" " + clock : clock + std::wstring(L" ") + mark;
    }
    time = WithDigits(words, L"- " + time + L" -");
    struct Line {
        const wchar_t* text;
        const wchar_t* font;
        float pt, spacing, cap;
        IDWriteTextLayout* layout;
        DWRITE_TEXT_METRICS m;
        float top;  // where the layout's top goes
    } lines[3] = {{day.c_str(), P.family.c_str(), 40 * P.dayScale, words.joined ? 0 : 10 * P.dayScale, P.dayCap, nullptr, {}, 0},
                  {date.c_str(), P.smallFamily.c_str(), 14 * P.smallScale, 0, P.smallCap, nullptr, {}, 0},
                  {time.c_str(), P.smallFamily.c_str(), 14 * P.smallScale, 0, P.smallCap, nullptr, {}, 0}};
    int n = 3;
    if (date.empty()) {  // no date line: the time comes right under the day
        lines[1] = lines[2];
        n = 2;
    }
    const float gaps[3] = {0, 25.7f, 31.5f};
    HRESULT hr = S_OK;
    float maxW = 0, capTop = 0, minTop = 0, bottom = 0;
    for (int i = 0; i < n; i++) {
        Line& l = lines[i];
        IDWriteTextFormat* fmt = nullptr;
        UINT32 len = (UINT32)wcslen(l.text);
        const float px = l.pt * size * 4 / 3 * k;
        hr = P.f->CreateTextFormat(l.font, P.fonts, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   px, L"en-us", &fmt);
        if (SUCCEEDED(hr)) {
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (words.rtl) fmt->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
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
        // As wide as the text, so it starts at the layout's left whichever way it reads.
        if (SUCCEEDED(hr)) hr = l.layout->SetMaxWidth(l.m.widthIncludingTrailingWhitespace);
        if (SUCCEEDED(hr)) hr = l.layout->GetMetrics(&l.m);
        DWRITE_LINE_METRICS lm{};
        UINT32 count = 0;
        if (SUCCEEDED(hr)) hr = l.layout->GetLineMetrics(&lm, 1, &count);
        if (FAILED(hr)) break;
        maxW = std::max(maxW, l.m.widthIncludingTrailingWhitespace);
        // Capitals start the gap below the line above (the day's at 0).
        if (i > 0) capTop += lines[i - 1].cap * lines[i - 1].pt * size * 4 / 3 * k + gaps[i] * size * k;
        l.top = capTop - (lm.baseline - l.cap * px);
        minTop = i ? std::min(minTop, l.top) : l.top;
        bottom = i ? std::max(bottom, l.top + l.m.height) : l.top + l.m.height;
    }
    const float glowR = look.glow > 0 ? look.glowSize * size * k : 0;
    const int pad = (int)ceilf(glowR);
    const float margin = ceilf(4 * k) + pad;
    const int w = SUCCEEDED(hr) ? (int)ceilf(maxW + 2 * margin) : 0;
    const int h = SUCCEEDED(hr) ? (int)ceilf(bottom - minTop + 2 * margin) : 0;
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
                for (int i = 0; i < n; i++) {
                    const Line& l = lines[i];
                    rt->DrawTextLayout(D2D1::Point2F((w - l.m.widthIncludingTrailingWhitespace) / 2, margin + l.top - minTop), l.layout,
                                       brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
                }
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

void Clock_Show(HWND host, const ClockLook& look) {
    W.host = host;
    W.look = look;
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

// ---------------------------------------------------------------------------------------
// Clock fonts

// Date styles, by number (stored in a look): the clock's middle line.
const int kDateStyles = 15;

int Clock_DateStyleCount() { return kDateStyles; }

std::wstring Clock_DateText(const ClockLook& look, int style, const SYSTEMTIME& t) {
    const Words& w = WordsFor(look.lang, look.nativeDigits);
    const int d = t.wDay, m = t.wMonth, y = t.wYear, mi = (m + 11) % 12;
    const wchar_t *month = w.months[mi].c_str(), *ofDay = w.monthsAfterDay[mi].c_str(), *shortMonth = w.shortMonths[mi].c_str();
    wchar_t b[160];
    switch (style) {
        case 1: swprintf(b, 160, L"%ls %d, %d", month, d, y); break;               // OCTOBER 9, 2026
        case 2: swprintf(b, 160, L"%d %ls %d", d, ofDay, y); break;                // 9 OCTOBER 2026
        case 3: swprintf(b, 160, L"%02d %ls %d", d, shortMonth, y); break;         // 09 OCT 2026
        case 4: swprintf(b, 160, L"%ls %02d, %d", shortMonth, d, y); break;        // OCT 09, 2026
        case 5: swprintf(b, 160, L"%02d . %02d . %d", d, m, y); break;             // 09 . 10 . 2026
        case 6: swprintf(b, 160, L"%02d . %02d . %d", m, d, y); break;             // 10 . 09 . 2026
        case 7: swprintf(b, 160, L"%02d / %02d / %02d", d, m, y % 100); break;     // 09 / 10 / 26
        case 8: swprintf(b, 160, L"%02d / %02d / %02d", m, d, y % 100); break;     // 10 / 09 / 26
        case 9: swprintf(b, 160, L"%d - %02d - %02d", y, m, d); break;             // 2026 - 10 - 09
        case 10: swprintf(b, 160, L"%02d  %ls", d, ofDay); break;                  // 09  OCTOBER
        case 11: swprintf(b, 160, L"%ls  %02d", month, d); break;                  // OCTOBER  09
        case 12: swprintf(b, 160, L"%ls  %d", month, y); break;                    // OCTOBER  2026
        case 13: swprintf(b, 160, L"%02d . %02d", d, m); break;                    // 09 . 10
        case 14: return L"";                                                        // no date
        default: swprintf(b, 160, L"%02d  %ls,  %d.", d, ofDay, y);                // Mond: 09  OCTOBER,  2026.
    }
    return WithDigits(w, b);
}

namespace {

BOOL CALLBACK AddLanguage(LPWSTR name, DWORD, LPARAM list) {
    const std::wstring code = name;
    // Languages (not regions): skip the invariant one, English (that's Mond's own) and test locales.
    if (code.empty() || code == L"en" || code.rfind(L"qps", 0) == 0 || code.rfind(L"x-", 0) == 0) return TRUE;
    if (LocaleText(code, LOCALE_SDAYNAME1).empty()) return TRUE;
    ClockLanguage l{code, LocaleText(code, LOCALE_SENGLISHDISPLAYNAME), LocaleText(code, LOCALE_SNATIVEDISPLAYNAME)};
    if (!l.english.empty()) ((std::vector<ClockLanguage>*)list)->push_back(l);
    return TRUE;
}

}  // namespace

std::vector<ClockLanguage> Clock_Languages() {
    std::vector<ClockLanguage> list;
    EnumSystemLocalesEx(AddLanguage, LOCALE_NEUTRALDATA, (LPARAM)&list, nullptr);
    std::sort(list.begin(), list.end(), [](const ClockLanguage& a, const ClockLanguage& b) { return _wcsicmp(a.english.c_str(), b.english.c_str()) < 0; });
    list.insert(list.begin(), ClockLanguage{L"", L"English", L"English"});
    return list;
}

std::wstring Clock_LanguageName(const std::wstring& code) {
    if (code.empty()) return L"English";
    std::wstring name = LocaleText(code, LOCALE_SENGLISHDISPLAYNAME);
    return name.empty() ? code : name;
}

std::wstring Clock_NativeDigits(const std::wstring& code) {
    if (code.empty()) return L"";
    std::wstring d = LocaleText(code, LOCALE_SNATIVEDIGITS);
    return d.size() == 10 && d != L"0123456789" ? d : L"";
}

bool Clock_PaintLine(const std::wstring& text, const std::wstring& font, float pt, const std::wstring& color, float k,
                     ClockImage* out, const ClockLook* glow) {
    if (!OpenPainter() || (!UseFont(font) && !UseFont(kFonts[0].key))) return false;
    IDWriteTextFormat* fmt = nullptr;
    IDWriteTextLayout* layout = nullptr;
    DWRITE_TEXT_METRICS m{};
    HRESULT hr = P.f->CreateTextFormat(P.family.c_str(), P.fonts, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, pt * 4 / 3 * k, L"en-us", &fmt);
    bool rtl = false;  // text in a right-to-left script reads that way
    for (wchar_t c : text) rtl = rtl || (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFEFF);
    if (SUCCEEDED(hr) && rtl) fmt->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
    if (SUCCEEDED(hr)) hr = P.f->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, 8192, 8192, &layout);
    if (SUCCEEDED(hr)) hr = layout->GetMetrics(&m);
    if (SUCCEEDED(hr)) hr = layout->SetMaxWidth(m.widthIncludingTrailingWhitespace);
    if (SUCCEEDED(hr)) hr = layout->GetMetrics(&m);
    // Room around the text for the glow (spread in proportion to the text's size).
    const float glowR = glow && glow->glow > 0 ? glow->glowSize * pt / 24 * k : 0;
    const int pad = (int)ceilf(glowR);
    const int w = SUCCEEDED(hr) ? (int)ceilf(m.widthIncludingTrailingWhitespace) + 2 + 2 * pad : 0,
              h = SUCCEEDED(hr) ? (int)ceilf(m.height) + 2 * pad : 0;
    bool ok = false;
    if (w > 2 && h > 0 && w < 16384 && h < 16384) {
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
            ParseColor(color, &r, &g, &b);
            D2D1_COLOR_F c = D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f);
            if (SUCCEEDED(P.d2f->CreateDCRenderTarget(&rp, &rt)) && SUCCEEDED(rt->BindDC(dc, &rc)) &&
                SUCCEEDED(rt->CreateSolidColorBrush(&c, nullptr, &brush))) {
                rt->BeginDraw();
                rt->Clear(D2D1::ColorF(0, 0, 0, 0));
                rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                rt->DrawTextLayout(D2D1::Point2F(1.f + pad, (float)pad), layout, brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
                ok = SUCCEEDED(rt->EndDraw());
            }
            if (ok) {
                GdiFlush();
                if (glowR > 0) {
                    BYTE gr = r, gg = g, gb = b;
                    if (!glow->glowColor.empty()) ParseColor(glow->glowColor, &gr, &gg, &gb);
                    AddGlow((uint32_t*)bits, w, h, glowR, glow->glow, D2D1::ColorF(gr / 255.f, gg / 255.f, gb / 255.f), 1);
                }
                out->px.assign((uint32_t*)bits, (uint32_t*)bits + (size_t)w * h);
                out->w = w;
                out->h = h;
                out->pad = 0;
            }
            SelectObject(dc, old);
            DeleteObject(bmp);
        }
        Rel(brush);
        Rel(rt);
        DeleteDC(dc);
    }
    Rel(layout);
    Rel(fmt);
    return ok;
}

std::vector<ClockFontInfo> Clock_Fonts() {
    std::vector<ClockFontInfo> list;
    const std::wstring dir = FontsDir();
    for (const FontDef& f : kFonts) {
        ClockFontInfo i{f.key, f.getUrl, false, true};
        if (!f.res) {
            i.removable = !FindFontFile(dir, i.key + L".*").empty();
            i.ready = i.removable || !FontFile(i.key).empty();
        }
        list.push_back(i);
    }
    // Then the user's own, by name: font files in VideoBG's fonts folder that aren't one of the above.
    std::vector<ClockFontInfo> own;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsFontName(n)) continue;
            std::wstring key = n.substr(0, n.find_last_of(L'.'));
            bool dup = FindFont(key) != nullptr;
            for (const ClockFontInfo& o : own) dup = dup || !_wcsicmp(o.key.c_str(), key.c_str());
            if (!dup) own.push_back({key, nullptr, true, true});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(own.begin(), own.end(), [](const ClockFontInfo& a, const ClockFontInfo& b) { return _wcsicmp(a.key.c_str(), b.key.c_str()) < 0; });
    list.insert(list.end(), own.begin(), own.end());
    return list;
}

bool Clock_FontReady(const std::wstring& key) {
    const FontDef* f = FindFont(key);
    return (f && f->res) || !FontFile(key).empty();
}

namespace {

bool ReadStream(IStream* st, std::string* data) {
    data->clear();
    char buf[65536];
    ULONG got = 0;
    while (SUCCEEDED(st->Read(buf, sizeof buf, &got)) && got > 0) {
        data->append(buf, got);
        if (data->size() > 32u << 20) return false;  // no font is this big
    }
    return !data->empty();
}

// The font file in a .zip (opened as a folder by the shell, a level of subfolders deep), preferring
// one named after the font.
bool ReadZipFont(IShellItem* folder, const std::wstring& key, int depth, std::string* data, std::wstring* name) {
    IEnumShellItems* items = nullptr;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items)))) return false;
    IShellItem* best = nullptr;
    std::wstring bestName;
    IShellItem* it = nullptr;
    bool found = false;
    while (!found && items->Next(1, &it, nullptr) == S_OK) {
        wchar_t* nm = nullptr;
        SFGAOF attr = 0;
        it->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &attr);
        if (SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &nm))) {
            std::wstring n = nm;
            CoTaskMemFree(nm);
            std::wstring lower = n, k = key;
            for (auto& c : lower) c = (wchar_t)towlower(c);
            for (auto& c : k) c = (wchar_t)towlower(c);
            if (IsFontName(n) && (!best || lower.find(k) != std::wstring::npos)) {
                Rel(best);
                best = it;
                best->AddRef();
                bestName = n;
            } else if ((attr & SFGAO_FOLDER) && !(attr & SFGAO_STREAM) && depth < 2 && !best) {
                found = ReadZipFont(it, key, depth + 1, data, name);
            }
        }
        Rel(it);
    }
    items->Release();
    if (!found && best) {
        IStream* st = nullptr;
        if (SUCCEEDED(best->BindToHandler(nullptr, BHID_Stream, IID_PPV_ARGS(&st)))) {
            found = ReadStream(st, data);
            st->Release();
        }
        *name = bestName;
    }
    Rel(best);
    return found;
}

bool ReadFontSource(const std::wstring& path, const std::wstring& key, std::string* data, std::wstring* name) {
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos && !_wcsicmp(path.c_str() + dot, L".zip")) {
        IShellItem* zip = nullptr;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&zip)))) return false;
        bool ok = ReadZipFont(zip, key, 0, data, name);
        zip->Release();
        return ok;
    }
    if (!IsFontName(path)) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (32ll << 20);
    if (ok) {
        data->resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, data->data(), (DWORD)data->size(), &got, nullptr) && got == data->size();
    }
    CloseHandle(h);
    *name = path.substr(path.find_last_of(L"\\/") + 1);
    return ok;
}

// Reads a font file (or a .zip's, preferring one named like `prefer`) and checks that DirectWrite
// can use it. *ext: its extension; *family: its family name.
HRESULT LoadFontSource(const std::wstring& path, const std::wstring& prefer, std::string* data, std::wstring* ext,
                       std::wstring* family) {
    std::wstring name;
    if (!ReadFontSource(path, prefer, data, &name)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    *ext = name.substr(name.find_last_of(L'.'));
    if (!OpenPainter()) return E_FAIL;
    IDWriteFontFile* file = nullptr;
    IDWriteFontSetBuilder1* b = nullptr;
    IDWriteFontSet* set = nullptr;
    IDWriteFontCollection1* c = nullptr;
    float cap = 0;
    bool digits = false;
    HRESULT hr = P.loader->CreateInMemoryFontFileReference(P.f, data->data(), (UINT32)data->size(), nullptr, &file);
    if (SUCCEEDED(hr)) hr = P.f->CreateFontSetBuilder(&b);
    if (SUCCEEDED(hr)) hr = b->AddFontFile(file);
    if (SUCCEEDED(hr)) hr = b->CreateFontSet(&set);
    if (SUCCEEDED(hr)) hr = P.f->CreateFontCollectionFromFontSet(set, &c);
    if (SUCCEEDED(hr) && (!FamilyInfo(c, 0, family, &cap, &digits) || family->empty())) hr = E_INVALIDARG;
    Rel(c);
    Rel(set);
    Rel(b);
    Rel(file);
    return FAILED(hr) ? E_INVALIDARG : S_OK;
}

// Into VideoBG's fonts folder as key + ext, in place of any copy before.
bool SaveFont(const std::wstring& key, const std::string& data, const std::wstring& ext) {
    Clock_ReleasePainter();  // DirectWrite may still have the old copy open
    const std::wstring dir = FontsDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    for (std::wstring old; !(old = FindFontFile(dir, key + L".*")).empty();)
        if (!DeleteFileW(old.c_str())) break;
    const std::wstring dst = dir + L"\\" + key + ext, tmp = dst + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD put = 0;
    bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    ok = ok && MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool Contains(std::wstring s, std::wstring part) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    for (auto& c : part) c = (wchar_t)towlower(c);
    return s.find(part) != std::wstring::npos;
}

}  // namespace

HRESULT Clock_AddFontFile(const std::wstring& key, const std::wstring& path) {
    const FontDef* f = FindFont(key);
    if (!f || f->res) return E_INVALIDARG;
    std::string data;
    std::wstring ext, family;
    HRESULT hr = LoadFontSource(path, key, &data, &ext, &family);
    if (SUCCEEDED(hr) && !Contains(family, key)) hr = E_INVALIDARG;  // it has to be that font
    if (FAILED(hr)) {
        Log(L"clock: %ls isn't the %ls font (%ls)", path.c_str(), key.c_str(), family.c_str());
        return hr;
    }
    bool ok = SaveFont(f->key, data, ext);
    Log(L"clock: added the %ls font from %ls -> %ls", key.c_str(), path.c_str(), ok ? L"ok" : L"failed");
    return ok ? S_OK : E_FAIL;
}

HRESULT Clock_AddOwnFont(const std::wstring& path, std::wstring* key) {
    std::string data;
    std::wstring ext, family;
    HRESULT hr = LoadFontSource(path, L"regular", &data, &ext, &family);
    if (FAILED(hr)) {
        Log(L"clock: no font VideoBG can use in %ls", path.c_str());
        return hr;
    }
    // Named after its family, as far as a file name allows; a font VideoBG knows keeps its name.
    std::wstring name;
    for (wchar_t c : family) name += (c < 32 || wcschr(L"\\/:*?\"<>|", c)) ? L'_' : c;
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    while (!name.empty() && name[0] == L' ') name.erase(0, 1);
    if (name.empty()) return E_INVALIDARG;
    for (const FontDef& f : kFonts)
        if (!_wcsicmp(f.key, name.c_str()) || (!f.res && Contains(family, f.key))) name = f.key;
    *key = name;
    const FontDef* known = FindFont(name);
    if (known && known->res) return S_FALSE;  // already in VideoBG
    bool ok = SaveFont(name, data, ext);
    Log(L"clock: added your font %ls from %ls -> %ls", name.c_str(), path.c_str(), ok ? L"ok" : L"failed");
    return ok ? S_OK : E_FAIL;
}

bool Clock_RemoveFont(const std::wstring& key) {
    const FontDef* f = FindFont(key);
    if (f && f->res) return false;
    Clock_ReleasePainter();  // DirectWrite may have it open
    const std::wstring dir = FontsDir();
    bool ok = true;
    for (std::wstring file; ok && !(file = FindFontFile(dir, key + L".*")).empty();) ok = DeleteFileW(file.c_str()) != 0;
    Log(L"clock: removed the %ls font -> %ls", key.c_str(), ok ? L"ok" : L"failed");
    return ok;
}

bool Clock_FindDownloadedFont(const std::wstring& key) {
    wchar_t* p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) return false;
    const std::wstring dir = p;
    CoTaskMemFree(p);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*" + key + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool added = false;
    do {
        std::wstring n = fd.cFileName;
        size_t dot = n.find_last_of(L'.');
        bool zip = dot != std::wstring::npos && !_wcsicmp(n.c_str() + dot, L".zip");
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (zip || IsFontName(n)))
            added = SUCCEEDED(Clock_AddFontFile(key, dir + L"\\" + n));
    } while (!added && FindNextFileW(h, &fd));
    FindClose(h);
    return added;
}
