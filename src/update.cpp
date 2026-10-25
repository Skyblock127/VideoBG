// Update (the sidebar's button, or by itself on start and every 3 hours, asking first): finds the newest release on GitHub, downloads its zip, puts its
// files in place of this copy's and starts the new VideoBG. No signing is needed: a file downloaded
// here has no "from the internet" mark, so Windows doesn't stop the new exe. The running exe can't
// be overwritten but can be renamed, so it steps aside as VideoBG.old.exe (deleted by the new one).
#include "common.h"
#include <winhttp.h>
#include <shellapi.h>
#include <atomic>
#include <thread>
#include "resource.h"

namespace {

std::atomic<int> g_stage{UP_IDLE}, g_percent{0};
std::wstring g_latest;  // the newest version's number, once known
SRWLOCK g_lock = SRWLOCK_INIT;  // g_latest and g_available: written by the update thread, read by the sidebar
HWND g_notify;          // told (WM_VBG_UPDATE) whenever the stage changes

void SetStage(int stage) {
    g_stage = stage;
    PostMessageW(g_notify, WM_VBG_UPDATE, stage, 0);
}

std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, n);
    return s.substr(0, s.find_last_of(L'\\'));
}

// "2.10.1" > "2.9": numbers compared one by one.
bool Newer(const std::wstring& a, const std::wstring& b) {
    unsigned na[4] = {}, nb[4] = {};
    swscanf(a.c_str(), L"%u.%u.%u.%u", &na[0], &na[1], &na[2], &na[3]);
    swscanf(b.c_str(), L"%u.%u.%u.%u", &nb[0], &nb[1], &nb[2], &nb[3]);
    for (int i = 0; i < 4; i++)
        if (na[i] != nb[i]) return na[i] > nb[i];
    return false;
}

struct Http {
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    DWORD error = 0;
    ~Http() {
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    }
    // GET an https address; follow: whether redirects are followed. False with `error` set.
    bool Get(const std::wstring& url, bool follow) {
        wchar_t host[256] = {}, path[2048] = {};
        URL_COMPONENTS uc{sizeof uc};
        uc.lpszHostName = host;
        uc.dwHostNameLength = (DWORD)std::size(host);
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = (DWORD)std::size(path);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return error = GetLastError(), false;
        ses = WinHttpOpen(L"VideoBG/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) ses = WinHttpOpen(L"VideoBG/" APP_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (ses) WinHttpSetTimeouts(ses, 10000, 10000, 20000, 30000);
        con = ses ? WinHttpConnect(ses, host, uc.nPort, 0) : nullptr;
        req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                  : nullptr;
        if (req && !follow) {
            DWORD off = WINHTTP_DISABLE_REDIRECTS;
            WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
        }
        if (!req || !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr))
            return error = GetLastError(), false;
        return true;
    }
    DWORD Status() {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        return status;
    }
    std::wstring Header(DWORD which) {
        wchar_t buf[2048] = {};
        DWORD size = sizeof buf;
        if (!WinHttpQueryHeaders(req, which, WINHTTP_HEADER_NAME_BY_INDEX, buf, &size, WINHTTP_NO_HEADER_INDEX)) return L"";
        return buf;
    }
    bool Offline() const {
        return error == ERROR_WINHTTP_NAME_NOT_RESOLVED || error == ERROR_WINHTTP_CANNOT_CONNECT || error == ERROR_WINHTTP_TIMEOUT ||
               error == ERROR_WINHTTP_CONNECTION_ERROR;
    }
};

// Runs a program hidden and waits for it; true if it ended with 0.
bool RunHidden(std::wstring cmd) {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// The zip's VideoBG folder over this copy's folder. In the project (where VideoBG runs from a
// build), only the exe: the zip's other files don't belong there.
bool PutInPlace(const std::wstring& from, const std::wstring& to) {
    const std::wstring exe = to + L"\\VideoBG.exe", old = to + L"\\VideoBG.old.exe";
    const bool project = GetFileAttributesW((to + L"\\build.ps1").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Log(L"update: couldn't move the running exe aside, error %lu", GetLastError());
        return false;
    }
    if (!CopyFileW((from + L"\\VideoBG.exe").c_str(), exe.c_str(), FALSE)) {
        Log(L"update: couldn't copy the new exe, error %lu", GetLastError());
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        return false;
    }
    if (project) return true;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return true;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || !_wcsicmp(fd.cFileName, L"VideoBG.exe")) continue;
        if (!CopyFileW((from + L"\\" + fd.cFileName).c_str(), (to + L"\\" + fd.cFileName).c_str(), FALSE))
            Log(L"update: couldn't copy %ls, error %lu", fd.cFileName, GetLastError());  // not needed to run
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return true;
}

