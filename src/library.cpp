// The modern look's wallpapers: the list of videos to pick from, and a picture of each.
// The pictures (each video's preview frame, a small one for its card and a screen-sized one for the
// picker's backdrop) are made by a worker thread and kept on disk, keyed by the video's content and
// frame time, so the picker shows them at once the next time it opens.
#include "mfhelp.h"
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <deque>

namespace {

std::wstring ListPath() { return AppDataDir() + L"\\wallpapers.m3u8"; }

std::wstring CacheDir() {
    std::wstring d = LocalDataDir() + L"\\Previews";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

bool SamePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// ---------------------------------------------------------------------------------------
// JPEG files through WIC (32-bit BGRX pixels in and out)

bool SaveJpeg(IWICImagingFactory* wic, const std::wstring& path, const std::vector<uint32_t>& px, int w, int h) {
    IWICBitmap* bm = nullptr;
    IWICFormatConverter* conv = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    IPropertyBag2* props = nullptr;
    const std::wstring tmp = path + L".tmp";
    bool ok = SUCCEEDED(wic->CreateBitmapFromMemory((UINT)w, (UINT)h, GUID_WICPixelFormat32bppBGR, (UINT)w * 4, (UINT)px.size() * 4,
                                                    (BYTE*)px.data(), &bm)) &&
              SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(bm, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, &props));
    if (ok && props) {
        PROPBAG2 opt{};
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.9f;
        props->Write(1, &opt, &v);
    }
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    ok = ok && SUCCEEDED(fr->Initialize(props)) && SUCCEEDED(fr->SetSize((UINT)w, (UINT)h)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
         SUCCEEDED(fr->WriteSource(conv, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    SafeRelease(props);
    SafeRelease(fr);
    SafeRelease(enc);
    SafeRelease(st);
    SafeRelease(conv);
    SafeRelease(bm);
    ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool LoadJpeg(IWICImagingFactory* wic, const std::wstring& path, std::vector<uint32_t>& px, int& w, int& h) {
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT uw = 0, uh = 0;
    bool ok = SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&uw, &uh)) && uw && uh && uw <= 8192 && uh <= 8192 &&
              SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    if (ok) {
        px.resize((size_t)uw * uh);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, (UINT)px.size() * 4, (BYTE*)px.data()));
        w = (int)uw;
        h = (int)uh;
        for (auto& p : px) p |= 0xFF000000u;
    }
    SafeRelease(conv);
    SafeRelease(fr);
    SafeRelease(dec);
    return ok;
}

// A smaller copy (high-quality WIC scaling) that fits in maxW x maxH.
void Shrink(IWICImagingFactory* wic, const std::vector<uint32_t>& px, int w, int h, int maxW, int maxH, std::vector<uint32_t>& out,
            int& ow, int& oh) {
    const double k = std::min({1.0, (double)maxW / w, (double)maxH / h});
    ow = std::max(1, (int)lround(w * k));
    oh = std::max(1, (int)lround(h * k));
    if (k >= 1.0) {
        out = px;
        ow = w;
        oh = h;
        return;
    }
    IWICBitmap* bm = nullptr;
    IWICBitmapScaler* sc = nullptr;
    out.assign((size_t)ow * oh, 0);
    if (SUCCEEDED(wic->CreateBitmapFromMemory((UINT)w, (UINT)h, GUID_WICPixelFormat32bppBGR, (UINT)w * 4, (UINT)px.size() * 4,
                                              (BYTE*)px.data(), &bm)) &&
        SUCCEEDED(wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(bm, (UINT)ow, (UINT)oh, WICBitmapInterpolationModeFant)))
        sc->CopyPixels(nullptr, (UINT)ow * 4, (UINT)out.size() * 4, (BYTE*)out.data());
    for (auto& p : out) p |= 0xFF000000u;
    SafeRelease(sc);
    SafeRelease(bm);
}

// The picture's main colours: pixels grouped into 4-bit-per-channel boxes, the fullest boxes'
// averages, each one clearly different from those before it.
void Palette(const std::vector<uint32_t>& px, UINT32 out[5]) {
    struct Box {
        UINT32 n = 0;
        UINT64 r = 0, g = 0, b = 0;
    };
    std::vector<Box> boxes(4096);
    for (size_t i = 0; i < px.size(); i += 3) {
        const UINT32 p = px[i];
        const UINT32 r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        Box& bx = boxes[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
        bx.n++;
        bx.r += r;
        bx.g += g;
        bx.b += b;
    }
    std::vector<int> order(boxes.size());
    for (int i = 0; i < (int)order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return boxes[a].n > boxes[b].n; });
    int found = 0;
    for (int i : order) {
        if (found == 5 || !boxes[i].n) break;
        const Box& bx = boxes[i];
        const int r = (int)(bx.r / bx.n), g = (int)(bx.g / bx.n), b = (int)(bx.b / bx.n);
        bool close = false;
        for (int k = 0; k < found && !close; k++) {
            const int dr = r - (int)(out[k] >> 16 & 255), dg = g - (int)(out[k] >> 8 & 255), db = b - (int)(out[k] & 255);
            close = dr * dr + dg * dg + db * db < 48 * 48;
        }
        if (!close) out[found++] = (UINT32)(r << 16 | g << 8 | b);
    }
    for (int k = found; k < 5; k++) out[k] = found ? out[k % found] : 0x303030;
}

// ---------------------------------------------------------------------------------------
// Worker

struct Want {
    std::wstring path;
    bool frame = false;
};

// One per picker session. Freed by whichever lets go last: the picker closing, or the thread
// finishing a slow video after the picker has gone.
struct Worker {
    LONG refs = 2;
    HANDLE thread = nullptr, wake = nullptr;
    CRITICAL_SECTION cs;
    std::deque<Want> queue;
    bool quit = false;
    HWND notify = nullptr;
    UINT msg = 0;
    int screenW = 1920, screenH = 1080;
    void Release() {
        if (InterlockedDecrement(&refs) != 0) return;
        CloseHandle(wake);
        DeleteCriticalSection(&cs);
        delete this;
    }
};
Worker* g_worker;

void Make(IWICImagingFactory* wic, FrameGrabber& grab, const Want& want, int screenW, int screenH, WallpaperInfo* info) {
    info->path = want.path;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(want.path.c_str(), GetFileExInfoStandard, &fa)) {
        info->fault = VF_MISSING;
        return;
    }
    info->bytes = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    Settings profile;  // the video's own preview frame
    profile.video = want.path;
    LoadVideoProfile(profile);
    const std::wstring pid = VideoProfileId(want.path);
    if (pid.empty()) {
        info->fault = VF_UNREADABLE;
        return;
    }
    wchar_t t[32];
    if (profile.previewTime >= 0) swprintf(t, 32, L"_%lld", (long long)llround(profile.previewTime * 1000));
    else wcscpy(t, L"_auto");
    const std::wstring base = CacheDir() + L"\\" + pid + t;

