// Renderer process: owns the wallpaper windows, the D3D11 device and the video pipeline. It
// only exists while the wallpaper is ON; turning the wallpaper off ends this process so every
// byte of RAM / GPU memory it used goes back to the system.
//
// Two video pipelines, switched in place (the windows and the last frame stay on screen):
//  * Silent reader: IMFSourceReader decodes only the video track on the GPU and the D3D11 video
//    processor scales/crops each frame straight into the wallpaper's swap chain. No audio stack,
//    no media session, no presentation clock thread.
//  * Media engine: used for the video's own sound (proper A/V sync) and for silent 4K, where it
//    needs less decoder memory.
// Plus an optional audio-only media engine for "my music".
#include "mfhelp.h"
#include <mfmediaengine.h>
#include <d3d10.h>
#include <dxgi1_4.h>
#include <dwmapi.h>
#include <wtsapi32.h>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace {

constexpr UINT WM_ENGINE_EVENT = WM_APP + 60;
constexpr UINT WM_FIRST_FRAME = WM_APP + 61;
constexpr UINT WM_WALL_LOST = WM_APP + 62;
constexpr UINT WM_DEVICE_LOST = WM_APP + 63;
constexpr UINT WM_READER_FAILED = WM_APP + 64;
constexpr UINT WM_MUSIC_EVENT = WM_APP + 65;
constexpr UINT_PTR TIMER_HEALTH = 1, TIMER_DISPLAY = 2, TIMER_TRIM = 3, TIMER_RECHECK = 4, TIMER_PARK = 5;

struct Target {
    HWND hwnd = nullptr;
    RECT mon{};  // monitor rect, screen coordinates (physical pixels)
    int w = 0, h = 0;
    IDXGISwapChain1* sc = nullptr;
    ID3D11Texture2D* bb = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11VideoProcessorOutputView* out = nullptr;  // silent pipeline
    MFVideoNormalizedRect src{0, 0, 1, 1};
    RECT dst{};
    bool letterbox = false;
};

struct InView {
    ID3D11Texture2D* tex;
    UINT slice;
    ID3D11VideoProcessorInputView* view;
};

struct State {
    HINSTANCE inst = nullptr;
    HWND host = nullptr, ctl = nullptr;
    DWORD hostPid = 0;
    Settings s;
    VideoInfo vi;
    double aspect = 16.0 / 9.0;

    HWND progman = nullptr, parent = nullptr, defview = nullptr;
    bool raised = false;
    std::vector<Target> targets;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IMFDXGIDeviceManager* mgr = nullptr;

    // media engine pipeline
    IMFMediaEngine* eng = nullptr;
    UINT engineGen = 0;          // tags engine events so late ones from a replaced engine are ignored
    bool metadata = false;
    bool videoAudio = false;     // engine plays the video's own sound
    double pendingSeek = 0;      // position to resume at once the engine has loaded
    std::atomic<bool> engineRun{false};  // engine clock running (may be true while the picture is paused)
    std::atomic<double> readerPos{0};    // silent pipeline: position of the frame on screen (s)
    double skipTo = 0;                   // silent pipeline: resuming mid-video, frames before this (s) aren't shown
    std::atomic<UINT> simWanted{0};      // silent pipeline: (h << 16) | w of a lighter version to preview, 0 = off

    // Paused for a while: the video pipeline (decoder frames: most of our memory) is let go and
    // the last frame stays on screen; playback picks up at parkedPos.
    bool parked = false;
    double parkedPos = 0;

    // silent pipeline (owned by the render thread once it runs)
    bool silent = false;
    IMFSourceReader* rd = nullptr;
    ID3D11VideoDevice* vdev = nullptr;
    ID3D11VideoContext* vctx = nullptr;
    ID3D11VideoProcessorEnumerator* venum = nullptr;
    ID3D11VideoProcessor* vp = nullptr;
    UINT fw = 0, fh = 0;  // coded frame size
    RECT ap{};            // visible area inside the coded frame
    std::vector<InView> views;

    HANDLE thread = nullptr, wake = nullptr, timer = nullptr;
    std::atomic<bool> quit{false}, paused{false}, redraw{false}, playing{false};
    std::atomic<int> fpsCap{0};
    std::atomic<double> rate{1.0}, frameDur{1.0 / 30};
    bool exiting = false;
    SRWLOCK lock = SRWLOCK_INIT;  // guards target src/dst rects

    LPARAM pauseMask = 0;
    bool shown = false, shuttingDown = false;
    bool previewing = false;
    Crop previewCrop;
    HWINEVENTHOOK hooks[3] = {};
    HPOWERNOTIFY powerDisplay = nullptr, powerSource = nullptr;
    bool onBattery = false;
    UINT taskbarCreated = 0;
    DWORD exitCode = 0;
} g;

// ---------------------------------------------------------------------------------------

class EngineNotify final : public IMFMediaEngineNotify {
    LONG ref_ = 1;
    UINT msg_, gen_;

public:
    EngineNotify(UINT msg, UINT gen) : msg_(msg), gen_(gen) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return (ULONG)r;
    }
    STDMETHODIMP EventNotify(DWORD ev, DWORD_PTR p1, DWORD p2) override {
        LPARAM info = ev == MF_MEDIA_ENGINE_EVENT_ERROR ? (LPARAM)(p2 ? (HRESULT)p2 : (HRESULT)(0x80070000 | (DWORD)p1)) : 0;
        PostMessageW(g.ctl, msg_, (WPARAM)ev | ((WPARAM)gen_ << 16), info);
        return S_OK;
    }
};

void ReportState() {
    WPARAM st = g.pauseMask ? RS_PAUSED : (g.playing ? RS_PLAYING : RS_LOADING);
    PostMessageW(g.host, WM_VBG_STATE, st, g.pauseMask);
}

void Exit(DWORD code) {
    if (g.exiting) return;  // first reason wins
    Log(L"renderer exit requested, code %lu", code);
    g.exiting = true;
    g.exitCode = code;
    PostQuitMessage((int)code);
}

// fault: a VideoFault, or -1 to work out what's wrong with the video.
void Fail(HRESULT hr, int fault = -1) {
    if (fault < 0) fault = DiagnoseVideo(g.s.video, g.vi);
    Log(L"renderer failure 0x%08lx (fault %d, codec %ls)", (unsigned long)hr, fault, g.vi.codec.c_str());
    PostMessageW(g.host, WM_VBG_FAULT, (WPARAM)fault, (LPARAM)g.vi.subtype.Data1);
    PostMessageW(g.host, WM_VBG_STATE, RS_ERROR, (LPARAM)hr);
    Exit(EXIT_RENDER_ERROR);
}

void RequestRedraw() {
    g.redraw = true;
    if (g.wake) SetEvent(g.wake);
}

void UpdateRects() {
    const Crop& c = g.previewing ? g.previewCrop : g.s.crop;
    AcquireSRWLockExclusive(&g.lock);
    for (auto& t : g.targets) {
        FitRect f = ComputeFit(g.aspect, c, g.s.scale, t.w, t.h);
        t.src = {f.sl, f.st, f.sr, f.sb};
        t.dst = {lroundf(f.dl), lroundf(f.dt), lroundf(f.dr), lroundf(f.db)};
        t.letterbox = t.dst.left > 0 || t.dst.top > 0 || t.dst.right < t.w || t.dst.bottom < t.h;
    }
    ReleaseSRWLockExclusive(&g.lock);
}

bool DeviceLost(HRESULT hr) { return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET; }