// What's new in a release: its notes on GitHub, as plain text (the install steps under them left out).
std::wstring ReleaseNotes(const std::wstring& tag) {
    Http h;
    std::wstring api = APP_REPO L"/releases/tags/v" + tag;
    api.replace(0, wcslen(L"https://github.com/"), L"https://api.github.com/repos/");
    if (!h.Get(api, true) || h.Status() != 200) return L"";
    std::string json;
    char buf[8192];
    DWORD got = 0;
    while (json.size() < 1 << 20 && WinHttpReadData(h.req, buf, sizeof buf, &got) && got) json.append(buf, got);
    size_t at = json.find("\"body\":\"");
    if (at == std::string::npos) return L"";
    std::string body;  // the JSON string, unescaped (UTF-8)
    for (size_t i = at + 8; i < json.size() && json[i] != '"'; i++) {
        if (json[i] != '\\' || i + 1 >= json.size()) { body += json[i]; continue; }
        const char e = json[++i];
        if (e == 'n') body += '\n';
        else if (e == 'u' && i + 4 < json.size()) {
            wchar_t c = (wchar_t)strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16);
            char u8[8];
            const int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, u8, sizeof u8, nullptr, nullptr);
            body.append(u8, n > 0 ? n : 0);
            i += 4;
        } else if (e != 'r') body += e == 't' ? ' ' : e;
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), nullptr, 0);
    std::wstring md(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), md.data(), n);
    // Markdown to plain text: no headings or bold, a dot for each point.
    std::wstring out, line;
    md += L'\n';
    for (wchar_t c : md) {
        if (c != L'\n') { line += c; continue; }
        if (line.rfind(L"### Install", 0) == 0) break;
        if (!line.empty() && line[0] != L'#') {
            for (size_t b; (b = line.find(L"**")) != std::wstring::npos;) line.erase(b, 2);
            if (line.rfind(L"- ", 0) == 0) line.replace(0, 2, L"\u2022 ");
            out += line + L"\n";
        }
        line.clear();
    }
    while (!out.empty() && out.back() == L'\n') out.pop_back();
    if (out.size() > 1500) out = out.substr(0, 1500) + L"\u2026";
    return out;
}

// A question or a note with VideoBG's icon (no sound), over everything.
int Ask(const std::wstring& text, UINT buttons) {
    MSGBOXPARAMSW mb{sizeof mb};
    mb.hInstance = GetModuleHandleW(nullptr);
    mb.lpszText = text.c_str();
    mb.lpszCaption = L"VideoBG";
    mb.lpszIcon = MAKEINTRESOURCEW(IDI_APP);
    mb.dwStyle = buttons | MB_USERICON | MB_TOPMOST | MB_SETFOREGROUND;
    return MessageBoxIndirectW(&mb);
}

std::atomic<bool> g_busy{false};
std::wstring g_available, g_asked;  // a newer version that's out; the one already asked about (once per run)

