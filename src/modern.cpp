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
constexpr UINT_PTR TIMER_DWELL = 1, TIMER_MINUTE = 2, TIMER_TOAST = 3;
constexpr UINT kDwellMs = 1200;  // on a card this long, its video plays
const GUID kShadowEffect = {0xC67EA361, 0x1863, 0x4E69, {0x89, 0xDB, 0x69, 0x5D, 0x3E, 0x9A, 0x5B, 0x6B}};  // CLSID_D2D1Shadow

// Card layout, in card units (scaled with the screen by P.k): a portrait card with the video's
// picture, five rows of facts tinted with its colours, and its name.
constexpr float kCardW = 170, kCardH = 262, kCardR = 16, kPad = 8, kThumbH = 100, kRowH = 18, kRowGap = 3;
constexpr float kLift = 34, kGrow = 0.2f;  // the middle card: raised and bigger

enum Phase { OPENING, BROWSING, SELECTING, CLOSING };
enum Id {
    ID_NONE, ID_ADD, ID_CLOCK, ID_MENU, ID_CLOSE, ID_SET, ID_EMPTY_ADD,
    ID_M_SETTINGS, ID_M_CLOCK, ID_M_ADD, ID_M_REMOVE, ID_M_NATIVE,
    ID_RM_YES, ID_RM_NO, ID_E_SAVE, ID_E_CANCEL,
    ID_CARD = 1000,  // + card index
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
    IDWriteTextFormat *fTitle = nullptr, *fBody = nullptr, *fBold = nullptr, *fSmall = nullptr, *fIcon = nullptr,
                      *fIconBig = nullptr, *fCardTitle = nullptr, *fCardMeta = nullptr, *fRowLabel = nullptr, *fRowValue = nullptr,
                      *fHero = nullptr;
    ID2D1Bitmap* shadow = nullptr;  // a soft shadow for a card (drawn under each)
    ID2D1Bitmap* glow = nullptr;    // the same in white: the chosen card's glow
    ID2D1Bitmap* logo = nullptr;
} G;

// ---------------------------------------------------------------------------------------
// State