// Waits `seconds` (or until woken). High-resolution timer: no global timer-resolution change.
void Nap(double seconds) {
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(std::clamp(seconds, 0.0005, 0.05) * 1e7);
    SetWaitableTimer(g.timer, &due, 0, nullptr, nullptr, FALSE);
    HANDLE waits[2] = {g.wake, g.timer};
    WaitForMultipleObjects(2, waits, FALSE, INFINITE);
}

double Now() {
    static double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart / freq;
}

// ---------------------------------------------------------------------------------------
// Silent pipeline: source reader -> D3D11 video processor -> swap chains

// Previewing a lighter version (Video versions in the settings): each frame is first scaled to
// that version's size, then to the screen as usual, so the wallpaper looks as soft as it would.
// Render thread only (or while it isn't running).
struct Sim {
    UINT w = 0, h = 0;
    ID3D11Texture2D* tex = nullptr;
    ID3D11VideoProcessorOutputView* out = nullptr;  // pass 1 writes the frame here at w x h
    ID3D11VideoProcessorEnumerator* venum = nullptr;
    ID3D11VideoProcessor* vp = nullptr;             // pass 2: from it to the screens
    ID3D11VideoProcessorInputView* in = nullptr;
    std::vector<ID3D11VideoProcessorOutputView*> outs;  // one per target
} sim;

void FreeSim() {
    for (auto* o : sim.outs) o->Release();
    sim.outs.clear();
    SafeRelease(sim.in);
    SafeRelease(sim.vp);
    SafeRelease(sim.venum);
    SafeRelease(sim.out);
    SafeRelease(sim.tex);
    sim.w = sim.h = 0;
}

bool BuildSim(UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
    od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = w;
    cd.InputHeight = h;
    cd.OutputWidth = (UINT)g.targets[0].w;
    cd.OutputHeight = (UINT)g.targets[0].h;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    UINT support = 0;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    bool ok = SUCCEEDED(g.dev->CreateTexture2D(&td, nullptr, &sim.tex)) &&
              SUCCEEDED(g.vdev->CreateVideoProcessorOutputView(sim.tex, g.venum, &od, &sim.out)) &&
              SUCCEEDED(g.vdev->CreateVideoProcessorEnumerator(&cd, &sim.venum)) &&
              SUCCEEDED(sim.venum->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &support)) &&
              (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) &&
              SUCCEEDED(g.vdev->CreateVideoProcessor(sim.venum, 0, &sim.vp)) &&
              SUCCEEDED(g.vdev->CreateVideoProcessorInputView(sim.tex, sim.venum, &iv, &sim.in));
    for (auto& t : g.targets) {
        ID3D11VideoProcessorOutputView* o = nullptr;
        if (ok && SUCCEEDED(g.vdev->CreateVideoProcessorOutputView(t.bb, sim.venum, &od, &o))) sim.outs.push_back(o);
        else ok = false;
    }
    if (!ok) { FreeSim(); return false; }
    sim.w = w;
    sim.h = h;
    g.vctx->VideoProcessorSetStreamFrameFormat(sim.vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    g.vctx->VideoProcessorSetStreamAutoProcessingMode(sim.vp, 0, FALSE);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};  // full-range RGB in and out
    g.vctx->VideoProcessorSetStreamColorSpace(sim.vp, 0, &rgb);
    g.vctx->VideoProcessorSetOutputColorSpace(sim.vp, &rgb);
    D3D11_VIDEO_COLOR black{};
    black.RGBA.A = 1;
    g.vctx->VideoProcessorSetOutputBackgroundColor(sim.vp, FALSE, &black);
    return true;
}

bool ReadReaderFormat() {
    IMFMediaType* mt = nullptr;
    if (FAILED(g.rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mt))) return false;
    MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &g.fw, &g.fh);
    g.ap = {0, 0, (LONG)g.fw, (LONG)g.fh};
    MFVideoArea area{};
    if (SUCCEEDED(mt->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof area, nullptr)) && area.Area.cx > 0 &&
        area.Area.cy > 0 && area.OffsetX.value + area.Area.cx <= (LONG)g.fw && area.OffsetY.value + area.Area.cy <= (LONG)g.fh)
        g.ap = {area.OffsetX.value, area.OffsetY.value, area.OffsetX.value + area.Area.cx, area.OffsetY.value + area.Area.cy};
    UINT32 pn = 1, pd = 1;
    if (FAILED(MFGetAttributeRatio(mt, MF_MT_PIXEL_ASPECT_RATIO, &pn, &pd)) || !pn || !pd) pn = pd = 1;
    LONG aw = g.ap.right - g.ap.left, ah = g.ap.bottom - g.ap.top;
    if (aw > 0 && ah > 0) g.aspect = (double)aw * pn / ((double)ah * pd);
    UINT32 matrix = 0, range = 0;
    mt->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
    mt->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range);
    mt->Release();

    // (Re)create the video processor for this input size.
    FreeSim();
    for (auto& v : g.views) v.view->Release();
    g.views.clear();
    for (auto& t : g.targets) SafeRelease(t.out);
    SafeRelease(g.vp);
    SafeRelease(g.venum);
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = g.fw;
    cd.InputHeight = g.fh;
    cd.OutputWidth = (UINT)g.targets[0].w;
    cd.OutputHeight = (UINT)g.targets[0].h;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(g.vdev->CreateVideoProcessorEnumerator(&cd, &g.venum))) return false;
    if (FAILED(g.vdev->CreateVideoProcessor(g.venum, 0, &g.vp))) return false;
    for (auto& t : g.targets) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(g.vdev->CreateVideoProcessorOutputView(t.bb, g.venum, &od, &t.out))) return false;
    }
    g.vctx->VideoProcessorSetStreamFrameFormat(g.vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    g.vctx->VideoProcessorSetStreamAutoProcessingMode(g.vp, 0, FALSE);  // no "enhancements": less GPU work
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
    in.YCbCr_Matrix = matrix == MFVideoTransferMatrix_BT601 ? 0 : (matrix == MFVideoTransferMatrix_BT709 || ah >= 720) ? 1 : 0;
    in.Nominal_Range = range == MFNominalRange_0_255 ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    g.vctx->VideoProcessorSetStreamColorSpace(g.vp, 0, &in);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};  // full-range RGB for the desktop
    g.vctx->VideoProcessorSetOutputColorSpace(g.vp, &out);
    D3D11_VIDEO_COLOR black{};
    black.RGBA.A = 1;
    g.vctx->VideoProcessorSetOutputBackgroundColor(g.vp, FALSE, &black);
    Log(L"silent pipeline: %ux%u frame, visible %ldx%ld, matrix %u range %u", g.fw, g.fh, aw, ah, matrix, range);
    return true;
}

bool OpenReader(const std::wstring& path) {
    if (FAILED(g.dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&g.vdev)) ||
        FAILED(g.ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&g.vctx)))
        return false;
    IMFAttributes* at = nullptr;
    MF.CreateAttributes(&at, 2);
    at->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, g.mgr);
    at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    HRESULT hr = MF.CreateSourceReaderFromURL(path.c_str(), at, &g.rd);
    at->Release();
    if (FAILED(hr)) { Log(L"source reader failed 0x%08lx", (unsigned long)hr); return false; }
    g.rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    g.rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);  // video only, audio never touched
    bool typed = false;
    for (const GUID& sub : {MFVideoFormat_NV12, MFVideoFormat_P010}) {
        IMFMediaType* mt = nullptr;
        MF.CreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, sub);
        typed = SUCCEEDED(g.rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt));
        mt->Release();
        if (typed) break;
    }
    if (!typed || !ReadReaderFormat()) { Log(L"silent pipeline: unsupported output format"); return false; }
    return true;
}

