#include "common.h"
#include <shlobj.h>
#include <cwchar>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cmath>

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return std::wstring(buf, n);
}

std::wstring AppDataDir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &p))) {
        dir = std::wstring(p) + L"\\VideoBG";
        CoTaskMemFree(p);
    } else {
        dir = L".";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring LocalDataDir() {
    PWSTR p = nullptr;
    std::wstring dir = AppDataDir();
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &p))) {
        dir = std::wstring(p) + L"\\VideoBG";
        CoTaskMemFree(p);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

void Log(const wchar_t* fmt, ...) {
    static std::wstring path;
    if (path.empty()) {
        path = AppDataDir() + L"\\videobg.log";
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 256 * 1024)
            DeleteFileW(path.c_str());
    }
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vswprintf(msg, 1024, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2048];
    int len = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] ", st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId());
    len += WideCharToMultiByte(CP_UTF8, 0, msg, n, line + len, (int)sizeof line - len - 3, nullptr, nullptr);
    line[len++] = '\r';
    line[len++] = '\n';
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, line, (DWORD)len, &w, nullptr);
    CloseHandle(f);
}

static LONG WINAPI CrashLogFilter(EXCEPTION_POINTERS* ep) {
    if (!ep) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE mod = nullptr;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &mod);
    wchar_t name[MAX_PATH] = L"unknown module";
    if (mod) GetModuleFileNameW(mod, name, MAX_PATH);
    // The return address on top of the stack tells where a bad call came from.
    void* ret = ep->ContextRecord ? *(void**)ep->ContextRecord->Rsp : nullptr;
    HMODULE rmod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ret, &rmod);
    Log(L"CRASH 0x%08lx at %ls+0x%llx (called from +0x%llx), thread %lu", ep->ExceptionRecord->ExceptionCode, name,
        mod ? (unsigned long long)((char*)addr - (char*)mod) : (unsigned long long)addr,
        rmod ? (unsigned long long)((char*)ret - (char*)rmod) : 0ULL, GetCurrentThreadId());
    return EXCEPTION_CONTINUE_SEARCH;  // let Windows write its crash report / dump as well
}

void InstallCrashLog(const wchar_t* role) {
    SetUnhandledExceptionFilter(CrashLogFilter);
    Log(L"%ls started (%ls)", role, APP_VERSION);
}

static std::wstring PlaylistPath() { return AppDataDir() + L"\\music.m3u8"; }

std::vector<std::wstring> LoadPlaylist() {
    std::vector<std::wstring> out;
    HANDLE f = CreateFileW(PlaylistPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    std::string data(size, '\0');
    if (size && size < (16u << 20)) ReadFile(f, data.data(), size, &got, nullptr);
    CloseHandle(f);
    data.resize(got);
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = line.substr(3);  // BOM
        if (line.empty() || line[0] == '#') continue;
        int n = MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), w.data(), n);
        out.push_back(w);
    }
    return out;
}

void SavePlaylist(const std::vector<std::wstring>& files) {
    std::string data = "#EXTM3U\r\n";
    for (auto& w : files) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
        data += s + "\r\n";
    }
    HANDLE f = CreateFileW(PlaylistPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr);
    CloseHandle(f);
}

static std::wstring Utf16Ini(const wchar_t* name) {
    std::wstring path = AppDataDir() + L"\\" + name;
    // WritePrivateProfileString keeps the file's encoding; start it as UTF-16 so
    // non-ASCII video paths survive.
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const WORD bom = 0xFEFF;
            DWORD w;
            WriteFile(f, &bom, sizeof bom, &w, nullptr);
            CloseHandle(f);
        }
    }
    return path;
}

static std::wstring IniPath() { return Utf16Ini(L"settings.ini"); }

static std::wstring CropKey(const std::wstring& video) {
    // FNV-1a over the lower-cased path: a stable INI key that avoids '=' / ']' in paths.
    unsigned long long h = 1469598103934665603ULL;
    for (wchar_t c : video) {
        h ^= (unsigned long long)towlower(c);
        h *= 1099511628211ULL;
    }
    wchar_t buf[24];
    swprintf(buf, 24, L"%016llx", h);
    return buf;
}

static int ReadInt(const wchar_t* ini, const wchar_t* key, int def, int lo, int hi) {
    int v = (int)GetPrivateProfileIntW(L"General", key, def, ini);
    return v < lo ? lo : (v > hi ? hi : v);
}

