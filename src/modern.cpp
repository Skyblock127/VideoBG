// Modern look: the wallpaper picker. A full-screen window on the main screen, transparent where
// nothing is drawn: while it's open the renderer shows the live wallpaper just under it (the
// "showcase", above the apps), and a deck of cards rises from the bottom, one per video in the
// library, fanned along an arc. Scrolling through them shows each one's picture at once and plays
// its video after a moment on it; Enter (or a click on the middle card) makes it the wallpaper.
// It also places the desktop clock (drag it where you want it).
//
// Drawn with Direct2D into a DirectComposition swap chain (per-pixel transparency, on the GPU). The
// host's message loop waits on the swap chain while something moves, so animation follows the
// display's refresh and nothing runs while it's still. Everything here (Direct3D, Direct2D,
// DirectWrite, the pictures) is created when the picker opens and released when it closes.
#include "mfhelp.h"
#include "resource.h"
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <dwrite.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <memory>

namespace {

constexpr wchar_t kClass[] = L"VideoBG.Picker";
constexpr UINT WM_PK_INFO = WM_APP + 120;  // lParam = WallpaperInfo* from the library worker
constexpr UINT_PTR TIMER_DWELL = 1, TIMER_MINUTE = 2, TIMER_TOAST = 3, TIMER_HOLD = 4, TIMER_RAM = 5;
constexpr UINT kDwellMs = 1200;  // on a card this long, its video plays
const GUID kShadowEffect = {0xC67EA361, 0x1863, 0x4E69, {0x89, 0xDB, 0x69, 0x5D, 0x3E, 0x9A, 0x5B, 0x6B}};  // CLSID_D2D1Shadow

// Card layout, in card units (scaled with the screen by P.k): a portrait card with the video's
// picture, five rows of facts tinted with its colours, and its name.
constexpr float kCardW = 170, kCardH = 262, kCardR = 16, kPad = 8, kThumbH = 100, kRowH = 18, kRowGap = 3;
constexpr float kLift = 34, kGrow = 0.2f;  // the middle card: raised and bigger

enum Phase { OPENING, BROWSING, SELECTING, CLOSING };
enum Id {
    ID_NONE, ID_MUSIC, ID_CLOCK, ID_MENU, ID_CLOSE, ID_EMPTY_ADD,
    ID_M_SETTINGS, ID_M_ADD, ID_M_LOCK, ID_M_OFF, ID_M_NATIVE,
    ID_A_BIN, ID_A_VERSIONS, ID_A_CROP, ID_A_ZONE, ID_V_MORE, ID_C_RESET, ID_C_FRAME, ID_E_SAVE, ID_E_CANCEL, ID_E_EYE,
    // the clock's settings
    ID_P_BG, ID_P_LIST, ID_P_SIDE, ID_P_SHOW, ID_P_HIDE, ID_P_STYLE_PREV, ID_P_STYLE_NEXT, ID_P_LANG, ID_P_DIG_LATIN, ID_P_DIG_OWN,
    ID_P_SIZE, ID_P_OPACITY, ID_P_GLOW, ID_P_GLOWSIZE,
    ID_P_DATE_PREV, ID_P_DATE_NEXT, ID_P_H12, ID_P_H24, ID_P_PICK_TEXT, ID_P_PICK_GLOW, ID_P_GRID_TEXT, ID_P_GRID_GLOW,
    // the music's
    ID_MU_PREV, ID_MU_PLAY, ID_MU_NEXT, ID_MU_SEEK, ID_MU_VOLUME, ID_MU_SHUFFLE, ID_MU_REPEAT, ID_MU_PICK, ID_MU_LIST,
    ID_MU_STYLE_PREV, ID_MU_STYLE_NEXT, ID_MU_PREVIEW, ID_P_ROTATE,
    ID_MU_DONE, ID_MU_PICK_COL, ID_MU_GRID_COL, ID_MU_OPACITY, ID_MU_CORNERS, ID_MU_RESET, ID_MU_SIZE, ID_MU_PICK_BODY, ID_MU_GRID_BODY,
    ID_MU_COL0,         // + swatch (0: the Windows accent colour)
    ID_MU_BODY0 = 560,  // + swatch (0: the style's own)
    ID_P_COLOR0 = 500,  // + swatch
    ID_P_GLOWC0 = 530,  // + swatch (0: the text's colour)
    ID_P_GRID0 = 700,   // + colour in the grid (40 on: the saved ones)
    ID_V0 = 900,        // + version (0: the original)
    ID_CARD = 1000,     // + card index
    ID_SONG0 = 2000,    // + song (in the playlist)
    ID_GRIP0 = 6000,    // + song: its grip
    ID_SONGX0 = 20000,  // + song: take it off the playlist
    ID_P_LANG0 = 10000, // + language
};

// ---------------------------------------------------------------------------------------
// Graphics, loaded on demand

struct Gfx {
    HMODULE d3dDll = nullptr, dcompDll = nullptr, d2dDll = nullptr, dwDll = nullptr;
    ID3D11Device* dev = nullptr;
    IDXGISwapChain2* sc = nullptr;
    HANDLE wait = nullptr;  // the swap chain is ready for the next frame
    IDCompositionDevice* dcomp = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual* visual = nullptr;
    ID2D1Factory1* d2d = nullptr;
    ID2D1Device* d2dev = nullptr;
    ID2D1DeviceContext* ctx = nullptr;
    ID2D1Bitmap1* back = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    ID2D1StrokeStyle* dash = nullptr;
    IDWriteFactory* dw = nullptr;
    IDWriteTextFormat *fTitle = nullptr, *fBody = nullptr, *fBold = nullptr, *fSmall = nullptr, *fIcon = nullptr, *fSmallIcon = nullptr,
                      *fIconBig = nullptr, *fCardTitle = nullptr, *fCardMeta = nullptr, *fRowLabel = nullptr, *fRowValue = nullptr,
                      *fHero = nullptr, *fWrap = nullptr;
    ID2D1Bitmap* shadow = nullptr;  // a soft shadow for a card (drawn under each)
    ID2D1Bitmap* glow = nullptr;    // the same in white: the chosen card's glow
    ID2D1Geometry* cardHole = nullptr;  // a card without its picture (the chosen one: its video plays there)
    ID2D1Geometry* glowMask = nullptr;  // around a card, all but its picture
    ID2D1Bitmap* logo = nullptr;
} G;

// ---------------------------------------------------------------------------------------
// State

struct Card {
    std::wstring path, title, meta;  // the video (the original of its versions); its name; "MP4 \u00B7 its folder"
    std::wstring play;               // the version it plays: the wallpaper's, if it's one of them, else the original
    UINT playW = 0, playH = 0;       // a lighter version's size (0: the original's)
    ULONGLONG playBytes = 0;
    WallpaperInfo info;
    bool ready = false;              // the worker has answered (info.ok or not)
    bool asked = false, frameAsked = false;
    bool failed = false;             // its video couldn't play
    Crop crop;
    ID2D1Bitmap* thumb = nullptr;
    ID2D1Bitmap* frame = nullptr;    // the whole frame, for the backdrop (kept for the focused card and its neighbours)
    ID2D1BitmapBrush* brush = nullptr;
    std::wstring rows[5][2];         // label, value
    IDWriteTextLayout* text[16] = {};  // its words, laid out once
    ID2D1Bitmap1* image = nullptr;   // the whole card drawn once, at the focused card's size: each frame just places it
};

struct Still {  // a card's picture full screen, over the live wallpaper
    int card = -1;
    float a = 0;
};

struct Hit {
    int id;
    D2D1_RECT_F r;
};

// The clock's styles on offer: the ones whose fonts are on this PC.
std::vector<ClockStyleInfo> ReadyStyles() {
    std::vector<ClockStyleInfo> v;
    for (const ClockStyleInfo& s : Clock_Styles())
        if (s.font.ready) v.push_back(s);
    return v;
}

struct Picker {
    HWND hwnd = nullptr;
    int mode = 0;                     // 0 picking, 1 the clock, 2 cropping
    RECT mon{};
    float scale = 1, W = 0, H = 0;    // DIPs
    float k = 1;                      // card size: screens of every size get a deck that fits
    Phase phase = OPENING;
    double phaseAt = 0, last = 0;
    bool dirty = true;
    std::vector<Card> cards;
    int applied = -1;                 // the wallpaper's card
    float scroll = 0, scrollV = 0, scrollTo = 0;
    int focus = 0;
    float wheel = 0;
    // What shows behind the deck: the live wallpaper (the showcase), a card's picture over it, and
    // black over both (opening and closing dip through it).
    float dark = 0, darkTo = 1;
    Still stills[2];
    std::wstring live;                // the video the showcase plays
    std::wstring previewAsked;        // the video it was last asked to play
    UINT previewSeq = 0;              // that request's number (the renderer answers each, in order)
    bool showcaseAsked = false, showcaseUp = false;
    float chrome = 0;                 // the top bar and the rest of the controls
    float enter = 0;                  // 0..1+: the cards rising
    // Input
    std::vector<Hit> hits;
    int hot = ID_NONE, press = ID_NONE;
    int hotCard = -1;                 // the card under the pointer: it lifts a little
    std::vector<float> lift;          // each card's hover lift, 0..1 (animated)
    bool down = false, dragging = false;
    POINT downAt{};
    float downScroll = 0;
    double dragT = 0;
    float dragX = 0, flingV = 0;
    bool menu = false;
    float menuA = 0;
    // The middle card's actions (right-click): the bin, and its versions
    bool actions = false, versions = false;
    int actionsCard = -1;
    float actionsA = 0, versionsA = 0;
    bool binArmed = false;            // clicked once: a tick now, a second click takes the card off
    float binTick = 0;
    std::vector<LightCopy> versionList;  // the card's lighter versions
    // Cropping (mode 2): the wallpaper's crop and its picture's moment, right on the screen
    Crop crop, cropWas;
    bool cropDrag = false;
    float cropX = 0, cropY = 0;       // the pointer, last seen while dragging
    double frameTime = 0;
    bool frameMoved = false;
    bool held = false;                // the wallpaper waits on the frame being picked
    bool turnOff = false;             // the wallpaper goes off once the picker has closed
    bool dialog = false;              // a dialog of ours is up: losing the focus doesn't close
    std::wstring toast;
    bool toastOn = false;
    float toastA = 0;
    // Choosing
    int chosen = -1;
    double chosenAt = 0;
    D2D1_POINT_2F chosenFrom{};
    float chosenRot = 0, chosenScale = 1;
    bool used = false;
    bool chosenClose = false;         // closing once the chosen video fills the screen (no dip to black)
    double holeAt = 0;                // when its picture gave way to its video, playing in its place
    double revealAt = 0;              // when the card started fading away around its playing picture
    int lookSent = 0;                 // how much of the choosing the renderer has been told (SendLook)
    // Clock
    ClockLook look;                   // the clock that shows (the focused card's, or the one being placed)
    std::wstring clockKey, clockStyle;  // what the clock's picture shows; its style
    ID2D1Bitmap* clockBmp = nullptr;
    SYSTEMTIME clockTime{};
    double flipAt = 0;                // a flip clock's flaps started falling (0: they don't)
    float clockW = 0, clockH = 0;     // DIPs
    bool clockDrag = false, clockHot = false, snapX = false, snapY = false;
    bool rotating = false;            // its handle is held: the clock turns
    float rotFrom = 0, rotPointer = 0;  // its angle when taken, and the pointer's
    float grabX = 0, grabY = 0;
    std::wstring clockFor;            // the video whose clock it is: its name
    std::wstring clockVideo;          // and its file (the middle card's when it started)
    std::wstring editPlay;            // the video the clock or music screen shows (the middle card's)
    // Its settings, in a panel beside it
    int slide = ID_NONE;              // the slider being dragged
    std::vector<Hit> tracks;          // each slider's track (for the value under the pointer)
    float panelPos = 0, panelV = 0;   // 0 on the right, 1 on the left: springs to panelSide
    int panelSide = 0;
    bool panelHidden = false;         // the eye: just the clock on the wallpaper, to take a look
    float panelA = 0, hideA = 0;
    std::vector<ClockStyleInfo> styles;  // the styles on offer
    bool langsOpen = false;           // the languages' list
    float langScroll = 0;
    std::vector<ClockLanguage> langs;
    int gridFor = 0;                  // any colour's grid, open for the text (1) or the glow (2)
    int picking = 0;                  // the eyedropper, for the text (1) or the glow (2)
    float pickA = 0;
    std::wstring pickWas;             // the colour before it
    D2D1_POINT_2F pickAt{};
    UINT32 pickRgb = 0;
    double pickTime = 0;
    std::vector<UINT32> swatches;     // the colours on offer
    // Music
    bool music = false;               // its dropdown is open
    float musicA = 0;
    std::vector<std::wstring> songs;  // the playlist
    float seekFrac = -1;              // while its place is dragged
    D2D1_RECT_F songView{};           // the playlist
    float songScroll = 0;
    int songDrag = -1, songIns = 0;   // a song being moved, and where it would land
    float songGrab = 0, songY = 0;
    ID2D1Bitmap* player = nullptr;    // the desktop player's style, as it looks (redrawn while it turns)
    int playerStyle = 0;
    double playerAt = 0;
    std::wstring ram;                 // "53 MB \u00B7 43 MB": the wallpaper's RAM, the tray app's (beside the title)
    // The player's picture is its controller too: what the pointer is over or holds there
    D2D1_RECT_F playerBox{};          // where it's drawn
    float playerK = 1;                // DIPs here per DIP of the style
    PlayerInput pv;
    bool pvSeeking = false;
    bool pvMoving = false;            // the music screen: the player is being dragged to a new place
    D2D1_POINT_2F pvGrab{};           // where it was taken, from its centre
    // How long drawing takes (logged when it closes)
    int frames = 0;
    double drawTime = 0, drawMax = 0;
} P;

double Now() {
    static double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart / freq;
}

float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float EaseOut(float t) { t = Clamp01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }
float EaseInOut(float t) { t = Clamp01(t); return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2; }
float EaseBack(float t) {  // overshoots a little, then settles
    t = Clamp01(t);
    const float c1 = 1.4f, c3 = c1 + 1;
    return 1 + c3 * powf(t - 1, 3) + c1 * powf(t - 1, 2);
}
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// A critically damped spring towards `to` (stable at any frame time).
void Spring(float& x, float& v, float to, float dt, float omega) {
    const float f = 1 + 2 * dt * omega, oo = omega * omega, hoo = dt * oo, hhoo = dt * hoo, inv = 1 / (f + hhoo);
    const float nx = (f * x + dt * v + hhoo * to) * inv;
    v = (v + hoo * (to - x)) * inv;
    x = nx;
}

// Moves v towards to at `rate` per second; true while it's still moving.
bool Approach(float& v, float to, float rate, float dt) {
    if (v == to) return false;
    const float step = rate * dt;
    v = fabsf(to - v) <= step ? to : v + (to > v ? step : -step);
    return true;
}

D2D1_COLOR_F Col(UINT32 rgb, float a = 1) {
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

D2D1_POINT_2F Apply(const D2D1_MATRIX_3X2_F& m, D2D1_POINT_2F p) {
    return D2D1::Point2F(p.x * m._11 + p.y * m._21 + m._31, p.x * m._12 + p.y * m._22 + m._32);
}

bool Invert(D2D1_MATRIX_3X2_F& m) {
    const float det = m._11 * m._22 - m._12 * m._21;
    if (fabsf(det) < 1e-12f) return false;
    const D2D1_MATRIX_3X2_F o = m;
    m._11 = o._22 / det;
    m._12 = -o._12 / det;
    m._21 = -o._21 / det;
    m._22 = o._11 / det;
    m._31 = (o._21 * o._32 - o._22 * o._31) / det;
    m._32 = (o._12 * o._31 - o._11 * o._32) / det;
    return true;
}
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// The Windows accent, in its lighter shade (what Windows uses on dark backgrounds).
D2D1_COLOR_F Accent(float a = 1) {
    BYTE pal[32] = {};
    DWORD size = sizeof pal;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                     RRF_RT_REG_BINARY, nullptr, pal, &size) == ERROR_SUCCESS && size >= 32)
        return D2D1::ColorF(pal[4] / 255.f, pal[5] / 255.f, pal[6] / 255.f, a);
    return Col(0x60CDFF, a);
}
D2D1_COLOR_F g_accent;
int g_panelSide = 0;  // the clock's settings: on the right (0) or the left (1), kept while VideoBG runs
D2D1_COLOR_F operator*(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

// ---------------------------------------------------------------------------------------
// Drawing helpers (DIPs)

void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius = 0) {
    G.br->SetColor(c);
    if (radius > 0) G.ctx->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), G.br);
    else G.ctx->FillRectangle(r, G.br);
}

void Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius, float width, ID2D1StrokeStyle* style = nullptr) {
    G.br->SetColor(c);
    G.ctx->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), G.br, width, style);
}

// Text that's drawn every frame in the same box (a card's): laid out once into `slot`.
void CachedText(IDWriteTextLayout*& slot, const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, D2D1_COLOR_F c,
                DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    if (s.empty() || !f) return;
    if (!slot) {
        f->SetTextAlignment(align);
        if (FAILED(G.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, r.right - r.left, r.bottom - r.top, &slot))) return;
    }
    G.br->SetColor(c);
    G.ctx->DrawTextLayout(D2D1::Point2F(r.left, r.top), slot, G.br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Text(const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, D2D1_COLOR_F c,
          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    if (s.empty() || !f) return;
    f->SetTextAlignment(align);
    G.br->SetColor(c);
    G.ctx->DrawText(s.c_str(), (UINT32)s.size(), f, r, G.br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
    IDWriteTextLayout* l = nullptr;
    float w = 0;
    if (SUCCEEDED(G.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 4000, 100, &l))) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
        l->Release();
    }
    return w;
}

// A pill-shaped button: icon and/or label on a dark glass background. Registered for hit-testing.
void Pill(int id, const D2D1_RECT_F& r, const wchar_t* icon, const std::wstring& label, float a, bool accent = false, wchar_t key = 0) {
    const bool hot = P.hot == id, pressed = P.press == id && hot;
    D2D1_COLOR_F bg = accent ? g_accent : Col(0x18181B, 0.72f);
    if (accent && hot) bg = D2D1::ColorF(std::min(1.f, bg.r * 1.08f + 0.04f), std::min(1.f, bg.g * 1.08f + 0.04f), std::min(1.f, bg.b * 1.08f + 0.04f));
    if (!accent && hot) bg = Col(0x2A2A2F, 0.86f);
    if (pressed) bg.a *= 0.85f;
    bg.a *= a;
    const float h = r.bottom - r.top;
    Fill(r, bg, h / 2);
    if (!accent) Stroke(r, Col(0xFFFFFF, 0.09f * a), h / 2, 1);
    const D2D1_COLOR_F fg = accent ? Col(0x000000, 0.88f * a) : Col(0xFFFFFF, 0.94f * a);
    float x = r.left + (label.empty() ? 0 : 14);
    if (icon) {
        const D2D1_RECT_F ir = label.empty() ? r : R(x, r.top, x + 18, r.bottom);
        Text(icon, ir, G.fIcon, fg, DWRITE_TEXT_ALIGNMENT_CENTER);
        x += 24;
    }
    if (!label.empty()) Text(label, R(x, r.top, r.right - (key ? 36 : 12), r.bottom), G.fBold, fg);
    if (key) {  // its key, on a small keycap at the end
        const float cy = (r.top + r.bottom) / 2;
        const D2D1_RECT_F cap = R(r.right - 32, cy - 9.5f, r.right - 13, cy + 9.5f);
        Stroke(cap, Col(0xFFFFFF, 0.32f * a), 5, 1);
        Text(std::wstring(1, key), cap, G.fSmall, Col(0xFFFFFF, 0.7f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (id) P.hits.push_back({id, r});
}

float PillWidth(const wchar_t* icon, const std::wstring& label, wchar_t key = 0) {
    return (label.empty() ? 36 : 14 + (icon ? 24 : 0) + TextWidth(label, G.fBold) + 16) + (key ? 24 : 0);
}

// Formats with vertical centring; DirectWrite falls back to Segoe UI where a font is missing.
IDWriteTextFormat* Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, bool ellipsis = true) {
    IDWriteTextFormat* f = nullptr;
    if (FAILED(G.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f)))
        return nullptr;
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis) {
        IDWriteInlineObject* dots = nullptr;
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        if (SUCCEEDED(G.dw->CreateEllipsisTrimmingSign(f, &dots))) {
            f->SetTrimming(&trim, dots);
            dots->Release();
        }
    }
    return f;
}

// ---------------------------------------------------------------------------------------
// Setup and teardown

template <class F> F Proc(HMODULE m, const char* name) { return m ? (F)(void*)GetProcAddress(m, name) : nullptr; }

// A soft shadow (or glow) the size of a card, made once with Direct2D's shadow effect.
ID2D1Bitmap* MakeShadow(D2D1_COLOR_F color, float blur) {
    const float pad = blur * 3;
    ID2D1Bitmap1* shape = nullptr;
    ID2D1Bitmap1* out = nullptr;
    ID2D1Effect* fx = nullptr;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    const D2D1_SIZE_U size = D2D1::SizeU((UINT32)(kCardW + 2 * pad), (UINT32)(kCardH + 2 * pad));
    if (FAILED(G.ctx->CreateBitmap(size, nullptr, 0, &bp, &shape)) || FAILED(G.ctx->CreateBitmap(size, nullptr, 0, &bp, &out)) ||
        FAILED(G.ctx->CreateEffect(kShadowEffect, &fx))) {
        SafeRelease(shape);
        SafeRelease(out);
        return nullptr;
    }
    ID2D1Image* old = nullptr;
    G.ctx->GetTarget(&old);
    const float oldDpiX = 96, oldDpiY = 96;
    float dx, dy;
    G.ctx->GetDpi(&dx, &dy);
    G.ctx->SetDpi(oldDpiX, oldDpiY);
    G.ctx->SetTarget(shape);
    G.ctx->BeginDraw();
    G.ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    Fill(R(pad, pad, pad + kCardW, pad + kCardH), Col(0x000000), kCardR);
    G.ctx->EndDraw();
    fx->SetInput(0, shape);
    fx->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, D2D1_PROPERTY_TYPE_FLOAT, (const BYTE*)&blur, sizeof blur);
    D2D1_VECTOR_4F c{color.r, color.g, color.b, color.a};
    fx->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1_PROPERTY_TYPE_VECTOR4, (const BYTE*)&c, sizeof c);
    G.ctx->SetTarget(out);
    G.ctx->BeginDraw();
    G.ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    G.ctx->DrawImage(fx);
    G.ctx->EndDraw();
    G.ctx->SetTarget(old);
    G.ctx->SetDpi(dx, dy);
    SafeRelease(old);
    fx->Release();
    shape->Release();
    return out;
}

ID2D1Bitmap* BitmapFrom(const std::vector<uint32_t>& px, int w, int h, bool premultiplied = false) {
    if (px.empty() || w <= 0 || h <= 0) return nullptr;
    ID2D1Bitmap* b = nullptr;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, premultiplied ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE));
    G.ctx->CreateBitmap(D2D1::SizeU((UINT32)w, (UINT32)h), px.data(), (UINT32)w * 4, bp, &b);
    return b;
}

ID2D1Bitmap* LogoBitmap() {
    HICON ico = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 64, 64, 0);
    if (!ico) return nullptr;
    IWICImagingFactory* wic = nullptr;
    IWICBitmap* wb = nullptr;
    IWICFormatConverter* conv = nullptr;
    ID2D1Bitmap* b = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateBitmapFromHICON(ico, &wb)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(wb, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        G.ctx->CreateBitmapFromWicBitmap(conv, nullptr, &b);
    SafeRelease(conv);
    SafeRelease(wb);
    SafeRelease(wic);
    DestroyIcon(ico);
    return b;
}