void CloseReader() {
    FreeSim();
    for (auto& v : g.views) v.view->Release();
    g.views.clear();
    for (auto& t : g.targets) SafeRelease(t.out);
    SafeRelease(g.vp);
    SafeRelease(g.venum);
    SafeRelease(g.vctx);
    SafeRelease(g.vdev);
    SafeRelease(g.rd);
}

// Input view for the decoder surface held by `s` (views are cached per texture array slice).
ID3D11VideoProcessorInputView* ViewFor(IMFSample* s) {
    IMFMediaBuffer* buf = nullptr;
    IMFDXGIBuffer* db = nullptr;
    ID3D11Texture2D* tex = nullptr;
    UINT slice = 0;
    ID3D11VideoProcessorInputView* view = nullptr;
    if (SUCCEEDED(s->GetBufferByIndex(0, &buf)) && SUCCEEDED(buf->QueryInterface(__uuidof(IMFDXGIBuffer), (void**)&db)) &&
        SUCCEEDED(db->GetResource(__uuidof(ID3D11Texture2D), (void**)&tex)) && SUCCEEDED(db->GetSubresourceIndex(&slice))) {
        for (auto& v : g.views)
            if (v.tex == tex && v.slice == slice) { view = v.view; break; }
        if (!view) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
            iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            iv.Texture2D.ArraySlice = slice;
            if (SUCCEEDED(g.vdev->CreateVideoProcessorInputView(tex, g.venum, &iv, &view))) {
                if (g.views.size() > 96) {  // decoder reallocated its surfaces
                    for (auto& v : g.views) v.view->Release();
                    g.views.clear();
                }
                g.views.push_back({tex, slice, view});
            }
        }
    }
    SafeRelease(tex);
    SafeRelease(db);
    SafeRelease(buf);
    return view;
}

// Returns false on device loss.
bool BlitAll(ID3D11VideoProcessorInputView* view) {
    bool ok = true;
    const float aw = (float)(g.ap.right - g.ap.left), ah = (float)(g.ap.bottom - g.ap.top);
    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = view;
    UINT want = g.simWanted;
    if (want != ((sim.h << 16) | sim.w)) {
        FreeSim();
        if (want && !BuildSim(want & 0xFFFF, want >> 16)) {
            Log(L"preview of a %ux%u version isn't supported here", want & 0xFFFF, want >> 16);
            g.simWanted = 0;
        }
    }
    AcquireSRWLockShared(&g.lock);
    if (sim.vp) {
        // The whole visible frame at the lighter version's size, then from that to each screen.
        RECT src = g.ap, dst{0, 0, (LONG)sim.w, (LONG)sim.h};
        g.vctx->VideoProcessorSetStreamSourceRect(g.vp, 0, TRUE, &src);
        g.vctx->VideoProcessorSetStreamDestRect(g.vp, 0, TRUE, &dst);
        g.vctx->VideoProcessorSetOutputTargetRect(g.vp, TRUE, &dst);
        bool scaled = SUCCEEDED(g.vctx->VideoProcessorBlt(g.vp, sim.out, 0, 1, &st));
        D3D11_VIDEO_PROCESSOR_STREAM st2{};
        st2.Enable = TRUE;
        st2.pInputSurface = sim.in;
        for (size_t i = 0; scaled && i < g.targets.size(); i++) {
            Target& t = g.targets[i];
            RECT s2{lroundf(t.src.left * sim.w), lroundf(t.src.top * sim.h), lroundf(t.src.right * sim.w),
                    lroundf(t.src.bottom * sim.h)};
            RECT full{0, 0, t.w, t.h};
            g.vctx->VideoProcessorSetStreamSourceRect(sim.vp, 0, TRUE, &s2);
            g.vctx->VideoProcessorSetStreamDestRect(sim.vp, 0, TRUE, &t.dst);
            g.vctx->VideoProcessorSetOutputTargetRect(sim.vp, TRUE, &full);
            if (FAILED(g.vctx->VideoProcessorBlt(sim.vp, sim.outs[i], 0, 1, &st2))) continue;
            if (DeviceLost(t.sc->Present(1, 0))) ok = false;
        }
        ReleaseSRWLockShared(&g.lock);
        return ok;
    }
    for (auto& t : g.targets) {
        RECT src{g.ap.left + lroundf(t.src.left * aw), g.ap.top + lroundf(t.src.top * ah), g.ap.left + lroundf(t.src.right * aw),
                 g.ap.top + lroundf(t.src.bottom * ah)};
        RECT full{0, 0, t.w, t.h};
        g.vctx->VideoProcessorSetStreamSourceRect(g.vp, 0, TRUE, &src);
        g.vctx->VideoProcessorSetStreamDestRect(g.vp, 0, TRUE, &t.dst);
        g.vctx->VideoProcessorSetOutputTargetRect(g.vp, TRUE, &full);  // outside dst = black background
        if (FAILED(g.vctx->VideoProcessorBlt(g.vp, t.out, 0, 1, &st))) continue;
        if (DeviceLost(t.sc->Present(1, 0))) ok = false;
    }
    ReleaseSRWLockShared(&g.lock);
    return ok;
}

DWORD WINAPI SilentThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMFSample *next = nullptr, *cur = nullptr;
    ID3D11VideoProcessorInputView* curView = nullptr;
    LONGLONG nextTs = 0, nextFileTs = 0, loopOffset = 0, lastTs = 0;
    double baseVt = 0, baseClock = 0, lastPresent = -1, pausedVt = 0, lastRate = g.rate;
    bool clock = false, wasPaused = false, first = true, looped = false;
    DWORD result = 0;
    // Resuming mid-video: seeking lands on the key frame before; decode from there without showing.
    LONGLONG skipTo = (LONGLONG)((g.skipTo - g.frameDur / 2) * 1e7);
    int skipped = 0;

    auto videoTime = [&](double now) { return baseVt + (now - baseClock) * lastRate; };
    while (!g.quit) {
        bool redraw = g.redraw.exchange(false);
        if (g.paused && !first) {
            if (!wasPaused) { wasPaused = true; pausedVt = clock ? videoTime(Now()) : 0; }
            if (redraw && curView) { if (!BlitAll(curView)) { result = 1; break; } }
            else WaitForSingleObject(g.wake, INFINITE);
            continue;
        }
        if (wasPaused) { wasPaused = false; baseVt = pausedVt; baseClock = Now(); }
        double r = g.rate;
        if (r != lastRate) { double n = Now(); baseVt = videoTime(n); baseClock = n; lastRate = r; }
        if (redraw && curView && !BlitAll(curView)) { result = 1; break; }

        if (!next) {
            DWORD flags = 0, idx = 0;
            LONGLONG ts = 0;
            IMFSample* s = nullptr;
            HRESULT hr = g.rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &s);
            if (FAILED(hr)) { Log(L"ReadSample failed 0x%08lx", (unsigned long)hr); result = 2; break; }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                curView = nullptr;  // views die with the old processor
                if (!ReadReaderFormat()) { SafeRelease(s); result = 2; break; }
                PostMessageW(g.ctl, WM_ENGINE_EVENT, MF_MEDIA_ENGINE_EVENT_FORMATCHANGE, 0);
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                // Loop: jump back to the start and keep the timeline running.
                SafeRelease(s);
                PROPVARIANT pv;
                PropVariantInit(&pv);
                pv.vt = VT_I8;
                pv.hVal.QuadPart = 0;
                g.rd->SetCurrentPosition(GUID_NULL, pv);
                loopOffset = lastTs + (LONGLONG)(g.frameDur * 1e7);
                looped = true;
                skipTo = 0;
                continue;
            }
            if (!s) continue;
            if (first && ts < skipTo) { SafeRelease(s); skipped++; continue; }
            if (first && skipTo > 0) Log(L"resumed at %.2fs (%d frames decoded to get there)", ts / 1e7, skipped);
            // The first frame comes one frame after the last, whatever time the file starts at
            // (some encoders start at a frame or two in).
            if (looped) { loopOffset -= ts; looped = false; }
            next = s;
            nextFileTs = ts;
            nextTs = ts + loopOffset;
            lastTs = nextTs;
        }

        double now = Now();
        if (!clock) { baseVt = nextTs / 1e7; baseClock = now; clock = true; }
        double vt = videoTime(now), due = nextTs / 1e7;
        if (due > vt + 0.0005 && !first) { Nap((due - vt) / lastRate); continue; }
        if (vt - due > 0.25) { baseVt = due; baseClock = now; }  // fell far behind (busy system): resync
        const int cap = g.fpsCap;
        if (cap > 0 && !first && now - lastPresent < 1.0 / cap - 0.002) { SafeRelease(next); continue; }

        ID3D11VideoProcessorInputView* view = ViewFor(next);
        if (!view) { Log(L"silent pipeline: frame is not a GPU surface"); result = 2; break; }
        if (!BlitAll(view)) { result = 1; break; }
        lastPresent = now;
        g.readerPos = nextFileTs / 1e7;
        SafeRelease(cur);  // keep the frame on screen alive for redraws (crop edits while paused)
        cur = next;
        curView = view;
        next = nullptr;
        if (first) { first = false; PostMessageW(g.ctl, WM_FIRST_FRAME, 0, 0); }
    }
    SafeRelease(next);
    SafeRelease(cur);
    if (result == 1) PostMessageW(g.ctl, WM_DEVICE_LOST, 0, 0);
    else if (result == 2) PostMessageW(g.ctl, WM_READER_FAILED, 0, 0);
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------------------
// Media engine pipeline (used when the video should be audible)

