// "Light copy": re-encodes a video at the size the screen actually needs (GPU decode, GPU scale,
// GPU H.264 encode via Media Foundation). A 4K video shown on a 1080p screen decodes 4x the
// pixels it can display; the copy cuts video-decoder load and memory accordingly.
// The original file is never modified. Also makes videos from animated GIFs (at the end).
#include "mfhelp.h"
#include <codecapi.h>
#include <mferror.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace {

struct Api {
    HMODULE rw = nullptr;
    decltype(&::MFCreateSinkWriterFromURL) CreateSinkWriterFromURL = nullptr;
};

HRESULT MakeDevice(ID3D11Device** dev, IMFDXGIDeviceManager** mgr) {
    if (!D3D.Load()) return E_NOINTERFACE;
    IDXGIFactory1* f = nullptr;
    HRESULT hr = D3D.CreateFactory1(__uuidof(IDXGIFactory1), (void**)&f);
    if (FAILED(hr)) return hr;
    IDXGIAdapter1* ad = PickDisplayAdapter(f, nullptr);
    hr = D3D.CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                          D3D11_SDK_VERSION, dev, nullptr, nullptr);
    SafeRelease(ad);
    f->Release();
    if (FAILED(hr)) return hr;
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED((*dev)->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    UINT token = 0;
    hr = MF.CreateDXGIDeviceManager(&token, mgr);
    if (SUCCEEDED(hr)) hr = (*mgr)->ResetDevice(*dev, token);
    return hr;
}

}  // namespace

// Size of a copy that still covers `screen` pixels for the visible (cropped) part of the video.
// Returns false when the video is already small enough.
bool LightCopySize(UINT vw, UINT vh, const Crop& c, int screenW, int screenH, UINT* ow, UINT* oh) {
    if (!vw || !vh) return false;
    double need = std::max(screenW / ((c.r - c.l) * vw), screenH / ((c.b - c.t) * vh));
    if (need > 0.8) return false;  // less than ~1.5x the pixels needed: not worth a copy
    *ow = ((UINT)(vw * need) + 1) & ~1u;
    *oh = ((UINT)(vh * need) + 1) & ~1u;
    return *ow >= 64 && *oh >= 64;
}