static std::wstring ReadStr(const wchar_t* ini, const wchar_t* sec, const wchar_t* key) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(sec, key, L"", buf, (DWORD)std::size(buf), ini);
    return buf;
}

static void WriteInt(const wchar_t* ini, const wchar_t* key, int v) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", v);
    WritePrivateProfileStringW(L"General", key, buf, ini);
}

std::wstring ReadState(const wchar_t* section, const std::wstring& key) {
    return ReadStr(IniPath().c_str(), section, key.c_str());
}

void WriteState(const wchar_t* section, const std::wstring& key, const wchar_t* value) {
    std::wstring ini = IniPath();
    WritePrivateProfileStringW(section, key.c_str(), value, ini.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
}

std::wstring PathKey(const std::wstring& path) { return CropKey(path); }

// ---------------------------------------------------------------------------------------
// Per-video data: videos.ini, one section per video content fingerprint:
//   Path=       where the file was last seen (to find a lighter version's original)
//   CopyOf=     for a light copy: the original's id, whose section holds the shared data
//   Crop=       l,t,r,b
//   PreviewTime= seconds: the frame for previews and the lock screen (absent: the default frame)
//   ClockOwn=1, ClockX=, ClockY=, ClockColor=, ClockSize=, ...   the video's own desktop clock look

static std::wstring VideosIni() { return Utf16Ini(L"videos.ini"); }

static std::wstring VRead(const std::wstring& id, const wchar_t* key) {
    return ReadStr(VideosIni().c_str(), id.c_str(), key);
}

static void VWrite(const std::wstring& id, const wchar_t* key, const wchar_t* value) {
    WritePrivateProfileStringW(id.c_str(), key, value, VideosIni().c_str());
}

std::wstring VideoId(const std::wstring& path) {
    struct Entry {
        std::wstring path, id;
        ULONGLONG size = 0;
        FILETIME mtime{};
    };
    static Entry cache[4];
    static int next;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return L"";
    ULONGLONG size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    for (auto& e : cache)
        if (e.size == size && !e.id.empty() && _wcsicmp(e.path.c_str(), path.c_str()) == 0 &&
            CompareFileTime(&e.mtime, &fa.ftLastWriteTime) == 0)
            return e.id;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    // FNV-1a over the size and 64 KB from the start, middle and end: instant even for big files,
    // and the same wherever the file lives or whatever it's called.
    unsigned long long h = 1469598103934665603ULL;
    auto mix = [&](const BYTE* p, size_t n) {
        for (size_t i = 0; i < n; i++) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
    };
    mix((const BYTE*)&size, sizeof size);
    std::vector<BYTE> buf(64 * 1024);
    const ULONGLONG offsets[3] = {0, size / 2, size > buf.size() ? size - buf.size() : 0};
    for (ULONGLONG off : offsets) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)off;
        DWORD got = 0;
        if (SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr))
            mix(buf.data(), got);
    }
    CloseHandle(f);
    wchar_t id[48];
    swprintf(id, 48, L"%016llx-%llu", h, size);
    cache[next++ % 4] = Entry{path, id, size, fa.ftLastWriteTime};
    return id;
}

std::wstring VideoProfileId(const std::wstring& path) {
    std::wstring id = VideoId(path);
    if (id.empty()) return L"";
    std::wstring orig = VRead(id, L"CopyOf");
    if (!orig.empty()) return orig;
    // Older versions linked a light copy to its original by path, in settings.ini.
    std::wstring legacy = ReadState(L"LightCopies", PathKey(path));
    if (!legacy.empty()) {
        std::wstring oid = VideoId(legacy);
        WriteState(L"LightCopies", PathKey(path), nullptr);
        if (!oid.empty() && oid != id) {
            VWrite(id, L"CopyOf", oid.c_str());
            VWrite(oid, L"Path", legacy.c_str());
            return oid;
        }
    }
    return id;
}

void LinkLightCopy(const std::wstring& copy, const std::wstring& original) {
    std::wstring cid = VideoId(copy), oid = VideoProfileId(original);
    if (cid.empty() || oid.empty() || cid == oid) return;
    VWrite(cid, L"CopyOf", oid.c_str());
    VWrite(cid, L"Path", copy.c_str());
    if (VideoId(original) == oid) VWrite(oid, L"Path", original.c_str());
}

std::wstring LightCopyOriginalPath(const std::wstring& copy) {
    std::wstring oid = VideoProfileId(copy);
    if (oid.empty() || oid == VideoId(copy)) return L"";
    std::wstring p = VRead(oid, L"Path");
    return !p.empty() && VideoId(p) == oid ? p : L"";
}