bool PresentEngineFrame(bool* transferred) {
    *transferred = false;
    bool ok = true;
    const MFARGB black{0, 0, 0, 255};
    AcquireSRWLockShared(&g.lock);
    for (auto& t : g.targets) {
        if (t.letterbox) {
            const float c[4] = {0, 0, 0, 1};
            g.ctx->ClearRenderTargetView(t.rtv, c);
        }
        if (FAILED(g.eng->TransferVideoFrame(t.bb, &t.src, &t.dst, &black))) continue;
        *transferred = true;
        if (DeviceLost(t.sc->Present(1, 0))) ok = false;
    }
    ReleaseSRWLockShared(&g.lock);
    return ok;
}

// Frame pump: sleeps until the next frame is due instead of spinning at the refresh rate.
DWORD WINAPI EngineThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    double lastPresent = 0, lastPts = -1;
    bool firstFrame = false;
    while (!g.quit) {
        bool redraw = g.redraw.exchange(false);
        bool active = g.playing && !g.paused;
        if (!active && !redraw) {
            if (g.playing && g.engineRun) {
                // Picture paused but the video's sound keeps playing: drain frames, don't draw them.
                LONGLONG pts = 0;
                g.eng->OnVideoStreamTick(&pts);
                WaitForSingleObject(g.wake, 250);
            } else {
                WaitForSingleObject(g.wake, INFINITE);
            }
            continue;
        }
        const int cap = g.fpsCap;
        const double minGap = cap > 0 ? 1.0 / cap : 0.0, r = g.rate > 0.05 ? g.rate.load() : 1.0, frameDur = g.frameDur;
        double wait = 0.002;
        bool present = redraw;
        if (active) {
            double since = Now() - lastPresent;
            if (cap > 0 && since < minGap - 0.001 && !redraw) {
                wait = minGap - since;
            } else {
                LONGLONG pts = 0;
                if (g.eng->OnVideoStreamTick(&pts) == S_OK) {
                    present = true;
                    lastPts = pts / 1e7;
                }
                // Sleep until just after the next frame is due (waking early only means polling again).
                if (lastPts >= 0) {
                    double due = (lastPts + frameDur - g.eng->GetCurrentTime()) / r;
                    wait = std::clamp(due + 0.001, 0.001, 2 * frameDur / r);
                }
                if (present && cap > 0) wait = std::max(wait, minGap);
            }
        }
        if (present) {
            bool transferred = false;
            if (!PresentEngineFrame(&transferred)) {
                PostMessageW(g.ctl, WM_DEVICE_LOST, 0, 0);
                break;
            }
            if (transferred) {
                lastPresent = Now();
                if (!firstFrame) { firstFrame = true; PostMessageW(g.ctl, WM_FIRST_FRAME, 0, 0); }
            }
        }
        if (active) Nap(wait);
    }
    CoUninitialize();
    return 0;
}

bool StartEngine(const std::wstring& path) {
    IMFMediaEngineClassFactory* cf = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IMFMediaEngineClassFactory), (void**)&cf);
    if (FAILED(hr)) { Fail(hr); return false; }
    IMFAttributes* at = nullptr;
    MF.CreateAttributes(&at, 3);
    g.videoAudio = WantVideoAudio(g.s);
    EngineNotify* cb = new EngineNotify(WM_ENGINE_EVENT, ++g.engineGen & 0xFFFF);
    at->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, cb);
    at->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, g.mgr);
    at->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    hr = cf->CreateInstance(g.videoAudio ? 0 : MF_MEDIA_ENGINE_FORCEMUTE, at, &g.eng);
    at->Release();
    cb->Release();
    cf->Release();
    if (FAILED(hr)) { Fail(hr); return false; }
    g.eng->SetLoop(TRUE);
    g.eng->SetAutoPlay(FALSE);
    g.eng->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
    g.eng->SetMuted(g.videoAudio ? FALSE : TRUE);
    g.eng->SetVolume(g.s.volume / 100.0);
    g.eng->SetDefaultPlaybackRate(g.rate);
    g.eng->SetPlaybackRate(g.rate);
    BSTR b = SysAllocString(path.c_str());
    hr = g.eng->SetSource(b);
    SysFreeString(b);
    if (FAILED(hr)) { Fail(hr); return false; }
    g.eng->Load();
    g.thread = CreateThread(nullptr, 0, EngineThread, nullptr, 0, nullptr);
    Log(L"media engine pipeline (%ls)", g.videoAudio ? L"with the video's sound" : L"muted");
    return true;
}

void ApplyPauseState();
void ArmPark();

void OnEngineEvent(DWORD ev, UINT gen, LPARAM info) {
    if (!g.eng) {  // silent pipeline reuses this message for format changes
        if (ev == MF_MEDIA_ENGINE_EVENT_FORMATCHANGE) { UpdateRects(); RequestRedraw(); }
        return;
    }
    if (gen != (g.engineGen & 0xFFFF)) return;  // from an engine that has since been replaced
    switch (ev) {
        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: {
            g.metadata = true;
            DWORD w = 0, h = 0;
            if (SUCCEEDED(g.eng->GetNativeVideoSize(&w, &h)) && w && h) g.aspect = (double)w / h;
            UpdateRects();
            if (g.pendingSeek > 0.05) g.eng->SetCurrentTime(g.pendingSeek);  // resume where we were
            g.pendingSeek = 0;
            // In frame-server mode the engine only reports PLAYING once we pull frames, so the
            // pump runs from here on (whenever not paused).
            g.playing = true;
            ApplyPauseState();
            ReportState();
            break;
        }
        case MF_MEDIA_ENGINE_EVENT_FORMATCHANGE: {
            DWORD w = 0, h = 0;
            if (SUCCEEDED(g.eng->GetNativeVideoSize(&w, &h)) && w && h) g.aspect = (double)w / h;
            UpdateRects();
            RequestRedraw();
            break;
        }
        case MF_MEDIA_ENGINE_EVENT_ERROR:
            Fail((HRESULT)info);
            break;
    }
}