// quiet: the check by itself (on start, then every 3 hours): nothing shows unless there's a new version,
// and then it asks first.
void Work(bool quiet) {
    // The latest release: github.com/.../releases/latest points to .../releases/tag/v<version>.
    std::wstring tag;
    {
        Http h;
        if (!h.Get(APP_REPO L"/releases/latest", false)) {
            Log(L"update: no connection, error %lu", h.error);
            if (!quiet) SetStage(h.Offline() ? UP_OFFLINE : UP_FAILED);
            return;
        }
        const std::wstring where = h.Header(WINHTTP_QUERY_LOCATION);
        const size_t at = where.rfind(L"/tag/v");
        if (at == std::wstring::npos) {
            Log(L"update: no release found (status %lu)", h.Status());
            if (!quiet) SetStage(UP_FAILED);
            return;
        }
        tag = where.substr(at + 6);
    }
    AcquireSRWLockExclusive(&g_lock);
    g_latest = tag;
    ReleaseSRWLockExclusive(&g_lock);
    if (!Newer(tag, APP_VERSION)) {
        if (!quiet) SetStage(UP_LATEST);
        return;
    }
    if (g_available != tag) {
        AcquireSRWLockExclusive(&g_lock);
        g_available = tag;
        ReleaseSRWLockExclusive(&g_lock);
        PostMessageW(g_notify, WM_VBG_UPDATE, UP_IDLE, 0);  // the sidebar marks Update
    }
    const std::wstring notes = ReleaseNotes(tag);
    if (quiet) {
        if (g_asked == tag) return;  // "Not now" holds until VideoBG starts again
        QUERY_USER_NOTIFICATION_STATE busy;  // a full-screen game, a presentation: the next check asks
        if (SUCCEEDED(SHQueryUserNotificationState(&busy)) && busy != QUNS_ACCEPTS_NOTIFICATIONS && busy != QUNS_QUIET_TIME) return;
        g_asked = tag;
        Log(L"update: %ls is out, asking", tag.c_str());
        const std::wstring q = L"VideoBG " + tag + L" is out (you have " APP_VERSION L")." +
                               (notes.empty() ? L"" : L"\n\nWhat's new:\n" + notes) +
                               L"\n\nUpdate now? VideoBG restarts by itself, and your settings are kept.";
        if (Ask(q, MB_YESNO) != IDYES) return;
    }
    Log(L"update: %ls -> %ls", APP_VERSION, tag.c_str());

    // Download the zip.
    g_percent = 0;
    SetStage(UP_DOWNLOADING);
    const std::wstring dir = LocalDataDir() + L"\\update", zip = dir + L"\\VideoBG.zip", out = dir + L"\\files";
    RunHidden(L"cmd.exe /c rd /s /q \"" + dir + L"\"");
    CreateDirectoryW(dir.c_str(), nullptr);
    {
        Http h;
        if (!h.Get(APP_REPO L"/releases/download/v" + tag + L"/VideoBG-" + tag + L".zip", true) || h.Status() != 200) {
            Log(L"update: download failed, error %lu, status %lu", h.error, h.req ? h.Status() : 0);
            return SetStage(h.Offline() ? UP_OFFLINE : UP_FAILED);
        }
        const double total = wcstod(h.Header(WINHTTP_QUERY_CONTENT_LENGTH).c_str(), nullptr);
        HANDLE f = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return SetStage(UP_FAILED);
        char buf[65536];
        DWORD got = 0, wrote = 0;
        double done = 0;
        bool ok = true;
        while ((ok = WinHttpReadData(h.req, buf, sizeof buf, &got)) && got) {
            if (!WriteFile(f, buf, got, &wrote, nullptr)) { ok = false; break; }
            done += got;
            const int pc = total > 0 ? (int)(done * 100 / total) : 0;
            if (pc != g_percent) {
                g_percent = pc;
                PostMessageW(g_notify, WM_VBG_UPDATE, UP_DOWNLOADING, 0);
            }
        }
        CloseHandle(f);
        if (!ok || (total > 0 && done < total)) {
            Log(L"update: download cut off at %.0f of %.0f bytes", done, total);
            return SetStage(UP_FAILED);
        }
    }

    // Unpack it (Windows' own tar reads zips) and swap the files.
    SetStage(UP_INSTALLING);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    CreateDirectoryW(out.c_str(), nullptr);
    if (!RunHidden(L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + out + L"\"") ||
        GetFileAttributesW((out + L"\\VideoBG\\VideoBG.exe").c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(L"update: couldn't unpack the zip");
        return SetStage(UP_FAILED);
    }
    if (!PutInPlace(out + L"\\VideoBG", ExeDir())) return SetStage(UP_FAILED);
    // The new version shows what's new once it's up.
    const std::wstring note = tag + L"\n" + notes;
    HANDLE f = CreateFileW((LocalDataDir() + L"\\whatsnew.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(f, note.data(), (DWORD)(note.size() * sizeof(wchar_t)), &wrote, nullptr);
        CloseHandle(f);
    }
    Log(L"update: %ls is in place, restarting", tag.c_str());
    SetStage(UP_RESTART);
}

void Run(bool quiet) {
    Work(quiet);
    g_busy = false;
}

}  // namespace

void Update_Start(HWND notify, bool quiet) {
    if (g_busy.exchange(true)) return;  // already checking (or asking, or updating)
    g_notify = notify;
    if (!quiet) g_stage = UP_CHECKING;
    std::thread(Run, quiet).detach();
}

int Update_Stage(int* percent, std::wstring* latest) {
    if (percent) *percent = g_percent;
    if (latest && g_stage >= UP_LATEST) {
        AcquireSRWLockShared(&g_lock);
        *latest = g_latest;
        ReleaseSRWLockShared(&g_lock);
    }
    return g_stage;
}

bool Update_Available() {
    AcquireSRWLockShared(&g_lock);
    const bool out = !g_available.empty();
    ReleaseSRWLockShared(&g_lock);
    return out && !g_busy;
}

void Update_Done() {
    if (g_stage != UP_RESTART) g_stage = UP_IDLE;
}

// The new VideoBG, once this one has closed (its mutex is free).
void Update_Relaunch(bool wallpaperOn) {
    std::wstring cmd = L"\"" + ExeDir() + L"\\VideoBG.exe\" --updated" + (wallpaperOn ? L" --on" : L"");
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// After an update: what's new shows, and the old exe goes once its processes have ended, and so do
// the downloaded files.
void Update_CleanUp() {
    std::thread([] {
        const std::wstring old = ExeDir() + L"\\VideoBG.old.exe";
        for (int i = 0; i < 40 && GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(old.c_str()); i++) Sleep(250);
        RunHidden(L"cmd.exe /c rd /s /q \"" + LocalDataDir() + L"\\update\"");
    }).detach();
    const std::wstring file = LocalDataDir() + L"\\whatsnew.txt";
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::wstring note(GetFileSize(f, nullptr) / sizeof(wchar_t), L'\0');
    DWORD got = 0;
    ReadFile(f, note.data(), (DWORD)(note.size() * sizeof(wchar_t)), &got, nullptr);
    CloseHandle(f);
    DeleteFileW(file.c_str());
    const size_t nl = note.find(L'\n');
    if (nl == std::wstring::npos || note.substr(0, nl) != APP_VERSION) return;  // not for this version
    const std::wstring notes = note.substr(nl + 1);
    std::thread([notes] {
        Ask(L"VideoBG is updated to " APP_VERSION L"." + (notes.empty() ? L"" : L"\n\nWhat's new:\n" + notes), MB_OK);
    }).detach();
}
