// Media Foundation / Direct3D helpers. All system media DLLs are loaded on demand so the
// always-running tray process never maps them.
#pragma once
#include "common.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdint>

struct MFApi {
    HMODULE plat = nullptr, rw = nullptr;
    int starts = 0;
    decltype(&::MFStartup) Startup = nullptr;
    decltype(&::MFShutdown) Shutdown = nullptr;
    decltype(&::MFCreateAttributes) CreateAttributes = nullptr;
    decltype(&::MFCreateMediaType) CreateMediaType = nullptr;
    decltype(&::MFCreateDXGIDeviceManager) CreateDXGIDeviceManager = nullptr;
    decltype(&::MFCreateSourceReaderFromURL) CreateSourceReaderFromURL = nullptr;
    decltype(&::MFCreateSample) CreateSample = nullptr;
    decltype(&::MFCreateMemoryBuffer) CreateMemoryBuffer = nullptr;
    decltype(&::MFTEnumEx) TEnumEx = nullptr;
    bool Start();  // load DLLs + MFStartup (ref-counted, any thread)
    void Stop();   // MFShutdown + unload when the last user is gone

private:
    bool StartLocked();
};
extern MFApi MF;

struct D3DApi {
    HMODULE d3d = nullptr, dxgi = nullptr;
    decltype(&::D3D11CreateDevice) CreateDevice = nullptr;
    decltype(&::CreateDXGIFactory1) CreateFactory1 = nullptr;
    bool Load();
    void Unload();
};
extern D3DApi D3D;

struct VideoInfo {
    bool ok = false;
    HRESULT hr = S_OK;
    bool opened = false;   // Media Foundation could open the file (it may still lack a decoder)
    GUID subtype{};        // the video stream's codec (GUID_NULL: no video stream)
    UINT32 w = 0, h = 0;   // display size
    double aspect = 0;     // display aspect ratio (includes pixel aspect)
    double duration = 0;   // seconds
    double fps = 0;
    std::wstring codec;
    std::vector<DWORD> audioStreams;  // stream indices of audio tracks
};

// Opens the file just long enough to read its headers. MF must be started.
bool ProbeVideo(const std::wstring& path, VideoInfo& vi);

// Why a file that failed to open or decode can't be played (a VideoFault); vi is what the attempt found.
VideoFault DiagnoseVideo(const std::wstring& path, const VideoInfo& vi);

// Decodes single frames to 32-bit BGRX pixels. Keeps the reader open for fast scrubbing.
class FrameGrabber {
public:
    ~FrameGrabber() { Close(); }
    bool Open(const std::wstring& path);
    bool Grab(double seconds, std::vector<uint32_t>& px, int& w, int& h);
    void Close();
    const std::wstring& Path() const { return path_; }
    bool IsOpen() const { return rd_ != nullptr; }
    VideoInfo info;

private:
    bool ReadFormat();
    IMFSourceReader* rd_ = nullptr;
    std::wstring path_;
    UINT32 fw_ = 0, fh_ = 0;
    LONG stride_ = 0;
    RECT ap_{};
};

// Halves the image until it fits in maxDim (cheap box filter).
void ShrinkPixels(std::vector<uint32_t>& px, int& w, int& h, int maxDim);

// The adapter that drives the primary monitor; on hybrid laptops this keeps the discrete GPU asleep.
IDXGIAdapter1* PickDisplayAdapter(IDXGIFactory1* f, std::wstring* name);

template <class T> inline void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}