bool IsLightCopyOf(const std::wstring& copy, const std::wstring& original) {
    std::wstring oid = VideoProfileId(original), cid = VideoId(copy);
    return !oid.empty() && !cid.empty() && cid != oid && VRead(cid, L"CopyOf") == oid;
}

static bool ParseCrop(const std::wstring& v, Crop* c) {
    float l, t, r, b;
    if (swscanf(v.c_str(), L"%f,%f,%f,%f", &l, &t, &r, &b) == 4 && l >= 0 && t >= 0 && r <= 1.0001f && b <= 1.0001f &&
        r - l > 0.01f && b - t > 0.01f) {
        *c = {l, t, r, b};
        return true;
    }
    return false;
}

Crop LoadCropFor(const std::wstring& video) {
    Crop c;
    if (video.empty()) return c;
    std::wstring pid = VideoProfileId(video);
    std::wstring v = pid.empty() ? L"" : VRead(pid, L"Crop");
    if (v.empty()) {  // older versions kept crops by path in settings.ini
        v = ReadState(L"Crops", PathKey(video));
        if (!v.empty() && !pid.empty()) {
            VWrite(pid, L"Crop", v.c_str());
            WriteState(L"Crops", PathKey(video), nullptr);
        }
    }
    ParseCrop(v, &c);
    return c;
}

double PreviewTimeFor(double setting, double duration) {
    double t = setting >= 0 ? setting : (duration > 0 ? std::min(duration * 0.1, 3.0) : 1.0);
    return duration > 0 ? std::min(t, duration) : t;
}

static float ReadFloat(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, float def, float lo, float hi) {
    std::wstring v = ReadStr(ini, sec, key);
    if (v.empty()) return def;
    float f = wcstof(v.c_str(), nullptr);
    return f < lo ? lo : (f > hi ? hi : f);
}

static ClockLook ReadLook(const wchar_t* ini, const wchar_t* sec, const std::wstring& prefix, const ClockLook& def) {
    // Place, colour, size and time format are always written; the rest only when not the default.
    const ClockLook base;
    ClockLook l;
    l.x = ReadFloat(ini, sec, (prefix + L"X").c_str(), def.x, 0, 1);
    l.y = ReadFloat(ini, sec, (prefix + L"Y").c_str(), def.y, 0, 1);
    l.size = ReadFloat(ini, sec, (prefix + L"Size").c_str(), def.size, 0.5f, 2.5f);
    l.opacity = ReadFloat(ini, sec, (prefix + L"Opacity").c_str(), base.opacity, 0.2f, 1);
    l.glow = ReadFloat(ini, sec, (prefix + L"Glow").c_str(), base.glow, 0, 1);
    l.glowSize = ReadFloat(ini, sec, (prefix + L"GlowSize").c_str(), base.glowSize, 2, 40);
    l.color = ReadStr(ini, sec, (prefix + L"Color").c_str());
    if (l.color.empty()) l.color = def.color;
    l.glowColor = ReadStr(ini, sec, (prefix + L"GlowColor").c_str());  // "" = the text colour
    l.show = GetPrivateProfileIntW(sec, (prefix + L"Show").c_str(), 1, ini) != 0;
    l.h24 = GetPrivateProfileIntW(sec, (prefix + L"Hours").c_str(), def.h24 ? 24 : 12, ini) == 24;
    l.date = std::clamp((int)GetPrivateProfileIntW(sec, (prefix + L"Date").c_str(), 0, ini), 0, 99);
    l.font = ReadStr(ini, sec, (prefix + L"Font").c_str());
    if (l.font.empty()) l.font = base.font;
    return l;
}

