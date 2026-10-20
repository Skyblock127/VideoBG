// Desktop music player: the song playing and its place, previous / play-pause / next and the volume,
// on the desktop above the wallpaper, in one of a few styles: a record on a turntable, a cassette, a
// disc in a pill, bars that move with the sound. Like the clock it's a per-pixel transparent window
// owned by the desktop (so it stays behind the apps), but it takes clicks: its buttons work, and
// dragging it anywhere else moves it. Drawn with Direct2D into the layered window when what it shows
// changes, and about 30 times a second while something on it turns (only while the music plays and
// the desktop can be seen): then what stays still is drawn once and kept, and each frame only adds
// what turns. Drawn on the CPU (for a picture this small that's quicker than the GPU and reading it
// back). The settings windows draw the same styles as previews (Player_Paint).
#include "common.h"
#include <d2d1.h>
#include <dwrite.h>
#include <shobjidl.h>
#include <mmdeviceapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace {

constexpr wchar_t kClass[] = L"VideoBG.Player";
constexpr UINT_PTR TIMER_TURN = 1;
constexpr UINT kTurnMs = 33;
constexpr int kBars = 30;
constexpr float kPi = 3.14159265f;
enum Part { PART_NONE = PLAYER_NONE, PART_PREV = PLAYER_PREV, PART_PLAY = PLAYER_PLAY, PART_NEXT = PLAYER_NEXT, PART_VOLUME = PLAYER_VOLUME,
            PART_SEEK = PLAYER_SEEK, PART_BODY = PLAYER_BODY };

// IAudioMeterInformation (the sound playing, for the bars), declared here: the headers only name it.
struct AudioMeter : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 count, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};

template <class T> void Rel(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

double Now() {
    static double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart / freq;
}

// What a style shows.
struct Shown {
    std::wstring song, where;  // the song's name; its place ("1:23 / 3:45", or "Paused ...")
    bool playing = false;
    float done = 0;            // how far into the song (0..1)
    int volume = 0;
    D2D1_COLOR_F accent{};
    float turn = 0, turn2 = 0, slow = 0;  // degrees turned: the record (and a cassette's left reel), a cassette's right reel, a disc
    float arm = 0;             // the record's arm: 0 resting beside it, 1 on it
    const float* bars = nullptr;  // each bar's height (0..1)
    float level = 0;           // how loud the sound is (0..1, smoothed: the Wave's threads)
    float clock = 0;           // seconds the threads have moved
    int hot = PART_NONE, press = PART_NONE;
    bool sliding = false;
};

// Direct2D and DirectWrite, shared by the window and the previews.
struct Drawing {
    HMODULE d2dDll = nullptr, dwDll = nullptr;
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    ID2D1DCRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    ID2D1StrokeStyle* round = nullptr;
    IDWriteTextFormat *fTitle = nullptr, *fSmall = nullptr, *fBold = nullptr, *fIcon = nullptr, *fIconBig = nullptr, *fMark = nullptr;
    IDWriteTextFormat *fMicroLeft = nullptr, *fMicroRight = nullptr, *fDial = nullptr, *fStereo = nullptr, *fUnit = nullptr, *fTime = nullptr;
    ID2D1Bitmap* art = nullptr;  // the song's cover, for this render target
    ID2D1Bitmap* still = nullptr;  // the window's style without what turns (drawn when what it shows changes)
    int layer = 0;               // what a style draws: 0 all of it, 1 what stays still, 2 what turns
    float corners = -1;          // the card's corner radius the user chose (-1: the style's own)
} D;

bool StillPart() { return D.layer != 2; }
bool TurningPart() { return D.layer != 1; }

struct PlayerWindow {
    HWND hwnd = nullptr, desktop = nullptr;
    std::vector<std::wstring> songs;
    float scale = 1;
    int w = 0, h = 0;  // pixels
    int hot = PART_NONE, press = PART_NONE;
    bool dragging = false, sliding = false;
    POINT grab{};      // where the pointer took hold of it, in the window
    std::wstring drawn;  // what it shows now, to skip identical redraws
    std::wstring stillKey;  // what D.still shows
    HDC dc = nullptr;     // the picture it's drawn into (kept while it shows)
    HBITMAP dib = nullptr;
    HGDIOBJ oldBmp = nullptr;
    uint32_t* bits = nullptr;
    int dibW = 0, dibH = 0;
    bool turning = false;          // the timer runs
    double last = 0;               // when the turning parts last moved
    float turn = 0, turn2 = 0, slow = 0, arm = 0;
    float bars[kBars] = {};
    float level = 0, waveT = 0;
    bool seeking = false;          // the song's place is being dragged
    float seekFrac = -1;           // to there (shown there while held, and a moment after)
    double seekHold = 0;
    AudioMeter* meter = nullptr;  // the sound playing (for the bars)
    IMMDeviceEnumerator* outputs = nullptr;  // (kept with the meter: to see the output change)
    std::wstring meterFor;        // the output the meter listens to
    double meterChecked = 0;      // when that was last checked
    // The song's cover, as pixels (made into D.art when drawn)
    std::wstring artFor;
    std::vector<uint32_t> artPx;
    int artW = 0, artH = 0;
} W;

D2D1_COLOR_F Col(UINT32 rgb, float a = 1) {
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}

D2D1_COLOR_F Accent() {
    static D2D1_COLOR_F kept{};
    static ULONGLONG at = 0;
    if (at && GetTickCount64() - at < 2000) return kept;  // (it changes rarely; frames come 30 a second)
    at = GetTickCount64();
    BYTE pal[32] = {};
    DWORD size = sizeof pal;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                     RRF_RT_REG_BINARY, nullptr, pal, &size) == ERROR_SUCCESS && size >= 32)
        return kept = D2D1::ColorF(pal[4] / 255.f, pal[5] / 255.f, pal[6] / 255.f);
    return kept = Col(0x60CDFF);
}

D2D1_COLOR_F Lighter(D2D1_COLOR_F c, float by) {
    return D2D1::ColorF(std::min(1.f, c.r + by), std::min(1.f, c.g + by), std::min(1.f, c.b + by), c.a);
}

float Luma(D2D1_COLOR_F c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

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
    if (FAILED(D.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f)))
        return nullptr;
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetTextAlignment(align);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteInlineObject* dots = nullptr;
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    if (SUCCEEDED(D.dw->CreateEllipsisTrimmingSign(f, &dots))) {
        f->SetTrimming(&trim, dots);
        dots->Release();
    }
    return f;
}

