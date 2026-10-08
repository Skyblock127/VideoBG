#include "mfhelp.h"
#include <propvarutil.h>
#include <mferror.h>
#include <cmath>

MFApi MF;
D3DApi D3D;

template <class F> static void Bind(F& fn, HMODULE m, const char* name) {
    fn = m ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

// The settings window and the lock screen worker (another thread) can both use Media Foundation.
static SRWLOCK g_mfLock = SRWLOCK_INIT;

bool MFApi::Start() {
    AcquireSRWLockExclusive(&g_mfLock);
    bool ok = StartLocked();
    ReleaseSRWLockExclusive(&g_mfLock);
    return ok;
}

bool MFApi::StartLocked() {
    if (starts > 0) { starts++; return true; }
    plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    rw = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Bind(Startup, plat, "MFStartup");
    Bind(Shutdown, plat, "MFShutdown");
    Bind(CreateAttributes, plat, "MFCreateAttributes");
    Bind(CreateMediaType, plat, "MFCreateMediaType");
    Bind(CreateDXGIDeviceManager, plat, "MFCreateDXGIDeviceManager");
    Bind(CreateSourceReaderFromURL, rw, "MFCreateSourceReaderFromURL");
    Bind(CreateSample, plat, "MFCreateSample");
    Bind(CreateMemoryBuffer, plat, "MFCreateMemoryBuffer");
    Bind(TEnumEx, plat, "MFTEnumEx");
    if (!Startup || !Shutdown || !CreateAttributes || !CreateMediaType || !CreateSourceReaderFromURL ||
        FAILED(Startup(MF_VERSION, MFSTARTUP_LITE))) {
        if (rw) FreeLibrary(rw);
        if (plat) FreeLibrary(plat);
        plat = rw = nullptr;
        return false;
    }
    starts = 1;
    return true;
}

void MFApi::Stop() {
    AcquireSRWLockExclusive(&g_mfLock);
    if (starts > 0 && --starts == 0) {
        Shutdown();
        FreeLibrary(rw);
        FreeLibrary(plat);
        plat = rw = nullptr;
    }
    ReleaseSRWLockExclusive(&g_mfLock);
}

bool D3DApi::Load() {
    if (d3d) return true;
    d3d = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Bind(CreateDevice, d3d, "D3D11CreateDevice");
    Bind(CreateFactory1, dxgi, "CreateDXGIFactory1");
    if (!CreateDevice || !CreateFactory1) { Unload(); return false; }
    return true;
}

void D3DApi::Unload() {
    if (d3d) FreeLibrary(d3d);
    if (dxgi) FreeLibrary(dxgi);
    d3d = dxgi = nullptr;
    CreateDevice = nullptr;
    CreateFactory1 = nullptr;
}

static void ReadVideoType(IMFMediaType* mt, VideoInfo& vi) {
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h);
    MFVideoArea area{};
    if (SUCCEEDED(mt->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof area, nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0) {
        w = (UINT32)area.Area.cx;
        h = (UINT32)area.Area.cy;
    }
    UINT32 pn = 1, pd = 1;
    if (FAILED(MFGetAttributeRatio(mt, MF_MT_PIXEL_ASPECT_RATIO, &pn, &pd)) || !pn || !pd) pn = pd = 1;
    UINT32 fn = 0, fd = 0;
    MFGetAttributeRatio(mt, MF_MT_FRAME_RATE, &fn, &fd);
    GUID sub{};
    mt->GetGUID(MF_MT_SUBTYPE, &sub);
    vi.w = w;
    vi.h = h;
    vi.aspect = h ? (double)w * pn / ((double)h * pd) : 0;
    vi.fps = fd ? (double)fn / fd : 0;
    vi.subtype = sub;
    vi.codec = CodecLabel(sub.Data1);
}

static void ReadDuration(IMFSourceReader* rd, VideoInfo& vi) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) {
        vi.duration = pv.uhVal.QuadPart / 1e7;
    }
    PropVariantClear(&pv);
}

bool ProbeVideo(const std::wstring& path, VideoInfo& vi) {
    vi = VideoInfo{};
    IMFSourceReader* rd = nullptr;
    vi.hr = MF.CreateSourceReaderFromURL(path.c_str(), nullptr, &rd);
    if (FAILED(vi.hr)) return false;
    vi.opened = true;
    bool gotVideo = false;
    for (DWORD i = 0;; i++) {
        IMFMediaType* mt = nullptr;
        HRESULT hr = rd->GetNativeMediaType(i, 0, &mt);
        if (FAILED(hr)) break;  // MF_E_INVALIDSTREAMNUMBER = no more streams
        GUID major{};
        mt->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (major == MFMediaType_Audio) vi.audioStreams.push_back(i);
        else if (major == MFMediaType_Video && !gotVideo) { ReadVideoType(mt, vi); gotVideo = true; }
        mt->Release();
    }
    ReadDuration(rd, vi);
    rd->Release();
    vi.ok = gotVideo && vi.w > 0 && vi.h > 0;
    if (!gotVideo) vi.hr = MF_E_INVALIDMEDIATYPE;
    return vi.ok;
}