// Stops the video pipeline but keeps the windows: the last frame stays on screen.
void StopPipeline() {
    g.quit = true;
    if (g.wake) SetEvent(g.wake);
    if (g.thread) { WaitForSingleObject(g.thread, 3000); CloseHandle(g.thread); g.thread = nullptr; }
    g.quit = false;
    if (g.eng) { g.eng->Shutdown(); SafeRelease(g.eng); }
    CloseReader();
    g.silent = false;
    g.playing = false;
    g.metadata = false;
    g.engineRun = false;
    g.videoAudio = false;
    g.redraw = false;
}

double CurrentPos() {
    if (g.parked) return g.parkedPos;
    if (g.eng) return g.eng->GetCurrentTime();
    return g.readerPos;
}

bool StartPlayback(double startPos = 0) {
    const std::wstring& path = g.s.video;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) { Fail(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)); return false; }
    ProbeVideo(path, g.vi);
    Log(L"source %ls: %ux%u %.2ffps %ls, %zu audio track(s)", path.c_str(), g.vi.w, g.vi.h, g.vi.fps, g.vi.codec.c_str(),
        g.vi.audioStreams.size());
    if (g.vi.aspect > 0) g.aspect = g.vi.aspect;
    g.frameDur = g.vi.fps > 1 ? 1.0 / g.vi.fps : 1.0 / 30;
    g.rate = std::clamp(g.s.speed, 25, 200) / 100.0;
    // The reader whenever the video's sound isn't wanted. Measured against the media engine at
    // 1080p, 1440p and 4K: the same CPU, 15-40 MB less graphics memory (which is RAM on integrated
    // GPUs) and ~10 MB less RAM. (Its private bytes look higher: the decoder's frames are charged to
    // the process, while the engine's are shared allocations.) The engine also opens an audio
    // stream whenever the file has an audio track, even force-muted.
    bool useReader = g.s.pipeline == 2 || (g.s.pipeline == 0 && !WantVideoAudio(g.s));
    if (startPos >= g.vi.duration - 0.1) startPos = 0;
    g.readerPos = startPos;
    g.skipTo = startPos;
    g.parked = false;
    if (useReader && OpenReader(path)) {
        g.silent = true;
        g.playing = true;
        if (startPos > 0.05) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt = VT_I8;
            pv.hVal.QuadPart = (LONGLONG)(startPos * 1e7);
            g.rd->SetCurrentPosition(GUID_NULL, pv);
        }
        UpdateRects();
        g.thread = CreateThread(nullptr, 0, SilentThread, nullptr, 0, nullptr);
        Log(L"silent pipeline started");
        ArmPark();  // started while paused (e.g. the desktop was already covered)
        return true;
    }
    CloseReader();
    g.pendingSeek = startPos;
    bool ok = StartEngine(path);
    ArmPark();
    return ok;
}

// ---------------------------------------------------------------------------------------
// "My music": an audio-only media engine playing the user's songs in order (or shuffled).

struct Music {
    IMFMediaEngine* eng = nullptr;
    UINT gen = 0;
    std::vector<std::wstring> list;
    std::vector<int> order;
    int pos = 0, failures = 0;
    bool shuffle = false;
} mu;

void ReportTrack(int idx) { PostMessageW(g.host, WM_VBG_TRACK, (WPARAM)(INT_PTR)idx, 0); }

void ShuffleOrder() {
    for (int i = (int)mu.order.size() - 1; i > 0; i--) {
        static unsigned long long seed = GetTickCount64() * 2654435761ULL;
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        std::swap(mu.order[i], mu.order[(int)((seed >> 33) % (unsigned long long)(i + 1))]);
    }
}

void MusicPlayCurrent() {
    const std::wstring& f = mu.list[mu.order[mu.pos]];
    BSTR b = SysAllocString(f.c_str());
    HRESULT hr = mu.eng->SetSource(b);
    SysFreeString(b);
    mu.eng->Load();
    Log(L"music: playing %ls (0x%08lx)", f.c_str(), (unsigned long)hr);
    ReportTrack(mu.order[mu.pos]);
    ApplyPauseState();
}

void MusicNext() {
    if (++mu.pos >= (int)mu.order.size()) {
        mu.pos = 0;
        if (mu.shuffle) ShuffleOrder();
    }
    MusicPlayCurrent();
}

void StopMusic() {
    if (!mu.eng) return;
    mu.eng->Shutdown();
    SafeRelease(mu.eng);
    mu.list.clear();
    mu.order.clear();
    ReportTrack(-1);
}

void StartMusic() {
    StopMusic();
    if (g.s.sound != 2) return;
    mu.list = LoadPlaylist();
    if (mu.list.empty()) { Log(L"music: no songs chosen"); ReportTrack(-1); return; }
    IMFMediaEngineClassFactory* cf = nullptr;
    IMFAttributes* at = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IMFMediaEngineClassFactory), (void**)&cf);
    if (SUCCEEDED(hr)) hr = MF.CreateAttributes(&at, 2);
    if (SUCCEEDED(hr)) {
        EngineNotify* cb = new EngineNotify(WM_MUSIC_EVENT, ++mu.gen & 0xFFFF);
        at->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, cb);
        at->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, 11 /* AudioCategory_Media */);
        hr = cf->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY, at, &mu.eng);
        cb->Release();
    }
    SafeRelease(at);
    SafeRelease(cf);
    if (FAILED(hr)) { Log(L"music: engine failed 0x%08lx", (unsigned long)hr); ReportTrack(-2); return; }
    mu.eng->SetAutoPlay(FALSE);
    mu.eng->SetLoop(mu.list.size() == 1 ? TRUE : FALSE);  // one song: seamless loop
    mu.eng->SetVolume(g.s.volume / 100.0);
    mu.shuffle = g.s.shuffle;
    mu.order.resize(mu.list.size());
    for (size_t i = 0; i < mu.order.size(); i++) mu.order[i] = (int)i;
    if (mu.shuffle) ShuffleOrder();
    mu.pos = 0;
    mu.failures = 0;
    Log(L"music: %zu song(s)%ls", mu.list.size(), mu.shuffle ? L", shuffled" : L"");
    MusicPlayCurrent();
}

void OnMusicEvent(DWORD ev, UINT gen, LPARAM info) {
    if (!mu.eng || gen != (mu.gen & 0xFFFF)) return;
    if (ev == MF_MEDIA_ENGINE_EVENT_PLAYING) {
        mu.failures = 0;
    } else if (ev == MF_MEDIA_ENGINE_EVENT_ENDED) {
        MusicNext();
    } else if (ev == MF_MEDIA_ENGINE_EVENT_ERROR) {
        Log(L"music: can't play %ls (0x%08lx), skipping", mu.list[mu.order[mu.pos]].c_str(), (unsigned long)info);
        if (++mu.failures >= (int)mu.list.size()) {
            Log(L"music: none of the songs could be played");
            StopMusic();
            ReportTrack(-2);
        } else {
            MusicNext();
        }
    }
}