static void WriteLook(const wchar_t* ini, const wchar_t* sec, const std::wstring& prefix, const ClockLook* l) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.5f", l ? l->x : 0.f);
    WritePrivateProfileStringW(sec, (prefix + L"X").c_str(), l ? buf : nullptr, ini);
    swprintf(buf, 32, L"%.5f", l ? l->y : 0.f);
    WritePrivateProfileStringW(sec, (prefix + L"Y").c_str(), l ? buf : nullptr, ini);
    WritePrivateProfileStringW(sec, (prefix + L"Color").c_str(), l ? l->color.c_str() : nullptr, ini);
    swprintf(buf, 32, L"%.3f", l ? l->size : 0.f);
    WritePrivateProfileStringW(sec, (prefix + L"Size").c_str(), l ? buf : nullptr, ini);
    // Opacity and glow are only written when they differ from the defaults, so a plain look stays short.
    swprintf(buf, 32, L"%.2f", l ? l->opacity : 1.f);
    WritePrivateProfileStringW(sec, (prefix + L"Opacity").c_str(), l && l->opacity < 1 ? buf : nullptr, ini);
    swprintf(buf, 32, L"%.2f", l ? l->glow : 0.f);
    WritePrivateProfileStringW(sec, (prefix + L"Glow").c_str(), l && l->glow > 0 ? buf : nullptr, ini);
    swprintf(buf, 32, L"%.0f", l ? l->glowSize : 0.f);
    WritePrivateProfileStringW(sec, (prefix + L"GlowSize").c_str(), l && l->glowSize != 12 ? buf : nullptr, ini);
    WritePrivateProfileStringW(sec, (prefix + L"GlowColor").c_str(), l && !l->glowColor.empty() ? l->glowColor.c_str() : nullptr, ini);
    WritePrivateProfileStringW(sec, (prefix + L"Show").c_str(), l && !l->show ? L"0" : nullptr, ini);
    WritePrivateProfileStringW(sec, (prefix + L"Hours").c_str(), l ? (l->h24 ? L"24" : L"12") : nullptr, ini);
    swprintf(buf, 32, L"%d", l ? l->date : 0);
    WritePrivateProfileStringW(sec, (prefix + L"Date").c_str(), l && l->date ? buf : nullptr, ini);
    WritePrivateProfileStringW(sec, (prefix + L"Font").c_str(), l && l->font != ClockLook{}.font ? l->font.c_str() : nullptr, ini);
}

// The colour dialog's saved colours: [Clock] MyColors=RRGGBB,,RRGGBB,... (16 boxes, empty ones blank).
static void LoadMyColors(const wchar_t* ini, Settings& s) {
    std::wstring v = ReadStr(ini, L"Clock", L"MyColors");
    size_t start = 0;
    for (int i = 0; i < 16 && start <= v.size(); i++) {
        size_t end = v.find(L',', start);
        if (end == std::wstring::npos) end = v.size();
        BYTE r, g, b;
        std::wstring item = v.substr(start, end - start);
        if (!item.empty() && ParseColor(item, &r, &g, &b)) {
            wchar_t c[16];
            swprintf(c, 16, L"%d,%d,%d", r, g, b);
            s.myColors[i] = c;
        }
        start = end + 1;
    }
}