// Whether any decoder on this PC (built in, a Store extension or the GPU's) takes this codec.
static bool HasDecoder(const GUID& sub) {
    // Uncompressed video needs no decoder.
    for (const GUID& raw : {MFVideoFormat_RGB32, MFVideoFormat_ARGB32, MFVideoFormat_RGB24, MFVideoFormat_NV12,
                            MFVideoFormat_YUY2, MFVideoFormat_UYVY, MFVideoFormat_I420, MFVideoFormat_IYUV,
                            MFVideoFormat_YV12, MFVideoFormat_P010})
        if (sub == raw) return true;
    if (!MF.TEnumEx) return true;  // can't tell
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, sub};
    IMFActivate** found = nullptr;
    UINT32 n = 0;
    const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_LOCALMFT |
                         MFT_ENUM_FLAG_SORTANDFILTER;
    if (FAILED(MF.TEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &in, nullptr, &found, &n))) return true;
    for (UINT32 i = 0; i < n; i++) found[i]->Release();
    CoTaskMemFree(found);
    return n > 0;
}

VideoFault DiagnoseVideo(const std::wstring& path, const VideoInfo& vi) {
    VideoFault f = SniffFile(path, nullptr);
    if (f == VF_MISSING || f == VF_UNREADABLE || f == VF_EMPTY) return f;
    if (!MF.starts) return VF_NO_MEDIA;
    if (!vi.opened) {
        if (vi.hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || vi.hr == E_ACCESSDENIED) return VF_UNREADABLE;
        bool unknown = vi.hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE;  // no part of Windows takes this kind of file
        if (f == VF_CONTAINER) return unknown ? f : VF_OTHER;      // else Windows takes the kind, but not this file
        if (f != VF_NONE) return f;                                // a GIF, a picture, a zip...
        return unknown ? VF_NOT_VIDEO : VF_OTHER;
    }
    if (vi.subtype == GUID_NULL) return vi.audioStreams.empty() ? VF_NOT_VIDEO : VF_SOUND_ONLY;
    return HasDecoder(vi.subtype) ? VF_OTHER : VF_NO_DECODER;
}

bool FrameGrabber::Open(const std::wstring& path) {
    Close();
    path_ = path;
    info = VideoInfo{};
    IMFAttributes* at = nullptr;
    if (!MF.starts || FAILED(MF.CreateAttributes(&at, 1))) { info.hr = E_NOINTERFACE; return false; }
    at->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    info.hr = MF.CreateSourceReaderFromURL(path.c_str(), at, &rd_);
    at->Release();
    if (FAILED(info.hr)) return false;
    info.opened = true;

    // Stream info (native types) before we change the output format.
    for (DWORD i = 0;; i++) {
        IMFMediaType* mt = nullptr;
        if (FAILED(rd_->GetNativeMediaType(i, 0, &mt))) break;
        GUID major{};
        mt->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (major == MFMediaType_Audio) info.audioStreams.push_back(i);
        else if (major == MFMediaType_Video && info.w == 0) ReadVideoType(mt, info);
        mt->Release();
    }
    ReadDuration(rd_, info);

    rd_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd_->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    IMFMediaType* out = nullptr;
    MF.CreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    info.hr = rd_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out);
    out->Release();
    if (FAILED(info.hr) || !ReadFormat()) { Close(); return false; }
    info.ok = true;
    return true;
}