// Picture and sound follow the pause reasons (covered / locked / screen off / battery), except
// that with "keep sound playing" the sound carries on while only the picture stops.
// Graphics memory this process holds (decoder frames, swap chains); on integrated GPUs it's RAM.
UINT64 GraphicsMemory() {
    IDXGIDevice* dd = nullptr;
    IDXGIAdapter* a = nullptr;
    IDXGIAdapter3* a3 = nullptr;
    UINT64 total = 0;
    if (g.dev && SUCCEEDED(g.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && SUCCEEDED(dd->GetAdapter(&a)) &&
        SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&a3))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
        a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
        a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
        total = local.CurrentUsage + nonLocal.CurrentUsage;
    }
    SafeRelease(a3);
    SafeRelease(a);
    SafeRelease(dd);
    return total;
}

// The video's own sound keeps its pipeline running while the picture is paused.
bool CanPark() { return g.pauseMask != 0 && !g.parked && !(g.videoAudio && g.s.keepSound) && (g.eng || g.silent); }

// Let the decoder go once paused a while: soon when nobody can see the desktop (locked, screen
// off), later when covered, so a quick look at the desktop doesn't wait for it.
void ArmPark() {
    if (CanPark()) SetTimer(g.ctl, TIMER_PARK, (g.pauseMask & (PR_LOCKED | PR_DISPLAY_OFF)) ? 5000 : 30000, nullptr);
    else KillTimer(g.ctl, TIMER_PARK);
}

void Park() {
    if (!CanPark()) return;
    g.parkedPos = CurrentPos();
    StopPipeline();
    g.parked = true;
    // The driver's scratch memory too, then give the now unused pages back.
    g.ctx->ClearState();
    g.ctx->Flush();
    IDXGIDevice3* d3 = nullptr;
    if (SUCCEEDED(g.dev->QueryInterface(__uuidof(IDXGIDevice3), (void**)&d3))) {
        d3->Trim();
        d3->Release();
    }
    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    Log(L"paused a while: video decoder released (at %.2fs)", g.parkedPos);
}

// Back to a running pipeline at the parked spot (the frame on screen stays until the next one).
void Unpark() {
    if (!g.parked) return;
    Log(L"reopening the video at %.2fs", g.parkedPos);
    StartPlayback(g.parkedPos);
}

void ApplyPauseState() {
    bool videoPaused = g.pauseMask != 0;
    g.paused = videoPaused;
    if (!videoPaused) Unpark();
    ArmPark();
    bool soundRuns = !videoPaused || g.s.keepSound;
    if (g.eng && g.metadata) {
        bool run = !videoPaused || (g.videoAudio && g.s.keepSound);
        g.engineRun = run;
        if (run) g.eng->Play();
        else g.eng->Pause();
    }
    if (mu.eng) {
        if (soundRuns) mu.eng->Play();
        else mu.eng->Pause();
        static int last = -1;
        if (last != (int)soundRuns) Log(L"music: %ls", soundRuns ? L"playing" : L"paused with the video");
        last = soundRuns;
    }
    if (g.wake) SetEvent(g.wake);
}

// ---------------------------------------------------------------------------------------
// Desktop attachment

bool FindDesktop() {
    g.progman = FindWindowW(L"Progman", nullptr);
    if (!g.progman) return false;
    DWORD_PTR res = 0;
    // Ask Explorer to create the wallpaper WorkerW (same message Windows uses for fades).
    SendMessageTimeoutW(g.progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &res);
    g.raised = (GetWindowLongPtrW(g.progman, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) != 0;
    g.defview = FindWindowExW(g.progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (g.raised && g.defview) {
        // Windows 11 24H2+: icons (DefView) and the static wallpaper (WorkerW) are both children
        // of Progman. We slot in between them as a layered child of Progman.
        g.parent = g.progman;
        return true;
    }
    // Classic layout: DefView lives in a top-level WorkerW; the wallpaper WorkerW follows it.
    g.raised = false;
    HWND worker = nullptr;
    EnumWindows(
        [](HWND top, LPARAM lp) -> BOOL {
            if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
                *(HWND*)lp = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&worker);
    g.parent = worker ? worker : g.progman;
    return g.parent != nullptr;
}
LRESULT CALLBACK WallProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: ValidateRect(h, nullptr); return 0;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_DESTROY:
            // Destroyed by someone else (Explorer restarted / crashed): start over.
            if (!g.shuttingDown && GetWindowLongPtrW(h, GWLP_USERDATA) == 1) PostMessageW(g.ctl, WM_WALL_LOST, 0, 0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

std::vector<RECT> WantedMonitors() {
    std::vector<RECT> v;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(mon, &mi);
            auto& v = *(std::vector<RECT>*)lp;
            if (g.s.monitors == 1 && !(mi.dwFlags & MONITORINFOF_PRIMARY)) return TRUE;
            if (mi.dwFlags & MONITORINFOF_PRIMARY) v.insert(v.begin(), mi.rcMonitor);
            else v.push_back(mi.rcMonitor);
            return TRUE;
        },
        (LPARAM)&v);
    return v;
}

void PlaceTarget(Target& t, bool show) {
    POINT pt[2] = {{t.mon.left, t.mon.top}, {t.mon.right, t.mon.bottom}};
    MapWindowPoints(HWND_DESKTOP, g.parent, pt, 2);
    SetWindowPos(t.hwnd, g.raised ? g.defview : HWND_TOP, pt[0].x, pt[0].y, t.w, t.h,
                 SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0));
}

bool CreateTargets(IDXGIFactory2* fac) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WallProc;
    wc.hInstance = g.inst;
    wc.lpszClassName = WALL_CLASS;
    RegisterClassW(&wc);

    for (const RECT& mon : WantedMonitors()) {
        Target t;
        t.mon = mon;
        t.w = mon.right - mon.left;
        t.h = mon.bottom - mon.top;
        DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (g.raised ? WS_EX_LAYERED : 0);
        t.hwnd = CreateWindowExW(ex, WALL_CLASS, L"VideoBG wallpaper", WS_POPUP, 0, 0, t.w, t.h, nullptr, nullptr, g.inst, nullptr);
        if (!t.hwnd) continue;
        if (g.raised) SetLayeredWindowAttributes(t.hwnd, 0, 255, LWA_ALPHA);
        SetParent(t.hwnd, g.parent);
        LONG_PTR st = GetWindowLongPtrW(t.hwnd, GWL_STYLE);
        st = (st & ~(LONG_PTR)WS_POPUP) | WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED;
        SetWindowLongPtrW(t.hwnd, GWL_STYLE, st);
        SetWindowPos(t.hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        PlaceTarget(t, false);

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = (UINT)t.w;
        sd.Height = (UINT)t.h;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        HRESULT hr = fac->CreateSwapChainForHwnd(g.dev, t.hwnd, &sd, nullptr, nullptr, &t.sc);
        if (FAILED(hr)) {
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            hr = fac->CreateSwapChainForHwnd(g.dev, t.hwnd, &sd, nullptr, nullptr, &t.sc);
        }
        if (FAILED(hr)) { DestroyWindow(t.hwnd); continue; }
        fac->MakeWindowAssociation(t.hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        t.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&t.bb);
        g.dev->CreateRenderTargetView(t.bb, nullptr, &t.rtv);
        SetWindowLongPtrW(t.hwnd, GWLP_USERDATA, 1);
        g.targets.push_back(t);
    }
    return !g.targets.empty();
}

void ShowTargets() {
    if (g.shown) return;
    g.shown = true;
    for (auto& t : g.targets) PlaceTarget(t, true);
    RequestRedraw();
    SetTimer(g.ctl, TIMER_TRIM, 4000, nullptr);
}

bool CreateDevice() {
    IDXGIFactory1* f1 = nullptr;
    if (FAILED(D3D.CreateFactory1(__uuidof(IDXGIFactory1), (void**)&f1))) return false;
    std::wstring gpu;
    IDXGIAdapter1* ad = PickDisplayAdapter(f1, &gpu);
    const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                    D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D.CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fl,
                                  (UINT)std::size(fl), D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx);
    if (FAILED(hr))  // no hardware video support: MF falls back to software decoding
        hr = D3D.CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, (UINT)std::size(fl), D3D11_SDK_VERSION, &g.dev,
                              nullptr, &g.ctx);
    SafeRelease(ad);
    Log(L"device on %ls: 0x%08lx", gpu.c_str(), (unsigned long)hr);
    if (FAILED(hr)) { f1->Release(); return false; }

    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(g.dev->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt))) {
        mt->SetMultithreadProtected(TRUE);  // MF decodes on its own threads with this device
        mt->Release();
    }
    UINT token = 0;
    if (FAILED(MF.CreateDXGIDeviceManager(&token, &g.mgr)) || FAILED(g.mgr->ResetDevice(g.dev, token))) {
        f1->Release();
        return false;
    }
    // Swap chains must come from the factory that owns the device's adapter.
    IDXGIDevice* dd = nullptr;
    IDXGIAdapter* da = nullptr;
    IDXGIFactory2* f2 = nullptr;
    g.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd);
    if (dd) dd->GetAdapter(&da);
    if (da) da->GetParent(__uuidof(IDXGIFactory2), (void**)&f2);
    bool ok = f2 && CreateTargets(f2);
    SafeRelease(f2);
    SafeRelease(da);
    SafeRelease(dd);
    f1->Release();
    return ok;
}

