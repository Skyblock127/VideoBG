// Music from other apps: what another app plays (YouTube Music in Chrome, YouTube in Brave, Spotify...),
// read from Windows' media controls (the media sessions the volume flyout shows), and its buttons. The
// app picked as the music's source (Settings::musicSource) is watched closely: its song, artist, cover,
// place, whether it plays and which of its buttons work. VideoBG's own session (My music, media.cpp)
// is left out. Windows tells us when something changes (the events post WM_VBG_NOWPLAYING to the
// host); the host also looks once a second while an app is the source (its place moves on), else
// every few seconds. Every change goes to the log, to see what Windows reports.
#include "common.h"
#include <inspectable.h>
#include <eventtoken.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>

namespace {

// The WinRT interfaces this needs (Windows.Media.Control), declared here as media.cpp does: MinGW has
// no header for them. Only the methods used have their real signatures; the rest hold their place.
enum { ASYNC_STARTED = 0, ASYNC_COMPLETED = 1 };   // Windows.Foundation.AsyncStatus
enum { SESSION_CLOSED = 0, SESSION_PLAYING = 4 };  // ...SessionPlaybackStatus (5: paused)

struct IAsyncInfoX : IInspectable {  // Windows.Foundation.IAsyncInfo
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

template <class T> struct IAsyncOp : IInspectable {  // Windows.Foundation.IAsyncOperation<T>
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(T**) = 0;
};

template <class T> struct IVectorViewX : IInspectable {  // Windows.Foundation.Collections.IVectorView<T>
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, T**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
};

struct IStreamRef : IInspectable {  // Windows.Storage.Streams.IRandomAccessStreamReference
    virtual HRESULT STDMETHODCALLTYPE OpenReadAsync(IAsyncOp<IInspectable>**) = 0;
};

struct IProps : IInspectable {  // IGlobalSystemMediaTransportControlsSessionMediaProperties
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Subtitle(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTitle(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TrackNumber(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Genres(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTrackCount(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackType(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(IStreamRef**) = 0;
};

struct IControlsX : IInspectable {  // IGlobalSystemMediaTransportControlsSessionPlaybackControls
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPauseEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsStopEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRecordEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsFastForwardEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRewindEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsNextEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPreviousEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelUpEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelDownEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayPauseToggleEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsShuffleEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRepeatEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlaybackRateEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlaybackPositionEnabled(boolean*) = 0;
};

struct IPlaybackInfo : IInspectable {  // IGlobalSystemMediaTransportControlsSessionPlaybackInfo
    virtual HRESULT STDMETHODCALLTYPE get_Controls(IControlsX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(int*) = 0;
};

struct ITimeline : IInspectable {  // IGlobalSystemMediaTransportControlsSessionTimelineProperties (100 ns units)
    virtual HRESULT STDMETHODCALLTYPE get_StartTime(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_EndTime(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MinSeekTime(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxSeekTime(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Position(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LastUpdatedTime(INT64*) = 0;  // a FILETIME
};

struct IHandler : IUnknown {  // TypedEventHandler<sender, args>: the same shape for every event here
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable* sender, IInspectable* args) = 0;
};

struct ISession : IInspectable {  // IGlobalSystemMediaTransportControlsSession
    virtual HRESULT STDMETHODCALLTYPE get_SourceAppUserModelId(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetMediaPropertiesAsync(IAsyncOp<IProps>**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTimelineProperties(ITimeline**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlaybackInfo(IPlaybackInfo**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPlayAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPauseAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryStopAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRecordAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryFastForwardAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRewindAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipNextAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipPreviousAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelUpAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelDownAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryTogglePlayPauseAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeAutoRepeatModeAsync(int, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackRateAsync(double, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeShuffleActiveAsync(boolean, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackPositionAsync(INT64, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_TimelinePropertiesChanged(IHandler*, EventRegistrationToken*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_TimelinePropertiesChanged(EventRegistrationToken) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_PlaybackInfoChanged(IHandler*, EventRegistrationToken*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_PlaybackInfoChanged(EventRegistrationToken) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_MediaPropertiesChanged(IHandler*, EventRegistrationToken*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_MediaPropertiesChanged(EventRegistrationToken) = 0;
};

struct IManager : IInspectable {  // IGlobalSystemMediaTransportControlsSessionManager
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSession(ISession**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSessions(IVectorViewX<ISession>**) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_CurrentSessionChanged(IHandler*, EventRegistrationToken*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_CurrentSessionChanged(EventRegistrationToken) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_SessionsChanged(IHandler*, EventRegistrationToken*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_SessionsChanged(EventRegistrationToken) = 0;
};

struct IManagerStatics : IInspectable {  // IGlobalSystemMediaTransportControlsSessionManagerStatics
    virtual HRESULT STDMETHODCALLTYPE RequestAsync(IAsyncOp<IManager>**) = 0;
};

const IID kIID_Statics = {0x2050c4ee, 0x11a0, 0x57de, {0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45}};
const IID kIID_AsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIID_Agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
// The events' handler types (TypedEventHandler<...>, worked out from their type names as WinRT does).
const IID kIID_Events[] = {
    {0x2e2a8630, 0xdc8c, 0x530a, {0x97, 0x46, 0xbc, 0x98, 0x4d, 0x4b, 0x02, 0x9e}},  // SessionsChanged
    {0x228bd0ed, 0x1fa2, 0x5e9b, {0xa6, 0xec, 0x42, 0x56, 0x61, 0x73, 0x10, 0x3b}},  // CurrentSessionChanged
    {0x0f2ce2b7, 0xafa7, 0x5ed0, {0x8c, 0xb6, 0x8c, 0x40, 0xcf, 0x9b, 0x3a, 0x5f}},  // MediaPropertiesChanged
    {0x2bdf1426, 0xd41f, 0x5896, {0x89, 0x7f, 0xef, 0xc0, 0xb0, 0xfa, 0x73, 0x92}},  // PlaybackInfoChanged
    {0xe8bf62af, 0xfac1, 0x5fff, {0x90, 0x53, 0x0b, 0xf1, 0x91, 0xae, 0x77, 0x7e}},  // TimelinePropertiesChanged
};

using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using CreateStringFn = HRESULT(WINAPI*)(PCWSTR, UINT32, HSTRING*);
using DeleteStringFn = HRESULT(WINAPI*)(HSTRING);
using RawBufferFn = PCWSTR(WINAPI*)(HSTRING, UINT32*);
using StreamOverFn = HRESULT(WINAPI*)(IUnknown*, REFIID, void**);  // shcore's CreateStreamOverRandomAccessStream

template <class T> void Rel(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

HWND g_host;
HMODULE g_combase, g_shcore;
CreateStringFn g_makeString;
DeleteStringFn g_freeString;
RawBufferFn g_rawBuffer;
StreamOverFn g_streamOver;
IAsyncOp<IManager>* g_request;  // asking Windows for the session manager
IManager* g_manager;
bool g_unavailable;             // Windows said no: not asked again
EventRegistrationToken g_tSessions{}, g_tCurrent{};
std::atomic<bool> g_posted{false};  // a look is on its way to the host (events come in bursts)

std::vector<NowPlayingApp> g_apps;  // every other app with a session
std::wstring g_self;                // VideoBG's own session (its exe's name)
std::wstring g_sessionsLogged;

ISession* g_watched;          // the source app's session, while it has one
IUnknown* g_watchedId;        // ...its identity (a new session for the same app is a new object)
EventRegistrationToken g_tProps{}, g_tInfo{}, g_tTime{};
IAsyncOp<IProps>* g_propsOp;  // its song, being read
IAsyncOp<IInspectable>* g_coverOp;  // its cover, being opened
std::wstring g_coverFor;      // the song the cover is (being) read for
std::wstring g_coverSong;     // the song the cover shown belongs to
int g_coverTries;             // reads of a new song's cover, until one works
int g_coverLooks;             // later looks at it (a few seconds in)
int g_coverSerial;            // each new picture (the players redraw it)
ULONGLONG g_songAt;           // when the song came
std::atomic<bool> g_propsChanged{false};  // the app said its song's details changed (its cover may be new)
bool g_opChanged;             // ...before the read on its way was asked for
NowPlaying g_now;
ULONGLONG g_lostAt;           // when the source app's session went (0: it's there)
int g_pending = 2;            // a button pressed meanwhile (-1 previous, 0 play / pause, 1 next; 2 none): sent once it's back
std::wstring g_logged;

struct Handler : IHandler {
    const bool props;  // the song's details changed
    explicit Handler(bool p) : props(p) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        bool ok = IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIID_Agile);
        for (const IID& e : kIID_Events) ok = ok || IsEqualIID(riid, e);
        *p = ok ? this : nullptr;
        return ok ? S_OK : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE Invoke(IInspectable*, IInspectable*) override {  // (on one of Windows' threads)
        if (props) g_propsChanged = true;
        if (!g_posted.exchange(true)) PostMessageW(g_host, WM_VBG_NOWPLAYING, 0, 0);
        return S_OK;
    }
} g_handler(false), g_propsHandler(true);

std::wstring Take(HSTRING h) {  // a WinRT string as text (and freed)
    std::wstring s;
    if (h && g_rawBuffer) {
        UINT32 n = 0;
        const wchar_t* p = g_rawBuffer(h, &n);
        if (p) s.assign(p, n);
    }
    if (h && g_freeString) g_freeString(h);
    return s;
}

// 0 still running, 1 done, -1 failed (or cancelled).
int Finished(IInspectable* op) {
    IAsyncInfoX* info = nullptr;
    if (FAILED(op->QueryInterface(kIID_AsyncInfo, (void**)&info))) return -1;
    int s = -1;
    info->get_Status(&s);
    info->Release();
    return s == ASYNC_STARTED ? 0 : s == ASYNC_COMPLETED ? 1 : -1;
}

std::wstring AppId(ISession* s) {
    HSTRING h = nullptr;
    return SUCCEEDED(s->get_SourceAppUserModelId(&h)) ? Take(h) : L"";
}

int Status(ISession* s) {
    IPlaybackInfo* info = nullptr;
    int st = SESSION_CLOSED;
    if (SUCCEEDED(s->GetPlaybackInfo(&info)) && info) info->get_PlaybackStatus(&st);
    Rel(info);
    return st;
}

// The app's name as the Start menu shows it ("YouTube Music" for a Chrome-installed app), else its exe's name.
std::wstring LookUpName(const std::wstring& id) {
    std::wstring name;
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, id.c_str(), IID_PPV_ARGS(&item)))) {
        PWSTR s = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &s)) && s) name = s;
        CoTaskMemFree(s);
        item->Release();
    }
    if (name.empty() || _wcsicmp(name.c_str(), id.c_str()) == 0) {
        const size_t slash = id.find_last_of(L"\\/");
        name = slash == std::wstring::npos ? id : id.substr(slash + 1);
        if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
        if (!name.empty()) name[0] = towupper(name[0]);
    }
    return name;
}

bool GetManager() {
    if (g_manager) return true;
    if (g_unavailable) return false;
    if (!g_request) {
        g_combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto getFactory = g_combase ? (RoGetActivationFactoryFn)(void*)GetProcAddress(g_combase, "RoGetActivationFactory") : nullptr;
        g_makeString = g_combase ? (CreateStringFn)(void*)GetProcAddress(g_combase, "WindowsCreateString") : nullptr;
        g_freeString = g_combase ? (DeleteStringFn)(void*)GetProcAddress(g_combase, "WindowsDeleteString") : nullptr;
        g_rawBuffer = g_combase ? (RawBufferFn)(void*)GetProcAddress(g_combase, "WindowsGetStringRawBuffer") : nullptr;
        g_shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        g_streamOver = g_shcore ? (StreamOverFn)(void*)GetProcAddress(g_shcore, "CreateStreamOverRandomAccessStream") : nullptr;
        const std::wstring cls = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
        HSTRING h = nullptr;
        if (g_makeString) g_makeString(cls.c_str(), (UINT32)cls.size(), &h);
        IManagerStatics* statics = nullptr;
        HRESULT hr = getFactory && h && g_rawBuffer ? getFactory(h, kIID_Statics, (void**)&statics) : E_NOINTERFACE;
        if (h) g_freeString(h);
        if (SUCCEEDED(hr)) hr = statics->RequestAsync(&g_request);
        Rel(statics);
        if (FAILED(hr) || !g_request) {
            Log(L"nowplaying: Windows' media sessions aren't available (0x%08lx)", (unsigned long)hr);
            g_unavailable = true;
        }
        return false;
    }
    const int done = Finished(g_request);
    if (!done) return false;
    if (done > 0) g_request->GetResults(&g_manager);
    Rel(g_request);
    if (!g_manager) {
        Log(L"nowplaying: Windows didn't give the media session manager");
        g_unavailable = true;
        return false;
    }
    const HRESULT a = g_manager->add_SessionsChanged(&g_handler, &g_tSessions), b = g_manager->add_CurrentSessionChanged(&g_handler, &g_tCurrent);
    Log(L"nowplaying: watching Windows' media sessions (events 0x%08lx 0x%08lx)", (unsigned long)a, (unsigned long)b);
    return true;
}

void Unwatch() {
    if (g_watched) {
        if (g_tProps.value) g_watched->remove_MediaPropertiesChanged(g_tProps);
        if (g_tInfo.value) g_watched->remove_PlaybackInfoChanged(g_tInfo);
        if (g_tTime.value) g_watched->remove_TimelinePropertiesChanged(g_tTime);
    }
    g_tProps = g_tInfo = g_tTime = EventRegistrationToken{};
    Rel(g_watched);
    Rel(g_watchedId);
    Rel(g_propsOp);
    Rel(g_coverOp);
    g_coverFor.clear();  // (watched again later: its cover is read again)
    g_coverSong.clear();
}

void Watch(ISession* s) {
    IUnknown* id = nullptr;
    s->QueryInterface(IID_IUnknown, (void**)&id);
    if (g_watched && id == g_watchedId) {
        Rel(id);
        return;
    }
    Unwatch();
    g_watched = s;
    s->AddRef();
    g_watchedId = id;
    s->add_MediaPropertiesChanged(&g_propsHandler, &g_tProps);
    s->add_PlaybackInfoChanged(&g_handler, &g_tInfo);
    s->add_TimelinePropertiesChanged(&g_handler, &g_tTime);
    Log(L"nowplaying: watching %ls's session", AppId(s).c_str());
}

// The cover: the picture's middle square, 160 pixels a side (it's drawn small).
bool DecodeCover(IInspectable* stream, std::vector<uint32_t>& px, UINT* srcW, UINT* srcH) {
    constexpr UINT kSide = 160;
    IStream* st = nullptr;
    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapClipper* clip = nullptr;
    IWICBitmapScaler* scale = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    bool ok = g_streamOver && SUCCEEDED(g_streamOver(stream, IID_PPV_ARGS(&st))) &&
              SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
              SUCCEEDED(wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
              SUCCEEDED(fr->GetSize(&w, &h)) && w && h && w <= 8192 && h <= 8192;
    if (ok) {
        const UINT side = std::min(w, h);
        const WICRect sq{(INT)(w - side) / 2, (INT)(h - side) / 2, (INT)side, (INT)side};
        ok = SUCCEEDED(wic->CreateBitmapClipper(&clip)) && SUCCEEDED(clip->Initialize(fr, &sq)) && SUCCEEDED(wic->CreateBitmapScaler(&scale)) &&
             SUCCEEDED(scale->Initialize(clip, kSide, kSide, WICBitmapInterpolationModeFant)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
             SUCCEEDED(conv->Initialize(scale, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    }
    if (ok) {
        px.resize((size_t)kSide * kSide);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, kSide * 4, (UINT)px.size() * 4, (BYTE*)px.data()));
        for (uint32_t& p : px) p |= 0xFF000000u;  // covers are opaque
    }
    Rel(conv);
    Rel(scale);
    Rel(clip);
    Rel(fr);
    Rel(dec);
    Rel(wic);
    Rel(st);
    *srcW = w;
    *srcH = h;
    return ok;
}

// The watched app's state now (the song comes from the last props read; its cover in its own time).
void ReadWatched(NowPlaying& now) {
    IPlaybackInfo* info = nullptr;
    IControlsX* ctl = nullptr;
    if (SUCCEEDED(g_watched->GetPlaybackInfo(&info)) && info) {
        int st = SESSION_CLOSED;
        info->get_PlaybackStatus(&st);
        now.playing = st == SESSION_PLAYING;
        if (SUCCEEDED(info->get_Controls(&ctl)) && ctl) {
            boolean b = false;
            if (SUCCEEDED(ctl->get_IsPreviousEnabled(&b))) now.canPrev = b;
            b = false;
            if (SUCCEEDED(ctl->get_IsNextEnabled(&b))) now.canNext = b;
            boolean p1 = false, p2 = false, p3 = false;
            ctl->get_IsPlayPauseToggleEnabled(&p1);
            ctl->get_IsPlayEnabled(&p2);
            ctl->get_IsPauseEnabled(&p3);
            now.canPlay = p1 || p2 || p3;
            b = false;
            if (SUCCEEDED(ctl->get_IsPlaybackPositionEnabled(&b))) now.canSeek = b;
        }
    }
    Rel(ctl);
    Rel(info);
    ITimeline* tl = nullptr;
    if (SUCCEEDED(g_watched->GetTimelineProperties(&tl)) && tl) {
        INT64 start = 0, end = 0, pos = 0, at = 0;
        tl->get_StartTime(&start);
        tl->get_EndTime(&end);
        tl->get_Position(&pos);
        tl->get_LastUpdatedTime(&at);
        if (end > start) {
            now.durMs = (UINT)((end - start) / 10000);
            now.posMs = (UINT)(std::max<INT64>(0, pos - start) / 10000);
            now.posAt = at;
            now.startTicks = start;
        }
    }
    Rel(tl);
    now.canSeek = now.canSeek && now.durMs > 0;
}

}  // namespace

void NowPlaying_Start(HWND host) { g_host = host; }

void NowPlaying_Tick() {
    g_posted = false;
    if (!GetManager()) return;
    if (g_self.empty()) {
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring path(exe, n);
        g_self = path.substr(path.find_last_of(L'\\') + 1);
    }
    // Every other app's session; the source app's is watched.
    const std::wstring want = g_settings.sound == 2 ? g_settings.musicSource : L"";
    std::vector<NowPlayingApp> apps;
    std::wstring all;
    ISession* source = nullptr;
    IVectorViewX<ISession>* list = nullptr;
    if (SUCCEEDED(g_manager->GetSessions(&list)) && list) {
        UINT32 n = 0;
        list->get_Size(&n);
        for (UINT32 i = 0; i < n; i++) {
            ISession* s = nullptr;
            if (FAILED(list->GetAt(i, &s)) || !s) continue;
            const std::wstring id = AppId(s);
            const int st = Status(s);
            all += (all.empty() ? L"" : L", ") + id + L" (" + std::to_wstring(st) + L")";
            const bool dup = std::any_of(apps.begin(), apps.end(), [&](const NowPlayingApp& a) { return a.id == id; });
            if (_wcsicmp(id.c_str(), g_self.c_str()) != 0 && !id.empty() && !dup) {
                NowPlayingApp a;
                a.id = id;
                auto known = std::find_if(g_apps.begin(), g_apps.end(), [&](const NowPlayingApp& o) { return o.id == id; });
                a.name = known != g_apps.end() ? known->name : LookUpName(id);
                a.playing = st == SESSION_PLAYING;
                apps.push_back(a);
                if (id == want && !source && st != SESSION_CLOSED) {
                    source = s;
                    continue;
                }
            }
            s->Release();
        }
    }
    Rel(list);
    const bool appsChanged = apps.size() != g_apps.size() ||
                             !std::equal(apps.begin(), apps.end(), g_apps.begin(), [](const NowPlayingApp& a, const NowPlayingApp& b) {
                                 return a.id == b.id && a.playing == b.playing;
                             });
    g_apps = apps;
    if (all != g_sessionsLogged) {
        g_sessionsLogged = all;
        Log(L"nowplaying: sessions (status 4 plays, 5 paused): %ls", all.empty() ? L"none" : all.c_str());
    }
    if (source) {
        g_lostAt = 0;
        Watch(source);
        source->Release();
        if (g_pending != 2) {
            const int what = g_pending;
            g_pending = 2;
            NowPlaying_Command(what);
        }
    } else {
        // Chrome closes a song's session and opens a new one when it skips (1-4 s with none): the
        // song stays shown meanwhile, rather than "Nothing playing" for a moment.
        const ULONGLONG at = GetTickCount64();
        if (!g_lostAt) g_lostAt = at;
        Unwatch();
        if (g_now.on && g_now.id == want && at - g_lostAt < 8000) {
            if (appsChanged) UI_Refresh();
            return;
        }
        g_pending = 2;
    }

    NowPlaying now;
    now.id = want;
    now.app = want.empty() ? L"" : NowPlaying_Name(want);
    if (g_watched) {
        now.on = true;
        if (g_now.on && g_now.id == want) {  // the song stays until it's read again
            now.title = g_now.title;
            now.artist = g_now.artist;
            now.cover = g_now.cover;
            now.coverKey = g_now.coverKey;
        }
        ReadWatched(now);
        if (g_propsOp && Finished(g_propsOp)) {
            IProps* props = nullptr;
            if (Finished(g_propsOp) > 0 && SUCCEEDED(g_propsOp->GetResults(&props)) && props) {
                HSTRING h = nullptr;
                now.title = SUCCEEDED(props->get_Title(&h)) ? Take(h) : L"";
                h = nullptr;
                now.artist = SUCCEEDED(props->get_Artist(&h)) ? Take(h) : L"";
                // Its cover: read when the song changes (tried a few times: the app may set it a moment
                // after the song), and again when the app says the song's details changed, and 3 and
                // 12 seconds in. Chrome sends its own icon until it has downloaded the song's picture,
                // then the picture: read only once, the icon could stay for the whole song.
                const std::wstring key = now.title + L"\n" + now.artist;
                const ULONGLONG at = GetTickCount64();
                if (key != g_coverFor) {
                    g_coverFor = key;
                    g_coverTries = g_coverLooks = 0;
                    g_songAt = at;
                }
                const bool late = g_coverLooks < 2 && at - g_songAt > (g_coverLooks ? 12000u : 3000u);
                const bool read = g_coverSong != key ? g_coverTries < 4 : g_opChanged || late;
                IStreamRef* ref = nullptr;
                if (read && !g_coverOp && SUCCEEDED(props->get_Thumbnail(&ref)) && ref) {
                    if (g_coverSong != key) g_coverTries++;
                    else if (late) g_coverLooks++;
                    ref->OpenReadAsync(&g_coverOp);
                }
                Rel(ref);
            }
            Rel(props);
            Rel(g_propsOp);
        }
        if (g_coverOp && Finished(g_coverOp)) {
            IInspectable* stream = nullptr;
            std::vector<uint32_t> px;
            UINT w = 0, h = 0;
            if (Finished(g_coverOp) > 0 && SUCCEEDED(g_coverOp->GetResults(&stream)) && stream && DecodeCover(stream, px, &w, &h) &&
                (g_coverSong != g_coverFor || !now.cover || *now.cover != px)) {  // (the same picture again: nothing to redraw)
                Log(L"nowplaying: cover %ux%u for \"%ls\"", w, h, now.title.c_str());
                now.cover = std::make_shared<std::vector<uint32_t>>(std::move(px));
                now.coverKey = std::to_wstring(++g_coverSerial);
                g_coverSong = g_coverFor;
            }
            Rel(stream);
            Rel(g_coverOp);
        }
        if (g_coverSong != now.title + L"\n" + now.artist && g_coverTries >= 4) now.cover.reset();  // a song without a cover
        if (!g_propsOp) {  // read on the next look
            g_opChanged = g_propsChanged.exchange(false);
            g_watched->TryGetMediaPropertiesAsync(&g_propsOp);
        }
    }
    const bool changed = now.on != g_now.on || now.id != g_now.id || now.playing != g_now.playing || now.title != g_now.title ||
                         now.artist != g_now.artist || now.durMs != g_now.durMs || now.canPrev != g_now.canPrev || now.canNext != g_now.canNext ||
                         now.canPlay != g_now.canPlay || now.canSeek != g_now.canSeek || now.cover != g_now.cover;
    const bool moved = now.posMs != g_now.posMs || now.posAt != g_now.posAt;
    g_now = now;
    if (changed) {
        wchar_t what[64];
        swprintf(what, 64, L"%ls%ls%ls%ls%ls", now.canPrev ? L" prev" : L"", now.canPlay ? L" play" : L"", now.canNext ? L" next" : L"",
                 now.canSeek ? L" seek" : L"", now.cover ? L" cover" : L"");
        const std::wstring line = now.id.empty() ? L"My music"
                                  : !now.on      ? now.app + L" has no session"
                                                 : now.app + L": " + (now.playing ? L"playing" : L"paused") + L" \"" + now.title + L"\" by \"" +
                                                  now.artist + L"\", " + std::to_wstring(now.durMs / 1000) + L" s, buttons:" + what;
        if (line != g_logged) {
            g_logged = line;
            Log(L"nowplaying: source %ls", line.c_str());
        }
    }
    if (changed || moved) Host_NowPlayingChanged();
    else if (appsChanged) UI_Refresh();
}

NowPlaying NowPlaying_Get() { return g_now; }

UINT NowPlaying::PosNow() const {
    if (!playing || !posAt) return std::min(posMs, durMs);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const INT64 nowT = (INT64)((UINT64)ft.dwHighDateTime << 32 | ft.dwLowDateTime);
    return (UINT)std::min<INT64>(durMs, posMs + std::max<INT64>(0, nowT - posAt) / 10000);
}

std::vector<NowPlayingApp> NowPlaying_Apps() { return g_apps; }

std::wstring NowPlaying_Name(const std::wstring& id) {
    for (const NowPlayingApp& a : g_apps)
        if (a.id == id) return a.name;
    static std::wstring lastId, lastName;  // (an app that's gone: still named as it was)
    if (id != lastId) {
        lastId = id;
        lastName = LookUpName(id);
    }
    return lastName;
}

void NowPlaying_Command(int what) {
    if (!g_watched) {
        if (g_lostAt && g_now.on) g_pending = what;  // (between two of its sessions)
        return;
    }
    IInspectable* op = nullptr;  // (it carries on once let go)
    const HRESULT hr = what < 0 ? g_watched->TrySkipPreviousAsync(&op) : what > 0 ? g_watched->TrySkipNextAsync(&op)
                                                                                    : g_watched->TryTogglePlayPauseAsync(&op);
    Log(L"nowplaying: %ls sent to %ls (0x%08lx)", what < 0 ? L"previous" : what > 0 ? L"next" : L"play/pause", g_now.app.c_str(),
        (unsigned long)hr);
    Rel(op);
}

void NowPlaying_Seek(UINT ms) {
    if (!g_watched || !g_now.canSeek) return;
    IInspectable* op = nullptr;
    const HRESULT hr = g_watched->TryChangePlaybackPositionAsync(g_now.startTicks + (INT64)ms * 10000, &op);
    Log(L"nowplaying: moved %ls to %u:%02u (0x%08lx)", g_now.app.c_str(), ms / 60000, ms / 1000 % 60, (unsigned long)hr);
    Rel(op);
    g_now.posMs = ms;  // shown there at once (the app says where it got to)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    g_now.posAt = (INT64)((UINT64)ft.dwHighDateTime << 32 | ft.dwLowDateTime);
}

void NowPlaying_Stop() {
    Unwatch();
    if (g_manager) {
        if (g_tSessions.value) g_manager->remove_SessionsChanged(g_tSessions);
        if (g_tCurrent.value) g_manager->remove_CurrentSessionChanged(g_tCurrent);
    }
    g_tSessions = g_tCurrent = EventRegistrationToken{};
    Rel(g_request);
    Rel(g_manager);
    g_now = NowPlaying{};
    g_apps.clear();
}
