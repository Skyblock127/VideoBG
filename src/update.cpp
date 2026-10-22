// Update from the General sidebar: finds the newest release on GitHub, downloads its zip, puts its
// files in place of this copy's and starts the new VideoBG. No signing is needed: a file downloaded
// here has no "from the internet" mark, so Windows doesn't stop the new exe. The running exe can't
// be overwritten but can be renamed, so it steps aside as VideoBG.old.exe (deleted by the new one).
#include "common.h"
#include <winhttp.h>
#include <atomic>
#include <thread>

namespace {

std::atomic<int> g_stage{UP_IDLE}, g_percent{0};
std::wstring g_latest;  // the newest version's number, once known
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

void Work() {
    // The latest release: github.com/.../releases/latest points to .../releases/tag/v<version>.
    std::wstring tag;
    {
        Http h;
        if (!h.Get(APP_REPO L"/releases/latest", false)) {
            Log(L"update: no connection, error %lu", h.error);
            return SetStage(h.Offline() ? UP_OFFLINE : UP_FAILED);
        }
        const std::wstring where = h.Header(WINHTTP_QUERY_LOCATION);
        const size_t at = where.rfind(L"/tag/v");
        if (at == std::wstring::npos) {
            Log(L"update: no release found (status %lu)", h.Status());
            return SetStage(UP_FAILED);
        }
        tag = where.substr(at + 6);
    }
    g_latest = tag;
    if (!Newer(tag, APP_VERSION)) return SetStage(UP_LATEST);
    Log(L"update: %ls -> %ls", APP_VERSION, tag.c_str());

    // Download the zip.
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
    Log(L"update: %ls is in place, restarting", tag.c_str());
    SetStage(UP_RESTART);
}

}  // namespace

void Update_Start(HWND notify) {
    const int s = g_stage;
    if (s == UP_CHECKING || s == UP_DOWNLOADING || s == UP_INSTALLING || s == UP_RESTART) return;
    g_notify = notify;
    g_percent = 0;
    g_stage = UP_CHECKING;
    std::thread(Work).detach();
}

int Update_Stage(int* percent, std::wstring* latest) {
    if (percent) *percent = g_percent;
    if (latest && g_stage >= UP_LATEST) *latest = g_latest;
    return g_stage;
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

// After an update: the old exe goes once its processes have ended, and so do the downloaded files.
void Update_CleanUp() {
    std::thread([] {
        const std::wstring old = ExeDir() + L"\\VideoBG.old.exe";
        for (int i = 0; i < 40 && GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(old.c_str()); i++) Sleep(250);
        RunHidden(L"cmd.exe /c rd /s /q \"" + LocalDataDir() + L"\\update\"");
    }).detach();
}