HRESULT MakeLightCopy(const std::wstring& src, const std::wstring& dst, UINT ow, UINT oh, HWND notify, UINT progressMsg,
                      volatile LONG* cancel) {
    Api api;
    api.rw = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    api.CreateSinkWriterFromURL = api.rw ? (decltype(api.CreateSinkWriterFromURL))(void*)GetProcAddress(api.rw, "MFCreateSinkWriterFromURL") : nullptr;
    if (!api.CreateSinkWriterFromURL) return E_NOINTERFACE;

    ID3D11Device* dev = nullptr;
    IMFDXGIDeviceManager* mgr = nullptr;
    IMFSourceReader* rd = nullptr;
    IMFSinkWriter* wr = nullptr;
    IMFAttributes* at = nullptr;
    IMFMediaType *vin = nullptr, *vout = nullptr, *ain = nullptr;
    DWORD vStream = 0, aStream = 0;
    bool hasAudio = false;
    VideoInfo vi;
    std::wstring part = dst + L".part";

    HRESULT hr = MakeDevice(&dev, &mgr);
    // Reader: hardware decode + hardware scaling to the target size.
    if (SUCCEEDED(hr)) hr = MF.CreateAttributes(&at, 3);
    if (SUCCEEDED(hr)) {
        at->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mgr);
        at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        at->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        hr = MF.CreateSourceReaderFromURL(src.c_str(), at, &rd);
        SafeRelease(at);
    }
    if (SUCCEEDED(hr)) hr = ProbeVideo(src, vi) ? S_OK : vi.hr;
    UINT32 fn = 30, fd = 1;
    if (SUCCEEDED(hr)) {
        IMFMediaType* native = nullptr;
        if (SUCCEEDED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) {
            MFGetAttributeRatio(native, MF_MT_FRAME_RATE, &fn, &fd);
            native->Release();
        }
        if (!fn || !fd) { fn = 30; fd = 1; }
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        hr = MF.CreateMediaType(&vin);
    }
    if (SUCCEEDED(hr)) {
        vin->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vin->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(vin, MF_MT_FRAME_SIZE, ow, oh);
        MFSetAttributeRatio(vin, MF_MT_FRAME_RATE, fn, fd);
        MFSetAttributeRatio(vin, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        vin->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, vin);
    }
    // Audio is copied as-is (no re-encode) so the copy still has sound if wanted.
    if (SUCCEEDED(hr) && !vi.audioStreams.empty()) {
        if (SUCCEEDED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &ain)) &&
            SUCCEEDED(rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE)) &&
            SUCCEEDED(rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, ain)))
            hasAudio = true;
        else
            rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE);
    }

    // Writer: hardware H.264 encoder.
    if (SUCCEEDED(hr)) hr = MF.CreateAttributes(&at, 4);
    if (SUCCEEDED(hr)) {
        at->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, mgr);
        at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        at->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        at->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        DeleteFileW(part.c_str());
        hr = api.CreateSinkWriterFromURL(part.c_str(), nullptr, at, &wr);
        SafeRelease(at);
    }
    if (SUCCEEDED(hr)) hr = MF.CreateMediaType(&vout);
    if (SUCCEEDED(hr)) {
        double fps = (double)fn / fd;
        UINT32 bitrate = (UINT32)std::clamp(ow * (double)oh * fps * 0.1, 2e6, 40e6);  // ~12 Mbit/s for 1080p60
        vout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vout->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        vout->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        vout->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        vout->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        MFSetAttributeSize(vout, MF_MT_FRAME_SIZE, ow, oh);
        MFSetAttributeRatio(vout, MF_MT_FRAME_RATE, fn, fd);
        MFSetAttributeRatio(vout, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = wr->AddStream(vout, &vStream);
    }
    if (SUCCEEDED(hr)) hr = wr->SetInputMediaType(vStream, vin, nullptr);
    if (SUCCEEDED(hr) && hasAudio && FAILED(wr->AddStream(ain, &aStream))) {
        hasAudio = false;  // container can't take this audio as-is: drop it rather than fail
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE);
    }
    if (SUCCEEDED(hr)) hr = wr->BeginWriting();
    Log(L"light copy %ls -> %ux%u @ %.2f fps, audio %d: start 0x%08lx", src.c_str(), ow, oh, (double)fn / fd, hasAudio,
        (unsigned long)hr);

    bool videoDone = false, audioDone = !hasAudio;
    int lastPct = -1;
    while (SUCCEEDED(hr) && !(videoDone && audioDone)) {
        if (cancel && *cancel) { hr = E_ABORT; break; }
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* s = nullptr;
        hr = rd->ReadSample((DWORD)MF_SOURCE_READER_ANY_STREAM, 0, &idx, &flags, &ts, &s);
        if (FAILED(hr)) break;
        // Map the reader's stream index to ours.
        IMFMediaType* cur = nullptr;
        bool isVideo = true;
        if (SUCCEEDED(rd->GetCurrentMediaType(idx, &cur))) {
            GUID major{};
            cur->GetGUID(MF_MT_MAJOR_TYPE, &major);
            isVideo = major == MFMediaType_Video;
            cur->Release();
        }
        DWORD out = isVideo ? vStream : aStream;
        if (flags & MF_SOURCE_READERF_STREAMTICK) wr->SendStreamTick(out, ts);
        if (s) {
            hr = wr->WriteSample(out, s);
            s->Release();
            if (isVideo && vi.duration > 0) {
                int pct = std::clamp((int)(ts / 1e7 / vi.duration * 100), 0, 99);
                if (pct != lastPct) { lastPct = pct; PostMessageW(notify, progressMsg, (WPARAM)pct, 0); }
            }
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (isVideo) videoDone = true;
            else audioDone = true;
        }
    }
    if (SUCCEEDED(hr)) hr = wr->Finalize();
    SafeRelease(wr);
    SafeRelease(rd);
    SafeRelease(vin);
    SafeRelease(vout);
    SafeRelease(ain);
    SafeRelease(mgr);
    SafeRelease(dev);
    D3D.Unload();
    FreeLibrary(api.rw);
    if (SUCCEEDED(hr) && !MoveFileExW(part.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) hr = HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr)) DeleteFileW(part.c_str());
    Log(L"light copy finished 0x%08lx", (unsigned long)hr);
    return hr;
}