void SaveMyColors(const Settings& s) {
    std::wstring v, ini = IniPath();
    bool any = false;
    for (int i = 0; i < 16; i++) {
        BYTE r, g, b;
        if (i) v += L',';
        if (!s.myColors[i].empty() && ParseColor(s.myColors[i], &r, &g, &b)) {
            wchar_t c[8];
            swprintf(c, 8, L"%02X%02X%02X", r, g, b);
            v += c;
            any = true;
        }
    }
    WritePrivateProfileStringW(L"Clock", L"MyColors", any ? v.c_str() : nullptr, ini.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
}

void LoadSettings(Settings& s) {
    std::wstring iniS = IniPath();
    const wchar_t* ini = iniS.c_str();
    s = Settings{};
    s.video = ReadStr(ini, L"General", L"Video");
    s.hkMods = (UINT)ReadInt(ini, L"HotkeyMods", MOD_CONTROL | MOD_ALT, 0, 0xF);
    s.hkVk = (UINT)ReadInt(ini, L"HotkeyKey", 'B', 0, 0xFE);
    if (s.hkVk == 0 || s.hkMods == 0) { s.hkMods = MOD_CONTROL | MOD_ALT; s.hkVk = 'B'; }
    s.scale = ReadInt(ini, L"Scale", 0, 0, 2);
    s.monitors = ReadInt(ini, L"Monitors", 0, 0, 1);
    s.speed = ReadInt(ini, L"Speed", 100, 25, 200);
    s.volume = ReadInt(ini, L"Volume", 60, 0, 100);
    // Older settings had no sound mode: any volume above 0 meant "the video's audio".
    s.sound = ReadInt(ini, L"Sound", -1, -1, 2);
    if (s.sound < 0) s.sound = GetPrivateProfileIntW(L"General", L"Volume", 0, ini) > 0 ? 1 : 0;
    s.shuffle = ReadInt(ini, L"Shuffle", 0, 0, 1) != 0;
    s.keepSound = ReadInt(ini, L"KeepSoundWhenPaused", 1, 0, 1) != 0;
    s.fpsCap = ReadInt(ini, L"FpsCap", 0, 0, 240);
    s.pauseCover = ReadInt(ini, L"PauseWhenCovered", 2, 0, 2);
    s.onBattery = ReadInt(ini, L"OnBattery", 0, 0, 2);
    s.launch = ReadInt(ini, L"LaunchState", 1, 0, 2);
    s.lastOn = ReadInt(ini, L"LastOn", 1, 0, 1) != 0;
    s.pipeline = ReadInt(ini, L"Pipeline", 0, 0, 2);
    s.lockFollow = ReadInt(ini, L"LockScreenFollows", 1, 0, 1) != 0;
    s.clockStill.size = s.clockLive.size = ReadFloat(ini, L"Clock", L"Size", 1.06f, 0.5f, 2.5f);  // older: one size for all
    // Older versions had one 12/24-hour setting for every look: it's where each look starts from.
    s.clockStill.h24 = s.clockLive.h24 = GetPrivateProfileIntW(L"Clock", L"Hours", 12, ini) == 24;
    s.clockStill = ReadLook(ini, L"Clock", L"Still", s.clockStill);
    s.clockLive = ReadLook(ini, L"Clock", L"Live", s.clockLive);
    LoadMyColors(ini, s);
    LoadVideoProfile(s);
    // Older versions had one switch for the clock: off there means off with every look.
    if (!GetPrivateProfileIntW(L"Clock", L"Enabled", 1, ini)) s.clockStill.show = s.clockLive.show = s.clockVideo.show = false;
    // Older versions had one preview frame for every video: it becomes the current video's.
    std::wstring pt = ReadStr(ini, L"General", L"PreviewTime");
    if (s.previewTime < 0 && !pt.empty()) s.previewTime = wcstod(pt.c_str(), nullptr);
}

void LoadVideoProfile(Settings& s) {
    s.crop = LoadCropFor(s.video);
    s.previewTime = -1;
    s.clockVideoOwn = s.clockVideoSaved = false;
    s.clockVideo = s.clockLive;
    std::wstring pid = VideoProfileId(s.video);
    if (pid.empty()) return;
    std::wstring vini = VideosIni();
    std::wstring pt = ReadStr(vini.c_str(), pid.c_str(), L"PreviewTime");
    if (!pt.empty()) s.previewTime = std::max(0.0, wcstod(pt.c_str(), nullptr));
    s.clockVideoOwn = GetPrivateProfileIntW(pid.c_str(), L"ClockOwn", 0, vini.c_str()) != 0;
    // Its own look stays stored while switched off, so switching it back on brings it back.
    s.clockVideoSaved = !ReadStr(vini.c_str(), pid.c_str(), L"ClockColor").empty();
    if (s.clockVideoSaved) s.clockVideo = ReadLook(vini.c_str(), pid.c_str(), L"Clock", s.clockLive);
}

void SaveSettings(const Settings& s) {
    std::wstring iniS = IniPath();
    const wchar_t* ini = iniS.c_str();
    WritePrivateProfileStringW(L"General", L"Video", s.video.c_str(), ini);
    WriteInt(ini, L"HotkeyMods", (int)s.hkMods);
    WriteInt(ini, L"HotkeyKey", (int)s.hkVk);
    WriteInt(ini, L"Scale", s.scale);
    WriteInt(ini, L"Monitors", s.monitors);
    WriteInt(ini, L"Speed", s.speed);
    WriteInt(ini, L"Volume", s.volume);
    WriteInt(ini, L"Sound", s.sound);
    WriteInt(ini, L"Shuffle", s.shuffle ? 1 : 0);
    WriteInt(ini, L"KeepSoundWhenPaused", s.keepSound ? 1 : 0);
    WriteInt(ini, L"FpsCap", s.fpsCap);
    WriteInt(ini, L"PauseWhenCovered", s.pauseCover);
    WriteInt(ini, L"OnBattery", s.onBattery);
    WriteInt(ini, L"LaunchState", s.launch);
    WriteInt(ini, L"LastOn", s.lastOn ? 1 : 0);
    WriteInt(ini, L"LockScreenFollows", s.lockFollow ? 1 : 0);
    WritePrivateProfileStringW(L"General", L"PreviewTime", nullptr, ini);  // per video now
    wchar_t buf[64];

    for (const wchar_t* old : {L"Skin", L"Variable", L"RainmeterExe", L"Size", L"Enabled", L"Hours"})  // from older versions
        WritePrivateProfileStringW(L"Clock", old, nullptr, ini);
    WriteLook(ini, L"Clock", L"Still", &s.clockStill);
    WriteLook(ini, L"Clock", L"Live", &s.clockLive);
    SaveMyColors(s);  // also flushes the cache

    std::wstring id = VideoId(s.video), pid = VideoProfileId(s.video);
    if (!id.empty() && !pid.empty()) {
        std::wstring vini = VideosIni();
        WritePrivateProfileStringW(id.c_str(), L"Path", s.video.c_str(), vini.c_str());
        swprintf(buf, 64, L"%.5f,%.5f,%.5f,%.5f", s.crop.l, s.crop.t, s.crop.r, s.crop.b);
        WritePrivateProfileStringW(pid.c_str(), L"Crop", s.crop.IsFull() ? nullptr : buf, vini.c_str());
        swprintf(buf, 64, L"%.3f", s.previewTime);
        WritePrivateProfileStringW(pid.c_str(), L"PreviewTime", s.previewTime >= 0 ? buf : nullptr, vini.c_str());
        WritePrivateProfileStringW(pid.c_str(), L"ClockOwn", s.clockVideoOwn ? L"1" : nullptr, vini.c_str());
        if (s.clockVideoOwn) WriteLook(vini.c_str(), pid.c_str(), L"Clock", &s.clockVideo);
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, vini.c_str());
    }
}