bool StartDrawing() {
    if (D.rt) return true;
    if (!D.d2dDll) D.d2dDll = LoadLibraryExW(L"d2d1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!D.dwDll) D.dwDll = LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto d2dCreate = D.d2dDll ? (HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**))(void*)GetProcAddress(
                                    D.d2dDll, "D2D1CreateFactory")
                              : nullptr;
    auto dwCreate = D.dwDll ? (HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**))(void*)GetProcAddress(D.dwDll, "DWriteCreateFactory")
                            : nullptr;
    D2D1_FACTORY_OPTIONS fo{};
    if (!d2dCreate || !dwCreate || FAILED(d2dCreate(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &fo, (void**)&D.d2d)) ||
        FAILED(dwCreate(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&D.dw)))
        return false;
    const D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(D.d2d->CreateDCRenderTarget(&rp, &D.rt))) return false;
    D.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    D.rt->CreateSolidColorBrush(Col(0xFFFFFF), &D.br);
    D.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &D.round);
    D.fTitle = Format(L"Segoe UI Variable Display", 14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    D.fSmall = Format(L"Segoe UI Variable Text", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    D.fBold = Format(L"Segoe UI Variable Text", 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    D.fMark = Format(L"Segoe UI Variable Text", 11, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    D.fIcon = Format(L"Segoe Fluent Icons", 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    D.fIconBig = Format(L"Segoe Fluent Icons", 15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    D.fMicroLeft = Format(L"Segoe UI Variable Text", 7.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    D.fMicroRight = Format(L"Segoe UI Variable Text", 7.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    D.fDial = Format(L"Segoe UI Variable Display", 9.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    D.fStereo = Format(L"Segoe UI Variable Text", 6.8f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    D.fUnit = Format(L"Segoe UI Variable Text", 6.8f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    D.fTime = Format(L"Segoe UI Variable Text", 10.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    return D.br && D.round && D.fTitle && D.fSmall && D.fBold && D.fMark && D.fIcon && D.fIconBig &&
           D.fMicroLeft && D.fMicroRight && D.fDial && D.fStereo && D.fUnit && D.fTime;
}

void StopDrawing() {
    for (IDWriteTextFormat** f : {&D.fTitle, &D.fSmall, &D.fBold, &D.fMark, &D.fIcon, &D.fIconBig, &D.fMicroLeft, &D.fMicroRight, &D.fDial, &D.fStereo, &D.fUnit, &D.fTime}) Rel(*f);
    Rel(D.art);
    Rel(D.still);
    W.stillKey.clear();
    Rel(D.round);
    Rel(D.br);
    Rel(D.rt);
    Rel(D.dw);
    Rel(D.d2d);
    for (HMODULE* m : {&D.d2dDll, &D.dwDll}) {
        if (*m) FreeLibrary(*m);
        *m = nullptr;
    }
}

// ---- Drawing helpers (DIPs) --------------------------------------------------------------------

D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
D2D1_POINT_2F Pt(float x, float y) { return D2D1::Point2F(x, y); }

void Text(const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, D2D1_COLOR_F c) {
    if (s.empty() || !f) return;
    D.br->SetColor(c);
    D.rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, D.br, D2D1_DRAW_TEXT_OPTIONS_NONE);  // (not clipped: a g or y may hang below its box; widths trim with an ellipsis)
}

void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius) {
    D.br->SetColor(c);
    D.rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), D.br);
}

void Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius, float width = 1) {
    D.br->SetColor(c);
    D.rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), D.br, width);
}

void Circle(D2D1_POINT_2F p, float r, D2D1_COLOR_F c) {
    D.br->SetColor(c);
    D.rt->FillEllipse(D2D1::Ellipse(p, r, r), D.br);
}

void Ring(D2D1_POINT_2F p, float r, D2D1_COLOR_F c, float width) {
    D.br->SetColor(c);
    D.rt->DrawEllipse(D2D1::Ellipse(p, r, r), D.br, width);
}

void Ellipse(D2D1_POINT_2F p, float rx, float ry, D2D1_COLOR_F c) {
    D.br->SetColor(c);
    D.rt->FillEllipse(D2D1::Ellipse(p, rx, ry), D.br);
}

void EllipseRing(D2D1_POINT_2F p, float rx, float ry, D2D1_COLOR_F c, float width) {
    D.br->SetColor(c);
    D.rt->DrawEllipse(D2D1::Ellipse(p, rx, ry), D.br, width);
}

void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F c, float width) {
    D.br->SetColor(c);
    D.rt->DrawLine(a, b, D.br, width, D.round);
}

D2D1_POINT_2F Polar(D2D1_POINT_2F c, float r, float deg) {  // degrees clockwise from 12 o'clock
    return Pt(c.x + r * sinf(deg * kPi / 180), c.y - r * cosf(deg * kPi / 180));
}

void Arc(D2D1_POINT_2F c, float r, float from, float sweep, D2D1_COLOR_F col, float width) {
    if (sweep <= 0.5f) return;
    if (sweep >= 359.5f) {
        Ring(c, r, col, width);
        return;
    }
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(D.d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) {
        Rel(g);
        return;
    }
    sink->BeginFigure(Polar(c, r, from), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(Polar(c, r, from + sweep), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  sweep > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    D.br->SetColor(col);
    D.rt->DrawGeometry(g, D.br, width, D.round);
    g->Release();
}

D2D1_MATRIX_3X2_F Rotation(float deg) {  // (Matrix3x2F::Rotation needs d2d1.dll linked)
    const float c = cosf(deg * kPi / 180), s = sinf(deg * kPi / 180);
    return D2D1::Matrix3x2F(c, s, -s, c, 0, 0);
}

// The song's cover, made ready to draw (D.art); false if the song has none.
bool Art() {
    if (!D.art && !W.artPx.empty()) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        D.rt->CreateBitmap(D2D1::SizeU((UINT32)W.artW, (UINT32)W.artH), W.artPx.data(), (UINT32)W.artW * 4, bp, &D.art);
    }
    return D.art != nullptr;
}

// The song's cover in a circle of radius r at c, turned `deg` degrees; false if the song has none.
bool Cover(D2D1_POINT_2F c, float r, float deg) {
    if (!Art()) return false;
    const D2D1_SIZE_F sz = D.art->GetSize();
    const float side = std::min(sz.width, sz.height), s = 2 * r / side;
    ID2D1BitmapBrush* brush = nullptr;
    const D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Translation(-sz.width / 2, -sz.height / 2) * D2D1::Matrix3x2F::Scale(s, s) *
                                Rotation(deg) * D2D1::Matrix3x2F::Translation(c.x, c.y);
    if (FAILED(D.rt->CreateBitmapBrush(D.art, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                                       D2D1::BrushProperties(1, m), &brush)))
        return false;
    D.rt->FillEllipse(D2D1::Ellipse(c, r, r), brush);
    brush->Release();
    return true;
}

// The song's cover in a rounded rectangle; false if the song has none (and draws a note on accent).
bool CoverBox(const D2D1_RECT_F& pic, float radius, const Shown& s) {
    ID2D1RoundedRectangleGeometry* shape = nullptr;
    bool drawn = false;
    if (Art() && SUCCEEDED(D.d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(pic, radius, radius), &shape))) {
        const D2D1_SIZE_F sz = D.art->GetSize();
        const float side = std::min(sz.width, sz.height), k = (pic.right - pic.left) / side;
        ID2D1BitmapBrush* brush = nullptr;
        const D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Translation(-(sz.width - side) / 2, -(sz.height - side) / 2) * D2D1::Matrix3x2F::Scale(k, k) *
                                    D2D1::Matrix3x2F::Translation(pic.left, pic.top);
        if (SUCCEEDED(D.rt->CreateBitmapBrush(D.art, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                                              D2D1::BrushProperties(1, m), &brush))) {
            D.rt->FillGeometry(shape, brush);
            brush->Release();
            drawn = true;
        }
    }
    Rel(shape);
    if (!drawn) {
        Fill(pic, s.accent, radius);
        const float iconSz = std::max(12.f, (pic.bottom - pic.top) * 0.44f);
        IDWriteTextFormat* iconFmt = Format(L"Segoe Fluent Icons", iconSz, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(L"\uE8D6", pic, iconFmt, Luma(s.accent) > 0.55f ? Col(0x000000, 0.6f) : Col(0xFFFFFF, 0.85f));
        Rel(iconFmt);
    }
    Stroke(pic, Col(0xFFFFFF, 0.1f), radius);
    return drawn;
}

// The glass card most styles sit on.
void Card(float w, float h, float radius, float a = 0.84f) {
    const D2D1_RECT_F r = R(0.5f, 0.5f, w - 0.5f, h - 0.5f);
    if (D.corners >= 0) radius = std::min(D.corners, h / 2 - 0.5f);
    Fill(r, Col(0x141417, a), radius);
    Stroke(r, Col(0xFFFFFF, 0.1f), radius);
}

// ---- Styles ---------------------------------------------------------------------------------
// Settings::musicPlayer is 0 for none, else 1 + one of these. Each has its size, where its buttons
// and its volume slider are (for clicks), whether anything on it turns, and how it draws itself.

struct PlayerStyle {
    const wchar_t* name;
    const wchar_t* about;            // what it is, in a few words
    float w, h;                      // DIPs
    D2D1_POINT_2F prev, play, next;  // its buttons' centres
    float btn, playR;                // their sizes
    D2D1_RECT_F volume;              // its volume slider's track
    bool turns;                      // something on it turns while the music plays
    bool sound;                      // it moves with the sound
    D2D1_RECT_F turning;             // where (only that part is drawn again, each frame)
    void (*draw)(const struct PlayerStyle&, const Shown&);
    // Where the song's place is moved: the share of the song at x, y; -1 if that's not on it (held:
    // the pointer took hold of it, so anywhere counts). Each style shows the place one way only.
    float (*seek)(const struct PlayerStyle&, float x, float y, bool held);
};

// Previous, play / pause (lit), next.
void Buttons(const PlayerStyle& st, const Shown& s) {
    for (int p : {PART_PREV, PART_NEXT}) {
        const D2D1_POINT_2F c = p == PART_PREV ? st.prev : st.next;
        if (s.hot == p) Circle(c, st.btn, Col(0xFFFFFF, s.press == p ? 0.16f : 0.1f));
        Text(p == PART_PREV ? L"\uE892" : L"\uE893", R(c.x - st.btn, c.y - st.btn, c.x + st.btn, c.y + st.btn), D.fIcon, Col(0xFFFFFF, 0.9f));
    }
    const D2D1_POINT_2F pc = st.play;
    Circle(pc, st.playR, s.hot == PART_PLAY ? Lighter(s.accent, 0.06f) : s.accent);
    Text(s.playing ? L"\uE769" : L"\uE768", R(pc.x - st.playR, pc.y - st.playR, pc.x + st.playR, pc.y + st.playR), D.fIconBig,
         Luma(s.accent) > 0.55f ? Col(0x000000, 0.88f) : Col(0xFFFFFF, 0.95f));
}

// The volume (VideoBG's own, as in its settings): its icon, then the slider.
void Volume(const PlayerStyle& st, const Shown& s) {
    const D2D1_RECT_F& t = st.volume;
    if (t.right <= t.left) return;  // (it has none)
    const float cy = (t.top + t.bottom) / 2;
    Text(s.volume == 0 ? L"\uE74F" : s.volume < 34 ? L"\uE993" : s.volume < 67 ? L"\uE994" : L"\uE995", R(t.left - 26, cy - 12, t.left - 4, cy + 12),
         D.fIcon, Col(0xFFFFFF, 0.8f));
    Fill(t, Col(0xFFFFFF, 0.16f), 2);
    const float vx = t.left + (t.right - t.left) * s.volume / 100.f;
    Fill(R(t.left, t.top, vx, t.bottom), s.accent, 2);
    Circle(Pt(vx, cy), s.hot == PART_VOLUME || s.sliding ? 6.5f : 5.5f, Col(0xFFFFFF));
}

void Progress(const D2D1_RECT_F& line, const Shown& s) {
    Fill(line, Col(0xFFFFFF, 0.14f), 1.25f);
    if (s.done > 0) Fill(R(line.left, line.top, line.left + (line.right - line.left) * s.done, line.bottom), s.accent, 1.25f);
}

// Vinyl: a record on a turntable, turning while the song plays; its arm rests beside it when paused
// and, playing, sits in the groove, moving in as the song goes. Beside it, the song and its controls.
constexpr D2D1_POINT_2F kVinylC{60, 60}, kVinylPivot{116, 16};
constexpr float kVinylR = 48, kArmLen = 76;

D2D1_POINT_2F ArmAt(float deg) {  // the arm's tip, pointing `deg` degrees from the pivot (0 = right, clockwise)
    return Pt(kVinylPivot.x + kArmLen * cosf(deg * kPi / 180), kVinylPivot.y + kArmLen * sinf(deg * kPi / 180));
}

float ArmOnRecord(float groove) {  // the arm's angle that puts its tip `groove` from the record's middle
    float lo = 100, hi = 140;
    for (int i = 0; i < 24; i++) {
        const float mid = (lo + hi) / 2;
        const D2D1_POINT_2F p = ArmAt(mid);
        if (hypotf(p.x - kVinylC.x, p.y - kVinylC.y) > groove) lo = mid;
        else hi = mid;
    }
    return (lo + hi) / 2;
}

void DrawVinyl(const PlayerStyle& st, const Shown& s) {
    const D2D1_POINT_2F c = kVinylC;
    if (StillPart()) {
        Card(st.w, st.h, 20);
        Circle(c, kVinylR, Col(0x0B0B0D, 0.98f));
        Ring(c, kVinylR, Col(0xFFFFFF, 0.12f), 1);
        for (int i = 0; i < 9; i++) Ring(c, 44 - i * 2.4f, Col(0xFFFFFF, i % 3 ? 0.035f : 0.07f), 0.8f);
        Arc(c, 36, 300, 32, Col(0xFFFFFF, 0.16f), 1.8f);  // light on the grooves (it stays: the record turns under it)
        Arc(c, 30, 120, 28, Col(0xFFFFFF, 0.1f), 1.8f);
        // The song, its place, the buttons and the volume.
        const float x = 140;
        Text(s.song, R(x, 15, st.w - 18, 37), D.fTitle, Col(0xFFFFFF, 0.95f));
        Text(s.where, R(x, 37, st.w - 18, 53), D.fSmall, Col(0xFFFFFF, 0.6f));
        Buttons(st, s);
        Volume(st, s);
    }
    if (!TurningPart()) return;
    // The label (the song's cover, if it has one) turns.
    if (!Cover(c, 18, s.turn)) {
        Circle(c, 18, s.accent);
        const D2D1_COLOR_F mark = Luma(s.accent) > 0.55f ? Col(0x000000, 0.45f) : Col(0xFFFFFF, 0.7f);
        Arc(c, 12, s.turn, 70, mark, 2.f);
        Circle(Polar(c, 7, s.turn + 200), 1.8f, mark);
    }
    Circle(c, 2.8f, Col(0x141417));
    // The arm: from its pivot at the corner, onto the groove (further in as the song goes).
    const float rest = 100, on = ArmOnRecord(44 - 20 * s.done);
    const float deg = rest + (on - rest) * s.arm;
    const D2D1_POINT_2F tip = ArmAt(deg), back = ArmAt(deg + 180);
    const D2D1_POINT_2F weight = Pt(kVinylPivot.x + (back.x - kVinylPivot.x) * 0.16f, kVinylPivot.y + (back.y - kVinylPivot.y) * 0.16f);
    Line(kVinylPivot, weight, Col(0xFFFFFF, 0.55f), 6);
    Line(kVinylPivot, tip, Col(0xFFFFFF, 0.85f), 2.8f);
    const float head = (deg + 28) * kPi / 180;
    Line(tip, Pt(tip.x + 9 * cosf(head), tip.y + 9 * sinf(head)), Col(0xFFFFFF, 0.92f), 5.2f);
    Circle(kVinylPivot, 8, Col(0x2A2A2F));
    Ring(kVinylPivot, 8, Col(0xFFFFFF, 0.2f), 1);
    Circle(kVinylPivot, 3, Col(0xFFFFFF, 0.7f));
}

// Cassette: a tape whose reels turn while the song plays, the tape winding from one reel to the
// other as it goes; the song on its label; the controls on a strip under it.
void Screw(D2D1_POINT_2F p, float r) {
    Circle(p, r, Col(0x1B232A));
    Ring(p, r, Col(0x45525C), 0.8f);
    Line(Pt(p.x - r * 0.55f, p.y - r * 0.55f), Pt(p.x + r * 0.55f, p.y + r * 0.55f), Col(0x080D11), 1.2f);
    Line(Pt(p.x - r * 0.55f, p.y + r * 0.55f), Pt(p.x + r * 0.55f, p.y - r * 0.55f), Col(0x080D11), 1.2f);
}

void CassetteBody(const PlayerStyle& st, const Shown& s, const D2D1_RECT_F& win);

void DrawCassette(const PlayerStyle& st, const Shown& s) {
    const D2D1_RECT_F win = R(14, 14, 218, 106);
    const D2D1_POINT_2F lc = Pt(54, 58), rc = Pt(178, 58);
    if (StillPart()) CassetteBody(st, s, win);
    if (!TurningPart()) return;

    // Moving parts 1: Cassette tape reels and strand inside the window
    const float lr = 11 + 18 * (1 - s.done), rr = 11 + 18 * s.done;
    D.rt->PushAxisAlignedClip(R(win.left + 1, win.top + 1, win.right - 1, win.bottom - 1), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const D2D1_COLOR_F tapeCol = Col(0x35261F);
    Circle(lc, lr, tapeCol);
    Circle(rc, rr, tapeCol);
    if (lr > 16) Ring(lc, lr - 4, Col(0x4C372E, 0.45f), 0.8f);
    if (rr > 16) Ring(rc, rr - 4, Col(0x4C372E, 0.45f), 0.8f);
    // Tape feeding from left (supply) to right (take-up) along the bottom
    Line(Pt(lc.x, lc.y + lr), Pt(rc.x, rc.y + rr), Col(0x3F2E26), 2.2f);
    D.rt->PopAxisAlignedClip();

    // Stamped metal hubs - rotating COUNTER-CLOCKWISE (-deg) so bottom moves rightward with the tape
    for (int k = 0; k < 2; k++) {
        const D2D1_POINT_2F c = k ? rc : lc;
        const float deg = k ? -s.turn2 : -s.turn;
        Circle(c, 11.5f, Col(0xD0D6DA));
        Ring(c, 11.5f, Col(0x75818A), 1);
        for (int i = 0; i < 6; i++) Line(Polar(c, 4.f, deg + i * 60), Polar(c, 10.f, deg + i * 60), Col(0x11171D), 2.2f);
        Circle(c, 5.2f, Col(0x1B242C));
        Circle(c, 3.f, Col(0x06090D));
    }

    // Moving parts 2: Equalizer bars moving with the sound
    const float ew = 4.5f, egap = 4.2f, ex0 = 276, eyBottom = 78, eMaxH = 32;
    for (int i = 0; i < 11; i++) {
        const float x = ex0 + i * (ew + egap);
        const float v = (s.playing && s.bars) ? std::clamp(s.bars[i * 2], 0.06f, 1.f) : 0.06f;
        const float h = std::clamp(v * eMaxH, 2.5f, eMaxH);
        Fill(R(x, eyBottom - h, x + ew, eyBottom), s.accent, 2);
    }
}

// The cassette's case, displays and controls (what stays still).
void CassetteBody(const PlayerStyle& st, const Shown& s, const D2D1_RECT_F& win) {
    // 1. Main Boombox chassis
    Fill(R(0.5f, 0.5f, st.w - 0.5f, st.h - 0.5f), Col(0x141B22, 0.96f), 18);
    Stroke(R(0.5f, 0.5f, st.w - 0.5f, st.h - 0.5f), Col(0x364552), 18, 1.2f);
    // Outer corner screws
    Screw(Pt(11, 11), 3.2f);
    Screw(Pt(st.w - 11, 11), 3.2f);
    Screw(Pt(11, st.h - 11), 3.2f);
    Screw(Pt(st.w - 11, st.h - 11), 3.2f);

    // 2. Left side: The Cassette Window
    Fill(win, Col(0x070B0E, 0.95f), 12);
    Stroke(win, Col(0x3B4854), 12, 1.5f);
    Stroke(R(win.left + 2, win.top + 2, win.right - 2, win.bottom - 2), Col(0xFFFFFF, 0.08f), 10, 0.8f);
    // 4 window screws
    Screw(Pt(win.left + 8, win.top + 8), 2.4f);
    Screw(Pt(win.right - 8, win.top + 8), 2.4f);
    Screw(Pt(win.left + 8, win.bottom - 8), 2.4f);
    Screw(Pt(win.right - 8, win.bottom - 8), 2.4f);

    // Time readout guide marks & text inside the tape window (centered between reels at x=116)
    Line(Pt(80, 50), Pt(80, 66), Col(0x354049), 1.5f);
    Line(Pt(152, 50), Pt(152, 66), Col(0x354049), 1.5f);
    Text(s.where, R(81, 49, 151, 67), D.fMark, Col(0xC4CDD2, 0.9f));

    // Bottom molded trapezoid detail & sprocket holes
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(D.d2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sink))) {
        sink->BeginFigure(Pt(54, 136), D2D1_FIGURE_BEGIN_FILLED);
        const D2D1_POINT_2F pts[3] = {Pt(68, 112), Pt(164, 112), Pt(178, 136)};
        sink->AddLines(pts, 3);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();
        D.br->SetColor(Col(0x10171F));
        D.rt->FillGeometry(g, D.br);
        D.br->SetColor(Col(0x323E48));
        D.rt->DrawGeometry(g, D.br, 1.2f);
    }
    Rel(g);
    Circle(Pt(80, 124), 4.2f, Col(0x05080B));
    Ring(Pt(80, 124), 4.2f, Col(0x323E48), 1);
    Circle(Pt(152, 124), 4.2f, Col(0x05080B));
    Ring(Pt(152, 124), 4.2f, Col(0x323E48), 1);

    // 3. Right side: Top LCD Song Title Display
    const D2D1_RECT_F disp = R(230, 14, 368, 38);
    Fill(disp, s.accent, 6);
    Stroke(disp, Col(0x0B2330), 6, 1);
    const float plateMidY = (disp.top + disp.bottom) / 2;
    Screw(Pt(disp.left + 8, plateMidY), 2.2f);
    Screw(Pt(disp.right - 8, plateMidY), 2.2f);
    const D2D1_COLOR_F ink = Luma(s.accent) > 0.55f ? Col(0x000000, 0.88f) : Col(0xFFFFFF, 0.95f);
    Text(s.song, R(disp.left + 15, disp.top, disp.right - 15, disp.bottom), D.fBold, ink);

    // 4. Right side: Middle Cover Art & Equalizer Slots
    const D2D1_RECT_F art = R(230, 44, 268, 78);
    Fill(art, Col(0x0E161D), 6);
    Stroke(art, Col(0x34424D), 6, 1);
    if (!Cover(Pt(249, 61), 15, 0)) {
        IDWriteTextFormat* iconFmt = Format(L"Segoe Fluent Icons", 18, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(L"\uE8D6", art, iconFmt, Col(0xFFFFFF, 0.4f));
        Rel(iconFmt);
    }
    // Equalizer slots background
    const float ew = 4.5f, egap = 4.2f, ex0 = 276, eyBottom = 78, eMaxH = 32;
    for (int i = 0; i < 11; i++) {
        const float x = ex0 + i * (ew + egap);
        Fill(R(x, eyBottom - eMaxH, x + ew, eyBottom), Col(0x070B0E), 2);
        Stroke(R(x, eyBottom - eMaxH, x + ew, eyBottom), Col(0x27333C), 2, 0.8f);
    }

    // 5. Right side: Buttons & Volume
    Buttons(st, s);
    Volume(st, s);
}

// Disc: a pill, the song's cover (or a disc) turning slowly in a ring that fills as the song goes.
void DrawDisc(const PlayerStyle& st, const Shown& s) {
    const D2D1_POINT_2F c = Pt(42, 42);
    if (StillPart()) {
        Card(st.w, st.h, st.h / 2 - 0.5f, 0.86f);
        Ring(c, 35, Col(0xFFFFFF, 0.14f), 3);
        Arc(c, 35, 0, s.done * 360, s.accent, 3.2f);
        Text(s.song, R(86, 12, st.w - 140, 34), D.fTitle, Col(0xFFFFFF, 0.95f));
        Text(s.where, R(86, 32, st.w - 140, 48), D.fSmall, Col(0xFFFFFF, 0.6f));
        Buttons(st, s);
        Volume(st, s);
    }
    if (!TurningPart()) return;
    if (!Cover(c, 29, s.slow)) {
        Circle(c, 29, Col(0x26262B));
        Ring(c, 21, Col(0xFFFFFF, 0.08f), 1);
        Ring(c, 15, Col(0xFFFFFF, 0.06f), 1);
        Arc(c, 25, s.slow, 50, Col(0xFFFFFF, 0.22f), 2);
        Arc(c, 25, s.slow + 180, 50, Col(0xFFFFFF, 0.12f), 2);
        Circle(c, 9, s.accent);
    }
    Circle(c, 2.6f, Col(0x141417));
}

// Bars: the song and its controls over bars that move with the sound.
void DrawBars(const PlayerStyle& st, const Shown& s) {
    if (StillPart()) {
        Card(st.w, st.h, 20);
        CoverBox(R(18, 14, 58, 54), 13, s);
        Text(s.song, R(68, 14, st.w - 138, 34), D.fTitle, Col(0xFFFFFF, 0.95f));
        Text(s.where, R(68, 36, st.w - 138, 54), D.fSmall, Col(0xFFFFFF, 0.6f));
        Buttons(st, s);
        Volume(st, s);
    }
    if (!TurningPart()) return;
    const float left = 18, right = st.w - 18, base = 106, maxHgt = 32.f, step = (right - left) / kBars, bw = step - 3;
    for (int i = 0; i < kBars; i++) {
        const float raw = s.bars ? std::clamp(s.bars[i], 0.f, 1.f) : 0;
        const float v = raw < 0.6f ? raw : 0.6f + 0.3f * tanhf((raw - 0.6f) / 0.4f);
        const float hgt = std::clamp(3.f + (maxHgt - 3.f) * v, 3.f, maxHgt);
        const float x = left + i * step + 1.5f;
        Fill(R(x, base - hgt, x + bw, base), i < s.done * kBars ? s.accent : Col(0xFFFFFF, 0.3f), std::min(2.f, bw / 2));
    }
}

// Mini: a small pill, just the cover, the song and its buttons; the song's place along its edge.
void DrawMini(const PlayerStyle& st, const Shown& s) {
    const D2D1_POINT_2F c = Pt(28, 28);
    Card(st.w, st.h, st.h / 2 - 0.5f, 0.86f);
    if (!Cover(c, 19, 0)) {
        Circle(c, 19, s.accent);
        Text(L"\uE8D6", R(c.x - 19, c.y - 19, c.x + 19, c.y + 19), D.fIcon, Luma(s.accent) > 0.55f ? Col(0x000000, 0.75f) : Col(0xFFFFFF, 0.9f));
    }
    Text(s.song, R(56, 8, st.prev.x - st.btn - 4, 30), D.fTitle, Col(0xFFFFFF, 0.95f));
    Text(s.where, R(56, 28, st.volume.left - 28, 46), D.fSmall, Col(0xFFFFFF, 0.6f));
    Volume(st, s);
    Buttons(st, s);
    D.rt->PushAxisAlignedClip(R(28, st.h - 4, st.w - 28, st.h - 1), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    Progress(R(28, st.h - 4, st.w - 28, st.h - 1.5f), s);
    D.rt->PopAxisAlignedClip();
}

// Cover: the song's cover, square and big, beside the song and its controls.
void DrawCover(const PlayerStyle& st, const Shown& s) {
    Card(st.w, st.h, 22);
    CoverBox(R(15, 15, 135, 135), 12, s);
    const float x = 154;
    Text(s.song, R(x, 18, st.w - 18, 40), D.fTitle, Col(0xFFFFFF, 0.95f));
    Text(s.where, R(x, 40, st.w - 18, 56), D.fSmall, Col(0xFFFFFF, 0.6f));
    Progress(R(x, 66, st.w - 18, 68.5f), s);
    Buttons(st, s);
    Volume(st, s);
}

// Halo: the song's cover turning slowly in a ring of bars that move with the sound; beside it, the
// song and its controls.
constexpr D2D1_POINT_2F kHaloC{60, 60};

void DrawHalo(const PlayerStyle& st, const Shown& s) {
    const D2D1_POINT_2F c = kHaloC;
    if (StillPart()) {
        Card(st.w, st.h, 20);
        const float x = 132;
        Text(s.song, R(x, 15, st.w - 18, 37), D.fTitle, Col(0xFFFFFF, 0.95f));
        Text(s.where, R(x, 37, st.w - 18, 53), D.fSmall, Col(0xFFFFFF, 0.6f));
        Buttons(st, s);
        Volume(st, s);
    }
    if (!TurningPart()) return;
    for (int i = 0; i < kBars; i++) {  // the bars, all round (the played part lit)
        const float raw = s.bars ? std::clamp(s.bars[i], 0.f, 1.f) : 0;
        const float v = raw < 0.6f ? raw : 0.6f + 0.3f * tanhf((raw - 0.6f) / 0.4f);
        const float deg = i * 360.f / kBars;
        Line(Polar(c, 32, deg), Polar(c, 34 + 16 * v, deg), i < s.done * kBars ? s.accent : Col(0xFFFFFF, 0.38f), 3.2f);
    }
    if (!Cover(c, 26, s.slow)) {
        Circle(c, 26, Col(0x26262B));
        Ring(c, 20, Col(0xFFFFFF, 0.08f), 1);
        Arc(c, 23, s.slow, 50, Col(0xFFFFFF, 0.22f), 2);
        Arc(c, 23, s.slow + 180, 50, Col(0xFFFFFF, 0.12f), 2);
        Circle(c, 9, s.accent);
    }
    Ring(c, 26, Col(0xFFFFFF, 0.14f), 1);
    Circle(c, 2.5f, Col(0x141417));
}

// Wave: the song and its controls over threads of light that swing with the sound.
void DrawWave(const PlayerStyle& st, const Shown& s) {
    if (StillPart()) {
        Card(st.w, st.h, 20);
        CoverBox(R(18, 14, 58, 54), 13, s);
        Text(s.song, R(68, 14, st.w - 138, 34), D.fTitle, Col(0xFFFFFF, 0.95f));
        Text(s.where, R(68, 36, st.w - 138, 54), D.fSmall, Col(0xFFFFFF, 0.6f));
        Buttons(st, s);
        Volume(st, s);
    }
    if (!TurningPart()) return;
    static const UINT32 kBlue[] = {0x0288D1, 0x29B6F6, 0x1E88E5, 0x81D4FA, 0x4FC3F7};
    static const float kWaves[] = {1.8f, 2.6f, 1.4f, 3.4f, 2.9f};      // across the player
    static const float kSpeed[] = {7.5f, -6.2f, 5.0f, 9.8f, -8.4f};    // radians a second (fast!)
    static const float kShare[] = {1.0f, 0.82f, 0.68f, 0.52f, 0.40f};  // of the swing
    static const float kWidth[] = {2.2f, 1.7f, 1.9f, 1.3f, 1.5f};
    const float left = 18, right = st.w - 18, mid = 86, amp = 14.f, split = left + (right - left) * s.done;
    const float jump = std::max(0.12f, std::clamp(s.level * 1.1f, 0.f, 1.0f));
    for (int k = 0; k < 5; k++) {
        ID2D1PathGeometry* g = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(D.d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) {
            Rel(g);
            return;
        }
        const float swing = std::min(13.5f, amp * jump * kShare[k] * (0.8f + 0.2f * sinf(s.clock * 3.2f + k * 1.5f)));
        for (float x = left; x <= right + 0.01f; x += 2.f) {
            const float u = (x - left) / (right - left), e = 4 * u - 2;  // e: -2..2
            const float fade = 4 / ((2 + e * e * e * e) * (2 + e * e * e * e));  // 1 in the middle, 0 at the ends
            const D2D1_POINT_2F p = Pt(x, mid + swing * fade * sinf(2 * kPi * kWaves[k] * u + kSpeed[k] * s.clock + k * 1.1f));
            if (x == left) sink->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW);
            else sink->AddLine(p);
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        sink->Release();
        for (int part = 0; part < 2; part++) {
            D.rt->PushAxisAlignedClip(part ? R(split, 68, right + 4, 104) : R(left - 4, 68, split, 104),
                                      D2D1_ANTIALIAS_MODE_ALIASED);
            D.br->SetColor(Col(kBlue[k], part ? 0.3f : 0.95f));
            D.rt->DrawGeometry(g, D.br, kWidth[k], D.round);
            D.rt->PopAxisAlignedClip();
        }
        g->Release();
    }
}

// Radio: a vintage transistor radio with tuner scale, telescopic antenna, carry handle, and album art.
void DrawRadio(const PlayerStyle&, const Shown& s) {
    // 1. Telescopic antenna tucked behind the body
    Line(Pt(329.4f, 41.6f), Pt(359.4f, 4.0f), Col(0x0B344E, 0.6f), 5.6f);
    Line(Pt(329.4f, 40.8f), Pt(358.6f, 4.0f), Col(0xA0B0BA), 3.6f);
    Line(Pt(331.0f, 38.8f), Pt(358.6f, 4.0f), Col(0xF3FBFF, 0.7f), 0.8f);
    Line(Pt(342.2f, 24.8f), Pt(345.4f, 27.2f), Col(0x526A79), 0.8f);
    Line(Pt(351.4f, 13.2f), Pt(354.6f, 15.6f), Col(0x526A79), 0.8f);
    Circle(Pt(359.4f, 3.6f), 2.2f, Col(0xDCE5EA));
    Ring(Pt(359.4f, 3.6f), 2.2f, Col(0x334551), 0.6f);
    Ellipse(Pt(328.6f, 42.0f), 6.8f, 3.2f, Col(0x111A22));
    EllipseRing(Pt(328.6f, 42.0f), 6.8f, 3.2f, Col(0x587080), 0.8f);
    Ellipse(Pt(328.6f, 41.2f), 4.0f, 1.6f, Col(0xA0B0BA));

    // 2. Folding carry handle
    auto drawHandlePath = [&](D2D1_POINT_2F start, float yTop, float xEnd, float rCorner, D2D1_COLOR_F col, float width) {
        ID2D1PathGeometry* g = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(D.d2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sink))) {
            sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(Pt(start.x, yTop + rCorner));
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(start.x, yTop), Pt(start.x + rCorner, yTop)));
            sink->AddLine(Pt(xEnd - rCorner, yTop));
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(xEnd, yTop), Pt(xEnd, yTop + rCorner)));
            sink->AddLine(Pt(xEnd, start.y));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            D.br->SetColor(col);
            D.rt->DrawGeometry(g, D.br, width, D.round);
            g->Release();
        }
    };
    drawHandlePath(Pt(97.0f, 36.0f), 14.8f, 190.6f, 12.4f, Col(0x0F1820), 9.2f);
    drawHandlePath(Pt(97.0f, 34.4f), 16.8f, 188.6f, 12.4f, Col(0xA0B0BA), 4.0f);
    drawHandlePath(Pt(99.8f, 33.6f), 19.6f, 185.8f, 10.0f, Col(0x243440), 2.0f);
    Fill(R(91.0f, 30.8f, 103.4f, 38.4f), Col(0xA0B0BA), 2.4f);
    Stroke(R(91.0f, 30.8f, 103.4f, 38.4f), Col(0x111B24), 2.4f, 1.2f);
    Fill(R(184.2f, 30.8f, 196.6f, 38.4f), Col(0xA0B0BA), 2.4f);
    Stroke(R(184.2f, 30.8f, 196.6f, 38.4f), Col(0x111B24), 2.4f, 1.2f);

    // 3. Main radio enclosure
    Fill(R(1.0f, 30.4f, 373.0f, 176.4f), Col(0x1B6D9C), 17.2f);
    Stroke(R(1.0f, 30.4f, 373.0f, 176.4f), Col(0x15354B), 17.2f, 0.8f);
    Stroke(R(1.5f, 30.9f, 372.5f, 175.9f), Col(0x75D8FF, 0.45f), 16.8f, 0.6f);
    Fill(R(3.4f, 32.8f, 370.6f, 174.0f), Col(0x18222B), 15.6f);
    Stroke(R(3.4f, 32.8f, 370.6f, 174.0f), Col(0x586773, 0.6f), 15.6f, 0.6f);

    // Inner top highlight curve
    ID2D1PathGeometry* hg = nullptr;
    ID2D1GeometrySink* hsink = nullptr;
    if (SUCCEEDED(D.d2d->CreatePathGeometry(&hg)) && SUCCEEDED(hg->Open(&hsink))) {
        hsink->BeginFigure(Pt(19.4f, 38.0f), D2D1_FIGURE_BEGIN_HOLLOW);
        hsink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(19.4f, 36.0f), Pt(25.4f, 36.0f)));
        hsink->AddLine(Pt(348.2f, 36.0f));
        hsink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(360.6f, 36.0f), Pt(365.4f, 46.8f)));
        hsink->EndFigure(D2D1_FIGURE_END_OPEN);
        hsink->Close();
        hsink->Release();
        D.br->SetColor(Col(0x8795A0, 0.28f));
        D.rt->DrawGeometry(hg, D.br, 0.6f, D.round);
        hg->Release();
    }

    // 4 Corner screws
    for (D2D1_POINT_2F sc : {Pt(14.2f, 44.0f), Pt(359.8f, 44.0f), Pt(14.2f, 162.8f), Pt(359.8f, 162.8f)}) {
        Circle(sc, 3.2f, Col(0x101820));
        Ring(sc, 3.2f, Col(0x697985), 0.6f);
        Line(Pt(sc.x - 1.6f, sc.y), Pt(sc.x + 1.6f, sc.y), Col(0xA4B2BC, 0.75f), 0.6f);
        Line(Pt(sc.x, sc.y - 1.6f), Pt(sc.x, sc.y + 1.6f), Col(0xA4B2BC, 0.75f), 0.6f);
    }

    // 4. Large circular artwork & speaker block
    Fill(R(17.4f, 44.4f, 131.4f, 162.0f), Col(0x101820, 0.72f), 10.0f);
    Fill(R(19.4f, 46.4f, 129.4f, 160.0f), Col(0x212B34), 8.8f);
    Stroke(R(19.4f, 46.4f, 129.4f, 160.0f), Col(0x344450), 8.8f, 0.6f);

    const D2D1_POINT_2F spkC = Pt(75.4f, 96.0f);
    Circle(spkC, 41.2f, Col(0x0C131A));
    Ring(spkC, 41.2f, Col(0x586B79), 0.8f);
    Circle(spkC, 38.4f, Col(0xA0B0BA, 0.55f));
    Circle(spkC, 36.4f, Col(0x101820));
    Ring(spkC, 36.4f, Col(0x080D12), 2.0f);

    // Album art in the large circle on the left (with vintage speaker fallback)
    if (!Cover(spkC, 35.0f, 0)) {
        Circle(spkC, 34.4f, Col(0x18222B));
        Ring(spkC, 34.4f, Col(0x35434E), 0.6f);
        for (int row = 0; row < 12; row++) {
            const float gy = 70.0f + row * 4.8f;
            const float xStart = (row % 2 == 0) ? 49.4f : 47.0f;
            for (float gx = xStart; gx <= 103.0f; gx += 4.8f) {
                const float d = hypotf(gx - spkC.x, gy - spkC.y);
                if (d <= 28.8f) {
                    Circle(Pt(gx, gy), d <= 18.0f ? 0.72f : 0.62f, Col(0x8BA0AD, d <= 22.0f ? 0.52f : 0.35f));
                }
            }
        }
        Ring(spkC, 30.8f, Col(0x738895, 0.16f), 0.4f);
    } else {
        Ring(spkC, 35.0f, Col(0xFFFFFF, 0.2f), 1.0f);
    }

    // Lower speaker panel hardware knobs
    Line(Pt(27.4f, 132.4f), Pt(121.4f, 132.4f), Col(0x4D5D68, 0.7f), 0.4f);

    auto drawKnob = [&](D2D1_POINT_2F kc) {
        Circle(kc, 8.0f, Col(0x0B1117));
        Ring(kc, 8.0f, Col(0x5C6D79), 0.8f);
        Circle(kc, 6.6f, Col(0x28333D));
        Ring(kc, 6.6f, Col(0x7E909B), 0.4f);
        Ring(kc, 4.8f, Col(0x596A76), 0.4f);
        Line(Pt(kc.x, kc.y - 4.8f), Pt(kc.x, kc.y - 1.6f), Col(0x7DE5FF), 1.1f);
        Circle(kc, 1.0f, Col(0xA6B6BF));
    };
    drawKnob(Pt(48.6f, 148.0f));
    drawKnob(Pt(100.2f, 148.0f));

    // Power indicator LED
    Circle(Pt(75.4f, 148.0f), 1.4f, Col(0x4ACFFF));
    Ring(Pt(75.4f, 148.0f), 2.6f, Col(0x4ACFFF, 0.35f), 0.8f);

    // 5. FM Tuner
    Text(L"FM / AM DIGITAL TUNER", R(143.0f, 41.0f, 287.0f, 49.0f), D.fMicroLeft, Col(0x9BABB6));
    Circle(Pt(301.4f, 41.6f), 1.2f, Col(0x52D5FF));
    Ring(Pt(301.4f, 41.6f), 2.2f, Col(0x52D5FF, 0.35f), 0.6f);
    Text(L"STEREO", R(302.5f, 37.5f, 333.5f, 45.5f), D.fStereo, Col(0xA7B6C0));
    Circle(Pt(334.6f, 41.6f), 1.2f, Col(0x7CE0B2));

    Fill(R(137.0f, 50.0f, 356.2f, 93.2f), Col(0x070C11), 6.8f);
    Stroke(R(137.0f, 50.0f, 356.2f, 93.2f), Col(0x0C1116), 6.8f, 1.6f);
    Fill(R(139.0f, 52.0f, 354.2f, 91.2f), Col(0x0C1218), 5.2f);
    Stroke(R(139.0f, 52.0f, 354.2f, 91.2f), Col(0x35434C), 5.2f, 0.6f);
    Line(Pt(143.4f, 54.4f), Pt(349.8f, 54.4f), Col(0x7F8D97, 0.17f), 0.4f);

    static const wchar_t* const kFrequencies[] = {L"88", L"92", L"96", L"100", L"104", L"108"};
    for (int i = 0; i < 6; i++) {
        const float fx = 147.4f + i * 37.2f;
        Text(kFrequencies[i], R(fx - 14.f, 60.0f, fx + 14.f, 69.0f), D.fDial, Col(0xE1EDF4, 0.68f));
    }

    for (int i = 0; i <= 100; i++) {
        const float tx = 147.8f + (347.4f - 147.8f) * i / 100.f;
        const float yTop = (i % 20 == 0) ? 69.2f : (i % 5 == 0) ? 71.6f : 74.8f;
        const float w = (i % 20 == 0) ? 0.7f : (i % 5 == 0) ? 0.5f : 0.4f;
        const D2D1_COLOR_F c = (i % 20 == 0) ? Col(0xADB9C1, 0.85f) : (i % 5 == 0) ? Col(0x9AA9B3, 0.65f) : Col(0x7E8E98, 0.48f);
        Line(Pt(tx, yTop), Pt(tx, 82.4f), c, w);
    }

    // Tuning needle at s.done
    const float nx = 147.8f + (347.4f - 147.8f) * s.done;
    ID2D1PathGeometry* fg = nullptr;
    ID2D1GeometrySink* fsink = nullptr;
    if (SUCCEEDED(D.d2d->CreatePathGeometry(&fg)) && SUCCEEDED(fg->Open(&fsink))) {
        fsink->BeginFigure(Pt(nx, 59.6f), D2D1_FIGURE_BEGIN_FILLED);
        const D2D1_POINT_2F pts[2] = {Pt(nx - 4.0f, 66.4f), Pt(nx + 4.0f, 66.4f)};
        fsink->AddLines(pts, 2);
        fsink->EndFigure(D2D1_FIGURE_END_CLOSED);
        fsink->Close();
        fsink->Release();
        D.br->SetColor(Col(0x3BCAFA, 0.24f));
        D.rt->FillGeometry(fg, D.br);
        fg->Release();
    }
    Line(Pt(nx, 54.0f), Pt(nx, 82.8f), Col(0x41D3FF), 1.2f);
    ID2D1PathGeometry* tg = nullptr;
    ID2D1GeometrySink* tsink = nullptr;
    if (SUCCEEDED(D.d2d->CreatePathGeometry(&tg)) && SUCCEEDED(tg->Open(&tsink))) {
        tsink->BeginFigure(Pt(nx - 2.0f, 54.0f), D2D1_FIGURE_BEGIN_FILLED);
        const D2D1_POINT_2F pts[2] = {Pt(nx, 50.8f), Pt(nx + 2.0f, 54.0f)};
        tsink->AddLines(pts, 2);
        tsink->EndFigure(D2D1_FIGURE_END_CLOSED);
        tsink->Close();
        tsink->Release();
        D.br->SetColor(Col(0x8BEAFF));
        D.rt->FillGeometry(tg, D.br);
        D.br->SetColor(Col(0x18AEE9));
        D.rt->DrawGeometry(tg, D.br, 0.4f);
        tg->Release();
    }
    Text(L"MHz", R(317.0f, 83.5f, 347.4f, 90.5f), D.fUnit, Col(0x6F8998));

    // 6. Track info: bold title and compact timing
    Text(s.song, R(143.0f, 94.5f, 347.0f, 112.5f), D.fTitle, Col(0xF0F5F8));
    Text(s.where, R(143.0f, 114.0f, 347.0f, 124.0f), D.fTime, Col(0xA8B6C0, 0.85f));

    // 7. Playback controls (centered vertically with 5 DIP margin to timing above and trim below)
    const D2D1_POINT_2F prevC = Pt(155.5f, 143.0f);
    if (s.hot == PART_PREV) Circle(prevC, 12.0f, Col(0xFFFFFF, s.press == PART_PREV ? 0.16f : 0.08f));
    Line(Pt(prevC.x - 4.0f, prevC.y - 4.5f), Pt(prevC.x - 4.0f, prevC.y + 4.5f), Col(0xDBE7EE), 1.2f);
    ID2D1PathGeometry* pg = nullptr;
    ID2D1GeometrySink* psink = nullptr;
    if (SUCCEEDED(D.d2d->CreatePathGeometry(&pg)) && SUCCEEDED(pg->Open(&psink))) {
        psink->BeginFigure(Pt(prevC.x + 4.0f, prevC.y - 4.5f), D2D1_FIGURE_BEGIN_HOLLOW);
        const D2D1_POINT_2F pts[2] = {Pt(prevC.x - 2.5f, prevC.y), Pt(prevC.x + 4.0f, prevC.y + 4.5f)};
        psink->AddLines(pts, 2);
        psink->EndFigure(D2D1_FIGURE_END_CLOSED);
        psink->Close();
        psink->Release();
        D.br->SetColor(Col(0xDBE7EE));
        D.rt->DrawGeometry(pg, D.br, 1.2f, D.round);
        pg->Release();
    }

    const D2D1_POINT_2F playC = Pt(188.5f, 143.0f);
    Circle(Pt(playC.x, playC.y + 1.0f), 14.5f, Col(0x0F1B24, 0.35f));
    Circle(playC, 14.0f, s.hot == PART_PLAY ? Col(0x56CEF7) : Col(0x3BC5F5));
    Ring(playC, 14.0f, Col(0x8CE7FF), 0.8f);
    if (s.playing) {
        Line(Pt(playC.x - 3.2f, playC.y - 4.8f), Pt(playC.x - 3.2f, playC.y + 4.8f), Col(0x123F57), 2.2f);
        Line(Pt(playC.x + 3.2f, playC.y - 4.8f), Pt(playC.x + 3.2f, playC.y + 4.8f), Col(0x123F57), 2.2f);
    } else {
        ID2D1PathGeometry* plg = nullptr;
        ID2D1GeometrySink* plsink = nullptr;
        if (SUCCEEDED(D.d2d->CreatePathGeometry(&plg)) && SUCCEEDED(plg->Open(&plsink))) {
            plsink->BeginFigure(Pt(playC.x - 3.0f, playC.y - 5.5f), D2D1_FIGURE_BEGIN_FILLED);
            const D2D1_POINT_2F pts[2] = {Pt(playC.x + 5.5f, playC.y), Pt(playC.x - 3.0f, playC.y + 5.5f)};
            plsink->AddLines(pts, 2);
            plsink->EndFigure(D2D1_FIGURE_END_CLOSED);
            plsink->Close();
            plsink->Release();
            D.br->SetColor(Col(0x123F57));
            D.rt->FillGeometry(plg, D.br);
            plg->Release();
        }
    }

    const D2D1_POINT_2F nextC = Pt(221.5f, 143.0f);
    if (s.hot == PART_NEXT) Circle(nextC, 12.0f, Col(0xFFFFFF, s.press == PART_NEXT ? 0.16f : 0.08f));
    Line(Pt(nextC.x + 4.0f, nextC.y - 4.5f), Pt(nextC.x + 4.0f, nextC.y + 4.5f), Col(0xDBE7EE), 1.2f);
    ID2D1PathGeometry* ng = nullptr;
    ID2D1GeometrySink* nsink = nullptr;
    if (SUCCEEDED(D.d2d->CreatePathGeometry(&ng)) && SUCCEEDED(ng->Open(&nsink))) {
        nsink->BeginFigure(Pt(nextC.x - 4.0f, nextC.y - 4.5f), D2D1_FIGURE_BEGIN_HOLLOW);
        const D2D1_POINT_2F pts[2] = {Pt(nextC.x + 2.5f, nextC.y), Pt(nextC.x - 4.0f, nextC.y + 4.5f)};
        nsink->AddLines(pts, 2);
        nsink->EndFigure(D2D1_FIGURE_END_CLOSED);
        nsink->Close();
        nsink->Release();
        D.br->SetColor(Col(0xDBE7EE));
        D.rt->DrawGeometry(ng, D.br, 1.2f, D.round);
        ng->Release();
    }

    // 8. Volume slider
    const D2D1_POINT_2F vi = Pt(248.0f, 143.0f);
    ID2D1PathGeometry* vig = nullptr;
    ID2D1GeometrySink* visink = nullptr;
    if (SUCCEEDED(D.d2d->CreatePathGeometry(&vig)) && SUCCEEDED(vig->Open(&visink))) {
        visink->BeginFigure(Pt(vi.x - 4.5f, vi.y - 2.5f), D2D1_FIGURE_BEGIN_HOLLOW);
        visink->AddLine(Pt(vi.x - 2.0f, vi.y - 2.5f));
        visink->AddLine(Pt(vi.x + 0.5f, vi.y - 4.5f));
        visink->AddLine(Pt(vi.x + 0.5f, vi.y + 4.5f));
        visink->AddLine(Pt(vi.x - 2.0f, vi.y + 2.5f));
        visink->AddLine(Pt(vi.x - 4.5f, vi.y + 2.5f));
        visink->EndFigure(D2D1_FIGURE_END_CLOSED);
        visink->Close();
        visink->Release();
        D.br->SetColor(Col(0xB8C8D2));
        D.rt->DrawGeometry(vig, D.br, 0.8f, D.round);
        vig->Release();
    }
    ID2D1PathGeometry* wg = nullptr;
    ID2D1GeometrySink* wsink = nullptr;
    if (SUCCEEDED(D.d2d->CreatePathGeometry(&wg)) && SUCCEEDED(wg->Open(&wsink))) {
        wsink->BeginFigure(Pt(vi.x + 2.2f, vi.y - 2.2f), D2D1_FIGURE_BEGIN_HOLLOW);
        wsink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(vi.x + 5.2f, vi.y), Pt(vi.x + 2.2f, vi.y + 2.2f)));
        wsink->EndFigure(D2D1_FIGURE_END_OPEN);
        wsink->BeginFigure(Pt(vi.x + 3.6f, vi.y - 4.0f), D2D1_FIGURE_BEGIN_HOLLOW);
        wsink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(vi.x + 8.5f, vi.y), Pt(vi.x + 3.6f, vi.y + 4.0f)));
        wsink->EndFigure(D2D1_FIGURE_END_OPEN);
        wsink->Close();
        wsink->Release();
        D.br->SetColor(Col(0xB8C8D2, 0.75f));
        D.rt->DrawGeometry(wg, D.br, 0.8f, D.round);
        wg->Release();
    }

    Fill(R(263.0f, 141.5f, 347.0f, 144.5f), Col(0x091017), 1.5f);
    Stroke(R(263.0f, 141.5f, 347.0f, 144.5f), Col(0x455660), 1.5f, 0.4f);

    const float fillW = 84.0f * (s.volume / 100.f);
    if (fillW > 0.5f) Fill(R(263.4f, 141.9f, 263.4f + fillW, 144.1f), Col(0x3BC5F5), 1.1f);
    const float thx = 263.0f + fillW;
    Circle(Pt(thx, 143.0f), 3.8f, Col(0xDCE8EE));
    Ring(Pt(thx, 143.0f), 3.8f, Col(0x7A8A94), 0.6f);
    Circle(Pt(thx, 142.6f), 1.4f, Col(0xFFFFFF, 0.85f));

    // 9. Lower trim line and markings
    Line(Pt(143.0f, 162.0f), Pt(347.0f, 162.0f), Col(0x60727E, 0.45f), 0.4f);
    Text(L"PORTABLE STEREO  \u2022  DIGITAL AUDIO", R(143.0f, 163.5f, 297.0f, 172.0f), D.fMicroLeft, Col(0x607684));
    Text(L"VOL. 07", R(301.0f, 163.5f, 347.0f, 172.0f), D.fMicroRight, Col(0x607684));
}

