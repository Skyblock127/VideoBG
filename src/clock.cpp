// Desktop clock: day name, date and time in the design of the "Mond" Rainmeter clock (its sizes and
// spacing; see Credits in README.md), or in a design of VideoBG's own (a dial, a ring, flip tiles...),
// drawn by VideoBG itself: one small see-through, click-through window on the desktop that redraws
// once a minute. Each look picks the clock's style (and Mond's font). DirectWrite and Direct2D, and
// only the fonts being drawn, are loaded only while drawing.
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
    std::wstring days[7], shortDays[7], months[12], monthsAfterDay[12], shortMonths[12], am, pm, digits;
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
    for (int i = 0; i < 7; i++) {
        w.days[i] = kDays[i];
        w.shortDays[i] = std::wstring(kDays[i]).substr(0, 3);
    }
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
        d = GregorianText(lang, i == 0 ? CAL_SABBREVDAYNAME7 : CAL_SABBREVDAYNAME1 + i - 1);
        if (!d.empty()) w.shortDays[i] = Upper(lang, d);
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
    struct Face {
        std::wstring family;
        float cap = 0.7f;  // capitals' height per em
    } faces[5];            // the designs' fonts (UseDesignFonts)
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
    std::wstring drawnStyle;  // its style alone
    int w = 0, h = 0;
    bool closing = false;
    ULONGLONG flipAt = 0;  // a flip clock's flaps started falling (0: they don't)
    SYSTEMTIME flipTo{};   // to this minute
} W;

constexpr UINT_PTR TIMER_MINUTE = 1, TIMER_FLIP = 2;

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
    W.drawnStyle.clear();
    return W.hwnd != nullptr;
}

void DestroyClockWindow() {
    if (!W.hwnd) return;
    W.closing = true;
    DestroyWindow(W.hwnd);
    W.closing = false;
    W.hwnd = nullptr;
    W.drawn.clear();
    W.drawnStyle.clear();
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

// Puts the clock's picture in the window; false if it couldn't.
bool ShowPicture(const ClockImage& img) {
    bool ok = false;
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
        ok = UpdateLayeredWindow(W.hwnd, nullptr, &dst, &sz, dc, &zero, 0, &bf, ULW_ALPHA);
        if (!ok) Log(L"clock: couldn't update the window (error %lu)", GetLastError());
        SelectObject(dc, old);
        DeleteObject(bmp);
    }
    DeleteDC(dc);
    return ok;
}