    // From the cache: the facts, the card's picture, and the frame when it's wanted.
    {
        std::vector<std::wstring> facts;
        if (ReadFileList(base + L".txt", &facts) && facts.size() >= 7) {
            info->vw = (UINT32)_wtoi(facts[0].c_str());
            info->vh = (UINT32)_wtoi(facts[1].c_str());
            info->duration = wcstod(facts[2].c_str(), nullptr);
            info->fps = wcstod(facts[3].c_str(), nullptr);
            info->sound = facts[4] == L"1";
            info->codec = facts[5];
            info->time = wcstod(facts[6].c_str(), nullptr);
            if (LoadJpeg(wic, base + L".card.jpg", info->thumb, info->tw, info->th) &&
                (!want.frame || LoadJpeg(wic, base + L".jpg", info->frame, info->fw, info->fh))) {
                Palette(info->thumb, info->palette);
                info->ok = true;
                return;
            }
        }
    }

    // Made afresh: the video's preview frame.
    if (SniffFile(want.path, nullptr) == VF_GIF) {
        info->fault = VF_GIF;
        return;
    }
    if (!SamePath(grab.Path(), want.path) || !grab.info.ok) grab.Open(want.path);
    if (!grab.info.ok) {
        info->fault = DiagnoseVideo(want.path, grab.info);
        return;
    }
    const VideoInfo& vi = grab.info;
    info->vw = vi.w;
    info->vh = vi.h;
    info->duration = vi.duration;
    info->fps = vi.fps;
    info->sound = !vi.audioStreams.empty();
    info->codec = vi.codec;
    info->time = PreviewTimeFor(profile.previewTime, vi.duration);
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    if (!grab.Grab(info->time, px, w, h)) {
        info->fault = VF_OTHER;
        return;
    }
    const int big = std::max({screenW, screenH, 1280});
    Shrink(wic, px, w, h, big, big, info->frame, info->fw, info->fh);
    Shrink(wic, px, w, h, 480, 480, info->thumb, info->tw, info->th);
    Palette(info->thumb, info->palette);
    info->ok = true;
    SaveJpeg(wic, base + L".jpg", info->frame, info->fw, info->fh);
    SaveJpeg(wic, base + L".card.jpg", info->thumb, info->tw, info->th);
    wchar_t num[4][32];
    swprintf(num[0], 32, L"%.3f", info->duration);
    swprintf(num[1], 32, L"%.3f", info->fps);
    swprintf(num[2], 32, L"%.3f", info->time);
    WriteFileList(base + L".txt", {std::to_wstring(info->vw), std::to_wstring(info->vh), num[0], num[1], info->sound ? L"1" : L"0",
                                   info->codec.empty() ? L"?" : info->codec, num[2]});
    if (!want.frame) {  // only asked for the card: no need to keep the big one in memory
        info->frame.clear();
        info->fw = info->fh = 0;
    }
}