// ---- Moving the song's place: on what shows it, in each style -------------------------------

float Along(float x, float l, float r) { return std::clamp((x - l) / (r - l), 0.f, 1.f); }

bool Inside(float x, float y, const D2D1_RECT_F& r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }

bool OnRing(D2D1_POINT_2F c, float x, float y, float r0, float r1) {
    const float d = hypotf(x - c.x, y - c.y);
    return d >= r0 && d <= r1;
}

float Around(D2D1_POINT_2F c, float x, float y) {  // clockwise from 12 o'clock, as a share of the way round
    float deg = atan2f(x - c.x, c.y - y) * 180 / kPi;
    if (deg < 0) deg += 360;
    return deg / 360;
}

float SeekVinyl(const PlayerStyle&, float x, float y, bool held) {  // the grooves: the arm goes in as the song goes
    if (!held && !OnRing(kVinylC, x, y, 20, kVinylR + 2)) return -1;
    return std::clamp((44 - hypotf(x - kVinylC.x, y - kVinylC.y)) / 20, 0.f, 1.f);
}

float SeekCassette(const PlayerStyle&, float x, float y, bool held) {  // the window: tape from reel to reel
    return held || Inside(x, y, R(14, 14, 218, 106)) ? Along(x, 64, 168) : -1;
}