// A flip clock's flaps falling over: the next picture, about 30 a second for a moment.
void FlipStep() {
    const float p = W.flipAt ? (float)((GetTickCount64() - W.flipAt) / (kClockFlipSeconds * 1000)) : 1;
    ClockImage img;
    if (p >= 1 || !Clock_Paint(W.look, Clock_PixelScale(), W.flipTo, &img, p) || !ShowPicture(img)) {
        KillTimer(W.hwnd, TIMER_FLIP);
        if (W.flipAt && Clock_Paint(W.look, Clock_PixelScale(), W.flipTo, &img)) ShowPicture(img);
        W.flipAt = 0;
        if (!UI_IsOpen()) Clock_ReleasePainter();
    }
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
    const std::wstring style = Clock_StyleKey(W.look) + std::to_wstring(k), key = Clock_StyleKey(W.look) + when;
    if (W.drawn != key) {
        // Only the minute changed, on a flip clock that can be seen: its flaps fall over.
        LPARAM mask = 0;
        Host_RendererState(&mask);
        const bool flip = Clock_Flips(W.look) && W.drawnStyle == style && !(mask & (PR_COVERED | PR_LOCKED | PR_DISPLAY_OFF));
        ClockImage img;
        if (Clock_Paint(W.look, k, t, &img, flip ? 0 : 1) && ShowPicture(img)) {
            W.drawn = key;
            W.drawnStyle = style;
        }
        if (flip) {
            W.flipAt = GetTickCount64();
            W.flipTo = t;
            SetTimer(W.hwnd, TIMER_FLIP, 30, nullptr);
        } else {
            KillTimer(W.hwnd, TIMER_FLIP);
            W.flipAt = 0;
            if (!UI_IsOpen()) Clock_ReleasePainter();  // nothing stays loaded between minutes
        }
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
            else if (w == TIMER_FLIP) FlipStep();
            return 0;
        case WM_DESTROY:
            if (!W.closing && h == W.hwnd) {  // destroyed with the desktop (Explorer restarting)
                W.hwnd = nullptr;
                W.drawn.clear();
                W.drawnStyle.clear();
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

// ---- Designs ----------------------------------------------------------------------------------
// Clocks of their own shape, beside the Mond clock (the text one, in any font). Each lays its parts
// out in pixels around 0,0 on a canvas, in the clock's colour at some strength; the canvas is then
// drawn once into a picture just big enough (and gets the look's glow and opacity like Mond).

enum DesignFace { F_MICHROMA, F_SYNCOPATE, F_QUICKSAND, F_ORBITRON, F_AUDIOWIDE };

// The designs' fonts (all in the exe, read where they are), loaded together.
bool UseDesignFonts() {
    if (P.fonts && P.fontKey == L"|designs") return true;
    Rel(P.fonts);
    P.fontKey.clear();
    IDWriteFontSetBuilder1* b = nullptr;
    IDWriteFontSet* set = nullptr;
    HRESULT hr = P.f->CreateFontSetBuilder(&b);
    for (int id : {IDR_FONT_MICHROMA, IDR_FONT_SYNCOPATE, IDR_FONT_QUICKSAND, IDR_FONT_ORBITRON, IDR_FONT_AUDIOWIDE})
        if (SUCCEEDED(hr) && !AddResFont(b, id)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = b->CreateFontSet(&set);
    if (SUCCEEDED(hr)) hr = P.f->CreateFontCollectionFromFontSet(set, &P.fonts);
    Rel(set);
    Rel(b);
    if (FAILED(hr)) return false;
    const wchar_t* names[] = {L"Michroma", L"Syncopate", L"Quicksand", L"Orbitron", L"Audiowide"};
    for (UINT32 i = 0; i < 5; i++) {
        UINT32 at = i;
        BOOL found = FALSE;
        bool digits = false;
        if (FAILED(P.fonts->FindFamilyName(names[i], &at, &found)) || !found) at = i;
        if (!FamilyInfo(P.fonts, at, &P.faces[i].family, &P.faces[i].cap, &digits)) {
            Rel(P.fonts);
            return false;
        }
    }
    P.fontKey = L"|designs";
    return true;
}

struct Item {
    enum Kind { TEXT, LINE, CIRCLE, RECT, ARC } kind = TEXT;
    D2D1_POINT_2F p{}, q{};       // line: its ends; circle, arc: the centre (q.x: the radius); rect: corners; text: top left
    float width = 0;              // a stroke's width (0: filled); text: added to its letters' strokes
    float radius = 0;             // rect: corners; arc: where it starts (degrees clockwise from 12 o'clock)
    float sweep = 0;              // arc: degrees clockwise
    float a = 1;                  // how strong the clock's colour is
    IDWriteTextLayout* text = nullptr;
    bool clipped = false;         // only what's inside `clip` shows
    D2D1_RECT_F clip{};
    float squash = 1, hinge = 0;  // folded towards the line y = hinge: its height times squash (a flip clock's flap)
};

struct Canvas {
    std::vector<Item> items;
    float l = 1e9f, t = 1e9f, r = -1e9f, b = -1e9f;  // what's drawn, all of it
    ~Canvas() {
        for (Item& i : items) Rel(i.text);
    }
    void Grow(float x0, float y0, float x1, float y1) {
        l = std::min(l, x0);
        t = std::min(t, y0);
        r = std::max(r, x1);
        b = std::max(b, y1);
    }
    void Line(D2D1_POINT_2F p, D2D1_POINT_2F q, float w, float a) {
        Item i;
        i.kind = Item::LINE;
        i.p = p;
        i.q = q;
        i.width = w;
        i.a = a;
        items.push_back(i);
        Grow(std::min(p.x, q.x) - w, std::min(p.y, q.y) - w, std::max(p.x, q.x) + w, std::max(p.y, q.y) + w);
    }
    void Circle(float cx, float cy, float rad, float w, float a) {  // w 0: filled
        Item i;
        i.kind = Item::CIRCLE;
        i.p = D2D1::Point2F(cx, cy);
        i.q.x = rad;
        i.width = w;
        i.a = a;
        items.push_back(i);
        Grow(cx - rad - w, cy - rad - w, cx + rad + w, cy + rad + w);
    }
    void Rect(const D2D1_RECT_F& rc, float radius, float a) {
        Item i;
        i.kind = Item::RECT;
        i.p = D2D1::Point2F(rc.left, rc.top);
        i.q = D2D1::Point2F(rc.right, rc.bottom);
        i.radius = radius;
        i.a = a;
        items.push_back(i);
        Grow(rc.left, rc.top, rc.right, rc.bottom);
    }
    void Frame(const D2D1_RECT_F& rc, float radius, float w, float a) {  // a rect's outline
        Rect(rc, radius, a);
        items.back().width = w;
        Grow(rc.left - w, rc.top - w, rc.right + w, rc.bottom + w);
    }
    void Arc(float cx, float cy, float rad, float from, float sweep, float w, float a) {
        Item i;
        i.kind = Item::ARC;
        i.p = D2D1::Point2F(cx, cy);
        i.q.x = rad;
        i.radius = from;
        i.sweep = sweep;
        i.width = w;
        i.a = a;
        items.push_back(i);
        Grow(cx - rad - w, cy - rad - w, cx + rad + w, cy + rad + w);
    }
    // The items from `from` on show only inside clip, folded towards hinge (see Item).
    void Fold(size_t from, const D2D1_RECT_F& clip, float hinge, float squash) {
        for (size_t k = from; k < items.size(); k++) {
            items[k].clipped = true;
            items[k].clip = clip;
            items[k].hinge = hinge;
            items[k].squash = squash;
        }
    }
};

// Weights. kThick: the font's own (regular) letters with their strokes made a little thicker, for
// the days' names and AM/PM (the fonts are thin there, and have no bolder face of their own).
constexpr int kRegular = 400, kThick = 1;
constexpr float kThickening = 0.06f;  // per em, added across each stroke

// A line of text in one of the designs' fonts, px pixels high, its letters `gap` pixels apart.
struct Run {
    IDWriteTextLayout* layout = nullptr;
    float w = 0, h = 0, base = 0, cap = 0;  // its width and height, its baseline (from its top), its capitals' height
};

bool Lay(const std::wstring& s, int face, float px, float gap, bool rtl, Run* r, int weight = kRegular) {
    if (s.empty() || px <= 0) return false;
    IDWriteTextFormat* fmt = nullptr;
    if (FAILED(P.f->CreateTextFormat(P.faces[face].family.c_str(), P.fonts, (DWRITE_FONT_WEIGHT)(weight == kThick ? kRegular : weight), DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, px, L"en-us", &fmt)))
        return false;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (rtl) fmt->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
    HRESULT hr = P.f->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, 8192, 8192, &r->layout);
    Rel(fmt);
    if (FAILED(hr)) return false;
    if (gap > 0) {  // half before each letter, half after: the line stays centred
        IDWriteTextLayout1* l1 = nullptr;
        if (SUCCEEDED(r->layout->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&l1))) {
            l1->SetCharacterSpacing(gap / 2, gap / 2, 0, DWRITE_TEXT_RANGE{0, (UINT32)s.size()});
            l1->Release();
        }
    }
    DWRITE_TEXT_METRICS m{};
    DWRITE_LINE_METRICS lm{};
    UINT32 n = 0;
    if (FAILED(r->layout->GetMetrics(&m)) || FAILED(r->layout->SetMaxWidth(m.widthIncludingTrailingWhitespace)) ||
        FAILED(r->layout->GetMetrics(&m)) || FAILED(r->layout->GetLineMetrics(&lm, 1, &n))) {
        Rel(r->layout);
        return false;
    }
    r->w = m.widthIncludingTrailingWhitespace;
    r->h = m.height;
    r->base = lm.baseline;
    r->cap = P.faces[face].cap * px;
    return true;
}

float Width(const std::wstring& s, int face, float px, float gap, int weight = kRegular) {
    Run r;
    if (!Lay(s, face, px, gap, false, &r, weight)) return 0;
    Rel(r.layout);
    return r.w;
}

// Where a text's ink starts and ends, from its layout's left (fonts leave room either side).
bool Ink(const std::wstring& s, int face, float px, float* left, float* right, int weight = kRegular) {
    Run r;
    DWRITE_OVERHANG_METRICS o{};
    if (!Lay(s, face, px, 0, false, &r, weight)) return false;
    const bool ok = SUCCEEDED(r.layout->GetOverhangMetrics(&o));
    *left = -o.left;
    *right = r.w + o.right;
    Rel(r.layout);
    return ok;
}

// Puts a line of text with its capitals' top at capTop: centred on x (align 0), starting at x (1) or
// ending at x (-1). Returns its width.
float Put(Canvas& cv, const std::wstring& s, int face, float px, float gap, float x, float capTop, int align, float a,
          bool rtl = false, int weight = kRegular) {
    Run r;
    if (!Lay(s, face, px, gap, rtl, &r, weight)) return 0;
    Item i;
    i.p = D2D1::Point2F(align == 0 ? x - r.w / 2 : align > 0 ? x : x - r.w, capTop - (r.base - r.cap));
    i.a = a;
    i.text = r.layout;
    if (weight == kThick) i.width = kThickening * px;
    cv.items.push_back(i);
    cv.Grow(i.p.x, i.p.y, i.p.x + r.w, i.p.y + r.h);
    return r.w;
}

// What a design writes: the day (and its short name), the date (in the look's date style; "" for
// none), the time's parts; and the numbers, for the designs that draw them.
struct Says {
    std::wstring day, shortDay, shortMonth, dayNumber, date, hh, mm, mark;  // mark: AM / PM (12-hour)
    int hour = 0, minute = 0;              // 24-hour
    bool rtl = false, joined = false;      // its script reads right to left; its letters join (no spacing)
};

Says SaysFor(const ClockLook& l, const SYSTEMTIME& t) {
    const Words& w = WordsFor(l.lang, l.nativeDigits);
    Says s;
    s.day = w.days[t.wDayOfWeek % 7];
    s.shortDay = w.shortDays[t.wDayOfWeek % 7];
    s.shortMonth = w.shortMonths[(t.wMonth + 11) % 12];
    s.date = Clock_DateText(l, l.date, t);
    wchar_t b[16];
    swprintf(b, 16, L"%d", t.wDay);
    s.dayNumber = WithDigits(w, b);
    swprintf(b, 16, l.h24 ? L"%02d" : L"%d", l.h24 ? t.wHour : (t.wHour + 11) % 12 + 1);
    s.hh = WithDigits(w, b);
    swprintf(b, 16, L"%02d", t.wMinute);
    s.mm = WithDigits(w, b);
    if (!l.h24) s.mark = t.wHour < 12 ? w.am : w.pm;
    s.hour = t.wHour;
    s.minute = t.wMinute;
    s.rtl = w.rtl;
    s.joined = w.joined;
    return s;
}

// A flip clock's flaps falling over at a new minute: how far (1: still) and what showed before.
struct {
    float p = 1;
    Says before;
} g_flip;

constexpr float kPi = 3.14159265f;

D2D1_POINT_2F Polar(float rad, float deg) {  // from 0,0: degrees clockwise from 12 o'clock
    return D2D1::Point2F(rad * sinf(deg * kPi / 180), -rad * cosf(deg * kPi / 180));
}

// The day's name, then the date (if any), centred under y.
float DayAndDate(Canvas& cv, const Says& s, float u, float y, bool day = true) {
    if (day) {
        Put(cv, s.day, F_SYNCOPATE, 16 * u, s.joined ? 0 : 5 * u, 0, y, 0, 0.9f, s.rtl, kThick);
        y += 16 * u * 0.72f + 14 * u;
    }
    if (!s.date.empty()) {
        Put(cv, s.date, F_QUICKSAND, 20 * u, 1.5f * u, 0, y, 0, 0.95f, s.rtl);
        y += 20 * u * 0.72f + 14 * u;
    }
    return y;
}

// AM / PM, small (none in 24-hour time).
void Mark(Canvas& cv, const Says& s, float px, float u, float x, float capTop, int align) {
    if (!s.mark.empty()) Put(cv, s.mark, F_SYNCOPATE, px, s.joined ? 0 : 3 * u, x, capTop, align, 0.85f, s.rtl, kThick);
}

// The time in Quicksand, its colon set by its ink, evenly between the hours and minutes (the font
// gives it room on one side only): centred on x (align 0) or starting at x (1). Returns its width;
// with no canvas, only that.
float RoundTime(Canvas* cv, const Says& s, float px, float x, float capTop, int align, float a = 1) {
    const float gap = 0.1f * px;
    const std::wstring part[3] = {s.hh, L":", s.mm};
    float l[3], r[3], tw = 2 * gap;
    for (int i = 0; i < 3; i++) {
        if (!Ink(part[i], F_QUICKSAND, px, &l[i], &r[i])) return 0;
        tw += r[i] - l[i];
    }
    float at = align == 0 ? x - tw / 2 : x;
    for (int i = 0; cv && i < 3; i++) {
        Put(*cv, part[i], F_QUICKSAND, px, 0, at - l[i], capTop, 1, a);
        at += r[i] - l[i] + gap;
    }
    return tw;
}

// Dial: hands on a round face with its minutes marked; the date under it, if one is chosen.
void Dial(Canvas& cv, const ClockLook&, const SYSTEMTIME& t, const Says& s, float u) {
    const float R = 120 * u;
    cv.Circle(0, 0, R, 3.2f * u, 0.95f);
    for (int i = 0; i < 60; i++) {
        const bool hour = i % 5 == 0, quarter = i % 15 == 0;
        const float len = (quarter ? 22 : hour ? 15 : 6) * u, w = (quarter ? 5.5f : hour ? 3.6f : 1.6f) * u;
        cv.Line(Polar(R - 11 * u, i * 6.f), Polar(R - 11 * u - len, i * 6.f), w, hour ? 1 : 0.45f);
    }
    const float hourDeg = ((t.wHour % 12) + t.wMinute / 60.f) * 30, minDeg = t.wMinute * 6.f;
    cv.Line(Polar(-14 * u, hourDeg), Polar(R * 0.5f, hourDeg), 9 * u, 1);
    cv.Line(Polar(-18 * u, minDeg), Polar(R * 0.8f, minDeg), 5.5f * u, 1);
    cv.Circle(0, 0, 9 * u, 0, 1);
    DayAndDate(cv, s, u, R + 30 * u, false);
}

// Watch: a watch face with batons for the hours and slim hands, the day and date in a window at three.
void Watch(Canvas& cv, const ClockLook&, const SYSTEMTIME& t, const Says& s, float u) {
    const float R = 122 * u;
    cv.Circle(0, 0, R, 2.4f * u, 0.6f);
    for (int i = 0; i < 60; i++) {
        if (i % 5) {
            const D2D1_POINT_2F p = Polar(R - 13 * u, i * 6.f);
            cv.Circle(p.x, p.y, 1.4f * u, 0, 0.55f);
        } else if (i != 15) {  // (three: the date window)
            const bool main = i % 15 == 0;
            cv.Line(Polar(R - 13 * u, i * 6.f), Polar(R - (main ? 42 : 32) * u, i * 6.f), (main ? 7.5f : 5) * u, 1);
        }
    }
    const std::wstring dd = s.shortDay + L"  " + s.dayNumber;
    const float px = 13 * u, cap = P.faces[F_SYNCOPATE].cap * px, right = R - 20 * u;
    const float tw = Put(cv, dd, F_SYNCOPATE, px, s.joined ? 0 : 2 * u, right - 9 * u, -cap / 2, -1, 1, false, kThick);
    cv.Frame(D2D1::RectF(right - 18 * u - tw, -cap / 2 - 9 * u, right, cap / 2 + 9 * u), 5 * u, 1.6f * u, 0.7f);
    const float hourDeg = ((t.wHour % 12) + t.wMinute / 60.f) * 30, minDeg = t.wMinute * 6.f;
    cv.Line(Polar(-12 * u, hourDeg), Polar(R * 0.52f, hourDeg), 7.5f * u, 1);
    cv.Line(Polar(-16 * u, minDeg), Polar(R * 0.84f, minDeg), 4.5f * u, 1);
    cv.Circle(0, 0, 7.5f * u, 0, 1);
}

// Ring: the hour fills a ring around the time; the day above the time, the date under the ring.
void RingDesign(Canvas& cv, const ClockLook&, const SYSTEMTIME& t, const Says& s, float u) {
    const float R = 118 * u, w = 7 * u;
    cv.Circle(0, 0, R, w, 0.2f);
    const float sweep = t.wMinute * 6.f;
    if (sweep > 0) cv.Arc(0, 0, R, 0, sweep, w, 1);
    const D2D1_POINT_2F end = Polar(R, sweep);
    cv.Circle(end.x, end.y, 6.5f * u, 0, 1);
    const float px = 46 * u, cap = P.faces[F_MICHROMA].cap * px;
    Put(cv, s.hh + L":" + s.mm, F_MICHROMA, px, 0, 0, -cap / 2, 0, 1);
    Put(cv, s.day, F_SYNCOPATE, 13 * u, s.joined ? 0 : 4 * u, 0, -cap / 2 - 30 * u, 0, 0.88f, s.rtl, kThick);
    Mark(cv, s, 12 * u, u, 0, cap / 2 + 20 * u, 0);
    DayAndDate(cv, s, u, R + 34 * u, false);
}

// Orbit: the minutes and the hours on two rings around the time (no day); the date under them.
void Orbit(Canvas& cv, const ClockLook&, const SYSTEMTIME&, const Says& s, float u) {
    const float R1 = 124 * u, R2 = 104 * u, w1 = 5 * u, w2 = 11 * u;
    cv.Circle(0, 0, R1, w1, 0.16f);
    cv.Circle(0, 0, R2, w2, 0.16f);
    const float minutes = s.minute * 6.f, hours = ((s.hour % 12) + s.minute / 60.f) * 30;
    if (hours > 0) cv.Arc(0, 0, R2, 0, hours, w2, 0.7f);
    if (minutes > 0) cv.Arc(0, 0, R1, 0, minutes, w1, 1);
    const D2D1_POINT_2F end = Polar(R1, minutes);
    cv.Circle(end.x, end.y, 6 * u, 0, 1);
    const float px = 58 * u, cap = P.faces[F_QUICKSAND].cap * px;
    RoundTime(&cv, s, px, 0, -cap / 2, 0);
    Mark(cv, s, 12 * u, u, 0, cap / 2 + 18 * u, 0);
    DayAndDate(cv, s, u, R1 + 34 * u, false);
}

// Flip: the hours and minutes on split-flap tiles, each number cut across the middle with its halves
// on the two flaps; at a new minute the top flap falls over (g_flip). AM/PM in the corner; the day
// and date under them.
void FlipTile(Canvas& cv, float x0, float W, float H, float u, const std::wstring& now, const std::wstring& was, float p) {
    const float split = 3 * u, px = 84 * u, cap = P.faces[F_ORBITRON].cap * px, half = H / 2 - split / 2;
    p *= p;  // it falls: faster as it goes
    const D2D1_RECT_F upper = D2D1::RectF(x0 - u, -H / 2 - u, x0 + W + u, -split / 2), lower = D2D1::RectF(x0 - u, split / 2, x0 + W + u, H / 2 + u);
    auto flap = [&](const std::wstring& digits, D2D1_RECT_F clip, float squash, float a) {
        const size_t from = cv.items.size();
        cv.Rect(D2D1::RectF(x0, -H / 2, x0 + W, H / 2), 20 * u, a);
        Put(cv, digits, F_ORBITRON, px, 0, x0 + W / 2, -cap / 2, 0, 1);
        cv.Fold(from, clip, 0, squash);
    };
    if (p >= 1 || now == was) {
        flap(now, upper, 1, 0.18f);
        flap(now, lower, 1, 0.18f);
    } else if (p < 0.5f) {  // the top flap (the number before) folds down; the new number's top shows above it
        const float s = cosf(p * kPi);
        flap(now, D2D1::RectF(upper.left, upper.top, upper.right, -split / 2 - half * s), 1, 0.18f);
        flap(was, upper, s, 0.18f + 0.1f * (1 - s));  // (lit as it turns towards you)
        flap(was, lower, 1, 0.18f);
    } else {  // and comes down over the bottom half: the new number's bottom
        const float s = -cosf(p * kPi);
        flap(now, upper, 1, 0.18f);
        flap(was, D2D1::RectF(lower.left, split / 2 + half * s, lower.right, lower.bottom), 1, 0.18f);
        flap(now, lower, s, 0.18f + 0.1f * (1 - s));
    }
}

void Flip(Canvas& cv, const ClockLook&, const SYSTEMTIME&, const Says& s, float u) {
    const float W = 148 * u, H = 168 * u, gap = 14 * u;
    const Says& was = g_flip.p < 1 ? g_flip.before : s;
    FlipTile(cv, -gap / 2 - W, W, H, u, s.hh, was.hh, g_flip.p);
    FlipTile(cv, gap / 2, W, H, u, s.mm, was.mm, g_flip.p);
    Mark(cv, s, 11 * u, u, -gap / 2 - W + 16 * u, -H / 2 + 16 * u, 1);
    DayAndDate(cv, s, u, H / 2 + 36 * u);
}

// Digital: LED digits, every segment there and the lit ones bright, leaning a little; the short day
// and AM/PM over them, the date under.
void Digit(Canvas& cv, float x, float y, float w, float h, float sw, int digit) {
    static const unsigned char kLit[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};  // segments a-g: bits 0-6
    const float g = sw * 1.15f, l = x, r = x + w, t = y, m = y + h / 2, b = y + h;
    auto at = [&](float px, float py) { return D2D1::Point2F(px - py * 0.08f, py); };  // leaning right
    const float seg[7][4] = {{l + g, t, r - g, t}, {r, t + g, r, m - g}, {r, m + g, r, b - g}, {l + g, b, r - g, b},
                             {l, m + g, l, b - g}, {l, t + g, l, m - g}, {l + g, m, r - g, m}};
    const unsigned lit = digit >= 0 && digit <= 9 ? kLit[digit] : 0;
    for (int k = 0; k < 7; k++) cv.Line(at(seg[k][0], seg[k][1]), at(seg[k][2], seg[k][3]), sw, lit >> k & 1 ? 1.f : 0.1f);
}

void DigitalDesign(Canvas& cv, const ClockLook& l, const SYSTEMTIME&, const Says& s, float u) {
    const float w = 56 * u, h = 104 * u, sw = 10 * u, pair = 22 * u, colon = 40 * u;
    const int hour = l.h24 ? s.hour : (s.hour + 11) % 12 + 1;
    const int digits[4] = {l.h24 || hour >= 10 ? hour / 10 : -1, hour % 10, s.minute / 10, s.minute % 10};
    const float x0 = -(4 * w + 2 * pair + colon) / 2, xs[4] = {x0, x0 + w + pair, x0 + 2 * w + pair + colon, x0 + 3 * w + 2 * pair + colon};
    for (int i = 0; i < 4; i++) Digit(cv, xs[i], -h / 2, w, h, sw, digits[i]);
    for (float y : {-h / 5, h / 5}) cv.Circle(-y * 0.08f, y, 6 * u, 0, 1);
    const float top = -h / 2 - 26 * u - P.faces[F_ORBITRON].cap * 18 * u;
    Put(cv, s.shortDay, F_ORBITRON, 18 * u, s.joined ? 0 : 5 * u, x0 + 6 * u, top, 1, 0.9f, s.rtl, 700);
    if (!s.mark.empty()) Put(cv, s.mark, F_ORBITRON, 18 * u, s.joined ? 0 : 4 * u, -x0 - 2 * u, top, -1, 0.9f, s.rtl, 700);
    DayAndDate(cv, s, u, h / 2 + 36 * u, false);
}

// Stack: big hours over fainter minutes; AM/PM beside the hours; the date under them.
void Stack(Canvas& cv, const ClockLook&, const SYSTEMTIME&, const Says& s, float u) {
    const float px = 118 * u, cap = P.faces[F_MICHROMA].cap * px, gap = 24 * u;
    const float hw = Put(cv, s.hh, F_MICHROMA, px, 0, 0, -cap - gap / 2, 0, 1);
    Put(cv, s.mm, F_MICHROMA, px, 0, 0, gap / 2, 0, 0.55f);
    Mark(cv, s, 14 * u, u, hw / 2 + 14 * u, -cap - gap / 2, 1);
    DayAndDate(cv, s, u, cap + gap / 2 + 36 * u, false);
}

// Line: the time, a bar, then the day over the date, all on one line.
void LineDesign(Canvas& cv, const ClockLook&, const SYSTEMTIME&, const Says& s, float u) {
    const float px = 64 * u, cap = P.faces[F_MICHROMA].cap * px;
    const std::wstring time = s.hh + L":" + s.mm;
    const float tw = Width(time, F_MICHROMA, px, 0);
    const float mw = s.mark.empty() ? 0 : Width(s.mark, F_SYNCOPATE, 14 * u, 3 * u, kThick) + 10 * u;
    const float dayPx = 17 * u, datePx = 19 * u;
    const float rw = std::max(Width(s.day, F_SYNCOPATE, dayPx, s.joined ? 0 : 4 * u, kThick), s.date.empty() ? 0 : Width(s.date, F_QUICKSAND, datePx, 1.5f * u));
    const float x0 = -(tw + mw + 52 * u + rw) / 2, top = -cap / 2;
    Put(cv, time, F_MICHROMA, px, 0, x0, top, 1, 1);
    if (mw > 0) Mark(cv, s, 14 * u, u, x0 + tw + 10 * u, top, 1);
    const float bar = x0 + tw + mw + 26 * u;
    cv.Line(D2D1::Point2F(bar, top - 8 * u), D2D1::Point2F(bar, top + cap + 8 * u), 3 * u, 0.8f);
    const float rx = bar + 26 * u;
    if (s.date.empty()) {
        Put(cv, s.day, F_SYNCOPATE, dayPx, s.joined ? 0 : 4 * u, rx, -dayPx * 0.36f, 1, 0.9f, s.rtl, kThick);
    } else {
        Put(cv, s.day, F_SYNCOPATE, dayPx, s.joined ? 0 : 4 * u, rx, top, 1, 0.9f, s.rtl, kThick);
        Put(cv, s.date, F_QUICKSAND, datePx, 1.5f * u, rx, top + cap - datePx * 0.72f, 1, 0.95f, s.rtl);
    }
}

// Big: the day and the date over a big, round time.
void Big(Canvas& cv, const ClockLook&, const SYSTEMTIME&, const Says& s, float u) {
    const float y = DayAndDate(cv, s, u, 0) + 6 * u;
    const float tw = RoundTime(&cv, s, 150 * u, 0, y, 0);
    Mark(cv, s, 16 * u, u, tw / 2 + 12 * u, y, 1);
}

// Bold: the time in heavy, wide letters under the day; the date under it.
void Bold(Canvas& cv, const ClockLook&, const SYSTEMTIME&, const Says& s, float u) {
    const float px = 112 * u, cap = P.faces[F_ORBITRON].cap * px, dayPx = 22 * u;
    const float tw = Put(cv, s.hh + L":" + s.mm, F_ORBITRON, px, 0, 0, -cap / 2, 0, 1, false, 900);
    Put(cv, s.day, F_ORBITRON, dayPx, s.joined ? 0 : 9 * u, 0, -cap / 2 - 24 * u - P.faces[F_ORBITRON].cap * dayPx, 0, 0.9f, s.rtl, 700);
    if (!s.mark.empty()) Put(cv, s.mark, F_ORBITRON, 16 * u, s.joined ? 0 : 3 * u, tw / 2 + 12 * u, -cap / 2, 1, 0.85f, s.rtl, 700);
    if (!s.date.empty()) Put(cv, s.date, F_QUICKSAND, 22 * u, 2 * u, 0, cap / 2 + 26 * u, 0, 0.95f, s.rtl);
}

// Calendar: a desk calendar's page (the short day, the day's number, the month) beside the time.
void CalendarDesign(Canvas& cv, const ClockLook&, const SYSTEMTIME&, const Says& s, float u) {
    const float W = 128 * u, H = 148 * u, head = 40 * u, tpx = 96 * u, tcap = P.faces[F_QUICKSAND].cap * tpx;
    const float tw = RoundTime(nullptr, s, tpx, 0, 0, 1), x0 = -(W + 36 * u + tw) / 2, cx = x0 + W / 2;
    cv.Frame(D2D1::RectF(x0, -H / 2, x0 + W, H / 2), 16 * u, 3 * u, 0.9f);
    cv.Line(D2D1::Point2F(x0 + 1.5f * u, -H / 2 + head), D2D1::Point2F(x0 + W - 1.5f * u, -H / 2 + head), 2 * u, 0.5f);
    for (float rx : {x0 + W * 0.3f, x0 + W * 0.7f}) cv.Line(D2D1::Point2F(rx, -H / 2 - 9 * u), D2D1::Point2F(rx, -H / 2 + 7 * u), 5 * u, 1);
    const float dpx = 15 * u, dcap = P.faces[F_SYNCOPATE].cap * dpx;
    Put(cv, s.shortDay, F_SYNCOPATE, dpx, s.joined ? 0 : 4 * u, cx, -H / 2 + (head - dcap) / 2 + 2 * u, 0, 0.95f, s.rtl, kThick);
    const float npx = 64 * u, ncap = P.faces[F_MICHROMA].cap * npx, mpx = 13 * u, mcap = P.faces[F_SYNCOPATE].cap * mpx;
    const float body = H - head, top = -H / 2 + head + (body - ncap - 14 * u - mcap) / 2;
    Put(cv, s.dayNumber, F_MICHROMA, npx, 0, cx, top, 0, 1);
    Put(cv, s.shortMonth, F_SYNCOPATE, mpx, s.joined ? 0 : 4 * u, cx, top + ncap + 14 * u, 0, 0.8f, s.rtl, kThick);
    const float tx = x0 + W + 36 * u;
    RoundTime(&cv, s, tpx, tx, -tcap / 2 - (s.mark.empty() ? 0 : 12 * u), 1);
    Mark(cv, s, 14 * u, u, tx + tw, tcap / 2 + 6 * u, -1);
}

// Words: the time in English words, the hour over the minutes, then the part of the day.
std::wstring Number(int n) {  // 1-59
    static const wchar_t* const kOnes[] = {L"",        L"ONE",     L"TWO",       L"THREE",    L"FOUR",     L"FIVE",    L"SIX",
                                           L"SEVEN",   L"EIGHT",   L"NINE",      L"TEN",      L"ELEVEN",   L"TWELVE",  L"THIRTEEN",
                                           L"FOURTEEN", L"FIFTEEN", L"SIXTEEN",  L"SEVENTEEN", L"EIGHTEEN", L"NINETEEN"};
    static const wchar_t* const kTens[] = {L"", L"", L"TWENTY", L"THIRTY", L"FORTY", L"FIFTY"};
    if (n < 20) return kOnes[n];
    return std::wstring(kTens[n / 10]) + (n % 10 ? std::wstring(L" ") + kOnes[n % 10] : L"");
}

void WordsDesign(Canvas& cv, const ClockLook&, const SYSTEMTIME&, const Says& s, float u) {
    const std::wstring hour = Number((s.hour + 11) % 12 + 1);
    const std::wstring minutes = s.minute == 0 ? L"O'CLOCK" : s.minute < 10 ? L"OH " + Number(s.minute) : Number(s.minute);
    const wchar_t* part = s.hour < 5 ? L"AT NIGHT" : s.hour < 12 ? L"IN THE MORNING" : s.hour < 17 ? L"IN THE AFTERNOON" : s.hour < 21 ? L"IN THE EVENING" : L"AT NIGHT";
    const float hpx = 70 * u, hcap = P.faces[F_ORBITRON].cap * hpx, mpx = 30 * u, mcap = P.faces[F_SYNCOPATE].cap * mpx;
    float y = -(hcap + 22 * u + mcap) / 2;
    Put(cv, hour, F_ORBITRON, hpx, 6 * u, 0, y, 0, 1, false, 800);
    y += hcap + 22 * u;
    Put(cv, minutes, F_SYNCOPATE, mpx, 7 * u, 0, y, 0, 0.92f, false, kThick);
    y += mcap + 20 * u;
    Put(cv, part, F_QUICKSAND, 18 * u, 3 * u, 0, y, 0, 0.7f);
    DayAndDate(cv, s, u, y + 18 * u * 0.72f + 26 * u, false);
}

struct Design {
    const wchar_t* key;
    const wchar_t* name;
    const wchar_t* about;
    bool day, hours, digits, date;  // what it shows: the day's name; 12- or 24-hour time; numbers; the date line
    void (*lay)(Canvas&, const ClockLook&, const SYSTEMTIME&, const Says&, float u);
};

const Design kDesigns[] = {
    {L"dial", L"Dial", L"Hands on a dial", false, false, false, true, Dial},
    {L"watch", L"Watch", L"A watch with the day and date", true, false, true, false, Watch},
    {L"ring", L"Ring", L"The hour fills a ring", true, true, true, true, RingDesign},
    {L"orbit", L"Orbit", L"Hours and minutes on two rings", false, true, true, true, Orbit},
    {L"flip", L"Flip", L"Flip-clock tiles", true, true, true, true, Flip},
    {L"digital", L"Digital", L"LED digits under the day", true, true, false, true, DigitalDesign},
    {L"stack", L"Stack", L"Hours over minutes", false, true, true, true, Stack},
    {L"line", L"Line", L"Time, then day and date", true, true, true, true, LineDesign},
    {L"big", L"Big", L"The date over a big time", true, true, true, true, Big},
    {L"bold", L"Bold", L"Heavy time under the day", true, true, true, true, Bold},
    {L"calendar", L"Calendar", L"A calendar page and the time", true, true, true, false, CalendarDesign},
    {L"words", L"Words", L"The time in words", false, false, false, true, WordsDesign},
};

const Design* FindDesign(const std::wstring& key) {
    for (const Design& d : kDesigns)
        if (!key.empty() && !_wcsicmp(d.key, key.c_str())) return &d;
    return nullptr;
}

ID2D1PathGeometry* ArcGeometry(const Item& i) {
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(P.d2f->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) {
        Rel(g);
        return nullptr;
    }
    const float rad = i.q.x, sweep = std::min(i.sweep, 359.9f);
    const D2D1_POINT_2F a = Polar(rad, i.radius), b = Polar(rad, i.radius + sweep);
    sink->BeginFigure(D2D1::Point2F(i.p.x + a.x, i.p.y + a.y), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(i.p.x + b.x, i.p.y + b.y), D2D1::SizeF(rad, rad), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  sweep > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    return g;
}

// Draws text as its letters' outlines, filled and widened by `thick` (one shape: no overlap shows
// where the item is see-through).
struct ThickText : IDWriteTextRenderer {
    ID2D1RenderTarget* rt;
    ID2D1Brush* brush;
    ID2D1StrokeStyle* round;
    float thick;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        *p = IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IDWritePixelSnapping)) || IsEqualIID(riid, __uuidof(IDWriteTextRenderer)) ? this
                                                                                                                                                 : nullptr;
        return *p ? S_OK : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* no) override {
        *no = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
        rt->GetTransform((D2D1_MATRIX_3X2_F*)m);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* p) override {
        *p = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        ID2D1PathGeometry *outline = nullptr, *wide = nullptr, *both = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        auto open = [&](ID2D1PathGeometry** g) { return SUCCEEDED(P.d2f->CreatePathGeometry(g)) && SUCCEEDED((*g)->Open(&sink)); };
        auto close = [&] {
            const bool ok = SUCCEEDED(sink->Close());
            Rel(sink);
            return ok;
        };
        bool ok = open(&outline) && SUCCEEDED(run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances, run->glyphOffsets,
                                                                                run->glyphCount, run->isSideways, run->bidiLevel % 2, sink)) && close();
        ok = ok && open(&wide) && SUCCEEDED(outline->Widen(thick, round, nullptr, 0.05f, sink)) && close();
        ok = ok && open(&both) && SUCCEEDED(outline->CombineWithGeometry(wide, D2D1_COMBINE_MODE_UNION, nullptr, 0.05f, sink)) && close();
        Rel(sink);
        if (ok) {
            D2D1_MATRIX_3X2_F was;
            rt->GetTransform(&was);
            rt->SetTransform(D2D1::Matrix3x2F::Translation(x, y) * was);
            rt->FillGeometry(both, brush);
            rt->SetTransform(was);
        }
        Rel(both);
        Rel(wide);
        Rel(outline);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }
};