// ---------------------------------------------------------------------------------------
// Animated GIF -> MP4 (H.264). The frames are put together the way browsers show them (frame
// offsets, transparency, disposal) at a steady frame rate that keeps every frame's timing. Small
// GIFs are scaled up to 720 lines: their sharp colour edges survive the video's half-size colour,
// and every player then reads the colours as HD (BT.709), the way they were written.

namespace {

struct GifFrame {
    UINT left = 0, top = 0, w = 0, h = 0;
    UINT delay = 10;    // 1/100 s
    UINT disposal = 0;  // 2: back to the background afterwards, 3: back to what was there before
};

UINT MetaUInt(IWICMetadataQueryReader* q, const wchar_t* name, UINT def) {
    PROPVARIANT v;
    PropVariantInit(&v);
    UINT r = def;
    if (q && SUCCEEDED(q->GetMetadataByName(name, &v))) {
        if (v.vt == VT_UI1) r = v.bVal;
        else if (v.vt == VT_UI2) r = v.uiVal;
        else if (v.vt == VT_UI4) r = v.ulVal;
    }
    PropVariantClear(&v);
    return r;
}

// BGRX -> NV12, limited range. BT.709 for HD sizes and BT.601 below, as players assume when the
// stream doesn't say. w and h are even.
void ToNV12(const uint32_t* px, UINT w, UINT h, bool bt709, BYTE* out) {
    const int yr = bt709 ? 47 : 66, yg = bt709 ? 157 : 129, yb = bt709 ? 16 : 25;
    const int ur = bt709 ? -26 : -38, ug = bt709 ? -87 : -74, ub = 112;
    const int vr = 112, vg = bt709 ? -102 : -94, vb = bt709 ? -10 : -18;
    BYTE* uv = out + (size_t)w * h;
    for (UINT y = 0; y < h; y++) {
        const uint32_t* row = px + (size_t)y * w;
        BYTE* dst = out + (size_t)y * w;
        for (UINT x = 0; x < w; x++) {
            int r = (row[x] >> 16) & 255, g = (row[x] >> 8) & 255, b = row[x] & 255;
            dst[x] = (BYTE)(((yr * r + yg * g + yb * b + 128) >> 8) + 16);
        }
    }
    for (UINT y = 0; y < h; y += 2) {
        const uint32_t* r0 = px + (size_t)y * w;
        const uint32_t* r1 = r0 + w;
        BYTE* dst = uv + (size_t)(y / 2) * w;
        for (UINT x = 0; x < w; x += 2) {
            int r = 0, g = 0, b = 0;
            for (uint32_t c : {r0[x], r0[x + 1], r1[x], r1[x + 1]}) {
                r += (c >> 16) & 255;
                g += (c >> 8) & 255;
                b += c & 255;
            }
            r = (r + 2) >> 2;
            g = (g + 2) >> 2;
            b = (b + 2) >> 2;
            dst[x] = (BYTE)std::clamp(((ur * r + ug * g + ub * b + 128) >> 8) + 128, 16, 240);
            dst[x + 1] = (BYTE)std::clamp(((vr * r + vg * g + vb * b + 128) >> 8) + 128, 16, 240);
        }
    }
}

}  // namespace