bool CreateGraphics() {
    // Direct3D and DirectComposition stay loaded once used: their own threads may outlive our
    // objects, so unloading them could pull the code out from under those threads.
    if (!G.d3dDll) G.d3dDll = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!G.dcompDll) G.dcompDll = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    G.d2dDll = LoadLibraryExW(L"d2d1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    G.dwDll = LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto createDevice = Proc<decltype(&D3D11CreateDevice)>(G.d3dDll, "D3D11CreateDevice");
    auto createDcomp = Proc<HRESULT(WINAPI*)(IDXGIDevice*, REFIID, void**)>(G.dcompDll, "DCompositionCreateDevice");
    auto createD2d = Proc<HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**)>(G.d2dDll, "D2D1CreateFactory");
    auto createDw = Proc<HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**)>(G.dwDll, "DWriteCreateFactory");
    if (!createDevice || !createDcomp || !createD2d || !createDw) return false;

    const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, (UINT)std::size(fl),
                              D3D11_SDK_VERSION, &G.dev, nullptr, nullptr);
    if (FAILED(hr))
        hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, (UINT)std::size(fl),
                          D3D11_SDK_VERSION, &G.dev, nullptr, nullptr);
    if (FAILED(hr)) return false;
    IDXGIDevice* dd = nullptr;
    IDXGIAdapter* da = nullptr;
    IDXGIFactory2* fac = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    G.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd);
    if (dd) dd->GetAdapter(&da);
    if (da) da->GetParent(__uuidof(IDXGIFactory2), (void**)&fac);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = (UINT)(P.mon.right - P.mon.left);
    sd.Height = (UINT)(P.mon.bottom - P.mon.top);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    bool ok = fac && SUCCEEDED(fac->CreateSwapChainForComposition(G.dev, &sd, nullptr, &sc1)) &&
              SUCCEEDED(sc1->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&G.sc));
    if (ok) {
        G.sc->SetMaximumFrameLatency(1);
        G.wait = G.sc->GetFrameLatencyWaitableObject();
    }
    ok = ok && SUCCEEDED(createDcomp(dd, __uuidof(IDCompositionDevice), (void**)&G.dcomp)) &&
         SUCCEEDED(G.dcomp->CreateTargetForHwnd(P.hwnd, TRUE, &G.target)) && SUCCEEDED(G.dcomp->CreateVisual(&G.visual)) &&
         SUCCEEDED(G.visual->SetContent(G.sc)) && SUCCEEDED(G.target->SetRoot(G.visual)) && SUCCEEDED(G.dcomp->Commit());
    D2D1_FACTORY_OPTIONS fo{};
    ok = ok && SUCCEEDED(createD2d(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)&G.d2d)) &&
         SUCCEEDED(G.d2d->CreateDevice(dd, &G.d2dev)) && SUCCEEDED(G.d2dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &G.ctx));
    IDXGISurface* surf = nullptr;
    if (ok) {
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                             96 * P.scale, 96 * P.scale);
        ok = SUCCEEDED(G.sc->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surf)) &&
             SUCCEEDED(G.ctx->CreateBitmapFromDxgiSurface(surf, &bp, &G.back));
    }
    SafeRelease(surf);
    SafeRelease(sc1);
    SafeRelease(fac);
    SafeRelease(da);
    SafeRelease(dd);
    ok = ok && SUCCEEDED(createDw(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&G.dw));
    if (!ok) return false;
    G.ctx->SetTarget(G.back);
    G.ctx->SetDpi(96 * P.scale, 96 * P.scale);
    G.ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    G.ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &G.br);
    const float dashes[] = {3, 3};
    G.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                          D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_CUSTOM, 0),
                             dashes, 2, &G.dash);
    const wchar_t* ui = L"Segoe UI Variable Text";
    const wchar_t* display = L"Segoe UI Variable Display";
    const wchar_t* icons = L"Segoe Fluent Icons";
    G.fHero = Format(display, 26, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    G.fTitle = Format(display, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    G.fBody = Format(ui, 13, DWRITE_FONT_WEIGHT_NORMAL);
    G.fBold = Format(ui, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    G.fSmall = Format(ui, 11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    G.fIcon = Format(icons, 14, DWRITE_FONT_WEIGHT_NORMAL, false);
    G.fSmallIcon = Format(icons, 11, DWRITE_FONT_WEIGHT_NORMAL, false);
    G.fIconBig = Format(icons, 20, DWRITE_FONT_WEIGHT_NORMAL, false);
    G.fCardTitle = Format(display, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    G.fCardMeta = Format(ui, 10.5f, DWRITE_FONT_WEIGHT_NORMAL);
    G.fRowLabel = Format(ui, 8.5f, DWRITE_FONT_WEIGHT_BOLD);
    G.fRowValue = Format(ui, 9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if ((G.fWrap = Format(ui, 12, DWRITE_FONT_WEIGHT_NORMAL, false))) {
        G.fWrap->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        G.fWrap->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    G.shadow = MakeShadow(Col(0x000000, 0.75f), 14);
    G.glow = MakeShadow(Col(0xFFFFFF, 0.9f), 10);
    {
        ID2D1RoundedRectangleGeometry *card = nullptr, *pic = nullptr;
        ID2D1RectangleGeometry* around = nullptr;
        const D2D1_RECT_F c = R(-kCardW / 2, -kCardH / 2, kCardW / 2, kCardH / 2);
        const D2D1_RECT_F p = R(c.left + kPad, c.top + kPad, c.right - kPad, c.top + kPad + kThumbH);
        G.d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(c, kCardR, kCardR), &card);
        G.d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(p, 10, 10), &pic);
        G.d2d->CreateRectangleGeometry(R(-kCardW * 2, -kCardH * 2, kCardW * 2, kCardH * 2), &around);
        ID2D1GeometryGroup* group = nullptr;
        if (card && pic) {
            ID2D1Geometry* parts[2] = {card, pic};
            if (SUCCEEDED(G.d2d->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE, parts, 2, &group))) G.cardHole = group;
        }
        if (around && pic) {
            ID2D1Geometry* parts[2] = {around, pic};
            if (SUCCEEDED(G.d2d->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE, parts, 2, &group))) G.glowMask = group;
        }
        SafeRelease(card);
        SafeRelease(pic);
        SafeRelease(around);
    }
    G.logo = LogoBitmap();
    return true;
}

void ReleaseCardText(Card& c) {
    for (auto*& l : c.text) SafeRelease(l);
}

void ReleaseCardBitmaps(Card& c) {
    SafeRelease(c.image);
    SafeRelease(c.brush);
    SafeRelease(c.thumb);
    SafeRelease(c.frame);
    ReleaseCardText(c);
}

void ReleaseGraphics() {
    for (auto& c : P.cards) ReleaseCardBitmaps(c);
    SafeRelease(P.clockBmp);
    SafeRelease(P.player);
    P.clockKey.clear();
    P.clockStyle.clear();
    for (IDWriteTextFormat** f : {&G.fHero, &G.fTitle, &G.fBody, &G.fBold, &G.fSmall, &G.fIcon, &G.fSmallIcon, &G.fIconBig, &G.fCardTitle, &G.fCardMeta,
                                  &G.fRowLabel, &G.fRowValue, &G.fWrap})
        SafeRelease(*f);
    SafeRelease(G.logo);
    SafeRelease(G.cardHole);
    SafeRelease(G.glowMask);
    SafeRelease(G.glow);
    SafeRelease(G.shadow);
    SafeRelease(G.dash);
    SafeRelease(G.br);
    SafeRelease(G.dw);
    if (G.ctx) G.ctx->SetTarget(nullptr);
    SafeRelease(G.back);
    SafeRelease(G.ctx);
    SafeRelease(G.d2dev);
    SafeRelease(G.d2d);
    SafeRelease(G.visual);
    SafeRelease(G.target);
    SafeRelease(G.dcomp);
    if (G.wait) CloseHandle(G.wait);
    G.wait = nullptr;
    SafeRelease(G.sc);
    SafeRelease(G.dev);
    for (HMODULE* m : {&G.d2dDll, &G.dwDll}) {
        if (*m) FreeLibrary(*m);
        *m = nullptr;
    }
}

// ---------------------------------------------------------------------------------------
// Cards

std::wstring FileTitle(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
}

std::wstring FileMeta(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    std::wstring ext = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? path.substr(dot + 1) : L"";
    for (auto& ch : ext) ch = towupper(ch);
    std::wstring folder;
    if (slash != std::wstring::npos && slash > 0) {
        size_t prev = path.find_last_of(L"\\/", slash - 1);
        folder = path.substr(prev == std::wstring::npos ? 0 : prev + 1, slash - (prev == std::wstring::npos ? 0 : prev + 1));
    }
    return ext.empty() ? folder : folder.empty() ? ext : ext + L"  \u00B7  " + folder;
}

std::wstring Duration(double s) {
    const int t = (int)lround(s);
    wchar_t b[32];
    if (t >= 3600) swprintf(b, 32, L"%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else swprintf(b, 32, L"%d:%02d", t / 60, t % 60);
    return b;
}

std::wstring Bytes(ULONGLONG n) {
    wchar_t b[32];
    if (n >= (1ULL << 30)) swprintf(b, 32, L"%.1f GB", n / (double)(1ULL << 30));
    else if (n >= (1ULL << 20)) swprintf(b, 32, L"%.1f MB", n / (double)(1ULL << 20));
    else swprintf(b, 32, L"%llu KB", (unsigned long long)(n >> 10));
    return b;
}

void FillRows(Card& c) {
    const WallpaperInfo& i = c.info;
    wchar_t b[64];
    const UINT32 vw = c.playW ? c.playW : i.vw, vh = c.playW ? c.playH : i.vh;  // the version it plays
    const UINT32 big = std::max(vw, vh), small = std::min(vw, vh);  // 4K, else its lines: 1440p, 1080p, 900p...
    if (big >= 3800) swprintf(b, 64, L"4K  \u00B7  %u\u00D7%u", vw, vh);
    else if (small >= 240) swprintf(b, 64, L"%up  \u00B7  %u\u00D7%u", small, vw, vh);
    else swprintf(b, 64, L"%u\u00D7%u", vw, vh);
    c.rows[0][0] = L"RESOLUTION";
    c.rows[0][1] = b;
    swprintf(b, 64, i.fps > 0 ? L"%ls  \u00B7  %.0f fps" : L"%ls", Duration(i.duration).c_str(), i.fps);
    c.rows[1][0] = L"LENGTH";
    c.rows[1][1] = b;
    c.rows[2][0] = L"CODEC";
    c.rows[2][1] = i.codec.empty() || i.codec == L"?" ? L"Video" : i.codec;
    c.rows[3][0] = L"SOUND";
    c.rows[3][1] = i.sound ? L"Has sound" : L"Silent";
    c.rows[4][0] = L"FILE";
    c.rows[4][1] = Bytes(c.playBytes ? c.playBytes : i.bytes);
}

int CardIndex(const std::wstring& path) {
    for (size_t i = 0; i < P.cards.size(); i++)
        if (_wcsicmp(P.cards[i].path.c_str(), path.c_str()) == 0) return (int)i;
    return -1;
}

bool SameFile(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

Card* WallpaperCard() { return P.applied >= 0 && P.applied < (int)P.cards.size() ? &P.cards[P.applied] : nullptr; }

// A lighter version's original ("" for anything else). Only files in VideoBG's light copies folder
// are looked up (that reads the file).
std::wstring OriginalOfCopy(const std::wstring& path) {
    static const std::wstring dir = LocalDataDir() + L"\\Light copies\\";
    if (path.size() <= dir.size() || _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0) return L"";
    return LightCopyOriginalPath(path);
}

Card NewCard(const std::wstring& path) {
    Card c;
    c.path = c.play = path;
    c.title = FileTitle(path);
    c.meta = FileMeta(path);
    c.crop = LoadCropFor(path);
    return c;
}

// The version a card plays; its size (and file size) show on the card.
void SetPlay(Card& c, const std::wstring& path) {
    c.play = path;
    c.playW = c.playH = 0;
    c.playBytes = 0;
    if (!SameFile(path, c.path)) {  // a lighter version: "<name> (<w>x<h>).mp4"
        const size_t open = path.find_last_of(L'(');
        unsigned w = 0, h = 0;
        if (open != std::wstring::npos && swscanf(path.c_str() + open, L"(%ux%u)", &w, &h) == 2) {
            c.playW = w;
            c.playH = h;
        }
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) c.playBytes = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    }
    if (c.info.ok) FillRows(c);
    ReleaseCardText(c);
    SafeRelease(c.image);
}

// One card per video: a lighter version joins its original's card, which plays it if it's the wallpaper.
void LoadCards() {
    P.cards.clear();
    P.applied = -1;
    for (const std::wstring& p : LoadWallpapers(g_settings.video)) {
        const std::wstring orig = OriginalOfCopy(p);
        const std::wstring& key = orig.empty() ? p : orig;
        int i = CardIndex(key);
        if (i < 0) {
            P.cards.push_back(NewCard(key));
            i = (int)P.cards.size() - 1;
        }
        if (SameFile(p, g_settings.video)) {
            SetPlay(P.cards[i], p);
            P.applied = i;
        }
    }
    for (int i = 0; i < (int)P.cards.size(); i++) {  // the others: the version last picked for each
        if (i == P.applied) continue;
        const std::wstring v = VideoVersion(P.cards[i].path);
        if (!v.empty()) SetPlay(P.cards[i], v);
    }
}

void SaveCards() {
    std::vector<std::wstring> list;
    for (auto& c : P.cards) list.push_back(c.path);
    SaveWallpapers(list);
}

// Pictures for the cards on screen first, and the focused card's frame (plus its neighbours',
// for quick scrolling); frames far from the focus are let go.
void AskPictures() {
    const int n = (int)P.cards.size();
    if (!n) return;
    auto each = [&](int r, auto fn) {
        for (int side : {1, -1}) {
            const int i = P.focus + side * r;
            if (i >= 0 && i < n && !(r == 0 && side < 0)) fn(P.cards[i]);
        }
    };
    // Every card's small picture, nearest first; then the big ones near the focus, which jump the
    // queue (the focused card's last, so it's at the very front).
    for (int r = 0; r < n; r++)
        each(r, [&](Card& c) {
            if (!c.asked) {
                Library_Request(c.path, r <= 2, false);
                c.asked = true;
                c.frameAsked = r <= 2;
            }
        });
    for (int r = 2; r >= 0; r--)
        each(r, [&](Card& c) {
            if (!c.frame && (!c.frameAsked || r == 0) && (!c.ready || c.info.ok)) {
                Library_Request(c.path, true, r <= 1);
                c.frameAsked = true;
            }
        });
    for (int r = 5; r < n; r++)  // far away now: their big pictures can go
        each(r, [&](Card& c) {
            if (c.frame) {
                SafeRelease(c.frame);
                c.frameAsked = false;
            }
        });
}

void OnInfo(WallpaperInfo* raw) {
    std::unique_ptr<WallpaperInfo> info(raw);
    const int i = CardIndex(info->path);
    if (i < 0) return;
    Card& c = P.cards[i];
    c.ready = true;
    if (!info->ok) {
        c.info.fault = info->fault;
        c.info.ok = false;
        ReleaseCardText(c);
        SafeRelease(c.image);
        P.dirty = true;
        return;
    }
    if (!c.thumb) {
        c.thumb = BitmapFrom(info->thumb, info->tw, info->th);
        SafeRelease(c.brush);
    }
    if (!info->frame.empty() && !c.frame) c.frame = BitmapFrom(info->frame, info->fw, info->fh);
    info->thumb.clear();
    info->frame.clear();
    c.info = std::move(*info);
    c.info.ok = true;
    FillRows(c);
    ReleaseCardText(c);  // laid out again with the new words
    SafeRelease(c.image);
    P.dirty = true;
}

// ---------------------------------------------------------------------------------------
// The clock

// The look the desktop clock has with this video: its own, or the one all videos share.
ClockLook LookFor(const std::wstring& path) {
    if (path.empty()) return g_settings.clockLive;
    Settings s = g_settings;
    s.video = path;
    LoadVideoProfile(s);
    return s.clockVideoOwn ? s.clockVideo : s.clockLive;
}

// The clock's picture at P.clockTime (flip: a flip clock's flaps that far over to it).
void PaintClock(float flip = 1) {
    ClockImage img;
    if (!Clock_Paint(P.look, P.scale, P.clockTime, &img, flip)) return;
    SafeRelease(P.clockBmp);
    P.clockBmp = BitmapFrom(img.px, img.w, img.h, true);
    P.clockW = img.w / P.scale;
    P.clockH = img.h / P.scale;
}

void UpdateClock() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t when[64];
    swprintf(when, 64, L"|%.3f|%04d%02d%02d%02d%02d", P.scale, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    const bool shown = P.look.show || P.mode == 1;  // placing it: hidden ones too
    const std::wstring style = shown ? Clock_StyleKey(P.look) : L"", key = shown ? style + when : L"";
    if (key == P.clockKey) return;
    const bool flip = !key.empty() && style == P.clockStyle && Clock_Flips(P.look);  // just the minute: its flaps fall
    P.clockKey = key;
    P.clockStyle = style;
    P.clockTime = t;
    P.flipAt = flip ? Now() : 0;
    if (key.empty()) SafeRelease(P.clockBmp);
    else PaintClock(flip ? 0 : 1);
}

D2D1_RECT_F ClockRect() {
    const float x = P.look.x * P.W, y = P.look.y * P.H;
    return R(x - P.clockW / 2, y - P.clockH / 2, x + P.clockW / 2, y + P.clockH / 2);
}

// ---------------------------------------------------------------------------------------
// The showcase and the preview

// Plays `path` on the showcase (the wallpaper's own video: back to it).
void AskPreview(const std::wstring& path, double time) {
    if (_wcsicmp(path.c_str(), P.previewAsked.c_str()) == 0) return;
    P.previewAsked = path;
    const bool own = _wcsicmp(path.c_str(), g_settings.video.c_str()) == 0;
    Host_PreviewVideo(own ? L"" : path, time, ++P.previewSeq);
}

void AskShowcase() {
    if (P.showcaseAsked || !P.hwnd) return;
    P.showcaseAsked = true;
    Host_Showcase(P.hwnd);
}

// The card under focus is the one to look at: its picture straight away, its video after a moment.
void FocusChanged() {
    P.actions = P.versions = P.binArmed = false;
    AskPictures();
    if (P.focus < 0 || P.focus >= (int)P.cards.size()) return;
    Card& c = P.cards[P.focus];
    if (P.mode == 0 && P.phase != CLOSING) {
        P.look = LookFor(c.path);
        UpdateClock();
    }
    // Its picture goes over whatever shows now, unless the live wallpaper is it already. Only one
    // picture fades in at a time: one still fading in takes the new picture instead (scrolling fast
    // never stacks several half-faded wallpapers).
    const bool isLive = P.showcaseUp && SameFile(c.play, P.live);
    Still& top = P.stills[1];
    if (!isLive && top.card != P.focus) {
        if (top.card >= 0 && top.a >= 0.999f) {  // all there: it's what the new one fades in over
            P.stills[0] = Still{top.card, 1};
            top = Still{P.focus, 0};
        } else if (top.card >= 0 && top.a > 0.01f) {
            top.card = P.focus;
        } else {
            top = Still{P.focus, 0};
        }
    }
    KillTimer(P.hwnd, TIMER_DWELL);
    if (!isLive) SetTimer(P.hwnd, TIMER_DWELL, kDwellMs, nullptr);
    P.dirty = true;
}

void Dwelled() {
    KillTimer(P.hwnd, TIMER_DWELL);
    if ((P.phase != BROWSING && P.phase != OPENING) || P.focus < 0 || P.focus >= (int)P.cards.size()) return;
    Card& c = P.cards[P.focus];
    if (c.failed || (c.ready && !c.info.ok)) return;
    AskPreview(c.play, c.info.time);
}

// The deck has come to rest on the focused card (only then does a click on it count).
bool Settled() { return !P.dragging && fabsf(P.scroll - (float)P.focus) < 0.02f && fabsf(P.scrollV) < 0.2f; }

// ---------------------------------------------------------------------------------------
// Layout

struct Pose {
    float x, y, rot, s, a, dim;
};

// Where card i sits: on an arc under the screen's bottom edge, the focused one raised and bigger.
Pose CardPose(int i) {
    const float d = i - P.scroll;
    const float R = std::max(P.W * 2.1f, 1600.f);
    const float step = kCardW * P.k * 0.93f / R;  // radians between neighbours
    const float ang = d * step;
    const float f = std::max(0.f, 1 - fabsf(d));
    const float baseY = P.H - 34 - kCardH * P.k / 2;
    Pose p;
    p.x = P.W / 2 + R * sinf(ang);
    p.y = baseY + R * (1 - cosf(ang)) - kLift * P.k * EaseInOut(f);
    p.rot = ang * 57.2958f * 1.7f;
    p.s = P.k * (1 + kGrow * EaseInOut(f));
    p.dim = std::min(0.38f, fabsf(d) * 0.045f);
    if (i < (int)P.lift.size() && P.lift[i] > 0) {  // pointed at: up a little, out of the dark
        const float h = EaseOut(P.lift[i]) * (1 - f);
        p.y -= 16 * P.k * h;
        p.s *= 1 + 0.04f * h;
        p.dim *= 1 - h;
    }
    // Rising in when the picker opens (the middle first, then outwards), sinking when it closes.
    const float delay = std::min(fabsf(d), 9.f) * 0.035f;
    const float e = P.phase == OPENING || P.phase == BROWSING ? EaseBack((P.enter - delay) / 0.55f)
                  : P.phase == SELECTING                       ? 1.f
                                                               : Clamp01(P.enter);
    p.y += (1 - e) * kCardH * P.k * 1.8f;
    p.a = Clamp01(e * 1.6f);
    return p;
}

D2D1_MATRIX_3X2_F CardMatrix(const Pose& p) {
    const float r = p.rot * 0.0174533f, c = cosf(r), s = sinf(r);  // (Matrix3x2F::Rotation needs d2d1.dll linked)
    return D2D1::Matrix3x2F::Scale(p.s, p.s) * D2D1::Matrix3x2F(c, s, -s, c, 0, 0) * D2D1::Matrix3x2F::Translation(p.x, p.y);
}

bool CardVisible(const Pose& p) {
    const float reach = (kCardW + kCardH) * p.s;
    return p.a > 0.01f && p.x > -reach && p.x < P.W + reach && p.y - reach < P.H;
}

// The card's picture: the part of the video the desktop shows, centred in the picture box.
void PictureBrush(Card& c, const D2D1_RECT_F& box) {
    if (c.brush || !c.thumb) return;
    const D2D1_SIZE_F sz = c.thumb->GetSize();
    const double aspect = c.info.vw && c.info.vh ? (double)c.info.vw / c.info.vh : sz.width / sz.height;
    FitRect f = ComputeFit(aspect, c.crop, CropMode(c.crop), P.W, P.H);
    float sx = f.sl * sz.width, sy = f.st * sz.height, sw = (f.sr - f.sl) * sz.width, sh = (f.sb - f.st) * sz.height;
    const float bw = box.right - box.left, bh = box.bottom - box.top;
    if (sw / sh > bw / bh) {
        const float nw = sh * bw / bh;
        sx += (sw - nw) / 2;
        sw = nw;
    } else {
        const float nh = sw * bh / bw;
        sy += (sh - nh) / 2;
        sh = nh;
    }
    const float scale = bw / sw;
    D2D1_BITMAP_BRUSH_PROPERTIES bp = D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                                  D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    D2D1_BRUSH_PROPERTIES brp = D2D1::BrushProperties(1, D2D1::Matrix3x2F::Translation(-sx, -sy) * D2D1::Matrix3x2F::Scale(scale, scale) *
                                                             D2D1::Matrix3x2F::Translation(box.left, box.top));
    G.ctx->CreateBitmapBrush(c.thumb, bp, brp, &c.brush);
}

float Luma(UINT32 rgb) { return (0.2126f * ((rgb >> 16) & 255) + 0.7152f * ((rgb >> 8) & 255) + 0.0722f * (rgb & 255)) / 255; }

// "Current", on the wallpaper's card: in the corner of its picture, in the picture's most colourful
// colour (so each card's is its own).
UINT32 BadgeColour(const Card& c) {
    UINT32 best = 0xFFFFFF;
    float bestScore = -1;
    for (UINT32 col : c.info.palette) {
        const float r = ((col >> 16) & 255) / 255.f, g = ((col >> 8) & 255) / 255.f, b = (col & 255) / 255.f;
        const float hi = std::max({r, g, b}), lo = std::min({r, g, b});
        const float score = (hi > 0 ? (hi - lo) / hi : 0) * 0.7f + hi * 0.3f;
        if (score > bestScore) {
            bestScore = score;
            best = col;
        }
    }
    return c.info.ok ? best : 0xFFFFFF;
}

void DrawBadge(Card& c, const D2D1_RECT_F& pic, float a, bool cached) {
    const D2D1_RECT_F chip = R(pic.left + 6, pic.bottom - 24, pic.left + 76, pic.bottom - 6);
    const UINT32 bg = BadgeColour(c);
    const D2D1_COLOR_F ink = Luma(bg) > 0.55f ? Col(0x111111, a) : Col(0xFFFFFF, a);
    Fill(chip, Col(bg, a), 9);
    Stroke(chip, Col(0x000000, 0.3f * a), 9, 1);
    const D2D1_RECT_F tick = R(chip.left + 6, chip.top, chip.left + 19, chip.bottom), word = R(chip.left + 22, chip.top, chip.right - 4, chip.bottom);
    if (cached) {
        CachedText(c.text[12], L"\uE73E", tick, G.fIcon, ink, DWRITE_TEXT_ALIGNMENT_CENTER);
        CachedText(c.text[13], L"Current", word, G.fRowValue, ink);
    } else {
        Text(L"\uE73E", tick, G.fIcon, ink, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(L"Current", word, G.fRowValue, ink);
    }
}

// What a card shows (its picture, facts and name), in its own coordinates: the centre at 0,0.
// The chosen card: its picture fades (picA) to a hole where its video plays, and its badge is its own.
void DrawCardContent(int i, float a, float infoA, float picA = 1, bool badge = true, bool hole = false) {
    Card& c = P.cards[i];
    const D2D1_RECT_F card = R(-kCardW / 2, -kCardH / 2, kCardW / 2, kCardH / 2);
    if (hole && G.cardHole) {
        G.br->SetColor(Col(0x141417, 0.96f * a));
        G.ctx->FillGeometry(G.cardHole, G.br);
    } else {
        Fill(card, Col(0x141417, 0.96f * a), kCardR);
    }
    const D2D1_RECT_F pic = R(card.left + kPad, card.top + kPad, card.right - kPad, card.top + kPad + kThumbH);
    PictureBrush(c, pic);
    if (picA > 0 && c.brush) {
        c.brush->SetOpacity(a * picA);
        G.ctx->FillRoundedRectangle(D2D1::RoundedRect(pic, 10, 10), c.brush);
    } else if (picA > 0) {
        Fill(pic, Col(0x222226, a * picA), 10);
        const bool bad = c.ready && !c.info.ok;
        Text(bad ? L"\uE783" : L"\uE714", pic, G.fIconBig, Col(0xFFFFFF, 0.35f * a * picA), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (badge && i == P.applied) DrawBadge(c, pic, a, true);  // the wallpaper now
    // The facts, each row in one of the picture's colours.
    float y = pic.bottom + kPad;
    const float ia = a * infoA;
    if (c.info.ok) {
        for (int r = 0; r < 5; r++) {
            const UINT32 col = c.info.palette[r];
            const D2D1_RECT_F row = R(card.left + kPad, y, card.right - kPad, y + kRowH);
            Fill(row, Col(col, ia), 5);
            const bool light = Luma(col) > 0.55f;
            const D2D1_COLOR_F ink = light ? Col(0x111111, 0.86f * ia) : Col(0xFFFFFF, 0.92f * ia);
            CachedText(c.text[r], c.rows[r][0], R(row.left + 7, row.top, row.left + 66, row.bottom), G.fRowLabel, ink);
            CachedText(c.text[5 + r], c.rows[r][1], R(row.left + 62, row.top, row.right - 7, row.bottom), G.fRowValue, ink,
                       DWRITE_TEXT_ALIGNMENT_TRAILING);
            y += kRowH + kRowGap;
        }
    } else {
        const D2D1_RECT_F box = R(card.left + kPad, y, card.right - kPad, y + 5 * kRowH + 4 * kRowGap);
        Fill(box, Col(0x222226, ia), 6);
        std::wstring why = !c.ready ? L"Reading the video\u2026" : c.info.fault == VF_GIF ? L"A GIF: convert it in Settings"
                         : c.info.fault == VF_MISSING ? L"The file isn't there"
                         : c.info.fault == VF_NO_DECODER ? L"Needs a codec" : L"Can't read this video";
        CachedText(c.text[14], why, box, G.fCardMeta, Col(0xFFFFFF, 0.6f * ia), DWRITE_TEXT_ALIGNMENT_CENTER);
        y = box.bottom + kRowGap;
    }
    y += 3;
    CachedText(c.text[10], c.title, R(card.left + kPad + 2, y, card.right - kPad, y + 17), G.fCardTitle, Col(0xFFFFFF, 0.96f * ia));
    CachedText(c.text[11], c.meta, R(card.left + kPad + 2, y + 17, card.right - kPad, y + 31), G.fCardMeta, Col(0xFFFFFF, 0.55f * ia));
}

constexpr float kImagePad = 2;  // clear edge around a card's image, so it stays smooth when turned

// Draws the card into its own bitmap once (text drawn turned and scaled every frame is what costs).
// Called outside the frame's drawing.
void MakeCardImage(int i) {
    Card& c = P.cards[i];
    if (c.image) return;
    const float S = P.k * (1 + kGrow) * P.scale;  // pixels per card unit, as big as a card gets
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * S, 96 * S);
    const D2D1_SIZE_U size = D2D1::SizeU((UINT32)ceilf((kCardW + 2 * kImagePad) * S), (UINT32)ceilf((kCardH + 2 * kImagePad) * S));
    if (FAILED(G.ctx->CreateBitmap(size, nullptr, 0, &bp, &c.image))) return;
    ID2D1Image* old = nullptr;
    G.ctx->GetTarget(&old);
    G.ctx->SetTarget(c.image);
    G.ctx->SetDpi(96 * S, 96 * S);
    G.ctx->BeginDraw();
    G.ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    G.ctx->SetTransform(D2D1::Matrix3x2F::Translation(kCardW / 2 + kImagePad, kCardH / 2 + kImagePad));
    DrawCardContent(i, 1, 1);
    G.ctx->EndDraw();
    G.ctx->SetTarget(old);
    G.ctx->SetDpi(96 * P.scale, 96 * P.scale);
    G.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    SafeRelease(old);
}

// One card in the deck under `m`: its shadow, its image (or, fading its facts out, drawn afresh),
// and what changes with its place: the focused card's bright edge, the dark the far ones sink into.
void DrawCard(int i, const Pose& p, const D2D1_MATRIX_3X2_F& m, float extraA = 1, float infoA = 1) {
    Card& c = P.cards[i];
    const float a = p.a * extraA;
    G.ctx->SetTransform(m);
    const D2D1_RECT_F card = R(-kCardW / 2, -kCardH / 2, kCardW / 2, kCardH / 2);
    if (G.shadow) {
        const D2D1_SIZE_F s = G.shadow->GetSize();
        const D2D1_RECT_F sr = R(-s.width / 2, -s.height / 2 + 10, s.width / 2, s.height / 2 + 10);
        G.ctx->DrawBitmap(G.shadow, sr, 0.85f * a);
    }
    if (c.image && infoA >= 1) {
        const float e = kImagePad;
        G.ctx->DrawBitmap(c.image, R(card.left - e, card.top - e, card.right + e, card.bottom + e), a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        DrawCardContent(i, a, infoA);
    }
    // The focused card wears a bright edge; the others sink into the dark a little.
    const float f = std::max(0.f, 1 - fabsf(i - P.scroll));
    Stroke(card, Col(0xFFFFFF, (0.10f + 0.75f * f) * a), kCardR, 1 + 0.6f * f);
    if (p.dim > 0) Fill(card, Col(0x000000, p.dim * a), kCardR);
}

// ---------------------------------------------------------------------------------------
// Painting

// A card's picture as the desktop shows it, over the whole screen (black where it doesn't reach).
void DrawScreenPicture(Card& c, ID2D1Bitmap* bm, float a) {
    const D2D1_SIZE_F sz = bm->GetSize();
    const double aspect = c.info.vw && c.info.vh ? (double)c.info.vw / c.info.vh : sz.width / sz.height;
    const FitRect f = ComputeFit(aspect, c.crop, CropMode(c.crop), P.W, P.H);
    const D2D1_COLOR_F black = Col(0x000000, a);
    if (f.dt > 0) Fill(R(0, 0, P.W, f.dt), black);
    if (f.db < P.H) Fill(R(0, f.db, P.W, P.H), black);
    if (f.dl > 0) Fill(R(0, f.dt, f.dl, f.db), black);
    if (f.dr < P.W) Fill(R(f.dr, f.dt, P.W, f.db), black);
    G.ctx->DrawBitmap(bm, R(f.dl, f.dt, f.dr, f.db), a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                      R(f.sl * sz.width, f.st * sz.height, f.sr * sz.width, f.sb * sz.height));
}

void DrawStillLayer() {
    for (const Still& s : P.stills) {
        if (s.card < 0 || s.card >= (int)P.cards.size() || s.a <= 0) continue;
        Card& c = P.cards[s.card];
        if (ID2D1Bitmap* bm = c.frame ? c.frame : c.thumb) DrawScreenPicture(c, bm, s.a);
    }
}

void DrawClock(float a) {
    if (!P.clockBmp || a <= 0) return;
    G.ctx->DrawBitmap(P.clockBmp, ClockRect(), a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

// ---------------------------------------------------------------------------------------
// Panels (the clock's settings, the music's): rows of controls on a dark glass card

constexpr float kPanelW = 320, kRow = 40, kLabelW = 96;

void PanelBox(const D2D1_RECT_F& box, float a, float radius = 16) {
    if (G.shadow) {
        const D2D1_SIZE_F s = G.shadow->GetSize();
        G.ctx->DrawBitmap(G.shadow, R(box.left - 40, box.top - 30, box.right + 40, box.bottom + 50), 0.6f * a,
                          D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, R(0, 0, s.width, s.height));
    }
    Fill(box, Col(0x17171A, 0.96f * a), radius);
    Stroke(box, Col(0xFFFFFF, 0.1f * a), radius, 1);
}

void Label(const std::wstring& s, float x, float y, float a, bool on = true) {
    Text(s, R(x, y, x + kLabelW, y + kRow), G.fBody, Col(0xFFFFFF, (on ? 0.78f : 0.35f) * a));
}

void Circle(float cx, float cy, float r, D2D1_COLOR_F c) {
    G.br->SetColor(c);
    G.ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), G.br);
}

void Ring(float cx, float cy, float r, D2D1_COLOR_F c, float width) {
    G.br->SetColor(c);
    G.ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), G.br, width);
}

// Choices side by side, the chosen one lit; each one's id is id0 + its place.
void Segs(int id0, const D2D1_RECT_F& r, const std::vector<std::wstring>& opts, int sel, float a, bool on = true) {
    const float rad = (r.bottom - r.top) / 2;
    Fill(r, Col(0xFFFFFF, 0.06f * a), rad);
    const float w = (r.right - r.left) / std::max<size_t>(1, opts.size());
    int i = 0;
    for (const std::wstring& o : opts) {
        const D2D1_RECT_F s = R(r.left + w * i + 3, r.top + 3, r.left + w * (i + 1) - 3, r.bottom - 3);
        if (i == sel) Fill(s, on ? g_accent * a : Col(0xFFFFFF, 0.16f * a), rad - 3);
        else if (on && P.hot == id0 + i) Fill(s, Col(0xFFFFFF, 0.07f * a), rad - 3);
        Text(o, s, G.fBody, i == sel && on ? Col(0x000000, 0.88f * a) : Col(0xFFFFFF, (on ? 0.85f : 0.35f) * a),
             DWRITE_TEXT_ALIGNMENT_CENTER);
        if (on) P.hits.push_back({id0 + i, s});
        i++;
    }
}

void Segs(int id0, const D2D1_RECT_F& r, std::initializer_list<const wchar_t*> opts, int sel, float a, bool on = true) {
    Segs(id0, r, std::vector<std::wstring>(opts.begin(), opts.end()), sel, a, on);
}

// A slider with its value on the right. P.tracks keeps its track, for the value under the pointer.
void SliderCtl(int id, const D2D1_RECT_F& r, float frac, const std::wstring& value, float a, bool on = true, float vw = 46) {
    const float cy = (r.top + r.bottom) / 2;
    const D2D1_RECT_F track = R(r.left + 8, cy - 2, r.right - vw - (vw > 0 ? 10 : 8), cy + 2);
    const float tx = Lerp(track.left, track.right, Clamp01(frac));
    Fill(track, Col(0xFFFFFF, 0.16f * a), 2);
    if (on) Fill(R(track.left, track.top, tx, track.bottom), g_accent * a, 2);
    const bool hot = on && (P.hot == id || P.slide == id);
    Circle(tx, cy, hot ? 8.f : 7.f, Col(on ? 0xFFFFFF : 0x77777C, a));
    if (vw > 0)
        Text(value, R(r.right - vw, r.top, r.right, r.bottom), G.fSmall, Col(0xFFFFFF, (on ? 0.7f : 0.35f) * a), DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (!on) return;
    P.hits.push_back({id, R(track.left - 9, r.top, track.right + 9, r.bottom)});
    P.tracks.push_back({id, track});
}

// A round icon button (no background until pointed at).
void IconBtn(int id, float cx, float cy, float r, const wchar_t* icon, float a, bool on = true, D2D1_COLOR_F ink = D2D1::ColorF(1, 1, 1)) {
    if (on && P.hot == id) Circle(cx, cy, r, Col(0xFFFFFF, (P.press == id ? 0.14f : 0.09f) * a));
    Text(icon, R(cx - r, cy - r, cx + r, cy + r), G.fIcon, ink * ((on ? 0.92f : 0.3f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
    if (on) P.hits.push_back({id, R(cx - r, cy - r, cx + r, cy + r)});
}

UINT32 RgbOf(const std::wstring& s) {
    int r = 0, g = 0, b = 0;
    swscanf(s.c_str(), L"%d,%d,%d", &r, &g, &b);
    return (UINT32)(std::clamp(r, 0, 255) << 16 | std::clamp(g, 0, 255) << 8 | std::clamp(b, 0, 255));
}

std::wstring RgbText(UINT32 c) {
    wchar_t b[32];
    swprintf(b, 32, L"%u,%u,%u", (c >> 16) & 255, (c >> 8) & 255, c & 255);
    return b;
}

// A colour to pick: a round swatch, ringed when it's the one in use (`mark`: an icon on it).
void Swatch(int id, float cx, float cy, UINT32 rgb, bool sel, float a, bool on, const wchar_t* mark = nullptr) {
    if (sel) Ring(cx, cy, 13, (on ? g_accent : Col(0xFFFFFF, 0.3f)) * a, 2);
    else if (on && P.hot == id) Ring(cx, cy, 13, Col(0xFFFFFF, 0.35f * a), 1.5f);
    const float k = on ? 1 : 0.4f;
    Circle(cx, cy, 9.5f, Col(rgb, a * k));
    Ring(cx, cy, 9.5f, Col(0xFFFFFF, 0.22f * a), 1);
    if (mark) Text(mark, R(cx - 9, cy - 9, cx + 9, cy + 9), G.fSmallIcon, Luma(rgb) > 0.55f ? Col(0x000000, 0.8f * a) : Col(0xFFFFFF, 0.9f * a),
                   DWRITE_TEXT_ALIGNMENT_CENTER);
    if (on) P.hits.push_back({id, R(cx - 13, cy - 13, cx + 13, cy + 13)});
}

// ---------------------------------------------------------------------------------------
// The clock's settings, beside the clock being placed: on the right, or dragged to the left

float PanelX() {
    return Lerp(P.W - 20 - kPanelW, 20, P.panelPos);
}

UINT32 Hsv(float h, float s, float v) {  // h in degrees
    h = fmodf(h + 360, 360.f) / 60.f;
    const int i = (int)h;
    const float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r = v, g = t, b = p;
    switch (i % 6) {
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        case 5: r = v, g = p, b = q; break;
    }
    return (UINT32)lroundf(r * 255) << 16 | (UINT32)lroundf(g * 255) << 8 | (UINT32)lroundf(b * 255);
}

void ToHsv(UINT32 c, float* h, float* s, float* v) {
    const float r = ((c >> 16) & 255) / 255.f, g = ((c >> 8) & 255) / 255.f, b = (c & 255) / 255.f;
    const float hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo;
    *v = hi;
    *s = hi > 0 ? d / hi : 0;
    *h = d <= 0 ? 0 : hi == r ? 60 * fmodf((g - b) / d + 6, 6) : hi == g ? 60 * ((b - r) / d + 2) : 60 * ((r - g) / d + 4);
}

// The quick colours: white, black, and three bright ones from the wallpaper's own colours (or, for
// a wallpaper without any, red, amber and sky).
void QuickColours() {
    P.swatches = {0xFFFFFF, 0x000000};
    std::vector<float> hues;
    if (Card* w = WallpaperCard(); w && w->info.ok)
        for (UINT32 c : w->info.palette) {
            float h, sat, val;
            ToHsv(c, &h, &sat, &val);
            bool skip = sat < 0.12f || hues.size() >= 3;  // (a grey has no colour to brighten)
            for (float o : hues) skip = skip || fabsf(fmodf(h - o + 540, 360) - 180) < 28;
            if (skip) continue;
            hues.push_back(h);
            P.swatches.push_back(Hsv(h, std::max(sat, 0.62f), std::max(val, 0.92f)));
        }
    for (UINT32 c : {0xFF5A5Au, 0xFFC23Du, 0x4FC3F7u})
        if (P.swatches.size() < 5) P.swatches.push_back(c);
}

// Any colour: eight hues in four shades, the greys, and the colours saved in the settings window.
const std::vector<UINT32>& GridColours() {
    static const std::vector<UINT32> grid = [] {
        std::vector<UINT32> v;
        const float hues[8] = {0, 28, 50, 130, 170, 200, 222, 330};
        const float shades[4][2] = {{0.3f, 1}, {0.7f, 1}, {0.85f, 0.72f}, {0.8f, 0.42f}};
        for (auto& k : shades)
            for (float h : hues) v.push_back(Hsv(h, k[0], k[1]));
        for (UINT32 c : {0xFFFFFFu, 0xD9D9D9u, 0xB3B3B3u, 0x8C8C8Cu, 0x666666u, 0x404040u, 0x1F1F1Fu, 0x000000u}) v.push_back(c);
        return v;
    }();
    return grid;
}

std::vector<UINT32> SavedColours() {
    std::vector<UINT32> v;
    for (const std::wstring& c : g_settings.myColors)
        if (!c.empty() && v.size() < 8) v.push_back(RgbOf(c));
    return v;
}

bool GridColourAt(int k, UINT32* c) {
    const auto& grid = GridColours();
    const std::vector<UINT32> saved = SavedColours();
    if (k >= 0 && k < (int)grid.size()) *c = grid[k];
    else if (k >= 40 && k - 40 < (int)saved.size()) *c = saved[k - 40];
    else return false;
    return true;
}

// The grid, hanging from (ax, ay).
void DrawColourGrid(float a, float ax, float ay, UINT32 inUse) {
    const auto& grid = GridColours();
    const std::vector<UINT32> saved = SavedColours();
    const int rows = (int)grid.size() / 8;
    const float cell = 28, w = 8 * cell + 16, h = 10 + rows * cell + (saved.empty() ? 0 : 24 + cell) + 8;
    const float x = std::clamp(ax, 20.f, P.W - 20 - w), y = std::min(ay, P.H - 20 - h);
    const D2D1_RECT_F box = R(x, y, x + w, y + h);
    PanelBox(box, a, 12);
    P.hits.push_back({ID_P_LIST, box});
    auto cellAt = [&](int id, int k, float top, UINT32 c) {
        Swatch(id, x + 8 + (k % 8) * cell + cell / 2, top + (k / 8) * cell + cell / 2, c, c == inUse, a, true);
    };
    for (size_t k = 0; k < grid.size(); k++) cellAt(ID_P_GRID0 + (int)k, (int)k, y + 10, grid[k]);
    if (saved.empty()) return;
    const float sy = y + 10 + rows * cell + 4;
    Text(L"Saved in the settings window", R(x + 12, sy, box.right - 10, sy + 20), G.fSmall, Col(0xFFFFFF, 0.5f * a));
    for (size_t k = 0; k < saved.size(); k++) cellAt(ID_P_GRID0 + 40 + (int)k, (int)k, sy + 20, saved[k]);
}

// A round tool beside the swatches (the eyedropper, any colour); `fill`: the colour it stands for.
void ToolSwatch(int id, float cx, float cy, const wchar_t* icon, bool lit, float a, bool on, const UINT32* fill = nullptr) {
    if (lit) Ring(cx, cy, 13, (on ? g_accent : Col(0xFFFFFF, 0.3f)) * a, 2);
    else if (on && P.hot == id) Ring(cx, cy, 13, Col(0xFFFFFF, 0.35f * a), 1.5f);
    Circle(cx, cy, 9.5f, fill ? Col(*fill, a * (on ? 1 : 0.4f)) : Col(0x2A2A2F, a));
    Ring(cx, cy, 9.5f, Col(0xFFFFFF, 0.22f * a), 1);
    const D2D1_COLOR_F ink = fill && Luma(*fill) > 0.55f ? Col(0x000000, 0.8f * a) : Col(0xFFFFFF, (on ? 0.9f : 0.35f) * a);
    Text(icon, R(cx - 9, cy - 9, cx + 9, cy + 9), G.fSmallIcon, ink, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (on) P.hits.push_back({id, R(cx - 13, cy - 13, cx + 13, cy + 13)});
}

// The eyedropper's loupe, beside the pointer (never under it: the colour comes from there).
void DrawLoupe(float a) {
    if (a <= 0) return;
    const float x = P.pickAt.x + 26, y = P.pickAt.y + 26;
    Circle(x, y + 2, 17, Col(0x000000, 0.35f * a));
    Circle(x, y, 16, Col(P.pickRgb, a));
    Ring(x, y, 16, Col(0xFFFFFF, 0.9f * a), 2);
}

// The panel's rows: the language's own digits only for a language that has them.
bool HasOwnDigits() { return !Clock_NativeDigits(P.look.lang).empty(); }

void DrawClockPanel(float a) {
    if (a <= 0.01f) return;
    const float x = PanelX(), y = 92, h = 64 + (HasOwnDigits() ? 12 : 11) * kRow + 10;
    const D2D1_RECT_F box = R(x, y, x + kPanelW, y + h);
    PanelBox(box, a);
    P.hits.push_back({ID_P_BG, box});
    // The header; its arrow sends the panel to the other side.
    Text(L"This video's clock", R(x + 18, y + 12, box.right - 52, y + 34), G.fTitle, Col(0xFFFFFF, a));
    Text(P.clockFor, R(x + 18, y + 34, box.right - 52, y + 52), G.fSmall, Col(0xFFFFFF, 0.55f * a));
    IconBtn(ID_P_SIDE, box.right - 30, y + 31, 16, P.panelSide ? L"\uE72A" : L"\uE72B", a);
    G.br->SetColor(Col(0xFFFFFF, 0.08f * a));
    G.ctx->DrawLine(D2D1::Point2F(box.left + 14, y + 60), D2D1::Point2F(box.right - 14, y + 60), G.br, 1);

    const ClockLook& L = P.look;
    const bool on = L.show;
    const float lx = x + 16, cx = x + 16 + kLabelW, cr = box.right - 16;
    float yy = y + 64;
    auto ctl = [&](float top) { return R(cx, top + 6, cr, top + kRow - 6); };
    Label(L"Clock", lx, yy, a);
    Segs(ID_P_SHOW, ctl(yy), {L"Show", L"Hide"}, on ? 0 : 1, a);
    yy += kRow;
    // A row that steps through the choices, like the date's.
    auto stepper = [&](float top, int prev, int next, const std::wstring& s, bool en, IDWriteTextFormat* f) {
        const D2D1_RECT_F r = ctl(top);
        Fill(r, Col(0xFFFFFF, 0.06f * a), 8);
        Text(s, R(r.left + 26, r.top, r.right - 26, r.bottom), f, Col(0xFFFFFF, (en ? 0.9f : 0.35f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        IconBtn(prev, r.left + 14, (r.top + r.bottom) / 2, 12, L"\uE76B", a, en);
        IconBtn(next, r.right - 14, (r.top + r.bottom) / 2, 12, L"\uE76C", a, en);
    };
    Label(L"Style", lx, yy, a, on);
    stepper(yy, ID_P_STYLE_PREV, ID_P_STYLE_NEXT, Clock_StyleName(L), on, G.fBody);
    yy += kRow;
    // The language of its words (greyed when the style has none), and its own digits when it has them.
    const bool words = on && Clock_UsesWords(L);
    Label(L"Language", lx, yy, a, words);
    const D2D1_RECT_F lb = ctl(yy);
    Fill(lb, Col(0xFFFFFF, (words && (P.hot == ID_P_LANG || P.langsOpen) ? 0.11f : 0.06f) * a), 8);
    Text(Clock_LanguageName(L.lang), R(lb.left + 12, lb.top, lb.right - 30, lb.bottom), G.fBody, Col(0xFFFFFF, (words ? 0.92f : 0.35f) * a));
    Text(P.langsOpen ? L"\uE70E" : L"\uE70D", R(lb.right - 28, lb.top, lb.right - 8, lb.bottom), G.fSmallIcon,
         Col(0xFFFFFF, (words ? 0.6f : 0.3f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
    if (words) P.hits.push_back({ID_P_LANG, lb});
    yy += kRow;
    if (HasOwnDigits()) {
        const bool digits = on && Clock_UsesDigits(L);
        Label(L"Digits", lx, yy, a, digits);
        Segs(ID_P_DIG_LATIN, ctl(yy), {std::wstring(L"123"), Clock_NativeDigits(L.lang).substr(1, 3)}, L.nativeDigits ? 1 : 0, a, digits);
        yy += kRow;
    }
    // The text's colour, then the glow's: the eyedropper, any colour (a grid), then quick ones (the
    // glow's first: the same as the text's).
    const UINT32 ink = RgbOf(L.color), glowInk = L.glowColor.empty() ? ink : RgbOf(L.glowColor);
    QuickColours();
    auto quick = [&](UINT32 c, size_t n) {
        const auto end = P.swatches.begin() + std::min(n, P.swatches.size());
        return std::find(P.swatches.begin(), end, c) != end;
    };
    const bool glowOn = on && L.glow > 0;
    Label(L"Colour", lx, yy, a, on);
    float sy = yy + kRow / 2;
    ToolSwatch(ID_P_PICK_TEXT, cx + 12, sy, L"\uEF3C", P.picking == 1, a, on);
    const bool inkQuick = quick(ink, 5);
    ToolSwatch(ID_P_GRID_TEXT, cx + 39, sy, L"\uE710", P.gridFor == 1 || !inkQuick, a, on, inkQuick ? nullptr : &ink);
    for (size_t i = 0; i < P.swatches.size() && i < 5; i++)
        Swatch(ID_P_COLOR0 + (int)i, cx + 66 + 27 * i, sy, P.swatches[i], P.swatches[i] == ink, a, on);
    const float textRow = yy + kRow;
    yy += kRow;
    Label(L"Glow colour", lx, yy, a, glowOn);
    sy = yy + kRow / 2;
    ToolSwatch(ID_P_PICK_GLOW, cx + 12, sy, L"\uEF3C", P.picking == 2, a, glowOn);
    const bool glowQuick = L.glowColor.empty() || quick(glowInk, 4);
    ToolSwatch(ID_P_GRID_GLOW, cx + 39, sy, L"\uE710", P.gridFor == 2 || !glowQuick, a, glowOn, glowQuick ? nullptr : &glowInk);
    Swatch(ID_P_GLOWC0, cx + 66, sy, ink, L.glowColor.empty(), a, glowOn, L"\uE71B");
    for (size_t i = 0; i < P.swatches.size() && i < 4; i++)
        Swatch(ID_P_GLOWC0 + 1 + (int)i, cx + 93 + 27 * i, sy, P.swatches[i], !L.glowColor.empty() && P.swatches[i] == glowInk, a, glowOn);
    const float glowRow = yy + kRow;
    yy += kRow;
    wchar_t v[32];
    Label(L"Size", lx, yy, a, on);
    swprintf(v, 32, L"%.0f%%", L.size * 100);
    SliderCtl(ID_P_SIZE, ctl(yy), (L.size - 0.5f) / 1.5f, v, a, on);
    yy += kRow;
    Label(L"Opacity", lx, yy, a, on);
    swprintf(v, 32, L"%.0f%%", L.opacity * 100);
    SliderCtl(ID_P_OPACITY, ctl(yy), (L.opacity - 0.2f) / 0.8f, v, a, on);
    yy += kRow;
    Label(L"Glow", lx, yy, a, on);
    swprintf(v, 32, L.glow > 0 ? L"%.0f%%" : L"Off", L.glow * 100);
    SliderCtl(ID_P_GLOW, ctl(yy), L.glow, v, a, on);
    yy += kRow;
    Label(L"Glow size", lx, yy, a, on && L.glow > 0);
    swprintf(v, 32, L"%.0f", L.glowSize);
    SliderCtl(ID_P_GLOWSIZE, ctl(yy), (L.glowSize - 2) / 38, v, a, on && L.glow > 0);
    yy += kRow;
    const bool dated = on && Clock_UsesDate(L);
    Label(L"Date", lx, yy, a, dated);
    SYSTEMTIME now;
    GetLocalTime(&now);
    const std::wstring date = Clock_DateText(L, L.date, now);
    stepper(yy, ID_P_DATE_PREV, ID_P_DATE_NEXT, date.empty() ? L"No date" : date, dated, G.fSmall);
    yy += kRow;
    const bool hours = on && Clock_UsesHours(L);
    Label(L"Time", lx, yy, a, hours);
    Segs(ID_P_H12, ctl(yy), {L"12-hour", L"24-hour"}, L.h24 ? 1 : 0, a, hours);

    // Any colour's grid, under its row; or the languages, over the rows under theirs.
    if (P.gridFor && (P.gridFor == 1 ? on : glowOn)) {
        DrawColourGrid(a, cx + 19, P.gridFor == 1 ? textRow : glowRow, P.gridFor == 1 ? ink : glowInk);
        return;
    }
    if (!P.langsOpen || !words) return;
    const float rh = 32, shown = (float)std::min<size_t>(P.langs.size(), 9);
    const D2D1_RECT_F list = R(lb.left - 4 - 60, lb.bottom + 4, lb.right + 4, lb.bottom + 4 + shown * rh + 8);
    PanelBox(list, a, 10);
    P.hits.push_back({ID_P_LIST, list});
    const D2D1_RECT_F view = R(list.left, list.top + 4, list.right, list.bottom - 4);
    P.langScroll = std::clamp(P.langScroll, 0.f, std::max(0.f, P.langs.size() * rh - shown * rh));
    G.ctx->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < P.langs.size(); i++) {
        const float ry = view.top + i * rh - P.langScroll;
        if (ry + rh <= view.top || ry >= view.bottom) continue;
        const D2D1_RECT_F r = R(view.left + 4, ry, view.right - 4, ry + rh);
        const ClockLanguage& lg = P.langs[i];
        if (P.hot == ID_P_LANG0 + (int)i) Fill(r, Col(0xFFFFFF, 0.08f * a), 7);
        if (lg.code == L.lang) Text(L"\uE73E", R(r.left + 4, r.top, r.left + 26, r.bottom), G.fSmallIcon, g_accent * a, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(lg.english, R(r.left + 30, r.top, r.left + 170, r.bottom), G.fBody, Col(0xFFFFFF, 0.9f * a));
        Text(lg.native, R(r.left + 170, r.top, r.right - 10, r.bottom), G.fSmall, Col(0xFFFFFF, 0.5f * a), DWRITE_TEXT_ALIGNMENT_TRAILING);
        P.hits.push_back({ID_P_LANG0 + (int)i, R(r.left, std::max(r.top, view.top), r.right, std::min(r.bottom, view.bottom))});
    }
    G.ctx->PopAxisAlignedClip();
}

// A letter typed while the languages are open: the next one starting with it.
void LanguageJump(wchar_t letter) {
    const int n = (int)P.langs.size(), from = (int)(P.langScroll / 32);
    for (int k = 1; k <= n; k++) {
        const int i = (from + k) % n;
        if (!P.langs[i].english.empty() && towupper(P.langs[i].english[0]) == letter) {
            P.langScroll = i * 32.f;
            P.dirty = true;
            return;
        }
    }
}

// The playlist, in play order: a click plays a song; its grip moves it (drag it up or down, the
// others make room). The wheel scrolls it.
constexpr float kSongRow = 30;

void DrawSongs(const D2D1_RECT_F& view, float a) {
    const int n = (int)P.songs.size();
    const std::vector<int> order = MusicOrderFor(g_settings, n);
    const MusicState ms = Host_MusicState();
    const int current = ms.track >= 0 ? ms.track : std::clamp(g_settings.musicTrack, -1, n - 1);
    P.songView = view;
    P.hits.push_back({ID_MU_LIST, view});
    P.songScroll = std::clamp(P.songScroll, 0.f, std::max(0.f, n * kSongRow - (view.bottom - view.top)));
    std::vector<int> shown = order;
    float liftTop = 0;
    const bool moving = P.songDrag >= 0 && P.songDrag < n;
    if (moving) {  // a gap where it would land
        shown.erase(std::find(shown.begin(), shown.end(), P.songDrag));
        liftTop = std::clamp(P.songY - P.songGrab, view.top, view.bottom - kSongRow);
        P.songIns = std::clamp((int)lroundf((liftTop - view.top + P.songScroll) / kSongRow), 0, n - 1);
        shown.insert(shown.begin() + P.songIns, -1);
    }
    auto clip = [&](const D2D1_RECT_F& h) { return R(h.left, std::max(h.top, view.top), h.right, std::min(h.bottom, view.bottom)); };
    auto row = [&](int song, int place, const D2D1_RECT_F& r, bool lifted) {
        const bool cur = song == current, hot = !moving && (P.hot == ID_SONG0 + song || P.hot == ID_GRIP0 + song || P.hot == ID_SONGX0 + song);
        if (lifted) {
            Fill(r, Col(0x2A2A2F, 0.98f * a), 7);
            Stroke(r, g_accent * a, 7, 1);
        } else if (cur) {
            Fill(r, g_accent * (0.16f * a), 7);
        } else if (hot) {
            Fill(r, Col(0xFFFFFF, 0.07f * a), 7);
        }
        if (cur) Text(L"\uE767", R(r.left + 4, r.top, r.left + 30, r.bottom), G.fSmallIcon, g_accent * a, DWRITE_TEXT_ALIGNMENT_CENTER);
        else Text(std::to_wstring(place + 1), R(r.left + 4, r.top, r.left + 30, r.bottom), G.fSmall, Col(0xFFFFFF, 0.4f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(FileTitle(P.songs[song]), R(r.left + 34, r.top, r.right - (hot ? 62 : 34), r.bottom), cur ? G.fBold : G.fBody,
             Col(0xFFFFFF, (cur || lifted ? 0.95f : 0.75f) * a));
        Text(L"\uE76F", R(r.right - 30, r.top, r.right - 6, r.bottom), G.fSmallIcon,
             Col(0xFFFFFF, (lifted || P.hot == ID_GRIP0 + song ? 0.9f : 0.35f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (hot)  // take it off the list: shown only on its row, in the row's own grey
            Text(L"\uE711", R(r.right - 58, r.top, r.right - 34, r.bottom), G.fSmallIcon,
                 Col(0xFFFFFF, (P.hot == ID_SONGX0 + song ? 0.9f : 0.45f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (moving) return;
        P.hits.push_back({ID_SONG0 + song, clip(R(r.left, r.top, r.right - 58, r.bottom))});
        P.hits.push_back({ID_SONGX0 + song, clip(R(r.right - 58, r.top, r.right - 34, r.bottom))});
        P.hits.push_back({ID_GRIP0 + song, clip(R(r.right - 34, r.top, r.right, r.bottom))});
    };
    G.ctx->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_ALIASED);
    for (int p = 0; p < n; p++) {
        const float ry = view.top - P.songScroll + p * kSongRow;
        if (ry + kSongRow <= view.top || ry >= view.bottom) continue;
        const D2D1_RECT_F r = R(view.left, ry + 1, view.right, ry + kSongRow - 1);
        if (shown[p] < 0) Stroke(R(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), g_accent * (0.5f * a), 7, 1);
        else row(shown[p], p, r, false);
    }
    if (moving) row(P.songDrag, P.songIns, R(view.left, liftTop + 1, view.right, liftTop + kSongRow - 1), true);
    G.ctx->PopAxisAlignedClip();
}

// The desktop player's style as it looks now, w DIPs wide at most (it turns while the music plays),
// with the pointer on it (it's the controller).
float PlayerPreview(float maxW, float* drawnW, float maxScale = 1) {
    const int style = g_settings.musicPlayer;
    float sw = 0, sh = 0;
    Player_Size(style, &sw, &sh);
    if (style < 1 || sw <= 0) {
        SafeRelease(P.player);
        return 0;
    }
    const float s = std::min(maxScale, maxW / sw);
    const double now = Now();
    if (!P.player || P.playerStyle != style || s != P.playerK || now - P.playerAt > (Player_Turns() ? 0.03 : 0.5)) {
        ClockImage img;
        SafeRelease(P.player);
        if (Player_Paint(style, s * P.scale, true, &img, &P.pv)) P.player = BitmapFrom(img.px, img.w, img.h, true);
        P.playerStyle = style;
        P.playerAt = now;
    }
    P.playerK = s;
    *drawnW = sw * s;
    return sh * s;
}

// The pointer on the player's picture: what it's over, in the style's own DIPs.
int PreviewPart(float x, float y) {
    return Player_PartAt(g_settings.musicPlayer, (x - P.playerBox.left) / P.playerK, (y - P.playerBox.top) / P.playerK);
}

void PreviewChanged() {
    P.playerAt = 0;  // drawn again with it
    P.dirty = true;
}

// The music screen (M, or Music): the video in the middle with the player on it, big, in the middle
// of what the panel leaves (it works: its buttons, its place, its volume); with no player, a word on
// how to get one.
void DrawMusicStage(float a) {
    if (a <= 0) return;
    const float left = P.panelSide ? 20 + kPanelW + 20 : 20, right = P.panelSide ? P.W - 20 : P.W - 20 - kPanelW - 20;
    float shownW = 0;
    const int style = g_settings.musicPlayer;
    const float size = style > 0 ? std::clamp(g_settings.playerLooks[style].size, 0.7f, 1.5f) : 1;
    const float ph = PlayerPreview(10000, &shownW, size);  // as big as on the desktop
    if (ph > 0 && P.player) {
        // Where it is on the desktop (drag it to move it there too).
        const float cx = std::clamp(g_settings.playerX * P.W, shownW / 2, P.W - shownW / 2);
        const float cy = std::clamp(g_settings.playerY * P.H, ph / 2, P.H - ph / 2);
        P.playerBox = R(cx - shownW / 2, cy - ph / 2, cx + shownW / 2, cy + ph / 2);
        if (P.pvMoving) Stroke(R(P.playerBox.left - 6, P.playerBox.top - 6, P.playerBox.right + 6, P.playerBox.bottom + 6),
                               Col(0xFFFFFF, 0.6f * a), 14, 1.5f, G.dash);
        G.ctx->DrawBitmap(P.player, P.playerBox, a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        P.hits.push_back({ID_MU_PREVIEW, P.playerBox});
        return;
    }
    const float cx = (left + right) / 2, cy = P.H / 2;
    const std::wstring t = L"No music player on the desktop: pick one in the panel";
    const float tw = TextWidth(t, G.fBody) + 32;
    PanelBox(R(cx - tw / 2, cy - 22, cx + tw / 2, cy + 22), a, 22);
    Text(t, R(cx - tw / 2, cy - 22, cx + tw / 2, cy + 22), G.fBody, Col(0xFFFFFF, 0.8f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// Its panel: the player (style, colour, opacity, corners, a reset), then the playlist with shuffle
// and repeat. The look is the player's own (each style keeps its own), the same on every wallpaper.
void DrawMusicEditPanel(float a) {
    if (a <= 0.01f) return;
    const int style = g_settings.musicPlayer;
    const bool mine = g_settings.sound == 2, has = style > 0;
    const int n = (int)P.songs.size();
    const float x = PanelX(), y = 92, h = P.H - 20 - y;
    const D2D1_RECT_F box = R(x, y, x + kPanelW, y + h);
    PanelBox(box, a);
    P.hits.push_back({ID_P_BG, box});
    Text(L"Music player", R(x + 18, y + 12, box.right - 52, y + 34), G.fTitle, Col(0xFFFFFF, a));
    const std::wstring sub = g_settings.sound == 0 ? L"Sound is off"
                           : !mine                ? L"Playing the video's own sound"
                           : n == 1               ? L"My music  \u00B7  1 song"
                                                  : L"My music  \u00B7  " + std::to_wstring(n) + L" songs";
    Text(sub, R(x + 18, y + 34, box.right - 52, y + 52), G.fSmall, Col(0xFFFFFF, 0.55f * a));
    IconBtn(ID_P_SIDE, box.right - 30, y + 31, 16, P.panelSide ? L"\uE72A" : L"\uE72B", a);
    G.br->SetColor(Col(0xFFFFFF, 0.08f * a));
    G.ctx->DrawLine(D2D1::Point2F(box.left + 14, y + 60), D2D1::Point2F(box.right - 14, y + 60), G.br, 1);
    const float lx = x + 16, cx = x + 16 + kLabelW, cr = box.right - 16;
    auto ctl = [&](float top) { return R(cx, top + 6, cr, top + kRow - 6); };
    float yy = y + 64;
    // Which player: off, or a style (step through them).
    Label(L"Player", lx, yy, a);
    {
        const std::vector<std::wstring> styles = Player_Styles();
        const int k = std::clamp(style, 0, (int)styles.size());
        const D2D1_RECT_F r = ctl(yy);
        Fill(r, Col(0xFFFFFF, 0.06f * a), 8);
        Text(k ? styles[k - 1] : L"Off", R(r.left + 26, r.top, r.right - 26, r.bottom), G.fBody, Col(0xFFFFFF, 0.9f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        IconBtn(ID_MU_STYLE_PREV, r.left + 14, (r.top + r.bottom) / 2, 12, L"\uE76B", a);
        IconBtn(ID_MU_STYLE_NEXT, r.right - 14, (r.top + r.bottom) / 2, 12, L"\uE76C", a);
    }
    yy += kRow;
    // Its colour (the play button, its place, what's lit): the eyedropper, any colour, the Windows
    // accent colour (the first, ticked), then quick ones from the wallpaper.
    const Settings::PlayerLook look = has ? g_settings.playerLooks[style] : Settings::PlayerLook{};
    const UINT32 accent = (UINT32)lroundf(g_accent.r * 255) << 16 | (UINT32)lroundf(g_accent.g * 255) << 8 | (UINT32)lroundf(g_accent.b * 255);
    const UINT32 ink = look.color.empty() ? accent : RgbOf(look.color);
    QuickColours();
    const bool quick = look.color.empty() || std::find(P.swatches.begin(), P.swatches.begin() + std::min<size_t>(4, P.swatches.size()), ink) !=
                                                 P.swatches.begin() + std::min<size_t>(4, P.swatches.size());
    Label(L"Colour", lx, yy, a, has);
    const float sy = yy + kRow / 2;
    ToolSwatch(ID_MU_PICK_COL, cx + 12, sy, L"\uEF3C", P.picking == 3, a, has);
    ToolSwatch(ID_MU_GRID_COL, cx + 39, sy, L"\uE710", P.gridFor == 3 || !quick, a, has, quick ? nullptr : &ink);
    Swatch(ID_MU_COL0, cx + 66, sy, accent, look.color.empty(), a, has, L"\uE7F4");  // (Windows' own)
    for (size_t i = 0; i < P.swatches.size() && i < 4; i++)
        Swatch(ID_MU_COL0 + 1 + (int)i, cx + 93 + 27 * i, sy, P.swatches[i], !look.color.empty() && P.swatches[i] == ink, a, has);
    const float colourRow = yy + kRow;
    yy += kRow;
    // The player's own colour (its card or case): the eyedropper, any colour, its own (the first), quick ones.
    const UINT32 own = 0x141417, bodyInk = look.body.empty() ? own : RgbOf(look.body);
    const bool bodyQuick = look.body.empty() || std::find(P.swatches.begin(), P.swatches.begin() + std::min<size_t>(4, P.swatches.size()), bodyInk) !=
                                                    P.swatches.begin() + std::min<size_t>(4, P.swatches.size());
    Label(L"Body", lx, yy, a, has);
    const float by = yy + kRow / 2;
    ToolSwatch(ID_MU_PICK_BODY, cx + 12, by, L"\uEF3C", P.picking == 4, a, has);
    ToolSwatch(ID_MU_GRID_BODY, cx + 39, by, L"\uE710", P.gridFor == 4 || !bodyQuick, a, has, bodyQuick ? nullptr : &bodyInk);
    Swatch(ID_MU_BODY0, cx + 66, by, own, look.body.empty(), a, has, L"\uE72C");  // (the style's own)
    for (size_t i = 0; i < P.swatches.size() && i < 4; i++)
        Swatch(ID_MU_BODY0 + 1 + (int)i, cx + 93 + 27 * i, by, P.swatches[i], !look.body.empty() && P.swatches[i] == bodyInk, a, has);
    const float bodyRow = yy + kRow;
    yy += kRow;
    wchar_t v[32];
    Label(L"Size", lx, yy, a, has);
    swprintf(v, 32, L"%.0f%%", look.size * 100);
    SliderCtl(ID_MU_SIZE, ctl(yy), (look.size - 0.7f) / 0.8f, v, a, has);
    yy += kRow;
    Label(L"Opacity", lx, yy, a, has);
    swprintf(v, 32, L"%.0f%%", look.opacity * 100);
    SliderCtl(ID_MU_OPACITY, ctl(yy), (look.opacity - 0.2f) / 0.8f, v, a, has);
    yy += kRow;
    const bool corners = has && Player_HasCorners(style);
    const float radius = has ? Player_Corners(style) : 0;
    Label(L"Corners", lx, yy, a, corners);
    swprintf(v, 32, L"%.0f", radius);
    SliderCtl(ID_MU_CORNERS, ctl(yy), radius / 40, corners ? v : L"", a, corners);
    yy += kRow;
    // Back to how this style comes (only when it's been changed).
    if (has && !look.IsDefault()) {
        const std::wstring t = L"Reset " + Player_Styles()[style - 1];
        const float tw = TextWidth(t, G.fBody);
        const D2D1_RECT_F r = R(cr - tw, yy + 4, cr, yy + 26);
        Text(t, r, G.fBody, P.hot == ID_MU_RESET ? Col(0xFFFFFF, a) : g_accent * a);
        P.hits.push_back({ID_MU_RESET, r});
    }
    if (!has || !Player_HasVolume(style)) {  // (no volume on the player: here)
        swprintf(v, 32, L"%d%%", g_settings.volume);
        Label(L"Volume", lx, yy, a, g_settings.sound != 0);
        SliderCtl(ID_MU_VOLUME, R(cx, yy + 6, has && !look.IsDefault() ? cr - 90 : cr, yy + kRow - 6), g_settings.volume / 100.f, v, a,
                  g_settings.sound != 0);
    }
    yy += kRow;
    G.br->SetColor(Col(0xFFFFFF, 0.08f * a));
    G.ctx->DrawLine(D2D1::Point2F(box.left + 14, yy + 2), D2D1::Point2F(box.right - 14, yy + 2), G.br, 1);
    yy += 12;
    // The playlist: shuffle and repeat on its heading, then choosing songs; the songs fill the rest.
    if (!mine) {
        Text(L"To play your own songs, choose My music under Sound in All settings.", R(x + 18, yy, box.right - 18, yy + 44), G.fWrap,
             Col(0xFFFFFF, 0.7f * a));
    } else {
        Text(L"Playlist", R(x + 18, yy, x + 120, yy + 24), G.fBold, Col(0xFFFFFF, 0.92f * a));
        const std::wstring pick = L"Choose songs\u2026";
        const float pw = TextWidth(pick, G.fBody);
        const D2D1_RECT_F pr = R(box.right - 18 - pw, yy, box.right - 18, yy + 24);
        Text(pick, pr, G.fBody, P.hot == ID_MU_PICK ? Col(0xFFFFFF, a) : g_accent * a);
        P.hits.push_back({ID_MU_PICK, pr});
        if (n) {
            const bool shuffled = MusicShuffled(MusicOrderFor(g_settings, n)), one = g_settings.musicRepeat == 2;  // (lit while on)
            const float hy = yy + 12, bx = pr.left - 22;
            IconBtn(ID_MU_REPEAT, bx, hy, 14, one ? L"\uE8ED" : L"\uE8EE", a, true, one ? g_accent : D2D1::ColorF(1, 1, 1));
            IconBtn(ID_MU_SHUFFLE, bx - 34, hy, 14, L"\uE8B1", a, n > 1, shuffled ? g_accent : D2D1::ColorF(1, 1, 1));
            yy += 30;
            const int rows = std::max(1, (int)((box.bottom - 10 - yy) / kSongRow));
            DrawSongs(R(x + 8, yy, box.right - 8, yy + std::min(n, rows) * kSongRow), a);
        } else {
            Text(L"Choose songs (a whole folder works too), and they play with the wallpaper.", R(x + 18, yy + 30, box.right - 18, yy + 74),
                 G.fWrap, Col(0xFFFFFF, 0.7f * a));
        }
    }
    if (P.gridFor == 3 && has) DrawColourGrid(a, cx + 19, colourRow, ink);
    if (P.gridFor == 4 && has) DrawColourGrid(a, cx + 19, bodyRow, bodyInk);
}

// A small mouse with its right button lit: "right-click" in a tip.
void DrawRightClick(float x, float cy, float a) {
    Fill(R(x + 6, cy - 6.5f, x + 10, cy - 1.5f), g_accent * a, 1.5f);
    Stroke(R(x + 0.5f, cy - 7, x + 10.5f, cy + 7), Col(0xFFFFFF, 0.72f * a), 5, 1.1f);
    G.br->SetColor(Col(0xFFFFFF, 0.72f * a));
    G.ctx->DrawLine(D2D1::Point2F(x + 5.5f, cy - 7), D2D1::Point2F(x + 5.5f, cy - 1), G.br, 1);
    G.ctx->DrawLine(D2D1::Point2F(x + 0.5f, cy - 1), D2D1::Point2F(x + 10.5f, cy - 1), G.br, 1);
}

// The line under the title: what can be done, with dots between. A tip starting with \1 has the
// right-click mouse in front of it. Returns its width; draws it when `draw` is set.
float Tips(const std::vector<std::wstring>& tips, float x, float y, float a, bool draw) {
    const std::wstring dot = L"\u00B7";
    const float gap = 9, dotW = TextWidth(dot, G.fSmall);
    float w = 0;
    for (size_t i = 0; i < tips.size(); i++) {
        if (i) {
            w += gap;
            if (draw) Text(dot, R(x + w, y, x + w + dotW + 2, y + 18), G.fSmall, Col(0xFFFFFF, 0.4f * a));
            w += dotW + gap;
        }
        std::wstring s = tips[i];
        if (!s.empty() && s[0] == L'\1') {
            if (draw) DrawRightClick(x + w, y + 9, a);
            w += 16;
            s.erase(0, 1);
        }
        const float tw = TextWidth(s, G.fSmall);
        if (draw) Text(s, R(x + w, y, x + w + tw + 4, y + 18), G.fSmall, Col(0xFFFFFF, 0.62f * a));
        w += tw;
    }
    return w;
}

// The wallpaper's and the tray app's RAM, as the settings window's sidebar shows them.
void UpdateRam() {
    auto mb = [](SIZE_T b) {
        wchar_t t[32];
        swprintf(t, 32, b < 10 * 1048576 ? L"%.1f MB" : L"%.0f MB", b / 1048576.0);
        return std::wstring(t);
    };
    SIZE_T wall = 0, gfx = 0;
    const std::wstring ram = Host_RendererMemory(&wall, &gfx) ? mb(wall) + L"  \u00B7  " + mb(Host_SelfMemory()) : mb(Host_SelfMemory());
    if (ram == P.ram) return;
    P.ram = ram;
    P.dirty = true;
}

void DrawTopBar(float a) {
    if (a <= 0) return;
    const float y = 18 - (1 - a) * 12;
    // Left: what this is, and what to press.
    std::wstring title;
    std::vector<std::wstring> tips;
    if (P.mode == 1) {
        title = L"Clock";
        tips = {L"Drag it where you want it", L"Arrow keys nudge it", L"Esc goes back"};
    } else if (P.mode == 3) {
        title = L"Music";
        tips = {L"The player works here as on your desktop", L"Esc goes back"};
    } else if (P.mode == 2) {
        title = L"Crop";
        tips = {L"Drag to move it", L"Scroll to zoom", L"Esc goes back"};
    } else if (P.cards.empty()) {
        title = L"Wallpapers";
        tips = {L"No videos yet", L"Esc closes"};
    } else {
        title = L"Wallpapers";
        tips = {std::to_wstring(P.cards.size()) + (P.cards.size() == 1 ? L" video" : L" videos"), L"\u2190 \u2192 browse",
                L"Enter sets", L"\1the middle card: more", L"Esc closes"};
    }
    const std::wstring ram = P.mode == 0 ? P.ram : L"";
    const float ramW = ram.empty() ? 0 : TextWidth(ram, G.fSmall) + 14;
    const float w = 60 + std::max(TextWidth(title, G.fTitle) + ramW, Tips(tips, 0, 0, a, false)) + 18;
    const D2D1_RECT_F box = R(20, y, 20 + w, y + 52);
    const float la = a * (1 - P.hideA);
    Fill(box, Col(0x141417, 0.74f * la), 16);
    Stroke(box, Col(0xFFFFFF, 0.08f * la), 16, 1);
    if (G.logo) G.ctx->DrawBitmap(G.logo, R(box.left + 12, box.top + 10, box.left + 44, box.top + 42), la);
    Text(title, R(box.left + 54, box.top + 7, box.right - 10, box.top + 29), G.fTitle, Col(0xFFFFFF, la));
    if (!ram.empty()) {
        const float rx = box.left + 54 + TextWidth(title, G.fTitle) + 14;
        Text(ram, R(rx, box.top + 9, rx + ramW, box.top + 29), G.fSmall, Col(0xFFFFFF, 0.55f * la));
    }
    Tips(tips, box.left + 54, box.top + 28, la, true);

    // Right: what can be done.
    float x = P.W - 20;
    auto place = [&](int id, const wchar_t* icon, const std::wstring& label, bool accent = false, float pa = -1, wchar_t key = 0) {
        const float pw = PillWidth(icon, label, key);
        if (pa < 0) pa = a;
        x -= pw;
        Pill(pa > 0.5f ? id : 0, R(x, y + 8, x + pw, y + 44), icon, label, pa, accent, key);
        x -= 8;
    };
    if (P.mode == 1) {  // the eye stays when the rest hides: it brings it back
        place(ID_E_EYE, P.panelHidden ? L"\uE7B3" : L"\uE890", L"");
        place(ID_E_SAVE, L"\uE73E", L"Save", true, a * (1 - P.hideA));
        place(ID_E_CANCEL, nullptr, L"Cancel", false, a * (1 - P.hideA));
        return;
    }
    if (P.mode == 3) {  // the eye stays when the rest hides: it brings it back
        place(ID_E_EYE, P.panelHidden ? L"\uE7B3" : L"\uE890", L"");
        place(ID_MU_DONE, L"\uE73E", L"Done", true, a * (1 - P.hideA));
        return;
    }
    if (P.mode == 2) {
        place(ID_E_SAVE, L"\uE73E", L"Save", true);
        place(ID_E_CANCEL, nullptr, L"Cancel");
        place(ID_C_RESET, L"\uE72C", L"Whole picture");
        return;
    }
    place(ID_CLOSE, L"\uE711", L"");
    place(ID_MENU, L"\uE713", L"Settings", false, -1, L'S');
    place(ID_CLOCK, L"\uE917", L"Clock", false, -1, L'C');
    place(ID_MUSIC, L"\uE8D6", L"Music", false, -1, L'M');
}

// The dropdown under Settings: everything else, and the way back to the classic look.
void DrawMenu() {
    if (P.menuA <= 0) return;
    const float a = P.menuA;
    D2D1_RECT_F anchor{};
    for (auto& h : P.hits)
        if (h.id == ID_MENU) anchor = h.r;
    const float w = 286, x = std::min(P.W - 20 - w, anchor.right - w), y = anchor.bottom + 10 - (1 - a) * 8;
    const struct { int id; const wchar_t* icon; const wchar_t* label; } items[] = {
        {ID_M_SETTINGS, L"\uE713", L"All settings"},
        {ID_M_ADD, L"\uE710", L"Add videos\u2026"},
        {ID_M_LOCK, L"\uE72E", L"Show on lock screen"},
        {ID_M_OFF, L"\uE7E8", L"Turn the video wallpaper off"},
        {ID_M_NATIVE, L"\uE7F4", L"Switch to the classic look"},
    };
    const float h = 12 + 40 * (float)std::size(items) + 12 + 8;
    const D2D1_RECT_F box = R(x, y, x + w, y + h);
    PanelBox(box, a, 14);
    float yy = box.top + 10;
    for (size_t n = 0; n < std::size(items); n++) {
        if (n == std::size(items) - 1) {  // the look switch, set apart
            G.br->SetColor(Col(0xFFFFFF, 0.08f * a));
            G.ctx->DrawLine(D2D1::Point2F(box.left + 14, yy + 6), D2D1::Point2F(box.right - 14, yy + 6), G.br, 1);
            yy += 12;
        }
        const D2D1_RECT_F r = R(box.left + 6, yy, box.right - 6, yy + 40);
        if (P.hot == items[n].id) Fill(r, Col(0xFFFFFF, 0.08f * a), 9);
        const D2D1_COLOR_F fg = Col(0xFFFFFF, 0.92f * a);
        Text(items[n].icon, R(r.left + 10, r.top, r.left + 34, r.bottom), G.fIcon, fg, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(items[n].label, R(r.left + 44, r.top, r.right - 10, r.bottom), G.fBody, fg);
        if (items[n].id == ID_M_LOCK) {  // a switch
            const D2D1_RECT_F sw = R(r.right - 52, r.top + 11, r.right - 14, r.bottom - 11);
            const bool on = g_settings.lockFollow;
            if (on) Fill(sw, g_accent * a, 9);
            else Stroke(sw, Col(0xFFFFFF, 0.6f * a), 9, 1.2f);
            Circle(on ? sw.right - 9 : sw.left + 9, (sw.top + sw.bottom) / 2, 5, on ? Col(0x000000, 0.85f * a) : Col(0xFFFFFF, 0.8f * a));
        }
        P.hits.push_back({items[n].id, r});
        yy += 40;
    }
}

// Above the focused card: why it can't be used, if it can't (a click on it sets it).
void DrawCardPill(float a) {
    if (P.cards.empty() || P.focus < 0 || a <= 0) return;
    const Pose p = CardPose(P.focus);
    const float settle = 1 - std::min(1.f, fabsf(P.scroll - P.focus) * 2.5f);
    a *= settle * p.a;
    if (a <= 0.01f) return;
    const float top = p.y - kCardH * p.s / 2 - 14 - 36;
    Card& c = P.cards[P.focus];
    if (c.failed || (c.ready && !c.info.ok)) {
        const std::wstring t = c.failed ? L"This video can't play here" : L"Can't use this one";
        const float w = PillWidth(L"\uE7BA", t);
        Pill(0, R(p.x - w / 2, top, p.x + w / 2, top + 36), L"\uE7BA", t, a);
    }
}

// ---------------------------------------------------------------------------------------
// The middle card's actions (a right-click on it): the bin and its versions, out of its side

constexpr UINT32 kRed = 0xE5484D;

// Where action k (0 the bin, 1 the versions) is: t 0 tucked in the card's edge, 1 out beside it.
D2D1_POINT_2F ActionAt(int k, float t) {
    const Pose p = CardPose(P.actionsCard);
    const float hw = kCardW * p.s / 2, hh = kCardH * p.s / 2;
    return D2D1::Point2F(Lerp(p.x + hw - 24, p.x + hw + 36, t), Lerp(p.y - hh * 0.5f, p.y - hh * 0.62f, t) + k * 58);
}

// The card's versions (the original and the lighter copies VideoBG made), beside the buttons.
void DrawVersions() {
    if (P.versionsA <= 0) return;
    const float a = P.versionsA;
    Card& c = P.cards[P.actionsCard];
    const D2D1_POINT_2F b = ActionAt(1, 1);
    const int n = 1 + (int)P.versionList.size();
    const float w = 300, rowH = 48, h = 58 + n * rowH + 40;
    float x = b.x + 34;
    if (x + w > P.W - 20) x = CardPose(P.actionsCard).x - kCardW * CardPose(P.actionsCard).s / 2 - 20 - w;  // no room: the other side
    const float y = std::clamp(b.y - 70, 90.f, P.H - h - 20) + (1 - a) * 8;
    const D2D1_RECT_F box = R(x, y, x + w, y + h);
    PanelBox(box, a, 14);
    P.hits.push_back({ID_P_BG, box});
    Text(L"Versions", R(x + 18, y + 12, box.right - 16, y + 34), G.fTitle, Col(0xFFFFFF, a));
    Text(L"Smaller ones use less memory", R(x + 18, y + 33, box.right - 16, y + 51), G.fSmall, Col(0xFFFFFF, 0.55f * a));
    float yy = y + 58;
    wchar_t b1[64], b2[96];
    for (int k = 0; k < n; k++) {
        const std::wstring& path = k == 0 ? c.path : P.versionList[k - 1].path;
        const UINT vw = k == 0 ? c.info.vw : P.versionList[k - 1].w, vh = k == 0 ? c.info.vh : P.versionList[k - 1].h;
        const ULONGLONG bytes = k == 0 ? c.info.bytes : P.versionList[k - 1].bytes;
        swprintf(b1, 64, k == 0 ? L"Original" : L"%up", vh);
        if (vw && vh) swprintf(b2, 96, L"%u \u00D7 %u  \u00B7  %ls", vw, vh, Bytes(bytes).c_str());
        else swprintf(b2, 96, L"%ls", bytes ? Bytes(bytes).c_str() : L"");
        const D2D1_RECT_F r = R(x + 6, yy, box.right - 6, yy + rowH - 4);
        const bool cur = SameFile(path, c.play);
        if (P.hot == ID_V0 + k && !cur) Fill(r, Col(0xFFFFFF, 0.08f * a), 9);
        else if (cur) Fill(r, Col(0xFFFFFF, 0.05f * a), 9);
        if (cur) Text(L"\uE73E", R(r.left + 6, r.top, r.left + 30, r.bottom), G.fIcon, g_accent * a, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(b1, R(r.left + 36, r.top + 4, r.right - 70, r.top + 24), G.fBold, Col(0xFFFFFF, 0.95f * a));
        Text(b2, R(r.left + 36, r.top + 23, r.right - 10, r.top + 41), G.fSmall, Col(0xFFFFFF, 0.6f * a));
        if (cur) Text(P.actionsCard == P.applied ? L"In use" : L"Chosen", R(r.right - 70, r.top + 4, r.right - 12, r.top + 24), G.fSmall, g_accent * a, DWRITE_TEXT_ALIGNMENT_TRAILING);
        P.hits.push_back({ID_V0 + k, r});
        yy += rowH;
    }
    // Making one takes the settings window (it shows its progress there); it's made for the wallpaper.
    const D2D1_RECT_F more = R(x + 18, yy + 4, box.right - 16, yy + 30);
    if (P.actionsCard == P.applied) {
        Text(L"Make a lighter version\u2026", more, G.fBody, P.hot == ID_V_MORE ? Col(0xFFFFFF, a) : g_accent * a);
        P.hits.push_back({ID_V_MORE, R(more.left, more.top, more.left + TextWidth(L"Make a lighter version\u2026", G.fBody), more.bottom)});
    } else {
        Text(L"Set it as your wallpaper to make lighter ones", more, G.fSmall, Col(0xFFFFFF, 0.45f * a));
    }
}

void DrawCardActions() {
    if (P.actionsA <= 0 || P.actionsCard < 0 || P.actionsCard >= (int)P.cards.size()) return;
    G.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    const bool wallpaper = P.actionsCard == P.applied;
    if (P.actionsA > 0.5f) {  // over the buttons and between them, the cards underneath don't light up
        const D2D1_POINT_2F b0 = ActionAt(0, 1), b2 = ActionAt(2, 1);
        P.hits.push_back({ID_A_ZONE, R(b0.x - 44, b0.y - 40, b0.x + 44, b2.y + 40)});
    }
    for (int k = 0; k < 3; k++) {
        const float t = Clamp01(P.actionsA * 1.6f - k * 0.3f), e = EaseBack(t), a = Clamp01(t * 2.5f);
        if (a <= 0) continue;
        const D2D1_POINT_2F c = ActionAt(k, e);
        const int id = k == 0 ? ID_A_BIN : k == 1 ? ID_A_VERSIONS : ID_A_CROP;
        const bool hot = P.hot == id;
        const float r = 22 * (0.5f + 0.5f * e) * (hot ? 1.06f : 1);
        Circle(c.x, c.y + 3, r + 2, Col(0x000000, 0.3f * a));  // a little shadow
        if (k == 0) {  // the bin; once clicked, a tick: a second click takes the card off
            Circle(c.x, c.y, r, Col(kRed, a));
            const float tick = EaseOut(P.binTick);
            Text(L"\uE74D", R(c.x - r, c.y - r, c.x + r, c.y + r), G.fIconBig, Col(0xFFFFFF, a * (1 - tick)), DWRITE_TEXT_ALIGNMENT_CENTER);
            if (tick > 0) {
                const float s = 0.6f + 0.4f * tick + 0.25f * sinf(tick * 3.14159f);
                G.ctx->SetTransform(D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F(c.x, c.y)));
                Text(L"\uE73E", R(c.x - r, c.y - r, c.x + r, c.y + r), G.fIconBig, Col(0xFFFFFF, a * tick), DWRITE_TEXT_ALIGNMENT_CENTER);
                G.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
            }
        } else if (k == 2) {  // crop (the wallpaper's: it's shown live on the screen)
            Circle(c.x, c.y, r, Col(hot ? 0x2A2A2F : 0x1E1E22, 0.96f * a));
            Ring(c.x, c.y, r, Col(0xFFFFFF, 0.14f * a), 1);
            Text(L"\uE7A8", R(c.x - r, c.y - r, c.x + r, c.y + r), G.fIconBig, Col(0xFFFFFF, (wallpaper ? 0.92f : 0.4f) * a),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {  // the versions, with how many lighter ones there are
            Circle(c.x, c.y, r, Col(hot || P.versions ? 0x2A2A2F : 0x1E1E22, 0.96f * a));
            Ring(c.x, c.y, r, Col(0xFFFFFF, (P.versions ? 0.4f : 0.14f) * a), 1);
            Text(L"\uE81E", R(c.x - r, c.y - r, c.x + r, c.y + r), G.fIconBig, Col(0xFFFFFF, 0.92f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
            if (!P.versionList.empty()) {
                const float bx = c.x + r * 0.72f, by = c.y - r * 0.72f;
                Circle(bx, by, 8.5f, g_accent * a);
                Text(std::to_wstring(P.versionList.size() + 1), R(bx - 8, by - 9, bx + 8, by + 8), G.fRowValue, Col(0x000000, 0.85f * a),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
        if (t < 0.99f) continue;  // (out all the way: then it takes clicks)
        P.hits.push_back({id, R(c.x - r, c.y - r, c.x + r, c.y + r)});
        // What it does, beside it while pointed at (the clicked bin always says).
        const bool tip = k == 0 ? P.binArmed || hot : k == 1 ? hot && !P.versions : hot;
        if (!tip) continue;
        const std::wstring s = k == 0   ? (P.binArmed ? L"Click again to take it off the list" : L"Take it off the list")
                             : k == 1   ? L"Versions"
                             : wallpaper ? L"Crop and picture"
                                         : L"Crop: set it as your wallpaper first";
        const float tw = TextWidth(s, G.fBody) + 26;
        const D2D1_RECT_F tr = R(c.x + r + 10, c.y - 15, c.x + r + 10 + tw, c.y + 15);
        Fill(tr, Col(0x141417, 0.92f * a), 15);
        Text(s, tr, G.fBody, k == 0 && P.binArmed ? Col(0xFF8A8D, a) : Col(0xFFFFFF, a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    DrawVersions();
}

void DrawEmpty(float a) {
    if (!P.cards.empty() || P.mode != 0 || a <= 0) return;
    const float w = 420, h = 180, x = (P.W - w) / 2, y = P.H - h - 80;
    const D2D1_RECT_F box = R(x, y, x + w, y + h);
    Fill(box, Col(0x141417, 0.9f * a), 20);
    Stroke(box, Col(0xFFFFFF, 0.1f * a), 20, 1);
    Text(L"Your wallpapers live here", R(x + 28, y + 26, x + w - 28, y + 60), G.fHero, Col(0xFFFFFF, a));
    Text(L"Add the videos you'd like to switch between.", R(x + 28, y + 62, x + w - 28, y + 86), G.fBody, Col(0xFFFFFF, 0.7f * a));
    const float bw = PillWidth(L"\uE710", L"Add videos");
    Pill(ID_EMPTY_ADD, R(x + 28, y + 110, x + 28 + bw, y + 150), L"\uE710", L"Add videos", a, true);
}

void DrawToast() {
    if (P.toastA <= 0 || P.toast.empty()) return;
    const float w = TextWidth(P.toast, G.fBody) + 36, y = 86;
    const D2D1_RECT_F r = R((P.W - w) / 2, y, (P.W + w) / 2, y + 36);
    Fill(r, Col(0x141417, 0.9f * P.toastA), 18);
    Text(P.toast, r, G.fBody, Col(0xFFFFFF, P.toastA), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// Placing the clock: a dashed frame around it, and guide lines when it snaps to the middle.
void DrawClockEdit(float a) {
    if (P.mode != 1 || !P.clockBmp || !P.look.show) return;
    const D2D1_RECT_F r = ClockRect();
    const D2D1_RECT_F box = R(r.left + 6, r.top + 6, r.right - 6, r.bottom - 6);
    G.br->SetColor(g_accent);
    if (P.clockDrag && P.snapX) G.ctx->DrawLine(D2D1::Point2F(P.W / 2, 0), D2D1::Point2F(P.W / 2, P.H), G.br, 1);
    if (P.clockDrag && P.snapY) G.ctx->DrawLine(D2D1::Point2F(0, P.H / 2), D2D1::Point2F(P.W, P.H / 2), G.br, 1);
    Stroke(box, Col(0xFFFFFF, (P.clockHot || P.clockDrag ? 0.9f : 0.5f) * a), 10, 1.5f, G.dash);
    // The handle that turns it: above the middle of its frame, quiet like the frame (it brightens
    // when pointed at); its angle beside it while it turns. A double click puts it upright.
    const float hx = (box.left + box.right) / 2, hy = box.top - 22;
    const bool lit = P.hot == ID_P_ROTATE || P.rotating;
    G.br->SetColor(Col(0xFFFFFF, (lit ? 0.5f : 0.25f) * a));
    G.ctx->DrawLine(D2D1::Point2F(hx, box.top), D2D1::Point2F(hx, hy + 11), G.br, 1, G.dash);
    Circle(hx, hy, 11, Col(0x000000, 0.28f * a));
    Ring(hx, hy, 11, Col(0xFFFFFF, (lit ? 0.75f : 0.4f) * a), 1);
    Text(L"\uE7AD", R(hx - 11, hy - 11, hx + 11, hy + 11), G.fSmallIcon, Col(0xFFFFFF, (lit ? 0.9f : 0.55f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
    P.hits.push_back({ID_P_ROTATE, R(hx - 14, hy - 14, hx + 14, hy + 14)});
    if (P.rotating || (P.hot == ID_P_ROTATE && P.look.rotate != 0)) {
        wchar_t t[16];
        swprintf(t, 16, L"%.0f\u00B0", P.look.rotate);
        Text(t, R(hx + 18, hy - 10, hx + 80, hy + 10), G.fSmall, Col(0xFFFFFF, 0.8f * a));
    }
}


// Cropping: under the live wallpaper (dragged and zoomed right on the screen), the timeline for the
// picture the lock screen and previews use.
void DrawCropEdit(float a) {
    Card* c = WallpaperCard();
    if (!c || a <= 0) return;
    const float w = std::min(620.f, P.W - 40), x = (P.W - w) / 2, y = P.H - 24 - 76;
    const D2D1_RECT_F box = R(x, y, x + w, y + 76);
    PanelBox(box, a);
    P.hits.push_back({ID_P_BG, box});
    Text(L"Picture for your lock screen and previews", R(x + 18, y + 10, x + w - 18, y + 32), G.fBold, Col(0xFFFFFF, a));
    const double dur = c->info.duration;
    SliderCtl(ID_C_FRAME, R(x + 10, y + 36, x + w - 14, y + 66), dur > 0 ? (float)(P.frameTime / dur) : 0, Duration(P.frameTime), a, dur > 0);
}

// Choosing (Step keeps the time): the chosen card comes up to the middle of the screen, bigger,
// and glows while the rest dims; its picture gives way to its video, playing in its place, and its
// "Current" badge pops in. A moment later the card fades away around the video, which grows to fill
// the screen. (In the middle, it waits for its video if that's still starting.) The renderer draws
// the video and the dimming by itself, at every refresh, on the same clock (SendLook).
constexpr float kSelRise0 = 0.05f, kSelRise1 = 0.8f, kSelPill = 1.05f, kSelReveal = 1.55f;  // from the click
constexpr float kSelDim = 0.45f, kSelDimFor = 0.5f;  // the rest of the screen: how dark, how quickly
constexpr float kSelHole = 0.2f;                      // the picture giving way to the video
constexpr float kSelFade = 0.38f, kSelGrowAt = 0.1f, kSelGrow = 0.6f, kSelEnd = kSelGrowAt + kSelGrow + 0.04f;  // from the reveal

Pose ChosenPose(float t) {
    const float lift = EaseInOut((t - kSelRise0) / (kSelRise1 - kSelRise0));
    const float big = std::min(P.H * 0.5f / kCardH, 2.f * P.k);
    return Pose{Lerp(P.chosenFrom.x, P.W / 2, lift), Lerp(P.chosenFrom.y, P.H / 2, lift), Lerp(P.chosenRot, 0, lift),
                Lerp(P.chosenScale, big, lift), 1, 0};
}

float SelBright(float t) { return 1 - (1 - kSelDim) * EaseInOut(t / kSelDimFor); }  // the rest of the screen, dimming (as the renderer does)

float SelReveal() { return P.revealAt > 0 ? (float)(Now() - P.revealAt) : -1; }

// The chosen card's picture, in the middle of the screen (DIPs): where its video plays.
D2D1_RECT_F PicBox() {
    const D2D1_MATRIX_3X2_F m = CardMatrix(ChosenPose(kSelRise1));
    const D2D1_POINT_2F tl = Apply(m, D2D1::Point2F(-kCardW / 2 + kPad, -kCardH / 2 + kPad));
    const D2D1_POINT_2F br = Apply(m, D2D1::Point2F(kCardW / 2 - kPad, -kCardH / 2 + kPad + kThumbH));
    return R(tl.x, tl.y, br.x, br.y);
}

// The renderer's part, told as each stage starts: 1 the rest dims, 2 the video plays in the card's
// picture, 3 that grows to fill the screen; 0 the showcase as usual.
void SendLook(int stage) {
    if (stage == P.lookSent) return;
    P.lookSent = stage;
    if (!stage) {
        Host_ShowcaseLook(nullptr);
        return;
    }
    ShowcaseLook l;
    l.bright = kSelDim;
    l.dimAt = P.chosenAt;
    l.dimFor = kSelDimFor;
    if (stage >= 2) {  // a pixel bigger all round: the card's edge covers it
        const D2D1_RECT_F b = PicBox();
        l.box = RECT{(LONG)floorf(b.left * P.scale) - 1, (LONG)floorf(b.top * P.scale) - 1, (LONG)ceilf(b.right * P.scale) + 1,
                     (LONG)ceilf(b.bottom * P.scale) + 1};
    }
    if (stage >= 3) {
        l.growAt = P.revealAt + kSelGrowAt;
        l.growFor = kSelGrow;
    }
    Host_ShowcaseLook(&l);
}

void DrawChosen(float t) {
    Card& c = P.cards[P.chosen];
    const float r = SelReveal();
    const float a = r < 0 ? 1 : 1 - EaseInOut(r / kSelFade);  // the card fades away around its playing video
    if (a <= 0) return;  // just the video now, growing
    const Pose p = ChosenPose(t);
    const D2D1_MATRIX_3X2_F m = CardMatrix(p);
    const D2D1_RECT_F card = R(-kCardW / 2, -kCardH / 2, kCardW / 2, kCardH / 2);
    const D2D1_RECT_F pic = R(card.left + kPad, card.top + kPad, card.right - kPad, card.top + kPad + kThumbH);
    const bool hole = P.holeAt > 0;
    const float picA = hole ? 1 - Clamp01((float)(Now() - P.holeAt) / kSelHole) : 1;
    G.ctx->SetTransform(m);
    // Its shadow, and a soft light around it, breathing while it waits: never over its video.
    if (hole && G.glowMask) G.ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), G.glowMask), nullptr);
    if (G.shadow) {
        const D2D1_SIZE_F s = G.shadow->GetSize();
        G.ctx->DrawBitmap(G.shadow, R(-s.width / 2, -s.height / 2 + 10, s.width / 2, s.height / 2 + 10), 0.85f * a);
    }
    const float glow = Clamp01((t - kSelRise1 + 0.2f) / 0.35f) * a;
    if (glow > 0 && G.glow) {
        const D2D1_SIZE_F s = G.glow->GetSize();
        const float g = 1.03f + 0.02f * sinf((t - kSelRise1) * 6);
        G.ctx->DrawBitmap(G.glow, R(-s.width / 2 * g, -s.height / 2 * g, s.width / 2 * g, s.height / 2 * g), 0.7f * glow);
    }
    if (hole && G.glowMask) G.ctx->PopLayer();
    DrawCardContent(P.chosen, a, 1, picA, false, hole);
    Stroke(card, Col(0xFFFFFF, 0.85f * a), kCardR, 1.6f);
    if (glow > 0) Stroke(card, Col(0xFFFFFF, 0.85f * glow), kCardR, 1.8f);
    // Its "Current" badge pops in once it's there (the wallpaper's own goes first, and comes back).
    const float pill = Clamp01((t - kSelPill) / 0.22f);
    const float out = P.chosen == P.applied ? 1 - Clamp01(t / 0.12f) : 0;
    if (pill > 0 || out > 0) {
        const float s = pill > 0 ? EaseBack(pill) : 0.7f + 0.3f * out;
        G.ctx->SetTransform(D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F(pic.left + 41, pic.bottom - 15)) * m);
        DrawBadge(c, pic, a * (pill > 0 ? Clamp01(pill * 2) : out), false);
    }
    G.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Paint() {
    P.hits.clear();
    P.tracks.clear();
    if (P.mode == 0)
        for (int i = 0; i < (int)P.cards.size(); i++) {
            if (fabsf(i - P.scroll) > 16) SafeRelease(P.cards[i].image);
            else if (!P.cards[i].image && CardVisible(CardPose(i))) MakeCardImage(i);
        }
    G.ctx->BeginDraw();
    G.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    G.ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    DrawStillLayer();
    if (P.phase == SELECTING) {  // the rest dims as the card comes up (the renderer dims the live video alike)
        const float k = (1 - SelBright((float)(Now() - P.chosenAt))) * std::max(P.stills[0].a, P.stills[1].a);
        if (k > 0) Fill(R(0, 0, P.W, P.H), Col(0x000000, k));
    }
    DrawClock(P.phase == SELECTING ? 1 - Clamp01((float)(Now() - P.chosenAt) * 3)
              : P.chosenClose          ? 0
              : P.mode == 1 && !P.look.show ? 0.35f
                                        : 1);
    if (P.dark > 0) Fill(R(0, 0, P.W, P.H), Col(0x000000, P.dark));

    if (P.mode == 0 && P.chosenClose) {
        // Closing over the chosen picture: nothing more on it (it fades into the apps).
    } else if (P.mode == 0) {
        // The deck, back to front.
        std::vector<std::pair<float, int>> order;
        for (int i = 0; i < (int)P.cards.size(); i++)
            if (i != P.chosen) order.push_back({fabsf(i - P.scroll), i});
        std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
        const float selT = P.phase == SELECTING ? (float)(Now() - P.chosenAt) : 0;
        for (auto& [dist, i] : order) {
            Pose p = CardPose(i);
            if (P.phase == SELECTING) {  // the others drop away
                const float t = EaseInOut(selT / 0.4f);
                p.y += t * kCardH * P.k * 1.6f;
                p.a *= 1 - t;
            }
            if (!CardVisible(p)) continue;
            DrawCard(i, p, CardMatrix(p));
        }
        if (P.phase == SELECTING && P.chosen >= 0) DrawChosen(selT);
        DrawCardActions();
        G.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
        DrawCardPill(P.chosen < 0 ? P.chrome : 0);
        DrawEmpty(P.chrome);
    } else if (P.mode == 1) {
        DrawClockEdit(P.chrome * (1 - P.hideA));
        DrawClockPanel(P.chrome * P.panelA);
        DrawLoupe(P.pickA);
    } else if (P.mode == 3) {
        DrawMusicStage(P.chrome);
        DrawMusicEditPanel(P.chrome * P.panelA);
        DrawLoupe(P.pickA);
    } else {
        DrawCropEdit(P.chrome);
    }
    DrawTopBar(P.chrome * (1 - P.pickA));
    DrawMenu();
    DrawToast();
    HRESULT hr = G.ctx->EndDraw();
    if (FAILED(hr)) Log(L"picker: drawing failed 0x%08lx", (unsigned long)hr);
}

// ---------------------------------------------------------------------------------------
// Moving

void SetFocus(int i, bool scrollThere = true) {
    if (P.cards.empty()) return;
    i = std::clamp(i, 0, (int)P.cards.size() - 1);
    if (scrollThere) P.scrollTo = (float)i;
    if (i == P.focus) return;
    P.focus = i;
    FocusChanged();
}

void ShowToast(const std::wstring& t) {
    P.toast = t;
    P.toastOn = true;
    SetTimer(P.hwnd, TIMER_TOAST, 2400, nullptr);
    P.dirty = true;
}

void BeginClose(const wchar_t* why) {
    if (P.phase == CLOSING) return;
    Log(L"picker: closing (%ls)", why);
    if (!P.used) AskPreview(g_settings.video, 0);  // nothing chosen: the wallpaper's own video again
    P.phase = CLOSING;
    P.phaseAt = Now();
    P.menu = false;
    KillTimer(P.hwnd, TIMER_DWELL);
    P.dirty = true;
}

// The chosen card becomes the wallpaper: it rises, glows and opens out to fill the screen.
void Choose(int i) {
    if ((P.phase != BROWSING && P.phase != OPENING) || i < 0 || i >= (int)P.cards.size()) return;
    Card& c = P.cards[i];
    if (c.failed || (c.ready && !c.info.ok)) {
        ShowToast(c.info.fault == VF_GIF ? L"Turn the GIF into a video in Settings first" : L"This one can't be used as a wallpaper");
        return;
    }
    if (i == P.applied && SameFile(c.play, g_settings.video)) {  // it's the wallpaper already: just back to it
        BeginClose(L"Esc");
        return;
    }
    const Pose p = CardPose(i);
    P.chosen = i;
    P.chosenAt = P.phaseAt = Now();  // (the timeline in Step runs from here)
    P.revealAt = P.holeAt = 0;
    P.chosenFrom = D2D1::Point2F(p.x, p.y);
    P.chosenRot = p.rot;
    P.chosenScale = p.s;
    P.used = false;
    P.phase = SELECTING;
    P.menu = false;
    P.actions = P.versions = P.binArmed = false;
    KillTimer(P.hwnd, TIMER_DWELL);
    AskPreview(c.play, c.info.time);  // its video plays behind it while it comes up: then the change is seamless
    AskPictures();
    P.lookSent = 0;
    SendLook(1);
    P.dirty = true;
}

void AddVideos() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    const COMDLG_FILTERSPEC types[] = {{L"Videos", kVideoPatterns}, {L"All files", L"*.*"}};
    dlg->SetFileTypes((UINT)std::size(types), types);
    dlg->SetTitle(L"Add video wallpapers (Ctrl+A picks a whole folder)");
    dlg->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT);
    if (!P.cards.empty()) {
        const std::wstring& last = P.cards[P.focus].path;
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(last.substr(0, last.find_last_of(L"\\/")).c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    std::vector<std::wstring> picked;
    const bool wasEmpty = P.cards.empty();
    P.dialog = true;
    if (SUCCEEDED(dlg->Show(P.hwnd))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem* it = nullptr;
                PWSTR path = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    picked.push_back(path);
                    CoTaskMemFree(path);
                }
                if (it) it->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    P.dialog = false;
    SetForegroundWindow(P.hwnd);
    int added = 0, first = -1;
    for (auto& p : picked) {
        if (CardIndex(p) >= 0) {
            if (first < 0) first = CardIndex(p);
            continue;
        }
        P.cards.push_back(NewCard(p));
        if (first < 0) first = (int)P.cards.size() - 1;
        added++;
    }
    if (added) {
        SaveCards();
        Log(L"picker: %d video(s) added", added);
        ShowToast(added == 1 ? L"1 video added" : std::to_wstring(added) + L" videos added");
    }
    if (wasEmpty && first >= 0) {  // the first cards: nothing had the focus yet
        P.focus = first;
        P.scroll = P.scrollTo = (float)first;
        FocusChanged();
    } else if (first >= 0) {
        SetFocus(first);
    }
    AskPictures();
    P.dirty = true;
}

void RemoveFocused() {
    if (P.cards.empty()) return;
    const int i = P.focus;
    Log(L"picker: %ls taken off the list", P.cards[i].path.c_str());
    ShowToast(L"\u201C" + P.cards[i].title + L"\u201D is off the list (the file is still there)");
    ReleaseCardBitmaps(P.cards[i]);
    P.cards.erase(P.cards.begin() + i);
    for (auto& s : P.stills) {
        if (s.card == i) s = Still{};
        else if (s.card > i) s.card--;
    }
    if (P.applied == i) P.applied = -1;
    else if (P.applied > i) P.applied--;
    SaveCards();
    P.actions = P.versions = P.binArmed = false;
    P.actionsCard = -1;
    P.focus = -1;
    const int next = std::min(i, (int)P.cards.size() - 1);
    P.scroll = std::min(P.scroll, (float)std::max(0, (int)P.cards.size() - 1));
    if (next >= 0) SetFocus(next);
    P.dirty = true;
}

void OpenActions(bool armBin) {
    if (P.focus < 0 || P.focus >= (int)P.cards.size()) return;
    if (!P.actions || P.actionsCard != P.focus) {
        P.actionsA = 0;
        P.binTick = 0;
        P.versionList = UI_LightCopiesOf(P.cards[P.focus].path);
    }
    P.actions = true;
    P.actionsCard = P.focus;
    P.binArmed = armBin && P.focus != P.applied;
    P.versions = false;
    P.menu = P.music = false;
    P.dirty = true;
}

void CloseActions() {
    P.actions = P.versions = P.binArmed = false;
    P.dirty = true;
}

// A version of the card: the wallpaper switches to it if the card is the wallpaper; otherwise it's
// the one the card plays (and sets).
void UseVersion(int k) {
    Card& c = P.cards[P.actionsCard];
    const std::wstring path = k == 0 ? c.path : P.versionList[k - 1].path;
    if (SameFile(path, c.play)) return;
    SetPlay(c, path);
    SetVideoVersion(c.path, k == 0 ? L"" : path);  // (kept for next time)
    const std::wstring name = k == 0 ? std::wstring(L"the original") : L"the " + std::to_wstring(P.versionList[k - 1].h) + L"p version";
    if (P.actionsCard == P.applied) {
        Log(L"picker: the wallpaper's version is %ls now", path.c_str());
        Host_SetVideo(path);
        P.live = P.previewAsked = path;
        ShowToast(L"Your wallpaper plays " + name + L" now");
    } else {
        KillTimer(P.hwnd, TIMER_DWELL);
        SetTimer(P.hwnd, TIMER_DWELL, kDwellMs, nullptr);  // its preview, in this version
        ShowToast(L"Setting it uses " + name);
    }
    P.versions = false;
}

// Cropping the wallpaper, right on the screen: the renderer shows the crop live while it changes.
// The crop is kept as what the screen shows of the video (dragging moves that, zooming resizes it).
void ReleaseHold() {
    KillTimer(P.hwnd, TIMER_HOLD);
    if (P.held) Host_HoldWallpaper(P.held = false);
}

void CropChanged() {
    Host_PreviewCrop(&P.crop);
    if (Card* c = WallpaperCard()) c->crop = P.crop;  // the picture standing in for the video shows it too
    P.dirty = true;
}

Crop ScreenCrop(const Crop& c) {
    Card* w = WallpaperCard();
    if (c.stretch || !w || !w->info.vw || !w->info.vh) return c;
    const FitRect f = ComputeFit((double)w->info.vw / w->info.vh, c, 0, P.W, P.H);
    Crop s;
    s.l = f.sl;
    s.t = f.st;
    s.r = f.sr;
    s.b = f.sb;
    return s;
}

void PanCrop(float dx, float dy) {  // the picture follows the pointer
    Crop& k = P.crop;
    const float w = k.r - k.l, h = k.b - k.t;
    k.l = std::clamp(k.l - dx / P.W * w, 0.f, std::max(0.f, 1 - w));
    k.t = std::clamp(k.t - dy / P.H * h, 0.f, std::max(0.f, 1 - h));
    k.r = k.l + w;
    k.b = k.t + h;
    CropChanged();
}

void ZoomCropAt(float factor, float x, float y) {  // the point under the pointer stays where it is
    Crop& k = P.crop;
    const float fx = Clamp01(x / P.W), fy = Clamp01(y / P.H);
    const float px = k.l + fx * (k.r - k.l), py = k.t + fy * (k.b - k.t);
    float w = (k.r - k.l) * factor, h = (k.b - k.t) * factor;
    if (w > 1 || h > 1) {  // no bigger than the video, in the same shape
        const float s = std::min(1 / w, 1 / h);
        w *= s;
        h *= s;
    }
    if (w < 0.04f || h < 0.04f) return;
    k.l = std::clamp(px - fx * w, 0.f, std::max(0.f, 1 - w));
    k.t = std::clamp(py - fy * h, 0.f, std::max(0.f, 1 - h));
    k.r = k.l + w;
    k.b = k.t + h;
    CropChanged();
}

void StartCropEdit() {
    Card* c = WallpaperCard();
    if (!c || !c->info.ok) return;
    CloseActions();
    P.mode = 2;
    P.menu = P.music = false;
    KillTimer(P.hwnd, TIMER_DWELL);
    AskPreview(g_settings.video, 0);  // the wallpaper's own video, live
    P.cropWas = g_settings.crop;
    P.crop = ScreenCrop(g_settings.crop);
    P.cropDrag = false;
    P.frameTime = PreviewTimeFor(g_settings.previewTime, c->info.duration);
    P.frameMoved = false;
    P.stills[0] = Still{};
    P.stills[1] = Still{P.applied, P.showcaseUp && SameFile(P.live, g_settings.video) ? 0.f : 1.f};
    Log(L"picker: cropping %ls", g_settings.video.c_str());
    P.dirty = true;
}

// Back to the deck, the crop (and the picture's moment) saved or not.
void EndCropEdit(bool save) {
    if (P.mode != 2) return;
    Card* c = WallpaperCard();
    if (save) {
        g_settings.crop = P.crop;
        if (P.frameMoved) g_settings.previewTime = P.frameTime;
        Log(L"picker: crop saved (%.3f, %.3f, %.3f, %.3f)", P.crop.l, P.crop.t, P.crop.r, P.crop.b);
        Host_SettingsChanged();
        UI_Refresh();
    } else if (c) {
        c->crop = P.cropWas;
    }
    Host_PreviewCrop(nullptr);
    ReleaseHold();
    if (c) {  // its picture, made again with the crop it has now
        SafeRelease(c->brush);
        SafeRelease(c->image);
    }
    P.cropDrag = false;
    P.mode = 0;
    P.enter = 0;  // the cards rise again
    if (P.focus >= 0 && P.focus < (int)P.cards.size()) FocusChanged();
    P.dirty = true;
}

// The clock of the video in the middle (the wallpaper's when there are no cards), over that video.
void StartClockEdit() {
    const int card = P.focus >= 0 && P.focus < (int)P.cards.size() ? P.focus : -1;
    if (card < 0 && g_settings.video.empty()) {
        ShowToast(L"Choose a wallpaper first: each video has its own clock");
        return;
    }
    P.mode = 1;
    P.menu = P.music = false;
    P.actions = P.versions = P.binArmed = false;
    KillTimer(P.hwnd, TIMER_DWELL);
    const std::wstring play = card >= 0 ? P.cards[card].play : g_settings.video;
    P.editPlay = play;
    P.clockVideo = card >= 0 ? P.cards[card].path : g_settings.video;
    if (card < 0 || !P.cards[card].failed) AskPreview(play, card >= 0 ? P.cards[card].info.time : 0);
    P.look = LookFor(P.clockVideo);
    const std::wstring orig = OriginalOfCopy(P.clockVideo);
    P.clockFor = L"Just for " + FileTitle(orig.empty() ? P.clockVideo : orig);
    P.styles = ReadyStyles();
    P.langsOpen = P.panelHidden = false;
    P.gridFor = P.picking = 0;
    P.langScroll = 0;
    P.panelSide = g_panelSide;
    P.panelPos = (float)P.panelSide;
    P.panelV = 0;
    P.clockKey.clear();
    P.clockStyle.clear();
    UpdateClock();
    const int shown = card >= 0 ? card : P.applied;
    if (shown >= 0) {
        P.stills[0] = Still{};
        P.stills[1] = Still{shown, P.showcaseUp && SameFile(P.live, play) ? 0.f : 1.f};
    }
    P.dirty = true;
}

// Back to the deck from the clock, saved or not: the cards rise again.
void EndClockEdit() {
    if (P.mode != 1) return;
    P.mode = 0;
    P.clockDrag = P.clockHot = P.snapX = P.snapY = false;
    P.langsOpen = false;
    P.gridFor = P.picking = 0;
    P.slide = ID_NONE;
    P.enter = 0;
    P.clockKey.clear();
    P.clockStyle.clear();
    if (P.focus >= 0 && P.focus < (int)P.cards.size()) {
        P.look = LookFor(P.cards[P.focus].path);
        UpdateClock();
        FocusChanged();
    }
    P.dirty = true;
}

void EndPick(bool keep);

// The music screen: the middle card's video, the player on it, its panel (the deck steps aside).
void StartMusicEdit() {
    const int card = P.focus >= 0 && P.focus < (int)P.cards.size() ? P.focus : -1;
    P.mode = 3;
    P.menu = P.music = false;
    P.actions = P.versions = P.binArmed = false;
    KillTimer(P.hwnd, TIMER_DWELL);
    P.editPlay = card >= 0 ? P.cards[card].play : g_settings.video;
    if (card < 0 || !P.cards[card].failed) AskPreview(P.editPlay, card >= 0 ? P.cards[card].info.time : 0);
    P.songs = LoadPlaylist();
    P.songDrag = -1;
    P.gridFor = P.picking = 0;
    P.panelHidden = false;
    P.panelSide = g_panelSide;
    P.panelPos = (float)P.panelSide;
    P.panelV = 0;
    const int shown = card >= 0 ? card : P.applied;
    if (shown >= 0) {
        P.stills[0] = Still{};
        P.stills[1] = Still{shown, P.showcaseUp && SameFile(P.live, P.editPlay) ? 0.f : 1.f};
    }
    PreviewChanged();
}

void EndMusicEdit() {
    if (P.mode != 3) return;
    if (P.picking) EndPick(true);
    P.mode = 0;
    P.gridFor = 0;
    P.slide = ID_NONE;
    P.songDrag = -1;
    P.enter = 0;  // the cards rise again
    if (P.focus >= 0 && P.focus < (int)P.cards.size()) FocusChanged();
    P.dirty = true;
}

// The clock as edited becomes its video's own (the look the other videos share stays as it is).
void SaveClockLook() {
    Log(L"picker: the clock of %ls saved (%ls, at %.1f%%, %.1f%%)", P.clockVideo.c_str(), P.look.show ? L"shown" : L"hidden", P.look.x * 100,
        P.look.y * 100);
    if (VideoProfileId(P.clockVideo) != VideoProfileId(g_settings.video)) {  // another video's: kept for when it's the wallpaper
        SaveVideoClock(P.clockVideo, P.look);
        return;
    }
    g_settings.clockVideo = P.look;
    g_settings.clockVideoOwn = g_settings.clockVideoSaved = true;
    Host_SettingsChanged();
    UI_Refresh();
}

void ClockEdited() {
    UpdateClock();
    P.dirty = true;
}

// The colour a colour row edits: the clock's text (1) or glow (2), or the music player's (3).
std::wstring& PickColour(int row) {
    static std::wstring none;
    if (row == 3 || row == 4) {
        if (g_settings.musicPlayer <= 0) return none;
        Settings::PlayerLook& pl = g_settings.playerLooks[g_settings.musicPlayer];
        return row == 3 ? pl.color : pl.body;
    }
    return row == 2 ? P.look.glowColor : P.look.color;
}

// The player's look changed: shown on the screen and the desktop; kept (saved) when done.
void PlayerLookChanged(bool keep) {
    PreviewChanged();
    Player_Update();
    if (keep) SaveSettings(g_settings);
}

bool IsSlider(int id) {
    return id == ID_P_SIZE || id == ID_P_OPACITY || id == ID_P_GLOW || id == ID_P_GLOWSIZE || id == ID_MU_VOLUME || id == ID_MU_SEEK ||
           id == ID_C_FRAME || id == ID_MU_OPACITY || id == ID_MU_CORNERS || id == ID_MU_SIZE;
}

void SetSlider(int id, float f) {
    f = Clamp01(f);
    switch (id) {
        case ID_MU_VOLUME: {  // heard while dragging; kept when let go
            const int v = (int)lroundf(f * 100);
            if (v != g_settings.volume) {
                g_settings.volume = v;
                g_settings.videoMuted = false;  // turning it up or down means listening
                Host_LiveVolume(v);
                Player_Update();
            }
            P.dirty = true;
            return;
        }
        case ID_MU_OPACITY:  // the player's look: shown as it changes, kept when let go
        case ID_MU_CORNERS:
        case ID_MU_SIZE:
            if (g_settings.musicPlayer > 0) {
                Settings::PlayerLook& pl = g_settings.playerLooks[g_settings.musicPlayer];
                if (id == ID_MU_OPACITY) pl.opacity = roundf((0.2f + f * 0.8f) * 100) / 100;
                else if (id == ID_MU_SIZE) pl.size = roundf((0.7f + f * 0.8f) * 20) / 20;  // (5% steps)
                else pl.corners = roundf(f * 40);
                PlayerLookChanged(false);
            }
            return;
        case ID_MU_SEEK:  // the song moves there when let go
            P.seekFrac = f;
            P.dirty = true;
            return;
        case ID_C_FRAME:  // the wallpaper shows that moment as it's dragged (held still there)
            if (Card* c = WallpaperCard()) {
                P.frameTime = f * c->info.duration;
                P.frameMoved = true;
                KillTimer(P.hwnd, TIMER_HOLD);
                if (!P.held) Host_HoldWallpaper(P.held = true);
                Host_SeekWallpaper(P.frameTime, false);
            }
            P.dirty = true;
            return;
        case ID_P_SIZE: P.look.size = roundf((0.5f + f * 1.5f) * 100) / 100; break;
        case ID_P_OPACITY: P.look.opacity = roundf((0.2f + f * 0.8f) * 100) / 100; break;
        case ID_P_GLOW: P.look.glow = roundf(f * 100) / 100; break;
        case ID_P_GLOWSIZE: P.look.glowSize = roundf(2 + f * 38); break;
        default: return;
    }
    ClockEdited();
}

// The eyedropper: the colour under the pointer goes on the clock as it moves (not from the clock
// itself); a click keeps it, Esc or a right-click puts the old one back. The rest steps aside.
UINT32 ScreenColour(POINT pt) {
    HDC dc = GetDC(nullptr);
    const COLORREF c = GetPixel(dc, pt.x, pt.y);
    ReleaseDC(nullptr, dc);
    return c == CLR_INVALID ? 0 : (UINT32)GetRValue(c) << 16 | (UINT32)GetGValue(c) << 8 | GetBValue(c);
}

void StartPick(int row) {
    P.picking = row;
    P.pickWas = PickColour(row);
    P.pickRgb = RgbOf(row == 2 && !P.look.glowColor.empty() ? P.look.glowColor : row >= 3 ? P.pickWas : P.look.color);
    POINT pt;
    GetCursorPos(&pt);
    P.pickAt = D2D1::Point2F((pt.x - P.mon.left) / P.scale, (pt.y - P.mon.top) / P.scale);
    P.langsOpen = false;
    P.gridFor = 0;
    ShowToast(L"Point at a colour on your wallpaper and click (Esc cancels)");
}

void EndPick(bool keep) {
    if (!P.picking) return;
    if (!keep) PickColour(P.picking) = P.pickWas;
    const int row = P.picking;
    P.picking = 0;
    P.toastOn = false;
    if (row >= 3) PlayerLookChanged(true);
    else ClockEdited();
}

void PickMouse(UINT m, float x, float y) {
    if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) {
        EndPick(true);
        return;
    }
    if (m != WM_MOUSEMOVE) return;
    P.pickAt = D2D1::Point2F(x, y);
    if (!In(P.picking >= 3 ? P.playerBox : ClockRect(), x, y) && Now() - P.pickTime > 0.025) {
        P.pickTime = Now();
        POINT pt;
        GetCursorPos(&pt);
        P.pickRgb = ScreenColour(pt);
        PickColour(P.picking) = RgbText(P.pickRgb);
        if (P.picking >= 3) PlayerLookChanged(false);
        else ClockEdited();
    }
    P.dirty = true;
}

void SlideTo(float x) {
    for (const Hit& t : P.tracks)
        if (t.id == P.slide) {
            SetSlider(P.slide, (x - t.r.left) / std::max(1.f, t.r.right - t.r.left));
            return;
        }
}

void Nudge(float dx, float dy) {
    if (!P.look.show) return;  // a hidden clock stays put
    P.look.x = std::clamp(P.look.x + dx / P.W, 0.f, 1.f);
    P.look.y = std::clamp(P.look.y + dy / P.H, 0.f, 1.f);
    P.dirty = true;
}

void Command(int id) {
    switch (id) {
        case ID_CLOSE: BeginClose(L"close button"); break;
        case ID_MENU:
            P.menu = !P.menu;
            P.music = false;
            break;
        case ID_MUSIC: StartMusicEdit(); break;
        case ID_MU_DONE: EndMusicEdit(); break;
        case ID_MU_PICK_COL: StartPick(3); break;
        case ID_MU_PICK_BODY: StartPick(4); break;
        case ID_MU_GRID_COL:
            P.gridFor = P.gridFor == 3 ? 0 : 3;
            break;
        case ID_MU_GRID_BODY:
            P.gridFor = P.gridFor == 4 ? 0 : 4;
            break;
        case ID_MU_RESET:
            if (g_settings.musicPlayer > 0) {
                g_settings.playerLooks[g_settings.musicPlayer] = Settings::PlayerLook{};
                PlayerLookChanged(true);
            }
            break;
        case ID_MU_PLAY: Host_PlayMusic(!g_settings.musicPlaying); break;
        case ID_MU_STYLE_PREV:
        case ID_MU_STYLE_NEXT: {  // off, then each style, round again
            const int n = (int)Player_Styles().size() + 1;
            g_settings.musicPlayer = (std::clamp(g_settings.musicPlayer, 0, n - 1) + (id == ID_MU_STYLE_NEXT ? 1 : n - 1)) % n;
            Log(L"picker: desktop music player %ls", g_settings.musicPlayer ? Player_Styles()[g_settings.musicPlayer - 1].c_str() : L"off");
            Host_SettingsChanged();
            UI_Refresh();
            break;
        }
        case ID_MU_PREV:
        case ID_MU_NEXT: Host_SkipMusic(id == ID_MU_NEXT); break;
        case ID_MU_SHUFFLE:
            Host_ShuffleMusic(P.songs.size());
            UI_Refresh();
            break;
        case ID_MU_REPEAT:  // the playlist (always) or just this song
            g_settings.musicRepeat = g_settings.musicRepeat == 2 ? 1 : 2;
            Host_SettingsChanged();
            UI_Refresh();
            break;
        case ID_MU_PICK:
            P.dialog = true;
            if (Host_PickMusic(P.hwnd)) P.songs = LoadPlaylist();
            P.dialog = false;
            SetForegroundWindow(P.hwnd);
            break;
        case ID_EMPTY_ADD:
        case ID_M_ADD: P.menu = false; AddVideos(); break;
        case ID_CLOCK: StartClockEdit(); break;
        case ID_M_SETTINGS:
            P.menu = false;
            BeginClose(L"all settings");
            UI_Show();
            break;
        case ID_M_NATIVE:
            BeginClose(L"classic look");
            Host_SetLook(0);
            UI_Show();
            break;
        case ID_A_BIN:
            if (P.actionsCard == P.applied) {
                ShowToast(L"It's your wallpaper: set another one before taking it off");
            } else if (!P.binArmed) {
                P.binArmed = true;
                P.versions = false;
            } else {
                RemoveFocused();
            }
            break;
        case ID_A_VERSIONS:
            P.versions = !P.versions;
            P.binArmed = false;
            break;
        case ID_V_MORE:
            BeginClose(L"making a version");
            UI_ShowVersions();
            break;
        case ID_E_SAVE:
            if (P.mode == 2) {
                EndCropEdit(true);
                ShowToast(L"Crop saved");
                break;
            }
            SaveClockLook();
            EndClockEdit();
            ShowToast(L"Clock saved");
            break;
        case ID_E_CANCEL:
            if (P.mode == 2) EndCropEdit(false);
            else EndClockEdit();
            break;
        case ID_C_RESET:
            P.crop = ScreenCrop(Crop{});
            CropChanged();
            break;
        case ID_A_CROP:
            if (P.actionsCard == P.applied) StartCropEdit();
            else ShowToast(L"Set it as your wallpaper first, then crop it");
            break;
        case ID_M_LOCK:
            Host_SetLockFollow(!g_settings.lockFollow);
            UI_Refresh();
            break;
        case ID_M_OFF:
            P.turnOff = true;
            BeginClose(L"turning the wallpaper off");
            break;
        case ID_E_EYE:
            P.panelHidden = !P.panelHidden;
            P.langsOpen = false;
            break;
        case ID_P_SIDE: P.panelSide = g_panelSide = 1 - P.panelSide; break;
        case ID_P_SHOW:
        case ID_P_HIDE:
            P.look.show = id == ID_P_SHOW;
            P.langsOpen = false;
            ClockEdited();
            break;
        case ID_P_STYLE_PREV:
        case ID_P_STYLE_NEXT: {  // the next style (or the one before), round the list
            const int n = (int)P.styles.size();
            if (!n) break;
            int at = -1;
            for (int i = 0; i < n; i++)
                if (Clock_IsStyle(P.look, P.styles[i])) at = i;
            at = at < 0 ? 0 : (at + (id == ID_P_STYLE_NEXT ? 1 : n - 1)) % n;
            Clock_UseStyle(P.look, P.styles[at]);
            P.langsOpen = false;
            ClockEdited();
            break;
        }
        case ID_P_LANG:
            P.langsOpen = !P.langsOpen;
            P.gridFor = 0;
            if (P.langsOpen) {  // opened at the language it has
                if (P.langs.empty()) P.langs = Clock_Languages();
                for (size_t i = 0; i < P.langs.size(); i++)
                    if (P.langs[i].code == P.look.lang) P.langScroll = std::max(0.f, (i - 3.f) * 32);
            }
            break;
        case ID_P_DIG_LATIN:
        case ID_P_DIG_OWN:
            P.look.nativeDigits = id == ID_P_DIG_OWN;
            ClockEdited();
            break;
        case ID_P_PICK_TEXT:
        case ID_P_PICK_GLOW: StartPick(id == ID_P_PICK_TEXT ? 1 : 2); break;
        case ID_P_GRID_TEXT:
        case ID_P_GRID_GLOW: {
            const int row = id == ID_P_GRID_TEXT ? 1 : 2;
            P.gridFor = P.gridFor == row ? 0 : row;
            P.langsOpen = false;
            break;
        }
        case ID_P_DATE_PREV:
        case ID_P_DATE_NEXT: {
            const int n = std::max(1, Clock_DateStyleCount());
            P.look.date = (P.look.date + (id == ID_P_DATE_NEXT ? 1 : n - 1)) % n;
            ClockEdited();
            break;
        }
        case ID_P_H12:
        case ID_P_H24:
            P.look.h24 = id == ID_P_H24;
            ClockEdited();
            break;
        default:
            if (id >= ID_SONGX0 && id < ID_SONGX0 + (int)P.songs.size()) {  // taken off the playlist
                Host_RemoveSong(id - ID_SONGX0);
                P.songs = LoadPlaylist();
                P.dirty = true;
                break;
            }
            if (id >= ID_SONG0 && id < ID_SONG0 + (int)P.songs.size()) {  // a song: it plays
                Host_MusicCommand(MC_PLAY_INDEX, id - ID_SONG0);
                Host_PlayMusic(true);
            } else if (id >= ID_V0 && id <= ID_V0 + (int)P.versionList.size() && P.actionsCard >= 0) {
                UseVersion(id - ID_V0);
            } else if (id >= ID_P_LANG0 && id < ID_P_LANG0 + (int)P.langs.size()) {
                P.look.lang = P.langs[id - ID_P_LANG0].code;
                if (!HasOwnDigits()) P.look.nativeDigits = false;
                P.langsOpen = false;
                ClockEdited();
            } else if (UINT32 c; id >= ID_P_GRID0 && GridColourAt(id - ID_P_GRID0, &c)) {
                const int row = P.gridFor;
                PickColour(row) = RgbText(c);
                P.gridFor = 0;
                if (row >= 3) PlayerLookChanged(true);
                else ClockEdited();
            } else if (id >= ID_MU_BODY0 && id <= ID_MU_BODY0 + (int)P.swatches.size() && g_settings.musicPlayer > 0) {
                PickColour(4) = id == ID_MU_BODY0 ? L"" : RgbText(P.swatches[id - ID_MU_BODY0 - 1]);  // (0: the style's own)
                PlayerLookChanged(true);
            } else if (id >= ID_MU_COL0 && id <= ID_MU_COL0 + (int)P.swatches.size() && g_settings.musicPlayer > 0) {
                PickColour(3) = id == ID_MU_COL0 ? L"" : RgbText(P.swatches[id - ID_MU_COL0 - 1]);  // (0: the Windows accent colour)
                PlayerLookChanged(true);
            } else if (id >= ID_P_COLOR0 && id < ID_P_COLOR0 + (int)P.swatches.size()) {
                P.look.color = RgbText(P.swatches[id - ID_P_COLOR0]);
                ClockEdited();
            } else if (id >= ID_P_GLOWC0 && id <= ID_P_GLOWC0 + (int)P.swatches.size()) {
                P.look.glowColor = id == ID_P_GLOWC0 ? L"" : RgbText(P.swatches[id - ID_P_GLOWC0 - 1]);
                ClockEdited();
            }
            break;
    }
    P.dirty = true;
}

// ---------------------------------------------------------------------------------------
// Input

int HitControl(float x, float y) {
    for (auto it = P.hits.rbegin(); it != P.hits.rend(); ++it)
        if (In(it->r, x, y)) return it->id;
    return ID_NONE;
}

// The card under the pointer: front cards first.
int HitCard(float x, float y) {
    std::vector<std::pair<float, int>> order;
    for (int i = 0; i < (int)P.cards.size(); i++) order.push_back({fabsf(i - P.scroll), i});
    std::sort(order.begin(), order.end());
    for (auto& [dist, i] : order) {
        const Pose p = CardPose(i);
        if (!CardVisible(p)) continue;
        D2D1_MATRIX_3X2_F m = CardMatrix(p);
        if (!Invert(m)) continue;
        const D2D1_POINT_2F l = Apply(m, D2D1::Point2F(x, y));
        if (fabsf(l.x) <= kCardW / 2 && fabsf(l.y) <= kCardH / 2) return i;
    }
    return -1;
}

float Spacing() {  // pointer travel (DIPs) for one card while dragging the deck
    return kCardW * P.k * 0.93f;
}

void OnMouse(UINT m, WPARAM w, LPARAM l) {
    const float x = GET_X_LPARAM(l) / P.scale, y = GET_Y_LPARAM(l) / P.scale;
    if (P.phase != BROWSING && P.phase != OPENING) return;
    if (P.picking) {
        PickMouse(m, x, y);
        return;
    }
    switch (m) {
        case WM_MOUSEMOVE: {
            int hot = HitControl(x, y);
            const int card = P.mode == 0 && hot == ID_NONE && !P.down && !P.menu ? HitCard(x, y) : -1;
            if (card != P.hotCard) {
                P.hotCard = card;
                P.dirty = true;
            }
            if (P.pv.sliding) {  // the player's volume
                SetSlider(ID_MU_VOLUME, Player_VolumeAt(g_settings.musicPlayer, (x - P.playerBox.left) / P.playerK));
                PreviewChanged();
            } else if (P.pvMoving) {  // the player follows the pointer, on the desktop too
                g_settings.playerX = Clamp01((x - P.pvGrab.x) / P.W);
                g_settings.playerY = Clamp01((y - P.pvGrab.y) / P.H);
                Player_Place();
                P.dirty = true;
            } else if (P.pvSeeking) {  // its place: shown where it's dragged, the song goes there when let go
                P.pv.seek = Player_SeekAt(g_settings.musicPlayer, (x - P.playerBox.left) / P.playerK, (y - P.playerBox.top) / P.playerK);
                PreviewChanged();
            } else if (P.slide != ID_NONE) {
                SlideTo(x);
            } else if (P.songDrag >= 0) {
                P.songY = y;
                P.dirty = true;
            } else if (P.mode == 1 && P.rotating) {  // turning it round its centre
                const float cx = P.look.x * P.W, cy = P.look.y * P.H;
                const float r = Clock_RotateTo(P.rotFrom, P.rotPointer, Clock_PointerAngle(cx, cy, x, y), (w & MK_SHIFT) != 0);
                if (r != P.look.rotate) {
                    P.look.rotate = r;
                    ClockEdited();
                }
            } else if (P.mode == 1) {
                const bool over = In(ClockRect(), x, y) && hot == ID_NONE && P.look.show;
                if (P.clockDrag) {
                    float cx = x - P.grabX, cy = y - P.grabY;
                    P.snapX = fabsf(cx - P.W / 2) < 10 && !(w & MK_SHIFT);
                    P.snapY = fabsf(cy - P.H / 2) < 10 && !(w & MK_SHIFT);
                    if (P.snapX) cx = P.W / 2;
                    if (P.snapY) cy = P.H / 2;
                    P.look.x = std::clamp(cx / P.W, 0.f, 1.f);
                    P.look.y = std::clamp(cy / P.H, 0.f, 1.f);
                    P.dirty = true;
                }
                if (over != P.clockHot) P.dirty = true;
                P.clockHot = over;
            } else if (P.mode == 2) {
                if (P.cropDrag) {
                    PanCrop(x - P.cropX, y - P.cropY);
                    P.cropX = x;
                    P.cropY = y;
                }
            } else if (P.down) {
                const float dx = x - P.downAt.x / P.scale;
                if (!P.dragging && fabsf(dx) > 6) P.dragging = true;
                if (P.dragging) {
                    const double t = Now();
                    const float sc = P.downScroll - dx / Spacing();
                    if (t > P.dragT) P.flingV = P.flingV * 0.6f + 0.4f * (sc - P.dragX) / (float)std::max(0.004, t - P.dragT);
                    P.dragT = t;
                    P.dragX = sc;
                    P.scrollTo = std::clamp(sc, -0.4f, (float)P.cards.size() - 0.6f);
                    P.scroll = P.scrollTo;
                    P.scrollV = 0;
                    const int f = std::clamp((int)lroundf(P.scroll), 0, (int)P.cards.size() - 1);
                    if (f != P.focus) {
                        P.focus = f;
                        FocusChanged();
                    }
                    P.dirty = true;
                }
            }
            if (hot != P.hot) {
                P.hot = hot;
                P.dirty = true;
            }
            const int part = hot == ID_MU_PREVIEW ? PreviewPart(x, y) : PLAYER_NONE;
            if (part != P.pv.hot) {
                P.pv.hot = part;
                PreviewChanged();
            }
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, P.hwnd, 0};
            TrackMouseEvent(&tme);
            break;
        }
        case WM_MOUSELEAVE:
            P.hot = ID_NONE;
            P.hotCard = -1;
            if (P.pv.hot != PLAYER_NONE) {
                P.pv.hot = PLAYER_NONE;
                PreviewChanged();
            }
            P.dirty = true;
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            SetCapture(P.hwnd);
            P.press = HitControl(x, y);
            if ((P.menu || P.music || P.actions) && P.press == ID_NONE) {  // a click outside a dropdown closes it
                P.menu = P.music = false;
                CloseActions();
                P.dirty = true;
                break;
            }
            if (P.press == ID_MU_PREVIEW) {  // the player: a button, its volume or its place
                P.pv.press = PreviewPart(x, y);
                const float px = (x - P.playerBox.left) / P.playerK, py = (y - P.playerBox.top) / P.playerK;
                if (P.mode == 3 && (P.pv.press == PLAYER_BODY || ((g_settings.sound != 2 || P.songs.empty()) && P.pv.press != PLAYER_NONE))) {
                    P.pvMoving = true;  // (with no music, any part of it moves it)
                    P.pvGrab = D2D1::Point2F(x - (P.playerBox.left + P.playerBox.right) / 2, y - (P.playerBox.top + P.playerBox.bottom) / 2);
                } else if (P.pv.press == PLAYER_VOLUME) {
                    P.pv.sliding = true;
                    SetSlider(ID_MU_VOLUME, Player_VolumeAt(g_settings.musicPlayer, px));
                } else if (P.pv.press == PLAYER_SEEK) {
                    P.pvSeeking = true;
                    P.pv.seek = Player_SeekAt(g_settings.musicPlayer, px, py);
                }
                PreviewChanged();
                break;
            }
            if (P.mode == 1) {
                const bool inPopup = P.press == ID_P_LANG || P.press == ID_P_LIST || P.press == ID_P_GRID_TEXT || P.press == ID_P_GRID_GLOW ||
                                     (P.press >= ID_P_GRID0 && P.press < ID_P_GRID0 + 48) || P.press >= ID_P_LANG0;
                if ((P.langsOpen || P.gridFor) && !inPopup) {  // a click elsewhere puts the list (or the grid) away
                    P.langsOpen = false;
                    P.gridFor = 0;
                    P.dirty = true;
                }
                if (IsSlider(P.press)) {
                    P.slide = P.press;
                    SlideTo(x);
                    break;
                }
                if (P.press == ID_P_ROTATE) {  // takes hold of the handle (a double click: upright)
                    if (m == WM_LBUTTONDBLCLK) {
                        P.look.rotate = 0;
                        ClockEdited();
                    } else {
                        P.rotating = true;
                        P.rotFrom = P.look.rotate;
                        P.rotPointer = Clock_PointerAngle(P.look.x * P.W, P.look.y * P.H, x, y);
                    }
                    break;
                }
                if (P.press == ID_NONE && P.look.show && In(ClockRect(), x, y)) {  // (a hidden clock stays put)
                    P.clockDrag = true;
                    P.grabX = x - P.look.x * P.W;
                    P.grabY = y - P.look.y * P.H;
                }
                break;
            }
            if (IsSlider(P.press)) {
                P.slide = P.press;
                SlideTo(x);
                break;
            }
            if (P.press >= ID_GRIP0 && P.press < ID_GRIP0 + (int)P.songs.size()) {  // takes hold of a song
                P.songDrag = P.press - ID_GRIP0;
                for (const Hit& hh : P.hits)
                    if (hh.id == P.press) P.songGrab = y - hh.r.top;
                P.songY = y;
                P.dirty = true;
                break;
            }
            if (P.mode == 2) {
                if (P.press == ID_NONE) {  // takes hold of the picture
                    P.cropDrag = true;
                    P.cropX = x;
                    P.cropY = y;
                }
                break;
            }
            if (P.mode == 3) {  // (no deck to drag here) a click elsewhere puts the colour grid away
                if (P.gridFor && P.press != ID_MU_GRID_COL && P.press != ID_MU_GRID_BODY && P.press != ID_P_LIST && !(P.press >= ID_P_GRID0 && P.press < ID_P_GRID0 + 48))
                    P.gridFor = 0;
                P.dirty = true;
                break;
            }
            if (P.press == ID_NONE) {
                P.down = true;
                P.dragging = false;
                P.downAt = POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                P.downScroll = P.scroll;
                P.dragT = Now();
                P.dragX = P.scroll;
                P.flingV = 0;
            }
            P.dirty = true;
            break;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            if (P.press == ID_MU_PREVIEW) {
                const int part = P.pv.press;
                if (P.pvMoving) {
                    P.pvMoving = false;
                    SaveSettings(g_settings);
                    Log(L"picker: the music player moved to %.1f%%, %.1f%%", g_settings.playerX * 100, g_settings.playerY * 100);
                } else if (P.pv.sliding) {
                    Host_SettingsChanged();
                    UI_Refresh();
                } else if (P.pvSeeking) {
                    Player_Seek(P.pv.seek);
                } else if (part != PLAYER_NONE && part == PreviewPart(x, y)) {
                    if (part == PLAYER_PLAY) Host_PlayMusic(!g_settings.musicPlaying);
                    else if (part == PLAYER_PREV || part == PLAYER_NEXT) Host_SkipMusic(part == PLAYER_NEXT);
                }
                P.pv.press = PLAYER_NONE;
                P.pv.sliding = P.pvSeeking = false;
                P.pv.seek = -1;
                P.press = ID_NONE;
                PreviewChanged();
                break;
            }
            if (P.slide != ID_NONE) {
                if (P.slide == ID_MU_VOLUME) {
                    Host_SettingsChanged();
                    UI_Refresh();
                } else if (P.slide == ID_MU_OPACITY || P.slide == ID_MU_CORNERS || P.slide == ID_MU_SIZE) {
                    PlayerLookChanged(true);
                } else if (P.slide == ID_MU_SEEK) {
                    const MusicState ms = Host_MusicState();
                    if (ms.durMs) Host_MusicCommand(MC_SEEK, (LPARAM)(P.seekFrac * ms.durMs));
                    P.seekFrac = -1;
                } else if (P.slide == ID_C_FRAME) {
                    Host_SeekWallpaper(P.frameTime, true);  // to the very frame; it plays on after a second there
                    SetTimer(P.hwnd, TIMER_HOLD, 1000, nullptr);
                }
                P.slide = P.press = ID_NONE;
                P.dirty = true;
                break;
            }
            if (P.songDrag >= 0) {  // let go: the song lands in the gap
                Host_PlaceSong(P.songs.size(), P.songDrag, P.songIns);
                UI_Refresh();
                P.songDrag = -1;
                P.press = ID_NONE;
                P.dirty = true;
                break;
            }
            if (P.cropDrag) {
                P.cropDrag = false;
                P.press = ID_NONE;
                break;
            }
            if (P.clockDrag || P.rotating) {
                P.clockDrag = P.rotating = false;
                P.snapX = P.snapY = false;
                P.press = ID_NONE;
                P.dirty = true;
                break;
            }
            const int id = P.press;
            P.press = ID_NONE;
            if (id != ID_NONE && HitControl(x, y) == id) {
                Command(id);
                break;
            }
            if (P.down) {
                P.down = false;
                if (P.dragging) {  // let go: carries on a little in the same direction, then settles on a card
                    P.dragging = false;
                    const float v = std::clamp(P.flingV, -40.f, 40.f);
                    SetFocus((int)lroundf(P.scroll + v * 0.18f));
                    P.scrollTo = (float)P.focus;
                } else {
                    const int card = HitCard(x, y);
                    if (card >= 0 && card == P.focus) {
                        if (Settled()) Choose(card);  // still rolling there: not yet
                    } else if (card >= 0) {
                        SetFocus(card);
                    }
                }
            }
            P.dirty = true;
            break;
        }
    }
}

void OnWheel(int delta, LPARAM l) {
    if (P.mode == 2) {  // zooms the crop where the pointer is
        const float x = (GET_X_LPARAM(l) - P.mon.left) / P.scale, y = (GET_Y_LPARAM(l) - P.mon.top) / P.scale;
        ZoomCropAt(powf(0.92f, delta / 120.f), x, y);
        return;
    }
    if (P.music || P.mode == 3) {  // the playlist
        const float px = (GET_X_LPARAM(l) - P.mon.left) / P.scale, py = (GET_Y_LPARAM(l) - P.mon.top) / P.scale;
        if (In(P.songView, px, py)) {
            P.songScroll -= delta / 120.f * 60;  // kept inside the list when drawn
            P.dirty = true;
            return;
        }
    }
    if (P.mode == 1 && P.langsOpen) {  // the languages' list
        P.langScroll -= delta / 120.f * 64;  // kept inside the list when drawn
        P.dirty = true;
        return;
    }
    if ((P.phase != BROWSING && P.phase != OPENING) || P.mode != 0 || P.cards.empty()) return;
    if ((delta > 0) != (P.wheel > 0)) P.wheel = 0;  // turned the other way
    P.wheel += delta;
    while (fabsf(P.wheel) >= 60) {  // a notch is 120; touchpads send smaller steps, and half a notch starts a card
        const float sign = P.wheel > 0 ? 1.f : -1.f;
        P.wheel = sign * std::max(0.f, fabsf(P.wheel) - 120);  // always smaller: the loop ends
        SetFocus(P.focus - (int)sign);
    }
}

void OnKey(WPARAM vk) {
    if (P.phase != BROWSING && P.phase != OPENING) return;  // choosing or closing: nearly done
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    if (P.mode == 2) {
        const float step = shift ? 1.f : 10.f;
        switch (vk) {
            case VK_LEFT: PanCrop(-step, 0); break;
            case VK_RIGHT: PanCrop(step, 0); break;
            case VK_UP: PanCrop(0, -step); break;
            case VK_DOWN: PanCrop(0, step); break;
            case VK_ADD:
            case VK_OEM_PLUS: ZoomCropAt(0.92f, P.W / 2, P.H / 2); break;
            case VK_SUBTRACT:
            case VK_OEM_MINUS: ZoomCropAt(1 / 0.92f, P.W / 2, P.H / 2); break;
            case VK_RETURN: Command(ID_E_SAVE); break;
            case VK_ESCAPE: Command(ID_E_CANCEL); break;
        }
        return;
    }
    if ((P.mode == 1 || P.mode == 3) && P.picking) {
        if (vk == VK_ESCAPE || vk == VK_RETURN) EndPick(vk == VK_RETURN);
        return;
    }
    if (P.mode == 3) {
        if (vk == VK_ESCAPE && P.gridFor) P.gridFor = 0;
        else if (vk == VK_ESCAPE || vk == VK_RETURN || (vk == 'M' && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0)) EndMusicEdit();
        P.dirty = true;
        return;
    }
    if (P.mode == 1 && P.langsOpen) {  // the languages: a letter jumps, Esc puts them away
        if (vk >= 'A' && vk <= 'Z') LanguageJump((wchar_t)vk);
        else if (vk == VK_ESCAPE) P.langsOpen = false;
        P.dirty = true;
        return;
    }
    if (P.mode == 1) {
        const float step = shift ? 10.f : 1.f;
        switch (vk) {
            case VK_LEFT: Nudge(-step, 0); break;
            case VK_RIGHT: Nudge(step, 0); break;
            case VK_UP: Nudge(0, -step); break;
            case VK_DOWN: Nudge(0, step); break;
            case VK_RETURN: Command(ID_E_SAVE); break;
            case VK_ESCAPE: Command(ID_E_CANCEL); break;
        }
        return;
    }
    if (P.actions && vk == VK_ESCAPE) {
        CloseActions();
        return;
    }
    if ((P.menu || P.music) && vk == VK_ESCAPE) {
        P.menu = P.music = false;
        P.dirty = true;
        return;
    }
    switch (vk) {
        case VK_LEFT:
        case VK_UP: SetFocus(P.focus - 1); break;
        case VK_RIGHT:
        case VK_DOWN: SetFocus(P.focus + 1); break;
        case VK_PRIOR: SetFocus(P.focus - 5); break;
        case VK_NEXT: SetFocus(P.focus + 5); break;
        case VK_HOME: SetFocus(0); break;
        case VK_END: SetFocus((int)P.cards.size() - 1); break;
        case VK_RETURN: Choose(P.focus); break;
        case VK_DELETE:  // the bin, ready: Delete again (or a click on it) takes the card off
            if (P.actions) Command(ID_A_BIN);
            else OpenActions(true);
            break;
        case VK_ESCAPE: BeginClose(L"Esc"); break;
        case 'C':  // the Clock, Settings and Music buttons' keys
        case 'S':
        case 'M':
            if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) break;
            if (vk == 'C') {
                StartClockEdit();
                break;
            }
            CloseActions();
            Command(vk == 'S' ? ID_MENU : ID_MUSIC);
            P.dirty = true;
            break;
    }
}

// ---------------------------------------------------------------------------------------
// The frame loop: everything that moves, then a picture

void Destroy();

// True while anything still moves.
bool Step(double now) {
    const double gap = now - P.last;  // after a while still, a frame's worth (not a jump in what fades)
    const float dt = gap > 0.1 ? 1 / 60.f : (float)std::max(0.0, gap);
    P.last = now;
    bool moving = false;
    const float t = (float)(now - P.phaseAt);

    // The deck.
    if (!P.dragging) {
        const float before = P.scroll;
        Spring(P.scroll, P.scrollV, P.scrollTo, dt, 15);
        if (fabsf(P.scroll - P.scrollTo) < 0.0005f && fabsf(P.scrollV) < 0.001f) {
            P.scroll = P.scrollTo;
            P.scrollV = 0;
        }
        moving = moving || before != P.scroll;
    }
    // The live wallpaper shows once the showcase is up and plays the focused card's video.
    const std::wstring& wanted = (P.mode == 1 || P.mode == 3) && !P.editPlay.empty()                 ? P.editPlay
                               : P.mode != 0 || P.focus < 0 || P.focus >= (int)P.cards.size() ? g_settings.video
                                                                                               : P.cards[P.focus].play;
    const bool focusLive = P.showcaseUp && _wcsicmp(wanted.c_str(), P.live.c_str()) == 0;
    switch (P.phase) {
        case OPENING:
            moving = true;
            P.enter += dt;
            moving |= Approach(P.chrome, 1, 4, dt);
            if (P.showcaseUp || t > 1.5f) {  // without a showcase (no wallpaper yet), after a moment anyway
                P.darkTo = 0;
                if (t > 0.6f && P.dark <= 0) P.phase = BROWSING;
            }
            break;
        case BROWSING:
            moving |= P.enter < 2;
            P.enter += dt;
            moving |= Approach(P.chrome, 1, 4, dt);
            P.darkTo = 0;
            break;
        case SELECTING: {
            moving = true;
            Card& c = P.cards[P.chosen];
            if (c.failed) {  // its video couldn't play: back to the deck
                SendLook(0);
                P.chosen = -1;
                P.revealAt = P.holeAt = 0;
                P.phase = BROWSING;
                ShowToast(L"This video can't play here");
                break;
            }
            // In the middle, its picture gives way to its video once that plays on the showcase (the
            // pictures over it gone); then the card fades around it, and it grows.
            const bool live = focusLive && P.stills[0].a <= 0.01f && P.stills[1].a <= 0.01f;
            if (t >= kSelRise1) SendLook(std::max(P.lookSent, 2));
            if (P.holeAt <= 0 && t >= kSelRise1 && live) P.holeAt = now;
            const bool playing = P.holeAt > 0 && now - P.holeAt >= kSelHole;
            if (P.revealAt <= 0 && t > kSelReveal && (playing || t > kSelReveal + 3)) {
                P.revealAt = now;
                SendLook(3);
            }
            const float r = P.revealAt > 0 ? (float)(now - P.revealAt) : -1;
            P.chrome = std::max(0.f, 1 - t * 4);
            if (r > kSelEnd) {  // the video fills the screen: it's the wallpaper, back to the apps
                P.used = true;
                if (!SameFile(c.play, g_settings.video)) {
                    Log(L"picker: %ls is the wallpaper now", c.play.c_str());
                    Host_SetVideo(c.play);  // the renderer keeps playing it: it's the preview
                }
                SendLook(0);
                if (P.applied >= 0 && P.applied != P.chosen) SafeRelease(P.cards[P.applied].image);
                P.applied = P.chosen;
                SafeRelease(c.image);
                P.stills[0] = P.stills[1] = Still{};
                P.chosen = -1;
                P.revealAt = P.holeAt = 0;
                P.chosenClose = true;
                P.phase = CLOSING;
                P.phaseAt = now;
                P.chrome = 0;
            }
            break;
        }
        case CLOSING:
            moving = true;
            P.enter = std::max(0.f, 1 - t / 0.28f);
            P.chrome = std::max(0.f, P.chrome - dt * 6);
            P.menuA = P.musicA = P.actionsA = P.versionsA = 0;
            if (P.chosenClose) {  // the chosen picture covers the screen: the apps come back under it, then it fades into them
                if (P.showcaseAsked) {
                    P.showcaseAsked = false;
                    Host_Showcase(nullptr);
                }
                P.darkTo = 0;
                if (t > 0.2f)
                    for (auto& s : P.stills) s.a = std::max(0.f, s.a - dt * 3);
            } else if (t < 0.26f) {  // dark comes up over the live wallpaper
                if (P.stills[1].a < 0.99f) P.darkTo = 1;
            } else {  // dark covers it: the apps come back under it, then the dark goes
                if (P.showcaseAsked) {
                    P.showcaseAsked = false;
                    Host_Showcase(nullptr);
                }
                P.darkTo = 0;
                for (auto& s : P.stills) s.a = std::max(0.f, s.a - dt * 4.5f);
            }
            if (t > (P.chosenClose ? 0.05f : 0.3f) && P.dark <= 0 && P.stills[0].a <= 0 && P.stills[1].a <= 0) {
                Destroy();
                return false;
            }
            break;
    }
    // Black over everything (opening and closing), and the focused card's picture over the live one.
    moving |= Approach(P.dark, P.darkTo, P.phase == CLOSING || P.darkTo > P.dark ? 6.f : 4.f, dt);
    if (P.phase != CLOSING) {
        Still& top = P.stills[1];
        const bool loaded = top.card >= 0 && top.card < (int)P.cards.size() && (P.cards[top.card].frame || P.cards[top.card].thumb);
        if (!focusLive) {
            if (loaded) moving |= Approach(top.a, 1, 8, dt);
        } else {  // the video is up: the pictures fade into it (the one under the top too)
            moving |= Approach(top.a, 0, 3.5f, dt);
            moving |= Approach(P.stills[0].a, 0, 3.5f, dt);
        }
        if ((top.a >= 1 || P.stills[0].a <= 0) && P.stills[0].card >= 0) P.stills[0] = Still{};
    }
    moving |= Approach(P.menuA, P.menu ? 1.f : 0.f, 8, dt);
    moving |= Approach(P.musicA, P.music ? 1.f : 0.f, 8, dt);
    moving |= (P.music || P.mode == 3) && g_settings.musicPlayer > 0 && Player_Turns();  // its player, turning
    moving |= Approach(P.actionsA, P.actions ? 1.f : 0.f, P.actions ? 3.5f : 7.f, dt);
    moving |= Approach(P.versionsA, P.actions && P.versions ? 1.f : 0.f, 8, dt);
    moving |= Approach(P.binTick, P.binArmed ? 1.f : 0.f, 5, dt);
    const bool panel = P.mode == 1 || P.mode == 3;
    moving |= Approach(P.panelA, panel && !P.panelHidden && !P.picking ? 1.f : 0.f, 7, dt);
    moving |= Approach(P.hideA, panel && (P.panelHidden || P.picking) ? 1.f : 0.f, 7, dt);
    moving |= Approach(P.pickA, P.picking ? 1.f : 0.f, 8, dt);
    {
        const float before = P.panelPos;
        Spring(P.panelPos, P.panelV, (float)P.panelSide, dt, 16);
        if (fabsf(P.panelPos - P.panelSide) < 0.0005f && fabsf(P.panelV) < 0.001f) {
            P.panelPos = (float)P.panelSide;
            P.panelV = 0;
        }
        moving |= before != P.panelPos;
    }
    P.lift.resize(P.cards.size(), 0.f);
    for (int i = 0; i < (int)P.lift.size(); i++) moving |= Approach(P.lift[i], i == P.hotCard ? 1.f : 0.f, 6, dt);
    moving |= Approach(P.toastA, P.toastOn ? 1.f : 0.f, 6, dt);
    if (P.flipAt > 0) {  // the clock's flaps falling over
        const float p = (float)((now - P.flipAt) / kClockFlipSeconds);
        if (p >= 1) P.flipAt = 0;
        PaintClock(std::min(p, 1.f));
        moving = true;
    }
    return moving;
}

// ---------------------------------------------------------------------------------------
// Window

void Destroy() {
    if (!P.hwnd) return;
    Log(L"picker: closed (%d frames drawn, %.1f ms each on average, %.1f ms at most)", P.frames,
        P.frames ? P.drawTime * 1000 / P.frames : 0.0, P.drawMax * 1000);
    if (P.showcaseAsked) Host_Showcase(nullptr);
    if (!P.used) AskPreview(g_settings.video, 0);  // back to the wallpaper's own video
    Library_Stop();
    Player_FreePreview();
    HWND h = P.hwnd;
    P.hwnd = nullptr;
    MSG msg;
    while (PeekMessageW(&msg, h, WM_PK_INFO, WM_PK_INFO, PM_REMOVE)) delete (WallpaperInfo*)msg.lParam;
    ReleaseGraphics();
    P.cards.clear();
    P.lift.clear();
    DestroyWindow(h);
    if (P.turnOff) Host_SetOn(false);
    Host_TrimMemory();
}

LRESULT CALLBACK PickerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP:
            OnMouse(m, w, l);
            return 0;
        case WM_RBUTTONUP:  // on the middle card (once it's there): its actions
            if (P.picking) {
                EndPick(false);
                return 0;
            }
            if (P.mode == 0 && P.phase == BROWSING) {
                const float x = GET_X_LPARAM(l) / P.scale, y = GET_Y_LPARAM(l) / P.scale;
                const int card = HitControl(x, y) == ID_NONE ? HitCard(x, y) : -1;
                if (card >= 0 && card == P.focus && Settled() && !P.actions) OpenActions(false);
                else CloseActions();
                P.dirty = true;
            }
            return 0;
        case WM_MOUSEWHEEL: OnWheel(GET_WHEEL_DELTA_WPARAM(w), l); return 0;
        case WM_MOUSEHWHEEL:
            if (P.mode != 2) OnWheel(-GET_WHEEL_DELTA_WPARAM(w), l);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (w == VK_F4 && (GetKeyState(VK_MENU) < 0)) { BeginClose(L"Alt+F4"); return 0; }
            OnKey(w);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                const bool hand = (P.hot != ID_NONE && P.hot != ID_P_BG && P.hot != ID_P_LIST && P.hot != ID_A_ZONE && P.hot != ID_MU_LIST &&
                                   !(P.hot == ID_MU_PREVIEW && (P.pv.hot == PLAYER_NONE || P.pv.hot == PLAYER_BODY))) ||
                                  (P.mode == 1 && (P.clockHot || P.clockDrag));
                const wchar_t* cur = P.picking                                  ? IDC_CROSS
                                   : P.mode == 2 && (P.cropDrag || P.hot == ID_NONE) ? IDC_SIZEALL
                                   : P.mode == 1 && (P.clockHot || P.clockDrag) ? IDC_SIZEALL
                                   : P.mode == 3 && (P.pvMoving || (P.hot == ID_MU_PREVIEW && P.pv.hot == PLAYER_BODY)) ? IDC_SIZEALL
                                   : hand                                       ? IDC_HAND
                                                                                : IDC_ARROW;
                SetCursor(LoadCursorW(nullptr, cur));
                return TRUE;
            }
            break;
        case WM_ACTIVATE:  // clicking another window (or another screen) puts the picker away
            if (LOWORD(w) == WA_INACTIVE && !P.dialog && P.phase == BROWSING && !IsChild(h, (HWND)l)) BeginClose(L"another window came up");
            return 0;
        case WM_TIMER:
            if (w == TIMER_DWELL) Dwelled();
            else if (w == TIMER_MINUTE) { UpdateClock(); P.dirty = true; }
            else if (w == TIMER_TOAST) { KillTimer(h, TIMER_TOAST); P.toastOn = false; P.dirty = true; }
            else if (w == TIMER_HOLD) ReleaseHold();
            else if (w == TIMER_RAM) UpdateRam();
            return 0;
        case WM_PK_INFO:
            if (P.hwnd) OnInfo((WallpaperInfo*)l);
            else delete (WallpaperInfo*)l;
            return 0;
        case WM_DISPLAYCHANGE:
            BeginClose(L"the screens changed");
            return 0;
        case WM_CLOSE:
            BeginClose(L"asked to");
            return 0;
        case WM_NCHITTEST: return HTCLIENT;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

// ---------------------------------------------------------------------------------------

bool Modern_IsOpen() { return P.hwnd != nullptr; }

bool Modern_Open(int mode) {
    if (P.hwnd) {  // open already: switch to what was asked
        if (mode == 1 && P.mode == 0) StartClockEdit();
        SetForegroundWindow(P.hwnd);
        return true;
    }
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);  // the clock's screen
    P = Picker{};
    P.mode = mode;
    P.mon = mi.rcMonitor;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = PickerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassW(&wc) != 0;
    }
    const int pw = P.mon.right - P.mon.left, ph = P.mon.bottom - P.mon.top;
    P.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kClass, L"VideoBG wallpapers", WS_POPUP,
                             P.mon.left, P.mon.top, pw, ph, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!P.hwnd) return false;
    P.scale = GetDpiForWindow(P.hwnd) / 96.f;
    P.W = pw / P.scale;
    P.H = ph / P.scale;
    P.k = std::clamp(P.H / 1040.f, 0.62f, 1.4f);
    g_accent = Accent();
    if (!CreateGraphics()) {
        Log(L"picker: graphics couldn't start");
        ReleaseGraphics();
        DestroyWindow(P.hwnd);
        P.hwnd = nullptr;
        return false;
    }
    LoadCards();
    Library_Start(P.hwnd, WM_PK_INFO, pw, ph);
    P.focus = P.applied >= 0 ? P.applied : 0;
    P.scroll = P.scrollTo = (float)P.focus;
    P.live = g_settings.video;
    P.previewAsked = g_settings.video;
    P.phase = OPENING;
    P.phaseAt = P.last = Now();
    P.dark = 0;
    P.darkTo = 1;
    if (mode == 1) StartClockEdit();
    else if (!P.cards.empty()) {
        P.look = LookFor(P.cards[P.focus].path);
        UpdateClock();
        P.stills[1] = Still{P.focus, 0};
    }
    AskPictures();
    SetTimer(P.hwnd, TIMER_MINUTE, 15000, nullptr);
    UpdateRam();
    SetTimer(P.hwnd, TIMER_RAM, 2000, nullptr);
    ShowWindow(P.hwnd, SW_SHOW);
    SetForegroundWindow(P.hwnd);
    if (!Host_IsOn() && !g_settings.video.empty()) Host_SetOn(true);  // the picker shows the wallpaper: it plays
    Log(L"picker: open (%ls, %zu videos, %dx%d at %.0f%%)", mode == 1 ? L"placing the clock" : L"picking", P.cards.size(), pw, ph,
        P.scale * 100);
    P.dirty = true;
    return true;
}

void Modern_Close() {
    if (P.hwnd) BeginClose(L"asked to");
}

void Modern_Toggle() {
    if (P.hwnd && P.phase != CLOSING) BeginClose(L"the shortcut");
    else if (!P.hwnd) Modern_Open(0);
}

HANDLE Modern_FrameWait() {
    if (!P.hwnd || !G.wait) return nullptr;
    return P.dirty ? G.wait : nullptr;
}

void Modern_Frame() {
    if (!P.hwnd) return;
    // The dark comes up first, then the showcase goes under it (it appears without a flash).
    if (P.phase == OPENING && P.dark >= 1 && !P.showcaseAsked) AskShowcase();
    const double t0 = Now();
    const bool moving = Step(t0);
    if (!P.hwnd) return;  // closed in this step
    Paint();
    const double took = Now() - t0;  // the CPU side (the picture is shown at the next refresh)
    P.frames++;
    P.drawTime += took;
    P.drawMax = std::max(P.drawMax, took);
    G.sc->Present(1, 0);  // shown at the next refresh
    P.dirty = moving;
}

void Modern_SettingsChanged() {
    if (!P.hwnd || P.phase == SELECTING || P.phase == CLOSING) return;
    // The wallpaper's card: the version it plays and its crop, as they are now (whichever window
    // changed them). The rest is read as it's drawn.
    Card* w = WallpaperCard();
    if (!w || !SameFile(w->play, g_settings.video)) {
        int i = CardIndex(g_settings.video);
        if (i < 0) i = CardIndex(OriginalOfCopy(g_settings.video));
        if (w) SafeRelease(w->image);  // its "Current" badge goes
        P.applied = i;
        if ((w = WallpaperCard())) SetPlay(*w, g_settings.video);
    }
    if (w && P.mode != 2 && w->crop != g_settings.crop) {
        w->crop = g_settings.crop;
        SafeRelease(w->brush);
        SafeRelease(w->image);
    }
    // The clock on show follows its look (unless it's being edited here).
    if (P.mode == 0 && P.focus >= 0 && P.focus < (int)P.cards.size()) {
        P.look = LookFor(P.cards[P.focus].path);
        UpdateClock();
    }
    P.dirty = true;
}

void Modern_MusicChanged() {
    if (P.hwnd && (P.music || P.mode == 3)) P.dirty = true;
}

void Modern_RendererEvent(UINT msg, WPARAM w, LPARAM l) {
    if (!P.hwnd) return;
    switch (msg) {
        case WM_VBG_RENDERER_READY:  // a new renderer: it has no showcase or preview yet
            if (P.showcaseAsked) {
                P.showcaseAsked = false;
                P.showcaseUp = false;
                AskShowcase();
            }
            P.live = g_settings.video;
            P.previewAsked = g_settings.video;
            break;
        case WM_VBG_SHOWCASE_UP:
            P.showcaseUp = true;
            Log(L"picker: the wallpaper shows");
            break;
        case WM_VBG_PREVIEW_UP:
            if ((UINT)l != P.previewSeq) break;  // an answer to an earlier request: another one's on its way
            if (w) {
                P.live = P.previewAsked;
            } else {  // it couldn't play: the renderer went back to the wallpaper's own
                const int i = CardIndex(P.previewAsked);
                if (i >= 0) P.cards[i].failed = true;
                P.live = g_settings.video;
                P.previewAsked = g_settings.video;
            }
            break;
    }
    P.dirty = true;
}