// A design's canvas, drawn into the picture (with the look's glow and opacity, as Mond).
bool PaintDesign(const Design& d, const ClockLook& look, float k, const SYSTEMTIME& t, ClockImage* out, float flip) {
    if (!UseDesignFonts()) return false;
    Canvas cv;
    const float u = k * look.size;
    g_flip.p = 1;
    if (flip < 1) {  // the minute before, for the flaps falling over
        FILETIME f;
        SYSTEMTIME before = t;
        if (SystemTimeToFileTime(&t, &f)) {
            ULARGE_INTEGER n{{f.dwLowDateTime, f.dwHighDateTime}};
            n.QuadPart -= 60ull * 10000000;
            f = FILETIME{n.LowPart, n.HighPart};
            FileTimeToSystemTime(&f, &before);
        }
        g_flip.p = std::clamp(flip, 0.f, 1.f);
        g_flip.before = SaysFor(look, before);
    }
    d.lay(cv, look, t, SaysFor(look, t), u);
    g_flip.p = 1;
    if (cv.items.empty()) return false;
    const float glowR = look.glow > 0 ? look.glowSize * look.size * k : 0;
    const int pad = (int)ceilf(glowR);
    const float margin = ceilf(4 * k) + pad;
    const int w = (int)ceilf(cv.r - cv.l + 2 * margin), h = (int)ceilf(cv.b - cv.t + 2 * margin);
    if (w <= 0 || h <= 0 || w >= 16384 || h >= 16384) return false;
    const float ox = margin - cv.l, oy = margin - cv.t;
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
    ID2D1StrokeStyle* round = nullptr;
    bool ok = false;
    BYTE r = 255, g = 255, b = 255;
    ParseColor(look.color, &r, &g, &b);
    if (bmp) {
        HGDIOBJ old = SelectObject(dc, bmp);
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        RECT rc{0, 0, w, h};
        D2D1_COLOR_F c = D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f);
        P.d2f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0,
                                 &round);
        if (SUCCEEDED(P.d2f->CreateDCRenderTarget(&rp, &rt)) && SUCCEEDED(rt->BindDC(dc, &rc)) &&
            SUCCEEDED(rt->CreateSolidColorBrush(&c, nullptr, &brush))) {
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt->SetTransform(D2D1::Matrix3x2F::Translation(ox, oy));
            for (const Item& i : cv.items) {
                if (i.clipped && i.squash < 0.01f) continue;  // a flap seen edge on
                brush->SetOpacity(i.a);
                if (i.clipped) {  // its clip folds with it
                    rt->SetTransform(D2D1::Matrix3x2F(1, 0, 0, i.squash, 0, i.hinge * (1 - i.squash)) * D2D1::Matrix3x2F::Translation(ox, oy));
                    rt->PushAxisAlignedClip(i.clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                }
                switch (i.kind) {
                    case Item::TEXT:
                        if (i.width > 0) {
                            ThickText thick;
                            thick.rt = rt;
                            thick.brush = brush;
                            thick.round = round;
                            thick.thick = i.width;
                            i.text->Draw(nullptr, &thick, i.p.x, i.p.y);
                        } else {
                            rt->DrawTextLayout(i.p, i.text, brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
                        }
                        break;
                    case Item::LINE: rt->DrawLine(i.p, i.q, brush, i.width, round); break;
                    case Item::CIRCLE:
                        if (i.width > 0) rt->DrawEllipse(D2D1::Ellipse(i.p, i.q.x, i.q.x), brush, i.width);
                        else rt->FillEllipse(D2D1::Ellipse(i.p, i.q.x, i.q.x), brush);
                        break;
                    case Item::RECT: {
                        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(i.p.x, i.p.y, i.q.x, i.q.y), i.radius, i.radius);
                        if (i.width > 0) rt->DrawRoundedRectangle(rr, brush, i.width);
                        else rt->FillRoundedRectangle(rr, brush);
                        break;
                    }
                    case Item::ARC:
                        if (ID2D1PathGeometry* arc = ArcGeometry(i)) {
                            rt->DrawGeometry(arc, brush, i.width, round);
                            arc->Release();
                        }
                        break;
                }
                if (i.clipped) {
                    rt->PopAxisAlignedClip();
                    rt->SetTransform(D2D1::Matrix3x2F::Translation(ox, oy));
                }
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
                AddGlow(out->px.data(), w, h, glowR, look.glow, D2D1::ColorF(gr / 255.f, gg / 255.f, gb / 255.f), std::clamp(look.opacity, 0.f, 1.f));
        }
        SelectObject(dc, old);
        DeleteObject(bmp);
    }
    Rel(round);
    Rel(brush);
    Rel(rt);
    DeleteDC(dc);
    return ok;
}

