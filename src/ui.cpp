// Settings window. Custom-drawn with Direct2D/DirectWrite in a Windows 11 style (follows the
// system light/dark theme and accent colour). Everything here - D2D, DirectWrite, Media
// Foundation for previews - is loaded when the window opens and released when it closes.
#include "mfhelp.h"
#include "resource.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cwchar>
#include <cwctype>

namespace {

constexpr float kW = 820;  // client width in DIPs
float kH = 660;            // client height: taller when the screen has room (set in UI_Show)
constexpr UINT WM_UI_FRAME = WM_APP + 100, WM_UI_INFO = WM_APP + 101, WM_UI_OPT_PROGRESS = WM_APP + 103,
               WM_UI_OPT_DONE = WM_APP + 104, WM_UI_STILL = WM_APP + 106;
constexpr UINT_PTR TIMER_STATUS = 1;
constexpr UINT_PTR TIMER_SCROLL = 2;  // the font list gliding to where the wheel sent it

enum Id {
    ID_NONE, ID_POWER, ID_CHOOSE, ID_CROP, ID_LOCK, ID_TIMELINE, ID_SCALE, ID_SPEED, ID_VOLUME, ID_FPS,
    ID_MONITORS, ID_COVER, ID_BATTERY, ID_HOTKEY, ID_LAUNCH, ID_STARTUP_LINK, ID_STARTUP_REG,
    ID_C_AREA, ID_C_LOCK, ID_C_RESET, ID_C_CANCEL, ID_C_APPLY, ID_VERSIONS, ID_VER_SCRIM, ID_VER_CANCEL, ID_VER_USE,
    ID_SOUND, ID_MUSIC_PICK, ID_SHUFFLE, ID_KEEPSOUND, ID_OPEN_DATA, ID_OPEN_LOCAL, ID_FIX,
    ID_CLK_SHOW, ID_CLK_MODE, ID_CLK_SCOPE, ID_CLK_AREA, ID_CLK_CUSTOM, ID_CLK_EYEDROP, ID_CLK_SIZE, ID_CLK_HOURS,
    ID_CLK_OPACITY, ID_CLK_GLOW, ID_CLK_GLOWSIZE, ID_CLK_GLOWCOLOR, ID_CLK_FONT, ID_FNT_SCRIM, ID_FNT_DONE, ID_FNT_BAR, ID_FNT_OWN,
    ID_DLG_SCRIM, ID_DLG_SV, ID_DLG_HUE, ID_DLG_PREVIEW, ID_DLG_HEX, ID_DLG_COPY, ID_DLG_PICK, ID_DLG_SAME, ID_DLG_SAVE,
    ID_DLG_USE, ID_DLG_OK, ID_DLG_CANCEL,
    ID_NAV = 200,        // sidebar pages: ID_NAV + page
    ID_DLG_SLOT0 = 300,  // colour dialog, "My colours" boxes: ID_DLG_SLOT0 + i
    ID_VER_ROW0 = 400,   // video versions dialog: a version's row, and its delete button
    ID_VER_DEL0 = 450,
    ID_FNT_ROW0 = 500,   // font picker: a font's row, its "Get it" link, "Add font file" and "Remove" buttons
    ID_FNT_GET0 = 600,
    ID_FNT_ADD0 = 700,
    ID_FNT_DEL0 = 800,
};
enum NavPage { NAV_VIDEO, NAV_CLOCK, NAV_SOUND, NAV_PLAYBACK, NAV_POWER, NAV_GENERAL, NAV_COUNT };  // sidebar order
enum Kind { K_BUTTON, K_TOGGLE, K_SEG, K_SLIDER, K_AREA };
enum Handle { H_NONE, H_NW, H_NE, H_SW, H_SE, H_N, H_S, H_W, H_E, H_MOVE, H_NEW };

const int kFpsValues[] = {0, 60, 30, 24};

// Layout, in DIPs. Pages are cards; a card has a header (icon + title) and then form rows: a label
// column and a control column, every control centred on its row.
const float kTop = 80, kSideX = 16, kSideW = 176, kContentX = 208;
const float kPad = 20;       // inside a card, left and right
const float kCardHead = 52;  // card top to its first row
const float kDialogW = 600;  // every dialog over the page (colour, video versions, clock font) is this wide
const float kRowH = 40, kLabelW = 176;

struct Hit {
    int id;
    Kind kind;
    D2D1_RECT_F r;
    bool enabled;
    std::vector<float> segs;  // segment boundaries (x) for K_SEG
};

struct Theme {
    bool dark;
    D2D1_COLOR_F bg, card, stroke, text, text2, text3, ctrl, ctrlHover, ctrlPress, ctrlStroke, track, accent,
        accentHover, onAccent, good, warn;
};

// ---------------------------------------------------------------------------------------
// Worker: decodes preview frames, makes light copies and loads the still wallpaper off the UI thread.

struct Job {
    int kind = 0;  // 0 preview frame, 2 light copy, 3 GIF to video, 5 load the still wallpaper
    std::wstring path;
    double t = 0;  // preview frame (as Settings::previewTime: -1 = the default one)
    Crop crop;
    int sw = 1920, sh = 1080;
    int serial = 0;
    std::wstring dst;  // light copy / video made from a GIF
    bool reopen = false;  // read the file afresh (it may have changed or gone)
    UINT ow = 0, oh = 0;
};
struct FrameMsg {
    std::wstring path;
    double t;
    std::vector<uint32_t> px;
    int w, h;
    int serial;
};
struct InfoMsg {
    std::wstring path;
    VideoInfo vi;
    bool ok;
    VideoFault fault;  // why not, when !ok
};
struct StillMsg {  // the desktop's still wallpaper, for the clock page
    std::vector<uint32_t> px;
    int w = 0, h = 0, srcW = 0, srcH = 0;
    int style = 0;  // 0 fill, 1 fit, 2 stretch, 3 centre
    COLORREF bg = 0;
};

// Loads the picture Windows uses as the desktop wallpaper, scaled down for a preview.
void LoadStillWallpaper(StillMsg* m) {
    wchar_t v[16] = {};
    DWORD size = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle", RRF_RT_REG_SZ, nullptr, v, &size);
    int style = _wtoi(v);
    size = sizeof v;
    v[0] = 0;
    RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper", RRF_RT_REG_SZ, nullptr, v, &size);
    m->style = style == 6 ? 1 : style == 2 ? 2 : (style == 0 && _wtoi(v) == 0) ? 3 : 0;
    wchar_t bg[32] = {};
    size = sizeof bg;
    int r = 0, g = 0, b = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Colors", L"Background", RRF_RT_REG_SZ, nullptr, bg, &size) == ERROR_SUCCESS)
        swscanf(bg, L"%d %d %d", &r, &g, &b);
    m->bg = RGB(r, g, b);

    std::wstring path = OwnDesktopBackground();  // the user's, also while the video's frame stands in for it
    if (path.empty()) return;
    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&w, &h);
    if (SUCCEEDED(hr) && (w == 0 || h == 0)) hr = E_FAIL;
    UINT tw = w, th = h;
    if (SUCCEEDED(hr) && w > 1280) {
        tw = 1280;
        th = std::max(1u, h * 1280 / w);
    }
    if (SUCCEEDED(hr)) hr = wic->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr)) hr = scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(scaler, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                             WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        m->px.resize((size_t)tw * th);
        hr = conv->CopyPixels(nullptr, tw * 4, (UINT)(m->px.size() * 4), (BYTE*)m->px.data());
        if (SUCCEEDED(hr)) {
            m->w = (int)tw;
            m->h = (int)th;
            m->srcW = (int)w;
            m->srcH = (int)h;
        } else {
            m->px.clear();
        }
    }
    SafeRelease(conv);
    SafeRelease(scaler);
    SafeRelease(frame);
    SafeRelease(dec);
    SafeRelease(wic);
}

class Worker {
public:
    void Start(HWND notify) {
        notify_ = notify;
        InitializeCriticalSection(&cs_);
        ev_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread_ = CreateThread(nullptr, 0, Proc, this, 0, nullptr);
    }
    void Stop() {
        if (!thread_) return;
        cancel_ = 1;  // abort a light copy in progress
        EnterCriticalSection(&cs_);
        quit_ = true;
        LeaveCriticalSection(&cs_);
        SetEvent(ev_);
        WaitForSingleObject(thread_, 20000);  // a light copy stops at its next frame
        CloseHandle(thread_);
        CloseHandle(ev_);
        DeleteCriticalSection(&cs_);
        thread_ = nullptr;
    }
    void Post(const Job& j) {
        EnterCriticalSection(&cs_);
        if (j.kind == 2 || j.kind == 3) { opt_ = j; hasOpt_ = true; }
        else if (j.kind == 5) hasStill_ = true;
        else { job_ = j; hasJob_ = true; }  // only the latest preview request matters
        LeaveCriticalSection(&cs_);
        SetEvent(ev_);
    }

private:
    static DWORD WINAPI Proc(void* p) { ((Worker*)p)->Run(); return 0; }
    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        FrameGrabber grab;
        for (;;) {
            // Release the decoder (and its buffers) once scrubbing stops; reopening is quick.
            if (WaitForSingleObject(ev_, grab.IsOpen() ? 2000 : INFINITE) == WAIT_TIMEOUT) {
                grab.Close();
                continue;
            }
            for (;;) {
                Job j;
                EnterCriticalSection(&cs_);
                bool quit = quit_, have = false;
                if (!quit && hasOpt_) { j = opt_; hasOpt_ = false; have = true; }
                else if (!quit && hasJob_) { j = job_; hasJob_ = false; have = true; }
                else if (!quit && hasStill_) { j.kind = 5; hasStill_ = false; have = true; }
                LeaveCriticalSection(&cs_);
                if (quit) { grab.Close(); CoUninitialize(); return; }
                if (!have) break;
                if (j.kind == 5) {
                    auto* sm = new StillMsg;
                    LoadStillWallpaper(sm);
                    if (!PostMessageW(notify_, WM_UI_STILL, 0, (LPARAM)sm)) delete sm;
                    continue;
                }
                if (j.kind == 2 || j.kind == 3) {
                    grab.Close();  // don't hold the source open while re-encoding it
                    HRESULT hr = j.kind == 2 ? MakeLightCopy(j.path, j.dst, j.ow, j.oh, notify_, WM_UI_OPT_PROGRESS, &cancel_)
                                             : MakeVideoFromGif(j.path, j.dst, notify_, WM_UI_OPT_PROGRESS, &cancel_);
                    PostMessageW(notify_, WM_UI_OPT_DONE, (WPARAM)hr, 0);
                    continue;
                }
                if (j.reopen) grab.Close();
                if (_wcsicmp(grab.Path().c_str(), j.path.c_str()) != 0 || !grab.info.ok) {
                    bool ok = grab.Open(j.path);
                    auto* im = new InfoMsg{j.path, grab.info, ok, ok ? VF_NONE : DiagnoseVideo(j.path, grab.info)};
                    if (!PostMessageW(notify_, WM_UI_INFO, 0, (LPARAM)im)) delete im;
                    if (!ok) continue;
                }
                // The default frame depends on the length, known only now (a short GIF video ends before 1 s).
                double t = PreviewTimeFor(j.t, grab.info.duration);
                auto* fm = new FrameMsg{j.path, t, {}, 0, 0, j.serial};
                if (grab.Grab(t, fm->px, fm->w, fm->h)) ShrinkPixels(fm->px, fm->w, fm->h, 2048);
                if (!PostMessageW(notify_, WM_UI_FRAME, 0, (LPARAM)fm)) delete fm;
            }
        }
    }

    HWND notify_ = nullptr;
    HANDLE thread_ = nullptr, ev_ = nullptr;
    CRITICAL_SECTION cs_;
    Job job_, opt_;
    bool hasJob_ = false, hasOpt_ = false, hasStill_ = false, quit_ = false;
    volatile LONG cancel_ = 0;
};

// ---------------------------------------------------------------------------------------

struct UI {
    HWND hwnd = nullptr;
    float dpi = 96;
    HMODULE d2dDll = nullptr, dwDll = nullptr;
    ID2D1Factory* d2d = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1DeviceContext* dc = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    IDWriteFactory* dw = nullptr;
    IDWriteTextFormat *fTitle = nullptr, *fSubtitle = nullptr, *fHeader = nullptr, *fBody = nullptr, *fStrong = nullptr,
                      *fSmall = nullptr, *fIcon = nullptr, *fIconSmall = nullptr, *fWrap = nullptr;
    Theme th{};

    std::vector<Hit> hits;
    int hot = 0, hotSeg = -1, press = 0, drag = 0;
    int keySlider = 0;      // the slider clicked last: arrow keys move it a step at a time
    bool keyStepped = false;  // it moved by key and isn't saved yet (saved when the key goes up)
    bool tracking = false;

    Worker worker;
    int serial = 0;
    ID2D1Bitmap* frame = nullptr;
    std::vector<uint32_t> framePx;
    int fw = 0, fh = 0;
    std::wstring framePath;  // video the current frame belongs to
    bool frameBusy = false, frameFailed = false;
    VideoInfo info;
    std::wstring infoPath;
    bool infoOk = false;
    std::wstring gpu;
    int screenW = 1920, screenH = 1080;

    bool recording = false;
    std::wstring hotkeyMsg;
    std::vector<std::wstring> playlist;

    bool optBusy = false;
    int optPct = 0;
    std::wstring optMsg, optDst, optSrc;
    bool optGif = false;   // the job turns a GIF into a video (else it makes a light copy)
    std::wstring optLabel; // the light copy's name in Video versions ("720p")
    UINT optW = 0, optH = 0;
    std::wstring optFrom;  // the video in use when the GIF job started: it's replaced only if still in use
    std::wstring origFor, origPath;  // OriginalOf() cache: the original of the lighter version origFor
    VideoInfo origInfo;
    VideoFault fault = VF_NONE;  // why the current video can't be read
    FaultText faultText;

    int nav = NAV_VIDEO;

    // Desktop clock page
    int clockEdit = 0;  // look being edited: 0 video wallpaper, 1 still wallpaper
    ClockImage clockImg;              // the clock as drawn for the edited look (same code as the desktop)
    std::wstring clockKey;            // look + size + minute it was drawn for
    ID2D1Bitmap* clockBmp = nullptr;
    D2D1_RECT_F clockBox{};           // the preview (whole screen) on the page
    D2D1_POINT_2F clockGrab{};        // where in the clock the drag started
    bool overClock = false, snapX = false, snapY = false;
    float snapYv = 0;
    bool picking = false;             // eyedropper on: clicking the preview takes the wallpaper's colour there
    std::wstring pickColor;           // colour under the eyedropper ("" when it isn't over the wallpaper)
    D2D1_POINT_2F pickAt{};
    DWORD lastClockMove = 0;
    std::wstring clockNote;           // short feedback in the help line (pasted, copied)
    DWORD clockNoteAt = 0;
    std::vector<uint32_t> stillPx;    // the still wallpaper, scaled down
    int stillW = 0, stillH = 0, stillSrcW = 0, stillSrcH = 0, stillStyle = 0;  // style: 0 fill 1 fit 2 stretch 3 centre
    D2D1_COLOR_F stillBg = D2D1::ColorF(0, 0, 0);
    ID2D1Bitmap* still = nullptr;
    ID2D1Bitmap* logo = nullptr;      // the app icon in the header
    bool stillLoading = false;
    Crop optCrop;
    double optTime = -1;

    int page = 0;  // 0 main, 1 crop
    Crop edit;
    bool lockAspect = true;
    int handle = H_NONE;
    D2D1_POINT_2F dragStart{};
    Crop dragOrig;
    D2D1_RECT_F imgRc{};
    DWORD lastPreview = 0;
} u;

// ---------------------------------------------------------------------------------------
// Theme

D2D1_COLOR_F Rgb(UINT32 rgb, float a = 1) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}

D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

void LoadTheme() {
    DWORD light = 1, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    Theme& t = u.th;
    t.dark = light == 0;
    // Accent palette: Light2 for dark mode, Dark1 for light mode (what Windows 11 controls use).
    BYTE pal[32] = {};
    size = sizeof pal;
    D2D1_COLOR_F accent = t.dark ? Rgb(0x60CDFF) : Rgb(0x005FB8);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &size) == ERROR_SUCCESS && size >= 32) {
        const BYTE* c = pal + (t.dark ? 1 : 4) * 4;
        accent = D2D1::ColorF(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f);
    }
    if (t.dark) {
        t.bg = Rgb(0x202020); t.card = Rgb(0x2B2B2B); t.stroke = Rgb(0x1C1C1C);
        t.text = Rgb(0xFFFFFF); t.text2 = Rgb(0xCFCFCF); t.text3 = Rgb(0x9D9D9D);
        t.ctrl = Rgb(0x373737); t.ctrlHover = Rgb(0x3D3D3D); t.ctrlPress = Rgb(0x323232); t.ctrlStroke = Rgb(0x454545);
        t.track = Rgb(0x9A9A9A); t.onAccent = Rgb(0x000000); t.good = Rgb(0x6CCB5F); t.warn = Rgb(0xFCE100);
    } else {
        t.bg = Rgb(0xF3F3F3); t.card = Rgb(0xFFFFFF); t.stroke = Rgb(0xE5E5E5);
        t.text = Rgb(0x1B1B1B); t.text2 = Rgb(0x5C5C5C); t.text3 = Rgb(0x8A8A8A);
        t.ctrl = Rgb(0xFBFBFB); t.ctrlHover = Rgb(0xF5F5F5); t.ctrlPress = Rgb(0xEFEFEF); t.ctrlStroke = Rgb(0xD5D5D5);
        t.track = Rgb(0x8A8A8A); t.onAccent = Rgb(0xFFFFFF); t.good = Rgb(0x0F7B0F); t.warn = Rgb(0x9D5D00);
    }
    t.accent = accent;
    t.accentHover = Mix(accent, t.dark ? Rgb(0x000000) : Rgb(0xFFFFFF), 0.1f);
    if (u.hwnd) {
        BOOL dark = t.dark;
        DwmSetWindowAttribute(u.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    }
}

// ---------------------------------------------------------------------------------------
// Direct2D / DirectWrite resources

bool FontExists(const wchar_t* family) {
    IDWriteFontCollection* fc = nullptr;
    if (FAILED(u.dw->GetSystemFontCollection(&fc, FALSE))) return false;
    UINT32 idx;
    BOOL exists = FALSE;
    fc->FindFamilyName(family, &idx, &exists);
    fc->Release();
    return exists;
}

IDWriteTextFormat* MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight) {
    IDWriteTextFormat* f = nullptr;
    u.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (!f) return nullptr;
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* sign = nullptr;
    u.dw->CreateEllipsisTrimmingSign(f, &sign);
    f->SetTrimming(&trim, sign);
    SafeRelease(sign);
    return f;
}

bool InitFactories() {
    u.d2dDll = LoadLibraryExW(L"d2d1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    u.dwDll = LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using D2DCreate = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
    using DWCreate = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
    auto d2dCreate = u.d2dDll ? (D2DCreate)(void*)GetProcAddress(u.d2dDll, "D2D1CreateFactory") : nullptr;
    auto dwCreate = u.dwDll ? (DWCreate)(void*)GetProcAddress(u.dwDll, "DWriteCreateFactory") : nullptr;
    if (!d2dCreate || !dwCreate) return false;
    D2D1_FACTORY_OPTIONS opt{D2D1_DEBUG_LEVEL_NONE};
    if (FAILED(d2dCreate(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opt, (void**)&u.d2d))) return false;
    if (FAILED(dwCreate(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&u.dw))) return false;
    const wchar_t* text = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* display = FontExists(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* icons = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    u.fTitle = MakeFormat(display, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    u.fSubtitle = MakeFormat(display, 18, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    u.fHeader = MakeFormat(text, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    u.fBody = MakeFormat(text, 13, DWRITE_FONT_WEIGHT_NORMAL);
    u.fStrong = MakeFormat(text, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    u.fSmall = MakeFormat(text, 12, DWRITE_FONT_WEIGHT_NORMAL);
    u.fIcon = MakeFormat(icons, 16, DWRITE_FONT_WEIGHT_NORMAL);
    u.fIconSmall = MakeFormat(icons, 12, DWRITE_FONT_WEIGHT_NORMAL);
    u.fWrap = MakeFormat(text, 12, DWRITE_FONT_WEIGHT_NORMAL);
    if (u.fWrap) {
        u.fWrap->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        u.fWrap->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    return u.fTitle && u.fSubtitle && u.fHeader && u.fBody && u.fStrong && u.fSmall && u.fIcon && u.fIconSmall && u.fWrap;
}

void ReleaseFontSamples();

void ReleaseTarget() {
    ReleaseFontSamples();  // the font picker's, if it's open
    SafeRelease(u.frame);
    SafeRelease(u.clockBmp);
    SafeRelease(u.still);
    SafeRelease(u.logo);
    SafeRelease(u.br);
    SafeRelease(u.dc);
    SafeRelease(u.rt);
}

bool EnsureTarget() {
    if (u.rt) return true;
    RECT rc;
    GetClientRect(u.hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), u.dpi, u.dpi);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(
        u.hwnd, D2D1::SizeU((UINT32)rc.right, (UINT32)rc.bottom));
    if (FAILED(u.d2d->CreateHwndRenderTarget(&rp, &hp, &u.rt))) return false;
    u.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    u.rt->QueryInterface(__uuidof(ID2D1DeviceContext), (void**)&u.dc);
    D2D1_COLOR_F c = D2D1::ColorF(0, 0, 0);
    u.rt->CreateSolidColorBrush(&c, nullptr, &u.br);
    return u.br != nullptr;
}

void EnsureFrameBitmap() {
    if (u.frame || u.framePx.empty() || !u.rt) return;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    u.rt->CreateBitmap(D2D1::SizeU((UINT32)u.fw, (UINT32)u.fh), u.framePx.data(), (UINT32)u.fw * 4, &bp, &u.frame);
}

// The app icon's largest image, for the header (drawn smaller with a high-quality filter).
void EnsureLogoBitmap() {
    if (u.logo || !u.rt) return;
    const int n = 256;
    HICON ico = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, n, n, 0);
    ICONINFO ii{};
    if (!ico || !GetIconInfo(ico, &ii)) {
        if (ico) DestroyIcon(ico);
        return;
    }
    std::vector<uint32_t> px((size_t)n * n);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), n, -n, 1, 32, BI_RGB};
    HDC dc = GetDC(nullptr);
    bool ok = GetDIBits(dc, ii.hbmColor, 0, n, px.data(), &bi, DIB_RGB_COLORS) == n;
    ReleaseDC(nullptr, dc);
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    DestroyIcon(ico);
    if (!ok) return;
    for (uint32_t& p : px) {  // straight alpha -> premultiplied
        uint32_t a = p >> 24;
        p = a << 24 | ((p >> 16 & 255) * a / 255) << 16 | ((p >> 8 & 255) * a / 255) << 8 | (p & 255) * a / 255;
    }
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    u.rt->CreateBitmap(D2D1::SizeU(n, n), px.data(), n * 4, &bp, &u.logo);
}

// ---------------------------------------------------------------------------------------
// Drawing helpers (all coordinates in DIPs)

D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius = 0) {
    u.br->SetColor(&c);
    if (radius > 0) {
        D2D1_ROUNDED_RECT rr{r, radius, radius};
        u.rt->FillRoundedRectangle(&rr, u.br);
    } else {
        u.rt->FillRectangle(&r, u.br);
    }
}

void Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius, float width = 1) {
    u.br->SetColor(&c);
    D2D1_RECT_F in = R(r.left + width / 2, r.top + width / 2, r.right - width / 2, r.bottom - width / 2);
    D2D1_ROUNDED_RECT rr{in, radius, radius};
    u.rt->DrawRoundedRectangle(&rr, u.br, width, nullptr);
}

void Circle(float x, float y, float rad, D2D1_COLOR_F c) {
    u.br->SetColor(&c);
    D2D1_ELLIPSE e{D2D1::Point2F(x, y), rad, rad};
    u.rt->FillEllipse(&e, u.br);
}

void Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float w = 1) {
    u.br->SetColor(&c);
    u.rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), u.br, w, nullptr);
}

