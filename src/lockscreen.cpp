// The Windows pictures that follow the wallpaper: the lock screen (WinRT API
// Windows.System.UserProfile.LockScreen) and the desktop background (further down). Windows only
// allows still images there, so while the video wallpaper shows, VideoBG gives them a frame of the
// video (cropped like the wallpaper); while it's off, the user's own pictures. Windows keeps its
// lock screen copy in a folder only the system can write, so every switch goes through the API
// (about 0.4 s), done here on a worker thread so the hotkey never waits.
//
// Whose picture Windows shows is decided by fingerprinting the image Windows actually has: a
// picture VideoBG didn't set is the user's own (the first time, or changed in Windows Settings) and
// is saved as the one to show while the wallpaper is off. That holds across restarts and crashes.
// Everything is loaded on demand; nothing here stays resident.
#include "mfhelp.h"
#include <inspectable.h>
#include <winstring.h>
#include <shcore.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <iterator>

// WinRT ABI interfaces MinGW's headers don't provide. They are implemented by Windows, so they
// must NOT live in an anonymous namespace: with link-time optimisation GCC would then conclude
// that nothing implements them and compile every call into a jump to address 0 (the crash in
// 1.0.0 when pressing "Lock screen").
struct IVbgLockScreenStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_OriginalImageFile(IUnknown** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetImageStream(IUnknown** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetImageFileAsync(IUnknown* file, IUnknown** action) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetImageStreamAsync(IUnknown* stream, IUnknown** action) = 0;
};

struct IVbgAsyncInfo : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;  // 0 started, 1 completed, 2 canceled, 3 error
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* code) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