float SeekDisc(const PlayerStyle&, float x, float y, bool held) {  // the ring round the disc
    return held || OnRing(Pt(42, 42), x, y, 26, 44) ? Around(Pt(42, 42), x, y) : -1;
}

float SeekBars(const PlayerStyle& st, float x, float y, bool held) {  // the bars: lit as far as it's played
    return held || Inside(x, y, R(16, 62, st.w - 16, 110)) ? Along(x, 18, st.w - 18) : -1;
}

float SeekWave(const PlayerStyle& st, float x, float y, bool held) {  // the threads: lit as far as it's played
    return held || Inside(x, y, R(16, 62, st.w - 16, 110)) ? Along(x, 18, st.w - 18) : -1;
}

float SeekHalo(const PlayerStyle&, float x, float y, bool held) {  // the bars round the cover
    return held || OnRing(kHaloC, x, y, 26, 54) ? Around(kHaloC, x, y) : -1;
}

float SeekCover(const PlayerStyle& st, float x, float y, bool held) {  // its line
    return held || Inside(x, y, R(150, 58, st.w - 14, 76)) ? Along(x, 154, st.w - 18) : -1;
}

float SeekRadio(const PlayerStyle&, float x, float y, bool held) {  // the FM tuner dial
    return held || Inside(x, y, R(137.0f, 50.0f, 356.2f, 93.2f)) ? Along(x, 147.8f, 347.4f) : -1;
}

