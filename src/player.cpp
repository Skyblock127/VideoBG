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
    ID2D1Bitmap* art = nullptr;  // the song's cover, for this render target
    ID2D1Bitmap* still = nullptr;  // the window's style without what turns (drawn when what it shows changes)
    int layer = 0;               // what a style draws: 0 all of it, 1 what stays still, 2 what turns
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
    D.fBold = Format(L"Segoe UI Variable Text", 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    D.fMark = Format(L"Segoe UI Variable Text", 11, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    D.fIcon = Format(L"Segoe Fluent Icons", 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    D.fIconBig = Format(L"Segoe Fluent Icons", 15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    return D.br && D.round && D.fTitle && D.fSmall && D.fBold && D.fMark && D.fIcon && D.fIconBig;
}

void StopDrawing() {
    for (IDWriteTextFormat** f : {&D.fTitle, &D.fSmall, &D.fBold, &D.fMark, &D.fIcon, &D.fIconBig}) Rel(*f);
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
    D.rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, D.br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
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

// The glass card most styles sit on.
void Card(float w, float h, float radius, float a = 0.84f) {
    const D2D1_RECT_F r = R(0.5f, 0.5f, w - 0.5f, h - 0.5f);
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
constexpr D2D1_POINT_2F kVinylC{80, 75}, kVinylPivot{152, 20};
constexpr float kVinylR = 62, kArmLen = 96;

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
        Card(st.w, st.h, 24);
        Circle(c, kVinylR, Col(0x0B0B0D, 0.98f));
        Ring(c, kVinylR, Col(0xFFFFFF, 0.12f), 1);
        for (int i = 0; i < 11; i++) Ring(c, 58 - i * 3.f, Col(0xFFFFFF, i % 3 ? 0.035f : 0.07f), 0.8f);
        Arc(c, 50, 300, 42, Col(0xFFFFFF, 0.16f), 2);  // light on the grooves (it stays: the record turns under it)
        Arc(c, 40, 120, 38, Col(0xFFFFFF, 0.1f), 2);
        // The song, its place, the buttons and the volume.
        const float x = 172;
        Text(s.song, R(x, 16, st.w - 18, 38), D.fTitle, Col(0xFFFFFF, 0.95f));
        Text(s.where, R(x, 38, st.w - 18, 54), D.fSmall, Col(0xFFFFFF, 0.6f));
        Buttons(st, s);
        Volume(st, s);
    }
    if (!TurningPart()) return;
    // The label (the song's cover, if it has one) turns.
    if (!Cover(c, 23, s.turn)) {
        Circle(c, 23, s.accent);
        const D2D1_COLOR_F mark = Luma(s.accent) > 0.55f ? Col(0x000000, 0.45f) : Col(0xFFFFFF, 0.7f);
        Arc(c, 16, s.turn, 70, mark, 2.4f);
        Circle(Polar(c, 9, s.turn + 200), 2, mark);
    }
    Circle(c, 3.2f, Col(0x141417));
    // The arm: from its pivot at the corner, onto the groove (further in as the song goes).
    const float rest = 100, on = ArmOnRecord(57 - 25 * s.done);
    const float deg = rest + (on - rest) * s.arm;
    const D2D1_POINT_2F tip = ArmAt(deg), back = ArmAt(deg + 180);
    const D2D1_POINT_2F weight = Pt(kVinylPivot.x + (back.x - kVinylPivot.x) * 0.16f, kVinylPivot.y + (back.y - kVinylPivot.y) * 0.16f);
    Line(kVinylPivot, weight, Col(0xFFFFFF, 0.55f), 7);
    Line(kVinylPivot, tip, Col(0xFFFFFF, 0.85f), 3.2f);
    const float head = (deg + 28) * kPi / 180;
    Line(tip, Pt(tip.x + 11 * cosf(head), tip.y + 11 * sinf(head)), Col(0xFFFFFF, 0.92f), 6);
    Circle(kVinylPivot, 9, Col(0x2A2A2F));
    Ring(kVinylPivot, 9, Col(0xFFFFFF, 0.2f), 1);
    Circle(kVinylPivot, 3.5f, Col(0xFFFFFF, 0.7f));
}

// Cassette: a tape whose reels turn while the song plays, the tape winding from one reel to the
// other as it goes; the song on its label; the controls on a strip under it.
void CassetteBody(const PlayerStyle& st, const Shown& s, const D2D1_RECT_F& win);

void DrawCassette(const PlayerStyle& st, const Shown& s) {
    const float w = st.w;
    const D2D1_RECT_F win = R(80, 62, w - 80, 110);
    const D2D1_POINT_2F lc = Pt(112, 86), rc = Pt(w - 112, 86);
    if (StillPart()) CassetteBody(st, s, win);
    if (!TurningPart()) return;
    const float lr = 10 + 18 * (1 - s.done), rr = 10 + 18 * s.done;
    D.rt->PushAxisAlignedClip(R(win.left + 1, win.top + 1, win.right - 1, win.bottom - 1), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const D2D1_COLOR_F tape = Col(0x3B2F27);
    Circle(lc, lr, tape);
    Circle(rc, rr, tape);
    Line(Pt(lc.x, lc.y + lr), Pt(rc.x, rc.y + rr), tape, 1.6f);
    D.rt->PopAxisAlignedClip();
    for (int k = 0; k < 2; k++) {  // the hubs, turning
        const D2D1_POINT_2F c = k ? rc : lc;
        const float deg = k ? s.turn2 : s.turn;
        Circle(c, 9, Col(0xFFFFFF, 0.88f));
        for (int i = 0; i < 6; i++) Line(Polar(c, 4.5f, deg + i * 60), Polar(c, 8.6f, deg + i * 60), Col(0x0A0A0C), 2.2f);
        Circle(c, 3, Col(0x0A0A0C));
    }
    Stroke(win, Col(0xFFFFFF, 0.1f), 12);
}

// The cassette's case, label and controls (what stays still).
void CassetteBody(const PlayerStyle& st, const Shown& s, const D2D1_RECT_F& win) {
    const float w = st.w;
    const D2D1_RECT_F body = R(10, 8, w - 10, 140);
    Fill(body, Col(0x1C1C20, 0.96f), 16);
    Stroke(body, Col(0xFFFFFF, 0.12f), 16);
    for (D2D1_POINT_2F p : {Pt(24, 22), Pt(w - 24, 22), Pt(24, 126), Pt(w - 24, 126)}) Circle(p, 3.2f, Col(0xFFFFFF, 0.16f));
    // The label, with the song on it.
    const D2D1_RECT_F label = R(32, 20, w - 32, 52);
    Fill(label, s.accent, 7);
    const D2D1_COLOR_F ink = Luma(s.accent) > 0.55f ? Col(0x000000, 0.85f) : Col(0xFFFFFF, 0.95f);
    D2D1_COLOR_F side = ink;
    side.a *= 0.6f;
    Text(L"A", R(label.left + 6, label.top, label.left + 24, label.bottom), D.fMark, side);
    Text(s.song, R(label.left + 26, label.top, label.right - 26, label.bottom), D.fBold, ink);
    // The window (its reels and tape turn over it), and the bottom edge, with the holes.
    Fill(win, Col(0x0A0A0C), 12);
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(D.d2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sink))) {
        sink->BeginFigure(Pt(62, 140), D2D1_FIGURE_BEGIN_FILLED);
        const D2D1_POINT_2F pts[3] = {Pt(76, 118), Pt(w - 76, 118), Pt(w - 62, 140)};
        sink->AddLines(pts, 3);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();
        D.br->SetColor(Col(0x26262B));
        D.rt->FillGeometry(g, D.br);
    }
    Rel(g);
    Circle(Pt(110, 130), 4, Col(0x0A0A0C));
    Circle(Pt(w - 110, 130), 4, Col(0x0A0A0C));
    // The controls.
    const D2D1_RECT_F strip = R(10, 148, w - 10, st.h - 2);
    Fill(strip, Col(0x141417, 0.86f), 16);
    Stroke(strip, Col(0xFFFFFF, 0.1f), 16);
    Text(s.where, R(26, strip.top, 124, strip.bottom), D.fSmall, Col(0xFFFFFF, 0.65f));
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

// Bars: the song over bars that move with the sound, the buttons beside the song, the volume under.
void DrawBars(const PlayerStyle& st, const Shown& s) {
    if (StillPart()) {
        Card(st.w, st.h, 20);
        Text(s.song, R(18, 12, st.w - 134, 34), D.fTitle, Col(0xFFFFFF, 0.95f));
        Text(s.where, R(18, 33, st.w - 134, 49), D.fSmall, Col(0xFFFFFF, 0.6f));
        Buttons(st, s);
        Volume(st, s);
    }
    if (!TurningPart()) return;
    const float left = 18, right = st.w - 18, base = 102, top = 58, step = (right - left) / kBars, bw = step - 3;
    for (int i = 0; i < kBars; i++) {
        const float v = s.bars ? std::clamp(s.bars[i], 0.f, 1.f) : 0, hgt = 3 + (base - top - 3) * v;
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
    Text(s.song, R(56, 8, 172, 30), D.fTitle, Col(0xFFFFFF, 0.95f));
    Text(s.where, R(56, 28, 172, 46), D.fSmall, Col(0xFFFFFF, 0.6f));
    Buttons(st, s);
    D.rt->PushAxisAlignedClip(R(28, st.h - 4, st.w - 28, st.h - 1), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    Progress(R(28, st.h - 4, st.w - 28, st.h - 1.5f), s);
    D.rt->PopAxisAlignedClip();
}

// Cover: the song's cover, square and big, beside the song and its controls.
void DrawCover(const PlayerStyle& st, const Shown& s) {
    Card(st.w, st.h, 22);
    const D2D1_RECT_F pic = R(15, 15, 135, 135);
    ID2D1RoundedRectangleGeometry* shape = nullptr;
    bool drawn = false;
    if (Art() && SUCCEEDED(D.d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(pic, 12, 12), &shape))) {
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
    if (!drawn) {  // no cover: a note on the accent
        Fill(pic, s.accent, 12);
        IDWriteTextFormat* big = Format(L"Segoe Fluent Icons", 44, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(L"\uE8D6", pic, big, Luma(s.accent) > 0.55f ? Col(0x000000, 0.6f) : Col(0xFFFFFF, 0.85f));
        Rel(big);
    }
    Stroke(pic, Col(0xFFFFFF, 0.1f), 12);
    const float x = 154;
    Text(s.song, R(x, 18, st.w - 18, 40), D.fTitle, Col(0xFFFFFF, 0.95f));
    Text(s.where, R(x, 40, st.w - 18, 56), D.fSmall, Col(0xFFFFFF, 0.6f));
    Progress(R(x, 66, st.w - 18, 68.5f), s);
    Buttons(st, s);
    Volume(st, s);
}

// Halo: the song's cover turning slowly in a ring of bars that move with the sound; beside it, the
// song and its controls.
constexpr D2D1_POINT_2F kHaloC{80, 75};

void DrawHalo(const PlayerStyle& st, const Shown& s) {
    const D2D1_POINT_2F c = kHaloC;
    if (StillPart()) {
        Card(st.w, st.h, 24);
        const float x = 168;
        Text(s.song, R(x, 16, st.w - 18, 38), D.fTitle, Col(0xFFFFFF, 0.95f));
        Text(s.where, R(x, 38, st.w - 18, 54), D.fSmall, Col(0xFFFFFF, 0.6f));
        Buttons(st, s);
        Volume(st, s);
    }
    if (!TurningPart()) return;
    for (int i = 0; i < kBars; i++) {  // the bars, all round (the played part lit)
        const float v = s.bars ? std::clamp(s.bars[i], 0.f, 1.f) : 0, deg = i * 360.f / kBars;
        Line(Polar(c, 42, deg), Polar(c, 45 + 22 * v, deg), i < s.done * kBars ? s.accent : Col(0xFFFFFF, 0.38f), 3.6f);
    }
    if (!Cover(c, 34, s.slow)) {
        Circle(c, 34, Col(0x26262B));
        Ring(c, 26, Col(0xFFFFFF, 0.08f), 1);
        Arc(c, 30, s.slow, 50, Col(0xFFFFFF, 0.22f), 2);
        Arc(c, 30, s.slow + 180, 50, Col(0xFFFFFF, 0.12f), 2);
        Circle(c, 11, s.accent);
    }
    Ring(c, 34, Col(0xFFFFFF, 0.14f), 1);
    Circle(c, 3, Col(0x141417));
}

// Wave: the song over threads of light that swing with the sound, each its own wave, in shades of
// blue (the played part bright, the rest faint); the buttons beside the song, the volume under.
void DrawWave(const PlayerStyle& st, const Shown& s) {
    if (StillPart()) {
        Card(st.w, st.h, 20);
        Text(s.song, R(18, 12, st.w - 134, 34), D.fTitle, Col(0xFFFFFF, 0.95f));
        Text(s.where, R(18, 33, st.w - 134, 49), D.fSmall, Col(0xFFFFFF, 0.6f));
        Buttons(st, s);
        Volume(st, s);
    }
    if (!TurningPart()) return;
    static const UINT32 kBlue[] = {0x0288D1, 0x29B6F6, 0x1E88E5, 0x81D4FA, 0x4FC3F7};
    static const float kWaves[] = {1.6f, 2.3f, 1.2f, 3.1f, 2.7f};      // across the player
    static const float kSpeed[] = {3.1f, -2.4f, 1.7f, 4.2f, -3.6f};    // radians a second
    static const float kShare[] = {1, 0.78f, 0.62f, 0.45f, 0.34f};     // of the swing
    static const float kWidth[] = {2.2f, 1.6f, 1.8f, 1.2f, 1.4f};
    const float left = 20, right = st.w - 20, mid = 80, amp = 23, split = left + (right - left) * s.done;
    const float level = std::max(0.04f, std::clamp(s.level, 0.f, 1.f));  // (quiet: nearly flat, still there)
    for (int k = 0; k < 5; k++) {
        ID2D1PathGeometry* g = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(D.d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) {
            Rel(g);
            return;
        }
        const float swing = amp * level * kShare[k] * (0.75f + 0.25f * sinf(s.clock * 0.9f + k * 1.3f));
        for (float x = left; x <= right + 0.01f; x += 3) {
            const float u = (x - left) / (right - left), e = 4 * u - 2;  // e: -2..2
            const float fade = 4 / ((2 + e * e * e * e) * (2 + e * e * e * e));  // (2 / (2 + e^4))^2: 1 in the middle, 0 at the ends
            const D2D1_POINT_2F p = Pt(x, mid + swing * fade * sinf(2 * kPi * kWaves[k] * u + kSpeed[k] * s.clock + k * 1.1f));
            if (x == left) sink->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW);
            else sink->AddLine(p);
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        sink->Release();
        for (int part = 0; part < 2; part++) {
            D.rt->PushAxisAlignedClip(part ? R(split, mid - amp - 4, right + 4, mid + amp + 4) : R(left - 4, mid - amp - 4, split, mid + amp + 4),
                                      D2D1_ANTIALIAS_MODE_ALIASED);
            D.br->SetColor(Col(kBlue[k], part ? 0.3f : 0.95f));
            D.rt->DrawGeometry(g, D.br, kWidth[k], D.round);
            D.rt->PopAxisAlignedClip();
        }
        g->Release();
    }
}

// Radio: an old radio, its speaker's grille, and a tuning dial whose needle moves along the song.
void DrawRadio(const PlayerStyle& st, const Shown& s) {
    Card(st.w, st.h, 20, 0.88f);
    // The grille: rows of holes, round at its ends.
    const D2D1_RECT_F grille = R(16, 16, 112, st.h - 16);
    Fill(grille, Col(0xFFFFFF, 0.04f), 12);
    const float gcx = (grille.left + grille.right) / 2, gcy = (grille.top + grille.bottom) / 2, gr = 40;
    for (float y = gcy - gr + 4; y <= gcy + gr - 3; y += 8)
        for (float x = gcx - gr + 4; x <= gcx + gr - 3; x += 8)
            if (hypotf(x - gcx, y - gcy) <= gr - 2) Circle(Pt(x, y), 1.9f, Col(0xFFFFFF, 0.22f));
    Circle(Pt(gcx, gcy + gr + 14), 3, s.playing ? s.accent : Col(0xFFFFFF, 0.25f));  // the light: on while it plays
    // The dial: a scale with its numbers, the needle where the song is.
    const D2D1_RECT_F dial = R(128, 16, st.w - 16, 62);
    Fill(dial, Col(0x0A0A0C, 0.9f), 9);
    Stroke(dial, Col(0xFFFFFF, 0.12f), 9);
    const float l = dial.left + 14, r = dial.right - 14;
    for (int i = 0; i <= 40; i++) {
        const float x = l + (r - l) * i / 40;
        Line(Pt(x, dial.bottom - 10 - (i % 5 ? 4 : 9)), Pt(x, dial.bottom - 10), Col(0xFFFFFF, i % 5 ? 0.25f : 0.5f), 1);
    }
    static const wchar_t* const kMarks[] = {L"88", L"92", L"96", L"100", L"104", L"108"};
    for (int i = 0; i < 6; i++) {
        const float x = l + (r - l) * i * 8 / 40;
        Text(kMarks[i], R(x - 14, dial.top + 2, x + 14, dial.top + 18), D.fMark, Col(0xFFFFFF, 0.45f));
    }
    const float nx = l + (r - l) * s.done;
    Line(Pt(nx, dial.top + 6), Pt(nx, dial.bottom - 6), s.accent, 2.2f);
    // The song, then the controls.
    Text(s.song, R(130, 70, st.w - 16, 92), D.fTitle, Col(0xFFFFFF, 0.95f));
    Text(s.where, R(130, 91, st.w - 16, 107), D.fSmall, Col(0xFFFFFF, 0.6f));
    Buttons(st, s);
    Volume(st, s);
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
    if (!held && !OnRing(kVinylC, x, y, 26, kVinylR + 2)) return -1;
    return std::clamp((57 - hypotf(x - kVinylC.x, y - kVinylC.y)) / 25, 0.f, 1.f);
}

float SeekCassette(const PlayerStyle& st, float x, float y, bool held) {  // the window: tape from reel to reel
    return held || Inside(x, y, R(80, 62, st.w - 80, 110)) ? Along(x, 92, st.w - 92) : -1;
}

float SeekDisc(const PlayerStyle&, float x, float y, bool held) {  // the ring round the disc
    return held || OnRing(Pt(42, 42), x, y, 26, 44) ? Around(Pt(42, 42), x, y) : -1;
}

float SeekBars(const PlayerStyle& st, float x, float y, bool held) {  // the bars (or threads): lit as far as it's played
    return held || Inside(x, y, R(16, 52, st.w - 16, 108)) ? Along(x, 18, st.w - 18) : -1;
}

float SeekHalo(const PlayerStyle&, float x, float y, bool held) {  // the bars round the cover
    return held || OnRing(kHaloC, x, y, 36, 70) ? Around(kHaloC, x, y) : -1;
}

float SeekCover(const PlayerStyle& st, float x, float y, bool held) {  // its line
    return held || Inside(x, y, R(150, 58, st.w - 14, 76)) ? Along(x, 154, st.w - 18) : -1;
}

float SeekRadio(const PlayerStyle& st, float x, float y, bool held) {  // the dial: the needle
    return held || Inside(x, y, R(128, 16, st.w - 16, 62)) ? Along(x, 142, st.w - 30) : -1;
}

float SeekMini(const PlayerStyle& st, float x, float y, bool held) {  // the line along its bottom edge
    return held || Inside(x, y, R(24, st.h - 12, st.w - 24, st.h)) ? Along(x, 28, st.w - 28) : -1;
}

const PlayerStyle kStyles[] = {
    {L"Vinyl", L"A record that turns", 400, 150, {192, 106}, {232, 106}, {272, 106}, 16, 19, {324, 104.5f, 380, 107.5f}, true, false, {12, 8, 172, 142}, DrawVinyl, SeekVinyl},
    {L"Cassette", L"Tape winds reel to reel", 340, 196, {128, 172}, {170, 172}, {212, 172}, 15, 17, {262, 170.5f, 314, 173.5f}, true, false, {78, 60, 262, 112}, DrawCassette, SeekCassette},
    {L"Disc", L"A disc turning in a ring", 350, 84, {240, 42}, {280, 42}, {320, 42}, 15, 19, {110, 63.5f, 200, 66.5f}, true, false, {10, 10, 74, 74}, DrawDisc, SeekDisc},
    {L"Bars", L"Bars move with the sound", 360, 136, {256, 30}, {294, 30}, {332, 30}, 15, 17, {44, 118.5f, 330, 121.5f}, true, true, {16, 54, 344, 104}, DrawBars, SeekBars},
    {L"Wave", L"Threads swing with the sound", 360, 136, {256, 30}, {294, 30}, {332, 30}, 15, 17, {44, 118.5f, 330, 121.5f}, true, true, {16, 54, 344, 106}, DrawWave, SeekBars},
    {L"Halo", L"Bars round the cover", 400, 150, {188, 106}, {228, 106}, {268, 106}, 16, 19, {320, 104.5f, 380, 107.5f}, true, true, {8, 6, 152, 146}, DrawHalo, SeekHalo},
    {L"Cover", L"The song's cover, big", 380, 150, {174, 106}, {214, 106}, {254, 106}, 16, 19, {304, 104.5f, 362, 107.5f}, false, false, {}, DrawCover, SeekCover},
    {L"Radio", L"A dial follows the song", 340, 170, {152, 138}, {192, 138}, {232, 138}, 15, 17, {276, 136.5f, 324, 139.5f}, false, false, {}, DrawRadio, SeekRadio},
    {L"Mini", L"Just the song, small", 300, 56, {196, 28}, {234, 28}, {272, 28}, 13, 17, {}, false, false, {}, DrawMini, SeekMini},
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
    if (v.right > v.left && x >= v.left - 26 && x <= v.right + 10 && y >= v.top - 12 && y <= v.bottom + 12) return PART_VOLUME;
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
    return std::clamp(sqrtf(peak) * 1.15f, 0.f, 1.f);
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
    W.level = loud > W.level ? W.level + (loud - W.level) * std::min(1.f, dt * 12) : std::max(loud, W.level - dt * 1.2f);
    for (int i = 0; i < kBars; i++) {  // the sound's level, shaped differently for each bar
        const float shape = 0.35f + 0.65f * (0.5f + 0.5f * sinf((float)t * (3.1f + i * 0.37f) + i * 1.7f)) *
                                        (0.6f + 0.4f * sinf((float)t * 1.3f + i * 0.9f));
        const float want = loud * shape * (1 - 0.25f * fabsf(i - kBars / 2.f) / kBars);
        W.bars[i] = want > W.bars[i] ? W.bars[i] + (want - W.bars[i]) * std::min(1.f, dt * 18) : std::max(want, W.bars[i] - dt * 1.6f);
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

// What shows now; seek (0..1): the place being dragged to, in a preview.
Shown Current(float seek = -1) {
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
    s.where = !playing ? L"Paused" + (dur ? L"  \u00B7  " + Minutes(pos) + L" / " + Minutes(dur) : L"")
                       : dur ? Minutes(pos) + L" / " + Minutes(dur) : L"";
    s.playing = playing;
    s.done = dur ? std::min(1.f, (float)pos / dur) : 0;
    s.volume = g_settings.volume;
    s.accent = Accent();
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
    wchar_t key[200];
    swprintf(key, 200, L"%d|%ls|%ls|%d|%d|%d|%d|%d|%d|%.2f|%ls", g_settings.musicPlayer, s.song.c_str(), s.where.c_str(), s.playing, (int)(s.done * 400),
             W.hot, W.press, W.sliding, g_settings.volume, W.scale, W.artFor.c_str());
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
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
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
    Shown s = Current(in ? in->seek : -1);
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
    return Render(st, s, scale, w, h, [&](HDC, const uint32_t* px) {
        out->px.assign(px, px + (size_t)w * h);
        out->w = w;
        out->h = h;
        out->pad = 0;
    });
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
