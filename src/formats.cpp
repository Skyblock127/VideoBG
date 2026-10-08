// Which files VideoBG takes, and when one can't be played, why - in plain words, with the fix
// (often a free codec extension from Microsoft Store).
#include "common.h"
#include <shellapi.h>
#include <cstring>

const wchar_t kVideoPatterns[] =
    L"*.mp4;*.m4v;*.mov;*.qt;*.mkv;*.webm;*.wmv;*.asf;*.avi;*.ts;*.m2ts;*.mts;*.mpg;*.mpeg;*.3gp;*.3g2;*.gif";

namespace {

constexpr DWORD Fcc(const char* s) {
    return (DWORD)(BYTE)s[0] | ((DWORD)(BYTE)s[1] << 8) | ((DWORD)(BYTE)s[2] << 16) | ((DWORD)(BYTE)s[3] << 24);
}
constexpr DWORD kMpeg2 = 0xE06D8026;  // MFVideoFormat_MPEG2 isn't a FOURCC subtype; its Data1 is unique

// Codecs Windows decodes only with a Store extension.
struct CodecFix {
    const char* fcc;
    const wchar_t* name;
    const wchar_t* extension;
    const wchar_t* storeId;
    bool free;
};
const CodecFix kFixes[] = {
    {"AV01", L"AV1", L"AV1 Video Extension", L"9MVZQVXJBQ9V", true},
    {"HEVC", L"HEVC (H.265)", L"HEVC Video Extensions", L"9NMZLZ57R3T7", false},
    {"HEVS", L"HEVC (H.265)", L"HEVC Video Extensions", L"9NMZLZ57R3T7", false},
    {"H265", L"HEVC (H.265)", L"HEVC Video Extensions", L"9NMZLZ57R3T7", false},
    {"VP90", L"VP9", L"VP9 Video Extensions", L"9N4D0MSMP0PT", true},
    {"VP80", L"VP8", L"VP9 Video Extensions", L"9N4D0MSMP0PT", true},  // its decoder ships in the VP9 package
    {"MPG1", L"MPEG-1", L"MPEG-2 Video Extension", L"9N95Q1ZZPMH4", true},
    {"theo", L"Theora", L"Web Media Extensions", L"9N5TDP8VCMHS", true},
};
const CodecFix kMpeg2Fix = {"", L"MPEG-2", L"MPEG-2 Video Extension", L"9N95Q1ZZPMH4", true};

// Names for the codecs people run into. Media Foundation names most by FOURCC; unknown ones in MP4
// files come through with the bytes the other way round, so both orders match.
struct CodecName {
    const char* fcc;
    const wchar_t* name;
};
const CodecName kNames[] = {
    {"H264", L"H.264"}, {"avc1", L"H.264"}, {"AVC1", L"H.264"}, {"HEVC", L"HEVC"}, {"H265", L"HEVC"}, {"hvc1", L"HEVC"},
    {"hev1", L"HEVC"}, {"HEVS", L"HEVC"}, {"AV01", L"AV1"}, {"av01", L"AV1"}, {"VP90", L"VP9"}, {"vp09", L"VP9"},
    {"VP80", L"VP8"}, {"vp08", L"VP8"}, {"WMV1", L"WMV"}, {"WMV2", L"WMV"}, {"WMV3", L"WMV"}, {"WVC1", L"VC-1"},
    {"MP4V", L"MPEG-4"}, {"mp4v", L"MPEG-4"}, {"XVID", L"Xvid"}, {"DIVX", L"DivX"}, {"DX50", L"DivX"},
    {"MPG1", L"MPEG-1"}, {"MJPG", L"MJPEG"}, {"theo", L"Theora"}, {"H263", L"H.263"}, {"s263", L"H.263"},
    {"apcn", L"ProRes"}, {"apch", L"ProRes"}, {"apcs", L"ProRes"}, {"apco", L"ProRes"}, {"ap4h", L"ProRes"},
    {"ap4x", L"ProRes"}, {"vvc1", L"H.266 (VVC)"}, {"vvi1", L"H.266 (VVC)"}, {"dvh1", L"Dolby Vision"},
    {"dvhe", L"Dolby Vision"}, {"mjp2", L"JPEG 2000"}, {"cvid", L"Cinepak"}, {"SVQ3", L"Sorenson"}, {"FFV1", L"FFV1"},
};

bool Matches(DWORD codec, const char* fcc) {
    DWORD le = Fcc(fcc), be = _byteswap_ulong(le);
    return codec == le || codec == be;
}

const CodecFix* FixFor(DWORD codec) {
    if (codec == kMpeg2) return &kMpeg2Fix;
    for (const CodecFix& f : kFixes)
        if (Matches(codec, f.fcc)) return &f;
    return nullptr;
}

std::wstring Hex(HRESULT hr) {
    wchar_t b[16];
    swprintf(b, 16, L"0x%08lX", (unsigned long)hr);
    return b;
}

const wchar_t kConvertFix[] = L"Convert it to MP4 (H.264), for example with the free HandBrake";
const wchar_t kHandBrake[] = L"https://handbrake.fr/";

}  // namespace