void Text(const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, D2D1_COLOR_F c,
          DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING) {
    f->SetTextAlignment(a);
    u.br->SetColor(&c);
    u.rt->DrawText(s.c_str(), (UINT32)s.size(), f, &r, u.br, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

float TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
    IDWriteTextLayout* l = nullptr;
    if (FAILED(u.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 2000, 100, &l))) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    l->Release();
    return m.widthIncludingTrailingWhitespace;
}

void AddHit(int id, Kind k, const D2D1_RECT_F& r, bool enabled = true, std::vector<float> segs = {}) {
    u.hits.push_back(Hit{id, k, r, enabled, std::move(segs)});
}

bool IsHot(int id) { return u.hot == id && (u.press == 0 || u.press == id); }

void Card(const D2D1_RECT_F& r, const wchar_t* icon, const std::wstring& title) {
    Fill(r, u.th.card, 8);
    Stroke(r, u.th.stroke, 8);
    Text(icon, R(r.left + kPad, r.top + 16, r.left + kPad + 20, r.top + 40), u.fIcon, u.th.accent);
    Text(title, R(r.left + kPad + 28, r.top + 16, r.right - kPad, r.top + 40), u.fHeader, u.th.text);
}

void Button(int id, const D2D1_RECT_F& r, const std::wstring& label, const wchar_t* icon = nullptr, bool accent = false,
            bool enabled = true) {
    bool hot = enabled && IsHot(id), pressed = enabled && u.press == id && u.hot == id;
    D2D1_COLOR_F bg, fg;
    if (accent) {
        bg = !enabled ? Mix(u.th.accent, u.th.card, 0.6f) : pressed ? Mix(u.th.accent, u.th.card, 0.2f) : hot ? u.th.accentHover : u.th.accent;
        fg = u.th.onAccent;
    } else {
        bg = pressed ? u.th.ctrlPress : hot ? u.th.ctrlHover : u.th.ctrl;
        fg = enabled ? u.th.text : u.th.text3;
    }
    Fill(r, bg, 4);
    if (!accent) Stroke(r, u.th.ctrlStroke, 4);
    float tw = (label.empty() ? 0 : TextWidth(label, u.fBody)) + (icon ? (label.empty() ? 16 : 22) : 0);
    float x = (r.left + r.right - tw) / 2;
    if (icon) {
        Text(icon, R(x, r.top, x + 16, r.bottom), u.fIconSmall, fg, DWRITE_TEXT_ALIGNMENT_CENTER);
        x += 22;
    }
    if (!label.empty()) Text(label, R(x, r.top, r.right, r.bottom), u.fBody, fg);
    AddHit(id, K_BUTTON, r, enabled);
}

void Toggle(int id, float x, float cy, bool on, bool enabled = true) {
    D2D1_RECT_F r = R(x, cy - 10, x + 40, cy + 10);
    bool hot = enabled && IsHot(id);
    if (!enabled) {
        Fill(r, u.th.ctrl, 10);
        Stroke(r, u.th.ctrlStroke, 10);
        Circle(on ? r.right - 10 : r.left + 10, cy, 6, u.th.ctrlStroke);
    } else if (on) {
        Fill(r, hot ? u.th.accentHover : u.th.accent, 10);
        Circle(r.right - 10, cy, hot ? 7 : 6, u.th.onAccent);
    } else {
        Fill(r, hot ? u.th.ctrlHover : u.th.ctrl, 10);
        Stroke(r, u.th.text2, 10);
        Circle(r.left + 10, cy, hot ? 7 : 6, u.th.text2);
    }
    AddHit(id, K_TOGGLE, R(r.left - 4, r.top - 6, r.right + 4, r.bottom + 6), enabled);
}

void Seg(int id, const D2D1_RECT_F& r, const std::vector<std::wstring>& items, int sel, bool enabled = true) {
    Fill(r, u.th.ctrl, 5);
    Stroke(r, u.th.ctrlStroke, 5);
    // Equal segments when every label fits, else each as wide as its label needs.
    const float inner = r.right - r.left - 4;
    std::vector<float> widths;
    float total = 0, widest = 0;
    for (auto& s : items) {
        widths.push_back(TextWidth(s, u.fBody) + 20);
        total += widths.back();
        widest = std::max(widest, widths.back());
    }
    const bool equal = widest * items.size() <= inner;
    float x = r.left + 2;
    std::vector<float> bounds{x};
    for (size_t i = 0; i < items.size(); i++) {
        float w = equal ? inner / items.size() : widths[i] * inner / total;
        D2D1_RECT_F s = R(x, r.top + 2, x + w, r.bottom - 2);
        bool isSel = (int)i == sel, hot = enabled && u.hot == id && u.hotSeg == (int)i && !isSel;
        if (isSel) Fill(s, enabled ? u.th.accent : Mix(u.th.accent, u.th.card, 0.6f), 4);
        else if (hot) Fill(s, u.th.ctrlHover, 4);
        Text(items[i], s, u.fBody, isSel ? u.th.onAccent : (enabled ? u.th.text : u.th.text3), DWRITE_TEXT_ALIGNMENT_CENTER);
        x += w;
        bounds.push_back(x);
    }
    AddHit(id, K_SEG, r, enabled, bounds);
}

void Slider(int id, const D2D1_RECT_F& r, float frac, bool enabled = true) {
    frac = std::clamp(frac, 0.f, 1.f);
    float cy = (r.top + r.bottom) / 2, x0 = r.left + 10, x1 = r.right - 10, x = x0 + (x1 - x0) * frac;
    Fill(R(x0, cy - 2, x1, cy + 2), u.th.track, 2);
    Fill(R(x0, cy - 2, x, cy + 2), enabled ? u.th.accent : u.th.text3, 2);
    bool hot = IsHot(id) || u.drag == id;
    Circle(x, cy, 10, u.th.ctrl);
    u.br->SetColor(&u.th.ctrlStroke);
    D2D1_ELLIPSE e{D2D1::Point2F(x, cy), 10, 10};
    u.rt->DrawEllipse(&e, u.br, 1, nullptr);
    Circle(x, cy, u.drag == id ? 5 : hot ? 7 : 6, enabled ? u.th.accent : u.th.text3);
    if (u.keySlider == id && enabled) {  // arrow keys move this one
        u.br->SetColor(&u.th.accent);
        D2D1_ELLIPSE ring{D2D1::Point2F(x, cy), 13, 13};
        u.rt->DrawEllipse(&ring, u.br, 1.5f, nullptr);
    }
    AddHit(id, K_SLIDER, r, enabled);
}

float SliderFrac(const Hit& h, float x) { return std::clamp((x - h.r.left - 10) / (h.r.right - h.r.left - 20), 0.f, 1.f); }

void Row(float x, float y, float w, const std::wstring& label, float h = kRowH, bool enabled = true) {
    Text(label, R(x, y, x + w, y + h), u.fBody, enabled ? u.th.text : u.th.text3);
}

// The control column of a form row in a card at x, w wide.
float CtrlX(float x) { return x + kPad + kLabelW; }
float CtrlW(float w) { return w - 2 * kPad - kLabelW; }
D2D1_RECT_F SegRect(float cx, float yy, float cw, float h = kRowH) { return R(cx, yy + (h - 32) / 2, cx + cw, yy + (h + 32) / 2); }
// A slider whose track starts at cx, with room for its value on the right.
D2D1_RECT_F SliderRect(float cx, float yy, float cw, float h = kRowH) {
    return R(cx - 10, yy + (h - 28) / 2, cx + cw - 46, yy + (h + 28) / 2);
}
void SliderValue(const std::wstring& v, float cx, float yy, float cw, bool enabled, float h = kRowH) {
    Text(v, R(cx + cw - 46, yy, cx + cw, yy + h), u.fBody, enabled ? u.th.text2 : u.th.text3, DWRITE_TEXT_ALIGNMENT_TRAILING);
}

ID2D1RoundedRectangleGeometry* PushRounded(const D2D1_RECT_F& r, float radius) {
    ID2D1RoundedRectangleGeometry* clip = nullptr;
    D2D1_ROUNDED_RECT rr{r, radius, radius};
    u.d2d->CreateRoundedRectangleGeometry(&rr, &clip);
    D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(D2D1::InfiniteRect(), clip);
    u.rt->PushLayer(&lp, nullptr);
    return clip;
}

void PopRounded(ID2D1RoundedRectangleGeometry* clip) {
    u.rt->PopLayer();
    SafeRelease(clip);
}

void FillGradient(const D2D1_RECT_F& r, float radius, D2D1_POINT_2F from, D2D1_POINT_2F to, const D2D1_GRADIENT_STOP* stops,
                  UINT n) {
    ID2D1GradientStopCollection* sc = nullptr;
    ID2D1LinearGradientBrush* gb = nullptr;
    if (SUCCEEDED(u.rt->CreateGradientStopCollection(stops, n, &sc)) &&
        SUCCEEDED(u.rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to), sc, &gb))) {
        D2D1_ROUNDED_RECT rr{r, radius, radius};
        u.rt->FillRoundedRectangle(&rr, gb);
    }
    SafeRelease(gb);
    SafeRelease(sc);
}

std::wstring Fmt(const wchar_t* f, ...) {
    wchar_t b[512];
    va_list ap;
    va_start(ap, f);
    vswprintf(b, 512, f, ap);
    va_end(ap);
    return b;
}