bool DateHasWords(int date) { return date <= 4 || (date >= 10 && date <= 12); }  // the month's name

}  // namespace

// ---------------------------------------------------------------------------------------

std::wstring Clock_StyleKey(const ClockLook& l) {
    wchar_t b[200];
    swprintf(b, 200, L"%ls|%.3f|%.3f|%.3f|%.2f|%ls|%d|%ls|%ls|%d|%ls|%d", l.color.c_str(), l.size, l.opacity, l.glow, l.glowSize,
             l.glowColor.c_str(), l.h24, l.style.c_str(), l.font.c_str(), l.date, l.lang.c_str(), l.nativeDigits);
    return b;
}

std::vector<ClockStyleInfo> Clock_Styles() {
    std::vector<ClockStyleInfo> list;
    const std::vector<ClockFontInfo> fonts = Clock_Fonts();
    auto mond = [&](const ClockFontInfo& f) {
        const bool first = !_wcsicmp(f.key.c_str(), kFonts[0].key);
        const wchar_t* about = first ? L"The Mond clock" : !f.getUrl && !f.removable ? L"Included" : !f.getUrl ? L"Added by you"
                             : f.ready ? L"Your copy" : L"Free for personal use only, so not included";
        list.push_back({L"", first ? std::wstring(L"Mond") : L"Mond \u00B7 " + f.key, about, f, true, true, true});
    };
    mond(fonts[0]);
    for (const Design& d : kDesigns) list.push_back({d.key, d.name, d.about, ClockFontInfo{L"", nullptr, false, true}, d.day, d.hours || d.digits, d.date});
    for (size_t i = 1; i < fonts.size(); i++) mond(fonts[i]);
    return list;
}