// ---------------------------------------------------------------------------------------
// Pausing

void SetPause(LPARAM bit, bool on) {
    LPARAM old = g.pauseMask;
    g.pauseMask = on ? (old | bit) : (old & ~bit);
    if (g.pauseMask == old) return;
    if (!!g.pauseMask != !!old) ApplyPauseState();
    else ArmPark();  // still paused, for another reason (covered, then locked)
    Log(L"pause mask 0x%llx", (unsigned long long)g.pauseMask);
    ReportState();
}

struct CoverScan {
    std::vector<RECT> mons;
    std::vector<bool> covered;
    int mode;
    DWORD self;
};

BOOL CALLBACK CoverEnum(HWND h, LPARAM lp) {
    auto& c = *(CoverScan*)lp;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == c.self || pid == g.hostPid) return TRUE;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return TRUE;  // overlays, desktop widgets, ...
    if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT)) return TRUE;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (cloaked) return TRUE;  // other virtual desktops, suspended UWP apps
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        return TRUE;
    RECT wr;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof wr))) GetWindowRect(h, &wr);
    bool zoomed = c.mode == 2 && IsZoomed(h);
    POINT mid{(wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2};
    for (size_t i = 0; i < c.mons.size(); i++) {
        const RECT& m = c.mons[i];
        bool full = wr.left <= m.left && wr.top <= m.top && wr.right >= m.right && wr.bottom >= m.bottom;
        if (full || (zoomed && PtInRect(&m, mid))) c.covered[i] = true;
    }
    return TRUE;
}

void CheckCovered() {
    bool covered = false;
    if (g.s.pauseCover != 0 && !g.targets.empty()) {
        CoverScan c;
        for (auto& t : g.targets) c.mons.push_back(t.mon);
        c.covered.assign(c.mons.size(), false);
        c.mode = g.s.pauseCover;
        c.self = GetCurrentProcessId();
        EnumWindows(CoverEnum, (LPARAM)&c);
        covered = true;
        for (bool b : c.covered) covered = covered && b;
    }
    SetPause(PR_COVERED, covered);
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG idObject, LONG, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW) return;
    SetTimer(g.ctl, TIMER_RECHECK, 150, nullptr);  // coalesce bursts into one scan
}

void ApplyBatteryPolicy() { SetPause(PR_BATTERY, g.s.onBattery == 1 && g.onBattery); }

