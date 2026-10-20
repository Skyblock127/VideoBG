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
#include <uxtheme.h>
#include <wincodec.h>
#include <windowsx.h>
#include <algorithm>
#include <memory>
#include <cmath>
#include <cstdarg>
#include <cwchar>
#include <cwctype>

namespace {

constexpr float kW = 820;  // client width in DIPs
float kH = 660;            // client height: taller when the screen has room (set in UI_Show)
constexpr UINT WM_UI_FRAME = WM_APP + 100, WM_UI_INFO = WM_APP + 101, WM_UI_OPT_PROGRESS = WM_APP + 103,
               WM_UI_OPT_DONE = WM_APP + 104, WM_UI_STILL = WM_APP + 106, WM_UI_FEEDBACK = WM_APP + 107, WM_UI_FB_CLOSE = WM_APP + 108,
               WM_UI_WALL = WM_APP + 109;  // lParam = WallpaperInfo* (the wallpapers' list)
constexpr UINT_PTR TIMER_STATUS = 1;
constexpr UINT_PTR TIMER_SCROLL = 2;  // the font list gliding to where the wheel sent it
constexpr UINT_PTR TIMER_SHARPEN = 4;  // a picture drawn quickly while it kept changing: sharpen it now it's settled
constexpr UINT_PTR TIMER_SONGS = 3;   // the playlist gliding, or scrolling by itself while a song is moved near its edge
constexpr UINT_PTR TIMER_HOLD = 5;    // the wallpaper held on the Preview slider's frame: a second after it's let go, it plays on
constexpr float kSongRowH = 34;       // a song's row in the playlist

enum Id {
    ID_NONE, ID_POWER, ID_CHOOSE, ID_CROP, ID_LOCK, ID_TIMELINE, ID_SPEED, ID_VOLUME, ID_FPS,
    ID_MONITORS, ID_GPU, ID_COVER, ID_BATTERY, ID_HOTKEY, ID_MUSIC_HOTKEY, ID_LAUNCH, ID_LOOK, ID_STARTUP_LINK, ID_STARTUP_REG,
    ID_C_AREA, ID_C_LOCK, ID_C_RESET, ID_C_CANCEL, ID_C_APPLY, ID_VERSIONS, ID_VER_SCRIM, ID_VER_CANCEL, ID_VER_USE,
    ID_SOUND, ID_MUSIC_PICK, ID_MUSIC_PLAY, ID_MUSIC_PREV, ID_MUSIC_NEXT, ID_MUSIC_SHUFFLE, ID_MUSIC_REPEAT, ID_MUSIC_SEEK,
    ID_MUSIC_LIST, ID_MUSIC_HEAD, ID_MUSIC_ADD, ID_MUSIC_SEARCH, ID_MUSIC_SEARCH_X, ID_UNMUTE, ID_KEEPSOUND, ID_OPEN_DATA, ID_OPEN_LOCAL, ID_FIX, ID_PLAYER_STYLE, ID_WALLS,
    ID_CLK_SHOW, ID_CLK_MODE, ID_CLK_SCOPE, ID_CLK_AREA, ID_CLK_ROTATE, ID_CLK_CUSTOM, ID_CLK_SIZE, ID_CLK_HOURS,
    ID_CLK_OPACITY, ID_CLK_GLOW, ID_CLK_GLOWSIZE, ID_CLK_FONT, ID_CLK_DATE, ID_FNT_SCRIM, ID_FNT_DONE, ID_FNT_BAR, ID_FNT_OWN, ID_FNT_LANG, ID_FNT_DIGITS, ID_REPO,
    ID_DLG_SCRIM, ID_DLG_TARGET, ID_DLG_SV, ID_DLG_HUE, ID_DLG_HEX, ID_DLG_COPY, ID_DLG_PICK, ID_DLG_SAME, ID_DLG_SAVE,
    ID_DLG_USE, ID_DLG_OK, ID_DLG_CANCEL,
    ID_FNT_YES, ID_FNT_NO, ID_VER_YES, ID_VER_NO,
    ID_FB_IDEA, ID_FB_BUG, ID_FB_SCRIM, ID_FB_TEXT, ID_FB_STEPS, ID_FB_EMAIL, ID_FB_OFTEN, ID_FB_LOG, ID_FB_PREVIEW, ID_FB_CANCEL,
    ID_FB_SEND, ID_MEMINFO, ID_FB_PICS, ID_FB_PICS_CLEAR,
    ID_PD_SCRIM, ID_PD_AREA, ID_PD_PREV, ID_PD_NEXT, ID_PD_COLOR, ID_PD_BODY, ID_PD_OPACITY, ID_PD_SIZE, ID_PD_CORNERS, ID_PD_RESET, ID_PD_DONE,
    ID_PD_PICK_COL, ID_PD_PICK_BODY, ID_PD_RST_COL, ID_PD_RST_BODY, ID_PD_RST_OPACITY, ID_PD_RST_SIZE, ID_PD_RST_CORNERS, ID_CLK_PICK, ID_PD_ROTATE, ID_PD_RST_ROTATE,
    ID_PRS, ID_PRS_SCRIM, ID_PRS_CLOSE, ID_PRS_YES, ID_PRS_NO, ID_PRS_NAME, ID_PRS_SAVE, ID_PRS_CANCEL, ID_PRS_CURRENT,
    ID_NAV = 200,        // sidebar pages: ID_NAV + page
    ID_DLG_SLOT0 = 300,  // colour dialog, "My colours" boxes: ID_DLG_SLOT0 + i
    ID_VER_ROW0 = 400,   // video versions dialog: a version's row, and its delete button
    ID_VER_DEL0 = 450,
    ID_FNT_ROW0 = 500,   // font picker: a font's row, its "Get it" link, "Add font file" and "Remove" buttons
    ID_FNT_GET0 = 600,
    ID_FNT_ADD0 = 700,
    ID_FNT_DEL0 = 800,
    ID_LANG_ROW0 = 1000,  // the clock language list: a language's row (as many as Windows has)
    ID_SONG_ROW0 = 10000,  // the playlist: a song's row, and its grip for moving it (up to 10000 songs)
    ID_SONG_GRIP0 = 20000,
    ID_SONG_DEL0 = 30000,  // + song: take it off the playlist
    ID_FB_PIC_X0 = 2050,  // the feedback dialog: a picture's remove button
    ID_PRS_USE0 = 2100,   // clock presets: a preset's Use, Edit, Rename and Delete buttons
    ID_PRS_EDIT0 = 2110,
    ID_PRS_REN0 = 2120,
    ID_PRS_DEL0 = 2130,
};
enum NavPage { NAV_VIDEO, NAV_PLAYBACK, NAV_CLOCK, NAV_SOUND, NAV_GENERAL, NAV_COUNT };  // sidebar order (the wallpaper, then the clock, the music, the rest)
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
    int sel = -1;             // K_SEG: the segment chosen
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
    int focus = 0;          // the control with the keyboard focus (Tab), outlined; 0 = none
    int tabFrom = 0;        // the control clicked last: Tab goes on from there
    float clockPrevX = -1;  // where Enter on the clock moved it from (to centre it), for Enter again
    int focusReturn = 0;    // what opened a dialog (or the crop page) by keyboard: the focus goes back to it
    bool focusFirst = false;  // a dialog just opened by keyboard: focus its first control once it's drawn
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
    std::wstring gpu;                 // the graphics chip the wallpaper uses
    std::wstring gpuLow, gpuHigh;     // the power-saving and the high-performance one (the same on most PCs)
    bool gpuAutoHigh = false;         // the one driving the main display is the high-performance one
    int screenW = 1920, screenH = 1080;
    int playW = 1920, playH = 1080, playCount = 1;
    float songScroll = 0, songTarget = 0, songMax = 0;  // the playlist: scrolled this far, gliding there, at most
    D2D1_RECT_F songView{};    // where the playlist's rows show
    // A press on a song: a click plays it, a drag scrolls the list, a drag by its grip moves the song.
    int songPress = -1;        // the song (playlist index)
    bool songMoving = false, songMoved = false;
    float songPressY = 0, songPressScroll = 0, songY = 0, songGrab = 0;
    int songIns = 0;           // moving: the place it would land
    DWORD songEdgeSince = 0;   // moving: since when the cursor has been near the list's edge
    float seekFrac = -1;       // the music's seek bar while it's dragged
    int songShown = -1;        // the song the player shows (clicking "Playlist" finds it in the list)
    bool searchOn = false;     // the playlist's search box is out
    std::wstring search;       // what's typed in it
    int searchHit = -1;        // the song that matches it best (playlist index)
    D2D1_RECT_F memInfo{};  // the sidebar's memory numbers (hovering them explains them)  // the largest display the video plays on, and how many it plays on