namespace {

const IID kIID_ILockScreenStatics = {0x3ee9d3ad, 0xb607, 0x40ae, {0xb4, 0x26, 0x76, 0x31, 0xd9, 0x82, 0x12, 0x69}};
const IID kIID_IRandomAccessStream = {0x905a0fe1, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
const IID kIID_IAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using CreateStringReferenceFn = HRESULT(WINAPI*)(PCWSTR, UINT32, HSTRING_HEADER*, HSTRING*);
using CreateRasOverStreamFn = HRESULT(WINAPI*)(IStream*, BSOS_OPTIONS, REFIID, void**);
using CreateStreamOverRasFn = HRESULT(WINAPI*)(IUnknown*, REFIID, void**);

template <class T> void Rel(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// [LockScreen] in settings.ini:
//   Ours=      fingerprint of the picture Windows showed right after VideoBG last set one
//   Shows=     what that was: "own" (the user's picture) or the frame's key
//   FrameKey=  what frame.jpg was made from (video, frame, crop, screen size)
std::wstring BackupPath() { return AppDataDir() + L"\\lockscreen-original.img"; }  // the user's own picture
std::wstring FramePath() { return AppDataDir() + L"\\frame.jpg"; }  // the video's frame: lock screen and background
bool HasBackup() { return GetFileAttributesW(BackupPath().c_str()) != INVALID_FILE_ATTRIBUTES; }

// Loads combase/shcore and gets the LockScreen statics; everything is freed with the object.
struct Api {
    HMODULE combase = nullptr, shcore = nullptr;
    CreateRasOverStreamFn makeRas = nullptr;
    CreateStreamOverRasFn makeStream = nullptr;
    IVbgLockScreenStatics* lock = nullptr;

    HRESULT Open() {
        combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto getFactory = combase ? (RoGetActivationFactoryFn)(void*)GetProcAddress(combase, "RoGetActivationFactory") : nullptr;
        auto makeRef = combase ? (CreateStringReferenceFn)(void*)GetProcAddress(combase, "WindowsCreateStringReference") : nullptr;
        makeRas = shcore ? (CreateRasOverStreamFn)(void*)GetProcAddress(shcore, "CreateRandomAccessStreamOverStream") : nullptr;
        makeStream = shcore ? (CreateStreamOverRasFn)(void*)GetProcAddress(shcore, "CreateStreamOverRandomAccessStream") : nullptr;
        if (!getFactory || !makeRef || !makeRas || !makeStream) return E_NOINTERFACE;
        static const wchar_t kClass[] = L"Windows.System.UserProfile.LockScreen";
        HSTRING_HEADER header;
        HSTRING cls = nullptr;
        HRESULT hr = makeRef(kClass, (UINT32)(std::size(kClass) - 1), &header, &cls);
        if (SUCCEEDED(hr)) hr = getFactory(cls, kIID_ILockScreenStatics, (void**)&lock);
        return hr;
    }
    ~Api() {
        Rel(lock);
        if (combase) FreeLibrary(combase);
        if (shcore) FreeLibrary(shcore);
    }
};

// Reads the picture Windows currently shows on the lock screen (the image file Windows keeps,
// which is a re-encoded copy of whatever was set).
bool ReadCurrent(Api& api, std::string& data) {
    data.clear();
    IUnknown* ras = nullptr;
    IStream* in = nullptr;
    HRESULT hr = api.lock->GetImageStream(&ras);
    if (SUCCEEDED(hr) && ras) hr = api.makeStream(ras, IID_IStream, (void**)&in);
    if (SUCCEEDED(hr) && in) {
        char buf[64 * 1024];
        ULONG got = 0;
        while (SUCCEEDED(in->Read(buf, sizeof buf, &got)) && got > 0 && data.size() < (64u << 20)) data.append(buf, got);
    } else {
        Log(L"lock screen: couldn't read the current picture (0x%08lx)", (unsigned long)hr);
    }
    Rel(in);
    Rel(ras);
    return !data.empty();
}

// Identifies a lock screen picture by its bytes (FNV-1a 64 + size).
std::wstring Fingerprint(const std::string& data) {
    if (data.empty()) return L"";
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    wchar_t buf[48];
    swprintf(buf, 48, L"%016llx-%zu", h, data.size());
    return buf;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
    CloseHandle(f);
    ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

HRESULT SetFromFile(Api& api, const wchar_t* path) {
    IStream* file = nullptr;
    IUnknown* stream = nullptr;
    IUnknown* op = nullptr;
    IVbgAsyncInfo* info = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &file);
    if (SUCCEEDED(hr)) hr = api.makeRas(file, BSOS_DEFAULT, kIID_IRandomAccessStream, (void**)&stream);
    if (SUCCEEDED(hr)) hr = api.lock->SetImageStreamAsync(stream, &op);
    if (SUCCEEDED(hr)) hr = op->QueryInterface(kIID_IAsyncInfo, (void**)&info);
    if (SUCCEEDED(hr)) {
        int status = 0;
        for (int i = 0; i < 400 && SUCCEEDED(info->get_Status(&status)) && status == 0; i++) Sleep(25);
        if (status == 1) hr = S_OK;
        else if (status == 3) { HRESULT e = E_FAIL; info->get_ErrorCode(&e); hr = FAILED(e) ? e : E_FAIL; }
        else hr = status == 0 ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : E_ABORT;
        info->Close();
    }
    Rel(info);
    Rel(op);
    Rel(stream);
    Rel(file);
    return hr;
}

std::wstring FrameKey(const LockScreenWant& w) {
    wchar_t b[96];
    swprintf(b, 96, L"|%.3f|%.4f,%.4f,%.4f,%.4f,%d|%dx%d", w.time, w.crop.l, w.crop.t, w.crop.r, w.crop.b, w.crop.stretch, w.sw, w.sh);
    return w.video + b;
}

// Decodes the frame, crops it exactly like the wallpaper (fill), scales it to the screen and saves
// it as a JPEG for Windows.
HRESULT MakeFrameJpeg(const LockScreenWant& w, const std::wstring& out) {
    if (!MF.Start()) return E_NOINTERFACE;
    std::vector<uint32_t> px;
    int fw = 0, fh = 0;
    double t = 0, va = 0;
    {
        FrameGrabber grab;
        bool ok = grab.Open(w.video);
        if (ok) {
            t = PreviewTimeFor(w.time, grab.info.duration);
            va = grab.info.aspect;
            ok = grab.Grab(t, px, fw, fh);
        }
        if (!ok) {
            Log(L"lock screen: couldn't decode a frame of %ls", w.video.c_str());
            MF.Stop();
            return E_FAIL;
        }
    }
    MF.Stop();
    if (va <= 0) va = (double)fw / fh;
    FitRect f = ComputeFit(va, w.crop, CropMode(w.crop), w.sw, w.sh);
    WICRect rc;
    rc.X = std::clamp((int)lround(f.sl * fw), 0, fw - 1);
    rc.Y = std::clamp((int)lround(f.st * fh), 0, fh - 1);
    rc.Width = std::clamp((int)lround((f.sr - f.sl) * fw), 1, fw - rc.X);
    rc.Height = std::clamp((int)lround((f.sb - f.st) * fh), 1, fh - rc.Y);

    IWICImagingFactory* wic = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) return hr;
    IWICBitmap* bmp = nullptr;
    IWICBitmapClipper* clip = nullptr;
    IWICBitmapScaler* scale = nullptr;
    IWICFormatConverter* conv = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    std::wstring tmp = out + L".tmp";
    hr = wic->CreateBitmapFromMemory(fw, fh, GUID_WICPixelFormat32bppBGR, fw * 4, (UINT)(px.size() * 4), (BYTE*)px.data(), &bmp);
    if (SUCCEEDED(hr)) hr = wic->CreateBitmapClipper(&clip);
    if (SUCCEEDED(hr)) hr = clip->Initialize(bmp, &rc);
    if (SUCCEEDED(hr)) hr = wic->CreateBitmapScaler(&scale);
    if (SUCCEEDED(hr)) hr = scale->Initialize(clip, w.sw, w.sh, (WICBitmapInterpolationMode)4 /* HighQualityCubic */);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(scale, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr)) {
        PROPBAG2 opt{};
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.93f;
        props->Write(1, &opt, &v);
        hr = frame->Initialize(props);
    }
    if (SUCCEEDED(hr)) hr = frame->SetSize(w.sw, w.sh);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(conv, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    Rel(props);
    Rel(frame);
    Rel(enc);
    Rel(stream);
    Rel(conv);
    Rel(scale);
    Rel(clip);
    Rel(bmp);
    wic->Release();
    if (SUCCEEDED(hr) && !MoveFileExW(tmp.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING)) hr = HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr)) {
        DeleteFileW(tmp.c_str());
        Log(L"lock screen: couldn't make the picture (0x%08lx)", (unsigned long)hr);
    } else {
        Log(L"lock screen: frame at %.2fs of %ls made (%dx%d)", t, w.video.c_str(), w.sw, w.sh);
    }
    return hr;
}