float SeekMini(const PlayerStyle& st, float x, float y, bool held) {  // the line along its bottom edge
    return held || Inside(x, y, R(24, st.h - 12, st.w - 24, st.h)) ? Along(x, 28, st.w - 28) : -1;
}

const PlayerStyle kStyles[] = {
    {L"Vinyl", L"A record that turns", 380, 120, {144, 82}, {182, 82}, {220, 82}, 15, 18, {276, 80.5f, 354, 83.5f}, true, false, {4, 0, 134, 114}, DrawVinyl, SeekVinyl},
    {L"Cassette", L"Tape winds reel to reel", 380, 146, {258, 99}, {299, 99}, {340, 99}, 13, 16, {262, 126.5f, 364, 129.5f}, true, true, {14, 14, 370, 108}, DrawCassette, SeekCassette},
    {L"Disc", L"A disc turning in a ring", 350, 84, {240, 42}, {280, 42}, {320, 42}, 15, 19, {110, 63.5f, 200, 66.5f}, true, false, {10, 10, 74, 74}, DrawDisc, SeekDisc},
    {L"Bars", L"Bars move with the sound", 360, 118, {258, 30}, {296, 30}, {334, 30}, 13, 16, {266, 52.5f, 340, 55.5f}, true, true, {16, 64, 344, 108}, DrawBars, SeekBars},
    {L"Wave", L"Threads swing with the sound", 360, 118, {258, 30}, {296, 30}, {334, 30}, 13, 16, {266, 52.5f, 340, 55.5f}, true, true, {14, 64, 346, 108}, DrawWave, SeekWave},
    {L"Halo", L"Bars round the cover", 380, 120, {136, 82}, {174, 82}, {212, 82}, 15, 18, {268, 80.5f, 350, 83.5f}, true, true, {6, 4, 116, 116}, DrawHalo, SeekHalo},
    {L"Cover", L"The song's cover, big", 380, 150, {174, 106}, {214, 106}, {254, 106}, 16, 19, {304, 104.5f, 362, 107.5f}, false, false, {}, DrawCover, SeekCover},
    {L"Radio", L"A dial follows the song", 374, 178, {155.5f, 143.0f}, {188.5f, 143.0f}, {221.5f, 143.0f}, 12, 14, {263.0f, 141.5f, 347.0f, 144.5f}, false, false, {}, DrawRadio, SeekRadio},
    {L"Mini", L"Just the song, small", 320, 56, {214, 28}, {252, 28}, {290, 28}, 13, 17, {148, 35.5f, 186, 38.5f}, false, false, {}, DrawMini, SeekMini},
};