    int recording = 0;  // a shortcut being recorded: ID_HOTKEY or ID_MUSIC_HOTKEY
    std::wstring hotkeyMsg;
    int hotkeyMsgFor = 0;  // the shortcut box it's about

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
    float rotFrom = 0, rotPointer = 0;  // turning the clock: its angle when taken, and the pointer's
    bool overClock = false, snapX = false, snapY = false;
    float snapYv = 0;
    bool picking = false;             // eyedropper on: clicking the preview takes the wallpaper's colour there
    std::wstring pickColor;           // colour under the eyedropper ("" when it isn't over the wallpaper)
    bool pickDirect = false;          // it came from the page, not the colour dialog: the click sets dlg.target's colour itself
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
    DWORD lastSeek = 0;               // the wallpaper last moved with the Preview slider (tick)
    bool held = false;                // the wallpaper waits on the Preview slider's frame
    struct Scaled {                   // a picture already scaled to where it's drawn (see DrawScaled)
        ID2D1Bitmap* from = nullptr;  // not held: ForgetScaled() runs before a picture is let go
        D2D1_RECT_F src{};
        UINT32 w = 0, h = 0;
        ID2D1Bitmap* bmp = nullptr;
    } scaled[3];
    int scaledNext = 0;
    DWORD scaledMissAt = 0;           // when a picture last had to be scaled anew (tick)
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
void ReleasePresetSamples();

void ForgetScaled();

void ReleaseTarget() {
    ReleaseFontSamples();  // the font picker's, if it's open
    ReleasePresetSamples();
    ForgetScaled();
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

// High-quality scaling of a 1080p frame takes ~9 ms on this (software) target, on every paint, so a
// picture shown smaller is scaled once and then copied 1:1 until it, the part of it shown or the
// size changes. While that keeps changing (a window being resized), quick linear scaling stands in
// and the sharp copy follows once it settles. Pictures shown larger are drawn as before.
void ForgetScaled() {
    for (auto& c : u.scaled) {
        SafeRelease(c.bmp);
        c = UI::Scaled{};
    }
}

void DrawScaled(ID2D1Bitmap* bm, const D2D1_RECT_F& dst, const D2D1_RECT_F& src) {
    if (!u.dc) {
        u.rt->DrawBitmap(bm, &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
        return;
    }
    const float k = u.dpi / 96;
    const long x = lroundf(dst.left * k), y = lroundf(dst.top * k);
    const long pw = lroundf(dst.right * k) - x, ph = lroundf(dst.bottom * k) - y;
    const bool smaller = pw > 0 && ph > 0 && pw <= lroundf(src.right - src.left) && ph <= lroundf(src.bottom - src.top);
    if (!smaller) {
        u.dc->DrawBitmap(bm, &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src, nullptr);
        return;
    }
    const UINT32 w = (UINT32)pw, h = (UINT32)ph;
    const D2D1_RECT_F at = D2D1::RectF(x / k, y / k, (x + pw) / k, (y + ph) / k);  // on whole pixels, so the copy lands 1:1
    auto same = [](const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
        return fabsf(a.left - b.left) < 0.01f && fabsf(a.top - b.top) < 0.01f && fabsf(a.right - b.right) < 0.01f &&
               fabsf(a.bottom - b.bottom) < 0.01f;
    };
    for (auto& c : u.scaled)
        if (c.bmp && c.from == bm && c.w == w && c.h == h && same(c.src, src)) {
            u.dc->DrawBitmap(c.bmp, &at, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
            return;
        }
    const DWORD now = GetTickCount();
    const bool settling = now - u.scaledMissAt < 250;
    u.scaledMissAt = now;
    ID2D1BitmapRenderTarget* brt = nullptr;
    if (settling || FAILED(u.rt->CreateCompatibleRenderTarget(D2D1::SizeF(pw / k, ph / k), D2D1::SizeU(w, h), &brt))) {
        u.dc->DrawBitmap(bm, &dst, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR, &src, nullptr);
        SetTimer(u.hwnd, TIMER_SHARPEN, 300, nullptr);
        return;
    }
    ID2D1DeviceContext* bdc = nullptr;
    brt->QueryInterface(__uuidof(ID2D1DeviceContext), (void**)&bdc);
    const D2D1_RECT_F all = D2D1::RectF(0, 0, pw / k, ph / k);
    brt->BeginDraw();
    brt->Clear(D2D1::ColorF(0, 0, 0));
    if (bdc) bdc->DrawBitmap(bm, &all, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src, nullptr);
    else brt->DrawBitmap(bm, &all, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
    HRESULT hr = brt->EndDraw();
    ID2D1Bitmap* out = nullptr;
    if (SUCCEEDED(hr)) brt->GetBitmap(&out);
    SafeRelease(bdc);
    SafeRelease(brt);
    if (!out) {
        u.dc->DrawBitmap(bm, &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src, nullptr);
        return;
    }
    UI::Scaled& c = u.scaled[u.scaledNext++ % 3];
    SafeRelease(c.bmp);
    c = UI::Scaled{bm, src, w, h, out};
    u.dc->DrawBitmap(out, &at, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
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
    u.hits.back().sel = sel;
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
        DrawScaled(u.frame, dst, src);
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
        if (mask & PR_HOLD) return L"Held on the frame being picked";
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
const NavItem kNav[NAV_COUNT] = {{L"\uE714", L"Video"}, {L"\uE768", L"Playback"}, {L"\uE121", L"Clock"}, {L"\uE8D6", L"Music"},
                                 {L"\uE713", L"Settings"}};

void PaintHeader() {
    const Theme& t = u.th;
    D2D1_RECT_F logo = R(24, 20, 60, 56);
    EnsureLogoBitmap();
    if (u.logo) {
        D2D1_RECT_F icon = R(21.5f, 17.5f, 62.5f, 58.5f);  // the icon's rounded square has a 6% margin
        if (u.dc) u.dc->DrawBitmap(u.logo, &icon, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        else u.rt->DrawBitmap(u.logo, &icon, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
    } else {
        Fill(logo, t.accent, 9);
        Text(L"\uE768", logo, u.fIcon, Rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    Text(L"VideoBG", R(72, 16, 400, 42), u.fTitle, t.text);
    const bool modern = g_settings.look == 1;
    Text(HotkeyToString(g_settings.hkMods, g_settings.hkVk) + (modern ? L" brings up your wallpapers" : L" turns the wallpaper on or off"),
         R(72, 42, 470, 60), u.fSmall, t.text2);
    // The look: this window (classic), or the wallpaper picker with this window for settings (modern).
    Seg(ID_LOOK, R(kW - 24 - 40 - 12 - 60 - 16 - 180, 24, kW - 24 - 40 - 12 - 60 - 16, 54), {L"Classic", L"Modern"}, modern ? 1 : 0);
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
        if (i == NAV_PLAYBACK || i == NAV_SOUND) {  // groups: the wallpaper | the clock and the music | the rest
            Line(kSideX + 8, y + 6, kSideX + kSideW - 8, y + 6, t.ctrlStroke);
            y += 13;
        }
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
    AddHit(ID_MEMINFO, K_AREA, R(sc.left, yy - 2, sc.right, yy + 102));  // hover: what these numbers mean
    u.memInfo = R(sc.left, yy - 2, sc.right, yy + 102);
    // The version, underlined: a link to VideoBG's page on GitHub.
    const std::wstring version = L"VideoBG " APP_VERSION;
    const float vw = TextWidth(version, u.fSmall);
    const D2D1_RECT_F vr = R(x, sc.bottom - 28, x + vw, sc.bottom - 10);
    const bool vhot = IsHot(ID_REPO);
    Text(version, vr, u.fSmall, vhot ? t.accentHover : t.text2);
    Line(x, vr.bottom - 1, x + vw, vr.bottom - 1, vhot ? t.accentHover : t.text3);
    AddHit(ID_REPO, K_BUTTON, vr);
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
    double need = std::max(u.playW / ((c.r - c.l) * vw), u.playH / ((c.b - c.t) * vh));
    return Fmt(L"%up", (UINT)lround(u.playH * h / (vh * need)));
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
    // file, a status line (only while there's one) and the help line.
    FaultText faultBuf;
    const FaultText* fault = CurrentFault(&faultBuf);
    const bool hasStatus = u.optBusy || !u.optMsg.empty() || fault;
    const float below = hasStatus ? 180 : 154;
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
    Button(ID_WALLS, R(x + w - kPad - 148 - 8 - 150, y + 12, x + w - kPad - 156, y + 44), L"Your wallpapers", L"\uE8B9");
    const std::wstring& video = g_settings.video;
    const bool infoCurrent = u.infoOk && _wcsicmp(u.infoPath.c_str(), video.c_str()) == 0;
    DrawComposed(box, g_settings.crop, CropMode(g_settings.crop));
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
        } else if (LightCopySize(u.info.w, u.info.h, g_settings.crop, u.playW, u.playH, &ow, &oh)) {
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
        case ID_TIMELINE: help = L"Picks the frame for the preview and the lock screen; the wallpaper holds on it, then plays on"; break;
        case ID_WALLS: help = L"The videos you switch between (the same as in the modern look): click one to make it your wallpaper"; break;
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

// A tip in two lines of about the same length (split at the space nearest the middle).
std::wstring TwoLines(const std::wstring& s, IDWriteTextFormat* f) {
    const float half = TextWidth(s, f) / 2;
    size_t best = std::wstring::npos;
    float bestDiff = 1e9f;
    for (size_t i = s.find(L' '); i != std::wstring::npos; i = s.find(L' ', i + 1)) {
        const float d = fabsf(TextWidth(s.substr(0, i), f) - half);
        if (d < bestDiff) {
            bestDiff = d;
            best = i;
        }
    }
    return best == std::wstring::npos ? s : s.substr(0, best) + L"\n" + s.substr(best + 1);
}

// An icon button for the music player: a round hover, the icon in the accent colour while it's on.
void IconButton(int id, float cx, float cy, float r, const wchar_t* icon, bool on, bool enabled) {
    const Theme& t = u.th;
    const bool hot = enabled && IsHot(id), pressed = enabled && u.press == id && u.hot == id;
    if (hot || pressed) Circle(cx, cy, r, pressed ? t.ctrlPress : t.ctrlHover);
    Text(icon, R(cx - r, cy - r, cx + r, cy + r), u.fIcon, !enabled ? t.text3 : on ? t.accent : t.text, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (on && enabled) Circle(cx, cy + r - 3, 2, t.accent);  // a dot under it, like a lit switch
    AddHit(id, K_BUTTON, R(cx - r, cy - r, cx + r, cy + r), enabled);
}

std::wstring SongName(const std::wstring& path) {
    std::wstring n = FileName(path);
    const size_t dot = n.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? n : n.substr(0, dot);
}

std::wstring Minutes(UINT ms) { return Fmt(L"%u:%02u", ms / 60000, ms / 1000 % 60); }

// The player: the song, a seek bar, and shuffle / previous / play-pause / next / repeat.
void PaintPlayer(float x, float w, float yy) {
    const Theme& t = u.th;
    const float lx = x + kPad, right = x + w - kPad;
    const MusicState ms = Host_MusicState();
    const std::vector<int> order = MusicOrderFor(g_settings, Host_Songs().size());
    const bool have = !Host_Songs().empty(), live = have && Host_IsOn() && ms.track >= 0;
    const int kept = g_settings.musicTrack >= 0 && g_settings.musicTrack < (int)Host_Songs().size() ? g_settings.musicTrack : -1;
    const int track = ms.track >= 0 && ms.track < (int)Host_Songs().size() ? ms.track : kept >= 0 ? kept : have ? order[0] : -1;
    std::wstring title = track >= 0 ? SongName(Host_Songs()[track]) : L"No songs chosen yet";
    std::wstring note;
    D2D1_COLOR_F noteColor = t.text2;
    if (Host_CurrentTrack() == -2) {
        note = L"Windows couldn't play these songs";
        noteColor = t.warn;
    } else if (have && !Host_IsOn()) {
        note = g_settings.musicPlaying ? L"Plays when the video wallpaper is back on" : L"Stays paused when the video wallpaper is back on";
    } else if (live && g_settings.musicPlaying && !ms.playing) {
        note = L"Paused with the wallpaper";
    } else if (live) {
        const float f = u.drag == ID_MUSIC_SEEK && u.seekFrac >= 0 ? u.seekFrac : 0;
        note = Minutes(u.drag == ID_MUSIC_SEEK ? (UINT)(f * ms.durMs) : ms.posMs) + L" / " + Minutes(ms.durMs);
    }
    Text(L"\uE8D6", R(lx, yy, lx + 18, yy + 22), u.fIconSmall, have ? t.accent : t.text3);
    const float noteW = note.empty() ? 0 : TextWidth(note, u.fSmall) + 16;
    Text(title, R(lx + 24, yy, right - noteW, yy + 22), u.fStrong, !have ? t.text2 : t.text);
    u.songShown = track;
    Text(note, R(right - noteW, yy, right, yy + 22), u.fSmall, noteColor, DWRITE_TEXT_ALIGNMENT_TRAILING);
    yy += 26;
    float frac = u.drag == ID_MUSIC_SEEK && u.seekFrac >= 0 ? u.seekFrac : ms.durMs ? (float)ms.posMs / ms.durMs : 0;
    if (!live && track == kept && g_settings.musicDurMs) frac = (float)g_settings.musicPosMs / g_settings.musicDurMs;  // where it'll carry on
    Slider(ID_MUSIC_SEEK, R(lx - 10, yy, right + 10, yy + 28), frac, live && ms.durMs > 0);
    yy += 34;
    // The buttons, centred: shuffle, previous, play / pause, next, repeat.
    const float cy = yy + 22, mid = (lx + right) / 2;
    const bool shuffled = MusicShuffled(order), on = have && Host_IsOn();  // off: all greyed out, as it was
    IconButton(ID_MUSIC_SHUFFLE, mid - 118, cy, 18, L"\uE8B1", shuffled, on && Host_Songs().size() > 1);
    IconButton(ID_MUSIC_PREV, mid - 62, cy, 18, L"\uE892", false, live);
    {
        // Usable with the wallpaper off too: it decides whether the music starts when the wallpaper's back.
        const bool hot = have && IsHot(ID_MUSIC_PLAY), pressed = hot && u.press == ID_MUSIC_PLAY;
        Circle(mid, cy, 22, !have ? Mix(t.accent, t.card, 0.6f) : pressed ? Mix(t.accent, t.card, 0.2f) : hot ? t.accentHover : t.accent);
        Text(g_settings.musicPlaying ? L"\uE769" : L"\uE768", R(mid - 22, cy - 22, mid + 22, cy + 22), u.fIcon, t.onAccent,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        AddHit(ID_MUSIC_PLAY, K_BUTTON, R(mid - 22, cy - 22, mid + 22, cy + 22), have);
    }
    IconButton(ID_MUSIC_NEXT, mid + 62, cy, 18, L"\uE893", false, live);
    IconButton(ID_MUSIC_REPEAT, mid + 118, cy, 18, g_settings.musicRepeat == 2 ? L"\uE8ED" : L"\uE8EE", g_settings.musicRepeat == 2,
               on);  // lit: repeats this song; else the playlist goes round
}

// The playlist, in play order: click a song to play it, drag it to scroll the list, or drag it by
// its grip to move it (the others make room; near the list's edges it scrolls by itself).
void PaintPlaylist(float x, float w, float top, float bottom) {
    const Theme& t = u.th;
    const D2D1_RECT_F card = R(x, top, x + w, bottom);
    const size_t n = Host_Songs().size();
    // Its heading finds the song playing in the list (a click); then the search, adding songs, choosing new ones.
    Card(card, L"\uE8FD", L"");
    {
        const float tw = TextWidth(L"Playlist", u.fHeader);
        const D2D1_RECT_F hr = R(x + kPad + 28, top + 16, x + kPad + 28 + tw, top + 40);
        const bool hot = n && IsHot(ID_MUSIC_HEAD);
        Text(L"Playlist", hr, u.fHeader, hot ? t.accentHover : t.text);
        if (hot) Line(hr.left, hr.bottom - 1, hr.right, hr.bottom - 1, t.accentHover);
        AddHit(ID_MUSIC_HEAD, K_BUTTON, hr, n > 0);
    }
    if (u.searchOn) {  // the search box, across the heading: it only scrolls to the best match
        const D2D1_RECT_F sb = R(x + kPad + 126, top + 12, x + w - kPad, top + 44);
        Fill(sb, t.ctrl, 6);
        Stroke(sb, u.focus == ID_MUSIC_SEARCH || u.focus == 0 ? t.accent : t.ctrlStroke, 6);
        Text(L"\uE721", R(sb.left + 10, sb.top, sb.left + 28, sb.bottom), u.fIconSmall, t.text2);
        const D2D1_RECT_F tr = R(sb.left + 34, sb.top, sb.right - 40, sb.bottom);
        const bool none = !u.search.empty() && u.searchHit < 0;
        if (u.search.empty()) Text(L"Find a song", tr, u.fBody, t.text3);
        else Text(u.search, tr, u.fBody, t.text);
        const float cw = std::min(TextWidth(u.search, u.fBody), tr.right - tr.left);
        Line(tr.left + cw + 1, sb.top + 8, tr.left + cw + 1, sb.bottom - 8, t.text);  // the caret
        if (none) Text(L"No match", R(tr.left + cw + 12, sb.top, sb.right - 40, sb.bottom), u.fSmall, t.text3, DWRITE_TEXT_ALIGNMENT_TRAILING);
        AddHit(ID_MUSIC_SEARCH, K_AREA, R(sb.left, sb.top, sb.right - 36, sb.bottom));
        IconButton(ID_MUSIC_SEARCH_X, sb.right - 18, (sb.top + sb.bottom) / 2, 13, L"\uE711", false, true);
    } else {
        const float right = x + w - kPad;
        Button(ID_MUSIC_PICK, R(right - 148, top + 12, right, top + 44), L"Choose songs", L"\uE8D6", true);
        Button(ID_MUSIC_ADD, R(right - 148 - 8 - 124, top + 12, right - 156, top + 44), L"Add songs", L"\uE710", false);
        IconButton(ID_MUSIC_SEARCH, right - 280 - 26, top + 28, 16, L"\uE721", false, n > 0);
        const std::wstring count = n ? Fmt(L"%zu song%ls", n, n == 1 ? L"" : L"s") : L"";
        Text(count, R(x + kPad + 126, top + 16, right - 330, top + 40), u.fSmall, t.text2);
    }
    const D2D1_RECT_F view = R(x + 8, top + kCardHead + 8, x + w - 8, bottom - 8);
    u.songView = view;
    AddHit(ID_MUSIC_LIST, K_AREA, view);  // the wheel scrolls it
    if (!n) {
        Text(L"Choose songs (a whole folder works too) and they play while the video wallpaper is on.",
             R(x + kPad, view.top + 6, x + w - kPad, view.top + 40), u.fWrap, t.text2);
        return;
    }
    const std::vector<int> order = MusicOrderFor(g_settings, n);
    const MusicState ms = Host_MusicState();
    const bool on = Host_IsOn();  // with the video wallpaper off, the songs can't be played or moved
    const int current = ms.track >= 0 ? ms.track : on ? -1 : std::clamp(g_settings.musicTrack, -1, (int)n - 1);
    const float rowH = kSongRowH, viewH = view.bottom - view.top;
    u.songMax = std::max(0.f, n * rowH - viewH);
    u.songScroll = std::clamp(u.songScroll, 0.f, u.songMax);
    u.songTarget = std::clamp(u.songTarget, 0.f, u.songMax);
    // A song being moved leaves a gap where it would land; it follows the cursor up and down, inside the list.
    std::vector<int> shown = order;
    const bool moving = u.drag == ID_MUSIC_LIST && u.songMoving && u.songPress >= 0 && u.songPress < (int)n;
    float liftTop = 0;
    if (moving) {
        shown.erase(std::find(shown.begin(), shown.end(), u.songPress));
        liftTop = std::clamp(u.songY - u.songGrab, view.top, view.bottom - rowH);
        u.songIns = std::clamp((int)lroundf((liftTop - view.top + u.songScroll) / rowH), 0, (int)n - 1);
        shown.insert(shown.begin() + u.songIns, -1);
    }
    auto row = [&](int song, size_t p, const D2D1_RECT_F& r, bool lifted) {
        const bool isCurrent = song == current;
        const bool hot = !moving && (IsHot(ID_SONG_ROW0 + song) || IsHot(ID_SONG_GRIP0 + song) || IsHot(ID_SONG_DEL0 + song) ||
                                     (u.drag == ID_MUSIC_LIST && u.songPress == song));
        if (lifted) {
            Fill(R(r.left, r.top + 2, r.right, r.bottom + 3), D2D1::ColorF(0, 0, 0, 0.25f), 5);  // shadow
            Fill(r, Mix(t.card, t.accent, current ? 0.22f : 0.1f), 5);
            Stroke(r, t.accent, 5);
        } else if (isCurrent) {
            Fill(r, Mix(t.card, t.accent, on ? 0.16f : 0.07f), 5);
        } else if (hot && on) {
            Fill(r, Mix(t.card, t.text, 0.05f), 5);
        }
        if (u.searchOn && song == u.searchHit && !lifted) Stroke(r, t.accent, 5);  // what the search found
        if (isCurrent) Text(L"\uE767", R(r.left + 6, r.top, r.left + 34, r.bottom), u.fIconSmall, on ? t.accent : t.text3, DWRITE_TEXT_ALIGNMENT_CENTER);
        else Text(std::to_wstring(p + 1), R(r.left + 6, r.top, r.left + 34, r.bottom), u.fSmall, t.text3, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(SongName(Host_Songs()[song]), R(r.left + 42, r.top, r.right - (hot && on ? 80 : 48), r.bottom), isCurrent ? u.fStrong : u.fBody,
             !on ? t.text3 : isCurrent || lifted ? t.text : t.text2);
        const bool gripHot = on && (IsHot(ID_SONG_GRIP0 + song) || lifted);
        Text(L"\uE76F", R(r.right - 40, r.top, r.right - 8, r.bottom), u.fIconSmall, gripHot ? t.text : t.text3, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (!moving) {
            if (hot && on)  // take it off the list: shown only on its row, in the row's own grey
                Text(L"\uE711", R(r.right - 76, r.top, r.right - 44, r.bottom), u.fIconSmall, IsHot(ID_SONG_DEL0 + song) ? t.text : t.text3,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            AddHit(ID_SONG_ROW0 + song, K_BUTTON, R(r.left, r.top, r.right - 76, r.bottom), on);
            AddHit(ID_SONG_DEL0 + song, K_BUTTON, R(r.right - 76, r.top, r.right - 44, r.bottom), on);
            AddHit(ID_SONG_GRIP0 + song, K_AREA, R(r.right - 44, r.top, r.right, r.bottom), on);
        }
    };
    const size_t firstHit = u.hits.size();
    u.rt->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t p = 0; p < n; p++) {
        const float ry = view.top - u.songScroll + p * rowH;
        if (ry + rowH < view.top || ry > view.bottom) continue;
        const D2D1_RECT_F r = R(view.left + 4, ry + 1, view.right - 4, ry + rowH - 1);
        if (shown[p] < 0) {  // where the moved song would land
            Stroke(R(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), Mix(t.card, t.accent, 0.5f), 5);
            continue;
        }
        row(shown[p], p, r, false);
    }
    if (moving) row(u.songPress, u.songIns, R(view.left + 4, liftTop + 1, view.right - 4, liftTop + rowH - 1), true);
    u.rt->PopAxisAlignedClip();
    for (size_t h = firstHit; h < u.hits.size(); h++) {  // what's scrolled out of the list can't be clicked
        if (u.hits[h].id == ID_MUSIC_LIST) continue;
        D2D1_RECT_F& hr = u.hits[h].r;
        hr = R(std::max(hr.left, view.left), std::max(hr.top, view.top), std::min(hr.right, view.right), std::min(hr.bottom, view.bottom));
        if (hr.bottom <= hr.top) hr = R(0, 0, 0, 0);
    }
}

void FontButton(int id, const D2D1_RECT_F& r, const std::wstring& font, bool enabled, const wchar_t* icon);

void PaintSoundPage(float x, float w) {
    const Theme& t = u.th;
    const float y = kTop, lx = x + kPad, cx = CtrlX(x), cw = CtrlW(w);
    const int mode = g_settings.sound;
    const float playerH = 110, styleH = kRowH + 8;  // (the player's style: a full-size button)
    const D2D1_RECT_F card = R(x, y, x + w, y + kCardHead + 3 * kRowH + styleH + (mode == 2 ? playerH : kRowH) + 10);
    Card(card, L"\uE767", L"Sound");
    float yy = y + kCardHead;
    Row(lx, yy, kLabelW, L"Play");
    Seg(ID_SOUND, SegRect(cx, yy, cw), {L"Nothing", L"Video's audio", L"My music"}, mode);
    yy += kRowH;
    // The sound when the video pauses (covered, locked, screen off, on battery). With the
    // still wallpaper there's no sound at all.
    Row(lx, yy, kLabelW, L"When video pauses", kRowH, mode != 0);
    Seg(ID_KEEPSOUND, SegRect(cx, yy, cw), {L"Pause the sound too", L"Keep the sound playing"}, g_settings.keepSound ? 1 : 0, mode != 0);
    yy += kRowH;
    Row(lx, yy, kLabelW, L"Volume", kRowH, mode != 0);
    Slider(ID_VOLUME, SliderRect(cx, yy, cw), g_settings.volume / 100.f, mode != 0);
    const bool muted = mode == 1 && g_settings.videoMuted;  // by the sound shortcut
    SliderValue(g_settings.volume && !muted ? Fmt(L"%d%%", g_settings.volume) : std::wstring(L"Muted"), cx, yy, cw, mode != 0);
    yy += kRowH;
    // The music player on the desktop (with My music): its style, picked from a list that shows each.
    {
        const std::vector<std::wstring> styles = Player_Styles();
        const int k = std::clamp(g_settings.musicPlayer, 0, (int)styles.size());
        Row(lx, yy, kLabelW, L"Player", styleH, mode == 2);
        FontButton(ID_PLAYER_STYLE, SegRect(cx, yy, cw, styleH), k ? styles[k - 1] + L" player" : L"No player", mode == 2,
                   L"\uE8D6");
    }
    yy += styleH;
    if (mode == 2) {
        PaintPlayer(x, w, yy + 4);
    } else {
        bool noAudio = mode == 1 && u.infoOk && u.info.audioStreams.empty();
        std::wstring info = mode == 0 ? L"Silent: the video's audio isn't even decoded, which saves power"
                          : noAudio   ? L"This video has no sound; try My music instead"
                          : muted     ? L"Muted"
                                      : L"Plays the video's own soundtrack, in sync with the picture";
        Text(muted ? L"\uE74F" : L"\uE946", R(lx, yy, lx + 16, yy + kRowH), u.fIconSmall, noAudio || muted ? t.warn : t.text3);
        Text(info, R(lx + 22, yy, x + w - kPad - (mode == 1 ? 70 : 0), yy + kRowH), u.fSmall, noAudio || muted ? t.warn : t.text2);
        if (mode == 1 && !noAudio)  // mute or unmute (as the sound shortcut does): the video plays on, in sync
            Link(ID_UNMUTE, R(x + w - kPad - 60, yy + 11, x + w - kPad, yy + 29), muted ? L"Unmute" : L"Mute");
    }
    if (mode == 2) PaintPlaylist(x, w, card.bottom + 12, kH - 24);
}

void QueryScreen();

void QueryGpu();

void PaintPowerCard(float x, float y, float w);

// Playback, then power saving (pausing while covered or on battery) under it.
void PaintPlaybackPage(float x, float w) {
    const Theme& t = u.th;
    const float y = kTop, lx = x + kPad, cx = CtrlX(x), cw = CtrlW(w);
    const bool twoGpus = !u.gpuLow.empty() && !u.gpuHigh.empty() && u.gpuLow != u.gpuHigh;  // laptops with a dedicated chip
    const D2D1_RECT_F card = R(x, y, x + w, y + kCardHead + 4 * kRowH + 92);
    Card(card, L"\uE768", L"Playback");
    float yy = y + kCardHead;
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
    yy += kRowH;
    Text(L"The video is decoded once and every display shows it at its own size, so more displays hardly add memory. "
         L"Versions are sized for the largest display it plays on.",
         R(lx, yy, x + w - kPad, yy + 38), u.fWrap, t.text2);
    // Which chip decodes and draws the video: a choice only on PCs with two (laptops with a dedicated one).
    yy += 44;
    Row(lx, yy, kLabelW, L"Graphics chip", kRowH, twoGpus);
    Seg(ID_GPU, SegRect(cx, yy, cw), {L"Power saving", L"High performance"},
        !twoGpus ? -1 : g_settings.gpu ? g_settings.gpu - 1 : (u.gpuAutoHigh ? 1 : 0), twoGpus);
    yy += kRowH;
    Text(!twoGpus ? L"Uses " + u.gpu + L". This PC has one graphics chip, so there's nothing to switch."
                  : L"Uses " + u.gpu + L". High performance keeps the video in the dedicated chip's own memory instead of your RAM, "
                                       L"but uses more power, which matters on battery.",
         R(lx, yy, x + w - kPad, yy + 38), u.fWrap, t.text2);
    PaintPowerCard(x, card.bottom + 12, w);
}

void PaintPowerCard(float x, float y, float w) {
    const Theme& t = u.th;
    const float lx = x + kPad, cx = CtrlX(x), cw = CtrlW(w);
    Card(R(x, y, x + w, y + kCardHead + 44 + 2 * kRowH + 10), L"\uE83F", L"Power saving");
    Text(L"While paused, the video freezes on its current frame and stops decoding, so it uses no CPU or GPU, and after a "
         L"while it frees its memory too. It always pauses while the PC is locked or the screen is off.",
         R(lx, y + kCardHead - 4, x + w - kPad, y + kCardHead + 36), u.fWrap, t.text2);
    float yy = y + kCardHead + 44;
    Row(lx, yy, kLabelW, L"Pause when covered by");
    Seg(ID_COVER, SegRect(cx, yy, cw), {L"Nothing", L"Fullscreen apps", L"Maximized apps"}, g_settings.pauseCover);
    yy += kRowH;
    Row(lx, yy, kLabelW, L"On battery");
    Seg(ID_BATTERY, SegRect(cx, yy, cw), {L"Keep playing", L"Pause", L"Still wallpaper"}, g_settings.onBattery);
}

// ---- Feedback (General page) -----------------------------------------------------------------
// A feature idea or a problem report, sent to the developer (feedback.cpp). The text boxes are
// Windows' own (typing, pasting, undo and every language for free), placed over the dialog.
struct FeedbackUI {
    bool open = false;
    bool bug = false;  // a problem report (else a feature idea)
    HWND text = nullptr, steps = nullptr, email = nullptr;
    HFONT font = nullptr;
    HBRUSH brush = nullptr;
    COLORREF face = 0;
    int often = -1;  // 0 once, 1 sometimes, 2 every time
    bool withLog = true;
    bool sending = false;
    std::wstring note;                     // the bottom bar: why it wasn't sent
    std::wstring sent;                     // on the page for a few seconds once sent
    DWORD sentAt = 0;
    std::wstring idea, what, doing, mail;  // drafts, kept while VideoBG runs (Cancel doesn't lose them)
    D2D1_RECT_F boxText{}, boxSteps{}, boxEmail{};
    D2D1_RECT_F picsButton{}, strip{};      // adding pictures; the strip that shows them (once there are some)
    float picScroll = 0, picMax = 0;        // the strip, scrolled sideways
    std::vector<FeedbackPicture> pictures;  // screenshots for it (added, or pasted with Ctrl+V)
    WNDPROC editProc = nullptr;
} fb;

std::wstring FbGet(HWND e) {
    std::wstring t(e ? GetWindowTextLengthW(e) : 0, L'\0');
    if (!t.empty()) GetWindowTextW(e, t.data(), (int)t.size() + 1);
    const size_t a = t.find_first_not_of(L" \t\r\n"), b = t.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : t.substr(a, b - a + 1);
}

bool FbBox(int id) { return id == ID_FB_TEXT || id == ID_FB_STEPS || id == ID_FB_EMAIL; }
HWND FbEditFor(int id) { return id == ID_FB_TEXT ? fb.text : id == ID_FB_STEPS ? fb.steps : id == ID_FB_EMAIL ? fb.email : nullptr; }

// The dialog (DIPs), and its text boxes.
constexpr float kFbStripH = 86;  // the pictures' strip (72 DIP tall pictures)

D2D1_RECT_F FeedbackLayout() {
    const float base = fb.bug ? 548 : 376, strip = fb.pictures.empty() ? 0 : kFbStripH, W = kDialogW, L = (kW - W) / 2;
    const float H = std::min(base + strip, kH - 24), squeeze = base + strip - H;  // (a short window: the text boxes give up the room)
    float T = std::max(12.f, (kH - base) / 2);
    if (T + H > kH - 12) T = std::max(12.f, kH - 12 - H);  // it grows down, as far as there's room
    float y = T + 84;
    if (!fb.bug) {
        fb.boxText = R(L + 24, y + 24, L + W - 24, y + 24 + 144 - squeeze);
        fb.boxSteps = fb.boxEmail = R(0, 0, 0, 0);
        fb.picsButton = R(L + 24, fb.boxText.bottom + 14, L + 164, fb.boxText.bottom + 46);
        y = fb.picsButton.bottom;
    } else {
        const float textCut = std::min(squeeze, 40.f);
        fb.boxText = R(L + 24, y + 24, L + W - 24, y + 24 + 96 - textCut);
        y = fb.boxText.bottom + 14;
        fb.boxSteps = R(L + 24, y + 24, L + W - 24, y + 24 + 64 - (squeeze - textCut));
        y = fb.boxSteps.bottom + 14 + 32 + 14 + 28 + 14;
        fb.boxEmail = R(L + 24, y + 24, L + 24 + 320, y + 24 + 32);
        fb.picsButton = R(L + 356, fb.boxEmail.top, L + 496, fb.boxEmail.bottom);
        y = fb.boxEmail.bottom;
    }
    fb.strip = strip ? R(L + 24, y + 12, L + W - 24, y + 12 + 72) : R(0, 0, 0, 0);
    return R(L, T, L + W, T + H);
}

void FbFitScroll(HWND e);

// The text boxes follow the layout (it changes as pictures come and go).
void FbPlaceEdits() {
    const float k = u.dpi / 96.f;
    auto place = [&](HWND e, const D2D1_RECT_F& box, bool multi) {
        if (!e) return;
        const int x = (int)lroundf((box.left + 8) * k), y = (int)lroundf((box.top + 6) * k);
        const int w = (int)lroundf((box.right - box.left - (multi ? 10 : 16)) * k), h = (int)lroundf((box.bottom - box.top - 12) * k);
        RECT r;
        GetWindowRect(e, &r);
        MapWindowPoints(nullptr, u.hwnd, (POINT*)&r, 2);
        if (r.left == x && r.top == y && r.right - r.left == w && r.bottom - r.top == h) return;
        MoveWindow(e, x, y, w, h, TRUE);
        FbFitScroll(e);
    };
    place(fb.text, fb.boxText, true);
    place(fb.steps, fb.boxSteps, true);
    place(fb.email, fb.boxEmail, false);
}

void PaintGeneralPage(float x, float w) {
    const Theme& t = u.th;
    const float y = kTop, lx = x + kPad, cx = CtrlX(x), cw = CtrlW(w);
    const float msgH = u.hotkeyMsg.empty() ? 0 : 18;
    const bool modern = g_settings.look == 1;  // the shortcut brings up the wallpapers
    D2D1_RECT_F gc = R(x, y, x + w, y + kCardHead + 4 * kRowH + msgH + 10);
    Card(gc, L"\uE713", L"General");
    float yy = y + kCardHead;
    // A shortcut box: click it, then press the keys (its message, if any, under it).
    auto shortcut = [&](int id, const wchar_t* label, UINT mods, UINT vk, const wchar_t* recordingText) {
        Row(lx, yy, kLabelW, label);
        const bool rec = u.recording == id;
        D2D1_RECT_F hk = SegRect(cx, yy, cw);
        Fill(hk, rec ? t.card : (IsHot(id) ? t.ctrlHover : t.ctrl), 4);
        Stroke(hk, rec ? t.accent : t.ctrlStroke, 4, rec ? 2 : 1);
        Text(rec ? std::wstring(recordingText) : HotkeyToString(mods, vk), hk, rec ? u.fBody : u.fStrong, rec || !vk ? t.text2 : t.text,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        if (!rec) Text(L"\uE765", R(hk.left + 12, hk.top, hk.left + 28, hk.bottom), u.fIconSmall, t.text3);
        AddHit(id, K_BUTTON, hk);
        yy += kRowH;
        if (msgH && u.hotkeyMsgFor == id) {
            Text(u.hotkeyMsg, R(cx, yy - 6, cx + cw, yy + 12), u.fSmall, t.warn);
            yy += msgH;
        }
    };
    shortcut(ID_HOTKEY, modern ? L"Wallpapers shortcut" : L"Toggle hotkey", g_settings.hkMods, g_settings.hkVk,
             L"Press the new shortcut (Esc cancels)");
    shortcut(ID_MUSIC_HOTKEY, L"Pause / mute sound", g_settings.musicHkMods, g_settings.musicHkVk,
             L"Press a key (Esc cancels, Backspace: none)");
    Row(lx, yy, kLabelW, L"When app starts");
    Seg(ID_LAUNCH, SegRect(cx, yy, cw), {L"Video wallpaper", L"As I left it", L"Still wallpaper"}, g_settings.launch);
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

    // Feedback: an idea or a problem, straight to the developer.
    const float by0 = gc.bottom + 12;
    const D2D1_RECT_F fc = R(x, by0, x + w, by0 + kCardHead + 74);
    Card(fc, L"\uE939", L"Feedback");
    // Once something's sent, the line above the buttons says so for 3 seconds.
    if (!fb.sent.empty() && GetTickCount() - fb.sentAt > 3000) fb.sent.clear();
    Text(fb.sent.empty() ? std::wstring(L"Ideas and problem reports go straight to the developer. No account or personal details needed.")
                         : fb.sent,
         R(lx, by0 + kCardHead - 6, x + w - kPad, by0 + kCardHead + 14), u.fSmall, fb.sent.empty() ? t.text2 : t.good);
    const float fby = by0 + kCardHead + 24, half = (x + w - kPad - lx - 12) / 2;  // two buttons across the card
    Button(ID_FB_IDEA, R(lx, fby, lx + half, fby + 32), L"Suggest a feature", L"\uE82F");
    Button(ID_FB_BUG, R(lx + half + 12, fby, x + w - kPad, fby + 32), L"Report a problem", L"\uE7BA");

    // Where things are kept.
    float fy = fc.bottom + 12;
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

// Clock presets: the list, and a preset being edited on the page (the choices above the preview
// wait meanwhile, and the desktop shows the preset).
struct PresetUI {
    bool open = false;       // the list
    int confirm = -1;        // Use or Delete was clicked: asking first
    bool confirmDel = false; // ...about deleting it
    int rename = -1;         // the preset whose name is being typed
    std::wstring name;
    bool nameFresh = false;  // the next key replaces the whole name
    std::wstring note;       // short note in the list's bottom bar
    int edit = -1;           // the preset being edited on the page
    ClockLook look;          // its look meanwhile
} prs;
const size_t kPresetNameMax = 20;

std::wstring PresetName(int i) {
    if (i < 0 || i >= kClockPresets) return L"";
    const std::wstring& n = g_settings.presets[i].name;
    return n.empty() ? Fmt(L"Preset %d", i + 1) : n;
}

// The look the choices above the preview pick.
ClockLook* PickedLook() {
    if (u.clockEdit == 1) return &g_settings.clockStill;
    return g_settings.clockVideoOwn ? &g_settings.clockVideo : &g_settings.clockLive;
}

std::wstring PickedLookName() {
    if (u.clockEdit == 1) return L"the still wallpaper's clock";
    return g_settings.clockVideoOwn ? L"this video's clock" : L"the clock all videos share";
}

// The look the page edits: a preset being edited, else the one picked.
ClockLook* EditedLook() { return prs.edit >= 0 ? &prs.look : PickedLook(); }

bool EditingShownLook() { return prs.edit >= 0 || (u.clockEdit == 0) == Host_ClockLive(); }

bool ClockUsable() { return EditedLook()->show; }

// The colour dialog (More, or Glow colour), drawn over the page. The colour shows live on the page
// and, when the look is the one on the desktop, on the desktop too; Cancel puts the old one back.
struct ColorDialog {
    bool open = false;
    int target = 0;              // the colour being edited: 0 the text's, 1 the glow's
    float h = 0, s = 0, v = 1;   // the colour being edited (the hue stays put while it's grey)
    std::wstring exact;          // "r,g,b" when set from a code, box or eyedropper (HSV would round it)
    std::wstring beforeText, beforeGlow;  // the look's colours when the dialog opened (Cancel puts both back)
    bool sameAsText = false;     // glow: follow the text colour
    int slot = -1;               // chosen box in My colours (kept between openings)
    bool hexEdit = false, hexFresh = false;  // typing a hex code (fresh: the next key replaces it)
    std::wstring hex;
    std::wstring msg;            // short note in the bottom bar: copied, pasted, saved
    DWORD msgAt = 0;
    bool msgWarn = false;
    D2D1_RECT_F sv{}, hue{}, preview{}, previewScreen{};  // previewScreen: the whole screen, zoomed onto the clock
    // The eyedropper (u.picking) hides the dialog: the colour comes from the whole wallpaper on the page.
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

// The music player dialog (the Sound page's Player): a close-up of the wallpaper where the player
// is, with the player on it at about its real size (it follows when the player's dragged on the
// desktop), its style, its colours (through the colour dialog), opacity, size and corners.
struct PlayerDialog {
    bool open = false;
    D2D1_RECT_F closeup{}, screen{};  // the close-up, and the whole screen it's a part of (zoomed alike)
    ID2D1Bitmap* pic = nullptr;       // the player, drawn for it
    std::wstring picKey;
    DWORD picAt = 0;
} pd;

Settings::PlayerLook& PdLook() { return g_settings.playerLooks[std::clamp(g_settings.musicPlayer, 1, (int)std::size(g_settings.playerLooks) - 1)]; }

std::wstring DlgColor() {
    if (!dlg.exact.empty()) return dlg.exact;
    BYTE r, g, b;
    HsvToRgb(dlg.h, dlg.s, dlg.v, &r, &g, &b);
    return Fmt(L"%d,%d,%d", r, g, b);
}

// The look's value the dialog edits.
// Targets 2 and 3: the music player's colour (its lit parts) and its body (from the player dialog).
std::wstring& DlgTarget() {
    if (dlg.target >= 2) return dlg.target == 2 ? PdLook().color : PdLook().body;
    return dlg.target == 0 ? EditedLook()->color : EditedLook()->glowColor;
}

// What a player colour is when it's left to the style: the Windows accent colour, or the style's own body.
std::wstring PdDefault(int target) {
    if (target == 3) return L"20,20,23";
    return Fmt(L"%d,%d,%d", (int)lroundf(u.th.accent.r * 255), (int)lroundf(u.th.accent.g * 255), (int)lroundf(u.th.accent.b * 255));
}

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
    DrawScaled(u.still, dst, src);
    PopRounded(clip);
}

// The wallpaper the edited look belongs to, drawn into `box` (the whole screen).
void DrawWallpaper(const D2D1_RECT_F& box) {
    if (u.clockEdit == 0) DrawComposed(box, g_settings.crop, CropMode(g_settings.crop));
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
        f = ComputeFit(VideoAspect(), g_settings.crop, CropMode(g_settings.crop), bw, bh);
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

// The font a look shows: its own, or Audiowide when that font isn't on this PC any more.
std::wstring ShownFont(const ClockLook& l) { return Clock_FontReady(l.font) ? l.font : std::wstring(L"Audiowide"); }

// A font for a look's samples (a time in its own font and colours): a design's is Mond's own.
std::wstring SampleFont(const ClockLook& l) { return l.style.empty() ? ShownFont(l) : std::wstring(L"Audiowide"); }

// A button that shows the clock's font (or another choice, with its icon) and opens its list.
void FontButton(int id, const D2D1_RECT_F& r, const std::wstring& font, bool enabled, const wchar_t* icon = L"\uE8D2") {
    const Theme& t = u.th;
    bool hot = enabled && IsHot(id), pressed = enabled && u.press == id && u.hot == id;
    Fill(r, pressed ? t.ctrlPress : hot ? t.ctrlHover : t.ctrl, 4);
    Stroke(r, t.ctrlStroke, 4);
    Text(icon, R(r.left + 8, r.top, r.left + 26, r.bottom), u.fIconSmall, enabled ? t.text2 : t.text3, DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(font, R(r.left + 33, r.top, r.right - 28, r.bottom), u.fBody, enabled ? t.text : t.text3);
    Text(L"\uE76C", R(r.right - 26, r.top, r.right - 10, r.bottom), u.fIconSmall, enabled ? t.text2 : t.text3,
         DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(id, K_BUTTON, r, enabled);
}

// A button that shows how the date reads (today, in the look's style) and opens the date styles.
void DateButton(int id, const D2D1_RECT_F& r, const ClockLook& look, bool enabled) {
    const Theme& t = u.th;
    bool hot = enabled && IsHot(id), pressed = enabled && u.press == id && u.hot == id;
    Fill(r, pressed ? t.ctrlPress : hot ? t.ctrlHover : t.ctrl, 4);
    Stroke(r, t.ctrlStroke, 4);
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::wstring date = Clock_DateText(look, look.date, now);
    if (date.empty()) date = L"No date";
    Text(L"\uE787", R(r.left + 8, r.top, r.left + 26, r.bottom), u.fIconSmall, enabled ? t.text2 : t.text3, DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(date, R(r.left + 33, r.top, r.right - 28, r.bottom), u.fSmall, enabled ? t.text : t.text3);
    Text(L"\uE76C", R(r.right - 26, r.top, r.right - 10, r.bottom), u.fIconSmall, enabled ? t.text2 : t.text3,
         DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(id, K_BUTTON, r, enabled);
}

// The colour button: the text's colour and code, with the glow's colour beside it.
void ColorPairButton(int id, const D2D1_RECT_F& r, const std::wstring& text, const std::wstring& glow, bool enabled) {
    const Theme& t = u.th;
    bool hot = enabled && IsHot(id), pressed = enabled && u.press == id && u.hot == id;
    Fill(r, pressed ? t.ctrlPress : hot ? t.ctrlHover : t.ctrl, 4);
    Stroke(r, t.ctrlStroke, 4);
    const float cy = (r.top + r.bottom) / 2;
    for (int i = 0; i < 2; i++) {
        const D2D1_COLOR_F c = LookColor(i ? glow : text);
        Circle(r.left + 17 + i * 19, cy, 8, t.ctrlStroke);
        Circle(r.left + 17 + i * 19, cy, 7, enabled ? c : Mix(c, t.card, 0.6f));
    }
    Text(Hex(text), R(r.left + 52, r.top, r.right - 28, r.bottom), u.fBody, enabled ? t.text : t.text3);
    Text(L"\uE790", R(r.right - 26, r.top, r.right - 10, r.bottom), u.fIconSmall, enabled ? t.text2 : t.text3,
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
    const bool editing = prs.edit >= 0;  // a preset, on its own: the choices above wait
    Card(R(x, y, x + w, y + kCardHead + 44 + bh + below + 14), editing ? L"\uE70F" : L"\uE121",
         editing ? PresetName(prs.edit) : std::wstring(L"Desktop clock"));
    if (editing) {
        const float bt = y + 12, bb = y + 44, br = x + w - kPad;
        Button(ID_PRS_CURRENT, R(br - 344, bt, br - 196, bb), L"Current clock", L"\uE72C");
        Button(ID_PRS_CANCEL, R(br - 188, bt, br - 98, bb), L"Cancel");
        Button(ID_PRS_SAVE, R(br - 90, bt, br, bb), L"Save", nullptr, true);
    } else {
        Button(ID_PRS, R(x + w - kPad - 120, y + 12, x + w - kPad, y + 44), L"Presets", L"\uE728", true);
        if (IsHot(ID_PRS) || u.focus == ID_PRS)  // the tip, while pointing at it
            Text(TwoLines(L"Tip: save your clock as a preset before changing it, so you can get it back later", u.fSmall),
                 R(x + kPad + 190, y + 12, x + w - kPad - 132, y + 44), u.fSmall, t.text2, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    const bool on = ClockUsable();
    const std::wstring& video = g_settings.video;
    // Which look is being edited: the wallpaper (in the header), then for the video wallpaper the
    // look every video shares or this video's own; Show / Hide is part of the look picked.
    // One row of three equal parts: which wallpaper, which video's clock, shown or hidden.
    const float segW = (iw - 2 * 12) / 3, ry = y + kCardHead;
    Seg(ID_CLK_MODE, R(lx, ry, lx + segW, ry + 32), {L"Video", L"Still"}, u.clockEdit, !editing);
    Seg(ID_CLK_SCOPE, R(lx + segW + 12, ry, lx + 2 * segW + 12, ry + 32), {L"This video", L"All videos"},
        g_settings.clockVideoOwn ? 0 : 1, u.clockEdit == 0 && !video.empty() && !editing);
    Seg(ID_CLK_SHOW, R(x + w - kPad - segW, ry, x + w - kPad, ry + 32), {L"Show", L"Hide"}, EditedLook()->show ? 0 : 1, !editing);

    // Preview: the wallpaper with the clock on it; drag the clock to place it.
    float bx = x + (w - bw) / 2;
    D2D1_RECT_F box = R(bx, ry + 44, bx + bw, ry + 44 + bh);
    u.clockBox = box;
    ClockLook* look = EditedLook();
    ClockLook shown = *look;  // an eyedropper previews the colour under it
    if (u.picking && !u.pickColor.empty()) (dlg.open && dlg.target == 1 ? shown.glowColor : shown.color) = u.pickColor;
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
    if (on && (u.drag == ID_CLK_AREA || u.focus == ID_CLK_AREA || (u.overClock && u.hot == ID_CLK_AREA && !u.picking)))
        Badge(Fmt(L"%.1f%% across  \u00B7  %.1f%% down", look->x * 100, look->y * 100), box.left + 8, box.bottom - 28);
    AddHit(ID_CLK_AREA, K_AREA, box, on);  // (first: the handle over it takes the clicks on it)
    // The handle that turns the clock, above it (below it near the top), quiet until pointed at; it
    // shows while the pointer is on the preview. A double click puts it upright; Shift turns it freely.
    if (on && look->show && !u.picking && (u.hot == ID_CLK_AREA || u.hot == ID_CLK_ROTATE || u.drag == ID_CLK_ROTATE || u.drag == ID_CLK_AREA)) {
        const D2D1_RECT_F cr = ClockRectIn(box, *look);
        const float hx = (cr.left + cr.right) / 2, hy = cr.top - 14 > box.top + 12 ? cr.top - 14 : cr.bottom + 14;
        const bool lit = u.hot == ID_CLK_ROTATE || u.drag == ID_CLK_ROTATE;
        Circle(hx, hy, 9, D2D1::ColorF(0, 0, 0, 0.3f));
        u.br->SetColor(D2D1::ColorF(1, 1, 1, lit ? 0.75f : 0.4f));
        u.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(hx, hy), 9, 9), u.br, 1);
        Text(L"\uE7AD", R(hx - 9, hy - 9, hx + 9, hy + 9), u.fIconSmall, D2D1::ColorF(1, 1, 1, lit ? 0.9f : 0.55f), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (u.drag == ID_CLK_ROTATE || (lit && look->rotate != 0)) Badge(Fmt(L"%.0f\u00B0", look->rotate), hx + 14, hy - 12);
        AddHit(ID_CLK_ROTATE, K_AREA, R(hx - 12, hy - 12, hx + 12, hy + 12));
    }
    if (!on) Fill(box, D2D1::ColorF(t.card.r, t.card.g, t.card.b, 0.55f), 6);  // greyed out
    float yy = box.bottom + 10;

    // The font and the colours (the text's and the glow's), the sizes and the glow, then the date and
    // the time: the text on the left, its colour and glow on the right.
    const float labW = 80, colW = (iw - 24) / 2, rx = lx + colW + 24, cw = colW - labW;
    const bool glowOn = on && look->glow > 0;
    auto sliderRow = [&](float cx0, int id, const wchar_t* label, float frac, const std::wstring& value, bool en) {
        Row(cx0, yy, labW, label, rowH, en);
        Slider(id, SliderRect(cx0 + labW, yy, cw), frac, en);
        SliderValue(value, cx0 + labW, yy, cw, en, rowH);
    };
    Row(lx, yy, labW, L"Style", rowH, on);
    FontButton(ID_CLK_FONT, R(lx + labW, yy + 3, lx + colW, yy + rowH - 3), Clock_StyleName(*look), on, L"\uE790");
    Row(rx, yy, labW, L"Colour", rowH, on);
    ColorPairButton(ID_CLK_CUSTOM, R(rx + labW, yy + 3, rx + colW - 40, yy + rowH - 3), look->color,
                    look->glowColor.empty() ? look->color : look->glowColor, on);
    Button(ID_CLK_PICK, R(rx + colW - 32, yy + 3, rx + colW, yy + rowH - 3), L"", L"\uEF3C", u.picking && u.pickDirect, on);
    yy += rowH;
    sliderRow(lx, ID_CLK_SIZE, L"Size", (look->size - 0.5f) / 1.5f, Fmt(L"%.0f%%", look->size * 100), on);
    sliderRow(rx, ID_CLK_GLOW, L"Glow", look->glow, look->glow > 0 ? Fmt(L"%.0f%%", look->glow * 100) : std::wstring(L"Off"), on);
    yy += rowH;
    sliderRow(lx, ID_CLK_OPACITY, L"Opacity", (look->opacity - 0.2f) / 0.8f, Fmt(L"%.0f%%", look->opacity * 100), on);
    sliderRow(rx, ID_CLK_GLOWSIZE, L"Glow size", (look->glowSize - 2) / 38, Fmt(L"%.0f", look->glowSize), glowOn);
    yy += rowH;
    // (What the style doesn't show greys out: a dial has no 12- or 24-hour time.)
    const bool dated = on && Clock_UsesDate(*look), hours = on && Clock_UsesHours(*look);
    Row(lx, yy, labW, L"Date", rowH, dated);
    DateButton(ID_CLK_DATE, R(lx + labW, yy + 3, lx + colW, yy + rowH - 3), *look, dated);
    Row(rx, yy, labW, L"Time", rowH, hours);
    Seg(ID_CLK_HOURS, SegRect(rx + labW, yy, cw, rowH), {L"12-hour", L"24-hour"}, look->h24 ? 1 : 0, hours);
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
        case ID_CLK_CUSTOM: help = L"The text's and the glow's colours: any colour, a code, your saved ones or the wallpaper's"; break;
        case ID_CLK_PICK: help = L"Take the text's colour from the wallpaper: click the preview where it is"; break;
        case ID_CLK_FONT: help = L"The clock's style (Mond in any font, a dial, a ring...), and the language of its words"; break;
        case ID_CLK_SIZE: help = L"The clock's size in this look"; break;
        case ID_CLK_OPACITY: help = L"How solid the clock is; lower lets the wallpaper show through (glow included)"; break;
        case ID_CLK_HOURS: help = L"12-hour (1:30 PM) or 24-hour (13:30) time"; break;
        case ID_CLK_GLOW: help = L"A soft glow behind the letters that makes the clock stand out on busy wallpapers"; break;
        case ID_CLK_GLOWSIZE: help = L"How far the glow spreads"; break;
        case ID_CLK_AREA:
            help = u.focus == ID_CLK_AREA ? L"Arrow keys move the clock (Shift: one pixel); Enter centres it across, and again puts it back"
                                          : L"Drag the clock to place it (Alt: no snapping); arrow keys move it (Shift: one pixel)";
            break;
        case ID_CLK_DATE: help = L"How the date reads in this look"; break;
        case ID_PRS: help = L"Saved clock looks: keep this one, or put one on this clock"; break;
        case ID_PRS_SAVE: help = L"Keep these settings in " + PresetName(prs.edit); break;
        case ID_PRS_CANCEL: help = L"Leave " + PresetName(prs.edit) + L" as it was"; break;
        case ID_PRS_CURRENT: help = L"Start again from " + PickedLookName(); break;
        default: break;
    }
    if (u.picking && (u.hot == ID_CLK_AREA || help.empty()))
        help = u.pickColor.empty() ? L"Click the preview to take the wallpaper's colour there (Esc cancels)"
                                   : L"Click to use " + Hex(u.pickColor) + (dlg.target == 1 ? L" for the glow (Esc cancels)" : L" for the text (Esc cancels)");
    if (!u.clockNote.empty() && GetTickCount() - u.clockNoteAt < 3000) help = u.clockNote;
    if (help.empty() && editing)
        help = L"Editing " + PresetName(prs.edit) + L": the choices above wait, and your desktop shows it until you save or cancel";
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

D2D1_RECT_F PdScreen(const D2D1_RECT_F& box);
void PdDraw(const D2D1_RECT_F& screen);

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
    const bool player = dlg.target >= 2;
    // Which colour: the text's or the glow's (which can follow the text's); for the player, its lit
    // parts' (or the Windows accent colour) or its body's (or the style's own).
    Text(player ? L"Player colour" : L"Clock colour", R(L + 24, T + 16, L + 200, T + 46), u.fSubtitle, t.text);
    const float segL = L + W - 24 - 170;
    Seg(ID_DLG_TARGET, R(segL, T + 14, L + W - 24, T + 46),
        player ? std::vector<std::wstring>{L"Colour", L"Body"} : std::vector<std::wstring>{L"Text", L"Glow"}, player ? dlg.target - 2 : dlg.target);
    if (dlg.target != 0) {
        Text(dlg.target == 1 ? L"Same as the text" : dlg.target == 2 ? L"Windows accent" : L"Its own", R(L + 180, T + 16, segL - 16 - 40 - 12, T + 46),
             u.fBody, t.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
        Toggle(ID_DLG_SAME, segL - 16 - 40, T + 31, dlg.sameAsText);
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
    if (player) {  // the player where it is, in the new colour
        dlg.previewScreen = PdScreen(dlg.preview);
        clip = PushRounded(dlg.preview, 6);
        PdDraw(dlg.previewScreen);
        PopRounded(clip);
    } else {
        dlg.previewScreen = ZoomedScreen(dlg.preview, *look);
        clip = PushRounded(dlg.preview, 6);
        DrawWallpaper(dlg.previewScreen);
        DrawClockIn(dlg.previewScreen, *look);
        PopRounded(clip);
    }
    Stroke(dlg.preview, t.ctrlStroke, 6);

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
    Button(ID_DLG_PICK, R(rx + 172, hy, rx + rw, hy + 32), L"", L"\uEF3C", u.picking);

    // RGB, and the colour before and now.
    const float iy = T + 236;
    Text(Fmt(L"RGB  %d, %d, %d", cr, cg, cb), R(rx, iy, rx + 150, iy + 20), u.fSmall, t.text2);
    D2D1_RECT_F ch = R(rx + 152, iy + 1, rx + rw, iy + 19);
    clip = PushRounded(ch, 4);
    const std::wstring& before = dlg.target == 0 ? dlg.beforeText : dlg.beforeGlow;
    Fill(R(ch.left, ch.top, (ch.left + ch.right) / 2, ch.bottom), LookColor(before.empty() ? dlg.beforeText : before));
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
    int confirm = -1;  // a version's delete button was clicked: asking first
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
    // Sizes that cover the screen for the part shown (the crop), and steps below that. With several
    // displays, the largest one the video plays on: they all show the same decoded video.
    const Crop& c = g_settings.crop;
    double need = vw && vh ? std::max(u.playW / ((c.r - c.l) * vw), u.playH / ((c.b - c.t) * vh)) : 1;
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
            UINT lines = (UINT)lround(u.playH * f);
            v.label = f != 1.0 ? Fmt(L"%up", lines) : Fmt(u.playCount > 1 ? L"%up (your largest screen)" : L"%up (your screen)", lines);
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
        ver.note = L"Turn the video's sound off (Music page) to see versions on the desktop";
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

    // Bottom bar: what the desktop shows, and the choice (or the question before deleting a version).
    const float by = T + H - 48;
    if (ver.confirm >= 0 && ver.confirm < n) {
        Text(L"Delete the " + ver.list[ver.confirm].label + L" version? You can make it again later.",
             R(L + 24, by - 4, L + W - 24 - 236, by + 36), u.fWrap, t.text);
        Button(ID_VER_NO, R(L + W - 24 - 228, by, L + W - 24 - 126, by + 32), L"Cancel");
        Button(ID_VER_YES, R(L + W - 24 - 118, by, L + W - 24, by + 32), L"Delete", nullptr, true);
        return;
    }
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
// Style picker: every clock style, each drawn as the clock (in the page's text colour): Mond, the
// designs, then Mond in each of the other fonts. A font VideoBG can't include has a link to get it
// and a button to add its file instead. At the end, the user can add any font of their own (for
// Mond); VideoBG keeps a copy in its fonts folder. Clicking a style uses it at once for the look
// being edited. Each font is loaded only while its sample is drawn.

void ClockEdited();

const float kFontRowH = 132;                    // a font's row
const float kDateRowH = 48;                     // a date style's row
const float kLangRowH = 40;                     // a language's row

struct FontDialog {
    bool open = false;
    bool dates = false;  // the date styles instead of the fonts
    bool players = false;  // the desktop player's styles instead
    bool walls = false;    // the wallpapers instead (WallRow each)
    int confirm = -1;      // a font's Remove was clicked: asking first
    float scrollBack = 0;  // where the font list was, for coming back from the languages
    bool langs = false;  // within the date styles: the list of languages
    std::vector<ClockLanguage> langList;
    std::vector<ClockStyleInfo> list;
    std::vector<ID2D1Bitmap*> samples;
    std::vector<D2D1_SIZE_F> sizes;  // in DIPs
    std::vector<bool> tried;
    float boxW = 0;                  // the room the samples were drawn for
    float nameW = 148;               // the clock styles' names (and what they are): as wide as the longest
    std::wstring note;
    float scroll = 0, target = 0, maxScroll = 0;  // the list, in DIPs: where it is, and where it's gliding to
    float trackTop = 0, trackH = 0, thumbH = 0, grab = 0;  // the scroll bar, for dragging it
    float viewH = 0;                                       // the list's visible height
    D2D1_RECT_F view{};                                    // and where it is
} fnt;

// A wallpaper in the list: one per video (a lighter version joins its original's row).
struct WallRow {
    std::wstring path, play;  // the video; the version it plays (the wallpaper's, if it's one of them)
    WallpaperInfo info;       // from the library worker (no pictures kept in it)
    bool ready = false;
    std::vector<uint32_t> thumb;
    int tw = 0, th = 0;
};
std::vector<WallRow> g_walls;

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

// The rows: languages, date styles, the player's styles (and none), wallpapers, or the clock's styles.
int PickerRows() {
    return fnt.langs     ? (int)fnt.langList.size()
           : fnt.dates   ? Clock_DateStyleCount()
           : fnt.players ? 1 + (int)Player_Styles().size()
           : fnt.walls   ? (int)g_walls.size()
                         : (int)fnt.list.size();
}
float PickerRowH() { return fnt.langs ? kLangRowH : fnt.dates ? kDateRowH : fnt.players ? 124.f : fnt.walls ? 88.f : kFontRowH; }

void ReleaseFontSamples() {
    for (ID2D1Bitmap*& b : fnt.samples) SafeRelease(b);
    const size_t n = PickerRows();
    fnt.samples.assign(n, nullptr);
    fnt.sizes.assign(n, D2D1::SizeF(0, 0));
    fnt.tried.assign(n, false);
}

void RefreshFontList() {
    fnt.list = Clock_Styles();
    ReleaseFontSamples();
}

int FontIndex(const std::wstring& key) {  // Mond in that font
    for (size_t i = 0; i < fnt.list.size(); i++)
        if (fnt.list[i].style.empty() && !_wcsicmp(fnt.list[i].font.key.c_str(), key.c_str())) return (int)i;
    return -1;
}

// Fonts VideoBG can't include that were just downloaded: added from the Downloads folder.
void FindDownloadedFonts() {
    bool added = false;
    for (const ClockStyleInfo& s : fnt.list)
        if (s.font.getUrl && !s.font.ready && Clock_FindDownloadedFont(s.font.key)) {
            fnt.note = L"Found " + s.font.key + L" in your Downloads and added it";
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

void OpenDateDialog() {
    fnt = FontDialog{};
    fnt.open = fnt.dates = true;
    ReleaseFontSamples();
}

// The wallpapers (the modern look's list): the videos in it, a picture of each made by the library's
// worker (posted back as WM_UI_WALL).
void OpenWallpapers() {
    fnt = FontDialog{};
    fnt.open = fnt.walls = true;
    g_walls.clear();
    const std::wstring copies = LocalDataDir() + L"\\Light copies\\";
    int current = 0;
    for (const std::wstring& p : LoadWallpapers(g_settings.video)) {
        const bool copy = p.size() > copies.size() && !_wcsnicmp(p.c_str(), copies.c_str(), copies.size());
        const std::wstring orig = copy ? LightCopyOriginalPath(p) : L"";
        const std::wstring& key = orig.empty() ? p : orig;
        int at = -1;
        for (size_t i = 0; i < g_walls.size(); i++)
            if (SamePath(g_walls[i].path, key)) at = (int)i;
        if (at < 0) {
            WallRow w;
            w.path = w.play = key;
            g_walls.push_back(w);
            at = (int)g_walls.size() - 1;
        }
        if (SamePath(p, g_settings.video)) {
            g_walls[at].play = p;
            current = at;
        }
    }
    ReleaseFontSamples();
    Library_Start(u.hwnd, WM_UI_WALL, u.screenW, u.screenH);
    for (const WallRow& w : g_walls) Library_Request(w.path, false, false);
    fnt.scroll = fnt.target = std::max(0, current - 1) * 88.f;  // the next paint keeps it in range
}

// A video's picture and facts, from the worker.
void WallInfo(WallpaperInfo* raw) {
    std::unique_ptr<WallpaperInfo> info(raw);
    if (!fnt.open || !fnt.walls) return;
    for (size_t i = 0; i < g_walls.size(); i++) {
        WallRow& w = g_walls[i];
        if (!SamePath(w.path, info->path)) continue;
        w.ready = true;
        w.thumb.swap(info->thumb);
        w.tw = info->tw;
        w.th = info->th;
        info->frame.clear();
        w.info = std::move(*info);
        if (i < fnt.samples.size()) {
            SafeRelease(fnt.samples[i]);
            fnt.tried[i] = false;
        }
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
}

// Adds videos to the list (the same file dialog as the picker's).
void WallsAdd() {
    IFileOpenDialog* dlgBox = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlgBox)))) return;
    const COMDLG_FILTERSPEC types[] = {{L"Videos", kVideoPatterns}, {L"All files", L"*.*"}};
    dlgBox->SetFileTypes((UINT)std::size(types), types);
    dlgBox->SetTitle(L"Add video wallpapers (Ctrl+A picks a whole folder)");
    dlgBox->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT);
    std::vector<std::wstring> picked;
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dlgBox->Show(u.hwnd)) && SUCCEEDED(dlgBox->GetResults(&items))) {
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
    dlgBox->Release();
    int added = 0;
    for (const std::wstring& p : picked) {
        bool have = false;
        for (const WallRow& w : g_walls) have = have || SamePath(w.path, p);
        if (have) continue;
        WallRow w;
        w.path = w.play = p;
        g_walls.push_back(w);
        Library_Request(p, false, true);
        added++;
    }
    if (!added) return;
    std::vector<std::wstring> list;
    for (const WallRow& w : g_walls) list.push_back(w.path);
    SaveWallpapers(list);
    ReleaseFontSamples();
    fnt.note = added == 1 ? L"1 video added" : Fmt(L"%d videos added", added);
    Log(L"ui: %d video(s) added to the wallpapers", added);
    FontScrollTo(1e6f, true);  // to the new ones, at the end
}

// Takes a wallpaper off the list (the file stays where it is).
void WallsRemove(int i) {
    if (i < 0 || i >= (int)g_walls.size() || SamePath(g_walls[i].play, g_settings.video)) return;
    Log(L"ui: %ls taken off the wallpapers", g_walls[i].path.c_str());
    fnt.note = L"\u201C" + SongName(g_walls[i].path) + L"\u201D is off the list (the file is still there)";
    g_walls.erase(g_walls.begin() + i);
    std::vector<std::wstring> list;
    for (const WallRow& w : g_walls) list.push_back(w.path);
    SaveWallpapers(list);
    ReleaseFontSamples();
}

void OpenPlayerDialog() {
    pd.open = true;
    pd.picKey.clear();
}

void ClosePlayerDialog() {
    if (!pd.open) return;
    pd.open = false;
    SafeRelease(pd.pic);
    pd.picKey.clear();
    SaveSettings(g_settings);
    Player_FreePreview();
}

// The whole main screen, zoomed (UI DIPs per screen pixel) so that the player shows at about its real
// size in `box` (smaller if it wouldn't fit), placed so the player sits in the middle of the box (kept
// inside the screen's edges).
D2D1_RECT_F PdScreen(const D2D1_RECT_F& box) {
    const int style = g_settings.musicPlayer;
    float sw = 0, sh = 0;
    Player_Size(style, &sw, &sh, true);
    const float pix = Clock_PixelScale(), size = style > 0 ? std::clamp(PdLook().size, 0.7f, 1.5f) : 1;
    const float bw = box.right - box.left, bh = box.bottom - box.top;
    const float scrW = (float)std::max(1, u.screenW), scrH = (float)std::max(1, u.screenH);
    float z = 96.f / u.dpi;  // one screen pixel, one window pixel
    if (sw > 0) z = std::min({z, (bw - 40) / (sw * size * pix), (bh - 28) / (sh * size * pix)});
    const float w = scrW * z, h = scrH * z;
    float l = (box.left + box.right) / 2 - g_settings.playerX * w, t = (box.top + box.bottom) / 2 - g_settings.playerY * h;
    if (w >= bw) l = std::clamp(l, box.right - w, box.left);
    if (h >= bh) t = std::clamp(t, box.bottom - h, box.top);
    return R(l, t, l + w, t + h);
}

// The video wallpaper in `screen` (a zoomed whole screen), and the player on it where it is.
void PdDraw(const D2D1_RECT_F& screen) {
    const int keep = u.clockEdit;
    u.clockEdit = 0;  // (the player shows with the video wallpaper)
    DrawWallpaper(screen);
    u.clockEdit = keep;
    const int style = g_settings.musicPlayer;
    float sw = 0, sh = 0;
    Player_Size(style, &sw, &sh, true);  // (as it's turned)
    if (style < 1 || sw <= 0 || u.screenW <= 0) return;
    const float z = (screen.right - screen.left) / u.screenW, k = u.dpi / 96.f;
    const float scale = std::clamp(PdLook().size, 0.7f, 1.5f) * Clock_PixelScale() * z;  // window DIPs per style DIP
    const std::wstring key = Fmt(L"%d|%.4f|%ls|%ls|%.2f|%.1f|%d|%d|%.1f", style, scale, PdLook().color.c_str(), PdLook().body.c_str(), PdLook().opacity,
                                 PdLook().corners, g_settings.musicPlaying, Host_MusicState().track, PdLook().rotate);
    if (!pd.pic || key != pd.picKey || GetTickCount() - pd.picAt > 1000) {
        SafeRelease(pd.pic);
        ClockImage img;
        if (Player_Paint(style, scale * k, false, &img, nullptr, true)) {
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            u.rt->CreateBitmap(D2D1::SizeU((UINT32)img.w, (UINT32)img.h), img.px.data(), (UINT32)img.w * 4, &bp, &pd.pic);
        }
        pd.picKey = key;
        pd.picAt = GetTickCount();
    }
    if (!pd.pic) return;
    const float pw = sw * scale, ph = sh * scale;
    const float cx = screen.left + g_settings.playerX * (screen.right - screen.left), cy = screen.top + g_settings.playerY * (screen.bottom - screen.top);
    D2D1_RECT_F dst = R(cx - pw / 2, cy - ph / 2, cx + pw / 2, cy + ph / 2);
    u.rt->DrawBitmap(pd.pic, &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
}

// The wallpaper's colour under the eyedropper, in the player dialog's close-up (or the colour
// dialog's own preview of it).
bool PdSample(float x, float y, std::wstring* out) {
    const int keep = u.clockEdit;
    u.clockEdit = 0;
    const bool ok = In(pd.closeup, x, y) ? SampleWallpaper(pd.screen, x, y, out)
                  : dlg.open && In(dlg.preview, x, y) ? SampleWallpaper(dlg.previewScreen, x, y, out) : false;
    u.clockEdit = keep;
    return ok;
}

void PaintPlayerDialog() {
    const Theme& t = u.th;
    Fill(R(0, 0, kW, kH), D2D1::ColorF(0, 0, 0, t.dark ? 0.5f : 0.3f));
    AddHit(ID_PD_SCRIM, K_AREA, R(0, 0, kW, kH));  // the page underneath can't be used meanwhile
    const float W = kDialogW, H = 548, L = (kW - W) / 2, T = std::max(12.f, (kH - H) / 2);
    const D2D1_RECT_F panel = R(L, T, L + W, T + H);
    const D2D1_COLOR_F face = t.dark ? Rgb(0x2B2B2B) : Rgb(0xFFFFFF), bar = t.dark ? Rgb(0x202020) : Rgb(0xF3F3F3);
    Fill(R(L - 2, T, L + W + 2, T + H + 4), D2D1::ColorF(0, 0, 0, 0.2f), 10);  // shadow
    Fill(panel, face, 8);
    ID2D1RoundedRectangleGeometry* clip = PushRounded(panel, 8);
    Fill(R(L, T + H - 64, L + W, T + H), bar);
    Line(L, T + H - 64, L + W, T + H - 64, t.stroke);
    PopRounded(clip);
    Stroke(panel, t.dark ? Rgb(0x3C3C3C) : Rgb(0xD5D5D5), 8);
    Text(L"Music player", R(L + 24, T + 16, L + W - 24, T + 46), u.fSubtitle, t.text);
    Text(L"On your desktop while My music plays. Drag it there to move it: this shows where it is.", R(L + 24, T + 50, L + W - 24, T + 72),
         u.fBody, t.text2);
    // The close-up: the wallpaper where the player is, the player on it.
    pd.closeup = R(L + 24, T + 82, L + W - 24, T + 82 + 220);
    pd.screen = PdScreen(pd.closeup);
    clip = PushRounded(pd.closeup, 6);
    {
        Settings::PlayerLook& pl = PdLook();
        const Settings::PlayerLook kept = pl;
        if (u.picking && dlg.target >= 2 && !u.pickColor.empty()) (dlg.target == 2 ? pl.color : pl.body) = u.pickColor;  // the colour under the eyedropper, on it
        PdDraw(pd.screen);
        pl = kept;
    }
    if (u.picking && dlg.target >= 2 && !u.pickColor.empty()) Loupe(u.pickAt.x, u.pickAt.y, pd.closeup, u.pickColor);
    PopRounded(clip);
    Stroke(pd.closeup, t.ctrlStroke, 6);
    AddHit(ID_PD_AREA, K_AREA, pd.closeup);
    // Its style: step through them (Off, then each).
    const int style = g_settings.musicPlayer;
    const bool has = style > 0;
    const Settings::PlayerLook look = has ? PdLook() : Settings::PlayerLook{};
    float y = pd.closeup.bottom + 14;
    const float lx = L + 24, colW = (W - 48 - 24) / 2, labW = 76, rx = lx + colW + 24;
    Text(L"Style", R(lx, y, lx + labW, y + 32), u.fBody, t.text);
    {
        const D2D1_RECT_F r = R(lx + labW, y, L + W - 24, y + 32);
        Fill(r, t.ctrl, 4);
        Stroke(r, t.ctrlStroke, 4);
        const std::vector<std::wstring> names = Player_Styles();
        Text(has && style <= (int)names.size() ? names[style - 1] + L"  \u00B7  " + Player_About(style) : std::wstring(L"Off: no player on the desktop"),
             R(r.left + 40, r.top, r.right - 40, r.bottom), u.fBody, t.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        IconButton(ID_PD_PREV, r.left + 18, (r.top + r.bottom) / 2, 14, L"\uE76B", false, true);
        IconButton(ID_PD_NEXT, r.right - 18, (r.top + r.bottom) / 2, 14, L"\uE76C", false, true);
    }
    y += 44;
    // Each setting changed from the style's own has a small arrow beside its name that puts it back.
    auto reset = [&](int id, float x, bool changed) {
        if (has && changed) IconButton(id, x + labW - 14, y + 16, 11, L"\uE7A7", false, true);
    };
    // Its colours: a swatch and what it is, each opening the colour dialog; an eyedropper beside each.
    auto colour = [&](int id, int pick, int target, float x, const wchar_t* label, const std::wstring& value, const std::wstring& dflt,
                      const wchar_t* dfltName) {
        Text(label, R(x, y, x + labW, y + 32), u.fBody, has ? t.text : t.text3);
        Button(pick, R(x + colW - 32, y, x + colW, y + 32), L"", L"\uEF3C", u.picking && u.pickDirect && dlg.target == target, has);
        const D2D1_RECT_F r = R(x + labW, y, x + colW - 40, y + 32);
        const bool hot = has && IsHot(id), pressed = hot && u.press == id;
        Fill(r, pressed ? t.ctrlPress : hot ? t.ctrlHover : t.ctrl, 4);
        Stroke(r, t.ctrlStroke, 4);
        const D2D1_COLOR_F c = LookColor(value.empty() ? dflt : value);
        Circle(r.left + 17, (r.top + r.bottom) / 2, 8, t.ctrlStroke);
        Circle(r.left + 17, (r.top + r.bottom) / 2, 7, has ? c : Mix(c, t.card, 0.6f));
        Text(value.empty() ? std::wstring(dfltName) : Hex(value), R(r.left + 32, r.top, r.right - 28, r.bottom), u.fBody, has ? t.text : t.text3);
        Text(L"\uE790", R(r.right - 26, r.top, r.right - 10, r.bottom), u.fIconSmall, has ? t.text2 : t.text3, DWRITE_TEXT_ALIGNMENT_CENTER);
        AddHit(id, K_BUTTON, r, has);
    };
    colour(ID_PD_COLOR, ID_PD_PICK_COL, 2, lx, L"Colour", look.color, PdDefault(2), L"Accent");
    colour(ID_PD_BODY, ID_PD_PICK_BODY, 3, rx, L"Body", look.body, PdDefault(3), L"Its own");
    reset(ID_PD_RST_COL, lx, !look.color.empty());
    reset(ID_PD_RST_BODY, rx, !look.body.empty());
    y += 44;
    // Opacity and size; then its corners (for the styles on a card).
    auto slider = [&](int id, float x, const wchar_t* label, float frac, const std::wstring& value, bool on) {
        Text(label, R(x, y, x + labW, y + 32), u.fBody, on ? t.text : t.text3);
        Slider(id, R(x + labW - 10, y + 2, x + colW - 40, y + 30), frac, on);
        Text(value, R(x + colW - 40, y, x + colW, y + 32), u.fBody, on ? t.text : t.text3, DWRITE_TEXT_ALIGNMENT_TRAILING);
    };
    slider(ID_PD_OPACITY, lx, L"Opacity", (look.opacity - 0.2f) / 0.8f, Fmt(L"%.0f%%", look.opacity * 100), has);
    slider(ID_PD_SIZE, rx, L"Size", (look.size - 0.7f) / 0.8f, Fmt(L"%.0f%%", look.size * 100), has);
    reset(ID_PD_RST_OPACITY, lx, look.opacity < 1);
    reset(ID_PD_RST_SIZE, rx, look.size != 1);
    y += 44;
    const bool corners = has && Player_HasCorners(style);
    slider(ID_PD_CORNERS, lx, L"Corners", has ? Player_Corners(style) / 40 : 0, corners ? Fmt(L"%.0f", Player_Corners(style)) : L"", corners);
    reset(ID_PD_RST_CORNERS, lx, corners && look.corners >= 0);
    slider(ID_PD_ROTATE, rx, L"Rotate", (look.rotate + 180) / 360, Fmt(L"%.0f\u00B0", look.rotate), has);
    reset(ID_PD_RST_ROTATE, rx, look.rotate != 0);
    // The bottom bar: back to how the style comes, and done.
    const float by = T + H - 48;
    if (has && !look.IsDefault()) {
        const std::wstring all = L"Reset all of " + Player_Styles()[style - 1];
        Link(ID_PD_RESET, R(L + 24, by + 7, L + 24 + TextWidth(all, u.fSmall) + 4, by + 25), all);
    }
    Button(ID_PD_DONE, R(L + W - 24 - 110, by, L + W - 24, by + 32), L"Done", nullptr, true);
}

// The languages, in place of the fonts (Back, Esc or picking one goes back to them).
void OpenLanguageList() {
    fnt.scrollBack = fnt.target;
    fnt.langs = true;
    fnt.langList = Clock_Languages();
    ReleaseFontSamples();
    int at = 0;
    for (size_t i = 0; i < fnt.langList.size(); i++)
        if (fnt.langList[i].code == EditedLook()->lang) at = (int)i;
    fnt.scroll = fnt.target = std::max(0.f, (at - 3) * kLangRowH);  // the next paint keeps it in range
    fnt.note.clear();
}

void CloseLanguageList() {
    fnt.langs = false;
    fnt.langList.clear();
    ReleaseFontSamples();
    fnt.scroll = fnt.target = fnt.scrollBack;
}

// A letter typed in the language list: the next language starting with it.
void LanguageJump(wchar_t letter) {
    const int n = (int)fnt.langList.size(), from = (int)(fnt.target / kLangRowH);
    for (int k = 1; k <= n; k++) {
        const int i = (from + k) % n;
        if (!fnt.langList[i].english.empty() && towupper(fnt.langList[i].english[0]) == letter) {
            FontScrollTo(i * kLangRowH, true);
            return;
        }
    }
}

void CloseFontDialog() {
    if (!fnt.open) return;
    ReleaseFontSamples();
    if (fnt.players) Player_FreePreview();
    if (fnt.walls) {
        Library_Stop();
        g_walls.clear();
    }
    fnt.open = false;
    Clock_ReleasePainter();  // the samples' fonts
}

void UseFontInLook(const std::wstring& key) {  // Mond, in that font
    EditedLook()->style.clear();
    EditedLook()->font = key;
    Log(L"ui: clock font %ls", key.c_str());
    ClockEdited();
}

void FontPick(int i) {
    if (fnt.langs) {
        if (i >= (int)fnt.langList.size()) return;
        ClockLook* look = EditedLook();
        look->lang = fnt.langList[i].code;
        if (Clock_NativeDigits(look->lang).empty()) look->nativeDigits = false;
        Log(L"ui: clock language %ls", fnt.langList[i].english.c_str());
        ClockEdited();
        CloseLanguageList();
        return;
    }
    if (fnt.walls) {  // it's the wallpaper now
        if (i >= (int)g_walls.size()) return;
        const WallRow& w = g_walls[i];
        if (w.ready && !w.info.ok) {
            fnt.note = w.info.fault == VF_GIF ? L"Turn the GIF into a video first (Choose video does it)" : L"This one can't be used as a wallpaper";
            return;
        }
        if (SamePath(w.play, g_settings.video)) return;
        Log(L"ui: %ls is the wallpaper now", w.play.c_str());
        Host_SetVideo(w.play);
        fnt.note = L"Your wallpaper is \u201C" + SongName(w.path) + L"\u201D now";
        return;
    }
    if (fnt.players) {
        g_settings.musicPlayer = i;
        Log(L"ui: desktop music player %ls", i ? Player_Styles()[i - 1].c_str() : L"off");
        Host_SettingsChanged();
    } else if (fnt.dates) {
        EditedLook()->date = i;
        Log(L"ui: date style %d", i);
        ClockEdited();
    } else if (i < (int)fnt.list.size() && fnt.list[i].font.ready) {
        Clock_UseStyle(*EditedLook(), fnt.list[i]);
        Log(L"ui: clock style %ls", fnt.list[i].name.c_str());
        ClockEdited();
    }
}

void FontGet(int i) {
    if (i >= (int)fnt.list.size() || !fnt.list[i].font.getUrl) return;
    ShellExecuteW(u.hwnd, L"open", fnt.list[i].font.getUrl, nullptr, nullptr, SW_SHOWNORMAL);
    fnt.note = L"Download " + fnt.list[i].font.key + L", then come back: VideoBG looks for it in your Downloads";
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
    const std::wstring key = fnt.list[i].font.key;
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
    if (fnt.walls) {
        WallsRemove(i);
        return;
    }
    if (i >= (int)fnt.list.size() || !fnt.list[i].font.removable) return;
    const std::wstring key = fnt.list[i].font.key;
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

// Date style i as it reads today, in a plain font (the clock's own font is a separate choice).
void EnsureDateSample(int i) {
    if (fnt.samples[i] || fnt.tried[i] || !u.rt) return;
    fnt.tried[i] = true;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::wstring text = Clock_DateText(*EditedLook(), i, t);
    if (text.empty()) text = L"NO DATE";
    const float k = u.dpi / 96;
    const std::wstring color =
        Fmt(L"%d,%d,%d", (int)lroundf(u.th.text.r * 255), (int)lroundf(u.th.text.g * 255), (int)lroundf(u.th.text.b * 255));
    ClockImage img;
    if (!Clock_PaintLine(text, L"Syncopate", 11, color, k, &img)) return;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    u.rt->CreateBitmap(D2D1::SizeU((UINT32)img.w, (UINT32)img.h), img.px.data(), (UINT32)img.w * 4, &bp, &fnt.samples[i]);
    fnt.sizes[i] = D2D1::SizeF(img.w / k, img.h / k);
}

// The player's style i as it looks with the song now, at exactly the size it's shown, as big as fits.
void EnsurePlayerSample(int i, float maxW, float maxH) {
    if (fnt.samples[i] || fnt.tried[i] || !u.rt || i < 1) return;
    fnt.tried[i] = true;
    float sw = 0, sh = 0;
    Player_Size(i, &sw, &sh);
    if (sw <= 0) return;
    const float k = u.dpi / 96, s = std::min({1.f, maxW / sw, maxH / sh});
    ClockImage img;
    if (!Player_Paint(i, s * k, false, &img)) return;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    u.rt->CreateBitmap(D2D1::SizeU((UINT32)img.w, (UINT32)img.h), img.px.data(), (UINT32)img.w * 4, &bp, &fnt.samples[i]);
    fnt.sizes[i] = D2D1::SizeF(img.w / k, img.h / k);
}

// Draws style i as the clock, at exactly the size it's shown (so it stays sharp), as big as fits in
// maxW x maxH DIPs.
void EnsureFontSample(int i, float maxW, float maxH) {
    if (fnt.samples[i] || fnt.tried[i] || !u.rt) return;
    fnt.tried[i] = true;
    if (!fnt.list[i].font.ready) return;
    ClockLook l;
    Clock_UseStyle(l, fnt.list[i]);
    l.date = EditedLook()->date;
    l.lang = EditedLook()->lang;
    l.nativeDigits = EditedLook()->nativeDigits;
    l.h24 = EditedLook()->h24;
    l.color = Fmt(L"%d,%d,%d", (int)lroundf(u.th.text.r * 255), (int)lroundf(u.th.text.g * 255), (int)lroundf(u.th.text.b * 255));
    SYSTEMTIME t;
    GetLocalTime(&t);
    const float k = u.dpi / 96;
    ClockImage img;
    for (float size = 0.8f, pass = 0; pass < 3; pass++) {
        l.size = size;
        if (!Clock_Paint(l, k, t, &img) || img.w <= 0) return;
        float fit = std::min(maxW / (img.w / k), maxH / (img.h / k));
        if (fit >= 1) break;
        size *= fit * 0.98f;  // again, at the size that fits (text doesn't shrink quite evenly)
    }
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    u.rt->CreateBitmap(D2D1::SizeU((UINT32)img.w, (UINT32)img.h), img.px.data(), (UINT32)img.w * 4, &bp, &fnt.samples[i]);
    fnt.sizes[i] = D2D1::SizeF(img.w / k, img.h / k);
}

void PaintFontDialog() {
    const Theme& t = u.th;
    Fill(R(0, 0, kW, kH), D2D1::ColorF(0, 0, 0, t.dark ? 0.5f : 0.3f));
    AddHit(ID_FNT_SCRIM, K_AREA, R(0, 0, kW, kH));  // the page underneath can't be used meanwhile
    const int n = PickerRows();
    const float rowH = PickerRowH(), W = kDialogW, content = n * rowH;
    const float listH = std::min(content, fnt.dates || fnt.langs ? 9.4f * rowH : fnt.players ? 3.6f * rowH : fnt.walls ? 5.5f * rowH : 3.4f * rowH);
    const float H = 84 + listH + 8 + 64;
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

    Text(fnt.langs ? L"Clock language" : fnt.dates ? L"Date style" : fnt.players ? L"Music player on the desktop" : fnt.walls ? L"Your wallpapers" : L"Clock style",
         R(L + 24, T + 16, L + W - 24, T + 46), u.fSubtitle, t.text);
    Text(fnt.langs     ? L"The day, month and AM/PM words come from Windows. Type a letter to jump."
         : fnt.players ? L"It shows while My music plays with the video wallpaper. Click one to use it."
         : fnt.walls   ? L"The videos you switch between (the modern look's too). Click one to make it your wallpaper."
                       : L"Click one to use it; your desktop shows it right away.",
         R(L + 24, T + 50, L + W - 24, T + 72), u.fBody, t.text2);
    if (!fnt.dates && !fnt.langs && !fnt.players && !fnt.walls) {  // the clock's language, and its own digits when it has them
        // (Greyed while the style shows no words, or no digits.)
        const ClockLook* look = EditedLook();
        const float bl = L + W - 24 - 190;
        const bool words = Clock_UsesWords(*look), digitsOn = Clock_UsesDigits(*look);
        FontButton(ID_FNT_LANG, R(bl, T + 14, L + W - 24, T + 46), Clock_LanguageName(look->lang), words, L"\uE774");
        const std::wstring digits = Clock_NativeDigits(look->lang);
        if (!digits.empty()) {
            Text(digits.substr(1, 3) + L" digits", R(L + 170, T + 16, bl - 16 - 40 - 10, T + 46), u.fBody, digitsOn ? t.text : t.text3,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
            Toggle(ID_FNT_DIGITS, bl - 16 - 40, T + 30, look->nativeDigits, digitsOn);
        }
    }

    const D2D1_RECT_F view = R(L + 1, T + 84, L + W - 1, T + 84 + listH);
    fnt.maxScroll = std::max(0.f, content - listH);
    fnt.scroll = std::clamp(fnt.scroll, 0.f, fnt.maxScroll);
    fnt.target = std::clamp(fnt.target, 0.f, fnt.maxScroll);
    const float barW = fnt.maxScroll > 0 ? 14 : 0;
    // The styles' names and what they are take the room their longest needs; a sample gets the rest.
    float nameW = 120;
    if (!fnt.dates && !fnt.langs && !fnt.players && !fnt.walls)
        for (const ClockStyleInfo& st : fnt.list)
            if (st.font.ready) nameW = std::max({nameW, TextWidth(st.name, u.fStrong), TextWidth(st.about, u.fSmall)});
    nameW = std::min(nameW + 4, 230.f);
    fnt.nameW = nameW;
    const float boxW = W - 32 - barW - 42 - nameW - 24 - 16, boxH = rowH - 20;  // room for a sample, right of the name
    if (boxW != fnt.boxW) {
        ReleaseFontSamples();
        fnt.boxW = boxW;
    }
    const size_t firstHit = u.hits.size();
    u.rt->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = view.top - fnt.scroll;
    fnt.viewH = listH;
    fnt.view = view;
    for (int i = 0; i < n; i++, y += rowH) {  // all rows, also the ones out of view: Tab goes through them
        if (fnt.langs) {  // a language: its name in English, and in itself
            if (y + rowH <= view.top - rowH || y >= view.bottom + rowH) {  // far out of view: just reachable by Tab
                AddHit(ID_LANG_ROW0 + i, K_BUTTON, R(0, 0, 0, 0));
                continue;
            }
            const ClockLanguage& lg = fnt.langList[i];
            const D2D1_RECT_F r = R(L + 16, y + 2, L + W - 16 - barW, y + rowH - 2);
            const bool sel = lg.code == EditedLook()->lang, hot = IsHot(ID_LANG_ROW0 + i);
            if (sel) {
                Fill(r, Mix(face, t.accent, 0.14f), 6);
                Stroke(r, t.accent, 6);
            } else if (hot) {
                Fill(r, Mix(face, t.text, 0.05f), 6);
            }
            AddHit(ID_LANG_ROW0 + i, K_BUTTON, r);
            const float cy = (r.top + r.bottom) / 2;
            Circle(r.left + 22, cy, 8, sel ? t.accent : t.text2);
            Circle(r.left + 22, cy, sel ? 3.5f : 6.8f, sel ? t.onAccent : hot ? Mix(face, t.text, 0.05f) : face);
            Text(lg.english, R(r.left + 42, r.top, r.left + 300, r.bottom), u.fBody, t.text);
            Text(lg.native, R(r.left + 300, r.top, r.right - 14, r.bottom), u.fBody, t.text2, DWRITE_TEXT_ALIGNMENT_TRAILING);
            continue;
        }
        if (fnt.walls) {  // a wallpaper: its picture, its name and facts; Current, or a bin to take it off
            WallRow& wr = g_walls[i];
            const D2D1_RECT_F r = R(L + 16, y + 3, L + W - 16 - barW, y + rowH - 3);
            const bool cur = SamePath(wr.play, g_settings.video), hot = IsHot(ID_FNT_ROW0 + i);
            if (cur) {
                Fill(r, Mix(face, t.accent, 0.14f), 6);
                Stroke(r, t.accent, 6);
            } else if (hot) {
                Fill(r, Mix(face, t.text, 0.05f), 6);
            }
            AddHit(ID_FNT_ROW0 + i, K_BUTTON, r);
            const D2D1_RECT_F pic = R(r.left + 10, r.top + 8, r.left + 10 + (r.bottom - r.top - 16) * 16 / 9, r.bottom - 8);
            if (!fnt.samples[i] && !fnt.tried[i] && !wr.thumb.empty()) {
                fnt.tried[i] = true;
                D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                u.rt->CreateBitmap(D2D1::SizeU((UINT32)wr.tw, (UINT32)wr.th), wr.thumb.data(), (UINT32)wr.tw * 4, &bp, &fnt.samples[i]);
            }
            Fill(pic, Mix(face, t.text, 0.08f), 5);
            if (fnt.samples[i]) {  // the middle of the picture, filling its box
                const D2D1_SIZE_F sz = fnt.samples[i]->GetSize();
                const float bw = pic.right - pic.left, bh = pic.bottom - pic.top, s = std::max(bw / sz.width, bh / sz.height);
                const float sw = bw / s, sh = bh / s;
                ID2D1RoundedRectangleGeometry* pclip = PushRounded(pic, 5);
                DrawScaled(fnt.samples[i], pic, R((sz.width - sw) / 2, (sz.height - sh) / 2, (sz.width + sw) / 2, (sz.height + sh) / 2));
                PopRounded(pclip);
            }
            const float tx = pic.right + 14, cy = (r.top + r.bottom) / 2;
            Text(SongName(wr.path), R(tx, cy - 22, r.right - 120, cy), u.fStrong, t.text);
            std::wstring about;
            if (!wr.ready) about = L"Reading the video\u2026";
            else if (!wr.info.ok) about = wr.info.fault == VF_GIF ? L"A GIF: turn it into a video first" : wr.info.fault == VF_MISSING ? L"The file isn't there" : L"Can't be used as a wallpaper";
            else about = Fmt(L"%u\u00D7%u  \u00B7  %ls  \u00B7  %.0f MB", wr.info.vw, wr.info.vh, Clock(wr.info.duration).c_str(), wr.info.bytes / 1048576.0);
            if (!SamePath(wr.play, wr.path)) about += L"  \u00B7  a lighter version";
            Text(about, R(tx, cy, r.right - 120, cy + 20), u.fSmall, t.text2);
            if (cur) Text(L"Current", R(r.right - 110, r.top, r.right - 14, r.bottom), u.fSmall, t.good, DWRITE_TEXT_ALIGNMENT_TRAILING);
            else Button(ID_FNT_DEL0 + i, R(r.right - 44, cy - 15, r.right - 12, cy + 15), L"", L"\uE74D");
            continue;
        }
        if (fnt.players) {  // a style: its name and what it is, then how it looks
            const D2D1_RECT_F r = R(L + 16, y + 3, L + W - 16 - barW, y + rowH - 3);
            const bool sel = std::clamp(g_settings.musicPlayer, 0, n - 1) == i, hot = IsHot(ID_FNT_ROW0 + i);
            if (sel) {
                Fill(r, Mix(face, t.accent, 0.14f), 6);
                Stroke(r, t.accent, 6);
            } else if (hot) {
                Fill(r, Mix(face, t.text, 0.05f), 6);
            }
            AddHit(ID_FNT_ROW0 + i, K_BUTTON, r);
            const float cy = (r.top + r.bottom) / 2;
            Circle(r.left + 22, cy, 8, sel ? t.accent : t.text2);
            Circle(r.left + 22, cy, sel ? 3.5f : 6.8f, sel ? t.onAccent : hot ? Mix(face, t.text, 0.05f) : face);
            Text(i ? Player_Styles()[i - 1] : L"None", R(r.left + 42, cy - 22, r.left + 150, cy), u.fStrong, t.text);
            Text(TwoLines(Player_About(i), u.fSmall), R(r.left + 42, cy, r.left + 190, cy + 34), u.fSmall, t.text2);
            EnsurePlayerSample(i, r.right - r.left - 216, rowH - 16);
            if (fnt.samples[i]) {  // 1:1 on whole pixels
                const D2D1_SIZE_F sz = fnt.sizes[i];
                const float x0 = roundf((r.right - 12 - sz.width) * k) / k, y0 = roundf((cy - sz.height / 2) * k) / k;
                D2D1_RECT_F dst = R(x0, y0, x0 + sz.width, y0 + sz.height);
                u.rt->DrawBitmap(fnt.samples[i], &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
            }
            continue;
        }
        if (fnt.dates) {  // just the date, as it reads today
            const D2D1_RECT_F r = R(L + 16, y + 2, L + W - 16 - barW, y + rowH - 2);
            const bool sel = EditedLook()->date == i, hot = IsHot(ID_FNT_ROW0 + i);
            if (sel) {
                Fill(r, Mix(face, t.accent, 0.14f), 6);
                Stroke(r, t.accent, 6);
            } else if (hot) {
                Fill(r, Mix(face, t.text, 0.05f), 6);
            }
            AddHit(ID_FNT_ROW0 + i, K_BUTTON, r);
            const float cy = (r.top + r.bottom) / 2;
            Circle(r.left + 22, cy, 8, sel ? t.accent : t.text2);
            Circle(r.left + 22, cy, sel ? 3.5f : 6.8f, sel ? t.onAccent : hot ? Mix(face, t.text, 0.05f) : face);
            EnsureDateSample(i);
            if (fnt.samples[i]) {  // 1:1 on whole pixels
                const D2D1_SIZE_F sz = fnt.sizes[i];
                const float x0 = roundf((r.left + 46) * k) / k, y0 = roundf((cy - sz.height / 2) * k) / k;
                D2D1_RECT_F dst = R(x0, y0, x0 + sz.width, y0 + sz.height);
                u.rt->DrawBitmap(fnt.samples[i], &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
            }
            continue;
        }
        const ClockStyleInfo& st = fnt.list[i];
        const ClockFontInfo& f = st.font;
        const bool sel = Clock_IsStyle(*EditedLook(), st);
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
        // The name and what it is (then Remove, for the user's copies), centred on the row.
        const float aboutH = f.ready ? 20.f : 38.f, blockH = 22 + aboutH + (f.removable ? 34 : 0), ny = cy - blockH / 2;
        const float nameR = r.left + 42 + fnt.nameW;
        Text(st.name, R(r.left + 42, ny, nameR, ny + 22), u.fStrong, f.ready ? t.text : t.text2);
        Text(st.about, R(r.left + 42, ny + 22, f.ready ? nameR : r.left + 290, ny + 22 + aboutH), f.ready ? u.fSmall : u.fWrap, t.text2);
        if (f.removable) Button(ID_FNT_DEL0 + i, R(r.left + 38, ny + 50, r.left + 134, ny + 78), L"Remove", L"\uE74D");
        if (f.ready) {
            EnsureFontSample(i, boxW, boxH);
            if (fnt.samples[i]) {  // drawn 1:1 on whole pixels (smaller, if it came out bigger than its room)
                D2D1_SIZE_F sz = fnt.sizes[i];
                const float fit = std::min({1.f, boxW / sz.width, boxH / sz.height});
                sz = D2D1::SizeF(sz.width * fit, sz.height * fit);
                const float x0 = roundf((r.right - 16 - sz.width) * k) / k, y0 = roundf((cy - sz.height / 2) * k) / k;
                D2D1_RECT_F dst = R(x0, y0, x0 + sz.width, y0 + sz.height);
                u.rt->DrawBitmap(fnt.samples[i], &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
            }
        } else {
            Button(ID_FNT_ADD0 + i, R(r.right - 12 - 150, cy - 16, r.right - 12, cy + 16), L"Add font file\u2026");
            Button(ID_FNT_GET0 + i, R(r.right - 12 - 150 - 8 - 96, cy - 16, r.right - 12 - 158, cy + 16), L"Get it", L"\uE8A7");
        }
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
    if (fnt.walls && fnt.confirm >= 0 && fnt.confirm < (int)g_walls.size()) {  // before taking a wallpaper off
        Text(L"Take \u201C" + SongName(g_walls[fnt.confirm].path) + L"\u201D off the list? The file stays where it is.",
             R(L + 24, by - 4, L + W - 24 - 236, by + 36), u.fWrap, t.text);
        Button(ID_FNT_NO, R(L + W - 24 - 228, by, L + W - 24 - 126, by + 32), L"Cancel");
        Button(ID_FNT_YES, R(L + W - 24 - 118, by, L + W - 24, by + 32), L"Take off", nullptr, true);
        return;
    }
    if (!fnt.walls && fnt.confirm >= 0 && fnt.confirm < (int)fnt.list.size()) {  // before removing a font
        Text(L"Remove " + fnt.list[fnt.confirm].font.key + L"? Clocks using it go back to Audiowide.", R(L + 24, by - 4, L + W - 24 - 236, by + 36),
             u.fWrap, t.text);
        Button(ID_FNT_NO, R(L + W - 24 - 228, by, L + W - 24 - 126, by + 32), L"Cancel");
        Button(ID_FNT_YES, R(L + W - 24 - 118, by, L + W - 24, by + 32), L"Remove", nullptr, true);
        return;
    }
    float noteL = L + 24;
    if (fnt.walls) {  // more videos for the list
        Button(ID_FNT_OWN, R(L + 24, by, L + 24 + 150, by + 32), L"Add videos\u2026", L"\uE710");
        noteL += 166;
    } else if (!fnt.dates && !fnt.langs && !fnt.players) {  // any font of the user's own, always at hand
        Button(ID_FNT_OWN, R(L + 24, by, L + 24 + 196, by + 32), L"Add your own font\u2026", L"\uE710");
        noteL += 212;
    }
    const bool hint = fnt.note.empty() && !fnt.dates && !fnt.langs && !fnt.players && !fnt.walls;
    Text(hint ? std::wstring(L"A .ttf or .otf file, or a .zip with one") : fnt.note, R(noteL, by - 2, L + W - 24 - 126, by + 34), u.fWrap,
         t.text3);
    Button(ID_FNT_DONE, R(L + W - 24 - 110, by, L + W - 24, by + 32), fnt.langs ? L"Back" : L"Done", nullptr, true);
}

// ---------------------------------------------------------------------------------------
// Clock presets: saved looks any wallpaper's or video's clock can use. Use puts one on the clock the
// page's choices pick (after asking); Edit changes one on the page itself.

std::wstring PresetDetails(const ClockLook& l) {
    return Clock_StyleName(l) + L" \u00B7 " + Fmt(L"%.0f%%", l.size * 100) + L" \u00B7 " + Clock_LanguageName(l.lang);
}

// Each preset's time, in its own font, colours and glow (redrawn when the minute or the preset changes).
struct PresetSample {
    ID2D1Bitmap* bmp = nullptr;
    D2D1_SIZE_F size{};
    std::wstring key;
} prsSample[kClockPresets];

void ReleasePresetSamples() {
    for (PresetSample& ps : prsSample) {
        SafeRelease(ps.bmp);
        ps.key.clear();
    }
}

void EnsurePresetSample(int i, float maxW, float maxH) {
    PresetSample& ps = prsSample[i];
    const ClockLook& l = g_settings.presets[i].look;
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::wstring text = l.h24 ? Fmt(L"%02d:%02d", t.wHour, t.wMinute) : Fmt(L"%d:%02d", (t.wHour + 11) % 12 + 1, t.wMinute);
    const std::wstring key = text + L"|" + Clock_StyleKey(l);
    if (ps.key == key || !u.rt) return;
    SafeRelease(ps.bmp);
    ps.key = key;
    const float k = u.dpi / 96;
    ClockImage img;
    float pt = 14;
    for (int pass = 0; pass < 2; pass++) {  // smaller when it doesn't fit
        if (!Clock_PaintLine(text, SampleFont(l), pt, l.color, k, &img, &l) || img.w <= 0) return;
        const float fit = std::min(maxW / (img.w / k), maxH / (img.h / k));
        if (fit >= 1) break;
        pt *= fit;
    }
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    u.rt->CreateBitmap(D2D1::SizeU((UINT32)img.w, (UINT32)img.h), img.px.data(), (UINT32)img.w * 4, &bp, &ps.bmp);
    ps.size = D2D1::SizeF(img.w / k, img.h / k);
}

void PaintPresetsDialog() {
    const Theme& t = u.th;
    Fill(R(0, 0, kW, kH), D2D1::ColorF(0, 0, 0, t.dark ? 0.5f : 0.3f));
    AddHit(ID_PRS_SCRIM, K_AREA, R(0, 0, kW, kH));  // the page underneath can't be used meanwhile
    const float rowH = 60, W = kDialogW, H = 100 + kClockPresets * rowH + 8 + 64, L = (kW - W) / 2, T = std::max(12.f, (kH - H) / 2);
    const D2D1_RECT_F panel = R(L, T, L + W, T + H);
    const D2D1_COLOR_F face = t.dark ? Rgb(0x2B2B2B) : Rgb(0xFFFFFF), bar = t.dark ? Rgb(0x202020) : Rgb(0xF3F3F3);
    Fill(R(L - 2, T, L + W + 2, T + H + 4), D2D1::ColorF(0, 0, 0, 0.2f), 10);  // shadow
    Fill(panel, face, 8);
    ID2D1RoundedRectangleGeometry* clip = PushRounded(panel, 8);
    Fill(R(L, T + H - 64, L + W, T + H), bar);
    Line(L, T + H - 64, L + W, T + H - 64, t.stroke);
    PopRounded(clip);
    Stroke(panel, t.dark ? Rgb(0x3C3C3C) : Rgb(0xD5D5D5), 8);

    Text(L"Clock presets", R(L + 24, T + 16, L + W - 24, T + 46), u.fSubtitle, t.text);
    Text(L"Clock looks to use again on any wallpaper or video. Use puts one on the clock picked on the page; Edit changes a preset there.",
         R(L + 24, T + 52, L + W - 24, T + 92), u.fWrap, t.text2);

    float y = T + 100;
    for (int i = 0; i < kClockPresets; i++, y += rowH) {
        const ClockPreset& p = g_settings.presets[i];
        const D2D1_RECT_F r = R(L + 16, y, L + W - 16, y + rowH - 6);
        const float cy = (r.top + r.bottom) / 2, nx = r.left + 106, bx = r.right - 8, bt = cy - 15, bb = cy + 15;
        if (prs.confirm == i) {
            Fill(r, Mix(face, t.accent, 0.14f), 6);
            Stroke(r, t.accent, 6);
        }
        // Its time as a little clock, on a tile dark or light enough for its colour.
        const D2D1_RECT_F tile = R(r.left + 8, r.top + 6, r.left + 94, r.bottom - 6);
        if (p.used) {
            BYTE cr = 255, cg = 255, cb = 255;
            ParseColor(p.look.color, &cr, &cg, &cb);
            const bool lightText = 0.299f * cr + 0.587f * cg + 0.114f * cb > 140;
            Fill(tile, lightText ? Rgb(0x1C1C1C) : Rgb(0xE8E8E8), 6);
            Stroke(tile, t.ctrlStroke, 6);
            EnsurePresetSample(i, tile.right - tile.left - 8, tile.bottom - tile.top - 4);
            if (prsSample[i].bmp) {  // 1:1 on whole pixels
                const D2D1_SIZE_F sz = prsSample[i].size;
                const float k = u.dpi / 96, x0 = roundf((tile.left + tile.right - sz.width) / 2 * k) / k,
                            y0 = roundf((tile.top + tile.bottom - sz.height) / 2 * k) / k;
                D2D1_RECT_F dst = R(x0, y0, x0 + sz.width, y0 + sz.height);
                u.rt->DrawBitmap(prsSample[i].bmp, &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
            }
        } else {
            Fill(tile, Mix(face, t.text, 0.04f), 6);
            Stroke(tile, t.ctrlStroke, 6);
            Text(L"\uE710", tile, u.fIconSmall, t.text3, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (prs.rename == i) {  // typing its name
            const D2D1_RECT_F nb = R(nx - 6, cy - 16, bx - 248, cy + 16);
            Fill(nb, face, 4);
            Stroke(nb, t.accent, 4, 2);
            const float tw = TextWidth(prs.name, u.fBody);
            if (prs.nameFresh && !prs.name.empty()) Fill(R(nb.left + 6, cy - 9, nb.left + 8 + tw, cy + 9), Mix(t.accent, face, 0.55f), 2);
            Text(prs.name, R(nb.left + 7, nb.top, nb.right - 6, nb.bottom), u.fBody, t.text);
            if (!prs.nameFresh) Line(nb.left + 8 + tw, cy - 8, nb.left + 8 + tw, cy + 8, t.text);
            AddHit(ID_PRS_NAME, K_AREA, nb);
        } else {
            Text(PresetName(i), R(nx, r.top + 5, bx - 248, cy + 1), u.fStrong, p.used ? t.text : t.text2);
            Text(p.used ? PresetDetails(p.look) : std::wstring(L"Empty: Edit makes one from your clock"), R(nx, cy, bx - 248, r.bottom - 4),
                 u.fSmall, t.text2);
        }
        // Use, Edit, Rename and Delete; an empty one can only be edited.
        Button(ID_PRS_USE0 + i, R(bx - 240, bt, bx - 168, bb), L"Use", L"\uE73E", false, p.used);
        Button(ID_PRS_EDIT0 + i, R(bx - 160, bt, bx - 88, bb), L"Edit", L"\uE70F");
        Button(ID_PRS_REN0 + i, R(bx - 80, bt, bx - 44, bb), L"", L"\uE8AC", false, p.used);
        Button(ID_PRS_DEL0 + i, R(bx - 36, bt, bx, bb), L"", L"\uE74D", false, p.used);
    }

    // Bottom bar: Close, or the question before Use replaces a clock or Delete deletes a preset.
    const float by = T + H - 48;
    if (prs.confirm >= 0) {
        Text(prs.confirmDel ? L"Delete " + PresetName(prs.confirm) + L"? It can't be undone."
                            : L"Use " + PresetName(prs.confirm) + L" for " + PickedLookName() + L"? It replaces that clock's look.",
             R(L + 24, by - 4, L + W - 24 - 236, by + 36), u.fWrap, t.text);
        Button(ID_PRS_NO, R(L + W - 24 - 228, by, L + W - 24 - 126, by + 32), L"Cancel");
        Button(ID_PRS_YES, R(L + W - 24 - 118, by, L + W - 24, by + 32), prs.confirmDel ? L"Delete" : L"Replace", nullptr, true);
    } else {
        Text(prs.note, R(L + 24, by - 2, L + W - 24 - 130, by + 34), u.fWrap, t.text3);
        Button(ID_PRS_CLOSE, R(L + W - 24 - 110, by, L + W - 24, by + 32), L"Close");
    }
}

void PaintFeedbackDialog() {
    const Theme& t = u.th;
    Fill(R(0, 0, kW, kH), D2D1::ColorF(0, 0, 0, t.dark ? 0.5f : 0.3f));
    AddHit(ID_FB_SCRIM, K_AREA, R(0, 0, kW, kH));  // the page underneath can't be used meanwhile
    const D2D1_RECT_F panel = FeedbackLayout();
    FbPlaceEdits();
    const float L = panel.left, T = panel.top, W = panel.right - L, H = panel.bottom - T;
    const D2D1_COLOR_F face = t.dark ? Rgb(0x2B2B2B) : Rgb(0xFFFFFF), bar = t.dark ? Rgb(0x202020) : Rgb(0xF3F3F3);
    Fill(R(L - 2, T, L + W + 2, T + H + 4), D2D1::ColorF(0, 0, 0, 0.2f), 10);  // shadow
    Fill(panel, face, 8);
    ID2D1RoundedRectangleGeometry* clip = PushRounded(panel, 8);
    Fill(R(L, T + H - 64, L + W, T + H), bar);
    Line(L, T + H - 64, L + W, T + H - 64, t.stroke);
    PopRounded(clip);
    Stroke(panel, t.dark ? Rgb(0x3C3C3C) : Rgb(0xD5D5D5), 8);

    Text(fb.bug ? L"Report a problem" : L"Suggest a feature", R(L + 24, T + 16, L + W - 24, T + 46), u.fSubtitle, t.text);
    Text(fb.bug ? L"Tell the developer what happened. No account or personal details needed."
                : L"Your idea goes straight to the developer. No account or personal details needed.",
         R(L + 24, T + 50, L + W - 24, T + 72), u.fBody, t.text2);
    // A text box: its label above, and a frame around Windows' own box (which draws the text).
    auto box = [&](int id, const D2D1_RECT_F& r, HWND edit, const wchar_t* label) {
        Text(label, R(r.left, r.top - 24, r.right, r.top - 4), u.fBody, t.text);
        const bool focused = edit && GetFocus() == edit;
        Fill(r, face, 4);
        Stroke(r, focused ? t.accent : t.ctrlStroke, 4, focused ? 2.f : 1.f);
        AddHit(id, K_AREA, r);
    };
    box(ID_FB_TEXT, fb.boxText, fb.text, fb.bug ? L"What went wrong?" : L"What would you like VideoBG to do?");
    if (fb.bug) {
        box(ID_FB_STEPS, fb.boxSteps, fb.steps, L"What were you doing when it happened? (optional)");
        float y = fb.boxSteps.bottom + 14;
        Text(L"How often does it happen?", R(L + 24, y, L + 260, y + 32), u.fBody, t.text);
        Seg(ID_FB_OFTEN, R(L + W - 24 - 330, y, L + W - 24, y + 32), {L"Once", L"Sometimes", L"Every time"}, fb.often);
        y += 32 + 14;
        Toggle(ID_FB_LOG, L + 24, y + 14, fb.withLog);
        Text(L"Include VideoBG's log (it helps find the cause)", R(L + 76, y, L + W - 24 - 130, y + 28), u.fBody, t.text);
        Link(ID_FB_PREVIEW, R(L + W - 24 - 130, y + 5, L + W - 24, y + 23), L"See what's sent");
        box(ID_FB_EMAIL, fb.boxEmail, fb.email, L"Your email, if you'd like a reply (optional)");
    }
    // Pictures: added from files, or a screenshot pasted into a text box; then shown in a strip under
    // the rest, each with a cross that takes it out (the strip scrolls sideways if they don't fit).
    {
        const D2D1_RECT_F b = fb.picsButton;
        const size_t n = fb.pictures.size();
        Button(ID_FB_PICS, b, L"Add pictures", L"\uEB9F", false, n < kFeedbackPictures && !fb.sending);
        float px = b.right + 12;
        if (n && !fb.bug) {
            const std::wstring count = Fmt(L"%zu of %zu", n, kFeedbackPictures);
            Text(count, R(px, b.top, px + 60, b.bottom), u.fSmall, t.text2);
            px += TextWidth(count, u.fSmall) + 14;
        }
        if (n > 1 && px + 76 <= L + W - 24) {
            {
                Link(ID_FB_PICS_CLEAR, R(px, b.top + 7, px + 76, b.top + 25), L"Remove all");
                px += 90;
            }
        }
        if (!fb.bug) Text(L"or paste a screenshot into the box (Ctrl+V)", R(px, b.top, L + W - 24, b.bottom), u.fSmall, t.text2);
        if (n) {
            const D2D1_RECT_F& s = fb.strip;
            float total = 0;
            for (const FeedbackPicture& p : fb.pictures) total += (p.thumbH ? std::clamp(72.f * p.thumbW / p.thumbH, 40.f, 160.f) : 96.f) + 10;
            fb.picMax = std::max(0.f, total - 10 - (s.right - s.left));
            fb.picScroll = std::clamp(fb.picScroll, 0.f, fb.picMax);
            const size_t firstHit = u.hits.size();
            u.rt->PushAxisAlignedClip(R(s.left, s.top - 2, s.right, s.bottom + 2), D2D1_ANTIALIAS_MODE_ALIASED);
            float x = s.left - fb.picScroll;
            for (size_t i = 0; i < n; i++) {
                const FeedbackPicture& p = fb.pictures[i];
                const float pw = p.thumbH ? std::clamp(72.f * p.thumbW / p.thumbH, 40.f, 160.f) : 96.f;
                const D2D1_RECT_F r = R(x, s.top, x + pw, s.bottom);
                ID2D1Bitmap* bmp = nullptr;
                if (!p.thumb.empty()) {
                    const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                    u.rt->CreateBitmap(D2D1::SizeU((UINT32)p.thumbW, (UINT32)p.thumbH), p.thumb.data(), (UINT32)p.thumbW * 4, &bp, &bmp);
                }
                ID2D1RoundedRectangleGeometry* round = PushRounded(r, 6);
                if (bmp) u.rt->DrawBitmap(bmp, &r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);  // (as wide as it is: not cropped)
                else Fill(r, t.ctrl);
                PopRounded(round);
                SafeRelease(bmp);
                Stroke(r, t.ctrlStroke, 6);
                // Its cross, on its top-right corner.
                const float cx = r.right - 12, cy = r.top + 12;
                const bool hot = IsHot(ID_FB_PIC_X0 + (int)i);
                Circle(cx, cy, 10, D2D1::ColorF(0, 0, 0, hot ? 0.8f : 0.55f));
                Text(L"\uE711", R(cx - 10, cy - 10, cx + 10, cy + 10), u.fIconSmall, D2D1::ColorF(1, 1, 1, hot ? 1.f : 0.85f), DWRITE_TEXT_ALIGNMENT_CENTER);
                AddHit(ID_FB_PIC_X0 + (int)i, K_BUTTON, R(cx - 11, cy - 11, cx + 11, cy + 11), !fb.sending);
                x += pw + 10;
            }
            u.rt->PopAxisAlignedClip();
            for (size_t h = firstHit; h < u.hits.size(); h++) {  // what's scrolled out of the strip can't be clicked
                D2D1_RECT_F& hr = u.hits[h].r;
                hr = R(std::max(hr.left, s.left), hr.top, std::min(hr.right, s.right), hr.bottom);
                if (hr.right <= hr.left) hr = R(0, 0, 0, 0);
            }
        }
    }
    const float by = T + H - 48;
    const bool canSend = !fb.sending && FbGet(fb.text).size() >= 3;
    Text(fb.sending ? std::wstring(L"Sending\u2026") : fb.note, R(L + 24, by - 4, L + W - 24 - 236, by + 36), u.fWrap,
         fb.sending ? t.text2 : t.warn);
    Button(ID_FB_CANCEL, R(L + W - 24 - 228, by, L + W - 24 - 126, by + 32), L"Cancel");
    Button(ID_FB_SEND, R(L + W - 24 - 118, by, L + W - 24, by + 32), L"Send", L"\uE724", true, canSend);
}

// Hovering the sidebar's memory numbers: why they rise while the video plays.
void PaintMemoryTip() {
    const Theme& t = u.th;
    const float w = 330, x = u.memInfo.right + 10, bottom = u.memInfo.bottom + 4;
    const std::wstring text =
        L"Wallpaper RAM climbs during the first minute of playback, then stays level: it's the video's decoded frames, "
        L"kept ready so playback is smooth. It isn't a leak.\n\nWhen the wallpaper pauses (covered by an app, PC locked or screen off), "
        L"VideoBG lets go of those frames and it drops to a few MB.\n\nA lighter version (Video page, "
        L"Versions) needs less while it plays.";
    const float h = 186, y = bottom - h;
    const D2D1_RECT_F r = R(x, y, x + w, bottom);
    Fill(R(r.left - 1, r.top + 1, r.right + 1, r.bottom + 3), D2D1::ColorF(0, 0, 0, 0.18f), 8);  // shadow
    Fill(r, t.dark ? Rgb(0x2B2B2B) : Rgb(0xFFFFFF), 8);
    Stroke(r, t.dark ? Rgb(0x3C3C3C) : Rgb(0xD5D5D5), 8);
    Text(text, R(r.left + 14, r.top + 12, r.right - 14, r.bottom - 10), u.fWrap, t.text);
}

// Hovering Classic / Modern: what each is, and what the modern one costs while it's open (measured
// 2026-10-15 at 1920x1080: the classic window about 45 MB, the picker about 180 MB, RAM and graphics
// memory together; both give it back when they close).
void PaintLookTip() {
    const Theme& t = u.th;
    const float w = 360, right = kW - 24 - 40 - 12 - 60 - 16, top = 60;
    const std::wstring text =
        L"Classic is this window. Modern brings up your wallpapers full screen (the shortcut and the tray icon open them), "
        L"with this window for the rest of the settings.\n\nWhile it's open, the modern look uses about 180 MB of memory (RAM "
        L"and graphics memory together), about four times this window's 45 MB. Both give it back when they close, so in "
        L"between, the two looks cost the same.";
    const D2D1_RECT_F r = R(right - w, top, right, top + 152);
    Fill(R(r.left - 1, r.top + 1, r.right + 1, r.bottom + 3), D2D1::ColorF(0, 0, 0, 0.18f), 8);  // shadow
    Fill(r, t.dark ? Rgb(0x2B2B2B) : Rgb(0xFFFFFF), 8);
    Stroke(r, t.dark ? Rgb(0x3C3C3C) : Rgb(0xD5D5D5), 8);
    Text(text, R(r.left + 14, r.top + 12, r.right - 14, r.bottom - 10), u.fWrap, t.text);
}

void PaintMain() {
    PaintHeader();
    PaintSidebar();
    const float x = kContentX, w = kW - 24 - kContentX;
    switch (u.nav) {
        case NAV_VIDEO: PaintVideoPage(x, w); break;
        case NAV_SOUND: PaintSoundPage(x, w); break;
        case NAV_PLAYBACK: PaintPlaybackPage(x, w); break;
        case NAV_CLOCK: PaintClockPage(x, w); break;
        case NAV_GENERAL: PaintGeneralPage(x, w); break;
    }
    if (IsHot(ID_MEMINFO) && !(dlg.open || ver.open || fnt.open || prs.open || fb.open)) PaintMemoryTip();
    if (IsHot(ID_LOOK) && !(dlg.open || ver.open || fnt.open || prs.open || fb.open)) PaintLookTip();
    if (pd.open) PaintPlayerDialog();
    if (dlg.open && !u.picking) PaintColorDialog();
    if (ver.open) PaintVersionsDialog();
    if (fnt.open) PaintFontDialog();
    if (prs.open) PaintPresetsDialog();
    if (fb.open) PaintFeedbackDialog();
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
        DrawScaled(u.frame, u.imgRc, src);
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
    Text(L"Match screen shape", R(76, by, 240, by + 40), u.fBody, t.text);
    Text(u.lockAspect ? L"Your desktop shows exactly this part" : L"Any shape, stretched to fill your screen",
         R(240, by, kW - 24 - 296 - 16, by + 40), u.fSmall, t.text2);
    float bx = kW - 24;
    Button(ID_C_APPLY, R(bx - 96, by + 4, bx, by + 36), L"Apply", nullptr, true);
    Button(ID_C_CANCEL, R(bx - 96 - 8 - 88, by + 4, bx - 104, by + 36), L"Cancel");
    Button(ID_C_RESET, R(bx - 96 - 8 - 88 - 8 - 88, by + 4, bx - 200, by + 36), L"Reset");
}

void EnterCrop() {
    u.page = 1;
    u.edit = g_settings.crop;
    // A crop that isn't stretched shows as the screen-shaped middle of it: that's what it starts as.
    u.lockAspect = !u.edit.stretch;
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
    if (prs.edit < 0) u.clockEdit = Host_ClockLive() ? 0 : 1;  // kept while a preset is edited
    if (u.stillPx.empty() && !u.stillLoading) {
        u.stillLoading = true;
        Job j;
        j.kind = 5;
        u.worker.Post(j);
    }
}

// Saves an edit of the clock look; the host moves/recolours the clock if that look is showing. A
// preset being edited is only kept on Save: meanwhile the desktop shows it instead.
void ClockEdited() {
    SafeRelease(u.clockBmp);
    Host_ClockPreview(prs.edit >= 0 ? &prs.look : nullptr);
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
        // Grab the clock where it was clicked (only the clock itself is dragged).
        u.clockGrab = D2D1::Point2F(mx - (cr.left + cr.right) / 2, my - (cr.top + cr.bottom) / 2);
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

// Turning the clock by its handle, round the clock's centre on the preview.
void ClockRotate(float mx, float my) {
    ClockLook* look = EditedLook();
    const D2D1_RECT_F cr = ClockRectIn(u.clockBox, *look);
    const float r = Clock_RotateTo(u.rotFrom, u.rotPointer, Clock_PointerAngle((cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2, mx, my),
                                   GetKeyState(VK_SHIFT) < 0);
    if (r == look->rotate) return;
    look->rotate = r;
    LiveClock();  // the real clock follows
}

void ClockNudge(UINT vk) {
    ClockLook* look = EditedLook();
    float step = (GetKeyState(VK_SHIFT) < 0 ? 1.f : 10.f);  // screen pixels; Shift for fine steps
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

// ---- Clock presets: interaction

void OpenPresets() {
    prs.open = true;
    prs.confirm = prs.rename = -1;
    prs.note.clear();
}

void PresetRenameDone(bool keep);

void ClosePresets() {
    PresetRenameDone(true);
    prs.open = false;
    prs.confirm = -1;
    ReleasePresetSamples();
}

// Puts a preset on the look the page's choices pick; that look keeps its Show / Hide.
void PresetUse(int i) {
    const ClockPreset& p = g_settings.presets[i];
    if (!p.used) return;
    ClockLook* look = PickedLook();
    const bool show = look->show;
    *look = p.look;
    look->show = show;
    Log(L"ui: clock preset %d used for %ls", i + 1, PickedLookName().c_str());
    ClosePresets();
    ClockEdited();
    ClockNote(PresetName(i) + L" is now " + PickedLookName() + (show ? L"" : L" (hidden: Show shows it)"));
}

// Edits a preset on the page: from its own look, or from the clock on the page when it's empty.
void PresetEdit(int i) {
    const ClockPreset& p = g_settings.presets[i];
    prs.look = p.used ? p.look : *PickedLook();
    prs.look.show = true;
    prs.edit = i;
    ClosePresets();
    u.keySlider = 0;
    Log(L"ui: editing clock preset %d", i + 1);
    ClockEdited();  // the desktop shows it from now on
}

void PresetEditEnd(bool save) {
    const int i = prs.edit;
    if (i < 0) return;
    if (save) {
        ClockPreset& p = g_settings.presets[i];
        p.used = true;
        p.look = prs.look;
        p.look.show = true;
        SavePresets(g_settings);
    }
    prs.edit = -1;
    Log(L"ui: clock preset %d %ls", i + 1, save ? L"saved" : L"left as it was");
    ClockEdited();  // the desktop goes back to the saved looks
    ClockNote(save ? L"Saved " + PresetName(i) : PresetName(i) + L" is left as it was");
}

// While editing: start again from the clock the page's choices pick.
void PresetFromCurrent() {
    prs.look = *PickedLook();
    prs.look.show = true;
    ClockEdited();
    ClockNote(L"Started again from " + PickedLookName());
}

void PresetDelete(int i) {
    if (!g_settings.presets[i].used) return;
    prs.note = L"Deleted " + PresetName(i);
    g_settings.presets[i] = ClockPreset{};
    SavePresets(g_settings);
    if (prs.confirm == i) prs.confirm = -1;
    Log(L"ui: clock preset %d deleted", i + 1);
}

// Replace or Delete, after asking.
void PresetConfirmed() {
    const int i = prs.confirm;
    if (i < 0) return;
    if (prs.confirmDel) PresetDelete(i);
    else PresetUse(i);
    prs.confirm = -1;
}

void PresetRenameStart(int i) {
    if (!g_settings.presets[i].used) return;
    prs.rename = i;
    prs.name = PresetName(i);
    prs.nameFresh = true;
    prs.confirm = -1;
}

// Keeps (or drops) the name typed; no name is "Preset N" again.
void PresetRenameDone(bool keep) {
    const int i = prs.rename;
    if (i < 0) return;
    prs.rename = -1;
    if (!keep) return;
    std::wstring n = prs.name;
    while (!n.empty() && iswspace(n.back())) n.pop_back();
    while (!n.empty() && iswspace(n.front())) n.erase(0, 1);
    g_settings.presets[i].name = n == Fmt(L"Preset %d", i + 1) ? std::wstring() : n;
    SavePresets(g_settings);
}

// A key typed into a preset's name: a letter, Backspace or Ctrl+V.
void PresetNameChar(wchar_t c) {
    if (c == 8) {
        if (prs.nameFresh) prs.name.clear();
        else if (!prs.name.empty()) prs.name.pop_back();
    } else if (c == 22 || (c >= 32 && c != 127)) {
        if (prs.nameFresh) prs.name.clear();
        const std::wstring add = c == 22 ? ClipboardText() : std::wstring(1, c);
        for (wchar_t ch : add) {
            if (ch == L'\r' || ch == L'\n') break;
            if (ch >= 32 && prs.name.size() < kPresetNameMax) prs.name += ch;
        }
    } else {
        return;
    }
    prs.nameFresh = false;
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
    DlgTarget() = dlg.target != 0 && dlg.sameAsText ? std::wstring() : DlgColor();
    if (dlg.target >= 2) Player_Update();
    else LiveClock(force);
}

void DlgSetRgb(BYTE r, BYTE g, BYTE b, bool typing = false) {
    RgbToHsv(r, g, b, &dlg.h, &dlg.s, &dlg.v);
    dlg.exact = Fmt(L"%d,%d,%d", r, g, b);
    dlg.sameAsText = false;
    if (!typing) dlg.hexEdit = false;
    DlgApply(true);
}

// Edits the text's (0) or the glow's (1) colour: the colour field starts from it.
void DlgSetTarget(int target) {
    if (target >= 2) {  // the player's
        dlg.target = target;
        dlg.sameAsText = DlgTarget().empty();
        dlg.hexEdit = false;
        BYTE r = 255, g = 255, b = 255;
        ParseColor(dlg.sameAsText ? PdDefault(target) : DlgTarget(), &r, &g, &b);
        RgbToHsv(r, g, b, &dlg.h, &dlg.s, &dlg.v);
        dlg.exact = Fmt(L"%d,%d,%d", r, g, b);
        return;
    }
    ClockLook* look = EditedLook();
    dlg.target = target;
    dlg.sameAsText = target == 1 && look->glowColor.empty();
    dlg.hexEdit = false;
    BYTE r = 255, g = 255, b = 255;
    ParseColor(dlg.sameAsText || target == 0 ? look->color : look->glowColor, &r, &g, &b);
    RgbToHsv(r, g, b, &dlg.h, &dlg.s, &dlg.v);
    dlg.exact = Fmt(L"%d,%d,%d", r, g, b);
}

void OpenColorDialog(int target) {
    ClockLook* look = EditedLook();
    int slot = dlg.slot;
    dlg = ColorDialog{};
    dlg.open = true;
    dlg.slot = slot;
    dlg.beforeText = target >= 2 ? PdLook().color : look->color;  // (the player's: its colour and body)
    dlg.beforeGlow = target >= 2 ? PdLook().body : look->glowColor;
    DlgSetTarget(target);
    u.picking = false;
    u.pickColor.clear();
}

// The eyedropper straight from the page (the colour dialog stays shut): the next click on the
// wallpaper sets that colour (0 the clock's text, 2 the player's colour, 3 its body); Esc cancels.
void StartPick(int target) {
    dlg.target = target;
    if (target < 2) dlg.sameAsText = false;
    u.picking = u.pickDirect = true;
    u.pickColor.clear();
}

void CloseColorDialog(bool keep) {
    if (dlg.target >= 2) {  // the player's colours: kept, or put back
        if (!keep) {
            PdLook().color = dlg.beforeText;
            PdLook().body = dlg.beforeGlow;
        }
        dlg.open = dlg.hexEdit = u.picking = false;
        u.pickColor.clear();
        SaveSettings(g_settings);
        Player_Update();
        return;
    }
    ClockLook* look = EditedLook();
    if (!keep) {
        look->color = dlg.beforeText;
        look->glowColor = dlg.beforeGlow;
    } else {
        if (look->color != dlg.beforeText) Log(L"ui: clock text colour set to %ls", look->color.c_str());
        if (look->glowColor != dlg.beforeGlow)
            Log(L"ui: clock glow colour set to %ls", look->glowColor.empty() ? L"the text's" : look->glowColor.c_str());
    }
    dlg.open = dlg.hexEdit = u.picking = false;
    u.pickColor.clear();
    ClockEdited();  // saves, and the desktop shows the kept (or the old) colour
}

void DlgSameAsText(bool on) {
    dlg.sameAsText = on;
    if (on) {
        BYTE r = 255, g = 255, b = 255;
        ParseColor(dlg.target >= 2 ? PdDefault(dlg.target) : EditedLook()->color, &r, &g, &b);
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
            if (u.picking) {
                u.picking = false;
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
    return false;
}

// ---------------------------------------------------------------------------------------

void PaintFocus();
void FocusMove(int dir);

void Paint() {
    if (!EnsureTarget()) return;
    u.hits.clear();
    u.rt->BeginDraw();
    u.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    D2D1_COLOR_F bg = u.th.bg;
    u.rt->Clear(&bg);
    if (u.page == 1) PaintCrop();
    else PaintMain();
    if (u.focusFirst) {
        u.focusFirst = false;
        FocusMove(1);
    }
    PaintFocus();
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

// The wallpaper waits on the frame being picked (and plays on when the timer lets it go).
void HoldWallpaper() {
    KillTimer(u.hwnd, TIMER_HOLD);
    if (!u.held) Host_HoldWallpaper(u.held = true);
}

void ReleaseWallpaper() {
    KillTimer(u.hwnd, TIMER_HOLD);
    if (u.held) Host_HoldWallpaper(u.held = false);
}

void OnSlider(int id, float frac, bool final) {
    switch (id) {
        case ID_PD_OPACITY:  // the music player's look: on the desktop as it moves, saved when let go
        case ID_PD_SIZE:
        case ID_PD_CORNERS:
        case ID_PD_ROTATE:
            if (g_settings.musicPlayer > 0) {
                Settings::PlayerLook& pl = PdLook();
                if (id == ID_PD_OPACITY) pl.opacity = roundf((0.2f + frac * 0.8f) * 100) / 100;
                else if (id == ID_PD_SIZE) pl.size = roundf((0.7f + frac * 0.8f) * 20) / 20;
                else if (id == ID_PD_ROTATE) {  // whole degrees; near a quarter or a 15-degree step it settles there
                    float r = roundf(frac * 360 - 180);
                    const float quarter = roundf(r / 90) * 90, step = roundf(r / 15) * 15;
                    if (fabsf(r - quarter) < 5) r = quarter;
                    else if (fabsf(r - step) < 2) r = step;
                    pl.rotate = r <= -180 ? 180 : r;
                }
                else pl.corners = roundf(frac * 40);
                Player_Update();
                if (final) SaveSettings(g_settings);
            }
            break;
        case ID_SPEED: {
            int v = 25 + (int)lround(frac * 175 / 5) * 5;  // 5% steps
            if (std::abs(v - 100) <= 5) v = 100;            // snap to normal speed
            g_settings.speed = v;
            if (final) Host_SettingsChanged();
            break;
        }
        case ID_VOLUME:
            g_settings.volume = (int)lround(frac * 100);
            g_settings.videoMuted = false;  // turning the volume up or down means listening
            Host_LiveVolume(g_settings.volume);  // heard while dragging; saved when let go
            if (final) Host_SettingsChanged();
            break;
        case ID_MUSIC_SEEK:
            u.seekFrac = frac;
            if (final) {
                Host_MusicCommand(MC_SEEK, (LPARAM)(frac * Host_MusicState().durMs));
                u.seekFrac = -1;
            }
            break;
        case ID_TIMELINE:
            // The wallpaper shows that frame and holds still on it while the slider moves; it plays on a
            // second after it's let go.
            SetPreviewTimeFromFrac(frac);
            HoldWallpaper();
            if (final || GetTickCount() - u.lastSeek >= 60) {
                u.lastSeek = GetTickCount();
                if (u.infoOk && u.info.duration > 0) Host_SeekWallpaper(PreviewTime(), final);
            }
            if (final) {
                Host_FrameChanged();  // this video's frame: saved, and the lock screen follows
                SetTimer(u.hwnd, TIMER_HOLD, 1000, nullptr);
            }
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
            g_settings.videoMuted = false;
            Host_LiveVolume(g_settings.volume);
            break;
        case ID_MUSIC_SEEK: {  // five seconds a step
            const MusicState ms = Host_MusicState();
            if (!ms.durMs) return false;
            Host_MusicCommand(MC_SEEK, (LPARAM)std::clamp((int)ms.posMs + dir * 5000, 0, (int)ms.durMs - 1000));
            break;
        }
        case ID_TIMELINE: {
            if (!u.infoOk || u.info.duration <= 0) return false;
            double step = u.info.fps > 0 ? 1 / u.info.fps : 0.1;  // one frame
            g_settings.previewTime = std::clamp(PreviewTime() + dir * step, 0.0, std::max(0.0, u.info.duration - step));
            RequestFrame();
            HoldWallpaper();
            Host_SeekWallpaper(PreviewTime(), true);
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
        case ID_TIMELINE:
            Host_FrameChanged();
            SetTimer(u.hwnd, TIMER_HOLD, 1000, nullptr);
            break;
        default: ClockEdited();
    }
}

void OnSeg(int id, int i) {
    switch (id) {
        case ID_FPS: g_settings.fpsCap = kFpsValues[i]; break;
        case ID_GPU:
            g_settings.gpu = i + 1;
            Log(L"ui: graphics chip set to %ls", i ? L"high performance" : L"power saving");
            Host_SettingsChanged();  // the wallpaper restarts on that chip
            QueryGpu();
            return;
        case ID_MONITORS:
            g_settings.monitors = i;
            QueryScreen();  // the displays the versions are sized for
            break;
        case ID_COVER: g_settings.pauseCover = i; break;
        case ID_SOUND:
            if (i == g_settings.sound) return;
            if (i == 2 && Host_Songs().empty()) {  // "my music" needs songs first
                Host_PickMusic(u.hwnd, false);
                return;
            }
            if (i == 2) g_settings.musicPlaying = false;  // ready to play, but silent until Play is pressed
            if (g_settings.sound == 2) Host_KeepMusicPlace();  // leaving My music: it carries on from here next time
            g_settings.sound = i;
            if (i != 0 && g_settings.volume == 0) g_settings.volume = 60;
            Log(L"ui: sound set to %ls", i == 0 ? L"nothing" : i == 1 ? L"video's audio" : L"my music");
            break;
        case ID_BATTERY: g_settings.onBattery = i; break;
        case ID_LAUNCH: g_settings.launch = i; break;
        case ID_LOOK:
            if (i == g_settings.look) return;
            Host_SetLook(i);
            InvalidateRect(u.hwnd, nullptr, FALSE);
            return;
        case ID_CLK_MODE:
            u.clockEdit = i;
            return;
        case ID_DLG_TARGET:
            if (i + (dlg.target >= 2 ? 2 : 0) != dlg.target) DlgSetTarget(i + (dlg.target >= 2 ? 2 : 0));
            return;
        case ID_FB_OFTEN: fb.often = i; return;
        case ID_KEEPSOUND: g_settings.keepSound = i == 1; break;
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

// ---- My music: shuffling and rearranging the play order. The order chosen (as Windows listed the
// files) is the only "unshuffled" one: shuffling makes a new order, and moving a song by hand
// does too; shuffle off goes back to the order chosen.

void MusicShuffle() { Host_ShuffleMusic(Host_Songs().size()); }

void OnClick(int id);

// Puts a song at place `to` in the play order (the others move along).
void MusicPlace(int song, int to) { Host_PlaceSong(Host_Songs().size(), song, to); }

// Moves a song one place up (-1) or down (1) in the play order (Alt+arrows on a focused song).
void MusicMove(int song, int dir) {
    const std::vector<int> order = MusicOrderFor(g_settings, Host_Songs().size());
    for (size_t p = 0; p < order.size(); p++)
        if (order[p] == song) MusicPlace(song, (int)p + dir);
}

// Glides the playlist to a song (playlist index), putting it in the middle of the list.
void SongGlide(int song) {
    const std::vector<int> order = MusicOrderFor(g_settings, Host_Songs().size());
    for (size_t p = 0; p < order.size(); p++)
        if (order[p] == song) {
            const float viewH = u.songView.bottom - u.songView.top;
            u.songTarget = std::clamp(p * kSongRowH - (viewH - kSongRowH) / 2, 0.f, u.songMax);
            SetTimer(u.hwnd, TIMER_SONGS, 15, nullptr);
        }
}

// The search: the list glides to the song that matches what's typed best (it only looks; nothing moves).
void SongSearch() {
    const std::vector<int> order = MusicOrderFor(g_settings, Host_Songs().size());
    std::vector<std::wstring> names;
    for (int s : order) names.push_back(SongName(Host_Songs()[s]));
    const int p = SearchBest(names, u.search);
    u.searchHit = p >= 0 ? order[p] : -1;
    if (p >= 0) SongGlide(u.searchHit);
}

void SearchChar(wchar_t c) {
    if (c == 8) {
        if (!u.search.empty()) u.search.pop_back();
    } else if (c == 127) {  // Ctrl+Backspace
        u.search.clear();
    } else if (c == 22 || c >= 32) {
        const std::wstring add = c == 22 ? ClipboardText() : std::wstring(1, c);
        for (wchar_t ch : add) {
            if (ch == L'\r' || ch == L'\n') break;
            if (ch >= 32 && u.search.size() < 80) u.search += ch;
        }
    } else {
        return;
    }
    SongSearch();
}

void SearchClose() {
    u.searchOn = false;
    u.search.clear();
    u.searchHit = -1;
    if (u.focus == ID_MUSIC_SEARCH) u.focus = 0;
}

// A press on a song: on its grip it starts moving the song, elsewhere a drag scrolls the list.
void SongPress(int id, float y) {
    const int song = id % 10000;
    const std::vector<int> order = MusicOrderFor(g_settings, Host_Songs().size());
    const int p = (int)(std::find(order.begin(), order.end(), song) - order.begin());
    u.drag = ID_MUSIC_LIST;
    u.songPress = song;
    u.songMoving = id >= ID_SONG_GRIP0;
    u.songMoved = false;
    u.songPressY = u.songY = y;
    u.songPressScroll = u.songTarget = u.songScroll;
    u.songGrab = y - (u.songView.top - u.songScroll + p * kSongRowH);
    u.songEdgeSince = 0;
    if (u.songMoving) SetTimer(u.hwnd, TIMER_SONGS, 15, nullptr);  // the edges scroll the list
}

void SongDrag(float y) {
    u.songY = y;
    if (fabsf(y - u.songPressY) > 4) u.songMoved = true;
    if (!u.songMoving && u.songMoved)  // the list follows the cursor
        u.songScroll = u.songTarget = std::clamp(u.songPressScroll - (y - u.songPressY), 0.f, u.songMax);
}

void SongRelease() {
    const int song = u.songPress;
    u.songPress = -1;
    if (song < 0) return;
    if (u.songMoving) {
        if (u.songMoved) MusicPlace(song, u.songIns);
    } else if (!u.songMoved) {
        OnClick(ID_SONG_ROW0 + song);  // a click: play it
    }
    u.songMoving = false;
}

// Moving a song near the list's top or bottom edge scrolls it: slowly at first, faster the closer
// (or further past) the cursor gets, and only once it's been there a moment. The wheel glides too.
void SongTick() {
    bool busy = false;
    if (u.drag == ID_MUSIC_LIST && u.songMoving) {
        const float zone = 40, top = u.songView.top + zone, bottom = u.songView.bottom - zone;
        float depth = u.songY < top ? (u.songY - top) / zone : u.songY > bottom ? (u.songY - bottom) / zone : 0;
        depth = std::clamp(depth, -2.f, 2.f);
        if (depth != 0) {
            if (!u.songEdgeSince) u.songEdgeSince = GetTickCount();
            const float ease = std::min(1.f, (GetTickCount() - u.songEdgeSince) / 500.f);
            u.songTarget = std::clamp(u.songTarget + depth * fabsf(depth) * 8 * ease, 0.f, u.songMax);
            u.songScroll = u.songTarget;
        } else {
            u.songEdgeSince = 0;
        }
        busy = true;
    }
    if (fabsf(u.songTarget - u.songScroll) > 0.5f) {
        u.songScroll += (u.songTarget - u.songScroll) * 0.25f;
        busy = true;
    } else {
        u.songScroll = u.songTarget;
    }
    if (!busy) KillTimer(u.hwnd, TIMER_SONGS);
    InvalidateRect(u.hwnd, nullptr, FALSE);
}

// ---- Feedback: interaction

void CloseFeedback() {
    if (!fb.open) return;
    (fb.bug ? fb.what : fb.idea) = FbGet(fb.text);  // kept for next time
    if (fb.bug) {
        fb.doing = FbGet(fb.steps);
        fb.mail = FbGet(fb.email);
    }
    for (HWND* e : {&fb.text, &fb.steps, &fb.email})
        if (*e) {
            DestroyWindow(*e);
            *e = nullptr;
        }
    if (fb.font) DeleteObject(fb.font);
    if (fb.brush) DeleteObject(fb.brush);
    fb.font = nullptr;
    fb.brush = nullptr;
    fb.open = false;
    SetFocus(u.hwnd);
    u.focus = 0;
}

void FeedbackSend();

// The text boxes' own keys: Tab moves on through the dialog, Esc closes it, Ctrl+Enter sends.
// A picture for the report (a file, or a screenshot from the clipboard).
void FbAddPicture(const std::wstring& file, HBITMAP bitmap) {
    if (fb.pictures.size() >= kFeedbackPictures) {
        fb.note = Fmt(L"Up to %zu pictures fit in a report.", kFeedbackPictures);
        return;
    }
    FeedbackPicture p;
    if (Feedback_Picture(file, bitmap, &p)) {
        fb.pictures.push_back(std::move(p));
        fb.note.clear();
    } else {
        fb.note = L"That picture couldn't be read.";
    }
    InvalidateRect(u.hwnd, nullptr, FALSE);
}

void FbPickPictures() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    const COMDLG_FILTERSPEC types[] = {{L"Pictures", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.heic"}};
    dlg->SetFileTypes(1, types);
    dlg->SetTitle(L"Pictures for your report");
    dlg->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT);
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dlg->Show(u.hwnd)) && SUCCEEDED(dlg->GetResults(&items))) {
        DWORD n = 0;
        items->GetCount(&n);
        for (DWORD i = 0; i < n; i++) {
            IShellItem* it = nullptr;
            PWSTR path = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                FbAddPicture(path, nullptr);
                CoTaskMemFree(path);
            }
            if (it) it->Release();
        }
        items->Release();
    }
    dlg->Release();
}

LRESULT CALLBACK FbEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    if (m == WM_PASTE && IsClipboardFormatAvailable(CF_BITMAP) && !IsClipboardFormatAvailable(CF_UNICODETEXT)) {  // a screenshot
        if (OpenClipboard(u.hwnd)) {
            if (HBITMAP bmp = (HBITMAP)GetClipboardData(CF_BITMAP)) FbAddPicture(L"", bmp);
            CloseClipboard();
        }
        return 0;
    }
    if (m == WM_KEYDOWN) {
        if (w == VK_TAB) {
            u.focus = GetDlgCtrlID(h);
            SetFocus(u.hwnd);
            FocusMove(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
            return 0;
        }
        if (w == VK_ESCAPE) {
            PostMessageW(u.hwnd, WM_UI_FB_CLOSE, 0, 0);
            return 0;
        }
        if (w == 'A' && ctrl) {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        if (w == VK_RETURN && (ctrl || h == fb.email)) {
            FeedbackSend();
            InvalidateRect(u.hwnd, nullptr, FALSE);
            return 0;
        }
    }
    if (m == WM_CHAR && (w == L'\t' || w == 27 || w == 1 || w == 10 || (w == L'\r' && h == fb.email))) return 0;  // no beeps
    return CallWindowProcW(fb.editProc, h, m, w, l);
}

// A text box shows its scroll bar only once its text is longer than the box.
void FbFitScroll(HWND e) {
    if (!e || !(GetWindowLongW(e, GWL_STYLE) & ES_MULTILINE)) return;
    RECT rc;
    GetClientRect(e, &rc);
    TEXTMETRICW tm{};
    HDC dc = GetDC(e);
    HGDIOBJ old = SelectObject(dc, fb.font);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(e, dc);
    const int lines = (int)SendMessageW(e, EM_GETLINECOUNT, 0, 0);
    ShowScrollBar(e, SB_VERT, tm.tmHeight > 0 && lines * tm.tmHeight > rc.bottom);
}

HWND FbEdit(int id, const D2D1_RECT_F& box, bool multi, const std::wstring& text) {
    const float k = u.dpi / 96.f;
    const int x = (int)lroundf((box.left + 8) * k), y = (int)lroundf((box.top + 6) * k);
    const int w = (int)lroundf((box.right - box.left - (multi ? 10 : 16)) * k), h = (int)lroundf((box.bottom - box.top - 12) * k);
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (multi ? ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL : ES_AUTOHSCROLL);
    HWND e = CreateWindowExW(0, L"EDIT", text.c_str(), style, x, y, w, h, u.hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    if (!e) return nullptr;
    SendMessageW(e, WM_SETFONT, (WPARAM)fb.font, FALSE);
    SendMessageW(e, EM_SETLIMITTEXT, multi ? 4000 : 200, 0);
    if (u.th.dark) SetWindowTheme(e, L"DarkMode_Explorer", nullptr);  // a dark scroll bar
    fb.editProc = (WNDPROC)SetWindowLongPtrW(e, GWLP_WNDPROC, (LONG_PTR)FbEditProc);
    FbFitScroll(e);
    return e;
}

void OpenFeedback(bool bug) {
    if (fb.open) return;
    fb.open = true;
    fb.bug = bug;
    fb.note.clear();
    fb.sending = false;
    FeedbackLayout();
    const float k = u.dpi / 96.f;
    fb.font = CreateFontW(-(int)lroundf(13 * k), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                          FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI");
    fb.face = u.th.dark ? RGB(0x2B, 0x2B, 0x2B) : RGB(0xFF, 0xFF, 0xFF);
    fb.brush = CreateSolidBrush(fb.face);
    fb.text = FbEdit(ID_FB_TEXT, fb.boxText, true, bug ? fb.what : fb.idea);
    if (bug) {
        fb.steps = FbEdit(ID_FB_STEPS, fb.boxSteps, true, fb.doing);
        fb.email = FbEdit(ID_FB_EMAIL, fb.boxEmail, false, fb.mail);
    }
    if (fb.text) {
        SetFocus(fb.text);
        SendMessageW(fb.text, EM_SETSEL, GetWindowTextLengthW(fb.text), GetWindowTextLengthW(fb.text));
    }
    u.focus = ID_FB_TEXT;
}

// The PC, the video and the settings, for a problem report: nothing personal.
std::wstring FeedbackSystem() {
    std::wstring s = Feedback_Windows() + L"\n";
    s += L"Graphics: " + u.gpu + (g_settings.gpu == 1 ? L" (power saving chosen)" : g_settings.gpu == 2 ? L" (high performance chosen)" : L"") +
         L"\n";
    std::wstring ds;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(m, &mi);
            auto* d = (std::wstring*)p;
            if (!d->empty()) *d += L", ";
            *d += Fmt(L"%ldx%ld%ls", mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                      (mi.dwFlags & MONITORINFOF_PRIMARY) ? L" (main)" : L"");
            return TRUE;
        },
        (LPARAM)&ds);
    s += L"Displays: " + ds + (g_settings.monitors ? L"; the video on the main one only" : L"; the video on all") + L"\n";
    const std::wstring& video = g_settings.video;
    if (video.empty()) s += L"Video: none chosen\n";
    else if (u.infoOk)
        s += Fmt(L"Video: %ls, %ux%u, %.0f fps, %.1f s%ls%ls%ls\n", u.info.codec.c_str(), u.info.w, u.info.h, u.info.fps, u.info.duration,
                 u.info.audioStreams.empty() ? L", no sound" : L", has sound", IsLightCopy(video) ? L", a lighter version" : L"",
                 IsFromGif(video) ? L", made from a GIF" : L"");
    else s += L"Video: couldn't be read\n";
    FaultText faultBuf;
    if (const FaultText* f = CurrentFault(&faultBuf)) s += L"Problem shown: " + std::wstring(f->what) + L"\n";
    static const wchar_t* sound[] = {L"none", L"the video's", L"own music"};
    static const wchar_t* cover[] = {L"nothing", L"fullscreen apps", L"maximized apps"};
    static const wchar_t* battery[] = {L"keep playing", L"pause", L"turn off"};
    s += Fmt(L"Wallpaper %ls; sound: %ls; speed %d%%; frame rate %ls; crop: %ls\n", Host_IsOn() ? L"on" : L"off",
             sound[std::clamp(g_settings.sound, 0, 2)], g_settings.speed,
             g_settings.fpsCap ? Fmt(L"%d fps", g_settings.fpsCap).c_str() : L"original",
             g_settings.crop.stretch ? L"stretched" : g_settings.crop.IsFull() ? L"none" : L"screen shape");
    s += Fmt(L"Pause when covered by: %ls; on battery: %ls; lock screen follows: %ls\n", cover[std::clamp(g_settings.pauseCover, 0, 2)],
             battery[std::clamp(g_settings.onBattery, 0, 2)], g_settings.lockFollow ? L"yes" : L"no");
    s += Fmt(L"Clock: %ls on the video wallpaper%ls, %ls on the still one", g_settings.clockLive.show ? L"shown" : L"hidden",
             g_settings.clockVideoOwn ? L" (this video has its own)" : L"", g_settings.clockStill.show ? L"shown" : L"hidden");
    return s;
}

FeedbackReport FeedbackCollect() {
    FeedbackReport r;
    r.bug = fb.bug;
    r.text = FbGet(fb.text);
    if (fb.bug) {
        static const wchar_t* often[] = {L"Once", L"Sometimes", L"Every time"};
        r.steps = FbGet(fb.steps);
        r.email = FbGet(fb.email);
        r.often = fb.often >= 0 && fb.often < 3 ? often[fb.often] : L"";
        r.system = FeedbackSystem();
        if (fb.withLog) r.log = Feedback_Log();
    }
    r.pictures = fb.pictures;
    return r;
}

struct FeedbackJob {
    FeedbackReport report;
    HWND hwnd;
};

DWORD WINAPI FeedbackThread(void* p) {
    auto* job = (FeedbackJob*)p;
    std::wstring error;
    const bool ok = Feedback_Send(job->report, &error);
    auto* msg = new std::wstring(error);
    if (!PostMessageW(job->hwnd, WM_UI_FEEDBACK, (ok ? 1 : 0) | (job->report.bug ? 2 : 0), (LPARAM)msg)) delete msg;  // the window's gone
    delete job;
    return 0;
}

void FeedbackSend() {
    if (!fb.open || fb.sending) return;
    FeedbackReport r = FeedbackCollect();
    if (r.text.size() < 3) {
        fb.note = L"Write a few words first";
        return;
    }
    fb.sending = true;
    fb.note.clear();
    auto* job = new FeedbackJob{r, u.hwnd};
    if (HANDLE th = CreateThread(nullptr, 0, FeedbackThread, job, 0, nullptr)) {
        CloseHandle(th);
    } else {
        delete job;
        fb.sending = false;
        fb.note = L"Couldn't start sending it";
    }
}

// Everything that would be sent, in Notepad.
void FeedbackPreview() {
    std::wstring text = Feedback_Preview(FeedbackCollect());
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    const std::wstring path = std::wstring(dir) + L"VideoBG feedback (what's sent).txt";
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8 = "\xef\xbb\xbf";  // a BOM, so Notepad reads it as UTF-8
    utf8.resize(3 + n);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), utf8.data() + 3, n, nullptr, nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, utf8.data(), (DWORD)utf8.size(), &w, nullptr);
    CloseHandle(f);
    ShellExecuteW(u.hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
    if (id >= ID_SONG_DEL0 && id < ID_SONG_DEL0 + 10000) {  // take it off the playlist
        Host_RemoveSong(id - ID_SONG_DEL0);
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
    if (id >= ID_SONG_ROW0 && id < ID_SONG_GRIP0) {  // play this song
        Host_MusicCommand(MC_PLAY_INDEX, id - ID_SONG_ROW0);
        if (!g_settings.musicPlaying) {
            g_settings.musicPlaying = true;
            Host_SettingsChanged();
        }
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
    if (id >= ID_PRS_USE0 && id < ID_PRS_DEL0 + kClockPresets) {
        const int i = (id - ID_PRS_USE0) % 10;
        if (id < ID_PRS_EDIT0 || id >= ID_PRS_DEL0) {  // Use or Delete: asks first
            prs.confirm = i;
            prs.confirmDel = id >= ID_PRS_DEL0;
            if (u.focus) u.focus = u.hot = ID_PRS_YES;  // from the keyboard: Enter goes ahead
        } else if (id < ID_PRS_REN0) {
            PresetEdit(i);
        } else {
            PresetRenameStart(i);
        }
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
    if (id >= ID_VER_ROW0 && id < ID_VER_DEL0) {
        ver.confirm = -1;
        if (id - ID_VER_ROW0 < (int)ver.list.size()) VerSelect(id - ID_VER_ROW0);
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
    if (id >= ID_VER_DEL0 && id < ID_VER_DEL0 + 50) {
        ver.confirm = id - ID_VER_DEL0;
        if (u.focus) u.focus = u.hot = ID_VER_YES;  // from the keyboard: Enter deletes
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
    if (id >= ID_DLG_SLOT0 && id < ID_DLG_SLOT0 + 16) {
        dlg.slot = id - ID_DLG_SLOT0;
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
    if (id >= ID_LANG_ROW0 && id < ID_LANG_ROW0 + 1000) {
        FontPick(id - ID_LANG_ROW0);
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
    if (id >= ID_FNT_ROW0 && id < ID_FNT_DEL0 + 100) {
        int i = (id - ID_FNT_ROW0) % 100;
        if (id < ID_FNT_GET0) FontPick(i);
        else if (id < ID_FNT_ADD0) FontGet(i);
        else if (id < ID_FNT_DEL0) FontAddFile(i);
        else {
            fnt.confirm = i;
            if (u.focus) u.focus = u.hot = ID_FNT_YES;  // from the keyboard: Enter removes
        }
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return;
    }
    switch (id) {
        case ID_POWER: Host_SetOn(!Host_IsOn()); break;
        case ID_CLK_CUSTOM: OpenColorDialog(0); break;
        case ID_DLG_OK: CloseColorDialog(true); break;
        case ID_DLG_CANCEL: CloseColorDialog(false); break;
        case ID_DLG_SAME: DlgSameAsText(!dlg.sameAsText); break;
        case ID_DLG_HEX:
            dlg.hexEdit = true;
            dlg.hexFresh = true;  // typing replaces it
            dlg.hex = Hex(DlgColor()).substr(1);
            break;
        case ID_DLG_COPY: DlgCopy(); break;
        case ID_DLG_PICK:  // the dialog steps aside: the colour comes from the wallpaper on the page
            u.picking = true;
            u.pickDirect = false;
            u.pickColor.clear();
            dlg.hexEdit = false;
            break;
        case ID_DLG_SAVE: DlgSaveSlot(); break;
        case ID_DLG_USE: DlgUseSlot(dlg.slot); break;
        case ID_REPO: ShellExecuteW(u.hwnd, L"open", APP_REPO, nullptr, nullptr, SW_SHOWNORMAL); break;
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
        case ID_MUSIC_ADD:
            Host_PickMusic(u.hwnd, id == ID_MUSIC_ADD);
            break;
        case ID_MUSIC_SEARCH:
            if (!u.searchOn) {
                u.searchOn = true;
                u.search.clear();
                u.searchHit = -1;
            }
            break;
        case ID_MUSIC_SEARCH_X: SearchClose(); break;
        case ID_MUSIC_PLAY:
            g_settings.musicPlaying = !g_settings.musicPlaying;
            Log(L"ui: music %ls", g_settings.musicPlaying ? L"played" : L"paused");
            Host_SettingsChanged();
            break;
        case ID_MUSIC_PREV:
        case ID_MUSIC_NEXT:
            Host_MusicCommand(id == ID_MUSIC_NEXT ? MC_NEXT : MC_PREV, 0);
            if (!g_settings.musicPlaying) {  // skipping means listening
                g_settings.musicPlaying = true;
                Host_SettingsChanged();
            }
            break;
        case ID_MUSIC_SHUFFLE: MusicShuffle(); break;
        case ID_UNMUTE:  // mute / unmute
            g_settings.videoMuted = !g_settings.videoMuted;
            Host_SettingsChanged();
            break;
        case ID_MUSIC_HEAD: SongGlide(u.songShown); break;
        case ID_MUSIC_REPEAT:  // the playlist (always) or just this song
            g_settings.musicRepeat = g_settings.musicRepeat == 2 ? 1 : 2;
            Host_SettingsChanged();
            break;

        case ID_VERSIONS: OpenVersions(); break;
        case ID_VER_CANCEL: CloseVersions(); break;
        case ID_VER_NO: ver.confirm = -1; break;
        case ID_VER_YES:
            VerDelete(ver.confirm);
            ver.confirm = -1;
            break;
        case ID_CLK_FONT: OpenFontDialog(); break;
        case ID_FB_IDEA: OpenFeedback(false); break;
        case ID_FB_BUG: OpenFeedback(true); break;
        case ID_FB_CANCEL: CloseFeedback(); break;
        case ID_FB_SEND: FeedbackSend(); break;
        case ID_FB_LOG: fb.withLog = !fb.withLog; break;
        case ID_FB_PREVIEW: FeedbackPreview(); break;
        case ID_FB_PICS: FbPickPictures(); break;
        case ID_FB_PICS_CLEAR: fb.pictures.clear(); break;
        case ID_FB_PIC_X0:
        case ID_FB_PIC_X0 + 1:
        case ID_FB_PIC_X0 + 2:
        case ID_FB_PIC_X0 + 3:
        case ID_FB_PIC_X0 + 4:
            if (id - ID_FB_PIC_X0 < (int)fb.pictures.size()) fb.pictures.erase(fb.pictures.begin() + (id - ID_FB_PIC_X0));
            fb.note.clear();
            break;
        case ID_PRS: OpenPresets(); break;
        case ID_PRS_CLOSE: ClosePresets(); break;
        case ID_PRS_NO: prs.confirm = -1; break;
        case ID_PRS_YES: PresetConfirmed(); break;
        case ID_PRS_SAVE: PresetEditEnd(true); break;
        case ID_PRS_CANCEL: PresetEditEnd(false); break;
        case ID_PRS_CURRENT: PresetFromCurrent(); break;
        case ID_CLK_DATE: OpenDateDialog(); break;
        case ID_PLAYER_STYLE: OpenPlayerDialog(); break;
        case ID_PD_PREV:
        case ID_PD_NEXT: {  // off, then each style, round again
            const int n = (int)Player_Styles().size() + 1;
            g_settings.musicPlayer = (std::clamp(g_settings.musicPlayer, 0, n - 1) + (id == ID_PD_NEXT ? 1 : n - 1)) % n;
            Log(L"ui: desktop music player %ls", g_settings.musicPlayer ? Player_Styles()[g_settings.musicPlayer - 1].c_str() : L"off");
            Host_SettingsChanged();
            break;
        }
        case ID_PD_COLOR: OpenColorDialog(2); break;
        case ID_PD_BODY: OpenColorDialog(3); break;
        case ID_PD_RESET:
            PdLook() = Settings::PlayerLook{};
            SaveSettings(g_settings);
            Player_Update();
            break;
        case ID_PD_RST_COL:
        case ID_PD_RST_BODY:
        case ID_PD_RST_OPACITY:
        case ID_PD_RST_SIZE:
        case ID_PD_RST_CORNERS:
        case ID_PD_RST_ROTATE: {  // one setting back to the style's own
            Settings::PlayerLook& pl = PdLook();
            const Settings::PlayerLook own;
            if (id == ID_PD_RST_COL) pl.color = own.color;
            else if (id == ID_PD_RST_BODY) pl.body = own.body;
            else if (id == ID_PD_RST_OPACITY) pl.opacity = own.opacity;
            else if (id == ID_PD_RST_SIZE) pl.size = own.size;
            else if (id == ID_PD_RST_ROTATE) pl.rotate = own.rotate;
            else pl.corners = own.corners;
            SaveSettings(g_settings);
            Player_Update();
            break;
        }
        case ID_PD_PICK_COL: StartPick(2); break;
        case ID_PD_PICK_BODY: StartPick(3); break;
        case ID_CLK_PICK: StartPick(0); break;
        case ID_PD_DONE: ClosePlayerDialog(); break;
        case ID_WALLS: OpenWallpapers(); break;
        case ID_FNT_DONE:
            if (fnt.langs) CloseLanguageList();
            else CloseFontDialog();
            break;
        case ID_FNT_OWN:
            if (fnt.walls) WallsAdd();
            else FontAddOwn();
            break;
        case ID_FNT_NO: fnt.confirm = -1; break;
        case ID_FNT_YES:
            FontRemove(fnt.confirm);
            fnt.confirm = -1;
            break;
        case ID_FNT_LANG: OpenLanguageList(); break;
        case ID_FNT_DIGITS:
            EditedLook()->nativeDigits = !EditedLook()->nativeDigits;
            ClockEdited();
            ReleaseFontSamples();
            break;
        case ID_VER_USE: VerApply(); break;
        case ID_LOCK: Host_SetLockFollow(!g_settings.lockFollow); break;
        case ID_HOTKEY:
        case ID_MUSIC_HOTKEY:
            if (u.recording) StopRecording();
            else {
                u.recording = id;
                Host_SuspendHotkey(true);  // so pressing the current shortcut reaches this window
            }
            u.hotkeyMsg.clear();
            SetFocus(u.hwnd);
            break;
        case ID_STARTUP_LINK: ShellExecuteW(u.hwnd, L"open", L"ms-settings:startupapps", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_STARTUP_REG: Host_RegisterStartup(); break;
        case ID_C_LOCK:
            u.lockAspect = !u.lockAspect;
            u.edit.stretch = !u.lockAspect;
            if (u.lockAspect) u.edit = FitAspect(u.edit);
            SendPreview(true);
            break;
        case ID_C_RESET:
            u.edit = Crop{};
            u.edit.stretch = !u.lockAspect;
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
    u.recording = 0;
    Host_SuspendHotkey(false);
}

// ---- Keyboard ------------------------------------------------------------------------------
// Tab / Shift+Tab move a focus through the controls in the order they're drawn: the header, the
// sidebar, then the page from the top (only a dialog's own while one is open). Enter or Space uses
// the focused control (a choice of several goes on to the next one, round and round); the arrow keys
// step a slider, nudge the clock or the crop box, move through a choice or a colour field, and
// otherwise move the focus. Esc lets go of the focus; Esc again closes the dialog or the window.

bool Focusable(const Hit& h) {
    if (!h.enabled) return false;
    if (h.kind == K_BUTTON || h.kind == K_TOGGLE || h.kind == K_SEG || h.kind == K_SLIDER) return true;
    return h.id == ID_CLK_AREA || h.id == ID_C_AREA || h.id == ID_DLG_SV || h.id == ID_DLG_HUE || FbBox(h.id);
}

// Where Tab starts: after the last dialog backdrop, so an open dialog keeps the focus to itself.
size_t FocusStart() {
    size_t start = 0;
    for (size_t i = 0; i < u.hits.size(); i++)
        if (u.hits[i].id == ID_DLG_SCRIM || u.hits[i].id == ID_VER_SCRIM || u.hits[i].id == ID_FNT_SCRIM || u.hits[i].id == ID_PRS_SCRIM ||
            u.hits[i].id == ID_FB_SCRIM || u.hits[i].id == ID_PD_SCRIM)
            start = i + 1;
    return start;
}

const Hit* FocusHit() {
    for (size_t i = u.hits.size(); i-- > FocusStart();)
        if (u.hits[i].id == u.focus) return &u.hits[i];
    return nullptr;
}

// In the font list, the focused row scrolls into view.
void FontScrollToFocus() {
    const int id = u.focus;
    float top;
    if (id >= ID_LANG_ROW0 && id < ID_LANG_ROW0 + 1000) top = (id - ID_LANG_ROW0) * kLangRowH;
    else if (id >= ID_FNT_ROW0 && id < ID_FNT_DEL0 + 100) top = ((id - ID_FNT_ROW0) % 100) * PickerRowH();
    else return;
    const float bottom = top + PickerRowH();
    if (top < fnt.target) FontScrollTo(top, true);
    else if (bottom > fnt.target + fnt.viewH) FontScrollTo(bottom - fnt.viewH, true);
}

void SetFocusTo(int id) {
    FinishStep();
    u.focus = id;
    u.hot = id;  // the help line says what it does, as on hover
    u.hotSeg = -1;
    const Hit* h = FocusHit();
    u.keySlider = h && h->kind == K_SLIDER ? id : 0;
    if (fnt.open) FontScrollToFocus();
    if (fb.open) SetFocus(FbBox(id) && FbEditFor(id) ? FbEditFor(id) : u.hwnd);  // a text box takes the keys
}

void FocusMove(int dir) {
    std::vector<int> ids;  // the controls Tab goes through, in order
    for (size_t i = FocusStart(); i < u.hits.size(); i++)
        if (Focusable(u.hits[i]) && std::find(ids.begin(), ids.end(), u.hits[i].id) == ids.end()) ids.push_back(u.hits[i].id);
    if (ids.empty()) return;
    auto it = std::find(ids.begin(), ids.end(), u.focus ? u.focus : u.tabFrom);
    const int n = (int)ids.size();
    int i = it == ids.end() ? (dir > 0 ? -1 : 0) : (int)(it - ids.begin());  // nothing yet: the first (backwards: the last)
    SetFocusTo(ids[(i + dir + n) % n]);
    InvalidateRect(u.hwnd, nullptr, FALSE);
}

// Enter on the clock: centred across the screen; Enter again puts it back.
void ClockCentre() {
    ClockLook* l = EditedLook();
    if (fabsf(l->x - 0.5f) < 0.0005f && u.clockPrevX >= 0) {
        l->x = u.clockPrevX;
        u.clockPrevX = -1;
    } else {
        u.clockPrevX = l->x;
        l->x = 0.5f;
    }
    ClockEdited();
}

void FocusActivate() {
    const Hit* h = FocusHit();
    if (!h || !h->enabled) return;
    if (h->id == ID_CLK_AREA) {
        if (ClockUsable()) ClockCentre();
        return;
    }
    const int id = h->id, n = (int)h->segs.size() - 1, sel = h->sel;
    if (h->kind == K_SEG) {
        if (n > 0) OnSeg(id, (sel + 1) % n);
    } else if (h->kind == K_BUTTON || h->kind == K_TOGGLE) {
        OnClick(id);
    }
}

// The crop box, moved a step (Shift: five) without changing its size.
void CropNudge(UINT vk) {
    const float step = GetKeyState(VK_SHIFT) < 0 ? 0.05f : 0.01f;
    Crop& c = u.edit;
    float dx = vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0, dy = vk == VK_UP ? -step : vk == VK_DOWN ? step : 0;
    dx = std::clamp(dx, -c.l, 1 - c.r);
    dy = std::clamp(dy, -c.t, 1 - c.b);
    c.l += dx;
    c.r += dx;
    c.t += dy;
    c.b += dy;
    SendPreview(true);
}

// The colour dialog's field (saturation across, brightness up) and hue bar, a step at a time.
void DlgNudge(int id, UINT vk) {
    const float step = GetKeyState(VK_SHIFT) < 0 ? 0.1f : 0.01f;
    const float dx = vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0, dy = vk == VK_UP ? step : vk == VK_DOWN ? -step : 0;
    if (id == ID_DLG_SV) {
        dlg.s = std::clamp(dlg.s + dx, 0.f, 1.f);
        dlg.v = std::clamp(dlg.v + dy, 0.f, 1.f);
    } else {
        dlg.h = std::clamp(dlg.h + (dx + dy) * 360, 0.f, 359.9f);
    }
    dlg.exact.clear();
    dlg.sameAsText = dlg.hexEdit = false;
    DlgApply(false);
}

void FocusArrow(UINT vk) {
    const Hit* h = FocusHit();
    if (!h) return;
    const int id = h->id, dir = vk == VK_RIGHT || vk == VK_DOWN ? 1 : -1;
    if (h->kind == K_SLIDER) {
        u.keySlider = id;
        StepSlider(vk == VK_RIGHT || vk == VK_UP ? 1 : -1);
    } else if (h->kind == K_SEG && (vk == VK_LEFT || vk == VK_RIGHT)) {
        const int n = (int)h->segs.size() - 1, to = std::clamp(h->sel + dir, 0, std::max(0, n - 1));
        if (to != h->sel) OnSeg(id, to);
    } else if (id == ID_CLK_AREA) {
        if (ClockUsable()) ClockNudge(vk);
    } else if (id == ID_C_AREA) {
        CropNudge(vk);
    } else if (id == ID_DLG_SV || id == ID_DLG_HUE) {
        DlgNudge(id, vk);
    } else {
        FocusMove(dir);
    }
}

// The outline around the focused control (a slider shows a ring around its knob instead).
void PaintFocus() {
    const Hit* h = u.focus ? FocusHit() : nullptr;
    if (!h || h->kind == K_SLIDER || h->r.right <= h->r.left || FbBox(h->id)) return;  // a text box shows its own
    D2D1_RECT_F r = R(h->r.left - 3, h->r.top - 3, h->r.right + 3, h->r.bottom + 3);
    if (h->id == ID_CLK_AREA) {  // around the clock, not the whole preview
        const D2D1_RECT_F c = ClockRectIn(u.clockBox, *EditedLook());
        r = R(c.left - 4, c.top - 4, c.right + 4, c.bottom + 4);
    }
    const bool inList = fnt.open && ((h->id >= ID_FNT_ROW0 && h->id < ID_FNT_DEL0 + 100) ||
                                     (h->id >= ID_LANG_ROW0 && h->id < ID_LANG_ROW0 + 1000));
    if (inList) u.rt->PushAxisAlignedClip(fnt.view, D2D1_ANTIALIAS_MODE_ALIASED);
    Stroke(r, u.th.accent, h->kind == K_AREA ? 8.f : 7.f, 2);
    if (inList) u.rt->PopAxisAlignedClip();
}

bool DialogOpen() { return dlg.open || ver.open || fnt.open || prs.open || fb.open || pd.open || u.page == 1; }  // the crop page counts too

bool OnKeyInner(UINT vk);

// Keys go to OnKeyInner; a dialog opened from the keyboard gets the focus, and gives it back to
// what opened it when it closes.
bool OnKey(UINT vk) {
    const bool was = DialogOpen();
    const int opener = FocusHit() ? u.focus : 0;
    const bool used = OnKeyInner(vk);
    if (!was && DialogOpen() && opener) {
        u.focusReturn = opener;
        u.focus = 0;
        u.focusFirst = true;
    } else if (was && !DialogOpen()) {
        if (u.focusReturn) u.focus = u.hot = u.focusReturn;
        u.focusReturn = 0;
    }
    return used;
}

bool OnKeyInner(UINT vk) {
    if (u.recording) {
        if (vk == VK_ESCAPE) { StopRecording(); InvalidateRect(u.hwnd, nullptr, FALSE); return true; }
        if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN || vk == VK_LCONTROL ||
            vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LSHIFT || vk == VK_RSHIFT)
            return true;
        UINT mods = (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) | (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) |
                    (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0) |
                    ((GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) ? MOD_WIN : 0);
        const int which = u.recording;
        u.hotkeyMsgFor = which;
        // The music shortcut may be a key no app uses on its own (Pause, Scroll Lock, F13-F24, the
        // media Play/Pause key); Backspace leaves it without one.
        const bool music = which == ID_MUSIC_HOTKEY;
        const bool alone = music && (vk == VK_PAUSE || vk == VK_SCROLL || (vk >= VK_F13 && vk <= VK_F24) || vk == VK_MEDIA_PLAY_PAUSE);
        if (music && !mods && vk == VK_BACK) {
            u.recording = 0;
            Host_SetMusicHotkey(0, 0);
            Host_SuspendHotkey(false);
            u.hotkeyMsg.clear();
        } else if (!(mods & (MOD_CONTROL | MOD_ALT | MOD_WIN)) && !alone) {
            u.hotkeyMsg = music ? L"Use Ctrl, Alt or Win with a key, or Pause, Scroll Lock or F13-F24 alone"
                                : L"Use Ctrl, Alt or Win together with a key";
        } else {
            u.recording = 0;  // Host_Set... registers the new combination itself
            const bool ok = music ? Host_SetMusicHotkey(mods, vk) : Host_SetHotkey(mods, vk);
            Host_SuspendHotkey(false);
            if (ok) u.hotkeyMsg.clear();
            else u.hotkeyMsg = L"That shortcut is already used by another app";
        }
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (prs.rename >= 0) {  // typing a preset's name (the letters come as WM_CHAR); Tab keeps it and goes on
        if (vk == VK_TAB) {
            PresetRenameDone(true);
        } else {
            if (vk == VK_RETURN || vk == VK_ESCAPE) PresetRenameDone(vk == VK_RETURN);
            InvalidateRect(u.hwnd, nullptr, FALSE);
            return true;
        }
    }
    if (u.searchOn && u.page == 0 && u.nav == NAV_SOUND && !DialogOpen() && (u.focus == ID_MUSIC_SEARCH || u.focus == 0)) {
        if (vk == VK_ESCAPE || vk == VK_RETURN) {  // put away (the list stays where it found the song)
            SearchClose();
            InvalidateRect(u.hwnd, nullptr, FALSE);
            return true;
        }
        if (vk != VK_TAB && vk != VK_UP && vk != VK_DOWN) return true;  // letters come as WM_CHAR
    }
    if (vk == VK_TAB && u.picking) return true;  // the eyedropper is out: Esc or a click first
    if (vk == VK_TAB) {
        if (dlg.open) dlg.hexEdit = false;
        FocusMove(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
        return true;
    }
    const bool typing = dlg.open && (dlg.hexEdit || u.picking);  // the colour dialog's own keys first
    if (vk == VK_ESCAPE && (FocusHit() || u.keySlider) && !typing && !u.picking) {  // Esc: no focus; Esc again closes
        FinishStep();
        u.focus = u.keySlider = 0;
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (u.focus >= ID_SONG_ROW0 && u.focus < ID_SONG_GRIP0 && GetKeyState(VK_MENU) < 0 && (vk == VK_UP || vk == VK_DOWN)) {
        MusicMove(u.focus - ID_SONG_ROW0, vk == VK_UP ? -1 : 1);  // Alt+arrows move the focused song
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (u.focus && !typing && FocusHit()) {
        if (vk == VK_RETURN || vk == VK_SPACE) {
            FocusActivate();
            InvalidateRect(u.hwnd, nullptr, FALSE);
            return true;
        }
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) {
            FocusArrow(vk);
            InvalidateRect(u.hwnd, nullptr, FALSE);
            return true;
        }
    }
    if (dlg.open) return DlgKey(vk);
    if (pd.open && (vk == VK_ESCAPE || vk == VK_RETURN) && !u.keySlider) {
        ClosePlayerDialog();
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (ver.open) {
        int n = (int)ver.list.size();
        if (vk == VK_ESCAPE && ver.confirm >= 0) {
            ver.confirm = -1;
        } else if (vk == VK_RETURN && ver.confirm >= 0) {
            VerDelete(ver.confirm);
            ver.confirm = -1;
        } else if (vk == VK_ESCAPE) {
            CloseVersions();
        } else if (vk == VK_RETURN) {
            VerApply();
        }
        else if ((vk == VK_UP || vk == VK_DOWN) && n) VerSelect(std::clamp(ver.sel + (vk == VK_DOWN ? 1 : -1), 0, n - 1));
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (fb.open) {
        if (vk == VK_ESCAPE) CloseFeedback();
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (prs.open) {
        if (vk == VK_ESCAPE && prs.confirm >= 0) prs.confirm = -1;
        else if (vk == VK_ESCAPE) ClosePresets();
        else if (vk == VK_RETURN && prs.confirm >= 0) PresetConfirmed();
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (fnt.open && fnt.confirm >= 0 && (vk == VK_ESCAPE || vk == VK_RETURN)) {
        if (vk == VK_RETURN) FontRemove(fnt.confirm);
        fnt.confirm = -1;
        InvalidateRect(u.hwnd, nullptr, FALSE);
        return true;
    }
    if (fnt.open) {
        if (fnt.langs && vk >= 'A' && vk <= 'Z') LanguageJump((wchar_t)vk);
        if ((vk == VK_ESCAPE || vk == VK_RETURN) && fnt.langs) CloseLanguageList();
        else if (vk == VK_ESCAPE || vk == VK_RETURN) CloseFontDialog();
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
        else if (prs.edit >= 0) PresetEditEnd(false);  // like a dialog's Cancel
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
            if (u.drag == ID_MUSIC_LIST) {
                SongDrag(y);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (u.drag) {
                for (auto& ht : u.hits)
                    if (ht.id == u.drag) {
                        if (ht.kind == K_SLIDER) OnSlider(ht.id, SliderFrac(ht, x), false);
                        else if (ht.id == ID_C_AREA) DragCrop(x, y);
                        else if (ht.id == ID_CLK_AREA) ClockDrag(x, y, false);
                        else if (ht.id == ID_CLK_ROTATE) ClockRotate(x, y);
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
            if (u.picking) {  // the colour under the eyedropper
                std::wstring pc;
                bool ok = dlg.target >= 2 ? PdSample(x, y, &pc) : SampleWallpaper(u.clockBox, x, y, &pc);
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
                bool cross = u.hot == ID_DLG_SV || u.drag == ID_DLG_SV;
                if (cross || u.hot == ID_DLG_HEX) {
                    SetCursor(LoadCursorW(nullptr, cross ? IDC_CROSS : IDC_IBEAM));
                    return TRUE;
                }
                break;
            }
            if (LOWORD(l) == HTCLIENT && u.picking && (u.hot == ID_PD_AREA || (u.page == 0 && u.nav == NAV_CLOCK && u.hot == ID_CLK_AREA))) {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                return TRUE;
            }
            if (LOWORD(l) == HTCLIENT && u.hot == ID_MUSIC_SEARCH && u.searchOn) {  // the playlist's search box
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            if (LOWORD(l) == HTCLIENT && u.page == 0 && u.nav == NAV_CLOCK && (u.hot == ID_CLK_AREA || u.drag == ID_CLK_AREA)) {
                SetCursor(LoadCursorW(nullptr, u.overClock || u.drag ? IDC_SIZEALL : IDC_ARROW));
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
            if (u.recording && (!ht || ht->id != u.recording)) StopRecording();
            if (prs.rename >= 0 && (!ht || ht->id != ID_PRS_NAME)) PresetRenameDone(true);  // clicking elsewhere keeps the name
            FinishStep();
            u.keySlider = ht && ht->kind == K_SLIDER && ht->enabled ? ht->id : 0;
            u.focus = 0;
            u.tabFrom = ht ? ht->id : 0;
            if (u.picking) {  // the colour dialog's eyedropper: take the wallpaper's colour, or put it away
                std::wstring pc;
                BYTE r = 0, g = 0, b = 0;
                if (dlg.target >= 2 ? ht && ht->id == ID_PD_AREA && PdSample(x, y, &pc) && ParseColor(pc, &r, &g, &b)
                                    : ht && ht->id == ID_CLK_AREA && SampleWallpaper(u.clockBox, x, y, &pc) && ParseColor(pc, &r, &g, &b)) {
                    DlgSetRgb(r, g, b);
                    if (u.pickDirect && !dlg.open) {  // from the page: kept now
                        Log(L"ui: %ls colour taken from the wallpaper: %ls", dlg.target == 0 ? L"clock text" : dlg.target == 2 ? L"player" : L"player body",
                            pc.c_str());
                        if (dlg.target >= 2) SaveSettings(g_settings);
                        else ClockEdited();
                    }
                }
                u.picking = u.pickDirect = false;
                u.pickColor.clear();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (dlg.open && DlgMouseDown(ht, x, y)) {
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (!ht || !ht->enabled) return 0;
            if (ht->id >= ID_SONG_ROW0 && ht->id < ID_SONG_GRIP0 + 10000) {  // a song: click, scroll or move
                SetCapture(h);
                SongPress(ht->id, y);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (FbBox(ht->id)) {  // the frame around a text box
                if (HWND e = FbEditFor(ht->id)) SetFocus(e);
                return 0;
            }
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
            } else if (ht->id == ID_CLK_ROTATE) {
                ClockLook* look = EditedLook();
                if (m == WM_LBUTTONDBLCLK) {  // upright again
                    look->rotate = 0;
                    ClockEdited();
                } else {
                    const D2D1_RECT_F cr = ClockRectIn(u.clockBox, *look);
                    u.drag = ht->id;
                    u.rotFrom = look->rotate;
                    u.rotPointer = Clock_PointerAngle((cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2, x, y);
                }
            } else if (ht->id == ID_CLK_AREA) {  // the clock itself moves when dragged (a click beside it does nothing)
                if (In(ClockRectIn(u.clockBox, *EditedLook()), x, y)) {
                    u.drag = ht->id;
                    u.overClock = true;
                    ClockDrag(x, y, true);
                }
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
            if (dragId == ID_MUSIC_LIST) {
                SongRelease();
                u.drag = u.press = 0;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            u.drag = 0;
            u.press = 0;
            if (dragId) {
                for (auto& ht : u.hits)
                    if (ht.id == dragId) {
                        if (ht.kind == K_SLIDER) OnSlider(ht.id, SliderFrac(ht, x), true);
                        else if (ht.id == ID_C_AREA) SendPreview(true);
                        else if (ht.id == ID_CLK_AREA || ht.id == ID_CLK_ROTATE) {
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
        case WM_CHAR:  // typing a preset's name, a hex code in the colour dialog, or a song to find
            if (u.searchOn && !DialogOpen() && u.page == 0 && u.nav == NAV_SOUND && (u.focus == ID_MUSIC_SEARCH || u.focus == 0)) {
                SearchChar((wchar_t)w);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (prs.rename >= 0) {
                PresetNameChar((wchar_t)w);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (dlg.open && (iswxdigit((wchar_t)w) || w == L'#')) {
                DlgHexChar((wchar_t)w);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            if (fb.open) {  // the pictures' strip, sideways
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                ScreenToClient(h, &pt);
                if (fb.picMax > 0 && In(fb.strip, Dip(pt.x), Dip(pt.y))) {
                    fb.picScroll = std::clamp(fb.picScroll - GET_WHEEL_DELTA_WPARAM(w) / 120.f * 60, 0.f, fb.picMax);
                    InvalidateRect(h, nullptr, FALSE);
                }
            } else if (fnt.open) {
                FontScrollTo(fnt.target - GET_WHEEL_DELTA_WPARAM(w) / 120.f * 80, true);
            } else if (u.page == 0 && u.nav == NAV_SOUND && !DialogOpen()) {  // the playlist
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                ScreenToClient(h, &pt);
                if (In(u.songView, Dip(pt.x), Dip(pt.y))) {  // glides there
                    u.songTarget = std::clamp(u.songTarget - GET_WHEEL_DELTA_WPARAM(w) / 120.f * kSongRowH * 2, 0.f, u.songMax);
                    SetTimer(h, TIMER_SONGS, 15, nullptr);
                }
            } else if (u.page == 1) {
                ZoomCrop(powf(0.92f, GET_WHEEL_DELTA_WPARAM(w) / 120.f));  // touchpads send many small steps
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
            if (w == TIMER_SONGS) SongTick();
            if (w == TIMER_HOLD) ReleaseWallpaper();
            if (w == TIMER_SHARPEN) {
                KillTimer(h, TIMER_SHARPEN);
                InvalidateRect(h, nullptr, FALSE);
            }
            if (w == TIMER_SCROLL) {  // ease towards the target: a third of the way each frame
                fnt.scroll += (fnt.target - fnt.scroll) * 0.3f;
                if (!fnt.open || std::abs(fnt.target - fnt.scroll) < 0.5f) {
                    fnt.scroll = fnt.target;
                    KillTimer(h, TIMER_SCROLL);
                }
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_UI_WALL:
            if (u.hwnd) WallInfo((WallpaperInfo*)l);
            else delete (WallpaperInfo*)l;
            return 0;
        case WM_UI_STILL: {
            auto* sm = (StillMsg*)l;
            u.stillLoading = false;
            ForgetScaled();
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
                    ForgetScaled();
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
            if (ver.open && SUCCEEDED(hr) && !u.optGif) OpenVersions();  // the list again, with the new version in it
            UI_Refresh();
            return 0;
        }
        case WM_UI_FEEDBACK: {  // sent, or why not
            auto* error = (std::wstring*)l;
            const bool ok = w & 1, bug = w & 2;
            fb.sending = false;
            if (ok) {
                if (fb.open && fb.bug == bug) {
                    for (HWND e : {fb.text, fb.steps, fb.email})
                        if (e) SetWindowTextW(e, L"");
                    CloseFeedback();
                }
                if (bug) fb.what.clear(), fb.doing.clear();
                else fb.idea.clear();
                fb.pictures.clear();
                fb.sent = L"Thanks, it's sent!";
                fb.sentAt = GetTickCount();
            } else {
                fb.note = L"Couldn't send it: " + *error + L" Your text is kept; try again.";
            }
            delete error;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_UI_FB_CLOSE:
            CloseFeedback();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_COMMAND:  // the feedback text boxes: typed in, or focused
            if (fb.open && (HIWORD(w) == EN_CHANGE || HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS)) {
                if (HIWORD(w) == EN_SETFOCUS) u.focus = LOWORD(w);
                if (HIWORD(w) == EN_CHANGE) FbFitScroll((HWND)l);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            if (fb.open && fb.brush) {
                const D2D1_COLOR_F& c = u.th.text;
                SetTextColor((HDC)w, RGB(lroundf(c.r * 255), lroundf(c.g * 255), lroundf(c.b * 255)));
                SetBkColor((HDC)w, fb.face);
                return (LRESULT)fb.brush;
            }
            break;
        case WM_DISPLAYCHANGE:  // a display added, removed or resized
            QueryScreen();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            if (u.page == 1) Host_PreviewCrop(nullptr);
            ReleaseWallpaper();  // (closed while a frame was being picked)
            if (fnt.players) Player_FreePreview();
            if (fnt.walls) {  // its worker stops; what it had sent is let go
                Library_Stop();
                MSG msg;
                while (PeekMessageW(&msg, h, WM_UI_WALL, WM_UI_WALL, PM_REMOVE)) delete (WallpaperInfo*)msg.lParam;
            }
            CloseVersions();
            if (dlg.open) {  // closed with the colour dialog open: as Cancel
                EditedLook()->color = dlg.beforeText;
                EditedLook()->glowColor = dlg.beforeGlow;
                dlg = ColorDialog{};
                Host_SettingsChanged();
            }
            PresetEditEnd(false);  // closed while editing a preset: as Cancel
            prs = PresetUI{};
            CloseFeedback();  // what was typed is kept for next time
            fb.sent.clear();
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
    // The displays the video plays on (Playback > Displays); versions are sized for the largest.
    struct Acc {
        int w, h, n;
        bool all;
    } acc{0, 0, 0, g_settings.monitors == 0};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            Acc* a = (Acc*)p;
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(m, &mi);
            if (!a->all && !(mi.dwFlags & MONITORINFOF_PRIMARY)) return TRUE;
            const int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
            if ((INT64)w * h > (INT64)a->w * a->h) {
                a->w = w;
                a->h = h;
            }
            a->n++;
            return TRUE;
        },
        (LPARAM)&acc);
    u.playW = acc.w > 0 ? acc.w : u.screenW;
    u.playH = acc.h > 0 ? acc.h : u.screenH;
    u.playCount = std::max(1, acc.n);
}

void QueryGpu() {
    const bool loaded = D3D.CreateFactory1 != nullptr;  // in use elsewhere (a version being made): leave it loaded
    if (!D3D.Load()) return;
    IDXGIFactory1* f = nullptr;
    if (SUCCEEDED(D3D.CreateFactory1(__uuidof(IDXGIFactory1), (void**)&f))) {
        std::wstring autoName;
        for (auto [name, pref] : {std::pair{&u.gpu, g_settings.gpu - 1}, {&u.gpuLow, 0}, {&u.gpuHigh, 1}, {&autoName, -1}}) {
            IDXGIAdapter1* a = PickDisplayAdapter(f, name, pref);
            SafeRelease(a);
        }
        u.gpuAutoHigh = autoName == u.gpuHigh;
        f->Release();
    }
    if (!loaded) D3D.Unload();
    // "Intel(R) Iris(R) Xe Graphics" -> "Intel Iris Xe Graphics"
    for (std::wstring* s : {&u.gpu, &u.gpuLow, &u.gpuHigh})
        for (const wchar_t* mark : {L"(R)", L"(TM)", L"(tm)"})
            for (size_t at; (at = s->find(mark)) != std::wstring::npos;) s->erase(at, wcslen(mark));
}

}  // namespace

std::vector<LightCopy> UI_LightCopiesOf(const std::wstring& original) {
    std::vector<LightCopy> out;
    for (const Version& v : LightCopiesOf(original)) out.push_back(LightCopy{v.path, v.w, v.h, v.bytes});
    std::sort(out.begin(), out.end(), [](const LightCopy& a, const LightCopy& b) { return (UINT64)a.w * a.h > (UINT64)b.w * b.h; });
    return out;
}

void UI_ShowVersions() {
    UI_Show();
    if (!u.hwnd || u.page != 0 || DialogOpen()) return;
    StopRecording();
    u.nav = NAV_VIDEO;
    OpenVersions();
    InvalidateRect(u.hwnd, nullptr, FALSE);
}

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
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;  // the feedback text boxes
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
        ForgetScaled();
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