// The video's frame as a picture file, made when the video, frame, crop or screen size changes.
HRESULT EnsureFrame(const LockScreenWant& w) {
    const std::wstring key = FrameKey(w), file = FramePath();
    if (ReadState(L"LockScreen", L"FrameKey") == key && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) return S_OK;
    DeleteFileW((AppDataDir() + L"\\lockscreen.jpg").c_str());  // its name before 1.1
    WriteState(L"LockScreen", L"FrameKey", nullptr);
    HRESULT hr = MakeFrameJpeg(w, file);
    if (SUCCEEDED(hr)) WriteState(L"LockScreen", L"FrameKey", key.c_str());
    return hr;
}

// Makes the lock screen show what's wanted. *frame: whether it shows the video's frame afterwards.
HRESULT ApplyLockScreen(const LockScreenWant& w, bool* frame) {
    *frame = false;
    Api api;
    HRESULT hr = api.Open();
    if (FAILED(hr)) {
        Log(L"lock screen: WinRT unavailable (0x%08lx)", (unsigned long)hr);
        return hr;
    }
    std::string cur;
    std::wstring fp = ReadCurrent(api, cur) ? Fingerprint(cur) : L"";
    std::wstring shows = ReadState(L"LockScreen", L"Shows");
    if (!fp.empty() && fp != ReadState(L"LockScreen", L"Ours")) {
        // A picture VideoBG didn't set: the user's own. It's the one for while the wallpaper is off.
        bool ok = WriteFileAtomic(BackupPath(), cur);
        Log(L"lock screen: kept your picture (%ls) for while the wallpaper is off -> %ls", fp.c_str(), ok ? L"ok" : L"failed");
        WriteState(L"LockScreen", L"Ours", fp.c_str());
        WriteState(L"LockScreen", L"Shows", L"own");
        shows = L"own";
    }
    const std::wstring key = w.frame ? FrameKey(w) : L"own";
    if (!fp.empty() && shows == key) {  // already showing it
        *frame = w.frame;
        return S_OK;
    }
    if (!w.frame && !HasBackup()) return S_FALSE;  // no picture of the user's known: leave it
    hr = SetFromFile(api, w.frame ? FramePath().c_str() : BackupPath().c_str());
    if (SUCCEEDED(hr)) {
        std::string now;
        std::wstring nfp = ReadCurrent(api, now) ? Fingerprint(now) : L"";
        WriteState(L"LockScreen", L"Ours", nfp.empty() ? nullptr : nfp.c_str());
        WriteState(L"LockScreen", L"Shows", nfp.empty() ? nullptr : key.c_str());
        *frame = w.frame;
    }
    Log(L"lock screen: %ls -> 0x%08lx", w.frame ? L"the video's frame" : L"your own picture", (unsigned long)hr);
    return hr;
}