const PlayerStyle& Style() {
    return kStyles[std::clamp(g_settings.musicPlayer - 1, 0, (int)std::size(kStyles) - 1)];
}

int HitPart(const PlayerStyle& st, float x, float y) {
    auto over = [&](D2D1_POINT_2F c, float r) { return hypotf(x - c.x, y - c.y) <= r + 3; };
    if (over(st.play, st.playR)) return PART_PLAY;
    if (over(st.prev, st.btn)) return PART_PREV;
    if (over(st.next, st.btn)) return PART_NEXT;
    const D2D1_RECT_F& v = st.volume;
    if (v.right > v.left && x >= v.left - 26 && x <= v.right + 10 && y >= v.top - 8 && y <= v.bottom + 6) return PART_VOLUME;
    const MusicState ms = Host_MusicState();
    if (ms.track >= 0 && ms.durMs && st.seek(st, x, y, false) >= 0) return PART_SEEK;  // (only while a song is there to move in)
    return x >= 0 && y >= 0 && x < st.w && y < st.h ? PART_BODY : PART_NONE;
}

// ---- The song and what turns -----------------------------------------------------------------

// The song's cover (the picture in the file, as Explorer shows it), when the song changes.
void LoadCover(const std::wstring& path) {
    if (path == W.artFor) return;
    W.artFor = path;
    W.artPx.clear();
    W.artW = W.artH = 0;
    Rel(D.art);
    IShellItemImageFactory* f = nullptr;
    HBITMAP bmp = nullptr;
    if (!path.empty() && SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->GetImage(SIZE{128, 128}, SIIGBF_THUMBNAILONLY, &bmp))) {  // (small: the label it's drawn on is)
        BITMAP b{};
        GetObjectW(bmp, sizeof b, &b);
        if (b.bmWidth > 0 && b.bmHeight > 0 && b.bmWidth <= 256 && b.bmHeight <= 256) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = b.bmWidth;
            bi.bmiHeader.biHeight = -b.bmHeight;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            W.artPx.resize((size_t)b.bmWidth * b.bmHeight);
            HDC dc = GetDC(nullptr);
            if (GetDIBits(dc, bmp, 0, b.bmHeight, W.artPx.data(), &bi, DIB_RGB_COLORS) == b.bmHeight) {
                W.artW = b.bmWidth;
                W.artH = b.bmHeight;
                for (uint32_t& p : W.artPx) p |= 0xFF000000;  // covers are opaque
            } else {
                W.artPx.clear();
            }
            ReleaseDC(nullptr, dc);
        }
        DeleteObject(bmp);
    }
    if (f) f->Release();
}