bool FrameGrabber::ReadFormat() {
    IMFMediaType* mt = nullptr;
    if (FAILED(rd_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mt))) return false;
    MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &fw_, &fh_);
    UINT32 st = 0;
    stride_ = SUCCEEDED(mt->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? (LONG)(INT32)st : (LONG)fw_ * 4;
    ap_ = {0, 0, (LONG)fw_, (LONG)fh_};
    MFVideoArea area{};
    if (SUCCEEDED(mt->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof area, nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0) {
        ap_.left = area.OffsetX.value;
        ap_.top = area.OffsetY.value;
        ap_.right = ap_.left + area.Area.cx;
        ap_.bottom = ap_.top + area.Area.cy;
        if (ap_.right > (LONG)fw_ || ap_.bottom > (LONG)fh_) ap_ = {0, 0, (LONG)fw_, (LONG)fh_};
    }
    mt->Release();
    return fw_ > 0 && fh_ > 0;
}

bool FrameGrabber::Grab(double seconds, std::vector<uint32_t>& px, int& w, int& h) {
    if (!rd_) return false;
    if (info.duration > 0 && seconds > info.duration - 0.05) seconds = info.duration - 0.05;
    if (seconds < 0) seconds = 0;
    LONGLONG target = (LONGLONG)(seconds * 1e7);
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_I8;
    pv.hVal.QuadPart = target;
    rd_->SetCurrentPosition(GUID_NULL, pv);

    // Seeking lands on the previous key frame; decode forward to the requested time.
    LONGLONG half = info.fps > 0 ? (LONGLONG)(0.5e7 / info.fps) : 166666;
    IMFSample* best = nullptr;
    for (int i = 0; i < 600; i++) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* s = nullptr;
        if (FAILED(rd_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s))) break;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) ReadFormat();
        if (s) {
            SafeRelease(best);
            best = s;
            if (ts + half >= target) break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (!best) return false;

    bool ok = false;
    IMFMediaBuffer* buf = nullptr;
    if (SUCCEEDED(best->ConvertToContiguousBuffer(&buf))) {
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        IMF2DBuffer* b2 = nullptr;
        BYTE* raw = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(buf->QueryInterface(__uuidof(IMF2DBuffer), (void**)&b2)) && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
        } else if (SUCCEEDED(buf->Lock(&raw, nullptr, &len))) {
            pitch = stride_;
            scan0 = pitch < 0 ? raw + (size_t)(-pitch) * (fh_ - 1) : raw;
        }
        if (scan0) {
            w = ap_.right - ap_.left;
            h = ap_.bottom - ap_.top;
            px.resize((size_t)w * h);
            for (int y = 0; y < h; y++) {
                const uint32_t* src = (const uint32_t*)(scan0 + (ptrdiff_t)pitch * (y + ap_.top)) + ap_.left;
                uint32_t* dst = &px[(size_t)y * w];
                for (int x = 0; x < w; x++) dst[x] = src[x] | 0xFF000000u;
            }
            ok = true;
        }
        if (b2) { if (scan0 && !raw) b2->Unlock2D(); b2->Release(); }
        if (raw) buf->Unlock();
        buf->Release();
    }
    best->Release();
    return ok;
}

void FrameGrabber::Close() {
    SafeRelease(rd_);
    path_.clear();
    fw_ = fh_ = 0;
}

void ShrinkPixels(std::vector<uint32_t>& px, int& w, int& h, int maxDim) {
    while ((w > maxDim || h > maxDim) && w >= 2 && h >= 2) {
        int nw = w / 2, nh = h / 2;
        std::vector<uint32_t> out((size_t)nw * nh);
        for (int y = 0; y < nh; y++) {
            const uint32_t* r0 = &px[(size_t)(2 * y) * w];
            const uint32_t* r1 = r0 + w;
            for (int x = 0; x < nw; x++) {
                uint32_t a = r0[2 * x], b = r0[2 * x + 1], c = r1[2 * x], d = r1[2 * x + 1];
                uint32_t rb = ((a & 0xFF00FF) + (b & 0xFF00FF) + (c & 0xFF00FF) + (d & 0xFF00FF) + 0x020002) >> 2;
                uint32_t g = ((a & 0xFF00) + (b & 0xFF00) + (c & 0xFF00) + (d & 0xFF00) + 0x200) >> 2;
                out[(size_t)y * nw + x] = 0xFF000000u | (rb & 0xFF00FF) | (g & 0xFF00);
            }
        }
        px.swap(out);
        w = nw;
        h = nh;
    }
}

IDXGIAdapter1* PickDisplayAdapter(IDXGIFactory1* f, std::wstring* name) {
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    IDXGIAdapter1* pick = nullptr;
    IDXGIAdapter1* first = nullptr;
    for (UINT i = 0;; i++) {
        IDXGIAdapter1* a = nullptr;
        if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { a->Release(); continue; }
        bool owns = false;
        for (UINT o = 0;; o++) {
            IDXGIOutput* out = nullptr;
            if (a->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od{};
            out->GetDesc(&od);
            out->Release();
            if (od.Monitor == primary) { owns = true; break; }
        }
        if (owns && !pick) { pick = a; a = nullptr; }
        else if (!first) { first = a; a = nullptr; }
        SafeRelease(a);
    }
    if (!pick) { pick = first; first = nullptr; }
    SafeRelease(first);
    if (pick && name) {
        DXGI_ADAPTER_DESC1 d{};
        pick->GetDesc1(&d);
        *name = d.Description;
    }
    return pick;
}