// ---- Desktop background --------------------------------------------------------------------------
// Windows shows the desktop background picture wherever VideoBG's video isn't: behind the admin
// (UAC) prompt, and at sign-in until VideoBG starts. So while the video shows, the background is
// the same frame too, set the way Windows Settings does it (only the picture: its fit is left as it
// is, and the frame is screen-sized anyway). [Background] in settings.ini:
//   Own=       the user's picture (anything that isn't VideoBG's frame is theirs)
//   OwnCopy=   a copy of it, in case the original file goes away while the frame is up
//   History0..4= Settings' "recent images" as they were, put back with the user's picture
//   Shows=     the frame's key while the frame is the background

const wchar_t kDesktopKey[] = L"Control Panel\\Desktop";
const wchar_t kWallpapersKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Wallpapers";

std::wstring RegString(const wchar_t* key, const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

bool SamePath(const std::wstring& a, const std::wstring& b) { return !a.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool Exists(const std::wstring& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring CurrentBackground() { return RegString(kDesktopKey, L"WallPaper"); }

// Only where it has been checked to be harmless: Windows 11 24H2's desktop (the video isn't
// disturbed by a background change there), one screen, and a single picture as the background (not
// a slideshow, Spotlight or a plain colour, which a picture would replace).
bool BackgroundFollowable() {
    HWND pm = FindWindowW(L"Progman", nullptr);
    if (!pm || !(GetWindowLongPtrW(pm, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP)) return false;
    if (GetSystemMetrics(SM_CMONITORS) != 1) return false;
    DWORD type = 0, size = sizeof type;
    RegGetValueW(HKEY_CURRENT_USER, kWallpapersKey, L"BackgroundType", RRF_RT_REG_DWORD, nullptr, &type, &size);
    return type == 0 && !CurrentBackground().empty();
}

HRESULT SetBackground(const std::wstring& path) {
    if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (void*)path.c_str(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) return S_OK;
    DWORD e = GetLastError();
    return e ? HRESULT_FROM_WIN32(e) : E_FAIL;
}

// A background VideoBG didn't set is the user's own: keep it (and Settings' recent pictures) for
// while the wallpaper is off.
void NoteOwnBackground() {
    std::wstring cur = CurrentBackground();
    if (cur.empty() || SamePath(cur, FramePath())) return;
    if (SamePath(cur, ReadState(L"Background", L"Own")) && Exists(ReadState(L"Background", L"OwnCopy"))) return;
    std::wstring copy = AppDataDir() + L"\\background-original" + PathFindExtensionW(cur.c_str()), old = ReadState(L"Background", L"OwnCopy");
    if (!SamePath(old, copy)) DeleteFileW(old.c_str());
    bool ok = CopyFileW(cur.c_str(), copy.c_str(), FALSE) != 0;
    WriteState(L"Background", L"Own", cur.c_str());
    WriteState(L"Background", L"OwnCopy", ok ? copy.c_str() : nullptr);
    for (int i = 0; i < 5; i++) {
        wchar_t name[32];
        swprintf(name, 32, L"BackgroundHistoryPath%d", i);
        std::wstring v = RegString(kWallpapersKey, name);
        swprintf(name, 32, L"History%d", i);
        WriteState(L"Background", name, v.empty() || SamePath(v, FramePath()) ? nullptr : v.c_str());
    }
    Log(L"background: kept your picture %ls for while the wallpaper is off (copy %ls)", cur.c_str(), ok ? L"ok" : L"failed");
}

// Settings' "recent images" as they were before the frame came and went.
void RestoreRecentPictures() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kWallpapersKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    for (int i = 0; i < 5; i++) {
        wchar_t name[32];
        swprintf(name, 32, L"History%d", i);
        std::wstring v = ReadState(L"Background", name);
        if (v.empty()) continue;
        swprintf(name, 32, L"BackgroundHistoryPath%d", i);
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(k);
}

// Makes the desktop background show what's wanted. *frame: whether it shows the video's frame after.
HRESULT ApplyBackground(const LockScreenWant& w, bool* frame) {
    NoteOwnBackground();
    const bool isFrame = SamePath(CurrentBackground(), FramePath());
    *frame = isFrame;
    if (!w.frame) {  // the user's own picture (always allowed: it only undoes VideoBG's change)
        if (!isFrame) return S_OK;
        std::wstring own = ReadState(L"Background", L"Own");
        if (!Exists(own)) own = ReadState(L"Background", L"OwnCopy");
        if (!Exists(own)) {
            Log(L"background: your picture is gone, so the frame stays");
            return S_FALSE;
        }
        ULONGLONG t0 = GetTickCount64();
        HRESULT hr = SetBackground(own);
        if (SUCCEEDED(hr)) {
            RestoreRecentPictures();
            WriteState(L"Background", L"Shows", nullptr);
            *frame = false;
        }
        Log(L"background: your own picture (%llu ms) -> 0x%08lx", GetTickCount64() - t0, (unsigned long)hr);
        return hr;
    }
    if (!w.background) return S_OK;  // not yet (the video isn't up)
    if (!BackgroundFollowable()) {
        static bool told;
        if (!told) Log(L"background: left alone (only followed with one screen, a picture background, Windows 11 24H2+)");
        told = true;
        return S_FALSE;
    }
    const std::wstring key = FrameKey(w);
    if (isFrame && ReadState(L"Background", L"Shows") == key) return S_OK;
    ULONGLONG t0 = GetTickCount64();
    HRESULT hr = SetBackground(FramePath());
    if (SUCCEEDED(hr)) {
        WriteState(L"Background", L"Shows", key.c_str());
        *frame = true;
    }
    Log(L"background: the video's frame (%llu ms) -> 0x%08lx", GetTickCount64() - t0, (unsigned long)hr);
    return hr;
}

// One worker at a time; the latest request wins.
struct Sync {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE idle = nullptr;   // set while no work is queued or running
    HANDLE early = nullptr;  // set once the background is the user's again (or the work is done)
    bool running = false, pending = false;
    LockScreenWant want;
    HWND notify = nullptr;
    UINT msg = 0;
} S;

// Lock screen and background. Going back to the user's pictures, the background goes first and the
// host hears about it at once (LSF_EARLY), so the video can stop without the frame showing through.
HRESULT Apply(const LockScreenWant& w, HWND notify, UINT msg, int* flags) {
    *flags = 0;
    bool lock = false, bg = false;
    HRESULT hr = S_OK, hb = S_OK;
    if (w.frame) {
        hr = EnsureFrame(w);
        if (FAILED(hr)) return hr;
    } else {
        hb = ApplyBackground(w, &bg);
        SetEvent(S.early);
        if (notify) PostMessageW(notify, msg, (WPARAM)hb, LSF_EARLY | (bg ? LSF_BACKGROUND : 0));
    }
    hr = ApplyLockScreen(w, &lock);
    if (w.frame) hb = ApplyBackground(w, &bg);
    *flags = (lock ? LSF_LOCKSCREEN : 0) | (bg ? LSF_BACKGROUND : 0);
    return FAILED(hr) ? hr : hb;
}

DWORD WINAPI SyncThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (bool more = true; more;) {
        AcquireSRWLockExclusive(&S.lock);
        LockScreenWant w = S.want;
        HWND notify = S.notify;
        UINT msg = S.msg;
        S.pending = false;
        ReleaseSRWLockExclusive(&S.lock);
        int flags = 0;
        HRESULT hr = Apply(w, notify, msg, &flags);
        // Idle before the last notification, so whoever gets it sees the work finished.
        AcquireSRWLockExclusive(&S.lock);
        more = S.pending;
        if (!more) {
            S.running = false;
            SetEvent(S.early);
            SetEvent(S.idle);
        }
        ReleaseSRWLockExclusive(&S.lock);
        if (notify) PostMessageW(notify, msg, (WPARAM)hr, flags);
    }
    CoUninitialize();
    return 0;
}

}  // namespace

std::wstring OwnDesktopBackground() {
    std::wstring cur = CurrentBackground();
    if (!SamePath(cur, FramePath())) return cur;
    std::wstring own = ReadState(L"Background", L"Own");
    return Exists(own) ? own : ReadState(L"Background", L"OwnCopy");
}

void LockScreen_Sync(HWND notify, UINT msg, const LockScreenWant& want) {
    AcquireSRWLockExclusive(&S.lock);
    if (!S.idle) S.idle = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!S.early) S.early = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    S.want = want;
    S.notify = notify;
    S.msg = msg;
    S.pending = true;
    ResetEvent(S.idle);
    if (!want.frame) ResetEvent(S.early);
    if (!S.running) {
        HANDLE t = CreateThread(nullptr, 0, SyncThread, nullptr, 0, nullptr);
        if (t) {
            S.running = true;
            CloseHandle(t);
        } else {
            S.pending = false;
            SetEvent(S.early);
            SetEvent(S.idle);
        }
    }
    ReleaseSRWLockExclusive(&S.lock);
}

bool LockScreen_Wait(DWORD ms) {
    AcquireSRWLockShared(&S.lock);
    HANDLE idle = S.idle;
    ReleaseSRWLockShared(&S.lock);
    return !idle || WaitForSingleObject(idle, ms) == WAIT_OBJECT_0;
}

bool LockScreen_Busy() { return !LockScreen_Wait(0); }

bool LockScreen_WaitBackground(DWORD ms) {
    AcquireSRWLockShared(&S.lock);
    HANDLE early = S.early;
    ReleaseSRWLockShared(&S.lock);
    return !early || WaitForSingleObject(early, ms) == WAIT_OBJECT_0;
}