// The speakers or headphones the sound goes to now ("" if none), and that device.
std::wstring DefaultOutput(IMMDevice** dev) {
    static const CLSID kEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
    static const IID kIEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
    if (!W.outputs && FAILED(CoCreateInstance(kEnumerator, nullptr, CLSCTX_INPROC_SERVER, kIEnumerator, (void**)&W.outputs))) return L"";
    IMMDevice* d = nullptr;
    if (FAILED(W.outputs->GetDefaultAudioEndpoint(eRender, eMultimedia, &d))) return L"";
    std::wstring id;
    LPWSTR p = nullptr;
    if (SUCCEEDED(d->GetId(&p))) {
        id = p;
        CoTaskMemFree(p);
    }
    if (dev) *dev = d;
    else d->Release();
    return id;
}

void StopMeter() {
    Rel(W.meter);
    Rel(W.outputs);
    W.meterFor.clear();
}

// How loud the sound playing is (0..1), for the bars.
float Loudness() {
    const double now = Now();
    if (W.meter && now - W.meterChecked > 1) {  // headphones plugged in or out: the sound plays somewhere else now
        W.meterChecked = now;
        if (DefaultOutput(nullptr) != W.meterFor) Rel(W.meter);
    }
    if (!W.meter) {
        static const IID kIMeter = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};
        IMMDevice* dev = nullptr;
        W.meterFor = DefaultOutput(&dev);
        W.meterChecked = now;
        if (dev) dev->Activate(kIMeter, CLSCTX_INPROC_SERVER, nullptr, (void**)&W.meter);
        Rel(dev);
        if (!W.meter) return 0;
    }
    float peak = 0;
    if (FAILED(W.meter->GetPeakValue(&peak))) {
        Rel(W.meter);  // the output changed: found again next time
        return 0;
    }
    const float vol = std::clamp(g_settings.volume / 100.f, 0.04f, 1.f);
    const float norm = std::clamp(peak / vol, 0.f, 1.f);
    return std::clamp(sqrtf(norm) * 1.15f, 0.f, 1.f);
}

// What turns, moved on by dt seconds: the record, the reels, the arm, the bars (bars: the style has them).
void Turn(float dt, bool playing, float done, bool bars) {
    if (playing) {
        W.waveT = fmodf(W.waveT + dt, 3600.f);
        W.turn = fmodf(W.turn + 200 * dt, 360.f);  // a record at 33 1/3
        W.turn2 = fmodf(W.turn2 + 160 * dt * (14 / (10 + 18 * done)), 360.f);  // a cassette's right reel (tape winding on)
        W.slow = fmodf(W.slow + 45 * dt, 360.f);
    }
    W.arm = std::clamp(W.arm + (playing ? 2.5f : -2.5f) * dt, 0.f, 1.f);
    if (!bars) return;
    const double t = Now();
    const float loud = playing ? Loudness() : 0;
    W.level = loud > W.level ? W.level + (loud - W.level) * std::min(1.f, dt * 24) : std::max(loud, W.level - dt * 3.6f);
    for (int i = 0; i < kBars; i++) {  // the sound's level, shaped differently for each bar
        const float shape = (0.5f + 0.5f * sinf((float)t * (7.2f + i * 0.45f) + i * 1.3f)) *
                            (0.65f + 0.35f * sinf((float)t * 4.1f + i * 0.8f));
        const float want = std::clamp(loud * (0.22f + 0.78f * shape), 0.f, 1.f);
        W.bars[i] = want > W.bars[i] ? W.bars[i] + (want - W.bars[i]) * std::min(1.f, dt * 24) : std::max(want, W.bars[i] - dt * 3.4f);
    }
}

bool Settled() {
    if ((W.arm > 0 && W.arm < 1) || W.level > 0.005f) return false;
    for (float b : W.bars)
        if (b > 0.005f) return false;
    return true;
}

// The desktop can be seen (nothing covers it, the screen's on and unlocked).
bool DesktopSeen() {
    LPARAM mask = 0;
    Host_RendererState(&mask);
    return !(mask & (PR_COVERED | PR_LOCKED | PR_DISPLAY_OFF));
}

const Settings::PlayerLook& LookOf(int style) {
    return g_settings.playerLooks[std::clamp(style, 0, (int)std::size(g_settings.playerLooks) - 1)];
}

// The colour of a style's lit parts: the user's, or the Windows accent colour.
D2D1_COLOR_F AccentOf(int style) {
    BYTE r, g, b;
    const std::wstring& c = LookOf(style).color;
    return !c.empty() && ParseColor(c, &r, &g, &b) ? D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f) : Accent();
}

// What shows now; seek (0..1): the place being dragged to, in a preview.
Shown Current(float seek = -1, int style = 0) {
    const MusicState ms = Host_MusicState();
    const int n = (int)W.songs.size();
    const int kept = g_settings.musicTrack >= 0 && g_settings.musicTrack < n ? g_settings.musicTrack : 0;
    const int track = ms.track >= 0 && ms.track < n ? ms.track : kept;
    const bool playing = g_settings.musicPlaying;
    const UINT dur = ms.track >= 0 ? ms.durMs : g_settings.musicDurMs;
    UINT pos = ms.track >= 0 ? ms.posMs : g_settings.musicPosMs;
    if (seek < 0 && W.seekFrac >= 0 && (W.seeking || Now() < W.seekHold)) seek = W.seekFrac;  // (until the song gets there)
    if (seek >= 0) pos = (UINT)(seek * dur);
    Shown s;
    s.song = n ? SongName(W.songs[track]) : L"No songs chosen yet";
    s.where = dur ? Minutes(pos) + L" / " + Minutes(dur) : L"";
    s.playing = playing;
    s.done = dur ? std::min(1.f, (float)pos / dur) : 0;
    s.volume = g_settings.volume;
    s.accent = AccentOf(style ? style : g_settings.musicPlayer);
    LoadCover(n ? W.songs[track] : L"");
    return s;
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

// Draws style st with what s shows into a w x h picture (pixels; scale: pixels per DIP).
template <class Use> bool Render(const PlayerStyle& st, const Shown& s, float scale, int w, int h, Use use) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    const RECT all{0, 0, w, h};
    D.rt->SetDpi(96 * scale, 96 * scale);
    D.rt->BindDC(dc, &all);
    D.rt->BeginDraw();
    D.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    st.draw(st, s);
    const HRESULT hr = D.rt->EndDraw();
    if (SUCCEEDED(hr)) {
        GdiFlush();
        use(dc, (const uint32_t*)bits);
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    if (hr == D2DERR_RECREATE_TARGET) StopDrawing();
    return SUCCEEDED(hr);
}

void KeepTurning();

// The window's picture, kept while it shows (made again when its size changes).
bool WindowDib() {
    if (W.dc && W.dibW == W.w && W.dibH == W.h) return true;
    if (W.dc) {
        SelectObject(W.dc, W.oldBmp);
        DeleteObject(W.dib);
        DeleteDC(W.dc);
        W.dc = nullptr;
    }
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
        return false;
    }
    W.dc = dc;
    W.dib = bmp;
    W.oldBmp = SelectObject(dc, bmp);
    W.bits = (uint32_t*)bits;
    W.dibW = W.w;
    W.dibH = W.h;
    return true;
}

void FreeWindowDib() {
    if (!W.dc) return;
    SelectObject(W.dc, W.oldBmp);
    DeleteObject(W.dib);
    DeleteDC(W.dc);
    W.dc = nullptr;
    W.dibW = W.dibH = 0;
}