std::wstring CodecLabel(DWORD codec) {
    if (codec == kMpeg2) return L"MPEG-2";
    for (const CodecName& n : kNames)
        if (Matches(codec, n.fcc)) return n.name;
    // Unknown: show its FOURCC as an MP4 file has it (most significant byte first).
    wchar_t b[5] = {};
    for (int i = 0; i < 4; i++) {
        wchar_t c = (wchar_t)((codec >> (8 * (3 - i))) & 0xFF);
        if (c < 32 || c >= 127) return L"an unusual codec";
        b[i] = c;
    }
    return b;
}

VideoFault SniffFile(const std::wstring& path, std::wstring* kind) {
    if (kind) kind->clear();
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (path.empty()) return VF_MISSING;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        DWORD e = GetLastError();
        bool gone = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_DRIVE ||
                    e == ERROR_INVALID_NAME || e == ERROR_BAD_NETPATH || e == ERROR_BAD_NET_NAME || e == ERROR_NOT_READY;
        return gone ? VF_MISSING : VF_UNREADABLE;
    }
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return VF_NOT_VIDEO;
    if (!fa.nFileSizeHigh && !fa.nFileSizeLow) return VF_EMPTY;
    // A browser's or downloader's file for a download that hasn't finished.
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        std::wstring ext = path.substr(dot);
        for (const wchar_t* p : {L".crdownload", L".part", L".partial", L".download", L".opdownload"})
            if (_wcsicmp(ext.c_str(), p) == 0) return VF_EMPTY;
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return VF_UNREADABLE;
    BYTE b[16] = {};
    DWORD n = 0;
    BOOL ok = ReadFile(f, b, sizeof b, &n, nullptr);
    CloseHandle(f);
    if (!ok) return VF_UNREADABLE;  // e.g. a cloud file that can't be downloaded right now

    auto is = [&](std::initializer_list<int> magic, DWORD at = 0) {
        if (n < at + magic.size()) return false;
        DWORD i = at;
        for (int m : magic)
            if (b[i++] != (BYTE)m) return false;
        return true;
    };
    auto say = [&](const wchar_t* k, VideoFault v) {
        if (kind) *kind = k;
        return v;
    };
    if (is({'G', 'I', 'F', '8'})) return say(L"GIF", VF_GIF);
    if (is({0x89, 'P', 'N', 'G'})) return say(L"PNG", VF_PICTURE);
    if (is({0xFF, 0xD8, 0xFF})) return say(L"JPEG", VF_PICTURE);
    if (is({'R', 'I', 'F', 'F'}) && is({'W', 'E', 'B', 'P'}, 8)) return say(L"WebP", VF_PICTURE);
    if (is({'f', 't', 'y', 'p'}, 4) && (is({'h', 'e', 'i', 'c'}, 8) || is({'m', 'i', 'f', '1'}, 8))) return say(L"HEIC", VF_PICTURE);
    if (is({'f', 't', 'y', 'p'}, 4) && is({'a', 'v', 'i', 'f'}, 8)) return say(L"AVIF", VF_PICTURE);
    if (is({'B', 'M'}) && n >= 14 && !b[6] && !b[7] && !b[8] && !b[9]) return say(L"BMP", VF_PICTURE);
    if (is({'F', 'L', 'V', 1})) return say(L"FLV", VF_CONTAINER);
    if (is({'.', 'R', 'M', 'F'})) return say(L"RealMedia", VF_CONTAINER);
    if (is({'O', 'g', 'g', 'S'})) return say(L"Ogg", VF_CONTAINER);
    if (is({'P', 'K', 3, 4}) || is({'R', 'a', 'r', '!'}) || is({'7', 'z', 0xBC, 0xAF})) return say(L"archive", VF_NOT_VIDEO);
    if (is({'<', '!'}) || is({'<', 'h'}) || is({'<', 'H'}) || is({0xEF, 0xBB, 0xBF, '<'})) return say(L"web page", VF_NOT_VIDEO);
    if (is({'%', 'P', 'D', 'F'})) return say(L"PDF", VF_NOT_VIDEO);
    if (is({'M', 'Z'})) return say(L"program", VF_NOT_VIDEO);
    return VF_NONE;
}