bool Clock_IsStyle(const ClockLook& l, const ClockStyleInfo& s) {
    const Design* d = FindDesign(l.style);
    if (!s.style.empty()) return d && !_wcsicmp(d->key, s.style.c_str());
    if (d) return false;
    const std::wstring shown = Clock_FontReady(l.font) ? l.font : std::wstring(kFonts[0].key);
    return !_wcsicmp(shown.c_str(), s.font.key.c_str());
}

void Clock_UseStyle(ClockLook& l, const ClockStyleInfo& s) {
    l.style = s.style;
    if (s.style.empty()) l.font = s.font.key;
}

std::wstring Clock_StyleName(const ClockLook& l) {
    if (const Design* d = FindDesign(l.style)) return d->name;
    const std::wstring shown = Clock_FontReady(l.font) ? l.font : std::wstring(kFonts[0].key);
    return !_wcsicmp(shown.c_str(), kFonts[0].key) ? std::wstring(L"Mond") : L"Mond \u00B7 " + shown;
}

bool Clock_UsesWords(const ClockLook& l) {
    const Design* d = FindDesign(l.style);
    if (!d) return true;
    return d->day || (d->hours && !l.h24) || (d->date && DateHasWords(l.date));
}

bool Clock_UsesDigits(const ClockLook& l) {
    const Design* d = FindDesign(l.style);
    return !d || d->digits || (d->date && l.date != 14);
}

bool Clock_UsesHours(const ClockLook& l) {
    const Design* d = FindDesign(l.style);
    return !d || d->hours;
}

bool Clock_Flips(const ClockLook& l) {
    const Design* d = FindDesign(l.style);
    return d && d->lay == Flip;
}

bool Clock_UsesDate(const ClockLook& l) {
    const Design* d = FindDesign(l.style);
    return !d || d->date;
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
bool Clock_Paint(const ClockLook& look, float k, const SYSTEMTIME& t, ClockImage* out, float flip) {
    const float size = look.size;
    if (!OpenPainter()) return false;
    if (const Design* d = FindDesign(look.style)) return PaintDesign(*d, look, k, t, out, flip);
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
    W.drawnStyle.clear();
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