std::wstring Clock(double s) {
    if (s < 0) s = 0;
    int t = (int)(s + 0.5);
    return t >= 3600 ? Fmt(L"%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60) : Fmt(L"%d:%02d", t / 60, t % 60);
}

std::wstring FileName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

// ---------------------------------------------------------------------------------------
// Light copies: "<name> (<w>x<h>).mp4" in %LOCALAPPDATA%\VideoBG\Light copies. videos.ini links each
// copy to its original by content, so renaming or moving either keeps the link.

const std::wstring& LightCopyDir() {
    static const std::wstring dir = LocalDataDir() + L"\\Light copies";
    return dir;
}

bool IsLightCopy(const std::wstring& video) {
    const std::wstring& dir = LightCopyDir();
    return video.size() > dir.size() + 1 && _wcsnicmp(video.c_str(), dir.c_str(), dir.size()) == 0 &&
           (video[dir.size()] == L'\\' || video[dir.size()] == L'/');
}

std::wstring StemOf(const std::wstring& path) {
    std::wstring name = FileName(path);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    return name;
}

// The decoder keeps ~24 frames (NV12, 1.5 bytes a pixel) whatever the video: most of the memory a
// video takes (measured 21-28 at 720p..4K).
double FrameMemoryMb(UINT w, UINT h) { return 24 * 1.5 * w * ((h + 15) & ~15u) / 1048576.0; }

bool SamePath(const std::wstring& a, const std::wstring& b) { return !a.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0; }

ULONGLONG FileBytes(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

struct Version {
    std::wstring label;  // "Original", "720p"
    UINT w = 0, h = 0;   // the whole frame
    std::wstring path;   // "" = not made yet
    ULONGLONG bytes = 0;
    bool original = false;
};

// Light copies made from `original` (linked to it by content, or for older copies named after it).
std::vector<Version> LightCopiesOf(const std::wstring& original) {
    std::vector<Version> out;
    std::wstring stem = StemOf(original);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((LightCopyDir() + L"\\*(*x*).mp4").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name = fd.cFileName, path = LightCopyDir() + L"\\" + name;
        size_t open = name.find_last_of(L'(');
        UINT w = 0, h = 0;
        wchar_t close = 0;
        if (open == std::wstring::npos || swscanf(name.c_str() + open + 1, L"%ux%u%lc", &w, &h, &close) != 3 || close != L')')
            continue;
        if (fd.nFileSizeLow + fd.nFileSizeHigh == 0) continue;
        bool named = open >= 2 && _wcsnicmp(name.c_str(), stem.c_str(), open - 1) == 0 && stem.size() == open - 1;
        if (!IsLightCopyOf(path, original) && !(named && VideoProfileId(path) == VideoId(path))) continue;
        Version v;
        v.w = w;
        v.h = h;
        v.path = path;
        v.bytes = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        out.push_back(v);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return out;
}

// Makes a light copy of `original` at ow x oh (reusing one made before), then switches to it.
void StartLightCopy(const std::wstring& original, UINT ow, UINT oh, const std::wstring& label) {
    if (u.optBusy) return;  // one job at a time
    CreateDirectoryW(LightCopyDir().c_str(), nullptr);
    std::wstring dst;
    bool made = false;
    for (int n = 1; n < 100; n++) {
        dst = LightCopyDir() + L"\\" + StemOf(original) + (n > 1 ? Fmt(L" %d", n) : L"") + Fmt(L" (%ux%u).mp4", ow, oh);
        if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES) break;
        if ((made = IsLightCopyOf(dst, original))) break;  // another video's copy has this name: next one
    }
    Job j;
    j.kind = 2;
    j.path = original;
    j.dst = dst;
    j.ow = ow;
    j.oh = oh;
    u.optBusy = true;
    u.optGif = false;
    u.optPct = 0;
    u.optMsg.clear();
    u.optSrc = original;
    u.optDst = dst;
    u.optFrom = g_settings.video;
    u.optLabel = label;
    u.optW = ow;
    u.optH = oh;
    u.optCrop = g_settings.crop;
    u.optTime = g_settings.previewTime;
    Log(L"ui: %ls version of %ls (%ux%u)%ls", label.c_str(), original.c_str(), ow, oh, made ? L", made before" : L"");
    if (made) PostMessageW(u.hwnd, WM_UI_OPT_DONE, (WPARAM)S_OK, 0);
    else u.worker.Post(j);
}

// Videos made from GIFs: "<name>.mp4" in %LOCALAPPDATA%\VideoBG\Converted, linked to their GIF
// the way light copies are, so a GIF is converted only once.
const std::wstring& ConvertedDir() {
    static const std::wstring dir = LocalDataDir() + L"\\Converted";
    return dir;
}

bool IsFromGif(const std::wstring& video) {
    const std::wstring& dir = ConvertedDir();
    return video.size() > dir.size() + 1 && _wcsnicmp(video.c_str(), dir.c_str(), dir.size()) == 0 &&
           (video[dir.size()] == L'\\' || video[dir.size()] == L'/');
}

void StartGifConvert(const std::wstring& gif) {
    if (u.optBusy) return;  // one job at a time
    CreateDirectoryW(ConvertedDir().c_str(), nullptr);
    std::wstring dst;
    bool made = false;
    for (int n = 1; n < 100; n++) {
        dst = ConvertedDir() + L"\\" + StemOf(gif) + (n > 1 ? Fmt(L" (%d)", n) : L"") + L".mp4";
        if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES) break;
        if ((made = IsLightCopyOf(dst, gif))) break;
    }
    u.optBusy = true;
    u.optGif = true;
    u.optPct = 0;
    u.optMsg.clear();
    u.optSrc = gif;
    u.optDst = dst;
    u.optFrom = g_settings.video;
    u.nav = NAV_VIDEO;
    Log(L"ui: video from GIF %ls -> %ls%ls", gif.c_str(), dst.c_str(), made ? L" (made before)" : L"");
    if (made) {
        PostMessageW(u.hwnd, WM_UI_OPT_DONE, (WPARAM)S_OK, 0);
    } else {
        Job j;
        j.kind = 3;
        j.path = gif;
        j.dst = dst;
        u.worker.Post(j);
    }
    InvalidateRect(u.hwnd, nullptr, FALSE);
}

double VideoAspect() {
    if (u.infoOk && u.info.aspect > 0) return u.info.aspect;
    if (u.fw > 0 && u.fh > 0) return (double)u.fw / u.fh;
    return 16.0 / 9.0;
}

double PreviewTime() { return PreviewTimeFor(g_settings.previewTime, u.infoOk ? u.info.duration : 0); }

// What's wrong with the video, when something is: what the preview found, else why the wallpaper
// couldn't play it. buf holds the latter.
const FaultText* CurrentFault(FaultText* buf) {
    if (u.fault != VF_NONE) return &u.faultText;
    return Host_PlayFailed(buf) ? buf : nullptr;
}

void RequestFrame(bool reopen = false) {
    if (g_settings.video.empty()) return;
    Job j;
    j.reopen = reopen;
    j.path = g_settings.video;
    j.t = g_settings.previewTime;  // -1 = the default frame
    j.serial = ++u.serial;
    u.frameBusy = true;
    u.worker.Post(j);
}

// Draws the video frame into `box` the way the wallpaper shows it (crop + scaling mode).
void DrawComposed(const D2D1_RECT_F& box, const Crop& crop, int mode) {
    Fill(box, D2D1::ColorF(0, 0, 0), 6);
    EnsureFrameBitmap();
    if (u.frame) {
        float bw = box.right - box.left, bh = box.bottom - box.top;
        FitRect f = ComputeFit(VideoAspect(), crop, mode, bw, bh);
        D2D1_RECT_F src = R(f.sl * u.fw, f.st * u.fh, f.sr * u.fw, f.sb * u.fh);
        D2D1_RECT_F dst = R(box.left + f.dl, box.top + f.dt, box.left + f.dr, box.top + f.db);
        ID2D1RoundedRectangleGeometry* clip = nullptr;
        D2D1_ROUNDED_RECT rr{box, 6, 6};
        u.d2d->CreateRoundedRectangleGeometry(&rr, &clip);
        D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(D2D1::InfiniteRect(), clip);
        u.rt->PushLayer(&lp, nullptr);
        if (u.dc) u.dc->DrawBitmap(u.frame, &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src, nullptr);
        else u.rt->DrawBitmap(u.frame, &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
        u.rt->PopLayer();
        SafeRelease(clip);
    } else {
        std::wstring msg = g_settings.video.empty() ? L"No video chosen"
                         : u.frameFailed            ? L"No preview for this file"
                                                    : L"Loading the preview\u2026";
        Text(msg, box, u.fBody, D2D1::ColorF(0.75f, 0.75f, 0.75f), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

// ---------------------------------------------------------------------------------------
// Main window: header, sidebar with one page per topic, the page itself.

double ScreenAspectH() { return u.screenW > 0 ? (double)u.screenH / u.screenW : 9.0 / 16.0; }

std::wstring StatusText(D2D1_COLOR_F* dot) {
    *dot = u.th.text3;
    if (!Host_IsOn()) return Host_BatteryOff() ? L"Off while on battery" : L"Off";
    LPARAM mask = 0;
    RendererState st = Host_RendererState(&mask);
    if (st == RS_PAUSED) {
        *dot = u.th.warn;
        if (mask & PR_LOCKED) return L"Paused while the PC is locked";
        if (mask & PR_DISPLAY_OFF) return L"Paused while the screen is off";
        if (mask & PR_BATTERY) return L"Paused on battery";
        return L"Paused while an app covers the desktop";
    }
    if (st == RS_PLAYING) { *dot = u.th.good; return L"Playing"; }
    return L"Starting\u2026";
}

void Link(int id, const D2D1_RECT_F& r, const std::wstring& label) {
    bool hot = IsHot(id);
    Text(label, r, u.fSmall, hot ? u.th.accentHover : u.th.accent, DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (hot) {
        float w = TextWidth(label, u.fSmall);
        Line(r.right - w, r.bottom - 2, r.right, r.bottom - 2, u.th.accentHover);
    }
    AddHit(id, K_BUTTON, r);
}


struct NavItem {
    const wchar_t* icon;
    const wchar_t* label;
};
const NavItem kNav[NAV_COUNT] = {{L"\uE714", L"Video"},    {L"\uE121", L"Desktop clock"}, {L"\uE767", L"Sound"},
                                 {L"\uE768", L"Playback"}, {L"\uE83F", L"Power saving"},  {L"\uE713", L"General"}};

void PaintHeader() {
    const Theme& t = u.th;
    D2D1_RECT_F logo = R(24, 20, 60, 56);
    EnsureLogoBitmap();
    if (u.logo) {
        D2D1_RECT_F icon = R(21.5f, 17.5f, 62.5f, 58.5f);  // the icon's rounded square has a 6% margin
        if (u.dc) u.dc->DrawBitmap(u.logo, &icon, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        else u.rt->DrawBitmap(u.logo, &icon, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
    } else {
        Fill(logo, Mix(t.accent, Rgb(0x7C5CFF), 0.5f), 9);
        Text(L"\uE768", logo, u.fIcon, Rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    Text(L"VideoBG", R(72, 16, 400, 42), u.fTitle, t.text);
    Text(L"Video wallpaper and desktop clock  \u00B7  " + HotkeyToString(g_settings.hkMods, g_settings.hkVk) +
             L" turns the wallpaper on or off",
         R(72, 42, 640, 60), u.fSmall, t.text2);
    bool on = Host_IsOn();
    Text(on ? L"On" : L"Off", R(kW - 24 - 40 - 12 - 60, 22, kW - 24 - 40 - 12, 56), u.fStrong, t.text,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
    Toggle(ID_POWER, kW - 24 - 40, 39, on);
}

void PaintSidebar() {
    const Theme& t = u.th;
    float y = kTop;
    for (int i = 0; i < NAV_COUNT; i++) {
        D2D1_RECT_F r = R(kSideX, y, kSideX + kSideW, y + 36);
        bool sel = u.nav == i, hot = IsHot(ID_NAV + i);
        if (sel || hot) Fill(r, Mix(t.bg, t.text, sel ? 0.07f : 0.04f), 5);
        if (sel) Fill(R(r.left, r.top + 10, r.left + 3, r.bottom - 10), t.accent, 1.5f);
        Text(kNav[i].icon, R(r.left + 14, r.top, r.left + 34, r.bottom), u.fIcon, t.text);
        Text(kNav[i].label, R(r.left + 44, r.top, r.right - 8, r.bottom), sel ? u.fStrong : u.fBody, t.text);
        AddHit(ID_NAV + i, K_BUTTON, r);
        y += 40;
    }

    // Status card at the bottom of the sidebar.
    D2D1_RECT_F sc = R(kSideX, kH - 24 - 196, kSideX + kSideW, kH - 24);
    Fill(sc, t.card, 8);
    Stroke(sc, t.stroke, 8);
    D2D1_COLOR_F dot;
    std::wstring st = StatusText(&dot);
    float x = sc.left + 12, yy = sc.top + 12, right = sc.right - 10;
    Circle(x + 4, yy + 9, 4, dot);
    Text(st, R(x + 14, yy + 1, right, yy + 36), u.fWrap, t.text);
    yy += 40;
    // Memory: RAM of each part and their total, then the graphics memory (on the GPU it names).
    auto mb = [](SIZE_T b) { return b < 10 * 1048576 ? Fmt(L"%.1f MB", b / 1048576.0) : Fmt(L"%.0f MB", b / 1048576.0); };
    auto stat = [&](float ry, const wchar_t* label, const std::wstring& value, IDWriteTextFormat* f) {
        Text(label, R(x, ry, right, ry + 18), f, t.text2);
        Text(value, R(x, ry, right, ry + 18), f, t.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
    };
    SIZE_T wall = 0, gfx = 0, tray = Host_SelfMemory();
    bool running = Host_RendererMemory(&wall, &gfx);
    stat(yy, L"Wallpaper RAM", mb(wall), u.fSmall);
    stat(yy + 18, L"Tray app RAM", mb(tray), u.fSmall);
    Line(x, yy + 39, right, yy + 39, t.stroke);
    stat(yy + 42, L"Total RAM", mb(wall + tray), u.fSmall);
    stat(yy + 66, L"Graphics memory", running && !gfx ? std::wstring(L"\u2026") : mb(gfx), u.fSmall);
    Text(u.gpu, R(x, yy + 84, right, yy + 100), u.fSmall, t.text3);
    Text(L"VideoBG " APP_VERSION, R(x, sc.bottom - 28, right, sc.bottom - 10), u.fSmall, t.text3);
}

// Info rows under a preview: icon + message + optional link on the right.
void InfoRow(const D2D1_RECT_F& box, float ry, const wchar_t* icon, const std::wstring& msg, D2D1_COLOR_F color, int linkId = 0,
             const std::wstring& linkText = L"") {
    if (msg.empty()) return;
    float linkW = linkId ? TextWidth(linkText, u.fSmall) + 4 : 0;
    Text(icon, R(box.left, ry, box.left + 16, ry + 20), u.fIconSmall, color);
    Text(msg, R(box.left + 22, ry, box.right - linkW - 10, ry + 20), u.fSmall, color);
    if (linkId) Link(linkId, R(box.right - linkW, ry, box.right, ry + 20), linkText);
}

// The original of a lighter version, "" if `video` isn't one (or the original is gone). Cached,
// with the original's details: it reads a little of both files.
const std::wstring& OriginalOf(const std::wstring& video) {
    if (u.origFor != video) {
        u.origFor = video;
        u.origPath = IsLightCopy(video) ? LightCopyOriginalPath(video) : L"";
        u.origInfo = VideoInfo{};
        if (!u.origPath.empty()) ProbeVideo(u.origPath, u.origInfo);
    }
    return u.origPath;
}

// "720p": a version's height in lines of this screen, for the part of the video shown (the crop).
std::wstring VersionLabel(UINT h, UINT vw, UINT vh) {
    const Crop& c = g_settings.crop;
    double need = std::max(u.screenW / ((c.r - c.l) * vw), u.screenH / ((c.b - c.t) * vh));
    return Fmt(L"%up", (UINT)lround(u.screenH * h / (vh * need)));
}

// A button drawn on a picture (dark, see-through).
void OverlayButton(int id, const D2D1_RECT_F& r, const std::wstring& label, const wchar_t* icon) {
    bool hot = IsHot(id), pressed = u.press == id && hot;
    Fill(r, D2D1::ColorF(0, 0, 0, pressed ? 0.75f : hot ? 0.65f : 0.5f), 4);
    float tw = 22 + TextWidth(label, u.fBody), bx = (r.left + r.right - tw) / 2;
    Text(icon, R(bx, r.top, bx + 16, r.bottom), u.fIconSmall, Rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(label, R(bx + 22, r.top, r.right, r.bottom), u.fBody, Rgb(0xFFFFFF));
    AddHit(id, K_BUTTON, r);
}

void PaintVideoPage(float x, float w) {
    const Theme& t = u.th;
    const float y = kTop;
    // The preview as big as fits over what goes under it: the frame slider, the lock screen, the
    // file, a status line and the help line.
    const float below = 180;
    float bw = w - 2 * kPad, previewH = bw * (float)ScreenAspectH();
    const float maxH = kH - 24 - y - kCardHead - below;
    if (previewH > maxH) {
        bw = maxH / (float)ScreenAspectH();
        previewH = maxH;
    }
    float bx = x + (w - bw) / 2;
    D2D1_RECT_F box = R(bx, y + kCardHead, bx + bw, y + kCardHead + previewH);
    D2D1_RECT_F vc = R(x, y, x + w, box.bottom + below);
    Card(vc, L"\uE714", L"Video");
    Button(ID_CHOOSE, R(x + w - kPad - 148, y + 12, x + w - kPad, y + 44), L"Choose video", L"\uE8E5", true);
    FaultText faultBuf;
    const FaultText* fault = CurrentFault(&faultBuf);
    const std::wstring& video = g_settings.video;
    const bool infoCurrent = u.infoOk && _wcsicmp(u.infoPath.c_str(), video.c_str()) == 0;
    DrawComposed(box, g_settings.crop, g_settings.scale);
    if (!video.empty() && !fault) {
        float cw = 32 + TextWidth(L"Crop", u.fBody) + 16;
        OverlayButton(ID_CROP, R(box.right - 8 - cw, box.top + 8, box.right - 8, box.top + 38), L"Crop", L"\uE7A8");
    }

    // The frame: for the preview, the lock screen and the desktop background.
    float ty = box.bottom + 8;
    double dur = u.infoOk ? u.info.duration : 0;
    Text(L"Preview", R(box.left, ty, box.left + 52, ty + 26), u.fSmall, t.text2);
    Slider(ID_TIMELINE, R(box.left + 46, ty, box.right - 84, ty + 26), dur > 0 ? (float)(PreviewTime() / dur) : 0, dur > 0);
    // Short clips (a GIF made into a video) in tenths of a second.
    std::wstring pos = dur <= 0 ? L"" : dur < 10 ? Fmt(L"%.1f / %.1f s", PreviewTime(), dur) : Clock(PreviewTime()) + L" / " + Clock(dur);
    Text(pos, R(box.right - 84, ty, box.right, ty + 26), u.fSmall, t.text2, DWRITE_TEXT_ALIGNMENT_TRAILING);

    // Lock screen and desktop background: what they show, and the switch for it.
    const float lockY = ty + 30;
    const std::wstring lockLabel = L"Show on lock screen";
    const float lockW = TextWidth(lockLabel, u.fBody);
    Toggle(ID_LOCK, box.right - 40, lockY + 10, g_settings.lockFollow);
    Text(lockLabel, R(box.right - 52 - lockW, lockY, box.right - 50, lockY + 20), u.fBody, t.text);
    const D2D1_RECT_F lockBox = R(box.left, 0, box.right - 64 - lockW, 0);
    HRESULT lockHr = S_OK;
    int lsf = 0;
    bool lockBusy = Host_LockScreen(&lockHr, &lsf);
    if (!g_settings.lockFollow)
        InfoRow(lockBox, lockY, L"\uE8B9", L"The lock screen and background aren't changed", t.text3);
    else if (fault && !lockBusy)
        InfoRow(lockBox, lockY, L"\uE8B9", L"Your own lock screen and background are showing", t.text2);
    else if (lockBusy)
        InfoRow(lockBox, lockY, L"\uE8B9", L"Updating the lock screen and background\u2026", t.text2);
    else if (FAILED(lockHr))
        InfoRow(lockBox, lockY, L"\uE8B9", Fmt(L"Couldn't update the lock screen (error 0x%08lX)", (unsigned long)lockHr), t.warn);
    else if (lsf & LSF_BACKGROUND)
        InfoRow(lockBox, lockY, L"\uE8B9", L"The lock screen and background show this frame", t.text2);
    else if (lsf & LSF_LOCKSCREEN)
        InfoRow(lockBox, lockY, L"\uE8B9", L"The lock screen shows this frame", t.text2);
    else
        InfoRow(lockBox, lockY, L"\uE8B9", L"They show this frame while the wallpaper is on", t.text2);

    // The video: always the original's name and details; a lighter version playing in its place
    // shows on the right, with the way to the versions.
    const float ny = lockY + 32, metaY = ny + 20;
    const std::wstring& orig = OriginalOf(video);
    const bool hasOrig = !orig.empty() && u.origInfo.ok;
    const VideoInfo& vi = hasOrig ? u.origInfo : u.info;
    Text(video.empty() ? L"No video chosen yet" : FileName(orig.empty() ? video : orig), R(box.left, ny, box.right, ny + 20),
         u.fStrong, t.text);
    std::wstring meta, note;
    D2D1_COLOR_F metaColor = t.text2, noteColor = t.good;
    UINT ow = 0, oh = 0;
    if (fault) {
        meta = fault->what;
        metaColor = t.warn;
    } else if (infoCurrent) {
        meta = Fmt(L"%u \u00D7 %u  \u00B7  %.0f fps  \u00B7  %ls  \u00B7  %ls", vi.w, vi.h, vi.fps, vi.codec.c_str(),
                   (vi.duration < 10 ? Fmt(L"%.1f s", vi.duration) : Clock(vi.duration)).c_str());
        meta += vi.audioStreams.empty() ? L"  \u00B7  no sound" : L"  \u00B7  has sound";
        if (IsFromGif(video)) {
            note = L"Made from your GIF";
        } else if (IsLightCopy(video)) {
            note = Fmt(L"Current version: %ls (%u\u00D7%u)", hasOrig ? VersionLabel(u.info.h, vi.w, vi.h).c_str() : L"smaller copy",
                       u.info.w, u.info.h);
        } else if (LightCopySize(u.info.w, u.info.h, g_settings.crop, u.screenW, u.screenH, &ow, &oh)) {
            note = Fmt(L"A lighter version saves ~%.0f MB",
                       std::round((FrameMemoryMb(u.info.w, u.info.h) - FrameMemoryMb(ow, oh)) / 10) * 10);
            noteColor = t.warn;
        }
    } else if (!video.empty()) {
        meta = u.frameFailed ? L"Couldn't read this file" : L"Reading the file\u2026";
    } else {
        meta = L"Choose a video (MP4, MOV, MKV, WebM and more) or an animated GIF";
    }
    float linkW = 0, noteW = note.empty() ? 0 : TextWidth(note, u.fSmall) + 16;
    if (infoCurrent && !fault) {
        linkW = TextWidth(L"Versions", u.fSmall) + 4;
        Link(ID_VERSIONS, R(box.right - linkW, metaY, box.right, metaY + 18), L"Versions");
        linkW += 16;
    }
    if (!note.empty())
        Text(note, R(box.right - linkW - noteW, metaY, box.right - linkW, metaY + 18), u.fSmall, noteColor,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    Text(meta, R(box.left, metaY, box.right - linkW - noteW, metaY + 18), u.fSmall, metaColor);

    // Status: a version or GIF being made, or what to do about a video that can't play.
    const float statusY = metaY + 24;
    if (u.optBusy) {
        InfoRow(box, statusY, L"\uE895",
                u.optGif ? Fmt(L"Turning the GIF into a video\u2026 %d%%", u.optPct)
                         : Fmt(L"Making the %ls version (%u\u00D7%u)\u2026 %d%%", u.optLabel.c_str(), u.optW, u.optH, u.optPct),
                t.text2);
    } else if (!u.optMsg.empty()) {
        InfoRow(box, statusY, L"\uE7BA", u.optMsg, t.warn);
    } else if (fault) {
        InfoRow(box, statusY, L"\uE82F", fault->fix, t.text2, fault->link.empty() ? 0 : ID_FIX, fault->linkText);
    }

    // Help line: what the control under the mouse does.
    std::wstring help;
    switch (u.drag ? u.drag : u.hot) {
        case ID_TIMELINE: help = L"Picks the frame used for the preview and the lock screen"; break;
        case ID_LOCK: help = L"Uses this frame as the lock screen and admin prompt background"; break;
        case ID_CROP: help = L"Choose which part of the video fills the screen"; break;
        case ID_CHOOSE: help = L"Pick a video or an animated GIF for your wallpaper"; break;
        case ID_VERSIONS: help = L"Smaller copies use less memory, and you can try them on your desktop first"; break;
        case ID_FIX:
            help = !fault                                     ? L""
                 : fault->link == L"gif:"                     ? L"Makes an MP4 copy of the GIF; the GIF itself isn't changed"
                 : fault->link.rfind(L"store:", 0) == 0       ? L"Opens Microsoft Store; VideoBG checks the video again when you come back"
                 : fault->link.rfind(L"ms-settings:", 0) == 0 ? L"Opens Windows Settings"
                                                                   : L"Opens the HandBrake website (a free video converter)";
            break;
    }
    const float helpY = vc.bottom - kPad - 16;
    InfoRow(box, helpY, L"\uE946", help.empty() ? std::wstring(L"Hover over a control to see what it does") : help,
            help.empty() ? t.text3 : t.text2);
}

void PaintSoundPage(float x, float w) {
    const Theme& t = u.th;
    const float y = kTop, lx = x + kPad, cx = CtrlX(x), cw = CtrlW(w);
    const int mode = g_settings.sound;
    Card(R(x, y, x + w, y + kCardHead + 5 * kRowH + 10), L"\uE767", L"Sound");
    float yy = y + kCardHead;
    Row(lx, yy, kLabelW, L"Play");
    Seg(ID_SOUND, SegRect(cx, yy, cw), {L"Nothing", L"Video's audio", L"My music"}, mode);
    yy += kRowH;
    Row(lx, yy, kLabelW, L"Volume", kRowH, mode != 0);
    Slider(ID_VOLUME, SliderRect(cx, yy, cw), g_settings.volume / 100.f, mode != 0);
    SliderValue(g_settings.volume ? Fmt(L"%d%%", g_settings.volume) : std::wstring(L"Muted"), cx, yy, cw, mode != 0);
    yy += kRowH;
    if (mode == 2) {
        Row(lx, yy, kLabelW, L"Songs");
        Button(ID_MUSIC_PICK, SegRect(cx, yy, 140), L"Choose songs", L"\uE8D6");
        std::wstring sum;
        D2D1_COLOR_F sc = t.text2;
        int tr = Host_CurrentTrack();
        if (u.playlist.empty()) {
            sum = L"No songs chosen yet";
            sc = t.warn;
        } else if (tr == -2) {
            sum = L"Windows couldn't play these songs";
            sc = t.warn;
        } else if (tr >= 0 && tr < (int)u.playlist.size() && Host_IsOn()) {
            sum = L"\u266A  " + FileName(u.playlist[tr]);
            sc = t.text;
        } else {
            sum = Fmt(L"%zu song%ls, played while the wallpaper is on", u.playlist.size(), u.playlist.size() == 1 ? L"" : L"s");
        }
        Text(sum, R(cx + 152, yy, cx + cw, yy + kRowH), u.fSmall, sc);
    } else {
        bool noAudio = mode == 1 && u.infoOk && u.info.audioStreams.empty();
        std::wstring info = mode == 0 ? L"Silent: the video's audio isn't even decoded, which saves power"
                          : noAudio   ? L"This video has no sound; try My music instead"
                                      : L"Plays the video's own soundtrack, in sync with the picture";
        Text(L"\uE946", R(lx, yy, lx + 16, yy + kRowH), u.fIconSmall, noAudio ? t.warn : t.text3);
        Text(info, R(lx + 22, yy, x + w - kPad, yy + kRowH), u.fSmall, noAudio ? t.warn : t.text2);
    }
    yy += kRowH;
    Row(lx, yy, kLabelW, L"Shuffle", kRowH, mode == 2);
    Toggle(ID_SHUFFLE, cx, yy + kRowH / 2, g_settings.shuffle, mode == 2);
    Text(L"Play your songs in random order", R(cx + 52, yy, cx + cw, yy + kRowH), u.fSmall, mode == 2 ? t.text2 : t.text3);
    yy += kRowH;
    Row(lx, yy, kLabelW, L"While paused", kRowH, mode != 0);
    Toggle(ID_KEEPSOUND, cx, yy + kRowH / 2, g_settings.keepSound, mode != 0);
    Text(L"Keep the sound playing when the picture pauses", R(cx + 52, yy, cx + cw, yy + kRowH), u.fSmall,
         mode != 0 ? t.text2 : t.text3);
}

void PaintPlaybackPage(float x, float w) {
    const float y = kTop, lx = x + kPad, cx = CtrlX(x), cw = CtrlW(w);
    Card(R(x, y, x + w, y + kCardHead + 4 * kRowH + 10), L"\uE768", L"Playback");
    float yy = y + kCardHead;
    Row(lx, yy, kLabelW, L"Scaling");
    Seg(ID_SCALE, SegRect(cx, yy, cw), {L"Fill", L"Fit", L"Stretch"}, g_settings.scale);
    yy += kRowH;
    Row(lx, yy, kLabelW, L"Speed");
    Slider(ID_SPEED, SliderRect(cx, yy, cw), (g_settings.speed - 25) / 175.f);
    SliderValue(Fmt(L"%.2f\u00D7", g_settings.speed / 100.0), cx, yy, cw, true);
    yy += kRowH;
    Row(lx, yy, kLabelW, L"Frame rate");
    int fpsSel = 0;
    for (int i = 0; i < 4; i++)
        if (kFpsValues[i] == g_settings.fpsCap) fpsSel = i;
    Seg(ID_FPS, SegRect(cx, yy, cw), {L"Original", L"60 fps", L"30 fps", L"24 fps"}, fpsSel);
    yy += kRowH;
    Row(lx, yy, kLabelW, L"Displays");
    Seg(ID_MONITORS, SegRect(cx, yy, cw), {L"All", L"Main only"}, g_settings.monitors);
}

void PaintPowerPage(float x, float w) {
    const Theme& t = u.th;
    const float y = kTop, lx = x + kPad, cx = CtrlX(x), cw = CtrlW(w);
    Card(R(x, y, x + w, y + kCardHead + 44 + 2 * kRowH + 10), L"\uE83F", L"Power saving");
    Text(L"While paused, the video freezes on its current frame and stops decoding, so it uses no CPU or GPU, and after a "
         L"while it frees its memory too. It always pauses while the PC is locked or the screen is off.",
         R(lx, y + kCardHead - 4, x + w - kPad, y + kCardHead + 36), u.fWrap, t.text2);
    float yy = y + kCardHead + 44;
    Row(lx, yy, kLabelW, L"Pause when covered by");
    Seg(ID_COVER, SegRect(cx, yy, cw), {L"Nothing", L"Fullscreen apps", L"Maximized apps"}, g_settings.pauseCover);
    yy += kRowH;
    Row(lx, yy, kLabelW, L"On battery");
    Seg(ID_BATTERY, SegRect(cx, yy, cw), {L"Keep playing", L"Pause", L"Turn off"}, g_settings.onBattery);
}

void PaintGeneralPage(float x, float w) {
    const Theme& t = u.th;
    const float y = kTop, lx = x + kPad, cx = CtrlX(x), cw = CtrlW(w);
    const float msgH = u.hotkeyMsg.empty() ? 0 : 18;
    D2D1_RECT_F gc = R(x, y, x + w, y + kCardHead + 3 * kRowH + msgH + 10);
    Card(gc, L"\uE713", L"General");
    float yy = y + kCardHead;
    Row(lx, yy, kLabelW, L"Toggle hotkey");
    D2D1_RECT_F hk = SegRect(cx, yy, cw);
    Fill(hk, u.recording ? t.card : (IsHot(ID_HOTKEY) ? t.ctrlHover : t.ctrl), 4);
    Stroke(hk, u.recording ? t.accent : t.ctrlStroke, 4, u.recording ? 2 : 1);
    std::wstring hkText = u.recording ? L"Press the new shortcut (Esc cancels)"
                                      : HotkeyToString(g_settings.hkMods, g_settings.hkVk);
    Text(hkText, hk, u.recording ? u.fBody : u.fStrong, u.recording ? t.text2 : t.text, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!u.recording) Text(L"\uE765", R(hk.left + 12, hk.top, hk.left + 28, hk.bottom), u.fIconSmall, t.text3);
    AddHit(ID_HOTKEY, K_BUTTON, hk);
    yy += kRowH;
    if (msgH) {
        Text(u.hotkeyMsg, R(cx, yy - 6, cx + cw, yy + 12), u.fSmall, t.warn);
        yy += msgH;
    }
    Row(lx, yy, kLabelW, L"When app starts");
    Seg(ID_LAUNCH, SegRect(cx, yy, cw), {L"Turn on", L"As I left it", L"Stay off"}, g_settings.launch);
    yy += kRowH;
    Row(lx, yy, kLabelW, L"Start with Windows");
    bool enabled = false;
    bool registered = Host_StartupRegistered(&enabled);
    if (registered) {
        Text(enabled ? L"On" : L"Off", R(cx, yy, cx + 60, yy + kRowH), u.fStrong, enabled ? t.good : t.text2);
        bool hot = IsHot(ID_STARTUP_LINK);
        D2D1_RECT_F lr = R(cx + 60, yy + 8, cx + cw, yy + kRowH - 8);
        Text(L"Manage in Windows Settings", R(lr.left, lr.top, lr.right - 20, lr.bottom), u.fBody, hot ? t.accentHover : t.accent,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
        Text(L"\uE8A7", R(lr.right - 14, lr.top, lr.right, lr.bottom), u.fIconSmall, hot ? t.accentHover : t.accent);
        AddHit(ID_STARTUP_LINK, K_BUTTON, lr);
    } else {
        Button(ID_STARTUP_REG, SegRect(cx, yy, cw), L"Add to Windows startup");
    }

    // Where things are kept.
    float fy = gc.bottom + 12;
    const float itemH = 46;
    Card(R(x, fy, x + w, fy + kCardHead + 2 * itemH + 34), L"\uE8B7", L"Your data");
    auto folder = [&](float iy, int id, const wchar_t* what, const wchar_t* where) {
        Text(what, R(lx, iy, x + w - kPad - 120, iy + 20), u.fBody, t.text);
        Text(where, R(lx, iy + 20, x + w - kPad - 120, iy + 38), u.fSmall, t.text3);
        Link(id, R(x + w - kPad - 120, iy, x + w - kPad, iy + 20), L"Open folder");
    };
    float iy = fy + kCardHead - 4;
    folder(iy, ID_OPEN_DATA, L"Settings, each video's crop and clock look, saved colours and the log", L"%APPDATA%\\VideoBG");
    folder(iy + itemH, ID_OPEN_LOCAL, L"Lighter versions of your videos, videos made from GIFs and fonts you added", L"%LOCALAPPDATA%\\VideoBG");
    Text(L"Your original videos stay where they are", R(lx, iy + 2 * itemH, x + w - kPad, iy + 2 * itemH + 20), u.fSmall, t.text3);
}

// ---- Desktop clock page --------------------------------------------------------------------

ClockLook* EditedLook() {
    if (u.clockEdit == 1) return &g_settings.clockStill;
    return g_settings.clockVideoOwn ? &g_settings.clockVideo : &g_settings.clockLive;
}

bool EditingShownLook() { return (u.clockEdit == 0) == Host_ClockLive(); }

bool ClockUsable() { return EditedLook()->show; }

// The colour dialog (More, or Glow colour), drawn over the page. The colour shows live on the page
// and, when the look is the one on the desktop, on the desktop too; Cancel puts the old one back.
struct ColorDialog {
    bool open = false;
    int target = 0;              // 0 the clock's colour, 1 its glow colour
    float h = 0, s = 0, v = 1;   // the colour being edited (the hue stays put while it's grey)
    std::wstring exact;          // "r,g,b" when set from a code, box or eyedropper (HSV would round it)
    std::wstring before;         // the look's value when the dialog opened
    bool sameAsText = false;     // glow: follow the text colour
    int slot = -1;               // chosen box in My colours (kept between openings)
    bool hexEdit = false, hexFresh = false;  // typing a hex code (fresh: the next key replaces it)
    std::wstring hex;
    bool picking = false;        // eyedropper over the dialog's preview
    std::wstring msg;            // short note in the bottom bar: copied, pasted, saved
    DWORD msgAt = 0;
    bool msgWarn = false;
    D2D1_RECT_F sv{}, hue{}, preview{}, previewScreen{};  // previewScreen: the whole screen, zoomed onto the clock
} dlg;

void HsvToRgb(float h, float s, float v, BYTE* r, BYTE* g, BYTE* b) {
    float c = v * s, hh = fmodf(h, 360.f) / 60.f, x = c * (1 - fabsf(fmodf(hh, 2.f) - 1)), m = v - c;
    float rr = 0, gg = 0, bb = 0;
    switch ((int)hh) {
        case 0: rr = c; gg = x; break;
        case 1: rr = x; gg = c; break;
        case 2: gg = c; bb = x; break;
        case 3: gg = x; bb = c; break;
        case 4: rr = x; bb = c; break;
        default: rr = c; bb = x; break;
    }
    *r = (BYTE)lroundf((rr + m) * 255);
    *g = (BYTE)lroundf((gg + m) * 255);
    *b = (BYTE)lroundf((bb + m) * 255);
}

void RgbToHsv(BYTE r, BYTE g, BYTE b, float* h, float* s, float* v) {
    float rf = r / 255.f, gf = g / 255.f, bf = b / 255.f;
    float mx = std::max({rf, gf, bf}), mn = std::min({rf, gf, bf}), d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    if (d > 0) {  // grey has no hue: keep the one there is
        float hh = (mx == rf ? fmodf((gf - bf) / d, 6.f) : mx == gf ? (bf - rf) / d + 2 : (rf - gf) / d + 4) * 60;
        *h = hh < 0 ? hh + 360 : hh;
    }
}

std::wstring DlgColor() {
    if (!dlg.exact.empty()) return dlg.exact;
    BYTE r, g, b;
    HsvToRgb(dlg.h, dlg.s, dlg.v, &r, &g, &b);
    return Fmt(L"%d,%d,%d", r, g, b);
}

// The look's value the dialog edits.
std::wstring& DlgTarget() { return dlg.target == 0 ? EditedLook()->color : EditedLook()->glowColor; }

// The clock as it will look on the desktop (same drawing code, real pixel size); the previews
// scale it down. Redrawn when the look or the minute changes.
void EnsureClockBitmap(const ClockLook& look) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    float k = Clock_PixelScale();
    std::wstring key = Clock_StyleKey(look) + Fmt(L"|%.3f|%04d%02d%02d%02d%02d", k, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    if (key != u.clockKey) {
        u.clockKey = key;
        SafeRelease(u.clockBmp);
        if (!Clock_Paint(look, k, t, &u.clockImg)) u.clockImg = ClockImage{};
    }
    if (!u.clockBmp && u.clockImg.w > 0 && u.rt) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        u.rt->CreateBitmap(D2D1::SizeU((UINT32)u.clockImg.w, (UINT32)u.clockImg.h), u.clockImg.px.data(), (UINT32)u.clockImg.w * 4,
                           &bp, &u.clockBmp);
    }
}

// Size of the clock's text as a share of the screen (its glow isn't counted).
void ClockSize(float* fw, float* fh) {
    int tw = u.clockImg.w - 2 * u.clockImg.pad, th = u.clockImg.h - 2 * u.clockImg.pad;
    *fw = tw > 0 && u.screenW > 0 ? std::min(1.f, (float)tw / u.screenW) : 0.35f;
    *fh = th > 0 && u.screenH > 0 ? std::min(1.f, (float)th / u.screenH) : 0.15f;
}

// Where the clock's text sits in `box` (the whole screen).
D2D1_RECT_F ClockRectIn(const D2D1_RECT_F& box, const ClockLook& l) {
    float fw, fh;
    ClockSize(&fw, &fh);
    float bw = box.right - box.left, bh = box.bottom - box.top;
    float cx = box.left + l.x * bw, cy = box.top + l.y * bh;
    return R(cx - fw * bw / 2, cy - fh * bh / 2, cx + fw * bw / 2, cy + fh * bh / 2);
}

// Draws the clock picture (text and glow) into `box` (the whole screen).
void DrawClockIn(const D2D1_RECT_F& box, const ClockLook& l) {
    if (!u.clockBmp) return;
    D2D1_RECT_F r = ClockRectIn(box, l);
    float px = u.screenW > 0 ? u.clockImg.pad * (box.right - box.left) / u.screenW : 0;
    float py = u.screenH > 0 ? u.clockImg.pad * (box.bottom - box.top) / u.screenH : 0;
    D2D1_RECT_F src = R(0, 0, (float)u.clockImg.w, (float)u.clockImg.h), dst = R(r.left - px, r.top - py, r.right + px, r.bottom + py);
    if (u.dc) u.dc->DrawBitmap(u.clockBmp, &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src, nullptr);
    else u.rt->DrawBitmap(u.clockBmp, &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
}

void DrawStill(const D2D1_RECT_F& box) {
    Fill(box, u.stillBg, 6);
    if (!u.still && !u.stillPx.empty() && u.rt) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        u.rt->CreateBitmap(D2D1::SizeU((UINT32)u.stillW, (UINT32)u.stillH), u.stillPx.data(), (UINT32)u.stillW * 4, &bp, &u.still);
    }
    if (!u.still) {
        Text(u.stillLoading ? L"Loading your wallpaper\u2026" : L"Your still wallpaper", box, u.fBody,
             D2D1::ColorF(0.75f, 0.75f, 0.75f), DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    float bw = box.right - box.left, bh = box.bottom - box.top;
    D2D1_RECT_F src = R(0, 0, (float)u.stillW, (float)u.stillH), dst;
    if (u.stillStyle == 3) {  // centred at its own size
        float sx = bw / std::max(1, u.screenW), w = u.stillSrcW * sx, h = u.stillSrcH * sx;
        dst = R(box.left + (bw - w) / 2, box.top + (bh - h) / 2, box.left + (bw + w) / 2, box.top + (bh + h) / 2);
    } else {
        FitRect f = ComputeFit((double)u.stillW / u.stillH, Crop{}, u.stillStyle, bw, bh);
        src = R(f.sl * u.stillW, f.st * u.stillH, f.sr * u.stillW, f.sb * u.stillH);
        dst = R(box.left + f.dl, box.top + f.dt, box.left + f.dr, box.top + f.db);
    }
    ID2D1RoundedRectangleGeometry* clip = PushRounded(box, 6);
    if (u.dc) u.dc->DrawBitmap(u.still, &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src, nullptr);
    else u.rt->DrawBitmap(u.still, &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
    PopRounded(clip);
}

// The wallpaper the edited look belongs to, drawn into `box` (the whole screen).
void DrawWallpaper(const D2D1_RECT_F& box) {
    if (u.clockEdit == 0) DrawComposed(box, g_settings.crop, g_settings.scale);
    else DrawStill(box);
}

D2D1_COLOR_F LookColor(const std::wstring& s) {
    BYTE r = 255, g = 255, b = 255;
    ParseColor(s, &r, &g, &b);
    return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f);
}

std::wstring Hex(const std::wstring& color) {
    BYTE r = 255, g = 255, b = 255;
    ParseColor(color, &r, &g, &b);
    return Fmt(L"#%02X%02X%02X", r, g, b);
}

// The wallpaper's colour at a point of `screen` (a preview of the whole screen), read from the
// picture itself so the clock drawn on top doesn't count. Averages 3x3 source pixels.
bool SampleWallpaper(const D2D1_RECT_F& screen, float mx, float my, std::wstring* out) {
    const D2D1_RECT_F& b = screen;
    if (!In(b, mx, my)) return false;
    float bw = b.right - b.left, bh = b.bottom - b.top, px = mx - b.left, py = my - b.top;
    const uint32_t* src;
    int sw, sh;
    FitRect f;
    D2D1_COLOR_F bg = D2D1::ColorF(0, 0, 0);
    if (u.clockEdit == 0) {
        if (u.framePx.empty()) return false;
        src = u.framePx.data();
        sw = u.fw;
        sh = u.fh;
        f = ComputeFit(VideoAspect(), g_settings.crop, g_settings.scale, bw, bh);
    } else {
        if (u.stillPx.empty()) return false;
        src = u.stillPx.data();
        sw = u.stillW;
        sh = u.stillH;
        bg = u.stillBg;
        if (u.stillStyle == 3) {  // centred at its own size
            float s = bw / std::max(1, u.screenW), w = u.stillSrcW * s, h = u.stillSrcH * s;
            f = FitRect{0, 0, 1, 1, (bw - w) / 2, (bh - h) / 2, (bw + w) / 2, (bh + h) / 2};
        } else {
            f = ComputeFit((double)u.stillW / u.stillH, Crop{}, u.stillStyle, bw, bh);
        }
    }
    int r, g, bl;
    if (px < f.dl || px >= f.dr || py < f.dt || py >= f.db || sw <= 0 || sh <= 0) {  // bars around the picture
        r = lroundf(bg.r * 255);
        g = lroundf(bg.g * 255);
        bl = lroundf(bg.b * 255);
    } else {
        int cx = (int)((f.sl + (px - f.dl) / (f.dr - f.dl) * (f.sr - f.sl)) * sw);
        int cy = (int)((f.st + (py - f.dt) / (f.db - f.dt) * (f.sb - f.st)) * sh);
        int n = 0;
        r = g = bl = 0;
        for (int yy = cy - 1; yy <= cy + 1; yy++)
            for (int xx = cx - 1; xx <= cx + 1; xx++) {
                if (xx < 0 || yy < 0 || xx >= sw || yy >= sh) continue;
                uint32_t v = src[(size_t)yy * sw + xx];
                r += (v >> 16) & 0xFF;
                g += (v >> 8) & 0xFF;
                bl += v & 0xFF;
                n++;
            }
        if (!n) return false;
        r /= n;
        g /= n;
        bl /= n;
    }
    *out = Fmt(L"%d,%d,%d", r, g, bl);
    return true;
}

// The eyedropper's loupe: the colour under it, next to the cursor.
void Loupe(float x, float y, const D2D1_RECT_F& box, const std::wstring& color) {
    float lx = std::min(x + 26, box.right - 20), ly = std::max(y - 26, box.top + 20);
    Circle(lx, ly, 17, D2D1::ColorF(0, 0, 0, 0.6f));
    Circle(lx, ly, 15.5f, D2D1::ColorF(1, 1, 1));
    Circle(lx, ly, 13, LookColor(color));
}

// A small dark label on a preview.
void Badge(const std::wstring& s, float x, float y) {
    float tw = TextWidth(s, u.fSmall) + 16;
    Fill(R(x, y, x + tw, y + 20), D2D1::ColorF(0, 0, 0, 0.55f), 10);
    Text(s, R(x, y, x + tw, y + 20), u.fSmall, Rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// A button that shows a colour and its name.
void ColorButton(int id, const D2D1_RECT_F& r, const std::wstring& color, const std::wstring& name, bool enabled) {
    const Theme& t = u.th;
    bool hot = enabled && IsHot(id), pressed = enabled && u.press == id && u.hot == id;
    Fill(r, pressed ? t.ctrlPress : hot ? t.ctrlHover : t.ctrl, 4);
    Stroke(r, t.ctrlStroke, 4);
    float cy = (r.top + r.bottom) / 2;
    Circle(r.left + 17, cy, 8, t.ctrlStroke);
    Circle(r.left + 17, cy, 7, enabled ? LookColor(color) : Mix(LookColor(color), t.card, 0.6f));
    Text(name, R(r.left + 33, r.top, r.right - 28, r.bottom), u.fBody, enabled ? t.text : t.text3);
    Text(L"\uE790", R(r.right - 26, r.top, r.right - 10, r.bottom), u.fIconSmall, enabled ? t.text2 : t.text3,
         DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(id, K_BUTTON, r, enabled);
}

// The font a look shows: its own, or Audiowide when that font isn't on this PC any more.
std::wstring ShownFont(const ClockLook& l) { return Clock_FontReady(l.font) ? l.font : std::wstring(L"Audiowide"); }

// A button that shows the clock's font and opens the font picker.
void FontButton(int id, const D2D1_RECT_F& r, const std::wstring& font, bool enabled) {
    const Theme& t = u.th;
    bool hot = enabled && IsHot(id), pressed = enabled && u.press == id && u.hot == id;
    Fill(r, pressed ? t.ctrlPress : hot ? t.ctrlHover : t.ctrl, 4);
    Stroke(r, t.ctrlStroke, 4);
    Text(L"\uE8D2", R(r.left + 8, r.top, r.left + 26, r.bottom), u.fIconSmall, enabled ? t.text2 : t.text3, DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(font, R(r.left + 33, r.top, r.right - 28, r.bottom), u.fBody, enabled ? t.text : t.text3);
    Text(L"\uE76C", R(r.right - 26, r.top, r.right - 10, r.bottom), u.fIconSmall, enabled ? t.text2 : t.text3,
         DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(id, K_BUTTON, r, enabled);
}

// Under the clock page's preview: the colour row, three rows of settings and the help line.
const float kClockBelow = 10 + 4 * 36 + 4 + 20;

// Window height that shows the clock page's preview at full width.
float ClockPageHeight() {
    return kTop + kCardHead + 44 + (kW - 24 - kContentX - 2 * kPad) * (float)ScreenAspectH() + kClockBelow + 14 + 24;
}

void PaintClockPage(float x, float w) {
    const Theme& t = u.th;
    const float y = kTop, lx = x + kPad, iw = w - 2 * kPad, rowH = 36, below = kClockBelow;
    // The preview as big as fits over the settings under it; the card ends under the help line.
    float bw = iw, bh = bw * (float)ScreenAspectH();
    const float maxH = kH - 24 - 14 - below - (y + kCardHead + 44);
    if (bh > maxH) {
        bh = maxH;
        bw = bh / (float)ScreenAspectH();
    }
    Card(R(x, y, x + w, y + kCardHead + 44 + bh + below + 14), L"\uE121", L"Desktop clock");
    const bool on = ClockUsable();
    const std::wstring& video = g_settings.video;
    // Which look is being edited: the wallpaper (in the header), then for the video wallpaper the
    // look every video shares or this video's own; Show / Hide is part of the look picked.
    // Two equal columns: the wallpaper choice sits right above Show / Hide.
    const float segW = (iw - 16) / 2, ry = y + kCardHead;
    Seg(ID_CLK_MODE, R(x + w - kPad - segW, y + 12, x + w - kPad, y + 44), {L"Video wallpaper", L"Still wallpaper"}, u.clockEdit);
    Seg(ID_CLK_SCOPE, R(lx, ry, lx + segW, ry + 32), {L"This video", L"All videos"}, g_settings.clockVideoOwn ? 0 : 1,
        u.clockEdit == 0 && !video.empty());
    Seg(ID_CLK_SHOW, R(x + w - kPad - segW, ry, x + w - kPad, ry + 32), {L"Show clock", L"Hide clock"}, EditedLook()->show ? 0 : 1);

    // Preview: the wallpaper with the clock on it; drag the clock to place it.
    float bx = x + (w - bw) / 2;
    D2D1_RECT_F box = R(bx, ry + 44, bx + bw, ry + 44 + bh);
    u.clockBox = box;
    ClockLook* look = EditedLook();
    ClockLook shown = *look;  // an eyedropper previews the colour under it
    if (u.picking && !u.pickColor.empty()) shown.color = u.pickColor;
    if (dlg.open && dlg.picking && !u.pickColor.empty()) (dlg.target == 0 ? shown.color : shown.glowColor) = u.pickColor;
    EnsureClockBitmap(shown);
    DrawWallpaper(box);
    ID2D1RoundedRectangleGeometry* clip = PushRounded(box, 6);
    DrawClockIn(box, *look);
    if (u.drag == ID_CLK_AREA) {  // snap guides
        D2D1_COLOR_F guide = D2D1::ColorF(1, 1, 1, 0.7f);
        if (u.snapX) Line(box.left + bw / 2, box.top, box.left + bw / 2, box.bottom, guide);
        if (u.snapY) Line(box.left, box.top + u.snapYv * bh, box.right, box.top + u.snapYv * bh, guide);
    }
    if (u.picking && !u.pickColor.empty()) Loupe(u.pickAt.x, u.pickAt.y, box, u.pickColor);
    PopRounded(clip);
    if (on && (u.drag == ID_CLK_AREA || (u.overClock && u.hot == ID_CLK_AREA && !u.picking)))
        Badge(Fmt(L"%.1f%% across  \u00B7  %.1f%% down", look->x * 100, look->y * 100), box.left + 8, box.bottom - 28);
    if (!on) Fill(box, D2D1::ColorF(t.card.r, t.card.g, t.card.b, 0.55f), 6);  // greyed out
    AddHit(ID_CLK_AREA, K_AREA, box, on);
    float yy = box.bottom + 10;

    // Colour (the colour dialog, or an eyedropper for the wallpaper) and the font, side by side.
    // Left column: the text (size, opacity, 12/24-hour). Right column: its glow.
    const float labW = 80, colW = (iw - 24) / 2, rx = lx + colW + 24, cw = colW - labW;
    Row(lx, yy, labW, L"Colour", rowH, on);
    ColorButton(ID_CLK_CUSTOM, R(lx + labW, yy + 3, lx + colW - 42, yy + rowH - 3), look->color, Hex(look->color), on);
    Button(ID_CLK_EYEDROP, R(lx + colW - 36, yy + 3, lx + colW, yy + rowH - 3), L"", L"\uEF3C", u.picking, on);
    Row(rx, yy, labW, L"Font", rowH, on);
    FontButton(ID_CLK_FONT, R(rx + labW, yy + 3, rx + colW, yy + rowH - 3), ShownFont(*look), on);
    yy += rowH;

    const bool glowOn = on && look->glow > 0;
    auto sliderRow = [&](float cx0, int id, const wchar_t* label, float frac, const std::wstring& value, bool en) {
        Row(cx0, yy, labW, label, rowH, en);
        Slider(id, SliderRect(cx0 + labW, yy, cw, rowH), frac, en);
        SliderValue(value, cx0 + labW, yy, cw, en, rowH);
    };
    sliderRow(lx, ID_CLK_SIZE, L"Size", (look->size - 0.5f) / 1.5f, Fmt(L"%.0f%%", look->size * 100), on);
    sliderRow(rx, ID_CLK_GLOW, L"Glow", look->glow, look->glow > 0 ? Fmt(L"%.0f%%", look->glow * 100) : std::wstring(L"Off"), on);
    yy += rowH;
    sliderRow(lx, ID_CLK_OPACITY, L"Opacity", (look->opacity - 0.2f) / 0.8f, Fmt(L"%.0f%%", look->opacity * 100), on);
    sliderRow(rx, ID_CLK_GLOWSIZE, L"Glow size", (look->glowSize - 2) / 38, Fmt(L"%.0f", look->glowSize), glowOn);
    yy += rowH;
    Row(lx, yy, labW, L"Time", rowH, on);
    Seg(ID_CLK_HOURS, SegRect(lx + labW, yy, cw, rowH), {L"12-hour", L"24-hour"}, look->h24 ? 1 : 0, on);
    Row(rx, yy, labW, L"Glow colour", rowH, glowOn);
    ColorButton(ID_CLK_GLOWCOLOR, R(rx + labW, yy + 3, rx + colW, yy + rowH - 3),
                look->glowColor.empty() ? look->color : look->glowColor,
                look->glowColor.empty() ? std::wstring(L"Same as the text") : Hex(look->glowColor), glowOn);
    yy += rowH + 4;

    // Help line.
    std::wstring help;
    switch (u.drag ? u.drag : u.hot) {
        case ID_CLK_SHOW:
            help = u.clockEdit == 1           ? L"Show or hide the clock while the video wallpaper is off"
                   : g_settings.clockVideoOwn ? L"Show or hide the clock with this video and all its versions"
                                              : L"Show or hide the clock on the video wallpaper, for every video using the shared look";
            break;
        case ID_CLK_MODE: help = L"Video wallpaper: the clock while a video plays. Still wallpaper: while the video wallpaper is off"; break;
        case ID_CLK_SCOPE:
            help = u.clockEdit == 1 ? L"Only for the video wallpaper: a clock just for this video, or the one every video shares"
                                    : L"This video's own clock, or the one every video shares (this video keeps its own)";
            break;
        case ID_CLK_CUSTOM: help = L"Any colour, from a colour field, a hex code or your saved colours (Ctrl+V pastes a code)"; break;
        case ID_CLK_FONT: help = L"The font of the whole clock in this look"; break;
        case ID_CLK_EYEDROP:
            help = u.picking ? L"Click here again (or press Esc) to stop picking"
                             : L"Take the colour from the wallpaper: click Pick, then the preview";
            break;
        case ID_CLK_SIZE: help = L"The clock's size in this look"; break;
        case ID_CLK_OPACITY: help = L"How solid the clock is; lower lets the wallpaper show through (glow included)"; break;
        case ID_CLK_HOURS: help = L"12-hour (1:30 PM) or 24-hour (13:30) time"; break;
        case ID_CLK_GLOW: help = L"A soft glow behind the letters that makes the clock stand out on busy wallpapers"; break;
        case ID_CLK_GLOWSIZE: help = L"How far the glow spreads"; break;
        case ID_CLK_GLOWCOLOR: help = L"The glow's colour; a dark glow works as a soft shadow"; break;
        case ID_CLK_AREA: help = L"Drag the clock to place it (Alt: no snapping); arrow keys nudge it (Shift: 10\u00D7)"; break;
        default: break;
    }
    if (u.picking && (u.hot == ID_CLK_AREA || help.empty()))
        help = u.pickColor.empty() ? L"Click the preview to take the wallpaper's colour there (Esc cancels)"
                                   : L"Click to use " + Hex(u.pickColor) + L" for the clock (Esc cancels)";
    if (!u.clockNote.empty() && GetTickCount() - u.clockNoteAt < 3000) help = u.clockNote;
    if (help.empty())
        help = !on                             ? L"The clock is hidden with this look; its settings are kept for when you show it again"
               : u.clockEdit == 1              ? L"Editing the clock for your still wallpaper"
               : g_settings.clockVideoOwn      ? L"Editing the clock just for " + FileName(OriginalOf(video).empty() ? video : OriginalOf(video)) +
                                                     L" and all its versions"
               : g_settings.clockVideoSaved    ? L"Editing the shared look; this video's own look is kept for when you switch it on"
                                               : L"Editing the shared look, used by every video without its own";
    Text(L"\uE946", R(lx, yy, lx + 16, yy + 20), u.fIconSmall, t.text3);
    Text(help, R(lx + 22, yy, x + w - kPad, yy + 20), u.fSmall, t.text3);
}

// A rect standing for the whole screen, zoomed so the clock fills about two thirds of `p`.
D2D1_RECT_F ZoomedScreen(const D2D1_RECT_F& p, const ClockLook& look) {
    float pw = p.right - p.left, ph = p.bottom - p.top, fw, fh;
    ClockSize(&fw, &fh);
    const float aspect = (float)ScreenAspectH();
    float vw = std::clamp(pw * 0.66f / std::max(fw, 0.02f), pw, pw * 10);
    if (vw * aspect < ph) vw = ph / aspect;  // always covers the preview
    float vh = vw * aspect;
    float l = std::clamp((p.left + p.right) / 2 - look.x * vw, p.right - vw, p.left);
    float t = std::clamp((p.top + p.bottom) / 2 - look.y * vh, p.bottom - vh, p.top);
    return R(l, t, l + vw, t + vh);
}

void PaintColorDialog() {
    const Theme& t = u.th;
    Fill(R(0, 0, kW, kH), D2D1::ColorF(0, 0, 0, t.dark ? 0.5f : 0.3f));
    AddHit(ID_DLG_SCRIM, K_AREA, R(0, 0, kW, kH));  // the page underneath can't be used meanwhile
    const float W = kDialogW, H = 480, L = (kW - W) / 2, T = std::max(12.f, (kH - H) / 2);
    const D2D1_RECT_F panel = R(L, T, L + W, T + H);
    const D2D1_COLOR_F face = t.dark ? Rgb(0x2B2B2B) : Rgb(0xFFFFFF), bar = t.dark ? Rgb(0x202020) : Rgb(0xF3F3F3);
    Fill(R(L - 2, T, L + W + 2, T + H + 4), D2D1::ColorF(0, 0, 0, 0.2f), 10);  // shadow
    Fill(panel, face, 8);
    ID2D1RoundedRectangleGeometry* clip = PushRounded(panel, 8);
    Fill(R(L, T + H - 64, L + W, T + H), bar);
    Line(L, T + H - 64, L + W, T + H - 64, t.stroke);
    PopRounded(clip);
    Stroke(panel, t.dark ? Rgb(0x3C3C3C) : Rgb(0xD5D5D5), 8);

    ClockLook* look = EditedLook();
    Text(dlg.target == 0 ? L"Clock colour" : L"Glow colour", R(L + 24, T + 16, L + 300, T + 46), u.fSubtitle, t.text);
    if (dlg.target == 1) {
        Text(L"Same as the text", R(L + W - 24 - 52 - 160, T + 16, L + W - 24 - 52, T + 46), u.fBody, t.text,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
        Toggle(ID_DLG_SAME, L + W - 24 - 40, T + 31, dlg.sameAsText);
    }
    BYTE cr = 255, cg = 255, cb = 255;
    ParseColor(DlgColor(), &cr, &cg, &cb);
    const D2D1_COLOR_F cur = D2D1::ColorF(cr / 255.f, cg / 255.f, cb / 255.f);

    // Saturation (across) and brightness (down) for the hue below.
    dlg.sv = R(L + 24, T + 62, L + 264, T + 234);
    const D2D1_RECT_F& sv = dlg.sv;
    BYTE hr, hg, hb;
    HsvToRgb(dlg.h, 1, 1, &hr, &hg, &hb);
    const D2D1_COLOR_F pure = D2D1::ColorF(hr / 255.f, hg / 255.f, hb / 255.f);
    Fill(sv, pure, 6);
    D2D1_GRADIENT_STOP white[2] = {{0, D2D1::ColorF(1, 1, 1, 1)}, {1, D2D1::ColorF(1, 1, 1, 0)}};
    FillGradient(sv, 6, D2D1::Point2F(sv.left, sv.top), D2D1::Point2F(sv.right, sv.top), white, 2);
    D2D1_GRADIENT_STOP black[2] = {{0, D2D1::ColorF(0, 0, 0, 0)}, {1, D2D1::ColorF(0, 0, 0, 1)}};
    FillGradient(sv, 6, D2D1::Point2F(sv.left, sv.top), D2D1::Point2F(sv.left, sv.bottom), black, 2);
    Stroke(sv, t.ctrlStroke, 6);
    float px = sv.left + dlg.s * (sv.right - sv.left), py = sv.top + (1 - dlg.v) * (sv.bottom - sv.top);
    Circle(px, py, 9, D2D1::ColorF(0, 0, 0, 0.45f));
    Circle(px, py, 8, D2D1::ColorF(1, 1, 1));
    Circle(px, py, 5.5f, cur);
    AddHit(ID_DLG_SV, K_AREA, R(sv.left - 6, sv.top - 6, sv.right + 6, sv.bottom + 6));

    // Hue.
    dlg.hue = R(L + 24, T + 248, L + 264, T + 264);
    const D2D1_RECT_F& hu = dlg.hue;
    D2D1_GRADIENT_STOP rainbow[7];
    for (int i = 0; i < 7; i++) {
        BYTE r, g, b;
        HsvToRgb(i * 60.f, 1, 1, &r, &g, &b);
        rainbow[i] = D2D1_GRADIENT_STOP{i / 6.f, D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f)};
    }
    FillGradient(hu, 8, D2D1::Point2F(hu.left, hu.top), D2D1::Point2F(hu.right, hu.top), rainbow, 7);
    float kx = hu.left + dlg.h / 360 * (hu.right - hu.left), ky = (hu.top + hu.bottom) / 2;
    Circle(kx, ky, 11, D2D1::ColorF(0, 0, 0, 0.45f));
    Circle(kx, ky, 10, D2D1::ColorF(1, 1, 1));
    Circle(kx, ky, 7, pure);
    AddHit(ID_DLG_HUE, K_AREA, R(hu.left - 8, hu.top - 8, hu.right + 8, hu.bottom + 8));

    // Preview: the wallpaper around the clock, in the new colour.
    const float rx = L + 284, rw = W - 24 - 284;
    dlg.preview = R(rx, T + 62, rx + rw, T + 182);
    dlg.previewScreen = ZoomedScreen(dlg.preview, *look);
    clip = PushRounded(dlg.preview, 6);
    DrawWallpaper(dlg.previewScreen);
    DrawClockIn(dlg.previewScreen, *look);
    if (dlg.picking && !u.pickColor.empty()) Loupe(u.pickAt.x, u.pickAt.y, dlg.preview, u.pickColor);
    PopRounded(clip);
    Stroke(dlg.preview, t.ctrlStroke, 6);
    AddHit(ID_DLG_PREVIEW, K_AREA, dlg.preview, dlg.picking);

    // Hex code (click to type, or just type / paste), copy, eyedropper.
    const float hy = T + 194;
    D2D1_RECT_F hx = R(rx, hy, rx + 124, hy + 32);
    Fill(hx, dlg.hexEdit ? face : (IsHot(ID_DLG_HEX) ? t.ctrlHover : t.ctrl), 4);
    Stroke(hx, dlg.hexEdit ? t.accent : t.ctrlStroke, 4, dlg.hexEdit ? 2 : 1);
    std::wstring code = dlg.hexEdit ? L"#" + dlg.hex : Hex(DlgColor());
    float cw = TextWidth(code, u.fBody);
    if (dlg.hexEdit && dlg.hexFresh) Fill(R(hx.left + 10, hy + 7, hx.left + 14 + cw, hy + 25), Mix(t.accent, face, 0.55f), 2);
    Text(code, R(hx.left + 12, hy, hx.right - 8, hy + 32), u.fBody, t.text);
    if (dlg.hexEdit && !dlg.hexFresh) Line(hx.left + 13 + cw, hy + 8, hx.left + 13 + cw, hy + 24, t.text);
    AddHit(ID_DLG_HEX, K_BUTTON, hx);
    Button(ID_DLG_COPY, R(rx + 130, hy, rx + 166, hy + 32), L"", L"\uE8C8");
    Button(ID_DLG_PICK, R(rx + 172, hy, rx + rw, hy + 32), L"", L"\uEF3C", dlg.picking);

    // RGB, and the colour before and now.
    const float iy = T + 236;
    Text(Fmt(L"RGB  %d, %d, %d", cr, cg, cb), R(rx, iy, rx + 150, iy + 20), u.fSmall, t.text2);
    D2D1_RECT_F ch = R(rx + 152, iy + 1, rx + rw, iy + 19);
    clip = PushRounded(ch, 4);
    Fill(R(ch.left, ch.top, (ch.left + ch.right) / 2, ch.bottom), LookColor(dlg.before.empty() ? look->color : dlg.before));
    Fill(R((ch.left + ch.right) / 2, ch.top, ch.right, ch.bottom), cur);
    PopRounded(clip);
    Stroke(ch, t.ctrlStroke, 4);

    // My colours: choose a box, then save into it; double-click a box to use its colour.
    const float my = T + 282;
    Text(L"My colours", R(L + 24, my, L + 260, my + 22), u.fStrong, t.text);
    for (int i = 0; i < 16; i++) {
        float bx = L + 24 + (i % 8) * 31, by = my + 30 + (i / 8) * 31;
        D2D1_RECT_F r = R(bx, by, bx + 25, by + 25);
        const std::wstring& c = g_settings.myColors[i];
        bool hot = IsHot(ID_DLG_SLOT0 + i);
        if (dlg.slot == i) Stroke(R(r.left - 3, r.top - 3, r.right + 3, r.bottom + 3), t.accent, 6, 2);
        Fill(r, c.empty() ? (hot ? t.ctrlHover : t.ctrl) : LookColor(c), 4);
        Stroke(r, hot ? t.text2 : t.ctrlStroke, 4);
        AddHit(ID_DLG_SLOT0 + i, K_BUTTON, r);
    }
    bool slotFull = dlg.slot >= 0 && !g_settings.myColors[dlg.slot].empty();
    Button(ID_DLG_SAVE, R(rx, my + 29, rx + rw, my + 59), L"Save to box", L"\uE74E");
    Button(ID_DLG_USE, R(rx, my + 65, rx + rw, my + 95), L"Use box colour", L"\uE790", false, slotFull);
    Text(L"Click a box to choose it, double-click to use it, right-click to empty it",
         R(L + 24, my + 104, L + W - 24, my + 124), u.fSmall, t.text3);

    // Bottom bar.
    const float by = T + H - 48;
    bool note = !dlg.msg.empty() && GetTickCount() - dlg.msgAt < 3000;
    Text(note ? dlg.msg : std::wstring(L"Ctrl+V pastes a colour code"), R(L + 24, by, L + W - 24 - 236, by + 32), u.fSmall,
         note ? (dlg.msgWarn ? t.warn : t.good) : t.text3);
    Button(ID_DLG_CANCEL, R(L + W - 24 - 228, by, L + W - 24 - 118, by + 32), L"Cancel");
    Button(ID_DLG_OK, R(L + W - 24 - 110, by, L + W - 24, by + 32), L"Done", nullptr, true);
}

// ---------------------------------------------------------------------------------------
// Video versions: the original and lighter copies of it, each shown on the desktop before choosing.

struct VersionsDialog {
    bool open = false;
    std::wstring original;  // "" = no longer there
    std::vector<Version> list;
    int sel = -1;
    std::wstring note;
} ver;

void VerSelect(int i);

void OpenVersions() {
    const std::wstring& video = g_settings.video;
    ver = VersionsDialog{};
    ver.original = IsLightCopy(video) ? LightCopyOriginalPath(video) : video;
    UINT vw = 0, vh = 0;
    if (SamePath(ver.original, video) && u.infoOk) {
        vw = u.info.w;
        vh = u.info.h;
    } else if (!ver.original.empty()) {
        VideoInfo vi;
        if (ProbeVideo(ver.original, vi)) { vw = vi.w; vh = vi.h; }
    }
    // Sizes that cover the screen for the part shown (the crop), and steps below that.
    const Crop& c = g_settings.crop;
    double need = vw && vh ? std::max(u.screenW / ((c.r - c.l) * vw), u.screenH / ((c.b - c.t) * vh)) : 1;
    if (vw && vh) {
        Version o;
        o.label = L"Original";
        o.w = vw;
        o.h = vh;
        o.path = ver.original;
        o.bytes = FileBytes(ver.original);
        o.original = true;
        ver.list.push_back(o);
        for (double f : {1.0, 5.0 / 6, 2.0 / 3, 0.5}) {
            double sc = need * f;
            if (sc > 0.95) continue;  // hardly smaller than the original
            Version v;
            v.w = ((UINT)lround(vw * sc) + 1) & ~1u;
            v.h = ((UINT)lround(vh * sc) + 1) & ~1u;
            if (v.h < 200) continue;
            UINT lines = (UINT)lround(u.screenH * f);
            v.label = f == 1.0 ? Fmt(L"%up (your screen)", lines) : Fmt(L"%up", lines);
            ver.list.push_back(v);
        }
    }
    // Copies made earlier fill in their size, or show as they are (made for another crop).
    if (!ver.original.empty()) {
        for (Version& cp : LightCopiesOf(ver.original)) {
            Version* match = nullptr;
            for (Version& v : ver.list)
                if (!v.original && v.path.empty() && (int)v.w - (int)cp.w <= 8 && (int)cp.w - (int)v.w <= 8 &&
                    (int)v.h - (int)cp.h <= 8 && (int)cp.h - (int)v.h <= 8)
                    match = &v;
            if (match) {
                match->path = cp.path;
                match->bytes = cp.bytes;
                continue;
            }
            cp.label = vh ? VersionLabel(cp.h, vw, vh) : Fmt(L"%u\u00D7%u", cp.w, cp.h);
            ver.list.push_back(cp);
        }
    } else if (u.infoOk) {  // the original is gone: just this copy
        Version v;
        v.label = L"This copy";
        v.w = u.info.w;
        v.h = u.info.h;
        v.path = video;
        v.bytes = FileBytes(video);
        ver.list.push_back(v);
    }
    std::stable_sort(ver.list.begin(), ver.list.end(), [](const Version& a, const Version& b) { return (UINT64)a.w * a.h > (UINT64)b.w * b.h; });
    if (ver.list.empty()) return;
    ver.open = true;
    int cur = 0;
    for (size_t i = 0; i < ver.list.size(); i++)
        if (SamePath(ver.list[i].path, video)) cur = (int)i;
    VerSelect(cur);
}

void CloseVersions() {
    if (!ver.open) return;
    Host_PreviewSize(0, 0);
    ver.open = false;
}

// Choosing a row shows that version on the desktop: the video playing, scaled down to its size.
void VerSelect(int i) {
    ver.sel = i;
    const Version& v = ver.list[i];
    UINT pw = u.infoOk ? u.info.w : 0;
    bool current = SamePath(v.path, g_settings.video);
    bool canShow = !current && v.w + 8 < pw;
    if (canShow && WantVideoAudio(g_settings)) {
        canShow = false;
        ver.note = L"Turn the video's sound off (Sound page) to see versions on the desktop";
    } else if (canShow) {
        ver.note = L"Showing " + v.label + L" on your desktop: look around this window";
    } else if (current) {
        ver.note = L"This is the version playing";
    } else {
        ver.note = L"Sharper than the one playing: use it to see it";
    }
    if (!current && v.path.empty()) ver.note += L"; making it takes a few seconds";
    Host_PreviewSize(canShow ? v.w : 0, canShow ? v.h : 0);
}

void VerApply() {
    if (ver.sel < 0 || ver.sel >= (int)ver.list.size()) return;
    Version v = ver.list[ver.sel];
    if (SamePath(v.path, g_settings.video)) { CloseVersions(); return; }
    CloseVersions();
    if (v.path.empty()) {
        if (!ver.original.empty()) StartLightCopy(ver.original, v.w, v.h, v.label);
        return;
    }
    if (!v.original) LinkLightCopy(v.path, ver.original);
    Crop crop = g_settings.crop;
    double t = g_settings.previewTime;
    Host_SetVideo(v.path, &crop);  // crops are normalized, so the same crop fits every version
    g_settings.previewTime = t;
    Host_SettingsChanged();
    Log(L"ui: switched to the %ls version", v.label.c_str());
}

void VerDelete(int i) {
    if (i < 0 || i >= (int)ver.list.size()) return;
    Version& v = ver.list[i];
    if (v.original || v.path.empty() || SamePath(v.path, g_settings.video)) return;
    if (DeleteFileW(v.path.c_str())) {
        Log(L"ui: deleted the %ls version %ls", v.label.c_str(), v.path.c_str());
        v.path.clear();
        v.bytes = 0;
    }
    VerSelect(ver.sel);
}

void PaintVersionsDialog() {
    const Theme& t = u.th;
    Fill(R(0, 0, kW, kH), D2D1::ColorF(0, 0, 0, t.dark ? 0.5f : 0.3f));
    AddHit(ID_VER_SCRIM, K_AREA, R(0, 0, kW, kH));  // the page underneath can't be used meanwhile
    const int n = (int)ver.list.size();
    const float rowH = 52, W = kDialogW, H = 112 + n * rowH + 72, L = (kW - W) / 2, T = std::max(12.f, (kH - H) / 2);
    const D2D1_RECT_F panel = R(L, T, L + W, T + H);
    const D2D1_COLOR_F face = t.dark ? Rgb(0x2B2B2B) : Rgb(0xFFFFFF), bar = t.dark ? Rgb(0x202020) : Rgb(0xF3F3F3);
    Fill(R(L - 2, T, L + W + 2, T + H + 4), D2D1::ColorF(0, 0, 0, 0.2f), 10);  // shadow
    Fill(panel, face, 8);
    ID2D1RoundedRectangleGeometry* clip = PushRounded(panel, 8);
    Fill(R(L, T + H - 64, L + W, T + H), bar);
    Line(L, T + H - 64, L + W, T + H - 64, t.stroke);
    PopRounded(clip);
    Stroke(panel, t.dark ? Rgb(0x3C3C3C) : Rgb(0xD5D5D5), 8);

    Text(L"Video versions", R(L + 24, T + 16, L + W - 24, T + 46), u.fSubtitle, t.text);
    Text(L"A smaller version takes less memory but looks softer. Click one to see it on your desktop, then choose.",
         R(L + 24, T + 52, L + W - 24, T + 92), u.fWrap, t.text2);

    float y = T + 100;
    for (int i = 0; i < n; i++, y += rowH) {
        const Version& v = ver.list[i];
        const D2D1_RECT_F r = R(L + 16, y, L + W - 16, y + rowH - 6);
        const bool sel = ver.sel == i, current = SamePath(v.path, g_settings.video);
        const bool making = u.optBusy && !u.optGif && u.optW == v.w && u.optH == v.h;
        if (sel) {
            Fill(r, Mix(face, t.accent, 0.14f), 6);
            Stroke(r, t.accent, 6);
        } else if (IsHot(ID_VER_ROW0 + i)) {
            Fill(r, Mix(face, t.text, 0.05f), 6);
        }
        AddHit(ID_VER_ROW0 + i, K_BUTTON, r);
        const float cy = (r.top + r.bottom) / 2;
        Circle(r.left + 22, cy, 8, sel ? t.accent : t.text2);
        Circle(r.left + 22, cy, sel ? 3.5f : 6.8f, sel ? t.onAccent : IsHot(ID_VER_ROW0 + i) ? Mix(face, t.text, 0.05f) : face);
        // Columns: the version, the memory it takes, its file's size, and its state (or a delete button).
        Text(v.label, R(r.left + 42, r.top + 5, r.left + 210, cy + 1), u.fStrong, t.text);
        Text(Fmt(L"%u\u00D7%u", v.w, v.h), R(r.left + 42, cy, r.left + 210, r.bottom - 4), u.fSmall, t.text2);
        Text(Fmt(L"~%.0f MB memory", FrameMemoryMb(v.w, v.h)), R(r.left + 210, r.top, r.left + 340, r.bottom), u.fBody, t.text);
        if (!v.path.empty())
            Text(v.bytes < 10 * 1048576ull ? Fmt(L"%.1f MB", v.bytes / 1048576.0) : Fmt(L"%.0f MB", v.bytes / 1048576.0),
                 R(r.left + 340, r.top, r.left + 420, r.bottom), u.fSmall, t.text2, DWRITE_TEXT_ALIGNMENT_TRAILING);
        const D2D1_RECT_F state = R(r.left + 430, r.top, r.right - 12, r.bottom);
        if (making) Text(Fmt(L"Making\u2026 %d%%", u.optPct), state, u.fSmall, t.text2, DWRITE_TEXT_ALIGNMENT_TRAILING);
        else if (current) Text(L"Playing", state, u.fSmall, t.good, DWRITE_TEXT_ALIGNMENT_TRAILING);
        else if (v.path.empty()) Text(L"Not made yet", state, u.fSmall, t.text3, DWRITE_TEXT_ALIGNMENT_TRAILING);
        else if (!v.original) Button(ID_VER_DEL0 + i, R(r.right - 40, cy - 14, r.right - 12, cy + 14), L"", L"\uE74D");
    }

    // Bottom bar: what the desktop shows, and the choice.
    const float by = T + H - 48;
    const Version* sv = ver.sel >= 0 && ver.sel < n ? &ver.list[ver.sel] : nullptr;
    bool current = sv && SamePath(sv->path, g_settings.video);
    bool toMake = sv && sv->path.empty();
    std::wstring use = current ? L"Playing" : toMake ? L"Make it" : L"Use it";
    bool enabled = sv && !current && !(toMake && (u.optBusy || ver.original.empty()));
    std::wstring note = toMake && u.optBusy ? L"Another copy is being made: wait for it to finish" : ver.note;
    Text(note, R(L + 24, by - 2, L + W - 24 - 256, by + 34), u.fWrap, t.text3);
    Button(ID_VER_CANCEL, R(L + W - 24 - 236, by, L + W - 24 - 126, by + 32), L"Close");
    Button(ID_VER_USE, R(L + W - 24 - 118, by, L + W - 24, by + 32), use, nullptr, true, enabled);
}

// ---------------------------------------------------------------------------------------
// Font picker: every font, each drawn as the clock in it (just the letters, in the page's text
// colour); a font VideoBG can't include has a link to get it and a button to add its file instead.
// At the end, the user can add any font of their own; VideoBG keeps a copy in its fonts folder.
// Clicking a font uses it at once for the look being edited. Each font is loaded only while its
// sample is drawn.

void ClockEdited();

const float kFontRowH = 132, kFontAddH = 76;  // a font's row; the "Add your own font" row at the end

struct FontDialog {
    bool open = false;
    std::vector<ClockFontInfo> list;
    std::vector<ID2D1Bitmap*> samples;
    std::vector<D2D1_SIZE_F> sizes;  // in DIPs
    std::vector<bool> tried;
    float boxW = 0;                  // the room the samples were drawn for
    std::wstring note;
    float scroll = 0, target = 0, maxScroll = 0;  // the list, in DIPs: where it is, and where it's gliding to
    float trackTop = 0, trackH = 0, thumbH = 0, grab = 0;  // the scroll bar, for dragging it
} fnt;

// The wheel and the arrow keys glide the list to `to`; dragging the bar moves it at once.
void FontScrollTo(float to, bool glide) {
    fnt.target = std::clamp(to, 0.f, fnt.maxScroll);
    if (!glide) fnt.scroll = fnt.target;
    else SetTimer(u.hwnd, TIMER_SCROLL, 15, nullptr);
}

// Pressing the bar grabs the thumb where it was pressed, or brings the thumb's middle there.
void FontBarDrag(float y, bool start) {
    if (fnt.trackH <= fnt.thumbH || fnt.maxScroll <= 0) return;
    const float thumbTop = fnt.trackTop + (fnt.trackH - fnt.thumbH) * fnt.scroll / fnt.maxScroll;
    if (start) fnt.grab = y >= thumbTop && y <= thumbTop + fnt.thumbH ? y - thumbTop : fnt.thumbH / 2;
    FontScrollTo((y - fnt.grab - fnt.trackTop) / (fnt.trackH - fnt.thumbH) * fnt.maxScroll, false);
}

void ReleaseFontSamples() {
    for (ID2D1Bitmap*& b : fnt.samples) SafeRelease(b);
    const size_t n = fnt.list.size();
    fnt.samples.assign(n, nullptr);
    fnt.sizes.assign(n, D2D1::SizeF(0, 0));
    fnt.tried.assign(n, false);
}

void RefreshFontList() {
    fnt.list = Clock_Fonts();
    ReleaseFontSamples();
}

int FontIndex(const std::wstring& key) {
    for (size_t i = 0; i < fnt.list.size(); i++)
        if (!_wcsicmp(fnt.list[i].key.c_str(), key.c_str())) return (int)i;
    return -1;
}

// Fonts VideoBG can't include that were just downloaded: added from the Downloads folder.
void FindDownloadedFonts() {
    bool added = false;
    for (const ClockFontInfo& f : fnt.list)
        if (f.getUrl && !f.ready && Clock_FindDownloadedFont(f.key)) {
            fnt.note = L"Found " + f.key + L" in your Downloads and added it";
            added = true;
        }
    if (added) RefreshFontList();
}

void OpenFontDialog() {
    fnt = FontDialog{};
    fnt.open = true;
    RefreshFontList();
    FindDownloadedFonts();
}

void CloseFontDialog() {
    if (!fnt.open) return;
    ReleaseFontSamples();
    fnt.open = false;
    Clock_ReleasePainter();  // the samples' fonts
}

void UseFontInLook(const std::wstring& key) {
    EditedLook()->font = key;
    Log(L"ui: clock font %ls", key.c_str());
    ClockEdited();
}

void FontPick(int i) {
    if (i < (int)fnt.list.size() && fnt.list[i].ready) UseFontInLook(fnt.list[i].key);
}

void FontGet(int i) {
    if (i >= (int)fnt.list.size() || !fnt.list[i].getUrl) return;
    ShellExecuteW(u.hwnd, L"open", fnt.list[i].getUrl, nullptr, nullptr, SW_SHOWNORMAL);
    fnt.note = L"Download " + fnt.list[i].key + L", then come back: VideoBG looks for it in your Downloads";
}

// A font file (or a .zip with one) from the file dialog; "" if cancelled.
std::wstring PickFontFile(const std::wstring& title) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return L"";
    const COMDLG_FILTERSPEC types[] = {{L"Fonts, or a .zip with one", L"*.otf;*.ttf;*.zip"}};
    d->SetFileTypes(1, types);
    d->SetTitle(title.c_str());
    d->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    IShellItem* item = nullptr;
    wchar_t* path = nullptr;
    std::wstring file;
    if (SUCCEEDED(d->Show(u.hwnd)) && SUCCEEDED(d->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        file = path;
        CoTaskMemFree(path);
    }
    if (item) item->Release();
    d->Release();
    return file;
}

void FontAddFile(int i) {
    if (i >= (int)fnt.list.size()) return;
    const std::wstring key = fnt.list[i].key;
    const std::wstring file = PickFontFile(L"Choose the " + key + L" font file");
    if (file.empty()) return;
    HRESULT hr = Clock_AddFontFile(key, file);
    fnt.note = SUCCEEDED(hr)        ? L"Added " + key + L": click it to use it"
               : hr == E_INVALIDARG ? L"That file isn't the " + key + L" font"
                                    : std::wstring(L"Couldn't read a font from that file");
    RefreshFontList();
}

// Any font of the user's: kept in VideoBG's fonts folder, and used for this look right away.
void FontAddOwn() {
    const std::wstring file = PickFontFile(L"Choose a font for the clock");
    if (file.empty()) return;
    std::wstring key;
    HRESULT hr = Clock_AddOwnFont(file, &key);
    if (FAILED(hr)) {
        fnt.note = hr == E_INVALIDARG ? L"That file isn't a font VideoBG can use" : L"Couldn't read that file";
        return;
    }
    RefreshFontList();
    UseFontInLook(key);
    fnt.note = hr == S_FALSE ? key + L" is already in the list" : L"Added " + key + L", and the clock uses it now";
    int at = FontIndex(key);
    if (at >= 0) {  // glide to it (the list just got longer: the next paint clamps)
        fnt.target = at * kFontRowH;
        SetTimer(u.hwnd, TIMER_SCROLL, 15, nullptr);
    }
}

// Deletes VideoBG's copy; looks that used it go back to Audiowide (other videos' do when drawn).
void FontRemove(int i) {
    if (i >= (int)fnt.list.size() || !fnt.list[i].removable) return;
    const std::wstring key = fnt.list[i].key;
    if (!Clock_RemoveFont(key)) {
        fnt.note = L"Couldn't remove " + key;
        return;
    }
    for (ClockLook* l : {&g_settings.clockStill, &g_settings.clockLive, &g_settings.clockVideo})
        if (!_wcsicmp(l->font.c_str(), key.c_str())) l->font = ClockLook{}.font;
    ClockEdited();
    RefreshFontList();
    fnt.note = L"Removed " + key;
}

// Draws font i as the clock, just the letters, at exactly the size it's shown (so it stays sharp),
// as big as fits in maxW x maxH DIPs.
void EnsureFontSample(int i, float maxW, float maxH) {
    if (fnt.samples[i] || fnt.tried[i] || !u.rt) return;
    fnt.tried[i] = true;
    if (!fnt.list[i].ready) return;
    ClockLook l;
    l.font = fnt.list[i].key;
    l.h24 = EditedLook()->h24;
    l.color = Fmt(L"%d,%d,%d", (int)lroundf(u.th.text.r * 255), (int)lroundf(u.th.text.g * 255), (int)lroundf(u.th.text.b * 255));
    SYSTEMTIME t;
    GetLocalTime(&t);
    const float k = u.dpi / 96;
    ClockImage img;
    for (float size = 0.8f, pass = 0; pass < 2; pass++) {
        l.size = size;
        if (!Clock_Paint(l, k, t, &img) || img.w <= 0) return;
        float fit = std::min(maxW / (img.w / k), maxH / (img.h / k));
        if (fit >= 1) break;
        size *= fit * 0.99f;  // once more, at the size that fits
    }
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    u.rt->CreateBitmap(D2D1::SizeU((UINT32)img.w, (UINT32)img.h), img.px.data(), (UINT32)img.w * 4, &bp, &fnt.samples[i]);
    fnt.sizes[i] = D2D1::SizeF(img.w / k, img.h / k);
}

void PaintFontDialog() {
    const Theme& t = u.th;
    Fill(R(0, 0, kW, kH), D2D1::ColorF(0, 0, 0, t.dark ? 0.5f : 0.3f));
    AddHit(ID_FNT_SCRIM, K_AREA, R(0, 0, kW, kH));  // the page underneath can't be used meanwhile
    const int n = (int)fnt.list.size();
    const float rowH = kFontRowH, W = kDialogW, content = n * rowH + kFontAddH;
    const float listH = std::min(content, 3.4f * rowH), H = 84 + listH + 8 + 64;
    const float L = (kW - W) / 2, T = std::max(12.f, (kH - H) / 2), k = u.dpi / 96;
    const D2D1_RECT_F panel = R(L, T, L + W, T + H);
    const D2D1_COLOR_F face = t.dark ? Rgb(0x2B2B2B) : Rgb(0xFFFFFF), bar = t.dark ? Rgb(0x202020) : Rgb(0xF3F3F3);
    Fill(R(L - 2, T, L + W + 2, T + H + 4), D2D1::ColorF(0, 0, 0, 0.2f), 10);  // shadow
    Fill(panel, face, 8);
    ID2D1RoundedRectangleGeometry* clip = PushRounded(panel, 8);
    Fill(R(L, T + H - 64, L + W, T + H), bar);
    Line(L, T + H - 64, L + W, T + H - 64, t.stroke);
    PopRounded(clip);
    Stroke(panel, t.dark ? Rgb(0x3C3C3C) : Rgb(0xD5D5D5), 8);

    Text(L"Clock font", R(L + 24, T + 16, L + W - 24, T + 46), u.fSubtitle, t.text);
    Text(L"Click one to use it; your desktop shows it right away.", R(L + 24, T + 50, L + W - 24, T + 72), u.fBody, t.text2);

    const D2D1_RECT_F view = R(L + 1, T + 84, L + W - 1, T + 84 + listH);
    fnt.maxScroll = std::max(0.f, content - listH);
    fnt.scroll = std::clamp(fnt.scroll, 0.f, fnt.maxScroll);
    fnt.target = std::clamp(fnt.target, 0.f, fnt.maxScroll);
    const float barW = fnt.maxScroll > 0 ? 14 : 0;
    const float boxW = W - 32 - barW - 150 - 16, boxH = rowH - 20;  // room for a sample, right of the name
    if (boxW != fnt.boxW) {
        ReleaseFontSamples();
        fnt.boxW = boxW;
    }
    const std::wstring shown = ShownFont(*EditedLook());
    const size_t firstHit = u.hits.size();
    u.rt->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = view.top - fnt.scroll;
    for (int i = 0; i < n; i++, y += rowH) {
        if (y + rowH <= view.top || y >= view.bottom) continue;
        const ClockFontInfo& f = fnt.list[i];
        const bool sel = !_wcsicmp(shown.c_str(), f.key.c_str());
        const D2D1_RECT_F r = R(L + 16, y + 3, L + W - 16 - barW, y + rowH - 3);
        const bool hot = f.ready && IsHot(ID_FNT_ROW0 + i);
        if (sel) {
            Fill(r, Mix(face, t.accent, 0.14f), 6);
            Stroke(r, t.accent, 6);
        } else if (hot) {
            Fill(r, Mix(face, t.text, 0.05f), 6);
        }
        if (f.ready) AddHit(ID_FNT_ROW0 + i, K_BUTTON, r);
        const float cy = (r.top + r.bottom) / 2;
        Circle(r.left + 22, cy, 8, sel ? t.accent : f.ready ? t.text2 : t.text3);
        Circle(r.left + 22, cy, sel ? 3.5f : 6.8f, sel ? t.onAccent : hot ? Mix(face, t.text, 0.05f) : face);
        const float ny = f.removable ? cy - 36 : cy - 22;  // name and what it is; then Remove, for the user's copies
        Text(f.key, R(r.left + 42, ny, r.left + 150, ny + 24), u.fStrong, f.ready ? t.text : t.text2);
        const wchar_t* about = !f.getUrl && !f.removable ? L"Included"
                               : !f.getUrl               ? L"Added by you"
                               : f.ready                 ? L"Your copy"
                                                         : L"Free for personal use only, so not included";
        Text(about, R(r.left + 42, ny + 24, r.left + (f.ready ? 150 : 290), ny + 62), f.ready ? u.fSmall : u.fWrap, t.text2);
        if (f.removable) Button(ID_FNT_DEL0 + i, R(r.left + 38, cy + 18, r.left + 134, cy + 46), L"Remove", L"\uE74D");
        if (f.ready) {
            EnsureFontSample(i, boxW, boxH);
            if (fnt.samples[i]) {  // drawn 1:1 on whole pixels
                const D2D1_SIZE_F sz = fnt.sizes[i];
                const float x0 = roundf((r.right - 16 - sz.width) * k) / k, y0 = roundf((cy - sz.height / 2) * k) / k;
                D2D1_RECT_F dst = R(x0, y0, x0 + sz.width, y0 + sz.height);
                u.rt->DrawBitmap(fnt.samples[i], &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
            }
        } else {
            Button(ID_FNT_ADD0 + i, R(r.right - 12 - 150, cy - 16, r.right - 12, cy + 16), L"Add font file\u2026");
            Button(ID_FNT_GET0 + i, R(r.right - 12 - 150 - 8 - 96, cy - 16, r.right - 12 - 158, cy + 16), L"Get it", L"\uE8A7");
        }
    }
    if (y < view.bottom) {  // the last row: any font of the user's own
        const float cy = y + kFontAddH / 2;
        Button(ID_FNT_OWN, R(L + 24, cy - 18, L + 24 + 200, cy + 18), L"Add your own font\u2026", L"\uE710");
        Text(L"A .ttf or .otf file, or a .zip with one. VideoBG keeps its own copy.", R(L + 24 + 216, cy - 20, L + W - 24 - barW, cy + 20),
             u.fWrap, t.text2);
    }
    u.rt->PopAxisAlignedClip();
    for (size_t h = firstHit; h < u.hits.size(); h++) {  // what's scrolled out of the list can't be clicked
        D2D1_RECT_F& hr = u.hits[h].r;
        hr = R(std::max(hr.left, view.left), std::max(hr.top, view.top), std::min(hr.right, view.right), std::min(hr.bottom, view.bottom));
        if (hr.bottom <= hr.top) hr = R(0, 0, 0, 0);
    }
    if (fnt.maxScroll > 0) {  // the scroll bar: drag its thumb, or press the track to jump there
        fnt.trackTop = view.top + 4;
        fnt.trackH = listH - 8;
        fnt.thumbH = std::max(36.f, fnt.trackH * listH / content);
        const float thumbY = fnt.trackTop + (fnt.trackH - fnt.thumbH) * fnt.scroll / fnt.maxScroll;
        const bool active = IsHot(ID_FNT_BAR) || u.drag == ID_FNT_BAR;
        const float bx = L + W - 13, bw = active ? 8.f : 5.f;
        Fill(R(bx - bw / 2, fnt.trackTop, bx + bw / 2, fnt.trackTop + fnt.trackH), Mix(face, t.text, 0.06f), bw / 2);
        Fill(R(bx - bw / 2, thumbY, bx + bw / 2, thumbY + fnt.thumbH), active ? t.text2 : t.text3, bw / 2);
        AddHit(ID_FNT_BAR, K_AREA, R(L + W - 22, view.top, L + W - 2, view.bottom));
    }

    const float by = T + H - 48;
    Text(fnt.note, R(L + 24, by - 2, L + W - 24 - 130, by + 34), u.fWrap, t.text3);
    Button(ID_FNT_DONE, R(L + W - 24 - 110, by, L + W - 24, by + 32), L"Done", nullptr, true);
}

void PaintMain() {
    PaintHeader();
    PaintSidebar();
    const float x = kContentX, w = kW - 24 - kContentX;
    switch (u.nav) {
        case NAV_VIDEO: PaintVideoPage(x, w); break;
        case NAV_SOUND: PaintSoundPage(x, w); break;
        case NAV_PLAYBACK: PaintPlaybackPage(x, w); break;
        case NAV_POWER: PaintPowerPage(x, w); break;
        case NAV_CLOCK: PaintClockPage(x, w); break;
        case NAV_GENERAL: PaintGeneralPage(x, w); break;
    }
    if (dlg.open) PaintColorDialog();
    if (ver.open) PaintVersionsDialog();
    if (fnt.open) PaintFontDialog();
}

// ---------------------------------------------------------------------------------------
// Crop page

double ScreenAspect() { return u.screenH > 0 ? (double)u.screenW / u.screenH : 16.0 / 9.0; }
double CropK() { return ScreenAspect() / VideoAspect(); }  // normalized width per normalized height

void ClampCrop(Crop& c) {
    float w = c.r - c.l, h = c.b - c.t;
    w = std::min(w, 1.f);
    h = std::min(h, 1.f);
    c.l = std::clamp(c.l, 0.f, 1.f - w);
    c.t = std::clamp(c.t, 0.f, 1.f - h);
    c.r = c.l + w;
    c.b = c.t + h;
}

// Largest rect of the screen's shape inside `c`, centred on it.
Crop FitAspect(Crop c) {
    double k = CropK(), w = c.r - c.l, h = c.b - c.t;
    if (w / h > k) w = h * k;
    else h = w / k;
    double cx = (c.l + c.r) / 2, cy = (c.t + c.b) / 2;
    Crop o{(float)(cx - w / 2), (float)(cy - h / 2), (float)(cx + w / 2), (float)(cy + h / 2)};
    ClampCrop(o);
    return o;
}

bool IsScreenShaped(const Crop& c) {
    double k = (c.r - c.l) / (c.b - c.t);
    return fabs(k / CropK() - 1) < 0.01;
}

D2D1_RECT_F CropToDip(const Crop& c) {
    float w = u.imgRc.right - u.imgRc.left, h = u.imgRc.bottom - u.imgRc.top;
    return R(u.imgRc.left + c.l * w, u.imgRc.top + c.t * h, u.imgRc.left + c.r * w, u.imgRc.top + c.b * h);
}

int HandleAt(float x, float y) {
    // Handles first: at the default (full) crop they sit on the image border, half outside it.
    D2D1_RECT_F r = CropToDip(u.edit);
    const float g = 12;
    auto closeTo = [&](float px, float py) { return fabsf(x - px) < g && fabsf(y - py) < g; };
    if (closeTo(r.left, r.top)) return H_NW;
    if (closeTo(r.right, r.top)) return H_NE;
    if (closeTo(r.left, r.bottom)) return H_SW;
    if (closeTo(r.right, r.bottom)) return H_SE;
    bool inX = x > r.left && x < r.right, inY = y > r.top && y < r.bottom;
    if (inX && fabsf(y - r.top) < 8) return H_N;
    if (inX && fabsf(y - r.bottom) < 8) return H_S;
    if (inY && fabsf(x - r.left) < 8) return H_W;
    if (inY && fabsf(x - r.right) < 8) return H_E;
    if (!In(u.imgRc, x, y)) return H_NONE;
    if (inX && inY) return H_MOVE;
    return H_NEW;
}

void SendPreview(bool force) {
    DWORD now = GetTickCount();
    if (!force && now - u.lastPreview < 33) return;
    u.lastPreview = now;
    Host_PreviewCrop(&u.edit);
}

void DragCrop(float x, float y) {
    float w = u.imgRc.right - u.imgRc.left, h = u.imgRc.bottom - u.imgRc.top;
    float mx = std::clamp((x - u.imgRc.left) / w, 0.f, 1.f), my = std::clamp((y - u.imgRc.top) / h, 0.f, 1.f);
    const Crop& o = u.dragOrig;
    Crop c = o;
    const double k = CropK();
    const float minW = 0.04f, minH = 0.04f;
    auto corner = [&](float ax, float ay) {
        float dx = mx - ax, dy = my - ay;
        float sx = dx >= 0 ? 1.f : -1.f, sy = dy >= 0 ? 1.f : -1.f;
        double cw = fabs(dx), ch = fabs(dy);
        double maxW = sx > 0 ? 1 - ax : ax, maxH = sy > 0 ? 1 - ay : ay;
        if (u.lockAspect) {
            if (cw / k > ch) ch = cw / k;
            else cw = ch * k;
            double s = std::min({1.0, maxW / std::max(cw, 1e-6), maxH / std::max(ch, 1e-6)});
            cw *= s;
            ch *= s;
            if (cw < minW) { cw = minW; ch = cw / k; }
        } else {
            cw = std::clamp(cw, (double)minW, maxW);
            ch = std::clamp(ch, (double)minH, maxH);
        }
        c.l = sx > 0 ? ax : ax - (float)cw;
        c.r = c.l + (float)cw;
        c.t = sy > 0 ? ay : ay - (float)ch;
        c.b = c.t + (float)ch;
    };
    auto edgeX = [&](bool right) {  // dragging the left/right edge
        double cw = right ? mx - o.l : o.r - mx;
        cw = std::clamp(cw, (double)minW, right ? 1.0 - o.l : (double)o.r);
        if (u.lockAspect) {
            double ch = cw / k;
            if (ch > 1) { ch = 1; cw = k; }
            double cy = (o.t + o.b) / 2;
            c.t = (float)std::clamp(cy - ch / 2, 0.0, 1 - ch);
            c.b = c.t + (float)ch;
        }
        if (right) c.r = o.l + (float)cw;
        else c.l = o.r - (float)cw;
    };
    auto edgeY = [&](bool bottom) {
        double ch = bottom ? my - o.t : o.b - my;
        ch = std::clamp(ch, (double)minH, bottom ? 1.0 - o.t : (double)o.b);
        if (u.lockAspect) {
            double cw = ch * k;
            if (cw > 1) { cw = 1; ch = 1 / k; }
            double cx = (o.l + o.r) / 2;
            c.l = (float)std::clamp(cx - cw / 2, 0.0, 1 - cw);
            c.r = c.l + (float)cw;
        }
        if (bottom) c.b = o.t + (float)ch;
        else c.t = o.b - (float)ch;
    };
    float sx0 = std::clamp((u.dragStart.x - u.imgRc.left) / w, 0.f, 1.f);
    float sy0 = std::clamp((u.dragStart.y - u.imgRc.top) / h, 0.f, 1.f);
    switch (u.handle) {
        case H_NW: corner(o.r, o.b); break;
        case H_NE: corner(o.l, o.b); break;
        case H_SW: corner(o.r, o.t); break;
        case H_SE: corner(o.l, o.t); break;
        case H_E: edgeX(true); break;
        case H_W: edgeX(false); break;
        case H_S: edgeY(true); break;
        case H_N: edgeY(false); break;
        case H_MOVE: {
            float dx = mx - sx0, dy = my - sy0;
            c = {o.l + dx, o.t + dy, o.r + dx, o.b + dy};
            ClampCrop(c);
            break;
        }
        case H_NEW:
            if (fabsf(mx - sx0) < 0.01f && fabsf(my - sy0) < 0.01f) return;
            corner(sx0, sy0);
            break;
        default: return;
    }
    u.edit = c;
    SendPreview(false);
}

void ZoomCrop(float factor) {
    Crop c = u.edit;
    double w = (c.r - c.l) * factor, h = (c.b - c.t) * factor;
    if (w > 1 || h > 1) {
        double s = std::min(1 / w, 1 / h);
        w *= s;
        h *= s;
    }
    if (w < 0.04 || h < 0.04) return;
    double cx = (c.l + c.r) / 2, cy = (c.t + c.b) / 2;
    c = {(float)(cx - w / 2), (float)(cy - h / 2), (float)(cx + w / 2), (float)(cy + h / 2)};
    ClampCrop(c);
    u.edit = c;
    SendPreview(true);
    InvalidateRect(u.hwnd, nullptr, FALSE);
}

void PaintCrop() {
    const Theme& t = u.th;
    Text(L"Crop", R(24, 14, 300, 42), u.fTitle, t.text);
    {
        int pw = (int)lround((u.edit.r - u.edit.l) * (u.infoOk ? u.info.w : u.fw));
        int ph = (int)lround((u.edit.b - u.edit.t) * (u.infoOk ? u.info.h : u.fh));
        Text(Fmt(L"Showing %d \u00D7 %d px of the video", pw, ph), R(kW - 324, 14, kW - 24, 42), u.fBody, t.text2,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    Text(L"Choose the part of the video that fills your desktop. Drag to move, drag edges or corners to resize, scroll to zoom. "
         L"Your wallpaper updates live.",
         R(24, 42, kW - 24, 60), u.fSmall, t.text2);
    D2D1_RECT_F area = R(24, 70, kW - 24, kH - 92);
    Fill(area, t.dark ? Rgb(0x141414) : Rgb(0xE6E6E6), 8);
    double va = VideoAspect();
    float aw = area.right - area.left - 24, ah = area.bottom - area.top - 24;
    float iw = aw, ih = (float)(aw / va);
    if (ih > ah) { ih = ah; iw = (float)(ah * va); }
    float ix = (area.left + area.right - iw) / 2, iy = (area.top + area.bottom - ih) / 2;
    u.imgRc = R(ix, iy, ix + iw, iy + ih);
    EnsureFrameBitmap();
    if (u.frame) {
        D2D1_RECT_F src = R(0, 0, (float)u.fw, (float)u.fh);
        if (u.dc) u.dc->DrawBitmap(u.frame, &u.imgRc, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src, nullptr);
        else u.rt->DrawBitmap(u.frame, &u.imgRc, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
    } else {
        Fill(u.imgRc, D2D1::ColorF(0, 0, 0));
        Text(u.frameFailed ? L"No preview for this file" : L"Loading\u2026", u.imgRc, u.fBody, D2D1::ColorF(0.8f, 0.8f, 0.8f),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    D2D1_RECT_F c = CropToDip(u.edit);
    D2D1_COLOR_F shade = D2D1::ColorF(0, 0, 0, 0.6f);
    Fill(R(u.imgRc.left, u.imgRc.top, u.imgRc.right, c.top), shade);
    Fill(R(u.imgRc.left, c.bottom, u.imgRc.right, u.imgRc.bottom), shade);
    Fill(R(u.imgRc.left, c.top, c.left, c.bottom), shade);
    Fill(R(c.right, c.top, u.imgRc.right, c.bottom), shade);
    D2D1_COLOR_F thirds = D2D1::ColorF(1, 1, 1, 0.35f);
    for (int i = 1; i < 3; i++) {
        float x = c.left + (c.right - c.left) * i / 3, yv = c.top + (c.bottom - c.top) * i / 3;
        Line(x, c.top, x, c.bottom, thirds);
        Line(c.left, yv, c.right, yv, thirds);
    }
    Stroke(R(c.left - 1, c.top - 1, c.right + 1, c.bottom + 1), D2D1::ColorF(0, 0, 0, 0.5f), 0, 1);
    Stroke(c, D2D1::ColorF(1, 1, 1), 0, 2);
    const float hs = 5.5f;
    float mxp = (c.left + c.right) / 2, myp = (c.top + c.bottom) / 2;
    D2D1_POINT_2F pts[8] = {{c.left, c.top}, {c.right, c.top}, {c.left, c.bottom}, {c.right, c.bottom},
                            {mxp, c.top},    {mxp, c.bottom},  {c.left, myp},     {c.right, myp}};
    for (auto& p : pts) {
        Circle(p.x, p.y, hs + 1.5f, D2D1::ColorF(0, 0, 0, 0.45f));
        Circle(p.x, p.y, hs, D2D1::ColorF(1, 1, 1));
    }
    AddHit(ID_C_AREA, K_AREA, area);

    // Bottom bar
    float by = kH - 72;
    Toggle(ID_C_LOCK, 28, by + 20, u.lockAspect);
    Text(L"Match screen shape", R(76, by, 300, by + 40), u.fBody, t.text);
    float bx = kW - 24;
    Button(ID_C_APPLY, R(bx - 96, by + 4, bx, by + 36), L"Apply", nullptr, true);
    Button(ID_C_CANCEL, R(bx - 96 - 8 - 88, by + 4, bx - 104, by + 36), L"Cancel");
    Button(ID_C_RESET, R(bx - 96 - 8 - 88 - 8 - 88, by + 4, bx - 200, by + 36), L"Reset");
}

void EnterCrop() {
    u.page = 1;
    u.edit = g_settings.crop;
    u.lockAspect = IsScreenShaped(u.edit) || u.edit.IsFull();
    if (u.lockAspect && !IsScreenShaped(u.edit)) u.edit = FitAspect(u.edit);
    SendPreview(true);
    InvalidateRect(u.hwnd, nullptr, FALSE);
}

void LeaveCrop(bool apply) {
    if (apply) {
        g_settings.crop = u.edit;
        Host_SettingsChanged();
    }
    Host_PreviewCrop(nullptr);
    u.page = 0;
    u.handle = H_NONE;
    InvalidateRect(u.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------------------
// Desktop clock page: interaction

void ShowClockPage() {
    u.nav = NAV_CLOCK;
    u.clockEdit = Host_ClockLive() ? 0 : 1;
    if (u.stillPx.empty() && !u.stillLoading) {
        u.stillLoading = true;
        Job j;
        j.kind = 5;
        u.worker.Post(j);
    }
}

// Saves an edit of the clock look; the host moves/recolours the clock if that look is showing.
void ClockEdited() {
    SafeRelease(u.clockBmp);
    Host_SettingsChanged();
}

// The desktop clock follows edits of the look it shows before they're saved (at most 25 times a second).
void LiveClock(bool force = false) {
    DWORD now = GetTickCount();
    if (!EditingShownLook() || (!force && now - u.lastClockMove < 40)) return;
    u.lastClockMove = now;
    Host_ClockMove(*EditedLook());
}

void ClockDrag(float mx, float my, bool start) {
    const D2D1_RECT_F& b = u.clockBox;
    float bw = b.right - b.left, bh = b.bottom - b.top;
    if (bw <= 0 || bh <= 0) return;
    ClockLook* look = EditedLook();
    if (start) {
        D2D1_RECT_F cr = ClockRectIn(b, *look);
        // Grab the clock where it was clicked; clicking elsewhere brings its centre there.
        u.clockGrab = In(cr, mx, my) ? D2D1::Point2F(mx - (cr.left + cr.right) / 2, my - (cr.top + cr.bottom) / 2)
                                     : D2D1::Point2F(0, 0);
    }
    float fw, fh;
    ClockSize(&fw, &fh);
    float cx = (mx - u.clockGrab.x - b.left) / bw, cy = (my - u.clockGrab.y - b.top) / bh;
    cx = fw < 1 ? std::clamp(cx, fw / 2, 1 - fw / 2) : 0.5f;
    cy = fh < 1 ? std::clamp(cy, fh / 2, 1 - fh / 2) : 0.5f;
    u.snapX = u.snapY = false;
    if (GetKeyState(VK_MENU) >= 0) {  // hold Alt to place freely
        if (fabsf(cx - 0.5f) * bw < 6) { cx = 0.5f; u.snapX = true; }
        for (float s : {0.25f, 0.5f, 0.75f})
            if (fabsf(cy - s) * bh < 6) { cy = s; u.snapY = true; u.snapYv = s; }
    }
    look->x = cx;
    look->y = cy;
    LiveClock();  // the real clock follows the drag
}

void ClockNudge(UINT vk) {
    ClockLook* look = EditedLook();
    float step = (GetKeyState(VK_SHIFT) < 0 ? 10.f : 1.f);
    float fw, fh;
    ClockSize(&fw, &fh);
    if (vk == VK_LEFT || vk == VK_RIGHT) look->x += (vk == VK_LEFT ? -step : step) / std::max(1, u.screenW);
    else look->y += (vk == VK_UP ? -step : step) / std::max(1, u.screenH);
    look->x = std::clamp(look->x, std::min(0.5f, fw / 2), std::max(0.5f, 1 - fw / 2));
    look->y = std::clamp(look->y, std::min(0.5f, fh / 2), std::max(0.5f, 1 - fh / 2));
    ClockEdited();
}

std::wstring ClipboardText() {
    std::wstring s;
    if (!OpenClipboard(u.hwnd)) return s;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = (const wchar_t*)GlobalLock(h)) {
            s.assign(p, wcsnlen(p, 256));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return s;
}

void SetClipboardText(const std::wstring& s) {
    if (!OpenClipboard(u.hwnd)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(g), s.c_str(), bytes);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

void ClockNote(const std::wstring& m) {
    u.clockNote = m;
    u.clockNoteAt = GetTickCount();
}

// Ctrl+C / Ctrl+V on the clock page: copy the clock colour as a hex code, or paste one.
void ClockClipboard(bool paste) {
    ClockLook* look = EditedLook();
    if (!paste) {
        SetClipboardText(Hex(look->color));
        ClockNote(L"Copied " + Hex(look->color));
        return;
    }
    BYTE r, g, b;
    if (!ParseColor(ClipboardText(), &r, &g, &b)) {
        ClockNote(L"The clipboard has no colour code (like #72ABDE or 114,171,222)");
        return;
    }
    look->color = Fmt(L"%d,%d,%d", r, g, b);
    Log(L"ui: clock colour pasted: %ls", look->color.c_str());
    ClockNote(L"Pasted " + Hex(look->color));
    ClockEdited();
}

// ---- Colour dialog ----------------------------------------------------------------------------

const Hit* HitAt(float x, float y);

void DlgNote(const std::wstring& m, bool warn = false) {
    dlg.msg = m;
    dlg.msgAt = GetTickCount();
    dlg.msgWarn = warn;
}

// Puts the dialog's colour on the look (saved on Done) and the desktop.
void DlgApply(bool force) {
    DlgTarget() = dlg.target == 1 && dlg.sameAsText ? std::wstring() : DlgColor();
    LiveClock(force);
}

void DlgSetRgb(BYTE r, BYTE g, BYTE b, bool typing = false) {
    RgbToHsv(r, g, b, &dlg.h, &dlg.s, &dlg.v);
    dlg.exact = Fmt(L"%d,%d,%d", r, g, b);
    dlg.sameAsText = false;
    if (!typing) dlg.hexEdit = false;
    DlgApply(true);
}

void OpenColorDialog(int target) {
    ClockLook* look = EditedLook();
    int slot = dlg.slot;
    dlg = ColorDialog{};
    dlg.open = true;
    dlg.slot = slot;
    dlg.target = target;
    dlg.before = target == 0 ? look->color : look->glowColor;
    dlg.sameAsText = target == 1 && look->glowColor.empty();
    BYTE r = 255, g = 255, b = 255;
    ParseColor(dlg.sameAsText || target == 0 ? look->color : look->glowColor, &r, &g, &b);
    RgbToHsv(r, g, b, &dlg.h, &dlg.s, &dlg.v);
    dlg.exact = Fmt(L"%d,%d,%d", r, g, b);
    u.picking = false;
    u.pickColor.clear();
}

void CloseColorDialog(bool keep) {
    if (!keep) DlgTarget() = dlg.before;
    else if (DlgTarget() != dlg.before)
        Log(L"ui: clock %ls colour set to %ls", dlg.target ? L"glow" : L"text", DlgTarget().empty() ? L"the text's" : DlgTarget().c_str());
    dlg.open = dlg.picking = dlg.hexEdit = false;
    u.pickColor.clear();
    ClockEdited();  // saves, and the desktop shows the kept (or the old) colour
}

void DlgSameAsText(bool on) {
    dlg.sameAsText = on;
    if (on) {
        BYTE r = 255, g = 255, b = 255;
        ParseColor(EditedLook()->color, &r, &g, &b);
        RgbToHsv(r, g, b, &dlg.h, &dlg.s, &dlg.v);
        dlg.exact = Fmt(L"%d,%d,%d", r, g, b);
    }
    DlgApply(true);
}

void DlgDrag(int id, float x, float y) {
    if (id == ID_DLG_SV) {
        const D2D1_RECT_F& r = dlg.sv;
        dlg.s = std::clamp((x - r.left) / (r.right - r.left), 0.f, 1.f);
        dlg.v = 1 - std::clamp((y - r.top) / (r.bottom - r.top), 0.f, 1.f);
    } else {
        const D2D1_RECT_F& r = dlg.hue;
        dlg.h = std::clamp((x - r.left) / (r.right - r.left), 0.f, 1.f) * 359.9f;
    }
    dlg.exact.clear();
    dlg.sameAsText = dlg.hexEdit = false;
    DlgApply(false);
}

// A typed hex digit: the code applies as soon as it's complete (3 or 6 digits).
void DlgHexChar(wchar_t c) {
    if (!dlg.hexEdit || dlg.hexFresh) {
        dlg.hex.clear();
        dlg.hexEdit = true;
        dlg.hexFresh = false;
    }
    if (c == L'#') return;
    if (dlg.hex.size() < 6) dlg.hex += (wchar_t)towupper(c);
    BYTE r, g, b;
    if ((dlg.hex.size() == 3 || dlg.hex.size() == 6) && ParseColor(dlg.hex, &r, &g, &b)) DlgSetRgb(r, g, b, true);
}

void DlgCopy() {
    SetClipboardText(Hex(DlgColor()));
    DlgNote(L"Copied " + Hex(DlgColor()));
}

void DlgPaste() {
    BYTE r, g, b;
    if (!ParseColor(ClipboardText(), &r, &g, &b)) {
        DlgNote(L"No colour code on the clipboard", true);
        return;
    }
    DlgSetRgb(r, g, b);
    DlgNote(L"Pasted " + Hex(DlgColor()));
}

void DlgSaveSlot() {
    int i = dlg.slot;
    for (int k = 0; i < 0 && k < 16; k++)  // no box chosen: the first empty one
        if (g_settings.myColors[k].empty()) i = k;
    if (i < 0) {
        DlgNote(L"All boxes are full: choose one to replace", true);
        return;
    }
    dlg.slot = i;
    g_settings.myColors[i] = DlgColor();
    SaveMyColors(g_settings);
    DlgNote(Fmt(L"Saved in box %d", i + 1));
}

void DlgUseSlot(int i) {
    BYTE r, g, b;
    if (i < 0 || i >= 16 || !ParseColor(g_settings.myColors[i], &r, &g, &b)) return;
    dlg.slot = i;
    DlgSetRgb(r, g, b);
}

void DlgClearSlot() {
    if (dlg.slot < 0 || g_settings.myColors[dlg.slot].empty()) return;
    g_settings.myColors[dlg.slot].clear();
    SaveMyColors(g_settings);
    DlgNote(Fmt(L"Box %d emptied", dlg.slot + 1));
}

// Keys while the dialog is open; nothing reaches the page underneath.
bool DlgKey(UINT vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    switch (vk) {
        case VK_ESCAPE:
            if (dlg.picking) {
                dlg.picking = false;
                u.pickColor.clear();
            } else if (dlg.hexEdit) {
                dlg.hexEdit = false;
            } else {
                CloseColorDialog(false);
            }
            break;
        case VK_RETURN:
            if (dlg.hexEdit) dlg.hexEdit = false;
            else CloseColorDialog(true);
            break;
        case VK_BACK:
            if (!dlg.hexEdit) return true;
            if (dlg.hexFresh) dlg.hex.clear();
            else if (!dlg.hex.empty()) dlg.hex.pop_back();
            dlg.hexFresh = false;
            break;
        case VK_DELETE: DlgClearSlot(); break;
        case 'V':
            if (!ctrl) return true;
            DlgPaste();
            break;
        case 'C':
            if (!ctrl) return true;
            DlgCopy();
            break;
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_TAB: return true;
        default: return false;
    }
    InvalidateRect(u.hwnd, nullptr, FALSE);
    return true;
}

bool DlgDoubleClick(float x, float y) {
    const Hit* ht = dlg.open ? HitAt(x, y) : nullptr;
    if (!ht || ht->id < ID_DLG_SLOT0 || ht->id >= ID_DLG_SLOT0 + 16) return false;
    DlgUseSlot(ht->id - ID_DLG_SLOT0);
    InvalidateRect(u.hwnd, nullptr, FALSE);
    return true;
}

// A click while the dialog is open; true when it's dealt with here.
bool DlgMouseDown(const Hit* ht, float x, float y) {
    if (!ht || ht->id != ID_DLG_HEX) dlg.hexEdit = false;
    if (dlg.picking && ht && ht->id == ID_DLG_PREVIEW) {  // eyedropper: take the colour and stop
        std::wstring pc;
        BYTE r, g, b;
        if (SampleWallpaper(dlg.previewScreen, x, y, &pc) && ParseColor(pc, &r, &g, &b)) DlgSetRgb(r, g, b);
        dlg.picking = false;
        u.pickColor.clear();
        return true;
    }
    if (dlg.picking && (!ht || ht->id != ID_DLG_PICK)) {
        dlg.picking = false;
        u.pickColor.clear();
    }
    return false;
}

// ---------------------------------------------------------------------------------------

void Paint() {
    if (!EnsureTarget()) return;
    u.hits.clear();
    u.rt->BeginDraw();
    u.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    D2D1_COLOR_F bg = u.th.bg;
    u.rt->Clear(&bg);
    if (u.page == 1) PaintCrop();
    else PaintMain();
    if (u.rt->EndDraw() == D2DERR_RECREATE_TARGET) ReleaseTarget();
}

const Hit* HitAt(float x, float y) {
    for (auto it = u.hits.rbegin(); it != u.hits.rend(); ++it)
        if (In(it->r, x, y)) return &*it;
    return nullptr;
}

int SegAt(const Hit& h, float x) {
    for (size_t i = 0; i + 1 < h.segs.size(); i++)
        if (x >= h.segs[i] && x < h.segs[i + 1]) return (int)i;
    return -1;
}

void SetPreviewTimeFromFrac(float frac) {
    if (!u.infoOk || u.info.duration <= 0) return;
    g_settings.previewTime = frac * u.info.duration;
    RequestFrame();
}

void OnSlider(int id, float frac, bool final) {
    switch (id) {
        case ID_SPEED: {
            int v = 25 + (int)lround(frac * 175 / 5) * 5;  // 5% steps
            if (std::abs(v - 100) <= 5) v = 100;            // snap to normal speed
            g_settings.speed = v;
            if (final) Host_SettingsChanged();
            break;
        }
        case ID_VOLUME:
            g_settings.volume = (int)lround(frac * 100);
            if (final) Host_SettingsChanged();
            break;
        case ID_TIMELINE:
            SetPreviewTimeFromFrac(frac);
            if (final) Host_FrameChanged();  // this video's frame: saved, and the lock screen follows
            break;
        case ID_CLK_SIZE:
        case ID_CLK_OPACITY:
        case ID_CLK_GLOW:
        case ID_CLK_GLOWSIZE: {
            ClockLook* look = EditedLook();
            if (id == ID_CLK_SIZE) look->size = roundf((0.5f + frac * 1.5f) * 100) / 100;
            if (id == ID_CLK_OPACITY) look->opacity = roundf((0.2f + frac * 0.8f) * 20) / 20;
            if (id == ID_CLK_GLOW) look->glow = roundf(frac * 20) / 20;
            if (id == ID_CLK_GLOWSIZE) look->glowSize = roundf(2 + frac * 38);
            if (final) ClockEdited();
            else LiveClock();  // the desktop clock follows the slider
            break;
        }
    }
}

// Arrow keys on the slider clicked last: one smallest step at a time, shown at once and saved
// when the key goes up.
bool StepSlider(int dir) {
    switch (u.keySlider) {
        case ID_SPEED: g_settings.speed = std::clamp(g_settings.speed + 5 * dir, 25, 200); break;
        case ID_VOLUME:
            if (g_settings.sound == 0) return false;
            g_settings.volume = std::clamp(g_settings.volume + dir, 0, 100);
            break;
        case ID_TIMELINE: {
            if (!u.infoOk || u.info.duration <= 0) return false;
            double step = u.info.fps > 0 ? 1 / u.info.fps : 0.1;  // one frame
            g_settings.previewTime = std::clamp(PreviewTime() + dir * step, 0.0, std::max(0.0, u.info.duration - step));
            RequestFrame();
            break;
        }
        case ID_CLK_SIZE:
        case ID_CLK_OPACITY:
        case ID_CLK_GLOW:
        case ID_CLK_GLOWSIZE: {
            ClockLook* look = EditedLook();
            if (!ClockUsable() || (u.keySlider == ID_CLK_GLOWSIZE && look->glow <= 0)) return false;
            if (u.keySlider == ID_CLK_SIZE) look->size = std::clamp(roundf(look->size * 100 + dir) / 100, 0.5f, 2.0f);
            if (u.keySlider == ID_CLK_OPACITY) look->opacity = std::clamp(roundf(look->opacity * 20 + dir) / 20, 0.2f, 1.0f);
            if (u.keySlider == ID_CLK_GLOW) look->glow = std::clamp(roundf(look->glow * 20 + dir) / 20, 0.0f, 1.0f);
            if (u.keySlider == ID_CLK_GLOWSIZE) look->glowSize = std::clamp(look->glowSize + dir, 2.0f, 40.0f);
            LiveClock(true);
            break;
        }
        default: return false;
    }
    u.keyStepped = true;
    return true;
}

void FinishStep() {
    if (!u.keyStepped) return;
    u.keyStepped = false;
    switch (u.keySlider) {
        case ID_SPEED:
        case ID_VOLUME: Host_SettingsChanged(); break;
        case ID_TIMELINE: Host_FrameChanged(); break;
        default: ClockEdited();
    }
}

void OnSeg(int id, int i) {
    switch (id) {
        case ID_SCALE: g_settings.scale = i; break;
        case ID_FPS: g_settings.fpsCap = kFpsValues[i]; break;
        case ID_MONITORS: g_settings.monitors = i; break;
        case ID_COVER: g_settings.pauseCover = i; break;
        case ID_SOUND:
            if (i == g_settings.sound) return;
            if (i == 2 && u.playlist.empty()) {  // "my music" needs songs first
                if (Host_PickMusic(u.hwnd)) u.playlist = LoadPlaylist();
                return;
            }
            g_settings.sound = i;
            if (i != 0 && g_settings.volume == 0) g_settings.volume = 60;
            Log(L"ui: sound set to %ls", i == 0 ? L"nothing" : i == 1 ? L"video's audio" : L"my music");
            break;
        case ID_BATTERY: g_settings.onBattery = i; break;
        case ID_LAUNCH: g_settings.launch = i; break;
        case ID_CLK_MODE:
            u.clockEdit = i;
            return;
        case ID_CLK_SCOPE:
            if ((i == 0) == g_settings.clockVideoOwn) return;
            g_settings.clockVideoOwn = i == 0;
            if (g_settings.clockVideoOwn) {
                // Back to the look it had before, or start from the shared one the first time.
                if (!g_settings.clockVideoSaved) g_settings.clockVideo = g_settings.clockLive;
                g_settings.clockVideoSaved = true;
            }
            Log(L"ui: clock look for the video wallpaper: %ls", g_settings.clockVideoOwn ? L"this video's own" : L"shared");
            ClockEdited();
            return;
        case ID_CLK_SHOW:
            EditedLook()->show = i == 0;
            Log(L"ui: desktop clock %ls with the %ls look", i == 0 ? L"shown" : L"hidden",
                u.clockEdit == 1 ? L"still" : (g_settings.clockVideoOwn ? L"video's own" : L"shared video"));
            ClockEdited();
            return;
        case ID_CLK_HOURS:
            EditedLook()->h24 = i == 1;
            ClockEdited();
            return;
        default: return;
    }
    Host_SettingsChanged();
}

void StopRecording();

void OnClick(int id) {
    if (id >= ID_NAV && id < ID_NAV + (int)NAV_COUNT) {
        StopRecording();
        u.picking = false;
        if (id - ID_NAV == NAV_CLOCK) ShowClockPage();
        else u.nav = id - ID_NAV;
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
    if (id >= ID_VER_ROW0 && id < ID_VER_DEL0) {
        if (id - ID_VER_ROW0 < (int)ver.list.size()) VerSelect(id - ID_VER_ROW0);
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
    if (id >= ID_VER_DEL0 && id < ID_VER_DEL0 + 50) {
        VerDelete(id - ID_VER_DEL0);
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
    if (id >= ID_DLG_SLOT0 && id < ID_DLG_SLOT0 + 16) {
        dlg.slot = id - ID_DLG_SLOT0;
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
    if (id >= ID_FNT_ROW0 && id < ID_FNT_DEL0 + 100) {
        int i = (id - ID_FNT_ROW0) % 100;
        if (id < ID_FNT_GET0) FontPick(i);
        else if (id < ID_FNT_ADD0) FontGet(i);
        else if (id < ID_FNT_DEL0) FontAddFile(i);
        else FontRemove(i);
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
    switch (id) {
        case ID_POWER: Host_SetOn(!Host_IsOn()); break;
        case ID_CLK_CUSTOM: OpenColorDialog(0); break;
        case ID_CLK_GLOWCOLOR: OpenColorDialog(1); break;
        case ID_CLK_EYEDROP:
            u.picking = !u.picking;
            u.pickColor.clear();
            break;
        case ID_DLG_OK: CloseColorDialog(true); break;
        case ID_DLG_CANCEL: CloseColorDialog(false); break;
        case ID_DLG_SAME: DlgSameAsText(!dlg.sameAsText); break;
        case ID_DLG_HEX:
            dlg.hexEdit = true;
            dlg.hexFresh = true;  // typing replaces it
            dlg.hex = Hex(DlgColor()).substr(1);
            break;
        case ID_DLG_COPY: DlgCopy(); break;
        case ID_DLG_PICK:
            dlg.picking = !dlg.picking;
            u.pickColor.clear();
            break;
        case ID_DLG_SAVE: DlgSaveSlot(); break;
        case ID_DLG_USE: DlgUseSlot(dlg.slot); break;
        case ID_OPEN_DATA: ShellExecuteW(u.hwnd, L"open", AppDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_OPEN_LOCAL: ShellExecuteW(u.hwnd, L"open", LocalDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_CHOOSE:
            if (Host_PickVideo(u.hwnd)) {
                u.optMsg.clear();
                u.frameFailed = false;
                u.fault = VF_NONE;
                RequestFrame();
            }
            break;
        case ID_FIX: {
            FaultText buf;
            if (const FaultText* f = CurrentFault(&buf)) {
                if (f->link == L"gif:") StartGifConvert(g_settings.video);
                else if (!f->link.empty()) OpenFixLink(u.hwnd, f->link);
            }
            break;
        }
        case ID_CROP: EnterCrop(); break;
        case ID_MUSIC_PICK:
            if (Host_PickMusic(u.hwnd)) u.playlist = LoadPlaylist();
            break;
        case ID_SHUFFLE:
            g_settings.shuffle = !g_settings.shuffle;
            Host_SettingsChanged();
            break;
        case ID_KEEPSOUND:
            g_settings.keepSound = !g_settings.keepSound;
            Host_SettingsChanged();
            break;
        case ID_VERSIONS: OpenVersions(); break;
        case ID_VER_CANCEL: CloseVersions(); break;
        case ID_CLK_FONT: OpenFontDialog(); break;
        case ID_FNT_DONE: CloseFontDialog(); break;
        case ID_FNT_OWN: FontAddOwn(); break;
        case ID_VER_USE: VerApply(); break;
        case ID_LOCK: Host_SetLockFollow(!g_settings.lockFollow); break;
        case ID_HOTKEY:
            u.recording = !u.recording;
            u.hotkeyMsg.clear();
            Host_SuspendHotkey(u.recording);
            SetFocus(u.hwnd);
            break;
        case ID_STARTUP_LINK: ShellExecuteW(u.hwnd, L"open", L"ms-settings:startupapps", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_STARTUP_REG: Host_RegisterStartup(); break;
        case ID_C_LOCK:
            u.lockAspect = !u.lockAspect;
            if (u.lockAspect) { u.edit = FitAspect(u.edit); SendPreview(true); }
            break;
        case ID_C_RESET:
            u.edit = Crop{};
            if (u.lockAspect) u.edit = FitAspect(u.edit);
            SendPreview(true);
            break;
        case ID_C_CANCEL: LeaveCrop(false); break;
        case ID_C_APPLY: LeaveCrop(true); break;
    }
    InvalidateRect(u.hwnd, nullptr, FALSE);
}

void StopRecording() {
    if (!u.recording) return;
    u.recording = false;
    Host_SuspendHotkey(false);
}

bool OnKey(UINT vk) {
    if (u.recording) {
        if (vk == VK_ESCAPE) { StopRecording(); InvalidateRect(u.hwnd, nullptr, FALSE); return true; }
        if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN || vk == VK_LCONTROL ||
            vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LSHIFT || vk == VK_RSHIFT)
            return true;
        UINT mods = (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) | (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) |
                    (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0) |
                    ((GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) ? MOD_WIN : 0);
        if (!(mods & (MOD_CONTROL | MOD_ALT | MOD_WIN))) {
            u.hotkeyMsg = L"Use Ctrl, Alt or Win together with a key";
        } else {
            u.recording = false;  // Host_SetHotkey registers the new combination itself
            if (Host_SetHotkey(mods, vk)) u.hotkeyMsg.clear();
            else u.hotkeyMsg = L"That shortcut is already used by another app";
        }
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (dlg.open) return DlgKey(vk);
    if (ver.open) {
        int n = (int)ver.list.size();
        if (vk == VK_ESCAPE) CloseVersions();
        else if (vk == VK_RETURN) VerApply();
        else if ((vk == VK_UP || vk == VK_DOWN) && n) VerSelect(std::clamp(ver.sel + (vk == VK_DOWN ? 1 : -1), 0, n - 1));
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (fnt.open) {
        if (vk == VK_ESCAPE || vk == VK_RETURN) CloseFontDialog();
        if (vk == VK_UP || vk == VK_DOWN) FontScrollTo(fnt.target + (vk == VK_DOWN ? 80.f : -80.f), true);
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (u.keySlider && u.page == 0 && (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN)) {
        if (StepSlider(vk == VK_RIGHT || vk == VK_UP ? 1 : -1)) InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (vk == VK_ESCAPE && u.keySlider) {
        FinishStep();
        u.keySlider = 0;
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (vk == VK_ESCAPE && u.picking) {
        u.picking = false;
        u.pickColor.clear();
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (vk == VK_ESCAPE) {
        if (u.page == 1) LeaveCrop(false);
        else DestroyWindow(u.hwnd);
        return true;
    }
    if (u.page == 0 && u.nav == NAV_CLOCK && ClockUsable() && GetKeyState(VK_CONTROL) < 0 && (vk == 'V' || vk == 'C')) {
        ClockClipboard(vk == 'V');
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (u.page == 0 && u.nav == NAV_CLOCK && ClockUsable() && !u.drag &&
        (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN)) {
        ClockNudge(vk);
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    return false;
}

float Dip(int px) { return px * 96.0f / u.dpi; }

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            Paint();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (u.rt) u.rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
            return 0;
        case WM_DPICHANGED: {
            u.dpi = (float)HIWORD(w);
            RECT* r = (RECT*)l;
            ReleaseTarget();
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (l && !wcscmp((const wchar_t*)l, L"ImmersiveColorSet")) {
                LoadTheme();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE: {
            float x = Dip(GET_X_LPARAM(l)), y = Dip(GET_Y_LPARAM(l));
            if (!u.tracking) {
                TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                u.tracking = true;
            }
            if (u.drag) {
                for (auto& ht : u.hits)
                    if (ht.id == u.drag) {
                        if (ht.kind == K_SLIDER) OnSlider(ht.id, SliderFrac(ht, x), false);
                        else if (ht.id == ID_C_AREA) DragCrop(x, y);
                        else if (ht.id == ID_CLK_AREA) ClockDrag(x, y, false);
                        else if (ht.id == ID_FNT_BAR) FontBarDrag(y, false);
                        else if (ht.id == ID_DLG_SV || ht.id == ID_DLG_HUE) DlgDrag(ht.id, x, y);
                        break;
                    }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            const Hit* ht = HitAt(x, y);
            int hot = ht && ht->enabled ? ht->id : 0;
            int seg = ht && ht->kind == K_SEG ? SegAt(*ht, x) : -1;
            if (u.picking || (dlg.open && dlg.picking)) {  // the colour under an eyedropper
                std::wstring pc;
                bool ok = u.picking ? SampleWallpaper(u.clockBox, x, y, &pc)
                                    : In(dlg.preview, x, y) && SampleWallpaper(dlg.previewScreen, x, y, &pc);
                if (!ok) pc.clear();
                if (pc != u.pickColor || u.pickAt.x != x || u.pickAt.y != y) {
                    u.pickColor = pc;
                    u.pickAt = D2D1::Point2F(x, y);
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
            bool overClock = hot == ID_CLK_AREA && In(ClockRectIn(u.clockBox, *EditedLook()), x, y);
            if (hot != u.hot || seg != u.hotSeg || overClock != u.overClock) {
                u.hot = hot;
                u.hotSeg = seg;
                u.overClock = overClock;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            u.tracking = false;
            if (u.hot && !u.drag) { u.hot = 0; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT && u.page == 1) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(h, &p);
                int hd = u.drag ? u.handle : HandleAt(Dip(p.x), Dip(p.y));
                LPCWSTR cur = IDC_ARROW;
                switch (hd) {
                    case H_NW: case H_SE: cur = IDC_SIZENWSE; break;
                    case H_NE: case H_SW: cur = IDC_SIZENESW; break;
                    case H_N: case H_S: cur = IDC_SIZENS; break;
                    case H_W: case H_E: cur = IDC_SIZEWE; break;
                    case H_MOVE: cur = IDC_SIZEALL; break;
                    case H_NEW: cur = IDC_CROSS; break;
                }
                SetCursor(LoadCursorW(nullptr, cur));
                return TRUE;
            }
            if (LOWORD(l) == HTCLIENT && u.page == 0 && dlg.open) {
                bool cross = u.hot == ID_DLG_SV || u.drag == ID_DLG_SV || (dlg.picking && u.hot == ID_DLG_PREVIEW);
                if (cross || u.hot == ID_DLG_HEX) {
                    SetCursor(LoadCursorW(nullptr, cross ? IDC_CROSS : IDC_IBEAM));
                    return TRUE;
                }
                break;
            }
            if (LOWORD(l) == HTCLIENT && u.page == 0 && u.nav == NAV_CLOCK && u.picking && u.hot == ID_CLK_AREA) {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                return TRUE;
            }
            if (LOWORD(l) == HTCLIENT && u.page == 0 && u.nav == NAV_CLOCK && (u.hot == ID_CLK_AREA || u.drag == ID_CLK_AREA)) {
                SetCursor(LoadCursorW(nullptr, u.overClock || u.drag ? IDC_SIZEALL : IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDBLCLK:  // double-clicking a saved colour uses it; anywhere else it's a click
            if (DlgDoubleClick(Dip(GET_X_LPARAM(l)), Dip(GET_Y_LPARAM(l)))) return 0;
            [[fallthrough]];
        case WM_LBUTTONDOWN: {
            float x = Dip(GET_X_LPARAM(l)), y = Dip(GET_Y_LPARAM(l));
            SetFocus(h);
            const Hit* ht = HitAt(x, y);
            if (u.recording && (!ht || ht->id != ID_HOTKEY)) StopRecording();
            FinishStep();
            u.keySlider = ht && ht->kind == K_SLIDER && ht->enabled ? ht->id : 0;
            if (dlg.open && DlgMouseDown(ht, x, y)) {
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (u.picking && ht && ht->id == ID_CLK_AREA) {  // eyedropper: take the colour and stop
                std::wstring pc;
                if (SampleWallpaper(u.clockBox, x, y, &pc)) {
                    EditedLook()->color = pc;
                    Log(L"ui: clock colour picked from the wallpaper: %ls", pc.c_str());
                    ClockEdited();
                }
                u.picking = false;
                u.pickColor.clear();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (u.picking && (!ht || ht->id != ID_CLK_EYEDROP)) {  // clicked elsewhere: stop picking
                u.picking = false;
                u.pickColor.clear();
            }
            if (!ht || !ht->enabled) return 0;
            SetCapture(h);
            u.press = ht->id;
            if (ht->kind == K_SLIDER) {
                u.drag = ht->id;
                OnSlider(ht->id, SliderFrac(*ht, x), false);
            } else if (ht->kind == K_SEG) {
                int s = SegAt(*ht, x);
                if (s >= 0) OnSeg(ht->id, s);
            } else if (ht->id == ID_FNT_BAR) {
                u.drag = ht->id;
                FontBarDrag(y, true);
            } else if (ht->id == ID_CLK_AREA) {
                u.drag = ht->id;
                u.overClock = true;
                ClockDrag(x, y, true);
            } else if (ht->id == ID_DLG_SV || ht->id == ID_DLG_HUE) {
                u.drag = ht->id;
                DlgDrag(ht->id, x, y);
            } else if (ht->id == ID_C_AREA) {
                u.handle = HandleAt(x, y);
                if (u.handle != H_NONE) {
                    u.drag = ht->id;
                    u.dragStart = D2D1::Point2F(x, y);
                    u.dragOrig = u.edit;
                }
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            float x = Dip(GET_X_LPARAM(l)), y = Dip(GET_Y_LPARAM(l));
            ReleaseCapture();
            int dragId = u.drag, pressId = u.press;
            u.drag = 0;
            u.press = 0;
            if (dragId) {
                for (auto& ht : u.hits)
                    if (ht.id == dragId) {
                        if (ht.kind == K_SLIDER) OnSlider(ht.id, SliderFrac(ht, x), true);
                        else if (ht.id == ID_C_AREA) SendPreview(true);
                        else if (ht.id == ID_CLK_AREA) {
                            u.snapX = u.snapY = false;
                            ClockEdited();  // save, and put the full look on the desktop
                        } else if (ht.id == ID_DLG_SV || ht.id == ID_DLG_HUE) {
                            LiveClock(true);
                        }
                        break;
                    }
                u.handle = H_NONE;
            } else if (pressId) {
                const Hit* ht = HitAt(x, y);
                if (ht && ht->id == pressId && (ht->kind == K_BUTTON || ht->kind == K_TOGGLE)) OnClick(pressId);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_RBUTTONUP:  // right-clicking a saved colour empties its box
            if (dlg.open) {
                const Hit* ht = HitAt(Dip(GET_X_LPARAM(l)), Dip(GET_Y_LPARAM(l)));
                if (ht && ht->id >= ID_DLG_SLOT0 && ht->id < ID_DLG_SLOT0 + 16) {
                    dlg.slot = ht->id - ID_DLG_SLOT0;
                    DlgClearSlot();
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
            return 0;
        case WM_CHAR:  // typing a hex code in the colour dialog
            if (dlg.open && (iswxdigit((wchar_t)w) || w == L'#')) {
                DlgHexChar((wchar_t)w);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            if (fnt.open) {
                FontScrollTo(fnt.target - GET_WHEEL_DELTA_WPARAM(w) / 120.f * 80, true);
            } else if (u.page == 1) {
                ZoomCrop(GET_WHEEL_DELTA_WPARAM(w) > 0 ? 0.92f : 1 / 0.92f);
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (OnKey((UINT)w)) return 0;
            break;
        case WM_KEYUP:
            if (w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN) FinishStep();
            break;
        case WM_SYSKEYUP:
        case WM_SYSCHAR:
            if (u.recording) return 0;  // keep Alt from opening the window menu while recording
            break;
        case WM_KILLFOCUS:
            if (u.recording) { StopRecording(); InvalidateRect(h, nullptr, FALSE); }
            return 0;
        case WM_ACTIVATE:
            // Back from Microsoft Store or Settings with a fix: look at the video again.
            if (LOWORD(w) != WA_INACTIVE && u.fault != VF_NONE && !u.frameBusy) RequestFrame();
            if (LOWORD(w) != WA_INACTIVE && fnt.open) {  // back from the browser with a font downloaded
                FindDownloadedFonts();
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_TIMER:
            if (w == TIMER_STATUS) InvalidateRect(h, nullptr, FALSE);  // status, memory, the clock's minute
            if (w == TIMER_SCROLL) {  // ease towards the target: a third of the way each frame
                fnt.scroll += (fnt.target - fnt.scroll) * 0.3f;
                if (!fnt.open || std::abs(fnt.target - fnt.scroll) < 0.5f) {
                    fnt.scroll = fnt.target;
                    KillTimer(h, TIMER_SCROLL);
                }
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_UI_STILL: {
            auto* sm = (StillMsg*)l;
            u.stillLoading = false;
            SafeRelease(u.still);
            u.stillPx.swap(sm->px);
            u.stillW = sm->w;
            u.stillH = sm->h;
            u.stillSrcW = sm->srcW;
            u.stillSrcH = sm->srcH;
            u.stillStyle = sm->style;
            u.stillBg = D2D1::ColorF(GetRValue(sm->bg) / 255.f, GetGValue(sm->bg) / 255.f, GetBValue(sm->bg) / 255.f);
            delete sm;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_UI_INFO: {
            auto* im = (InfoMsg*)l;
            if (_wcsicmp(im->path.c_str(), g_settings.video.c_str()) == 0) {
                u.info = im->vi;
                u.infoOk = im->ok;
                u.infoPath = im->path;
                u.fault = im->ok ? VF_NONE : im->fault;
                u.faultText = im->ok ? FaultText{} : DescribeFault(im->fault, im->vi.subtype.Data1, im->vi.hr, im->path);
                if (!im->ok) { u.frameFailed = true; u.frameBusy = false; }
                else Host_VideoPlayable(im->path);  // it had failed to play: it can now (a codec was installed)
            }
            delete im;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_UI_FRAME: {
            auto* fm = (FrameMsg*)l;
            if (fm->serial == u.serial) u.frameBusy = false;
            if (_wcsicmp(fm->path.c_str(), g_settings.video.c_str()) == 0) {
                if (!fm->px.empty()) {
                    SafeRelease(u.frame);
                    u.framePx.swap(fm->px);
                    u.fw = fm->w;
                    u.fh = fm->h;
                    u.framePath = fm->path;
                    u.frameFailed = false;
                } else if (u.framePx.empty()) {
                    u.frameFailed = true;
                }
            }
            delete fm;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_UI_OPT_PROGRESS:
            u.optPct = (int)w;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_UI_OPT_DONE: {
            HRESULT hr = (HRESULT)w;
            u.optBusy = false;
            if (u.optGif) {
                if (SUCCEEDED(hr)) {
                    LinkLightCopy(u.optDst, u.optSrc);
                    if (_wcsicmp(g_settings.video.c_str(), u.optFrom.c_str()) == 0) Host_SetVideo(u.optDst);
                } else if (hr != E_ABORT) {
                    u.optMsg = Fmt(L"Couldn't turn the GIF into a video (0x%08lX)", (unsigned long)hr);
                }
            } else if (SUCCEEDED(hr) && _wcsicmp(g_settings.video.c_str(), u.optFrom.c_str()) == 0) {
                double t = u.optTime;
                LinkLightCopy(u.optDst, u.optSrc);
                Host_SetVideo(u.optDst, &u.optCrop);  // same crop: it is stored normalized
                g_settings.previewTime = t;
                Host_SettingsChanged();
                u.optMsg.clear();  // the details line now names the version playing
            } else if (FAILED(hr)) {
                u.optMsg = Fmt(L"Couldn't make the %ls version (0x%08lX)", u.optLabel.c_str(), (unsigned long)hr);
            }
            UI_Refresh();
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            if (u.page == 1) Host_PreviewCrop(nullptr);
            CloseVersions();
            if (dlg.open) {  // closed with the colour dialog open: as Cancel
                DlgTarget() = dlg.before;
                dlg = ColorDialog{};
                Host_SettingsChanged();
            }
            StopRecording();
            KillTimer(h, TIMER_STATUS);
            u.worker.Stop();
            ReleaseTarget();
            u.framePx = {};
            IUnknown** all[] = {(IUnknown**)&u.fTitle, (IUnknown**)&u.fSubtitle, (IUnknown**)&u.fHeader, (IUnknown**)&u.fBody, (IUnknown**)&u.fStrong,
                                (IUnknown**)&u.fSmall, (IUnknown**)&u.fIcon, (IUnknown**)&u.fIconSmall, (IUnknown**)&u.fWrap,
                                (IUnknown**)&u.dw, (IUnknown**)&u.d2d};
            for (auto p : all) SafeRelease(*p);
            MF.Stop();
            if (u.d2dDll) FreeLibrary(u.d2dDll);
            if (u.dwDll) FreeLibrary(u.dwDll);
            u = UI{};
            fnt = FontDialog{};
            Clock_ReleasePainter();  // the preview kept the clock's fonts loaded
            Host_TrimMemory();
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

void QueryScreen() {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    u.screenW = mi.rcMonitor.right - mi.rcMonitor.left;
    u.screenH = mi.rcMonitor.bottom - mi.rcMonitor.top;
}

void QueryGpu() {
    if (!D3D.Load()) return;
    IDXGIFactory1* f = nullptr;
    if (SUCCEEDED(D3D.CreateFactory1(__uuidof(IDXGIFactory1), (void**)&f))) {
        IDXGIAdapter1* a = PickDisplayAdapter(f, &u.gpu);
        SafeRelease(a);
        f->Release();
    }
    D3D.Unload();
    // "Intel(R) Iris(R) Xe Graphics" -> "Intel Iris Xe Graphics"
    for (const wchar_t* mark : {L"(R)", L"(TM)", L"(tm)"})
        for (size_t at; (at = u.gpu.find(mark)) != std::wstring::npos;) u.gpu.erase(at, wcslen(mark));
}

}  // namespace

void UI_Show() {
    if (u.hwnd) {
        if (IsIconic(u.hwnd)) ShowWindow(u.hwnd, SW_RESTORE);
        SetForegroundWindow(u.hwnd);
        return;
    }
    if (!InitFactories()) {
        MessageBoxW(nullptr, L"Direct2D is not available.", APP_NAME, MB_ICONERROR);
        return;
    }
    MF.Start();
    QueryScreen();
    QueryGpu();
    u.playlist = LoadPlaylist();
    LoadTheme();

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = UI_CLASS;
    RegisterClassExW(&wc);

    // Size the window for the DPI of the primary monitor, centred in its work area.
    MONITORINFO mi{sizeof mi};
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (auto fn = shcore ? (GetDpiForMonitorFn)(void*)GetProcAddress(shcore, "GetDpiForMonitor") : nullptr) fn(mon, 0, &dpiX, &dpiY);
    if (shcore) FreeLibrary(shcore);
    u.dpi = (float)dpiX;
    // Tall enough for the clock page's full-width preview when the screen has room.
    kH = std::clamp(std::min((mi.rcWork.bottom - mi.rcWork.top) * 96.f / dpiX - 64, ceilf(ClockPageHeight())), 620.f, 760.f);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, (LONG)lround(kW * dpiX / 96), (LONG)lround(kH * dpiX / 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpiX);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = (mi.rcWork.left + mi.rcWork.right - ww) / 2, y = std::max((int)mi.rcWork.top, (int)(mi.rcWork.top + mi.rcWork.bottom - wh) / 2);
    u.hwnd = CreateWindowExW(0, UI_CLASS, L"VideoBG", style, x, y, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!u.hwnd) return;
    u.dpi = (float)GetDpiForWindow(u.hwnd);
    LoadTheme();  // applies the dark title bar now that the window exists
    u.worker.Start(u.hwnd);
    RequestFrame();
    SetTimer(u.hwnd, TIMER_STATUS, 1000, nullptr);
    ShowWindow(u.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(u.hwnd);
}

bool UI_IsOpen() { return u.hwnd != nullptr; }

void UI_Refresh() {
    if (!u.hwnd) return;
    // A different video was chosen elsewhere (tray menu): fetch its preview.
    if (!g_settings.video.empty() && _wcsicmp(u.framePath.c_str(), g_settings.video.c_str()) != 0 && !u.frameBusy) {
        SafeRelease(u.frame);
        u.framePx.clear();
        u.frameFailed = false;
        u.infoOk = false;
        u.fault = VF_NONE;
        RequestFrame();
    }
    InvalidateRect(u.hwnd, nullptr, FALSE);
}

void UI_VideoFailed() {
    if (!u.hwnd) return;
    RequestFrame(true);
    InvalidateRect(u.hwnd, nullptr, FALSE);
}

void UI_ConvertGif(const std::wstring& gif) {
    UI_Show();
    if (u.hwnd) StartGifConvert(gif);
}