struct Card {
    std::wstring path, title, meta;  // the file's name; "MP4 \u00B7 its folder"
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

struct Picker {
    HWND hwnd = nullptr;
    int mode = 0;                     // 0 picking, 1 placing the clock
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
    bool confirmRemove = false;
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
    // Clock
    ClockLook look;                   // the clock that shows (the focused card's, or the one being placed)
    std::wstring clockKey;
    ID2D1Bitmap* clockBmp = nullptr;
    float clockW = 0, clockH = 0;     // DIPs
    bool clockDrag = false, clockHot = false, snapX = false, snapY = false;
    float grabX = 0, grabY = 0;
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
void Pill(int id, const D2D1_RECT_F& r, const wchar_t* icon, const std::wstring& label, float a, bool accent = false) {
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
    if (!label.empty()) Text(label, R(x, r.top, r.right - 12, r.bottom), G.fBold, fg);
    if (id) P.hits.push_back({id, r});
}

float PillWidth(const wchar_t* icon, const std::wstring& label) {
    return label.empty() ? 36 : 14 + (icon ? 24 : 0) + TextWidth(label, G.fBold) + 16;
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
    G.fIconBig = Format(icons, 20, DWRITE_FONT_WEIGHT_NORMAL, false);
    G.fCardTitle = Format(display, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    G.fCardMeta = Format(ui, 10.5f, DWRITE_FONT_WEIGHT_NORMAL);
    G.fRowLabel = Format(ui, 8.5f, DWRITE_FONT_WEIGHT_BOLD);
    G.fRowValue = Format(ui, 9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    G.shadow = MakeShadow(Col(0x000000, 0.75f), 14);
    G.glow = MakeShadow(Col(0xFFFFFF, 0.9f), 10);
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
    P.clockKey.clear();
    for (IDWriteTextFormat** f : {&G.fHero, &G.fTitle, &G.fBody, &G.fBold, &G.fSmall, &G.fIcon, &G.fIconBig, &G.fCardTitle, &G.fCardMeta,
                                  &G.fRowLabel, &G.fRowValue})
        SafeRelease(*f);
    SafeRelease(G.logo);
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
    const UINT32 big = std::max(i.vw, i.vh);
    const wchar_t* tag = big >= 3800 ? L"4K  \u00B7  " : big >= 2500 ? L"1440p  \u00B7  " : big >= 1900 ? L"1080p  \u00B7  " : L"";
    swprintf(b, 64, L"%ls%u \u00D7 %u", tag, i.vw, i.vh);
    c.rows[0][0] = L"PICTURE";
    c.rows[0][1] = b;
    swprintf(b, 64, i.fps > 0 ? L"%ls  \u00B7  %.0f fps" : L"%ls", Duration(i.duration).c_str(), i.fps);
    c.rows[1][0] = L"LENGTH";
    c.rows[1][1] = b;
    c.rows[2][0] = L"CODEC";
    c.rows[2][1] = i.codec.empty() || i.codec == L"?" ? L"Video" : i.codec;
    c.rows[3][0] = L"SOUND";
    c.rows[3][1] = i.sound ? L"Has sound" : L"Silent";
    c.rows[4][0] = L"FILE";
    c.rows[4][1] = Bytes(i.bytes);
}

void LoadCards() {
    std::vector<std::wstring> list = LoadWallpapers(g_settings.video);
    P.cards.clear();
    P.cards.resize(list.size());
    P.applied = -1;
    for (size_t i = 0; i < list.size(); i++) {
        Card& c = P.cards[i];
        c.path = list[i];
        c.title = FileTitle(c.path);
        c.meta = FileMeta(c.path);
        c.crop = LoadCropFor(c.path);
        if (_wcsicmp(c.path.c_str(), g_settings.video.c_str()) == 0) P.applied = (int)i;
    }
}

int CardIndex(const std::wstring& path) {
    for (size_t i = 0; i < P.cards.size(); i++)
        if (_wcsicmp(P.cards[i].path.c_str(), path.c_str()) == 0) return (int)i;
    return -1;
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

void UpdateClock() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t when[64];
    swprintf(when, 64, L"|%.3f|%04d%02d%02d%02d%02d", P.scale, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    const std::wstring key = P.look.show ? Clock_StyleKey(P.look) + when : L"";
    if (key == P.clockKey) return;
    P.clockKey = key;
    SafeRelease(P.clockBmp);
    if (key.empty()) return;
    ClockImage img;
    if (!Clock_Paint(P.look, P.scale, t, &img)) return;
    P.clockBmp = BitmapFrom(img.px, img.w, img.h, true);
    P.clockW = img.w / P.scale;
    P.clockH = img.h / P.scale;
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
    P.confirmRemove = false;
    AskPictures();
    if (P.focus < 0 || P.focus >= (int)P.cards.size()) return;
    Card& c = P.cards[P.focus];
    if (P.mode == 0 && P.phase != CLOSING) {
        P.look = LookFor(c.path);
        UpdateClock();
    }
    // Its picture goes over whatever shows now, unless the live wallpaper is it already.
    const bool isLive = P.showcaseUp && _wcsicmp(c.path.c_str(), P.live.c_str()) == 0;
    if (!isLive && P.stills[1].card != P.focus) {
        if (P.stills[1].card >= 0 && P.stills[1].a > 0.01f) P.stills[0] = P.stills[1];
        P.stills[1] = Still{P.focus, 0};
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
    const bool own = P.focus == P.applied;
    AskPreview(own ? g_settings.video : c.path, c.info.time);
}

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

// What a card shows (its picture, facts and name), in its own coordinates: the centre at 0,0.
void DrawCardContent(int i, float a, float infoA) {
    Card& c = P.cards[i];
    const D2D1_RECT_F card = R(-kCardW / 2, -kCardH / 2, kCardW / 2, kCardH / 2);
    Fill(card, Col(0x141417, 0.96f * a), kCardR);
    const D2D1_RECT_F pic = R(card.left + kPad, card.top + kPad, card.right - kPad, card.top + kPad + kThumbH);
    PictureBrush(c, pic);
    if (c.brush) {
        c.brush->SetOpacity(a);
        G.ctx->FillRoundedRectangle(D2D1::RoundedRect(pic, 10, 10), c.brush);
    } else {
        Fill(pic, Col(0x222226, a), 10);
        const bool bad = c.ready && !c.info.ok;
        Text(bad ? L"\uE783" : L"\uE714", pic, G.fIconBig, Col(0xFFFFFF, 0.35f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (i == P.applied) {  // the wallpaper now
        const D2D1_RECT_F chip = R(pic.left + 6, pic.bottom - 24, pic.left + 74, pic.bottom - 6);
        Fill(chip, Col(0x000000, 0.55f * a), 9);
        CachedText(c.text[12], L"\uE73E", R(chip.left + 6, chip.top, chip.left + 18, chip.bottom), G.fIcon, g_accent * a,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
        CachedText(c.text[13], L"Current", R(chip.left + 20, chip.top, chip.right - 4, chip.bottom), G.fRowValue, Col(0xFFFFFF, a));
    }
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
            CachedText(c.text[r], c.rows[r][0], R(row.left + 7, row.top, row.left + 60, row.bottom), G.fRowLabel, ink);
            CachedText(c.text[5 + r], c.rows[r][1], R(row.left + 52, row.top, row.right - 7, row.bottom), G.fRowValue, ink,
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

void DrawStillLayer() {
    for (const Still& s : P.stills) {
        if (s.card < 0 || s.card >= (int)P.cards.size() || s.a <= 0) continue;
        Card& c = P.cards[s.card];
        ID2D1Bitmap* bm = c.frame ? c.frame : c.thumb;
        if (!bm) continue;
        const D2D1_SIZE_F sz = bm->GetSize();
        const double aspect = c.info.vw && c.info.vh ? (double)c.info.vw / c.info.vh : sz.width / sz.height;
        FitRect f = ComputeFit(aspect, c.crop, CropMode(c.crop), P.W, P.H);
        Fill(R(0, 0, P.W, P.H), Col(0x000000, s.a));
        G.ctx->DrawBitmap(bm, R(f.dl, f.dt, f.dr, f.db), s.a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                          R(f.sl * sz.width, f.st * sz.height, f.sr * sz.width, f.sb * sz.height));
    }
}

void DrawClock(float a) {
    if (!P.clockBmp || a <= 0) return;
    G.ctx->DrawBitmap(P.clockBmp, ClockRect(), a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void DrawTopBar(float a) {
    if (a <= 0) return;
    const float y = 18 - (1 - a) * 12;
    // Left: what this is.
    const std::wstring sub = P.mode == 1 ? L"Drag the clock where you want it"
                           : P.cards.empty() ? L"No videos yet"
                           : std::to_wstring(P.cards.size()) + (P.cards.size() == 1 ? L" video" : L" videos") +
                                 L"   \u2190 \u2192 browse   \u00B7   Enter sets   \u00B7   Esc closes";
    const std::wstring title = P.mode == 1 ? L"Place the clock" : L"Wallpapers";
    const float w = 60 + std::max(TextWidth(title, G.fTitle), TextWidth(sub, G.fSmall)) + 18;
    const D2D1_RECT_F box = R(20, y, 20 + w, y + 52);
    Fill(box, Col(0x141417, 0.74f * a), 16);
    Stroke(box, Col(0xFFFFFF, 0.08f * a), 16, 1);
    if (G.logo) G.ctx->DrawBitmap(G.logo, R(box.left + 12, box.top + 10, box.left + 44, box.top + 42), a);
    Text(title, R(box.left + 54, box.top + 7, box.right - 10, box.top + 29), G.fTitle, Col(0xFFFFFF, a));
    Text(sub, R(box.left + 54, box.top + 28, box.right - 10, box.top + 46), G.fSmall, Col(0xFFFFFF, 0.62f * a));

    // Right: what can be done.
    float x = P.W - 20;
    auto place = [&](int id, const wchar_t* icon, const std::wstring& label, bool accent = false) {
        const float pw = PillWidth(icon, label);
        x -= pw;
        Pill(id, R(x, y + 8, x + pw, y + 44), icon, label, a, accent);
        x -= 8;
    };
    if (P.mode == 1) {
        place(ID_E_SAVE, L"\uE73E", L"Save", true);
        place(ID_E_CANCEL, nullptr, L"Cancel");
        return;
    }
    place(ID_CLOSE, L"\uE711", L"");
    place(ID_MENU, L"\uE713", L"Settings");
    place(ID_CLOCK, L"\uE917", L"Clock");
    place(ID_ADD, L"\uE710", L"Add videos");
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
        {ID_M_CLOCK, L"\uE917", L"Place the clock on the desktop"},
        {ID_M_ADD, L"\uE710", L"Add videos\u2026"},
        {ID_M_REMOVE, L"\uE74D", L"Take this video off the list"},
        {ID_M_NATIVE, L"\uE7F4", L"Switch to the classic look"},
    };
    const float h = 12 + 40 * (float)std::size(items) + 12 + 8;
    const D2D1_RECT_F box = R(x, y, x + w, y + h);
    if (G.shadow) {
        D2D1_SIZE_F s = G.shadow->GetSize();
        G.ctx->DrawBitmap(G.shadow, R(box.left - 40, box.top - 30, box.right + 40, box.bottom + 50), 0.6f * a,
                          D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, R(0, 0, s.width, s.height));
    }
    Fill(box, Col(0x17171A, 0.97f * a), 14);
    Stroke(box, Col(0xFFFFFF, 0.1f * a), 14, 1);
    float yy = box.top + 10;
    for (size_t n = 0; n < std::size(items); n++) {
        if (n == std::size(items) - 1) {  // the look switch, set apart
            G.br->SetColor(Col(0xFFFFFF, 0.08f * a));
            G.ctx->DrawLine(D2D1::Point2F(box.left + 14, yy + 6), D2D1::Point2F(box.right - 14, yy + 6), G.br, 1);
            yy += 12;
        }
        const D2D1_RECT_F r = R(box.left + 6, yy, box.right - 6, yy + 40);
        const bool disabled = items[n].id == ID_M_REMOVE && P.cards.empty();
        if (P.hot == items[n].id && !disabled) Fill(r, Col(0xFFFFFF, 0.08f * a), 9);
        const D2D1_COLOR_F fg = Col(0xFFFFFF, (disabled ? 0.35f : 0.92f) * a);
        Text(items[n].icon, R(r.left + 10, r.top, r.left + 34, r.bottom), G.fIcon, fg, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(items[n].label, R(r.left + 44, r.top, r.right - 10, r.bottom), G.fBody, fg);
        if (!disabled) P.hits.push_back({items[n].id, r});
        yy += 40;
    }
}

// Above the focused card: whether it's on the desktop, or the button that puts it there.
void DrawCardPill(float a) {
    if (P.cards.empty() || P.focus < 0 || a <= 0) return;
    const Pose p = CardPose(P.focus);
    const float settle = 1 - std::min(1.f, fabsf(P.scroll - P.focus) * 2.5f);
    a *= settle * p.a;
    if (a <= 0.01f) return;
    const float top = p.y - kCardH * p.s / 2 - 14 - 36;
    Card& c = P.cards[P.focus];
    if (P.confirmRemove) {
        const std::wstring q = L"Take \u201C" + c.title + L"\u201D off the list?";
        const float qw = std::min(TextWidth(q, G.fBody) + 28, P.W * 0.5f);
        const float bw = PillWidth(nullptr, L"Take off"), kw = PillWidth(nullptr, L"Keep");
        const float total = qw + bw + kw + 16;
        const float x = p.x - total / 2;
        const D2D1_RECT_F box = R(x, top - 6, x + total + 8, top + 42);
        Fill(box, Col(0x141417, 0.9f * a), 24);
        Text(q, R(x + 16, top, x + qw, top + 36), G.fBody, Col(0xFFFFFF, a));
        Pill(ID_RM_YES, R(x + qw, top + 2, x + qw + bw, top + 34), nullptr, L"Take off", a, true);
        Pill(ID_RM_NO, R(x + qw + bw + 6, top + 2, x + qw + bw + 6 + kw, top + 34), nullptr, L"Keep", a);
        return;
    }
    if (P.focus == P.applied) {
        const std::wstring t = L"On your desktop";
        const float w = PillWidth(L"\uE73E", t);
        Pill(0, R(p.x - w / 2, top, p.x + w / 2, top + 36), L"\uE73E", t, a);
    } else if (c.failed || (c.ready && !c.info.ok)) {
        const std::wstring t = c.failed ? L"This video can't play here" : L"Can't use this one";
        const float w = PillWidth(L"\uE7BA", t);
        Pill(0, R(p.x - w / 2, top, p.x + w / 2, top + 36), L"\uE7BA", t, a);
    } else {
        const std::wstring t = L"Set as wallpaper   \u23CE";
        const float w = PillWidth(L"\uE7C5", t);
        Pill(ID_SET, R(p.x - w / 2, top, p.x + w / 2, top + 36), L"\uE7C5", t, a, true);
    }
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
    if (P.mode != 1 || !P.clockBmp) return;
    const D2D1_RECT_F r = ClockRect();
    const D2D1_RECT_F box = R(r.left + 6, r.top + 6, r.right - 6, r.bottom - 6);
    G.br->SetColor(g_accent);
    if (P.clockDrag && P.snapX) G.ctx->DrawLine(D2D1::Point2F(P.W / 2, 0), D2D1::Point2F(P.W / 2, P.H), G.br, 1);
    if (P.clockDrag && P.snapY) G.ctx->DrawLine(D2D1::Point2F(0, P.H / 2), D2D1::Point2F(P.W, P.H / 2), G.br, 1);
    Stroke(box, Col(0xFFFFFF, (P.clockHot || P.clockDrag ? 0.9f : 0.5f) * a), 10, 1.5f, G.dash);
}

void Paint() {
    P.hits.clear();
    if (P.mode == 0)
        for (int i = 0; i < (int)P.cards.size(); i++) {
            if (fabsf(i - P.scroll) > 16) SafeRelease(P.cards[i].image);
            else if (!P.cards[i].image && CardVisible(CardPose(i))) MakeCardImage(i);
        }
    G.ctx->BeginDraw();
    G.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    G.ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    DrawStillLayer();
    DrawClock(P.phase == SELECTING ? 1 - Clamp01((float)(Now() - P.chosenAt) * 3) : 1);
    if (P.dark > 0) Fill(R(0, 0, P.W, P.H), Col(0x000000, P.dark));

    if (P.mode == 0) {
        // The deck, back to front.
        std::vector<std::pair<float, int>> order;
        for (int i = 0; i < (int)P.cards.size(); i++)
            if (i != P.chosen) order.push_back({fabsf(i - P.scroll), i});
        std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
        const float selT = P.phase == SELECTING ? (float)(Now() - P.chosenAt) : 0;
        for (auto& [dist, i] : order) {
            Pose p = CardPose(i);
            if (P.phase == SELECTING) {  // the others drop away
                const float t = EaseInOut(selT / 0.35f);
                p.y += t * kCardH * P.k * 1.6f;
                p.a *= 1 - t;
            }
            if (!CardVisible(p)) continue;
            DrawCard(i, p, CardMatrix(p));
        }
        if (P.phase == SELECTING && P.chosen >= 0) {
            // The chosen card: up to the middle of the screen, a pulse of light, then its picture
            // opens out to fill the screen.
            Card& c = P.cards[P.chosen];
            const float lift = EaseInOut(selT / 0.38f);
            const float bigS = std::min(P.H * 0.48f / kCardH, 1.9f * P.k);
            Pose p{Lerp(P.chosenFrom.x, P.W / 2, lift), Lerp(P.chosenFrom.y, P.H / 2, lift), Lerp(P.chosenRot, 0, lift),
                   Lerp(P.chosenScale, bigS, lift), 1, 0};
            const float open = EaseInOut((selT - 0.62f) / 0.42f);
            if (open < 1) {
                const float pulse = Clamp01((selT - 0.3f) / 0.4f);
                if (pulse > 0 && pulse < 1 && G.glow) {
                    G.ctx->SetTransform(CardMatrix(p));
                    const D2D1_SIZE_F s = G.glow->GetSize();
                    const float grow = 1 + 0.12f * EaseOut(pulse);
                    G.ctx->DrawBitmap(G.glow, R(-s.width / 2 * grow, -s.height / 2 * grow, s.width / 2 * grow, s.height / 2 * grow),
                                      (1 - pulse) * 0.9f);
                }
                DrawCard(P.chosen, p, CardMatrix(p), 1 - open, 1 - Clamp01(open * 3));
                if (pulse > 0 && pulse < 1) {  // a ring of light leaving the card
                    G.ctx->SetTransform(CardMatrix(p));
                    const float g = 3 + 16 * EaseOut(pulse);
                    Stroke(R(-kCardW / 2 - g, -kCardH / 2 - g, kCardW / 2 + g, kCardH / 2 + g), Col(0xFFFFFF, 0.9f * (1 - pulse)), kCardR + g,
                           1.6f);
                }
            }
            if (open > 0) {
                // From the card's picture box to the whole screen.
                const D2D1_RECT_F pic = R(-kCardW / 2 + kPad, -kCardH / 2 + kPad, kCardW / 2 - kPad, -kCardH / 2 + kPad + kThumbH);
                const D2D1_MATRIX_3X2_F m = CardMatrix(p);
                const D2D1_POINT_2F tl = Apply(m, D2D1::Point2F(pic.left, pic.top));
                const D2D1_POINT_2F br = Apply(m, D2D1::Point2F(pic.right, pic.bottom));
                ID2D1Bitmap* bm = c.frame ? c.frame : c.thumb;
                G.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
                if (bm) {
                    const D2D1_SIZE_F sz = bm->GetSize();
                    const double aspect = c.info.vw && c.info.vh ? (double)c.info.vw / c.info.vh : sz.width / sz.height;
                    FitRect f = ComputeFit(aspect, c.crop, CropMode(c.crop), P.W, P.H);
                    const D2D1_RECT_F to = R(f.dl, f.dt, f.dr, f.db);
                    const D2D1_RECT_F now = R(Lerp(tl.x, to.left, open), Lerp(tl.y, to.top, open), Lerp(br.x, to.right, open),
                                              Lerp(br.y, to.bottom, open));
                    ID2D1RoundedRectangleGeometry* geo = nullptr;
                    const float rad = 10 * p.s * (1 - open);
                    G.d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(now, rad, rad), &geo);
                    if (geo) {
                        G.ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo), nullptr);
                        Fill(now, Col(0x000000));
                        // The picture's full-screen view, moving and scaling with the box.
                        const float sx = (now.right - now.left) / (to.right - to.left), sy = (now.bottom - now.top) / (to.bottom - to.top);
                        G.ctx->SetTransform(D2D1::Matrix3x2F::Translation(-to.left, -to.top) * D2D1::Matrix3x2F::Scale(sx, sy) *
                                            D2D1::Matrix3x2F::Translation(now.left, now.top));
                        G.ctx->DrawBitmap(bm, to, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                                          R(f.sl * sz.width, f.st * sz.height, f.sr * sz.width, f.sb * sz.height));
                        G.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
                        G.ctx->PopLayer();
                        geo->Release();
                    }
                }
            }
        }
        G.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
        DrawCardPill(P.chosen < 0 ? P.chrome : 0);
        DrawEmpty(P.chrome);
    } else {
        DrawClockEdit(P.chrome);
    }
    DrawTopBar(P.chrome);
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
    if (i == P.applied) {  // already the wallpaper: nothing to change
        BeginClose(L"chose the wallpaper it has");
        return;
    }
    const Pose p = CardPose(i);
    P.chosen = i;
    P.chosenAt = Now();
    P.chosenFrom = D2D1::Point2F(p.x, p.y);
    P.chosenRot = p.rot;
    P.chosenScale = p.s;
    P.used = false;
    P.phase = SELECTING;
    P.menu = false;
    KillTimer(P.hwnd, TIMER_DWELL);
    AskPictures();
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
    std::vector<std::wstring> list;
    for (auto& c : P.cards) list.push_back(c.path);
    for (auto& p : picked) {
        if (CardIndex(p) >= 0) {
            if (first < 0) first = CardIndex(p);
            continue;
        }
        Card c;
        c.path = p;
        c.title = FileTitle(p);
        c.meta = FileMeta(p);
        c.crop = LoadCropFor(p);
        P.cards.push_back(c);
        list.push_back(p);
        if (first < 0) first = (int)P.cards.size() - 1;
        added++;
    }
    if (added) {
        SaveWallpapers(list);
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
    ReleaseCardBitmaps(P.cards[i]);
    P.cards.erase(P.cards.begin() + i);
    for (auto& s : P.stills) {
        if (s.card == i) s = Still{};
        else if (s.card > i) s.card--;
    }
    if (P.applied == i) P.applied = -1;
    else if (P.applied > i) P.applied--;
    std::vector<std::wstring> list;
    for (auto& c : P.cards) list.push_back(c.path);
    SaveWallpapers(list);
    P.confirmRemove = false;
    P.focus = -1;
    const int next = std::min(i, (int)P.cards.size() - 1);
    P.scroll = std::min(P.scroll, (float)std::max(0, (int)P.cards.size() - 1));
    if (next >= 0) SetFocus(next);
    P.dirty = true;
}

void StartClockEdit() {
    P.mode = 1;
    P.menu = false;
    P.confirmRemove = false;
    KillTimer(P.hwnd, TIMER_DWELL);
    // The wallpaper's own clock, over its own picture.
    AskPreview(g_settings.video, 0);
    P.look = g_settings.clockVideoOwn ? g_settings.clockVideo : g_settings.clockLive;
    if (!P.look.show) {
        P.look.show = true;
        ShowToast(L"The clock was hidden: saving shows it again");
    }
    P.clockKey.clear();
    UpdateClock();
    if (P.applied >= 0) {
        P.stills[0] = Still{};
        P.stills[1] = Still{P.applied, P.showcaseUp && _wcsicmp(P.live.c_str(), g_settings.video.c_str()) == 0 ? 0.f : 1.f};
    }
    P.dirty = true;
}

void SaveClockPlace() {
    ClockLook& target = g_settings.clockVideoOwn ? g_settings.clockVideo : g_settings.clockLive;
    target.x = P.look.x;
    target.y = P.look.y;
    target.show = true;
    Log(L"picker: clock placed at %.1f%%, %.1f%%", target.x * 100, target.y * 100);
    Host_SettingsChanged();
    UI_Refresh();
}

void Nudge(float dx, float dy) {
    P.look.x = std::clamp(P.look.x + dx / P.W, 0.f, 1.f);
    P.look.y = std::clamp(P.look.y + dy / P.H, 0.f, 1.f);
    P.dirty = true;
}

void Command(int id) {
    switch (id) {
        case ID_CLOSE: BeginClose(L"close button"); break;
        case ID_MENU: P.menu = !P.menu; break;
        case ID_ADD:
        case ID_EMPTY_ADD:
        case ID_M_ADD: P.menu = false; AddVideos(); break;
        case ID_CLOCK:
        case ID_M_CLOCK: StartClockEdit(); break;
        case ID_SET: Choose(P.focus); break;
        case ID_M_SETTINGS:
            P.menu = false;
            BeginClose(L"all settings");
            UI_Show();
            break;
        case ID_M_REMOVE:
            P.menu = false;
            P.confirmRemove = !P.cards.empty();
            break;
        case ID_M_NATIVE:
            BeginClose(L"classic look");
            Host_SetLook(0);
            UI_Show();
            break;
        case ID_RM_YES: RemoveFocused(); break;
        case ID_RM_NO: P.confirmRemove = false; break;
        case ID_E_SAVE:
            SaveClockPlace();
            BeginClose(L"clock placed");
            break;
        case ID_E_CANCEL: BeginClose(L"clock not moved"); break;
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
    switch (m) {
        case WM_MOUSEMOVE: {
            int hot = HitControl(x, y);
            const int card = P.mode == 0 && hot == ID_NONE && !P.down && !P.menu ? HitCard(x, y) : -1;
            if (card != P.hotCard) {
                P.hotCard = card;
                P.dirty = true;
            }
            if (P.mode == 1) {
                const bool over = In(ClockRect(), x, y);
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
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, P.hwnd, 0};
            TrackMouseEvent(&tme);
            break;
        }
        case WM_MOUSELEAVE:
            P.hot = ID_NONE;
            P.hotCard = -1;
            P.dirty = true;
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            SetCapture(P.hwnd);
            P.press = HitControl(x, y);
            if (P.menu && P.press == ID_NONE) {  // a click outside the menu closes it
                P.menu = false;
                P.dirty = true;
                break;
            }
            if (P.mode == 1) {
                if (P.press == ID_NONE && In(ClockRect(), x, y)) {
                    P.clockDrag = true;
                    P.grabX = x - P.look.x * P.W;
                    P.grabY = y - P.look.y * P.H;
                }
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
                if (m == WM_LBUTTONDBLCLK) {
                    const int card = HitCard(x, y);
                    if (card >= 0) {
                        P.down = false;
                        SetFocus(card);
                        Choose(card);
                    }
                }
            }
            P.dirty = true;
            break;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            if (P.clockDrag) {
                P.clockDrag = false;
                P.snapX = P.snapY = false;
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
                    if (card >= 0 && card == P.focus) Choose(card);
                    else if (card >= 0) SetFocus(card);
                }
            }
            P.dirty = true;
            break;
        }
    }
}

void OnWheel(int delta) {
    if ((P.phase != BROWSING && P.phase != OPENING) || P.mode != 0 || P.cards.empty()) return;
    P.wheel += delta;
    while (fabsf(P.wheel) >= 60) {  // a notch is 120; touchpads send smaller steps
        const int dir = P.wheel > 0 ? -1 : 1;
        P.wheel -= dir > 0 ? -120 : 120;
        if (fabsf(P.wheel) < 60) P.wheel = 0;
        SetFocus(P.focus + dir);
    }
}

void OnKey(WPARAM vk) {
    if (P.phase != BROWSING && P.phase != OPENING) return;  // choosing or closing: nearly done
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    if (P.mode == 1) {
        const float step = shift ? 10.f : 1.f;
        switch (vk) {
            case VK_LEFT: Nudge(-step, 0); break;
            case VK_RIGHT: Nudge(step, 0); break;
            case VK_UP: Nudge(0, -step); break;
            case VK_DOWN: Nudge(0, step); break;
            case VK_RETURN: Command(ID_E_SAVE); break;
            case VK_ESCAPE: BeginClose(L"Esc"); break;
        }
        return;
    }
    if (P.confirmRemove) {
        if (vk == VK_RETURN) Command(ID_RM_YES);
        else if (vk == VK_ESCAPE) Command(ID_RM_NO);
        return;
    }
    if (P.menu && vk == VK_ESCAPE) {
        P.menu = false;
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
        case VK_DELETE: P.confirmRemove = !P.cards.empty(); P.dirty = true; break;
        case VK_ESCAPE: BeginClose(L"Esc"); break;
        case 'C': StartClockEdit(); break;
    }
}

// ---------------------------------------------------------------------------------------
// The frame loop: everything that moves, then a picture

void Destroy();

// True while anything still moves.
bool Step(double now) {
    const float dt = (float)std::clamp(now - P.last, 0.0, 0.05);
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
    const std::wstring& wanted = P.mode == 1 || P.focus < 0 || P.focus >= (int)P.cards.size() ? g_settings.video : P.cards[P.focus].path;
    const bool focusLive = P.showcaseUp && _wcsicmp(wanted.c_str(), P.live.c_str()) == 0;
    switch (P.phase) {
        case OPENING:
            moving = true;
            P.enter = t;
            moving |= Approach(P.chrome, 1, 4, dt);
            if (P.showcaseUp || t > 1.5f) {  // without a showcase (no wallpaper yet), after a moment anyway
                P.darkTo = 0;
                if (t > 0.6f && P.dark <= 0) P.phase = BROWSING;
            }
            break;
        case BROWSING:
            P.enter = 1 + t;
            moving |= Approach(P.chrome, 1, 4, dt);
            P.darkTo = 0;
            break;
        case SELECTING:
            moving = true;
            if (t > 0.3f && P.applied != P.chosen) {
                if (P.applied >= 0) SafeRelease(P.cards[P.applied].image);
                SafeRelease(P.cards[P.chosen].image);  // drawn again, with the badge
                P.applied = P.chosen;  // its card shows "Current" now (it becomes the wallpaper a moment later)
            }
            if (t > 1.1f) {  // the chosen picture fills the screen: it becomes the wallpaper, back to the apps
                // (Saving and the rest take a moment: done now, under a still picture, not mid-animation.)
                P.used = true;
                Card& c = P.cards[P.chosen];
                Log(L"picker: %ls is the wallpaper now", c.path.c_str());
                Host_SetVideo(c.path);  // the renderer keeps playing it if it's the preview
                P.stills[0] = Still{};
                P.stills[1] = Still{P.chosen, 1};
                P.chosen = -1;
                P.phase = CLOSING;
                P.phaseAt = now;
                P.chrome = 0;
            }
            P.chrome = std::max(0.f, 1 - t * 4);
            break;
        case CLOSING:
            moving = true;
            P.enter = std::max(0.f, 1 - t / 0.28f);
            P.chrome = std::max(0.f, P.chrome - dt * 6);
            P.menuA = 0;
            if (t >= 0.26f && P.showcaseAsked) {  // dark (or the chosen picture) covers it: the apps come back under it
                P.showcaseAsked = false;
                Host_Showcase(nullptr);
            }
            if (t < 0.26f) {
                if (P.stills[1].a < 0.99f) P.darkTo = 1;
            } else {
                P.darkTo = 0;
                for (auto& s : P.stills) s.a = std::max(0.f, s.a - dt * 4.5f);
            }
            if (t > 0.3f && P.dark <= 0 && P.stills[0].a <= 0 && P.stills[1].a <= 0) {
                Destroy();
                return false;
            }
            break;
    }
    // Black over everything (opening and closing), and the focused card's picture over the live one.
    moving |= Approach(P.dark, P.darkTo, P.phase == CLOSING || P.darkTo > P.dark ? 6.f : 4.f, dt);
    if (P.phase == OPENING || P.phase == BROWSING) {
        Still& top = P.stills[1];
        const bool loaded = top.card >= 0 && top.card < (int)P.cards.size() && (P.cards[top.card].frame || P.cards[top.card].thumb);
        if (!focusLive) {
            if (loaded) moving |= Approach(top.a, 1, 7, dt);
        } else {
            moving |= Approach(top.a, 0, 3.5f, dt);  // the video is up: the picture fades into it
        }
        if (top.a >= 1 && P.stills[0].card >= 0) P.stills[0] = Still{};
    }
    moving |= Approach(P.menuA, P.menu ? 1.f : 0.f, 8, dt);
    P.lift.resize(P.cards.size(), 0.f);
    for (int i = 0; i < (int)P.lift.size(); i++) moving |= Approach(P.lift[i], i == P.hotCard ? 1.f : 0.f, 6, dt);
    moving |= Approach(P.toastA, P.toastOn ? 1.f : 0.f, 6, dt);
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
    HWND h = P.hwnd;
    P.hwnd = nullptr;
    MSG msg;
    while (PeekMessageW(&msg, h, WM_PK_INFO, WM_PK_INFO, PM_REMOVE)) delete (WallpaperInfo*)msg.lParam;
    ReleaseGraphics();
    P.cards.clear();
    P.lift.clear();
    DestroyWindow(h);
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
        case WM_RBUTTONUP:
            if (P.mode == 0 && P.phase == BROWSING) {
                P.menu = !P.menu;
                P.dirty = true;
            }
            return 0;
        case WM_MOUSEWHEEL: OnWheel(GET_WHEEL_DELTA_WPARAM(w)); return 0;
        case WM_MOUSEHWHEEL: OnWheel(-GET_WHEEL_DELTA_WPARAM(w)); return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (w == VK_F4 && (GetKeyState(VK_MENU) < 0)) { BeginClose(L"Alt+F4"); return 0; }
            OnKey(w);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                const bool hand = P.hot != ID_NONE || (P.mode == 1 && (P.clockHot || P.clockDrag));
                SetCursor(LoadCursorW(nullptr, P.mode == 1 && (P.clockHot || P.clockDrag) ? IDC_SIZEALL : hand ? IDC_HAND : IDC_ARROW));
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