std::wstring HotkeyToString(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_WIN) s += L"Win + ";
    if (mods & MOD_CONTROL) s += L"Ctrl + ";
    if (mods & MOD_ALT) s += L"Alt + ";
    if (mods & MOD_SHIFT) s += L"Shift + ";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        s += (wchar_t)vk;
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        wchar_t b[8];
        swprintf(b, 8, L"F%u", vk - VK_F1 + 1);
        s += b;
    } else {
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        LONG lp = (LONG)(sc << 16);
        switch (vk) {  // keys that need the "extended" bit for a proper name
            case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
                lp |= 1 << 24;
                break;
        }
        wchar_t name[64] = {};
        if (GetKeyNameTextW(lp, name, 64) > 0) s += name;
        else {
            wchar_t b[16];
            swprintf(b, 16, L"Key %u", vk);
            s += b;
        }
    }
    return s;
}

LPARAM PackCrop(const Crop& c) {
    auto q = [](float v) -> unsigned long long {
        long x = lroundf(v * 65535.0f);
        return (unsigned long long)(x < 0 ? 0 : (x > 65535 ? 65535 : x));
    };
    return (LPARAM)(q(c.l) | (q(c.t) << 16) | (q(c.r) << 32) | (q(c.b) << 48));
}

Crop UnpackCrop(LPARAM p) {
    unsigned long long v = (unsigned long long)p;
    Crop c;
    c.l = (float)(v & 0xFFFF) / 65535.0f;
    c.t = (float)((v >> 16) & 0xFFFF) / 65535.0f;
    c.r = (float)((v >> 32) & 0xFFFF) / 65535.0f;
    c.b = (float)((v >> 48) & 0xFFFF) / 65535.0f;
    return c;
}

FitRect ComputeFit(double va, const Crop& c, int mode, double tw, double th) {
    FitRect f{c.l, c.t, c.r, c.b, 0, 0, (float)tw, (float)th};
    if (va <= 0 || tw <= 0 || th <= 0) return f;
    double cw = (c.r - c.l) * va, ch = (c.b - c.t);  // crop size in "video height = 1" units
    if (cw <= 0 || ch <= 0) return f;
    double ca = cw / ch, ta = tw / th;
    if (mode == 0) {  // fill: trim the crop further so it covers the target exactly
        if (ca > ta) {
            double nw = (c.r - c.l) * (ta / ca), cx = (c.l + c.r) / 2;
            f.sl = (float)(cx - nw / 2);
            f.sr = (float)(cx + nw / 2);
        } else if (ca < ta) {
            double nh = (c.b - c.t) * (ca / ta), cy = (c.t + c.b) / 2;
            f.st = (float)(cy - nh / 2);
            f.sb = (float)(cy + nh / 2);
        }
    } else if (mode == 1) {  // fit: letterbox inside the target
        if (ca > ta) {
            double h = tw / ca;
            f.dt = (float)((th - h) / 2);
            f.db = (float)(f.dt + h);
        } else {
            double w = th * ca;
            f.dl = (float)((tw - w) / 2);
            f.dr = (float)(f.dl + w);
        }
    }
    return f;
}