void HealthCheck() {
    if (!IsWindow(g.host)) { Exit(0); return; }
    if (!IsWindow(g.progman)) { Exit(EXIT_RENDER_RESTART); return; }
    for (auto& t : g.targets) {
        if (!IsWindow(t.hwnd) || GetParent(t.hwnd) != g.parent) { Exit(EXIT_RENDER_RESTART); return; }
    }
    if (g.raised && g.shown) {
        // Explorer may re-stack its WorkerW above us (e.g. after a static wallpaper change).
        HWND expectedPrev = g.defview;
        for (auto& t : g.targets) {
            if (GetWindow(t.hwnd, GW_HWNDPREV) != expectedPrev)
                SetWindowPos(t.hwnd, expectedPrev, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            expectedPrev = t.hwnd;
        }
    }
}

void Reload() {
    Settings ns;
    LoadSettings(ns);
    Settings old = g.s;
    g.s = ns;
    if (ns.monitors != old.monitors) { Exit(EXIT_RENDER_RESTART); return; }  // new set of windows
    // A new video, or the video's own sound switching on/off, needs the other pipeline. It is
    // swapped in place: the windows and the current frame stay, playback resumes at the same spot.
    bool videoChanged = _wcsicmp(ns.video.c_str(), old.video.c_str()) != 0;
    if (videoChanged || WantVideoAudio(ns) != WantVideoAudio(old)) {
        double pos = videoChanged ? 0 : CurrentPos();
        if (videoChanged) g.simWanted = 0;
        Log(L"switching video pipeline in place (%ls, at %.2fs)", videoChanged ? L"new video" : L"sound changed", pos);
        StopPipeline();
        if (!StartPlayback(pos)) return;
    }
    // A parked pipeline has no frame to show a new crop or fit with.
    if (g.parked && (memcmp(&ns.crop, &old.crop, sizeof(Crop)) != 0 || ns.scale != old.scale)) Unpark();
    g.fpsCap = g.s.fpsCap;
    double r = std::clamp(g.s.speed, 25, 200) / 100.0;
    g.rate = r;
    if (g.eng) {
        g.eng->SetDefaultPlaybackRate(r);
        g.eng->SetPlaybackRate(r);
        g.eng->SetVolume(g.s.volume / 100.0);
    }
    if (ns.sound != 2) StopMusic();
    else if (!mu.eng || old.sound != 2 || old.shuffle != ns.shuffle || LoadPlaylist() != mu.list) StartMusic();
    else mu.eng->SetVolume(ns.volume / 100.0);
    UpdateRects();
    RequestRedraw();
    ApplyBatteryPolicy();
    CheckCovered();
    ApplyPauseState();
}

LRESULT CALLBACK CtlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ENGINE_EVENT: OnEngineEvent((DWORD)(w & 0xFFFF), (UINT)(w >> 16), l); return 0;
        case WM_MUSIC_EVENT: OnMusicEvent((DWORD)(w & 0xFFFF), (UINT)(w >> 16), l); return 0;
        case WM_FIRST_FRAME:
            Log(L"first frame presented on %zu display(s)", g.targets.size());
            ShowTargets();
            ReportState();
            return 0;
        case WM_WALL_LOST: Exit(EXIT_RENDER_RESTART); return 0;
        case WM_DEVICE_LOST: Log(L"graphics device lost"); Exit(EXIT_RENDER_RESTART); return 0;
        case WM_READER_FAILED:
            // Something in the silent pipeline isn't supported here: fall back to the media engine.
            if (g.thread) { WaitForSingleObject(g.thread, 2000); CloseHandle(g.thread); g.thread = nullptr; }
            Log(L"silent pipeline unsupported here, using the media engine");
            CloseReader();
            g.silent = false;
            g.playing = false;
            g.pendingSeek = g.readerPos;
            StartEngine(g.s.video);
            return 0;
        case WM_VBG_QUIT: Exit(0); return 0;
        case WM_VBG_MEMORY: PostMessageW(g.host, WM_VBG_MEMORY, 0, (LPARAM)(GraphicsMemory() / 1024)); return 0;
        case WM_VBG_RELOAD: Reload(); return 0;
        case WM_VBG_PREVIEW_SIZE:
            g.simWanted = (UINT)l;
            if (g.parked) { Unpark(); ApplyPauseState(); }
            RequestRedraw();
            return 0;
        case WM_VBG_PREVIEW_CROP:
            g.previewing = w != 0;
            if (w) g.previewCrop = UnpackCrop(l);
            if (g.parked) { Unpark(); ApplyPauseState(); }  // a frame to show the new crop with
            UpdateRects();
            RequestRedraw();
            return 0;
        case WM_TIMER:
            if (w == TIMER_HEALTH) { HealthCheck(); CheckCovered(); }
            else if (w == TIMER_RECHECK) { KillTimer(h, TIMER_RECHECK); CheckCovered(); }
            else if (w == TIMER_PARK) { KillTimer(h, TIMER_PARK); Park(); }
            else if (w == TIMER_TRIM) {
                KillTimer(h, TIMER_TRIM);
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            } else if (w == TIMER_DISPLAY) {
                KillTimer(h, TIMER_DISPLAY);
                std::vector<RECT> now = WantedMonitors();
                bool same = now.size() == g.targets.size();
                for (size_t i = 0; same && i < now.size(); i++) same = EqualRect(&now[i], &g.targets[i].mon);
                if (!same) Exit(EXIT_RENDER_RESTART);
                else for (auto& t : g.targets) PlaceTarget(t, g.shown);
            }
            return 0;
        case WM_DISPLAYCHANGE: SetTimer(h, TIMER_DISPLAY, 1500, nullptr); return 0;
        case WM_WTSSESSION_CHANGE:
            if (w == WTS_SESSION_LOCK) SetPause(PR_LOCKED, true);
            else if (w == WTS_SESSION_UNLOCK) SetPause(PR_LOCKED, false);
            return 0;
        case WM_POWERBROADCAST:
            if (w == PBT_POWERSETTINGCHANGE) {
                auto* ps = (POWERBROADCAST_SETTING*)l;
                DWORD v = ps->DataLength >= sizeof(DWORD) ? *(DWORD*)ps->Data : 1;
                if (ps->PowerSetting == GUID_CONSOLE_DISPLAY_STATE) SetPause(PR_DISPLAY_OFF, v == 0);
                else if (ps->PowerSetting == GUID_ACDC_POWER_SOURCE) { g.onBattery = v != 0; ApplyBatteryPolicy(); }
            } else if (w == PBT_APMRESUMEAUTOMATIC) {
                RequestRedraw();
            }
            return TRUE;
    }
    if (m == g.taskbarCreated && m) { Exit(EXIT_RENDER_RESTART); return 0; }
    return DefWindowProcW(h, m, w, l);
}

void Shutdown() {
    g.shuttingDown = true;
    g.quit = true;
    if (g.wake) SetEvent(g.wake);
    if (g.thread) { WaitForSingleObject(g.thread, 3000); CloseHandle(g.thread); }
    // Hide first: the static wallpaper comes back the instant our windows go away.
    for (auto& t : g.targets) if (t.hwnd) ShowWindow(t.hwnd, SW_HIDE);
    StopMusic();
    if (g.eng) g.eng->Shutdown();
    SafeRelease(g.eng);
    CloseReader();
    for (auto& t : g.targets) {
        SafeRelease(t.rtv);
        SafeRelease(t.bb);
        SafeRelease(t.sc);
        if (t.hwnd) DestroyWindow(t.hwnd);
    }
    g.targets.clear();
    for (auto hk : g.hooks) if (hk) UnhookWinEvent(hk);
    if (g.powerDisplay) UnregisterPowerSettingNotification(g.powerDisplay);
    if (g.powerSource) UnregisterPowerSettingNotification(g.powerSource);
    if (g.ctl) { WTSUnRegisterSessionNotification(g.ctl); DestroyWindow(g.ctl); }
    SafeRelease(g.mgr);
    if (g.ctx) g.ctx->ClearState();
    SafeRelease(g.ctx);
    SafeRelease(g.dev);
    MF.Stop();
}

}  // namespace

int RendererMain(HINSTANCE inst, HWND host) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    InstallCrashLog(L"video renderer");
    g.inst = inst;
    g.host = host;
    GetWindowThreadProcessId(host, &g.hostPid);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LoadSettings(g.s);
    g.fpsCap = g.s.fpsCap;

    WNDCLASSW wc{};
    wc.lpfnWndProc = CtlProc;
    wc.hInstance = inst;
    wc.lpszClassName = RCTL_CLASS;
    RegisterClassW(&wc);
    // A hidden top-level window (not message-only) so it also receives broadcasts.
    g.ctl = CreateWindowExW(WS_EX_TOOLWINDOW, RCTL_CLASS, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    PostMessageW(host, WM_VBG_RENDERER_READY, 0, (LPARAM)g.ctl);

    if (g.s.video.empty()) {
        Fail(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), VF_MISSING);
    } else if (!MF.Start()) {
        Fail(E_NOINTERFACE, VF_NO_MEDIA);
    } else if (!D3D.Load()) {
        Fail(E_NOINTERFACE, VF_GPU);
    } else {
        // Explorer may still be starting (login, or right after an Explorer restart).
        bool found = false;
        for (int i = 0; i < 40 && !(found = FindDesktop()); i++) Sleep(500);
        Log(L"desktop: progman=%p parent=%p defview=%p raised=%d", g.progman, g.parent, g.defview, g.raised);
        if (!found) Exit(EXIT_RENDER_RESTART);
        else if (!CreateDevice()) Fail(E_FAIL, VF_GPU);
        else {
            g.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!g.timer) g.timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
            WTSRegisterSessionNotification(g.ctl, NOTIFY_FOR_THIS_SESSION);
            g.powerDisplay = RegisterPowerSettingNotification(g.ctl, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
            g.powerSource = RegisterPowerSettingNotification(g.ctl, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
            SYSTEM_POWER_STATUS ps{};
            if (GetSystemPowerStatus(&ps)) g.onBattery = ps.ACLineStatus == 0;
            ApplyBatteryPolicy();
            const DWORD hf = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
            g.hooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, hf);
            g.hooks[1] = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MINIMIZEEND, nullptr, WinEventProc, 0, 0, hf);
            g.hooks[2] = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, WinEventProc, 0, 0, hf);
            SetTimer(g.ctl, TIMER_HEALTH, 1000, nullptr);
            CheckCovered();
            if (StartPlayback()) ReportState();
            StartMusic();
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Shutdown();
    CoUninitialize();
    return (int)g.exitCode;
}