HRESULT MakeVideoFromGif(const std::wstring& gif, const std::wstring& dst, HWND notify, UINT progressMsg, volatile LONG* cancel) {
    Api api;
    api.rw = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    api.CreateSinkWriterFromURL = api.rw ? (decltype(api.CreateSinkWriterFromURL))(void*)GetProcAddress(api.rw, "MFCreateSinkWriterFromURL") : nullptr;
    if (!api.CreateSinkWriterFromURL || !MF.CreateSample || !MF.CreateMemoryBuffer) {
        if (api.rw) FreeLibrary(api.rw);
        return E_NOINTERFACE;
    }

    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IMFSinkWriter* wr = nullptr;
    IMFAttributes* at = nullptr;
    IMFMediaType *vin = nullptr, *vout = nullptr;
    DWORD stream = 0;
    std::wstring part = dst + L".part";
    UINT count = 0, cw = 0, ch = 0;
    std::vector<GifFrame> fr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(gif.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrameCount(&count);
    if (SUCCEEDED(hr) && !count) hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    // Where each frame goes and how long it shows.
    if (SUCCEEDED(hr)) {
        IWICMetadataQueryReader* q = nullptr;
        if (SUCCEEDED(dec->GetMetadataQueryReader(&q))) {
            cw = MetaUInt(q, L"/logscrdesc/Width", 0);
            ch = MetaUInt(q, L"/logscrdesc/Height", 0);
            q->Release();
        }
        fr.resize(count);
        for (UINT i = 0; i < count && SUCCEEDED(hr); i++) {
            IWICBitmapFrameDecode* f = nullptr;
            hr = dec->GetFrame(i, &f);
            if (FAILED(hr)) break;
            f->GetSize(&fr[i].w, &fr[i].h);
            if (SUCCEEDED(f->GetMetadataQueryReader(&q))) {
                fr[i].left = MetaUInt(q, L"/imgdesc/Left", 0);
                fr[i].top = MetaUInt(q, L"/imgdesc/Top", 0);
                fr[i].delay = MetaUInt(q, L"/grctlext/Delay", 0);
                fr[i].disposal = MetaUInt(q, L"/grctlext/Disposal", 0);
                q->Release();
            }
            if (fr[i].delay < 2) fr[i].delay = 10;  // "as fast as possible": browsers show these for 0.1 s
            f->Release();
        }
        if (!cw || !ch) { cw = fr.empty() ? 0 : fr[0].w; ch = fr.empty() ? 0 : fr[0].h; }
        if (SUCCEEDED(hr) && (!cw || !ch || cw > 16384 || ch > 16384)) hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    // A frame rate every delay is a whole number of frames of (at most 50 fps); a still GIF
    // becomes 2 seconds at 1 fps.
    UINT step = 0;
    UINT64 total = 0;
    if (count == 1) fr[0].delay = 200;
    for (const GifFrame& f : fr) {
        step = std::gcd(step, f.delay);
        total += f.delay;
    }
    step = std::clamp(step, 2u, 100u);
    UINT64 frames = (total + step - 1) / std::max(step, 1u);

    // Output size: small GIFs scaled up to 720 lines, big ones down to 4K.
    double sc = ch && ch < 720 ? 720.0 / ch : 1.0;
    if (cw && ch) sc = std::min({sc, 3840.0 / cw, 2160.0 / ch});
    UINT ow = std::max(2u, (UINT)(cw * sc + 0.5) & ~1u), oh = std::max(2u, (UINT)(ch * sc + 0.5) & ~1u);
    const bool bt709 = oh >= 720;
    const UINT32 matrix = bt709 ? MFVideoTransferMatrix_BT709 : MFVideoTransferMatrix_BT601;

    // Writer: H.264 in MP4. Software encoding: GIFs are small, and it behaves the same on every PC.
    if (SUCCEEDED(hr)) hr = MF.CreateAttributes(&at, 2);
    if (SUCCEEDED(hr)) {
        at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
        at->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        DeleteFileW(part.c_str());
        hr = api.CreateSinkWriterFromURL(part.c_str(), nullptr, at, &wr);
        SafeRelease(at);
    }
    if (SUCCEEDED(hr)) hr = MF.CreateMediaType(&vout);
    if (SUCCEEDED(hr)) {
        double fps = 100.0 / step;
        vout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vout->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        vout->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)std::clamp(ow * (double)oh * fps * 0.15, 2e6, 24e6));
        vout->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        vout->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        vout->SetUINT32(MF_MT_YUV_MATRIX, matrix);
        vout->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        MFSetAttributeSize(vout, MF_MT_FRAME_SIZE, ow, oh);
        MFSetAttributeRatio(vout, MF_MT_FRAME_RATE, 100, step);
        MFSetAttributeRatio(vout, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = wr->AddStream(vout, &stream);
    }
    if (SUCCEEDED(hr)) hr = MF.CreateMediaType(&vin);
    if (SUCCEEDED(hr)) {
        vin->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vin->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        vin->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        vin->SetUINT32(MF_MT_DEFAULT_STRIDE, ow);
        vin->SetUINT32(MF_MT_YUV_MATRIX, matrix);
        vin->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        MFSetAttributeSize(vin, MF_MT_FRAME_SIZE, ow, oh);
        MFSetAttributeRatio(vin, MF_MT_FRAME_RATE, 100, step);
        MFSetAttributeRatio(vin, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        // Constant quality (bitrate follows the content: a GIF has long still stretches and sharp
        // edges) and no B-frames, so the video starts at 0 like the GIF.
        static const GUID kRateControl = {STATIC_CODECAPI_AVEncCommonRateControlMode};
        static const GUID kQuality = {STATIC_CODECAPI_AVEncCommonQuality};
        static const GUID kBFrames = {STATIC_CODECAPI_AVEncMPVDefaultBPictureCount};
        IMFAttributes* enc = nullptr;
        if (SUCCEEDED(MF.CreateAttributes(&enc, 3))) {
            enc->SetUINT32(kRateControl, eAVEncCommonRateControlMode_Quality);
            enc->SetUINT32(kQuality, 90);
            enc->SetUINT32(kBFrames, 0);
        }
        hr = wr->SetInputMediaType(stream, vin, enc);
        SafeRelease(enc);
    }
    if (SUCCEEDED(hr)) hr = wr->BeginWriting();
    Log(L"video from GIF %ls: %u frames %ux%u -> %ux%u @ %.1f fps, %llu frames: start 0x%08lx", gif.c_str(), count, cw, ch,
        ow, oh, 100.0 / step, (unsigned long long)frames, (unsigned long)hr);

    // Compose each GIF frame onto the canvas, then write it for as many video frames as it lasts.
    std::vector<uint32_t> canvas, saved, px, scaled;
    std::vector<BYTE> nv12((size_t)ow * oh * 3 / 2);
    if (SUCCEEDED(hr)) canvas.assign((size_t)cw * ch, 0xFF000000u);
    UINT64 shown = 0, end = 0;
    int lastPct = -1;
    for (UINT i = 0; i < count && SUCCEEDED(hr); i++) {
        if (i > 0) {  // what the previous frame leaves behind
            const GifFrame& p = fr[i - 1];
            if (p.disposal == 2) {
                for (UINT y = p.top; y < std::min(ch, p.top + p.h); y++)
                    for (UINT x = p.left; x < std::min(cw, p.left + p.w); x++) canvas[(size_t)y * cw + x] = 0xFF000000u;
            } else if (p.disposal == 3 && !saved.empty()) {
                canvas = saved;
            }
        }
        if (fr[i].disposal == 3) saved = canvas;

        IWICBitmapFrameDecode* f = nullptr;
        IWICFormatConverter* cv = nullptr;
        hr = dec->GetFrame(i, &f);
        if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&cv);
        if (SUCCEEDED(hr))
            hr = cv->Initialize(f, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
        const UINT fw = fr[i].w, fh = fr[i].h;
        if (SUCCEEDED(hr)) {
            px.resize((size_t)fw * fh);
            hr = cv->CopyPixels(nullptr, fw * 4, (UINT)(px.size() * 4), (BYTE*)px.data());
        }
        SafeRelease(cv);
        SafeRelease(f);
        if (FAILED(hr)) break;
        for (UINT y = 0; y < fh && fr[i].top + y < ch; y++)
            for (UINT x = 0; x < fw && fr[i].left + x < cw; x++) {
                uint32_t c = px[(size_t)y * fw + x];
                if (c >> 24) canvas[(size_t)(fr[i].top + y) * cw + fr[i].left + x] = c | 0xFF000000u;  // transparent: keep what's there
            }

        const uint32_t* frame = canvas.data();
        if (ow != cw || oh != ch) {
            IWICBitmap* bm = nullptr;
            IWICBitmapScaler* sz = nullptr;
            hr = wic->CreateBitmapFromMemory(cw, ch, GUID_WICPixelFormat32bppBGRA, cw * 4, (UINT)(canvas.size() * 4),
                                             (BYTE*)canvas.data(), &bm);
            if (SUCCEEDED(hr)) hr = wic->CreateBitmapScaler(&sz);
            if (SUCCEEDED(hr) && FAILED(sz->Initialize(bm, ow, oh, (WICBitmapInterpolationMode)4)))  // high-quality cubic
                hr = sz->Initialize(bm, ow, oh, WICBitmapInterpolationModeCubic);
            if (SUCCEEDED(hr)) {
                scaled.resize((size_t)ow * oh);
                hr = sz->CopyPixels(nullptr, ow * 4, (UINT)(scaled.size() * 4), (BYTE*)scaled.data());
            }
            SafeRelease(sz);
            SafeRelease(bm);
            frame = scaled.data();
        }
        if (FAILED(hr)) break;
        ToNV12(frame, ow, oh, bt709, nv12.data());

        end += fr[i].delay;
        for (; shown * step < end && SUCCEEDED(hr); shown++) {
            if (cancel && *cancel) { hr = E_ABORT; break; }
            IMFSample* s = nullptr;
            IMFMediaBuffer* b = nullptr;
            BYTE* dstp = nullptr;
            hr = MF.CreateMemoryBuffer((DWORD)nv12.size(), &b);
            if (SUCCEEDED(hr)) hr = b->Lock(&dstp, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                memcpy(dstp, nv12.data(), nv12.size());
                b->Unlock();
                b->SetCurrentLength((DWORD)nv12.size());
                hr = MF.CreateSample(&s);
            }
            if (SUCCEEDED(hr)) hr = s->AddBuffer(b);
            if (SUCCEEDED(hr)) {
                s->SetSampleTime((LONGLONG)shown * step * 100000);  // 1/100 s in 100 ns units
                s->SetSampleDuration((LONGLONG)step * 100000);
                hr = wr->WriteSample(stream, s);  // blocks while the encoder catches up
            }
            SafeRelease(s);
            SafeRelease(b);
            int pct = frames ? (int)std::min<UINT64>(99, shown * 100 / frames) : 0;
            if (pct != lastPct) { lastPct = pct; PostMessageW(notify, progressMsg, (WPARAM)pct, 0); }
        }
    }
    if (SUCCEEDED(hr)) hr = wr->Finalize();
    SafeRelease(wr);
    SafeRelease(vin);
    SafeRelease(vout);
    SafeRelease(dec);
    SafeRelease(wic);
    FreeLibrary(api.rw);
    if (SUCCEEDED(hr) && !MoveFileExW(part.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) hr = HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr)) DeleteFileW(part.c_str());
    Log(L"video from GIF finished 0x%08lx", (unsigned long)hr);
    return hr;
}