FaultText DescribeFault(int fault, DWORD codec, HRESULT hr, const std::wstring& path) {
    FaultText t;
    std::wstring kind;
    if (fault == VF_PICTURE || fault == VF_NOT_VIDEO || fault == VF_CONTAINER) SniffFile(path, &kind);
    switch (fault) {
        case VF_MISSING:
            t.what = L"The video file isn't there any more";
            t.fix = L"It was moved, renamed or deleted: choose it again";
            break;
        case VF_UNREADABLE:
            t.what = L"Windows won't let VideoBG read this file";
            t.fix = L"Close the app using it, or copy the video into your Videos folder";
            break;
        case VF_EMPTY:
            t.what = L"This file is empty or hasn't finished downloading";
            t.fix = L"Let the download finish, then choose the video again";
            break;
        case VF_GIF:
            t.what = L"This is an animated GIF, not a video";
            t.fix = L"VideoBG can turn it into a video in a few seconds";
            t.linkText = L"Convert";
            t.link = L"gif:";
            break;
        case VF_PICTURE:
            t.what = L"This is a picture (" + kind + L"), not a video";
            t.fix = L"Choose a video file, like an MP4";
            break;
        case VF_NOT_VIDEO:
            if (kind == L"archive") {
                t.what = L"This is a zip or other archive, not a video";
                t.fix = L"Extract the video from it first, then choose that";
            } else if (kind == L"web page") {
                t.what = L"This file is a web page, not a video";
                t.fix = L"The download didn't get the video itself: download it again";
            } else {
                t.what = kind.empty() ? L"Windows doesn't recognise this file as a video" : L"This is a " + kind + L" file, not a video";
                t.fix = L"Choose a video file, like an MP4";
            }
            break;
        case VF_CONTAINER:
            if (kind == L"Ogg") {
                t.what = L"Windows can't open Ogg videos yet";
                t.fix = L"Install the free Web Media Extensions from Microsoft Store, then come back";
                t.linkText = L"Get it";
                t.link = L"store:9N5TDP8VCMHS";
            } else {
                t.what = L"Windows can't play " + (kind.empty() ? std::wstring(L"this kind of") : kind) + L" files";
                t.fix = kConvertFix;
                t.linkText = L"HandBrake";
                t.link = kHandBrake;
            }
            break;
        case VF_SOUND_ONLY:
            t.what = L"This file has only sound, no picture";
            t.fix = L"Choose a video file (for music, use the Sound page)";
            break;
        case VF_NO_DECODER:
            if (const CodecFix* f = FixFor(codec)) {
                t.what = std::wstring(L"This video is ") + f->name + L", which this PC can't play yet";
                t.fix = f->free ? std::wstring(L"Install the free ") + f->extension + L" from Microsoft Store, then come back"
                                : std::wstring(L"Get ") + f->extension + L" from Microsoft Store (small fee), then come back";
                t.linkText = f->free ? L"Get it free" : L"Open Store";
                t.link = std::wstring(L"store:") + f->storeId;
            } else {
                t.what = L"This video is " + CodecLabel(codec) + L", which Windows can't play";
                t.fix = kConvertFix;
                t.linkText = L"HandBrake";
                t.link = kHandBrake;
            }
            break;
        case VF_NO_MEDIA:
            t.what = L"Windows' media features are missing on this PC";
            t.fix = L"Add the Media Feature Pack in Settings > Optional features, then restart";
            t.linkText = L"Open Settings";
            t.link = L"ms-settings:optionalfeatures";
            break;
        case VF_GPU:
            t.what = L"VideoBG couldn't use the graphics card (error " + Hex(hr) + L")";
            t.fix = L"Restart the PC or update the graphics driver, then turn the wallpaper on";
            break;
        default:
            t.what = L"Windows couldn't play this video (error " + Hex(hr) + L")";
            t.fix = kConvertFix;
            t.linkText = L"HandBrake";
            t.link = kHandBrake;
            break;
    }
    return t;
}

void OpenFixLink(HWND owner, const std::wstring& link) {
    std::wstring target = link;
    if (link.rfind(L"store:", 0) == 0) {
        std::wstring id = link.substr(6);
        std::wstring app = L"ms-windows-store://pdp/?ProductId=" + id;
        if ((INT_PTR)ShellExecuteW(owner, L"open", app.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32) return;
        target = L"https://apps.microsoft.com/detail/" + id;  // no Store app: its web page
    }
    ShellExecuteW(owner, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