// Draws one layer of the style into the window's picture; part: only that part of it (the rest of
// the picture stays as it was).
bool DrawLayer(const PlayerStyle& st, const Shown& s, int layer, bool withStill, const RECT* part = nullptr) {
    const RECT all{0, 0, W.w, W.h};
    D.rt->SetDpi(96 * W.scale, 96 * W.scale);
    if (FAILED(D.rt->BindDC(W.dc, part ? part : &all))) return false;
    D.layer = layer;
    D.rt->BeginDraw();
    if (part) D.rt->SetTransform(D2D1::Matrix3x2F::Translation(-part->left / W.scale, -part->top / W.scale));
    D.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (withStill && D.still) D.rt->DrawBitmap(D.still, R(0, 0, st.w, st.h), 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    st.draw(st, s);
    D.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT hr = D.rt->EndDraw();
    D.layer = 0;
    if (hr == D2DERR_RECREATE_TARGET) StopDrawing();
    if (FAILED(hr)) return false;
    GdiFlush();
    return true;
}

void Draw(bool force = false) {
    if (!W.hwnd || !StartDrawing()) return;
    Shown s = Current();
    const PlayerStyle& st = Style();
    const Settings::PlayerLook& look = LookOf(g_settings.musicPlayer);
    D.corners = look.corners;
    wchar_t key[260];
    swprintf(key, 260, L"%d|%ls|%ls|%d|%d|%d|%d|%d|%d|%.2f|%ls|%ls|%.1f|%.2f", g_settings.musicPlayer, s.song.c_str(), s.where.c_str(), s.playing,
             (int)(s.done * 400), W.hot, W.press, W.sliding, g_settings.volume, W.scale, W.artFor.c_str(), look.color.c_str(), look.corners, look.opacity);
    if (!force && W.drawn == key) {
        KeepTurning();
        return;
    }
    s.turn = W.turn;
    s.turn2 = W.turn2;
    s.slow = W.slow;
    s.arm = W.arm;
    s.bars = W.bars;
    s.level = W.level;
    s.clock = W.waveT;
    s.hot = W.hot;
    s.press = W.press;
    s.sliding = W.sliding;
    W.w = (int)ceilf(st.w * W.scale);
    W.h = (int)ceilf(st.h * W.scale);
    if (!WindowDib()) return;
    bool ok;
    RECT dirty{0, 0, W.w, W.h};  // what changed in the picture
    if (st.turns) {  // what stays still, drawn when it changes and kept; then what turns over it
        const D2D1_RECT_F& t = st.turning;
        const bool same = D.still && W.stillKey == key && W.drawn == key;  // just turning: only that part
        if (!D.still || W.stillKey != key) {
            Rel(D.still);
            W.stillKey.clear();
            if (DrawLayer(st, s, 1, false)) {
                D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * W.scale, 96 * W.scale);
                if (SUCCEEDED(D.rt->CreateBitmap(D2D1::SizeU((UINT32)W.w, (UINT32)W.h), W.bits, (UINT32)W.w * 4, bp, &D.still))) W.stillKey = key;
            }
        }
        if (same) dirty = RECT{(LONG)floorf(t.left * W.scale), (LONG)floorf(t.top * W.scale), (LONG)ceilf(t.right * W.scale), (LONG)ceilf(t.bottom * W.scale)};
        ok = D.still && DrawLayer(st, s, 2, true, same ? &dirty : nullptr);
    } else {
        ok = DrawLayer(st, s, 0, false);
    }
    if (ok) {
        POINT dst = Origin(), zero{0, 0};
        SIZE sz{W.w, W.h};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)lroundf(255 * std::clamp(look.opacity, 0.2f, 1.f)), AC_SRC_ALPHA};  // (the user's opacity)
        UPDATELAYEREDWINDOWINFO ul{sizeof ul, nullptr, &dst, &sz, W.dc, &zero, 0, &bf, ULW_ALPHA, &dirty};
        if (UpdateLayeredWindowIndirect(W.hwnd, &ul)) W.drawn = key;
    }
    KeepTurning();
}

// About 30 times a second while something on it turns and can be seen; otherwise nothing runs.
void KeepTurning() {
    const bool playing = g_settings.musicPlaying && Host_MusicState().playing;
    const bool want = W.hwnd && Style().turns && DesktopSeen() && !W.dragging && (playing || !Settled());
    if (want == W.turning) return;
    W.turning = want;
    if (want) {
        W.last = Now();
        SetTimer(W.hwnd, TIMER_TURN, kTurnMs, nullptr);
    } else if (W.hwnd) {
        KillTimer(W.hwnd, TIMER_TURN);
    }
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
        case WM_TIMER:
            if (w == TIMER_TURN) {
                const double now = Now();
                const float dt = (float)std::clamp(now - W.last, 0.0, 0.1);
                W.last = now;
                const MusicState ms = Host_MusicState();
                Turn(dt, g_settings.musicPlaying && ms.playing, ms.durMs ? std::min(1.f, (float)ms.posMs / ms.durMs) : 0, Style().sound);
                Draw(true);
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (W.sliding) {
                SetVolumeAt(x);
                return 0;
            }
            if (W.seeking) {  // the place follows the pointer; the song moves there when let go
                W.seekFrac = Style().seek(Style(), x, y, true);
                Draw();
                return 0;
            }
            if (W.dragging) {  // follows the pointer; the place is kept when let go
                POINT p;
                GetCursorPos(&p);
                SetWindowPos(h, nullptr, p.x - W.grab.x, p.y - W.grab.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            const int hot = HitPart(Style(), x, y);
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
            W.press = HitPart(Style(), x, y);
            if (W.press == PART_VOLUME) {
                W.sliding = true;
                SetVolumeAt(x);
            } else if (W.press == PART_SEEK) {
                W.seeking = true;
                W.seekFrac = Style().seek(Style(), x, y, true);
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
            } else if (W.seeking) {
                W.seeking = false;
                Player_Seek(W.seekFrac);
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
            } else if (part == HitPart(Style(), x, y)) {
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
    W.turning = false;
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
        force = true;
    }
    W.songs = songs;
    if (fresh) W.arm = g_settings.musicPlaying ? 1.f : 0.f;
    Draw(force || fresh);
    const POINT p = Origin();
    SetWindowPos(W.hwnd, HWND_BOTTOM, p.x, p.y, W.w, W.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);  // bottom = just above the desktop
    if (fresh) Log(L"player: on the desktop (%ls)", Style().name);
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
    KillTimer(W.hwnd, TIMER_TURN);
    DestroyWindow(W.hwnd);
    W.hwnd = nullptr;
    W.hot = W.press = PART_NONE;
    W.dragging = W.sliding = W.turning = W.seeking = false;
    StopMeter();
    StopDrawing();
    FreeWindowDib();
    Log(L"player: off the desktop");
}

bool Player_Paint(int style, float scale, bool live, ClockImage* out, const PlayerInput* in) {
    if (style < 1 || style > (int)std::size(kStyles) || !StartDrawing()) return false;
    const PlayerStyle& st = kStyles[style - 1];
    if (!W.hwnd) W.songs = LoadPlaylist();  // (the window keeps its own up to date)
    Shown s = Current(in ? in->seek : -1, style);
    const Settings::PlayerLook& look = LookOf(style);
    D.corners = look.corners;
    if (in) {
        s.hot = in->hot;
        s.press = in->press;
        s.sliding = in->sliding;
    }
    // What turns: by the clock, so a preview moves on its own (or, still, as it looks mid-song).
    const float t = live && s.playing ? (float)fmod(Now(), 3600.0) : 0;
    s.turn = fmodf(30 + 200 * t, 360.f);
    s.turn2 = fmodf(90 + 160 * t, 360.f);
    s.slow = fmodf(20 + 45 * t, 360.f);
    s.arm = live ? (s.playing ? 1.f : 0.f) : 1.f;
    if (!live && s.done <= 0) s.done = 0.35f;
    float bars[kBars];
    for (int i = 0; i < kBars; i++)
        bars[i] = (live && !s.playing) ? 0 : 0.25f + 0.55f * (0.5f + 0.5f * sinf(t * (3.1f + i * 0.37f) + i * 1.7f)) * (0.6f + 0.4f * sinf(t * 1.3f + i * 0.9f));
    s.bars = bars;
    s.level = live ? (s.playing ? 0.55f + 0.25f * sinf(t * 1.7f) : 0) : 0.6f;
    s.clock = live ? t : 1.3f;
    const int w = (int)ceilf(st.w * scale), h = (int)ceilf(st.h * scale);
    W.drawn.clear();  // (the window's next frame is drawn whole: this used the same drawing surface)
    const bool ok = Render(st, s, scale, w, h, [&](HDC, const uint32_t* px) {
        out->px.assign(px, px + (size_t)w * h);
        out->w = w;
        out->h = h;
        out->pad = 0;
    });
    D.corners = LookOf(g_settings.musicPlayer).corners;  // (the window's again)
    if (ok && look.opacity < 1) {  // the user's opacity, as on the desktop
        const UINT32 k = (UINT32)lroundf(256 * std::clamp(look.opacity, 0.2f, 1.f));
        for (uint32_t& p : out->px)
            p = ((p >> 24) * k >> 8) << 24 | (((p >> 16) & 255) * k >> 8) << 16 | (((p >> 8) & 255) * k >> 8) << 8 | ((p & 255) * k >> 8);
    }
    return ok;
}

bool Player_HasCorners(int style) {  // its card's corners can be changed (the cassette and the radio have their own bodies)
    return style >= 1 && style <= (int)std::size(kStyles) && kStyles[style - 1].draw != DrawCassette && kStyles[style - 1].draw != DrawRadio;
}

float Player_Corners(int style) {  // the corner radius it has now (DIPs)
    static const float own[] = {0, 20, 0, 41.5f, 20, 20, 20, 22, 0, 27.5f};  // each style's own (Card radius; Disc and Mini: pills)
    const float c = LookOf(style).corners;
    return c >= 0 ? c : style >= 1 && style < (int)std::size(own) ? own[style] : 20;
}

std::wstring Player_About(int style) {
    return style >= 1 && style <= (int)std::size(kStyles) ? kStyles[style - 1].about : L"Nothing on the desktop";
}

void Player_Size(int style, float* w, float* h) {
    const bool ok = style >= 1 && style <= (int)std::size(kStyles);
    *w = ok ? kStyles[style - 1].w : 0;
    *h = ok ? kStyles[style - 1].h : 0;
}

int Player_PartAt(int style, float x, float y) {
    return style >= 1 && style <= (int)std::size(kStyles) ? HitPart(kStyles[style - 1], x, y) : PART_NONE;
}

float Player_VolumeAt(int style, float x) {
    if (style < 1 || style > (int)std::size(kStyles)) return 0;
    const D2D1_RECT_F& t = kStyles[style - 1].volume;
    return t.right > t.left ? std::clamp((x - t.left) / (t.right - t.left), 0.f, 1.f) : 0;
}

float Player_SeekAt(int style, float x, float y) {
    return style >= 1 && style <= (int)std::size(kStyles) ? kStyles[style - 1].seek(kStyles[style - 1], x, y, true) : 0;
}

bool Player_HasVolume(int style) {
    return style >= 1 && style <= (int)std::size(kStyles) && kStyles[style - 1].volume.right > kStyles[style - 1].volume.left;
}

void Player_Seek(float frac) {
    const MusicState ms = Host_MusicState();
    W.seekFrac = std::clamp(frac, 0.f, 1.f);
    W.seekHold = Now() + 1.2;  // shown there until the song has got there
    if (ms.track >= 0 && ms.durMs) Host_MusicCommand(MC_SEEK, (LPARAM)(W.seekFrac * ms.durMs));
    Player_Update();
}

bool Player_Turns() { return g_settings.musicPlaying && Host_MusicState().playing; }

void Player_FreePreview() {
    if (!W.hwnd) StopDrawing();
}