DWORD WINAPI WorkerMain(void* arg) {
    Worker& W = *(Worker*)arg;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    const bool mf = MF.Start();
    {
        FrameGrabber grab;
        for (;;) {
            Want want;
            EnterCriticalSection(&W.cs);
            while (!W.quit && W.queue.empty()) {
                LeaveCriticalSection(&W.cs);
                WaitForSingleObject(W.wake, INFINITE);
                EnterCriticalSection(&W.cs);
            }
            const bool quit = W.quit;
            if (!quit) {
                want = W.queue.front();
                W.queue.pop_front();
            }
            LeaveCriticalSection(&W.cs);
            if (quit) break;
            auto* info = new WallpaperInfo;
            if (wic && mf) Make(wic, grab, want, W.screenW, W.screenH, info);
            else info->fault = VF_NO_MEDIA;
            if (!PostMessageW(W.notify, W.msg, 0, (LPARAM)info)) delete info;
        }
    }
    if (mf) MF.Stop();
    SafeRelease(wic);
    CoUninitialize();
    W.Release();
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------

std::vector<std::wstring> LoadWallpapers(const std::wstring& current) {
    std::vector<std::wstring> list;
    bool changed = false;
    if (!ReadFileList(ListPath(), &list)) {  // the first time: every video used before
        list = KnownVideos();
        changed = true;
    }
    // A lighter version belongs on its original's line: one that got in on its own (put there when
    // it was the wallpaper) goes, so a version change never moves the video in the list.
    for (size_t i = 0; i < list.size();) {
        const std::wstring orig = LightCopyOriginalPath(list[i]);
        bool dup = false;
        for (auto& p : list) dup = dup || (!orig.empty() && SamePath(p, orig));
        if (dup) {
            list.erase(list.begin() + i);
            changed = true;
        } else {
            i++;
        }
    }
    const std::wstring currentOrig = current.empty() ? L"" : LightCopyOriginalPath(current);
    bool has = current.empty();
    for (auto& p : list) has = has || SamePath(p, current) || (!currentOrig.empty() && SamePath(p, currentOrig));
    if (!has) {  // chosen some other way (the classic settings, the tray): it belongs here too
        list.insert(list.begin(), current);
        changed = true;
    }
    if (changed) SaveWallpapers(list);
    return list;
}

void SaveWallpapers(const std::vector<std::wstring>& files) { WriteFileList(ListPath(), files); }

void Library_Start(HWND notify, UINT msg, int screenW, int screenH) {
    Library_Stop();
    auto* w = new Worker;
    InitializeCriticalSection(&w->cs);
    w->notify = notify;
    w->msg = msg;
    w->screenW = screenW;
    w->screenH = screenH;
    w->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    w->thread = CreateThread(nullptr, 0, WorkerMain, w, 0, nullptr);
    if (!w->thread) w->refs = 1;
    g_worker = w;
}

void Library_Request(const std::wstring& path, bool frame, bool urgent) {
    if (!g_worker) return;
    Worker& W = *g_worker;
    EnterCriticalSection(&W.cs);
    for (auto it = W.queue.begin(); it != W.queue.end(); ++it)
        if (SamePath(it->path, path)) {  // already asked: maybe sooner, maybe with the frame now
            frame = frame || it->frame;
            if (!urgent) {
                it->frame = frame;
                LeaveCriticalSection(&W.cs);
                return;
            }
            W.queue.erase(it);
            break;
        }
    if (urgent) W.queue.push_front(Want{path, frame});
    else W.queue.push_back(Want{path, frame});
    LeaveCriticalSection(&W.cs);
    SetEvent(W.wake);
}

void Library_Stop() {
    if (!g_worker) return;
    Worker& W = *g_worker;
    g_worker = nullptr;
    EnterCriticalSection(&W.cs);
    W.quit = true;
    W.queue.clear();
    LeaveCriticalSection(&W.cs);
    SetEvent(W.wake);
    // A video being read right now finishes on its own; its result can't reach the closed picker.
    if (W.thread) {
        if (WaitForSingleObject(W.thread, 300) == WAIT_TIMEOUT) Log(L"picker: preview worker still busy at close");
        CloseHandle(W.thread);
    }
    W.Release();
}
