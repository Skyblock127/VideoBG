// Windows' media controls: the play/pause, next and previous keys on keyboards (and headsets), and
// the media card in the volume flyout. My music takes part as its own media session, so Windows
// sends those keys to whichever app played last (VideoBG's music, or YouTube Music in a browser...),
// never to two at once. The buttons arrive on another thread and are posted to the host window.
#include "common.h"
#include <inspectable.h>
#include <eventtoken.h>

namespace {

// The few WinRT interfaces this needs, declared here (MinGW's windows.media.h doesn't compile).
// Only the methods used have their real signatures; the rest just hold their place in the table.
enum Status { STATUS_PAUSED = 4, STATUS_PLAYING = 3 };  // Windows.Media.MediaPlaybackStatus
enum Button { BUTTON_PLAY = 0, BUTTON_PAUSE = 1, BUTTON_STOP = 2, BUTTON_NEXT = 6, BUTTON_PREVIOUS = 7 };

struct IMusicProps : IInspectable {  // Windows.Media.IMusicDisplayProperties
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Title(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AlbumArtist(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Artist(HSTRING) = 0;
};

struct IDisplayUpdater : IInspectable {  // Windows.Media.ISystemMediaTransportControlsDisplayUpdater
    virtual HRESULT STDMETHODCALLTYPE get_Type(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Type(int) = 0;  // 1: music
    virtual HRESULT STDMETHODCALLTYPE get_AppMediaId(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AppMediaId(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Thumbnail(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MusicProperties(IMusicProps**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_VideoProperties(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ImageProperties(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyFromFileAsync(int, void*, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAll() = 0;
    virtual HRESULT STDMETHODCALLTYPE Update() = 0;
};

struct IButtonArgs : IInspectable {  // Windows.Media.ISystemMediaTransportControlsButtonPressedEventArgs
    virtual HRESULT STDMETHODCALLTYPE get_Button(int*) = 0;
};

struct IButtonHandler : IUnknown {  // TypedEventHandler<SystemMediaTransportControls, ...ButtonPressedEventArgs>
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable* sender, IButtonArgs* args) = 0;
};

struct IControls : IInspectable {  // Windows.Media.ISystemMediaTransportControls
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PlaybackStatus(int) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DisplayUpdater(IDisplayUpdater**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SoundLevel(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsEnabled(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsPlayEnabled(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsStopEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsStopEnabled(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPauseEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsPauseEnabled(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRecordEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsRecordEnabled(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsFastForwardEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsFastForwardEnabled(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRewindEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsRewindEnabled(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPreviousEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsPreviousEnabled(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsNextEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsNextEnabled(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelUpEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsChannelUpEnabled(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelDownEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsChannelDownEnabled(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ButtonPressed(IButtonHandler*, EventRegistrationToken*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ButtonPressed(EventRegistrationToken) = 0;
};

struct IControlsInterop : IInspectable {  // ISystemMediaTransportControlsInterop
    virtual HRESULT STDMETHODCALLTYPE GetForWindow(HWND, REFIID, void**) = 0;
};

const IID kIID_Interop = {0xddb0472d, 0xc911, 0x4a1f, {0x86, 0xd9, 0xdc, 0x3d, 0x71, 0xa9, 0x5f, 0x5a}};
const IID kIID_Controls = {0x99fa3ff4, 0x1742, 0x42a6, {0x90, 0x2e, 0x08, 0x7d, 0x41, 0xf9, 0x65, 0xec}};
const IID kIID_Handler = {0x0557e996, 0x7b23, 0x5bae, {0xaa, 0x81, 0xea, 0x0d, 0x67, 0x11, 0x43, 0xa4}};
const IID kIID_Agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using CreateStringFn = HRESULT(WINAPI*)(PCWSTR, UINT32, HSTRING*);
using DeleteStringFn = HRESULT(WINAPI*)(HSTRING);

template <class T> void Rel(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

HWND g_host;
HMODULE g_combase;
CreateStringFn g_makeString;
DeleteStringFn g_freeString;
IControls* g_controls;
EventRegistrationToken g_token{};
std::wstring g_shown;  // what the media card was last told (skips repeats)

struct Handler : IButtonHandler {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        *p = IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIID_Agile) || IsEqualIID(riid, kIID_Handler) ? this : nullptr;
        return *p ? S_OK : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE Invoke(IInspectable*, IButtonArgs* args) override {
        int b;
        if (args && SUCCEEDED(args->get_Button(&b))) PostMessageW(g_host, WM_VBG_MEDIA_KEY, (WPARAM)b, 0);
        return S_OK;
    }
} g_handler;

HSTRING MakeString(const std::wstring& s) {
    HSTRING h = nullptr;
    if (g_makeString) g_makeString(s.c_str(), (UINT32)s.size(), &h);
    return h;
}

std::wstring SongName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring n = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = n.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? n : n.substr(0, dot);
}

}  // namespace

void Media_Start(HWND host) {
    if (g_controls) return;
    g_host = host;
    g_combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto getFactory = g_combase ? (RoGetActivationFactoryFn)(void*)GetProcAddress(g_combase, "RoGetActivationFactory") : nullptr;
    g_makeString = g_combase ? (CreateStringFn)(void*)GetProcAddress(g_combase, "WindowsCreateString") : nullptr;
    g_freeString = g_combase ? (DeleteStringFn)(void*)GetProcAddress(g_combase, "WindowsDeleteString") : nullptr;
    IControlsInterop* interop = nullptr;
    HSTRING cls = MakeString(L"Windows.Media.SystemMediaTransportControls");
    HRESULT hr = getFactory && cls ? getFactory(cls, kIID_Interop, (void**)&interop) : E_NOINTERFACE;
    if (cls) g_freeString(cls);
    if (SUCCEEDED(hr)) hr = interop->GetForWindow(host, kIID_Controls, (void**)&g_controls);
    Rel(interop);
    if (SUCCEEDED(hr)) {
        g_controls->put_IsPlayEnabled(true);
        g_controls->put_IsPauseEnabled(true);
        g_controls->put_IsNextEnabled(true);
        g_controls->put_IsPreviousEnabled(true);
        hr = g_controls->add_ButtonPressed(&g_handler, &g_token);
    }
    if (FAILED(hr)) {
        Log(L"media keys: Windows' media controls aren't available (0x%08lx)", (unsigned long)hr);
        Media_Stop();
        return;
    }
    Media_Update();
}

// My music's state on the media card: there while My music has songs, with the song and whether it plays.
void Media_Update() {
    if (!g_controls) return;
    const std::vector<std::wstring> songs = g_settings.sound == 2 ? Host_Songs() : std::vector<std::wstring>{};
    const MusicState ms = Host_MusicState();
    const int n = (int)songs.size();
    const int track = ms.track >= 0 && ms.track < n ? ms.track : g_settings.musicTrack >= 0 && g_settings.musicTrack < n ? g_settings.musicTrack : 0;
    const bool on = n > 0, playing = on && g_settings.musicPlaying;
    const std::wstring title = on ? SongName(songs[track]) : L"";
    const std::wstring key = std::to_wstring(on) + L"|" + std::to_wstring(playing) + L"|" + title;
    if (key == g_shown) return;
    g_shown = key;
    g_controls->put_IsEnabled(on);
    if (!on) return;
    g_controls->put_PlaybackStatus(playing ? STATUS_PLAYING : STATUS_PAUSED);
    IDisplayUpdater* display = nullptr;
    IMusicProps* music = nullptr;
    if (SUCCEEDED(g_controls->get_DisplayUpdater(&display)) && SUCCEEDED(display->put_Type(1)) &&
        SUCCEEDED(display->get_MusicProperties(&music))) {
        HSTRING t = MakeString(title), a = MakeString(L"VideoBG");
        music->put_Title(t);
        music->put_Artist(a);
        if (t) g_freeString(t);
        if (a) g_freeString(a);
        display->Update();
    }
    Rel(music);
    Rel(display);
}

void Media_Stop() {
    if (g_controls) {
        g_controls->put_IsEnabled(false);
        if (g_token.value) g_controls->remove_ButtonPressed(g_token);
    }
    g_token = EventRegistrationToken{};
    Rel(g_controls);
    g_shown.clear();
}
